/*
 * HTT/1 mux-demux for connections
 *
 * Copyright 2018 Christopher Faulet <cfaulet@haproxy.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */
#include <import/ebistree.h>

#include <haproxy/api.h>
#include <haproxy/cfgparse.h>
#include <haproxy/connection.h>
#include <haproxy/h1.h>
#include <haproxy/h1_htx.h>
#include <haproxy/h2.h>
#include <haproxy/http_htx.h>
#include <haproxy/htx.h>
#include <haproxy/istbuf.h>
#include <haproxy/log.h>
#include <haproxy/pipe-t.h>
#include <haproxy/proxy-t.h>
#include <haproxy/session-t.h>
#include <haproxy/stream.h>
#include <haproxy/stream_interface.h>
#include <haproxy/trace.h>

/*
 *  H1 Connection flags (32 bits)
 */
#define H1C_F_NONE           0x00000000

/* Flags indicating why writing output data are blocked */
#define H1C_F_OUT_ALLOC      0x00000001 /* mux is blocked on lack of output buffer */
#define H1C_F_OUT_FULL       0x00000002 /* mux is blocked on output buffer full */
/* 0x00000004 - 0x00000008 unused */

/* Flags indicating why reading input data are blocked. */
#define H1C_F_IN_ALLOC       0x00000010 /* mux is blocked on lack of input buffer */
#define H1C_F_IN_FULL        0x00000020 /* mux is blocked on input buffer full */
#define H1C_F_IN_BUSY        0x00000040 /* mux is blocked on input waiting the other side */
/* 0x00000040 - 0x00000800 unused */

/* Flags indicating the connection state */
#define H1C_F_CS_ERROR       0x00001000 /* connection must be closed ASAP because an error occurred */
#define H1C_F_CS_SHUTW_NOW   0x00002000 /* connection must be shut down for writes ASAP */
#define H1C_F_CS_SHUTDOWN    0x00004000 /* connection is shut down */
#define H1C_F_CS_IDLE        0x00008000 /* connection is idle and may be reused
					 * (exclusive to all H1C_F_CS flags and never set when an h1s is attached) */

#define H1C_F_WAIT_NEXT_REQ  0x00010000 /*  waiting for the next request to start, use keep-alive timeout */
#define H1C_F_UPG_H2C        0x00020000 /* set if an upgrade to h2 should be done */

#define H1C_F_CO_MSG_MORE    0x00040000 /* set if CO_SFL_MSG_MORE must be set when calling xprt->snd_buf() */
#define H1C_F_CO_STREAMER    0x00080000 /* set if CO_SFL_STREAMER must be set when calling xprt->snd_buf() */

/*
 * H1 Stream flags (32 bits)
 */
#define H1S_F_NONE           0x00000000
#define H1S_F_ERROR          0x00000001 /* An error occurred on the H1 stream */
#define H1S_F_REQ_ERROR      0x00000002 /* An error occurred during the request parsing/xfer */
#define H1S_F_RES_ERROR      0x00000004 /* An error occurred during the response parsing/xfer */
#define H1S_F_REOS           0x00000008 /* End of input stream seen even if not delivered yet */
#define H1S_F_WANT_KAL       0x00000010
#define H1S_F_WANT_TUN       0x00000020
#define H1S_F_WANT_CLO       0x00000040
#define H1S_F_WANT_MSK       0x00000070
#define H1S_F_NOT_FIRST      0x00000080 /* The H1 stream is not the first one */
#define H1S_F_BUF_FLUSH      0x00000100 /* Flush input buffer and don't read more data */
#define H1S_F_SPLICED_DATA   0x00000200 /* Set when the kernel splicing is in used */
#define H1S_F_PARSING_DONE   0x00000400 /* Set when incoming message parsing is finished (EOM added) */
/* 0x00000800 .. 0x00001000 unused */
#define H1S_F_HAVE_SRV_NAME  0x00002000 /* Set during output process if the server name header was added to the request */
#define H1S_F_HAVE_O_CONN    0x00004000 /* Set during output process to know connection mode was processed */

/* H1 connection descriptor */
struct h1c {
	struct connection *conn;
	struct proxy *px;
	uint32_t flags;                  /* Connection flags: H1C_F_* */

	struct buffer ibuf;              /* Input buffer to store data before parsing */
	struct buffer obuf;              /* Output buffer to store data after reformatting */

	struct buffer_wait buf_wait;     /* Wait list for buffer allocation */
	struct wait_event wait_event;    /* To be used if we're waiting for I/Os */

	struct h1s *h1s;                 /* H1 stream descriptor */
	struct task *task;               /* timeout management task */
	int timeout;                     /* idle timeout duration in ticks */
	int shut_timeout;                /* idle timeout duration in ticks after stream shutdown */
};

/* H1 stream descriptor */
struct h1s {
	struct h1c *h1c;
	struct conn_stream *cs;
	struct cs_info csinfo;         /* CS info, only used for client connections */
	uint32_t flags;                /* Connection flags: H1S_F_* */

	struct wait_event *subs;      /* Address of the wait_event the conn_stream associated is waiting on */

	struct session *sess;         /* Associated session */
	struct h1m req;
	struct h1m res;

	enum http_meth_t meth; /* HTTP request method  */
	uint16_t status;       /* HTTP response status */
};

/* Map of headers used to convert outgoing headers */
struct h1_hdrs_map {
	char *name;
	struct eb_root map;
};

/* An entry in a headers map */
struct h1_hdr_entry  {
	struct ist name;
	struct ebpt_node node;
};

/* Declare the headers map */
static struct h1_hdrs_map hdrs_map = { .name = NULL, .map  = EB_ROOT };


/* trace source and events */
static void h1_trace(enum trace_level level, uint64_t mask,
                     const struct trace_source *src,
                     const struct ist where, const struct ist func,
                     const void *a1, const void *a2, const void *a3, const void *a4);

/* The event representation is split like this :
 *   h1c   - internal H1 connection
 *   h1s   - internal H1 stream
 *   strm  - application layer
 *   rx    - data receipt
 *   tx    - data transmission
 *
 */
static const struct trace_event h1_trace_events[] = {
#define           H1_EV_H1C_NEW       (1ULL <<  0)
	{ .mask = H1_EV_H1C_NEW,      .name = "h1c_new",      .desc = "new H1 connection" },
#define           H1_EV_H1C_RECV      (1ULL <<  1)
	{ .mask = H1_EV_H1C_RECV,     .name = "h1c_recv",     .desc = "Rx on H1 connection" },
#define           H1_EV_H1C_SEND      (1ULL <<  2)
	{ .mask = H1_EV_H1C_SEND,     .name = "h1c_send",     .desc = "Tx on H1 connection" },
#define           H1_EV_H1C_BLK       (1ULL <<  3)
	{ .mask = H1_EV_H1C_BLK,      .name = "h1c_blk",      .desc = "H1 connection blocked" },
#define           H1_EV_H1C_WAKE      (1ULL <<  4)
	{ .mask = H1_EV_H1C_WAKE,     .name = "h1c_wake",     .desc = "H1 connection woken up" },
#define           H1_EV_H1C_END       (1ULL <<  5)
	{ .mask = H1_EV_H1C_END,      .name = "h1c_end",      .desc = "H1 connection terminated" },
#define           H1_EV_H1C_ERR       (1ULL <<  6)
	{ .mask = H1_EV_H1C_ERR,      .name = "h1c_err",      .desc = "error on H1 connection" },

#define           H1_EV_RX_DATA       (1ULL <<  7)
	{ .mask = H1_EV_RX_DATA,      .name = "rx_data",      .desc = "receipt of any H1 data" },
#define           H1_EV_RX_EOI        (1ULL <<  8)
	{ .mask = H1_EV_RX_EOI,       .name = "rx_eoi",       .desc = "receipt of end of H1 input" },
#define           H1_EV_RX_HDRS       (1ULL <<  9)
	{ .mask = H1_EV_RX_HDRS,      .name = "rx_headers",   .desc = "receipt of H1 headers" },
#define           H1_EV_RX_BODY       (1ULL << 10)
	{ .mask = H1_EV_RX_BODY,      .name = "rx_body",      .desc = "receipt of H1 body" },
#define           H1_EV_RX_TLRS       (1ULL << 11)
	{ .mask = H1_EV_RX_TLRS,      .name = "rx_trailerus", .desc = "receipt of H1 trailers" },

#define           H1_EV_TX_DATA       (1ULL << 12)
	{ .mask = H1_EV_TX_DATA,      .name = "tx_data",      .desc = "transmission of any H1 data" },
#define           H1_EV_TX_EOI        (1ULL << 13)
	{ .mask = H1_EV_TX_EOI,       .name = "tx_eoi",       .desc = "transmission of end of H1 input" },
#define           H1_EV_TX_HDRS       (1ULL << 14)
	{ .mask = H1_EV_TX_HDRS,      .name = "tx_headers",   .desc = "transmission of all headers" },
#define           H1_EV_TX_BODY       (1ULL << 15)
	{ .mask = H1_EV_TX_BODY,      .name = "tx_body",      .desc = "transmission of H1 body" },
#define           H1_EV_TX_TLRS       (1ULL << 16)
	{ .mask = H1_EV_TX_TLRS,      .name = "tx_trailerus", .desc = "transmission of H1 trailers" },

#define           H1_EV_H1S_NEW       (1ULL << 17)
	{ .mask = H1_EV_H1S_NEW,      .name = "h1s_new",     .desc = "new H1 stream" },
#define           H1_EV_H1S_BLK       (1ULL << 18)
	{ .mask = H1_EV_H1S_BLK,      .name = "h1s_blk",     .desc = "H1 stream blocked" },
#define           H1_EV_H1S_END       (1ULL << 19)
	{ .mask = H1_EV_H1S_END,      .name = "h1s_end",     .desc = "H1 stream terminated" },
#define           H1_EV_H1S_ERR       (1ULL << 20)
	{ .mask = H1_EV_H1S_ERR,      .name = "h1s_err",     .desc = "error on H1 stream" },

#define           H1_EV_STRM_NEW      (1ULL << 21)
	{ .mask = H1_EV_STRM_NEW,     .name = "strm_new",    .desc = "app-layer stream creation" },
#define           H1_EV_STRM_RECV     (1ULL << 22)
	{ .mask = H1_EV_STRM_RECV,    .name = "strm_recv",   .desc = "receiving data for stream" },
#define           H1_EV_STRM_SEND     (1ULL << 23)
	{ .mask = H1_EV_STRM_SEND,    .name = "strm_send",   .desc = "sending data for stream" },
#define           H1_EV_STRM_WAKE     (1ULL << 24)
	{ .mask = H1_EV_STRM_WAKE,    .name = "strm_wake",   .desc = "stream woken up" },
#define           H1_EV_STRM_SHUT     (1ULL << 25)
	{ .mask = H1_EV_STRM_SHUT,    .name = "strm_shut",   .desc = "stream shutdown" },
#define           H1_EV_STRM_END      (1ULL << 26)
	{ .mask = H1_EV_STRM_END,     .name = "strm_end",    .desc = "detaching app-layer stream" },
#define           H1_EV_STRM_ERR      (1ULL << 27)
	{ .mask = H1_EV_STRM_ERR,     .name = "strm_err",    .desc = "stream error" },

	{ }
};

static const struct name_desc h1_trace_lockon_args[4] = {
	/* arg1 */ { /* already used by the connection */ },
	/* arg2 */ { .name="h1s", .desc="H1 stream" },
	/* arg3 */ { },
	/* arg4 */ { }
};

static const struct name_desc h1_trace_decoding[] = {
#define H1_VERB_CLEAN    1
	{ .name="clean",    .desc="only user-friendly stuff, generally suitable for level \"user\"" },
#define H1_VERB_MINIMAL  2
	{ .name="minimal",  .desc="report only h1c/h1s state and flags, no real decoding" },
#define H1_VERB_SIMPLE   3
	{ .name="simple",   .desc="add request/response status line or htx info when available" },
#define H1_VERB_ADVANCED 4
	{ .name="advanced", .desc="add header fields or frame decoding when available" },
#define H1_VERB_COMPLETE 5
	{ .name="complete", .desc="add full data dump when available" },
	{ /* end */ }
};

static struct trace_source trace_h1 = {
	.name = IST("h1"),
	.desc = "HTTP/1 multiplexer",
	.arg_def = TRC_ARG1_CONN,  // TRACE()'s first argument is always a connection
	.default_cb = h1_trace,
	.known_events = h1_trace_events,
	.lockon_args = h1_trace_lockon_args,
	.decoding = h1_trace_decoding,
	.report_events = ~0,  // report everything by default
};

#define TRACE_SOURCE &trace_h1
INITCALL1(STG_REGISTER, trace_register_source, TRACE_SOURCE);

/* the h1c and h1s pools */
DECLARE_STATIC_POOL(pool_head_h1c, "h1c", sizeof(struct h1c));
DECLARE_STATIC_POOL(pool_head_h1s, "h1s", sizeof(struct h1s));

static int h1_recv(struct h1c *h1c);
static int h1_send(struct h1c *h1c);
static int h1_process(struct h1c *h1c);
static struct task *h1_io_cb(struct task *t, void *ctx, unsigned short state);
static void h1_shutw_conn(struct connection *conn, enum cs_shw_mode mode);
static struct task *h1_timeout_task(struct task *t, void *context, unsigned short state);
static void h1_wake_stream_for_recv(struct h1s *h1s);
static void h1_wake_stream_for_send(struct h1s *h1s);

/* the H1 traces always expect that arg1, if non-null, is of type connection
 * (from which we can derive h1c), that arg2, if non-null, is of type h1s, and
 * that arg3, if non-null, is a htx for rx/tx headers.
 */
static void h1_trace(enum trace_level level, uint64_t mask, const struct trace_source *src,
                     const struct ist where, const struct ist func,
                     const void *a1, const void *a2, const void *a3, const void *a4)
{
	const struct connection *conn = a1;
	const struct h1c *h1c = conn ? conn->ctx : NULL;
	const struct h1s *h1s = a2;
	const struct htx *htx = a3;
	const size_t     *val = a4;

	if (!h1c)
		h1c = (h1s ? h1s->h1c : NULL);

	if (!h1c || src->verbosity < H1_VERB_CLEAN)
		return;

	/* Display frontend/backend info by default */
	chunk_appendf(&trace_buf, " : [%c]", (conn_is_back(h1c->conn) ? 'B' : 'F'));

	/* Display request and response states if h1s is defined */
	if (h1s)
		chunk_appendf(&trace_buf, " [%s, %s]",
			      h1m_state_str(h1s->req.state), h1m_state_str(h1s->res.state));

	if (src->verbosity == H1_VERB_CLEAN)
		return;

	/* Display the value to the 4th argument (level > STATE) */
	if (src->level > TRACE_LEVEL_STATE && val)
		chunk_appendf(&trace_buf, " - VAL=%lu", (long)*val);

	/* Display status-line if possible (verbosity > MINIMAL) */
	if (src->verbosity > H1_VERB_MINIMAL && htx && htx_nbblks(htx)) {
		const struct htx_blk *blk = htx_get_head_blk(htx);
		const struct htx_sl  *sl  = htx_get_blk_ptr(htx, blk);
		enum htx_blk_type    type = htx_get_blk_type(blk);

		if (type == HTX_BLK_REQ_SL || type == HTX_BLK_RES_SL)
			chunk_appendf(&trace_buf, " - \"%.*s %.*s %.*s\"",
				      HTX_SL_P1_LEN(sl), HTX_SL_P1_PTR(sl),
				      HTX_SL_P2_LEN(sl), HTX_SL_P2_PTR(sl),
				      HTX_SL_P3_LEN(sl), HTX_SL_P3_PTR(sl));
	}

	/* Display h1c info and, if defined, h1s info (pointer + flags) */
	chunk_appendf(&trace_buf, " - h1c=%p(0x%08x)", h1c, h1c->flags);
	if (h1s)
		chunk_appendf(&trace_buf, " h1s=%p(0x%08x)", h1s, h1s->flags);

	if (src->verbosity == H1_VERB_MINIMAL)
		return;

	/* Display input and output buffer info (level > USER & verbosity > SIMPLE) */
	if (src->level > TRACE_LEVEL_USER) {
		if (src->verbosity == H1_VERB_COMPLETE ||
		    (src->verbosity == H1_VERB_ADVANCED && (mask & (H1_EV_H1C_RECV|H1_EV_STRM_RECV))))
			chunk_appendf(&trace_buf, " ibuf=%u@%p+%u/%u",
				      (unsigned int)b_data(&h1c->ibuf), b_orig(&h1c->ibuf),
				      (unsigned int)b_head_ofs(&h1c->ibuf), (unsigned int)b_size(&h1c->ibuf));
		if (src->verbosity == H1_VERB_COMPLETE ||
		    (src->verbosity == H1_VERB_ADVANCED && (mask & (H1_EV_H1C_SEND|H1_EV_STRM_SEND))))
			chunk_appendf(&trace_buf, " obuf=%u@%p+%u/%u",
				      (unsigned int)b_data(&h1c->obuf), b_orig(&h1c->obuf),
				      (unsigned int)b_head_ofs(&h1c->obuf), (unsigned int)b_size(&h1c->obuf));
	}

	/* Display htx info if defined (level > USER) */
	if (src->level > TRACE_LEVEL_USER && htx) {
		int full = 0;

		/* Full htx info (level > STATE && verbosity > SIMPLE) */
		if (src->level > TRACE_LEVEL_STATE) {
			if (src->verbosity == H1_VERB_COMPLETE)
				full = 1;
			else if (src->verbosity == H1_VERB_ADVANCED && (mask & (H1_EV_RX_HDRS|H1_EV_TX_HDRS)))
				full = 1;
		}

		chunk_memcat(&trace_buf, "\n\t", 2);
		htx_dump(&trace_buf, htx, full);
	}
}


/*****************************************************/
/* functions below are for dynamic buffer management */
/*****************************************************/
/*
 * Indicates whether or not we may receive data. The rules are the following :
 *   - if an error or a shutdown for reads was detected on the connection we
       must not attempt to receive
 *   - if the input buffer failed to be allocated or is full , we must not try
 *     to receive
 *   - if he input processing is busy waiting for the output side, we may
 *     attempt to receive
 *   - otherwise must may not attempt to receive
 */
