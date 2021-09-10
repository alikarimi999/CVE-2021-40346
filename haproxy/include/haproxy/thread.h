/*
 * include/haproxy/thread.h
 * definitions, macros and inline functions used by threads.
 *
 * Copyright (C) 2017 Christopher Faulet - cfaulet@haproxy.com
 * Copyright (C) 2020 Willy Tarreau - w@1wt.eu
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

#ifndef _HAPROXY_THREAD_H
#define _HAPROXY_THREAD_H

#include <signal.h>
#include <unistd.h>
#ifdef _POSIX_PRIORITY_SCHEDULING
#include <sched.h>
#endif

#include <haproxy/api.h>
#include <haproxy/thread-t.h>
#include <haproxy/tinfo.h>


/* Note: this file mainly contains 5 sections:
 *   - a small common part, which also corresponds to the common API
 *   - one used solely when USE_THREAD is *not* set
 *   - one used solely when USE_THREAD is set
 *   - one used solely when USE_THREAD is set WITHOUT debugging
 *   - one used solely when USE_THREAD is set WITH debugging
 *
 */


/* Generic exports */
int parse_nbthread(const char *arg, char **err);
int thread_get_default_count();
extern int thread_cpus_enabled_at_boot;


#ifndef USE_THREAD

/********************** THREADS DISABLED ************************/

/* Only way found to replace variables with constants that are optimized away
 * at build time.
 */
enum { all_threads_mask = 1UL };
enum { threads_harmless_mask = 0 };
enum { threads_sync_mask = 0 };
enum { threads_want_rdv_mask = 0 };
enum { tid_bit = 1UL };
enum { tid = 0 };

#define HA_SPIN_INIT(l)               do { /* do nothing */ } while(0)
#define HA_SPIN_DESTROY(l)            do { /* do nothing */ } while(0)
#define HA_SPIN_LOCK(lbl, l)          do { /* do nothing */ } while(0)
#define HA_SPIN_TRYLOCK(lbl, l)       ({ 0; })
#define HA_SPIN_UNLOCK(lbl, l)        do { /* do nothing */ } while(0)

#define HA_RWLOCK_INIT(l)             do { /* do nothing */ } while(0)
#define HA_RWLOCK_DESTROY(l)          do { /* do nothing */ } while(0)
#define HA_RWLOCK_WRLOCK(lbl, l)      do { /* do nothing */ } while(0)
#define HA_RWLOCK_TRYWRLOCK(lbl, l)   ({ 0; })
#define HA_RWLOCK_WRUNLOCK(lbl, l)    do { /* do nothing */ } while(0)
#define HA_RWLOCK_RDLOCK(lbl, l)      do { /* do nothing */ } while(0)
#define HA_RWLOCK_TRYRDLOCK(lbl, l)   ({ 0; })
#define HA_RWLOCK_RDUNLOCK(lbl, l)    do { /* do nothing */ } while(0)

#define ha_sigmask(how, set, oldset)  sigprocmask(how, set, oldset)

static inline void ha_set_tid(unsigned int tid)
{
	ti = &ha_thread_info[tid];
}

static inline unsigned long long ha_get_pthread_id(unsigned int thr)
{
	return 0;
}

static inline void ha_thread_relax(void)
{
#if _POSIX_PRIORITY_SCHEDULING
	sched_yield();
#endif
}

/* send signal <sig> to thread <thr> */
static inline void ha_tkill(unsigned int thr, int sig)
{
	raise(sig);
}

/* send signal <sig> to all threads */
static inline void ha_tkillall(int sig)
{
	raise(sig);
}

static inline void thread_harmless_now()
{
}

static inline void thread_harmless_end()
{
}

static inline void thread_isolate()
{
}

static inline void thread_release()
{
}

static inline void thread_sync_release()
{
}

static inline unsigned long thread_isolated()
{
	return 1;
}

#else /* !USE_THREAD */

/********************** THREADS ENABLED ************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <import/plock.h>

void thread_harmless_till_end();
void thread_isolate();
void thread_release();
void thread_sync_release();
void ha_tkill(unsigned int thr, int sig);
void ha_tkillall(int sig);
void ha_spin_init(HA_SPINLOCK_T *l);
void ha_rwlock_init(HA_RWLOCK_T *l);

extern volatile unsigned long all_threads_mask;
extern volatile unsigned long threads_harmless_mask;
extern volatile unsigned long threads_sync_mask;
extern volatile unsigned long threads_want_rdv_mask;
extern THREAD_LOCAL unsigned long tid_bit; /* The bit corresponding to the thread id */
extern THREAD_LOCAL unsigned int tid;      /* The thread id */

