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
#include "lua_common.h"
#include "message.h"
#include "libserver/html/html.h"
#include "libserver/html/html_tag.hxx"
#include "images.h"

#include <contrib/robin-hood/robin_hood.h>
#include <frozen/string.h>
#include <frozen/unordered_map.h>

/***
 * @module rspamd_html
 * This module provides different methods to access HTML tags. To get HTML context
 * from an HTML part you could use method `part:get_html()`
 * @example
rspamd_config.R_EMPTY_IMAGE = function(task)
  local tp = task:get_text_parts() -- get text parts in a message

  for _,p in ipairs(tp) do -- iterate over text parts array using `ipairs`
    if p:is_html() then -- if the current part is html part
      local hc = p:get_html() -- we get HTML context
      local len = p:get_length() -- and part's length

      if len < 50 then -- if we have a part that has less than 50 bytes of text
        local images = hc:get_images() -- then we check for HTML images

        if images then -- if there are images
          for _,i in ipairs(images) do -- then iterate over images in the part
            if i['height'] + i['width'] >= 400 then -- if we have a large image
              return true -- add symbol
            end
          end
        end
      end
    end
  end
end
 */

/***
 * @method html:has_tag(name)
 * Checks if a specified tag `name` is presented in a part
 * @param {string} name name of tag to check
 * @return {boolean} `true` if the tag exists in HTML tree
 */
LUA_FUNCTION_DEF (html, has_tag);

/***
 * @method html:check_property(name)
 * Checks if the HTML has a specific property. Here is the list of available properties:
 *
 * - `no_html` - no html tag presented
 * - `bad_element` - part has some broken elements
 * - `xml` - part is xhtml
 * - `unknown_element` - part has some unknown elements
 * - `duplicate_element` - part has some duplicate elements that should be unique (namely, `title` tag)
 * - `unbalanced` - part has unbalanced tags
 * @param {string} name name of property
 * @return {boolean} true if the part has the specified property
 */
LUA_FUNCTION_DEF (html, has_property);

/***
 * @method html:get_images()
 * Returns a table of images found in html. Each image is, in turn, a table with the following fields:
 *
 * - `src` - link to the source
 * - `height` - height in pixels
 * - `width` - width in pixels
 * - `embedded` - `true` if an image is embedded in a message
 * @return {table} table of images in html part
 */
LUA_FUNCTION_DEF (html, get_images);

/***
 * @method html:get_blocks()
 * Returns a table of html blocks. Each block provides the following data:
 *
 * `tag` - corresponding tag
 * `color` - a triplet (r g b) for font color
 * `bgcolor` - a triplet (r g b) for background color
 * `style` - rspamd{text} with the full style description
 * `font_size` - font size
 * @return {table} table of blocks in html part
 */
LUA_FUNCTION_DEF (html, get_blocks);

/***
 * @method html:foreach_tag(tagname, callback)
 * Processes HTML tree calling the specified callback for each tag of the specified
 * type.
 *
 * Callback is called with the following attributes:
 *
 * - `tag`: html tag structure
 * - `content_length`: length of content within a tag
 *
 * Callback function should return `true` to **stop** processing and `false` to continue
 * @return nothing
 */
LUA_FUNCTION_DEF (html, foreach_tag);

static const struct luaL_reg htmllib_m[] = {
	LUA_INTERFACE_DEF (html, has_tag),
	LUA_INTERFACE_DEF (html, has_property),
	LUA_INTERFACE_DEF (html, get_images),
	LUA_INTERFACE_DEF (html, get_blocks),
	LUA_INTERFACE_DEF (html, foreach_tag),
	{"__tostring", rspamd_lua_class_tostring},
	{NULL, NULL}
};

/***
 * @method html_tag:get_type()
 * Returns string representation of HTML type for a tag
 * @return {string} type of tag
 */