static inline int h1_recv_allowed(const struct h1c *h1c)
{
	if (h1c->flags & H1C_F_CS_ERROR) {
		TRACE_DEVEL("recv not allowed because of error on h1c", H1_EV_H1C_RECV|H1_EV_H1C_BLK, h1c->conn);
		return 0;
	}

	if (h1c->conn->flags & (CO_FL_ERROR|CO_FL_SOCK_RD_SH)) {
		TRACE_DEVEL("recv not allowed because of (error|read0) on connection", H1_EV_H1C_RECV|H1_EV_H1C_BLK, h1c->conn);
		return 0;
	}

	if (!(h1c->flags & (H1C_F_IN_ALLOC|H1C_F_IN_FULL)))
		return 1;

	TRACE_DEVEL("recv not allowed because input is blocked", H1_EV_H1C_RECV|H1_EV_H1C_BLK, h1c->conn);
	return 0;
}

/*
 * Tries to grab a buffer and to re-enables processing on mux <target>. The h1
 * flags are used to figure what buffer was requested. It returns 1 if the
 * allocation succeeds, in which case the connection is woken up, or 0 if it's
 * impossible to wake up and we prefer to be woken up later.
 */
static int h1_buf_available(void *target)
{
	struct h1c *h1c = target;

	if ((h1c->flags & H1C_F_IN_ALLOC) && b_alloc_margin(&h1c->ibuf, 0)) {
		TRACE_STATE("unblocking h1c, ibuf allocated", H1_EV_H1C_RECV|H1_EV_H1C_BLK|H1_EV_H1C_WAKE, h1c->conn);
		h1c->flags &= ~H1C_F_IN_ALLOC;
		if (h1_recv_allowed(h1c))
			tasklet_wakeup(h1c->wait_event.tasklet);
		return 1;
	}

	if ((h1c->flags & H1C_F_OUT_ALLOC) && b_alloc_margin(&h1c->obuf, 0)) {
		TRACE_STATE("unblocking h1s, obuf allocated", H1_EV_TX_DATA|H1_EV_H1S_BLK|H1_EV_STRM_WAKE, h1c->conn, h1c->h1s);
		h1c->flags &= ~H1C_F_OUT_ALLOC;
		tasklet_wakeup(h1c->wait_event.tasklet);
		if (h1c->h1s)
			h1_wake_stream_for_send(h1c->h1s);
		return 1;
	}

	return 0;
}

/*
 * Allocate a buffer. If if fails, it adds the mux in buffer wait queue.
 */
static inline struct buffer *h1_get_buf(struct h1c *h1c, struct buffer *bptr)
{
	struct buffer *buf = NULL;

	if (likely(!MT_LIST_ADDED(&h1c->buf_wait.list)) &&
	    unlikely((buf = b_alloc_margin(bptr, 0)) == NULL)) {
		h1c->buf_wait.target = h1c;
		h1c->buf_wait.wakeup_cb = h1_buf_available;
		MT_LIST_ADDQ(&buffer_wq, &h1c->buf_wait.list);
	}
	return buf;
}

/*
 * Release a buffer, if any, and try to wake up entities waiting in the buffer
 * wait queue.
 */
static inline void h1_release_buf(struct h1c *h1c, struct buffer *bptr)
{
	if (bptr->size) {
		b_free(bptr);
		offer_buffers(h1c->buf_wait.target, tasks_run_queue);
	}
}

/* returns the number of streams in use on a connection to figure if it's idle
 * or not. We rely on H1C_F_CS_IDLE to know if the connection is in-use or
 * not. This flag is only set when no H1S is attached and when the previous
 * stream, if any, was fully terminated without any error and in K/A mode.
 */
static int h1_used_streams(struct connection *conn)
{
	struct h1c *h1c = conn->ctx;

	return ((h1c->flags & H1C_F_CS_IDLE) ? 0 : 1);
}

/* returns the number of streams still available on a connection */
static int h1_avail_streams(struct connection *conn)
{
	return 1 - h1_used_streams(conn);
}

/* Refresh the h1c task timeout if necessary */
static void h1_refresh_timeout(struct h1c *h1c)
{
	if (h1c->task) {
		h1c->task->expire = TICK_ETERNITY;
		if (h1c->flags & H1C_F_CS_SHUTDOWN) {
			/* half-closed connections switch to clientfin/serverfin
			 * timeouts so that we don't hang too long on clients
			 * that have gone away (especially in tunnel mode).
			 */
			h1c->task->expire = tick_add(now_ms, h1c->shut_timeout);
			task_queue(h1c->task);
			TRACE_DEVEL("refreshing connection's timeout (half-closed)", H1_EV_H1C_SEND, h1c->conn);
		} else if (b_data(&h1c->obuf)) {
			/* any connection with pending data, need a timeout (server or client).
			 */
			h1c->task->expire = tick_add(now_ms, ((h1c->flags & H1C_F_CS_SHUTW_NOW)
							      ? h1c->shut_timeout
							      : h1c->timeout));
			task_queue(h1c->task);
			TRACE_DEVEL("refreshing connection's timeout", H1_EV_H1C_SEND, h1c->conn);
		} else if ((h1c->flags & (H1C_F_CS_IDLE|H1C_F_WAIT_NEXT_REQ)) && !conn_is_back(h1c->conn)) {
			/* front connections waiting for a stream need a timeout. client timeout by
			 * default but http-keep-alive if defined
			 */
			int timeout = h1c->timeout;

			if (h1c->flags & H1C_F_WAIT_NEXT_REQ)
				timeout = tick_first(timeout, h1c->px->timeout.httpka);

			h1c->task->expire = tick_add(now_ms, ((h1c->flags & H1C_F_CS_SHUTW_NOW)
							      ? h1c->shut_timeout
							      : timeout));
			task_queue(h1c->task);
			TRACE_DEVEL("refreshing connection's timeout", H1_EV_H1C_SEND, h1c->conn);
		}
	}
}
/*****************************************************************/
/* functions below are dedicated to the mux setup and management */
/*****************************************************************/

/* returns non-zero if there are input data pending for stream h1s. */
static inline size_t h1s_data_pending(const struct h1s *h1s)
{
	const struct h1m *h1m;

	h1m = conn_is_back(h1s->h1c->conn) ? &h1s->res : &h1s->req;
	if (h1m->state == H1_MSG_DONE)
		return !(h1s->flags & H1S_F_PARSING_DONE);

	return b_data(&h1s->h1c->ibuf);
}

static struct conn_stream *h1s_new_cs(struct h1s *h1s)
{
	struct conn_stream *cs;

	TRACE_ENTER(H1_EV_STRM_NEW, h1s->h1c->conn, h1s);
	cs = cs_new(h1s->h1c->conn);
	if (!cs) {
		TRACE_DEVEL("leaving on CS allocation failure", H1_EV_STRM_NEW|H1_EV_STRM_END|H1_EV_STRM_ERR, h1s->h1c->conn, h1s);
		goto err;
	}
	h1s->cs = cs;
	cs->ctx = h1s;

	if (h1s->flags & H1S_F_NOT_FIRST)
		cs->flags |= CS_FL_NOT_FIRST;

	if (global.tune.options & GTUNE_USE_SPLICE) {
		TRACE_STATE("notify the mux can use splicing", H1_EV_STRM_NEW, h1s->h1c->conn, h1s);
		cs->flags |= CS_FL_MAY_SPLICE;
	}

	if (stream_create_from_cs(cs) < 0) {
		TRACE_DEVEL("leaving on stream creation failure", H1_EV_STRM_NEW|H1_EV_STRM_END|H1_EV_STRM_ERR, h1s->h1c->conn, h1s);
		goto err;
	}

	TRACE_LEAVE(H1_EV_STRM_NEW, h1s->h1c->conn, h1s);
	return cs;

  err:
	cs_free(cs);
	h1s->cs = NULL;
	return NULL;
}

static struct h1s *h1s_create(struct h1c *h1c, struct conn_stream *cs, struct session *sess)
{
	struct h1s *h1s;

	TRACE_ENTER(H1_EV_H1S_NEW, h1c->conn);

	h1s = pool_alloc(pool_head_h1s);
	if (!h1s)
		goto fail;

	h1s->h1c = h1c;
	h1c->h1s = h1s;

	h1s->sess = sess;

	h1s->cs    = NULL;
	h1s->flags = H1S_F_WANT_KAL;

	h1s->subs = NULL;

	h1m_init_req(&h1s->req);
	h1s->req.flags |= (H1_MF_NO_PHDR|H1_MF_CLEAN_CONN_HDR);

	h1m_init_res(&h1s->res);
	h1s->res.flags |= (H1_MF_NO_PHDR|H1_MF_CLEAN_CONN_HDR);

	h1s->status = 0;
	h1s->meth   = HTTP_METH_OTHER;

	if (h1c->flags & H1C_F_WAIT_NEXT_REQ)
		h1s->flags |= H1S_F_NOT_FIRST;
	h1c->flags &= ~(H1C_F_CS_IDLE|H1C_F_WAIT_NEXT_REQ);

	if (!conn_is_back(h1c->conn)) {
		if (h1c->px->options2 & PR_O2_REQBUG_OK)
			h1s->req.err_pos = -1;

		/* For frontend connections we should always have a session */
		if (!sess)
			h1s->sess = sess = h1c->conn->owner;

		/* Timers for subsequent sessions on the same HTTP 1.x connection
		 * measure from `now`, not from the connection accept time */
		if (h1s->flags & H1S_F_NOT_FIRST) {
			h1s->csinfo.create_date = date;
			h1s->csinfo.tv_create   = now;
			h1s->csinfo.t_handshake = 0;
			h1s->csinfo.t_idle      = -1;
		}
		else {
			h1s->csinfo.create_date = sess->accept_date;
			h1s->csinfo.tv_create   = sess->tv_accept;
			h1s->csinfo.t_handshake = sess->t_handshake;
			h1s->csinfo.t_idle      = -1;
		}
	}
	else {
		if (h1c->px->options2 & PR_O2_RSPBUG_OK)
			h1s->res.err_pos = -1;

		h1s->csinfo.create_date = date;
		h1s->csinfo.tv_create   = now;
		h1s->csinfo.t_handshake = 0;
		h1s->csinfo.t_idle      = -1;
	}

	/* If a conn_stream already exists, attach it to this H1S. Otherwise we
	 * create a new one.
	 */
	if (cs) {
		cs->ctx = h1s;
		h1s->cs = cs;
	}
	else {
		cs = h1s_new_cs(h1s);
		if (!cs)
			goto fail;
	}
	TRACE_LEAVE(H1_EV_H1S_NEW, h1c->conn, h1s);
	return h1s;

  fail:
	pool_free(pool_head_h1s, h1s);
	TRACE_DEVEL("leaving in error", H1_EV_H1S_NEW|H1_EV_H1S_END|H1_EV_H1S_ERR, h1c->conn);
	return NULL;
}

static void h1s_destroy(struct h1s *h1s)
{
	if (h1s) {
		struct h1c *h1c = h1s->h1c;

		TRACE_POINT(H1_EV_H1S_END, h1c->conn, h1s);
		h1c->h1s = NULL;

		if (h1s->subs)
			h1s->subs->events = 0;

		h1c->flags &= ~H1C_F_IN_BUSY;
		if (h1s->flags & (H1S_F_REQ_ERROR|H1S_F_RES_ERROR)) {
			h1c->flags |= H1C_F_CS_ERROR;
			TRACE_STATE("h1s on error, set error on h1c", H1_EV_H1C_ERR, h1c->conn, h1s);
		}

		if (!(h1c->flags & (H1C_F_CS_ERROR|H1C_F_CS_SHUTW_NOW|H1C_F_CS_SHUTDOWN)) && /* No error/shutdown on h1c */
		    !(h1c->conn->flags & (CO_FL_ERROR|CO_FL_SOCK_RD_SH|CO_FL_SOCK_WR_SH)) && /* No error/shutdown on conn */
		    (h1s->flags & (H1S_F_WANT_KAL|H1S_F_PARSING_DONE)) == (H1S_F_WANT_KAL|H1S_F_PARSING_DONE) && /* K/A possible */
		    h1s->req.state == H1_MSG_DONE && h1s->res.state == H1_MSG_DONE) {        /* req/res in DONE state */
			h1c->flags |= (H1C_F_CS_IDLE|H1C_F_WAIT_NEXT_REQ);
			TRACE_STATE("set idle mode on h1c, waiting for the next request", H1_EV_H1C_ERR, h1c->conn, h1s);
		}
		pool_free(pool_head_h1s, h1s);
	}
}

static const struct cs_info *h1_get_cs_info(struct conn_stream *cs)
{
	struct h1s *h1s = cs->ctx;

	if (h1s && !conn_is_back(cs->conn))
		return &h1s->csinfo;
	return NULL;
}

/*
 * Initialize the mux once it's attached. It is expected that conn->ctx points
 * to the existing conn_stream (for outgoing connections or for incoming ones
 * during a mux upgrade) or NULL (for incoming ones during the connection
 * establishment). <input> is always used as Input buffer and may contain
 * data. It is the caller responsibility to not reuse it anymore. Returns < 0 on
 * error.
 */
static int h1_init(struct connection *conn, struct proxy *proxy, struct session *sess,
		   struct buffer *input)
{
	struct h1c *h1c;
	struct task *t = NULL;
	void *conn_ctx = conn->ctx;

	TRACE_ENTER(H1_EV_H1C_NEW);

	h1c = pool_alloc(pool_head_h1c);
	if (!h1c)
		goto fail_h1c;
	h1c->conn = conn;
	h1c->px   = proxy;

	h1c->flags = H1C_F_CS_IDLE;
	h1c->ibuf  = *input;
	h1c->obuf  = BUF_NULL;
	h1c->h1s   = NULL;
	h1c->task  = NULL;

	MT_LIST_INIT(&h1c->buf_wait.list);
	h1c->wait_event.tasklet = tasklet_new();
	if (!h1c->wait_event.tasklet)
		goto fail;
	h1c->wait_event.tasklet->process = h1_io_cb;
	h1c->wait_event.tasklet->context = h1c;
	h1c->wait_event.events   = 0;

	if (conn_is_back(conn)) {
		h1c->shut_timeout = h1c->timeout = proxy->timeout.server;
		if (tick_isset(proxy->timeout.serverfin))
			h1c->shut_timeout = proxy->timeout.serverfin;
	} else {
		h1c->shut_timeout = h1c->timeout = proxy->timeout.client;
		if (tick_isset(proxy->timeout.clientfin))
			h1c->shut_timeout = proxy->timeout.clientfin;
	}
	if (tick_isset(h1c->timeout)) {
		t = task_new(tid_bit);
		if (!t)
			goto fail;

		h1c->task = t;
		t->process = h1_timeout_task;
		t->context = h1c;
		t->expire = tick_add(now_ms, h1c->timeout);
	}

	conn->ctx = h1c;

	/* Always Create a new H1S */
	if (!h1s_create(h1c, conn_ctx, sess))
		goto fail;

	if (t)
		task_queue(t);

	/* Try to read, if nothing is available yet we'll just subscribe */
	h1c->conn->xprt->subscribe(h1c->conn, h1c->conn->xprt_ctx, SUB_RETRY_RECV, &h1c->wait_event);

	/* mux->wake will be called soon to complete the operation */
	TRACE_LEAVE(H1_EV_H1C_NEW, conn, h1c->h1s);
	return 0;

  fail:
	task_destroy(t);
	if (h1c->wait_event.tasklet)
		tasklet_free(h1c->wait_event.tasklet);
	pool_free(pool_head_h1c, h1c);
 fail_h1c:
	conn->ctx = conn_ctx; // restore saved context
	TRACE_DEVEL("leaving in error", H1_EV_H1C_NEW|H1_EV_H1C_END|H1_EV_H1C_ERR);
	return -1;
}

/* release function. This one should be called to free all resources allocated
 * to the mux.
 */
static void h1_release(struct h1c *h1c)
{
	struct connection *conn = NULL;

	TRACE_POINT(H1_EV_H1C_END);

	if (h1c) {
		/* The connection must be aattached to this mux to be released */
		if (h1c->conn && h1c->conn->ctx == h1c)
			conn = h1c->conn;

		TRACE_DEVEL("freeing h1c", H1_EV_H1C_END, conn);

		if (conn && h1c->flags & H1C_F_UPG_H2C) {
			TRACE_DEVEL("upgrading H1 to H2", H1_EV_H1C_END, conn);
			h1c->flags &= ~H1C_F_UPG_H2C;
			/* Make sure we're no longer subscribed to anything */
			if (h1c->wait_event.events)
				conn->xprt->unsubscribe(conn, conn->xprt_ctx,
				    h1c->wait_event.events, &h1c->wait_event);
			if (conn_upgrade_mux_fe(conn, NULL, &h1c->ibuf, ist("h2"), PROTO_MODE_HTTP) != -1) {
				/* connection successfully upgraded to H2, this
				 * mux was already released */
				return;
			}
			TRACE_DEVEL("h2 upgrade failed", H1_EV_H1C_END|H1_EV_H1C_ERR, conn);
			sess_log(conn->owner); /* Log if the upgrade failed */
		}


		if (MT_LIST_ADDED(&h1c->buf_wait.list))
			MT_LIST_DEL(&h1c->buf_wait.list);

		h1_release_buf(h1c, &h1c->ibuf);
		h1_release_buf(h1c, &h1c->obuf);

		if (h1c->task) {
			h1c->task->context = NULL;
			task_wakeup(h1c->task, TASK_WOKEN_OTHER);
			h1c->task = NULL;
		}

		if (h1c->wait_event.tasklet)
			tasklet_free(h1c->wait_event.tasklet);

		h1s_destroy(h1c->h1s);
		if (conn && h1c->wait_event.events != 0)
			conn->xprt->unsubscribe(conn, conn->xprt_ctx, h1c->wait_event.events,
			    &h1c->wait_event);
		pool_free(pool_head_h1c, h1c);
	}

	if (conn) {
		conn->mux = NULL;
		conn->ctx = NULL;
		TRACE_DEVEL("freeing conn", H1_EV_H1C_END, conn);

		conn_stop_tracking(conn);
		conn_full_close(conn);
		if (conn->destroy_cb)
			conn->destroy_cb(conn);
		conn_free(conn);
	}
}