/* explanation for threads_want_rdv_mask, threads_harmless_mask, and
 * threads_sync_mask :
 * - threads_want_rdv_mask is a bit field indicating all threads that have
 *   requested a rendez-vous of other threads using thread_isolate().
 * - threads_harmless_mask is a bit field indicating all threads that are
 *   currently harmless in that they promise not to access a shared resource.
 * - threads_sync_mask is a bit field indicating that a thread waiting for
 *   others to finish wants to leave synchronized with others and as such
 *   promises to do so as well using thread_sync_release().
 *
 * For a given thread, its bits in want_rdv and harmless can be translated like
 * this :
 *
 *  ----------+----------+----------------------------------------------------
 *   want_rdv | harmless | description
 *  ----------+----------+----------------------------------------------------
 *       0    |     0    | thread not interested in RDV, possibly harmful
 *       0    |     1    | thread not interested in RDV but harmless
 *       1    |     1    | thread interested in RDV and waiting for its turn
 *       1    |     0    | thread currently working isolated from others
 *  ----------+----------+----------------------------------------------------
 *
 * thread_sync_mask only delays the leaving of threads_sync_release() to make
 * sure that each thread's harmless bit is cleared before leaving the function.
 */

#define ha_sigmask(how, set, oldset)  pthread_sigmask(how, set, oldset)

/* sets the thread ID and the TID bit for the current thread */
static inline void ha_set_tid(unsigned int data)
{
	tid     = data;
	tid_bit = (1UL << tid);
	ti      = &ha_thread_info[tid];
}

/* Retrieves the opaque pthread_t of thread <thr> cast to an unsigned long long
 * since POSIX took great care of not specifying its representation, making it
 * hard to export for post-mortem analysis. For this reason we copy it into a
 * union and will use the smallest scalar type at least as large as its size,
 * which will keep endianness and alignment for all regular sizes. As a last
 * resort we end up with a long long ligned to the first bytes in memory, which
 * will be endian-dependent if pthread_t is larger than a long long (not seen
 * yet).
 */
static inline unsigned long long ha_get_pthread_id(unsigned int thr)
{
	union {
		pthread_t t;
		unsigned long long ll;
		unsigned int i;
		unsigned short s;
		unsigned char c;
	} u;

	memset(&u, 0, sizeof(u));
	u.t = ha_thread_info[thr].pthread;

	if (sizeof(u.t) <= sizeof(u.c))
		return u.c;
	else if (sizeof(u.t) <= sizeof(u.s))
		return u.s;
	else if (sizeof(u.t) <= sizeof(u.i))
		return u.i;
	return u.ll;
}

static inline void ha_thread_relax(void)
{
#if _POSIX_PRIORITY_SCHEDULING
	sched_yield();
#else
	pl_cpu_relax();
#endif
}

/* Marks the thread as harmless. Note: this must be true, i.e. the thread must
 * not be touching any unprotected shared resource during this period. Usually
 * this is called before poll(), but it may also be placed around very slow
 * calls (eg: some crypto operations). Needs to be terminated using
 * thread_harmless_end().
 */
static inline void thread_harmless_now()
{
	HA_ATOMIC_OR(&threads_harmless_mask, tid_bit);
}

/* Ends the harmless period started by thread_harmless_now(). Usually this is
 * placed after the poll() call. If it is discovered that a job was running and
 * is relying on the thread still being harmless, the thread waits for the
 * other one to finish.
 */
static inline void thread_harmless_end()
{
	while (1) {
		HA_ATOMIC_AND(&threads_harmless_mask, ~tid_bit);
		if (likely((threads_want_rdv_mask & ~tid_bit) == 0))
			break;
		thread_harmless_till_end();
	}
}

/* an isolated thread has harmless cleared and want_rdv set */
static inline unsigned long thread_isolated()
{
	return threads_want_rdv_mask & ~threads_harmless_mask & tid_bit;
}


#if !defined(DEBUG_THREAD) && !defined(DEBUG_FULL)

/* Thread debugging is DISABLED, these are the regular locking functions */

