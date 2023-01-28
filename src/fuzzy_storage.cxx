/*-
 * Copyright 2016 Vsevolod Stakhov
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/*
 * Rspamd fuzzy storage server
 */

#include "config.h"
#include "libserver/fuzzy_wire.h"
#include "util.h"
#include "rspamd.h"
#include "libserver/maps/map.h"
#include "libserver/maps/map_helpers.h"
#include "libserver/fuzzy_backend/fuzzy_backend.h"
#include "ottery.h"
#include "ref.h"
#include "xxhash.h"
#include "libserver/worker_util.h"
#include "libserver/rspamd_control.h"
#include "libcryptobox/cryptobox.h"
#include "libcryptobox/keypairs_cache.h"
#include "libcryptobox/keypair.h"
#include "libutil/hash.h"
#include "libserver/maps/map_private.h"
#include "contrib/uthash/utlist.h"
#include "lua/lua_common.h"
#include "unix-std.h"

#include <cmath>
#include <vector>
#include "contrib/ankerl/unordered_dense.h"
#include "contrib/ankerl/svector.h"

/* Resync value in seconds */
#define DEFAULT_SYNC_TIMEOUT 60.0
#define DEFAULT_KEYPAIR_CACHE_SIZE 512
#define DEFAULT_MASTER_TIMEOUT 10.0
#define DEFAULT_UPDATES_MAXFAIL 3
#define DEFAULT_MAX_BUCKETS 2000
#define DEFAULT_BUCKET_TTL 3600
#define DEFAULT_BUCKET_MASK 24
/* Update stats on keys each 1 hour */
#define KEY_STAT_INTERVAL 3600.0

static const char *local_db_name = "local";

/* Init functions */
namespace rspamd {
gpointer init_fuzzy(struct rspamd_config *cfg);

void start_fuzzy(struct rspamd_worker *worker);

}

worker_t fuzzy_worker = {
	"fuzzy",                    /* Name */
	rspamd::init_fuzzy,                 /* Init function */
	rspamd::start_fuzzy,                /* Start function */
	RSPAMD_WORKER_HAS_SOCKET,
	RSPAMD_WORKER_SOCKET_UDP,   /* UDP socket */
	RSPAMD_WORKER_VER           /* Version info */
};

namespace rspamd {

struct fuzzy_global_stat {
	std::uint64_t fuzzy_hashes;
	/**< number of fuzzy hashes stored					*/
	std::uint64_t fuzzy_hashes_expired;
	/**< number of fuzzy hashes expired					*/
	std::uint64_t fuzzy_hashes_checked[RSPAMD_FUZZY_EPOCH_MAX];
	/**< amount of check requests for each epoch		*/
	std::uint64_t fuzzy_shingles_checked[RSPAMD_FUZZY_EPOCH_MAX];
	/**< amount of shingle check requests for each epoch	*/
	std::uint64_t fuzzy_hashes_found[RSPAMD_FUZZY_EPOCH_MAX];
	/**< amount of invalid requests				*/
	std::uint64_t invalid_requests;
	/**< amount of delayed hashes found				*/
	std::uint64_t delayed_hashes;
};

struct fuzzy_generic_stat {
	std::uint64_t checked = 0;
	std::uint64_t matched = 0;
	std::uint64_t added = 0;
	std::uint64_t deleted = 0;
	std::uint64_t errors = 0;
	/* Store averages for checked/matched per minute */
	struct rspamd_counter_data checked_ctr;
	struct rspamd_counter_data matched_ctr;
	double last_checked_time = NAN;
	std::uint64_t last_checked_count = 0;
	std::uint64_t last_matched_count = 0;

	fuzzy_generic_stat() {
		memset((void *)&checked_ctr, 0, sizeof(checked_ctr));
		memset((void *)&matched_ctr, 0, sizeof(checked_ctr));
	}

	static auto generic_stat_dtor(void *chunk) -> void {
		delete reinterpret_cast<fuzzy_generic_stat *>(chunk);
	}
};

struct fuzzy_key_stat : public fuzzy_generic_stat {
	rspamd_lru_hash_t *last_ips;

	virtual ~fuzzy_key_stat() {
		rspamd_lru_hash_destroy(last_ips);
	};

	/* Default ctor */
	fuzzy_key_stat() : fuzzy_generic_stat() {
		last_ips = rspamd_lru_hash_new_full(1024,
			(GDestroyNotify) rspamd_inet_address_free,
			fuzzy_generic_stat::generic_stat_dtor,
			rspamd_inet_address_hash, rspamd_inet_address_equal);
	}
};

struct fuzzy_key {
	struct rspamd_cryptobox_keypair *key;
	ankerl::svector<std::uint32_t, 16> forbidden_ids;
	struct fuzzy_key_stat stat;

	explicit fuzzy_key(struct rspamd_cryptobox_keypair *kp) : key(rspamd_keypair_ref(kp)) {}

	virtual ~fuzzy_key() {
		rspamd_keypair_unref(key);
	}

	inline auto is_forbidden(std::uint32_t flag) {
		/* We use just a linear search here as it is faster than any other alternatives for small arrays */
		return std::find(forbidden_ids.begin(), forbidden_ids.end(), flag) != std::end(forbidden_ids);
	}
};

struct fuzzy_key_hash {
public:
	using is_avalanching = void;
	using is_transparent = void;
	inline auto operator() (const fuzzy_key &key) const -> std::uint64_t {
		const auto *pk_bytes = rspamd_keypair_component(key.key, RSPAMD_KEYPAIR_COMPONENT_PK, nullptr);
		std::uint64_t res;
		// pk_bytes is always longer than sizeof(uint64_t), qed.
		memcpy(&res, pk_bytes, sizeof(res));
		return res;
	}

	inline auto operator() (const unsigned char id[RSPAMD_FUZZY_KEYLEN]) const -> std::uint64_t {
		static_assert(RSPAMD_FUZZY_KEYLEN >= sizeof(std::uint64_t));
		std::uint64_t res;
		memcpy(&res, id, sizeof(res));
		return res;
	}
};

struct fuzzy_key_equal {
	using is_transparent = void;
	auto operator()(const fuzzy_key &key1, const fuzzy_key &key2) const {
		const auto *pk_bytes1 = rspamd_keypair_component(key1.key, RSPAMD_KEYPAIR_COMPONENT_PK, nullptr);
		const auto *pk_bytes2 = rspamd_keypair_component(key2.key, RSPAMD_KEYPAIR_COMPONENT_PK, nullptr);

		return (memcmp(pk_bytes1, pk_bytes2, RSPAMD_FUZZY_KEYLEN) == 0);
	}

	auto operator()(const fuzzy_key &key1, const unsigned char id[RSPAMD_FUZZY_KEYLEN]) const {
		const auto *pk_bytes1 = rspamd_keypair_component(key1.key, RSPAMD_KEYPAIR_COMPONENT_PK, nullptr);
		return (memcmp(pk_bytes1, id, RSPAMD_FUZZY_KEYLEN) == 0);
	}
	auto operator()(const unsigned char id[RSPAMD_FUZZY_KEYLEN], const fuzzy_key &key2) const {
		const auto *pk_bytes2 = rspamd_keypair_component(key2.key, RSPAMD_KEYPAIR_COMPONENT_PK, nullptr);
		return (memcmp(pk_bytes2, id, RSPAMD_FUZZY_KEYLEN) == 0);
	}
};

struct rspamd_leaky_bucket_elt {
	rspamd_inet_addr_t *addr = nullptr;
	double last = NAN;
	double cur = NAN;

	virtual ~rspamd_leaky_bucket_elt() {
		if (addr) {
			rspamd_inet_address_free(addr);
		}
	}

	static auto rspamd_leaky_bucket_elt_dtor(void *chunk) -> void {
		delete reinterpret_cast<rspamd_leaky_bucket_elt *>(chunk);
	}
};

static const std::uint64_t rspamd_fuzzy_storage_magic = 0x291a3253eb1b3ea5ULL;

struct rspamd_fuzzy_storage_ctx {
	std::uint64_t magic;
	/* Events base */
	struct ev_loop *event_loop;
	/* DNS resolver */
	struct rspamd_dns_resolver *resolver;
	/* Config */
	struct rspamd_config *cfg;
	/* END OF COMMON PART */
	struct fuzzy_global_stat stat;
	double expire;
	double sync_timeout;
	double delay;
	struct rspamd_radix_map_helper *update_ips;
	struct rspamd_hash_map_helper *update_keys;
	struct rspamd_radix_map_helper *blocked_ips;
	struct rspamd_radix_map_helper *ratelimit_whitelist;
	struct rspamd_radix_map_helper *delay_whitelist;

	const ucl_object_t *update_map;
	const ucl_object_t *update_keys_map;
	const ucl_object_t *delay_whitelist_map;
	const ucl_object_t *blocked_map;
	const ucl_object_t *ratelimit_whitelist_map;

	unsigned int keypair_cache_size;
	ev_timer stat_ev;
	ev_io peer_ev;

	/* Local keypair */
	struct rspamd_cryptobox_keypair *default_keypair; /* Bad clash, need for parse keypair */
	struct fuzzy_key *default_key;
	ankerl::unordered_dense::set<fuzzy_key, fuzzy_key_hash, fuzzy_key_equal> keys;
	bool encrypted_only;
	bool read_only;
	struct rspamd_keypair_cache *keypair_cache;
	struct rspamd_http_context *http_ctx;
	rspamd_lru_hash_t *errors_ips;
	rspamd_lru_hash_t *ratelimit_buckets;
	struct rspamd_fuzzy_backend *backend;
	std::vector<fuzzy_peer_cmd> *updates_pending; /* Pointer due to callbacks complexity */
	unsigned int updates_failed;
	unsigned int updates_maxfail;
	/* Used to send data between workers */
	int peer_fd;

	/* Ratelimits */
	unsigned int leaky_bucket_ttl;
	unsigned int leaky_bucket_mask;
	unsigned int max_buckets;
	bool ratelimit_log_only;
	double leaky_bucket_burst;
	double leaky_bucket_rate;

	struct rspamd_worker *worker;
	const ucl_object_t *skip_map;
	struct rspamd_hash_map_helper *skip_hashes;
	int lua_pre_handler_cbref;
	int lua_post_handler_cbref;
	int lua_blacklist_cbref;
};

enum fuzzy_cmd_type {
	CMD_NORMAL,
	CMD_SHINGLE,
	CMD_ENCRYPTED_NORMAL,
	CMD_ENCRYPTED_SHINGLE
};

struct fuzzy_session {
	struct rspamd_worker *worker;
	rspamd_inet_addr_t *addr;
	struct rspamd_fuzzy_storage_ctx *ctx;

	struct rspamd_fuzzy_shingle_cmd cmd; /* Can handle both shingles and non-shingles */
	struct rspamd_fuzzy_encrypted_reply reply; /* Again: contains everything */

	enum rspamd_fuzzy_epoch epoch;
	enum fuzzy_cmd_type cmd_type;
	int fd;
	ev_tstamp timestamp;
	struct ev_io io;
	ref_entry_t ref; /* Used in C++ mode as well, as we are still bound by libev restrictions */
	fuzzy_generic_stat *ip_stat;
	fuzzy_key* key;
	rspamd_fuzzy_cmd_extension *extensions;
	guchar nm[rspamd_cryptobox_MAX_NMBYTES];
};

struct fuzzy_peer_request {
	ev_io io_ev;
	struct fuzzy_peer_cmd cmd;
};


struct rspamd_updates_cbdata {
	std::vector<fuzzy_peer_cmd> *updates_pending;
	struct rspamd_fuzzy_storage_ctx *ctx;
	std::string source;
	bool final;

	explicit rspamd_updates_cbdata(std::size_t reserved_updates_size, rspamd_fuzzy_storage_ctx *ctx, const char *src, bool final) :
		ctx(ctx), source(src), final(final) {
		updates_pending = new std::vector<fuzzy_peer_cmd>;
		updates_pending->reserve(reserved_updates_size);
	}