/******************************************************/
/* functions below are for the H1 protocol processing */
/******************************************************/
/* Parse the request version and set H1_MF_VER_11 on <h1m> if the version is
 * greater or equal to 1.1
 */
static void h1_parse_req_vsn(struct h1m *h1m, const struct htx_sl *sl)
{
	const char *p = HTX_SL_REQ_VPTR(sl);

	if ((HTX_SL_REQ_VLEN(sl) == 8) &&
	    (*(p + 5) > '1' ||
	     (*(p + 5) == '1' && *(p + 7) >= '1')))
		h1m->flags |= H1_MF_VER_11;
}

/* Parse the response version and set H1_MF_VER_11 on <h1m> if the version is
 * greater or equal to 1.1
 */
static void h1_parse_res_vsn(struct h1m *h1m, const struct htx_sl *sl)
{
	const char *p = HTX_SL_RES_VPTR(sl);

	if ((HTX_SL_RES_VLEN(sl) == 8) &&
	    (*(p + 5) > '1' ||
	     (*(p + 5) == '1' && *(p + 7) >= '1')))
		h1m->flags |= H1_MF_VER_11;
}

/* Deduce the connection mode of the client connection, depending on the
 * configuration and the H1 message flags. This function is called twice, the
 * first time when the request is parsed and the second time when the response
 * is parsed.
 */
static void h1_set_cli_conn_mode(struct h1s *h1s, struct h1m *h1m)
{
	struct proxy *fe = h1s->h1c->px;

	if (h1m->flags & H1_MF_RESP) {
		/* Output direction: second pass */
		if ((h1s->meth == HTTP_METH_CONNECT && h1s->status == 200) ||
		    h1s->status == 101) {
			/* Either we've established an explicit tunnel, or we're
			 * switching the protocol. In both cases, we're very unlikely to
			 * understand the next protocols. We have to switch to tunnel
			 * mode, so that we transfer the request and responses then let
			 * this protocol pass unmodified. When we later implement
			 * specific parsers for such protocols, we'll want to check the
			 * Upgrade header which contains information about that protocol
			 * for responses with status 101 (eg: see RFC2817 about TLS).
			 */
			h1s->flags = (h1s->flags & ~H1S_F_WANT_MSK) | H1S_F_WANT_TUN;
			TRACE_STATE("set tunnel mode (resp)", H1_EV_TX_DATA|H1_EV_TX_HDRS, h1s->h1c->conn, h1s);
		}
		else if (h1s->flags & H1S_F_WANT_KAL) {
			/* By default the client is in KAL mode. CLOSE mode mean
			 * it is imposed by the client itself. So only change
			 * KAL mode here. */
			if (!(h1m->flags & H1_MF_XFER_LEN) || (h1m->flags & H1_MF_CONN_CLO)) {
				/* no length known or explicit close => close */
				h1s->flags = (h1s->flags & ~H1S_F_WANT_MSK) | H1S_F_WANT_CLO;
				TRACE_STATE("detect close mode (resp)", H1_EV_TX_DATA|H1_EV_TX_HDRS, h1s->h1c->conn, h1s);
			}
			else if (!(h1m->flags & H1_MF_CONN_KAL) &&
				 (fe->options & PR_O_HTTP_MODE) == PR_O_HTTP_CLO) {
				/* no explicit keep-alive and option httpclose => close */
				h1s->flags = (h1s->flags & ~H1S_F_WANT_MSK) | H1S_F_WANT_CLO;
				TRACE_STATE("force close mode (resp)", H1_EV_TX_DATA|H1_EV_TX_HDRS, h1s->h1c->conn, h1s);
			}
		}
	}
	else {
		/* Input direction: first pass */
		if (!(h1m->flags & (H1_MF_VER_11|H1_MF_CONN_KAL)) || h1m->flags & H1_MF_CONN_CLO)  {
			/* no explicit keep-alive in HTTP/1.0 or explicit close => close*/
			h1s->flags = (h1s->flags & ~H1S_F_WANT_MSK) | H1S_F_WANT_CLO;
			TRACE_STATE("detect close mode (req)", H1_EV_RX_DATA|H1_EV_RX_HDRS, h1s->h1c->conn, h1s);
		}
	}

	/* If KAL, check if the frontend is stopping. If yes, switch in CLO mode */
	if (h1s->flags & H1S_F_WANT_KAL && fe->state == PR_STSTOPPED) {
		h1s->flags = (h1s->flags & ~H1S_F_WANT_MSK) | H1S_F_WANT_CLO;
		TRACE_STATE("stopping, set close mode", H1_EV_RX_DATA|H1_EV_RX_HDRS|H1_EV_TX_DATA|H1_EV_TX_HDRS, h1s->h1c->conn, h1s);
	}
}

/* Deduce the connection mode of the client connection, depending on the
 * configuration and the H1 message flags. This function is called twice, the
 * first time when the request is parsed and the second time when the response
 * is parsed.
 */
static void h1_set_srv_conn_mode(struct h1s *h1s, struct h1m *h1m)
{
	struct session *sess = h1s->sess;
	struct proxy *be = h1s->h1c->px;
	int fe_flags = sess ? sess->fe->options : 0;

	if (h1m->flags & H1_MF_RESP) {
		/* Input direction: second pass */
		if ((h1s->meth == HTTP_METH_CONNECT && h1s->status == 200) ||
		    h1s->status == 101) {
			/* Either we've established an explicit tunnel, or we're
			 * switching the protocol. In both cases, we're very unlikely to
			 * understand the next protocols. We have to switch to tunnel
			 * mode, so that we transfer the request and responses then let
			 * this protocol pass unmodified. When we later implement
			 * specific parsers for such protocols, we'll want to check the
			 * Upgrade header which contains information about that protocol
			 * for responses with status 101 (eg: see RFC2817 about TLS).
			 */
			h1s->flags = (h1s->flags & ~H1S_F_WANT_MSK) | H1S_F_WANT_TUN;
			TRACE_STATE("set tunnel mode (resp)", H1_EV_RX_DATA|H1_EV_RX_HDRS, h1s->h1c->conn, h1s);
		}
		else if (h1s->flags & H1S_F_WANT_KAL) {
			/* By default the server is in KAL mode. CLOSE mode mean
			 * it is imposed by haproxy itself. So only change KAL
			 * mode here. */
			if (!(h1m->flags & H1_MF_XFER_LEN) || h1m->flags & H1_MF_CONN_CLO ||
			    !(h1m->flags & (H1_MF_VER_11|H1_MF_CONN_KAL))){
				/* no length known or explicit close or no explicit keep-alive in HTTP/1.0 => close */
				h1s->flags = (h1s->flags & ~H1S_F_WANT_MSK) | H1S_F_WANT_CLO;
				TRACE_STATE("detect close mode (resp)", H1_EV_RX_DATA|H1_EV_RX_HDRS, h1s->h1c->conn, h1s);
			}
		}
	}
	else {
		/* Output direction: first pass */
		if (h1m->flags & H1_MF_CONN_CLO) {
			/* explicit close => close */
			h1s->flags = (h1s->flags & ~H1S_F_WANT_MSK) | H1S_F_WANT_CLO;
			TRACE_STATE("detect close mode (req)", H1_EV_TX_DATA|H1_EV_TX_HDRS, h1s->h1c->conn, h1s);
		}
		else if (!(h1m->flags & H1_MF_CONN_KAL) &&
			 ((fe_flags & PR_O_HTTP_MODE) == PR_O_HTTP_SCL ||
			  (be->options & PR_O_HTTP_MODE) == PR_O_HTTP_SCL ||
			  (fe_flags & PR_O_HTTP_MODE) == PR_O_HTTP_CLO ||
			  (be->options & PR_O_HTTP_MODE) == PR_O_HTTP_CLO)) {
			/* no explicit keep-alive option httpclose/server-close => close */
			h1s->flags = (h1s->flags & ~H1S_F_WANT_MSK) | H1S_F_WANT_CLO;
			TRACE_STATE("force close mode (req)", H1_EV_TX_DATA|H1_EV_TX_HDRS, h1s->h1c->conn, h1s);
		}
	}

	/* If KAL, check if the backend is stopping. If yes, switch in CLO mode */
	if (h1s->flags & H1S_F_WANT_KAL && be->state == PR_STSTOPPED) {
		h1s->flags = (h1s->flags & ~H1S_F_WANT_MSK) | H1S_F_WANT_CLO;
		TRACE_STATE("stopping, set close mode", H1_EV_RX_DATA|H1_EV_RX_HDRS|H1_EV_TX_DATA|H1_EV_TX_HDRS, h1s->h1c->conn, h1s);
	}
}

static void h1_update_req_conn_value(struct h1s *h1s, struct h1m *h1m, struct ist *conn_val)
{
	struct proxy *px = h1s->h1c->px;

	/* Don't update "Connection:" header in TUNNEL mode or if "Upgrage"
	 * token is found
	 */
	if (h1s->flags & H1S_F_WANT_TUN || h1m->flags & H1_MF_CONN_UPG)
		return;

	if (h1s->flags & H1S_F_WANT_KAL || px->options2 & PR_O2_FAKE_KA) {
		if (!(h1m->flags & H1_MF_VER_11)) {
			TRACE_STATE("add \"Connection: keep-alive\"", H1_EV_TX_DATA|H1_EV_TX_HDRS, h1s->h1c->conn, h1s);
			*conn_val = ist("keep-alive");
		}
	}
	else { /* H1S_F_WANT_CLO && !PR_O2_FAKE_KA */
		if (h1m->flags & H1_MF_VER_11) {
			TRACE_STATE("add \"Connection: close\"", H1_EV_TX_DATA|H1_EV_TX_HDRS, h1s->h1c->conn, h1s);
			*conn_val = ist("close");
		}
	}
}

static void h1_update_res_conn_value(struct h1s *h1s, struct h1m *h1m, struct ist *conn_val)
{
	/* Don't update "Connection:" header in TUNNEL mode or if "Upgrage"
	 * token is found
	 */
	if (h1s->flags & H1S_F_WANT_TUN || h1m->flags & H1_MF_CONN_UPG)
		return;

	if (h1s->flags & H1S_F_WANT_KAL) {
		if (!(h1m->flags & H1_MF_VER_11) ||
		    !((h1m->flags & h1s->req.flags) & H1_MF_VER_11)) {
			TRACE_STATE("add \"Connection: keep-alive\"", H1_EV_TX_DATA|H1_EV_TX_HDRS, h1s->h1c->conn, h1s);
			*conn_val = ist("keep-alive");
		}
	}
	else { /* H1S_F_WANT_CLO */
		if (h1m->flags & H1_MF_VER_11) {
			TRACE_STATE("add \"Connection: close\"", H1_EV_TX_DATA|H1_EV_TX_HDRS, h1s->h1c->conn, h1s);
			*conn_val = ist("close");
		}
	}
}

static void h1_process_input_conn_mode(struct h1s *h1s, struct h1m *h1m, struct htx *htx)
{
	if (!conn_is_back(h1s->h1c->conn))
		h1_set_cli_conn_mode(h1s, h1m);
	else
		h1_set_srv_conn_mode(h1s, h1m);
}

static void h1_process_output_conn_mode(struct h1s *h1s, struct h1m *h1m, struct ist *conn_val)
{
	if (!conn_is_back(h1s->h1c->conn))
		h1_set_cli_conn_mode(h1s, h1m);
	else
		h1_set_srv_conn_mode(h1s, h1m);

	if (!(h1m->flags & H1_MF_RESP))
		h1_update_req_conn_value(h1s, h1m, conn_val);
	else
		h1_update_res_conn_value(h1s, h1m, conn_val);
}

/* Try to adjust the case of the message header name using the global map
 * <hdrs_map>.
 */
static void h1_adjust_case_outgoing_hdr(struct h1s *h1s, struct h1m *h1m, struct ist *name)
{
	struct ebpt_node *node;
	struct h1_hdr_entry *entry;

	/* No entry in the map, do nothing */
	if (eb_is_empty(&hdrs_map.map))
		return;

	/* No conversion fo the request headers */
	if (!(h1m->flags & H1_MF_RESP) && !(h1s->h1c->px->options2 & PR_O2_H1_ADJ_BUGSRV))
		return;

	/* No conversion fo the response headers */
	if ((h1m->flags & H1_MF_RESP) && !(h1s->h1c->px->options2 & PR_O2_H1_ADJ_BUGCLI))
		return;

	node = ebis_lookup_len(&hdrs_map.map, name->ptr, name->len);
	if (!node)
		return;
	entry = container_of(node, struct h1_hdr_entry, node);
	name->ptr = entry->name.ptr;
	name->len = entry->name.len;
}

/* Append the description of what is present in error snapshot <es> into <out>.
 * The description must be small enough to always fit in a buffer. The output
 * buffer may be the trash so the trash must not be used inside this function.
 */
static void h1_show_error_snapshot(struct buffer *out, const struct error_snapshot *es)
{
	chunk_appendf(out,
		      "  H1 connection flags 0x%08x, H1 stream flags 0x%08x\n"
		      "  H1 msg state %s(%d), H1 msg flags 0x%08x\n"
		      "  H1 chunk len %lld bytes, H1 body len %lld bytes :\n",
		      es->ctx.h1.c_flags, es->ctx.h1.s_flags,
		      h1m_state_str(es->ctx.h1.state), es->ctx.h1.state,
		      es->ctx.h1.m_flags, es->ctx.h1.m_clen, es->ctx.h1.m_blen);
}
/*
 * Capture a bad request or response and archive it in the proxy's structure.
 * By default it tries to report the error position as h1m->err_pos. However if
 * this one is not set, it will then report h1m->next, which is the last known
 * parsing point. The function is able to deal with wrapping buffers. It always
 * displays buffers as a contiguous area starting at buf->p. The direction is
 * determined thanks to the h1m's flags.
 */
static void h1_capture_bad_message(struct h1c *h1c, struct h1s *h1s,
				   struct h1m *h1m, struct buffer *buf)
{
	struct session *sess = h1s->sess;
	struct proxy *proxy = h1c->px;
	struct proxy *other_end;
	union error_snapshot_ctx ctx;

	if (h1s->cs && h1s->cs->data) {
		if (sess == NULL)
			sess = si_strm(h1s->cs->data)->sess;
		if (!(h1m->flags & H1_MF_RESP))
			other_end = si_strm(h1s->cs->data)->be;
		else
			other_end = sess->fe;
	} else
		other_end = NULL;

	/* http-specific part now */
	ctx.h1.state   = h1m->state;
	ctx.h1.c_flags = h1c->flags;
	ctx.h1.s_flags = h1s->flags;
	ctx.h1.m_flags = h1m->flags;
	ctx.h1.m_clen  = h1m->curr_len;
	ctx.h1.m_blen  = h1m->body_len;

	proxy_capture_error(proxy, !!(h1m->flags & H1_MF_RESP), other_end,
			    h1c->conn->target, sess, buf, 0, 0,
			    (h1m->err_pos >= 0) ? h1m->err_pos : h1m->next,
			    &ctx, h1_show_error_snapshot);
}

/* Emit the chunksize followed by a CRLF in front of data of the buffer
 * <buf>. It goes backwards and starts with the byte before the buffer's
 * head. The caller is responsible for ensuring there is enough room left before
 * the buffer's head for the string.
 */
static void h1_emit_chunk_size(struct buffer *buf, size_t chksz)
{
	char *beg, *end;

	beg = end = b_head(buf);
	*--beg = '\n';
	*--beg = '\r';
	do {
		*--beg = hextab[chksz & 0xF];
	} while (chksz >>= 4);
	buf->head -= (end - beg);
	b_add(buf, end - beg);
}

/* Emit a CRLF after the data of the buffer <buf>. The caller is responsible for
 * ensuring there is enough room left in the buffer for the string. */
static void h1_emit_chunk_crlf(struct buffer *buf)
{
	*(b_peek(buf, b_data(buf)))     = '\r';
	*(b_peek(buf, b_data(buf) + 1)) = '\n';
	b_add(buf, 2);
}

/*
 * Switch the request to tunnel mode. This function must only be called for
 * CONNECT requests. On the client side, if the response is not finished, the
 * mux is mark as busy on input.
 */
static void h1_set_req_tunnel_mode(struct h1s *h1s)
{
	h1s->req.flags &= ~(H1_MF_XFER_LEN|H1_MF_CLEN|H1_MF_CHNK);
	h1s->req.state = H1_MSG_TUNNEL;
	TRACE_STATE("switch H1 request in tunnel mode", H1_EV_TX_DATA|H1_EV_TX_HDRS, h1s->h1c->conn, h1s);

	if (!conn_is_back(h1s->h1c->conn)) {
		h1s->flags &= ~H1S_F_PARSING_DONE;
		if (h1s->res.state < H1_MSG_DONE) {
			h1s->h1c->flags |= H1C_F_IN_BUSY;
			TRACE_STATE("switch h1c in busy mode", H1_EV_RX_DATA|H1_EV_H1C_BLK, h1s->h1c->conn, h1s);
		}
	}
	else if (h1s->h1c->flags & H1C_F_IN_BUSY) {
		h1s->h1c->flags &= ~H1C_F_IN_BUSY;
		tasklet_wakeup(h1s->h1c->wait_event.tasklet);
		TRACE_STATE("h1c no more busy", H1_EV_RX_DATA|H1_EV_H1C_BLK|H1_EV_H1C_WAKE, h1s->h1c->conn, h1s);
	}
}

/*
 * Switch the response to tunnel mode. This function must only be called on
 * successful replies to CONNECT requests or on protocol switching. In this
 * last case, this function takes care to switch the request to tunnel mode if
 * possible. On the server side, if the request is not finished, the mux is mark
 * as busy on input.
 */
