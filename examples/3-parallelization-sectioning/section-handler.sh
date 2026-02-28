#!/bin/sh
# section-handler.sh — Process one section and push result
set -eu

QUEUE_ROOT="${QUEUE_ROOT:-/tmp}"

RESULT=$(claude -p "$(cat)")
CORR=$(fbmq inspect "$FBMQ_TASK_PATH" | grep "Correlation" | awk '{print $NF}')
printf '%s' "$RESULT" | fbmq push "$QUEUE_ROOT/section-results" -c "$CORR"
