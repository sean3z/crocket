#!/usr/bin/env bash
# Acceptance 7 over real sockets: HTTPS via TLS, h2 when ALPN offers it,
# http/1.1 otherwise; plus request ids, limits and graceful drain.
#   tls_h2.sh <crocket_serve> <crocket_hello>
set -u
SERVE=$1
HELLO=$2
PORT=${CROCKET_TEST_PORT:-18443}
BASE="https://127.0.0.1:$PORT"
WORK=$(mktemp -d)
FAILS=0
trap 'kill $SRV_PID $HELLO_PID 2>/dev/null; rm -rf "$WORK"' EXIT

pass() { echo "  ok   $1"; }
fail() { echo "  FAIL $1"; FAILS=$((FAILS + 1)); }
expect() { # name, expected, actual
  if [[ "$3" == "$2" ]]; then pass "$1"; else fail "$1 (expected '$2', got '$3')"; fi
}
expect_match() { # name, regex, actual
  if [[ "$3" =~ $2 ]]; then pass "$1"; else fail "$1 (no match for /$2/ in '$3')"; fi
}
wait_up() { # url
  for _ in $(seq 1 100); do curl -sk -o /dev/null "$1" && return 0; sleep 0.1; done
  return 1
}

openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj "/CN=localhost" \
  -keyout "$WORK/key.pem" -out "$WORK/cert.pem" >/dev/null 2>&1 || { echo "openssl failed"; exit 1; }

CROCKET_PORT=$PORT CROCKET_TLS_CERT="$WORK/cert.pem" CROCKET_TLS_KEY="$WORK/key.pem" \
  "$SERVE" >"$WORK/serve.log" 2>&1 &
SRV_PID=$!
wait_up "$BASE/healthz" || { echo "server did not start"; cat "$WORK/serve.log"; exit 1; }

echo "[TLS + ALPN]"
expect "h2 negotiated when offered" "2" "$(curl -sk --http2 -o /dev/null -w '%{http_version}' "$BASE/healthz")"
expect "http/1.1 when h2 not offered" "1.1" "$(curl -sk --http1.1 -o /dev/null -w '%{http_version}' "$BASE/healthz")"
expect "plain http is not served on the TLS port" "000" \
  "$(curl -s -o /dev/null -w '%{http_code}' --max-time 3 "http://127.0.0.1:$PORT/healthz")"

for proto in --http2 --http1.1; do
  echo "[handlers over $proto]"
  out=$(curl -sk $proto -D "$WORK/h" -X POST "$BASE/api/users" -H 'authorization: Bearer alice' \
        -H 'content-type: application/json' -d '{"email":"a@example.com","name":"Ada"}' -w '\n%{http_code}')
  expect "POST json -> 201" "201" "$(tail -n1 <<<"$out")"
  expect_match "location header" "location: /api/users/[0-9]+" "$(tr -d '\r' <"$WORK/h")"
  expect_match "created body" '"email":"a@example.com"' "$out"

  expect "missing auth -> 401" "401" "$(curl -sk $proto -o /dev/null -w '%{http_code}' -X POST "$BASE/api/users" \
        -H 'content-type: application/json' -d '{"email":"a@b"}')"
  body=$(curl -sk $proto -X POST "$BASE/api/users" -H 'authorization: Bearer alice' \
        -H 'content-type: application/json' -d '{"email":')
  expect_match "invalid json -> json.invalid" '"code":"json.invalid"' "$body"
  body=$(curl -sk $proto "$BASE/api/users/not-a-number")
  expect_match "unparsable id -> path.invalid" '"code":"path.invalid"' "$body"
  expect "GET user 1" "200" "$(curl -sk $proto -o /dev/null -w '%{http_code}' "$BASE/api/users/1")"
  expect "missing user -> 404" "404" "$(curl -sk $proto -o /dev/null -w '%{http_code}' "$BASE/api/users/999")"
  expect "async handler" "49" "$(curl -sk $proto "$BASE/api/square/7")"
  expect "controller" "crocket-serve 0.1.0" "$(curl -sk $proto "$BASE/version")"
  h=$(curl -sk $proto -I "$BASE/version" | tr -d '\r')
  expect_match "HEAD -> 200 with content-length" "content-length: 19" "$h"
  expect "405 for wrong verb" "405" "$(curl -sk $proto -o /dev/null -w '%{http_code}' -X DELETE "$BASE/version")"
  gen=$(curl -sk $proto -D - -o /dev/null "$BASE/healthz" | tr -d '\r' | sed -n 's/^x-request-id: //p')
  expect "generated request id is 32 hex" "32" "${#gen}"
  big=$(head -c 1100000 /dev/zero | tr '\0' 'a')
  expect "body over limit -> 413" "413" "$(printf '%s' "$big" | curl -sk $proto -o /dev/null -w '%{http_code}' \
        -X POST "$BASE/api/users" -H 'content-type: application/json' --data-binary @-)"
  expect "query + metrics" "200" "$(curl -sk $proto -o /dev/null -w '%{http_code}' "$BASE/metrics?x=1")"
