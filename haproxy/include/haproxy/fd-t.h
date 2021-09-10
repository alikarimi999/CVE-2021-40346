/*
 * include/haproxy/fd-t.h
 * File descriptors states - check src/fd.c for explanations.
 *
 * Copyright (C) 2000-2014 Willy Tarreau - w@1wt.eu
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

#ifndef _HAPROXY_FD_T_H
#define _HAPROXY_FD_T_H

#include <haproxy/api-t.h>
#include <haproxy/port_range-t.h>

/* Direction for each FD event update */
enum {
	DIR_RD=0,
	DIR_WR=1,
};

/* Polling status flags returned in fdtab[].ev :
 * FD_POLL_IN remains set as long as some data is pending for read.
 * FD_POLL_OUT remains set as long as the fd accepts to write data.
 * FD_POLL_ERR and FD_POLL_ERR remain set forever (until processed).
 */
#define FD_POLL_IN	0x01
#define FD_POLL_PRI	0x02
#define FD_POLL_OUT	0x04
#define FD_POLL_ERR	0x08
#define FD_POLL_HUP	0x10

#define FD_POLL_DATA    (FD_POLL_IN  | FD_POLL_OUT)
#define FD_POLL_STICKY  (FD_POLL_ERR | FD_POLL_HUP)

/* FD_EV_* are the values used in fdtab[].state to define the polling states in
 * each direction. Most of them are manipulated using test-and-set operations
 * which require the bit position in the mask, which is given in the _BIT
 * variant.
 */

/* bits positions for a few flags */
#define FD_EV_ACTIVE_R_BIT 0
#define FD_EV_READY_R_BIT  1
#define FD_EV_SHUT_R_BIT   2
/* unused: 3 */

#define FD_EV_ACTIVE_W_BIT 4
#define FD_EV_READY_W_BIT  5
#define FD_EV_SHUT_W_BIT   6
#define FD_EV_ERR_RW_BIT   7

/* and flag values */
#define FD_EV_ACTIVE_R  (1U << FD_EV_ACTIVE_R_BIT)
#define FD_EV_ACTIVE_W  (1U << FD_EV_ACTIVE_W_BIT)
#define FD_EV_ACTIVE_RW (FD_EV_ACTIVE_R | FD_EV_ACTIVE_W)

#define FD_EV_READY_R   (1U << FD_EV_READY_R_BIT)
#define FD_EV_READY_W   (1U << FD_EV_READY_W_BIT)
#define FD_EV_READY_RW  (FD_EV_READY_R | FD_EV_READY_W)

/* note that when FD_EV_SHUT is set, ACTIVE and READY are cleared */
#define FD_EV_SHUT_R    (1U << FD_EV_SHUT_R_BIT)
#define FD_EV_SHUT_W    (1U << FD_EV_SHUT_W_BIT)
#define FD_EV_SHUT_RW   (FD_EV_SHUT_R | FD_EV_SHUT_W)

/* note that when FD_EV_ERR is set, SHUT is also set. Also, ERR is for both
 * directions at once (write error, socket dead, etc).
 */
#define FD_EV_ERR_RW    (1U << FD_EV_ERR_RW_BIT)


/* This is the value used to mark a file descriptor as dead. This value is
 * negative, this is important so that tests on fd < 0 properly match. It
 * also has the nice property of being highly negative but neither overflowing
 * nor changing sign on 32-bit machines when multiplied by sizeof(fdtab).
 * This ensures that any unexpected dereference of such an uninitialized
 * file descriptor will lead to so large a dereference that it will crash
 * the process at the exact location of the bug with a clean stack trace
 * instead of causing silent manipulation of other FDs. And it's readable
 * when found in a dump.
 */
#define DEAD_FD_MAGIC 0xFDDEADFD