#define HA_SPIN_INIT(l)            ({ (*l) = 0; })
#define HA_SPIN_DESTROY(l)         ({ (*l) = 0; })
#define HA_SPIN_LOCK(lbl, l)       pl_take_s(l)
#define HA_SPIN_TRYLOCK(lbl, l)    (!pl_try_s(l))
#define HA_SPIN_UNLOCK(lbl, l)     pl_drop_s(l)

#define HA_RWLOCK_INIT(l)          ({ (*l) = 0; })
#define HA_RWLOCK_DESTROY(l)       ({ (*l) = 0; })
#define HA_RWLOCK_WRLOCK(lbl,l)    pl_take_w(l)
#define HA_RWLOCK_TRYWRLOCK(lbl,l) (!pl_try_w(l))
#define HA_RWLOCK_WRUNLOCK(lbl,l)  pl_drop_w(l)
#define HA_RWLOCK_RDLOCK(lbl,l)    pl_take_r(l)
#define HA_RWLOCK_TRYRDLOCK(lbl,l) (!pl_try_r(l))
#define HA_RWLOCK_RDUNLOCK(lbl,l)  pl_drop_r(l)

#else /* !defined(DEBUG_THREAD) && !defined(DEBUG_FULL) */

/* Thread debugging is ENABLED, these are the instrumented functions */

#define __SPIN_INIT(l)             ({ (*l) = 0; })
#define __SPIN_DESTROY(l)          ({ (*l) = 0; })
#define __SPIN_LOCK(l)             pl_take_s(l)
#define __SPIN_TRYLOCK(l)          (!pl_try_s(l))
#define __SPIN_UNLOCK(l)           pl_drop_s(l)

#define __RWLOCK_INIT(l)           ({ (*l) = 0; })
#define __RWLOCK_DESTROY(l)        ({ (*l) = 0; })
#define __RWLOCK_WRLOCK(l)         pl_take_w(l)
#define __RWLOCK_TRYWRLOCK(l)      (!pl_try_w(l))
#define __RWLOCK_WRUNLOCK(l)       pl_drop_w(l)
#define __RWLOCK_RDLOCK(l)         pl_take_r(l)
#define __RWLOCK_TRYRDLOCK(l)      (!pl_try_r(l))
#define __RWLOCK_RDUNLOCK(l)       pl_drop_r(l)

#define HA_SPIN_INIT(l)            __spin_init(l)
#define HA_SPIN_DESTROY(l)         __spin_destroy(l)

#define HA_SPIN_LOCK(lbl, l)       __spin_lock(lbl, l, __func__, __FILE__, __LINE__)
#define HA_SPIN_TRYLOCK(lbl, l)    __spin_trylock(lbl, l, __func__, __FILE__, __LINE__)
#define HA_SPIN_UNLOCK(lbl, l)     __spin_unlock(lbl, l, __func__, __FILE__, __LINE__)

#define HA_RWLOCK_INIT(l)          __ha_rwlock_init((l))
#define HA_RWLOCK_DESTROY(l)       __ha_rwlock_destroy((l))
#define HA_RWLOCK_WRLOCK(lbl,l)    __ha_rwlock_wrlock(lbl, l, __func__, __FILE__, __LINE__)
#define HA_RWLOCK_TRYWRLOCK(lbl,l) __ha_rwlock_trywrlock(lbl, l, __func__, __FILE__, __LINE__)
#define HA_RWLOCK_WRUNLOCK(lbl,l)  __ha_rwlock_wrunlock(lbl, l, __func__, __FILE__, __LINE__)
#define HA_RWLOCK_RDLOCK(lbl,l)    __ha_rwlock_rdlock(lbl, l)
#define HA_RWLOCK_TRYRDLOCK(lbl,l) __ha_rwlock_tryrdlock(lbl, l)
#define HA_RWLOCK_RDUNLOCK(lbl,l)  __ha_rwlock_rdunlock(lbl, l)

/* WARNING!!! if you update this enum, please also keep lock_label() up to date
 * below.
 */
