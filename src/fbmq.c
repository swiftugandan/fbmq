/*
 * fbmq.c — Core library implementation
 *
 * All review fixes applied:
 *  #8:  Buffered single-write serialization (no dprintf)
 *  #11: RFC 822 header parser with continuation-line support
 *  #14: getrandom(2) with fallback chain
 *  #15: Configurable file/dir permissions (default 0640/0750)
 *  #16: TTL expiry reaper
 *  #19: Deduplicated claim logic
 */

#include "fbmq.h"
#include "md5.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/types.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#include <time.h>
#include <limits.h>
#include <signal.h>

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

    /* Days from epoch (1970-01-01) to start of year y */
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
    if (p >= 0 && p < FBMQ_PRIO_COUNT) return prio_names[p];
    return "normal";
}

fbmq_priority_t fbmq_priority_parse(const char *s)
{
    if (!s) return FBMQ_PRIO_NORMAL;
    if (strcmp(s, "critical") == 0) return FBMQ_PRIO_CRITICAL;
    if (strcmp(s, "high") == 0)     return FBMQ_PRIO_HIGH;
    if (strcmp(s, "normal") == 0)   return FBMQ_PRIO_NORMAL;
    if (strcmp(s, "low") == 0)      return FBMQ_PRIO_LOW;
    return FBMQ_PRIO_NORMAL;
}

/* ────────────────────────────────────────────
 * Queue defaults  (FIX #15: secure permissions)
 * ──────────────────────────────────────────── */

void fbmq_queue_defaults(fbmq_queue_t *q)
{
    memset(q, 0, sizeof(*q));
    q->max_retries   = FBMQ_DEFAULT_RETRIES;
    q->lease_timeout  = FBMQ_DEFAULT_LEASE;
    q->file_mode      = 0640;
    q->dir_mode       = 0750;
    q->hints_ttl      = FBMQ_DEFAULT_HINTS_TTL;
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
    /* Try getrandom(2) first — works in containers/chroots */
    long rc = syscall(SYS_getrandom, buf, len, 0);
    if (rc == (long)len) return 0;

    /* Fallback: /dev/urandom */
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ssize_t n = read(fd, buf, len);
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
            if (custom_pos + ll + 2 < sizeof(h->custom)) {
                memcpy(h->custom + custom_pos, line, ll);
                custom_pos += ll;
                h->custom[custom_pos++] = '\n';
                h->custom[custom_pos] = '\0';
            }
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
            if (vlen != FBMQ_ID_LEN) { free(buf); return -1; }
            for (size_t k = 0; k < vlen; k++) {
                char c = val[k];
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                    free(buf);
                    return -1;
                }
            }
            snprintf(h->id, sizeof(h->id), "%s", val);
        } else if (strcmp(key, "created_at") == 0) {
            if (strlen(val) >= sizeof(h->created_at)) { free(buf); return -1; }
            snprintf(h->created_at, sizeof(h->created_at), "%s", val);
        } else if (strcmp(key, "created_by") == 0) {
            if (strlen(val) >= sizeof(h->created_by)) { free(buf); return -1; }
            snprintf(h->created_by, sizeof(h->created_by), "%s", val);
        } else if (strcmp(key, "priority") == 0) {
            h->priority = fbmq_priority_parse(val);
        } else if (strcmp(key, "retry_count") == 0) {
            char *end;
            long v = strtol(val, &end, 10);
            if (end != val && v >= 0 && v <= 10000)
                h->retry_count = (int)v;
        } else if (strcmp(key, "ttl") == 0) {
            char *end;
            long v = strtol(val, &end, 10);
            if (end != val && v >= 0 && v <= 2147483647)
                h->ttl = (int)v;
        } else if (strcmp(key, "correlation_id") == 0) {
            if (strlen(val) >= sizeof(h->correlation_id)) { free(buf); return -1; }
            snprintf(h->correlation_id, sizeof(h->correlation_id), "%s", val);
        } else if (strcmp(key, "tags") == 0) {
            if (strlen(val) >= sizeof(h->tags)) { free(buf); return -1; }
            snprintf(h->tags, sizeof(h->tags), "%s", val);
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
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return -1;

    uint32_t rnd;
    if (get_random_bytes(&rnd, sizeof(rnd)) != 0) return -1;

    char nonce[128];
    int nlen = snprintf(nonce, sizeof(nonce), "%ld.%09ld.%d.%08x",
                        (long)ts.tv_sec, ts.tv_nsec, (int)getpid(), rnd);

    fbmq_md5_ctx ctx;
    fbmq_md5_init(&ctx);
    fbmq_md5_update(&ctx, nonce, (size_t)nlen);

    unsigned char digest[16];
    fbmq_md5_final(digest, &ctx);

    for (int i = 0; i < 16; i++)
        snprintf(out + i * 2, 3, "%02x", digest[i]);
    out[FBMQ_ID_LEN] = '\0';
    return 0;
}

