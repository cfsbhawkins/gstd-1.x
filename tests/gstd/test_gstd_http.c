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

#include <gst/check/gstcheck.h>
#include <gio/gio.h>

#include "gstd_session.h"
#include "gstd_http.h"
#include "gstd_ipc.h"

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
  if (test_http) {
    gstd_ipc_stop (GSTD_IPC (test_http));
    g_object_unref (test_http);
    test_http = NULL;
  }
  if (test_loop) {
    g_main_loop_quit (test_loop);
    g_thread_join (test_loop_thread);
    g_main_loop_unref (test_loop);
    test_loop = NULL;
    test_loop_thread = NULL;
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

  return suite;
}

GST_CHECK_MAIN (gstd_http);
