/*
 * include/haproxy/session.h
 * This file contains functions used to manage sessions.
 *
 * Copyright (C) 2000-2020 Willy Tarreau - w@1wt.eu
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

#ifndef _HAPROXY_SESSION_H
#define _HAPROXY_SESSION_H

#include <haproxy/api.h>
#include <haproxy/global-t.h>
#include <haproxy/obj_type-t.h>
#include <haproxy/pool.h>
#include <haproxy/server.h>
#include <haproxy/session-t.h>
#include <haproxy/stick_table.h>

extern struct pool_head *pool_head_session;
extern struct pool_head *pool_head_sess_srv_list;

struct session *session_new(struct proxy *fe, struct listener *li, enum obj_type *origin);
void session_free(struct session *sess);
int session_accept_fd(struct listener *l, int cfd, struct sockaddr_storage *addr);
int conn_complete_session(struct connection *conn);

/* Remove the refcount from the session to the tracked counters, and clear the
 * pointer to ensure this is only performed once. The caller is responsible for
 * ensuring that the pointer is valid first.
 */
static inline void session_store_counters(struct session *sess)
{
	void *ptr;
	int i;
	struct stksess *ts;

	for (i = 0; i < MAX_SESS_STKCTR; i++) {
		struct stkctr *stkctr = &sess->stkctr[i];

		ts = stkctr_entry(stkctr);
		if (!ts)
			continue;

		ptr = stktable_data_ptr(stkctr->table, ts, STKTABLE_DT_CONN_CUR);
		if (ptr) {
			HA_RWLOCK_WRLOCK(STK_SESS_LOCK, &ts->lock);

			if (stktable_data_cast(ptr, conn_cur) > 0)
				stktable_data_cast(ptr, conn_cur)--;

			HA_RWLOCK_WRUNLOCK(STK_SESS_LOCK, &ts->lock);

			/* If data was modified, we need to touch to re-schedule sync */
			stktable_touch_local(stkctr->table, ts, 0);
		}

		stkctr_set_entry(stkctr, NULL);
		stksess_kill_if_expired(stkctr->table, ts, 1);
	}
}

/* Remove the connection from the session list, and destroy the srv_list if it's now empty */
static inline void session_unown_conn(struct session *sess, struct connection *conn)
{
	struct sess_srv_list *srv_list = NULL;

	/* WT: this currently is a workaround for an inconsistency between
	 * the link status of the connection in the session list and the
	 * connection's owner. This should be removed as soon as all this
	 * is addressed. Right now it's possible to enter here with a non-null
	 * conn->owner that points to a dead session, but in this case the
	 * element is not linked.
	 */
	if (!LIST_ADDED(&conn->session_list))
		return;

	LIST_DEL_INIT(&conn->session_list);
	conn->owner = NULL;
	list_for_each_entry(srv_list, &sess->srv_list, srv_list) {
		if (srv_list->target == conn->target) {
			if (LIST_ISEMPTY(&srv_list->conn_list)) {
				LIST_DEL(&srv_list->srv_list);
				pool_free(pool_head_sess_srv_list, srv_list);
			}
			break;
		}
	}
}

static inline int session_add_conn(struct session *sess, struct connection *conn, void *target)
{
	struct sess_srv_list *srv_list = NULL;
	int found = 0;

	list_for_each_entry(srv_list, &sess->srv_list, srv_list) {
		if (srv_list->target == target) {
			found = 1;
			break;
		}
	}
	if (!found) {
		/* The session has no connection for the server, create a new entry */
		srv_list = pool_alloc(pool_head_sess_srv_list);
		if (!srv_list)
			return 0;
		srv_list->target = target;
		LIST_INIT(&srv_list->conn_list);
		LIST_ADDQ(&sess->srv_list, &srv_list->srv_list);
	}
	LIST_ADDQ(&srv_list->conn_list, &conn->session_list);
	return 1;
}

/* Returns 0 if the session can keep the idle conn, -1 if it was destroyed, or 1 if it was added to the server list */
static inline int session_check_idle_conn(struct session *sess, struct connection *conn)
{
	if (!(conn->flags & CO_FL_PRIVATE) ||
	    sess->idle_conns >= sess->fe->max_out_conns) {
		session_unown_conn(sess, conn);
		conn->owner = NULL;
		conn->flags &= ~CO_FL_SESS_IDLE;
		conn->mux->destroy(conn->ctx);
		return -1;
	} else {
		conn->flags |= CO_FL_SESS_IDLE;
		sess->idle_conns++;
	}
	return 0;
}

#endif /* _HAPROXY_SESSION_H */

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