void fbmq_bucket(const char *id, char *out)
{
    if (!id || !id[0] || !id[1]) {
        out[0] = '0'; out[1] = '0'; out[2] = '\0';
        return;
    }
    out[0] = id[0];
    out[1] = id[1];
    out[2] = '\0';
}

/* ────────────────────────────────────────────
 * Queue initialization
 * ──────────────────────────────────────────── */

int fbmq_detect_priority(const char *root)
{
    char path[FBMQ_MAX_PATH];
    struct stat st;
    if (path_fmt(path, sizeof(path), "%s/pending/00/2-normal", root) != 0)
        return 0;
    return (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

int fbmq_init(fbmq_queue_t *q, const char *root)
{
    if (path_fmt(q->root, sizeof(q->root), "%s", root) != 0) return -1;
    mode_t dm = q->dir_mode ? q->dir_mode : 0750;

    char path[FBMQ_MAX_PATH];

    for (int i = 0; i < FBMQ_BUCKET_COUNT; i++) {
        if (q->use_priority_dirs) {
            for (int p = 0; p < FBMQ_PRIO_COUNT; p++) {
                if (path_fmt(path, sizeof(path), "%s/pending/%02x/%d-%s",
                             root, i, p, fbmq_priority_str(p)) != 0) return -1;
                if (mkdirp(path, dm) != 0) return -1;
            }
        } else {
            if (path_fmt(path, sizeof(path), "%s/pending/%02x", root, i) != 0) return -1;
            if (mkdirp(path, dm) != 0) return -1;
        }
    }

    const char *subdirs[] = { "processing", "done", "failed", ".tmp" };
    for (int i = 0; i < 4; i++) {
        if (path_fmt(path, sizeof(path), "%s/%s", root, subdirs[i]) != 0) return -1;
        if (mkdirp(path, dm) != 0) return -1;
    }

    return 0;
}

/* ────────────────────────────────────────────
 * Depth counter (stateless full scan)
 *
 * Count-on-read: scan all 256 pending buckets plus processing/.
 * No persistent state, no .meta/ files.
 * ──────────────────────────────────────────── */

/* Count .md files in a single directory */
static int64_t count_md_files(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return 0;

    int64_t count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        size_t nlen = strlen(ent->d_name);
        if (nlen > 3 && strcmp(ent->d_name + nlen - 3, ".md") == 0)
            count++;
    }
    closedir(d);
    return count;
}

/* Count .md files in a bucket (with or without priority subdirs) */
static int64_t count_bucket(fbmq_queue_t *q, int bucket_num)
{
    char dir[FBMQ_MAX_PATH];
    int64_t count = 0;

    if (q->use_priority_dirs) {
        for (int p = 0; p < FBMQ_PRIO_COUNT; p++) {
            if (path_fmt(dir, sizeof(dir), "%s/pending/%02x/%d-%s",
                         q->root, bucket_num, p, fbmq_priority_str(p)) != 0)
                continue;
            count += count_md_files(dir);
        }
    } else {
        if (path_fmt(dir, sizeof(dir), "%s/pending/%02x",
                     q->root, bucket_num) != 0)
            return 0;
        count = count_md_files(dir);
    }
    return count;
}

int fbmq_depth(fbmq_queue_t *q, int64_t *depth)
{
    /* Verify queue root exists */
    struct stat root_st;
    if (stat(q->root, &root_st) != 0) return -1;

    /* Full scan of all 256 buckets */
    int64_t pending = 0;
    for (int b = 0; b < FBMQ_BUCKET_COUNT; b++)
        pending += count_bucket(q, b);

    /* Exact count of processing/ (flat directory, typically small) */
    char proc_dir[FBMQ_MAX_PATH];
    if (path_fmt(proc_dir, sizeof(proc_dir), "%s/processing", q->root) != 0)
        return -1;
    int64_t processing = count_md_files(proc_dir);

    *depth = pending + processing;
    return 0;
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
    if (h->custom[0])
        sb_printf(&sb, "Custom:\n%s", h->custom);

    sb_printf(&sb, "\n");  /* blank line separates headers from body */

    if (msg->body && msg->body_len > 0)
        sb_append(&sb, msg->body, msg->body_len);

    if (!sb.data) return -1;

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
    #define FBMQ_HEADER_MAX 4096
    size_t peek = filesz < FBMQ_HEADER_MAX ? filesz : FBMQ_HEADER_MAX;
    char hdr_buf[FBMQ_HEADER_MAX + 1];

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
                 tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec);
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

    char bucket[3];
    fbmq_bucket(msg->header.id, bucket);

    /* Serialize to buffer */
    char *buf = NULL;
    size_t buflen = 0;
    if (fbmq_serialize(msg, &buf, &buflen) != 0) return -1;

    /* Write to temp file in a single write(2) */
    char tmp_path[FBMQ_MAX_PATH];
    if (path_fmt(tmp_path, sizeof(tmp_path), "%s/.tmp/%s.md", q->root, msg->header.id) != 0) {
        free(buf); return -1;
    }

    mode_t fm = q->file_mode ? q->file_mode : 0640;
    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, fm);
    if (fd < 0) { free(buf); return -1; }

    ssize_t written = safe_write(fd, buf, buflen);
    free(buf);
    if (written < 0 || (size_t)written != buflen) {
        close(fd); unlink(tmp_path); return -1;
    }

    if (q->fsync_mode != FBMQ_FSYNC_NONE) {
        if (fsync(fd) != 0) { close(fd); unlink(tmp_path); return -1; }
    }
    close(fd);

    /* Atomic rename into pending bucket — prefix with nanosecond timestamp for FIFO */
    char dest[FBMQ_MAX_PATH];
    int pfmt_rc;
    if (q->use_priority_dirs) {
        pfmt_rc = path_fmt(dest, sizeof(dest), "%s/pending/%s/%d-%s/%ld%09ld.%s.md",
                           q->root, bucket, (int)msg->header.priority,
                           fbmq_priority_str(msg->header.priority),
                           (long)ts.tv_sec, ts.tv_nsec, msg->header.id);
    } else {
        pfmt_rc = path_fmt(dest, sizeof(dest), "%s/pending/%s/%ld%09ld.%s.md",
                           q->root, bucket,
                           (long)ts.tv_sec, ts.tv_nsec, msg->header.id);
    }
    if (pfmt_rc != 0) { unlink(tmp_path); return -1; }

    if (rename(tmp_path, dest) != 0) {
        unlink(tmp_path);
        return -1;
    }

    if (q->fsync_mode == FBMQ_FSYNC_FULL) {
        char dir[FBMQ_MAX_PATH];
        if (q->use_priority_dirs) {
            if (path_fmt(dir, sizeof(dir), "%s/pending/%s/%d-%s",
                         q->root, bucket, (int)msg->header.priority,
                         fbmq_priority_str(msg->header.priority)) == 0)
                fbmq_fsync_dir(dir);
        } else {
            if (path_fmt(dir, sizeof(dir), "%s/pending/%s", q->root, bucket) == 0)
                fbmq_fsync_dir(dir);
        }
    }

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

    for (int i = 0; i < FBMQ_BUCKET_COUNT; i++) {
        if (q->use_priority_dirs) {
            for (int p = 0; p < FBMQ_PRIO_COUNT; p++) {
                if (path_fmt(dir, sizeof(dir), "%s/pending/%02x/%d-%s",
                             q->root, i, p, fbmq_priority_str(p)) == 0)
                    fbmq_fsync_dir(dir);
            }
        } else {
            if (path_fmt(dir, sizeof(dir), "%s/pending/%02x", q->root, i) == 0)
                fbmq_fsync_dir(dir);
        }
    }

    if (path_fmt(dir, sizeof(dir), "%s/processing", q->root) == 0)
        fbmq_fsync_dir(dir);
    if (path_fmt(dir, sizeof(dir), "%s/failed", q->root) == 0)
        fbmq_fsync_dir(dir);

    return 0;
}

