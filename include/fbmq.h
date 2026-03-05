/*
 * fbmq.h — File-Based Message Queue
 *
 * A Unix-philosophy message queue where every message is a Markdown file
 * with RFC 822 headers, every queue is a directory, and atomic rename(2)
 * is the sole coordination primitive.
 *
 * Copyright (c) 2026. Released under the MIT License.
 */

#ifndef FBMQ_H
#define FBMQ_H

/* ── Shared library visibility ── */

#if defined(FBMQ_SHARED_BUILD)
  #define FBMQ_API __attribute__((visibility("default")))
#else
  #define FBMQ_API
#endif

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#define FBMQ_VERSION        "1.0.0"
#define FBMQ_ID_LEN         32       /* 32-char hex ID */
#define FBMQ_MAX_PATH       4096
#define FBMQ_DEFAULT_RETRIES    3
#define FBMQ_DEFAULT_LEASE      300  /* seconds */
#define FBMQ_DEFAULT_PURGE_AGE  604800 /* 7 days */
#define FBMQ_MAX_MESSAGE_SIZE   (64 * 1024 * 1024) /* 64 MiB */
#define FBMQ_SCAN_RETRIES       3
#define FBMQ_DEFAULT_MAX_PENDING 10000

/* ── Priority ── */

typedef enum {
    FBMQ_PRIO_CRITICAL = 0,
    FBMQ_PRIO_HIGH     = 1,
    FBMQ_PRIO_NORMAL   = 2,
    FBMQ_PRIO_LOW      = 3,
    FBMQ_PRIO_COUNT    = 4
} fbmq_priority_t;

/* ── Fsync mode ── */

typedef enum {
    FBMQ_FSYNC_FULL  = 0,  /* per-message file + dir fsync (default, zero-init safe) */
    FBMQ_FSYNC_BATCH = 1,  /* file fsync only; dir fsync deferred to fbmq_sync() */
    FBMQ_FSYNC_NONE  = 2   /* no fsync at all (tmpfs mode) */
} fbmq_fsync_mode_t;

FBMQ_API const char *fbmq_priority_str(fbmq_priority_t p);
FBMQ_API int fbmq_priority_parse(const char *s);

/* ── Message ── */

typedef struct {
    char     id[FBMQ_ID_LEN + 1];
    char     created_at[64];
    char     created_by[256];
    fbmq_priority_t priority;
    int      retry_count;
    int      ttl;               /* seconds, 0 = no expiry */
    char     tags[1024];        /* comma-separated list, e.g. "orders, processing" */
    char     correlation_id[256];
    char     custom[4096];      /* RFC 822 continuation lines for Custom: block */
} fbmq_header_t;

typedef struct {
    fbmq_header_t header;
    char    *body;              /* markdown body (heap-allocated) */
    size_t   body_len;
} fbmq_message_t;

FBMQ_API void fbmq_message_free(fbmq_message_t *msg);

/* ── Queue handle ──
 *
 * NOT thread-safe. Each thread must use its own fbmq_queue_t handle.
 *
 * Concurrent access across processes (each with their own handle)
 * is safe by design — coordination is via rename(2) only.
 */

typedef struct {
    char     root[FBMQ_MAX_PATH];
    int      max_retries;
    int      lease_timeout;          /* seconds */
    int      use_priority_dirs;
    fbmq_fsync_mode_t fsync_mode;    /* FULL (default), BATCH, or NONE */
    mode_t   file_mode;              /* default 0640 */
    mode_t   dir_mode;               /* default 0750 */
    int64_t  max_pending;            /* 0 = unlimited */
} fbmq_queue_t;

FBMQ_API void fbmq_queue_defaults(fbmq_queue_t *q);

/*
 * ── Core API ──
 *
 * Return convention: 0 = success, -1 = error (errno set).
 * fbmq_dequeue() is special: 0 = claimed, 1 = empty, -1 = error.
 * fbmq_reap/purge return count of affected messages, or -1 on error.
 */

FBMQ_API int fbmq_init(fbmq_queue_t *q, const char *root);
FBMQ_API int fbmq_enqueue(fbmq_queue_t *q, fbmq_message_t *msg);
FBMQ_API int fbmq_sync(fbmq_queue_t *q);

/*
 * Claim the next available message. Atomically moves from pending/ to
 * processing/. Writes the claimed path to claimed_path.
 * Returns 0=claimed, 1=empty, -1=error.
 *
 * FIX #1: pop only returns the claimed path. The message is already on
 * disk — read it with cat or fbmq_parse_file(). One tool, one output.
 */
FBMQ_API int fbmq_dequeue(fbmq_queue_t *q, char *claimed_path, size_t pathlen);

FBMQ_API int fbmq_complete(fbmq_queue_t *q, const char *claimed_path);
FBMQ_API int fbmq_fail(fbmq_queue_t *q, const char *claimed_path);
FBMQ_API int fbmq_depth(fbmq_queue_t *q, int64_t *depth);
FBMQ_API int fbmq_reap(fbmq_queue_t *q);
FBMQ_API int fbmq_reap_ttl(fbmq_queue_t *q);
FBMQ_API int fbmq_purge(fbmq_queue_t *q, int max_age_seconds);

/* ── Utilities ── */

FBMQ_API int  fbmq_generate_id(char *out);
FBMQ_API int fbmq_parse_file(const char *path, fbmq_message_t *msg);
FBMQ_API int fbmq_parse_headers(const char *path, fbmq_message_t *msg);

/*
 * FIX #8: Serialize to a single buffer, then write in one syscall.
 * Returns 0 on success. Caller must free(*out).
 */
FBMQ_API int fbmq_serialize(const fbmq_message_t *msg, char **out, size_t *outlen);

FBMQ_API int fbmq_fsync_fd(int fd);
FBMQ_API int fbmq_fsync_dir(const char *path);
FBMQ_API int fbmq_detect_priority(const char *root);

#endif /* FBMQ_H */