enum lock_label {
	TASK_RQ_LOCK,
	TASK_WQ_LOCK,
	POOL_LOCK,
	LISTENER_LOCK,
	PROXY_LOCK,
	SERVER_LOCK,
	LBPRM_LOCK,
	SIGNALS_LOCK,
	STK_TABLE_LOCK,
	STK_SESS_LOCK,
	APPLETS_LOCK,
	PEER_LOCK,
	STRMS_LOCK,
	SHCTX_LOCK,
	SSL_LOCK,
	SSL_GEN_CERTS_LOCK,
	PATREF_LOCK,
	PATEXP_LOCK,
	VARS_LOCK,
	COMP_POOL_LOCK,
	LUA_LOCK,
	NOTIF_LOCK,
	SPOE_APPLET_LOCK,
	DNS_LOCK,
	PID_LIST_LOCK,
	EMAIL_ALERTS_LOCK,
	PIPES_LOCK,
	TLSKEYS_REF_LOCK,
	AUTH_LOCK,
	LOGSRV_LOCK,
	DICT_LOCK,
	PROTO_LOCK,
	CKCH_LOCK,
	SNI_LOCK,
	SFT_LOCK, /* sink forward target */
	OTHER_LOCK,
	LOCK_LABELS
};

extern struct lock_stat lock_stats[LOCK_LABELS];

static inline const char *lock_label(enum lock_label label)
{
	switch (label) {
	case TASK_RQ_LOCK:         return "TASK_RQ";
	case TASK_WQ_LOCK:         return "TASK_WQ";
	case POOL_LOCK:            return "POOL";
	case LISTENER_LOCK:        return "LISTENER";
	case PROXY_LOCK:           return "PROXY";
	case SERVER_LOCK:          return "SERVER";
	case LBPRM_LOCK:           return "LBPRM";
	case SIGNALS_LOCK:         return "SIGNALS";
	case STK_TABLE_LOCK:       return "STK_TABLE";
	case STK_SESS_LOCK:        return "STK_SESS";
	case APPLETS_LOCK:         return "APPLETS";
	case PEER_LOCK:            return "PEER";
	case STRMS_LOCK:           return "STRMS";
	case SHCTX_LOCK:           return "SHCTX";
	case SSL_LOCK:             return "SSL";
	case SSL_GEN_CERTS_LOCK:   return "SSL_GEN_CERTS";
	case PATREF_LOCK:          return "PATREF";
	case PATEXP_LOCK:          return "PATEXP";
	case VARS_LOCK:            return "VARS";
	case COMP_POOL_LOCK:       return "COMP_POOL";
	case LUA_LOCK:             return "LUA";
	case NOTIF_LOCK:           return "NOTIF";
	case SPOE_APPLET_LOCK:     return "SPOE_APPLET";
	case DNS_LOCK:             return "DNS";
	case PID_LIST_LOCK:        return "PID_LIST";
	case EMAIL_ALERTS_LOCK:    return "EMAIL_ALERTS";
	case PIPES_LOCK:           return "PIPES";
	case TLSKEYS_REF_LOCK:     return "TLSKEYS_REF";
	case AUTH_LOCK:            return "AUTH";
	case LOGSRV_LOCK:          return "LOGSRV";
	case DICT_LOCK:            return "DICT";
	case PROTO_LOCK:           return "PROTO";
	case CKCH_LOCK:            return "CKCH";
	case SNI_LOCK:             return "SNI";
	case SFT_LOCK:             return "SFT";
	case OTHER_LOCK:           return "OTHER";
	case LOCK_LABELS:          break; /* keep compiler happy */
	};
	/* only way to come here is consecutive to an internal bug */
	abort();
}

static inline void show_lock_stats()
{
	int lbl;

	for (lbl = 0; lbl < LOCK_LABELS; lbl++) {
		fprintf(stderr,
			"Stats about Lock %s: \n"
			"\t # write lock  : %lu\n"
			"\t # write unlock: %lu (%ld)\n"
			"\t # wait time for write     : %.3f msec\n"
			"\t # wait time for write/lock: %.3f nsec\n"
			"\t # read lock   : %lu\n"
			"\t # read unlock : %lu (%ld)\n"
			"\t # wait time for read      : %.3f msec\n"
			"\t # wait time for read/lock : %.3f nsec\n",
			lock_label(lbl),
			lock_stats[lbl].num_write_locked,
			lock_stats[lbl].num_write_unlocked,
			lock_stats[lbl].num_write_unlocked - lock_stats[lbl].num_write_locked,
			(double)lock_stats[lbl].nsec_wait_for_write / 1000000.0,
			lock_stats[lbl].num_write_locked ? ((double)lock_stats[lbl].nsec_wait_for_write / (double)lock_stats[lbl].num_write_locked) : 0,
			lock_stats[lbl].num_read_locked,
			lock_stats[lbl].num_read_unlocked,
			lock_stats[lbl].num_read_unlocked - lock_stats[lbl].num_read_locked,
			(double)lock_stats[lbl].nsec_wait_for_read / 1000000.0,
			lock_stats[lbl].num_read_locked ? ((double)lock_stats[lbl].nsec_wait_for_read / (double)lock_stats[lbl].num_read_locked) : 0);
	}
}