/* ────────────────────────────────────────────
 * Dequeue  (FIX #1: only returns claimed path)
 *          (FIX #19: deduplicated claim logic)
 * ──────────────────────────────────────────── */

/*
 * Scan a single directory for the first .md file and try to claim it.
 * Returns: 0=claimed, 1=nothing found, -1=error.
 */
static int scan_and_claim(fbmq_queue_t *q, const char *scan_dir,
                          char *claimed_path, size_t pathlen)
{
    for (int attempt = 0; attempt < FBMQ_SCAN_RETRIES; attempt++) {
        DIR *d = opendir(scan_dir);
        if (!d) return 1;

        /* Scan all entries and track lexicographic minimum for FIFO order */
        char min_name[256];
        min_name[0] = '\0';

        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            size_t nlen = strlen(ent->d_name);
            if (nlen <= 3 || strcmp(ent->d_name + nlen - 3, ".md") != 0)
                continue;
            if (nlen >= sizeof(min_name))
                continue;
            if (min_name[0] == '\0' || strcmp(ent->d_name, min_name) < 0)
                snprintf(min_name, sizeof(min_name), "%s", ent->d_name);
        }
        closedir(d);

        if (min_name[0] == '\0')
            return 1; /* empty directory */

        char src[FBMQ_MAX_PATH];
        if (path_fmt(src, sizeof(src), "%s/%s", scan_dir, min_name) != 0)
            return -1;

        struct timespec claim_ts;
        if (clock_gettime(CLOCK_REALTIME, &claim_ts) != 0)
            return -1;

        /* Strip enqueue timestamp prefix from min_name so claimed path
         * becomes <claim_ts>.<hash>.md instead of <claim_ts>.<enqueue_ts>.<hash>.md */
        const char *base = min_name;
        const char *p = base;
        while (*p >= '0' && *p <= '9') p++;
        if (*p == '.' && p > base) base = p + 1;

        if (path_fmt(claimed_path, pathlen, "%s/processing/%ld%09ld.%s",
                     q->root, (long)claim_ts.tv_sec, claim_ts.tv_nsec, base) != 0)
            return -1;

        if (rename(src, claimed_path) == 0)
            return 0; /* claimed */

        if (errno != ENOENT)
            return -1; /* real error */

        /* ENOENT = race lost, retry */
    }

    return 1; /* exhausted retries */
}

