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
 * Tests for the daemon's shutdown path in gstd.c: SIGINT/SIGTERM are
 * received off the main loop, pipelines are released before GStreamer is
 * deinitialized, and the shutdown is bounded by --shutdown-timeout. They
 * drive the real gstd binary over its TCP protocol.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <gio/gio.h>
#include <gst/check/gstcheck.h>

/* Away from the daemon's default and from test_gstd_http's port */
#define TEST_PORT_BASE 15100

/* How long a clean exit may take before the test gives up on it */
#define TEST_EXIT_LIMIT_MS 10000

/* A pipeline whose teardown blocks: identity's sleep-time is an
 * uninterruptible g_usleep() in the chain function, so PAUSED->READY waits
 * for it before the streaming thread can be stopped. */
#define STUCK_PIPELINE "videotestsrc ! identity sleep-time=60000000 ! fakesink"

static GPid
spawn_gstd (guint port, const gchar * shutdown_timeout)
{
  gchar port_str[16];
  gchar *argv[] = { (gchar *) GSTD_BINARY, (gchar *) "-q", (gchar *) "-p",
    port_str, (gchar *) "--shutdown-timeout", (gchar *) shutdown_timeout, NULL
  };
  GPid pid = 0;
  GError *error = NULL;

  g_snprintf (port_str, sizeof (port_str), "%u", port);
  fail_unless (g_spawn_async (NULL, argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD,
          NULL, NULL, &pid, &error), "spawning %s: %s", GSTD_BINARY,
      error ? error->message : "unknown error");

  return pid;
}

static GSocketConnection *
connect_gstd (guint port)
{
  GSocketClient *client = g_socket_client_new ();
  GSocketConnection *conn = NULL;
  gint attempt;

  for (attempt = 0; attempt < 100 && NULL == conn; attempt++) {
    conn = g_socket_client_connect_to_host (client, "127.0.0.1", port, NULL,
        NULL);
    if (NULL == conn) {
      g_usleep (100 * 1000);
    }
  }
  g_object_unref (client);

  fail_unless (conn != NULL, "gstd did not start listening on port %u", port);
  return conn;
}

/* Sends one command and checks that the NUL-terminated JSON reply is a
 * success */
static void
send_command (GSocketConnection * conn, const gchar * command)
{
  GOutputStream *out = g_io_stream_get_output_stream (G_IO_STREAM (conn));
  GInputStream *in = g_io_stream_get_input_stream (G_IO_STREAM (conn));
  GString *response = g_string_new (NULL);
  gchar *line = g_strconcat (command, "\n", NULL);
  gchar byte = 0;

  fail_unless (g_output_stream_write_all (out, line, strlen (line), NULL,
          NULL, NULL), "sending %s", command);
  g_free (line);

  while (g_input_stream_read (in, &byte, 1, NULL, NULL) == 1 && byte != '\0') {
    g_string_append_c (response, byte);
  }

  fail_unless (strstr (response->str, "\"code\" : 0") != NULL,
      "%s failed: %s", command, response->str);
  g_string_free (response, TRUE);
}

/* Creates and plays a pipeline, and gives it time to push a buffer */
static void
play_pipeline (guint port, const gchar * description)
{
  GSocketConnection *conn = connect_gstd (port);
  gchar *create = g_strdup_printf ("pipeline_create p %s", description);

  send_command (conn, create);
  send_command (conn, "pipeline_play p");
  g_free (create);

  g_io_stream_close (G_IO_STREAM (conn), NULL, NULL);
  g_object_unref (conn);

  g_usleep (300 * 1000);
}

/* Waits up to limit_ms for the process to exit. Returns FALSE if it is
 * still running, in which case it is killed so the test leaves nothing
 * behind. */
static gboolean
wait_exit (GPid pid, guint limit_ms, gint * status, gint64 * elapsed_ms)
{
  const gint64 start = g_get_monotonic_time ();
  pid_t reaped;

  for (;;) {
    reaped = waitpid (pid, status, WNOHANG);
    *elapsed_ms = (g_get_monotonic_time () - start) / 1000;

    if (reaped == pid) {
      return TRUE;
    }
    fail_unless (reaped == 0, "waitpid: %s", g_strerror (errno));

    if (*elapsed_ms > limit_ms) {
      kill (pid, SIGKILL);
      waitpid (pid, NULL, 0);
      return FALSE;
    }
    g_usleep (20 * 1000);
  }
}

static gboolean
is_running (GPid pid)
{
  return waitpid (pid, NULL, WNOHANG) == 0;
}

/*
 * SIGINT with nothing running: the dedicated receiver quits the loop and
 * the clean path exits 0.
 */
GST_START_TEST (test_sigint_idle_exits_clean)
{
  const guint port = TEST_PORT_BASE;
  GPid pid = spawn_gstd (port, "5");
  GSocketConnection *conn = connect_gstd (port);
  gint status = 0;
  gint64 elapsed_ms = 0;

  g_io_stream_close (G_IO_STREAM (conn), NULL, NULL);
  g_object_unref (conn);

  fail_unless (kill (pid, SIGINT) == 0);
  fail_unless (wait_exit (pid, TEST_EXIT_LIMIT_MS, &status, &elapsed_ms),
      "gstd did not exit on SIGINT");
  fail_unless (WIFEXITED (status) && WEXITSTATUS (status) == 0,
      "status 0x%x after %" G_GINT64_FORMAT " ms", status, elapsed_ms);
}

GST_END_TEST;

