#!/usr/bin/env bash
# End-to-end test of the real binary: `axon serve`, `axon list` and `axon fetch`
# as separate processes over loopback, results compared with diff -r.
# Usage: serve_fetch.sh path/to/axon
set -euo pipefail

AXON=$1
WORK=$(mktemp -d)
SERVER_PID=

cleanup() {
    if [[ -n $SERVER_PID ]]; then
        kill -KILL "$SERVER_PID" 2> /dev/null || true
    fi
    rm -rf "$WORK"
}
trap cleanup EXIT

fail() {
    echo "FAIL: $*"
    echo "--- server log"
    cat "$WORK/server.log" 2> /dev/null || true
    exit 1
}

# expect_rc CODE COMMAND...: runs COMMAND and checks its exit code.
expect_rc() {
    local want=$1 got=0
    shift
    "$@" > "$WORK/out.txt" 2>&1 || got=$?
    if [[ $got != "$want" ]]; then
        cat "$WORK/out.txt"
        fail "expected exit code $want, got $got: $*"
    fi
}

# --- a dataset with awkward names and sizes ---
DATA=$WORK/data
mkdir -p "$DATA/mixed" "$DATA/bulk" "$DATA/linked" "$DATA/empty"
: > "$DATA/mixed/empty"
head -c 1 /dev/urandom > "$DATA/mixed/one-byte"
echo spaces > "$DATA/mixed/name with spaces.txt"
echo utf8 > "$DATA/mixed/café-日本.txt"
echo dash > "$DATA/mixed/-starts-with-dash"
echo longest > "$DATA/mixed/$(printf 'L%.0s' $(seq 255))"
for i in $(seq 40); do
    head -c $((i * 997)) /dev/urandom > "$DATA/mixed/small-$i.bin"
done
head -c 20000000 /dev/urandom > "$DATA/bulk/20MB.bin"
echo real > "$DATA/linked/real.txt"
ln -s /etc/passwd "$DATA/linked/passwd"  # must not be served

# start_server [EXTRA ARGS...]: starts `axon serve` on a free port and sets
# SERVER_PID, PORT and SERVER.
start_server() {
    : > "$WORK/server.log"
    "$AXON" serve --root "$DATA" --port 0 --bind 127.0.0.1 "$@" 2> "$WORK/server.log" &
    SERVER_PID=$!
    for _ in $(seq 100); do
        grep -q "serving" "$WORK/server.log" && break
        kill -0 "$SERVER_PID" 2> /dev/null || fail "server exited during startup"
        sleep 0.1
    done
    PORT=$(grep -o '127\.0\.0\.1:[0-9]*' "$WORK/server.log" | head -1 | cut -d: -f2)
    [[ -n $PORT ]] || fail "server never reported its port"
    SERVER=127.0.0.1:$PORT
}

# stop_server: Ctrl+C, and the server must exit 0.
stop_server() {
    local rc=0
    kill -INT "$SERVER_PID"
    wait "$SERVER_PID" || rc=$?
    SERVER_PID=
    [[ $rc == 0 ]] || fail "server exited with $rc after SIGINT"
    grep -q "stopped" "$WORK/server.log" || fail "server did not log a clean stop"
}

# --- start the server ---
start_server
grep -q "skipped linked/passwd: symlinks are not followed" "$WORK/server.log" ||
    fail "symlink was not reported as skipped"

# --- list ---
expect_rc 0 "$AXON" list "$SERVER"
grep -Eq '^bulk +1 +20\.0 MB$' "$WORK/out.txt" || fail "list: bad bulk line: $(cat "$WORK/out.txt")"
grep -Eq '^mixed +46 ' "$WORK/out.txt" || fail "list: bad mixed line: $(cat "$WORK/out.txt")"
grep -Eq '^empty +0 +0 B$' "$WORK/out.txt" || fail "list: bad empty line: $(cat "$WORK/out.txt")"

# --- fetch every topic and compare ---
OUT=$WORK/out
for topic in mixed bulk empty; do
    expect_rc 0 "$AXON" fetch "$SERVER" "$topic" --output "$OUT"
    diff -r "$DATA/$topic" "$OUT/$topic" > /dev/null || fail "fetch $topic: files differ"
