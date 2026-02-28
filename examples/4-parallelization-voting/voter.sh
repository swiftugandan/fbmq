#!/bin/sh
# voter.sh — Cast an independent vote on a task
set -eu

QUEUE_ROOT="${QUEUE_ROOT:-/tmp}"

VERDICT=$(claude -p "Answer YES or NO: Is this code safe? Then explain briefly.\n\n$(cat)")
CORR=$(fbmq inspect "$FBMQ_TASK_PATH" | grep "Correlation" | awk '{print $NF}')
printf '%s' "$VERDICT" | fbmq push "$QUEUE_ROOT/ballot" -c "$CORR"
