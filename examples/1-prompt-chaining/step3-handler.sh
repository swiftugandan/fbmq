#!/bin/sh
# step3-handler.sh — Edit the draft, save final result
set -eu

QUEUE_ROOT="${QUEUE_ROOT:-/tmp}"

ID=$(basename "$FBMQ_TASK_PATH" .md)
mkdir -p "$QUEUE_ROOT/results"
claude -p "Edit this draft for clarity and grammar:\n\n$(cat)" \
  > "$QUEUE_ROOT/results/$ID.md"