static void h1_set_res_tunnel_mode(struct h1s *h1s)
{

	h1s->res.flags &= ~(H1_MF_XFER_LEN|H1_MF_CLEN|H1_MF_CHNK);
	h1s->res.state = H1_MSG_TUNNEL;
	TRACE_STATE("switch H1 response in tunnel mode", H1_EV_TX_DATA|H1_EV_TX_HDRS, h1s->h1c->conn, h1s);

	if (conn_is_back(h1s->h1c->conn)) {
		h1s->flags &= ~H1S_F_PARSING_DONE;
		/* On protocol switching, switch the request to tunnel mode if it is in
		 * DONE state. Otherwise we will wait the end of the request to switch
		 * it in tunnel mode.
		 */
		if (h1s->req.state < H1_MSG_DONE) {
			h1s->h1c->flags |= H1C_F_IN_BUSY;
			TRACE_STATE("switch h1c in busy mode", H1_EV_RX_DATA|H1_EV_H1C_BLK, h1s->h1c->conn, h1s);
		}
		else if (h1s->status == 101 && h1s->req.state == H1_MSG_DONE) {
			h1s->req.flags &= ~(H1_MF_XFER_LEN|H1_MF_CLEN|H1_MF_CHNK);
			h1s->req.state = H1_MSG_TUNNEL;
			TRACE_STATE("switch H1 request in tunnel mode", H1_EV_TX_DATA|H1_EV_TX_HDRS, h1s->h1c->conn, h1s);
		}
	}
	else if (h1s->h1c->flags & H1C_F_IN_BUSY) {
		h1s->h1c->flags &= ~H1C_F_IN_BUSY;
		tasklet_wakeup(h1s->h1c->wait_event.tasklet);
		TRACE_STATE("h1c no more busy", H1_EV_RX_DATA|H1_EV_H1C_BLK|H1_EV_H1C_WAKE, h1s->h1c->conn, h1s);
	}
}

/*
 * Parse HTTP/1 headers. It returns the number of bytes parsed if > 0, or 0 if
 * it couldn't proceed. Parsing errors are reported by setting H1S_F_*_ERROR
 * flag. If relies on the function http_parse_msg_hdrs() to do the parsing.
 */
static size_t h1_process_headers(struct h1s *h1s, struct h1m *h1m, struct htx *htx,
				 struct buffer *buf, size_t *ofs, size_t max)
{
	union h1_sl h1sl;
	int ret = 0;

	TRACE_ENTER(H1_EV_RX_DATA|H1_EV_RX_HDRS, h1s->h1c->conn, h1s,, (size_t[]){max});

	if (!(h1s->h1c->px->options2 & PR_O2_NO_H2_UPGRADE) && /* H2 upgrade supported by the proxy */
	    !(h1s->flags & H1S_F_NOT_FIRST) &&                 /* It is the first transaction */
	    !(h1m->flags & H1_MF_RESP)) {                      /* It is a request */
		/* Try to match H2 preface before parsing the request headers. */
		ret = b_isteq(buf, 0, b_data(buf), ist(H2_CONN_PREFACE));
		if (ret > 0) {
			goto h2c_upgrade;
		}
	}
	else {
		if (h1s->meth == HTTP_METH_CONNECT)
			h1m->flags |= H1_MF_METH_CONNECT;
		if (h1s->meth == HTTP_METH_HEAD)
			h1m->flags |= H1_MF_METH_HEAD;
	}

	ret = h1_parse_msg_hdrs(h1m, &h1sl, htx, buf, *ofs, max);
	if (!ret) {
		TRACE_DEVEL("leaving on missing data or error", H1_EV_RX_DATA|H1_EV_RX_HDRS, h1s->h1c->conn, h1s);
		if (htx->flags & HTX_FL_PARSING_ERROR) {
			if (!(h1m->flags & H1_MF_RESP)) {
				h1s->flags |= H1S_F_REQ_ERROR;
				TRACE_USER("rejected H1 request", H1_EV_RX_DATA|H1_EV_RX_HDRS|H1_EV_H1S_ERR, h1s->h1c->conn, h1s);
			}
			else {
				h1s->flags |= H1S_F_RES_ERROR;
				TRACE_USER("rejected H1 response", H1_EV_RX_DATA|H1_EV_RX_HDRS|H1_EV_H1S_ERR, h1s->h1c->conn, h1s);
			}
			h1s->cs->flags |= CS_FL_EOI;
			TRACE_STATE("parsing error", H1_EV_RX_DATA|H1_EV_RX_HDRS|H1_EV_H1S_ERR, h1s->h1c->conn, h1s);
			h1_capture_bad_message(h1s->h1c, h1s, h1m, buf);
		}
		goto end;
	}

	if (h1m->err_pos >= 0)  {
		/* Maybe we found an error during the parsing while we were
		 * configured not to block on that, so we have to capture it
		 * now.
		 */
		TRACE_STATE("Ignored parsing error", H1_EV_RX_DATA|H1_EV_RX_HDRS, h1s->h1c->conn, h1s);
		h1_capture_bad_message(h1s->h1c, h1s, h1m, buf);
	}

	if (!(h1m->flags & H1_MF_RESP)) {
		h1s->meth = h1sl.rq.meth;
		if (h1m->state == H1_MSG_TUNNEL)
			h1_set_req_tunnel_mode(h1s);
	}
	else {
		h1s->status = h1sl.st.status;
		if (h1m->state == H1_MSG_TUNNEL)
			h1_set_res_tunnel_mode(h1s);
	}
	h1_process_input_conn_mode(h1s, h1m, htx);
	*ofs += ret;

  end:
	TRACE_LEAVE(H1_EV_RX_DATA|H1_EV_RX_HDRS, h1s->h1c->conn, h1s,, (size_t[]){ret});
	return ret;

  h2c_upgrade:
	h1s->h1c->flags |= H1C_F_UPG_H2C;
	h1s->cs->flags |= CS_FL_EOI;
	htx->flags |= HTX_FL_UPGRADE;
	TRACE_DEVEL("leaving on H2 update", H1_EV_RX_DATA|H1_EV_RX_HDRS|H1_EV_RX_EOI, h1s->h1c->conn, h1s);
	return 0;
}

/*
 * Parse HTTP/1 body. It returns the number of bytes parsed if > 0, or 0 if it
 * couldn't proceed. Parsing errors are reported by setting H1S_F_*_ERROR flag.
 * If relies on the function http_parse_msg_data() to do the parsing.
 */
static size_t h1_process_data(struct h1s *h1s, struct h1m *h1m, struct htx **htx,
			      struct buffer *buf, size_t *ofs, size_t max,
			      struct buffer *htxbuf)
{
	int ret;

	TRACE_ENTER(H1_EV_RX_DATA|H1_EV_RX_BODY, h1s->h1c->conn, h1s,, (size_t[]){max});
	ret = h1_parse_msg_data(h1m, htx, buf, *ofs, max, htxbuf);
	if (!ret) {
		TRACE_DEVEL("leaving on missing data or error", H1_EV_RX_DATA|H1_EV_RX_BODY, h1s->h1c->conn, h1s);
		if ((*htx)->flags & HTX_FL_PARSING_ERROR) {
			if (!(h1m->flags & H1_MF_RESP)) {
				h1s->flags |= H1S_F_REQ_ERROR;
				TRACE_USER("rejected H1 request", H1_EV_RX_DATA|H1_EV_RX_BODY|H1_EV_H1S_ERR, h1s->h1c->conn, h1s);
			}
			else {
				h1s->flags |= H1S_F_RES_ERROR;
				TRACE_USER("rejected H1 response", H1_EV_RX_DATA|H1_EV_RX_BODY|H1_EV_H1S_ERR, h1s->h1c->conn, h1s);
			}
			h1s->cs->flags |= CS_FL_EOI;
			TRACE_STATE("parsing error", H1_EV_RX_DATA|H1_EV_RX_BODY|H1_EV_H1S_ERR, h1s->h1c->conn, h1s);
			h1_capture_bad_message(h1s->h1c, h1s, h1m, buf);
		}
		goto end;
	}

	if (h1s->cs && !(h1m->flags & H1_MF_CHNK) &&
	    ((h1m->state == H1_MSG_DATA && h1m->curr_len) || (h1m->state == H1_MSG_TUNNEL))) {
		TRACE_STATE("notify the mux can use splicing", H1_EV_RX_DATA|H1_EV_RX_BODY, h1s->h1c->conn, h1s);
		h1s->cs->flags |= CS_FL_MAY_SPLICE;
	}
	else if (h1s->cs) {
		TRACE_STATE("notify the mux can't use splicing anymore", H1_EV_RX_DATA|H1_EV_RX_BODY, h1s->h1c->conn, h1s);
		h1s->cs->flags &= ~CS_FL_MAY_SPLICE;
	}

	*ofs += ret;

  end:
	TRACE_LEAVE(H1_EV_RX_DATA|H1_EV_RX_BODY, h1s->h1c->conn, h1s,, (size_t[]){ret});
	return ret;
}

/*
 * Parse HTTP/1 trailers. It returns the number of bytes parsed if > 0, or 0 if
 * it couldn't proceed. Parsing errors are reported by setting H1S_F_*_ERROR
 * flag and filling h1s->err_pos and h1s->err_state fields. This functions is
 * responsible to update the parser state <h1m>.
 */
static size_t h1_process_trailers(struct h1s *h1s, struct h1m *h1m, struct htx *htx,
				  struct buffer *buf, size_t *ofs, size_t max)
{
	int ret;

	TRACE_ENTER(H1_EV_RX_DATA|H1_EV_RX_TLRS, h1s->h1c->conn, h1s,, (size_t[]){max});
	ret = h1_parse_msg_tlrs(h1m, htx, buf, *ofs, max);
	if (!ret) {
		TRACE_DEVEL("leaving on missing data or error", H1_EV_RX_DATA|H1_EV_RX_BODY, h1s->h1c->conn, h1s);
		if (htx->flags & HTX_FL_PARSING_ERROR) {
			if (!(h1m->flags & H1_MF_RESP)) {
				h1s->flags |= H1S_F_REQ_ERROR;
				TRACE_USER("rejected H1 request", H1_EV_RX_DATA|H1_EV_RX_TLRS|H1_EV_H1S_ERR, h1s->h1c->conn, h1s);
			}
			else {
				h1s->flags |= H1S_F_RES_ERROR;
				TRACE_USER("rejected H1 response", H1_EV_RX_DATA|H1_EV_RX_TLRS|H1_EV_H1S_ERR, h1s->h1c->conn, h1s);
			}
			h1s->cs->flags |= CS_FL_EOI;
			TRACE_STATE("parsing error", H1_EV_RX_DATA|H1_EV_RX_TLRS|H1_EV_H1S_ERR, h1s->h1c->conn, h1s);
			h1_capture_bad_message(h1s->h1c, h1s, h1m, buf);
		}
		goto end;
	}

	*ofs += ret;

  end:
	TRACE_LEAVE(H1_EV_RX_DATA|H1_EV_RX_TLRS, h1s->h1c->conn, h1s,, (size_t[]){ret});
	return ret;
}

/*
 * Add the EOM in the HTX message. It returns 1 on success or 0 if it couldn't
 * proceed. This functions is responsible to update the parser state <h1m>. It
 * also add the flag CS_FL_EOI on the CS.
 */
static size_t h1_process_eom(struct h1s *h1s, struct h1m *h1m, struct htx *htx,
			     struct buffer *buf, size_t *ofs, size_t max)
{
	int ret;

	TRACE_ENTER(H1_EV_RX_DATA|H1_EV_RX_EOI, h1s->h1c->conn, h1s,, (size_t[]){max});
	ret = h1_parse_msg_eom(h1m, htx, max);
	if (!ret) {
		TRACE_DEVEL("leaving on missing data or error", H1_EV_RX_DATA|H1_EV_RX_EOI, h1s->h1c->conn, h1s);
		if (htx->flags & HTX_FL_PARSING_ERROR) {
			if (!(h1m->flags & H1_MF_RESP)) {
				h1s->flags |= H1S_F_REQ_ERROR;
				TRACE_USER("rejected H1 request", H1_EV_RX_DATA|H1_EV_RX_EOI|H1_EV_H1S_ERR, h1s->h1c->conn, h1s);
			}
			else {
				h1s->flags |= H1S_F_RES_ERROR;
				TRACE_USER("rejected H1 response", H1_EV_RX_DATA|H1_EV_RX_EOI|H1_EV_H1S_ERR, h1s->h1c->conn, h1s);
			}
			h1s->cs->flags |= CS_FL_EOI;
			TRACE_STATE("parsing error", H1_EV_RX_DATA|H1_EV_RX_EOI|H1_EV_H1S_ERR, h1s->h1c->conn, h1s);
			h1_capture_bad_message(h1s->h1c, h1s, h1m, buf);
		}
		goto end;
	}

	h1s->flags |= H1S_F_PARSING_DONE;
	/* Set EOI on conn-stream in DONE state iff:
	 *  - it is a response
	 *  - it is a request but no a protocol upgrade nor a CONNECT
	 *
	 * If not set, Wait the response to do so or not depending on the status
         * code.
         */
	if ((h1m->flags & H1_MF_RESP) || ((h1s->meth != HTTP_METH_CONNECT) && !(h1m->flags & H1_MF_CONN_UPG)))
		h1s->cs->flags |= CS_FL_EOI;
  end:
	TRACE_LEAVE(H1_EV_RX_DATA|H1_EV_RX_EOI, h1s->h1c->conn, h1s,, (size_t[]){ret});
	return ret;
}

/*
 * Process incoming data. It parses data and transfer them from h1c->ibuf into
 * <buf>. It returns the number of bytes parsed and transferred if > 0, or 0 if
 * it couldn't proceed.
 */
static size_t h1_process_input(struct h1c *h1c, struct buffer *buf, size_t count)
{
	struct h1s *h1s = h1c->h1s;
	struct h1m *h1m;
	struct htx *htx;
	size_t ret, data;
	size_t total = 0;
	int errflag;

	htx = htx_from_buf(buf);
	TRACE_ENTER(H1_EV_RX_DATA, h1c->conn, h1s, htx, (size_t[]){count});

	if (!conn_is_back(h1c->conn)) {
		h1m = &h1s->req;
		errflag = H1S_F_REQ_ERROR;
	}
	else {
		h1m = &h1s->res;
		errflag = H1S_F_RES_ERROR;
	}

	data = htx->data;
	if (h1s->flags & errflag)
		goto end;

	if (h1c->flags & H1C_F_IN_BUSY)
		goto end;

	do {
		size_t used = htx_used_space(htx);

		if (h1m->state <= H1_MSG_LAST_LF) {
			TRACE_PROTO("parsing message headers", H1_EV_RX_DATA|H1_EV_RX_HDRS, h1c->conn, h1s);
			ret = h1_process_headers(h1s, h1m, htx, &h1c->ibuf, &total, count);
			if (!ret)
				break;

			TRACE_USER((!(h1m->flags & H1_MF_RESP) ? "rcvd H1 request headers" : "rcvd H1 response headers"),
				   H1_EV_RX_DATA|H1_EV_RX_HDRS, h1c->conn, h1s, htx, (size_t[]){ret});

			if ((h1m->flags & H1_MF_RESP) &&
			    h1s->status < 200 && (h1s->status == 100 || h1s->status >= 102)) {
				h1m_init_res(&h1s->res);
				h1m->flags |= (H1_MF_NO_PHDR|H1_MF_CLEAN_CONN_HDR);
				TRACE_STATE("1xx response rcvd", H1_EV_RX_DATA|H1_EV_RX_HDRS, h1c->conn, h1s);
			}
		}
		else if (h1m->state < H1_MSG_TRAILERS) {
			TRACE_PROTO("parsing message payload", H1_EV_RX_DATA|H1_EV_RX_BODY, h1c->conn, h1s);
			ret = h1_process_data(h1s, h1m, &htx, &h1c->ibuf, &total, count, buf);
			if (!ret && h1m->state != H1_MSG_DONE)
				break;

			TRACE_PROTO((!(h1m->flags & H1_MF_RESP) ? "rcvd H1 request payload data" : "rcvd H1 response payload data"),
				    H1_EV_RX_DATA|H1_EV_RX_BODY, h1c->conn, h1s, htx, (size_t[]){ret});
		}
		else if (h1m->state == H1_MSG_TRAILERS) {
			TRACE_PROTO("parsing message trailers", H1_EV_RX_DATA|H1_EV_RX_TLRS, h1c->conn, h1s);
			ret = h1_process_trailers(h1s, h1m, htx, &h1c->ibuf, &total, count);
			if (!ret && h1m->state != H1_MSG_DONE)
				break;

			TRACE_PROTO((!(h1m->flags & H1_MF_RESP) ? "rcvd H1 request trailers" : "rcvd H1 response trailers"),
				    H1_EV_RX_DATA|H1_EV_RX_TLRS, h1c->conn, h1s, htx, (size_t[]){ret});
		}
		else if (h1m->state == H1_MSG_DONE) {
			if (!(h1s->flags & H1S_F_PARSING_DONE)) {
				if (!h1_process_eom(h1s, h1m, htx, &h1c->ibuf, &total, count))
					break;

				TRACE_USER((!(h1m->flags & H1_MF_RESP) ? "H1 request fully rcvd" : "H1 response fully rcvd"),
					   H1_EV_RX_DATA|H1_EV_RX_EOI, h1c->conn, h1s, htx);
			}

			if (!(h1m->flags & H1_MF_RESP) && h1s->status == 101)
				h1_set_req_tunnel_mode(h1s);
			else if ((h1m->flags & H1_MF_RESP) && h1s->req.state == H1_MSG_TUNNEL) {
				TRACE_STATE("switch back H1 request from tunnel mode", H1_EV_RX_DATA|H1_EV_H1C_BLK, h1c->conn, h1s);
				h1s->req.state = H1_MSG_DONE;
			}
			else if (h1s->req.state < H1_MSG_DONE || h1s->res.state < H1_MSG_DONE) {
				h1c->flags |= H1C_F_IN_BUSY;
				TRACE_STATE("switch h1c in busy mode", H1_EV_RX_DATA|H1_EV_H1C_BLK, h1c->conn, h1s);
				break;
			}
			else
				break;
		}
		else if (h1m->state == H1_MSG_TUNNEL) {
			TRACE_PROTO("parsing tunneled data", H1_EV_RX_DATA, h1c->conn, h1s);
			ret = h1_process_data(h1s, h1m, &htx, &h1c->ibuf, &total, count, buf);
			if (!ret)
				break;

			TRACE_PROTO((!(h1m->flags & H1_MF_RESP) ? "rcvd H1 request tunneled data" : "rcvd H1 response tunneled data"),
				    H1_EV_RX_DATA|H1_EV_RX_EOI, h1c->conn, h1s, htx, (size_t[]){ret});
		}
		else {
			h1s->flags |= errflag;
			break;
		}

		count -= htx_used_space(htx) - used;
	} while (!(h1s->flags & errflag));

	if (h1s->flags & errflag) {
		TRACE_PROTO("parsing error", H1_EV_RX_DATA, h1c->conn, h1s);
		goto parsing_err;
	}

	b_del(&h1c->ibuf, total);

  end:
	htx_to_buf(htx, buf);
	ret = htx->data - data;
	if ((h1c->flags & H1C_F_IN_FULL) && buf_room_for_htx_data(&h1c->ibuf)) {
		h1c->flags &= ~H1C_F_IN_FULL;
		TRACE_STATE("h1c ibuf not full anymore", H1_EV_RX_DATA|H1_EV_H1C_BLK|H1_EV_H1C_WAKE);
		h1c->conn->xprt->subscribe(h1c->conn, h1c->conn->xprt_ctx, SUB_RETRY_RECV, &h1c->wait_event);
	}

	h1s->cs->flags &= ~(CS_FL_RCV_MORE | CS_FL_WANT_ROOM);

	if (!b_data(&h1c->ibuf))
		h1_release_buf(h1c, &h1c->ibuf);
	if (h1s_data_pending(h1s) && !htx_is_empty(htx))
		h1s->cs->flags |= CS_FL_RCV_MORE | CS_FL_WANT_ROOM;
	else if (h1s->flags & H1S_F_REOS) {
		h1s->cs->flags |= CS_FL_EOS;
		if (h1m->state >= H1_MSG_DONE)
			h1s->cs->flags |= CS_FL_EOI;
		else if (h1m->state > H1_MSG_LAST_LF && h1m->state < H1_MSG_DONE)
			h1s->cs->flags |= CS_FL_ERROR;
	}

	TRACE_LEAVE(H1_EV_RX_DATA, h1c->conn, h1s, htx, (size_t[]){ret});
	return ret;

  parsing_err:
	b_reset(&h1c->ibuf);
	htx_to_buf(htx, buf);
	TRACE_DEVEL("leaving on error", H1_EV_RX_DATA|H1_EV_STRM_ERR, h1c->conn, h1s);
	return 0;
}

