#!/usr/bin/env bash
# gRPC over real sockets, driven by curl: plaintext HTTP/2 with prior knowledge
# (gRPC's insecure channel) and TLS with ALPN. Covers trailers, trailers-only
# errors, request bodies spread over many DATA frames, and replies larger than
# the peer's 64 KiB flow-control window.
#   grpc_h2.sh <crocket_grpc>
set -u
GRPC=$1
PORT=${CROCKET_TEST_PORT:-18551}
TLS_PORT=$((PORT + 1))
WORK=$(mktemp -d)
FAILS=0
trap 'kill $H2C_PID $TLS_PID 2>/dev/null; rm -rf "$WORK"' EXIT

pass() { echo "  ok   $1"; }
fail() { echo "  FAIL $1"; FAILS=$((FAILS + 1)); }
expect() { # name, expected, actual
  if [[ "$3" == "$2" ]]; then pass "$1"; else fail "$1 (expected '$2', got '$3')"; fi
}
expect_match() { # name, regex, actual
  if [[ "$3" =~ $2 ]]; then pass "$1"; else fail "$1 (no match for /$2/ in '$3')"; fi
}

# call <base url> <method> <request file> [curl args...]: headers+trailers to $WORK/h, body to $WORK/b
call() {
  local base=$1 method=$2 req=$3
  shift 3
  curl -s "$@" -D "$WORK/h" -o "$WORK/b" -H 'content-type: application/grpc' -H 'te: trailers' \
    --data-binary @"$req" "$base/helloworld.Greeter/$method"
  tr -d '\r' <"$WORK/h" >"$WORK/h.txt"
}
header() { grep -i "^$1:" "$WORK/h.txt" | tail -n1 | cut -d' ' -f2-; }

# HelloRequest{name = "Ada"}, framed: flag 0, length 5, then field 1 (LEN 3) "Ada"
printf '\x00\x00\x00\x00\x05\x0a\x03Ada' >"$WORK/ada.bin"
# An empty message (google.protobuf.Empty)
printf '\x00\x00\x00\x00\x00' >"$WORK/empty.bin"
# name = 300000 x's: length 300004 = 0x000493e4, varint(300000) = e0 a7 12
{ printf '\x00\x00\x04\x93\xe4\x0a\xe0\xa7\x12'; head -c 300000 /dev/zero | tr '\0' x; } >"$WORK/big.bin"
# 2 MiB, over the default 1 MiB max_body_bytes (the frame header is all that matters)
{ printf '\x00\x00\x20\x00\x00'; head -c 2097152 /dev/zero; } >"$WORK/huge.bin"

CROCKET_PORT=$PORT "$GRPC" >"$WORK/h2c.log" 2>&1 &
H2C_PID=$!
openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj "/CN=localhost" \
  -keyout "$WORK/key.pem" -out "$WORK/cert.pem" >/dev/null 2>&1 || { echo "openssl failed"; exit 1; }
CROCKET_PORT=$TLS_PORT CROCKET_TLS_CERT="$WORK/cert.pem" CROCKET_TLS_KEY="$WORK/key.pem" \
  "$GRPC" >"$WORK/tls.log" 2>&1 &
TLS_PID=$!
H2C="http://127.0.0.1:$PORT"
TLS="https://127.0.0.1:$TLS_PORT"
for _ in $(seq 1 100); do
  curl -s --http2-prior-knowledge -o /dev/null "$H2C/healthz" && curl -sk -o /dev/null "$TLS/healthz" && break
  sleep 0.1
done

echo "[plaintext h2, prior knowledge]"
call "$H2C" SayHello "$WORK/ada.bin" --http2-prior-knowledge
expect "HTTP status is 200" "200" "$(head -n1 "$WORK/h.txt" | cut -d' ' -f2)"
expect "content-type" "application/grpc" "$(header content-type)"
expect "grpc-status 0 in the trailers" "0" "$(header grpc-status)"
expect_match "reply message" "Hello, Ada!" "$(tr -d '\0' <"$WORK/b")"
expect "no content-length" "" "$(header content-length)"

call "$H2C" Whoami "$WORK/empty.bin" --http2-prior-knowledge
expect "no token -> UNAUTHENTICATED (16)" "16" "$(header grpc-status)"
expect "grpc-message" "missing bearer token" "$(header grpc-message)"
expect "trailers-only: no body" "0" "$(wc -c <"$WORK/b" | tr -d ' ')"
call "$H2C" Whoami "$WORK/empty.bin" --http2-prior-knowledge -H 'authorization: Bearer letmein'
expect "token -> OK" "0" "$(header grpc-status)"

call "$H2C" SayHello "$WORK/empty.bin" --http2-prior-knowledge
expect "empty name -> INVALID_ARGUMENT (3)" "3" "$(header grpc-status)"
call "$H2C" Nope "$WORK/empty.bin" --http2-prior-knowledge
expect "unknown method -> UNIMPLEMENTED (12)" "12" "$(header grpc-status)"
call "$H2C" Tenant "$WORK/empty.bin" --http2-prior-knowledge -H 'x-tenant: acme'
expect_match "custom metadata reaches the handler" "acme" "$(tr -d '\0' <"$WORK/b")"
expect "the plaintext h2 port also serves HTTP/1.1" "200" \
  "$(curl -s --http1.1 -o /dev/null -w '%{http_code}' "$H2C/healthz")"

call "$H2C" SayHello "$WORK/big.bin" --http2-prior-knowledge
expect "300 KB request over many DATA frames, 300 KB reply past the flow-control window" "0" "$(header grpc-status)"
expect "whole reply received" "300019" "$(wc -c <"$WORK/b" | tr -d ' ')"
# curl sends a body read from stdin without a Content-Length, as gRPC clients do
curl -s --http2-prior-knowledge -D "$WORK/h" -o "$WORK/b" -H 'content-type: application/grpc' -H 'te: trailers' \
  --data-binary @- "$H2C/helloworld.Greeter/SayHello" <"$WORK/big.bin"
tr -d '\r' <"$WORK/h" >"$WORK/h.txt"
expect "300 KB request without Content-Length" "0" "$(header grpc-status)"
expect "whole reply received (no Content-Length)" "300019" "$(wc -c <"$WORK/b" | tr -d ' ')"
call "$H2C" SayHello "$WORK/huge.bin" --http2-prior-knowledge
expect "2 MB request -> RESOURCE_EXHAUSTED (8)" "8" "$(header grpc-status)"

echo "[TLS, h2 via ALPN]"
call "$TLS" SayHello "$WORK/ada.bin" -k --http2
expect "grpc-status 0" "0" "$(header grpc-status)"
expect_match "reply message" "Hello, Ada!" "$(tr -d '\0' <"$WORK/b")"
call "$TLS" SayHello "$WORK/big.bin" -k --http2
expect "large reply over TLS" "300019" "$(wc -c <"$WORK/b" | tr -d ' ')"
call "$TLS" Tenant "$WORK/empty.bin" -k --http2 -H 'x-tenant: acme'
expect_match "custom metadata over TLS" "acme" "$(tr -d '\0' <"$WORK/b")"

echo "[logs]"
# Every line is a JSON log line or the listening banner; anything else came from the wire library.
errors=$(grep -hv -e '^{' -e '^crocket: listening' "$WORK/h2c.log" "$WORK/tls.log")
expect "no engine errors" "" "$errors"

if ((FAILS)); then
  echo "--- h2c log"; cat "$WORK/h2c.log"
  echo "--- tls log"; cat "$WORK/tls.log"
  echo "$FAILS failure(s)"
  exit 1
fi
echo "all passed"
