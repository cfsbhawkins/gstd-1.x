# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Security
- **HTTP body limit enforced while the request is received**
  (`gstd_http.c`). The 8 MiB cap used to be a `Content-Length` check
  that ran after the fast paths, plus a length check after
  `soup_message_body_flatten()`. A chunked body was therefore buffered and
  flattened in full before it was checked, and fast-path or non-JSON
  requests were never checked. The server now hooks every request on
  `request-started`. A declared `Content-Length` over the cap is rejected
  from the headers, so an `Expect: 100-continue` client never sends the
  body. Chunked bodies are counted as they arrive and rejected once the
  total passes the cap. Either way the request gets `413` for every
  method, content type, and endpoint before any handler runs. Buffered
  bytes are released, the remainder is discarded without buffering, and
  the connection closes after the response. `parse_json_body` now checks
  the content type and size before it flattens anything.
- **`/health` auth exemption limited to read methods** (`gstd_http.c`).
  `/health` was dispatched by path before the method was read and before
  bearer auth, and the handler ignored the method, so any verb got an
  unauthenticated `200`. Only `GET` and `HEAD` are exempt now. Every
  other method gets `405` with `Allow: GET, HEAD`, whether or not it
  carries a token, and `OPTIONS` goes to the shared empty preflight.
- **An empty API token no longer disables authentication while
  reporting it enabled** (`gstd_http.c`). `NULL` and `""` both skipped
  auth, but startup logged it as enabled for any non-`NULL` value, so
  `GSTD_HTTP_API_TOKEN=""` or `--http-api-token=` failed open. `NULL` is
  now the only disabled state. An empty token from the environment or
  the command line makes the HTTP server refuse to start. The
  `api-token` property rejects `""` with a warning and keeps its
  current value, and the request check fails closed if an empty token
  ever reaches it.
- **Wildcard and malformed CORS origins rejected** (`gstd_http.c`). The
  configured origin was echoed verbatim, `*` was explicitly supported,
  and the command line, environment, and property accepted any string.
  The origin must now be exactly one serialized origin,
  `http(s)://host[:port]` (lowercase, no default port). `*`, `null`,
  paths, trailing slashes, queries, fragments, credentials, and lists
  make the HTTP server refuse to start. The `cors-origin` property
  rejects them with a warning and keeps its current value. `Vary: Origin`
  is now always sent with the origin.