/*
 * Process outgoing data. It parses data and transfer them from the channel buffer into
 * h1c->obuf. It returns the number of bytes parsed and transferred if > 0, or
 * 0 if it couldn't proceed.
 */
static size_t h1_process_output(struct h1c *h1c, struct buffer *buf, size_t count)
{
	struct h1s *h1s = h1c->h1s;
	struct h1m *h1m;
	struct htx *chn_htx = NULL;
	struct htx_blk *blk;
	struct buffer tmp;
	size_t total = 0;
	int errflag;

	if (!count)
		goto end;

	chn_htx = htxbuf(buf);
	TRACE_ENTER(H1_EV_TX_DATA, h1c->conn, h1s, chn_htx, (size_t[]){count});

	if (htx_is_empty(chn_htx))
		goto end;

	if (!h1_get_buf(h1c, &h1c->obuf)) {
		h1c->flags |= H1C_F_OUT_ALLOC;
		TRACE_STATE("waiting for h1c obuf allocation", H1_EV_TX_DATA|H1_EV_H1S_BLK, h1c->conn, h1s);
		goto end;
	}

	if (!conn_is_back(h1c->conn)) {
		h1m = &h1s->res;
		errflag = H1S_F_RES_ERROR;
	}
	else {
		h1m = &h1s->req;
		errflag = H1S_F_REQ_ERROR;
	}

	if (h1s->flags & errflag)
		goto end;

	/* the htx is non-empty thus has at least one block */
	blk = htx_get_head_blk(chn_htx);

	/* Perform some optimizations to reduce the number of buffer copies.
	 * First, if the mux's buffer is empty and the htx area contains
	 * exactly one data block of the same size as the requested count,
	 * then it's possible to simply swap the caller's buffer with the
	 * mux's output buffer and adjust offsets and length to match the
	 * entire DATA HTX block in the middle. In this case we perform a
	 * true zero-copy operation from end-to-end. This is the situation
	 * that happens all the time with large files. Second, if this is not
	 * possible, but the mux's output buffer is empty, we still have an
	 * opportunity to avoid the copy to the intermediary buffer, by making
	 * the intermediary buffer's area point to the output buffer's area.
	 * In this case we want to skip the HTX header to make sure that copies
	 * remain aligned and that this operation remains possible all the
	 * time. This goes for headers, data blocks and any data extracted from
	 * the HTX blocks.
	 */
	if (!b_data(&h1c->obuf)) {
		if (htx_nbblks(chn_htx) == 1 &&
		    htx_get_blk_type(blk) == HTX_BLK_DATA &&
		    htx_get_blk_value(chn_htx, blk).len == count) {
			void *old_area = h1c->obuf.area;

			TRACE_PROTO("sending message data (zero-copy)", H1_EV_TX_DATA|H1_EV_TX_BODY, h1c->conn, h1s, chn_htx, (size_t[]){count});
			h1c->obuf.area = buf->area;
			h1c->obuf.head = sizeof(struct htx) + blk->addr;
			h1c->obuf.data = count;

			buf->area = old_area;
			buf->data = buf->head = 0;

			chn_htx = (struct htx *)buf->area;
			htx_reset(chn_htx);

			/* The message is chunked. We need to emit the chunk
			 * size. We have at least the size of the struct htx to
			 * write the chunk envelope. It should be enough.
			 */
			if (h1m->flags & H1_MF_CHNK) {
				h1_emit_chunk_size(&h1c->obuf, count);
				h1_emit_chunk_crlf(&h1c->obuf);
			}

			total += count;
			if (h1m->state == H1_MSG_DATA)
				TRACE_PROTO((!(h1m->flags & H1_MF_RESP) ? "H1 request payload data xferred" : "H1 response payload data xferred"),
					    H1_EV_TX_DATA|H1_EV_TX_BODY, h1c->conn, h1s,, (size_t[]){count});
			else
				TRACE_PROTO((!(h1m->flags & H1_MF_RESP) ? "H1 request tunneled data xferred" : "H1 response tunneled data xferred"),
					    H1_EV_TX_DATA|H1_EV_TX_BODY, h1c->conn, h1s,, (size_t[]){count});
			goto out;
		}
		tmp.area = h1c->obuf.area + h1c->obuf.head;
	}
	else
		tmp.area = trash.area;

	tmp.data = 0;
	tmp.size = b_room(&h1c->obuf);
	while (count && !(h1s->flags & errflag) && blk) {
		struct htx_sl *sl;
		struct ist n, v;
		enum htx_blk_type type = htx_get_blk_type(blk);
		uint32_t sz = htx_get_blksz(blk);
		uint32_t vlen, chklen;

		vlen = sz;
		if (type != HTX_BLK_DATA && vlen > count)
			goto full;

		if (type == HTX_BLK_UNUSED)
			goto nextblk;

		switch (h1m->state) {
			case H1_MSG_RQBEFORE:
				if (type != HTX_BLK_REQ_SL)
					goto error;
				TRACE_USER("sending request headers", H1_EV_TX_DATA|H1_EV_TX_HDRS, h1c->conn, h1s, chn_htx);
				sl = htx_get_blk_ptr(chn_htx, blk);
				h1s->meth = sl->info.req.meth;
				h1_parse_req_vsn(h1m, sl);
				if (!h1_format_htx_reqline(sl, &tmp))
					goto full;
				h1m->flags |= H1_MF_XFER_LEN;
				if (sl->flags & HTX_SL_F_BODYLESS)
					h1m->flags |= H1_MF_CLEN;
				h1m->state = H1_MSG_HDR_FIRST;
				break;

			case H1_MSG_RPBEFORE:
				if (type != HTX_BLK_RES_SL)
					goto error;
				TRACE_USER("sending response headers", H1_EV_TX_DATA|H1_EV_TX_HDRS, h1c->conn, h1s, chn_htx);
				sl = htx_get_blk_ptr(chn_htx, blk);
				h1s->status = sl->info.res.status;
				h1_parse_res_vsn(h1m, sl);
				if (!h1_format_htx_stline(sl, &tmp))
					goto full;
				if (sl->flags & HTX_SL_F_XFER_LEN)
					h1m->flags |= H1_MF_XFER_LEN;
				if (sl->info.res.status < 200 &&
				    (sl->info.res.status == 100 || sl->info.res.status >= 102))
					h1s->flags |= H1S_F_HAVE_O_CONN;
				h1m->state = H1_MSG_HDR_FIRST;
				break;

			case H1_MSG_HDR_FIRST:
			case H1_MSG_HDR_NAME:
			case H1_MSG_HDR_L2_LWS:
				if (type == HTX_BLK_EOH)
					goto last_lf;
				if (type != HTX_BLK_HDR)
					goto error;

				h1m->state = H1_MSG_HDR_NAME;
				n = htx_get_blk_name(chn_htx, blk);
				v = htx_get_blk_value(chn_htx, blk);

				/* Skip all pseudo-headers */
				if (*(n.ptr) == ':')
					goto skip_hdr;

				if (isteq(n, ist("transfer-encoding")))
					h1_parse_xfer_enc_header(h1m, v);
				else if (isteq(n, ist("content-length"))) {
					/* Only skip C-L header with invalid value. */
					if (h1_parse_cont_len_header(h1m, &v) < 0)
						goto skip_hdr;
				}
				else if (isteq(n, ist("connection"))) {
					h1_parse_connection_header(h1m, &v);
					if (!v.len)
						goto skip_hdr;
				}

				/* Skip header if same name is used to add the server name */
				if (!(h1m->flags & H1_MF_RESP) && h1c->px->server_id_hdr_name &&
				    isteqi(n, ist2(h1c->px->server_id_hdr_name, h1c->px->server_id_hdr_len)))
					goto skip_hdr;

				/* Try to adjust the case of the header name */
				if (h1c->px->options2 & (PR_O2_H1_ADJ_BUGCLI|PR_O2_H1_ADJ_BUGSRV))
					h1_adjust_case_outgoing_hdr(h1s, h1m, &n);
				if (!h1_format_htx_hdr(n, v, &tmp))
					goto full;
			  skip_hdr:
				h1m->state = H1_MSG_HDR_L2_LWS;
				break;

			case H1_MSG_LAST_LF:
				if (type != HTX_BLK_EOH)
					goto error;
			  last_lf:
				h1m->state = H1_MSG_LAST_LF;
				if (!(h1s->flags & H1S_F_HAVE_O_CONN)) {
					/* If the reply comes from haproxy while the request is
					 * not finished, we force the connection close. */
					if ((chn_htx->flags & HTX_FL_PROXY_RESP) && h1s->req.state != H1_MSG_DONE) {
						h1s->flags = (h1s->flags & ~H1S_F_WANT_MSK) | H1S_F_WANT_CLO;
						TRACE_STATE("force close mode (resp)", H1_EV_TX_DATA|H1_EV_TX_HDRS, h1s->h1c->conn, h1s);
					}

					/* the conn_mode must be processed. So do it */
					n = ist("connection");
					v = ist("");
					h1_process_output_conn_mode(h1s, h1m, &v);
					if (v.len) {
						/* Try to adjust the case of the header name */
						if (h1c->px->options2 & (PR_O2_H1_ADJ_BUGCLI|PR_O2_H1_ADJ_BUGSRV))
							h1_adjust_case_outgoing_hdr(h1s, h1m, &n);
						if (!h1_format_htx_hdr(n, v, &tmp))
							goto full;
					}
					h1s->flags |= H1S_F_HAVE_O_CONN;
				}

				if ((h1s->meth != HTTP_METH_CONNECT &&
				     (h1m->flags & (H1_MF_VER_11|H1_MF_RESP|H1_MF_CLEN|H1_MF_CHNK|H1_MF_XFER_LEN)) ==
				     (H1_MF_VER_11|H1_MF_XFER_LEN)) ||
				    (h1s->status >= 200 && h1s->status != 204 && h1s->status != 304 &&
				     h1s->meth != HTTP_METH_HEAD && !(h1s->meth == HTTP_METH_CONNECT && h1s->status == 200) &&
				     (h1m->flags & (H1_MF_VER_11|H1_MF_RESP|H1_MF_CLEN|H1_MF_CHNK|H1_MF_XFER_LEN)) ==
				     (H1_MF_VER_11|H1_MF_RESP|H1_MF_XFER_LEN))) {
					/* chunking needed but header not seen */
					n = ist("transfer-encoding");
					v = ist("chunked");
					if (h1c->px->options2 & (PR_O2_H1_ADJ_BUGCLI|PR_O2_H1_ADJ_BUGSRV))
						h1_adjust_case_outgoing_hdr(h1s, h1m, &n);
					if (!h1_format_htx_hdr(n, v, &tmp))
						goto full;
					TRACE_STATE("add \"Transfer-Encoding: chunked\"", H1_EV_TX_DATA|H1_EV_TX_HDRS, h1c->conn, h1s);
					h1m->flags |= H1_MF_CHNK;
				}

				/* Now add the server name to a header (if requested) */
				if (!(h1s->flags & H1S_F_HAVE_SRV_NAME) &&
				    !(h1m->flags & H1_MF_RESP) && h1c->px->server_id_hdr_name) {
					struct server *srv = objt_server(h1c->conn->target);

					if (srv) {
						n = ist2(h1c->px->server_id_hdr_name, h1c->px->server_id_hdr_len);
						v = ist(srv->id);

						/* Try to adjust the case of the header name */
						if (h1c->px->options2 & (PR_O2_H1_ADJ_BUGCLI|PR_O2_H1_ADJ_BUGSRV))
							h1_adjust_case_outgoing_hdr(h1s, h1m, &n);
						if (!h1_format_htx_hdr(n, v, &tmp))
							goto full;
					}
					TRACE_STATE("add server name header", H1_EV_TX_DATA|H1_EV_TX_HDRS, h1c->conn, h1s);
					h1s->flags |= H1S_F_HAVE_SRV_NAME;
				}

				if (!chunk_memcat(&tmp, "\r\n", 2))
					goto full;

				TRACE_PROTO((!(h1m->flags & H1_MF_RESP) ? "H1 request headers xferred" : "H1 response headers xferred"),
					    H1_EV_TX_DATA|H1_EV_TX_HDRS, h1c->conn, h1s);

				if (!(h1m->flags & H1_MF_RESP) && h1s->meth == HTTP_METH_CONNECT) {
					/* a CONNECT request is sent to the server. Switch it to tunnel mode. */
					h1_set_req_tunnel_mode(h1s);
				}
				else if ((h1m->flags & H1_MF_RESP) &&
					 ((h1s->meth == HTTP_METH_CONNECT && h1s->status == 200) || h1s->status == 101)) {
					/* a successful reply to a CONNECT or a protocol switching is sent
					 * to the client. Switch the response to tunnel mode.
					 */
					h1_set_res_tunnel_mode(h1s);
					TRACE_STATE("switch H1 response in tunnel mode", H1_EV_TX_DATA|H1_EV_TX_HDRS, h1c->conn, h1s);
				}
				else if ((h1m->flags & H1_MF_RESP) &&
					 h1s->status < 200 && (h1s->status == 100 || h1s->status >= 102)) {
					h1m_init_res(&h1s->res);
					h1m->flags |= (H1_MF_NO_PHDR|H1_MF_CLEAN_CONN_HDR);
					h1s->flags &= ~H1S_F_HAVE_O_CONN;
					TRACE_STATE("1xx response xferred", H1_EV_TX_DATA|H1_EV_TX_HDRS, h1c->conn, h1s);
				}
				else if ((h1m->flags & H1_MF_RESP) &&  h1s->meth == HTTP_METH_HEAD) {
					h1m->state = H1_MSG_DONE;
					TRACE_STATE("HEAD response processed", H1_EV_TX_DATA|H1_EV_TX_HDRS, h1c->conn, h1s);
				}
				else
					h1m->state = H1_MSG_DATA;
				break;

			case H1_MSG_DATA:
			case H1_MSG_TUNNEL:
				if (type == HTX_BLK_EOM) {
					/* Chunked message without explicit trailers */
					if (h1m->flags & H1_MF_CHNK) {
						if (!chunk_memcat(&tmp, "0\r\n\r\n", 5))
							goto full;
					}
					goto done;
				}
				else if (type == HTX_BLK_EOT || type == HTX_BLK_TLR) {
					/* If the message is not chunked, never
					 * add the last chunk. */
					if ((h1m->flags & H1_MF_CHNK) && !chunk_memcat(&tmp, "0\r\n", 3))
						goto full;
					TRACE_PROTO("sending message trailers", H1_EV_TX_DATA|H1_EV_TX_TLRS, h1c->conn, h1s, chn_htx);
					goto trailers;
				}
				else if (type != HTX_BLK_DATA)
					goto error;

				TRACE_PROTO("sending message data", H1_EV_TX_DATA|H1_EV_TX_BODY, h1c->conn, h1s, chn_htx, (size_t[]){sz});


				if (vlen > count) {
					/* Get the maximum amount of data we can xferred */
					vlen = count;
				}

				chklen = 0;
				if (h1m->flags & H1_MF_CHNK) {
					chklen = b_room(&tmp);
					chklen = ((chklen < 16) ? 1 : (chklen < 256) ? 2 :
						  (chklen < 4096) ? 3 : (chklen < 65536) ? 4 :
						  (chklen < 1048576) ? 5 : 8);
					chklen += 4; /* 2 x CRLF */
				}

				if (vlen + chklen > b_room(&tmp)) {
					/* too large for the buffer */
					if (chklen >= b_room(&tmp))
						goto full;
					vlen = b_room(&tmp) - chklen;
				}
				v = htx_get_blk_value(chn_htx, blk);
				v.len = vlen;
				if (!h1_format_htx_data(v, &tmp, !!(h1m->flags & H1_MF_CHNK)))
					goto full;

				if (h1m->state == H1_MSG_DATA)
					TRACE_PROTO((!(h1m->flags & H1_MF_RESP) ? "H1 request payload data xferred" : "H1 response payload data xferred"),
						    H1_EV_TX_DATA|H1_EV_TX_BODY, h1c->conn, h1s,, (size_t[]){v.len});
				else
					TRACE_PROTO((!(h1m->flags & H1_MF_RESP) ? "H1 request tunneled data xferred" : "H1 response tunneled data xferred"),
						    H1_EV_TX_DATA|H1_EV_TX_BODY, h1c->conn, h1s,, (size_t[]){v.len});
				break;

			case H1_MSG_TRAILERS:
				if (type == HTX_BLK_EOM)
					goto done;
				else if (type != HTX_BLK_TLR && type != HTX_BLK_EOT)
					goto error;
			  trailers:
				h1m->state = H1_MSG_TRAILERS;
				/* If the message is not chunked, ignore
				 * trailers. It may happen with H2 messages. */
				if (!(h1m->flags & H1_MF_CHNK))
					break;

				if (type == HTX_BLK_EOT) {
					if (!chunk_memcat(&tmp, "\r\n", 2))
						goto full;
					TRACE_PROTO((!(h1m->flags & H1_MF_RESP) ? "H1 request trailers xferred" : "H1 response trailers xferred"),
						    H1_EV_TX_DATA|H1_EV_TX_TLRS, h1c->conn, h1s);
				}
				else { // HTX_BLK_TLR
					n = htx_get_blk_name(chn_htx, blk);
					v = htx_get_blk_value(chn_htx, blk);

					/* Try to adjust the case of the header name */
					if (h1c->px->options2 & (PR_O2_H1_ADJ_BUGCLI|PR_O2_H1_ADJ_BUGSRV))
						h1_adjust_case_outgoing_hdr(h1s, h1m, &n);
					if (!h1_format_htx_hdr(n, v, &tmp))
						goto full;
				}
				break;

			case H1_MSG_DONE:
				if (type != HTX_BLK_EOM)
					goto error;
			  done:
				h1m->state = H1_MSG_DONE;
				if (!(h1m->flags & H1_MF_RESP) && h1s->status == 101) {
					h1_set_req_tunnel_mode(h1s);
					TRACE_STATE("switch H1 request in tunnel mode", H1_EV_TX_DATA|H1_EV_TX_HDRS, h1c->conn, h1s);
				}
				else if ((h1m->flags & H1_MF_RESP) && h1s->req.state == H1_MSG_TUNNEL) {
					TRACE_STATE("switch back H1 request from tunnel mode", H1_EV_RX_DATA|H1_EV_H1C_BLK, h1c->conn, h1s);
					h1s->req.state = H1_MSG_DONE;
					h1s->flags |= H1S_F_PARSING_DONE;
				}

				if (h1s->h1c->flags & H1C_F_IN_BUSY) {
					h1s->h1c->flags &= ~H1C_F_IN_BUSY;
					h1c->conn->xprt->subscribe(h1c->conn, h1c->conn->xprt_ctx, SUB_RETRY_RECV, &h1c->wait_event);
					TRACE_STATE("h1c no more busy", H1_EV_TX_DATA|H1_EV_H1C_BLK|H1_EV_H1C_WAKE, h1c->conn, h1s);
				}

				TRACE_USER((!(h1m->flags & H1_MF_RESP) ? "H1 request fully xferred" : "H1 response fully xferred"),
					   H1_EV_TX_DATA, h1c->conn, h1s);
				break;

			default:
			  error:
				TRACE_PROTO("formatting error", H1_EV_TX_DATA, h1c->conn, h1s);
				/* Unexpected error during output processing */
				chn_htx->flags |= HTX_FL_PROCESSING_ERROR;
				h1s->flags |= errflag;
				h1c->flags |= H1C_F_CS_ERROR;
				TRACE_STATE("processing error, set error on h1c/h1s", H1_EV_H1C_ERR|H1_EV_H1S_ERR, h1c->conn, h1s);
				TRACE_DEVEL("unexpected error", H1_EV_TX_DATA|H1_EV_STRM_ERR, h1c->conn, h1s);
				break;
		}

	  nextblk:
		total += vlen;
		count -= vlen;
		if (sz == vlen)
			blk = htx_remove_blk(chn_htx, blk);
		else {
			htx_cut_data_blk(chn_htx, blk, vlen);
			break;
		}
	}

  copy:
	/* when the output buffer is empty, tmp shares the same area so that we
	 * only have to update pointers and lengths.
	 */
	if (tmp.area == h1c->obuf.area + h1c->obuf.head)
		h1c->obuf.data = tmp.data;
	else
		b_putblk(&h1c->obuf, tmp.area, tmp.data);

	htx_to_buf(chn_htx, buf);
  out:
	/* Both the request and the response reached the DONE state. So set EOI
	 * flag on the conn-stream. Most of time, the flag will already be set,
	 * except for protocol upgrades.
	 */
	if (h1s->cs && h1s->req.state == H1_MSG_DONE && h1s->res.state == H1_MSG_DONE)
			h1s->cs->flags |= CS_FL_EOI;

	if (!buf_room_for_htx_data(&h1c->obuf)) {
		TRACE_STATE("h1c obuf full", H1_EV_TX_DATA|H1_EV_H1S_BLK, h1c->conn, h1s);
		h1c->flags |= H1C_F_OUT_FULL;
	}
  end:
	TRACE_LEAVE(H1_EV_TX_DATA, h1c->conn, h1s, chn_htx, (size_t[]){total});
	return total;

  full:
	TRACE_STATE("h1c obuf full", H1_EV_TX_DATA|H1_EV_H1S_BLK, h1c->conn, h1s);
	h1c->flags |= H1C_F_OUT_FULL;
	goto copy;
}

