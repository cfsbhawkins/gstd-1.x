/*
 * This file is part of GStreamer Daemon
 * Copyright 2015-2022 Ridgerun, LLC (http://www.ridgerun.com)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

/*
 * Tests for HTTP server functionality:
 * - Server startup/shutdown
 * - Request handling
 * - Fast-path endpoints
 * - Error handling
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <stdio.h>

#include <gst/check/gstcheck.h>
#include <gio/gio.h>

#include "gstd_session.h"
#include "gstd_http.h"
#include "gstd_ipc.h"
#include "gstd_list.h"

#define TEST_HTTP_PORT 15000
#define TEST_HTTP_ADDRESS "127.0.0.1"

static GstdSession *test_session = NULL;
static GstdHttp *test_http = NULL;
static GMainLoop *test_loop = NULL;
static GThread *test_loop_thread = NULL;

static gpointer
main_loop_thread_func (gpointer data)
{
  g_main_loop_run ((GMainLoop *) data);
  return NULL;
}

static void
setup (void)
{
  /* The daemon reads these as fallbacks; a developer shell exporting
   * them must not change what these tests observe */
  g_unsetenv ("GSTD_HTTP_API_TOKEN");
  g_unsetenv ("GSTD_HTTP_CORS_ORIGIN");
  g_unsetenv ("GSTD_MAX_PIPELINES");

  test_session = gstd_session_new ("HTTP Test Session");
  fail_if (NULL == test_session);

  test_http = g_object_new (GSTD_TYPE_HTTP,
      "port", TEST_HTTP_PORT,
      "address", TEST_HTTP_ADDRESS,
      NULL);
  fail_if (NULL == test_http);

  /* gstd_ipc_start is a no-op unless the IPC is enabled */
  g_object_set (test_http, "enabled", TRUE, NULL);

  /* The soup server dispatches on the default main context; run it from
   * a thread so the blocking client calls in the tests get serviced. */
  test_loop = g_main_loop_new (NULL, FALSE);
  test_loop_thread = g_thread_new ("http-test-loop",
      main_loop_thread_func, test_loop);

  /* Don't proceed until the loop actually runs: g_main_loop_quit before
   * g_main_loop_run is lost, which would leave teardown joining forever. */
  while (!g_main_loop_is_running (test_loop)) {
    g_usleep (1000);
  }
}

static void
teardown (void)
{
  /* Stop dispatching before destroying the server: tearing the soup
   * server down while the loop thread is mid-dispatch on one of its
   * sources races and triggers GLib criticals under load */
  if (test_loop) {
    g_main_loop_quit (test_loop);
    g_thread_join (test_loop_thread);
    g_main_loop_unref (test_loop);
    test_loop = NULL;
    test_loop_thread = NULL;
  }
  if (test_http) {
    gstd_ipc_stop (GSTD_IPC (test_http));
    g_object_unref (test_http);
    test_http = NULL;
  }
  if (test_session) {
    g_object_unref (test_session);
    test_session = NULL;
  }
}

/*
 * Helper to make a raw HTTP request using GIO. @request_target may carry a
 * query string. @extra_header, when non-NULL, is inserted verbatim (without
 * the trailing CRLF). Returns the body; the full raw response (headers
 * included) is stored in @raw_out when non-NULL. Caller frees both.
 */
static gchar *
http_request (const gchar * method, const gchar * request_target,
    const gchar * extra_header, guint * status_code, gchar ** raw_out)
{
  GSocketClient *client;
  GSocketConnection *conn;
  GInputStream *istream;
  GOutputStream *ostream;
  GError *error = NULL;
  gchar *request;
  gchar *response;
  gssize bytes_read;
  gsize total_read = 0;
  gchar buffer[8192];

  *status_code = 0;
  if (raw_out) {
    *raw_out = NULL;
  }

  client = g_socket_client_new ();
  conn = g_socket_client_connect_to_host (client,
      TEST_HTTP_ADDRESS, TEST_HTTP_PORT, NULL, &error);

  if (!conn) {
    g_clear_error (&error);
    g_object_unref (client);
    return NULL;
  }

  ostream = g_io_stream_get_output_stream (G_IO_STREAM (conn));
  istream = g_io_stream_get_input_stream (G_IO_STREAM (conn));

  request = g_strdup_printf (
      "%s %s HTTP/1.1\r\n"
      "Host: %s:%d\r\n"
      "%s%s"
      "Connection: close\r\n"
      "\r\n",
      method, request_target, TEST_HTTP_ADDRESS, TEST_HTTP_PORT,
      extra_header ? extra_header : "", extra_header ? "\r\n" : "");

  g_output_stream_write_all (ostream, request, strlen (request), NULL, NULL,
      &error);
  g_free (request);

  if (error) {
    g_error_free (error);
    g_io_stream_close (G_IO_STREAM (conn), NULL, NULL);
    g_object_unref (conn);
    g_object_unref (client);
    return NULL;
  }

  /* Connection: close, so read until EOF */
  while (total_read < sizeof (buffer) - 1) {
    bytes_read = g_input_stream_read (istream, buffer + total_read,
        sizeof (buffer) - 1 - total_read, NULL, NULL);
    if (bytes_read <= 0) {
      break;
    }
    total_read += bytes_read;
  }
  buffer[total_read] = '\0';

  /* Parse status code from HTTP response */
  if (sscanf (buffer, "HTTP/1.%*d %u", status_code) != 1) {
    *status_code = 0;
  }

  if (raw_out) {
    *raw_out = g_strdup (buffer);
  }

  /* Find body after headers */
  response = strstr (buffer, "\r\n\r\n");
  if (response) {
    response = g_strdup (response + 4);
  } else {
    response = g_strdup ("");
  }

  g_io_stream_close (G_IO_STREAM (conn), NULL, NULL);
  g_object_unref (conn);
  g_object_unref (client);

  return response;
}