done

echo "[h2 connection lifetime and concurrency]"
# lws closes an h2 connection after keepalive_timeout (5 s) unless active
# streams are marked immortal; a 6 s handler catches a regression.
expect "h2 handler longer than lws keepalive" "200" \
  "$(curl -sk --http2 -o /dev/null -w '%{http_code}' "$BASE/api/slow/6000")"
codes=$(for i in $(seq 1 20); do printf -- "-o /dev/null $BASE/api/slow/200 "; done |
        xargs curl -sk --http2 --parallel --parallel-max 20 -w '%{http_code}\n' | sort | uniq -c | xargs)
expect "20 concurrent h2 streams" "20 200" "$codes"
expect "http/1.1 keep-alive reuses the connection" "1 0" \
  "$(curl -sk --http1.1 -o /dev/null -o /dev/null -w '%{num_connects} ' "$BASE/healthz" "$BASE/version" | xargs)"

echo "[request id propagation]"
rid=$(curl -sk --http1.1 -D - -o /dev/null -H 'x-request-id: trace-123' "$BASE/healthz" | tr -d '\r' |
      sed -n 's/^x-request-id: //p')
expect "X-Request-Id honoured over http/1.1" "trace-123" "$rid"
grep -q '"id":"trace-123"' "$WORK/serve.log" && pass "request id in log line" || fail "request id in log line"
grep -q '"route":"/api/users/{id}"' "$WORK/serve.log" && pass "log uses route template" || fail "log uses route template"
curl -sk "$BASE/metrics" | grep -q 'route="/api/users/{id}"' && pass "metrics by route template" ||
  fail "metrics by route template"

echo "[graceful shutdown]"
curl -sk --http2 -o "$WORK/slow.out" -w '%{http_code}' "$BASE/api/slow/1500" >"$WORK/slow.code" &
CURL_PID=$!
sleep 0.4
kill -TERM $SRV_PID
sleep 0.3
expect "new connections refused while draining" "000" \
  "$(curl -sk -o /dev/null -w '%{http_code}' --max-time 2 "$BASE/healthz")"
wait $CURL_PID
expect "in-flight request completes" "200" "$(cat "$WORK/slow.code")"
wait $SRV_PID
expect "server exits 0 after drain" "0" "$?"
grep -q '"msg":"shutdown"' "$WORK/serve.log" && pass "on_shutdown ran" || fail "on_shutdown ran"

echo "[crocket_hello on :8000]"
"$HELLO" >"$WORK/hello.log" 2>&1 &
HELLO_PID=$!
wait_up "http://127.0.0.1:8000/" || fail "hello did not start"
expect "hello" "Hello, 42 year old named Rocketeer!" "$(curl -s http://127.0.0.1:8000/hello/Rocketeer/42)"
expect "age 400 -> 404" "404" "$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:8000/hello/Rocketeer/400)"
expect "percent-decoded capture" "Hello, 36 year old named Ada L!" "$(curl -s 'http://127.0.0.1:8000/hello/Ada%20L/36')"
kill -INT $HELLO_PID; wait $HELLO_PID
expect "hello exits 0 on SIGINT" "0" "$?"

echo
if ((FAILS)); then echo "$FAILS failure(s)"; echo "--- serve.log (tail)"; tail -20 "$WORK/serve.log"; exit 1; fi
echo "all TLS/h2 checks passed"
