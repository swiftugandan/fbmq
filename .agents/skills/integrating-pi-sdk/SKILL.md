---
name: integrating-pi-sdk
description: "Integrate fbmq with the Pi coding agent SDK. Use when setting up task queues for Pi agents, creating Pi skills/extensions that use fbmq, building headless workers, or orchestrating multi-agent pipelines with file-based queues."
---

# Integrating fbmq with the Pi Coding Agent SDK

fbmq (file-based message queue) and the Pi coding agent (`@mariozechner/pi-coding-agent`) share a common philosophy: the shell is the universal integration layer. This skill covers four integration approaches, from zero-code to fully embedded.

## Choose Your Integration Layer

| Need | Approach | Complexity |
|---|---|---|
| Quick and simple | **Skill** — drop a SKILL.md, Pi uses bash | Zero code |
| LLM-native tools + commands | **Extension** — TypeScript, registers tools/commands | Low |
| Headless background worker | **SDK** — Node.js app embedding Pi | Medium |
| Non-Node.js integration | **RPC** — JSON over stdin/stdout | Low |
| Multi-agent pipeline | Multiple SDK workers on different queues | High |

## Layer 1: Pi Skill (Zero Code)

Drop `.pi/skills/queue/SKILL.md` into your project. Pi learns fbmq commands and uses them via its built-in `bash` tool.

**Create the skill file with this content:**

```markdown
---
name: queue
description: Manage a file-based task queue using fbmq. Use when you need to queue work, process tasks, check queue depth, or handle failures.
---

# fbmq — File-Based Message Queue

fbmq is installed at /usr/local/bin/fbmq. Queues are directories. Messages are Markdown files.

## Commands

- **Initialize a queue:** `fbmq init <dir> [--priority]`
- **Push a task:** `echo "task body" | fbmq push <dir> -p <priority> [-T tag1,tag2]`
- **Pop next task:** `TASK=$(fbmq pop <dir>)` — prints the claimed file path
- **Read task body:** `fbmq cat "$TASK"` — strips headers, prints body only
- **Read full metadata:** `fbmq inspect "$TASK"` — shows ID, priority, retry count, tags
- **Mark done:** `fbmq ack <dir> "$TASK"`
- **Mark failed:** `fbmq nack <dir> "$TASK"`
- **Queue depth:** `fbmq depth <dir>` — prints count of pending messages
- **Reclaim stale:** `fbmq reap <dir> -l <seconds>`
- **Purge old done:** `fbmq purge <dir> -a <seconds>`

## Priority levels (when --priority enabled)
critical > high > normal > low

## Correlation tracking
Use `-C <correlation-id>` on push to trace tasks across multiple queues.

## Conventions
- Default queue location: /tmp/tasks (or project-local .queue/)
- Always ack on success, nack on failure
- Use `fbmq depth` before popping to check if work exists
```

**Usage:** Tell Pi "Initialize a priority queue at .queue/ and push three tasks for the refactoring we discussed" — it reads the skill and runs the bash commands.

## Layer 2: Pi Extension (TypeScript)

For LLM-native tools and slash commands. See `reference/extension.md` for the full `.pi/extensions/fbmq.ts` implementation.

**What it provides:**
- `queue_push` — push a task with priority, tags, and correlation ID
- `queue_pop` — claim the next task and return its body + metadata
- `queue_complete` — ack (success) or nack (failure) a task
- `queue_depth` — check pending count
- `/plan <goal>` — Pi analyzes the project and pushes decomposed tasks
- `/work` — Pi enters a pop→execute→ack loop until the queue is empty

Read `reference/extension.md` for the complete TypeScript source.

## Layer 3: SDK Worker (Headless)

Embed Pi as a library and drive it with fbmq for fully automated background processing.

**Pattern:**
1. Poll queue with `fbmq depth`
2. `fbmq pop` to claim a task
3. Feed the task body to `createAgentSession()` + `session.prompt()`
4. `fbmq ack` on success, `fbmq nack` on failure
5. Run as a daemon via `pm2` / `systemd`

Read `reference/sdk-worker.md` for the complete Node.js implementation.

## Layer 4: RPC Mode (Non-Node.js)

For shell scripts, C programs, or any language that can do stdin/stdout JSON:

```bash
TASK=$(fbmq pop /var/queue/tasks)
BODY=$(fbmq cat "$TASK")

# Pipe into Pi's RPC mode
echo "{\"type\":\"prompt\",\"text\":$(echo "$BODY" | jq -Rs .)}" | pi --rpc | \
  jq -r 'select(.type=="result") | .status'

# Ack or nack based on exit code
if [ $? -eq 0 ]; then
  fbmq ack /var/queue/tasks "$TASK"
else
  fbmq nack /var/queue/tasks "$TASK"
fi
```

## Use Cases

### 1. Automated Code Review Pipeline
Push PR diffs as tasks → SDK worker runs Pi to review each → results written to `done/`.

### 2. Decompose-and-Conquer Refactoring
`/plan refactor the auth module` → Pi pushes prioritized sub-tasks → `/work` processes them in order.

### 3. Multi-Agent Coordination
Multiple SDK workers consume from separate queues (e.g., `frontend-tasks/`, `backend-tasks/`, `test-tasks/`). A coordinator pushes correlated tasks across all queues.

### 4. CI/CD Task Offloading
CI pushes slow tasks (large test suites, doc generation) to fbmq. A pool of Pi workers drains the queue in parallel.

### 5. Interactive Planning + Background Execution
Use Pi interactively to `/plan`, review the queued tasks with `fbmq inspect`, reprioritize, then let a headless worker `/work` through them overnight.