LUA_FUNCTION_DEF (html_tag, get_type);
/***
 * @method html_tag:get_extra()
 * Returns extra data associated with the tag
 * @return {url|image|nil} extra data associated with the tag
 */
LUA_FUNCTION_DEF (html_tag, get_extra);
/***
 * @method html_tag:get_parent()
 * Returns parent node for a specified tag
 * @return {html_tag} parent object for a specified tag
 */
LUA_FUNCTION_DEF (html_tag, get_parent);

/***
 * @method html_tag:get_flags()
 * Returns flags a specified tag:
 *
 * - `closed`: tag is properly closed
 * - `closing`: tag is a closing tag
 * - `broken`: tag is somehow broken
 * - `unbalanced`: tag is unbalanced
 * - `xml`: tag is xml tag
 * @return {table} table of flags
 */
LUA_FUNCTION_DEF (html_tag, get_flags);
/***
 * @method html_tag:get_content()
 * Returns content of tag (approximate for some cases)
 * @return {rspamd_text} rspamd text with tag's content
 */
LUA_FUNCTION_DEF (html_tag, get_content);
/***
 * @method html_tag:get_content_length()
 * Returns length of a tag's content
 * @return {number} size of content enclosed within a tag
 */
LUA_FUNCTION_DEF (html_tag, get_content_length);

static const struct luaL_reg taglib_m[] = {
	LUA_INTERFACE_DEF (html_tag, get_type),
	LUA_INTERFACE_DEF (html_tag, get_extra),
	LUA_INTERFACE_DEF (html_tag, get_parent),
	LUA_INTERFACE_DEF (html_tag, get_flags),
	LUA_INTERFACE_DEF (html_tag, get_content),
	LUA_INTERFACE_DEF (html_tag, get_content_length),
	{"__tostring", rspamd_lua_class_tostring},
	{NULL, NULL}
};

static struct html_content *
lua_check_html (lua_State * L, gint pos)
{
	void *ud = rspamd_lua_check_udata (L, pos, "rspamd{html}");
	luaL_argcheck (L, ud != NULL, pos, "'html' expected");
	return ud ? *((struct html_content **)ud) : NULL;
}

struct lua_html_tag {
	struct html_content *html;
	rspamd::html::html_tag *tag;
};

static struct lua_html_tag *
lua_check_html_tag (lua_State * L, gint pos)
{
	void *ud = rspamd_lua_check_udata (L, pos, "rspamd{html_tag}");
	luaL_argcheck (L, ud != NULL, pos, "'html_tag' expected");
	return ud ? ((struct lua_html_tag *)ud) : NULL;
}

static gint
lua_html_has_tag (lua_State *L)
{
	LUA_TRACE_POINT;
	struct html_content *hc = lua_check_html (L, 1);
	const gchar *tagname = luaL_checkstring (L, 2);
	gboolean ret = FALSE;

	if (hc && tagname) {
		if (rspamd_html_tag_seen (hc, tagname)) {
			ret = TRUE;
		}
	}

	lua_pushboolean (L, ret);

	return 1;
}

constexpr const auto prop_map = frozen::make_unordered_map<frozen::string, int>({
		{"no_html", RSPAMD_HTML_FLAG_BAD_START},
		{"bad_start", RSPAMD_HTML_FLAG_BAD_START},
		{"bad_element", RSPAMD_HTML_FLAG_BAD_ELEMENTS},
		{"bad_elements", RSPAMD_HTML_FLAG_BAD_ELEMENTS},
		{"xml", RSPAMD_HTML_FLAG_XML},
		{"unknown_element", RSPAMD_HTML_FLAG_UNKNOWN_ELEMENTS},
		{"unknown_elements", RSPAMD_HTML_FLAG_UNKNOWN_ELEMENTS},
		{"duplicate_element", RSPAMD_HTML_FLAG_DUPLICATE_ELEMENTS},
		{"duplicate_elements", RSPAMD_HTML_FLAG_DUPLICATE_ELEMENTS},
		{"unbalanced", RSPAMD_HTML_FLAG_UNBALANCED},
		{"data_urls", RSPAMD_HTML_FLAG_HAS_DATA_URLS},
});

