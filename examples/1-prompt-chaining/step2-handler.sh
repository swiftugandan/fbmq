#!/bin/sh
# step2-handler.sh — Expand outline into a draft, push to step 3
set -eu

QUEUE_ROOT="${QUEUE_ROOT:-/tmp}"

DRAFT=$(claude -p "Expand this outline into prose:\n\n$(cat)")
printf '%s' "$DRAFT" | fbmq push "$QUEUE_ROOT/chain-step3"
