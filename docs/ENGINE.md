# The wire engine

crocket's framework layer (routing, extractors, responders, fairings) knows nothing
about sockets. `Crocket::handle(Request) -> Response` is the whole contract, and the
in-process `LocalClient` drives exactly that function. The engine in
`src/engine_h2o.cpp` adapts [h2o](https://github.com/h2o/h2o) to it. It is the only
translation unit that includes `<h2o.h>`, and h2o is linked `PRIVATE`, so application
code never sees h2o types.

h2o is built from its own CMake as the static library `libh2o-evloop` (h2o's own event
loop, no libuv). `cmake/h2o.cmake` pins it and switches off everything crocket does not
use: mruby, brotli, zstd, io_uring, dtrace and the shared library.

## Updating h2o

h2o has had no release tag since 2.2.6 (2019). Fastly runs its master branch in
production, so crocket pins a master commit, plus the SHA-256 of that commit's GitHub
tarball:

```cmake
set(CROCKET_H2O_COMMIT cac7e6568ad98a848f099ecd0a18b881f632479a)  # 2026-09-10
set(CROCKET_H2O_SHA256 be4a7792211ab0d70513e5a2eea4e360b223dd1f8e43af4eacac214d1fba011b)
```

To move to the newest h2o:

```bash
./dev h2o-update            # latest master
./dev h2o-update <ref>      # a branch, tag, or short or full commit
```

The command resolves the ref through the GitHub API, downloads and hashes the tarball,
rewrites the two lines, prints a compare link for the upstream changes, then runs
`./dev check` (release build and the full test suite). If the check fails, the new pin
stays in place so you can investigate; `git checkout cmake/h2o.cmake` reverts it.

What to look at in the upstream compare, because the engine relies on it:

- `h2o_req_t` fields: `entity`, `proceed_req`, `write_req`, `content_length`,
  `path_normalized`, `res.trailers`, `http1_is_persistent`.
- The streaming request-body contract in `include/h2o.h`
  (`h2o_handler_t::supports_request_streaming`).
- `h2o_httpclient_error_is_eos`, which the engine passes to `proceed_req` to end a
  request body early.

To build offline, set `FETCHCONTENT_SOURCE_DIR_H2O` to an h2o checkout.

## Threading model

Network I/O runs on `LaunchOptions::event_loops` event-loop threads. The default, 0,
means one per physical core this process may use: distinct (package, core) pairs from
`/sys/devices/system/cpu/cpuN/topology` over the CPUs in its affinity mask, or every
logical CPU when the topology can't be read (`src/cpus.cpp`). Hyperthread siblings would
compete for one core, and a loop that waits for a CPU stalls all of its connections. On a
16-core, 32-thread machine, 16 loops served about 1.6 times the requests of 32. Each has its own `h2o_context_t`, `h2o_evloop_t` and listening
socket. The sockets share the port through `SO_REUSEPORT`, so the kernel spreads new
connections across the loops, and a connection stays on one loop for its whole life. A
loop owns every h2o object of its connections. Only the `h2o_globalconf_t` (read-only
once serving) and the TLS context are shared.

Handlers run on one worker pool of `LaunchOptions::workers` threads (0 means
`max(4, hardware_concurrency)`), shared by every loop. Handlers may block, for example on
`Pool::checkout`, without stalling I/O for other connections. Loops and workers
communicate through two queues:

- **Jobs:** a loop pushes a closure owning the `Request`. A worker runs
  `Crocket::handle_async`, which includes fairings, routing, extractors and the handler.
- **Completions:** whichever worker finishes the request posts `{txn, Response}` to the
  loop that owns it, with `h2o_multithread_send_message`. That writes to an eventfd the
  loop watches, so the loop wakes and responds.

Workers never touch an `h2o_req_t`. Each transaction gets an id that increases per loop,
and a completion whose id is no longer live is dropped. That happens when the client
disconnected or the deadline already produced a 504.

`launch()`'s thread starts the loops and workers, then waits for SIGINT or SIGTERM, which
only it takes. Shutdown is in three phases:

1. Every loop stops accepting, sends GOAWAY, and drains its requests until
   `drain_timeout`. Then it tells what is left to stop.
2. `launch()`'s thread waits for the workers and for suspended handlers, then stops the
   workers. Meanwhile the loops keep delivering completions.
3. Each loop closes its connections and tears itself down.

A request whose every candidate route runs on the loop (`Crocket::loop_route`) is handled
on its loop. That is an async handler (`RouteDef::async`), or a plain function that
adaptive placement has promoted. A request no route matches is handled there too: its
404 or 405 is crocket's own, with nothing that could block. `run_routes` times each plain function: on a worker it
counts consecutive runs under 100 µs and promotes the route after 1,000. On a loop, one
run over 50 ms demotes it, and so do 8 runs over 1 ms within a second. A single slow run
may only mean the thread was descheduled. The back-off grows with each demotion (`detail::Placement`, one per route,
built at ignite). Routes that take an extractor with `may_block` (a `Pool`) are never
promoted. Such a request runs on its loop: fairings, routing, extractors and the handler, with no handoff. The loop is the
exchange's executor, so the task resumes there, through a second `h2o_multithread`
receiver for jobs, and a response produced on the loop is written immediately. If routing
on the loop reaches a handler that may block (an `on_request` fairing rewrote the
request), the exchange is routed again on a worker. A loop held for more than 10 ms is
logged once per handler (`detail::LoopHold`). Other requests go to the workers as below.

Each request lives in a heap-allocated `detail::Exchange` from the start of the pipeline,
so the `Request` and `Response` stay at one address while a handler refers to them. A
`Task<T>` handler runs on the worker until it first suspends. The worker then returns to
the pool, and whatever the task awaits resumes it through the engine (the `Executor`), as
a job on a second queue that workers take before new requests. When the task finishes,
that worker writes the response, runs `on_response` fairings and posts the completion. A
task that finishes without suspending takes the ordinary path.

`Task<T>` wraps every `co_await` on a foreign awaitable: if the awaitable resumes the
task on another thread, the task hops back to a worker before its code continues, with
the request's log context and exchange installed. `sleep_for` (one timer thread for the
process, woken early by the request's stop token) and `callback<T>()` resume on a worker
themselves. `max_in_flight` counts suspended requests, and the drain at shutdown waits
for them as well as for busy workers. `LocalClient` resumes tasks on a small pool of its
own.

## Per-transaction flow

h2o gives every HTTP/1.1 request and every HTTP/2 stream its own `h2o_req_t`. The engine
registers one handler for every path and keeps a `Session` per request. The session is
tied to the request's memory pool (`h2o_mem_alloc_shared`), so h2o's release of the
request, after the response is sent or when the client goes away, is the one place the
session is freed.

| Event | Engine action |
|---|---|
| `on_req` | Build `Request` from h2o's parsed request and call `Crocket::prepare` to fix the request id and deadline. Arm the deadline timer. Reject early (see limits), otherwise take the body or dispatch. |
| `on_body` (`write_req.cb`) | Append the next piece of the body and enforce `max_body_bytes`. Dispatch at the end of the body, otherwise ask h2o for more. |
| completion message | Respond: status, headers and body in one `h2o_send`, and trailers over h2. |
| deadline timer | Request stop on the token and answer 504 `deadline.exceeded`. |
| request released | Request stop on the token and forget the transaction. |

Request construction:

- **Path and query:** h2o has removed dot segments and percent-decoded the path
  (`path_normalized`). The query string is split and decoded by `detail::parse_query`.
  A path that decodes to a NUL byte is 400 `http.invalid`.
- **Headers:** every request header reaches the handler over both protocols, custom ones
  included. Over h2 the `host` header is filled from `:authority`.
- **`protocol`:** `"h2"` for HTTP/2 requests, otherwise `"http/1.1"`.
- **`tls`, `peer_addr`:** whether the listener has TLS, and the peer's numeric address.
  `Crocket::prepare` derives `remote_addr` and `scheme` from them, through
  `Config::trusted_proxies` when the peer is one.

### Request bodies

The handler declares `supports_request_streaming`, so h2o calls `on_req` as soon as the
headers are in, instead of buffering the whole body itself. A body that arrived with the
headers is complete at once (`proceed_req` is null). Otherwise the body arrives in pieces:
h2o hands one over in `req->entity`, and the engine releases it with
`proceed_req(req, nullptr)`, which asks for the next. h2o allows exactly one such call per
piece, so the session tracks whether it holds one. The call runs from a zero-delay timer,
never inside one of h2o's own callbacks.

This is how crocket keeps enforcing `max_body_bytes` itself: the 413 carries the usual
JSON error body, log line and metrics, and is `RESOURCE_EXHAUSTED` for gRPC. h2o's own
limit is off.

When the engine answers before the body is in (413, or a 504 during a slow upload):

- **h2:** it ends the request body with `h2o_httpclient_error_is_eos`, so h2o resets the
  stream with `NO_ERROR` once the response is out (RFC 9113, section 8.1).
- **h1:** it keeps reading and dropping the body so the connection can take the next
  request. Past 8 MiB it stops, and the connection is closed after the response.

HTTP/2 bodies are read until END_STREAM, so gRPC needs no framing of its own. Chunked
HTTP/1.1 uploads are decoded by h2o and accepted.

### Responses

- Header names are written as given (lowercase); h2o writes its own `Content-Length` and
  `Connection` headers with HTTP/1.1 capitalisation.
- `content-length` is set from the body, except for gRPC and for responses with trailers:
  without a length the stream stays open for the trailers. `connection`,
  `transfer-encoding` and `keep-alive` from handlers are dropped.
- h2o handles flow control: the body goes out in one `h2o_send` and h2o writes it as the
  peer's window allows.
- **Trailers** (`Response::trailers`) go out over h2 as a final HEADERS frame with
  END_STREAM. They are dropped over HTTP/1.1.
- **gRPC** (`content-type: application/grpc`) is always sent with `:status 200`; the
  outcome is in `grpc-status`. `Response::status` keeps the HTTP equivalent for logs and
  metrics. Errors are trailers-only: one HEADERS frame carrying `grpc-status`.
- 1xx, 204 and 304 responses carry neither a body nor a length.
- HEAD sends the GET response's headers, including its `content-length`, and no body.
- No `Server` header is sent.

## Limits

| Condition | Response |
|---|---|
| Request headers larger than `max_header_bytes` (names plus values, default 8 KiB) | 431 `headers.too_large` |
| More than `max_header_count` request headers (default 100) | 431 `headers.too_large` |
| `Content-Length` over `max_body_bytes`, or a body that grows past it | 413 `body.too_large` |
| `max_in_flight` requests already dispatched | 503 `server.busy` |
| Deadline (`request_timeout`) reached | 504 `deadline.exceeded` |

All of these go through `Crocket::reject`, so they get a request id, a JSON error body, a
completion log line and metrics like any other response. The handler never runs.

Requests h2o cannot parse never reach the engine. h2o answers them itself with a plain
400 (h1: a malformed request line or `Content-Length`, or more than 100 headers or
about 400 KiB of them) or an h2 protocol error. They get no crocket log line.

h2o's own timeouts still apply to connections: 10 s to receive an HTTP/1.1 request
(`http1.req_timeout`) and 10 s of idleness on an HTTP/2 connection. Neither runs while a
request is with a handler: h2o counts an h2 stream as "blocked by the server" from the
moment it starts, and an h1 request has no read timeout while it is processed.

## Deadlines and cancellation

Every request carries a `Deadline` with an absolute time and a `std::stop_token`. The
token is triggered in two cases:

- **Deadline timer fires.** The client gets 504 immediately, whatever the handler is
  doing.
- **Client disconnects**, including an h2 `RST_STREAM`: h2o releases the request.

`Pool::checkout` waits on the token, so a request blocked on a busy pool is released at
once. Handlers doing their own long work should poll `Deadline::expired()`
(see `api::slow` in `examples/serve.cpp`).

A handler that ignores the token keeps running after its 504. Its late response is
discarded, but it still passes through `on_response` fairings. The request id then
appears in two log lines: the 504 and the late outcome, which is not sent.

gRPC's `grpc-timeout` header can shorten the deadline, over both h2 with prior knowledge
and TLS.

## TLS, ALPN and HTTP/2

TLS is on when both `tls_cert` and `tls_key` are set. `Crocket::launch` checks both files
are readable before starting and fails ignite if not. The engine loads them into an
OpenSSL context with TLS 1.2 as the minimum.

- With `http2 = true` (the default), ALPN offers `h2` and `http/1.1`.
- With `http2 = false`, ALPN offers only `http/1.1`.
- A plain-text listener serves HTTP/1.1. With `h2_prior_knowledge = true` it also serves
  HTTP/2 with prior knowledge, which is how gRPC clients connect without TLS, and
  `Upgrade: h2c`. HTTP/1.1 clients can still use the same port.

`tests/tls_h2.sh` verifies ALPN selection with curl, runs the handler suite over both
protocols, including custom headers, `X-Request-Id`, CORS preflight and the 431, and
checks 20 concurrent h2 streams on one connection.

## Graceful shutdown

On SIGINT or SIGTERM:

1. Mark the app draining. `/readyz` returns 503, and h1 responses carry
   `connection: close`.
2. Close the listen socket. New connections are refused; existing ones continue. Ask h2o
   to shut down its connections (`h2o_context_request_shutdown`): h2 connections get
   GOAWAY and idle h1 connections close.
3. Run the loop until no transaction is open or `drain_timeout` passes.
4. Request stop on anything still open, discard queued jobs that never started, and give
   running handlers 1 s to finish. If a handler is still running, log it and
   `std::quick_exit(1)`. Destroying managed state under a running handler would be
   undefined behaviour.
5. Give h2o up to 1 s to finish writing and close the remaining connections, then
   destroy the h2o context, and call `detail::shut_down`. That runs `on_shutdown`
   fairings in reverse attach order, then destroys managed state in reverse
   `manage` order, which drops pools. If a connection is still open at that point the
   h2o context is left for process exit rather than destroyed under it.

The exit code is 0 if everything drained, 2 if the drain timed out, and 1 if handlers had
to be abandoned.

The signal handler only sets a flag and writes to an eventfd the loop watches. Worker
threads block SIGINT and SIGTERM. SIGPIPE is ignored while the engine runs, because a
peer closing during a write is an ordinary error.

## Binary size

h2o's core references parts crocket never calls, such as HTTP/3 and proxying. h2o is
compiled with `-ffunction-sections -fdata-sections`, so an application that links with
`-Wl,--gc-sections` drops them. The stripped `crocket_serve` example is about 1.4 MB
without it and 1.1 MB with it.