/*********************************************************/
/* functions below are I/O callbacks from the connection */
/*********************************************************/
static void h1_wake_stream_for_recv(struct h1s *h1s)
{
	if (h1s && h1s->subs && h1s->subs->events & SUB_RETRY_RECV) {
		TRACE_POINT(H1_EV_STRM_WAKE, h1s->h1c->conn, h1s);
		tasklet_wakeup(h1s->subs->tasklet);
		h1s->subs->events &= ~SUB_RETRY_RECV;
		if (!h1s->subs->events)
			h1s->subs = NULL;
	}
}
static void h1_wake_stream_for_send(struct h1s *h1s)
{
	if (h1s && h1s->subs && h1s->subs->events & SUB_RETRY_SEND) {
		TRACE_POINT(H1_EV_STRM_WAKE, h1s->h1c->conn, h1s);
		tasklet_wakeup(h1s->subs->tasklet);
		h1s->subs->events &= ~SUB_RETRY_SEND;
		if (!h1s->subs->events)
			h1s->subs = NULL;
	}
}

/*
 * Attempt to read data, and subscribe if none available
 */
static int h1_recv(struct h1c *h1c)
{
	struct connection *conn = h1c->conn;
	struct h1s *h1s = h1c->h1s;
	size_t ret = 0, max;
	int rcvd = 0;
	int flags = 0;

	TRACE_ENTER(H1_EV_H1C_RECV, h1c->conn);

	if (h1c->wait_event.events & SUB_RETRY_RECV) {
		TRACE_DEVEL("leaving on sub_recv", H1_EV_H1C_RECV, h1c->conn);
		return (b_data(&h1c->ibuf));
	}

	if (!h1_recv_allowed(h1c)) {
		TRACE_DEVEL("leaving on !recv_allowed", H1_EV_H1C_RECV, h1c->conn);
		rcvd = 1;
		goto end;
	}

	if (!h1_get_buf(h1c, &h1c->ibuf)) {
		h1c->flags |= H1C_F_IN_ALLOC;
		TRACE_STATE("waiting for h1c ibuf allocation", H1_EV_H1C_RECV|H1_EV_H1C_BLK, h1c->conn);
		goto end;
	}

	if (h1s && (h1s->flags & (H1S_F_BUF_FLUSH|H1S_F_SPLICED_DATA))) {
		if (!h1s_data_pending(h1s))
			h1_wake_stream_for_recv(h1s);
		rcvd = 1;
		TRACE_DEVEL("leaving on (buf_flush|spliced_data)", H1_EV_H1C_RECV, h1c->conn);
		goto end;
	}

	/*
	 * If we only have a small amount of data, realign it,
	 * it's probably cheaper than doing 2 recv() calls.
	 */
	if (b_data(&h1c->ibuf) > 0 && b_data(&h1c->ibuf) < 128)
		b_slow_realign(&h1c->ibuf, trash.area, 0);

	/* avoid useless reads after first responses */
	if (h1s && ((!conn_is_back(conn) && h1s->req.state == H1_MSG_RQBEFORE) ||
		    (conn_is_back(conn) && h1s->res.state == H1_MSG_RPBEFORE)))
		flags |= CO_RFL_READ_ONCE;

	max = buf_room_for_htx_data(&h1c->ibuf);
	if (max) {
		if (h1c->flags & H1C_F_IN_FULL) {
			h1c->flags &= ~H1C_F_IN_FULL;
			TRACE_STATE("h1c ibuf not full anymore", H1_EV_H1C_RECV|H1_EV_H1C_BLK);
		}

		b_realign_if_empty(&h1c->ibuf);
		if (!b_data(&h1c->ibuf)) {
			/* try to pre-align the buffer like the rxbufs will be
			 * to optimize memory copies.
			 */
			h1c->ibuf.head  = sizeof(struct htx);
		}
		ret = conn->xprt->rcv_buf(conn, conn->xprt_ctx, &h1c->ibuf, max, flags);
	}
	if (ret > 0) {
		TRACE_DATA("data received", H1_EV_H1C_RECV, h1c->conn,,, (size_t[]){ret});
		rcvd = 1;
		if (h1s && h1s->cs) {
			h1s->cs->flags |= (CS_FL_READ_PARTIAL|CS_FL_RCV_MORE);
			if (h1s->csinfo.t_idle == -1)
				h1s->csinfo.t_idle = tv_ms_elapsed(&h1s->csinfo.tv_create, &now) - h1s->csinfo.t_handshake;
		}
	}

	if (ret > 0 || !h1_recv_allowed(h1c) || !buf_room_for_htx_data(&h1c->ibuf)) {
		rcvd = 1;
		goto end;
	}

	TRACE_STATE("failed to receive data, subscribing", H1_EV_H1C_RECV, h1c->conn);
	conn->xprt->subscribe(conn, conn->xprt_ctx, SUB_RETRY_RECV, &h1c->wait_event);

  end:
	if (ret > 0 || (conn->flags & CO_FL_ERROR) || conn_xprt_read0_pending(conn))
		h1_wake_stream_for_recv(h1s);

	if (conn_xprt_read0_pending(conn) && h1s) {
		h1s->flags |= H1S_F_REOS;
		TRACE_STATE("read0 on connection", H1_EV_H1C_RECV, conn, h1s);
		rcvd = 1;
	}

	if (!b_data(&h1c->ibuf))
		h1_release_buf(h1c, &h1c->ibuf);
	else if (!buf_room_for_htx_data(&h1c->ibuf)) {
		h1c->flags |= H1C_F_IN_FULL;
		TRACE_STATE("h1c ibuf full", H1_EV_H1C_RECV|H1_EV_H1C_BLK);
	}

	TRACE_LEAVE(H1_EV_H1C_RECV, h1c->conn);
	return rcvd;
}


/*
 * Try to send data if possible
 */
static int h1_send(struct h1c *h1c)
{
	struct connection *conn = h1c->conn;
	unsigned int flags = 0;
	size_t ret;
	int sent = 0;

	TRACE_ENTER(H1_EV_H1C_SEND, h1c->conn);

	if (conn->flags & CO_FL_ERROR) {
		TRACE_DEVEL("leaving on connection error", H1_EV_H1C_SEND, h1c->conn);
		return 0;
	}

	if (!b_data(&h1c->obuf))
		goto end;

	if (h1c->flags & H1C_F_CO_MSG_MORE)
		flags |= CO_SFL_MSG_MORE;
	if (h1c->flags & H1C_F_CO_STREAMER)
		flags |= CO_SFL_STREAMER;

	ret = conn->xprt->snd_buf(conn, conn->xprt_ctx, &h1c->obuf, b_data(&h1c->obuf), flags);
	if (ret > 0) {
		TRACE_DATA("data sent", H1_EV_H1C_SEND, h1c->conn,,, (size_t[]){ret});
		if (h1c->flags & H1C_F_OUT_FULL) {
			h1c->flags &= ~H1C_F_OUT_FULL;
			TRACE_STATE("h1c obuf not full anymore", H1_EV_STRM_SEND|H1_EV_H1S_BLK, h1c->conn);
		}
		b_del(&h1c->obuf, ret);
		sent = 1;
	}

	if (conn->flags & (CO_FL_ERROR|CO_FL_SOCK_WR_SH)) {
		TRACE_DEVEL("connection error or output closed", H1_EV_H1C_SEND, h1c->conn);
		/* error or output closed, nothing to send, clear the buffer to release it */
		b_reset(&h1c->obuf);
	}

  end:
	if (!(h1c->flags & H1C_F_OUT_FULL))
		h1_wake_stream_for_send(h1c->h1s);

	/* We're done, no more to send */
	if (!b_data(&h1c->obuf)) {
		TRACE_DEVEL("leaving with everything sent", H1_EV_H1C_SEND, h1c->conn);
		h1_release_buf(h1c, &h1c->obuf);
		if (h1c->flags & H1C_F_CS_SHUTW_NOW) {
			TRACE_STATE("process pending shutdown for writes", H1_EV_H1C_SEND, h1c->conn);
			h1_shutw_conn(conn, CS_SHW_NORMAL);
		}
	}
	else if (!(h1c->wait_event.events & SUB_RETRY_SEND)) {
		TRACE_STATE("more data to send, subscribing", H1_EV_H1C_SEND, h1c->conn);
		conn->xprt->subscribe(conn, conn->xprt_ctx, SUB_RETRY_SEND, &h1c->wait_event);
	}

	TRACE_LEAVE(H1_EV_H1C_SEND, h1c->conn);
	return sent;
}


/* callback called on any event by the connection handler.
 * It applies changes and returns zero, or < 0 if it wants immediate
 * destruction of the connection.
 */
static int h1_process(struct h1c * h1c)
{
	struct connection *conn = h1c->conn;
	struct h1s *h1s = h1c->h1s;

	TRACE_ENTER(H1_EV_H1C_WAKE, conn);

	if (!conn->ctx)
		return -1;

	if (!h1s) {
		if (h1c->flags & (H1C_F_CS_ERROR|H1C_F_CS_SHUTDOWN) ||
		    conn->flags & (CO_FL_ERROR|CO_FL_SOCK_RD_SH|CO_FL_SOCK_WR_SH))
			goto release;
		if (!conn_is_back(conn) && (h1c->flags & H1C_F_CS_IDLE)) {
			TRACE_STATE("K/A incoming connection, create new H1 stream", H1_EV_H1C_WAKE, conn);
			if (!h1s_create(h1c, NULL, NULL))
				goto release;
		}
		else if (conn_is_back(conn) && (h1c->flags & H1C_F_CS_IDLE) && b_data(&h1c->ibuf))
			goto release;
		else
			goto end;
		h1s = h1c->h1s;
	}

	if (b_data(&h1c->ibuf) && h1s->csinfo.t_idle == -1)
		h1s->csinfo.t_idle = tv_ms_elapsed(&h1s->csinfo.tv_create, &now) - h1s->csinfo.t_handshake;

	if (conn_xprt_read0_pending(conn)) {
		h1s->flags |= H1S_F_REOS;
		TRACE_STATE("read0 on connection", H1_EV_H1C_RECV, conn, h1s);
	}

	if (!h1s_data_pending(h1s) && h1s && h1s->cs && h1s->cs->data_cb->wake &&
	    (h1s->flags & H1S_F_REOS || h1c->flags & H1C_F_CS_ERROR ||
	    conn->flags & (CO_FL_ERROR | CO_FL_SOCK_WR_SH))) {
		if (h1c->flags & H1C_F_CS_ERROR || conn->flags & CO_FL_ERROR)
			h1s->cs->flags |= CS_FL_ERROR;
		TRACE_POINT(H1_EV_STRM_WAKE, h1c->conn, h1s);
		h1s->cs->data_cb->wake(h1s->cs);
	}
  end:
	h1_refresh_timeout(h1c);
	TRACE_LEAVE(H1_EV_H1C_WAKE, conn);
	return 0;

  release:
	h1_release(h1c);
	TRACE_DEVEL("leaving after releasing the connection", H1_EV_H1C_WAKE);
	return -1;
}

struct task *h1_io_cb(struct task *t, void *ctx, unsigned short status)
{
	struct connection *conn;
	struct tasklet *tl = (struct tasklet *)t;
	int conn_in_list;
	struct h1c *h1c;
	int ret = 0;


	HA_SPIN_LOCK(OTHER_LOCK, &idle_conns[tid].takeover_lock);
	if (tl->context == NULL) {
		/* The connection has been taken over by another thread,
		 * we're no longer responsible for it, so just free the
		 * tasklet, and do nothing.
		 */
		HA_SPIN_UNLOCK(OTHER_LOCK, &idle_conns[tid].takeover_lock);
		tasklet_free(tl);
		return NULL;
	}
	h1c = ctx;
	conn = h1c->conn;

	TRACE_POINT(H1_EV_H1C_WAKE, conn);

	/* Remove the connection from the list, to be sure nobody attempts
	 * to use it while we handle the I/O events
	 */
	conn_in_list = conn->flags & CO_FL_LIST_MASK;
	if (conn_in_list)
		MT_LIST_DEL(&conn->list);

	HA_SPIN_UNLOCK(OTHER_LOCK, &idle_conns[tid].takeover_lock);

	if (!(h1c->wait_event.events & SUB_RETRY_SEND))
		ret = h1_send(h1c);
	if (!(h1c->wait_event.events & SUB_RETRY_RECV))
		ret |= h1_recv(h1c);
	if (ret || !h1c->h1s)
		ret = h1_process(h1c);
	/* If we were in an idle list, we want to add it back into it,
	 * unless h1_process() returned -1, which mean it has destroyed
	 * the connection (testing !ret is enough, if h1_process() wasn't
	 * called then ret will be 0 anyway.
	 */
	if (!ret && conn_in_list) {
		struct server *srv = objt_server(conn->target);

		if (conn_in_list == CO_FL_SAFE_LIST)
			MT_LIST_ADDQ(&srv->safe_conns[tid], &conn->list);
		else
			MT_LIST_ADDQ(&srv->idle_conns[tid], &conn->list);
	}
	return NULL;
}

