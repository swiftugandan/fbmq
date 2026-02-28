# Agent Patterns with fbmq

How each of Anthropic's composable agent patterns maps to fbmq primitives,
with complete shell examples you can copy-paste. Every example uses only the
`fbmq` CLI, `fbmq-worker`, `claude -p`, and standard shell.

Source: [Building Effective Agents](https://www.anthropic.com/research/building-effective-agents) (Anthropic, 2024)

---

## 1. Prompt Chaining

Run a task through a fixed sequence of LLM steps, where each step's output
feeds the next. An `ack` gates each transition — if step 2 fails, the
message stays in its queue for retry without corrupting downstream work.

```
  push        worker 1        worker 2        worker 3
   |              |               |               |
   v              v               v               v
 [q-step1] --> [q-step2] ----> [q-step3] ----> [q-done]
           ack            ack            ack
```

**Setup:**

```bash
fbmq init /tmp/chain-step1
fbmq init /tmp/chain-step2
fbmq init /tmp/chain-step3
```

**Worker scripts:**

```bash
# step1-handler.sh — Generate an outline, push it to step 2
cat > /tmp/step1-handler.sh << 'HANDLER'
#!/bin/sh
OUTLINE=$(claude -p "Create a bullet-point outline for: $(cat)")
printf '%s' "$OUTLINE" | fbmq push /tmp/chain-step2
HANDLER
chmod +x /tmp/step1-handler.sh

# step2-handler.sh — Expand outline into a draft, push to step 3
cat > /tmp/step2-handler.sh << 'HANDLER'
#!/bin/sh
DRAFT=$(claude -p "Expand this outline into prose:\n\n$(cat)")
printf '%s' "$DRAFT" | fbmq push /tmp/chain-step3
HANDLER
chmod +x /tmp/step2-handler.sh

# step3-handler.sh — Edit the draft, save final result
cat > /tmp/step3-handler.sh << 'HANDLER'
#!/bin/sh
ID=$(basename "$FBMQ_TASK_PATH" .md)
claude -p "Edit this draft for clarity and grammar:\n\n$(cat)" \
  > "/tmp/results/$ID.md"
HANDLER
chmod +x /tmp/step3-handler.sh
mkdir -p /tmp/results
```

**Run:**

```bash
fbmq-worker -v /tmp/chain-step1 /tmp/step1-handler.sh &
fbmq-worker -v /tmp/chain-step2 /tmp/step2-handler.sh &
fbmq-worker -v /tmp/chain-step3 /tmp/step3-handler.sh &

echo "Write a blog post about file-based message queues" \
  | fbmq push /tmp/chain-step1
```

**Key header:** None required — the queue topology itself enforces the sequence.

---

## 2. Routing

A single router worker reads each incoming message and dispatches it to a
specialized queue based on the content, tags, or priority.

```
              router worker
                  |
  push            |---> [q-code]    code worker
   |              |---> [q-docs]    docs worker
   v              |---> [q-data]    data worker
 [q-inbox] ------'
```

**Setup:**

```bash
fbmq init /tmp/inbox
fbmq init /tmp/code-tasks
fbmq init /tmp/docs-tasks
fbmq init /tmp/data-tasks
```

**Router script:**

```bash
cat > /tmp/router.sh << 'HANDLER'
#!/bin/sh
BODY=$(cat)
CATEGORY=$(printf '%s' "$BODY" | claude -p \
  "Classify this task as exactly one of: code, docs, data. Reply with only that word.")

case "$CATEGORY" in
  code) printf '%s' "$BODY" | fbmq push /tmp/code-tasks ;;
  docs) printf '%s' "$BODY" | fbmq push /tmp/docs-tasks ;;
  data) printf '%s' "$BODY" | fbmq push /tmp/data-tasks ;;
  *)    printf '%s' "$BODY" | fbmq push /tmp/code-tasks ;;  # default
esac
HANDLER
chmod +x /tmp/router.sh
```

**Specialized workers:**

```bash
cat > /tmp/code-handler.sh << 'HANDLER'
#!/bin/sh
claude -p "You are an expert programmer. Complete this task: $(cat)"
HANDLER
chmod +x /tmp/code-handler.sh

cat > /tmp/docs-handler.sh << 'HANDLER'
#!/bin/sh
claude -p "You are a technical writer. Complete this task: $(cat)"
HANDLER
chmod +x /tmp/docs-handler.sh
```

**Run:**

```bash
fbmq-worker -v /tmp/inbox /tmp/router.sh &
fbmq-worker -v /tmp/code-tasks /tmp/code-handler.sh &
fbmq-worker -v /tmp/docs-tasks /tmp/docs-handler.sh &

echo "Write unit tests for the auth module" | fbmq push /tmp/inbox -T code
echo "Update the API reference for /users" | fbmq push /tmp/inbox -T docs
```

**Key header:** `Tags` (`-T`) — the router can inspect tags with
`fbmq inspect` instead of (or in addition to) calling the LLM.

---

## 3. Parallelization — Sectioning

Split a large task into independent sections, fan out to parallel workers,
then aggregate when all are done.

```
  orchestrator                              aggregator
      |                                         ^
      |--push--> [q-sections] --worker 1--+     |
      |--push-->               --worker 2--+--> [q-results]
      |--push-->               --worker 3--+
      |
   depth == 0? --------------------------------> done
```

**Setup:**

```bash
fbmq init /tmp/sections
fbmq init /tmp/section-results
```

**Fan-out (orchestrator):**

```bash
CORR="batch-$(date +%s)"

echo "Write the Introduction section for a Unix IPC guide" \
  | fbmq push /tmp/sections -c "$CORR" -T section-1

echo "Write the Pipes and FIFOs section for a Unix IPC guide" \
  | fbmq push /tmp/sections -c "$CORR" -T section-2

echo "Write the Shared Memory section for a Unix IPC guide" \
  | fbmq push /tmp/sections -c "$CORR" -T section-3
```

**Section worker:**

```bash
cat > /tmp/section-handler.sh << 'HANDLER'
#!/bin/sh
RESULT=$(claude -p "$(cat)")
CORR=$(fbmq inspect "$FBMQ_TASK_PATH" | grep "Correlation" | awk '{print $NF}')
printf '%s' "$RESULT" | fbmq push /tmp/section-results -c "$CORR"
HANDLER
chmod +x /tmp/section-handler.sh
```

**Run workers and wait for completion:**

```bash
# Run 3 workers in parallel against the same queue
fbmq-worker -v /tmp/sections /tmp/section-handler.sh &
fbmq-worker -v /tmp/sections /tmp/section-handler.sh &
fbmq-worker -v /tmp/sections /tmp/section-handler.sh &

# Poll until all results arrive
while [ "$(fbmq depth /tmp/section-results)" -lt 3 ]; do
  sleep 2
done
echo "All sections complete — aggregate results from /tmp/section-results/done/"
```

**Key header:** `Correlation-Id` (`-c`) — ties all sections to the same batch.

---

## 4. Parallelization — Voting

Push the same task N times, collect independent answers, and pick the
majority or best result.

```
  push x3       3 workers         aggregator
    |               |                 |
    v               v                 v
 [q-votes] --> worker 1 --+--> [q-ballot]
               worker 2 --+
               worker 3 --+
```

**Setup:**

```bash
fbmq init /tmp/votes
fbmq init /tmp/ballot
```

**Push the same task 3 times:**

```bash
CORR="vote-$(date +%s)"
TASK="Is this function safe from SQL injection? SELECT * FROM users WHERE id = '\$input'"

echo "$TASK" | fbmq push /tmp/votes -c "$CORR"
echo "$TASK" | fbmq push /tmp/votes -c "$CORR"
echo "$TASK" | fbmq push /tmp/votes -c "$CORR"
```

**Voter worker:**

```bash
cat > /tmp/voter.sh << 'HANDLER'
#!/bin/sh
VERDICT=$(claude -p "Answer YES or NO: Is this code safe? Then explain briefly.\n\n$(cat)")
CORR=$(fbmq inspect "$FBMQ_TASK_PATH" | grep "Correlation" | awk '{print $NF}')
printf '%s' "$VERDICT" | fbmq push /tmp/ballot -c "$CORR"
HANDLER
chmod +x /tmp/voter.sh
```

**Run and tally:**

```bash
fbmq-worker -v /tmp/votes /tmp/voter.sh &
fbmq-worker -v /tmp/votes /tmp/voter.sh &
fbmq-worker -v /tmp/votes /tmp/voter.sh &

# Wait for 3 votes
while [ "$(fbmq depth /tmp/ballot)" -lt 3 ]; do sleep 2; done

# Tally
for f in /tmp/ballot/pending/*/; do
  for msg in "$f"*.md; do
    [ -f "$msg" ] && fbmq cat "$msg"
    echo "---"
  done
done
```

**Key header:** `Correlation-Id` (`-c`) — groups votes for the same question.

---

## 5. Orchestrator-Workers

A central orchestrator dynamically breaks a task into subtasks at runtime,
pushes them to a work queue, and tracks completion via `Correlation-Id` and
`depth`.

```
                    orchestrator
                   /      |      \
                  v       v       v
              [q-work] [q-work] [q-work]  (same queue)
                  \       |       /
                   v      v      v
                    [q-results]
                        |
                        v
                   orchestrator
                   (synthesize)
```

**Setup:**

```bash
fbmq init /tmp/work
fbmq init /tmp/results
```

**Orchestrator script:**

```bash
#!/bin/sh
# orchestrate.sh — break a task into subtasks dynamically
TASK="$1"
CORR="orch-$(date +%s)"

# Ask Claude to decompose the task
SUBTASKS=$(claude -p "Break this task into 3-5 independent subtasks. \
Output one subtask per line, no numbering:\n\n$TASK")

# Push each subtask
COUNT=0
printf '%s\n' "$SUBTASKS" | while IFS= read -r line; do
  [ -z "$line" ] && continue
  printf '%s' "$line" | fbmq push /tmp/work -c "$CORR"
  COUNT=$((COUNT + 1))
done

echo "Pushed subtasks with correlation: $CORR"
```

**Worker:**

```bash
cat > /tmp/work-handler.sh << 'HANDLER'
#!/bin/sh
RESULT=$(claude -p "Complete this subtask thoroughly:\n\n$(cat)")
CORR=$(fbmq inspect "$FBMQ_TASK_PATH" | grep "Correlation" | awk '{print $NF}')
printf '%s' "$RESULT" | fbmq push /tmp/results -c "$CORR"
HANDLER
chmod +x /tmp/work-handler.sh
```

**Run:**

```bash
fbmq-worker -v /tmp/work /tmp/work-handler.sh &
fbmq-worker -v /tmp/work /tmp/work-handler.sh &

sh orchestrate.sh "Refactor the authentication module to use JWT tokens"

# Wait for all subtasks to complete
while [ "$(fbmq depth /tmp/work)" -gt 0 ]; do sleep 3; done

# Synthesize results
COMBINED=""
for f in /tmp/results/pending/*/; do
  for msg in "$f"*.md; do
    [ -f "$msg" ] && COMBINED="$COMBINED\n---\n$(fbmq cat "$msg")"
  done
done

printf '%b' "$COMBINED" | claude -p "Synthesize these subtask results into a \
coherent final deliverable:\n\n$(cat)"
```

**Key header:** `Correlation-Id` (`-c`) — the orchestrator generates one ID
per task and uses it to track which results belong together.

---

## 6. Evaluator-Optimizer

A two-queue feedback loop: one worker generates output, another evaluates
it. If the evaluation fails, the task goes back for another attempt.
`Retry-Count` caps the iterations.

```
             generate           evaluate
                |                   |
                v                   v
 push --> [q-generate] ------> [q-evaluate]
               ^                    |
               |   nack (retry)     |
               +--------------------+
               |
          ack (pass)
               |
               v
           [q-done]
```

**Setup:**

```bash
fbmq init /tmp/generate
fbmq init /tmp/evaluate
fbmq init /tmp/final
```

**Generator worker:**

```bash
cat > /tmp/generate-handler.sh << 'HANDLER'
#!/bin/sh
BODY=$(cat)
RESULT=$(claude -p "Write a solution for:\n\n$BODY")
CORR=$(fbmq inspect "$FBMQ_TASK_PATH" | grep "Correlation" | awk '{print $NF}')

# Carry the original prompt + new output to the evaluator
printf 'PROMPT: %s\n\nSOLUTION:\n%s' "$BODY" "$RESULT" \
  | fbmq push /tmp/evaluate -c "$CORR"
HANDLER
chmod +x /tmp/generate-handler.sh
```

**Evaluator worker:**

```bash
cat > /tmp/evaluate-handler.sh << 'HANDLER'
#!/bin/sh
BODY=$(cat)
CORR=$(fbmq inspect "$FBMQ_TASK_PATH" | grep "Correlation" | awk '{print $NF}')
RETRIES=$(fbmq inspect "$FBMQ_TASK_PATH" | grep "Retry count" | awk '{print $NF}')

VERDICT=$(printf '%s' "$BODY" | claude -p \
  "Evaluate this solution. Reply PASS if it is correct and complete, \
or FAIL with specific feedback for improvement.\n\n$(cat)")

case "$VERDICT" in
  PASS*)
    # Extract the solution and push to final
    printf '%s' "$BODY" | fbmq push /tmp/final -c "$CORR"
    ;;
  *)
    if [ "$RETRIES" -ge 3 ]; then
      echo "Max iterations reached, accepting current solution" >&2
      printf '%s' "$BODY" | fbmq push /tmp/final -c "$CORR"
    else
      # Send feedback + original prompt back to generator
      PROMPT=$(printf '%s' "$BODY" | sed -n 's/^PROMPT: //p')
      printf 'Previous feedback: %s\n\n%s' "$VERDICT" "$PROMPT" \
        | fbmq push /tmp/generate -c "$CORR"
    fi
    ;;
esac
HANDLER
chmod +x /tmp/evaluate-handler.sh
```

**Run:**

```bash
fbmq-worker -v /tmp/generate /tmp/generate-handler.sh &
fbmq-worker -v /tmp/evaluate /tmp/evaluate-handler.sh &

echo "Write a Python function that finds all prime numbers up to N using the Sieve of Eratosthenes" \
  | fbmq push /tmp/generate -c "eval-$(date +%s)"
```

**Key header:** `Retry-Count` — tracks iterations through the loop.
The evaluator checks it to enforce a maximum number of refinement cycles.

---

## 7. Autonomous Agent

The simplest pattern: `fbmq-worker` polls a queue and runs `claude -p` for
each task. The agent runs autonomously in the background — push tasks
whenever you want and they get processed.

```
  push          fbmq-worker          claude -p
   |                |                    |
   v                v                    v
 [q-tasks] ---> pop + cat stdin ---> LLM call ---> ack/nack
                    ^                                  |
                    |           poll loop               |
                    +----------------------------------+
```

**Setup:**

```bash
fbmq init /tmp/agent-tasks
mkdir -p /tmp/agent-output
```

**Handler script:**

```bash
cat > /tmp/agent-handler.sh << 'HANDLER'
#!/bin/sh
ID=$(basename "$FBMQ_TASK_PATH" .md)
claude -p "$(cat)" > "/tmp/agent-output/$ID.md"
HANDLER
chmod +x /tmp/agent-handler.sh
```

**Run:**

```bash
nohup fbmq-worker -v /tmp/agent-tasks /tmp/agent-handler.sh \
  >> /tmp/agent.log 2>&1 &

echo "Explain the difference between processes and threads" \
  | fbmq push /tmp/agent-tasks

echo "Write a Makefile for a C project with src/ and include/ dirs" \
  | fbmq push /tmp/agent-tasks -p high

echo "Review this code for security issues: $(cat main.c)" \
  | fbmq push /tmp/agent-tasks -p critical
```

**Check on it:**

```bash
fbmq depth /tmp/agent-tasks        # tasks remaining
ls /tmp/agent-output/               # completed results
tail -f /tmp/agent.log              # live worker logs
ls /tmp/agent-tasks/failed/         # anything that failed
```

**Key header:** `Priority` (`-p`) — critical and high tasks get processed
before normal and low ones (requires `fbmq init --priority`).

---

## Header Reference

| Header | CLI flag | Pattern use |
|--------|----------|-------------|
| `Priority` | `-p critical\|high\|normal\|low` | Autonomous agent, routing |
| `Correlation-Id` | `-c <id>` | Orchestrator, parallelization, evaluator |
| `Tags` | `-T <tag>` (repeatable) | Routing, classification |
| `Retry-Count` | (automatic on nack) | Evaluator-optimizer iteration guard |
| `TTL` | `-t <seconds>` | Time-bounded tasks, auto-expiry via `fbmq reap` |
| `Created-By` | `-b <name>` | Audit trail across multi-agent systems |