static gchar *
http_get (const gchar * path, guint * status_code)
{
  return http_request ("GET", path, NULL, status_code, NULL);
}

/* Must match GSTD_HTTP_MAX_BODY_SIZE in gstd_http.c */
#define TEST_MAX_BODY_SIZE (8 * 1024 * 1024)

typedef enum
{
  BODY_CHUNKED,                 /* Transfer-Encoding: chunked */
  BODY_FIXED,                   /* Content-Length, whole body sent */
  BODY_EXPECT_CONTINUE          /* Content-Length + Expect: 100-continue,
                                 * no body sent unless the server asks */
} BodyMode;

/*
 * Send @body_size bytes of filler (or @body verbatim when non-NULL) as the
 * request body of @method @request_target, then read the response status.
 * Returns the response body; caller frees.
 */
static gchar *
http_request_with_body (const gchar * method, const gchar * request_target,
    const gchar * content_type, BodyMode mode, const gchar * body,
    gsize body_size, guint * status_code)
{
  GSocketClient *client;
  GSocketConnection *conn;
  GInputStream *istream;
  GOutputStream *ostream;
  GError *error = NULL;
  GString *head;
  gchar *filler;
  gchar *response;
  gsize sent = 0;
  gsize piece;
  gssize bytes_read;
  gsize total_read = 0;
  gboolean ok = TRUE;
  gchar buffer[8192];
  const gsize piece_max = 64 * 1024;

  *status_code = 0;
  if (body) {
    body_size = strlen (body);
  }

  client = g_socket_client_new ();
  conn = g_socket_client_connect_to_host (client,
      TEST_HTTP_ADDRESS, TEST_HTTP_PORT, NULL, &error);
  if (!conn) {
    g_clear_error (&error);
    g_object_unref (client);
    return NULL;
  }

  ostream = g_io_stream_get_output_stream (G_IO_STREAM (conn));
  istream = g_io_stream_get_input_stream (G_IO_STREAM (conn));

  head = g_string_new (NULL);
  g_string_append_printf (head, "%s %s HTTP/1.1\r\nHost: %s:%d\r\n",
      method, request_target, TEST_HTTP_ADDRESS, TEST_HTTP_PORT);
  g_string_append_printf (head, "Content-Type: %s\r\n", content_type);
  if (mode == BODY_CHUNKED) {
    g_string_append (head, "Transfer-Encoding: chunked\r\n");
  } else {
    g_string_append_printf (head, "Content-Length: %" G_GSIZE_FORMAT "\r\n",
        body_size);
  }
  if (mode == BODY_EXPECT_CONTINUE) {
    g_string_append (head, "Expect: 100-continue\r\n");
  }
  g_string_append (head, "Connection: close\r\n\r\n");

  ok = g_output_stream_write_all (ostream, head->str, head->len, NULL, NULL,
      NULL);
  g_string_free (head, TRUE);

  filler = g_malloc (piece_max);
  memset (filler, 'a', piece_max);

  while (ok && mode != BODY_EXPECT_CONTINUE && sent < body_size) {
    const gchar *data = body ? body + sent : filler;

    piece = MIN (piece_max, body_size - sent);
    if (mode == BODY_CHUNKED) {
      gchar *size_line = g_strdup_printf ("%" G_GSIZE_MODIFIER "x\r\n", piece);

      ok = g_output_stream_write_all (ostream, size_line, strlen (size_line),
          NULL, NULL, NULL);
      g_free (size_line);
    }
    ok = ok && g_output_stream_write_all (ostream, data, piece, NULL, NULL,
        NULL);
    if (mode == BODY_CHUNKED) {
      ok = ok && g_output_stream_write_all (ostream, "\r\n", 2, NULL, NULL,
          NULL);
    }
    sent += piece;
  }
  if (ok && mode == BODY_CHUNKED) {
    g_output_stream_write_all (ostream, "0\r\n\r\n", 5, NULL, NULL, NULL);
  }
  g_free (filler);

  /* Connection: close, so read until EOF */
  while (total_read < sizeof (buffer) - 1) {
    bytes_read = g_input_stream_read (istream, buffer + total_read,
        sizeof (buffer) - 1 - total_read, NULL, NULL);
    if (bytes_read <= 0) {
      break;
    }
    total_read += bytes_read;
  }
  buffer[total_read] = '\0';

  if (sscanf (buffer, "HTTP/1.%*d %u", status_code) != 1) {
    *status_code = 0;
  }

  response = strstr (buffer, "\r\n\r\n");
  response = g_strdup (response ? response + 4 : "");

  g_io_stream_close (G_IO_STREAM (conn), NULL, NULL);
  g_object_unref (conn);
  g_object_unref (client);

  return response;
}

/*
 * Test: HTTP server starts successfully
 */
GST_START_TEST (test_http_server_start)
{
  GstdReturnCode ret;

  ret = gstd_ipc_start (GSTD_IPC (test_http), test_session);
  fail_if (ret != GSTD_EOK, "HTTP server failed to start");
}
GST_END_TEST;

/*
 * Test: HTTP server stops gracefully
 */