/* Following functions are used to collect some stats about locks. We wrap
 * pthread functions to known how much time we wait in a lock. */

static uint64_t nsec_now(void)
{
        struct timespec ts;

        clock_gettime(CLOCK_MONOTONIC, &ts);
        return ((uint64_t) ts.tv_sec * 1000000000ULL +
                (uint64_t) ts.tv_nsec);
}

static inline void __ha_rwlock_init(struct ha_rwlock *l)
{
	memset(l, 0, sizeof(struct ha_rwlock));
	__RWLOCK_INIT(&l->lock);
}

static inline void __ha_rwlock_destroy(struct ha_rwlock *l)
{
	__RWLOCK_DESTROY(&l->lock);
	memset(l, 0, sizeof(struct ha_rwlock));
}


static inline void __ha_rwlock_wrlock(enum lock_label lbl, struct ha_rwlock *l,
				      const char *func, const char *file, int line)
{
	uint64_t start_time;

	if (unlikely(l->info.cur_writer & tid_bit)) {
		/* the thread is already owning the lock for write */
		abort();
	}

	if (unlikely(l->info.cur_readers & tid_bit)) {
		/* the thread is already owning the lock for read */
		abort();
	}

	HA_ATOMIC_OR(&l->info.wait_writers, tid_bit);

	start_time = nsec_now();
	__RWLOCK_WRLOCK(&l->lock);
	HA_ATOMIC_ADD(&lock_stats[lbl].nsec_wait_for_write, (nsec_now() - start_time));

	HA_ATOMIC_ADD(&lock_stats[lbl].num_write_locked, 1);

	l->info.cur_writer             = tid_bit;
	l->info.last_location.function = func;
	l->info.last_location.file     = file;
	l->info.last_location.line     = line;

	HA_ATOMIC_AND(&l->info.wait_writers, ~tid_bit);
}

static inline int __ha_rwlock_trywrlock(enum lock_label lbl, struct ha_rwlock *l,
				        const char *func, const char *file, int line)
{
	uint64_t start_time;
	int r;

	if (unlikely(l->info.cur_writer & tid_bit)) {
		/* the thread is already owning the lock for write */
		abort();
	}

	if (unlikely(l->info.cur_readers & tid_bit)) {
		/* the thread is already owning the lock for read */
		abort();
	}

	/* We set waiting writer because trywrlock could wait for readers to quit */
	HA_ATOMIC_OR(&l->info.wait_writers, tid_bit);

	start_time = nsec_now();
	r = __RWLOCK_TRYWRLOCK(&l->lock);
	HA_ATOMIC_ADD(&lock_stats[lbl].nsec_wait_for_write, (nsec_now() - start_time));
	if (unlikely(r)) {
		HA_ATOMIC_AND(&l->info.wait_writers, ~tid_bit);
		return r;
	}
	HA_ATOMIC_ADD(&lock_stats[lbl].num_write_locked, 1);

	l->info.cur_writer             = tid_bit;
	l->info.last_location.function = func;
	l->info.last_location.file     = file;
	l->info.last_location.line     = line;

	HA_ATOMIC_AND(&l->info.wait_writers, ~tid_bit);

	return 0;
}

static inline void __ha_rwlock_wrunlock(enum lock_label lbl,struct ha_rwlock *l,
				        const char *func, const char *file, int line)
{
	if (unlikely(!(l->info.cur_writer & tid_bit))) {
		/* the thread is not owning the lock for write */
		abort();
	}

	l->info.cur_writer             = 0;
	l->info.last_location.function = func;
	l->info.last_location.file     = file;
	l->info.last_location.line     = line;

	__RWLOCK_WRUNLOCK(&l->lock);

	HA_ATOMIC_ADD(&lock_stats[lbl].num_write_unlocked, 1);
}

