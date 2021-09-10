/*
 * Protocol registration functions.
 *
 * Copyright 2000-2012 Willy Tarreau <w@1wt.eu>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <sys/types.h>
#include <sys/socket.h>

#include <haproxy/api.h>
#include <haproxy/errors.h>
#include <haproxy/list.h>
#include <haproxy/protocol.h>
#include <haproxy/tools.h>


/* List head of all registered protocols */
static struct list protocols = LIST_HEAD_INIT(protocols);
struct protocol *__protocol_by_family[AF_CUST_MAX] = { };

/* This is the global spinlock we may need to register/unregister listeners or
 * protocols. Its main purpose is in fact to serialize the rare stop/deinit()
 * phases.
 */
__decl_spinlock(proto_lock);

/* Registers the protocol <proto> */
void protocol_register(struct protocol *proto)
{
	HA_SPIN_LOCK(PROTO_LOCK, &proto_lock);
	LIST_ADDQ(&protocols, &proto->list);
	if (proto->sock_domain >= 0 && proto->sock_domain < AF_CUST_MAX)
		__protocol_by_family[proto->sock_domain] = proto;
	HA_SPIN_UNLOCK(PROTO_LOCK, &proto_lock);
}

/* Unregisters the protocol <proto>. Note that all listeners must have
 * previously been unbound.
 */
void protocol_unregister(struct protocol *proto)
{
	HA_SPIN_LOCK(PROTO_LOCK, &proto_lock);
	LIST_DEL(&proto->list);
	LIST_INIT(&proto->list);
	HA_SPIN_UNLOCK(PROTO_LOCK, &proto_lock);
}

/* binds all listeners of all registered protocols. Returns a composition
 * of ERR_NONE, ERR_RETRYABLE, ERR_FATAL.
 */
int protocol_bind_all(char *errmsg, int errlen)
{
	struct protocol *proto;
	int err;

	err = 0;
	HA_SPIN_LOCK(PROTO_LOCK, &proto_lock);
	list_for_each_entry(proto, &protocols, list) {
		if (proto->bind_all) {
			err |= proto->bind_all(proto, errmsg, errlen);
			if ( err & ERR_ABORT )
				break;
		}
	}
	HA_SPIN_UNLOCK(PROTO_LOCK, &proto_lock);
	return err;
}

/* unbinds all listeners of all registered protocols. They are also closed.
 * This must be performed before calling exit() in order to get a chance to
 * remove file-system based sockets and pipes.
 * Returns a composition of ERR_NONE, ERR_RETRYABLE, ERR_FATAL, ERR_ABORT.
 */
int protocol_unbind_all(void)
{
	struct protocol *proto;
	int err;

	err = 0;
	HA_SPIN_LOCK(PROTO_LOCK, &proto_lock);
	list_for_each_entry(proto, &protocols, list) {
		if (proto->unbind_all) {
			err |= proto->unbind_all(proto);
		}
	}
	HA_SPIN_UNLOCK(PROTO_LOCK, &proto_lock);
	return err;
}

/* enables all listeners of all registered protocols. This is intended to be
 * used after a fork() to enable reading on all file descriptors. Returns a
 * composition of ERR_NONE, ERR_RETRYABLE, ERR_FATAL.
 */
int protocol_enable_all(void)
{
	struct protocol *proto;
	int err;

	err = 0;
	HA_SPIN_LOCK(PROTO_LOCK, &proto_lock);
	list_for_each_entry(proto, &protocols, list) {
		if (proto->enable_all) {
			err |= proto->enable_all(proto);
		}
	}
	HA_SPIN_UNLOCK(PROTO_LOCK, &proto_lock);
	return err;
}

/* disables all listeners of all registered protocols. This may be used before
 * a fork() to avoid duplicating poll lists. Returns a composition of ERR_NONE,
 * ERR_RETRYABLE, ERR_FATAL.
 */
int protocol_disable_all(void)
{
	struct protocol *proto;
	int err;

	err = 0;
	HA_SPIN_LOCK(PROTO_LOCK, &proto_lock);
	list_for_each_entry(proto, &protocols, list) {
		if (proto->disable_all) {
			err |= proto->disable_all(proto);
		}
	}
	HA_SPIN_UNLOCK(PROTO_LOCK, &proto_lock);
	return err;
}

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