GST_START_TEST (test_http_server_stop)
{
  GstdReturnCode ret;

  ret = gstd_ipc_start (GSTD_IPC (test_http), test_session);
  fail_if (ret != GSTD_EOK);

  ret = gstd_ipc_stop (GSTD_IPC (test_http));
  fail_if (ret != GSTD_EOK, "HTTP server failed to stop");
}
GST_END_TEST;

/*
 * Test: Health endpoint returns 200 OK
 */
GST_START_TEST (test_http_health_endpoint)
{
  GstdReturnCode ret;
  gchar *response;
  guint status_code;

  ret = gstd_ipc_start (GSTD_IPC (test_http), test_session);
  fail_if (ret != GSTD_EOK);

  /* Give server time to start */
  g_usleep (100000);

  response = http_get ("/health", &status_code);
  fail_if (status_code != 200, "Health endpoint returned %u, expected 200", status_code);
  fail_if (response == NULL);
  fail_if (strstr (response, "healthy") == NULL,
      "Health response should contain 'healthy'");

  g_free (response);
}
GST_END_TEST;

/*
 * Test: Pipelines status endpoint returns valid JSON
 */
GST_START_TEST (test_http_pipelines_status_endpoint)
{
  GstdReturnCode ret;
  GstdObject *node;
  gchar *response;
  guint status_code;

  ret = gstd_ipc_start (GSTD_IPC (test_http), test_session);
  fail_if (ret != GSTD_EOK);

  /* Create a test pipeline */
  ret = gstd_get_by_uri (test_session, "/pipelines", &node);
  fail_if (ret != GSTD_EOK);
  ret = gstd_object_create (node, "test_pipe", "fakesrc ! fakesink");
  fail_if (ret != GSTD_EOK);
  gst_object_unref (node);

  g_usleep (100000);

  response = http_get ("/pipelines/status", &status_code);
  fail_if (status_code != 200, "Pipelines status returned %u", status_code);
  fail_if (response == NULL);
  fail_if (strstr (response, "pipelines") == NULL, "Response should contain 'pipelines'");
  fail_if (strstr (response, "test_pipe") == NULL, "Response should contain pipeline name");

  g_free (response);
}
GST_END_TEST;

/*
 * Test: GET /pipelines returns pipeline list
 */
GST_START_TEST (test_http_get_pipelines)
{
  GstdReturnCode ret;
  gchar *response;
  guint status_code;

  ret = gstd_ipc_start (GSTD_IPC (test_http), test_session);
  fail_if (ret != GSTD_EOK);

  g_usleep (100000);

  response = http_get ("/pipelines", &status_code);
  fail_if (status_code != 200, "GET /pipelines returned %u", status_code);
  fail_if (response == NULL);
  /* Response should be valid JSON with code field */
  fail_if (strstr (response, "\"code\"") == NULL, "Response should contain code field");

  g_free (response);
}
GST_END_TEST;

/*
 * Test: Invalid path returns 404
 */
GST_START_TEST (test_http_invalid_path)
{
  GstdReturnCode ret;
  gchar *response;
  guint status_code;

  ret = gstd_ipc_start (GSTD_IPC (test_http), test_session);
  fail_if (ret != GSTD_EOK);

  g_usleep (100000);

  response = http_get ("/nonexistent/path/here", &status_code);
  /* Should return 404 Not Found */
  fail_if (status_code != 404, "Invalid path returned %u, expected 404", status_code);

  g_free (response);
}
GST_END_TEST;

/*
 * Test: Multiple concurrent requests don't crash
 */
GST_START_TEST (test_http_concurrent_requests)
{
  GstdReturnCode ret;
  gchar *response;
  guint status_code;
  int i;

  ret = gstd_ipc_start (GSTD_IPC (test_http), test_session);
  fail_if (ret != GSTD_EOK);

  g_usleep (100000);

  /* Make several sequential requests to verify server stability */
  for (i = 0; i < 20; i++) {
    response = http_get ("/health", &status_code);
    fail_if (status_code != 200, "Request %d failed with status %u", i, status_code);
    g_free (response);
  }
}
GST_END_TEST;

/*
 * Test: Server restart after stop
 */
GST_START_TEST (test_http_server_restart)
{
  GstdReturnCode ret;
  gchar *response;
  guint status_code;

  /* Start server */
  ret = gstd_ipc_start (GSTD_IPC (test_http), test_session);
  fail_if (ret != GSTD_EOK);

  g_usleep (100000);

  response = http_get ("/health", &status_code);
  fail_if (status_code != 200);
  g_free (response);

  /* Stop server */
  ret = gstd_ipc_stop (GSTD_IPC (test_http));
  fail_if (ret != GSTD_EOK);

  g_usleep (100000);

  /* Start again */
  ret = gstd_ipc_start (GSTD_IPC (test_http), test_session);
  fail_if (ret != GSTD_EOK);

  g_usleep (100000);

  response = http_get ("/health", &status_code);
  fail_if (status_code != 200, "Server restart failed, status %u", status_code);
  g_free (response);
}
GST_END_TEST;

/*
 * Test: no CORS headers are emitted unless an origin is configured
 */
GST_START_TEST (test_http_no_cors_by_default)
{
  GstdReturnCode ret;
  gchar *response;
  gchar *raw = NULL;
  guint status_code;

  ret = gstd_ipc_start (GSTD_IPC (test_http), test_session);
  fail_if (ret != GSTD_EOK);

  g_usleep (100000);

  response = http_request ("GET", "/health", NULL, &status_code, &raw);
  fail_if (status_code != 200);
  fail_if (raw == NULL);
  fail_if (strstr (raw, "Access-Control-Allow-Origin") != NULL,
      "No CORS headers should be emitted without a configured origin");

  g_free (response);
  g_free (raw);
}
GST_END_TEST;