static gint
lua_html_has_property (lua_State *L)
{
	LUA_TRACE_POINT;
	struct html_content *hc = lua_check_html (L, 1);
	const gchar *propname = luaL_checkstring (L, 2);
	gboolean ret = FALSE;

	if (hc && propname) {
		auto found_prop = prop_map.find(frozen::string(propname));

		if (found_prop != prop_map.end()) {
			ret = hc->flags & found_prop->second;
		}
	}

	lua_pushboolean (L, ret);

	return 1;
}

static void
lua_html_push_image (lua_State *L, struct html_image *img)
{
	LUA_TRACE_POINT;
	struct lua_html_tag *ltag;
	struct rspamd_url **purl;

	lua_createtable (L, 0, 7);

	if (img->src) {
		lua_pushstring (L, "src");

		if (img->flags & RSPAMD_HTML_FLAG_IMAGE_DATA) {
			struct rspamd_lua_text *t;

			t = static_cast<rspamd_lua_text *>(lua_newuserdata(L, sizeof(*t)));
			t->start = img->src;
			t->len = strlen (img->src);
			t->flags = 0;

			rspamd_lua_setclass (L, "rspamd{text}", -1);
		}
		else {
			lua_pushstring (L, img->src);
		}

		lua_settable (L, -3);
	}

	if (img->url) {
		lua_pushstring (L, "url");
		purl = static_cast<rspamd_url **>(lua_newuserdata(L, sizeof(gpointer)));
		*purl = img->url;
		rspamd_lua_setclass (L, "rspamd{url}", -1);
		lua_settable (L, -3);
	}

	if (img->tag) {
		lua_pushstring (L, "tag");
		ltag = static_cast<lua_html_tag *>(lua_newuserdata(L, sizeof(struct lua_html_tag)));
		ltag->tag = static_cast<rspamd::html::html_tag *>(img->tag);
		ltag->html = NULL;
		rspamd_lua_setclass (L, "rspamd{html_tag}", -1);
		lua_settable (L, -3);
	}

	lua_pushstring (L, "height");
	lua_pushinteger (L, img->height);
	lua_settable (L, -3);
	lua_pushstring (L, "width");
	lua_pushinteger (L, img->width);
	lua_settable (L, -3);
	lua_pushstring (L, "embedded");
	lua_pushboolean (L, img->flags & RSPAMD_HTML_FLAG_IMAGE_EMBEDDED);
	lua_settable (L, -3);
	lua_pushstring (L, "data");
	lua_pushboolean (L, img->flags & RSPAMD_HTML_FLAG_IMAGE_DATA);
	lua_settable (L, -3);
}

static gint
lua_html_get_images (lua_State *L)
{
	LUA_TRACE_POINT;
	struct html_content *hc = lua_check_html (L, 1);
	struct html_image *img;

	guint i;

	if (hc != NULL) {
		if (hc->images) {
			lua_createtable (L, hc->images->len, 0);

			PTR_ARRAY_FOREACH (hc->images, i, img) {
				lua_html_push_image (L, img);
				lua_rawseti (L, -2, i + 1);
			}
		}
		else {
			lua_newtable (L);
		}
	}
	else {
		lua_newtable (L);
	}

	return 1;
}

