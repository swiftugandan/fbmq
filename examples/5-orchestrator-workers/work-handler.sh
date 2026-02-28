#!/bin/sh
# work-handler.sh — Complete a subtask and push result
set -eu

QUEUE_ROOT="${QUEUE_ROOT:-/tmp}"

RESULT=$(claude -p "Complete this subtask thoroughly:\n\n$(cat)")
CORR=$(fbmq inspect "$FBMQ_TASK_PATH" | grep "Correlation" | awk '{print $NF}')
printf '%s' "$RESULT" | fbmq push "$QUEUE_ROOT/results" -c "$CORR"