/*
 * Test: a configured CORS origin is echoed instead of a wildcard
 */
GST_START_TEST (test_http_cors_origin_configured)
{
  GstdReturnCode ret;
  gchar *response;
  gchar *raw = NULL;
  guint status_code;

  g_object_set (test_http, "cors-origin", "http://example.com", NULL);

  ret = gstd_ipc_start (GSTD_IPC (test_http), test_session);
  fail_if (ret != GSTD_EOK);

  g_usleep (100000);

  response = http_request ("GET", "/health", NULL, &status_code, &raw);
  fail_if (status_code != 200);
  fail_if (raw == NULL);
  fail_if (strstr (raw,
          "Access-Control-Allow-Origin: http://example.com") == NULL,
      "Configured origin should appear in CORS headers");
  fail_if (strstr (raw, "Access-Control-Allow-Origin: *") != NULL,
      "Wildcard origin must not be emitted");

  g_free (response);
  g_free (raw);
}
GST_END_TEST;

/*
 * Test: with an API token configured, requests need a bearer token,
 * while /health stays open for liveness probes
 */
GST_START_TEST (test_http_api_token)
{
  GstdReturnCode ret;
  gchar *response;
  guint status_code;

  g_object_set (test_http, "api-token", "test-secret-token", NULL);

  ret = gstd_ipc_start (GSTD_IPC (test_http), test_session);
  fail_if (ret != GSTD_EOK);

  g_usleep (100000);

  /* No credentials: rejected */
  response = http_get ("/pipelines", &status_code);
  fail_if (status_code != 401,
      "Request without token returned %u, expected 401", status_code);
  g_free (response);

  /* Wrong credentials: rejected */
  response = http_request ("GET", "/pipelines",
      "Authorization: Bearer wrong-token", &status_code, NULL);
  fail_if (status_code != 401,
      "Request with bad token returned %u, expected 401", status_code);
  g_free (response);

  /* Fast-path endpoints are covered too */
  response = http_get ("/pipelines/status", &status_code);
  fail_if (status_code != 401,
      "Fast-path without token returned %u, expected 401", status_code);
  g_free (response);

  /* Correct credentials: accepted */
  response = http_request ("GET", "/pipelines",
      "Authorization: Bearer test-secret-token", &status_code, NULL);
  fail_if (status_code != 200,
      "Request with valid token returned %u, expected 200", status_code);
  g_free (response);

  /* Health probes stay unauthenticated */
  response = http_get ("/health", &status_code);
  fail_if (status_code != 200,
      "/health with token configured returned %u, expected 200", status_code);
  g_free (response);
}
GST_END_TEST;

/*
 * Test: resource names with whitespace are rejected before reaching the
 * space-separated parser command language
 */
GST_START_TEST (test_http_rejects_name_with_whitespace)
{
  GstdReturnCode ret;
  gchar *response;
  guint status_code;

  ret = gstd_ipc_start (GSTD_IPC (test_http), test_session);
  fail_if (ret != GSTD_EOK);

  g_usleep (100000);

  /* %20 decodes to a space inside the name */
  response = http_request ("POST",
      "/pipelines?name=evil%20injected&description=fakesrc%20!%20fakesink",
      NULL, &status_code, NULL);
  fail_if (status_code != 400,
      "POST with whitespace in name returned %u, expected 400", status_code);
  g_free (response);

  /* A clean name on the same endpoint still works */
  response = http_request ("POST",
      "/pipelines?name=clean_name&description=fakesrc%20!%20fakesink",
      NULL, &status_code, NULL);
  fail_if (status_code != 200,
      "POST with valid name returned %u, expected 200", status_code);
  g_free (response);
}
GST_END_TEST;

/*
 * Test: OPTIONS never reaches state-disclosing handlers, so a CORS
 * preflight on /pipelines/status cannot bypass a configured token
 */
GST_START_TEST (test_http_options_does_not_leak_status)
{
  GstdReturnCode ret;
  GstdObject *node;
  gchar *response;
  guint status_code;

  g_object_set (test_http, "api-token", "test-secret-token", NULL);

  ret = gstd_ipc_start (GSTD_IPC (test_http), test_session);
  fail_if (ret != GSTD_EOK);

  /* Create a pipeline whose name must not appear in any OPTIONS body */
  ret = gstd_get_by_uri (test_session, "/pipelines", &node);
  fail_if (ret != GSTD_EOK);
  ret = gstd_object_create (node, "secret_pipe", "fakesrc ! fakesink");
  fail_if (ret != GSTD_EOK);
  gst_object_unref (node);

  g_usleep (100000);

  response = http_request ("OPTIONS", "/pipelines/status", NULL,
      &status_code, NULL);
  fail_if (status_code != 200,
      "OPTIONS preflight returned %u, expected 200", status_code);
  fail_if (response == NULL);
  fail_if (strstr (response, "secret_pipe") != NULL,
      "OPTIONS response must not contain pipeline names");
  fail_if (strstr (response, "pipelines") != NULL,
      "OPTIONS response must not contain pipeline state JSON");
  g_free (response);

  /* Non-GET methods on the status endpoint are rejected even when
   * authenticated (unauthenticated ones already get 401 first) */
  response = http_request ("POST", "/pipelines/status",
      "Authorization: Bearer test-secret-token", &status_code, NULL);
  fail_if (status_code != 405,
      "POST /pipelines/status returned %u, expected 405", status_code);
  g_free (response);
}
GST_END_TEST;

