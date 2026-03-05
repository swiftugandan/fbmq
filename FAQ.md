# fbmq FAQ

Common questions about using fbmq for AI agent workflows — task management,
context passing, and persistent memory — answered in terms of the primitives
fbmq already provides.

Every answer here follows the project's principles: the filesystem is enough,
compose with Unix pipes, no daemons, no cleverness until measurements demand
it.

---

## Task Management

### How do I wait for a message instead of polling?

Use `inotifywait` (Linux) or `fswatch` (macOS) as your event loop:

```bash
while true; do
  path=$(fbmq pop "$QUEUE" 2>/dev/null) && break
  inotifywait -qq -e moved_to "$QUEUE/pending/"
done
cat "$path"
```

fbmq intentionally has no blocking pop — *"No background threads. No hidden
garbage collection."* The kernel's filesystem notification API is the event
loop.

### How do I express task dependencies (task B waits for task A)?

Put dependency IDs in the `Custom:` block and check at pop time:

```markdown
Correlation-Id: plan-42
Tags: step-2
Custom:
  depends-on: a3f2e1b4c5d6a7b8c9d0e1f2a3b4c5d6
```

A shell scheduler enforces the dependency:

```bash
msg=$(fbmq pop "$QUEUE")
dep=$(grep '  depends-on:' "$msg" | awk '{print $2}')
if [ -n "$dep" ]; then
  find "$QUEUE/done" -name "*${dep}*" | grep -q . || {
    fbmq nack "$QUEUE" "$msg"
    exit 0
  }
fi
# All dependencies met — process the task
```

Enforcement stays in the shell — *"Explicit over automatic."*

### How do I fan out a plan into parallel sub-tasks?

Push N messages in a loop with a shared `Correlation-Id`:

```bash
PLAN_ID=$(uuidgen)
for subtask in "implement auth" "write tests" "update docs"; do
  echo "$subtask" | fbmq push "$WORKER_QUEUE" --correlation-id "$PLAN_ID"
done
```

Each sub-task is independently claimable, retryable, and ackable. The
orchestrator collects results by scanning `done/` for the correlation ID:

```bash
grep -rl "Correlation-Id: $PLAN_ID" "$WORKER_QUEUE/done/"
```

fbmq is a work queue: one message, one consumer. Fan-out is N pushes.

### How do I route tasks to specific agents?

Use **one queue per task type** instead of selective consume from a single
queue:

```
queues/
  coding/        ← coding agent pops here
  review/        ← review agent pops here
  planning/      ← orchestrator pops here
```

Routing is the pusher's responsibility. Priority subdirs handle urgency
within each queue. This follows the Unix model — route at write time, not
read time.

### How do I implement request-reply (send a task, get a result)?

Use two queues and `Correlation-Id`. Optionally stash the reply queue
name in `Custom:` so workers know where to send results:

```bash
# Orchestrator pushes a task with a reply-to convention
echo "Summarize this document" \
  | fbmq push tasks/ --correlation-id req-001 --custom "reply-to: results/"

# Worker pops, does the work, pushes the result to the reply queue
TASK=$(fbmq pop tasks/)
CORR=$(grep '^Correlation-Id:' "$TASK" | cut -d' ' -f2)
REPLY_Q=$(grep '  reply-to:' "$TASK" | awk '{print $2}')
echo "Here is the summary..." | fbmq push "$REPLY_Q" --correlation-id "$CORR"
fbmq ack tasks/ "$TASK"

# Orchestrator watches for the reply
inotifywait -qq -e moved_to results/pending/
REPLY=$(fbmq pop results/)
```

No special mechanism needed — `Custom:` carries the convention, two queues
and a shared ID do the rest.

---

## Context & Memory

### How do I pass large context (conversation history, code, embeddings)?

Use the **message body**. Headers are for routing metadata (limited to
fixed-size buffers); the body supports up to 64 MiB of Markdown content:

```bash
cat <<'EOF' | fbmq push "$QUEUE" -p normal
# Refactor auth module

## Context

The current implementation uses session cookies. Migrate to JWT.

## Files to modify

- src/auth/login.ts
- src/auth/middleware.ts

## Conversation history

User: "The auth module needs to support SSO"
Agent: "I'll add SAML support to the login flow..."
EOF
```

If you need structured data, put JSON or YAML in the body. The RFC 822
headers handle routing; the Markdown body carries the payload.

### How do I store intermediate results while processing a task?