done
for n in 2 4 8; do
    expect_rc 0 "$AXON" fetch "$SERVER" mixed --output "$WORK/out-$n" --connections "$n"
    diff -r "$DATA/mixed" "$WORK/out-$n/mixed" > /dev/null || fail "fetch with $n connections: files differ"
done
expect_rc 2 "$AXON" fetch "$SERVER" mixed --connections 0
expect_rc 2 "$AXON" fetch "$SERVER" mixed --connections 65
expect_rc 0 "$AXON" fetch "$SERVER" linked --output "$OUT"
[[ $(ls -A "$OUT/linked") == real.txt ]] || fail "linked: expected only real.txt, got $(ls -A "$OUT/linked")"
[[ -z $(find "$OUT" -name '.axon-*') ]] || fail "temporary files left behind"

# A second fetch over the first replaces the files and keeps what else is there.
echo "not from the server" > "$OUT/mixed/mine.txt"
echo stale > "$OUT/mixed/one-byte"
expect_rc 0 "$AXON" fetch "$SERVER" mixed --output "$OUT"
cmp -s "$DATA/mixed/one-byte" "$OUT/mixed/one-byte" || fail "refetch did not replace one-byte"
[[ -f $OUT/mixed/mine.txt ]] || fail "refetch removed a file it does not own"

# --- errors and their exit codes ---
expect_rc 1 "$AXON" fetch "$SERVER" zebras --output "$OUT"
grep -q "UNKNOWN_TOPIC" "$WORK/out.txt" || fail "unknown topic not reported"
[[ ! -e $OUT/zebras ]] || fail "a folder was created for an unknown topic"
expect_rc 2 "$AXON" fetch "$SERVER" ../etc
expect_rc 2 "$AXON" fetch "$SERVER"
expect_rc 2 "$AXON" fetch "::1:$PORT" mixed
expect_rc 2 "$AXON" list "$SERVER" extra
expect_rc 2 "$AXON" serve --root "$DATA" --port 70000
expect_rc 1 "$AXON" serve --root "$WORK/missing" --port 0
expect_rc 1 "$AXON" serve --root "$DATA" --port "$PORT" --bind 127.0.0.1  # port taken

# --- Ctrl+C stops the server cleanly ---
exec 3<> "/dev/tcp/127.0.0.1/$PORT"  # an idle client still connected
stop_server
exec 3>&-

# --- the same downloads through a server that cuts 70% of connections ---
start_server --fail-prob 0.7 --seed 1
grep -q "fault injection: each connection is cut with probability 0.7" "$WORK/server.log" ||
    fail "fault injection not announced"
OUT=$WORK/out-faulty
for topic in mixed bulk; do
    expect_rc 0 "$AXON" fetch "$SERVER" "$topic" --output "$OUT"
    diff -r "$DATA/$topic" "$OUT/$topic" > /dev/null || fail "faulty fetch $topic: files differ"
done
expect_rc 0 "$AXON" fetch "$SERVER" mixed --output "$WORK/out-faulty-4" --connections 4
diff -r "$DATA/mixed" "$WORK/out-faulty-4/mixed" > /dev/null || fail "faulty fetch with 4 connections: files differ"
[[ -z $(find "$OUT" "$WORK/out-faulty-4" -name '.axon-*') ]] || fail "temporary files left behind after faults"
cuts=$(grep -c "fault injection cut the connection" "$WORK/server.log" || true)
[[ $cuts -ge 3 ]] || fail "expected several injected cuts, saw $cuts"
# Every byte that got through a cut was kept, so no session was sent anything twice.
sessions=$(grep -c "session .* done:" "$WORK/server.log" || true)
[[ $sessions == 3 ]] || fail "expected 3 finished sessions, saw $sessions"
if grep "session .* done:" "$WORK/server.log" | grep -qv ", 0 resent,"; then
    fail "a session was sent bytes twice: $(grep 'session .* done:' "$WORK/server.log")"
fi
stop_server

echo "PASS ($cuts injected cuts survived)"
