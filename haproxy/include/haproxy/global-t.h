/*
 * include/haproxy/global-t.h
 * Global types and macros. Please avoid adding more stuff here!
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

#ifndef _HAPROXY_GLOBAL_T_H
#define _HAPROXY_GLOBAL_T_H

#include <haproxy/api-t.h>
#include <haproxy/buf-t.h>
#include <haproxy/freq_ctr-t.h>
#include <haproxy/vars-t.h>

/* modes of operation (global.mode) */
#define	MODE_DEBUG	0x01
#define	MODE_DAEMON	0x02
#define	MODE_QUIET	0x04
#define	MODE_CHECK	0x08
#define	MODE_VERBOSE	0x10
#define	MODE_STARTING	0x20
#define	MODE_FOREGROUND	0x40
#define	MODE_MWORKER	0x80    /* Master Worker */
#define	MODE_MWORKER_WAIT	0x100    /* Master Worker wait mode */
#define	MODE_ZERO_WARNING       0x200    /* warnings cause a failure */

/* list of last checks to perform, depending on config options */
#define LSTCHK_CAP_BIND	0x00000001	/* check that we can bind to any port */
#define LSTCHK_NETADM	0x00000002	/* check that we have CAP_NET_ADMIN */

/* Global tuning options */
/* available polling mechanisms */
#define GTUNE_USE_SELECT         (1<<0)
#define GTUNE_USE_POLL           (1<<1)
#define GTUNE_USE_EPOLL          (1<<2)
#define GTUNE_USE_KQUEUE         (1<<3)
/* platform-specific options */
#define GTUNE_USE_SPLICE         (1<<4)
#define GTUNE_USE_GAI            (1<<5)
#define GTUNE_USE_REUSEPORT      (1<<6)
#define GTUNE_RESOLVE_DONTFAIL   (1<<7)

#define GTUNE_SOCKET_TRANSFER	 (1<<8)
#define GTUNE_NOEXIT_ONFAILURE   (1<<9)
#define GTUNE_USE_SYSTEMD        (1<<10)

#define GTUNE_BUSY_POLLING       (1<<11)
#define GTUNE_LISTENER_MQ        (1<<12)
#define GTUNE_SET_DUMPABLE       (1<<13)
#define GTUNE_USE_EVPORTS        (1<<14)
#define GTUNE_STRICT_LIMITS      (1<<15)
#define GTUNE_INSECURE_FORK      (1<<16)
#define GTUNE_INSECURE_SETUID    (1<<17)
#define GTUNE_FD_ET              (1<<18)
#define GTUNE_SCHED_LOW_LATENCY  (1<<19)
#define GTUNE_IDLE_POOL_SHARED   (1<<20)

/* SSL server verify mode */
enum {
	SSL_SERVER_VERIFY_NONE = 0,
	SSL_SERVER_VERIFY_REQUIRED = 1,
};

/* bit values to go with "warned" above */
#define WARN_ANY                    0x00000001 /* any warning was emitted */
#define WARN_FORCECLOSE_DEPRECATED  0x00000002
#define WARN_EXEC_PATH              0x00000004 /* executable path already reported */

/* put there the forward declarations needed for global.h */
struct proxy;