static void h1_reset(struct connection *conn)
{

}

static int h1_wake(struct connection *conn)
{
	struct h1c *h1c = conn->ctx;
	int ret;

	TRACE_POINT(H1_EV_H1C_WAKE, conn);

	h1_send(h1c);
	ret = h1_process(h1c);
	if (ret == 0) {
		struct h1s *h1s = h1c->h1s;

		if (h1s && h1s->cs && h1s->cs->data_cb->wake) {
			TRACE_POINT(H1_EV_STRM_WAKE, h1c->conn, h1s);
			ret = h1s->cs->data_cb->wake(h1s->cs);
		}
	}
	return ret;
}

/* Connection timeout management. The principle is that if there's no receipt
 * nor sending for a certain amount of time, the connection is closed.
 */
static struct task *h1_timeout_task(struct task *t, void *context, unsigned short state)
{
	struct h1c *h1c = context;
	int expired = tick_is_expired(t->expire, now_ms);

	TRACE_POINT(H1_EV_H1C_WAKE, h1c ? h1c->conn : NULL);

	if (h1c) {
		if (!expired) {
			TRACE_DEVEL("leaving (not expired)", H1_EV_H1C_WAKE, h1c->conn);
			return t;
		}

		/* We're about to destroy the connection, so make sure nobody attempts
		 * to steal it from us.
		 */
		HA_SPIN_LOCK(OTHER_LOCK, &idle_conns[tid].takeover_lock);

		/* Somebody already stole the connection from us, so we should not
		 * free it, we just have to free the task.
		 */
		if (!t->context)
			h1c = NULL;
		else if (h1c->conn->flags & CO_FL_LIST_MASK)
			MT_LIST_DEL(&h1c->conn->list);

		HA_SPIN_UNLOCK(OTHER_LOCK, &idle_conns[tid].takeover_lock);
	}

	task_destroy(t);

	if (!h1c) {
		/* resources were already deleted */
		TRACE_DEVEL("leaving (not more h1c)", H1_EV_H1C_WAKE);
		return NULL;
	}

	h1c->task = NULL;
	/* If a stream is still attached to the mux, just set an error and wait
	 * for the stream's timeout. Otherwise, release the mux. This is only ok
	 * because same timeouts are used.
	 */
	if (h1c->h1s && h1c->h1s->cs) {
		h1c->flags |= H1C_F_CS_ERROR;
		TRACE_STATE("error on h1c, h1s still attached (expired)", H1_EV_H1C_WAKE|H1_EV_H1C_ERR, h1c->conn, h1c->h1s);
	}
	else
		h1_release(h1c);

	return NULL;
}

/*******************************************/
/* functions below are used by the streams */
/*******************************************/

/*
 * Attach a new stream to a connection
 * (Used for outgoing connections)
 */
static struct conn_stream *h1_attach(struct connection *conn, struct session *sess)
{
	struct h1c *h1c = conn->ctx;
	struct conn_stream *cs = NULL;
	struct h1s *h1s;

	TRACE_ENTER(H1_EV_STRM_NEW, conn);
	if (h1c->flags & H1C_F_CS_ERROR) {
		TRACE_DEVEL("leaving on h1c error", H1_EV_STRM_NEW|H1_EV_STRM_END|H1_EV_STRM_ERR, conn);
		goto end;
	}

	cs = cs_new(h1c->conn);
	if (!cs) {
		TRACE_DEVEL("leaving on CS allocation failure", H1_EV_STRM_NEW|H1_EV_STRM_END|H1_EV_STRM_ERR, conn);
		goto end;
	}

	h1s = h1s_create(h1c, cs, sess);
	if (h1s == NULL) {
		TRACE_DEVEL("leaving on h1s creation failure", H1_EV_STRM_NEW|H1_EV_STRM_END|H1_EV_STRM_ERR, conn);
		goto end;
	}

	TRACE_LEAVE(H1_EV_STRM_NEW, conn, h1s);
	return cs;
  end:
	cs_free(cs);
	return NULL;
}

/* Retrieves a valid conn_stream from this connection, or returns NULL. For
 * this mux, it's easy as we can only store a single conn_stream.
 */
static const struct conn_stream *h1_get_first_cs(const struct connection *conn)
{
	struct h1c *h1c = conn->ctx;
	struct h1s *h1s = h1c->h1s;

	if (h1s)
		return h1s->cs;

	return NULL;
}

static void h1_destroy(void *ctx)
{
	struct h1c *h1c = ctx;

	TRACE_POINT(H1_EV_H1C_END, h1c->conn);
	if (!h1c->h1s || !h1c->conn || h1c->conn->ctx != h1c)
		h1_release(h1c);
}

/*
 * Detach the stream from the connection and possibly release the connection.
 */
static void h1_detach(struct conn_stream *cs)
{
	struct h1s *h1s = cs->ctx;
	struct h1c *h1c;
	struct session *sess;
	int is_not_first;

	TRACE_ENTER(H1_EV_STRM_END, h1s ? h1s->h1c->conn : NULL, h1s);

	cs->ctx = NULL;
	if (!h1s) {
		TRACE_LEAVE(H1_EV_STRM_END);
		return;
	}

	sess = h1s->sess;
	h1c = h1s->h1c;
	h1s->cs = NULL;

	is_not_first = h1s->flags & H1S_F_NOT_FIRST;
	h1s_destroy(h1s);

	if (conn_is_back(h1c->conn) && (h1c->flags & H1C_F_CS_IDLE)) {
		/* If there are any excess server data in the input buffer,
		 * release it and close the connection ASAP (some data may
		 * remain in the output buffer). This happens if a server sends
		 * invalid responses. So in such case, we don't want to reuse
		 * the connection
		 */
		if (b_data(&h1c->ibuf)) {
			h1_release_buf(h1c, &h1c->ibuf);
			h1c->flags = (h1c->flags & ~H1C_F_CS_IDLE) | H1C_F_CS_SHUTW_NOW;
			TRACE_DEVEL("remaining data on detach, kill connection", H1_EV_STRM_END|H1_EV_H1C_END);
			goto release;
		}

		/* Never ever allow to reuse a connection from a non-reuse backend */
		if ((h1c->px->options & PR_O_REUSE_MASK) == PR_O_REUSE_NEVR)
			h1c->conn->flags |= CO_FL_PRIVATE;

		if (!(h1c->conn->owner) && (h1c->conn->flags & CO_FL_PRIVATE)) {
			h1c->conn->owner = sess;
			if (!session_add_conn(sess, h1c->conn, h1c->conn->target)) {
				h1c->conn->owner = NULL;
				h1c->conn->mux->destroy(h1c);
				goto end;
			}
			if (session_check_idle_conn(sess, h1c->conn)) {
				/* The connection got destroyed, let's leave */
				TRACE_DEVEL("outgoing connection killed", H1_EV_STRM_END|H1_EV_H1C_END);
				goto end;
			}
		}
		if (!(h1c->conn->flags & CO_FL_PRIVATE)) {
			if (h1c->conn->owner == sess)
				h1c->conn->owner = NULL;
			h1c->conn->xprt->subscribe(h1c->conn, h1c->conn->xprt_ctx, SUB_RETRY_RECV, &h1c->wait_event);
			if (!srv_add_to_idle_list(objt_server(h1c->conn->target), h1c->conn, is_not_first)) {
				/* The server doesn't want it, let's kill the connection right away */
				h1c->conn->mux->destroy(h1c);
				TRACE_DEVEL("outgoing connection killed", H1_EV_STRM_END|H1_EV_H1C_END);
				goto end;
			}
			/* At this point, the connection has been added to the
			 * server idle list, so another thread may already have
			 * hijacked it, so we can't do anything with it.
			 */
			return;
		}
	}

  release:
	/* We don't want to close right now unless the connection is in error or shut down for writes */
	if ((h1c->flags & (H1C_F_CS_ERROR|H1C_F_CS_SHUTDOWN|H1C_F_UPG_H2C)) ||
	    (h1c->conn->flags & (CO_FL_ERROR|CO_FL_SOCK_WR_SH)) ||
	    ((h1c->flags & H1C_F_CS_SHUTW_NOW) && !b_data(&h1c->obuf)) ||
	    !h1c->conn->owner) {
		TRACE_DEVEL("killing dead connection", H1_EV_STRM_END, h1c->conn);
		h1_release(h1c);
	}
	else {
		/* If we have a new request, process it immediately */
		if (unlikely(b_data(&h1c->ibuf))) {
			if (h1_process(h1c) == -1)
				goto end;
		}
		else
			h1c->conn->xprt->subscribe(h1c->conn, h1c->conn->xprt_ctx, SUB_RETRY_RECV, &h1c->wait_event);
		h1_refresh_timeout(h1c);
	}
  end:
	TRACE_LEAVE(H1_EV_STRM_END);
}


static void h1_shutr(struct conn_stream *cs, enum cs_shr_mode mode)
{
	struct h1s *h1s = cs->ctx;
	struct h1c *h1c;

	if (!h1s)
		return;
	h1c = h1s->h1c;

	TRACE_ENTER(H1_EV_STRM_SHUT, h1c->conn, h1s);

	if (cs->flags & CS_FL_KILL_CONN) {
		TRACE_STATE("stream wants to kill the connection", H1_EV_STRM_SHUT, h1c->conn, h1s);
		goto do_shutr;
	}
	if (h1c->conn->flags & (CO_FL_ERROR | CO_FL_SOCK_RD_SH | CO_FL_SOCK_WR_SH)) {
		TRACE_STATE("shutdown on connection (error|rd_sh|wr_sh)", H1_EV_STRM_SHUT, h1c->conn, h1s);
		goto do_shutr;
	}

	if ((h1c->flags & H1C_F_UPG_H2C) || (h1s->flags & H1S_F_WANT_KAL)) {
		TRACE_STATE("keep connection alive (upg_h2c|want_kal)", H1_EV_STRM_SHUT, h1c->conn, h1s);
		goto end;
	}

  do_shutr:
	/* NOTE: Be sure to handle abort (cf. h2_shutr) */
	if (cs->flags & CS_FL_SHR)
		goto end;
	if (conn_xprt_ready(cs->conn) && cs->conn->xprt->shutr)
		cs->conn->xprt->shutr(cs->conn, cs->conn->xprt_ctx,
				      (mode == CS_SHR_DRAIN));
  end:
	TRACE_LEAVE(H1_EV_STRM_SHUT, h1c->conn, h1s);
}

static void h1_shutw(struct conn_stream *cs, enum cs_shw_mode mode)
{
	struct h1s *h1s = cs->ctx;
	struct h1c *h1c;

	if (!h1s)
		return;
	h1c = h1s->h1c;

	TRACE_ENTER(H1_EV_STRM_SHUT, h1c->conn, h1s);

	if (cs->flags & CS_FL_KILL_CONN) {
		TRACE_STATE("stream wants to kill the connection", H1_EV_STRM_SHUT, h1c->conn, h1s);
		goto do_shutw;
	}
	if (h1c->conn->flags & (CO_FL_ERROR | CO_FL_SOCK_RD_SH | CO_FL_SOCK_WR_SH)) {
		TRACE_STATE("shutdown on connection (error|rd_sh|wr_sh)", H1_EV_STRM_SHUT, h1c->conn, h1s);
		goto do_shutw;
	}

	if ((h1c->flags & H1C_F_UPG_H2C) ||
	    ((h1s->flags & H1S_F_WANT_KAL) && h1s->req.state == H1_MSG_DONE && h1s->res.state == H1_MSG_DONE)) {
		TRACE_STATE("keep connection alive (upg_h2c|want_kal)", H1_EV_STRM_SHUT, h1c->conn, h1s);
		goto end;
	}

  do_shutw:
	h1c->flags |= H1C_F_CS_SHUTW_NOW;
	if ((cs->flags & CS_FL_SHW) || b_data(&h1c->obuf))
		goto end;
	h1_shutw_conn(cs->conn, mode);
  end:
	TRACE_LEAVE(H1_EV_STRM_SHUT, h1c->conn, h1s);
}

static void h1_shutw_conn(struct connection *conn, enum cs_shw_mode mode)
{
	struct h1c *h1c = conn->ctx;

	if (conn->flags & CO_FL_SOCK_WR_SH)
		return;

	TRACE_ENTER(H1_EV_STRM_SHUT, conn, h1c->h1s);
	conn_xprt_shutw(conn);
	conn_sock_shutw(conn, (mode == CS_SHW_NORMAL));
	h1c->flags = (h1c->flags & ~H1C_F_CS_SHUTW_NOW) | H1C_F_CS_SHUTDOWN;
	TRACE_LEAVE(H1_EV_STRM_SHUT, conn, h1c->h1s);
}

/* Called from the upper layer, to unsubscribe <es> from events <event_type>
 * The <es> pointer is not allowed to differ from the one passed to the
 * subscribe() call. It always returns zero.
 */
static int h1_unsubscribe(struct conn_stream *cs, int event_type, struct wait_event *es)
{
	struct h1s *h1s = cs->ctx;

	if (!h1s)
		return 0;

	BUG_ON(event_type & ~(SUB_RETRY_SEND|SUB_RETRY_RECV));
	BUG_ON(h1s->subs && h1s->subs != es);

	es->events &= ~event_type;
	if (!es->events)
		h1s->subs = NULL;

	if (event_type & SUB_RETRY_RECV)
		TRACE_DEVEL("unsubscribe(recv)", H1_EV_STRM_RECV, h1s->h1c->conn, h1s);

	if (event_type & SUB_RETRY_SEND)
		TRACE_DEVEL("unsubscribe(send)", H1_EV_STRM_SEND, h1s->h1c->conn, h1s);

	return 0;
}

/* Called from the upper layer, to subscribe <es> to events <event_type>. The
 * event subscriber <es> is not allowed to change from a previous call as long
 * as at least one event is still subscribed. The <event_type> must only be a
 * combination of SUB_RETRY_RECV and SUB_RETRY_SEND. It always returns 0, unless
 * the conn_stream <cs> was already detached, in which case it will return -1.
 */
static int h1_subscribe(struct conn_stream *cs, int event_type, struct wait_event *es)
{
	struct h1s *h1s = cs->ctx;
	struct h1c *h1c;

	if (!h1s)
		return -1;

	BUG_ON(event_type & ~(SUB_RETRY_SEND|SUB_RETRY_RECV));
	BUG_ON(h1s->subs && h1s->subs != es);

	es->events |= event_type;
	h1s->subs = es;

	if (event_type & SUB_RETRY_RECV)
		TRACE_DEVEL("subscribe(recv)", H1_EV_STRM_RECV, h1s->h1c->conn, h1s);


	if (event_type & SUB_RETRY_SEND) {
		TRACE_DEVEL("subscribe(send)", H1_EV_STRM_SEND, h1s->h1c->conn, h1s);
		/*
		 * If the conn_stream attempt to subscribe, and the
		 * mux isn't subscribed to the connection, then it
		 * probably means the connection wasn't established
		 * yet, so we have to subscribe.
		 */
		h1c = h1s->h1c;
		if (!(h1c->wait_event.events & SUB_RETRY_SEND))
			h1c->conn->xprt->subscribe(h1c->conn,
						   h1c->conn->xprt_ctx,
						   SUB_RETRY_SEND,
						   &h1c->wait_event);
	}
	return 0;
}

/* Called from the upper layer, to receive data */
static size_t h1_rcv_buf(struct conn_stream *cs, struct buffer *buf, size_t count, int flags)
{
	struct h1s *h1s = cs->ctx;
	struct h1c *h1c = h1s->h1c;
	struct h1m *h1m = (!conn_is_back(cs->conn) ? &h1s->req : &h1s->res);
	size_t ret = 0;

	TRACE_ENTER(H1_EV_STRM_RECV, h1c->conn, h1s,, (size_t[]){count});
	if (!(h1c->flags & H1C_F_IN_ALLOC))
		ret = h1_process_input(h1c, buf, count);
	else
		TRACE_DEVEL("h1c ibuf not allocated", H1_EV_H1C_RECV|H1_EV_H1C_BLK, h1c->conn);

	if (flags & CO_RFL_BUF_FLUSH) {
		if (h1m->state == H1_MSG_TUNNEL || (h1m->state == H1_MSG_DATA && h1m->curr_len)) {
			h1s->flags |= H1S_F_BUF_FLUSH;
			TRACE_STATE("flush stream's buffer", H1_EV_STRM_RECV, h1c->conn, h1s);
		}
	}
	else {
		if (ret && h1s->flags & H1S_F_SPLICED_DATA) {
			h1s->flags &= ~H1S_F_SPLICED_DATA;
			TRACE_STATE("disable splicing", H1_EV_STRM_RECV, h1c->conn, h1s);
		}
		if (((flags & CO_RFL_KEEP_RECV) || (h1m->state != H1_MSG_DONE)) && !(h1c->wait_event.events & SUB_RETRY_RECV))
			h1c->conn->xprt->subscribe(h1c->conn, h1c->conn->xprt_ctx, SUB_RETRY_RECV, &h1c->wait_event);
	}
	TRACE_LEAVE(H1_EV_STRM_RECV, h1c->conn, h1s,, (size_t[]){ret});
	return ret;
}