/*
 * SIGTERM with a PLAYING pipeline: the session is released before
 * gst_deinit(), so the streaming threads are stopped rather than waited
 * for forever, and the exit is clean and well inside the deadline.
 */
GST_START_TEST (test_sigterm_playing_pipeline_exits_clean)
{
  const guint port = TEST_PORT_BASE + 1;
  GPid pid = spawn_gstd (port, "5");
  gint status = 0;
  gint64 elapsed_ms = 0;

  play_pipeline (port, "videotestsrc ! fakesink");

  fail_unless (kill (pid, SIGTERM) == 0);
  fail_unless (wait_exit (pid, TEST_EXIT_LIMIT_MS, &status, &elapsed_ms),
      "gstd did not exit on SIGTERM with a playing pipeline");
  fail_unless (WIFEXITED (status) && WEXITSTATUS (status) == 0,
      "status 0x%x after %" G_GINT64_FORMAT " ms", status, elapsed_ms);
  fail_unless (elapsed_ms < 5000,
      "clean exit took %" G_GINT64_FORMAT " ms, the deadline must not be "
      "what ended it", elapsed_ms);
}

GST_END_TEST;

/*
 * A client that stays connected holds a TCP handler blocked in a read.
 * Stopping the socket cancels that read and waits for the handler, so the
 * exit is still clean and prompt, and the handler never touches the
 * session after it is gone.
 */
GST_START_TEST (test_sigterm_with_idle_client_exits_clean)
{
  const guint port = TEST_PORT_BASE + 4;
  GPid pid = spawn_gstd (port, "5");
  GSocketConnection *conn = connect_gstd (port);
  gint status = 0;
  gint64 elapsed_ms = 0;

  /* The handler for conn is now, or is about to be, blocked reading it */
  g_usleep (200 * 1000);

  fail_unless (kill (pid, SIGTERM) == 0);
  fail_unless (wait_exit (pid, TEST_EXIT_LIMIT_MS, &status, &elapsed_ms),
      "gstd did not exit on SIGTERM with a client connected");
  fail_unless (WIFEXITED (status) && WEXITSTATUS (status) == 0,
      "status 0x%x after %" G_GINT64_FORMAT " ms", status, elapsed_ms);
  fail_unless (elapsed_ms < 5000,
      "clean exit took %" G_GINT64_FORMAT " ms with an idle client",
      elapsed_ms);

  g_io_stream_close (G_IO_STREAM (conn), NULL, NULL);
  g_object_unref (conn);
}

GST_END_TEST;

/*
 * A teardown that never returns: after --shutdown-timeout the deadline
 * thread exits with status 1 instead of hanging.
 */
GST_START_TEST (test_stuck_teardown_exits_at_deadline)
{
  const guint port = TEST_PORT_BASE + 2;
  GPid pid = spawn_gstd (port, "1");
  gint status = 0;
  gint64 elapsed_ms = 0;

  play_pipeline (port, STUCK_PIPELINE);

  fail_unless (kill (pid, SIGTERM) == 0);
  fail_unless (wait_exit (pid, TEST_EXIT_LIMIT_MS, &status, &elapsed_ms),
      "gstd did not exit at the shutdown deadline");
  fail_unless (WIFEXITED (status) && WEXITSTATUS (status) == 1,
      "status 0x%x after %" G_GINT64_FORMAT " ms", status, elapsed_ms);
  fail_unless (elapsed_ms >= 1000 && elapsed_ms < 5000,
      "exit after %" G_GINT64_FORMAT " ms for a 1 s deadline", elapsed_ms);
}

GST_END_TEST;

/*
 * --shutdown-timeout 0 waits forever, but a second signal still ends it.
 */
GST_START_TEST (test_second_signal_forces_exit)
{
  const guint port = TEST_PORT_BASE + 3;
  GPid pid = spawn_gstd (port, "0");
  gint status = 0;
  gint64 elapsed_ms = 0;

  play_pipeline (port, STUCK_PIPELINE);

  fail_unless (kill (pid, SIGTERM) == 0);
  g_usleep (1500 * 1000);
  fail_unless (is_running (pid),
      "gstd exited on its own with the teardown stuck and no deadline");

  fail_unless (kill (pid, SIGTERM) == 0);
  fail_unless (wait_exit (pid, TEST_EXIT_LIMIT_MS, &status, &elapsed_ms),
      "gstd did not exit on the second SIGTERM");
  fail_unless (WIFEXITED (status) && WEXITSTATUS (status) == 1,
      "status 0x%x after %" G_GINT64_FORMAT " ms", status, elapsed_ms);
  fail_unless (elapsed_ms < 2000,
      "second signal took %" G_GINT64_FORMAT " ms to end the process",
      elapsed_ms);
}

GST_END_TEST;

static Suite *
gstd_shutdown_suite (void)
{
  Suite *suite = suite_create ("gstd_shutdown");
  TCase *tc = tcase_create ("general");

  suite_add_tcase (suite, tc);
  tcase_set_timeout (tc, 60);

  tcase_add_test (tc, test_sigint_idle_exits_clean);
  tcase_add_test (tc, test_sigterm_playing_pipeline_exits_clean);
  tcase_add_test (tc, test_sigterm_with_idle_client_exits_clean);
  tcase_add_test (tc, test_stuck_teardown_exits_at_deadline);
  tcase_add_test (tc, test_second_signal_forces_exit);

  return suite;
}

GST_CHECK_MAIN (gstd_shutdown);
