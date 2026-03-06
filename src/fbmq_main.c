/*
 * fbmq_main.c — CLI entry point
 *
 */

#include "fbmq.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <limits.h>

/* ── Usage ── */

static void usage(void)
{
    fprintf(stderr,
        "Usage: fbmq <command> [options]\n"
        "\n"
        "Commands:\n"
        "  init    <dir> [--priority] [--max-pending N]  Initialize a queue\n"
        "  push    <dir> [opts] [file|-]           Enqueue a message\n"
        "  pop     <dir>                           Claim next message (prints path)\n"
        "  ack     <dir> <path>                    Complete a message\n"
        "  nack    <dir> <path>                    Return for retry / dead-letter\n"
        "  depth   <dir>                           Print queue depth\n"
        "  reap    <dir> [-l secs] [--no-ttl]       Reclaim stale processing msgs\n"
        "  purge   <dir> [-a secs]                 Delete old done messages\n"
        "  sync    <dir>                           Flush deferred dir fsyncs\n"
        "  ready   <dir>                           List IDs ready to process\n"
        "  inspect <file>                          Show message metadata\n"
        "  cat     <file>                          Print message body only\n"
        "  version                                 Print version\n"
        "\n"
        "Push options:\n"
        "  -p, --priority <critical|high|normal|low>\n"
        "  -t, --ttl <seconds>\n"
        "  -c, --correlation-id <id>\n"
        "  -r, --reply-to <queue-path>\n"
        "  -T, --tag <tag>              (repeatable)\n"
        "  -d, --depends-on <id>        (repeatable)\n"
        "  -b, --created-by <name>\n"
        "      --no-fsync               Skip fsync (tmpfs mode)\n"
        "      --batch-fsync            Defer dir fsync (use with fbmq sync)\n"
        "\n"
        "Pop outputs the claimed file path to stdout. Read the file with cat:\n"
        "  CLAIMED=$(fbmq pop /var/queue/jobs)\n"
        "  cat \"$CLAIMED\"                 # read the message\n"
        "  fbmq ack /var/queue/jobs \"$CLAIMED\"\n"
    );
    exit(1);
}

/* ── Helpers ── */

static char *read_all(int fd, size_t *len)
{
    size_t cap = 4096, n = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;

    ssize_t r;
    for (;;) {
        r = read(fd, buf + n, cap - n);
        if (r < 0) {
            if (errno == EINTR) continue;
            free(buf);
            return NULL;
        }
        if (r == 0) break;
        n += (size_t)r;
        if (n > FBMQ_MAX_MESSAGE_SIZE) {
            fprintf(stderr, "fbmq: input exceeds %d MiB limit\n",
                    (int)(FBMQ_MAX_MESSAGE_SIZE / (1024 * 1024)));
            free(buf);
            errno = EFBIG;
            return NULL;
        }
        if (n >= cap - 1) {
            if (cap > SIZE_MAX / 2) { errno = ENOMEM; free(buf); return NULL; }
            cap *= 2;
            char *tmp = realloc(buf, cap);
            if (!tmp) { free(buf); return NULL; }
            buf = tmp;
        }
    }
    buf[n] = '\0';
    *len = n;
    return buf;
}

static char *read_file(const char *path, size_t *len)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    char *buf = read_all(fd, len);
    close(fd);
    return buf;
}

static void load_max_pending(fbmq_queue_t *q);

/* Reject header values containing newlines or control characters (header injection) */
static int validate_header_value(const char *val, const char *name)
{
    for (const char *p = val; *p; p++) {
        if (*p == '\n' || *p == '\r' || (*p < 0x20 && *p != '\t')) {
            fprintf(stderr, "fbmq push: invalid character in %s (newlines/control chars not allowed)\n", name);
            return -1;
        }
    }
    return 0;
}

/* Append a value to a comma-separated buffer (e.g. tags, depends-on). Returns 0 on success. */
static int append_csv(char *buf, size_t bufsz, const char *item, const char *field_name)
{
    size_t pos = strlen(buf);
    int needed = 0;
    if (pos > 0)
        needed = snprintf(buf + pos, bufsz - pos, ", ");
    if (needed < 0 || pos + (size_t)needed >= bufsz) {
        fprintf(stderr, "fbmq push: %s exceeds %zu byte limit\n", field_name, bufsz - 1);
        return -1;
    }
    pos += (size_t)needed;
    int wrote = snprintf(buf + pos, bufsz - pos, "%s", item);
    if (wrote < 0 || pos + (size_t)wrote >= bufsz) {
        fprintf(stderr, "fbmq push: %s exceeds %zu byte limit\n", field_name, bufsz - 1);
        return -1;
    }
    return 0;
}

