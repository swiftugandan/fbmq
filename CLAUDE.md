# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

fbmq (File-Based Message Queue) — a C11 message queue where every message is a Markdown file with RFC 822 headers, every queue is a directory, and atomic `rename(2)` is the sole coordination primitive. No external dependencies.

## Build & Test Commands

```bash
make              # Build the fbmq binary
make test         # Run all integration tests (50+ bash tests in test/test_fbmq.sh)
make clean        # Remove build artifacts
make install      # Install to /usr/local/bin + man pages
```

There is no way to run a single test — the test suite is a single bash script. To debug a specific test, comment out others in `test/test_fbmq.sh`.

Compiler flags: `-O2 -Wall -Wextra -Wpedantic -std=c11 -D_GNU_SOURCE`

## Architecture

### Source layout

- `include/fbmq.h` — Public API: structs (`fbmq_message_t`, `fbmq_queue_t`, `fbmq_header_t`), core functions, constants
- `src/fbmq.c` — Core library: queue init, enqueue/dequeue, depth tracking, serialization, RFC 822 parser, reapers
- `src/fbmq_main.c` — CLI: commands (init, push, pop, ack, nack, depth, inspect, cat, reap, purge), signal handling
- `src/md5.c` / `src/md5.h` — Public domain MD5 (used for ID generation, not crypto)
- `scripts/fbmq-reaper` — Cron-ready shell wrapper for reap + purge
- `man/` — Man pages: fbmq.1 (commands), fbmq-message.5 (format), fbmq-design.7 (architecture)

### Queue directory structure

```
<queue-root>/
├── pending/00..ff/       256 hash-sharded buckets (optionally with priority subdirs)
├── processing/           Claimed messages (timestamp-prefixed filenames)
├── done/                 Completed messages
├── failed/               Dead-letter queue
└── .tmp/                 Atomic write staging
```

### Key design patterns

- **Lock-free concurrency**: `rename(2)` is the only coordination primitive. One consumer wins the rename; others get ENOENT and retry another bucket.
- **Message lifecycle**: `.tmp/` → `pending/` (enqueue) → `processing/` (pop) → `done/` (ack) or back to `pending/`/`failed/` (nack)
- **Count-on-read depth**: `fbmq_depth()` scans all 256 pending buckets plus `processing/` on each call. Stateless — no persistent counter files.
- **Buffered single-write serialization**: `fbmq_serialize()` builds the entire message in memory and writes with a single `write(2)` syscall.
- **Priority scanning**: When enabled, pending buckets contain subdirs `0-critical/`, `1-high/`, `2-normal/`, `3-low/`. All buckets at critical are scanned before any bucket at high.
- **ID generation**: MD5(timestamp + pid + random bytes) → 32-char hex. Entropy from `getrandom(2)` with `/dev/urandom` fallback.

### Platform considerations

- Primary target: Linux (uses `getrandom`, `O_DIRECTORY`)
- macOS support via `arc4random_buf()` fallback
- `pop` prints only the claimed file path to stdout (one tool, one output philosophy)
- `--no-fsync` flag available for tmpfs/RAM-disk deployments
