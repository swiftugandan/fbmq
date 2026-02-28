#!/bin/sh
# run.sh — Parallelization (voting): push same task 3x, tally results
set -eu

QUEUE_ROOT="${QUEUE_ROOT:-/tmp}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

fbmq init "$QUEUE_ROOT/votes"
fbmq init "$QUEUE_ROOT/ballot"

# Push the same task 3 times
CORR="vote-$(date +%s)"
TASK="Is this function safe from SQL injection? SELECT * FROM users WHERE id = '\$input'"

echo "$TASK" | fbmq push "$QUEUE_ROOT/votes" -c "$CORR"
echo "$TASK" | fbmq push "$QUEUE_ROOT/votes" -c "$CORR"
echo "$TASK" | fbmq push "$QUEUE_ROOT/votes" -c "$CORR"

fbmq-worker -v "$QUEUE_ROOT/votes" "$SCRIPT_DIR/voter.sh" &
fbmq-worker -v "$QUEUE_ROOT/votes" "$SCRIPT_DIR/voter.sh" &
fbmq-worker -v "$QUEUE_ROOT/votes" "$SCRIPT_DIR/voter.sh" &

# Wait for 3 votes
while [ "$(fbmq depth "$QUEUE_ROOT/ballot")" -lt 3 ]; do sleep 2; done

# Tally
for f in "$QUEUE_ROOT/ballot/pending/"*/; do
  for msg in "$f"*.md; do
    [ -f "$msg" ] && fbmq cat "$msg"
    echo "---"
  done
done
