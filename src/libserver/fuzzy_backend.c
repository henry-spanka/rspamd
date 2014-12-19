/* Copyright (c) 2014, Vsevolod Stakhov
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *       * Redistributions of source code must retain the above copyright
 *         notice, this list of conditions and the following disclaimer.
 *       * Redistributions in binary form must reproduce the above copyright
 *         notice, this list of conditions and the following disclaimer in the
 *         documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "main.h"
#include "fuzzy_backend.h"
#include "fuzzy_storage.h"

#include <sqlite3.h>

/* Magic sequence for hashes file */
#define FUZZY_FILE_MAGIC "rsh"

struct rspamd_fuzzy_backend {
	sqlite3 *db;
	const char *path;
};

static GQuark
rspamd_fuzzy_backend_quark(void)
{
	return g_quark_from_static_string ("fuzzy-storage-backend");
}

struct rspamd_fuzzy_backend*
rspamd_fuzzy_backend_open (const gchar *path, GError **err)
{
	gchar *dir, header[4];
	gint fd, r;
	struct rspamd_fuzzy_backend *res;

	/* First of all we check path for existence */
	dir = g_path_get_dirname (path);
	if (dir == NULL) {
		g_set_error (err, rspamd_fuzzy_backend_quark (),
				errno, "Cannot get directory name for %s: %s", path,
				strerror (errno));
		return NULL;
	}

	if (access (path, W_OK) == -1 && access (dir, W_OK) == -1) {
		g_set_error (err, rspamd_fuzzy_backend_quark (),
				errno, "Cannot access directory %s to create database: %s",
				dir, strerror (errno));
		g_free (dir);

		return NULL;
	}

	g_free (dir);
	if ((fd = open (path, O_RDONLY)) == -1) {
		g_set_error (err, rspamd_fuzzy_backend_quark (),
				errno, "Cannot open file %s: %s",
				path, strerror (errno));

		return NULL;
	}

	/* Check for legacy format */
	if ((r = read (fd, header, sizeof (header))) == sizeof (header)) {
		if (memcmp (header, FUZZY_FILE_MAGIC, sizeof (header) - 1) == 0) {
			msg_info ("Trying to convert old fuzzy database");
			if (!rspamd_fuzzy_backend_convert (fd, err)) {
				close (fd);
				return NULL;
			}
		}
		close (fd);
	}

	/* Open database */
	if ((res = rspamd_fuzzy_backend_open_db (path, err)) == NULL) {
		GError *tmp = NULL;

		if ((res = rspamd_fuzzy_backend_create_db (path, tmp)) == NULL) {
			g_clear_error (err);
			g_propagate_error (err, tmp);
			return NULL;
		}
		g_clear_error (err);
	}

	return res;
}

struct rspamd_fuzzy_reply
rspamd_fuzzy_backend_check (struct rspamd_fuzzy_backend *backend,
		const struct rspamd_fuzzy_cmd *cmd)
{

}

gboolean
rspamd_fuzzy_backend_add (struct rspamd_fuzzy_backend *backend,
		const struct rspamd_fuzzy_cmd *cmd)
{

}


gboolean
rspamd_fuzzy_backend_del (struct rspamd_fuzzy_backend *backend,
		const struct rspamd_fuzzy_cmd *cmd)
{

}

gboolean
rspamd_fuzzy_backend_sync (struct rspamd_fuzzy_backend *backend)
{

}


void
rspamd_fuzzy_backend_close (struct rspamd_fuzzy_backend *backend)
{

}