static fbmq_queue_t make_queue(const char *dir)
{
    fbmq_queue_t q;
    fbmq_queue_defaults(&q);
    int n = snprintf(q.root, sizeof(q.root), "%s", dir);
    if (n < 0 || (size_t)n >= sizeof(q.root)) {
        fprintf(stderr, "fbmq: queue path too long\n");
        exit(1);
    }
    q.use_priority_dirs = fbmq_detect_priority(dir);
    load_max_pending(&q);

    return q;
}

/* Read persisted max_pending from .meta/ into queue handle */
static void load_max_pending(fbmq_queue_t *q)
{
    char meta_path[FBMQ_MAX_PATH];
    int mp = snprintf(meta_path, sizeof(meta_path), "%s/.meta/max_pending", q->root);
    if (mp < 0 || (size_t)mp >= sizeof(meta_path)) return;
    size_t meta_len = 0;
    char *meta_buf = read_file(meta_path, &meta_len);
    if (meta_buf) {
        char *end;
        long long v = strtoll(meta_buf, &end, 10);
        if (end != meta_buf && v >= 0)
            q->max_pending = (int64_t)v;
        else
            fprintf(stderr, "fbmq: warning: ignoring corrupt .meta/max_pending\n");
        free(meta_buf);
    }
}

/* ── Commands ── */

static int cmd_init(int argc, char **argv)
{
    if (argc < 1) { fprintf(stderr, "fbmq init: missing queue-dir\n"); return 1; }

    int use_prio = 0;
    int64_t max_pending = FBMQ_DEFAULT_MAX_PENDING;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--priority") == 0)
            use_prio = 1;
        else if (strcmp(argv[i], "--max-pending") == 0 && i+1 < argc) {
            char *end;
            errno = 0;
            long long v = strtoll(argv[++i], &end, 10);
            if (*end != '\0' || v < 0 || errno == ERANGE) {
                fprintf(stderr, "fbmq init: invalid max-pending: %s\n", argv[i]);
                return 1;
            }
            max_pending = (int64_t)v;
        }
    }

    fbmq_queue_t q;
    fbmq_queue_defaults(&q);
    q.use_priority_dirs = use_prio;
    q.max_pending = max_pending;

    if (fbmq_init(&q, argv[0]) != 0) {
        fprintf(stderr, "fbmq init: %s: %s\n", argv[0], strerror(errno));
        return 1;
    }

    if (max_pending > 0)
        fprintf(stderr, "Initialized queue at %s (max-pending: %lld%s)\n",
                argv[0], (long long)max_pending, use_prio ? ", priority dirs" : "");
    else
        fprintf(stderr, "Initialized queue at %s (max-pending: unlimited%s)\n",
                argv[0], use_prio ? ", priority dirs" : "");
    return 0;
}