static inline void __ha_rwlock_rdlock(enum lock_label lbl,struct ha_rwlock *l)
{
	uint64_t start_time;

	if (unlikely(l->info.cur_writer & tid_bit)) {
		/* the thread is already owning the lock for write */
		abort();
	}

	if (unlikely(l->info.cur_readers & tid_bit)) {
		/* the thread is already owning the lock for read */
		abort();
	}

	HA_ATOMIC_OR(&l->info.wait_readers, tid_bit);

	start_time = nsec_now();
	__RWLOCK_RDLOCK(&l->lock);
	HA_ATOMIC_ADD(&lock_stats[lbl].nsec_wait_for_read, (nsec_now() - start_time));
	HA_ATOMIC_ADD(&lock_stats[lbl].num_read_locked, 1);

	HA_ATOMIC_OR(&l->info.cur_readers, tid_bit);

	HA_ATOMIC_AND(&l->info.wait_readers, ~tid_bit);
}

static inline int __ha_rwlock_tryrdlock(enum lock_label lbl,struct ha_rwlock *l)
{
	int r;

	if (unlikely(l->info.cur_writer & tid_bit)) {
		/* the thread is already owning the lock for write */
		abort();
	}

	if (unlikely(l->info.cur_readers & tid_bit)) {
		/* the thread is already owning the lock for read */
		abort();
	}

	/* try read should never wait */
	r = __RWLOCK_TRYRDLOCK(&l->lock);
	if (unlikely(r))
		return r;
	HA_ATOMIC_ADD(&lock_stats[lbl].num_read_locked, 1);

	HA_ATOMIC_OR(&l->info.cur_readers, tid_bit);

	return 0;
}

static inline void __ha_rwlock_rdunlock(enum lock_label lbl,struct ha_rwlock *l)
{
	if (unlikely(!(l->info.cur_readers & tid_bit))) {
		/* the thread is not owning the lock for read */
		abort();
	}

	HA_ATOMIC_AND(&l->info.cur_readers, ~tid_bit);

	__RWLOCK_RDUNLOCK(&l->lock);

	HA_ATOMIC_ADD(&lock_stats[lbl].num_read_unlocked, 1);
}

static inline void __spin_init(struct ha_spinlock *l)
{
	memset(l, 0, sizeof(struct ha_spinlock));
	__SPIN_INIT(&l->lock);
}

static inline void __spin_destroy(struct ha_spinlock *l)
{
	__SPIN_DESTROY(&l->lock);
	memset(l, 0, sizeof(struct ha_spinlock));
}

static inline void __spin_lock(enum lock_label lbl, struct ha_spinlock *l,
			      const char *func, const char *file, int line)
{
	uint64_t start_time;

	if (unlikely(l->info.owner & tid_bit)) {
		/* the thread is already owning the lock */
		abort();
	}

	HA_ATOMIC_OR(&l->info.waiters, tid_bit);

	start_time = nsec_now();
	__SPIN_LOCK(&l->lock);
	HA_ATOMIC_ADD(&lock_stats[lbl].nsec_wait_for_write, (nsec_now() - start_time));

	HA_ATOMIC_ADD(&lock_stats[lbl].num_write_locked, 1);


	l->info.owner                  = tid_bit;
	l->info.last_location.function = func;
	l->info.last_location.file     = file;
	l->info.last_location.line     = line;

	HA_ATOMIC_AND(&l->info.waiters, ~tid_bit);
}

static inline int __spin_trylock(enum lock_label lbl, struct ha_spinlock *l,
				 const char *func, const char *file, int line)
{
	int r;

	if (unlikely(l->info.owner & tid_bit)) {
		/* the thread is already owning the lock */
		abort();
	}

	/* try read should never wait */
	r = __SPIN_TRYLOCK(&l->lock);
	if (unlikely(r))
		return r;
	HA_ATOMIC_ADD(&lock_stats[lbl].num_write_locked, 1);

	l->info.owner                  = tid_bit;
	l->info.last_location.function = func;
	l->info.last_location.file     = file;
	l->info.last_location.line     = line;

	return 0;
}

static inline void __spin_unlock(enum lock_label lbl, struct ha_spinlock *l,
				 const char *func, const char *file, int line)
{
	if (unlikely(!(l->info.owner & tid_bit))) {
		/* the thread is not owning the lock */
		abort();
	}

	l->info.owner                  = 0;
	l->info.last_location.function = func;
	l->info.last_location.file     = file;
	l->info.last_location.line     = line;

	__SPIN_UNLOCK(&l->lock);
	HA_ATOMIC_ADD(&lock_stats[lbl].num_write_unlocked, 1);
}

#endif  /* DEBUG_THREAD */

#endif /* USE_THREAD */

#endif /* _HAPROXY_THREAD_H */
