#!/bin/sh
# run.sh — Evaluator-optimizer: generate/evaluate feedback loop
set -eu

QUEUE_ROOT="${QUEUE_ROOT:-/tmp}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

fbmq init "$QUEUE_ROOT/generate"
fbmq init "$QUEUE_ROOT/evaluate"
fbmq init "$QUEUE_ROOT/final"

fbmq-worker -v "$QUEUE_ROOT/generate" "$SCRIPT_DIR/generate-handler.sh" &
fbmq-worker -v "$QUEUE_ROOT/evaluate" "$SCRIPT_DIR/evaluate-handler.sh" &

echo "Write a Python function that finds all prime numbers up to N using the Sieve of Eratosthenes" \
  | fbmq push "$QUEUE_ROOT/generate" -c "eval-$(date +%s)"
