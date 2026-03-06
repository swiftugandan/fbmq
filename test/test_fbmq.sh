#!/bin/bash
# test_fbmq.sh — Integration tests for fbmq
set -euo pipefail

FBMQ="${FBMQ:-./fbmq}"
TEST_TMPDIR=$(mktemp -d /tmp/fbmq-test.XXXXXX)
QUEUE="$TEST_TMPDIR/testq"
PASS=0
FAIL=0

cleanup() { rm -rf "$TEST_TMPDIR"; }
trap cleanup EXIT

pass() { PASS=$((PASS+1)); printf "  \033[32mPASS\033[0m %s\n" "$1"; }
fail() { FAIL=$((FAIL+1)); printf "  \033[31mFAIL\033[0m %s: %s\n" "$1" "${2:-}"; }

assert_eq() {
    if [ "$1" = "$2" ]; then pass "$3"; else fail "$3" "expected '$1', got '$2'"; fi
}

# Helper: find message file by ID in a bucket directory (handles timestamp prefix)
find_msg() {
    local dir="$1" id="$2"
    find "$dir" \( -name "$id.md" -o -name "*.$id.md" \) -print -quit 2>/dev/null
}

# Helper: assert that every element of expected_arr exists in actual_arr
assert_set_eq() {
    local label="$1"; shift
    local ok=true
    local -a expected=() actual=()
    while [ "$1" != "--" ]; do expected+=("$1"); shift; done
    shift  # skip --
    while [ $# -gt 0 ]; do actual+=("$1"); shift; done
    for eid in "${expected[@]}"; do
        local found=false
        for gid in "${actual[@]}"; do
            [ "$gid" = "$eid" ] && found=true
        done
        $found || ok=false
    done
    $ok && pass "$label" || fail "$label"
}

echo "╔══════════════════════════════════════╗"
echo "║     fbmq integration tests           ║"
echo "╚══════════════════════════════════════╝"
echo ""

# ── init ──
echo "── init ──"
$FBMQ init "$QUEUE" --max-pending 0 2>/dev/null
[ -d "$QUEUE/pending" ]    && pass "pending/ exists" || fail "pending/"
[ -d "$QUEUE/processing" ] && pass "processing/ exists" || fail "processing/"
[ -d "$QUEUE/done" ]       && pass "done/ exists" || fail "done/"
[ -d "$QUEUE/failed" ]     && pass "failed/ exists" || fail "failed/"
[ -d "$QUEUE/.tmp" ]       && pass ".tmp/ exists" || fail ".tmp/"
[ -d "$QUEUE/.meta" ]      && pass ".meta/ exists" || fail ".meta/"

# ── init --priority ──
echo ""
echo "── init --priority ──"
PQUEUE="$TEST_TMPDIR/prioq"
$FBMQ init "$PQUEUE" --priority --max-pending 0 2>/dev/null
[ -d "$PQUEUE/pending/0-critical" ] && pass "0-critical" || fail "0-critical"
[ -d "$PQUEUE/pending/1-high" ]     && pass "1-high" || fail "1-high"
[ -d "$PQUEUE/pending/2-normal" ]   && pass "2-normal" || fail "2-normal"
[ -d "$PQUEUE/pending/3-low" ]      && pass "3-low" || fail "3-low"

# ── push ──
echo ""
echo "── push ──"
ID=$(echo "# Hello World" | $FBMQ push "$QUEUE")
[ ${#ID} -eq 32 ] && pass "32-char ID returned" || fail "ID length" "got ${#ID}"
FILE=$(find_msg "$QUEUE/pending" "$ID")
[ -n "$FILE" ] && pass "file in pending/" || fail "file not in pending/"

# Verify content
grep -q "^Id: $ID" "$FILE" && pass "id in frontmatter" || fail "id missing"
grep -q "^Priority: normal" "$FILE" && pass "priority in frontmatter" || fail "priority missing"
grep -q "# Hello World" "$FILE" && pass "body present" || fail "body missing"

# Verify timestamp prefix in filename
FNAME=$(basename "$FILE")
echo "$FNAME" | grep -qE '^[0-9]+\.[0-9a-f]{32}\.md$' && pass "timestamp prefix" || fail "filename format" "$FNAME"

# ── push with options ──
echo ""
echo "── push with options ──"
ID2=$(echo "# Urgent deploy" | $FBMQ push "$QUEUE" -p high -t 3600 -c req-42 -T urgent -T deploy -b ci-server)
FILE2=$(find_msg "$QUEUE/pending" "$ID2")
grep -q "Priority: high" "$FILE2" && pass "priority=high" || fail "priority"
grep -q "TTL: 3600" "$FILE2" && pass "ttl=3600" || fail "ttl"
grep -q "Correlation-Id: req-42" "$FILE2" && pass "correlation_id" || fail "corr_id"
grep -q "Tags: urgent, deploy" "$FILE2" && pass "tags" || fail "tags"
grep -q "Created-By: ci-server" "$FILE2" && pass "created_by" || fail "created_by"

# ── push from file ──
echo ""
echo "── push from file ──"
echo "# File-based message" > "$TEST_TMPDIR/task.md"
echo "" >> "$TEST_TMPDIR/task.md"
echo "Do the thing." >> "$TEST_TMPDIR/task.md"
ID3=$($FBMQ push "$QUEUE" "$TEST_TMPDIR/task.md")
FILE3=$(find_msg "$QUEUE/pending" "$ID3")
[ -n "$FILE3" ] && pass "file push works" || fail "file push"

# ── depth ──
echo ""
echo "── depth ──"
DEPTH=$($FBMQ depth "$QUEUE")
assert_eq "3" "$DEPTH" "depth=3 after 3 pushes"

# ── inspect ──
echo ""
echo "── inspect ──"
INSPECT=$($FBMQ inspect "$FILE")
echo "$INSPECT" | grep -q "ID:" && pass "inspect: ID" || fail "inspect ID"
echo "$INSPECT" | grep -q "Priority:.*normal" && pass "inspect: priority" || fail "inspect priority"
echo "$INSPECT" | grep -q "Body:" && pass "inspect: body size" || fail "inspect body"

# ── cat ──
echo ""
echo "── cat ──"
BODY=$($FBMQ cat "$FILE")
echo "$BODY" | grep -q "# Hello World" && pass "cat: body only" || fail "cat body"
echo "$BODY" | grep -qv "^---" && pass "cat: no frontmatter" || fail "cat frontmatter leak"

# ── pop (FIX #1: prints path to stdout) ──
echo ""
echo "── pop ──"
CLAIMED=$($FBMQ pop "$QUEUE")
[ $? -eq 0 ] && pass "pop exits 0" || fail "pop exit code"
[ -n "$CLAIMED" ] && pass "pop prints path" || fail "pop empty output"
[ -f "$CLAIMED" ] && pass "claimed file exists" || fail "claimed file missing"

# Verify claimed path format: processing/<claim_ts>.<hash>.md
CLAIMED_BASE=$(basename "$CLAIMED")
echo "$CLAIMED_BASE" | grep -qE '^[0-9]+\.[0-9a-f]{32}\.md$' && pass "claimed path format" || fail "path format" "$CLAIMED_BASE"

# Read the claimed message
head -1 "$CLAIMED" | grep -qE '^[A-Za-z][-A-Za-z0-9]*:' && pass "claimed file has headers" || fail "no headers"

DEPTH=$($FBMQ depth "$QUEUE")
assert_eq "3" "$DEPTH" "depth=3 after 1 pop (includes processing)"

# ── ack ──
echo ""
echo "── ack ──"
$FBMQ ack "$QUEUE" "$CLAIMED"
[ ! -f "$CLAIMED" ] && pass "removed from processing" || fail "still in processing"
DONE_COUNT=$(find "$QUEUE/done" -name '*.md' | wc -l | tr -d ' ')
assert_eq "1" "$DONE_COUNT" "1 message in done/"
# Verify done/ filename is <hash>.md (no timestamps)
DONE_FILE=$(find "$QUEUE/done" -name '*.md' -print -quit)
DONE_BASE=$(basename "$DONE_FILE")
echo "$DONE_BASE" | grep -qE '^[0-9a-f]{32}\.md$' && pass "done: filename is <hash>.md" \
    || fail "done: unexpected filename format" "$DONE_BASE"

# ── nack → retry ──
echo ""
echo "── nack (retry) ──"
CLAIMED2=$($FBMQ pop "$QUEUE")
$FBMQ nack "$QUEUE" "$CLAIMED2"
[ ! -f "$CLAIMED2" ] && pass "removed from processing" || fail "still in processing"
DEPTH=$($FBMQ depth "$QUEUE")
assert_eq "2" "$DEPTH" "depth=2 after nack (returned)"

# Verify retry_count incremented — find the nacked message by ID
CLAIMED2_BASE=$(basename "$CLAIMED2")
# Strip claim prefix (first segment) and .md suffix to get hash
NACKED_ORIG=$(echo "$CLAIMED2_BASE" | sed 's/^[0-9]*\.//')
NACKED_ID=$(echo "$NACKED_ORIG" | sed 's/\.md$//')
NACKED_FILE=$(find_msg "$QUEUE/pending" "$NACKED_ID")
grep -q "Retry-Count: 1" "$NACKED_FILE" && pass "retry_count=1" || fail "retry_count"

# ── nack → dead-letter ──
echo ""
echo "── nack (dead-letter) ──"
# Drain all but one message by acking extras
while true; do
    DEPTH_NOW=$($FBMQ depth "$QUEUE")
    [ "$DEPTH_NOW" -le 1 ] && break
    CL_DRAIN=$($FBMQ pop "$QUEUE" 2>/dev/null) || break
    $FBMQ ack "$QUEUE" "$CL_DRAIN"
done
# Now repeatedly pop+nack the single remaining message until it dead-letters.
# It already has retry_count=1 from the previous nack test, so we need
# nack 3 more times: 1→2, 2→3, 3→4 (>3 = dead-letter).
for i in $(seq 1 5); do
    CL=$($FBMQ pop "$QUEUE" 2>/dev/null) || break
    $FBMQ nack "$QUEUE" "$CL" 2>/dev/null || true
done
DL_COUNT=$(find "$QUEUE/failed" -name '*.md' | wc -l)
[ "$DL_COUNT" -ge 1 ] && pass "dead-lettered after max retries" || fail "not dead-lettered" "count=$DL_COUNT"
# Verify failed/ filename is <hash>.md (no timestamps)
DL_FILE=$(find "$QUEUE/failed" -name '*.md' -print -quit)
DL_BASE=$(basename "$DL_FILE")
echo "$DL_BASE" | grep -qE '^[0-9a-f]{32}\.md$' && pass "failed: filename is <hash>.md" \
    || fail "failed: unexpected filename format" "$DL_BASE"

# ── pop empty ──
echo ""
echo "── pop (empty queue) ──"
# Drain remaining
while true; do
    CL=$($FBMQ pop "$QUEUE" 2>/dev/null) || break
    $FBMQ ack "$QUEUE" "$CL"
done
RC=0
$FBMQ pop "$QUEUE" >/dev/null 2>&1 || RC=$?
assert_eq "1" "$RC" "pop returns 1 when empty"

# ── push --no-fsync ──
echo ""
echo "── push --no-fsync ──"
ID4=$(echo "# Fast message" | $FBMQ push "$QUEUE" --no-fsync)
[ ${#ID4} -eq 32 ] && pass "no-fsync push works" || fail "no-fsync push"
$FBMQ pop "$QUEUE" >/dev/null && pass "no-fsync message consumable" || fail "no-fsync consume"

# ── purge ──
echo ""
echo "── purge ──"
# Make done files look old
find "$QUEUE/done" -name '*.md' -exec touch -t 202001010000.00 {} \; 2>/dev/null
PURGE_OUT=$($FBMQ purge "$QUEUE" -a 1 2>&1)
echo "$PURGE_OUT" | grep -q "Purged" && pass "purge reports count" || fail "purge output"

# ── priority queue push/pop ──
echo ""
echo "── priority queue ──"
echo "# Low prio" | $FBMQ push "$PQUEUE" -p low >/dev/null
echo "# Critical" | $FBMQ push "$PQUEUE" -p critical >/dev/null
echo "# Normal" | $FBMQ push "$PQUEUE" -p normal >/dev/null

# Pop should get critical first (scan order: critical → high → normal → low)
CL_PRIO=$($FBMQ pop "$PQUEUE")
grep -q "Priority: critical" "$CL_PRIO" && pass "critical popped first" || fail "priority order"
$FBMQ ack "$PQUEUE" "$CL_PRIO"

# ── pop set correctness ──
echo ""
echo "── pop set correctness ──"
FIFO_QUEUE="$TEST_TMPDIR/fifoq"
$FBMQ init "$FIFO_QUEUE" 2>/dev/null

# Push 3 messages
ID_A=$(echo "# Message A" | $FBMQ push "$FIFO_QUEUE" --no-fsync)
sleep 0.1
ID_B=$(echo "# Message B" | $FBMQ push "$FIFO_QUEUE" --no-fsync)
sleep 0.1
ID_C=$(echo "# Message C" | $FBMQ push "$FIFO_QUEUE" --no-fsync)

# Pop all and verify the correct set is returned
declare -a POP_ORDER=()
for i in 1 2 3; do
    CL=$($FBMQ pop "$FIFO_QUEUE")
    MSG_ID=$(grep -i '^Id:' "$CL" | sed 's/^Id: *//')
    POP_ORDER+=("$MSG_ID")
    $FBMQ ack "$FIFO_QUEUE" "$CL"
done

# All three messages should have been popped (none lost)
[ ${#POP_ORDER[@]} -eq 3 ] && pass "pop: all 3 messages popped" || fail "pop: lost messages"

assert_set_eq "pop: correct message set" "$ID_A" "$ID_B" "$ID_C" -- "${POP_ORDER[@]}"

# Strict FIFO order check
[ "${POP_ORDER[0]}" = "$ID_A" ] && [ "${POP_ORDER[1]}" = "$ID_B" ] && [ "${POP_ORDER[2]}" = "$ID_C" ] && \
    pass "pop: FIFO order A→B→C" || fail "pop: FIFO order"

# ── custom fields ──
echo ""
echo "── custom fields ──"
cat > "$TEST_TMPDIR/custom.md" <<'BODY'
# Custom test
BODY

# Create a message with custom fields manually
CUSTOM_ID=$(echo "# Custom test" | $FBMQ push "$QUEUE")
CUSTOM_FILE=$(find_msg "$QUEUE/pending" "$CUSTOM_ID")
INSPECT_CUSTOM=$($FBMQ inspect "$CUSTOM_FILE")
echo "$INSPECT_CUSTOM" | grep -q "ID:" && pass "custom msg created" || fail "custom msg"

# ── depth locking ──
echo ""
echo "── depth locking ──"
LOCK_QUEUE="$TEST_TMPDIR/lockq"
$FBMQ init "$LOCK_QUEUE" 2>/dev/null
# Push 10 messages concurrently and verify depth is consistent
for i in $(seq 1 10); do
    echo "# Lock test $i" | $FBMQ push "$LOCK_QUEUE" --no-fsync &
done
wait
LOCK_DEPTH=$($FBMQ depth "$LOCK_QUEUE")
LOCK_FILES=$(find "$LOCK_QUEUE/pending" -name '*.md' | wc -l | tr -d ' ')
assert_eq "$LOCK_FILES" "$LOCK_DEPTH" "depth matches file count under concurrency"

# ── version ──
echo ""
echo "── version ──"
VER=$($FBMQ version)
echo "$VER" | grep -q "fbmq 1.0.0" && pass "version output" || fail "version" "$VER"

# ── concurrent push (basic) ──
echo ""
echo "── concurrent push ──"
CONC_QUEUE="$TEST_TMPDIR/concq"
$FBMQ init "$CONC_QUEUE" 2>/dev/null
for i in $(seq 1 20); do
    echo "# Message $i" | $FBMQ push "$CONC_QUEUE" --no-fsync &
done
wait
CONC_DEPTH=$($FBMQ depth "$CONC_QUEUE")
assert_eq "20" "$CONC_DEPTH" "20 concurrent pushes"

# Count actual files
ACTUAL=$(find "$CONC_QUEUE/pending" -name '*.md' | wc -l | tr -d ' ')
assert_eq "20" "$ACTUAL" "20 files on disk"

# ── parser edge cases ──
echo ""
echo "── parser edge cases ──"
# Empty file
touch "$TEST_TMPDIR/empty.md"
$FBMQ inspect "$TEST_TMPDIR/empty.md" >/dev/null 2>&1 && pass "empty file parses" || fail "empty file"

# No-body file (headers only, no blank line separator)
cat > "$TEST_TMPDIR/nohead.md" <<'EOF'
Just some text with no headers at all.
EOF
BODY_ONLY=$($FBMQ cat "$TEST_TMPDIR/nohead.md")
echo "$BODY_ONLY" | grep -q "Just some text" && pass "no-header file: body only" || fail "no-header file"

# Oversized ID field (should be rejected by parser)
cat > "$TEST_TMPDIR/bad_id.md" <<'EOF'
Id: zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz
Created-At: 2026-01-01T00:00:00Z

body
EOF
RC_BAD=0
$FBMQ inspect "$TEST_TMPDIR/bad_id.md" >/dev/null 2>&1 || RC_BAD=$?
[ "$RC_BAD" -ne 0 ] && pass "oversized ID rejected" || fail "oversized ID accepted"

# Colon in body should not confuse parser
cat > "$TEST_TMPDIR/colon.md" <<'EOF'
Id: aaaabbbbccccddddaaaabbbbccccdddd
Created-At: 2026-01-01T00:00:00Z
Priority: normal
Retry-Count: 0

Key: Value in body should be preserved
EOF
COLON_BODY=$($FBMQ cat "$TEST_TMPDIR/colon.md")
echo "$COLON_BODY" | grep -q "Key: Value in body" && pass "colon in body preserved" || fail "colon in body"

# ── concurrent push (50 workers) ──
echo ""
echo "── concurrent push (50 workers) ──"
CONC50_QUEUE="$TEST_TMPDIR/conc50q"
$FBMQ init "$CONC50_QUEUE" 2>/dev/null
for i in $(seq 1 50); do
    echo "# Concurrent $i" | $FBMQ push "$CONC50_QUEUE" --no-fsync &
done
wait
CONC50_DEPTH=$($FBMQ depth "$CONC50_QUEUE")
CONC50_FILES=$(find "$CONC50_QUEUE/pending" -name '*.md' | wc -l | tr -d ' ')
assert_eq "50" "$CONC50_DEPTH" "50 concurrent pushes: depth"
assert_eq "50" "$CONC50_FILES" "50 concurrent pushes: files"

# ── batch-fsync ──
echo ""
echo "── batch-fsync ──"
BATCH_QUEUE="$TEST_TMPDIR/batchq"
$FBMQ init "$BATCH_QUEUE" 2>/dev/null

# Push with --batch-fsync produces valid message
BATCH_ID=$(echo "# Batch msg" | $FBMQ push "$BATCH_QUEUE" --batch-fsync)
[ ${#BATCH_ID} -eq 32 ] && pass "batch-fsync push returns 32-char ID" || fail "batch-fsync ID" "got ${#BATCH_ID}"
BATCH_FILE=$(find_msg "$BATCH_QUEUE/pending" "$BATCH_ID")
[ -n "$BATCH_FILE" ] && pass "batch-fsync file in pending" || fail "batch-fsync file missing"

# sync command exits 0
$FBMQ sync "$BATCH_QUEUE"
[ $? -eq 0 ] && pass "sync exits 0" || fail "sync exit code"

# Messages consumable after batch push + sync
BATCH_CL=$($FBMQ pop "$BATCH_QUEUE")
[ -n "$BATCH_CL" ] && pass "batch-fsync message consumable" || fail "batch-fsync consume"
$FBMQ ack "$BATCH_QUEUE" "$BATCH_CL"

# --batch-fsync and --no-fsync: mutually exclusive, should error
RC_CONFLICT=0
echo "# Both flags" | $FBMQ push "$BATCH_QUEUE" --batch-fsync --no-fsync >/dev/null 2>&1 || RC_CONFLICT=$?
[ "$RC_CONFLICT" -ne 0 ] && pass "both flags: rejected" || fail "both flags: should error"

# ── count-on-read depth ──
echo ""
echo "── count-on-read depth ──"
COR_QUEUE="$TEST_TMPDIR/corq"
$FBMQ init "$COR_QUEUE" 2>/dev/null
echo "# msg1" | $FBMQ push "$COR_QUEUE" --no-fsync >/dev/null
echo "# msg2" | $FBMQ push "$COR_QUEUE" --no-fsync >/dev/null
# .meta/ directory exists for max_pending config
[ -d "$COR_QUEUE/.meta" ] && pass ".meta/ directory exists" || fail ".meta/ missing"
COR_DEPTH=$($FBMQ depth "$COR_QUEUE")
assert_eq "2" "$COR_DEPTH" "count-on-read depth=2"

# Pop both and verify depth returns to 0
CL1=$($FBMQ pop "$COR_QUEUE")
$FBMQ ack "$COR_QUEUE" "$CL1"
CL2=$($FBMQ pop "$COR_QUEUE")
$FBMQ ack "$COR_QUEUE" "$CL2"
COR_DEPTH_ZERO=$($FBMQ depth "$COR_QUEUE")
assert_eq "0" "$COR_DEPTH_ZERO" "count-on-read depth=0 after drain"

# ── pop set correctness (12 messages) ──
echo ""
echo "── pop set correctness (12 messages) ──"
FIFO12_QUEUE="$TEST_TMPDIR/fifo12q"
$FBMQ init "$FIFO12_QUEUE" --max-pending 0 2>/dev/null

declare -a FIFO12_IDS=()
for i in $(seq 1 12); do
    sleep 0.1
    FID=$(echo "# FIFO msg $i" | $FBMQ push "$FIFO12_QUEUE" --no-fsync)
    FIFO12_IDS+=("$FID")
done

# Pop all and record order
declare -a FIFO12_POP=()
for i in $(seq 1 12); do
    CL=$($FBMQ pop "$FIFO12_QUEUE") || break
    MID=$(grep -i '^Id:' "$CL" | sed 's/^Id: *//')
    FIFO12_POP+=("$MID")
    $FBMQ ack "$FIFO12_QUEUE" "$CL"
done

[ ${#FIFO12_POP[@]} -eq 12 ] && pass "FIFO12: all 12 popped" || fail "FIFO12: lost messages"

assert_set_eq "FIFO12: correct message set" "${FIFO12_IDS[@]}" -- "${FIFO12_POP[@]}"

# Strict FIFO order check
FIFO12_OK=true
for i in $(seq 0 11); do
    [ "${FIFO12_POP[$i]}" = "${FIFO12_IDS[$i]}" ] || FIFO12_OK=false
done
$FIFO12_OK && pass "FIFO12: strict FIFO order" || fail "FIFO12: order mismatch"

# ── depth on nonexistent path (#4) ──
echo ""
echo "── depth error handling ──"
RC_DEPTH=0
$FBMQ depth /nonexistent/path/to/queue >/dev/null 2>&1 || RC_DEPTH=$?
[ "$RC_DEPTH" -ne 0 ] && pass "depth returns error for bad path" || fail "depth should error"

# ── reap ignores malformed filenames (#5) ──
echo ""
echo "── reap malformed filename ──"
REAP_QUEUE="$TEST_TMPDIR/reapq"
$FBMQ init "$REAP_QUEUE" 2>/dev/null
# Place a malformed file (non-numeric prefix) in processing/
echo "# bogus" > "$REAP_QUEUE/processing/abc.deadbeef01234567890abcdef01234.md"
# Place a valid expired file too
PAST=$(($(date +%s) - 999999))
PAST_NS="${PAST}000000000"
echo "# stale" > "$REAP_QUEUE/processing/${PAST_NS}.1234567890123456789012345678abcd.md"
REAP_OUT=$($FBMQ reap "$REAP_QUEUE" -l 1 2>&1)
# Malformed file should still be in processing (not reaped)
[ -f "$REAP_QUEUE/processing/abc.deadbeef01234567890abcdef01234.md" ] \
    && pass "reap: malformed file untouched" || fail "reap: malformed file removed"
# Valid expired file should have been reaped
[ ! -f "$REAP_QUEUE/processing/${PAST_NS}.1234567890123456789012345678abcd.md" ] \
    && pass "reap: expired file reaped" || fail "reap: expired file not reaped"

# ── stress: push 50, pop all, reap, purge (#3) ──
echo ""
echo "── stress: push/pop/reap/purge ──"
STRESS_QUEUE="$TEST_TMPDIR/stressq"
$FBMQ init "$STRESS_QUEUE" 2>/dev/null
for i in $(seq 1 50); do
    echo "# Stress $i" | $FBMQ push "$STRESS_QUEUE" --no-fsync &
done
wait
# Pop all
STRESS_POPPED=0
while true; do
    CL=$($FBMQ pop "$STRESS_QUEUE" 2>/dev/null) || break
    $FBMQ ack "$STRESS_QUEUE" "$CL"
    STRESS_POPPED=$((STRESS_POPPED + 1))
done
assert_eq "50" "$STRESS_POPPED" "stress: popped all 50"
# Reap should succeed (nothing to reap)
$FBMQ reap "$STRESS_QUEUE" -l 0 2>/dev/null
[ $? -eq 0 ] && pass "stress: reap succeeds" || fail "stress: reap failed"
# Purge done messages
$FBMQ purge "$STRESS_QUEUE" -a 0 2>/dev/null
[ $? -eq 0 ] && pass "stress: purge succeeds" || fail "stress: purge failed"
DONE_LEFT=$(find "$STRESS_QUEUE/done" -name '*.md' 2>/dev/null | wc -l | tr -d ' ')
assert_eq "0" "$DONE_LEFT" "stress: done/ empty after purge"

# ── nack returns correctly formatted filename (#9) ──
echo ""
echo "── nack filename format ──"
NACK_QUEUE="$TEST_TMPDIR/nackq"
$FBMQ init "$NACK_QUEUE" 2>/dev/null
NACK_ID=$(echo "# Nack test" | $FBMQ push "$NACK_QUEUE" --no-fsync)
NACK_CL=$($FBMQ pop "$NACK_QUEUE")
$FBMQ nack "$NACK_QUEUE" "$NACK_CL"
# Find the nacked message back in pending
NACK_FILE=$(find_msg "$NACK_QUEUE/pending" "$NACK_ID")
[ -n "$NACK_FILE" ] && pass "nack: message returned to pending" || fail "nack: message lost"
# Verify the filename has the expected format (<timestamp>.<hash>.md)
NACK_BASE=$(basename "$NACK_FILE")
echo "$NACK_BASE" | grep -qE '^[0-9]+\.[0-9a-f]{32}\.md$' && pass "nack: filename format correct" \
    || fail "nack: bad filename format" "$NACK_BASE"

# ── timestamp stripping lifecycle ──
echo ""
echo "── timestamp stripping lifecycle ──"
TS_STRIP_QUEUE="$TEST_TMPDIR/tsstripq"
$FBMQ init "$TS_STRIP_QUEUE" 2>/dev/null

# Push: pending/ should have <enqueue_ts>.<hash>.md
TS_STRIP_ID=$(echo "# Lifecycle test" | $FBMQ push "$TS_STRIP_QUEUE" --no-fsync)
TS_STRIP_PENDING=$(find_msg "$TS_STRIP_QUEUE/pending" "$TS_STRIP_ID")
TS_STRIP_PBASE=$(basename "$TS_STRIP_PENDING")
echo "$TS_STRIP_PBASE" | grep -qE '^[0-9]+\.[0-9a-f]{32}\.md$' \
    && pass "lifecycle: pending has <enqueue_ts>.<hash>.md" \
    || fail "lifecycle: pending format" "$TS_STRIP_PBASE"

# Pop: processing/ should have <claim_ts>.<hash>.md (single timestamp, no enqueue ts)
TS_STRIP_CL=$($FBMQ pop "$TS_STRIP_QUEUE")
TS_STRIP_CBASE=$(basename "$TS_STRIP_CL")
echo "$TS_STRIP_CBASE" | grep -qE '^[0-9]+\.[0-9a-f]{32}\.md$' \
    && pass "lifecycle: processing has <claim_ts>.<hash>.md" \
    || fail "lifecycle: processing format" "$TS_STRIP_CBASE"
# Verify exactly 2 dots: <claim_ts>.<hash>.md (not 3 from double timestamp)
TS_STRIP_DOTS=$(echo "$TS_STRIP_CBASE" | tr -cd '.' | wc -c | tr -d ' ')
assert_eq "2" "$TS_STRIP_DOTS" "lifecycle: processing filename has exactly 2 dots"

# Ack: done/ should have <hash>.md (no timestamps at all)
$FBMQ ack "$TS_STRIP_QUEUE" "$TS_STRIP_CL"
TS_STRIP_DONE=$(find "$TS_STRIP_QUEUE/done" -name '*.md' -print -quit)
TS_STRIP_DBASE=$(basename "$TS_STRIP_DONE")
echo "$TS_STRIP_DBASE" | grep -qE '^[0-9a-f]{32}\.md$' \
    && pass "lifecycle: done has <hash>.md" \
    || fail "lifecycle: done format" "$TS_STRIP_DBASE"
assert_eq "$TS_STRIP_ID.md" "$TS_STRIP_DBASE" "lifecycle: done filename is exactly <id>.md"

# Nack path: push another, pop, nack — pending/ should have <ts>.<hash>.md
TS_STRIP_ID2=$(echo "# Nack lifecycle" | $FBMQ push "$TS_STRIP_QUEUE" --no-fsync)
TS_STRIP_CL2=$($FBMQ pop "$TS_STRIP_QUEUE")
$FBMQ nack "$TS_STRIP_QUEUE" "$TS_STRIP_CL2"
TS_STRIP_NACKED=$(find_msg "$TS_STRIP_QUEUE/pending" "$TS_STRIP_ID2")
TS_STRIP_NBASE=$(basename "$TS_STRIP_NACKED")
echo "$TS_STRIP_NBASE" | grep -qE '^[0-9]+\.[0-9a-f]{32}\.md$' \
    && pass "lifecycle: nack-to-pending has <ts>.<hash>.md" \
    || fail "lifecycle: nack-to-pending format" "$TS_STRIP_NBASE"

# Dead-letter path: drain queue, nack until dead-lettered, verify failed/ has <hash>.md
while CL=$($FBMQ pop "$TS_STRIP_QUEUE" 2>/dev/null); do
    $FBMQ ack "$TS_STRIP_QUEUE" "$CL"
done
TS_STRIP_ID3=$(echo "# DL lifecycle" | $FBMQ push "$TS_STRIP_QUEUE" --no-fsync)
for i in $(seq 1 5); do
    CL=$($FBMQ pop "$TS_STRIP_QUEUE" 2>/dev/null) || break
    $FBMQ nack "$TS_STRIP_QUEUE" "$CL" 2>/dev/null || true
done
TS_STRIP_DL=$(find "$TS_STRIP_QUEUE/failed" -name "$TS_STRIP_ID3.md" -print -quit)
[ -n "$TS_STRIP_DL" ] && pass "lifecycle: dead-letter is <hash>.md" \
    || fail "lifecycle: dead-letter format"

# ── oversized tags field rejected (#11) ──
echo ""
echo "── oversized field rejection ──"
# Create a message file with a tags field > 1024 bytes
LONG_TAGS=$(printf '%0*d' 1100 0 | tr '0' 'x')
cat > "$TEST_TMPDIR/long_tags.md" <<EOF
Id: aaaabbbbccccddddaaaabbbbccccdddd
Created-At: 2026-01-01T00:00:00Z
Priority: normal
Retry-Count: 0
Tags: $LONG_TAGS

body
EOF
RC_LONG=0
$FBMQ inspect "$TEST_TMPDIR/long_tags.md" >/dev/null 2>&1 || RC_LONG=$?
[ "$RC_LONG" -ne 0 ] && pass "oversized tags field rejected" || fail "oversized tags accepted"

# ── .tmp/ orphan cleanup (#16) ──
echo ""
echo "── .tmp/ orphan cleanup ──"
TMP_QUEUE="$TEST_TMPDIR/tmpcleanq"
$FBMQ init "$TMP_QUEUE" 2>/dev/null
# Place a stale orphan (old mtime) in .tmp/
echo "# orphan" > "$TMP_QUEUE/.tmp/stale_orphan.md"
touch -t 202001010000.00 "$TMP_QUEUE/.tmp/stale_orphan.md"
# Place a fresh file in .tmp/ (should be preserved)
echo "# fresh" > "$TMP_QUEUE/.tmp/fresh_file.md"
# Run reap with lease_timeout=1
$FBMQ reap "$TMP_QUEUE" -l 1 2>/dev/null
# Stale orphan should be deleted
[ ! -f "$TMP_QUEUE/.tmp/stale_orphan.md" ] && pass "tmp cleanup: stale orphan removed" \
    || fail "tmp cleanup: stale orphan still exists"
# Fresh file should be preserved
[ -f "$TMP_QUEUE/.tmp/fresh_file.md" ] && pass "tmp cleanup: fresh file preserved" \
    || fail "tmp cleanup: fresh file removed"

# ── concurrent pop (race contention) ──
echo ""
echo "── concurrent pop (race contention) ──"
RACE_QUEUE="$TEST_TMPDIR/raceq"
$FBMQ init "$RACE_QUEUE" 2>/dev/null
# Push 20 messages
for i in $(seq 1 20); do
    echo "# Race msg $i" | $FBMQ push "$RACE_QUEUE" --no-fsync >/dev/null
done
# Pop all 20 from 10 concurrent consumers — each pops in a loop
RACE_DIR="$TEST_TMPDIR/race_results"
mkdir -p "$RACE_DIR"
for w in $(seq 1 10); do
    (
        count=0
        while true; do
            CL=$($FBMQ pop "$RACE_QUEUE" 2>/dev/null) || break
            $FBMQ ack "$RACE_QUEUE" "$CL" 2>/dev/null
            count=$((count + 1))
        done
        echo "$count" > "$RACE_DIR/worker_$w"
    ) &
done
wait
TOTAL_POPPED=0
for w in $(seq 1 10); do
    WC=$(cat "$RACE_DIR/worker_$w" 2>/dev/null || echo 0)
    TOTAL_POPPED=$((TOTAL_POPPED + WC))
done
assert_eq "20" "$TOTAL_POPPED" "concurrent pop: all 20 claimed exactly once"
RACE_DEPTH=$($FBMQ depth "$RACE_QUEUE")
assert_eq "0" "$RACE_DEPTH" "concurrent pop: queue drained"

# ── concurrent push+pop (simultaneous producers and consumers) ──
echo ""
echo "── concurrent push+pop ──"
PUSHPOP_QUEUE="$TEST_TMPDIR/pushpopq"
$FBMQ init "$PUSHPOP_QUEUE" 2>/dev/null
PUSHPOP_DIR="$TEST_TMPDIR/pushpop_results"
mkdir -p "$PUSHPOP_DIR"
# 5 producers push 10 messages each = 50 total
for p in $(seq 1 5); do
    (
        for i in $(seq 1 10); do
            echo "# Producer $p msg $i" | $FBMQ push "$PUSHPOP_QUEUE" --no-fsync >/dev/null
        done
    ) &
done
# 5 consumers pop in a loop (with a timeout to avoid infinite wait)
for c in $(seq 1 5); do
    (
        count=0
        attempts=0
        while [ $attempts -lt 200 ]; do
            CL=$($FBMQ pop "$PUSHPOP_QUEUE" 2>/dev/null) || { attempts=$((attempts + 1)); sleep 0.05; continue; }
            $FBMQ ack "$PUSHPOP_QUEUE" "$CL" 2>/dev/null
            count=$((count + 1))
            attempts=0
        done
        echo "$count" > "$PUSHPOP_DIR/consumer_$c"
    ) &
done
wait
PUSHPOP_CONSUMED=0
for c in $(seq 1 5); do
    CC=$(cat "$PUSHPOP_DIR/consumer_$c" 2>/dev/null || echo 0)
    PUSHPOP_CONSUMED=$((PUSHPOP_CONSUMED + CC))
done
# All 50 messages should have been consumed (none lost, none duplicated)
PUSHPOP_REMAIN=$($FBMQ depth "$PUSHPOP_QUEUE")
PUSHPOP_TOTAL=$((PUSHPOP_CONSUMED + PUSHPOP_REMAIN))
assert_eq "50" "$PUSHPOP_TOTAL" "push+pop: no messages lost (consumed=$PUSHPOP_CONSUMED remain=$PUSHPOP_REMAIN)"

# ── reap during active processing ──
echo ""
echo "── reap during active processing ──"
REAP_RACE_QUEUE="$TEST_TMPDIR/reapraceq"
$FBMQ init "$REAP_RACE_QUEUE" 2>/dev/null
# Push 5 messages
for i in $(seq 1 5); do
    echo "# Reap race $i" | $FBMQ push "$REAP_RACE_QUEUE" --no-fsync >/dev/null
done
# Pop all 5 (leave in processing)
REAP_CLAIMED=()
for i in $(seq 1 5); do
    CL=$($FBMQ pop "$REAP_RACE_QUEUE") || break
    REAP_CLAIMED+=("$CL")
done
# Run reap with a very long lease (should NOT reap any)
$FBMQ reap "$REAP_RACE_QUEUE" -l 9999 2>/dev/null
PROC_COUNT=$(find "$REAP_RACE_QUEUE/processing" -name '*.md' | wc -l | tr -d ' ')
assert_eq "5" "$PROC_COUNT" "reap: active messages not reaped (lease=9999)"
# Ack them all to clean up
for CL in "${REAP_CLAIMED[@]}"; do
    $FBMQ ack "$REAP_RACE_QUEUE" "$CL" 2>/dev/null
done

# ── claim timestamp precision ──
echo ""
echo "── claim timestamp precision ──"
TS_QUEUE="$TEST_TMPDIR/tsq"
$FBMQ init "$TS_QUEUE" 2>/dev/null
echo "# Timestamp test" | $FBMQ push "$TS_QUEUE" --no-fsync >/dev/null
TS_CL=$($FBMQ pop "$TS_QUEUE")
TS_BASE=$(basename "$TS_CL")
# Extract claim timestamp (first dot-delimited segment)
CLAIM_TS=$(echo "$TS_BASE" | cut -d. -f1)
CLAIM_DIGITS=${#CLAIM_TS}
[ "$CLAIM_DIGITS" -ge 18 ] && pass "claim timestamp >= 18 digits (got $CLAIM_DIGITS)" \
    || fail "claim timestamp too short" "expected >= 18 digits, got $CLAIM_DIGITS"
$FBMQ ack "$TS_QUEUE" "$TS_CL"

# ── depth full-scan correctness ──
echo ""
echo "── depth full-scan correctness ──"
DEPTH_QUEUE="$TEST_TMPDIR/depthq"
$FBMQ init "$DEPTH_QUEUE" 2>/dev/null
for i in $(seq 1 100); do
    echo "# Depth msg $i" | $FBMQ push "$DEPTH_QUEUE" --no-fsync >/dev/null &
done
wait
DEPTH_FILES=$(find "$DEPTH_QUEUE/pending" -name '*.md' | wc -l | tr -d ' ')
DEPTH_REPORT=$($FBMQ depth "$DEPTH_QUEUE")
assert_eq "$DEPTH_FILES" "$DEPTH_REPORT" "depth matches file count for 100 messages (files=$DEPTH_FILES)"

# ── reap with >64 processing entries ──
echo ""
echo "── reap with >64 processing entries ──"
REAP64_QUEUE="$TEST_TMPDIR/reap64q"
$FBMQ init "$REAP64_QUEUE" 2>/dev/null
for i in $(seq 1 70); do
    echo "# Reap64 msg $i" | $FBMQ push "$REAP64_QUEUE" --no-fsync >/dev/null &
done
wait
# Pop all 70 (leave in processing)
REAP64_POPPED=0
while true; do
    CL=$($FBMQ pop "$REAP64_QUEUE" 2>/dev/null) || break
    REAP64_POPPED=$((REAP64_POPPED + 1))
done
assert_eq "70" "$REAP64_POPPED" "reap64: popped all 70"
# Rename all processing files to have old claim timestamps so reap will collect them
# Processing filenames: <claim_ts>.<hash>.md — unique by hash
PAST_TS=$(($(date +%s) - 999999))
PAST_NS_TS="${PAST_TS}000000000"
for F in "$REAP64_QUEUE/processing/"*.md; do
    BASE=$(basename "$F")
    # Replace claim timestamp prefix (first segment) with old one; rest is unique
    REST=$(echo "$BASE" | sed 's/^[0-9]*\.//')
    mv "$F" "$REAP64_QUEUE/processing/${PAST_NS_TS}.${REST}"
done
# Reap with lease=1 (all are expired)
$FBMQ reap "$REAP64_QUEUE" -l 1 2>/dev/null
PROC_LEFT=$(find "$REAP64_QUEUE/processing" -name '*.md' | wc -l | tr -d ' ')
assert_eq "0" "$PROC_LEFT" "reap64: processing/ empty after reap (all 70 reaped)"

# ── fbmq-reaper script ──
echo ""
echo "── fbmq-reaper script ──"
REAPER_QUEUE="$TEST_TMPDIR/reaperq"
$FBMQ init "$REAPER_QUEUE" 2>/dev/null
echo "# Reaper test" | $FBMQ push "$REAPER_QUEUE" --no-fsync >/dev/null
REAPER_CL=$($FBMQ pop "$REAPER_QUEUE")
# Rename the processing file to have an old claim timestamp (reap checks filename)
REAPER_BASE=$(basename "$REAPER_CL")
REAPER_OLD_TS="$(($(date +%s) - 999999))000000000"
REAPER_NEW_BASE=$(echo "$REAPER_BASE" | sed "s/^[0-9]*\./${REAPER_OLD_TS}./")
mv "$REAPER_CL" "$REAPER_QUEUE/processing/$REAPER_NEW_BASE"
REAPER_OUT=$(FBMQ="$FBMQ" sh scripts/fbmq-reaper "$REAPER_QUEUE" 1 1 2>&1)
REAPER_RC=$?
[ "$REAPER_RC" -eq 0 ] && pass "fbmq-reaper: exits 0" || fail "fbmq-reaper: exit code" "$REAPER_RC"
echo "$REAPER_OUT" | grep -qi "reap" && pass "fbmq-reaper: output mentions reap" \
    || fail "fbmq-reaper: no reap output" "$REAPER_OUT"
# Error case: nonexistent queue
REAPER_ERR_RC=0
FBMQ="$FBMQ" sh scripts/fbmq-reaper /nonexistent/queue 1 1 2>/dev/null || REAPER_ERR_RC=$?
[ "$REAPER_ERR_RC" -ne 0 ] && pass "fbmq-reaper: errors on bad path" \
    || fail "fbmq-reaper: should error on bad path"

# ── max-pending enforcement ──
echo ""
echo "── max-pending enforcement ──"
MAXP_QUEUE="$TEST_TMPDIR/maxpq"
$FBMQ init "$MAXP_QUEUE" --max-pending 3 2>/dev/null
echo "# msg1" | $FBMQ push "$MAXP_QUEUE" --no-fsync >/dev/null
echo "# msg2" | $FBMQ push "$MAXP_QUEUE" --no-fsync >/dev/null
echo "# msg3" | $FBMQ push "$MAXP_QUEUE" --no-fsync >/dev/null
# Fourth push should fail with exit code 2
MAXP_RC=0
echo "# msg4" | $FBMQ push "$MAXP_QUEUE" --no-fsync >/dev/null 2>&1 || MAXP_RC=$?
assert_eq "2" "$MAXP_RC" "max-pending: push rejected at limit"
MAXP_DEPTH=$($FBMQ depth "$MAXP_QUEUE")
assert_eq "3" "$MAXP_DEPTH" "max-pending: depth stays at 3"
# Pop one and push should work again
MAXP_CL=$($FBMQ pop "$MAXP_QUEUE")
$FBMQ ack "$MAXP_QUEUE" "$MAXP_CL"
MAXP_RC2=0
echo "# msg4 retry" | $FBMQ push "$MAXP_QUEUE" --no-fsync >/dev/null 2>&1 || MAXP_RC2=$?
assert_eq "0" "$MAXP_RC2" "max-pending: push succeeds after pop"

# ── max-pending unlimited ──
echo ""
echo "── max-pending unlimited ──"
MAXPU_QUEUE="$TEST_TMPDIR/maxpuq"
$FBMQ init "$MAXPU_QUEUE" --max-pending 0 2>/dev/null
for i in $(seq 1 50); do
    echo "# Unlimited $i" | $FBMQ push "$MAXPU_QUEUE" --no-fsync >/dev/null
done
MAXPU_DEPTH=$($FBMQ depth "$MAXPU_QUEUE")
assert_eq "50" "$MAXPU_DEPTH" "max-pending unlimited: 50 messages pushed"

# ── reap real nanosecond timestamps ──
echo ""
echo "── reap real nanosecond timestamps ──"
REAP_NS_QUEUE="$TEST_TMPDIR/reapnsq"
$FBMQ init "$REAP_NS_QUEUE" 2>/dev/null
echo "# NS reap test" | $FBMQ push "$REAP_NS_QUEUE" --no-fsync >/dev/null
REAP_NS_CL=$($FBMQ pop "$REAP_NS_QUEUE")
# Rename the file to have an old claim timestamp (reap checks filename, not mtime)
REAP_NS_BASE=$(basename "$REAP_NS_CL")
REAP_NS_OLD_TS="$(($(date +%s) - 999999))000000000"
REAP_NS_NEW_BASE=$(echo "$REAP_NS_BASE" | sed "s/^[0-9]*\./${REAP_NS_OLD_TS}./")
REAP_NS_NEW_PATH="$REAP_NS_QUEUE/processing/$REAP_NS_NEW_BASE"
mv "$REAP_NS_CL" "$REAP_NS_NEW_PATH"
$FBMQ reap "$REAP_NS_QUEUE" -l 1 2>/dev/null
[ ! -f "$REAP_NS_NEW_PATH" ] && pass "reap: real ns-prefixed file reaped" \
    || fail "reap: real ns-prefixed file not reaped"

# ── empty purge ──
echo ""
echo "── empty purge ──"
EPURGE_QUEUE="$TEST_TMPDIR/epurgeq"
$FBMQ init "$EPURGE_QUEUE" 2>/dev/null
RC_EPURGE=0
$FBMQ purge "$EPURGE_QUEUE" -a 0 2>/dev/null || RC_EPURGE=$?
assert_eq "0" "$RC_EPURGE" "empty purge: exits 0"

# ── nack atomicity (.tmp/ clean after nack) ──
echo ""
echo "── nack atomicity ──"
NATOM_QUEUE="$TEST_TMPDIR/natomq"
$FBMQ init "$NATOM_QUEUE" 2>/dev/null
echo "# Nack atomicity test" | $FBMQ push "$NATOM_QUEUE" --no-fsync >/dev/null
NATOM_CL=$($FBMQ pop "$NATOM_QUEUE")
$FBMQ nack "$NATOM_QUEUE" "$NATOM_CL"
NATOM_TMP_COUNT=$(find "$NATOM_QUEUE/.tmp" -name '*.md' | wc -l | tr -d ' ')
assert_eq "0" "$NATOM_TMP_COUNT" "nack atomicity: no leftover in .tmp/"

# ── corrupt max_pending warning ──
echo ""
echo "── corrupt max_pending warning ──"
CORRUPT_QUEUE="$TEST_TMPDIR/corruptq"
$FBMQ init "$CORRUPT_QUEUE" 2>/dev/null
echo "not-a-number" > "$CORRUPT_QUEUE/.meta/max_pending"
CORRUPT_STDERR=$(echo "# test" | $FBMQ push "$CORRUPT_QUEUE" --no-fsync 2>&1 >/dev/null)
echo "$CORRUPT_STDERR" | grep -q "warning" && pass "corrupt max_pending: warning emitted" \
    || fail "corrupt max_pending: no warning" "$CORRUPT_STDERR"

# ── nack no-loss (push A, push B, pop, nack, pop all → both consumed) ──
echo ""
echo "── nack no-loss ──"
NFIFO_QUEUE="$TEST_TMPDIR/nfifoq"
$FBMQ init "$NFIFO_QUEUE" --max-pending 0 2>/dev/null
NFIFO_A=$(echo "# Message A" | $FBMQ push "$NFIFO_QUEUE" --no-fsync)
sleep 0.1
NFIFO_B=$(echo "# Message B" | $FBMQ push "$NFIFO_QUEUE" --no-fsync)
# Pop one message, then nack it
NFIFO_CL=$($FBMQ pop "$NFIFO_QUEUE")
$FBMQ nack "$NFIFO_QUEUE" "$NFIFO_CL"
# Pop remaining two messages and verify both A and B are consumed
declare -a NFIFO_GOT=()
for i in 1 2; do
    NCL=$($FBMQ pop "$NFIFO_QUEUE") || break
    NID=$(grep -i '^Id:' "$NCL" | sed 's/^Id: *//')
    NFIFO_GOT+=("$NID")
    $FBMQ ack "$NFIFO_QUEUE" "$NCL"
done
[ ${#NFIFO_GOT[@]} -eq 2 ] && pass "nack-noloss: both messages consumed" || fail "nack-noloss: lost messages"
assert_set_eq "nack-noloss: correct message set" "$NFIFO_A" "$NFIFO_B" -- "${NFIFO_GOT[@]}"

# ── reap: orphan detection with timestamp-prefixed pending entries ──
echo ""
echo "── reap: orphan detection with timestamp-prefixed pending ──"
ORPHAN_QUEUE="$TEST_TMPDIR/orphanq"
$FBMQ init "$ORPHAN_QUEUE" 2>/dev/null
# Push a message and pop it to get a valid message file
echo "# Orphan test" | $FBMQ push "$ORPHAN_QUEUE" --no-fsync >/dev/null
ORPHAN_CL=$($FBMQ pop "$ORPHAN_QUEUE")
# Extract the hash from the claimed path (format: <timestamp>.<hash>.md)
ORPHAN_BASE=$(basename "$ORPHAN_CL")
ORPHAN_HASH=$(echo "$ORPHAN_BASE" | sed 's/^[0-9]*\.//')
# Simulate nack crash window: copy the message back to pending/ with a timestamp
# prefix (as nack would do) AND leave the stale entry in processing/
cp "$ORPHAN_CL" "$ORPHAN_QUEUE/pending/9999999999000000000.${ORPHAN_HASH}"
# Now processing/ has stale entry, pending/ has the re-queued copy
# Reap should detect the pending/ copy and remove the processing/ orphan
sleep 2
$FBMQ reap "$ORPHAN_QUEUE" -l 1 2>/dev/null
# The processing/ orphan should be removed
ORPHAN_PROC_COUNT=$(find "$ORPHAN_QUEUE/processing" -name "*.md" | wc -l | tr -d ' ')
assert_eq "0" "$ORPHAN_PROC_COUNT" "reap orphan: processing/ stale entry removed"
# The pending/ copy should survive
ORPHAN_PEND_COUNT=$(find "$ORPHAN_QUEUE/pending" -name "*.md" | wc -l | tr -d ' ')
assert_eq "1" "$ORPHAN_PEND_COUNT" "reap orphan: pending/ copy preserved"

# ── concurrent pop: no restart thundering herd ──
echo ""
echo "── concurrent pop: heavy contention ──"
HERD_QUEUE="$TEST_TMPDIR/herdq"
$FBMQ init "$HERD_QUEUE" 2>/dev/null
# Push 50 messages
for i in $(seq 1 50); do
    echo "# Herd msg $i" | $FBMQ push "$HERD_QUEUE" --no-fsync >/dev/null
done
# Pop all 50 from 20 concurrent consumers
HERD_DIR="$TEST_TMPDIR/herd_results"
mkdir -p "$HERD_DIR"
for w in $(seq 1 20); do
    (
        count=0
        while true; do
            CL=$($FBMQ pop "$HERD_QUEUE" 2>/dev/null) || break
            $FBMQ ack "$HERD_QUEUE" "$CL" 2>/dev/null
            count=$((count + 1))
        done
        echo "$count" > "$HERD_DIR/worker_$w"
    ) &
done
wait
HERD_TOTAL=0
for w in $(seq 1 20); do
    WC=$(cat "$HERD_DIR/worker_$w" 2>/dev/null || echo 0)
    HERD_TOTAL=$((HERD_TOTAL + WC))
done
assert_eq "50" "$HERD_TOTAL" "heavy contention: all 50 claimed exactly once"
HERD_DEPTH=$($FBMQ depth "$HERD_QUEUE")
assert_eq "0" "$HERD_DEPTH" "heavy contention: queue drained"

# ── reply-to ──
echo ""
echo "── reply-to ──"
RT_QUEUE="$TEST_TMPDIR/rtq"
$FBMQ init "$RT_QUEUE" --max-pending 0 2>/dev/null

# Header serialized with --reply-to
RT_ID=$(echo "# Reply test" | $FBMQ push "$RT_QUEUE" --reply-to /tmp/replies)
RT_FILE=$(find_msg "$RT_QUEUE/pending" "$RT_ID")
grep -q '^Reply-To: /tmp/replies' "$RT_FILE" && pass "reply-to: header serialized" || fail "reply-to: header missing"

# Short flag -r
RT_ID2=$(echo "# Short flag" | $FBMQ push "$RT_QUEUE" -r /tmp/results)
RT_FILE2=$(find_msg "$RT_QUEUE/pending" "$RT_ID2")
grep -q '^Reply-To: /tmp/results' "$RT_FILE2" && pass "reply-to: short flag -r works" || fail "reply-to: short flag"

# Inspect displays Reply-To
RT_INSPECT=$($FBMQ inspect "$RT_FILE")
echo "$RT_INSPECT" | grep -q "Reply-To:" && pass "reply-to: inspect displays it" || fail "reply-to: inspect missing"

# Combined with Correlation-Id
RT_ID3=$(echo "# Both headers" | $FBMQ push "$RT_QUEUE" -c req-99 -r /tmp/combo)
RT_FILE3=$(find_msg "$RT_QUEUE/pending" "$RT_ID3")
grep -q '^Correlation-Id: req-99' "$RT_FILE3" && grep -q '^Reply-To: /tmp/combo' "$RT_FILE3" \
    && pass "reply-to: combined with correlation-id" || fail "reply-to: combined headers"

# Absent when unset
RT_ID4=$(echo "# No reply-to" | $FBMQ push "$RT_QUEUE")
RT_FILE4=$(find_msg "$RT_QUEUE/pending" "$RT_ID4")
grep -q '^Reply-To:' "$RT_FILE4" && fail "reply-to: present when unset" || pass "reply-to: absent when unset"

# ── depends-on + ready ──
echo ""
echo "── depends-on + ready ──"
DEP_QUEUE="$TEST_TMPDIR/depq"
$FBMQ init "$DEP_QUEUE" --max-pending 0 2>/dev/null

# Push A (no deps), B (depends on A), C (depends on A + B), D (no deps)
A=$(echo "task A" | $FBMQ push "$DEP_QUEUE")
B=$(echo "task B" | $FBMQ push "$DEP_QUEUE" -d "$A")
C=$(echo "task C" | $FBMQ push "$DEP_QUEUE" -d "$A" -d "$B")
D=$(echo "task D" | $FBMQ push "$DEP_QUEUE")

# Verify Depends-On header is serialized in message files
B_FILE=$(find_msg "$DEP_QUEUE/pending" "$B")
grep -q "Depends-On: $A" "$B_FILE" && pass "depends-on: header serialized (single dep)" || fail "depends-on: header serialized (single dep)"

C_FILE=$(find_msg "$DEP_QUEUE/pending" "$C")
grep -q "Depends-On: $A, $B" "$C_FILE" && pass "depends-on: header serialized (multi dep)" || fail "depends-on: header serialized (multi dep)"

# Verify inspect shows Depends-On
INSPECT_OUT=$($FBMQ inspect "$B_FILE")
echo "$INSPECT_OUT" | grep -q "Depends-On:" && pass "depends-on: inspect displays header" || fail "depends-on: inspect displays header"

# Verify ready lists only A and D initially
READY_IDS=$($FBMQ ready "$DEP_QUEUE")
echo "$READY_IDS" | grep -q "$A" && pass "ready: A is ready (no deps)" || fail "ready: A is ready (no deps)"
echo "$READY_IDS" | grep -q "$D" && pass "ready: D is ready (no deps)" || fail "ready: D is ready (no deps)"
echo "$READY_IDS" | grep -q "$B" && fail "ready: B should not be ready" || pass "ready: B not ready (dep A unmet)"
echo "$READY_IDS" | grep -q "$C" && fail "ready: C should not be ready" || pass "ready: C not ready (deps unmet)"

# Simulate completing A by moving its pending file to done/
A_PEND=$(find_msg "$DEP_QUEUE/pending" "$A")
mv "$A_PEND" "$DEP_QUEUE/done/${A}.md"
READY_IDS2=$($FBMQ ready "$DEP_QUEUE")
echo "$READY_IDS2" | grep -q "$B" && pass "ready: B ready after A done" || fail "ready: B ready after A done"
echo "$READY_IDS2" | grep -q "$C" && fail "ready: C should not be ready (B unmet)" || pass "ready: C not ready (B unmet)"

# Simulate completing B
B_PEND=$(find_msg "$DEP_QUEUE/pending" "$B")
mv "$B_PEND" "$DEP_QUEUE/done/${B}.md"
READY_IDS3=$($FBMQ ready "$DEP_QUEUE")
echo "$READY_IDS3" | grep -q "$C" && pass "ready: C ready after A+B done" || fail "ready: C ready after A+B done"

# Simulate completing C and D, then check exit code
C_PEND=$(find_msg "$DEP_QUEUE/pending" "$C")
mv "$C_PEND" "$DEP_QUEUE/done/${C}.md"
D_PEND=$(find_msg "$DEP_QUEUE/pending" "$D")
mv "$D_PEND" "$DEP_QUEUE/done/${D}.md"
$FBMQ ready "$DEP_QUEUE" >/dev/null 2>&1 && fail "ready: exit 0 on empty queue" || pass "ready: exit 1 when no ready msgs"

# ── Summary ──
echo ""
echo "╔══════════════════════════════════════╗"
printf "║  Results: \033[32m%d passed\033[0m, \033[31m%d failed\033[0m        ║\n" "$PASS" "$FAIL"
echo "╚══════════════════════════════════════╝"

[ "$FAIL" -eq 0 ] && exit 0 || exit 1