static void
lua_html_push_block (lua_State *L, struct html_block *bl)
{
	LUA_TRACE_POINT;
	struct rspamd_lua_text *t;

	lua_createtable (L, 0, 6);

	if (bl->tag) {
		gsize name_len;
		const gchar *name = rspamd_html_tag_name(bl->tag, &name_len);
		lua_pushstring (L, "tag");
		lua_pushlstring (L, name, name_len);
		lua_settable (L, -3);
	}

	if (bl->font_color.valid) {
		lua_pushstring (L, "color");
		lua_createtable (L, 4, 0);
		lua_pushinteger (L, bl->font_color.d.comp.r);
		lua_rawseti (L, -2, 1);
		lua_pushinteger (L, bl->font_color.d.comp.g);
		lua_rawseti (L, -2, 2);
		lua_pushinteger (L, bl->font_color.d.comp.b);
		lua_rawseti (L, -2, 3);
		lua_pushinteger (L, bl->font_color.d.comp.alpha);
		lua_rawseti (L, -2, 4);
		lua_settable (L, -3);
	}
	if (bl->background_color.valid) {
		lua_pushstring (L, "bgcolor");
		lua_createtable (L, 4, 0);
		lua_pushinteger (L, bl->background_color.d.comp.r);
		lua_rawseti (L, -2, 1);
		lua_pushinteger (L, bl->background_color.d.comp.g);
		lua_rawseti (L, -2, 2);
		lua_pushinteger (L, bl->background_color.d.comp.b);
		lua_rawseti (L, -2, 3);
		lua_pushinteger (L, bl->background_color.d.comp.alpha);
		lua_rawseti (L, -2, 4);
		lua_settable (L, -3);
	}

	if (bl->style.len > 0) {
		lua_pushstring (L, "style");
		t = static_cast<rspamd_lua_text *>(lua_newuserdata(L, sizeof(*t)));
		rspamd_lua_setclass (L, "rspamd{text}", -1);
		t->start = bl->style.begin;
		t->len = bl->style.len;
		t->flags = 0;
		lua_settable (L, -3);
	}

	lua_pushstring (L, "visible");
	lua_pushboolean (L, bl->visible);
	lua_settable (L, -3);

	lua_pushstring (L, "font_size");
	lua_pushinteger (L, bl->font_size);
	lua_settable (L, -3);
}

static gint
lua_html_get_blocks (lua_State *L)
{
	LUA_TRACE_POINT;
	struct html_content *hc = lua_check_html (L, 1);
	struct html_block *bl;

	guint i;

	if (hc != NULL) {
		if (hc->blocks && hc->blocks->len > 0) {
			lua_createtable (L, hc->blocks->len, 0);

			for (i = 0; i < hc->blocks->len; i ++) {
				bl = static_cast<decltype(bl)>(g_ptr_array_index (hc->blocks, i));
				lua_html_push_block (L, bl);
				lua_rawseti (L, -2, i + 1);
			}
		}
		else {
			lua_pushnil (L);
		}
	}
	else {
		return luaL_error (L, "invalid arguments");
	}

	return 1;
}

struct lua_html_traverse_ud {
	lua_State *L;
	struct html_content *html;
	gint cbref;
	robin_hood::unordered_flat_set<int> tags;
	gboolean any;
};

static gboolean
lua_html_node_foreach_cb (GNode *n, gpointer d)
{
	struct lua_html_traverse_ud *ud = (struct lua_html_traverse_ud *)d;
	auto *tag = (rspamd::html::html_tag *)n->data;
	struct lua_html_tag *ltag;

	if (tag && (ud->any || ud->tags.contains(tag->id))) {

		lua_rawgeti (ud->L, LUA_REGISTRYINDEX, ud->cbref);

		ltag = static_cast<lua_html_tag *>(lua_newuserdata(ud->L, sizeof(*ltag)));
		ltag->tag = tag;
		ltag->html = ud->html;
		rspamd_lua_setclass (ud->L, "rspamd{html_tag}", -1);
		lua_pushinteger (ud->L, tag->content_length);

		/* Leaf flag */
		if (g_node_first_child (n)) {
			lua_pushboolean (ud->L, false);
		}
		else {
			lua_pushboolean (ud->L, true);
		}

		if (lua_pcall (ud->L, 3, 1, 0) != 0) {
			msg_err ("error in foreach_tag callback: %s", lua_tostring (ud->L, -1));
			lua_pop (ud->L, 1);
			return TRUE;
		}

		if (lua_toboolean (ud->L, -1)) {
			lua_pop (ud->L, 1);
			return TRUE;
		}

		lua_pop (ud->L, 1);
	}

	return FALSE;
}

