#!/bin/sh
# evaluate-handler.sh — Evaluate a solution; pass or send back for retry
set -eu

QUEUE_ROOT="${QUEUE_ROOT:-/tmp}"

BODY=$(cat)
CORR=$(fbmq inspect "$FBMQ_TASK_PATH" | grep "Correlation" | awk '{print $NF}')

# Track iteration count via a file (push creates new messages, so Retry-Count is always 0)
ITER_FILE="$QUEUE_ROOT/.iter-$CORR"
RETRIES=0
[ -f "$ITER_FILE" ] && RETRIES=$(cat "$ITER_FILE")

VERDICT=$(printf '%s' "$BODY" | claude -p \
  "Evaluate this solution. Reply PASS if it is correct and complete, \
or FAIL with specific feedback for improvement.\n\n$(cat)")

case "$VERDICT" in
  PASS*)
    printf '%s' "$BODY" | fbmq push "$QUEUE_ROOT/final" -c "$CORR"
    rm -f "$ITER_FILE"
    ;;
  *)
    if [ "$RETRIES" -ge 3 ]; then
      echo "Max iterations reached, accepting current solution" >&2
      printf '%s' "$BODY" | fbmq push "$QUEUE_ROOT/final" -c "$CORR"
      rm -f "$ITER_FILE"
    else
      echo $((RETRIES + 1)) > "$ITER_FILE"
      # Send feedback + original prompt back to generator
      PROMPT=$(printf '%s' "$BODY" | sed -n 's/^PROMPT: //p')
      printf 'Previous feedback: %s\n\n%s' "$VERDICT" "$PROMPT" \
        | fbmq push "$QUEUE_ROOT/generate" -c "$CORR"
    fi
    ;;
esac
