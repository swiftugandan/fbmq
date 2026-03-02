#!/bin/sh
# run.sh — Parallelization (sectioning): fan out sections, wait for all
set -eu

QUEUE_ROOT="${QUEUE_ROOT:-/tmp}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

fbmq init "$QUEUE_ROOT/sections"
fbmq init "$QUEUE_ROOT/section-results"

# Fan out three sections
CORR="batch-$(date +%s)"

echo "Write the Introduction section for a Unix IPC guide" \
  | fbmq push "$QUEUE_ROOT/sections" -c "$CORR" -T section-1

echo "Write the Pipes and FIFOs section for a Unix IPC guide" \
  | fbmq push "$QUEUE_ROOT/sections" -c "$CORR" -T section-2

echo "Write the Shared Memory section for a Unix IPC guide" \
  | fbmq push "$QUEUE_ROOT/sections" -c "$CORR" -T section-3

# Run 3 workers in parallel against the same queue
fbmq-worker -v "$QUEUE_ROOT/sections" "$SCRIPT_DIR/section-handler.sh" &
fbmq-worker -v "$QUEUE_ROOT/sections" "$SCRIPT_DIR/section-handler.sh" &
fbmq-worker -v "$QUEUE_ROOT/sections" "$SCRIPT_DIR/section-handler.sh" &

# Poll until all results arrive
while [ "$(fbmq depth "$QUEUE_ROOT/section-results")" -lt 3 ]; do
  inotifywait -r -qq -e moved_to -t 30 "$QUEUE_ROOT/section-results/pending/"
done
echo "All sections complete — aggregate results from $QUEUE_ROOT/section-results/done/"