static int cmd_push(int argc, char **argv)
{
    if (argc < 1) { fprintf(stderr, "fbmq push: missing queue-dir\n"); return 1; }

    const char *dir = argv[0];
    fbmq_priority_t prio = FBMQ_PRIO_NORMAL;
    int ttl = 0, no_fsync = 0, batch_fsync = 0;
    const char *corr_id = NULL, *reply_to = NULL, *created_by = NULL, *infile = NULL;
    char tags[1024] = {0};
    char depends_on[2048] = {0};

    int end_of_opts = 0;
    for (int i = 1; i < argc; i++) {
        if (!end_of_opts && strcmp(argv[i], "--") == 0) {
            end_of_opts = 1;
            continue;
        }
        if (end_of_opts) {
            infile = argv[i];
            continue;
        }
        if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--priority") == 0) && i+1 < argc)
        {
            int parsed = fbmq_priority_parse(argv[++i]);
            if (parsed < 0) {
                fprintf(stderr, "fbmq push: invalid priority '%s' (expected: critical, high, normal, low)\n", argv[i]);
                return 1;
            }
            prio = (fbmq_priority_t)parsed;
        }
        else if ((strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--ttl") == 0) && i+1 < argc) {
            char *end;
            errno = 0;
            long v = strtol(argv[++i], &end, 10);
            if (*end != '\0' || v < 0 || v > INT_MAX || errno == ERANGE) {
                fprintf(stderr, "fbmq push: invalid TTL: %s\n", argv[i]);
                return 1;
            }
            ttl = (int)v;
        }
        else if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--correlation-id") == 0) && i+1 < argc)
            corr_id = argv[++i];
        else if ((strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--created-by") == 0) && i+1 < argc)
            created_by = argv[++i];
        else if ((strcmp(argv[i], "-T") == 0 || strcmp(argv[i], "--tag") == 0) && i+1 < argc) {
            if (append_csv(tags, sizeof(tags), argv[++i], "tags") != 0) return 1;
        }
        else if ((strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--reply-to") == 0) && i+1 < argc)
            reply_to = argv[++i];
        else if ((strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--depends-on") == 0) && i+1 < argc) {
            if (append_csv(depends_on, sizeof(depends_on), argv[++i], "depends-on") != 0) return 1;
        }
        else if (strcmp(argv[i], "--no-fsync") == 0)
            no_fsync = 1;
        else if (strcmp(argv[i], "--batch-fsync") == 0)
            batch_fsync = 1;
        else if (argv[i][0] == '-') {
            fprintf(stderr, "fbmq push: unrecognized option '%s'\n", argv[i]);
            return 1;
        }
        else
            infile = argv[i];
    }

    size_t body_len = 0;
    char *body;
    if (infile && strcmp(infile, "-") != 0)
        body = read_file(infile, &body_len);
    else
        body = read_all(STDIN_FILENO, &body_len);

    if (!body) {
        if (errno == EFBIG)
            fprintf(stderr, "fbmq push: payload too large\n");
        else
            fprintf(stderr, "fbmq push: failed to read input: %s\n", strerror(errno));
        return 1;
    }

    if (no_fsync && batch_fsync) {
        fprintf(stderr, "fbmq push: --no-fsync and --batch-fsync are mutually exclusive\n");
        free(body);
        return 1;
    }

    fbmq_queue_t q = make_queue(dir);
    if (no_fsync)
        q.fsync_mode = FBMQ_FSYNC_NONE;
    else if (batch_fsync)
        q.fsync_mode = FBMQ_FSYNC_BATCH;

    fbmq_message_t msg = {0};
    msg.header.priority = prio;
    msg.header.ttl = ttl;
    msg.body = body;
    msg.body_len = body_len;

    if (corr_id) {
        if (validate_header_value(corr_id, "correlation-id") != 0) { free(body); return 1; }
        snprintf(msg.header.correlation_id, sizeof(msg.header.correlation_id), "%s", corr_id);
    }
    if (reply_to) {
        if (validate_header_value(reply_to, "reply-to") != 0) { free(body); return 1; }
        snprintf(msg.header.reply_to, sizeof(msg.header.reply_to), "%s", reply_to);
    }
    if (tags[0]) {
        if (validate_header_value(tags, "tags") != 0) { free(body); return 1; }
        snprintf(msg.header.tags, sizeof(msg.header.tags), "%s", tags);
    }
    if (depends_on[0]) {
        if (validate_header_value(depends_on, "depends-on") != 0) { free(body); return 1; }
        snprintf(msg.header.depends_on, sizeof(msg.header.depends_on), "%s", depends_on);
    }
    if (created_by) {
        if (validate_header_value(created_by, "created-by") != 0) { free(body); return 1; }
        snprintf(msg.header.created_by, sizeof(msg.header.created_by), "%s", created_by);
    }

    if (fbmq_enqueue(&q, &msg) != 0) {
        if (errno == ENOSPC) {
            fprintf(stderr, "fbmq push: queue full (max-pending reached)\n");
            free(body);
            return 2;
        }
        fprintf(stderr, "fbmq push: enqueue failed: %s\n", strerror(errno));
        free(body);
        return 1;
    }

    /* In batch mode, flush dir fsyncs so standalone pushes are still durable */
    if (batch_fsync) {
        if (fbmq_sync(&q) != 0) {
            fprintf(stderr, "fbmq push: sync failed: %s\n", strerror(errno));
            free(body);
            return 1;
        }
    }

    /* Print only the ID to stdout */
    printf("%s\n", msg.header.id);
    free(body);
    return 0;
}

static int cmd_sync(int argc, char **argv)
{
    if (argc < 1) { fprintf(stderr, "fbmq sync: missing queue-dir\n"); return 1; }
    fbmq_queue_t q = make_queue(argv[0]);
    if (fbmq_sync(&q) != 0) {
        fprintf(stderr, "fbmq sync: %s\n", strerror(errno));
        return 1;
    }
    return 0;
}

static int cmd_pop(int argc, char **argv)
{
    if (argc < 1) { fprintf(stderr, "fbmq pop: missing queue-dir\n"); return 1; }

    fbmq_queue_t q = make_queue(argv[0]);
    char claimed[FBMQ_MAX_PATH];

    int rc = fbmq_dequeue(&q, claimed, sizeof(claimed));
    if (rc == 1) return 1;  /* empty, silent */
    if (rc < 0) {
        fprintf(stderr, "fbmq pop: %s\n", strerror(errno));
        return 2;
    }

    /*
     * FIX #1: Print ONLY the claimed path to stdout.
     * The message is already a file on disk. Read it with:
     *   cat "$CLAIMED"
     *   fbmq inspect "$CLAIMED"
     *   fbmq cat "$CLAIMED"
     */
    printf("%s\n", claimed);
    return 0;
}

static int cmd_ack(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "fbmq ack: usage: fbmq ack <dir> <path>\n"); return 1; }
    fbmq_queue_t q = make_queue(argv[0]);
    if (fbmq_complete(&q, argv[1]) != 0) {
        fprintf(stderr, "fbmq ack: %s\n", strerror(errno));
        return 1;
    }
    return 0;
}

static int cmd_nack(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "fbmq nack: usage: fbmq nack <dir> <path>\n"); return 1; }
    fbmq_queue_t q = make_queue(argv[0]);
    if (fbmq_fail(&q, argv[1]) != 0) {
        fprintf(stderr, "fbmq nack: %s\n", strerror(errno));
        return 1;
    }
    return 0;
}

static int cmd_depth(int argc, char **argv)
{
    if (argc < 1) { fprintf(stderr, "fbmq depth: missing queue-dir\n"); return 1; }
    fbmq_queue_t q = make_queue(argv[0]);
    int64_t d;
    if (fbmq_depth(&q, &d) != 0) {
        fprintf(stderr, "fbmq depth: %s\n", strerror(errno));
        return 1;
    }
    printf("%lld\n", (long long)d);
    return 0;
}

static int cmd_reap(int argc, char **argv)
{
    if (argc < 1) { fprintf(stderr, "fbmq reap: missing queue-dir\n"); return 1; }

    int lease = FBMQ_DEFAULT_LEASE;
    int do_ttl = 1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-l") == 0 && i+1 < argc) {
            char *end;
            errno = 0;
            long v = strtol(argv[++i], &end, 10);
            if (*end != '\0' || v < 0 || v > INT_MAX || errno == ERANGE) {
                fprintf(stderr, "fbmq reap: invalid lease: %s\n", argv[i]);
                return 1;
            }
            lease = (int)v;
        }
        else if (strcmp(argv[i], "--no-ttl") == 0)
            do_ttl = 0;
    }

    fbmq_queue_t q = make_queue(argv[0]);
    q.lease_timeout = lease;

    int n = fbmq_reap(&q);
    if (n < 0) { fprintf(stderr, "fbmq reap: %s\n", strerror(errno)); return 1; }

    int t = 0;
    if (do_ttl) {
        t = fbmq_reap_ttl(&q);
        if (t < 0) {
            fprintf(stderr, "fbmq reap: ttl scan failed: %s\n", strerror(errno));
            return 1;
        }
    }

    fprintf(stderr, "Reaped %d stale, %d expired\n", n, t);
    return 0;
}

