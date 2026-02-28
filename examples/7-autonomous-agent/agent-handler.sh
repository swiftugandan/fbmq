#!/bin/sh
# agent-handler.sh — Process a task with Claude and save output
set -eu

QUEUE_ROOT="${QUEUE_ROOT:-/tmp}"

ID=$(basename "$FBMQ_TASK_PATH" .md)
mkdir -p "$QUEUE_ROOT/agent-output"
claude -p "$(cat)" > "$QUEUE_ROOT/agent-output/$ID.md"