static int try_claim_bucket(fbmq_queue_t *q, int bucket_num,
                            char *claimed_path, size_t pathlen)
{
    char dir[FBMQ_MAX_PATH];

    if (q->use_priority_dirs) {
        /* Scan priorities in order: critical → high → normal → low */
        for (int p = 0; p < FBMQ_PRIO_COUNT; p++) {
            if (path_fmt(dir, sizeof(dir), "%s/pending/%02x/%d-%s",
                         q->root, bucket_num, p, fbmq_priority_str(p)) != 0) return -1;
            int rc = scan_and_claim(q, dir, claimed_path, pathlen);
            if (rc != 1) return rc;  /* 0=claimed or -1=error */
        }
    } else {
        if (path_fmt(dir, sizeof(dir), "%s/pending/%02x", q->root, bucket_num) != 0) return -1;
        return scan_and_claim(q, dir, claimed_path, pathlen);
    }

    return 1;  /* nothing in this bucket */
}

/* ── Scan hint helpers ── */

static inline int hint_index(int bucket, int prio, int use_prio)
{
    return use_prio ? bucket * FBMQ_PRIO_COUNT + prio : bucket;
}

static inline int hint_get(const fbmq_queue_t *q, int idx)
{
    return (q->scan_hints[idx / 8] >> (idx % 8)) & 1;
}

