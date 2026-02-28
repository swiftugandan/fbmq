#!/bin/sh
# run.sh — Prompt chaining: three-step blog post pipeline
set -eu

QUEUE_ROOT="${QUEUE_ROOT:-/tmp}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

fbmq init "$QUEUE_ROOT/chain-step1"
fbmq init "$QUEUE_ROOT/chain-step2"
fbmq init "$QUEUE_ROOT/chain-step3"
mkdir -p "$QUEUE_ROOT/results"

fbmq-worker -v "$QUEUE_ROOT/chain-step1" "$SCRIPT_DIR/step1-handler.sh" &
fbmq-worker -v "$QUEUE_ROOT/chain-step2" "$SCRIPT_DIR/step2-handler.sh" &
fbmq-worker -v "$QUEUE_ROOT/chain-step3" "$SCRIPT_DIR/step3-handler.sh" &

echo "Write a blog post about file-based message queues" \
  | fbmq push "$QUEUE_ROOT/chain-step1"
