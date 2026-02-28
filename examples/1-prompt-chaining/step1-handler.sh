#!/bin/sh
# step1-handler.sh — Generate an outline, push it to step 2
set -eu

QUEUE_ROOT="${QUEUE_ROOT:-/tmp}"

OUTLINE=$(claude -p "Create a bullet-point outline for: $(cat)")
printf '%s' "$OUTLINE" | fbmq push "$QUEUE_ROOT/chain-step2"
