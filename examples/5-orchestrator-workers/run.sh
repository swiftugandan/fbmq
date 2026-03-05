#!/bin/sh
# run.sh — Orchestrator-workers: decompose, execute, synthesize
set -eu

QUEUE_ROOT="${QUEUE_ROOT:-/tmp}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

fbmq init "$QUEUE_ROOT/work"
fbmq init "$QUEUE_ROOT/results"

fbmq-worker -v "$QUEUE_ROOT/work" "$SCRIPT_DIR/work-handler.sh" &
fbmq-worker -v "$QUEUE_ROOT/work" "$SCRIPT_DIR/work-handler.sh" &

sh "$SCRIPT_DIR/orchestrate.sh" "Refactor the authentication module to use JWT tokens"

# Wait for all subtasks to complete
while [ "$(fbmq depth "$QUEUE_ROOT/work")" -gt 0 ]; do
  inotifywait -r -qq -e moved_from,delete -t 30 \
    "$QUEUE_ROOT/work/pending/" "$QUEUE_ROOT/work/processing/"
done

# Synthesize results
COMBINED=""
for msg in "$QUEUE_ROOT/results/pending/"*.md; do
  [ -f "$msg" ] && COMBINED="$COMBINED\n---\n$(fbmq cat "$msg")"
done

printf '%b' "$COMBINED" | claude -p "Synthesize these subtask results into a \
coherent final deliverable:\n\n$(cat)"
