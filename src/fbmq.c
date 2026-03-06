/*
 * fbmq.c — Core library implementation
 *
 */

#include "fbmq.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#include <time.h>
#include <limits.h>

#define FBMQ_HEADER_PEEK 4096  /* max bytes to peek for RFC 822 headers */

/* ────────────────────────────────────────────
 * Portable timegm: timegm(3) is not POSIX.
 * Use the native version on glibc/macOS, otherwise
 * fall back to the TZ manipulation trick.
 * ──────────────────────────────────────────── */

#if defined(__GLIBC__) || defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
static time_t portable_timegm(struct tm *tm) { return timegm(tm); }
#else
/* Pure calendar arithmetic fallback — no TZ manipulation, thread-safe. */
static time_t portable_timegm(struct tm *tm)
{
    static const int mdays[] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    int y = tm->tm_year + 1900;
    int m = tm->tm_mon;          /* 0-based */
    int d = tm->tm_mday;

    if (m < 0 || m > 11 || d < 1) return (time_t)-1;

    /* O(year-1970) — adequate for practical dates. */
    long days = 0;
    for (int i = 1970; i < y; i++) {
        days += 365;
        if ((i % 4 == 0 && i % 100 != 0) || i % 400 == 0) days++;
    }
    /* Days from start of year to start of month m */
    for (int i = 0; i < m; i++) {
        days += mdays[i];
        if (i == 1 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) days++;
    }
    days += d - 1;

    return (time_t)(days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec);
}
#endif

/* ────────────────────────────────────────────
 * Priority helpers
 * ──────────────────────────────────────────── */

static const char *prio_names[] = { "critical", "high", "normal", "low" };

const char *fbmq_priority_str(fbmq_priority_t p)
{
    if (p < FBMQ_PRIO_COUNT) return prio_names[p];
    return "normal";
}

int fbmq_priority_parse(const char *s)
{
    if (!s) return -1;
    if (strcmp(s, "critical") == 0) return FBMQ_PRIO_CRITICAL;
    if (strcmp(s, "high") == 0)     return FBMQ_PRIO_HIGH;
    if (strcmp(s, "normal") == 0)   return FBMQ_PRIO_NORMAL;
    if (strcmp(s, "low") == 0)      return FBMQ_PRIO_LOW;
    return -1;
}

/* ────────────────────────────────────────────
 * Queue defaults  (FIX #15: secure permissions)
 * ──────────────────────────────────────────── */

void fbmq_queue_defaults(fbmq_queue_t *q)
{
    memset(q, 0, sizeof(*q));
    q->max_retries   = FBMQ_DEFAULT_RETRIES;
    q->lease_timeout  = FBMQ_DEFAULT_LEASE;
    q->file_mode      = 0640;  /* rw-r----- */
    q->dir_mode       = 0750;  /* rwxr-x--- */
    q->max_pending    = FBMQ_DEFAULT_MAX_PENDING;
}

/* ────────────────────────────────────────────
 * Internal: mkdirp, fsync helpers
 * ──────────────────────────────────────────── */

static int mkdirp(const char *path, mode_t mode)
{
    char tmp[FBMQ_MAX_PATH];
    char *p;
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len > 0 && tmp[len - 1] == '/') tmp[len - 1] = '\0';

    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    return (mkdir(tmp, mode) == 0 || errno == EEXIST) ? 0 : -1;
}

/*
 * Safe path formatting: returns -1 and sets errno=ENAMETOOLONG on truncation.
 */
static int path_fmt(char *buf, size_t sz, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static int path_fmt(char *buf, size_t sz, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sz, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sz) { errno = ENAMETOOLONG; return -1; }
    return 0;
}

/* EINTR-safe, short-write-safe wrappers */
static ssize_t safe_write(int fd, const void *buf, size_t count)
{
    const unsigned char *p = buf;
    size_t remaining = count;
    while (remaining > 0) {
        ssize_t n = write(fd, p, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += n;
        remaining -= (size_t)n;
    }
    return (ssize_t)count;
}

static ssize_t safe_read(int fd, void *buf, size_t count)
{
    unsigned char *p = buf;
    size_t remaining = count;
    while (remaining > 0) {
        ssize_t n = read(fd, p, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) break;  /* EOF */
        p += n;
        remaining -= (size_t)n;
    }
    return (ssize_t)(count - remaining);
}

int fbmq_fsync_fd(int fd) { return fsync(fd); }

int fbmq_fsync_dir(const char *path)
{
#ifdef O_DIRECTORY
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
#else
    int fd = open(path, O_RDONLY | O_CLOEXEC);
#endif
    if (fd < 0) return -1;
    int rc = fsync(fd);
    close(fd);
    return rc;
}

/* ────────────────────────────────────────────
 * Internal: entropy  (FIX #14: getrandom with fallback)
 * ──────────────────────────────────────────── */

static int get_random_bytes(void *buf, size_t len)
{
#ifdef __APPLE__
    arc4random_buf(buf, len);
    return 0;
#else
    /* Try getrandom(2) first — works in containers/chroots.
     * Loop on short reads (can happen on signal interruption). */
    {
        unsigned char *p = buf;
        size_t remaining = len;
        while (remaining > 0) {
            long rc = syscall(SYS_getrandom, p, remaining, 0);
            if (rc < 0) {
                if (errno == EINTR) continue;
                break;          /* getrandom failed, fall through */
            }
            p += rc;
            remaining -= (size_t)rc;
        }
        if (remaining == 0) return 0;
    }

    /* Fallback: /dev/urandom — use safe_read() to handle short reads */
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ssize_t n = safe_read(fd, buf, len);
        close(fd);
        if (n == (ssize_t)len) return 0;
    }

    /* No strong entropy available — fail hard rather than risk ID collisions */
    return -1;
#endif
}

/* ────────────────────────────────────────────
 * Internal: RFC 822 header parser
 *
 * Handles:
 *  - Header-Name: value  (case-insensitive field names)
 *  - Comma-separated lists: Tags: a, b, c
 *  - Continuation lines under Custom: (lines starting with whitespace)
 *
 * Field names are normalized: lowercased, dashes replaced with underscores.
 * ──────────────────────────────────────────── */

/* Strip leading/trailing whitespace in-place, return new start */
static char *strip(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    size_t len = strlen(s);
    while (len > 0 && (s[len-1] == '\n' || s[len-1] == '\r' || s[len-1] == ' ' || s[len-1] == '\t'))
        s[--len] = '\0';
    return s;
}

/* Normalize RFC 822 field name: lowercase, replace '-' with '_' */
static void normalize_key(char *key)
{
    for (char *p = key; *p; p++) {
        if (*p >= 'A' && *p <= 'Z') *p = *p - 'A' + 'a';
        if (*p == '-') *p = '_';
    }
}

