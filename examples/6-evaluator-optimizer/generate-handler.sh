#!/bin/sh
# generate-handler.sh — Generate a solution and send to evaluator
set -eu

QUEUE_ROOT="${QUEUE_ROOT:-/tmp}"

BODY=$(cat)
RESULT=$(claude -p "Write a solution for:\n\n$BODY")
CORR=$(fbmq inspect "$FBMQ_TASK_PATH" | grep "Correlation" | awk '{print $NF}')

# Carry the original prompt + new output to the evaluator
printf 'PROMPT: %s\n\nSOLUTION:\n%s' "$BODY" "$RESULT" \
  | fbmq push "$QUEUE_ROOT/evaluate" -c "$CORR"
