/*
 * This file is part of mplayer.
 *
 * mplayer is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * mplayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with mplayer.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
#include <luajit.h>

#include "config.h"
#include "mp_msg.h"

#include "img_format.h"
#include "mp_image.h"
#include "vf.h"

#include "m_option.h"
#include "m_struct.h"

#define PLANES 3

static struct vf_priv_s {
    char *cfg_fn[PLANES];
    char *cfg_file;

    lua_State *L;
} const vf_priv_dflt = {};

static const char *lua_code =
"local ffi = require('ffi')\n"
"require('v').start('yourlogdump.txt')\n"
"dst = {{}, {}, {}}\n"
"src = {{}, {}, {}}\n"
"function _prepare_filter()\n"
"    _G.width = src.width\n"
"    _G.height = src.height\n"
"    -- can't do this type conversion with the C API\n"
"    -- also, doing the cast in an inner loop makes it slow\n"
"    for i = 1, src.plane_count do\n"
"        src[i].ptr = ffi.cast('uint8_t*', src[i].ptr)\n"
"        dst[i].ptr = ffi.cast('uint8_t*', dst[i].ptr)\n"
"    end\n"
"end\n"
"local function _px_rowptr(plane, y)\n"
"    return ffi.cast(plane.pixel_ptr_type, plane.ptr + plane.stride * y)\n"
"end\n"
"local function _px_noclip(plane, x, y)\n"
"    return _px_rowptr(plane, y)[x] / plane.max\n"
"end\n"
"function px(plane_nr, x, y)\n"
"    local plane = src[plane_nr]\n"
"    if x >= plane.width then x = plane.width - 1 end\n"
"    if x < 0 then x = 0 end\n"
"    if y >= plane.height then y = plane.height - 1 end\n"
"    if y < 0 then y = 0 end\n"
"    return _px_noclip(plane, x, y)\n"
"end\n"
"px_fns = {function(x, y) return px(1, x, y) end,\n"
"          function(x, y) return px(2, x, y) end,\n"
"          function(x, y) return px(3, x, y) end}\n"
"function _filter_plane(dst, src, pixel_fn)\n"
"    local src_start_ptr = src.ptr\n"
"    local src_stride = src.stride\n"
"    local dst_start_ptr = dst.ptr\n"
"    local dst_stride = dst.stride\n"
"    local width = src.width\n"
"    local max = src.max\n"
"    local type = src.pixel_ptr_type\n"
"    _G.p = px_fns[dst.plane_nr]\n"
"    for y = 0, src.height - 1 do\n"
"        local src_ptr = ffi.cast(type, src_start_ptr + src_stride * y)\n"
"        local dst_ptr = ffi.cast(type, dst_start_ptr + dst_stride * y)\n"
"        for x = 0, width - 1 do\n"
"            dst_ptr[x] = pixel_fn(x, y, src_ptr[x] / max) * max\n"
"        end\n"
"    end\n"
"end\n"
"function _copy_plane(dst, src)\n"
"    for y = 0, src.height - 1 do\n"
"        ffi.copy(_px_rowptr(dst, y), _px_rowptr(src, y),\n"
"                 src.bytes_per_pixel * src.width)\n"
"    end\n"
"end\n"
"function filter_image()\n"
"    for i = 1, src.plane_count do\n"
"        if plane_fn and plane_fn[i] then\n"
"            _filter_plane(dst[i], src[i], plane_fn[i])\n"
"        else\n"
"            _copy_plane(dst[i], src[i])\n"
"        end\n"
"    end\n"
"end\n"
;

static int config(struct vf_instance *vf,
                  int width, int height, int d_width, int d_height,
                  unsigned int flags, unsigned int fmt)
{
    return vf_next_config(vf, width, height, d_width, d_height, flags, fmt);
}

static void uninit(struct vf_instance *vf)
{
    lua_close(vf->priv->L);
}

// Set the field of the passed Lua table to the given mpi
// Lua stack: p (Lua table for image)
static void set_lua_mpi_fields(lua_State *L, mp_image_t *mpi)
{
    int chroma_xs, chroma_ys, bits;
    mp_get_chroma_shift(mpi->imgfmt, &chroma_xs, &chroma_ys, &bits);

    for (int n = 0; n < PLANES; n++) {
        lua_pushinteger(L, n + 1); // p n
        lua_gettable(L, -2); // p ps

        bool valid = n < mpi->num_planes;

        lua_pushinteger(L, valid ? n + 1 : 0);
        lua_setfield(L, -2, "plane_nr");

        lua_pushlightuserdata(L, valid ? mpi->planes[n] : NULL);
        lua_setfield(L, -2, "ptr");

        lua_pushinteger(L, valid ? mpi->stride[n] : 0);
        lua_setfield(L, -2, "stride");

        bool chroma = (n == 1 || n == 2);
        int sx = chroma ? chroma_xs : 0;
        int sy = chroma ? chroma_ys : 0;

        lua_pushinteger(L, valid ? (mpi->w >> sx) : 0);
        lua_setfield(L, -2, "width");

        lua_pushinteger(L, valid ? (mpi->h >> sy) : 0);
        lua_setfield(L, -2, "height");

        const char *pixel_ptr_type;
        if (bits <= 8) {
            pixel_ptr_type = "uint8_t*";
        } else if (bits <= 16) {
            pixel_ptr_type = "uint16_t*";
        } else {
            abort();
        }
        lua_pushstring(L, valid ? pixel_ptr_type : "");
        lua_setfield(L, -2, "pixel_ptr_type");

        lua_pushinteger(L, valid ? ((bits + 7) / 8) : 0);
        lua_setfield(L, -2, "bytes_per_pixel");

        int max = (1 << bits) - 1;
        lua_pushinteger(L, valid ? max : 0);
        lua_setfield(L, -2, "max");

        lua_pop(L, 1); // p
    }

    lua_pushinteger(L, mpi->w);
    lua_setfield(L, -2, "width");

    lua_pushinteger(L, mpi->h);
    lua_setfield(L, -2, "height");

    lua_pushinteger(L, mpi->num_planes);
    lua_setfield(L, -2, "plane_count");
}

struct closure {
    mp_image_t *mpi, *dmpi;
};

static int run_lua_filter(lua_State *L)
{
    struct closure *c = lua_touserdata(L, 1);

    lua_getglobal(L, "src"); // p
    set_lua_mpi_fields(L, c->mpi); // p
    lua_pop(L, 1); // -

    lua_getglobal(L, "dst"); // p
    set_lua_mpi_fields(L, c->dmpi); // p
    lua_pop(L, 1); // -

    lua_getglobal(L, "_prepare_filter");
    lua_call(L, 0, 0);

    // Call the Lua filter function
    lua_getglobal(L, "filter_image");
    lua_call(L, 0, 0);

    return 0;
}

static int put_image(struct vf_instance *vf, mp_image_t *mpi, double pts)
{
    lua_State *L = vf->priv->L;
    mp_image_t *dmpi;

    if(!(mpi->flags&MP_IMGFLAG_DIRECT)){
        // no DR, so get a new image! hope we'll get DR buffer:
        vf->dmpi=vf_get_image(vf->next,mpi->imgfmt, MP_IMGTYPE_TEMP,
                              MP_IMGFLAG_ACCEPT_STRIDE|MP_IMGFLAG_PREFER_ALIGNED_STRIDE,
                              mpi->w,mpi->h);
    }

    dmpi= vf->dmpi;

    vf_clone_mpi_attributes(dmpi, mpi);

    struct closure c = { .mpi = mpi, .dmpi = dmpi };
    if (lua_cpcall(L, run_lua_filter, &c)) {
        const char *err = "<unknown>";
        if (lua_isstring(L, -1))
            err = lua_tostring(L, -1);
        mp_msg(MSGT_VFILTER, MSGL_ERR, "vf_lua: error running filter: %s\n",
               err);
        lua_pop(L, 1);
    }

    return vf_next_put_image(vf, dmpi, pts);
}

//===========================================================================//

static int query_format(struct vf_instance *vf, unsigned int fmt)
{
    if (mp_get_chroma_shift(fmt, NULL, NULL, NULL) == 0)
        return 0;
    return vf_next_query_format(vf, fmt);
}

static int vf_open(vf_instance_t *vf, char *args)
{
    bool have_fn = false;
    for (int n = 0; n < PLANES; n++)
        have_fn |= vf->priv->cfg_fn[n] && vf->priv->cfg_fn[n][0];
    if (!vf->priv->cfg_file && !have_fn) {
        mp_msg(MSGT_VFILTER, MSGL_ERR, "vf_lua: no arguments\n");
        return 0;
    }

    lua_State *L = luaL_newstate();
    vf->priv->L = L;

    luaL_openlibs(L);

    if (luaL_dostring(L, lua_code))
        goto lua_error;

    if (have_fn) {
        lua_newtable(L); // t
        for (int n = 0; n < PLANES; n++) {
            char *expr = vf->priv->cfg_fn[n];

            // please kill me now
            // or better, kill the retarded suboption parser
            for (int n = 0; expr && expr[n]; n++) {
                if (expr[n] == ';')
                    expr[n] = ',';
            }

            if (expr && expr[0]) {
                lua_pushstring(L, "local x, y, c = ...; return ("); // t s
                lua_pushstring(L, expr); // t s s
                lua_pushstring(L, ")"); // t s s s
                lua_concat(L, 3); // t s
                const char *s = lua_tostring(L, -1); // t s
                if (luaL_loadstring(L, s))
                    goto lua_error;
                // t s fn
                lua_remove(L, -2); // t fn
                lua_rawseti(L, -2, n + 1); // t
            }
        }
        lua_setglobal(L, "plane_fn"); // -
    } else {
        if (luaL_loadfile(L, vf->priv->cfg_file) || lua_pcall(L, 0, 0, 0))
            goto lua_error;
    }

    vf->put_image = put_image;
    vf->query_format = query_format;
    vf->config = config;
    vf->uninit = uninit;

    return 1;

lua_error:
    mp_msg(MSGT_VFILTER, MSGL_ERR, "Lua: %s\n", lua_tolstring(L, -1, NULL));
    uninit(vf);
    return 0;
}

#define ST_OFF(f) M_ST_OFF(struct vf_priv_s, f)
static m_option_t vf_opts_fields[] = {
    {"fn_l", ST_OFF(cfg_fn[0]), CONF_TYPE_STRING, 0, 0, 0, NULL},
    {"fn_u", ST_OFF(cfg_fn[1]), CONF_TYPE_STRING, 0, 0, 0, NULL},
    {"fn_v", ST_OFF(cfg_fn[2]), CONF_TYPE_STRING, 0, 0, 0, NULL},
    {"file", ST_OFF(cfg_file), CONF_TYPE_STRING, 0, 0, 0, NULL},
    { NULL, NULL, 0, 0, 0, 0, NULL }
};

static const m_struct_t vf_opts = {
    "dlopen",
    sizeof(struct vf_priv_s),
    &vf_priv_dflt,
    vf_opts_fields
};

const vf_info_t vf_info_lua = {
    "Lua equation/function filter using LuaJIT",
    "lua",
    "wm4",
    "",
    vf_open,
    &vf_opts
};