/*
 * Test: exceeding the pipeline cap over HTTP returns 429
 */
GST_START_TEST (test_http_pipeline_cap_returns_429)
{
  GstdReturnCode ret;
  GstdList *pipelines = NULL;
  gchar *response;
  guint status_code;

  g_object_get (test_session, "pipelines", &pipelines, NULL);
  fail_if (NULL == pipelines);
  g_object_set (pipelines, "max-children", 1, NULL);
  g_object_unref (pipelines);

  ret = gstd_ipc_start (GSTD_IPC (test_http), test_session);
  fail_if (ret != GSTD_EOK);

  g_usleep (100000);

  response = http_request ("POST",
      "/pipelines?name=first&description=fakesrc%20!%20fakesink",
      NULL, &status_code, NULL);
  fail_if (status_code != 200,
      "First create returned %u, expected 200", status_code);
  g_free (response);

  response = http_request ("POST",
      "/pipelines?name=second&description=fakesrc%20!%20fakesink",
      NULL, &status_code, NULL);
  fail_if (status_code != 429,
      "Create past the cap returned %u, expected 429", status_code);
  g_free (response);
}
GST_END_TEST;

/*
 * Test: encoded whitespace in the request path is rejected, since the
 * decoded path is spliced into the same parser command as the name
 */
GST_START_TEST (test_http_rejects_path_with_whitespace)
{
  GstdReturnCode ret;
  gchar *response;
  guint status_code;

  ret = gstd_ipc_start (GSTD_IPC (test_http), test_session);
  fail_if (ret != GSTD_EOK);

  g_usleep (100000);

  response = http_request ("POST",
      "/pipelines%20other?name=safe&description=fakesrc",
      NULL, &status_code, NULL);
  fail_if (status_code != 400,
      "POST with whitespace in path returned %u, expected 400", status_code);
  g_free (response);
}
GST_END_TEST;

/*
 * Test: a 401 carries CORS headers when an origin is configured, so a
 * browser page sees the auth failure instead of a network error
 */
GST_START_TEST (test_http_unauthorized_carries_cors)
{
  GstdReturnCode ret;
  gchar *response;
  gchar *raw = NULL;
  guint status_code;

  g_object_set (test_http,
      "api-token", "test-secret-token",
      "cors-origin", "http://example.com", NULL);

  ret = gstd_ipc_start (GSTD_IPC (test_http), test_session);
  fail_if (ret != GSTD_EOK);

  g_usleep (100000);

  response = http_request ("GET", "/pipelines", NULL, &status_code, &raw);
  fail_if (status_code != 401,
      "Request without token returned %u, expected 401", status_code);
  fail_if (raw == NULL);
  fail_if (strstr (raw,
          "Access-Control-Allow-Origin: http://example.com") == NULL,
      "401 should carry the configured CORS origin");

  g_free (response);
  g_free (raw);
}
GST_END_TEST;

/*
 * Test: a chunked body (no Content-Length) past the limit is rejected
 * with 413 while it is received, for JSON and non-JSON alike
 */
GST_START_TEST (test_http_chunked_body_too_large)
{
  GstdReturnCode ret;
  gchar *response;
  guint status_code;

  ret = gstd_ipc_start (GSTD_IPC (test_http), test_session);
  fail_if (ret != GSTD_EOK);

  g_usleep (100000);

  response = http_request_with_body ("POST", "/pipelines",
      "application/json", BODY_CHUNKED, NULL, TEST_MAX_BODY_SIZE + 1,
      &status_code);
  fail_if (status_code != 413,
      "Oversized chunked JSON returned %u, expected 413", status_code);
  g_free (response);

  response = http_request_with_body ("POST",
      "/pipelines?name=p&description=fakesrc%20!%20fakesink", "text/plain",
      BODY_CHUNKED, NULL, TEST_MAX_BODY_SIZE + 1, &status_code);
  fail_if (status_code != 413,
      "Oversized chunked text/plain returned %u, expected 413", status_code);
  g_free (response);

  /* The rejected create must not have run */
  response = http_get ("/pipelines/status", &status_code);
  fail_if (status_code != 200);
  fail_if (strstr (response, "\"count\": 0") == NULL,
      "An oversized request must not reach the handler: %s", response);
  g_free (response);
}
GST_END_TEST;

/*
 * Test: fast-path endpoints, which answer without the thread pool, are
 * subject to the same limit
 */
GST_START_TEST (test_http_fast_path_body_too_large)
{
  GstdReturnCode ret;
  gchar *response;
  guint status_code;

  ret = gstd_ipc_start (GSTD_IPC (test_http), test_session);
  fail_if (ret != GSTD_EOK);

  g_usleep (100000);

  response = http_request_with_body ("POST",
      "/pipelines/clock_sync?source=a&targets=b", "application/octet-stream",
      BODY_CHUNKED, NULL, TEST_MAX_BODY_SIZE + 1, &status_code);
  fail_if (status_code != 413,
      "Oversized clock_sync body returned %u, expected 413", status_code);
  g_free (response);

  response = http_request_with_body ("GET", "/pipelines/status",
      "application/json", BODY_FIXED, NULL, TEST_MAX_BODY_SIZE + 1,
      &status_code);
  fail_if (status_code != 413,
      "Oversized /pipelines/status body returned %u, expected 413",
      status_code);
  g_free (response);

  response = http_request_with_body ("GET", "/health", "text/plain",
      BODY_CHUNKED, NULL, TEST_MAX_BODY_SIZE + 1, &status_code);
  fail_if (status_code != 413,
      "Oversized /health body returned %u, expected 413", status_code);
  g_free (response);

  response = http_request_with_body ("GET", "/elements/fakesrc", "text/plain",
      BODY_CHUNKED, NULL, TEST_MAX_BODY_SIZE + 1, &status_code);
  fail_if (status_code != 413,
      "Oversized /elements body returned %u, expected 413", status_code);
  g_free (response);
}
GST_END_TEST;

