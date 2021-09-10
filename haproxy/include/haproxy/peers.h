/*
 * include/haproxy/peers.h
 * This file defines function prototypes for peers management.
 *
 * Copyright 2010 EXCELIANCE, Emeric Brun <ebrun@exceliance.fr>
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

#ifndef _HAPROXY_PEERS_H
#define _HAPROXY_PEERS_H

#include <haproxy/api.h>
#include <haproxy/connection.h>
#include <haproxy/obj_type.h>
#include <haproxy/peers-t.h>
#include <haproxy/proxy-t.h>
#include <haproxy/stick_table-t.h>
#include <haproxy/stream-t.h>
#include <haproxy/time.h>


extern struct peers *cfg_peers;

int peers_init_sync(struct peers *peers);
int peers_alloc_dcache(struct peers *peers);
int peers_register_table(struct peers *, struct stktable *table);
void peers_setup_frontend(struct proxy *fe);

#if defined(USE_OPENSSL)
static inline enum obj_type *peer_session_target(struct peer *p, struct stream *s)
{
	if (p->srv->use_ssl)
		return &p->srv->obj_type;
	else
		return &s->be->obj_type;
}

static inline struct xprt_ops *peer_xprt(struct peer *p)
{
	return p->srv->use_ssl ? xprt_get(XPRT_SSL) : xprt_get(XPRT_RAW);
}
#else
static inline enum obj_type *peer_session_target(struct peer *p, struct stream *s)
{
	return &s->be->obj_type;
}

static inline struct xprt_ops *peer_xprt(struct peer *p)
{
	return xprt_get(XPRT_RAW);
}
#endif

#endif /* _HAPROXY_PEERS_H */

