#!/bin/sh
# run.sh — Autonomous agent: background worker processing a task queue
set -eu

QUEUE_ROOT="${QUEUE_ROOT:-/tmp}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

fbmq init "$QUEUE_ROOT/agent-tasks"
mkdir -p "$QUEUE_ROOT/agent-output"

nohup fbmq-worker -v "$QUEUE_ROOT/agent-tasks" "$SCRIPT_DIR/agent-handler.sh" \
  >> "$QUEUE_ROOT/agent.log" 2>&1 &

echo "Explain the difference between processes and threads" \
  | fbmq push "$QUEUE_ROOT/agent-tasks"

echo "Write a Makefile for a C project with src/ and include/ dirs" \
  | fbmq push "$QUEUE_ROOT/agent-tasks" -p high

echo "Review this code for security issues: int main() { gets(buf); }" \
  | fbmq push "$QUEUE_ROOT/agent-tasks" -p critical

echo "Worker running in background. Check progress:"
echo "  fbmq depth $QUEUE_ROOT/agent-tasks"
echo "  ls $QUEUE_ROOT/agent-output/"
echo "  tail -f $QUEUE_ROOT/agent.log"