### Fixed
- **CI breakage** across the workflow matrix:
  - `gstd_action.c` / `gstd_http.c` mixed declarations failed
    `meson --werror` (`-Wdeclaration-after-statement`); an unused variable
    in `test_gstd_refcount.c` would have been next. The meson jobs had
    been red since the code-audit commit.
  - `tests/gstd/Makefile.am` now links `$(GIO_LIBS)`: registering
    `test_gstd_http` exposed that the autotools test harness never linked
    GIO (`g_socket_client_new` undefined at link).
  - `test_gstd_http.c` includes `<stdio.h>` for `sscanf` (Ubuntu 20.04's
    glibc headers don't pull it in transitively).
- **HTTP shutdown race**: `gstd_http_stop` now calls
  `soup_server_disconnect()` before dropping the server reference, and the
  HTTP test teardown stops its main-loop thread before destroying the
  server. Destroying the soup server concurrently with source dispatch
  raced and produced GLib criticals, seen as flaky failures in the ASan
  workflow.

### Added
- **Opt-in HTTP API token authentication** (`gstd_http.c`)
  - `--http-api-token <token>` or `GSTD_HTTP_API_TOKEN` (preferred; command
    lines are visible to other local processes). When configured, every HTTP
    request except `GET /health` must carry `Authorization: Bearer <token>`;
    failures get `401` with `WWW-Authenticate: Bearer` (and CORS headers when
    an origin is configured, so browser pages see the 401 rather than an
    opaque network error). Token comparison hashes both sides and compares
    the digests without an early exit; oversized `Authorization` values are
    rejected before hashing. Default: disabled, matching upstream.
  - `OPTIONS` requests are answered centrally with an empty preflight
    response before any handler runs, so a preflight can neither bypass the
    token nor reach a state-disclosing endpoint such as `/pipelines/status`
    (which is now also `GET`-only). The token and CORS origin are swapped
    and read under a lock, so runtime rotation through the GObject
    properties is safe.
- **Pipeline count limit** (`gstd_list.c`, `libgstd.c`, `gstd.c`)
  - `--max-pipelines <count>` or `GSTD_MAX_PIPELINES` caps simultaneous
    pipelines as a resource-exhaustion guard for the unauthenticated API.
    Backed by a new `GstdList` `max-children` property (0 = unlimited,
    enforced under the list lock) and a new `GSTD_MAX_LIMIT_REACHED`
    return code, mapped to HTTP 429 — including for concurrent creates
    that lose the race on the locked append path. An explicit
    `--max-pipelines 0` clears a cap applied via the environment, and the
    env value must be entirely numeric.
  - New public API `gstd_set_max_pipelines()`.
- **GObject properties on `GstdHttp`** — `port`, `address`, `max-threads`,
  `api-token`, `cors-origin` — so embedders and tests can configure the
  server without the option group.
- Tests: `test_gstd_pipeline_limit.c` (cap enforce/release/disable) and
  new HTTP tests for token auth, CORS defaults, name and path validation,
  OPTIONS non-disclosure, HTTP 429 on the cap, and CORS on 401. Fixtures
  unset `GSTD_MAX_PIPELINES`, `GSTD_HTTP_API_TOKEN`, and
  `GSTD_HTTP_CORS_ORIGIN` so an exporting shell cannot skew results.
  `test_gstd_http.c` existed but was never registered with any build
  system and could not have passed (no main loop, wrong assertions,
  properties that did not exist); it is now fixed and wired into meson
  and autotools.

### Changed
- **CORS is now opt-in** (`gstd_http.c`) — the server no longer sends
  `Access-Control-Allow-Origin: *` on every response, which let any web
  page in a local browser read API responses cross-origin and quietly
  defeated the 127.0.0.1 binding. No CORS headers are emitted unless
  `--http-cors-origin <origin>` / `GSTD_HTTP_CORS_ORIGIN` is set; a
  non-wildcard origin also gets `Vary: Origin`, and
  `authorization` was added to `Access-Control-Allow-Headers`.
- **Runtime/log directories are created as 1777 instead of 777**
  (`gstd/meson.build`, `gstd/Makefile.am`, `init/gstd.in`) — the sticky
  bit (like `/tmp`) stops local users deleting or replacing each other's
  pid, socket, and log files while still letting any user run gstd. The
  sysv init script also re-applies the mode on every start so a
  pre-existing weaker directory gets corrected. Deployments with a
  dedicated gstd user should tighten to 0750.
- **HTTP `?name=` values and request paths are validated** (`gstd_http.c`)
  — names or percent-decoded paths with whitespace or control characters
  were re-tokenized by the internal space-separated command parser into
  extra arguments (e.g. `POST /pipelines%20other?name=safe`); both are now
  rejected with `400`. PUT's `name` is exempt since it carries property
  values, where spaces are legitimate.
- **`GSTD_BAD_VALUE` now maps to HTTP 400** instead of `204 No Content`,
  which mislabeled a client error as success.
- **TCP commands tolerate trailing CR/LF** (`gstd_socket.c`) so
  line-oriented clients (telnet, netcat) don't leak framing bytes into
  the last token. Only CR/LF is stripped — property values may
  legitimately end in spaces or tabs. Real message framing remains one
  command per `read()`: the client protocol sends no delimiter, so this
  cannot be fixed without a protocol change.

## [0.16.2] - 2026-05-21

### Fixed
- **GstChildProxy array-property writes used the prefixed name** (`gstd_property_array.c`)
  - `gstd_property_array_update()` looked up the pspec and called `g_object_set`
    with the prefixed GstdObject name (e.g. `sink_0::positions`) instead of the
    bare property name. For child-proxy children this made the lookup fail
    (returning `GSTD_MISSING_INITIALIZATION`) and the write silently miss.
  - Now prefers the pspec stored at construction and writes with `pspec->name`,
    matching the read path and the base-class update handler.
  - Non-array and non-child-proxy properties were already correct (handled by
    the base `GstdProperty` update path); they are covered by new tests to
    prevent regression.
- **Code-review fixes across the daemon** (mostly pre-existing defects)
  - `gstd_socket.c`: one-byte heap overflow on a maximum-size read; the socket
    service was never stored back into the object so it could not be stopped or
    released.
  - `gstd_event_handler.c`: stored a borrowed receiver reference but unref'd it
    on dispose (refcount underflow); now uses `g_value_dup_object`.
  - Memory leaks: `GArray`/token leaks in `gstd_property_array.c`; formatter in
    `gstd_action.c`; receiver in `gstd_event_creator.c` (added dispose); socket
    address/service in `gstd_unix.c` and `gstd_tcp.c`; split tokens on argument
    errors in `gstd_parser.c`; state object on the pipeline-delete error path.
  - Robustness: `gstd_bus_msg_notify.c` always returned an error code on
    success; NULL element-factory deref in `gstd_bus_msg_stream_status.c`;
    infinite loop on `GST_ITERATOR_ERROR` in `gstd_http.c`; NULL token passed to
    `printf` in event seek/flush_stop; missing parent dispose chain in
    `gstd_signal_reader.c`.
  - Cleanup: removed unreachable code in `gstd_element.c` property-type
    selection; aligned `gstd_property_int.c` with the pspec/bare-name pattern.
- **HTTP boundary hardening** (`gstd_http.c`)
  - JSON output injection: `/pipelines/status` emitted pipeline names without
    escaping, so a name containing `"` or control characters could inject into
    or invalidate the JSON response. Names are now escaped.
  - `json_escape_string` now escapes control characters (RFC 8259), not just
    `"` and `\`.
  - Added an 8 MiB request-body cap (returns 413) to bound resource use on
    hostile oversized requests; legitimate descriptions/values are unaffected.

### Added
- **Regression tests for GstChildProxy property access**
  (`tests/gstd/test_gstd_childproxy_property.c`)
  - Verifies `compositor` request-pad properties are enumerated under prefixed
    names and that GET/PUT round-trips work for `sink_0::alpha` (double),
    `sink_0::xpos` (int), plus a plain (non-child-proxy) property.

### Documentation
- **OpenAPI**: documented prefixed child-proxy property names
  (`sink_0::alpha`) on the `property_name` path parameter, and the `?name=`
  query-parameter alternative to the JSON body on `setProperty`.

## [0.16.1] - 2026-01-14

### Added
- Docker support for local test execution
  - Added `Dockerfile` with Ubuntu 22.04 and all build dependencies
  - Added `docker-test.sh` helper script for running test suite

- Valgrind memory leak testing support
  - Added `Dockerfile.valgrind` for running tests under valgrind
  - Added `docker-valgrind.sh` script for easy leak checking
  - Added `tests/gstd.supp` suppression file for GStreamer/GLib known allocations
  - Usage: `./docker-valgrind.sh` (all tests) or `./docker-valgrind.sh test_name`

### Fixed
- **Build error: Incomplete type access in HTTP handler** (`gstd_http.c`)
  - Added `gstd_pipeline_get_element()` accessor function to `gstd_pipeline.h`
  - Replaced direct struct field access with accessor for proper encapsulation
  - Fixes compilation with libsoup 3.0.x

- **Build warning: Incorrect libsoup version checks** (`gstd_http.c`)
  - Fixed version macro usage for `soup_server_message_set_status()` (needs 3.0.0)
  - Fixed version macro usage for `soup_server_message_unpause()` (needs 3.2.0)
  - Properly handles libsoup 3.0.x which has different API than 3.2.x

- **Thread safety: State refcount race condition** (`gstd_state.c`)
  - Added `GST_OBJECT_LOCK/UNLOCK` around refcount operations
  - Matches thread-safe pattern used in `gstd_pipeline.c`

- **Bug: CORS headers not set on HTTP responses** (`gstd_http.c`)
  - Fixed `soup_server_message_get_request_headers()` → `soup_server_message_get_response_headers()`
  - CORS headers were being appended to wrong header collection in libsoup 3.0+

- **Memory leak: Session property setter** (`gstd_session.c`)
  - Added `g_object_unref()` for previous value before setting new pipelines/debug objects
  - Prevents leak when properties are set multiple times

- **Memory leak: GValue not unset on error path** (`gstd_state.c`)
  - Added `g_value_unset()` before early return in `gstd_state_update()`
  - Prevents leak when state deserialization fails

- **Critical: Session singleton race condition** (`gstd_session.c`)
  - Replaced weak pointer with `GWeakRef` for thread-safe singleton pattern
  - Fixed race where `g_object_ref()` could be called on finalizing object
  - Moved singleton logic from GObject constructor to `gstd_session_new()`

- **Bug: No-arg actions broken in parser** (`gstd_parser.c`)
  - `action_emit` passed unvalidated NULL `tokens[3]` to URI builder
  - Actions without arguments now work correctly

- **Memory leak: soup_message_body_flatten buffer not freed** (`gstd_http.c`)
  - `parse_json_body()` leaked the SoupBuffer/GBytes returned by flatten
  - Now properly frees buffer on all exit paths for both libsoup 2.x and 3.x

- **Bug: GValue unset on uninitialized values** (`gstd_action.c`)
  - Cleanup loop could call `g_value_unset()` on uninitialized GValues
  - Now tracks actual initialized count to prevent GLib criticals

- **Bug: Extra action arguments silently merged** (`gstd_action.c`)
  - `g_strsplit(..., query.n_params)` hid extra arguments in last token
  - Now splits with no limit and properly validates argument count

- **Memory leak: g_value_unset not called** (`gstd_property_flags.c`)
  - `g_value_init()` was called but `g_value_unset()` was never called before return
  - Prevents memory leak on every flags property update in long-running daemons

- **Memory leak: g_inet_address_to_string not freed** (`gstd_socket.c`)
  - Address string leaked on each client connection
  - Now properly freed after use

### Improved
- **Logging for unhandled bus message types** (`gstd_bus_msg.c`)
  - Added GST_DEBUG logging for message types without specialized handlers
  - Helps diagnose missing message handling in production (enable with `GST_DEBUG=gstd*:5`)

### Tests
- Added `test_gstd_refcount.c` with new thread safety tests:
  - `test_concurrent_state_changes` - Tests state changes from multiple threads
  - `test_invalid_state_no_leak` - Tests GValue cleanup on invalid state
  - `test_pipeline_refcount_balance` - Tests play/stop refcount cycles
  - `test_session_singleton` - Tests singleton pattern behavior
  - `test_concurrent_session_access` - Tests concurrent session creation/destruction

- Added `test_gstd_parser.c` with command parser tests (17 tests):
  - Pipeline lifecycle: `pipeline_create`, `pipeline_delete`, `pipeline_play`, `pipeline_pause`, `pipeline_stop`
  - Query commands: `list_pipelines`, `read`, `list_elements`
  - Element property: `element_get`, `element_set`
  - Events: `event_eos`
  - Error handling: Invalid commands, NULL commands, invalid pipeline descriptions, missing arguments

## [0.16.0] - 2026-01-14

### Added
- New `/pipelines/status` fast-path endpoint for lightweight pipeline monitoring
  - Bypasses thread pool to avoid contention during frequent polling
  - Returns only pipeline names and states for minimal overhead
  - Documented in OpenAPI specification

### Fixed
- **Critical: Type confusion crash in pipeline cleanup** (`gstd_pipeline.c`)
  - Changed `g_object_unref()` to `g_free()` for `graph` string field
  - Prevented crashes when pipelines were destroyed

- **Critical: Memory leak on thread pool push failure** (`gstd_http.c`)
  - Added cleanup for request struct and query hash table when thread pool is full
  - Returns 503 Service Unavailable instead of leaking resources

- **Critical: Race condition in HTTP request handling** (`gstd_http.c`)
  - Extended mutex critical section to protect all shared request fields
  - Previously only server pointer was protected, leaving other fields vulnerable

- **Critical: Thread pool cleanup race condition** (`gstd_http.c`)
  - Changed `g_thread_pool_free()` to wait for pending requests before shutdown
  - Prevents use-after-free when stopping HTTP server with in-flight requests

- **Memory leak: GSocketAddress not freed** (`gstd_http.c`)
  - Added `g_object_unref()` after `soup_server_listen()` call

- **Memory leak: Response not freed on early exit** (`gstd_socket.c`)
  - Moved `g_free(response)` before break statement in processing loop

- **Memory leak: g_strsplit result not freed** (`gstd_action.c`)
  - Added `g_strfreev()` on early return paths

- **File descriptor exhaustion** (`gstd_socket.c`)
  - Added `g_io_stream_close()` to properly close socket connections
  - Prevents running out of file descriptors under sustained load

- **Double-free risk in socket stop** (`gstd_socket.c`)
  - Set `self->service = NULL` before cleanup to prevent double-free

- **NULL pointer dereference in HTTP stop** (`gstd_http.c`)
  - Removed invalid session reference that could crash during shutdown

- **Memory allocation safety** (`gstd_http.c`)
  - Changed `malloc()` to `g_new0()` for GLib consistency and zero-initialization

### GStreamer Handling Fixes

- **Critical: Uninitialized variable in bus message parsing** (`gstd_bus_msg_simple.c`)
  - Initialized `debug` variable to NULL to prevent undefined behavior
  - Added NULL check for parsed error before accessing fields
  - Added warning log for unexpected message types

- **Critical: Bus reference leak in pipeline creation** (`gstd_pipeline.c`)
  - Fixed GstBus reference leak when `gstd_pipeline_bus_new()` fails
  - Added proper cleanup path with `gst_object_unref()` on error

- **Critical: Iterator infinite loop prevention** (`gstd_pipeline.c`)
  - Added resync counter with 10-attempt limit to prevent infinite loops
  - Protects against dynamic pipeline modifications during iteration
  - Added debug logging for resync events

- **Race condition: Zero timeout in state query** (`gstd_state.c`)
  - Changed from 0ns (no wait) to 100ms timeout for state queries
  - Prevents incorrect state reporting during async state changes
  - Added logging for pending and failed state queries

- **NULL pointer dereference in state error handling** (`gstd_state.c`)
  - Added NULL check for parsed GError before accessing message field
  - Improved error logging with debug string information

- **Improved state change logging** (`gstd_state.c`)
  - Added INFO logging when state changes are requested
  - Added logging for async state change notifications
  - Better diagnostics for production troubleshooting

### Improved Logging
- **Error logging for IPC startup failures** (`libgstd.c`)
  - Logs IPC type name and error code when startup fails
  - Helps diagnose port conflicts and configuration issues

- **HTTP address validation logging** (`gstd_http.c`)
  - Logs invalid address errors before connection attempts
  - Prevents silent failures with bad configuration

- **Socket error logging** (`gstd_socket.c`)
  - Logs read/write errors with client address for debugging
  - Logs connection close errors
  - Command failures logged at WARNING level with return codes
  - Connection/disconnection events at DEBUG level (non-spammy)

### Changed
- Default `max_threads` changed from `-1` (unlimited) to `16` for both HTTP and TCP
  - Prevents thread exhaustion under heavy load
  - Configurable via `--http-max-threads` and `--tcp-max-threads` options

### Security
- Bounded thread pool prevents resource exhaustion attacks

### Tests
- Added `test_gstd_stability.c` with new test cases:
  - `test_state_query_during_transition` - Tests state query with 100ms timeout
  - `test_rapid_state_changes` - Tests async state change handling
  - `test_pipeline_create_delete_cycle` - Tests for memory leaks in bus references
  - `test_pipeline_many_elements` - Tests iterator with larger pipelines
  - `test_invalid_state_string` - Tests error handling for bad state values
  - `test_multiple_pipelines` - Tests concurrent pipeline operations

## [0.15.2] - Previous release

See git history for changes prior to this changelog.