/* fdlist_entry: entry used by the fd cache.
 *    >= 0 means we're in the cache and gives the FD of the next in the cache,
 *      -1 means we're in the cache and the last element,
 *      -2 means the entry is locked,
 *   <= -3 means not in the cache, and next element is -4-fd
 *
 * It must remain 8-aligned so that aligned CAS operations may be done on both
 * entries at once.
 */
struct fdlist_entry {
	int next;
	int prev;
} ALIGNED(8);

/* head of the fd cache */
struct fdlist {
	int first;
	int last;
} ALIGNED(8);

/* info about one given fd. Note: only align on cache lines when using threads;
 * 32-bit small archs can put everything in 32-bytes when threads are disabled.
 *
 * NOTE: DO NOT REORDER THIS STRUCT AT ALL! Some code parts rely on exact field
 * ordering, for example fd_takeover() and fd_set_running() want running_mask
 * immediately followed by thread_mask to perform a double-word-CAS on them.
 */
struct fdtab {
	unsigned long running_mask;          /* mask of thread IDs currently using the fd */
	unsigned long thread_mask;           /* mask of thread IDs authorized to process the fd */
	unsigned long update_mask;           /* mask of thread IDs having an update for fd */
	struct fdlist_entry update;          /* Entry in the global update list */
	void (*iocb)(int fd);                /* I/O handler */
	void *owner;                         /* the connection or listener associated with this fd, NULL if closed */
	unsigned char state;                 /* FD state for read and write directions (FD_EV_*) */
	unsigned char ev;                    /* event seen in return of poll() : FD_POLL_* */
	unsigned char linger_risk:1;         /* 1 if we must kill lingering before closing */
	unsigned char cloned:1;              /* 1 if a cloned socket, requires EPOLL_CTL_DEL on close */
	unsigned char initialized:1;         /* 1 if init phase was done on this fd (e.g. set non-blocking) */
	unsigned char et_possible:1;         /* 1 if edge-triggered is possible on this FD */
#ifdef DEBUG_FD
	unsigned int event_count;            /* number of events reported */
#endif
} THREAD_ALIGNED(64);

/* polled mask, one bit per thread and per direction for each FD */
struct polled_mask {
	unsigned long poll_recv;
	unsigned long poll_send;
};

/* less often used information */
struct fdinfo {
	struct port_range *port_range;       /* optional port range to bind to */
	int local_port;                      /* optional local port */
};

/*
 * Poller descriptors.
 *  - <name> is initialized by the poller's register() function, and should not
 *    be allocated, just linked to.
 *  - <pref> is initialized by the poller's register() function. It is set to 0
 *    by default, meaning the poller is disabled. init() should set it to 0 in
 *    case of failure. term() must set it to 0. A generic unoptimized select()
 *    poller should set it to 100.
 *  - <private> is initialized by the poller's init() function, and cleaned by
 *    the term() function.
 *  - clo() should be used to do indicate the poller that fd will be closed.
 *  - poll() calls the poller, expiring at <exp>, or immediately if <wake> is set
 *  - flags indicate what the poller supports (HAP_POLL_F_*)
 */

#define HAP_POLL_F_RDHUP        0x00000001                   /* the poller notifies of HUP with reads */
#define HAP_POLL_F_ERRHUP       0x00000002                   /* the poller reports ERR and HUP */

struct poller {
	void   *private;                                     /* any private data for the poller */
	void   (*clo)(const int fd);                 /* mark <fd> as closed */
	void   (*poll)(struct poller *p, int exp, int wake);  /* the poller itself */
	int    (*init)(struct poller *p);            /* poller initialization */
	void   (*term)(struct poller *p);            /* termination of this poller */
	int    (*test)(struct poller *p);            /* pre-init check of the poller */
	int    (*fork)(struct poller *p);            /* post-fork re-opening */
	const char   *name;                                  /* poller name */
	unsigned int flags;                                  /* HAP_POLL_F_* */
	int    pref;                                         /* try pollers with higher preference first */
};

#endif /* _HAPROXY_FD_T_H */

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