	virtual ~rspamd_updates_cbdata() {
		if (updates_pending) {
			delete updates_pending;
		}
	}
};


static void rspamd_fuzzy_write_reply(struct fuzzy_session *session);

static bool rspamd_fuzzy_process_updates_queue(struct rspamd_fuzzy_storage_ctx *ctx,
												   const char *source, bool final);

static bool rspamd_fuzzy_check_client(struct rspamd_fuzzy_storage_ctx *ctx,
										  rspamd_inet_addr_t *addr);

static void rspamd_fuzzy_maybe_call_blacklisted(struct rspamd_fuzzy_storage_ctx *ctx,
												rspamd_inet_addr_t *addr,
												const char *reason);

static bool
rspamd_fuzzy_check_ratelimit(struct fuzzy_session *session)
{
	if (!session->addr) {
		return true;
	}

	if (session->ctx->ratelimit_whitelist != nullptr) {
		if (rspamd_match_radix_map_addr(session->ctx->ratelimit_whitelist,
			session->addr) != nullptr) {
			return true;
		}
	}

	/*
	if (rspamd_inet_address_is_local (session->addr, true)) {
		return true;
	}
	*/

	auto *masked = rspamd_inet_address_copy(session->addr, nullptr);

	if (rspamd_inet_address_get_af(masked) == AF_INET) {
		rspamd_inet_address_apply_mask(masked,
			MIN (session->ctx->leaky_bucket_mask, 32));
	}
	else {
		/* Must be at least /64 */
		rspamd_inet_address_apply_mask(masked,
			MIN (MAX(session->ctx->leaky_bucket_mask * 4, 64), 128));
	}

	auto *elt = (struct rspamd_leaky_bucket_elt *)rspamd_lru_hash_lookup(session->ctx->ratelimit_buckets, masked,
		(time_t) session->timestamp);

	if (elt) {
		auto ratelimited = false;

		if (isnan(elt->cur)) {
			/* Ratelimit exceeded, preserve it for the whole ttl */
			ratelimited = true;
		}
		else {
			/* Update bucket */
			if (elt->last < session->timestamp) {
				elt->cur -= session->ctx->leaky_bucket_rate * (session->timestamp - elt->last);
				elt->last = session->timestamp;

				if (elt->cur < 0) {
					elt->cur = 0;
				}
			}
			else {
				elt->last = session->timestamp;
			}

			/* Check bucket */
			if (elt->cur >= session->ctx->leaky_bucket_burst) {

				msg_info ("ratelimiting %s (%s), %.1f max elts",
					rspamd_inet_address_to_string(session->addr),
					rspamd_inet_address_to_string(masked),
					session->ctx->leaky_bucket_burst);
				elt->cur = NAN;
			}
			else {
				elt->cur++; /* Allow one more request */
			}
		}

		rspamd_inet_address_free(masked);

		if (ratelimited) {
			rspamd_fuzzy_maybe_call_blacklisted(session->ctx, session->addr, "ratelimit");
		}

		return !ratelimited;
	}
	else {
		/* New bucket */
		elt = new rspamd_leaky_bucket_elt;
		elt->addr = masked; /* transfer ownership */
		elt->cur = 1;
		elt->last = session->timestamp;

		rspamd_lru_hash_insert(session->ctx->ratelimit_buckets,
			(void *)masked,
			(void *)elt,
			session->timestamp,
			session->ctx->leaky_bucket_ttl);
	}

	return true;
}

static void
rspamd_fuzzy_maybe_call_blacklisted(struct rspamd_fuzzy_storage_ctx *ctx,
									rspamd_inet_addr_t *addr,
									const char *reason)
{
	if (ctx->lua_blacklist_cbref != -1) {
		auto *L = (lua_State *)ctx->cfg->lua_state;

		lua_pushcfunction (L, &rspamd_lua_traceback);
		auto err_idx = lua_gettop(L);
		lua_rawgeti(L, LUA_REGISTRYINDEX, ctx->lua_blacklist_cbref);
		/* client IP */
		rspamd_lua_ip_push(L, addr);
		/* block reason */
		lua_pushstring(L, reason);

		if (int ret; (ret = lua_pcall(L, 2, 0, err_idx)) != 0) {
			msg_err ("call to lua_blacklist_cbref "
					 "script failed (%d): %s", ret, lua_tostring(L, -1));
		}

		lua_settop(L, 0);
	}
}

static bool
rspamd_fuzzy_check_client(struct rspamd_fuzzy_storage_ctx *ctx,
						  rspamd_inet_addr_t *addr)
{
	if (ctx->blocked_ips != nullptr) {
		if (rspamd_match_radix_map_addr(ctx->blocked_ips,
			addr) != nullptr) {

			rspamd_fuzzy_maybe_call_blacklisted(ctx, addr, "blacklisted");
			return false;
		}
	}

	return true;
}

static bool
rspamd_fuzzy_check_write(struct fuzzy_session *session)
{
	if (session->ctx->read_only) {
		return false;
	}

	if (session->ctx->update_ips != nullptr && session->addr) {
		if (rspamd_inet_address_get_af(session->addr) == AF_UNIX) {
			return true;
		}
		if (rspamd_match_radix_map_addr(session->ctx->update_ips,
			session->addr) == nullptr) {
			return false;
		}
		else {
			return true;
		}
	}

	if (session->ctx->update_keys != nullptr && session->key->key) {
		static char base32_buf[rspamd_cryptobox_HASHBYTES * 2 + 1];
		unsigned int raw_len;
		const guchar *pk_raw = rspamd_keypair_component(session->key->key,
			RSPAMD_KEYPAIR_COMPONENT_ID, &raw_len);
		int encoded_len = rspamd_encode_base32_buf(pk_raw, raw_len,
			base32_buf, sizeof(base32_buf),
			RSPAMD_BASE32_DEFAULT);

		if (rspamd_match_hash_map(session->ctx->update_keys, base32_buf, encoded_len)) {
			return true;
		}
	}

	return false;
}


static void
fuzzy_count_callback(std::uint64_t count, void *ud)
{
	auto *ctx = (struct rspamd_fuzzy_storage_ctx *)ud;

	ctx->stat.fuzzy_hashes = count;
}

static void
fuzzy_stat_count_callback(std::uint64_t count, void *ud)
{
	auto *ctx = (struct rspamd_fuzzy_storage_ctx *)ud;

	ev_timer_again(ctx->event_loop, &ctx->stat_ev);
	ctx->stat.fuzzy_hashes = count;
}

static void
rspamd_fuzzy_stat_callback(EV_P_ ev_timer *w, int revents)
{
	auto *ctx = (struct rspamd_fuzzy_storage_ctx *) w->data;
	rspamd_fuzzy_backend_count(ctx->backend, fuzzy_stat_count_callback, ctx);
}


static void
fuzzy_update_version_callback(std::uint64_t ver, void *ud)
{
}

static void
rspamd_fuzzy_updates_cb(bool success,
						unsigned int nadded,
						unsigned int ndeleted,
						unsigned int nextended,
						unsigned int nignored,
						void *ud)
{
	auto *cbdata = (struct rspamd_updates_cbdata *)ud;
	struct rspamd_fuzzy_storage_ctx *ctx;

	ctx = cbdata->ctx;
	const auto &source = cbdata->source;

	if (success) {
		rspamd_fuzzy_backend_count(ctx->backend, fuzzy_count_callback, ctx);

		msg_info ("successfully updated fuzzy storage %s: %d updates in queue; "
				  "%d pending currently; "
				  "%d added; %d deleted; %d extended; %d duplicates",
			ctx->worker->cf->bind_conf ?
			ctx->worker->cf->bind_conf->bind_line :
			"unknown",
			cbdata->updates_pending->size(),
			ctx->updates_pending->size(),
			nadded, ndeleted, nextended, nignored);
		rspamd_fuzzy_backend_version(ctx->backend, source.c_str(),
			fuzzy_update_version_callback, nullptr);
		ctx->updates_failed = 0;

		if (cbdata->final || ctx->worker->state != rspamd_worker_state_running) {
			/* Plan exit */
			ev_break(ctx->event_loop, EVBREAK_ALL);
		}
	}
	else {
		if (++ctx->updates_failed > ctx->updates_maxfail) {
			msg_err ("cannot commit update transaction to fuzzy backend %s, discard "
					 "%ud updates after %d retries",
				ctx->worker->cf->bind_conf ?
				ctx->worker->cf->bind_conf->bind_line :
				"unknown",
				cbdata->updates_pending->size(),
				ctx->updates_maxfail);
			ctx->updates_failed = 0;

			if (cbdata->final || ctx->worker->state != rspamd_worker_state_running) {
				/* Plan exit */
				ev_break(ctx->event_loop, EVBREAK_ALL);
			}
		}
		else {
			if (ctx->updates_pending) {
				msg_err ("cannot commit update transaction to fuzzy backend %s; "
						 "%ud updates are still left; %ud currently pending;"
						 " %d retries remaining",
					ctx->worker->cf->bind_conf ?
					ctx->worker->cf->bind_conf->bind_line : "unknown",
					cbdata->updates_pending->size(),
					ctx->updates_pending->size(),
					ctx->updates_maxfail - ctx->updates_failed);
				/* Move the remaining updates to ctx queue */
				ctx->updates_pending->reserve(ctx->updates_pending->size() + cbdata->updates_pending->size());
				ctx->updates_pending->insert(std::end(*ctx->updates_pending),
					std::begin(*cbdata->updates_pending),
					std::end(*cbdata->updates_pending));

				if (cbdata->final) {
					/* Try one more time */
					rspamd_fuzzy_process_updates_queue(cbdata->ctx, cbdata->source.c_str(),
						cbdata->final);
				}
			}
			else {
				/* We have lost our ctx, so it is a race condition case */
				msg_err ("cannot commit update transaction to fuzzy backend %s; "
						 "%ud updates are still left; no more retries: a worker is terminated",
					ctx->worker->cf->bind_conf ?
					ctx->worker->cf->bind_conf->bind_line : "unknown",
					cbdata->updates_pending->size());
			}
		}
	}

	delete cbdata;
}

static bool
rspamd_fuzzy_process_updates_queue(struct rspamd_fuzzy_storage_ctx *ctx,
								   const char *source, bool final)
{
	if (!ctx->updates_pending->empty()) {

		auto cbdata = new rspamd_updates_cbdata{std::max(ctx->updates_pending->size(), std::size_t{1024}), ctx, source, final};
		std::swap(ctx->updates_pending, cbdata->updates_pending);
		rspamd_fuzzy_backend_process_updates(ctx->backend,
			cbdata->updates_pending,
			source, rspamd_fuzzy_updates_cb, (void *)cbdata);
		return true;
	}
	else if (final) {
		/* No need to sync */
		ev_break(ctx->event_loop, EVBREAK_ALL);
	}

	return false;
}

static void
rspamd_fuzzy_reply_io(EV_P_ ev_io *w, int _revents)
{
	auto *session = (struct fuzzy_session *) w->data;

	ev_io_stop(EV_A_ w);
	rspamd_fuzzy_write_reply(session);
	REF_RELEASE (session);
}

static void
rspamd_fuzzy_write_reply(struct fuzzy_session *session)
{
	gssize r;
	gsize len;
	gconstpointer data;

	if (session->cmd_type == CMD_ENCRYPTED_NORMAL ||
		session->cmd_type == CMD_ENCRYPTED_SHINGLE) {
		/* Encrypted reply */
		data = &session->reply;

		if (session->epoch > RSPAMD_FUZZY_EPOCH10) {
			len = sizeof(session->reply);
		}
		else {
			len = sizeof(session->reply.hdr) + sizeof(session->reply.rep.v1);
		}
	}
	else {
		data = &session->reply.rep;

		if (session->epoch > RSPAMD_FUZZY_EPOCH10) {
			len = sizeof(session->reply.rep);
		}
		else {
			len = sizeof(session->reply.rep.v1);
		}
	}

	r = rspamd_inet_address_sendto(session->fd, data, len, 0,
		session->addr);

	if (r == -1) {
		if (errno == EINTR || errno == EWOULDBLOCK || errno == EAGAIN) {
			/* Grab reference to avoid early destruction */
			REF_RETAIN (session);
			session->io.data = session;
			ev_io_init (&session->io,
				rspamd_fuzzy_reply_io, session->fd, EV_WRITE);
			ev_io_start(session->ctx->event_loop, &session->io);
		}
		else {
			msg_err ("error while writing reply: %s", strerror(errno));
		}
	}
}

static void
rspamd_fuzzy_update_stats(struct rspamd_fuzzy_storage_ctx *ctx,
						  enum rspamd_fuzzy_epoch epoch,
						  bool matched,
						  bool is_shingle,
						  bool is_delayed,
						  struct fuzzy_key_stat *key_stat,
						  struct fuzzy_generic_stat *ip_stat,
						  unsigned int cmd,
						  unsigned int reply,
						  ev_tstamp timestamp)
{
	ctx->stat.fuzzy_hashes_checked[epoch]++;

	if (matched) {
		ctx->stat.fuzzy_hashes_found[epoch]++;
	}
	if (is_shingle) {
		ctx->stat.fuzzy_shingles_checked[epoch]++;
	}
	if (is_delayed) {
		ctx->stat.delayed_hashes++;
	}

	if (key_stat) {
		if (!matched && reply != 0) {
			key_stat->errors++;
		}
		else {
			if (cmd == FUZZY_CHECK) {
				key_stat->checked++;

				if (matched) {
					key_stat->matched++;
				}

				if (G_UNLIKELY(key_stat->last_checked_time == 0.0)) {
					key_stat->last_checked_time = timestamp;
					key_stat->last_checked_count = key_stat->checked;
					key_stat->last_matched_count = key_stat->matched;
				}
				else if (G_UNLIKELY(timestamp > key_stat->last_checked_time + KEY_STAT_INTERVAL)) {
					std::uint64_t nchecked = key_stat->checked - key_stat->last_checked_count;
					std::uint64_t nmatched = key_stat->matched - key_stat->last_matched_count;

					rspamd_set_counter_ema(&key_stat->checked_ctr, nchecked, 0.5);
					rspamd_set_counter_ema(&key_stat->checked_ctr, nmatched, 0.5);
					key_stat->last_checked_time = timestamp;
					key_stat->last_checked_count = key_stat->checked;
					key_stat->last_matched_count = key_stat->matched;
				}
			}
			else if (cmd == FUZZY_WRITE) {
				key_stat->added++;
			}
			else if (cmd == FUZZY_DEL) {
				key_stat->deleted++;
			}
		}
	}

	if (ip_stat) {
		if (!matched && reply != 0) {
			ip_stat->errors++;
		}
		else {
			if (cmd == FUZZY_CHECK) {
				ip_stat->checked++;

				if (matched) {
					ip_stat->matched++;
				}
			}
			else if (cmd == FUZZY_WRITE) {
				ip_stat->added++;
			}
			else if (cmd == FUZZY_DEL) {
				ip_stat->deleted++;
			}
		}
	}
}

enum rspamd_fuzzy_reply_flags {
	RSPAMD_FUZZY_REPLY_ENCRYPTED = 0x1u << 0u,
	RSPAMD_FUZZY_REPLY_SHINGLE = 0x1u << 1u,
	RSPAMD_FUZZY_REPLY_DELAY = 0x1u << 2u,
};

static void
rspamd_fuzzy_make_reply(struct rspamd_fuzzy_cmd *cmd,
						struct rspamd_fuzzy_reply *result,
						struct fuzzy_session *session,
						int flags)
{
	gsize len;

	if (cmd) {
		result->v1.tag = cmd->tag;
		memcpy(&session->reply.rep, result, sizeof(*result));

		rspamd_fuzzy_update_stats(session->ctx,
			session->epoch,
			result->v1.prob > 0.5,
			flags & RSPAMD_FUZZY_REPLY_SHINGLE,
			flags & RSPAMD_FUZZY_REPLY_DELAY,
			session->key ? &session->key->stat : nullptr,
			session->ip_stat,
			cmd->cmd,
			result->v1.value,
			session->timestamp);

		if (flags & RSPAMD_FUZZY_REPLY_DELAY) {
			/* Hash is too fresh, need to delay it */
			session->reply.rep.ts = 0;
			session->reply.rep.v1.prob = 0.0;
			session->reply.rep.v1.value = 0;
		}

		if (flags & RSPAMD_FUZZY_REPLY_ENCRYPTED) {

			if (session->reply.rep.v1.prob > 0 && session->key) {
				if (session->key->is_forbidden(session->reply.rep.v1.flag)) {
					/* Hash is from a forbidden flag for this key */
					session->reply.rep.ts = 0;
					session->reply.rep.v1.prob = 0.0;
					session->reply.rep.v1.value = 0;
					session->reply.rep.v1.flag = 0;
				}
			}

			/* We need also to encrypt reply */
			ottery_rand_bytes(session->reply.hdr.nonce,
				sizeof(session->reply.hdr.nonce));

			/*
			 * For old replies we need to encrypt just old part, otherwise
			 * decryption would fail due to mac verification mistake
			 */

			if (session->epoch > RSPAMD_FUZZY_EPOCH10) {
				len = sizeof(session->reply.rep);
			}
			else {
				len = sizeof(session->reply.rep.v1);
			}

			rspamd_cryptobox_encrypt_nm_inplace((guchar *) &session->reply.rep,
				len,
				session->reply.hdr.nonce,
				session->nm,
				session->reply.hdr.mac,
				RSPAMD_CRYPTOBOX_MODE_25519);
		}
	}

	rspamd_fuzzy_write_reply(session);
}

static bool
fuzzy_peer_try_send(int fd, struct fuzzy_peer_request *up_req)
{
	gssize r;

	r = write(fd, &up_req->cmd, sizeof(up_req->cmd));

	if (r != sizeof(up_req->cmd)) {
		return false;
	}

	return true;
}

static void
fuzzy_peer_send_io(EV_P_ ev_io *w, int revents)
{
	auto *up_req = (struct fuzzy_peer_request *) w->data;

	if (!fuzzy_peer_try_send(w->fd, up_req)) {
		msg_err ("cannot send update request to the peer: %s", strerror(errno));
	}

	ev_io_stop(EV_A_ w);
	delete up_req;
}

static void
rspamd_fuzzy_extensions_tolua(lua_State *L,
							  struct fuzzy_session *session)
{
	struct rspamd_fuzzy_cmd_extension *ext;
	rspamd_inet_addr_t *addr;

