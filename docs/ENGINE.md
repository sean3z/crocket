# The wire engine

crocket's framework layer (routing, extractors, responders, fairings) knows nothing
about sockets. `Crocket::handle(Request) -> Response` is the whole contract, and the
in-process `LocalClient` drives exactly that function. The engine in
`src/engine_lws.cpp` adapts libwebsockets v4.3.5 to it. It is the only translation
unit that includes `<libwebsockets.h>`, and lws is linked `PRIVATE`, so application
code never sees lws types.

## Threading model

One event-loop thread runs `lws_service` and owns every lws object. Handlers run on a
worker pool of `LaunchOptions::workers` threads (0 means `max(4, hardware_concurrency)`).
Handlers may block, for example on `Pool::checkout`, without stalling I/O for other
connections. The two sides communicate through two queues:

- **Jobs:** the event loop pushes a closure owning the `Request`. A worker runs
  `Crocket::handle`, which includes fairings, routing, extractors and the handler.
- **Completions:** the worker pushes `{txn, Response}` and calls `lws_cancel_service`.
  That call is the one lws function documented as safe from other threads. It wakes the
  loop with `LWS_CALLBACK_EVENT_WAIT_CANCELLED`, which drains the queue.

Workers never touch a `wsi`. Each transaction gets a monotonically increasing id, and a
completion whose id is no longer live is dropped. That happens when the client
disconnected or the deadline already produced a 504.

`Task<T>` handlers are awaited with `sync_wait` on the worker. Coroutines that resume on
other threads work, but a waiting worker is occupied until the task finishes.

## Per-transaction flow

An h1 request and an h2 stream each have their own `wsi` and per-session slot. On h1
keep-alive the slot is reused for the next request after `lws_http_transaction_completed`.

| lws callback | engine action |
|---|---|
| `HTTP` | Build `Request` from lws tokens and call `Crocket::prepare` to fix the request id and deadline. Arm a timer for the deadline. Reject early (see limits), otherwise wait for the body or dispatch. |
| `HTTP_BODY` | Append to the body and enforce `max_body_bytes`. Dispatch to a worker once the body is complete (see below). |
| `HTTP_BODY_COMPLETION` | Dispatch an h2 body that has neither a `Content-Length` nor gRPC framing. Otherwise the engine has already counted the body complete, or is still waiting for the rest. |
| `EVENT_WAIT_CANCELLED` | Pull completions and request writeable. |
| `HTTP_WRITEABLE` | Write status and headers, then the body in chunks of up to 16 KiB (`LWS_WRITE_HTTP_FINAL` on the last), then trailers when there are any, then complete the transaction. |
| `TIMER` | The deadline passed: request stop on the token and answer 504 `deadline.exceeded`. |
| `CLOSED_HTTP` / `HTTP_DROP_PROTOCOL` | Request stop on the token and forget the transaction. |

Request construction:

- **Path and query:** lws has already percent-decoded and normalised the path, and has
  split and decoded the query string into `URI_ARGS` fragments. The engine does not
  decode again.
- **Method:** over h1 it comes from the per-method URI token; over h2 from `:method`.
  Over h2 the `host` header is filled from `:authority`.
- **`protocol`:** `"h2"` when the stream's network wsi differs from the stream wsi,
  otherwise `"http/1.1"`.
- **`tls`, `remote_addr`:** taken from the network connection.
- **Body length:** the engine counts the body itself rather than trust lws 4.3.5's
  `HTTP_BODY_COMPLETION` on h2. That callback never comes when DATA frames arrive with
  the HEADERS and lws replays them from a buffer, because the replay decrements a
  different counter from the one lws checks. Without a `Content-Length`, it comes too
  early: on the first piece of the END_STREAM frame. So a body with a `Content-Length` is
  complete at that many bytes, and a gRPC body at the length its frame announces. Only
  an h2 POST with neither relies on lws. `tls_h2.sh` and `grpc_h2.sh` cover all three.

Response writing:

- Header names are already lowercase, which h2 requires.
- `content-length` is computed by the engine, except for gRPC and for responses with
  trailers: a length would make lws end the stream with the body, before the trailers.
  `connection`, `transfer-encoding` and `keep-alive` from handlers are dropped.
- **h2 flow control:** each DATA write is capped at the peer's send window
  (`lws_get_peer_write_allowance`). At zero the engine asks for WRITEABLE again, and lws
  calls back when the peer's WINDOW_UPDATE arrives. Writing past the window is a
  FLOW_CONTROL_ERROR that kills the whole connection, which used to happen to any h2
  response over 64 KiB.
- **Trailers** (`Response::trailers`) go out over h2 as a final HEADERS frame with
  END_STREAM. They are dropped over HTTP/1.1.
- **gRPC** (`content-type: application/grpc`) is always sent with `:status 200`; the
  outcome is in `grpc-status`. `Response::status` keeps the HTTP equivalent for logs and
  metrics. Errors are trailers-only: one HEADERS frame carrying `grpc-status`.
- 1xx, 204 and 304 responses carry neither a body nor a length.
- HEAD sends the GET response's headers, including its `content-length`, and no body.

## Limits