static inline void hint_set(fbmq_queue_t *q, int idx)
{
    q->scan_hints[idx / 8] |= (uint8_t)(1 << (idx % 8));
}

static void hints_clear(fbmq_queue_t *q)
{
    memset(q->scan_hints, 0, sizeof(q->scan_hints));
    q->hints_reset_at = time(NULL);
}

static void hints_expire_if_stale(fbmq_queue_t *q)
{
    if (q->hints_ttl <= 0) return;
    time_t now = time(NULL);
    if (now - q->hints_reset_at > q->hints_ttl)
        hints_clear(q);
}

int fbmq_dequeue(fbmq_queue_t *q, char *claimed_path, size_t pathlen)
{
    /* Expire stale hints before scanning */
    hints_expire_if_stale(q);

    /* Start at random bucket to distribute contention */
    unsigned int seed;
    get_random_bytes(&seed, sizeof(seed));
    int start = (int)(seed % FBMQ_BUCKET_COUNT);

    for (int pass = 0; pass < 2; pass++) {
        if (q->use_priority_dirs) {
            /*
             * Priority mode: scan ALL buckets at each priority level before
             * descending. This ensures a critical message in any bucket is
             * always claimed before a normal message in any bucket.
             */
            for (int p = 0; p < FBMQ_PRIO_COUNT; p++) {
                for (int i = 0; i < FBMQ_BUCKET_COUNT; i++) {
                    int b = (start + i) % FBMQ_BUCKET_COUNT;
                    int hidx = hint_index(b, p, 1);
                    if (pass == 0 && hint_get(q, hidx))
                        continue;  /* skip hinted-empty slot */

                    char dir[FBMQ_MAX_PATH];
                    if (path_fmt(dir, sizeof(dir), "%s/pending/%02x/%d-%s",
                                 q->root, b, p, fbmq_priority_str(p)) != 0) return -1;
                    int rc = scan_and_claim(q, dir, claimed_path, pathlen);
                    if (rc == 0) return 0;
                    if (rc < 0) return -1;
                    /* rc == 1: empty — mark hint */
                    hint_set(q, hidx);
                }
            }
        } else {
            for (int i = 0; i < FBMQ_BUCKET_COUNT; i++) {
                int b = (start + i) % FBMQ_BUCKET_COUNT;
                int hidx = hint_index(b, 0, 0);
                if (pass == 0 && hint_get(q, hidx))
                    continue;

                int rc = try_claim_bucket(q, b, claimed_path, pathlen);
                if (rc == 0) return 0;
                if (rc < 0) return -1;
                hint_set(q, hidx);
            }
        }

        /*
         * Safety net: if pass 0 found nothing (all hinted-empty or truly empty),
         * clear hints and do one unhinted full scan to catch stale hints.
         */
        if (pass == 0) {
            /* Check if any hints were active — if not, no point in a second pass */
            int any_hinted = 0;
            size_t hint_bytes = q->use_priority_dirs ? sizeof(q->scan_hints) : 32;
            for (size_t j = 0; j < hint_bytes; j++) {
                if (q->scan_hints[j]) { any_hinted = 1; break; }
            }
            if (!any_hinted)
                return 1;  /* truly empty, no stale hints possible */
            hints_clear(q);
        }
    }

    return 1;  /* empty */
}