/*
 * Test: an oversized Content-Length is rejected from the headers alone,
 * so a client using Expect: 100-continue never has to send the body
 */
GST_START_TEST (test_http_declared_length_too_large)
{
  GstdReturnCode ret;
  gchar *response;
  guint status_code;

  ret = gstd_ipc_start (GSTD_IPC (test_http), test_session);
  fail_if (ret != GSTD_EOK);

  g_usleep (100000);

  response = http_request_with_body ("POST", "/pipelines", "text/plain",
      BODY_EXPECT_CONTINUE, NULL, TEST_MAX_BODY_SIZE + 1, &status_code);
  fail_if (status_code != 413,
      "Oversized declared length returned %u, expected 413", status_code);
  g_free (response);

  response = http_request_with_body ("PUT", "/pipelines/p/state",
      "application/json", BODY_FIXED, NULL, TEST_MAX_BODY_SIZE + 1,
      &status_code);
  fail_if (status_code != 413,
      "Oversized fixed-length body returned %u, expected 413", status_code);
  g_free (response);
}
GST_END_TEST;

/*
 * Test: bodies within the limit still work, chunked or not
 */
GST_START_TEST (test_http_body_within_limit)
{
  GstdReturnCode ret;
  gchar *response;
  guint status_code;

  ret = gstd_ipc_start (GSTD_IPC (test_http), test_session);
  fail_if (ret != GSTD_EOK);

  g_usleep (100000);

  response = http_request_with_body ("POST", "/pipelines",
      "application/json", BODY_CHUNKED,
      "{\"name\": \"chunked_pipe\", \"description\": \"fakesrc ! fakesink\"}",
      0, &status_code);
  fail_if (status_code != 200,
      "Chunked JSON create returned %u, expected 200", status_code);
  g_free (response);

  /* Exactly at the limit is allowed */
  response = http_request_with_body ("GET", "/health", "text/plain",
      BODY_CHUNKED, NULL, TEST_MAX_BODY_SIZE, &status_code);
  fail_if (status_code != 200,
      "Body at the limit returned %u, expected 200", status_code);
  g_free (response);

  response = http_get ("/pipelines/status", &status_code);
  fail_if (strstr (response, "chunked_pipe") == NULL,
      "Chunked JSON create did not create the pipeline: %s", response);
  g_free (response);
}
GST_END_TEST;

/*
 * Test: with bearer auth enabled, only GET and HEAD on /health skip
 * authentication; every other method gets 405 and never the health body
 */
GST_START_TEST (test_http_health_method_matrix)
{
  static const gchar *refused[] = { "POST", "PUT", "DELETE", "PATCH",
    "TRACE", "PROPFIND", NULL
  };
  GstdReturnCode ret;
  gchar *response;
  gchar *raw = NULL;
  guint status_code;
  guint i;

  g_object_set (test_http, "api-token", "test-secret-token", NULL);

  ret = gstd_ipc_start (GSTD_IPC (test_http), test_session);
  fail_if (ret != GSTD_EOK);

  g_usleep (100000);

  response = http_get ("/health", &status_code);
  fail_if (status_code != 200, "GET /health returned %u", status_code);
  fail_if (strstr (response, "healthy") == NULL);
  g_free (response);

  response = http_request ("HEAD", "/health", NULL, &status_code, NULL);
  fail_if (status_code != 200, "HEAD /health returned %u", status_code);
  fail_if (response[0] != '\0', "HEAD /health must not carry a body");
  g_free (response);

  for (i = 0; refused[i]; i++) {
    /* Unauthenticated and authenticated alike: the method is refused */
    response = http_request (refused[i], "/health", NULL, &status_code,
        &raw);
    fail_if (status_code != 405, "%s /health returned %u, expected 405",
        refused[i], status_code);
    fail_if (strstr (response, "healthy") != NULL,
        "%s /health must not return the health response", refused[i]);
    fail_if (strstr (raw, "Allow: GET, HEAD") == NULL,
        "%s /health 405 should advertise Allow: GET, HEAD", refused[i]);
    g_free (response);
    g_free (raw);

    response = http_request (refused[i], "/health",
        "Authorization: Bearer test-secret-token", &status_code, NULL);
    fail_if (status_code != 405,
        "Authenticated %s /health returned %u, expected 405", refused[i],
        status_code);
    g_free (response);
  }

  /* OPTIONS is the shared, empty CORS preflight, not the health body */
  response = http_request ("OPTIONS", "/health", NULL, &status_code, NULL);
  fail_if (status_code != 200, "OPTIONS /health returned %u", status_code);
  fail_if (strstr (response, "healthy") != NULL,
      "OPTIONS /health must not return the health response");
  g_free (response);
}
GST_END_TEST;

/* Feed @argv through the HTTP option group, as the daemon does */
static void
parse_http_options (gint argc, gchar ** argv)
{
  GOptionContext *context;
  GOptionGroup *group = NULL;
  GError *error = NULL;
  gboolean parsed;

  gstd_ipc_get_option_group (GSTD_IPC (test_http), &group);
  fail_if (NULL == group);

  context = g_option_context_new (NULL);
  g_option_context_add_group (context, group);
  parsed = g_option_context_parse (context, &argc, &argv, &error);
  fail_if (!parsed, "Option parsing failed: %s",
      error ? error->message : "unknown");
  g_option_context_free (context);
}

