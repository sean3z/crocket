#!/usr/bin/env bash
# Logging over real sockets, with stderr to a file and to a pipe:
#   - the line format, byte for byte, against a golden file (times, durations
#     and the port normalized), for a fixed set of requests
#   - redaction: the password never reaches the output
#   - no lost lines at moderate load (h2load or curl)
#   - order within a connection: each connection's numbered lines in order
#   logging.sh <crocket_log_server> <golden file> [--write-golden]
set -u
SERVER=$1
GOLDEN=$2
PORT=${CROCKET_TEST_PORT:-18452}
BASE="http://127.0.0.1:$PORT"
WORK=$(mktemp -d)
FAILS=0
SRV_PID=
trap '[[ -n $SRV_PID ]] && kill $SRV_PID 2>/dev/null; rm -rf "$WORK"' EXIT

pass() { echo "  ok   $1"; }
fail() { echo "  FAIL $1"; FAILS=$((FAILS + 1)); }
expect() { if [[ "$3" == "$2" ]]; then pass "$1"; else fail "$1 (expected '$2', got '$3')"; fi; }

start() { # mode(file|pipe) logfile
  # The buffer holds more than every line a mode logs (about 43k), so a drop can
  # only mean a lost line, never the writer briefly stalling on a slow disk or a
  # busy pipe reader (which overflowed the default 10k once on a 2-vCPU runner).
  export CROCKET_LOG_BUFFER=1000000
  if [[ $1 == pipe ]]; then
    ( CROCKET_PORT=$PORT CROCKET_H2_PRIOR_KNOWLEDGE=1 CROCKET_EVENT_LOOPS=2 CROCKET_WORKERS=2 "$SERVER" 2>&1 >/dev/null | cat > "$2" ) &
  else
    CROCKET_PORT=$PORT CROCKET_H2_PRIOR_KNOWLEDGE=1 CROCKET_EVENT_LOOPS=2 CROCKET_WORKERS=2 "$SERVER" >/dev/null 2>"$2" &
  fi
  SRV_PID=$!
  for _ in $(seq 1 100); do curl -s -o /dev/null "$BASE/ok" && return 0; sleep 0.1; done
  fail "server did not start"; exit 1
}
stop() { # graceful: the writer drains before exit
  pkill -TERM -x "$(basename "$SERVER" | cut -c1-15)"
  wait $SRV_PID 2>/dev/null
  SRV_PID=
}
normalize() { # the parts that differ run to run
  sed -E -e 's/"ts":"[0-9T:.Z-]+"/"ts":"TS"/' -e 's/"duration_ms":[0-9.]+/"duration_ms":D/' \
         -e "s/:$PORT\\b/:PORT/g" -e 's/"routes":[0-9]+/"routes":N/'
}

fixed_requests() {
  local c=(curl -s -o /dev/null -A crocket-test)
  "${c[@]}" -H 'x-request-id: req-ok-1' "$BASE/ok"
  "${c[@]}" -H 'x-request-id: req-fail-1' "$BASE/fail"
  "${c[@]}" -H 'x-request-id: req-missing-1' "$BASE/missing"
  "${c[@]}" -H 'x-request-id: req-nope-1' "$BASE/nope"
  "${c[@]}" -H 'x-request-id: req-login-1' -H 'content-type: application/json' \
    -H 'traceparent: 00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01' \
    -d '{"user":"ada","password":"hunter2"}' "$BASE/login"
  "${c[@]}" -H 'x-request-id: req-seq-1' "$BASE/seq/7/1"
  "${c[@]}" -X DELETE -H 'x-request-id: req-del-1' "$BASE/ok"
  "${c[@]}" -H 'x-request-id: req-esc-1' -A 'agent "quoted" \ back' "$BASE/ok"
}

