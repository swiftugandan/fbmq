# fbmq — A message queue for local agents

A file-based queue for local AI agents and workers: no broker, no cloud—just
directories and Markdown. Every message is a file, every queue is a directory,
and `rename(2)` is the sole coordination primitive. Ideal for local LLM
pipelines, cron workers, and multi-step agent workflows.

- [NotebookLM](https://notebooklm.google.com/notebook/04836816-b49d-49f1-b451-45ef65d39035) — AI notebook over project sources
- [DeepWiki](https://deepwiki.com/swiftugandan/fbmq) — Codebase index and navigation

## Quick Start

```bash
make
sudo make install

fbmq init /var/queue/jobs

echo "# Deploy v2.1" | fbmq push /var/queue/jobs -p high
# a3f2e1b4c5d6a7b8c9d0e1f2a3b4c5d6

CLAIMED=$(fbmq pop /var/queue/jobs)
cat "$CLAIMED"                           # read the message
fbmq ack /var/queue/jobs "$CLAIMED"      # done
fbmq nack /var/queue/jobs "$CLAIMED"     # retry
```

## Messages are Markdown

```markdown
Id: a3f2e1b4c5d6a7b8c9d0e1f2a3b4c5d6
Created-At: 2026-02-26T14:30:01.123456789Z
Created-By: 28431@worker-12
Priority: high
Retry-Count: 0
TTL: 3600
Tags: deploy, urgent
Correlation-Id: req-abc-123

# Deploy v2.1

Expedited rollout requested.
```

The filename **is** the ID. The first two characters **are** the bucket.

## Directory Layout

```
/var/queue/jobs/
    pending/00..ff/    256 hash-sharded buckets
    processing/        Claimed (timestamp-prefixed filenames)
    done/              Completed
    failed/            Dead-letter
    .tmp/              Atomic write staging
```

## Commands

| Command | Description |
|---------|-------------|
| `fbmq init <dir> [--priority]` | Create a queue |
| `fbmq push <dir> [opts] [file]` | Enqueue (prints ID) |
| `fbmq pop <dir>` | Claim next message (prints path) |
| `fbmq ack <dir> <path>` | Mark done |
| `fbmq nack <dir> <path>` | Return for retry / dead-letter |
| `fbmq depth <dir>` | Queue depth (full scan of all 256 buckets + processing) |
| `fbmq inspect <file>` | Show metadata |
| `fbmq cat <file>` | Print body only (strip frontmatter) |
| `fbmq reap <dir> [-l secs]` | Reclaim stale + expire TTL |
| `fbmq purge <dir> [-a secs]` | Delete old done messages |

## Operations with Unix tools

```bash
grep -rl "Priority: critical" pending/     # find critical
cat pending/a3/a3f2...md                   # inspect
mv failed/a3f2...md pending/a3/a3f2...md   # manual retry
inotifywait -mr pending/                   # monitor
find done/ -mtime +7 -delete               # purge
```


### No SDK, no MCP server, no wrapper

The CLI _is_ the integration API. Any agent that can run a shell command —
Claude Code, OpenClaw, a cron job, a bash script — can push, pop, ack, and
nack without importing a library or standing up a server. stdin, stdout, and
exit codes are the entire contract. Unix made this decision fifty years ago;
fbmq just inherits it.

#### Step-by-step: Claude Code as a task producer and consumer

This walkthrough assumes fbmq is built and installed (`make && sudo make install`).
Claude Code uses its **Bash** tool for every step — no SDK, no plugin, nothing
else to set up.

**Step 1 — Create a queue.** Tell Claude Code:

> Create a queue at /tmp/demo

Claude Code runs:

```bash
fbmq init /tmp/demo
```

This creates the directory structure (`pending/`, `processing/`, `done/`,
`failed/`, `.tmp/`) inside `/tmp/demo`.

**Step 2 — Push a task.** Tell Claude Code:

> Push a high-priority task to summarize a document

Claude Code runs:

```bash
echo "Summarize the Q4 earnings report at ~/documents/q4.pdf" \
  | fbmq push /tmp/demo -p high
```

`push` reads the message body from stdin and prints the 32-character message
ID to stdout:

```
a3f2e1b4c5d6a7b8c9d0e1f2a3b4c5d6
```

The message is now a Markdown file sitting in one of the 256 `pending/`
buckets.

**Step 3 — Pop the next task.** Tell Claude Code:

> Pop the next task from the queue

Claude Code runs:

```bash
TASK=$(fbmq pop /tmp/demo)
echo "$TASK"
```

`pop` atomically claims the highest-priority pending message and prints its
full filesystem path:

```
/tmp/demo/processing/1719500000-a3f2e1b4c5d6a7b8c9d0e1f2a3b4c5d6.md
```

The message has moved from `pending/` to `processing/`. No other consumer
can claim it.

**Step 4 — Read the task body.** Tell Claude Code:

> Read that task so I can see what it says

Claude Code runs:

```bash
fbmq cat "$TASK"
```

`cat` strips the RFC 822 headers and prints only the body:

```
Summarize the Q4 earnings report at ~/documents/q4.pdf
```

(To see the full headers too, use `fbmq inspect "$TASK"` instead — it prints
ID, priority, creation time, retry count, tags, and body size.)

**Step 5a — Ack on success.** After the work is done, tell Claude Code:

> Mark that task as done

Claude Code runs:

```bash
fbmq ack /tmp/demo "$TASK"
```

The message moves from `processing/` to `done/`. It is finished.

**Step 5b — Nack on failure.** If something went wrong instead:

> That task failed, nack it

Claude Code runs:

```bash
fbmq nack /tmp/demo "$TASK"
```

The message moves from `processing/` to `failed/` (the dead-letter queue).

**Step 6 — Check queue depth.** Tell Claude Code:

> How many tasks are in the queue?

Claude Code runs:

```bash
fbmq depth /tmp/demo
```

`depth` counts all pending messages across the 256 buckets and prints a
single number:

```
3
```

**Step 7 — Inspect failed tasks.** Tell Claude Code:

> List the failed tasks and show me what went wrong

Claude Code runs:

```bash
ls /tmp/demo/failed/
```

```
1719500000-a3f2e1b4c5d6a7b8c9d0e1f2a3b4c5d6.md
```

Then to see the metadata of a specific failure:

```bash
fbmq inspect /tmp/demo/failed/1719500000-a3f2e1b4c5d6a7b8c9d0e1f2a3b4c5d6.md
```

```
ID:             a3f2e1b4c5d6a7b8c9d0e1f2a3b4c5d6
Bucket:         a3
Created:        2026-02-26T14:30:01.123456789Z
Created by:     28431@worker-12
Priority:       high
Retry count:    0
Body:           58 bytes
```

That's it. Every interaction is a plain shell command. No wrapper, no
configuration file, no running daemon. The same commands work from any agent,
any shell, any language that can exec a process.

## Using fbmq with the Pi Coding Agent

[Pi](https://github.com/mariozechner/pi-coding-agent) is an extensible AI
coding agent that speaks bash natively — which means it already knows how to
use fbmq. This repo ships a ready-made agent skill at
`.agents/skills/integrating-pi-sdk/` that teaches Pi (and compatible agents)
how to manage queues, push tasks, and process work.

### The simplest setup: drop in a skill file

Create `.pi/skills/queue/SKILL.md` in your project with fbmq's commands and
conventions. Pi reads this file on demand and uses its built-in `bash` tool
to run fbmq — zero code required.

```bash
# Copy the skill template into your project
mkdir -p .pi/skills/queue
cp .agents/skills/integrating-pi-sdk/SKILL.md .pi/skills/queue/SKILL.md
```

Then just tell Pi what you need in plain language:

> "Initialize a priority queue at .queue/ and push three tasks for the
> refactoring we discussed"

Pi reads the skill, runs `fbmq init`, `fbmq push`, and you're done.

### Going further: TypeScript extension

For tighter integration, a Pi extension registers fbmq as native LLM tools
(`queue_push`, `queue_pop`, `queue_complete`, `queue_depth`) and adds two
slash commands:

- **`/plan refactor the auth module`** — Pi analyzes the code and pushes
  prioritized sub-tasks to the queue
- **`/work`** — Pi pops tasks one by one, does the work, and acks each on
  completion

See `.agents/skills/integrating-pi-sdk/reference/extension.md` for the full
TypeScript source you can drop into `.pi/extensions/fbmq.ts`.

### Headless workers: fully automated processing

For background automation, embed Pi as a Node.js library and drive it with
fbmq in a poll loop:

```
fbmq pop → fbmq cat → Pi session.prompt() → fbmq ack / nack
```

Run it as a daemon with `pm2` or `systemd`. Any process can push tasks to the
queue and the worker processes them autonomously. See
`.agents/skills/integrating-pi-sdk/reference/sdk-worker.md` for the complete
implementation.

### Which approach should I pick?

| I want to… | Use |
|---|---|
| Get started in 2 minutes | Skill (just a SKILL.md) |
| Have `/plan` and `/work` commands | Extension (TypeScript) |
| Run an unattended agent worker | SDK worker (Node.js) |
| Integrate from Python / C / shell | RPC mode (`pi --rpc` + fbmq CLI) |

## Cron

```crontab
* * * * * fbmq-reaper /var/queue/jobs 300 604800
```

## Build

```bash
make            # build CLI + libraries
make fbmq       # build CLI binary only
make libs       # build libfbmq.a + libfbmq.so/dylib + fbmq.pc
make test       # 50+ integration tests
make install    # install everything (binary, libs, header, man pages)
make install-bin   # install CLI binary + man pages only
make install-lib   # install libraries + header + pkg-config only
```

Requires: GCC/Clang, GNU Make, Linux or macOS. No external libraries.

## Library Usage

fbmq ships as both a CLI tool and a C library (`libfbmq.a` / `libfbmq.so`).

```c
#include <fbmq.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    fbmq_queue_t q;
    fbmq_queue_defaults(&q);

    if (fbmq_init(&q, "/var/queue/jobs") != 0)
        return 1;

    fbmq_message_t msg = {0};
    msg.header.priority = FBMQ_PRIO_HIGH;
    msg.body     = "# Deploy v2.1\n\nRoll it out.\n";
    msg.body_len = strlen(msg.body);

    fbmq_enqueue(&q, &msg);
    printf("enqueued: %s\n", msg.header.id);
    return 0;
}
```

Compile with pkg-config:

```bash
gcc -o myapp myapp.c $(pkg-config --cflags --libs fbmq)
```

Or link directly:

```bash
gcc -o myapp myapp.c -lfbmq
```

## Man pages

```bash
man fbmq            # command reference
man fbmq-message    # message format
man fbmq-design     # architecture
```

## License

MIT