/*
 * Test: an explicitly empty token in the environment is refused at
 * startup instead of silently disabling authentication
 */
GST_START_TEST (test_http_empty_token_env_rejected)
{
  GstdReturnCode ret;

  g_setenv ("GSTD_HTTP_API_TOKEN", "", TRUE);

  ret = gstd_ipc_start (GSTD_IPC (test_http), test_session);
  g_unsetenv ("GSTD_HTTP_API_TOKEN");
  fail_if (ret == GSTD_EOK,
      "HTTP server started with an empty GSTD_HTTP_API_TOKEN");
}
GST_END_TEST;

/*
 * Test: --http-api-token= (empty) is refused at startup
 */
GST_START_TEST (test_http_empty_token_cli_rejected)
{
  GstdReturnCode ret;
  gchar *argv[] = { (gchar *) "gstd", (gchar *) "--http-api-token=", NULL };

  parse_http_options (2, argv);

  ret = gstd_ipc_start (GSTD_IPC (test_http), test_session);
  fail_if (ret == GSTD_EOK,
      "HTTP server started with an empty --http-api-token");
}
GST_END_TEST;

/*
 * Test: a non-empty token from the command line still enables auth
 */
GST_START_TEST (test_http_token_cli_accepted)
{
  GstdReturnCode ret;
  gchar *response;
  guint status_code;
  gchar *argv[] = { (gchar *) "gstd",
    (gchar *) "--http-api-token=cli-token", NULL
  };

  parse_http_options (2, argv);

  ret = gstd_ipc_start (GSTD_IPC (test_http), test_session);
  fail_if (ret != GSTD_EOK);

  g_usleep (100000);

  response = http_get ("/pipelines", &status_code);
  fail_if (status_code != 401, "Expected 401, got %u", status_code);
  g_free (response);

  response = http_request ("GET", "/pipelines",
      "Authorization: Bearer cli-token", &status_code, NULL);
  fail_if (status_code != 200, "Expected 200, got %u", status_code);
  g_free (response);
}
GST_END_TEST;

/*
 * Test: the api-token property refuses an empty string and keeps its
 * current value, so a rotation to "" cannot disable authentication
 */
GST_START_TEST (test_http_empty_token_property_rejected)
{
  GstdReturnCode ret;
  gchar *token = NULL;
  gchar *response;
  guint status_code;

  /* From the default (disabled) state */
  ASSERT_WARNING (g_object_set (test_http, "api-token", "", NULL));
  g_object_get (test_http, "api-token", &token, NULL);
  fail_if (token != NULL, "Empty token was stored: \"%s\"", token);

  /* From a configured token: the rotation to "" is refused */
  g_object_set (test_http, "api-token", "test-secret-token", NULL);
  ASSERT_WARNING (g_object_set (test_http, "api-token", "", NULL));
  g_object_get (test_http, "api-token", &token, NULL);
  fail_if (g_strcmp0 (token, "test-secret-token") != 0,
      "Token changed to \"%s\" after an empty assignment", token);
  g_free (token);

  ret = gstd_ipc_start (GSTD_IPC (test_http), test_session);
  fail_if (ret != GSTD_EOK);

  g_usleep (100000);

  /* Still enforced; an empty bearer credential does not match */
  response = http_request ("GET", "/pipelines", "Authorization: Bearer ",
      &status_code, NULL);
  fail_if (status_code != 401, "Empty bearer returned %u, expected 401",
      status_code);
  g_free (response);

  /* NULL remains the explicit way to disable authentication */
  g_object_set (test_http, "api-token", NULL, NULL);
  response = http_get ("/pipelines", &status_code);
  fail_if (status_code != 200, "Expected 200 once disabled, got %u",
      status_code);
  g_free (response);
}
GST_END_TEST;

/* Origins that are not exactly one concrete serialized http(s) origin */
static const gchar *invalid_cors_origins[] = {
  "*",
  "",
  "null",
  "example.com",
  "ftp://example.com",
  "http://",
  "http://example.com/",
  "http://example.com/app",
  "http://example.com?x=1",
  "http://example.com#top",
  "http://user@example.com",
  "http://user:pass@example.com",
  "http://a.example.com,http://b.example.com",
  "http://a.example.com http://b.example.com",
  "http://example.com\r\nX-Injected: 1",
  "HTTP://example.com",
  "http://Example.com",
  "http://*.example.com",
  "http://example..com",
  "http://.example.com",
  "http://example.com.",
  "http://example.com:",
  "http://example.com:0",
  "http://example.com:080",
  "http://example.com:65536",
  "http://example.com:8080x",
  "http://example.com:80",
  "https://example.com:443",
  "http://[::1",
  "http://[zz::1]",
  "http://[127.0.0.1]",
  "http://[::1]x",
  NULL
};

/*
 * Test: the cors-origin property refuses wildcard and malformed origins
 * and keeps its current value; concrete origins are accepted
 */