static gint
lua_html_foreach_tag (lua_State *L)
{
	LUA_TRACE_POINT;
	struct html_content *hc = lua_check_html (L, 1);
	struct lua_html_traverse_ud ud;
	const gchar *tagname;
	gint id;

	ud.any = FALSE;
	ud.html = hc;

	if (lua_type (L, 2) == LUA_TSTRING) {
		tagname = luaL_checkstring (L, 2);
		if (strcmp (tagname, "any") == 0) {
			ud.any = TRUE;
		}
		else {
			id = rspamd_html_tag_by_name(tagname);

			if (id == -1) {
				return luaL_error (L, "invalid tagname: %s", tagname);
			}


			ud.tags.insert(id);
		}
	}
	else if (lua_type (L, 2) == LUA_TTABLE) {
		lua_pushvalue (L, 2);

		for (lua_pushnil (L); lua_next (L, -2); lua_pop (L, 1)) {
			tagname = luaL_checkstring (L, -1);
			if (strcmp (tagname, "any") == 0) {
				ud.any = TRUE;
			}
			else {
				id = rspamd_html_tag_by_name (tagname);

				if (id == -1) {
					return luaL_error (L, "invalid tagname: %s", tagname);
				}
				ud.tags.insert(id);
			}
		}

		lua_pop (L, 1);
	}

	if (hc && (ud.any || !ud.tags.empty()) && lua_isfunction (L, 3)) {
		if (hc->html_tags) {

			lua_pushvalue (L, 3);
			ud.cbref = luaL_ref (L, LUA_REGISTRYINDEX);
			ud.L = L;

			g_node_traverse (hc->html_tags, G_PRE_ORDER, G_TRAVERSE_ALL, -1,
					lua_html_node_foreach_cb, &ud);

			luaL_unref (L, LUA_REGISTRYINDEX, ud.cbref);
		}
	}
	else {
		return luaL_error (L, "invalid arguments");
	}

	return 0;
}

static gint
lua_html_tag_get_type (lua_State *L)
{
	LUA_TRACE_POINT;
	struct lua_html_tag *ltag = lua_check_html_tag (L, 1);
	const gchar *tagname;

	if (ltag != NULL) {
		tagname = rspamd_html_tag_by_id (ltag->tag->id);

		if (tagname) {
			lua_pushstring (L, tagname);
		}
		else {
			lua_pushnil (L);
		}
	}
	else {
		return luaL_error (L, "invalid arguments");
	}

	return 1;
}

static gint
lua_html_tag_get_parent (lua_State *L)
{
	LUA_TRACE_POINT;
	struct lua_html_tag *ltag = lua_check_html_tag (L, 1), *ptag;
	GNode *node;

	if (ltag != NULL) {
		node = ltag->tag->parent;

		if (node && node->data) {
			ptag = static_cast<lua_html_tag *>(lua_newuserdata(L, sizeof(*ptag)));
			ptag->tag = static_cast<rspamd::html::html_tag *>(node->data);
			ptag->html = ltag->html;
			rspamd_lua_setclass (L, "rspamd{html_tag}", -1);
		}
		else {
			lua_pushnil (L);
		}
	}
	else {
		return luaL_error (L, "invalid arguments");
	}

	return 1;
}