	lua_createtable(L, 0, 0);

	LL_FOREACH (session->extensions, ext) {
		switch (ext->ext) {
		case RSPAMD_FUZZY_EXT_SOURCE_DOMAIN:
			lua_pushlstring(L, (const char *)ext->payload, ext->length);
			lua_setfield(L, -2, "domain");
			break;
		case RSPAMD_FUZZY_EXT_SOURCE_IP4:
			addr = rspamd_inet_address_new(AF_INET, ext->payload);
			rspamd_lua_ip_push(L, addr);
			rspamd_inet_address_free(addr);
			lua_setfield(L, -2, "ip");
			break;
		case RSPAMD_FUZZY_EXT_SOURCE_IP6:
			addr = rspamd_inet_address_new(AF_INET6, ext->payload);
			rspamd_lua_ip_push(L, addr);
			rspamd_inet_address_free(addr);
			lua_setfield(L, -2, "ip");
			break;
		}
	}
}

static void
rspamd_fuzzy_check_callback(struct rspamd_fuzzy_reply *result, void *ud)
{
	auto *session = (struct fuzzy_session *)ud;
	bool is_shingle = false, __attribute__ ((unused)) encrypted = false;
	struct rspamd_fuzzy_cmd *cmd = nullptr;
	const struct rspamd_shingle *shingle = nullptr;
	struct rspamd_shingle sgl_cpy;
	int send_flags = 0;

	switch (session->cmd_type) {
	case CMD_ENCRYPTED_NORMAL:
		encrypted = true;
		send_flags |= RSPAMD_FUZZY_REPLY_ENCRYPTED;
		/* Fallthrough */
	case CMD_NORMAL:
		cmd = &session->cmd.basic;
		break;

	case CMD_ENCRYPTED_SHINGLE:
		encrypted = true;
		send_flags |= RSPAMD_FUZZY_REPLY_ENCRYPTED;
		/* Fallthrough */
	case CMD_SHINGLE:
		cmd = &session->cmd.basic;
		memcpy(&sgl_cpy, &session->cmd.sgl, sizeof(sgl_cpy));
		shingle = &sgl_cpy;
		is_shingle = true;
		send_flags |= RSPAMD_FUZZY_REPLY_SHINGLE;
		break;
	}

	if (session->ctx->lua_post_handler_cbref != -1) {
		/* Start lua post handler */
		auto *L = (lua_State *)session->ctx->cfg->lua_state;

		lua_pushcfunction (L, &rspamd_lua_traceback);
		auto err_idx = lua_gettop(L);
		/* Preallocate stack (small opt) */
		lua_checkstack(L, err_idx + 9);
		/* function */
		lua_rawgeti(L, LUA_REGISTRYINDEX, session->ctx->lua_post_handler_cbref);
		/* client IP */
		if (session->addr) {
			rspamd_lua_ip_push(L, session->addr);
		}
		else {
			lua_pushnil(L);
		}
		/* client command */
		lua_pushinteger(L, cmd->cmd);
		/* command value (push as rspamd_text) */
		(void) lua_new_text(L, result->digest, sizeof(result->digest), false);
		/* is shingle */
		lua_pushboolean(L, is_shingle);
		/* result value */
		lua_pushinteger(L, result->v1.value);
		/* result probability */
		lua_pushnumber(L, result->v1.prob);
		/* result flag */
		lua_pushinteger(L, result->v1.flag);
		/* result timestamp */
		lua_pushinteger(L, result->ts);
		/* TODO: add additional data maybe (encryption, pubkey, etc) */
		rspamd_fuzzy_extensions_tolua(L, session);

		if (int ret; (ret = lua_pcall(L, 9, LUA_MULTRET, err_idx)) != 0) {
			msg_err ("call to lua_post_handler lua "
					 "script failed (%d): %s", ret, lua_tostring(L, -1));
		}
		else {
			/* Return values order:
			 * the first reply will be on err_idx + 1
			 * if it is true, then we need to read the former ones:
			 * 2-nd will be reply code
			 * 3-rd will be probability (or 0.0 if missing)
			 * 4-th value is flag (or default flag if missing)
			 */
			ret = lua_toboolean(L, err_idx + 1);

			if (ret) {
				/* Artificial reply */
				result->v1.value = lua_tointeger(L, err_idx + 2);

				if (lua_isnumber(L, err_idx + 3)) {
					result->v1.prob = lua_tonumber(L, err_idx + 3);
				}
				else {
					result->v1.prob = 0.0f;
				}

				if (lua_isnumber(L, err_idx + 4)) {
					result->v1.flag = lua_tointeger(L, err_idx + 4);
				}

				lua_settop(L, 0);
				rspamd_fuzzy_make_reply(cmd, result, session, send_flags);
				REF_RELEASE (session);

				return;
			}
		}

		lua_settop(L, 0);
	}

	if (!isnan(session->ctx->delay) &&
		rspamd_match_radix_map_addr(session->ctx->delay_whitelist,
			session->addr) == nullptr) {
		double hash_age = rspamd_get_calendar_ticks() - result->ts;
		double jittered_age = rspamd_time_jitter(session->ctx->delay,
			session->ctx->delay / 2.0);

		if (hash_age < jittered_age) {
			send_flags |= RSPAMD_FUZZY_REPLY_DELAY;
		}
	}

	/* Refresh hash if found with strong confidence */
	if (result->v1.prob > 0.9 && !session->ctx->read_only) {

		if (session->worker->index == 0) {
			struct fuzzy_peer_cmd up_cmd;
			/* Just add to the queue */
			memset(&up_cmd, 0, sizeof(up_cmd));
			up_cmd.is_shingle = is_shingle;
			memcpy(up_cmd.cmd.normal.digest, result->digest,
				sizeof(up_cmd.cmd.normal.digest));
			up_cmd.cmd.normal.flag = result->v1.flag;
			up_cmd.cmd.normal.cmd = FUZZY_REFRESH;
			up_cmd.cmd.normal.shingles_count = cmd->shingles_count;

			if (is_shingle && shingle) {
				memcpy(&up_cmd.cmd.shingle.sgl, shingle,
					sizeof(up_cmd.cmd.shingle.sgl));
			}

			session->ctx->updates_pending->push_back(std::move(up_cmd));
		}
		else {
			/* We need to send request to the peer */
			auto up_req = new struct fuzzy_peer_request;
			up_req->cmd.is_shingle = is_shingle;

			memcpy(up_req->cmd.cmd.normal.digest, result->digest,
				sizeof(up_req->cmd.cmd.normal.digest));
			up_req->cmd.cmd.normal.flag = result->v1.flag;
			up_req->cmd.cmd.normal.cmd = FUZZY_REFRESH;
			up_req->cmd.cmd.normal.shingles_count = cmd->shingles_count;

			if (is_shingle && shingle) {
				memcpy(&up_req->cmd.cmd.shingle.sgl, shingle,
					sizeof(up_req->cmd.cmd.shingle.sgl));
			}

			if (!fuzzy_peer_try_send(session->ctx->peer_fd, up_req)) {
				up_req->io_ev.data = up_req;
				ev_io_init (&up_req->io_ev, fuzzy_peer_send_io,
					session->ctx->peer_fd, EV_WRITE);
				ev_io_start(session->ctx->event_loop, &up_req->io_ev);
			}
			else {
				delete up_req;
			}
		}
	}

	rspamd_fuzzy_make_reply(cmd, result, session, send_flags);

	REF_RELEASE (session);
}

static void
rspamd_fuzzy_process_command(struct fuzzy_session *session)
{
	bool is_shingle = false, __attribute__ ((unused)) encrypted = false;
	struct rspamd_fuzzy_cmd *cmd = nullptr;
	struct rspamd_fuzzy_reply result;
	struct fuzzy_peer_cmd up_cmd;
	gpointer ptr;
	gsize up_len = 0;
	int send_flags = 0;

	cmd = &session->cmd.basic;

	switch (session->cmd_type) {
	case CMD_NORMAL:
		up_len = sizeof(session->cmd.basic);
		break;
	case CMD_SHINGLE:
		up_len = sizeof(session->cmd);
		is_shingle = true;
		send_flags |= RSPAMD_FUZZY_REPLY_SHINGLE;
		break;
	case CMD_ENCRYPTED_NORMAL:
		up_len = sizeof(session->cmd.basic);
		encrypted = true;
		send_flags |= RSPAMD_FUZZY_REPLY_ENCRYPTED;
		break;
	case CMD_ENCRYPTED_SHINGLE:
		up_len = sizeof(session->cmd);
		encrypted = true;
		is_shingle = true;
		send_flags |= RSPAMD_FUZZY_REPLY_SHINGLE | RSPAMD_FUZZY_REPLY_ENCRYPTED;
		break;
	default:
		msg_err ("invalid command type: %d", session->cmd_type);
		return;
	}

	memset(&result, 0, sizeof(result));
	memcpy(result.digest, cmd->digest, sizeof(result.digest));
	result.v1.flag = cmd->flag;
	result.v1.tag = cmd->tag;

	if (session->ctx->lua_pre_handler_cbref != -1) {
		/* Start lua pre handler */
		auto *L = (lua_State *)session->ctx->cfg->lua_state;
		int err_idx, ret;

		lua_pushcfunction (L, &rspamd_lua_traceback);
		err_idx = lua_gettop(L);
		/* Preallocate stack (small opt) */
		lua_checkstack(L, err_idx + 5);
		/* function */
		lua_rawgeti(L, LUA_REGISTRYINDEX, session->ctx->lua_pre_handler_cbref);
		/* client IP */
		rspamd_lua_ip_push(L, session->addr);
		/* client command */
		lua_pushinteger(L, cmd->cmd);
		/* command value (push as rspamd_text) */
		(void) lua_new_text(L, cmd->digest, sizeof(cmd->digest), false);
		/* is shingle */
		lua_pushboolean(L, is_shingle);
		/* TODO: add additional data maybe (encryption, pubkey, etc) */
		rspamd_fuzzy_extensions_tolua(L, session);

		if ((ret = lua_pcall(L, 5, LUA_MULTRET, err_idx)) != 0) {
			msg_err ("call to lua_pre_handler lua "
					 "script failed (%d): %s", ret, lua_tostring(L, -1));
		}
		else {
			/* Return values order:
			 * the first reply will be on err_idx + 1
			 * if it is true, then we need to read the former ones:
			 * 2-nd will be reply code
			 * 3-rd will be probability (or 0.0 if missing)
			 */
			ret = lua_toboolean(L, err_idx + 1);

			if (ret) {
				/* Artificial reply */
				result.v1.value = lua_tointeger(L, err_idx + 2);

				if (lua_isnumber(L, err_idx + 3)) {
					result.v1.prob = lua_tonumber(L, err_idx + 3);
				}
				else {
					result.v1.prob = 0.0f;
				}

				lua_settop(L, 0);
				rspamd_fuzzy_make_reply(cmd, &result, session, send_flags);

				return;
			}
		}

		lua_settop(L, 0);
	}


	if (G_UNLIKELY (cmd == nullptr || up_len == 0)) {
		result.v1.value = 500;
		result.v1.prob = 0.0f;
		rspamd_fuzzy_make_reply(cmd, &result, session, send_flags);
		return;
	}

	if (session->ctx->encrypted_only && !encrypted) {
		/* Do not accept unencrypted commands */
		result.v1.value = 403;
		result.v1.prob = 0.0f;
		rspamd_fuzzy_make_reply(cmd, &result, session, send_flags);
		return;
	}

	if (session->key && session->addr) {
		auto *ip_stat = (fuzzy_generic_stat *)rspamd_lru_hash_lookup(session->key->stat->last_ips,
			session->addr, -1);

		if (ip_stat == nullptr) {
			auto *naddr = rspamd_inet_address_copy(session->addr, nullptr);
			ip_stat = new fuzzy_generic_stat;
			rspamd_lru_hash_insert(session->key->stat.last_ips,
				naddr, ip_stat, -1, 0);
		}

		REF_RETAIN (ip_stat);
		session->ip_stat = ip_stat;
	}

	if (cmd->cmd == FUZZY_CHECK) {
		bool can_continue = true;

		if (session->ctx->ratelimit_buckets) {
			if (session->ctx->ratelimit_log_only) {
				(void) rspamd_fuzzy_check_ratelimit(session); /* Check but ignore */
			}
			else {
				can_continue = rspamd_fuzzy_check_ratelimit(session);
			}
		}

		if (can_continue) {
			REF_RETAIN (session);
			rspamd_fuzzy_backend_check(session->ctx->backend, cmd,
				rspamd_fuzzy_check_callback, session);
		}
		else {
			result.v1.value = 403;
			result.v1.prob = 0.0f;
			result.v1.flag = 0;
			rspamd_fuzzy_make_reply(cmd, &result, session, send_flags);
		}
	}
	else if (cmd->cmd == FUZZY_STAT) {
		result.v1.prob = 1.0f;
		result.v1.value = 0;
		result.v1.flag = session->ctx->stat.fuzzy_hashes;
		rspamd_fuzzy_make_reply(cmd, &result, session, send_flags);
	}
	else {
		if (rspamd_fuzzy_check_write(session)) {
			/* Check whitelist */
			if (session->ctx->skip_hashes && cmd->cmd == FUZZY_WRITE) {
				char hexbuf[rspamd_cryptobox_HASHBYTES * 2 + 1];
				rspamd_encode_hex_buf((unsigned char *)cmd->digest, sizeof(cmd->digest),
					hexbuf, sizeof(hexbuf) - 1);
				hexbuf[sizeof(hexbuf) - 1] = '\0';

				if (rspamd_match_hash_map(session->ctx->skip_hashes,
					hexbuf, sizeof(hexbuf) - 1)) {
					result.v1.value = 401;
					result.v1.prob = 0.0f;

					goto reply;
				}
			}

			if (session->worker->index == 0 || session->ctx->peer_fd == -1) {
				/* Just add to the queue */
				up_cmd.is_shingle = is_shingle;
				ptr = is_shingle ?
					  (gpointer) &up_cmd.cmd.shingle :
					  (gpointer) &up_cmd.cmd.normal;
				memcpy(ptr, cmd, up_len);
				session->ctx->updates_pending->push_back(std::move(up_cmd));
			}
			else {
				/* We need to send request to the peer */
				auto *up_req = new struct fuzzy_peer_request;
				up_req->cmd.is_shingle = is_shingle;
				ptr = is_shingle ?
					  (gpointer) &up_req->cmd.cmd.shingle :
					  (gpointer) &up_req->cmd.cmd.normal;
				memcpy(ptr, cmd, up_len);

				if (!fuzzy_peer_try_send(session->ctx->peer_fd, up_req)) {
					up_req->io_ev.data = up_req;
					ev_io_init (&up_req->io_ev, fuzzy_peer_send_io,
						session->ctx->peer_fd, EV_WRITE);
					ev_io_start(session->ctx->event_loop, &up_req->io_ev);
				}
				else {
					delete up_req;
				}
			}

			result.v1.value = 0;
			result.v1.prob = 1.0f;
		}
		else {
			result.v1.value = 403;
			result.v1.prob = 0.0f;
		}
reply:
		rspamd_fuzzy_make_reply(cmd, &result, session, send_flags);
	}
}


static enum rspamd_fuzzy_epoch
rspamd_fuzzy_command_valid(struct rspamd_fuzzy_cmd *cmd, int r)
{
	enum rspamd_fuzzy_epoch ret = RSPAMD_FUZZY_EPOCH_MAX;

	switch (cmd->version) {
	case 4:
		if (cmd->shingles_count > 0) {
			if (r >= sizeof(struct rspamd_fuzzy_shingle_cmd)) {
				ret = RSPAMD_FUZZY_EPOCH11;
			}
		}
		else {
			if (r >= sizeof(*cmd)) {
				ret = RSPAMD_FUZZY_EPOCH11;
			}
		}
		break;
	case 3:
		if (cmd->shingles_count > 0) {
			if (r == sizeof(struct rspamd_fuzzy_shingle_cmd)) {
				ret = RSPAMD_FUZZY_EPOCH10;
			}
		}
		else {
			if (r == sizeof(*cmd)) {
				ret = RSPAMD_FUZZY_EPOCH10;
			}
		}
		break;
	default:
		break;
	}

	return ret;
}

static bool
rspamd_fuzzy_decrypt_command(struct fuzzy_session *session, guchar *buf, gsize buflen)
{
	struct rspamd_fuzzy_encrypted_req_hdr hdr;
	struct fuzzy_key *key = nullptr;

	if (session->ctx->default_key == nullptr) {
		msg_warn ("received encrypted request when encryption is not enabled");
		return false;
	}

	if (buflen < sizeof(hdr)) {
		msg_warn ("XXX: should not be reached");
		return false;
	}

	memcpy(&hdr, buf, sizeof(hdr));
	buf += sizeof(hdr);
	buflen -= sizeof(hdr);

	/* Try to find the desired key */
	auto maybe_key = session->ctx->keys.find(hdr.key_id);

	if (maybe_key == std::end(session->ctx->keys)) {
		/* Unknown key, assume default one */
		key = session->ctx->default_key;
	}
	else {
		key = &*maybe_key;
	}

	session->key = key;

	/* Now process keypair */
	auto *rk = rspamd_pubkey_from_bin(hdr.pubkey, sizeof(hdr.pubkey),
		RSPAMD_KEYPAIR_KEX, RSPAMD_CRYPTOBOX_MODE_25519);

	if (rk == nullptr) {
		msg_err ("bad key; ip=%s",
			rspamd_inet_address_to_string(session->addr));
		return false;
	}

	rspamd_keypair_cache_process(session->ctx->keypair_cache, key->key, rk);

	/* Now decrypt request */
	if (!rspamd_cryptobox_decrypt_nm_inplace(buf, buflen, hdr.nonce,
		rspamd_pubkey_get_nm(rk, key->key),
		hdr.mac, RSPAMD_CRYPTOBOX_MODE_25519)) {
		msg_err ("decryption failed; ip=%s",
			rspamd_inet_address_to_string(session->addr));
		rspamd_pubkey_unref(rk);

		return false;
	}

	memcpy(session->nm, rspamd_pubkey_get_nm(rk, key->key), sizeof(session->nm));
	rspamd_pubkey_unref(rk);

	return true;
}


static bool
rspamd_fuzzy_extensions_from_wire(struct fuzzy_session *s, guchar *buf, gsize buflen)
{
	struct rspamd_fuzzy_cmd_extension *ext, *prev_ext;
	std::uint8_t *storage, *p = buf, *end = buf + buflen;
	gsize st_len = 0, n_ext = 0;

	/* Read number of extensions to allocate array */
	while (p < end) {
		std::uint8_t cmd = *p++;

		if (p < end) {
			if (cmd == RSPAMD_FUZZY_EXT_SOURCE_DOMAIN) {
				/* Next byte is buf length */
				guchar dom_len = *p++;

				if (dom_len <= (end - p)) {
					st_len += dom_len;
					n_ext++;
				}
				else {
					/* Truncation */
					return false;
				}

				p += dom_len;
			}
			else if (cmd == RSPAMD_FUZZY_EXT_SOURCE_IP4) {
				if (end - p >= sizeof(in_addr_t)) {
					n_ext++;
					st_len += sizeof(in_addr_t);
				}
				else {
					/* Truncation */
					return false;
				}

				p += sizeof(in_addr_t);
			}
			else if (cmd == RSPAMD_FUZZY_EXT_SOURCE_IP6) {
				if (end - p >= sizeof(struct in6_addr)) {
					n_ext++;
					st_len += sizeof(struct in6_addr);
				}
				else {
					/* Truncation */
					return false;
				}

				p += sizeof(struct in6_addr);
			}
			else {
				/* Invalid command */
				return false;
			}
		}
		else {
			/* Truncated extension */
			return false;
		}
	}

	if (n_ext > 0) {
		p = buf;
		/*
		 * Memory layout: n_ext of struct rspamd_fuzzy_cmd_extension
		 *                payload for each extension in a continuous data segment
		 */
		storage = (std::uint8_t *)g_malloc(n_ext * sizeof(struct rspamd_fuzzy_cmd_extension) +
						   st_len);

		std::uint8_t *data_buf = storage +
						   n_ext * sizeof(struct rspamd_fuzzy_cmd_extension);
		ext = (struct rspamd_fuzzy_cmd_extension *) storage;

		/* All validation has been done, so we can just go further */
		while (p < end) {
			prev_ext = ext;
			guchar cmd = *p++;

			if (cmd == RSPAMD_FUZZY_EXT_SOURCE_DOMAIN) {
				/* Next byte is buf length */
				guchar dom_len = *p++;
				guchar *dest = data_buf;

				ext->ext = RSPAMD_FUZZY_EXT_SOURCE_DOMAIN;
				ext->next = ext + 1;
				ext->length = dom_len;
				ext->payload = dest;
				memcpy(dest, p, dom_len);
				p += dom_len;
				data_buf += dom_len;
				ext = ext->next;
			}
			else if (cmd == RSPAMD_FUZZY_EXT_SOURCE_IP4) {
				guchar *dest = data_buf;

				ext->ext = RSPAMD_FUZZY_EXT_SOURCE_IP4;
				ext->next = ext + 1;
				ext->length = sizeof(in_addr_t);
				ext->payload = dest;
				memcpy(dest, p, sizeof(in_addr_t));
				p += sizeof(in_addr_t);
				data_buf += sizeof(in_addr_t);
				ext = ext->next;
			}
			else if (cmd == RSPAMD_FUZZY_EXT_SOURCE_IP6) {
				guchar *dest = data_buf;

				ext->ext = RSPAMD_FUZZY_EXT_SOURCE_IP6;
				ext->next = ext + 1;
				ext->length = sizeof(struct in6_addr);
				ext->payload = dest;
				memcpy(dest, p, sizeof(struct in6_addr));
				p += sizeof(struct in6_addr);
				data_buf += sizeof(struct in6_addr);
				ext = ext->next;
			}
			else {
				g_assert_not_reached ();
			}
		}

		/* Last next should be nullptr */
		prev_ext->next = nullptr;

		/* Rewind to the begin */
		ext = (struct rspamd_fuzzy_cmd_extension *) storage;
		s->extensions = ext;
	}

	return true;
}

static bool
rspamd_fuzzy_cmd_from_wire(guchar *buf, unsigned int buflen, struct fuzzy_session *session)
{
	enum rspamd_fuzzy_epoch epoch;
	bool encrypted = false;

	if (buflen < sizeof(struct rspamd_fuzzy_cmd)) {
		msg_debug ("truncated fuzzy command of size %d received", buflen);
		return false;
	}

	/* Now check encryption */

	if (buflen >= sizeof(struct rspamd_fuzzy_encrypted_cmd)) {
		if (memcmp(buf, fuzzy_encrypted_magic, sizeof(fuzzy_encrypted_magic)) == 0) {
			/* Encrypted command */
			encrypted = true;
		}
	}

	if (encrypted) {
		/* Decrypt first */
		if (!rspamd_fuzzy_decrypt_command(session, buf, buflen)) {
			return false;
		}
		else {
			/*
			 * Advance buffer to skip encrypted header.
			 * Note that after rspamd_fuzzy_decrypt_command buf is unencrypted
			 */
			buf += sizeof(struct rspamd_fuzzy_encrypted_req_hdr);
			buflen -= sizeof(struct rspamd_fuzzy_encrypted_req_hdr);
		}
	}

	/* Fill the normal command */
	if (buflen < sizeof(session->cmd.basic)) {
		msg_debug ("truncated normal fuzzy command of size %d received", buflen);
		return false;
	}

	memcpy(&session->cmd.basic, buf, sizeof(session->cmd.basic));
	epoch = rspamd_fuzzy_command_valid(&session->cmd.basic, buflen);

	if (epoch == RSPAMD_FUZZY_EPOCH_MAX) {
		msg_debug ("invalid fuzzy command of size %d received", buflen);
		return false;
	}

	session->epoch = epoch;

	/* Advance buf */
	buf += sizeof(session->cmd.basic);
	buflen -= sizeof(session->cmd.basic);

	if (session->cmd.basic.shingles_count > 0) {
		if (buflen >= sizeof(session->cmd.sgl)) {
			/* Copy the shingles part */
			memcpy(&session->cmd.sgl, buf, sizeof(session->cmd.sgl));
		}
		else {
			/* Truncated stuff */
			msg_debug ("truncated fuzzy shingles command of size %d received", buflen);
			return false;
		}

		buf += sizeof(session->cmd.sgl);
		buflen -= sizeof(session->cmd.sgl);

		if (encrypted) {
			session->cmd_type = CMD_ENCRYPTED_SHINGLE;
		}
		else {
			session->cmd_type = CMD_SHINGLE;
		}
	}
	else {
		if (encrypted) {
			session->cmd_type = CMD_ENCRYPTED_NORMAL;
		}
		else {
			session->cmd_type = CMD_NORMAL;
		}
	}

	if (buflen > 0) {
		/* Process possible extensions */
		if (!rspamd_fuzzy_extensions_from_wire(session, buf, buflen)) {
			msg_debug ("truncated fuzzy shingles command of size %d received", buflen);
			return false;
		}
	}

	return true;
}


static void
fuzzy_session_destroy(gpointer d)
{
	auto *session = (struct fuzzy_session *)d;

	rspamd_inet_address_free(session->addr);
	rspamd_explicit_memzero(session->nm, sizeof(session->nm));
	session->worker->nconns--;

	if (session->ip_stat) {
		REF_RELEASE (session->ip_stat);
	}

	if (session->extensions) {
		g_free(session->extensions);
	}

	g_free(session);
}

#define FUZZY_INPUT_BUFLEN 1024
#ifdef HAVE_RECVMMSG
#define MSGVEC_LEN 16
#else
#define MSGVEC_LEN 1
#endif

union sa_union {
	struct sockaddr sa;
	struct sockaddr_in s4;
	struct sockaddr_in6 s6;
	struct sockaddr_un su;
	struct sockaddr_storage ss;
};

/*
 * Accept new connection and construct task
 */
static void
accept_fuzzy_socket(EV_P_ ev_io *w, int revents)
{
	auto *worker = (struct rspamd_worker *) w->data;
	struct rspamd_fuzzy_storage_ctx *ctx;
	struct fuzzy_session *session;
	gssize r, msg_len;
	std::uint64_t *nerrors;
	struct iovec iovs[MSGVEC_LEN];
	std::uint8_t bufs[MSGVEC_LEN][FUZZY_INPUT_BUFLEN];
	union sa_union peer_sa[MSGVEC_LEN];
	socklen_t salen = sizeof(peer_sa[0]);
#ifdef HAVE_RECVMMSG
#define MSG_FIELD(msg, field) msg.msg_hdr.field
	struct mmsghdr msg[MSGVEC_LEN];
#else
#define MSG_FIELD(msg, field) msg.field
	struct msghdr msg[MSGVEC_LEN];
#endif

	memset(msg, 0, sizeof(*msg) * MSGVEC_LEN);
	ctx = (struct rspamd_fuzzy_storage_ctx *) worker->ctx;

	/* Prepare messages to receive */
	for (int i = 0; i < MSGVEC_LEN; i++) {
		/* Prepare msghdr structs */
		iovs[i].iov_base = bufs[i];
		iovs[i].iov_len = sizeof(bufs[i]);
		MSG_FIELD(msg[i], msg_name) = (void *) &peer_sa[i];
		MSG_FIELD(msg[i], msg_namelen) = salen;
		MSG_FIELD(msg[i], msg_iov) = &iovs[i];
		MSG_FIELD(msg[i], msg_iovlen) = 1;
	}

	/* Got some data */
	if (revents == EV_READ) {
		ev_now_update_if_cheap(ctx->event_loop);
		for (;;) {
#ifdef HAVE_RECVMMSG
			r = recvmmsg (w->fd, msg, MSGVEC_LEN, 0, nullptr);
#else
			r = recvmsg(w->fd, msg, 0);
#endif

			if (r == -1) {
				if (errno == EINTR) {
					continue;
				}
				else if (errno == EAGAIN || errno == EWOULDBLOCK) {

					return;
				}

				msg_err ("got error while reading from socket: %d, %s",
					errno,
					strerror(errno));
				return;
			}

#ifndef HAVE_RECVMMSG
			msg_len = r; /* Save real length in bytes here */
			r = 1; /* Assume that we have received a single message */
#endif

			for (int i = 0; i < r; i++) {
				rspamd_inet_addr_t *client_addr;

				if (MSG_FIELD(msg[i], msg_namelen) >= sizeof(struct sockaddr)) {
					client_addr = rspamd_inet_address_from_sa((struct sockaddr *)MSG_FIELD(msg[i], msg_name),
						MSG_FIELD(msg[i], msg_namelen));
					if (!rspamd_fuzzy_check_client(ctx, client_addr)) {
						/* Disallow forbidden clients silently */
						rspamd_inet_address_free(client_addr);
						continue;
					}
				}
				else {
					client_addr = nullptr;
				}

				/* TODO: modernize this at some point
				 * Will require to modify LRU cache for being used with C++
				 * data structures. Otherwise it is fine...
				 */
				session = (struct fuzzy_session *)g_malloc0(sizeof(*session));
				REF_INIT_RETAIN (session, fuzzy_session_destroy);
				session->worker = worker;
				session->fd = w->fd;
				session->ctx = ctx;
				session->timestamp = ev_now(ctx->event_loop);
				session->addr = client_addr;
				worker->nconns++;

				/* Each message can have its length in case of recvmmsg */
#ifdef HAVE_RECVMMSG
				msg_len = msg[i].msg_len;
#endif

				if (rspamd_fuzzy_cmd_from_wire((unsigned char *)iovs[i].iov_base,
					msg_len, session)) {
					/* Check shingles count sanity */
					rspamd_fuzzy_process_command(session);
				}
				else {
					/* Discard input */
					session->ctx->stat.invalid_requests++;
					msg_debug ("invalid fuzzy command of size %z received", r);

					if (session->addr) {
						nerrors = rspamd_lru_hash_lookup(session->ctx->errors_ips,
							session->addr, -1);

						if (nerrors == nullptr) {
							nerrors = g_malloc(sizeof(*nerrors));
							*nerrors = 1;
							rspamd_lru_hash_insert(session->ctx->errors_ips,
								rspamd_inet_address_copy(session->addr, nullptr),
								nerrors, -1, -1);
						}
						else {
							*nerrors = *nerrors + 1;
						}
					}
				}

				REF_RELEASE (session);
			}
#ifdef HAVE_RECVMMSG
			/* Stop reading as we are using recvmmsg instead of recvmsg */
			break;
#endif
		}
	}
}

static bool
rspamd_fuzzy_storage_periodic_callback(void *ud)
{
	auto *ctx = (struct rspamd_fuzzy_storage_ctx *)ud;

	if (!ctx->updates_pending->empty()) {
		rspamd_fuzzy_process_updates_queue(ctx, local_db_name, false);

		return true;
	}

	return false;
}

static bool
rspamd_fuzzy_storage_sync(struct rspamd_main *rspamd_main,
						  struct rspamd_worker *worker, int fd,
						  int attached_fd,
						  struct rspamd_control_command *cmd,
						  gpointer ud)
{
	auto *ctx = (struct rspamd_fuzzy_storage_ctx *)ud;
	struct rspamd_control_reply rep;

	rep.reply.fuzzy_sync.status = 0;
	rep.type = RSPAMD_CONTROL_FUZZY_SYNC;

	if (ctx->backend && worker->index == 0) {
		rspamd_fuzzy_process_updates_queue(ctx, local_db_name, false);
		rspamd_fuzzy_backend_start_update(ctx->backend, ctx->sync_timeout,
			rspamd_fuzzy_storage_periodic_callback, ctx);
	}

	if (write(fd, &rep, sizeof(rep)) != sizeof(rep)) {
		msg_err ("cannot write reply to the control socket: %s",
			strerror(errno));
	}

	return true;
}

static bool
rspamd_fuzzy_storage_reload(struct rspamd_main *rspamd_main,
							struct rspamd_worker *worker, int fd,
							int attached_fd,
							struct rspamd_control_command *cmd,
							gpointer ud)
{
	auto *ctx = (struct rspamd_fuzzy_storage_ctx *)ud;
	GError *err = nullptr;
	struct rspamd_control_reply rep;

	msg_info ("reloading fuzzy storage after receiving reload command");

	if (ctx->backend) {
		/* Close backend and reopen it one more time */
		rspamd_fuzzy_backend_close(ctx->backend);
	}

	memset(&rep, 0, sizeof(rep));
	rep.type = RSPAMD_CONTROL_RELOAD;

	if ((ctx->backend = rspamd_fuzzy_backend_create(ctx->event_loop,
		worker->cf->options, rspamd_main->cfg,
		&err)) == nullptr) {
		msg_err ("cannot open backend after reload: %e", err);
		rep.reply.reload.status = err->code;
		g_error_free(err);
	}
	else {
		rep.reply.reload.status = 0;
	}

	if (ctx->backend && worker->index == 0) {
		rspamd_fuzzy_backend_start_update(ctx->backend, ctx->sync_timeout,
			rspamd_fuzzy_storage_periodic_callback, ctx);
	}

	if (write(fd, &rep, sizeof(rep)) != sizeof(rep)) {
		msg_err ("cannot write reply to the control socket: %s",
			strerror(errno));
	}

	return true;
}

static ucl_object_t *
rspamd_fuzzy_storage_stat_key(struct fuzzy_key_stat *key_stat)
{
	ucl_object_t *res;

	res = ucl_object_typed_new(UCL_OBJECT);

	ucl_object_insert_key(res, ucl_object_fromint(key_stat->checked),
		"checked", 0, false);
	ucl_object_insert_key(res, ucl_object_fromdouble(key_stat->checked_ctr.mean),
		"checked_per_hour", 0, false);
	ucl_object_insert_key(res, ucl_object_fromint(key_stat->matched),
		"matched", 0, false);
	ucl_object_insert_key(res, ucl_object_fromdouble(key_stat->matched_ctr.mean),
		"matched_per_hour", 0, false);
	ucl_object_insert_key(res, ucl_object_fromint(key_stat->added),
		"added", 0, false);
	ucl_object_insert_key(res, ucl_object_fromint(key_stat->deleted),
		"deleted", 0, false);
	ucl_object_insert_key(res, ucl_object_fromint(key_stat->errors),
		"errors", 0, false);

	return res;
}

static ucl_object_t *
rspamd_fuzzy_stat_to_ucl(struct rspamd_fuzzy_storage_ctx *ctx, bool ip_stat)
{
	struct fuzzy_key_stat *key_stat;
	GHashTableIter it;
	struct fuzzy_key *fuzzy_key;
	ucl_object_t *obj, *keys_obj, *elt, *ip_elt, *ip_cur;
	gpointer k, v;
	int i;
	char keyname[17];

	obj = ucl_object_typed_new(UCL_OBJECT);

	keys_obj = ucl_object_typed_new(UCL_OBJECT);
	g_hash_table_iter_init(&it, ctx->keys);

	while (g_hash_table_iter_next(&it, &k, &v)) {
		fuzzy_key = v;
		key_stat = fuzzy_key->stat;

		if (key_stat) {
			rspamd_snprintf(keyname, sizeof(keyname), "%8bs", k);

			elt = rspamd_fuzzy_storage_stat_key(key_stat);

			if (key_stat->last_ips && ip_stat) {
				i = 0;

				ip_elt = ucl_object_typed_new(UCL_OBJECT);

				while ((i = rspamd_lru_hash_foreach(key_stat->last_ips,
					i, &k, &v)) != -1) {
					ip_cur = rspamd_fuzzy_storage_stat_key(v);
					ucl_object_insert_key(ip_elt, ip_cur,
						rspamd_inet_address_to_string(k), 0, true);
				}
				ucl_object_insert_key(elt, ip_elt, "ips", 0, false);
			}

			ucl_object_insert_key(elt,
				rspamd_keypair_to_ucl(fuzzy_key->key, RSPAMD_KEYPAIR_DUMP_NO_SECRET | RSPAMD_KEYPAIR_DUMP_FLATTENED),
				"keypair", 0, false);
			ucl_object_insert_key(keys_obj, elt, keyname, 0, true);
		}
	}

	ucl_object_insert_key(obj, keys_obj, "keys", 0, false);

	/* Now generic stats */
	ucl_object_insert_key(obj,
		ucl_object_fromint(ctx->stat.fuzzy_hashes),
		"fuzzy_stored",
		0,
		false);
	ucl_object_insert_key(obj,
		ucl_object_fromint(ctx->stat.fuzzy_hashes_expired),
		"fuzzy_expired",
		0,
		false);
	ucl_object_insert_key(obj,
		ucl_object_fromint(ctx->stat.invalid_requests),
		"invalid_requests",
		0,
		false);
	ucl_object_insert_key(obj,
		ucl_object_fromint(ctx->stat.delayed_hashes),
		"delayed_hashes",
		0,
		false);

	if (ctx->errors_ips && ip_stat) {
		i = 0;

		ip_elt = ucl_object_typed_new(UCL_OBJECT);

		while ((i = rspamd_lru_hash_foreach(ctx->errors_ips, i, &k, &v)) != -1) {
			ucl_object_insert_key(ip_elt,
				ucl_object_fromint(*(std::uint64_t *) v),
				rspamd_inet_address_to_string(k), 0, true);
		}

		ucl_object_insert_key(obj,
			ip_elt,
			"errors_ips",
			0,
			false);
	}

	/* Checked by epoch */
	elt = ucl_object_typed_new(UCL_ARRAY);

	for (i = RSPAMD_FUZZY_EPOCH10; i < RSPAMD_FUZZY_EPOCH_MAX; i++) {
		ucl_array_append(elt,
			ucl_object_fromint(ctx->stat.fuzzy_hashes_checked[i]));
	}

	ucl_object_insert_key(obj, elt, "fuzzy_checked", 0, false);

	/* Shingles by epoch */
	elt = ucl_object_typed_new(UCL_ARRAY);

	for (i = RSPAMD_FUZZY_EPOCH10; i < RSPAMD_FUZZY_EPOCH_MAX; i++) {
		ucl_array_append(elt,
			ucl_object_fromint(ctx->stat.fuzzy_shingles_checked[i]));
	}

	ucl_object_insert_key(obj, elt, "fuzzy_shingles", 0, false);

	/* Matched by epoch */
	elt = ucl_object_typed_new(UCL_ARRAY);

	for (i = RSPAMD_FUZZY_EPOCH10; i < RSPAMD_FUZZY_EPOCH_MAX; i++) {
		ucl_array_append(elt,
			ucl_object_fromint(ctx->stat.fuzzy_hashes_found[i]));
	}

	ucl_object_insert_key(obj, elt, "fuzzy_found", 0, false);


	return obj;
}

static int
lua_fuzzy_add_pre_handler(lua_State *L)
{
	struct rspamd_worker *wrk, **pwrk = (struct rspamd_worker **)
		rspamd_lua_check_udata(L, 1, "rspamd{worker}");
	struct rspamd_fuzzy_storage_ctx *ctx;

	if (!pwrk) {
		return luaL_error(L, "invalid arguments, worker + function are expected");
	}

	wrk = *pwrk;

	if (wrk && lua_isfunction (L, 2)) {
		ctx = (struct rspamd_fuzzy_storage_ctx *) wrk->ctx;

		if (ctx->lua_pre_handler_cbref != -1) {
			/* Should not happen */
			luaL_unref(L, LUA_REGISTRYINDEX, ctx->lua_pre_handler_cbref);
		}

		lua_pushvalue(L, 2);
		ctx->lua_pre_handler_cbref = luaL_ref(L, LUA_REGISTRYINDEX);
	}
	else {
		return luaL_error(L, "invalid arguments, worker + function are expected");
	}

	return 0;
}

static int
lua_fuzzy_add_post_handler(lua_State *L)
{
	struct rspamd_worker *wrk, **pwrk = (struct rspamd_worker **)
		rspamd_lua_check_udata(L, 1, "rspamd{worker}");
	struct rspamd_fuzzy_storage_ctx *ctx;

	if (!pwrk) {
		return luaL_error(L, "invalid arguments, worker + function are expected");
	}

	wrk = *pwrk;

	if (wrk && lua_isfunction (L, 2)) {
		ctx = (struct rspamd_fuzzy_storage_ctx *) wrk->ctx;

		if (ctx->lua_post_handler_cbref != -1) {
			/* Should not happen */
			luaL_unref(L, LUA_REGISTRYINDEX, ctx->lua_post_handler_cbref);
		}

		lua_pushvalue(L, 2);
		ctx->lua_post_handler_cbref = luaL_ref(L, LUA_REGISTRYINDEX);
	}
	else {
		return luaL_error(L, "invalid arguments, worker + function are expected");
	}

	return 0;
}

static int
lua_fuzzy_add_blacklist_handler(lua_State *L)
{
	struct rspamd_worker *wrk, **pwrk = (struct rspamd_worker **)
		rspamd_lua_check_udata(L, 1, "rspamd{worker}");
	struct rspamd_fuzzy_storage_ctx *ctx;

	if (!pwrk) {
		return luaL_error(L, "invalid arguments, worker + function are expected");
	}

	wrk = *pwrk;

	if (wrk && lua_isfunction (L, 2)) {
		ctx = (struct rspamd_fuzzy_storage_ctx *) wrk->ctx;

		if (ctx->lua_blacklist_cbref != -1) {
			/* Should not happen */
			luaL_unref(L, LUA_REGISTRYINDEX, ctx->lua_blacklist_cbref);
		}

		lua_pushvalue(L, 2);
		ctx->lua_blacklist_cbref = luaL_ref(L, LUA_REGISTRYINDEX);
	}
	else {
		return luaL_error(L, "invalid arguments, worker + function are expected");
	}

	return 0;
}

static bool
rspamd_fuzzy_storage_stat(struct rspamd_main *rspamd_main,
						  struct rspamd_worker *worker, int fd,
						  int attached_fd,
						  struct rspamd_control_command *cmd,
						  gpointer ud)
{
	struct rspamd_fuzzy_storage_ctx *ctx = ud;
	struct rspamd_control_reply rep;
	ucl_object_t *obj;
	struct ucl_emitter_functions *emit_subr;
	guchar fdspace[CMSG_SPACE(sizeof(int))];
	struct iovec iov;
	struct msghdr msg;
	struct cmsghdr *cmsg;

	int outfd = -1;
	char tmppath[PATH_MAX];

	memset(&rep, 0, sizeof(rep));
	rep.type = RSPAMD_CONTROL_FUZZY_STAT;

	rspamd_snprintf(tmppath, sizeof(tmppath), "%s%c%s-XXXXXXXXXX",
		rspamd_main->cfg->temp_dir, G_DIR_SEPARATOR, "fuzzy-stat");

	if ((outfd = mkstemp(tmppath)) == -1) {
		rep.reply.fuzzy_stat.status = errno;
		msg_info_main ("cannot make temporary stat file for fuzzy stat: %s",
			strerror(errno));
	}
	else {
		rep.reply.fuzzy_stat.status = 0;

		memcpy(rep.reply.fuzzy_stat.storage_id,
			rspamd_fuzzy_backend_id(ctx->backend),
			sizeof(rep.reply.fuzzy_stat.storage_id));

		obj = rspamd_fuzzy_stat_to_ucl(ctx, true);
		emit_subr = ucl_object_emit_fd_funcs(outfd);
		ucl_object_emit_full(obj, UCL_EMIT_JSON_COMPACT, emit_subr, nullptr);
		ucl_object_emit_funcs_free(emit_subr);
		ucl_object_unref(obj);
		/* Rewind output file */
		close(outfd);
		outfd = open(tmppath, O_RDONLY);
		unlink(tmppath);
	}

	/* Now we can send outfd and status message */
	memset(&msg, 0, sizeof(msg));

	/* Attach fd to the message */
	if (outfd != -1) {
		memset(fdspace, 0, sizeof(fdspace));
		msg.msg_control = fdspace;
		msg.msg_controllen = sizeof(fdspace);
		cmsg = CMSG_FIRSTHDR (&msg);

		if (cmsg) {
			cmsg->cmsg_level = SOL_SOCKET;
			cmsg->cmsg_type = SCM_RIGHTS;
			cmsg->cmsg_len = CMSG_LEN (sizeof(int));
			memcpy(CMSG_DATA (cmsg), &outfd, sizeof(int));
		}
	}

	iov.iov_base = &rep;
	iov.iov_len = sizeof(rep);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;

	if (sendmsg(fd, &msg, 0) == -1) {
		msg_err_main ("cannot send fuzzy stat: %s", strerror(errno));
	}

	if (outfd != -1) {
		close(outfd);
	}

	return true;
}

static bool
fuzzy_parse_keypair(rspamd_mempool_t *pool,
					const ucl_object_t *obj,
					gpointer ud,
					struct rspamd_rcl_section *section,
					GError **err)
{
	struct rspamd_rcl_struct_parser *pd = ud;
	struct rspamd_fuzzy_storage_ctx *ctx;
	struct rspamd_cryptobox_keypair *kp;
	struct fuzzy_key_stat *keystat;
	struct fuzzy_key *key;
	const ucl_object_t *cur;
	const guchar *pk;
	ucl_object_iter_t it = nullptr;
	bool ret;

	ctx = pd->user_struct;
	pd->offset = G_STRUCT_OFFSET (struct rspamd_fuzzy_storage_ctx, default_keypair);

	/*
	 * Single key
	 */
	if (ucl_object_type(obj) == UCL_STRING || ucl_object_type(obj)
											  == UCL_OBJECT) {
		ret = rspamd_rcl_parse_struct_keypair(pool, obj, pd, section, err);

		if (!ret) {
			return ret;
		}

		/* Insert key to the hash table */
		kp = ctx->default_keypair;

		if (kp == nullptr) {
			return false;
		}

		if (rspamd_keypair_alg(kp) != RSPAMD_CRYPTOBOX_MODE_25519 ||
			rspamd_keypair_type(kp) != RSPAMD_KEYPAIR_KEX) {
			return false;
		}

		key = g_malloc0(sizeof(*key));
		key->key = kp;
		keystat = g_malloc0(sizeof(*keystat));
		REF_INIT_RETAIN (keystat, fuzzy_key_stat_dtor);
		/* Hash of ip -> fuzzy_key_stat */
		keystat->last_ips = rspamd_lru_hash_new_full(1024,
			(GDestroyNotify) rspamd_inet_address_free,
			fuzzy_key_stat_unref,
			rspamd_inet_address_hash, rspamd_inet_address_equal);
		key->stat = keystat;
		pk = rspamd_keypair_component(kp, RSPAMD_KEYPAIR_COMPONENT_PK,
			nullptr);
		keystat->keypair = rspamd_keypair_ref(kp);
		/* We map entries by pubkey in binary form for speed lookup */
		g_hash_table_insert(ctx->keys, (gpointer) pk, key);
		ctx->default_key = key;

		const ucl_object_t *extensions = rspamd_keypair_get_extensions(kp);

		if (extensions) {
			const ucl_object_t *forbidden_ids = ucl_object_lookup(extensions, "forbidden_ids");

			if (forbidden_ids && ucl_object_type(forbidden_ids) == UCL_ARRAY) {
				key->forbidden_ids = kh_init(fuzzy_key_forbidden_ids);
				while ((cur = ucl_object_iterate (forbidden_ids, &it, true)) != nullptr) {
					if (ucl_object_type(cur) == UCL_INT || ucl_object_type(cur) == UCL_FLOAT) {
						int id = ucl_object_toint(cur);
						int r;

						kh_put(fuzzy_key_forbidden_ids, key->forbidden_ids, id, &r);
					}
				}
			}
		}

		msg_debug_pool_check("loaded keypair %*xs", 8, pk);
	}
	else if (ucl_object_type(obj) == UCL_ARRAY) {
		while ((cur = ucl_object_iterate (obj, &it, true)) != nullptr) {
			if (!fuzzy_parse_keypair(pool, cur, pd, section, err)) {
				msg_err_pool ("cannot parse keypair");
			}
		}
	}

	return true;
}

gpointer
init_fuzzy(struct rspamd_config *cfg)
{
	struct rspamd_fuzzy_storage_ctx *ctx;
	GQuark type;

	type = g_quark_try_string("fuzzy");

	ctx = rspamd_mempool_alloc0 (cfg->cfg_pool,
		sizeof(struct rspamd_fuzzy_storage_ctx));

	ctx->magic = rspamd_fuzzy_storage_magic;
	ctx->sync_timeout = DEFAULT_SYNC_TIMEOUT;
	ctx->keypair_cache_size = DEFAULT_KEYPAIR_CACHE_SIZE;
	ctx->lua_pre_handler_cbref = -1;
	ctx->lua_post_handler_cbref = -1;
	ctx->lua_blacklist_cbref = -1;
	ctx->keys = g_hash_table_new_full(fuzzy_kp_hash, fuzzy_kp_equal,
		nullptr, fuzzy_key_dtor);
	rspamd_mempool_add_destructor (cfg->cfg_pool,
		(rspamd_mempool_destruct_t) g_hash_table_unref, ctx->keys);
	ctx->errors_ips = rspamd_lru_hash_new_full(1024,
		(GDestroyNotify) rspamd_inet_address_free, g_free,
		rspamd_inet_address_hash, rspamd_inet_address_equal);
	rspamd_mempool_add_destructor (cfg->cfg_pool,
		(rspamd_mempool_destruct_t) rspamd_lru_hash_destroy, ctx->errors_ips);
	ctx->cfg = cfg;
	ctx->updates_maxfail = DEFAULT_UPDATES_MAXFAIL;
	ctx->leaky_bucket_mask = DEFAULT_BUCKET_MASK;
	ctx->leaky_bucket_ttl = DEFAULT_BUCKET_TTL;
	ctx->max_buckets = DEFAULT_MAX_BUCKETS;
	ctx->leaky_bucket_burst = NAN;
	ctx->leaky_bucket_rate = NAN;
	ctx->delay = NAN;

	rspamd_rcl_register_worker_option(cfg,
		type,
		"sync",
		rspamd_rcl_parse_struct_time,
		ctx,
		G_STRUCT_OFFSET (struct rspamd_fuzzy_storage_ctx,
			sync_timeout),
		RSPAMD_CL_FLAG_TIME_FLOAT,
		"Time to perform database sync, default: "
		G_STRINGIFY (DEFAULT_SYNC_TIMEOUT) " seconds");

	rspamd_rcl_register_worker_option(cfg,
		type,
		"expire",
		rspamd_rcl_parse_struct_time,
		ctx,
		G_STRUCT_OFFSET (struct rspamd_fuzzy_storage_ctx,
			expire),
		RSPAMD_CL_FLAG_TIME_FLOAT,
		"Default expire time for hashes, default: "
		G_STRINGIFY (DEFAULT_EXPIRE) " seconds");

	rspamd_rcl_register_worker_option(cfg,
		type,
		"delay",
		rspamd_rcl_parse_struct_time,
		ctx,
		G_STRUCT_OFFSET (struct rspamd_fuzzy_storage_ctx,
			delay),
		RSPAMD_CL_FLAG_TIME_FLOAT,
		"Default delay time for hashes, default: not enabled");

	rspamd_rcl_register_worker_option(cfg,
		type,
		"allow_update",
		rspamd_rcl_parse_struct_ucl,
		ctx,
		G_STRUCT_OFFSET (struct rspamd_fuzzy_storage_ctx, update_map),
		0,
		"Allow modifications from the following IP addresses");

	rspamd_rcl_register_worker_option(cfg,
		type,
		"allow_update_keys",
		rspamd_rcl_parse_struct_ucl,
		ctx,
		G_STRUCT_OFFSET (struct rspamd_fuzzy_storage_ctx, update_keys_map),
		0,
		"Allow modifications for those using specific public keys");

	rspamd_rcl_register_worker_option(cfg,
		type,
		"delay_whitelist",
		rspamd_rcl_parse_struct_ucl,
		ctx,
		G_STRUCT_OFFSET (struct rspamd_fuzzy_storage_ctx, delay_whitelist_map),
		0,
		"Disable delay check for the following IP addresses");

	rspamd_rcl_register_worker_option(cfg,
		type,
		"keypair",
		fuzzy_parse_keypair,
		ctx,
		0,
		RSPAMD_CL_FLAG_MULTIPLE,
		"Encryption keypair (can be repeated for different keys)");

	rspamd_rcl_register_worker_option(cfg,
		type,
		"keypair_cache_size",
		rspamd_rcl_parse_struct_integer,
		ctx,
		G_STRUCT_OFFSET (struct rspamd_fuzzy_storage_ctx,
			keypair_cache_size),
		RSPAMD_CL_FLAG_UINT,
		"Size of keypairs cache, default: "
		G_STRINGIFY (DEFAULT_KEYPAIR_CACHE_SIZE));

	rspamd_rcl_register_worker_option(cfg,
		type,
		"encrypted_only",
		rspamd_rcl_parse_struct_boolean,
		ctx,
		G_STRUCT_OFFSET (struct rspamd_fuzzy_storage_ctx, encrypted_only),
		0,
		"Allow encrypted requests only (and forbid all unknown keys or plaintext requests)");
	rspamd_rcl_register_worker_option(cfg,
		type,
		"dedicated_update_worker",
		rspamd_rcl_parse_struct_boolean,
		ctx,
		G_STRUCT_OFFSET (struct rspamd_fuzzy_storage_ctx, dedicated_update_worker),
		0,
		"Use worker 0 for updates only");

	rspamd_rcl_register_worker_option(cfg,
		type,
		"read_only",
		rspamd_rcl_parse_struct_boolean,
		ctx,
		G_STRUCT_OFFSET (struct rspamd_fuzzy_storage_ctx, read_only),
		0,
		"Work in read only mode");


	rspamd_rcl_register_worker_option(cfg,
		type,
		"blocked",
		rspamd_rcl_parse_struct_ucl,
		ctx,
		G_STRUCT_OFFSET (struct rspamd_fuzzy_storage_ctx, blocked_map),
		0,
		"Block requests from specific networks");


	rspamd_rcl_register_worker_option(cfg,
		type,
		"updates_maxfail",
		rspamd_rcl_parse_struct_integer,
		ctx,
		G_STRUCT_OFFSET (struct rspamd_fuzzy_storage_ctx, updates_maxfail),
		RSPAMD_CL_FLAG_UINT,
		"Maximum number of updates to be failed before discarding");
	rspamd_rcl_register_worker_option(cfg,
		type,
		"skip_hashes",
		rspamd_rcl_parse_struct_ucl,
		ctx,
		G_STRUCT_OFFSET (struct rspamd_fuzzy_storage_ctx, skip_map),
		0,
		"Skip specific hashes from the map");

	/* Ratelimits */
	rspamd_rcl_register_worker_option(cfg,
		type,
		"ratelimit_whitelist",
		rspamd_rcl_parse_struct_ucl,
		ctx,
		G_STRUCT_OFFSET (struct rspamd_fuzzy_storage_ctx, ratelimit_whitelist_map),
		0,
		"Skip specific addresses from rate limiting");
	rspamd_rcl_register_worker_option(cfg,
		type,
		"ratelimit_max_buckets",
		rspamd_rcl_parse_struct_integer,
		ctx,
		G_STRUCT_OFFSET (struct rspamd_fuzzy_storage_ctx, max_buckets),
		RSPAMD_CL_FLAG_UINT,
		"Maximum number of leaky buckets (default: " G_STRINGIFY(DEFAULT_MAX_BUCKETS) ")");
	rspamd_rcl_register_worker_option(cfg,
		type,
		"ratelimit_network_mask",
		rspamd_rcl_parse_struct_integer,
		ctx,
		G_STRUCT_OFFSET (struct rspamd_fuzzy_storage_ctx, leaky_bucket_mask),
		RSPAMD_CL_FLAG_UINT,
		"Network mask to apply for IPv4 rate addresses (default: " G_STRINGIFY(DEFAULT_BUCKET_MASK) ")");
	rspamd_rcl_register_worker_option(cfg,
		type,
		"ratelimit_bucket_ttl",
		rspamd_rcl_parse_struct_time,
		ctx,
		G_STRUCT_OFFSET (struct rspamd_fuzzy_storage_ctx, leaky_bucket_ttl),
		RSPAMD_CL_FLAG_TIME_INTEGER,
		"Time to live for ratelimit element (default: " G_STRINGIFY(DEFAULT_BUCKET_TTL) ")");
	rspamd_rcl_register_worker_option(cfg,
		type,
		"ratelimit_rate",
		rspamd_rcl_parse_struct_double,
		ctx,
		G_STRUCT_OFFSET (struct rspamd_fuzzy_storage_ctx, leaky_bucket_rate),
		0,
		"Leak rate in requests per second");
	rspamd_rcl_register_worker_option(cfg,
		type,
		"ratelimit_burst",
		rspamd_rcl_parse_struct_double,
		ctx,
		G_STRUCT_OFFSET (struct rspamd_fuzzy_storage_ctx, leaky_bucket_burst),
		0,
		"Peak value for ratelimit bucket");
	rspamd_rcl_register_worker_option(cfg,
		type,
		"ratelimit_log_only",
		rspamd_rcl_parse_struct_boolean,
		ctx,
		G_STRUCT_OFFSET (struct rspamd_fuzzy_storage_ctx, ratelimit_log_only),
		0,
		"Don't really ban on ratelimit reaching, just log");


	return ctx;
}

static void
rspamd_fuzzy_peer_io(EV_P_ ev_io *w, int revents)
{
	struct fuzzy_peer_cmd cmd;
	struct rspamd_fuzzy_storage_ctx *ctx =
		(struct rspamd_fuzzy_storage_ctx *) w->data;
	gssize r;

	for (;;) {
		r = read(w->fd, &cmd, sizeof(cmd));

		if (r != sizeof(cmd)) {
			if (errno == EINTR) {
				continue;
			}
			if (errno != EAGAIN) {
				msg_err ("cannot read command from peers: %s", strerror(errno));
			}

			break;
		}
		else {
			g_array_append_val (ctx->updates_pending, cmd);
		}
	}
}

static void
fuzzy_peer_rep(struct rspamd_worker *worker,
			   struct rspamd_srv_reply *rep, int rep_fd,
			   gpointer ud)
{
	struct rspamd_fuzzy_storage_ctx *ctx = ud;
	GList *cur;
	struct rspamd_worker_listen_socket *ls;
	struct rspamd_worker_accept_event *ac_ev;

	ctx->peer_fd = rep_fd;

	if (rep_fd == -1) {
		msg_err ("cannot receive peer fd from the main process");
		exit(EXIT_FAILURE);
	}
	else {
		rspamd_socket_nonblocking(rep_fd);
	}

	msg_info ("got peer fd reply from the main process");

	/* Start listening */
	cur = worker->cf->listen_socks;
	while (cur) {
		ls = cur->data;

		if (ls->fd != -1) {
			msg_info ("start listening on %s",
				rspamd_inet_address_to_string_pretty(ls->addr));

			if (ls->type == RSPAMD_WORKER_SOCKET_UDP) {
				ac_ev = g_malloc0(sizeof(*ac_ev));
				ac_ev->accept_ev.data = worker;
				ac_ev->event_loop = ctx->event_loop;
				ev_io_init (&ac_ev->accept_ev, accept_fuzzy_socket, ls->fd,
					EV_READ);
				ev_io_start(ctx->event_loop, &ac_ev->accept_ev);
				DL_APPEND (worker->accept_events, ac_ev);
			}
			else {
				/* We allow TCP listeners only for a update worker */
				g_assert_not_reached ();
			}
		}

		cur = g_list_next (cur);
	}

	if (ctx->peer_fd != -1) {
		if (worker->index == 0) {
			/* Listen for peer requests */
			shutdown(ctx->peer_fd, SHUT_WR);
			ctx->peer_ev.data = ctx;
			ev_io_init (&ctx->peer_ev, rspamd_fuzzy_peer_io, ctx->peer_fd, EV_READ);
			ev_io_start(ctx->event_loop, &ctx->peer_ev);
		}
		else {
			shutdown(ctx->peer_fd, SHUT_RD);
		}
	}
}

/*
 * Start worker process
 */
__attribute__((noreturn))
void
start_fuzzy(struct rspamd_worker *worker)
{
	struct rspamd_fuzzy_storage_ctx *ctx = worker->ctx;
	GError *err = nullptr;
	struct rspamd_srv_command srv_cmd;
	struct rspamd_config *cfg = worker->srv->cfg;

	g_assert (rspamd_worker_check_context(worker->ctx, rspamd_fuzzy_storage_magic));
	ctx->event_loop = rspamd_prepare_worker(worker,
		"fuzzy",
		nullptr);
	ctx->peer_fd = -1;
	ctx->worker = worker;
	ctx->cfg = worker->srv->cfg;
	ctx->resolver = rspamd_dns_resolver_init(worker->srv->logger,
		ctx->event_loop,
		worker->srv->cfg);
	rspamd_upstreams_library_config(worker->srv->cfg, ctx->cfg->ups_ctx,
		ctx->event_loop, ctx->resolver->r);
	/* Since this worker uses maps it needs a valid HTTP context */
	ctx->http_ctx = rspamd_http_context_create(ctx->cfg, ctx->event_loop,
		ctx->cfg->ups_ctx);

	if (ctx->keypair_cache_size > 0) {
		/* Create keypairs cache */
		ctx->keypair_cache = rspamd_keypair_cache_new(ctx->keypair_cache_size);
	}


	if ((ctx->backend = rspamd_fuzzy_backend_create(ctx->event_loop,
		worker->cf->options, cfg, &err)) == nullptr) {
		msg_err ("cannot open backend: %e", err);
		if (err) {
			g_error_free(err);
		}
		exit(EXIT_SUCCESS);
	}

	rspamd_fuzzy_backend_count(ctx->backend, fuzzy_count_callback, ctx);


	if (worker->index == 0) {
		ctx->updates_pending = g_array_sized_new(false, false,
			sizeof(struct fuzzy_peer_cmd), 1024);
		rspamd_fuzzy_backend_start_update(ctx->backend, ctx->sync_timeout,
			rspamd_fuzzy_storage_periodic_callback, ctx);

		if (ctx->dedicated_update_worker && worker->cf->count > 1) {
			msg_info_config ("stop serving clients request in dedicated update mode");
			rspamd_worker_stop_accept(worker);

			GList *cur = worker->cf->listen_socks;

			while (cur) {
				struct rspamd_worker_listen_socket *ls =
					(struct rspamd_worker_listen_socket *) cur->data;

				if (ls->fd != -1) {
					close(ls->fd);
				}

				cur = g_list_next (cur);
			}
		}
	}

	ctx->stat_ev.data = ctx;
	ev_timer_init (&ctx->stat_ev, rspamd_fuzzy_stat_callback, ctx->sync_timeout,
		ctx->sync_timeout);
	ev_timer_start(ctx->event_loop, &ctx->stat_ev);
	/* Register custom reload and stat commands for the control socket */
	rspamd_control_worker_add_cmd_handler(worker, RSPAMD_CONTROL_RELOAD,
		rspamd_fuzzy_storage_reload, ctx);
	rspamd_control_worker_add_cmd_handler(worker, RSPAMD_CONTROL_FUZZY_STAT,
		rspamd_fuzzy_storage_stat, ctx);
	rspamd_control_worker_add_cmd_handler(worker, RSPAMD_CONTROL_FUZZY_SYNC,
		rspamd_fuzzy_storage_sync, ctx);


	if (ctx->update_map != nullptr) {
		rspamd_config_radix_from_ucl(worker->srv->cfg, ctx->update_map,
			"Allow fuzzy updates from specified addresses",
			&ctx->update_ips, nullptr, worker, "fuzzy update");
	}

	if (ctx->update_keys_map != nullptr) {
		struct rspamd_map *m;

		if ((m = rspamd_map_add_from_ucl(worker->srv->cfg, ctx->update_keys_map,
			"Allow fuzzy updates from specified public keys",
			rspamd_kv_list_read,
			rspamd_kv_list_fin,
			rspamd_kv_list_dtor,
			(void **) &ctx->update_keys, worker, RSPAMD_MAP_DEFAULT)) == nullptr) {
			msg_warn_config ("cannot load allow keys map from %s",
				ucl_object_tostring(ctx->update_keys_map));
		}
		else {
			m->active_http = true;
		}
	}

	if (ctx->skip_map != nullptr) {
		struct rspamd_map *m;

		if ((m = rspamd_map_add_from_ucl(cfg, ctx->skip_map,
			"Skip hashes",
			rspamd_kv_list_read,
			rspamd_kv_list_fin,
			rspamd_kv_list_dtor,
			(void **) &ctx->skip_hashes,
			worker, RSPAMD_MAP_DEFAULT)) == nullptr) {
			msg_warn_config ("cannot load hashes list from %s",
				ucl_object_tostring(ctx->skip_map));
		}
		else {
			m->active_http = true;
		}
	}

	if (ctx->blocked_map != nullptr) {
		rspamd_config_radix_from_ucl(worker->srv->cfg, ctx->blocked_map,
			"Block fuzzy requests from the specific IPs",
			&ctx->blocked_ips,
			nullptr,
			worker, "fuzzy blocked");
	}

	/* Create radix trees */
	if (ctx->ratelimit_whitelist_map != nullptr) {
		rspamd_config_radix_from_ucl(worker->srv->cfg, ctx->ratelimit_whitelist_map,
			"Skip ratelimits from specific ip addresses/networks",
			&ctx->ratelimit_whitelist,
			nullptr,
			worker, "fuzzy ratelimit whitelist");
	}

	if (!isnan(ctx->delay) && ctx->delay_whitelist_map != nullptr) {
		rspamd_config_radix_from_ucl(worker->srv->cfg, ctx->delay_whitelist_map,
			"Skip delay from the following ips",
			&ctx->delay_whitelist, nullptr, worker,
			"fuzzy delayed whitelist");
	}

	/* Ratelimits */
	if (!isnan(ctx->leaky_bucket_rate) && !isnan(ctx->leaky_bucket_burst)) {
		ctx->ratelimit_buckets = rspamd_lru_hash_new_full(ctx->max_buckets,
			nullptr, fuzzy_rl_bucket_free,
			rspamd_inet_address_hash, rspamd_inet_address_equal);
	}

	/* Maps events */
	ctx->resolver = rspamd_dns_resolver_init(worker->srv->logger,
		ctx->event_loop,
		worker->srv->cfg);
	rspamd_map_watch(worker->srv->cfg, ctx->event_loop,
		ctx->resolver, worker, RSPAMD_MAP_WATCH_WORKER);

	/* Get peer pipe */
	memset(&srv_cmd, 0, sizeof(srv_cmd));
	srv_cmd.type = RSPAMD_SRV_SOCKETPAIR;
	srv_cmd.cmd.spair.af = SOCK_DGRAM;
	srv_cmd.cmd.spair.pair_num = worker->index;
	memset(srv_cmd.cmd.spair.pair_id, 0, sizeof(srv_cmd.cmd.spair.pair_id));
	/* 6 bytes of id (including \0) and bind_conf id */
	G_STATIC_ASSERT (sizeof(srv_cmd.cmd.spair.pair_id) >=
					 sizeof("fuzzy") + sizeof(std::uint64_t));

	memcpy(srv_cmd.cmd.spair.pair_id, "fuzzy", sizeof("fuzzy"));

	/* Distinguish workers from each others... */
	if (worker->cf->bind_conf && worker->cf->bind_conf->bind_line) {
		std::uint64_t bind_hash = rspamd_cryptobox_fast_hash(worker->cf->bind_conf->bind_line,
			strlen(worker->cf->bind_conf->bind_line), 0xdeadbabe);

		/* 8 more bytes */
		memcpy(srv_cmd.cmd.spair.pair_id + sizeof("fuzzy"), &bind_hash,
			sizeof(bind_hash));
	}

	rspamd_srv_send_command(worker, ctx->event_loop, &srv_cmd, -1,
		fuzzy_peer_rep, ctx);

	/*
	 * Extra fields available for this particular worker
	 */
	luaL_Reg fuzzy_lua_reg = {
		.name = "add_fuzzy_pre_handler",
		.func = lua_fuzzy_add_pre_handler,
	};
	rspamd_lua_add_metamethod(ctx->cfg->lua_state, "rspamd{worker}", &fuzzy_lua_reg);
	fuzzy_lua_reg = (luaL_Reg) {
		.name = "add_fuzzy_post_handler",
		.func = lua_fuzzy_add_post_handler,
	};
	rspamd_lua_add_metamethod(ctx->cfg->lua_state, "rspamd{worker}", &fuzzy_lua_reg);
	fuzzy_lua_reg = (luaL_Reg) {
		.name = "add_fuzzy_blacklist_handler",
		.func = lua_fuzzy_add_blacklist_handler,
	};
	rspamd_lua_add_metamethod(ctx->cfg->lua_state, "rspamd{worker}", &fuzzy_lua_reg);

	rspamd_lua_run_postloads(ctx->cfg->lua_state, ctx->cfg, ctx->event_loop,
		worker);

	ev_loop(ctx->event_loop, 0);
	rspamd_worker_block_signals();

	if (ctx->peer_fd != -1) {
		if (worker->index == 0) {
			ev_io_stop(ctx->event_loop, &ctx->peer_ev);
		}
		close(ctx->peer_fd);
	}

	if (worker->index == 0 && ctx->updates_pending->len > 0) {

		msg_info_config ("start another event loop to sync fuzzy storage");

		if (rspamd_fuzzy_process_updates_queue(ctx, local_db_name, true)) {
			ev_loop(ctx->event_loop, 0);
			msg_info_config ("sync cycle is done");
		}
		else {
			msg_info_config ("no need to sync");
		}
	}

	rspamd_fuzzy_backend_close(ctx->backend);

	if (worker->index == 0) {
		g_array_free(ctx->updates_pending, true);
		ctx->updates_pending = nullptr;
	}

	if (ctx->keypair_cache) {
		rspamd_keypair_cache_destroy(ctx->keypair_cache);
	}

	if (ctx->ratelimit_buckets) {
		rspamd_lru_hash_destroy(ctx->ratelimit_buckets);
	}

	if (ctx->lua_pre_handler_cbref != -1) {
		luaL_unref(ctx->cfg->lua_state, LUA_REGISTRYINDEX, ctx->lua_pre_handler_cbref);
	}

	if (ctx->lua_post_handler_cbref != -1) {
		luaL_unref(ctx->cfg->lua_state, LUA_REGISTRYINDEX, ctx->lua_post_handler_cbref);
	}

	if (ctx->lua_blacklist_cbref != -1) {
		luaL_unref(ctx->cfg->lua_state, LUA_REGISTRYINDEX, ctx->lua_blacklist_cbref);
	}

	REF_RELEASE (ctx->cfg);
	rspamd_log_close(worker->srv->logger);
	rspamd_unset_crash_handler(worker->srv);

	exit(EXIT_SUCCESS);
}

}
