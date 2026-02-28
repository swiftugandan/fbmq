#!/bin/sh
# orchestrate.sh — Break a task into subtasks dynamically
set -eu

QUEUE_ROOT="${QUEUE_ROOT:-/tmp}"

TASK="$1"
CORR="orch-$(date +%s)"

# Ask Claude to decompose the task
SUBTASKS=$(claude -p "Break this task into 3-5 independent subtasks. \
Output one subtask per line, no numbering:\n\n$TASK")

# Push each subtask
COUNT=0
printf '%s\n' "$SUBTASKS" | while IFS= read -r line; do
  [ -z "$line" ] && continue
  printf '%s' "$line" | fbmq push "$QUEUE_ROOT/work" -c "$CORR"
  COUNT=$((COUNT + 1))
done

echo "Pushed subtasks with correlation: $CORR"
