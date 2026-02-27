/*
 * fbmq_main.c — CLI entry point
 *
 * FIX #1:  pop prints ONLY the claimed path to stdout.
 *          The message is on disk — use cat/fbmq inspect to read it.
 * FIX #6:  --no-fsync flag on push for tmpfs mode.
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
        "  init    <dir> [--priority]             Initialize a queue\n"
        "  push    <dir> [opts] [file|-]           Enqueue a message\n"
        "  pop     <dir>                           Claim next message (prints path)\n"
        "  ack     <dir> <path>                    Complete a message\n"
        "  nack    <dir> <path>                    Return for retry / dead-letter\n"
        "  depth   <dir>                           Print queue depth\n"
        "  reap    <dir> [-l secs]                 Reclaim stale processing msgs\n"
        "  purge   <dir> [-a secs]                 Delete old done messages\n"
        "  sync    <dir>                           Flush deferred dir fsyncs\n"
        "  inspect <file>                          Show message metadata\n"
        "  cat     <file>                          Print message body only\n"
        "  version                                 Print version\n"
        "\n"
        "Push options:\n"
        "  -p, --priority <critical|high|normal|low>\n"
        "  -t, --ttl <seconds>\n"
        "  -c, --correlation-id <id>\n"
        "  -T, --tag <tag>              (repeatable)\n"
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

static fbmq_queue_t make_queue(const char *dir)
{
    fbmq_queue_t q;
    fbmq_queue_defaults(&q);
    snprintf(q.root, sizeof(q.root), "%s", dir);
    q.use_priority_dirs = fbmq_detect_priority(dir);
    return q;
}

/* ── Commands ── */

static int cmd_init(int argc, char **argv)
{
    if (argc < 1) { fprintf(stderr, "fbmq init: missing queue-dir\n"); return 1; }

    int use_prio = 0;
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--priority") == 0) use_prio = 1;

    fbmq_queue_t q;
    fbmq_queue_defaults(&q);
    q.use_priority_dirs = use_prio;

    if (fbmq_init(&q, argv[0]) != 0) {
        fprintf(stderr, "fbmq init: %s: %s\n", argv[0], strerror(errno));
        return 1;
    }

    fprintf(stderr, "Initialized queue at %s (%d buckets%s)\n",
            argv[0], FBMQ_BUCKET_COUNT, use_prio ? ", priority dirs" : "");
    return 0;
}

static int cmd_push(int argc, char **argv)
{
    if (argc < 1) { fprintf(stderr, "fbmq push: missing queue-dir\n"); return 1; }

    const char *dir = argv[0];
    fbmq_priority_t prio = FBMQ_PRIO_NORMAL;
    int ttl = 0, no_fsync = 0, batch_fsync = 0;
    const char *corr_id = NULL, *created_by = NULL, *infile = NULL;
    char tags[1024] = {0};

    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--priority") == 0) && i+1 < argc)
            prio = fbmq_priority_parse(argv[++i]);
        else if ((strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--ttl") == 0) && i+1 < argc) {
            char *end;
            long v = strtol(argv[++i], &end, 10);
            if (*end != '\0' || v < 0 || v > INT_MAX) {
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
            size_t pos = strlen(tags);
            if (pos > 0)
                pos += (size_t)snprintf(tags + pos, sizeof(tags) - pos, ", ");
            snprintf(tags + pos, sizeof(tags) - pos, "%s", argv[++i]);
        }
        else if (strcmp(argv[i], "--no-fsync") == 0)
            no_fsync = 1;
        else if (strcmp(argv[i], "--batch-fsync") == 0)
            batch_fsync = 1;
        else if (argv[i][0] != '-')
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

    if (corr_id) snprintf(msg.header.correlation_id, sizeof(msg.header.correlation_id), "%s", corr_id);
    if (tags[0]) snprintf(msg.header.tags, sizeof(msg.header.tags), "%s", tags);
    if (created_by) snprintf(msg.header.created_by, sizeof(msg.header.created_by), "%s", created_by);

    if (fbmq_enqueue(&q, &msg) != 0) {
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
        return 1;
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
            long v = strtol(argv[++i], &end, 10);
            if (*end != '\0' || v < 0 || v > INT_MAX) {
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
        if (t < 0) t = 0;
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
            long v = strtol(argv[++i], &end, 10);
            if (*end != '\0' || v < 0 || v > INT_MAX) {
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
    char bucket[3];
    fbmq_bucket(msg.header.id, bucket);
    printf("Bucket:         %s\n", bucket);
    printf("Created:        %s\n", msg.header.created_at);
    printf("Created by:     %s\n", msg.header.created_by);
    printf("Priority:       %s\n", fbmq_priority_str(msg.header.priority));
    printf("Retry count:    %d\n", msg.header.retry_count);
    if (msg.header.ttl > 0)
        printf("TTL:            %d seconds\n", msg.header.ttl);
    if (msg.header.correlation_id[0])
        printf("Correlation ID: %s\n", msg.header.correlation_id);
    if (msg.header.tags[0])
        printf("Tags:           %s\n", msg.header.tags);
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