/* FIXME : this will have to be redefined correctly */
struct global {
	int uid;
	int gid;
	int external_check;
	int nbproc;
	int nbthread;
	unsigned int hard_stop_after;	/* maximum time allowed to perform a soft-stop */
	int maxconn, hardmaxconn;
	int maxsslconn;
	int ssl_session_max_cost;   /* how many bytes an SSL session may cost */
	int ssl_handshake_max_cost; /* how many bytes an SSL handshake may use */
	int ssl_used_frontend;      /* non-zero if SSL is used in a frontend */
	int ssl_used_backend;       /* non-zero if SSL is used in a backend */
	int ssl_used_async_engines; /* number of used async engines */
	unsigned int ssl_server_verify; /* default verify mode on servers side */
	struct freq_ctr conn_per_sec;
	struct freq_ctr sess_per_sec;
	struct freq_ctr ssl_per_sec;
	struct freq_ctr ssl_fe_keys_per_sec;
	struct freq_ctr ssl_be_keys_per_sec;
	struct freq_ctr comp_bps_in;	/* bytes per second, before http compression */
	struct freq_ctr comp_bps_out;	/* bytes per second, after http compression */
	struct freq_ctr out_32bps;      /* #of 32-byte blocks emitted per second */
	unsigned long long out_bytes;   /* total #of bytes emitted */
	int cps_lim, cps_max;
	int sps_lim, sps_max;
	int ssl_lim, ssl_max;
	int ssl_fe_keys_max, ssl_be_keys_max;
	unsigned int shctx_lookups, shctx_misses;
	int comp_rate_lim;           /* HTTP compression rate limit */
	int maxpipes;		/* max # of pipes */
	int maxsock;		/* max # of sockets */
	int rlimit_nofile;	/* default ulimit-n value : 0=unset */
	int rlimit_memmax_all;	/* default all-process memory limit in megs ; 0=unset */
	int rlimit_memmax;	/* default per-process memory limit in megs ; 0=unset */
	long maxzlibmem;        /* max RAM for zlib in bytes */
	int mode;
	unsigned int req_count; /* request counter (HTTP or TCP session) for logs and unique_id */
	int last_checks;
	int spread_checks;
	int max_spread_checks;
	int max_syslog_len;
	char *chroot;
	char *pidfile;
	char *node, *desc;		/* node name & description */
	int localpeer_cmdline;		/* whether or not the commandline "-L" was set */
	struct buffer log_tag;           /* name for syslog */
	struct list logsrvs;
	char *log_send_hostname;   /* set hostname in syslog header */
	char *server_state_base;   /* path to a directory where server state files can be found */
	char *server_state_file;   /* path to the file where server states are loaded from */
	struct {
		int maxpollevents; /* max number of poll events at once */
		int maxaccept;     /* max number of consecutive accept() */
		int options;       /* various tuning options */
		int runqueue_depth;/* max number of tasks to run at once */
		int recv_enough;   /* how many input bytes at once are "enough" */
		int bufsize;       /* buffer size in bytes, defaults to BUFSIZE */
		int maxrewrite;    /* buffer max rewrite size in bytes, defaults to MAXREWRITE */
		int reserved_bufs; /* how many buffers can only be allocated for response */
		int buf_limit;     /* if not null, how many total buffers may only be allocated */
		int client_sndbuf; /* set client sndbuf to this value if not null */
		int client_rcvbuf; /* set client rcvbuf to this value if not null */
		int server_sndbuf; /* set server sndbuf to this value if not null */
		int server_rcvbuf; /* set server rcvbuf to this value if not null */
		int pipesize;      /* pipe size in bytes, system defaults if zero */
		int max_http_hdr;  /* max number of HTTP headers, use MAX_HTTP_HDR if zero */
		int requri_len;    /* max len of request URI, use REQURI_LEN if zero */
		int cookie_len;    /* max length of cookie captures */
		int pattern_cache; /* max number of entries in the pattern cache. */
		int sslcachesize;  /* SSL cache size in session, defaults to 20000 */
		int comp_maxlevel;    /* max HTTP compression level */
		int pool_low_ratio;   /* max ratio of FDs used before we stop using new idle connections */
		int pool_high_ratio;  /* max ratio of FDs used before we start killing idle connections when creating new connections */
		int pool_low_count;   /* max number of opened fd before we stop using new idle connections */
		int pool_high_count;  /* max number of opened fd before we start killing idle connections when creating new connections */
		unsigned short idle_timer; /* how long before an empty buffer is considered idle (ms) */
	} tune;
	struct {
		char *prefix;           /* path prefix of unix bind socket */
		struct {                /* UNIX socket permissions */
			uid_t uid;      /* -1 to leave unchanged */
			gid_t gid;      /* -1 to leave unchanged */
			mode_t mode;    /* 0 to leave unchanged */
		} ux;
	} unix_bind;
	struct proxy *stats_fe;     /* the frontend holding the stats settings */
	struct vars   vars;         /* list of variables for the process scope. */
#ifdef USE_CPU_AFFINITY
	struct {
		unsigned long proc[MAX_PROCS];      /* list of CPU masks for the 32/64 first processes */
		unsigned long proc_t1[MAX_PROCS];   /* list of CPU masks for the 1st thread of each process */
		unsigned long thread[MAX_THREADS];  /* list of CPU masks for the 32/64 first threads of the 1st process */
	} cpu_map;
#endif
};

#endif /* _HAPROXY_GLOBAL_T_H */

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