for mode in ${MODES:-file pipe}; do
  echo "[$mode]"
  log="$WORK/$mode.log"
  start $mode "$log"
  sleep 0.3
  fixed_requests
  # Moderate load: no line may be lost.
  if command -v h2load >/dev/null; then
    timeout 30 h2load --h1 -n 20000 -c 8 -t 2 "$BASE/ok" >/dev/null 2>&1
    timeout 30 h2load -n 20000 -c 8 -m 4 -t 2 "$BASE/ok" >/dev/null 2>&1
  else
    pids=(); for i in $(seq 8); do curl -s -o /dev/null "$BASE/ok?[1-1000]" & pids+=($!); done; wait "${pids[@]}"
  fi
  # Order within a connection: 8 connections (11-18; 7 is a fixed request), 300 numbered requests each.
  pids=(); for c in $(seq 11 18); do curl -s -o /dev/null "$BASE/seq/$c/[1-300]" & pids+=($!); done; wait "${pids[@]}"
  metrics=$(curl -s "$BASE/metrics")
  stop
  dropped=$(echo "$metrics" | awk '/^crocket_log_lines_dropped_total/{print $2}')
  served=$(echo "$metrics" | awk '/^crocket_http_requests_total/{s+=$2} END{print s}')
  expect "no lines dropped" "0" "$dropped"
  ok_lines=$(grep -c '"msg":"request"' "$log")
  # Every request the metrics counted has its line; the /metrics request itself is counted after.
  expect "every request logged" "$served" "$((ok_lines - 1))"
  unset CROCKET_LOG_BUFFER
  ! grep -q "hunter2" "$log" && pass "password redacted" || fail "password reached the log"
  grep -q '"password":"\[redacted\]"' "$log" && pass "redacted field present" || fail "no redacted field"
  grep -q '"msg":"issued \[redacted\] for ada"' "$log" && pass "token redacted in the message" || fail "token message"
  bad=0
  for c in $(seq 11 18); do
    got=$(grep -o "\"msg\":\"seq $c [0-9]*\"" "$log" | awk '{print $3+0}' | tr -d '"' | tr '\n' ' ')
    want=$(seq 1 300 | tr '\n' ' ')
    [[ "$got" == "$want" ]] || bad=$((bad + 1))
  done
  expect "order within each connection" 0 $bad
  # Format: the fixed requests' lines, normalized.
  grep -E '"request_id":"req-' "$log" | normalize > "$WORK/$mode.fmt"
  if [[ "${3:-}" == "--write-golden" && $mode == file ]]; then cp "$WORK/$mode.fmt" "$GOLDEN"; echo "  wrote $GOLDEN"; fi
  if diff -u "$GOLDEN" "$WORK/$mode.fmt" > "$WORK/diff"; then pass "format matches golden"; else fail "format differs:"; cat "$WORK/diff"; fi
done

echo "[CROCKET_LOG_SAMPLE]"
log="$WORK/sample.log"
CROCKET_LOG_SAMPLE=0 CROCKET_PORT=$PORT "$SERVER" >/dev/null 2>"$log" &
SRV_PID=$!
for _ in $(seq 1 100); do curl -s -o /dev/null "$BASE/ok" && break; sleep 0.1; done
curl -s -o /dev/null -H 'x-request-id: s-ok' "$BASE/ok"
curl -s -o /dev/null -H 'x-request-id: s-fail' "$BASE/fail"
stop
expect "sample 0 skips successes" 0 "$(grep -c '"request_id":"s-ok"' "$log")"
expect "sample 0 keeps errors" 1 "$(grep -c '"request_id":"s-fail"' "$log")"
( CROCKET_LOG_SAMPLE=2 CROCKET_PORT=$PORT "$SERVER" >/dev/null 2>"$WORK/bad.log" ) 2>/dev/null
expect "a bad sample stops the server" 1 "$(grep -c CROCKET_LOG_SAMPLE "$WORK/bad.log")"

echo "$FAILS failures"
exit $((FAILS > 0))