/* ────────────────────────────────────────────
 * Complete / Fail
 * ──────────────────────────────────────────── */

/*
 * Extract the original filename from a claimed path.
 * Claimed format: .../processing/<claim_timestamp>.<hash>.md
 * Returns pointer to "<hash>.md" portion.
 */
static const char *original_name(const char *claimed_path)
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
    const char *orig = original_name(claimed_path);
    char dest[FBMQ_MAX_PATH];
    if (path_fmt(dest, sizeof(dest), "%s/done/%s", q->root, orig) != 0) return -1;
    return rename(claimed_path, dest);
}

int fbmq_fail(fbmq_queue_t *q, const char *claimed_path)
{
    const char *orig = original_name(claimed_path);

    /* Parse, increment retry, rewrite */
    fbmq_message_t msg = {0};
    if (fbmq_parse_file(claimed_path, &msg) != 0) return -1;

    msg.header.retry_count++;

    /* Rewrite using buffered serialize (FIX #8) */
    char *buf = NULL;
    size_t buflen = 0;
    if (fbmq_serialize(&msg, &buf, &buflen) != 0) {
        fbmq_message_free(&msg);
        return -1;
    }

    int fd = open(claimed_path, O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (fd < 0) { free(buf); fbmq_message_free(&msg); return -1; }
    ssize_t written = safe_write(fd, buf, buflen);
    if (written < 0 || (size_t)written != buflen) {
        close(fd); free(buf); fbmq_message_free(&msg); return -1;
    }
    if (q->fsync_mode != FBMQ_FSYNC_NONE) {
        if (fsync(fd) != 0) {
            close(fd); free(buf); fbmq_message_free(&msg); return -1;
        }
    }
    close(fd);
    free(buf);

    char dest[FBMQ_MAX_PATH];
    if (msg.header.retry_count > q->max_retries) {
        if (path_fmt(dest, sizeof(dest), "%s/failed/%s", q->root, orig) != 0) {
            fbmq_message_free(&msg); return -1;
        }
    } else {
        char bucket[3];
        fbmq_bucket(msg.header.id, bucket);
        int prc;
        if (q->use_priority_dirs) {
            prc = path_fmt(dest, sizeof(dest), "%s/pending/%s/%d-%s/%s",
                           q->root, bucket, (int)msg.header.priority,
                           fbmq_priority_str(msg.header.priority), orig);
        } else {
            prc = path_fmt(dest, sizeof(dest), "%s/pending/%s/%s",
                           q->root, bucket, orig);
        }
        if (prc != 0) { fbmq_message_free(&msg); return -1; }
    }

    int rc = rename(claimed_path, dest);
    fbmq_message_free(&msg);
    return rc;
}

/* ────────────────────────────────────────────
 * Reapers (collect-then-act to avoid readdir-while-modifying)
 * ──────────────────────────────────────────── */

/* Collect .md filenames from a directory into a heap-allocated array.
 * Caller must free each entry and the array itself. */
static char **collect_md_entries(const char *dir, int *count_out)
{
    DIR *d = opendir(dir);
    if (!d) { *count_out = 0; return NULL; }

    int cap = 64, n = 0;
    char **entries = malloc((size_t)cap * sizeof(char *));
    if (!entries) { closedir(d); *count_out = 0; return NULL; }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        size_t nlen = strlen(ent->d_name);
        if (nlen <= 3 || strcmp(ent->d_name + nlen - 3, ".md") != 0)
            continue;
        if (n >= cap) {
            int newcap = cap * 2;
            char **tmp = realloc(entries, (size_t)newcap * sizeof(char *));
            if (!tmp) break;
            entries = tmp;
            cap = newcap;
        }
        entries[n] = strdup(ent->d_name);
        if (!entries[n]) break;
        n++;
    }
    closedir(d);
    *count_out = n;
    return entries;
}