Don't mutate the message in `processing/`. Use **sidecar files**:

```bash
TASK=$(fbmq pop "$QUEUE")
TASK_ID=$(basename "$TASK" .md | sed 's/^[0-9]*\.//')

# Write intermediate results alongside the task
echo "$tool_output" > "$QUEUE/processing/${TASK_ID}.step1.md"
echo "$analysis"    > "$QUEUE/processing/${TASK_ID}.step2.md"

# Clean up sidecars when done
rm -f "$QUEUE/processing/${TASK_ID}".step*.md
fbmq ack "$QUEUE" "$TASK"
```

This preserves the single-write atomicity model — no partial writes, no
append API, no corruption risk.

### How do I use `done/` as agent memory / audit log?

Completed messages persist in `done/` until purged. Query them with standard
tools:

```bash
# Find all completed tasks for a correlation ID
grep -rl "Correlation-Id: plan-42" "$QUEUE/done/"

# Find tasks tagged with "coding"
grep -rl "Tags:.*coding" "$QUEUE/done/"

# Read the body of a completed task
fbmq cat "$QUEUE/done/a3f2e1b4...md"

# Count completed tasks from the last hour
find "$QUEUE/done/" -name '*.md' -mmin -60 | wc -l
```

Control retention with `fbmq purge`:

```bash
# Keep 7 days of history (default)
fbmq purge "$QUEUE" -a 604800

# Keep 24 hours
fbmq purge "$QUEUE" -a 86400
```

### How do I look up a specific message by ID?

The message ID is embedded in every filename. Use `find`:

```bash
find "$QUEUE" -name "*a3f2e1b4c5d6a7b8c9d0e1f2a3b4c5d6*"
```

This searches all lifecycle directories (`pending/`, `processing/`, `done/`,
`failed/`) and returns the current location. The filesystem is the index.

### How do I inspect failed tasks to learn from errors?

`failed/` is the dead-letter queue. Browse it like any directory:

```bash
ls "$QUEUE/failed/"
fbmq inspect "$QUEUE/failed/a3f2e1b4...md"   # metadata
fbmq cat "$QUEUE/failed/a3f2e1b4...md"        # body

# Retry a specific failed task manually
mv "$QUEUE/failed/a3f2e1b4...md" "$QUEUE/pending/a3f2e1b4...md"
```

The `Retry-Count` header shows how many attempts were made before
dead-lettering.

---

## Operations

### Will `done/` grow unbounded and slow things down?

Run `fbmq purge` on a schedule:

```crontab
* * * * * fbmq-reaper /var/queue/jobs 300 604800
```

The reaper reclaims stale processing messages and purges `done/` entries
older than the configured age (604800 seconds = 7 days). ext4 with
`dir_index` handles 100K+ entries per directory — measure before optimizing.

### Can multiple agents share the same queue safely?

Yes. That is the core design. Each agent calls `fbmq pop` independently.
`rename(2)` serializes access — exactly one consumer wins, others get ENOENT
and retry automatically. No locks, no coordination protocol.

Each process needs its own `fbmq_queue_t` handle (the struct is not
thread-safe), but cross-process concurrency is safe by design.

### Can I use fbmq across multiple machines?

fbmq operates on a single filesystem. For multi-host agents, your options:

- **Shared filesystem** (NFS, EFS, GlusterFS) — fbmq works if `rename(2)`
  is atomic on the mount (NFS v3+ with same-directory renames)
- **Thin HTTP shim** — wrap fbmq CLI in a REST endpoint
- **Sync script** — `rsync` completed results between hosts

If you need true multi-node replication, you need a distributed broker.
The manifesto is explicit: *"If you need messages replicated across machines,
you need a distributed broker."*

### How do I handle backpressure?

`max_pending` (set at `fbmq init --max-pending N`) returns `ENOSPC` when the
queue is full. Producers check the exit code:

```bash
if ! echo "task" | fbmq push "$QUEUE"; then
  echo "Queue full, backing off" >&2
  sleep 5
fi
```

Default is 10,000 pending messages. Use `--max-pending 0` for unlimited.

### What metadata can I put in the `Custom:` header?

The `Custom:` block supports RFC 822 continuation lines (indented lines
append to the block), up to 4096 bytes:

```markdown
Custom:
  agent: claude-3.5
  model-temperature: 0.7
  tool-calls: 3
  last-error: rate limit exceeded
```

For structured data larger than 4KB, use the message body instead.