static int cmd_purge(int argc, char **argv)
{
    if (argc < 1) { fprintf(stderr, "fbmq purge: missing queue-dir\n"); return 1; }

    int max_age = FBMQ_DEFAULT_PURGE_AGE;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-a") == 0 && i+1 < argc) {
            char *end;
            errno = 0;
            long v = strtol(argv[++i], &end, 10);
            if (*end != '\0' || v < 0 || v > INT_MAX || errno == ERANGE) {
                fprintf(stderr, "fbmq purge: invalid age: %s\n", argv[i]);
                return 1;
            }
            max_age = (int)v;
        }
    }

    fbmq_queue_t q = make_queue(argv[0]);
    int n = fbmq_purge(&q, max_age);
    if (n < 0) { fprintf(stderr, "fbmq purge: %s\n", strerror(errno)); return 1; }
    fprintf(stderr, "Purged %d message(s)\n", n);
    return 0;
}

static int cmd_inspect(int argc, char **argv)
{
    if (argc < 1) { fprintf(stderr, "fbmq inspect: missing file\n"); return 1; }

    fbmq_message_t msg = {0};
    if (fbmq_parse_headers(argv[0], &msg) != 0) {
        fprintf(stderr, "fbmq inspect: %s: %s\n", argv[0], strerror(errno));
        return 1;
    }

    printf("ID:             %s\n", msg.header.id);
    printf("Created:        %s\n", msg.header.created_at);
    printf("Created by:     %s\n", msg.header.created_by);
    printf("Priority:       %s\n", fbmq_priority_str(msg.header.priority));
    printf("Retry count:    %d\n", msg.header.retry_count);
    if (msg.header.ttl > 0)
        printf("TTL:            %d seconds\n", msg.header.ttl);
    if (msg.header.correlation_id[0])
        printf("Correlation ID: %s\n", msg.header.correlation_id);
    if (msg.header.reply_to[0])
        printf("Reply-To:       %s\n", msg.header.reply_to);
    if (msg.header.tags[0])
        printf("Tags:           %s\n", msg.header.tags);
    if (msg.header.depends_on[0])
        printf("Depends-On:     %s\n", msg.header.depends_on);
    if (msg.header.custom[0])
        printf("Custom:\n%s", msg.header.custom);
    printf("Body:           %zu bytes\n", msg.body_len);

    fbmq_message_free(&msg);
    return 0;
}