static void free_entries(char **entries, int count)
{
    if (!entries) return;
    for (int i = 0; i < count; i++) free(entries[i]);
    free(entries);
}

int fbmq_reap(fbmq_queue_t *q)
{
    char proc_dir[FBMQ_MAX_PATH];
    if (path_fmt(proc_dir, sizeof(proc_dir), "%s/processing", q->root) != 0) return -1;

    int count = 0;
    char **entries = collect_md_entries(proc_dir, &count);
    if (!entries && count == 0 && errno != 0) return -1;

    time_t now = time(NULL);
    int reaped = 0;

    for (int i = 0; i < count; i++) {
        char *endptr;
        long long raw_ts = strtoll(entries[i], &endptr, 10);
        if (raw_ts <= 0 || *endptr != '.') continue;

        /* Claim timestamp is <sec><09nsec> — extract seconds by dividing out nanoseconds */
        time_t claimed_at = (time_t)(raw_ts / 1000000000LL);
        if (now - claimed_at > q->lease_timeout) {
            char full[FBMQ_MAX_PATH];
            if (path_fmt(full, sizeof(full), "%s/%s", proc_dir, entries[i]) != 0)
                continue;
            if (fbmq_fail(q, full) == 0)
                reaped++;
        }
    }

    free_entries(entries, count);

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
    if (!entries) return 0;

    int expired = 0;
    for (int i = 0; i < count; i++) {
        char full[FBMQ_MAX_PATH];
        if (path_fmt(full, sizeof(full), "%s/%s", dir, entries[i]) != 0)
            continue;

        fbmq_message_t msg = {0};
        if (fbmq_parse_headers(full, &msg) != 0) continue;

        if (msg.header.ttl > 0 && msg.header.created_at[0]) {
            struct tm tm = {0};
            if (strptime(msg.header.created_at, "%Y-%m-%dT%H:%M:%S", &tm)) {
                time_t created = portable_timegm(&tm);
                if (created > 0 && (now - created) > msg.header.ttl) {
                    char dest[FBMQ_MAX_PATH];
                    if (path_fmt(dest, sizeof(dest), "%s/failed/%s",
                                 q->root, entries[i]) != 0) {
                        fbmq_message_free(&msg);
                        continue;
                    }
                    if (rename(full, dest) == 0)
                        expired++;
                }
            }
        }
        fbmq_message_free(&msg);
    }
    free_entries(entries, count);
    return expired;
}

/* FIX #16: TTL expiry reaper */
int fbmq_reap_ttl(fbmq_queue_t *q)
{
    int expired = 0;
    time_t now = time(NULL);

    for (int b = 0; b < FBMQ_BUCKET_COUNT; b++) {
        char bdir[FBMQ_MAX_PATH];

        if (q->use_priority_dirs) {
            for (int p = 0; p < FBMQ_PRIO_COUNT; p++) {
                if (path_fmt(bdir, sizeof(bdir), "%s/pending/%02x/%d-%s",
                             q->root, b, p, fbmq_priority_str(p)) != 0) continue;
                expired += reap_ttl_dir(q, bdir, now);
            }
        } else {
            if (path_fmt(bdir, sizeof(bdir), "%s/pending/%02x", q->root, b) != 0) continue;
            expired += reap_ttl_dir(q, bdir, now);
        }
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
    if (!entries && count == 0) return -1;

    time_t now = time(NULL);
    int purged = 0;

    for (int i = 0; i < count; i++) {
        char full[FBMQ_MAX_PATH];
        if (path_fmt(full, sizeof(full), "%s/%s", done_dir, entries[i]) != 0)
            continue;

        struct stat st;
        if (stat(full, &st) != 0) continue;

        if (now - st.st_mtime >= max_age_seconds) {
            if (unlink(full) == 0) purged++;
        }
    }

    free_entries(entries, count);
    return purged;
}