static int parse_headers(const char *header, size_t header_len,
                          fbmq_header_t *h)
{
    /* Work on a mutable copy */
    char *buf = strndup(header, header_len);
    if (!buf) return -1;

    int in_custom = 0;
    size_t custom_pos = 0;

    char *line = buf;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        /* RFC 822 continuation: indented line appends to custom block */
        if (in_custom && (line[0] == ' ' || line[0] == '\t')) {
            size_t ll = strlen(line);
            if (custom_pos + ll + 2 >= sizeof(h->custom)) {
                free(buf);
                errno = EINVAL;
                return -1;
            }
            memcpy(h->custom + custom_pos, line, ll);
            custom_pos += ll;
            h->custom[custom_pos++] = '\n';
            h->custom[custom_pos] = '\0';
            line = nl ? nl + 1 : NULL;
            continue;
        }
        in_custom = 0;

        /* Find colon separator */
        char *colon = strchr(line, ':');
        if (!colon) { line = nl ? nl + 1 : NULL; continue; }

        *colon = '\0';
        char *key = strip(line);
        char *val = strip(colon + 1);
        normalize_key(key);

        if (strcmp(key, "id") == 0) {
            /* ID must be exactly 32 hex characters */
            size_t vlen = strlen(val);
            if (vlen != FBMQ_ID_LEN) { free(buf); errno = EINVAL; return -1; }
            for (size_t k = 0; k < vlen; k++) {
                char c = val[k];
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                    free(buf);
                    errno = EINVAL;
                    return -1;
                }
            }
            snprintf(h->id, sizeof(h->id), "%s", val);
        } else if (strcmp(key, "created_at") == 0) {
            if (strlen(val) >= sizeof(h->created_at)) { free(buf); errno = EINVAL; return -1; }
            snprintf(h->created_at, sizeof(h->created_at), "%s", val);
        } else if (strcmp(key, "created_by") == 0) {
            if (strlen(val) >= sizeof(h->created_by)) { free(buf); errno = EINVAL; return -1; }
            snprintf(h->created_by, sizeof(h->created_by), "%s", val);
        } else if (strcmp(key, "priority") == 0) {
            int pp = fbmq_priority_parse(val);
            h->priority = (pp >= 0) ? (fbmq_priority_t)pp : FBMQ_PRIO_NORMAL;
        } else if (strcmp(key, "retry_count") == 0) {
            char *end;
            long v = strtol(val, &end, 10);
            if (end != val && v >= 0 && v <= 10000)
                h->retry_count = (int)v;
        } else if (strcmp(key, "ttl") == 0) {
            char *end;
            long v = strtol(val, &end, 10);
            if (end != val && v >= 0 && v <= INT_MAX)
                h->ttl = (int)v;
        } else if (strcmp(key, "correlation_id") == 0) {
            if (strlen(val) >= sizeof(h->correlation_id)) { free(buf); errno = EINVAL; return -1; }
            snprintf(h->correlation_id, sizeof(h->correlation_id), "%s", val);
        } else if (strcmp(key, "reply_to") == 0) {
            if (strlen(val) >= sizeof(h->reply_to)) { free(buf); errno = EINVAL; return -1; }
            snprintf(h->reply_to, sizeof(h->reply_to), "%s", val);
        } else if (strcmp(key, "tags") == 0) {
            if (strlen(val) >= sizeof(h->tags)) { free(buf); errno = EINVAL; return -1; }
            snprintf(h->tags, sizeof(h->tags), "%s", val);
        } else if (strcmp(key, "depends_on") == 0) {
            if (strlen(val) >= sizeof(h->depends_on)) { free(buf); errno = EINVAL; return -1; }
            snprintf(h->depends_on, sizeof(h->depends_on), "%s", val);
        } else if (strcmp(key, "custom") == 0) {
            in_custom = 1;  /* next indented lines go to custom block */
        }

        line = nl ? nl + 1 : NULL;
    }

    free(buf);
    return 0;
}

/* ────────────────────────────────────────────
 * ID generation
 * ──────────────────────────────────────────── */

int fbmq_generate_id(char *out)
{
    unsigned char rnd[16];
    if (get_random_bytes(rnd, sizeof(rnd)) != 0) return -1;

    for (int i = 0; i < 16; i++)
        snprintf(out + i * 2, 3, "%02x", rnd[i]);
    out[FBMQ_ID_LEN] = '\0';
    return 0;
}

/* ────────────────────────────────────────────
 * Pending path helper — single source of truth for pending directory layout.
 * If prio < 0 or queue has no priority dirs, returns "<root>/pending".
 * ──────────────────────────────────────────── */

static int pending_path(const fbmq_queue_t *q, int prio,
                        char *buf, size_t buflen)
{
    if (q->use_priority_dirs && prio >= 0)
        return path_fmt(buf, buflen, "%s/pending/%d-%s",
                        q->root, prio, fbmq_priority_str(prio));
    return path_fmt(buf, buflen, "%s/pending", q->root);
}

/* Iterate over pending directory paths (flat or 4 priority subdirs).
 * Body executes once per pending dir with _buf holding the path.
 * Intentionally skips dirs where pending_path() fails (path too long) —
 * callers like reap_ttl and sync tolerate partial iteration since failing
 * to process one priority level shouldn't abort the whole operation. */