GST_START_TEST (test_http_cors_origin_property_validation)
{
  static const gchar *valid[] = {
    "http://example.com",
    "https://ui.example.com:8443",
    "http://127.0.0.1:3000",
    "http://[::1]:8080",
    "http://localhost",
    "https://example.com:80",
    NULL
  };
  gchar *origin = NULL;
  guint i;

  for (i = 0; invalid_cors_origins[i]; i++) {
    ASSERT_WARNING (g_object_set (test_http, "cors-origin",
            invalid_cors_origins[i], NULL));
    g_object_get (test_http, "cors-origin", &origin, NULL);
    fail_if (origin != NULL, "Invalid origin \"%s\" was stored",
        invalid_cors_origins[i]);
  }

  for (i = 0; valid[i]; i++) {
    g_object_set (test_http, "cors-origin", valid[i], NULL);
    g_object_get (test_http, "cors-origin", &origin, NULL);
    fail_if (g_strcmp0 (origin, valid[i]) != 0,
        "Valid origin \"%s\" was not stored", valid[i]);
    g_free (origin);
  }

  /* A rejected assignment keeps the configured origin */
  g_object_set (test_http, "cors-origin", "http://example.com", NULL);
  ASSERT_WARNING (g_object_set (test_http, "cors-origin", "*", NULL));
  g_object_get (test_http, "cors-origin", &origin, NULL);
  fail_if (g_strcmp0 (origin, "http://example.com") != 0,
      "Origin changed to \"%s\" after a rejected assignment", origin);
  g_free (origin);

  /* NULL still disables CORS */
  g_object_set (test_http, "cors-origin", NULL, NULL);
  g_object_get (test_http, "cors-origin", &origin, NULL);
  fail_if (origin != NULL);
}
GST_END_TEST;

/*
 * Test: a wildcard or malformed GSTD_HTTP_CORS_ORIGIN is refused at startup
 */
GST_START_TEST (test_http_cors_origin_env_rejected)
{
  GstdReturnCode ret;
  guint i;

  for (i = 0; invalid_cors_origins[i]; i++) {
    g_setenv ("GSTD_HTTP_CORS_ORIGIN", invalid_cors_origins[i], TRUE);

    ret = gstd_ipc_start (GSTD_IPC (test_http), test_session);
    fail_if (ret == GSTD_EOK,
        "HTTP server started with GSTD_HTTP_CORS_ORIGIN=\"%s\"",
        invalid_cors_origins[i]);

    /* The env value is only a fallback for an unset origin: clear it so
     * the next iteration reads the environment again */
    g_object_set (test_http, "cors-origin", NULL, NULL);
  }
  g_unsetenv ("GSTD_HTTP_CORS_ORIGIN");
}
GST_END_TEST;

/*
 * Test: --http-cors-origin=* and a path-bearing origin are refused at
 * startup
 */
GST_START_TEST (test_http_cors_origin_cli_rejected)
{
  GstdReturnCode ret;
  gchar *wildcard[] = { (gchar *) "gstd", (gchar *) "--http-cors-origin=*",
    NULL
  };
  gchar *with_path[] = { (gchar *) "gstd",
    (gchar *) "--http-cors-origin=http://example.com/app", NULL
  };

  parse_http_options (2, wildcard);
  ret = gstd_ipc_start (GSTD_IPC (test_http), test_session);
  fail_if (ret == GSTD_EOK, "HTTP server started with --http-cors-origin=*");

  parse_http_options (2, with_path);
  ret = gstd_ipc_start (GSTD_IPC (test_http), test_session);
  fail_if (ret == GSTD_EOK,
      "HTTP server started with an origin carrying a path");
}
GST_END_TEST;

static Suite *
gstd_http_suite (void)
{
  Suite *suite = suite_create ("gstd_http");
  TCase *tc = tcase_create ("general");

  suite_add_tcase (suite, tc);
  tcase_set_timeout (tc, 30);
  tcase_add_checked_fixture (tc, setup, teardown);

  tcase_add_test (tc, test_http_server_start);
  tcase_add_test (tc, test_http_server_stop);
  tcase_add_test (tc, test_http_health_endpoint);
  tcase_add_test (tc, test_http_pipelines_status_endpoint);
  tcase_add_test (tc, test_http_get_pipelines);
  tcase_add_test (tc, test_http_invalid_path);
  tcase_add_test (tc, test_http_concurrent_requests);
  tcase_add_test (tc, test_http_server_restart);
  tcase_add_test (tc, test_http_no_cors_by_default);
  tcase_add_test (tc, test_http_cors_origin_configured);
  tcase_add_test (tc, test_http_api_token);
  tcase_add_test (tc, test_http_rejects_name_with_whitespace);
  tcase_add_test (tc, test_http_options_does_not_leak_status);
  tcase_add_test (tc, test_http_pipeline_cap_returns_429);
  tcase_add_test (tc, test_http_rejects_path_with_whitespace);
  tcase_add_test (tc, test_http_unauthorized_carries_cors);
  tcase_add_test (tc, test_http_chunked_body_too_large);
  tcase_add_test (tc, test_http_fast_path_body_too_large);
  tcase_add_test (tc, test_http_declared_length_too_large);
  tcase_add_test (tc, test_http_body_within_limit);
  tcase_add_test (tc, test_http_health_method_matrix);
  tcase_add_test (tc, test_http_empty_token_env_rejected);
  tcase_add_test (tc, test_http_empty_token_cli_rejected);
  tcase_add_test (tc, test_http_token_cli_accepted);
  tcase_add_test (tc, test_http_empty_token_property_rejected);
  tcase_add_test (tc, test_http_cors_origin_property_validation);
  tcase_add_test (tc, test_http_cors_origin_env_rejected);
  tcase_add_test (tc, test_http_cors_origin_cli_rejected);

  return suite;
}

GST_CHECK_MAIN (gstd_http);
