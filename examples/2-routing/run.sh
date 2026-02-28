#!/bin/sh
# run.sh — Routing: classify tasks and dispatch to specialized workers
set -eu

QUEUE_ROOT="${QUEUE_ROOT:-/tmp}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

fbmq init "$QUEUE_ROOT/inbox"
fbmq init "$QUEUE_ROOT/code-tasks"
fbmq init "$QUEUE_ROOT/docs-tasks"
fbmq init "$QUEUE_ROOT/data-tasks"

fbmq-worker -v "$QUEUE_ROOT/inbox"      "$SCRIPT_DIR/router.sh" &
fbmq-worker -v "$QUEUE_ROOT/code-tasks" "$SCRIPT_DIR/code-handler.sh" &
fbmq-worker -v "$QUEUE_ROOT/docs-tasks" "$SCRIPT_DIR/docs-handler.sh" &

echo "Write unit tests for the auth module" | fbmq push "$QUEUE_ROOT/inbox" -T code
echo "Update the API reference for /users" | fbmq push "$QUEUE_ROOT/inbox" -T docs