static gint
lua_html_tag_get_flags (lua_State *L)
{
	LUA_TRACE_POINT;
	struct lua_html_tag *ltag = lua_check_html_tag (L, 1);
	gint i = 1;

	if (ltag->tag) {
		/* Push flags */
		lua_createtable (L, 4, 0);
		if (ltag->tag->flags & FL_CLOSING) {
			lua_pushstring (L, "closing");
			lua_rawseti (L, -2, i++);
		}
		if (ltag->tag->flags & FL_HREF) {
			lua_pushstring (L, "href");
			lua_rawseti (L, -2, i++);
		}
		if (ltag->tag->flags & FL_CLOSED) {
			lua_pushstring (L, "closed");
			lua_rawseti (L, -2, i++);
		}
		if (ltag->tag->flags & FL_BROKEN) {
			lua_pushstring (L, "broken");
			lua_rawseti (L, -2, i++);
		}
		if (ltag->tag->flags & FL_XML) {
			lua_pushstring (L, "xml");
			lua_rawseti (L, -2, i++);
		}
		if (ltag->tag->flags & RSPAMD_HTML_FLAG_UNBALANCED) {
			lua_pushstring (L, "unbalanced");
			lua_rawseti (L, -2, i++);
		}
	}
	else {
		return luaL_error (L, "invalid arguments");
	}

	return 1;
}

static gint
lua_html_tag_get_content (lua_State *L)
{
	LUA_TRACE_POINT;
	struct lua_html_tag *ltag = lua_check_html_tag (L, 1);
	struct rspamd_lua_text *t;

	if (ltag) {
		if (ltag->html && ltag->tag->content_length &&
				ltag->html->parsed->len >= ltag->tag->content_offset + ltag->tag->content_length) {
			t = static_cast<rspamd_lua_text *>(lua_newuserdata(L, sizeof(*t)));
			rspamd_lua_setclass (L, "rspamd{text}", -1);
			t->start = reinterpret_cast<const char *>(ltag->html->parsed->data) +
					ltag->tag->content_offset;
			t->len = ltag->tag->content_length;
			t->flags = 0;
		}
		else {
			lua_pushnil (L);
		}
	}
	else {
		return luaL_error (L, "invalid arguments");
	}

	return 1;
}

static gint
lua_html_tag_get_content_length (lua_State *L)
{
	LUA_TRACE_POINT;
	struct lua_html_tag *ltag = lua_check_html_tag (L, 1);

	if (ltag) {
		lua_pushinteger (L, ltag->tag->content_length);
	}
	else {
		return luaL_error (L, "invalid arguments");
	}

	return 1;
}

static gint
lua_html_tag_get_extra (lua_State *L)
{
	LUA_TRACE_POINT;
	struct lua_html_tag *ltag = lua_check_html_tag (L, 1);
	struct html_image *img;
	struct rspamd_url **purl;

	if (ltag) {
		if (!std::holds_alternative<std::monostate>(ltag->tag->extra)) {
			if (std::holds_alternative<struct html_image *>(ltag->tag->extra)) {
				img = std::get<struct html_image *>(ltag->tag->extra);
				lua_html_push_image (L, img);
			}
			else if (std::holds_alternative<struct rspamd_url *>(ltag->tag->extra)) {
				/* For A that's URL */
				purl = static_cast<rspamd_url **>(lua_newuserdata(L, sizeof(gpointer)));
				*purl = std::get<struct rspamd_url *>(ltag->tag->extra);
				rspamd_lua_setclass (L, "rspamd{url}", -1);
			}
			else if (ltag->tag->flags & FL_BLOCK) {
				lua_html_push_block (L, ltag->tag->block);
			}
			else {
				/* Unknown extra ? */
				lua_pushnil (L);
			}
		}
		else {
			lua_pushnil (L);
		}
	}
	else {
		return luaL_error (L, "invalid arguments");
	}

	return 1;
}

void
luaopen_html (lua_State * L)
{
	rspamd_lua_new_class (L, "rspamd{html}", htmllib_m);
	lua_pop (L, 1);
	rspamd_lua_new_class (L, "rspamd{html_tag}", taglib_m);
	lua_pop (L, 1);
}