#define FOR_EACH_PENDING(_q, _pvar, _buf, _bufsz)                        \
    for (int _pvar = (_q)->use_priority_dirs ? 0 : -1,                    \
             _pvar##_end = (_q)->use_priority_dirs ? FBMQ_PRIO_COUNT : 0; \
         _pvar < _pvar##_end; _pvar++)                                    \
        if (pending_path((_q), _pvar, (_buf), (_bufsz)) == 0)

/* Parse an ISO 8601 Created-At string into seconds + nanoseconds.
 * Returns 0 on success, -1 on parse failure. */
static int parse_created_at(const char *created_at, time_t *sec_out, long *nsec_out)
{
    *sec_out = 0;
    *nsec_out = 0;
    if (!created_at || !created_at[0]) return -1;

    struct tm tm = {0};
    char *sp = strptime(created_at, "%Y-%m-%dT%H:%M:%S", &tm);
    if (!sp) return -1;

    time_t t = portable_timegm(&tm);
    if (t == (time_t)-1) return -1;

    long nsec = 0;
    if (*sp == '.') {
        sp++;
        char frac[10] = "000000000";
        int fi = 0;
        while (*sp >= '0' && *sp <= '9' && fi < 9)
            frac[fi++] = *sp++;
        nsec = strtol(frac, NULL, 10);
    }

    *sec_out = t;
    *nsec_out = nsec;
    return 0;
}

/* ────────────────────────────────────────────
 * Queue initialization
 * ──────────────────────────────────────────── */

int fbmq_detect_priority(const char *root)
{
    char path[FBMQ_MAX_PATH];
    struct stat st;
    fbmq_queue_t probe = { .use_priority_dirs = 1 };
    if (path_fmt(probe.root, sizeof(probe.root), "%s", root) != 0) return 0;
    if (pending_path(&probe, FBMQ_PRIO_NORMAL, path, sizeof(path)) != 0)
        return 0;
    return (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

int fbmq_init(fbmq_queue_t *q, const char *root)
{
    if (path_fmt(q->root, sizeof(q->root), "%s", root) != 0) return -1;
    mode_t dm = q->dir_mode ? q->dir_mode : 0750;

    char path[FBMQ_MAX_PATH];

    /* Must fail hard if any pending path can't be created */
    {
        int pstart = q->use_priority_dirs ? 0 : -1;
        int pend   = q->use_priority_dirs ? FBMQ_PRIO_COUNT : 0;
        for (int p = pstart; p < pend; p++) {
            if (pending_path(q, p, path, sizeof(path)) != 0) return -1;
            if (mkdirp(path, dm) != 0) return -1;
        }
    }

    const char *subdirs[] = { "processing", "done", "failed", ".tmp", ".meta" };
    for (int i = 0; i < (int)(sizeof(subdirs)/sizeof(subdirs[0])); i++) {
        if (path_fmt(path, sizeof(path), "%s/%s", root, subdirs[i]) != 0) return -1;
        if (mkdirp(path, dm) != 0) return -1;
    }

    /* Persist max_pending */
    if (path_fmt(path, sizeof(path), "%s/.meta/max_pending", root) != 0) return -1;
    {
        mode_t fm = q->file_mode;
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, fm);
        if (fd < 0) return -1;
        char buf[32];
        int n = snprintf(buf, sizeof(buf), "%lld\n", (long long)q->max_pending);
        if (n <= 0 || (size_t)n >= sizeof(buf)) { close(fd); return -1; }
        if (safe_write(fd, buf, (size_t)n) < 0) {
            close(fd);
            return -1;
        }
        close(fd);
    }

    /* Fsync all created directories so the queue structure is durable */
    if (q->fsync_mode != FBMQ_FSYNC_NONE) {
        FOR_EACH_PENDING(q, p2, path, sizeof(path)) {
            if (fbmq_fsync_dir(path) != 0) return -1;
        }
        for (int i = 0; i < (int)(sizeof(subdirs)/sizeof(subdirs[0])); i++) {
            if (path_fmt(path, sizeof(path), "%s/%s", root, subdirs[i]) == 0) {
                if (fbmq_fsync_dir(path) != 0) return -1;
            }
        }
        if (fbmq_fsync_dir(root) != 0) return -1;
    }

    return 0;
}

/* ────────────────────────────────────────────
 * Depth counter (stateless scan)
 *
 * Count-on-read: scan pending/ (flat or 4 priority subdirs) plus processing/.
 * ──────────────────────────────────────────── */

/* Count .md files in a single directory.  Stop early if limit > 0
 * and count reaches limit (used by max_pending check to avoid scanning
 * the entire directory when we only need a threshold answer). */
static int64_t count_md_files(const char *dir, int64_t limit)
{
    /* Note: opendir() does not support O_CLOEXEC portably. The
     * open()+fdopendir() pattern is Linux-specific and would break macOS. */
    DIR *d = opendir(dir);
    if (!d) {
        /* ENOENT is normal (dir may not exist yet); other errors (EMFILE,
         * EACCES) must propagate to prevent silent max_pending bypass. */
        return (errno == ENOENT) ? 0 : -1;
    }

    int64_t count = 0;
    struct dirent *ent;
    errno = 0;
    while ((ent = readdir(d)) != NULL) {
        size_t nlen = strlen(ent->d_name);
        if (nlen > 3 && strcmp(ent->d_name + nlen - 3, ".md") == 0) {
            count++;
            if (limit > 0 && count >= limit) break;
        }
    }
    if (!ent && errno) { int e = errno; closedir(d); errno = e; return -1; }
    if (closedir(d) != 0) return -1;
    return count;
}

/* Count pending .md files (excluding processing).
 * limit=0 means count all; limit>0 stops early once count >= limit.
 * Returns -1 on error (pending_path failure). */
static int64_t count_pending(fbmq_queue_t *q, int64_t limit)
{
    char dir[FBMQ_MAX_PATH];
    int64_t count = 0;

    int pstart = q->use_priority_dirs ? 0 : -1;
    int pend   = q->use_priority_dirs ? FBMQ_PRIO_COUNT : 0;
    for (int p = pstart; p < pend; p++) {
        if (pending_path(q, p, dir, sizeof(dir)) != 0)
            return -1;
        if (limit > 0 && count >= limit) break;
        int64_t remaining = (limit > 0) ? limit - count : 0;
        int64_t n = count_md_files(dir, remaining);
        if (n < 0) return -1;
        count += n;
    }
    return count;
}

int fbmq_depth(fbmq_queue_t *q, int64_t *depth)
{
    /* Verify queue root exists */
    struct stat root_st;
    if (stat(q->root, &root_st) != 0) return -1;

    int64_t pending = count_pending(q, 0);
    if (pending < 0) return -1;

    /* Exact count of processing/ */
    char proc_dir[FBMQ_MAX_PATH];
    if (path_fmt(proc_dir, sizeof(proc_dir), "%s/processing", q->root) != 0)
        return -1;
    int64_t processing = count_md_files(proc_dir, 0);
    if (processing < 0) return -1;

    *depth = pending + processing;
    return 0;
}

/* Forward declarations needed by fbmq_list_ready */
static char **collect_md_entries(const char *dir, int *count_out);
static void free_entries(char **entries, int count);

/* ────────────────────────────────────────────
 * Ready-list: pending messages with all deps satisfied
 * ──────────────────────────────────────────── */

char **fbmq_list_ready(fbmq_queue_t *q, int *count)
{
    *count = 0;
    int cap = 64;
    char **ids = malloc((size_t)cap * sizeof(char *));
    if (!ids) { *count = -1; return NULL; }

    char pdir[FBMQ_MAX_PATH];
    FOR_EACH_PENDING(q, p, pdir, sizeof(pdir)) {
        int entry_count = 0;
        char **entries = collect_md_entries(pdir, &entry_count);
        if (!entries) continue;

        for (int i = 0; i < entry_count; i++) {
            char filepath[FBMQ_MAX_PATH];
            if (path_fmt(filepath, sizeof(filepath), "%s/%s", pdir, entries[i]) != 0)
                continue;

            fbmq_message_t msg = {0};
            if (fbmq_parse_headers(filepath, &msg) != 0) {
                fbmq_message_free(&msg);
                continue;
            }

            int ready = 1;
            if (msg.header.depends_on[0]) {
                char deps[sizeof(msg.header.depends_on)];
                snprintf(deps, sizeof(deps), "%s", msg.header.depends_on);
                char *saveptr = NULL;
                for (char *tok = strtok_r(deps, ",", &saveptr);
                     tok; tok = strtok_r(NULL, ",", &saveptr)) {
                    tok = strip(tok);
                    if (!*tok) continue;

                    char done_path[FBMQ_MAX_PATH];
                    if (path_fmt(done_path, sizeof(done_path),
                                 "%s/done/%s.md", q->root, tok) != 0) {
                        ready = 0; break;
                    }
                    struct stat st;
                    if (stat(done_path, &st) != 0) {
                        ready = 0; break;
                    }
                }
            }

            if (ready) {
                if (*count >= cap) {
                    cap *= 2;
                    char **tmp = realloc(ids, (size_t)cap * sizeof(char *));
                    if (!tmp) {
                        fbmq_message_free(&msg);
                        free_entries(entries, entry_count);
                        for (int j = 0; j < *count; j++) free(ids[j]);
                        free(ids);
                        *count = -1;
                        return NULL;
                    }
                    ids = tmp;
                }
                ids[*count] = strdup(msg.header.id);
                if (!ids[*count]) {
                    fbmq_message_free(&msg);
                    free_entries(entries, entry_count);
                    for (int j = 0; j < *count; j++) free(ids[j]);
                    free(ids);
                    *count = -1;
                    return NULL;
                }
                (*count)++;
            }
            fbmq_message_free(&msg);
        }
        free_entries(entries, entry_count);
    }

    if (*count == 0) {
        free(ids);
        return NULL;
    }
    return ids;
}

void fbmq_free_id_list(char **ids, int count)
{
    free_entries(ids, count);
}

/* ────────────────────────────────────────────
 * Serialization  (FIX #8: single buffer, single write)
 * ──────────────────────────────────────────── */

/* Dynamic string buffer */
typedef struct {
    char   *data;
    size_t  len;
    size_t  cap;
} strbuf_t;

static int sb_init(strbuf_t *sb)
{
    sb->cap = 4096;
    sb->data = malloc(sb->cap);
    sb->len = 0;
    if (!sb->data) return -1;
    sb->data[0] = '\0';
    return 0;
}

static void sb_append(strbuf_t *sb, const char *s, size_t slen)
{
    if (!sb->data) return;
    while (sb->len + slen + 1 > sb->cap) {
        if (sb->cap > SIZE_MAX / 2) { free(sb->data); sb->data = NULL; return; }
        sb->cap *= 2;
        char *tmp = realloc(sb->data, sb->cap);
        if (!tmp) { free(sb->data); sb->data = NULL; return; }
        sb->data = tmp;
    }
    memcpy(sb->data + sb->len, s, slen);
    sb->len += slen;
    sb->data[sb->len] = '\0';
}

static void sb_printf(strbuf_t *sb, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void sb_printf(strbuf_t *sb, const char *fmt, ...)
{
    if (!sb->data) return;
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n <= 0) { va_end(ap2); return; }

    /* Ensure enough space in the buffer directly */
    while (sb->len + (size_t)n + 1 > sb->cap) {
        if (sb->cap > SIZE_MAX / 2) { free(sb->data); sb->data = NULL; va_end(ap2); return; }
        sb->cap *= 2;
        char *tmp = realloc(sb->data, sb->cap);
        if (!tmp) { free(sb->data); sb->data = NULL; va_end(ap2); return; }
        sb->data = tmp;
    }
    vsnprintf(sb->data + sb->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    sb->len += (size_t)n;
}

int fbmq_serialize(const fbmq_message_t *msg, char **out, size_t *outlen)
{
    const fbmq_header_t *h = &msg->header;
    strbuf_t sb;
    if (sb_init(&sb) != 0) return -1;

    sb_printf(&sb, "Id: %s\n", h->id);
    sb_printf(&sb, "Created-At: %s\n", h->created_at);
    sb_printf(&sb, "Created-By: %s\n", h->created_by);
    sb_printf(&sb, "Priority: %s\n", fbmq_priority_str(h->priority));
    sb_printf(&sb, "Retry-Count: %d\n", h->retry_count);

    if (h->ttl > 0)
        sb_printf(&sb, "TTL: %d\n", h->ttl);
    if (h->tags[0])
        sb_printf(&sb, "Tags: %s\n", h->tags);
    if (h->correlation_id[0])
        sb_printf(&sb, "Correlation-Id: %s\n", h->correlation_id);
    if (h->reply_to[0])
        sb_printf(&sb, "Reply-To: %s\n", h->reply_to);
    if (h->depends_on[0])
        sb_printf(&sb, "Depends-On: %s\n", h->depends_on);
    if (h->custom[0]) {
        /* Validate custom block: every line must be indented (continuation
         * line) or empty, to prevent header injection via the C API. */
        const char *cp = h->custom;
        int custom_valid = 1;
        while (*cp) {
            if (*cp != ' ' && *cp != '\t' && *cp != '\n') {
                custom_valid = 0;
                break;
            }
            /* Skip to next line */
            const char *eol = strchr(cp, '\n');
            if (!eol) break; /* last line without newline — already validated above */
            cp = eol + 1;
        }
        if (!custom_valid) {
            free(sb.data);
            errno = EINVAL;
            return -1;
        }
        sb_printf(&sb, "Custom:\n%s", h->custom);
    }

    sb_printf(&sb, "\n");  /* blank line separates headers from body */

    /* Check for OOM before appending potentially large body */
    if (!sb.data) { errno = ENOMEM; return -1; }

    if (msg->body && msg->body_len > 0)
        sb_append(&sb, msg->body, msg->body_len);

    if (!sb.data) { errno = ENOMEM; return -1; }

    *out = sb.data;
    *outlen = sb.len;
    return 0;
}

/* ────────────────────────────────────────────
 * Message parsing  (RFC 822 headers)
 *
 * Headers end at the first blank line (\n\n).
 * A file is recognized as having headers when the first line
 * matches the RFC 822 field pattern (no spaces before the colon).
 * ──────────────────────────────────────────── */

/*
 * Internal parser: shared by fbmq_parse_file() and fbmq_parse_headers().
 * When headers_only is true, the body is not read or allocated — only
 * msg->body_len is set (from file size math) so callers like inspect
 * can still report body size.
 */
static int parse_file_internal(const char *path, fbmq_message_t *msg,
                               int headers_only)
{
    memset(msg, 0, sizeof(*msg));

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;

    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return -1; }

    size_t filesz = (size_t)st.st_size;
    if (filesz > FBMQ_MAX_MESSAGE_SIZE) { close(fd); errno = EFBIG; return -1; }

    /*
     * Read only the first 4KB to find and parse RFC 822 headers.
     * Only allocate heap memory for the body portion.
     * Falls back to full-file read if headers exceed 4KB.
     */
    size_t peek = filesz < FBMQ_HEADER_PEEK ? filesz : FBMQ_HEADER_PEEK;
    char hdr_buf[FBMQ_HEADER_PEEK + 1];

    ssize_t n = safe_read(fd, hdr_buf, peek);
    if (n < 0) { close(fd); return -1; }
    hdr_buf[n] = '\0';

    /* Detect RFC 822 headers: first line must have a colon with no
     * preceding whitespace (field-name ":" field-body). */
    int has_headers = 0;
    if (n > 0 && ((hdr_buf[0] >= 'A' && hdr_buf[0] <= 'Z') ||
                   (hdr_buf[0] >= 'a' && hdr_buf[0] <= 'z'))) {
        char *first_nl = memchr(hdr_buf, '\n', (size_t)n);
        size_t line_len = first_nl ? (size_t)(first_nl - hdr_buf) : (size_t)n;
        char *first_colon = memchr(hdr_buf, ':', line_len);
        if (first_colon && first_colon > hdr_buf) {
            has_headers = 1;
            for (char *p = hdr_buf; p < first_colon; p++) {
                if (*p == ' ' || *p == '\t') { has_headers = 0; break; }
            }
        }
    }

    /* No RFC 822 headers — entire file is body */
    if (!has_headers) {
        if (headers_only) {
            msg->body_len = filesz;
            close(fd);
            return 0;
        }
        if (lseek(fd, 0, SEEK_SET) == (off_t)-1) { close(fd); return -1; }
        char *body = malloc(filesz + 1);
        if (!body) { close(fd); return -1; }
        ssize_t br = safe_read(fd, body, filesz);
        close(fd);
        if (br < 0) { free(body); return -1; }
        body[br] = '\0';
        msg->body = body;
        msg->body_len = (size_t)br;
        return 0;
    }

    /* Find end of headers: blank line (\n\n) */
    char *end = strstr(hdr_buf, "\n\n");
    if (!end) {
        /* Headers might exceed 4KB — fall back to full read */
        char *data = malloc(filesz + 1);
        if (!data) { close(fd); return -1; }
        memcpy(data, hdr_buf, (size_t)n);
        if ((size_t)n < filesz) {
            ssize_t rest = safe_read(fd, data + n, filesz - (size_t)n);
            if (rest < 0) { close(fd); free(data); return -1; }
            n += rest;
        }
        close(fd);
        data[n] = '\0';

        end = strstr(data, "\n\n");
        if (!end) {
            /* No blank line — entire file is headers, no body */
            if (parse_headers(data, (size_t)n, &msg->header) != 0) {
                free(data);
                return -1;
            }
            free(data);
            return 0;
        }

        size_t header_len = (size_t)(end - data);
        if (parse_headers(data, header_len, &msg->header) != 0) {
            free(data);
            return -1;
        }

        if (headers_only) {
            char *body_start = end + 2;
            msg->body_len = (size_t)(n - (body_start - data));
            free(data);
            return 0;
        }

        char *body_start = end + 2;
        size_t body_len = (size_t)(n - (body_start - data));
        msg->body = malloc(body_len + 1);
        if (!msg->body) { free(data); return -1; }
        memcpy(msg->body, body_start, body_len);
        msg->body[body_len] = '\0';
        msg->body_len = body_len;
        free(data);
        return 0;
    }

    /* Fast path: headers fit within first 4KB peek */
    size_t header_len = (size_t)(end - hdr_buf);
    if (parse_headers(hdr_buf, header_len, &msg->header) != 0) {
        close(fd);
        return -1;
    }

    size_t body_offset = (size_t)((end - hdr_buf) + 2);
    size_t body_len = filesz > body_offset ? filesz - body_offset : 0;

    if (headers_only) {
        msg->body_len = body_len;
        close(fd);
        return 0;
    }

    if (body_len > 0) {
        msg->body = malloc(body_len + 1);
        if (!msg->body) { close(fd); return -1; }
        size_t already = (size_t)n > body_offset ? (size_t)n - body_offset : 0;
        if (already > body_len) already = body_len;
        if (already > 0)
            memcpy(msg->body, hdr_buf + body_offset, already);
        if (already < body_len) {
            ssize_t br = safe_read(fd, msg->body + already, body_len - already);
            if (br < 0) { close(fd); free(msg->body); msg->body = NULL; return -1; }
        }
        msg->body[body_len] = '\0';
        msg->body_len = body_len;
    }

    close(fd);
    return 0;
}

int fbmq_parse_file(const char *path, fbmq_message_t *msg)
{
    return parse_file_internal(path, msg, 0);
}

int fbmq_parse_headers(const char *path, fbmq_message_t *msg)
{
    return parse_file_internal(path, msg, 1);
}

void fbmq_message_free(fbmq_message_t *msg)
{
    free(msg->body);
    msg->body = NULL;
    msg->body_len = 0;
}

/* ────────────────────────────────────────────
 * Internal: atomic write to .tmp/
 *
 * Writes buf to tmp_path atomically: create, write, optional fsync, close.
 * On failure, unlinks tmp_path and returns -1.
 * ──────────────────────────────────────────── */

static int write_tmp(const fbmq_queue_t *q, const char *tmp_path,
                     const char *buf, size_t buflen)
{
    mode_t fm = q->file_mode;
    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, fm);
    if (fd < 0) return -1;

    /* safe_write guarantees full write or -1; no partial-write check needed */
    if (safe_write(fd, buf, buflen) < 0) {
        close(fd); unlink(tmp_path); return -1;
    }

    if (q->fsync_mode != FBMQ_FSYNC_NONE) {
        if (fsync(fd) != 0) { close(fd); unlink(tmp_path); return -1; }
    }
    close(fd);
    return 0;
}

/* ────────────────────────────────────────────
 * Enqueue  (FIX #8: single write via serialize)
 * ──────────────────────────────────────────── */

int fbmq_enqueue(fbmq_queue_t *q, fbmq_message_t *msg)
{
    /* Generate ID */
    if (fbmq_generate_id(msg->header.id) != 0)
        return -1;

    /* Fill created_at if empty */
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return -1;
    if (!msg->header.created_at[0]) {
        struct tm tm;
        gmtime_r(&ts.tv_sec, &tm);
        snprintf(msg->header.created_at, sizeof(msg->header.created_at),
                 "%04d-%02d-%02dT%02d:%02d:%02d.%09ldZ",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec, (long)ts.tv_nsec);
    }

    /* Fill created_by if empty */
    if (!msg->header.created_by[0]) {
        /* Zero-init guarantees null-termination: POSIX doesn't null-terminate
         * gethostname() output when the hostname is exactly sizeof(buf)-1 bytes. */
        char hostname[128] = {0};
        gethostname(hostname, sizeof(hostname) - 1);
        snprintf(msg->header.created_by, sizeof(msg->header.created_by),
                 "%d@%s", (int)getpid(), hostname);
    }

    /* Max pending check — counts both pending/ and processing/ to match
     * fbmq_depth() semantics. Prevents bypassing the limit by leaving
     * messages in processing without acking. Soft limit: under concurrent
     * push, up to N concurrent pushers can overshoot by N. See fbmq-design(7). */
    if (q->max_pending > 0) {
        int64_t pending = count_pending(q, q->max_pending);
        if (pending < 0) return -1;
        if (pending < q->max_pending) {
            char proc_dir[FBMQ_MAX_PATH];
            if (path_fmt(proc_dir, sizeof(proc_dir), "%s/processing", q->root) != 0)
                return -1;
            int64_t processing = count_md_files(proc_dir, q->max_pending - pending);
            if (processing < 0) return -1;
            pending += processing;
        }
        if (pending >= q->max_pending) {
            errno = ENOSPC;
            return -1;
        }
    }

    /* Serialize to buffer */
    char *buf = NULL;
    size_t buflen = 0;
    if (fbmq_serialize(msg, &buf, &buflen) != 0) return -1;

    /* Write to temp file in a single write(2) */
    char tmp_path[FBMQ_MAX_PATH];
    if (path_fmt(tmp_path, sizeof(tmp_path), "%s/.tmp/%s.md", q->root, msg->header.id) != 0) {
        free(buf); return -1;
    }

    if (write_tmp(q, tmp_path, buf, buflen) != 0) {
        free(buf); return -1;
    }
    free(buf);

    /* Atomic rename into pending — prefix with nanosecond timestamp */
    char pdir[FBMQ_MAX_PATH], dest[FBMQ_MAX_PATH];
    if (pending_path(q, (int)msg->header.priority, pdir, sizeof(pdir)) != 0) {
        unlink(tmp_path); return -1;
    }
    if (path_fmt(dest, sizeof(dest), "%s/%ld%09ld.%s.md",
                 pdir, (long)ts.tv_sec, (long)ts.tv_nsec, msg->header.id) != 0) {
        unlink(tmp_path); return -1;
    }

    if (rename(tmp_path, dest) != 0) {
        unlink(tmp_path);
        return -1;
    }

    if (q->fsync_mode == FBMQ_FSYNC_FULL)
        fbmq_fsync_dir(pdir);

    return 0;
}

/*
 * Flush deferred directory fsyncs. In BATCH mode, file data is already
 * durable but directory entries are not. This iterates all bucket dirs
 * plus processing/ and failed/, issuing fsync_dir on each. The kernel
 * coalesces these into a single journal transaction.
 */
int fbmq_sync(fbmq_queue_t *q)
{
    if (q->fsync_mode == FBMQ_FSYNC_NONE)
        return 0;

    char dir[FBMQ_MAX_PATH];
    int err = 0;

    FOR_EACH_PENDING(q, p, dir, sizeof(dir)) {
        if (fbmq_fsync_dir(dir) != 0) err = errno;
    } else {
        fprintf(stderr, "fbmq: sync: pending path too long (priority %d)\n", p);
    }

    if (path_fmt(dir, sizeof(dir), "%s/processing", q->root) == 0) {
        if (fbmq_fsync_dir(dir) != 0) err = errno;
    }
    if (path_fmt(dir, sizeof(dir), "%s/failed", q->root) == 0) {
        if (fbmq_fsync_dir(dir) != 0) err = errno;
    }

    if (err) { errno = err; return -1; }
    return 0;
}

/* ────────────────────────────────────────────
 * Dequeue  (FIX #1: only returns claimed path)
 *          (FIX #19: deduplicated claim logic)
 * ──────────────────────────────────────────── */

static char **collect_md_entries(const char *dir, int *count_out);
static void free_entries(char **entries, int count);

static int cmp_entry_name(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/*
 * Scan a single directory for the oldest .md file and try to claim it.
 * Returns: 0=claimed, 1=nothing found, -1=error.
 *
 * Collects all .md entries, sorts by filename (which embeds a fixed-width
 * enqueue timestamp), then iterates oldest-first. rename(2) serializes
 * access — losers get ENOENT and fall through to the next-oldest entry.
 * Single consumer = strict FIFO. Multi-consumer = best-effort FIFO.
 */
static int scan_and_claim(fbmq_queue_t *q, const char *scan_dir,
                          char *claimed_path, size_t pathlen)
{
    for (int attempt = 0; attempt < FBMQ_SCAN_RETRIES; attempt++) {
        int count = 0;
        char **entries = collect_md_entries(scan_dir, &count);
        if (count < 0) return -1;         /* error (errno set) */
        if (count == 0) { free(entries); return 1; } /* empty directory */

        qsort(entries, (size_t)count, sizeof(char *), cmp_entry_name);

        int found_claimable = 0;
        for (int i = 0; i < count; i++) {
            char src[FBMQ_MAX_PATH];
            if (path_fmt(src, sizeof(src), "%s/%s", scan_dir, entries[i]) != 0) {
                free_entries(entries, count);
                return -1;
            }

            struct timespec claim_ts;
            if (clock_gettime(CLOCK_REALTIME, &claim_ts) != 0) {
                free_entries(entries, count);
                return -1;
            }

            /* Strip enqueue timestamp prefix so claimed path
             * becomes <claim_ts>.<hash>.md */
            const char *base = entries[i];
            const char *p = base;
            while (*p >= '0' && *p <= '9') p++;
            if (*p == '.' && p > base) base = p + 1;

            if (path_fmt(claimed_path, pathlen, "%s/processing/%ld%09ld.%s",
                         q->root, (long)claim_ts.tv_sec, (long)claim_ts.tv_nsec, base) != 0) {
                free_entries(entries, count);
                return -1;
            }

            if (rename(src, claimed_path) == 0) {
                free_entries(entries, count);
                return 0; /* claimed */
            }

            if (errno != ENOENT) {
                free_entries(entries, count);
                return -1; /* real error */
            }

            found_claimable = 1; /* at least one entry existed (race lost) */
        }
        free_entries(entries, count);

        if (!found_claimable)
            return 1; /* all entries vanished but none raced (shouldn't happen) */

        /* All entries in this pass were raced away, retry with fresh scan */
    }

    return 1; /* exhausted retries */
}

int fbmq_dequeue(fbmq_queue_t *q, char *claimed_path, size_t pathlen)
{
    char dir[FBMQ_MAX_PATH];

    /* Scan priorities in order: critical → high → normal → low */
    int pstart = q->use_priority_dirs ? 0 : -1;
    int pend   = q->use_priority_dirs ? FBMQ_PRIO_COUNT : 0;
    for (int p = pstart; p < pend; p++) {
        if (pending_path(q, p, dir, sizeof(dir)) != 0) return -1;
        int rc = scan_and_claim(q, dir, claimed_path, pathlen);
        if (rc == 0) return 0;
        if (rc < 0) return -1;
    }

    return 1;  /* empty */
}

/* ────────────────────────────────────────────
 * Complete / Fail
 * ──────────────────────────────────────────── */

/*
 * Strip the numeric timestamp prefix from a claimed path's basename.
 * Claimed format: .../processing/<claim_timestamp>.<hash>.md
 * Returns pointer to "<hash>.md" portion within claimed_path.
 */
static const char *strip_timestamp_prefix(const char *claimed_path)
{
    const char *base = strrchr(claimed_path, '/');
    base = base ? base + 1 : claimed_path;

    /* Skip the claim timestamp prefix: digits followed by '.' */
    const char *p = base;
    while (*p >= '0' && *p <= '9') p++;
    if (*p == '.' && p > base)
        return p + 1;

    /* No valid prefix found — return base unchanged */
    return base;
}

int fbmq_complete(fbmq_queue_t *q, const char *claimed_path)
{
    const char *orig = strip_timestamp_prefix(claimed_path);
    char dest[FBMQ_MAX_PATH];
    if (path_fmt(dest, sizeof(dest), "%s/done/%s", q->root, orig) != 0) return -1;
    if (rename(claimed_path, dest) != 0) return -1;

    if (q->fsync_mode == FBMQ_FSYNC_FULL) {
        char dir[FBMQ_MAX_PATH];
        if (path_fmt(dir, sizeof(dir), "%s/done", q->root) == 0)
            fbmq_fsync_dir(dir);
        if (path_fmt(dir, sizeof(dir), "%s/processing", q->root) == 0)
            fbmq_fsync_dir(dir);
    }
    return 0;
}

/*
 * Note: the read-modify-write of retry_count is not atomic. If the reaper
 * and a slow consumer race on the same message, a retry increment can be
 * lost. In practice this is rare — only one consumer should hold a given
 * message in processing/ — but the reaper's orphan detection path can
 * trigger it.
 */
int fbmq_fail(fbmq_queue_t *q, const char *claimed_path)
{
    const char *orig = strip_timestamp_prefix(claimed_path);

    /* Parse, increment retry, rewrite */
    fbmq_message_t msg = {0};
    if (fbmq_parse_file(claimed_path, &msg) != 0) return -1;

    msg.header.retry_count++;

    /* Serialize updated message to buffer */
    char *buf = NULL;
    size_t buflen = 0;
    if (fbmq_serialize(&msg, &buf, &buflen) != 0) {
        fbmq_message_free(&msg);
        return -1;
    }

    /* Crash-safe rewrite: write to .tmp/, rename to destination, unlink claimed.
     * Ordering: rename(dest) before unlink(claimed) ensures at-least-once
     * delivery on crash. A crash between rename and unlink leaves a duplicate
     * that the reaper's hash_exists_elsewhere() will clean up. */
    char tmp_path[FBMQ_MAX_PATH];
    if (path_fmt(tmp_path, sizeof(tmp_path), "%s/.tmp/nack-%s", q->root, orig) != 0) {
        free(buf); fbmq_message_free(&msg); return -1;
    }

    if (write_tmp(q, tmp_path, buf, buflen) != 0) {
        free(buf); fbmq_message_free(&msg); return -1;
    }
    free(buf);

    char dest[FBMQ_MAX_PATH];
    if (msg.header.retry_count > q->max_retries) {
        if (path_fmt(dest, sizeof(dest), "%s/failed/%s", q->root, orig) != 0) {
            unlink(tmp_path); fbmq_message_free(&msg); return -1;
        }
    } else {
        /* Re-add the original enqueue timestamp derived from Created-At so
         * the filename format stays consistent (<timestamp>.<hash>.md). */
        char pdir[FBMQ_MAX_PATH];
        if (pending_path(q, (int)msg.header.priority, pdir, sizeof(pdir)) != 0) {
            unlink(tmp_path); fbmq_message_free(&msg); return -1;
        }

        /* Reconstruct original nanosecond timestamp prefix from Created-At */
        time_t enq_sec;
        long enq_nsec;
        if (parse_created_at(msg.header.created_at, &enq_sec, &enq_nsec) == 0) {
            if (path_fmt(dest, sizeof(dest), "%s/%ld%09ld.%s", pdir, (long)enq_sec, (long)enq_nsec, orig) != 0) {
                unlink(tmp_path); fbmq_message_free(&msg); return -1;
            }
        } else {
            /* Fallback: no valid timestamp, use bare hash */
            if (path_fmt(dest, sizeof(dest), "%s/%s", pdir, orig) != 0) {
                unlink(tmp_path); fbmq_message_free(&msg); return -1;
            }
        }
    }

    /* Rename the .tmp/ copy into place BEFORE unlinking the claimed file.
     * This creates a brief duplication window (message in both processing/
     * and pending/failed/), but a crash at any point leaves at least one
     * copy — at-least-once beats at-most-once for a message queue.
     * The reaper's hash_exists_elsewhere() handles the duplicate. */
    if (rename(tmp_path, dest) != 0) {
        unlink(tmp_path); fbmq_message_free(&msg); return -1;
    }
    if (unlink(claimed_path) != 0 && errno != ENOENT) {
        /* Non-fatal: the file is already safely in dest.
         * The reaper will clean up the orphan in processing/. */
    }

    if (q->fsync_mode == FBMQ_FSYNC_FULL) {
        /* Fsync destination dir (pending/ or failed/) and processing/ */
        char *last_slash = strrchr(dest, '/');
        if (last_slash) {
            char dir[FBMQ_MAX_PATH];
            size_t dlen = (size_t)(last_slash - dest);
            if (dlen < sizeof(dir)) {
                memcpy(dir, dest, dlen);
                dir[dlen] = '\0';
                fbmq_fsync_dir(dir);
            }
        }
        char proc_dir[FBMQ_MAX_PATH];
        if (path_fmt(proc_dir, sizeof(proc_dir), "%s/processing", q->root) == 0)
            fbmq_fsync_dir(proc_dir);
    }

    fbmq_message_free(&msg);
    return 0;
}

/* ────────────────────────────────────────────
 * Reapers (collect-then-act to avoid readdir-while-modifying)
 * ──────────────────────────────────────────── */

/* Collect .md filenames from a directory into a heap-allocated array.
 * Caller must free each entry and the array itself.
 * On OOM, sets *count_out = -1, errno = ENOMEM, and returns NULL. */
static char **collect_md_entries(const char *dir, int *count_out)
{
    DIR *d = opendir(dir);
    if (!d) {
        if (errno == ENOENT) { *count_out = 0; return NULL; }
        *count_out = -1;
        return NULL;
    }

    size_t cap = 64, n = 0;
    char **entries = malloc(cap * sizeof(char *));
    if (!entries) { closedir(d); *count_out = -1; errno = ENOMEM; return NULL; }

    struct dirent *ent;
    int oom = 0;
    errno = 0;
    while ((ent = readdir(d)) != NULL) {
        size_t nlen = strlen(ent->d_name);
        if (nlen <= 3 || strcmp(ent->d_name + nlen - 3, ".md") != 0)
            continue;
        if (n >= cap) {
            if (cap > SIZE_MAX / (2 * sizeof(char *))) { oom = 1; break; }
            size_t newcap = cap * 2;
            char **tmp = realloc(entries, newcap * sizeof(char *));
            if (!tmp) { oom = 1; break; }
            entries = tmp;
            cap = newcap;
        }
        entries[n] = strdup(ent->d_name);
        if (!entries[n]) { oom = 1; break; }
        n++;
        errno = 0;
    }
    int readdir_err = errno;
    closedir(d);
    if (oom) {
        free_entries(entries, (int)n);
        *count_out = -1;
        errno = ENOMEM;
        return NULL;
    }
    if (readdir_err) {
        free_entries(entries, (int)n);
        *count_out = -1;
        errno = readdir_err;
        return NULL;
    }
    if (n > INT_MAX) {
        for (size_t j = 0; j < n; j++) free(entries[j]);
        free(entries);
        *count_out = -1;
        errno = EOVERFLOW;
        return NULL;
    }
    *count_out = (int)n;
    return entries;
}

static void free_entries(char **entries, int count)
{
    if (!entries) return;
    for (int i = 0; i < count; i++) free(entries[i]);
    free(entries);
}

/* Check if a directory contains a file whose name matches hash_md exactly
 * or ends with .<hash_md> (timestamp-prefixed entries in pending/). */
static int dir_contains_hash(const char *dir, const char *hash_md)
{
    DIR *d = opendir(dir);
    if (!d) return 0;
    size_t hlen = strlen(hash_md);
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        size_t nlen = strlen(ent->d_name);
        if (nlen == hlen && strcmp(ent->d_name, hash_md) == 0) {
            closedir(d); return 1;
        }
        if (nlen > hlen && ent->d_name[nlen - hlen - 1] == '.' &&
            strcmp(ent->d_name + nlen - hlen, hash_md) == 0) {
            closedir(d); return 1;
        }
    }
    closedir(d);
    return 0;
}

/*
 * Check if a message hash already exists in pending/ or failed/.
 * Used by the reaper to detect orphans from crash between rename and unlink
 * in fbmq_fail() — avoids double-delivery and corrupted retry counts.
 */
static int hash_exists_elsewhere(fbmq_queue_t *q, const char *hash_md)
{
    char path[FBMQ_MAX_PATH];

    /* Check failed/ — access() has a TOCTOU race but it's harmless here:
     * worst case the orphan survives one reap cycle and is cleaned up next time. */
    if (path_fmt(path, sizeof(path), "%s/failed/%s", q->root, hash_md) == 0 &&
        access(path, F_OK) == 0)
        return 1;

    /* Check pending/ (flat or priority subdirs) — scan for timestamp-prefixed entries */
    char pdir[FBMQ_MAX_PATH];
    FOR_EACH_PENDING(q, p, pdir, sizeof(pdir)) {
        if (dir_contains_hash(pdir, hash_md))
            return 1;
    }

    return 0;
}

int fbmq_reap(fbmq_queue_t *q)
{
    char proc_dir[FBMQ_MAX_PATH];
    if (path_fmt(proc_dir, sizeof(proc_dir), "%s/processing", q->root) != 0) return -1;

    /* Verify processing directory exists — fail on nonexistent queues */
    struct stat proc_st;
    if (stat(proc_dir, &proc_st) != 0) return -1;

    int count = 0;
    char **entries = collect_md_entries(proc_dir, &count);
    if (count < 0) return -1;

    time_t now = time(NULL);
    int reaped = 0;

    for (int i = 0; i < count; i++) {
        char *endptr;
        errno = 0;
        long long raw_ts = strtoll(entries[i], &endptr, 10);
        if (raw_ts <= 0 || *endptr != '.' || errno == ERANGE) continue;

        /* Claim timestamp is <sec><09nsec> — extract seconds by dividing out nanoseconds */
        time_t claimed_at = (time_t)(raw_ts / 1000000000LL);
        if (now - claimed_at > q->lease_timeout) {
            char full[FBMQ_MAX_PATH];
            if (path_fmt(full, sizeof(full), "%s/%s", proc_dir, entries[i]) != 0)
                continue;

            /* Orphan detection: if the hash already exists in pending/ or
             * failed/, this is a stale leftover from a crash between rename
             * and unlink in fbmq_fail(). Just remove the orphan. */
            const char *hash_md = strip_timestamp_prefix(full);
            if (hash_exists_elsewhere(q, hash_md)) {
                unlink(full);
                reaped++;
                continue;
            }

            if (fbmq_fail(q, full) == 0)
                reaped++;
        }
    }

    free_entries(entries, count);

    if (reaped > 0 && q->fsync_mode == FBMQ_FSYNC_FULL)
        fbmq_fsync_dir(proc_dir);

    /* Clean up stale orphans in .tmp/ (interrupted pushes) */
    char tmp_dir[FBMQ_MAX_PATH];
    if (path_fmt(tmp_dir, sizeof(tmp_dir), "%s/.tmp", q->root) == 0) {
        int tmp_count = 0;
        char **tmp_entries = collect_md_entries(tmp_dir, &tmp_count);
        if (tmp_entries) {
            for (int i = 0; i < tmp_count; i++) {
                char tmp_full[FBMQ_MAX_PATH];
                if (path_fmt(tmp_full, sizeof(tmp_full), "%s/%s", tmp_dir, tmp_entries[i]) != 0)
                    continue;
                struct stat st;
                if (stat(tmp_full, &st) != 0) continue;
                if (now - st.st_mtime > q->lease_timeout) {
                    unlink(tmp_full);
                }
            }
            free_entries(tmp_entries, tmp_count);
        }
    }

    return reaped;
}

/* Scan a single directory for TTL-expired messages, moving them to failed/ */
static int reap_ttl_dir(fbmq_queue_t *q, const char *dir, time_t now)
{
    int count = 0;
    char **entries = collect_md_entries(dir, &count);
    if (count < 0) return -1;
    if (!entries) return 0;

    int expired = 0;
    for (int i = 0; i < count; i++) {
        char full[FBMQ_MAX_PATH];
        if (path_fmt(full, sizeof(full), "%s/%s", dir, entries[i]) != 0)
            continue;

        fbmq_message_t msg = {0};
        if (fbmq_parse_headers(full, &msg) != 0) continue;

        if (msg.header.ttl > 0 && msg.header.created_at[0]) {
            time_t created;
            long ttl_nsec;
            if (parse_created_at(msg.header.created_at, &created, &ttl_nsec) == 0 &&
                (now - created) > msg.header.ttl) {
                /* Strip timestamp prefix so failed/ gets <hash>.md */
                const char *bare = entries[i];
                const char *ep = bare;
                while (*ep >= '0' && *ep <= '9') ep++;
                if (*ep == '.' && ep > bare) bare = ep + 1;
                char dest[FBMQ_MAX_PATH];
                if (path_fmt(dest, sizeof(dest), "%s/failed/%s",
                             q->root, bare) != 0) {
                    fbmq_message_free(&msg);
                    continue;
                }
                if (rename(full, dest) == 0)
                    expired++;
            }
        }
        fbmq_message_free(&msg);
    }
    free_entries(entries, count);

    if (expired > 0 && q->fsync_mode == FBMQ_FSYNC_FULL)
        fbmq_fsync_dir(dir);

    return expired;
}

/* FIX #16: TTL expiry reaper */
int fbmq_reap_ttl(fbmq_queue_t *q)
{
    int expired = 0;
    time_t now = time(NULL);
    char bdir[FBMQ_MAX_PATH];

    FOR_EACH_PENDING(q, p, bdir, sizeof(bdir)) {
        int r = reap_ttl_dir(q, bdir, now);
        if (r < 0) return -1;
        expired += r;
    } else {
        fprintf(stderr, "fbmq: reap_ttl: pending path too long (priority %d)\n", p);
    }

    if (expired > 0 && q->fsync_mode == FBMQ_FSYNC_FULL) {
        char failed_dir[FBMQ_MAX_PATH];
        if (path_fmt(failed_dir, sizeof(failed_dir), "%s/failed", q->root) == 0)
            fbmq_fsync_dir(failed_dir);
    }

    return expired;
}

/* ────────────────────────────────────────────
 * Purge
 * ──────────────────────────────────────────── */

int fbmq_purge(fbmq_queue_t *q, int max_age_seconds)
{
    char done_dir[FBMQ_MAX_PATH];
    if (path_fmt(done_dir, sizeof(done_dir), "%s/done", q->root) != 0) return -1;

    int count = 0;
    char **entries = collect_md_entries(done_dir, &count);
    if (count < 0) return -1;
    if (!entries && count == 0) return 0;

    time_t now = time(NULL);
    int purged = 0;

    for (int i = 0; i < count; i++) {
        char full[FBMQ_MAX_PATH];
        if (path_fmt(full, sizeof(full), "%s/%s", done_dir, entries[i]) != 0)
            continue;

        /* Use mtime for age calculation — in done/ this reflects ack time,
         * which is the relevant timestamp for purge decisions. */
        struct stat st;
        if (stat(full, &st) != 0) continue;
        time_t created = st.st_mtime;

        if (now - created >= max_age_seconds) {
            if (unlink(full) == 0) purged++;
        }
    }

    if (purged > 0 && q->fsync_mode == FBMQ_FSYNC_FULL)
        fbmq_fsync_dir(done_dir);

    free_entries(entries, count);
    return purged;
}
