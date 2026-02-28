#!/bin/sh
# router.sh — Classify a task and dispatch to the right queue
set -eu

QUEUE_ROOT="${QUEUE_ROOT:-/tmp}"

BODY=$(cat)
CATEGORY=$(printf '%s' "$BODY" | claude -p \
  "Classify this task as exactly one of: code, docs, data. Reply with only that word.")

case "$CATEGORY" in
  code) printf '%s' "$BODY" | fbmq push "$QUEUE_ROOT/code-tasks" ;;
  docs) printf '%s' "$BODY" | fbmq push "$QUEUE_ROOT/docs-tasks" ;;
  data) printf '%s' "$BODY" | fbmq push "$QUEUE_ROOT/data-tasks" ;;
  *)    printf '%s' "$BODY" | fbmq push "$QUEUE_ROOT/code-tasks" ;;
esac