| Condition | Response |
|---|---|
| Request headers larger than `max_header_bytes` (the lws header buffer, clamped to 1024..65535) | Connection closed by lws |
| `Content-Length` over `max_body_bytes`, or more body than declared | 413 `body.too_large` |
| Malformed `Content-Length` | 400 `http.invalid` |
| `Transfer-Encoding` without `Content-Length` (chunked upload) | 411 `body.length_required` |
| An h2 POST without `Content-Length` (how gRPC sends) | Read until END_STREAM, or until the gRPC message is complete; 413 past `max_body_bytes` |
| `max_in_flight` requests already dispatched | 503 `server.busy` |
| Deadline (`request_timeout`) reached | 504 `deadline.exceeded` |

All of these go through `Crocket::reject`, so they get a request id, a JSON error body, a
completion log line and metrics like any other response. The handler never runs.

When the 413 is sent before the body arrives, lws discards the rest of the body
(`LRS_DISCARD_BODY`) so an h1 connection stays usable.

## Deadlines and cancellation

Every request carries a `Deadline` with an absolute time and a `std::stop_token`. The
token is triggered in two cases:

- **Deadline timer fires.** The client gets 504 immediately, whatever the handler is
  doing.
- **Client disconnects**, including an h2 `RST_STREAM`.

`Pool::checkout` waits on the token, so a request blocked on a busy pool is released at
once. Handlers doing their own long work should poll `Deadline::expired()`
(see `api::slow` in `examples/serve.cpp`).

A handler that ignores the token keeps running after its 504. Its late response is
discarded, but it still passes through `on_response` fairings. The request id then
appears in two log lines: the 504 and the late outcome, which is not sent.

### h2 connection keepalive

lws closes an idle h2 network connection after the vhost `keepalive_timeout` (5 s). It
treats "no frames received" as idle even when a stream is waiting on a slow handler, and
would drop the whole connection under that handler.

To prevent this, the engine marks each h2 stream "immortal" with `lws_mux_mark_immortal`
when it dispatches the request. Not earlier: during an upload data is flowing, and lws
re-arms a body timeout for every DATA frame and logs an error each time on an immortal
stream. That suspends the connection's idle timer while the stream is
open, and lws restores it when the stream closes. The engine's own deadline timer still
bounds each request.

`lws_mux_mark_immortal` is internal to lws: it is exported from the static library but
not in the public headers. It is pinned with v4.3.5; revisit it when upgrading.
`tests/tls_h2.sh` covers it with a 6 s handler over h2.

## TLS, ALPN and HTTP/2

TLS is on when both `tls_cert` and `tls_key` are set. `Crocket::launch` checks both files
are readable before starting and fails ignite if not.

- With `http2 = true` (the default), ALPN offers `h2,http/1.1`.
- With `http2 = false`, ALPN offers only `http/1.1`.
- Plain-text listeners serve HTTP/1.1, or with `h2_prior_knowledge = true` only HTTP/2
  with prior knowledge (`LWS_SERVER_OPTION_H2_PRIOR_KNOWLEDGE`), which is how gRPC clients
  connect without TLS. There is no `Upgrade: h2c`.

lws logs two ordinary h2 events at error level, so the engine's log sink drops them:
`skint`, a stream waiting for the peer's WINDOW_UPDATE; and `lws_set_timeout: on immortal
stream`, lws re-arming a body timeout after the engine has already counted the whole
body in.

`tests/tls_h2.sh` verifies ALPN selection with curl, runs the handler suite over both
protocols and checks 20 concurrent h2 streams on one connection.

### Known limitation: custom request headers over h2

lws 4.3.5 stores headers it has no token for (custom headers) only for HTTP/1.1; the
h2 HPACK path drops them. The code is under `!wsi->mux_substream` in
`lib/roles/http/parsers.c`.

Standard headers, including `authorization`, `content-type`, `te` and `origin`, work over
both protocols. The visible consequences over h2:

- A client-supplied `X-Request-Id` is ignored, and the server generates one. It is still
  returned in the `x-request-id` response header and appears in logs and error bodies.
- `Header<"x-...">` extractors for custom headers see the header as missing.
- gRPC's `grpc-timeout` and custom metadata do not arrive, so the configured
  `request_timeout` applies to gRPC calls.

Over HTTP/1.1 all of these work.

## Graceful shutdown

On SIGINT or SIGTERM:

1. Mark the app draining. `/readyz` returns 503, and h1 responses carry
   `connection: close`.
2. Close the listen socket with `lws_context_deprecate`. New connections are refused;
   existing ones continue.
3. Service the loop until no transaction is open or `drain_timeout` passes. A helper
   thread wakes the loop every 100 ms to check the clock.
4. Request stop on anything still open, discard queued jobs that never started, and give
   running handlers 1 s to finish. If a handler is still running, log it and
   `std::quick_exit(1)`. Destroying managed state under a running handler would be
   undefined behaviour.
5. Destroy the lws context, then call `detail::shutdown_app`. That runs `on_shutdown`
   fairings in reverse attach order, then destroys managed state in reverse
   `manage` order, which drops pools.

The exit code is 0 if everything drained, 2 if the drain timed out, and 1 if handlers had
to be abandoned.

The signal handler only sets a flag and calls `lws_cancel_service`, which writes one byte
to the context's wakeup descriptor. Worker threads block SIGINT and SIGTERM.
