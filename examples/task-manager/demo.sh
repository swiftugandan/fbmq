#!/usr/bin/env bash
# ──────────────────────────────────────────────────────────────────────
# Task Management with fbmq
# ──────────────────────────────────────────────────────────────────────
# Scenario: A small dev team uses fbmq to manage a sprint backlog.
# Tasks are Markdown files with RFC 822 headers. Workers (developers)
# pop tasks, do the work, and ack/nack depending on outcome.
# ──────────────────────────────────────────────────────────────────────
set -euo pipefail

FBMQ="$(cd "$(dirname "$0")/../.." && pwd)/fbmq"
QUEUE=$(mktemp -d)/sprint-backlog

bold()  { printf '\033[1m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
red()   { printf '\033[31m%s\033[0m\n' "$*"; }
dim()   { printf '\033[2m%s\033[0m\n' "$*"; }

# ── 1. Initialize the queue with priority support ────────────────────
bold "=== 1. Initialize sprint backlog queue ==="
$FBMQ init "$QUEUE" --priority
green "Queue created at $QUEUE"
echo

# ── 2. Product owner pushes tasks into the backlog ───────────────────
bold "=== 2. Product owner adds tasks to the sprint ==="

echo "Fix login timeout bug — users report session expires after 5min instead of 30min." |
  $FBMQ push "$QUEUE" -p critical \
    -T "bug" -T "auth" \
    -b "product-owner" \
    -c "SPRINT-42-001" -

echo "Add dark mode toggle to settings page." |
  $FBMQ push "$QUEUE" -p normal \
    -T "feature" -T "ui" \
    -b "product-owner" \
    -c "SPRINT-42-002" -

echo "Upgrade Node.js from 18 to 22 LTS across all services." |
  $FBMQ push "$QUEUE" -p high \
    -T "chore" -T "infra" \
    -b "product-owner" \
    -c "SPRINT-42-003" -

echo "Write API docs for the /users endpoint." |
  $FBMQ push "$QUEUE" -p low \
    -T "docs" \
    -b "product-owner" \
    -c "SPRINT-42-004" -

echo "Investigate flaky CI test in payment module." |
  $FBMQ push "$QUEUE" -p high \
    -T "bug" -T "ci" \
    -b "product-owner" \
    -c "SPRINT-42-005" -

green "5 tasks added to the sprint backlog."
echo
$FBMQ depth "$QUEUE"
echo

# ── 3. Inspect the backlog ───────────────────────────────────────────
bold "=== 3. Inspect the highest-priority task ==="
dim "(fbmq pop returns the path to the claimed message)"

TASK=$($FBMQ pop "$QUEUE")
echo
bold "Claimed: $(basename "$TASK")"
$FBMQ inspect "$TASK"
echo
bold "Task body:"
$FBMQ cat "$TASK"
echo

# ── 4. Developer completes the critical bug fix ──────────────────────
bold "=== 4. Developer Alice completes the critical bug fix ==="
dim "(Simulating work...)"
sleep 0.2
$FBMQ ack "$QUEUE" "$TASK"
green "Task acknowledged — moved to done/"
echo

# ── 5. Another developer picks up the next task ─────────────────────
bold "=== 5. Developer Bob picks up next task ==="
TASK2=$($FBMQ pop "$QUEUE")
bold "Claimed: $(basename "$TASK2")"
$FBMQ cat "$TASK2"
echo

# ── 6. Task fails — nack it back for retry ──────────────────────────
bold "=== 6. Bob hits a blocker — nacking the task ==="
dim "(Task goes back to pending for someone else to pick up)"
$FBMQ nack "$QUEUE" "$TASK2"
red "Task returned to pending queue."
echo

# ── 7. Check remaining depth ────────────────────────────────────────
bold "=== 7. Sprint backlog status ==="
$FBMQ depth "$QUEUE"
echo

# ── 8. Drain remaining tasks (parallel workers) ─────────────────────
bold "=== 8. Team drains the remaining backlog ==="
while TASK=$($FBMQ pop "$QUEUE" 2>/dev/null); do
  [ -z "$TASK" ] && break
  BODY=$($FBMQ cat "$TASK")
  dim "  Working on: $BODY"
  $FBMQ ack "$QUEUE" "$TASK"
  green "  ✓ Done"
done
echo

bold "=== 9. Final status ==="
$FBMQ depth "$QUEUE"
green "Sprint backlog cleared!"

# ── Cleanup ──────────────────────────────────────────────────────────
rm -rf "$(dirname "$QUEUE")"
