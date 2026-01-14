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
}

static void
teardown (void)
{
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
 * Helper function to make HTTP request using GIO
 */
static gchar *
http_get (const gchar * path, guint * status_code)
{
  GSocketClient *client;
  GSocketConnection *conn;
  GInputStream *istream;
  GOutputStream *ostream;
  GError *error = NULL;
  gchar *request;
  gchar *response;
  gsize bytes_read;
  gchar buffer[4096];

  client = g_socket_client_new ();
  conn = g_socket_client_connect_to_host (client,
      TEST_HTTP_ADDRESS, TEST_HTTP_PORT, NULL, &error);

  if (!conn) {
    g_object_unref (client);
    *status_code = 0;
    return NULL;
  }

  ostream = g_io_stream_get_output_stream (G_IO_STREAM (conn));
  istream = g_io_stream_get_input_stream (G_IO_STREAM (conn));

  request = g_strdup_printf (
      "GET %s HTTP/1.1\r\n"
      "Host: %s:%d\r\n"
      "Connection: close\r\n"
      "\r\n",
      path, TEST_HTTP_ADDRESS, TEST_HTTP_PORT);

  g_output_stream_write_all (ostream, request, strlen (request), NULL, NULL, &error);
  g_free (request);

  if (error) {
    g_error_free (error);
    g_io_stream_close (G_IO_STREAM (conn), NULL, NULL);
    g_object_unref (conn);
    g_object_unref (client);
    *status_code = 0;
    return NULL;
  }

  bytes_read = g_input_stream_read (istream, buffer, sizeof (buffer) - 1, NULL, NULL);
  buffer[bytes_read] = '\0';

  /* Parse status code from HTTP response */
  if (sscanf (buffer, "HTTP/1.%*d %u", status_code) != 1) {
    *status_code = 0;
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
  fail_if (strstr (response, "ok") == NULL, "Health response should contain 'ok'");

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

  return suite;
}

GST_CHECK_MAIN (gstd_http);