/* Called from the upper layer, to send data */
static size_t h1_snd_buf(struct conn_stream *cs, struct buffer *buf, size_t count, int flags)
{
	struct h1s *h1s = cs->ctx;
	struct h1c *h1c;
	size_t total = 0;

	if (!h1s)
		return 0;
	h1c = h1s->h1c;

	TRACE_ENTER(H1_EV_STRM_SEND, h1c->conn, h1s,, (size_t[]){count});

	/* If we're not connected yet, or we're waiting for a handshake, stop
	 * now, as we don't want to remove everything from the channel buffer
	 * before we're sure we can send it.
	 */
	if (h1c->conn->flags & CO_FL_WAIT_XPRT) {
		TRACE_LEAVE(H1_EV_STRM_SEND, h1c->conn, h1s);
		return 0;
	}

	if (h1c->flags & H1C_F_CS_ERROR) {
		cs->flags |= CS_FL_ERROR;
		TRACE_DEVEL("H1 connection is in error, leaving in error", H1_EV_STRM_SEND|H1_EV_H1C_ERR|H1_EV_H1S_ERR|H1_EV_STRM_ERR, h1c->conn, h1s);
		return 0;
	}

	/* Inherit some flags from the upper layer */
	h1c->flags &= ~(H1C_F_CO_MSG_MORE|H1C_F_CO_STREAMER);
	if (flags & CO_SFL_MSG_MORE)
		h1c->flags |= H1C_F_CO_MSG_MORE;
	if (flags & CO_SFL_STREAMER)
		h1c->flags |= H1C_F_CO_STREAMER;

	while (count) {
		size_t ret = 0;

		if (!(h1c->flags & (H1C_F_OUT_FULL|H1C_F_OUT_ALLOC)))
			ret = h1_process_output(h1c, buf, count);
		else
			TRACE_DEVEL("h1c obuf not allocated", H1_EV_STRM_SEND|H1_EV_H1S_BLK, h1c->conn, h1s);

		if ((count - ret) > 0)
			h1c->flags |= H1C_F_CO_MSG_MORE;

		if (!ret)
			break;
		total += ret;
		count -= ret;
		if ((h1c->wait_event.events & SUB_RETRY_SEND) || !h1_send(h1c))
			break;
	}

	if (h1c->flags & H1C_F_CS_ERROR) {
		TRACE_DEVEL("reporting error to the app-layer stream", H1_EV_STRM_SEND|H1_EV_H1S_ERR|H1_EV_STRM_ERR, h1c->conn, h1s);
		cs->flags |= CS_FL_ERROR;
	}

	h1_refresh_timeout(h1c);
	TRACE_LEAVE(H1_EV_STRM_SEND, h1c->conn, h1s,, (size_t[]){total});
	return total;
}

#if defined(USE_LINUX_SPLICE)
/* Send and get, using splicing */
static int h1_rcv_pipe(struct conn_stream *cs, struct pipe *pipe, unsigned int count)
{
	struct h1s *h1s = cs->ctx;
	struct h1m *h1m = (!conn_is_back(cs->conn) ? &h1s->req : &h1s->res);
	int ret = 0;

	TRACE_ENTER(H1_EV_STRM_RECV, cs->conn, h1s,, (size_t[]){count});

	if ((h1m->flags & H1_MF_CHNK) || (h1m->state != H1_MSG_DATA && h1m->state != H1_MSG_TUNNEL)) {
		h1s->flags &= ~(H1S_F_BUF_FLUSH|H1S_F_SPLICED_DATA);
		TRACE_STATE("disable splicing on !(msg_data|msg_tunnel)", H1_EV_STRM_RECV, cs->conn, h1s);
		if (!(h1s->h1c->wait_event.events & SUB_RETRY_RECV)) {
			TRACE_STATE("restart receiving data, subscribing", H1_EV_STRM_RECV, cs->conn, h1s);
			cs->conn->xprt->subscribe(cs->conn, cs->conn->xprt_ctx, SUB_RETRY_RECV, &h1s->h1c->wait_event);
		}
		goto end;
	}

	if (h1s_data_pending(h1s)) {
		h1s->flags |= H1S_F_BUF_FLUSH;
		TRACE_STATE("flush input buffer before splicing", H1_EV_STRM_RECV, cs->conn, h1s);
		goto end;
	}

	if (!(h1s->flags & H1S_F_SPLICED_DATA)) {
		h1s->flags &= ~H1S_F_BUF_FLUSH;
		h1s->flags |= H1S_F_SPLICED_DATA;
		TRACE_STATE("enable splicing", H1_EV_STRM_RECV, cs->conn, h1s);
	}

	if (!h1_recv_allowed(h1s->h1c)) {
		TRACE_DEVEL("leaving on !recv_allowed", H1_EV_STRM_RECV, cs->conn, h1s);
		goto end;
	}

	if (h1m->state == H1_MSG_DATA && count > h1m->curr_len)
		count = h1m->curr_len;
	ret = cs->conn->xprt->rcv_pipe(cs->conn, cs->conn->xprt_ctx, pipe, count);
	if (h1m->state == H1_MSG_DATA && ret >= 0) {
		h1m->curr_len -= ret;
		if (!h1m->curr_len) {
			h1s->flags &= ~(H1S_F_BUF_FLUSH|H1S_F_SPLICED_DATA);
			TRACE_STATE("disable splicing on !curr_len", H1_EV_STRM_RECV, cs->conn, h1s);
		}
	}

  end:
	if (conn_xprt_read0_pending(cs->conn)) {
		h1s->flags |= H1S_F_REOS;
		h1s->flags &= ~(H1S_F_BUF_FLUSH|H1S_F_SPLICED_DATA);
		TRACE_STATE("read0 on connection", H1_EV_STRM_RECV, cs->conn, h1s);
	}

	if ((h1s->flags & H1S_F_REOS) ||
	    (h1m->state != H1_MSG_TUNNEL && h1m->state != H1_MSG_DATA) ||
	    (h1m->state == H1_MSG_DATA && !h1m->curr_len)) {
		TRACE_STATE("notify the mux can't use splicing anymore", H1_EV_STRM_RECV, h1s->h1c->conn, h1s);
		cs->flags &= ~CS_FL_MAY_SPLICE;
	}

	TRACE_LEAVE(H1_EV_STRM_RECV, cs->conn, h1s,, (size_t[]){ret});
	return ret;
}

static int h1_snd_pipe(struct conn_stream *cs, struct pipe *pipe)
{
	struct h1s *h1s = cs->ctx;
	int ret = 0;

	TRACE_ENTER(H1_EV_STRM_SEND, cs->conn, h1s,, (size_t[]){pipe->data});

	if (b_data(&h1s->h1c->obuf))
		goto end;

	ret = cs->conn->xprt->snd_pipe(cs->conn, cs->conn->xprt_ctx, pipe);
  end:
	if (pipe->data) {
		if (!(h1s->h1c->wait_event.events & SUB_RETRY_SEND)) {
			TRACE_STATE("more data to send, subscribing", H1_EV_STRM_SEND, cs->conn, h1s);
			cs->conn->xprt->subscribe(cs->conn, cs->conn->xprt_ctx, SUB_RETRY_SEND, &h1s->h1c->wait_event);
		}
	}

	TRACE_LEAVE(H1_EV_STRM_SEND, cs->conn, h1s,, (size_t[]){ret});
	return ret;
}
#endif

static int h1_ctl(struct connection *conn, enum mux_ctl_type mux_ctl, void *output)
{
	int ret = 0;
	switch (mux_ctl) {
	case MUX_STATUS:
		if (!(conn->flags & CO_FL_WAIT_XPRT))
			ret |= MUX_STATUS_READY;
		return ret;
	default:
		return -1;
	}
}

/* for debugging with CLI's "show fd" command */
static int h1_show_fd(struct buffer *msg, struct connection *conn)
{
	struct h1c *h1c = conn->ctx;
	struct h1s *h1s = h1c->h1s;
	int ret = 0;

	chunk_appendf(msg, " h1c.flg=0x%x .sub=%d .ibuf=%u@%p+%u/%u .obuf=%u@%p+%u/%u",
		      h1c->flags,  h1c->wait_event.events,
		      (unsigned int)b_data(&h1c->ibuf), b_orig(&h1c->ibuf),
		      (unsigned int)b_head_ofs(&h1c->ibuf), (unsigned int)b_size(&h1c->ibuf),
		       (unsigned int)b_data(&h1c->obuf), b_orig(&h1c->obuf),
		      (unsigned int)b_head_ofs(&h1c->obuf), (unsigned int)b_size(&h1c->obuf));

	if (h1s) {
		char *method;

		if (h1s->meth < HTTP_METH_OTHER)
			method = http_known_methods[h1s->meth].ptr;
		else
			method = "UNKNOWN";
		chunk_appendf(msg, " h1s=%p h1s.flg=0x%x .req.state=%s .res.state=%s"
		    " .meth=%s status=%d",
			      h1s, h1s->flags,
			      h1m_state_str(h1s->req.state),
			      h1m_state_str(h1s->res.state), method, h1s->status);
		if (h1s->cs)
			chunk_appendf(msg, " .cs.flg=0x%08x .cs.data=%p",
				      h1s->cs->flags, h1s->cs->data);

		chunk_appendf(&trash, " .subs=%p", h1s->subs);
		if (h1s->subs) {
			chunk_appendf(&trash, "(ev=%d tl=%p", h1s->subs->events, h1s->subs->tasklet);
			chunk_appendf(&trash, " tl.calls=%d tl.ctx=%p tl.fct=",
				      h1s->subs->tasklet->calls,
				      h1s->subs->tasklet->context);
			if (h1s->subs->tasklet->calls >= 1000000)
				ret = 1;
			resolve_sym_name(&trash, NULL, h1s->subs->tasklet->process);
			chunk_appendf(&trash, ")");
		}
	}
	return ret;
}


/* Add an entry in the headers map. Returns -1 on error and 0 on success. */
static int add_hdr_case_adjust(const char *from, const char *to, char **err)
{
	struct h1_hdr_entry *entry;

	/* Be sure there is a non-empty <to> */
	if (!strlen(to)) {
		memprintf(err, "expect <to>");
		return -1;
	}

	/* Be sure only the case differs between <from> and <to> */
	if (strcasecmp(from, to)) {
		memprintf(err, "<from> and <to> must not differ execpt the case");
		return -1;
	}

	/* Be sure <from> does not already existsin the tree */
	if (ebis_lookup(&hdrs_map.map, from)) {
		memprintf(err, "duplicate entry '%s'", from);
		return -1;
	}

	/* Create the entry and insert it in the tree */
	entry = malloc(sizeof(*entry));
	if (!entry) {
		memprintf(err, "out of memory");
		return -1;
	}

	entry->node.key = strdup(from);
	entry->name.ptr = strdup(to);
	entry->name.len = strlen(to);
	if (!entry->node.key || !entry->name.ptr) {
		free(entry->node.key);
		istfree(&entry->name);
		free(entry);
		memprintf(err, "out of memory");
		return -1;
	}
	ebis_insert(&hdrs_map.map, &entry->node);
	return 0;
}

/* Migrate the the connection to the current thread.
 * Return 0 if successful, non-zero otherwise.
 * Expected to be called with the old thread lock held.
 */
static int h1_takeover(struct connection *conn, int orig_tid)
{
	struct h1c *h1c = conn->ctx;
	struct task *task;

	if (fd_takeover(conn->handle.fd, conn) != 0)
		return -1;

	if (conn->xprt->takeover && conn->xprt->takeover(conn, conn->xprt_ctx, orig_tid) != 0) {
		/* We failed to takeover the xprt, even if the connection may
		 * still be valid, flag it as error'd, as we have already
		 * taken over the fd, and wake the tasklet, so that it will
		 * destroy it.
		 */
		conn->flags |= CO_FL_ERROR;
		tasklet_wakeup_on(h1c->wait_event.tasklet, orig_tid);
		return -1;
	}

	if (h1c->wait_event.events)
		h1c->conn->xprt->unsubscribe(h1c->conn, h1c->conn->xprt_ctx,
		    h1c->wait_event.events, &h1c->wait_event);
	/* To let the tasklet know it should free itself, and do nothing else,
	 * set its context to NULL.
	 */
	h1c->wait_event.tasklet->context = NULL;
	tasklet_wakeup_on(h1c->wait_event.tasklet, orig_tid);

	task = h1c->task;
	if (task) {
		task->context = NULL;
		h1c->task = NULL;
		__ha_barrier_store();
		task_kill(task);

		h1c->task = task_new(tid_bit);
		if (!h1c->task) {
			h1_release(h1c);
			return -1;
		}
		h1c->task->process = h1_timeout_task;
		h1c->task->context = h1c;
	}
	h1c->wait_event.tasklet = tasklet_new();
	if (!h1c->wait_event.tasklet) {
		h1_release(h1c);
		return -1;
	}
	h1c->wait_event.tasklet->process = h1_io_cb;
	h1c->wait_event.tasklet->context = h1c;
	h1c->conn->xprt->subscribe(h1c->conn, h1c->conn->xprt_ctx,
		                   SUB_RETRY_RECV, &h1c->wait_event);

	return 0;
}


static void h1_hdeaders_case_adjust_deinit()
{
	struct ebpt_node *node, *next;
	struct h1_hdr_entry *entry;

	node = ebpt_first(&hdrs_map.map);
	while (node) {
		next = ebpt_next(node);
		ebpt_delete(node);
		entry = container_of(node, struct h1_hdr_entry, node);
		free(entry->node.key);
		istfree(&entry->name);
		free(entry);
		node = next;
	}
	free(hdrs_map.name);
}

static int cfg_h1_headers_case_adjust_postparser()
{
	FILE *file = NULL;
	char *c, *key_beg, *key_end, *value_beg, *value_end;
	char *err;
	int rc, line = 0, err_code = 0;

	if (!hdrs_map.name)
		goto end;

	file = fopen(hdrs_map.name, "r");
	if (!file) {
		ha_alert("config : h1-outgoing-headers-case-adjust-file '%s': failed to open file.\n",
			 hdrs_map.name);
                err_code |= ERR_ALERT | ERR_FATAL;
		goto end;
	}

	/* now parse all lines. The file may contain only two header name per
	 * line, separated by spaces. All heading and trailing spaces will be
	 * ignored. Lines starting with a # are ignored.
	 */
	while (fgets(trash.area, trash.size, file) != NULL) {
		line++;
		c = trash.area;

		/* strip leading spaces and tabs */
		while (*c == ' ' || *c == '\t')
			c++;

		/* ignore emptu lines, or lines beginning with a dash */
		if (*c == '#' || *c == '\0' || *c == '\r' || *c == '\n')
			continue;

		/* look for the end of the key */
		key_beg = c;
		while (*c != '\0' && *c != ' ' && *c != '\t' && *c != '\n' && *c != '\r')
			c++;
		key_end = c;

		/* strip middle spaces and tabs */
		while (*c == ' ' || *c == '\t')
			c++;

		/* look for the end of the value, it is the end of the line */
		value_beg = c;
		while (*c && *c != '\n' && *c != '\r')
			c++;
		value_end = c;

		/* trim possibly trailing spaces and tabs */
		while (value_end > value_beg && (value_end[-1] == ' ' || value_end[-1] == '\t'))
			value_end--;

		/* set final \0 and check entries */
		*key_end = '\0';
		*value_end = '\0';

		err = NULL;
		rc = add_hdr_case_adjust(key_beg, value_beg, &err);
		if (rc < 0) {
			ha_alert("config : h1-outgoing-headers-case-adjust-file '%s' : %s at line %d.\n",
				 hdrs_map.name, err, line);
			err_code |= ERR_ALERT | ERR_FATAL;
			free(err);
			goto end;
		}
		if (rc > 0) {
			ha_warning("config : h1-outgoing-headers-case-adjust-file '%s' : %s at line %d.\n",
				   hdrs_map.name, err, line);
			err_code |= ERR_WARN;
			free(err);
		}
	}

  end:
	if (file)
		fclose(file);
	hap_register_post_deinit(h1_hdeaders_case_adjust_deinit);
	return err_code;
}


/* config parser for global "h1-outgoing-header-case-adjust" */
static int cfg_parse_h1_header_case_adjust(char **args, int section_type, struct proxy *curpx,
					   struct proxy *defpx, const char *file, int line,
					   char **err)
{
        if (too_many_args(2, args, err, NULL))
                return -1;
        if (!*(args[1]) || !*(args[2])) {
                memprintf(err, "'%s' expects <from> and <to> as argument.", args[0]);
		return -1;
	}
	return add_hdr_case_adjust(args[1], args[2], err);
}

/* config parser for global "h1-outgoing-headers-case-adjust-file" */
static int cfg_parse_h1_headers_case_adjust_file(char **args, int section_type, struct proxy *curpx,
						 struct proxy *defpx, const char *file, int line,
						 char **err)
{
        if (too_many_args(1, args, err, NULL))
                return -1;
        if (!*(args[1])) {
                memprintf(err, "'%s' expects <file> as argument.", args[0]);
		return -1;
	}
	free(hdrs_map.name);
	hdrs_map.name = strdup(args[1]);
        return 0;
}


/* config keyword parsers */
static struct cfg_kw_list cfg_kws = {{ }, {
		{ CFG_GLOBAL, "h1-case-adjust", cfg_parse_h1_header_case_adjust },
		{ CFG_GLOBAL, "h1-case-adjust-file", cfg_parse_h1_headers_case_adjust_file },
		{ 0, NULL, NULL },
	}
};

INITCALL1(STG_REGISTER, cfg_register_keywords, &cfg_kws);
REGISTER_CONFIG_POSTPARSER("h1-headers-map", cfg_h1_headers_case_adjust_postparser);


/****************************************/
/* MUX initialization and instantiation */
/****************************************/

/* The mux operations */
static const struct mux_ops mux_h1_ops = {
	.init        = h1_init,
	.wake        = h1_wake,
	.attach      = h1_attach,
	.get_first_cs = h1_get_first_cs,
	.get_cs_info = h1_get_cs_info,
	.detach      = h1_detach,
	.destroy     = h1_destroy,
	.avail_streams = h1_avail_streams,
	.used_streams = h1_used_streams,
	.rcv_buf     = h1_rcv_buf,
	.snd_buf     = h1_snd_buf,
#if defined(USE_LINUX_SPLICE)
	.rcv_pipe    = h1_rcv_pipe,
	.snd_pipe    = h1_snd_pipe,
#endif
	.subscribe   = h1_subscribe,
	.unsubscribe = h1_unsubscribe,
	.shutr       = h1_shutr,
	.shutw       = h1_shutw,
	.show_fd     = h1_show_fd,
	.reset       = h1_reset,
	.ctl         = h1_ctl,
	.takeover    = h1_takeover,
	.flags       = MX_FL_HTX,
	.name        = "H1",
};


/* this mux registers default HTX proto */
static struct mux_proto_list mux_proto_htx =
{ .token = IST(""), .mode = PROTO_MODE_HTTP, .side = PROTO_SIDE_BOTH, .mux = &mux_h1_ops };

INITCALL1(STG_REGISTER, register_mux_proto, &mux_proto_htx);

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
