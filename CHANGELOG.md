# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.16.1] - 2026-01-14

### Added
- Docker support for local test execution
  - Added `Dockerfile` with Ubuntu 22.04 and all build dependencies
  - Added `docker-test.sh` helper script for running test suite

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
