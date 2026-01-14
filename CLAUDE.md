# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/claude-code) when working with code in this repository.

## Project Overview

GStreamer Daemon (gstd) is a C-based framework that provides remote control of GStreamer multimedia pipelines over TCP/HTTP connections. It separates control logic from streaming logic, enabling production deployments where pipelines are managed remotely.

## Build Commands

### Meson (Recommended)
```bash
meson setup build
ninja -C build
sudo ninja -C build install
ninja -C build test
```

### Autotools (Legacy - being deprecated)
```bash
./autogen.sh
./configure
make
sudo make install
make check
```

## Project Structure

- **libgstd/** - Core daemon library (~11,500 LOC). Manages pipelines, TCP protocol, HTTP server, message buses.
- **libgstc/** - Client libraries with C, Python (pygstc), and JavaScript implementations.
- **gstd/** - Main daemon application entry point and daemonization logic.
- **gst_client/** - Interactive command-line client with tab completion.
- **tests/** - Test suites for gstd, libgstc, and libgstd.
- **examples/** - C, Python, and web UI examples.
- **init/** - Systemd service file and init.d script.

## Dependencies

Required packages:
- gstreamer-1.0, gstreamer-base-1.0 (>= 1.0.0)
- json-glib-1.0 (>= 0.16.2)
- jansson (>= 2.7)
- gio-2.0, gio-unix-2.0 (>= 2.44.1)
- libsoup-3.0 (>= 3.0) or libsoup-2.4 (fallback)
- libdaemon (>= 0.14)
- libedit (>= 3.0)

## Code Style

- C11 standard
- GLib/GObject conventions (g_ prefixes, GstdObject base class)
- All public functions prefixed with `gstd_`
- Header guards use `GSTD_*_H_` pattern

## Testing

Run the full test suite:
```bash
ninja -C build test
```

Tests are located in `tests/gstd/`, `tests/libgstc/`, and `tests/libgstd/`.

## Fork Notes

This is a fork of RidgeRun/gstd-1.x.

## Stability Improvements (v0.16.0)

Key stability changes made in this fork:

- **Thread pool limits**: Default max_threads changed from unlimited to 16 for HTTP/TCP
- **Fast-path endpoints**: `/health` and `/pipelines/status` bypass thread pool
- **Memory leak fixes**: Multiple fixes in HTTP, socket, pipeline, and action handlers
- **Race condition fixes**: Mutex critical sections, thread pool cleanup
- **GStreamer handling**: State query timeout (100ms), iterator resync limits, bus reference fixes

## Key Files for Stability

When investigating stability issues, focus on:
- `libgstd/gstd_http.c` - HTTP server, thread pool, fast-path handlers
- `libgstd/gstd_socket.c` - TCP connections, FD management
- `libgstd/gstd_pipeline.c` - Pipeline lifecycle, bus handling
- `libgstd/gstd_state.c` - State transitions, async handling
