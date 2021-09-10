/*
 * include/haproxy/hlua.h
 * Lua core management functions
 *
 * Copyright (C) 2015-2016 Thierry Fournier <tfournier@arpalert.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation, version 2.1
 * exclusively.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifndef _HAPROXY_HLUA_H
#define _HAPROXY_HLUA_H

#include <haproxy/hlua-t.h>

#ifdef USE_LUA

/* The following macros are used to set flags. */
#define HLUA_SET_RUN(__hlua)         do {(__hlua)->flags |= HLUA_RUN;} while(0)
#define HLUA_CLR_RUN(__hlua)         do {(__hlua)->flags &= ~HLUA_RUN;} while(0)
#define HLUA_IS_RUNNING(__hlua)      ((__hlua)->flags & HLUA_RUN)
#define HLUA_SET_CTRLYIELD(__hlua)   do {(__hlua)->flags |= HLUA_CTRLYIELD;} while(0)
#define HLUA_CLR_CTRLYIELD(__hlua)   do {(__hlua)->flags &= ~HLUA_CTRLYIELD;} while(0)
#define HLUA_IS_CTRLYIELDING(__hlua) ((__hlua)->flags & HLUA_CTRLYIELD)
#define HLUA_SET_WAKERESWR(__hlua)   do {(__hlua)->flags |= HLUA_WAKERESWR;} while(0)
#define HLUA_CLR_WAKERESWR(__hlua)   do {(__hlua)->flags &= ~HLUA_WAKERESWR;} while(0)
#define HLUA_IS_WAKERESWR(__hlua)    ((__hlua)->flags & HLUA_WAKERESWR)
#define HLUA_SET_WAKEREQWR(__hlua)   do {(__hlua)->flags |= HLUA_WAKEREQWR;} while(0)
#define HLUA_CLR_WAKEREQWR(__hlua)   do {(__hlua)->flags &= ~HLUA_WAKEREQWR;} while(0)
#define HLUA_IS_WAKEREQWR(__hlua)    ((__hlua)->flags & HLUA_WAKEREQWR)

#define HLUA_INIT(__hlua) do { (__hlua)->T = 0; } while(0)

/* Lua HAProxy integration functions. */
const char *hlua_traceback(lua_State *L, const char* sep);
void hlua_ctx_destroy(struct hlua *lua);
void hlua_init();
int hlua_post_init();
void hlua_applet_tcp_fct(struct appctx *ctx);
void hlua_applet_http_fct(struct appctx *ctx);
struct task *hlua_process_task(struct task *task, void *context, unsigned short state);

#else /* USE_LUA */

/************************ For use when Lua is disabled ********************/

#define HLUA_IS_RUNNING(__hlua) 0

#define HLUA_INIT(__hlua)

/* Empty function for compilation without Lua. */
static inline void hlua_init() { }
static inline int hlua_post_init() { return 1; }
static inline void hlua_ctx_destroy(struct hlua *lua) { }

#endif /* USE_LUA */

#endif /* _HAPROXY_HLUA_H */