static int cmd_cat(int argc, char **argv)
{
    if (argc < 1) { fprintf(stderr, "fbmq cat: missing file\n"); return 1; }

    fbmq_message_t msg = {0};
    if (fbmq_parse_file(argv[0], &msg) != 0) {
        fprintf(stderr, "fbmq cat: %s: %s\n", argv[0], strerror(errno));
        return 1;
    }

    if (msg.body && msg.body_len > 0)
        fwrite(msg.body, 1, msg.body_len, stdout);

    fbmq_message_free(&msg);
    return 0;
}

static int cmd_ready(int argc, char **argv)
{
    if (argc < 1) { fprintf(stderr, "fbmq ready: missing queue-dir\n"); return 1; }
    fbmq_queue_t q = make_queue(argv[0]);
    int count = 0;
    char **ids = fbmq_list_ready(&q, &count);
    if (count < 0) { fprintf(stderr, "fbmq ready: %s\n", strerror(errno)); return 1; }
    for (int i = 0; i < count; i++)
        printf("%s\n", ids[i]);
    fbmq_free_id_list(ids, count);
    return count > 0 ? 0 : 1;
}

/* ── Main ── */

int main(int argc, char **argv)
{
    if (argc < 2) usage();

    const char *cmd = argv[1];
    int sa = argc - 2;
    char **sv = argv + 2;

    if (strcmp(cmd, "init") == 0)       return cmd_init(sa, sv);
    if (strcmp(cmd, "push") == 0)       return cmd_push(sa, sv);
    if (strcmp(cmd, "sync") == 0)       return cmd_sync(sa, sv);
    if (strcmp(cmd, "pop") == 0)        return cmd_pop(sa, sv);
    if (strcmp(cmd, "ack") == 0)        return cmd_ack(sa, sv);
    if (strcmp(cmd, "nack") == 0)       return cmd_nack(sa, sv);
    if (strcmp(cmd, "depth") == 0)      return cmd_depth(sa, sv);
    if (strcmp(cmd, "reap") == 0)       return cmd_reap(sa, sv);
    if (strcmp(cmd, "purge") == 0)      return cmd_purge(sa, sv);
    if (strcmp(cmd, "ready") == 0)      return cmd_ready(sa, sv);
    if (strcmp(cmd, "inspect") == 0)    return cmd_inspect(sa, sv);
    if (strcmp(cmd, "cat") == 0)        return cmd_cat(sa, sv);
    if (strcmp(cmd, "version") == 0) {
        printf("fbmq %s\n", FBMQ_VERSION);
        return 0;
    }
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0)
        usage();

    fprintf(stderr, "fbmq: unknown command '%s'\n", cmd);
    return 1;
}
