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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <glib-unix.h>

#include "gstd.h"
#include "gstd_daemon.h"
#include "gstd_log.h"

#define HEADER \
      "\nGstD version " PACKAGE_VERSION "\n" \
      "Copyright (C) 2015-2021 RidgeRun (https://www.ridgerun.com)\n\n"

/* Shutdown is driven by a dedicated thread rather than by the main loop. A
 * GLib signal source needs the main loop to dispatch it, and the main loop
 * is exactly what a stuck pipeline can block: a state change that never
 * returns holds a lock the main thread needs for its own requests, and then
 * a daemon that only quits from the main loop never quits.
 *
 * The signal handler writes the signal number to a pipe and the shutdown
 * thread blocks reading it (the self-pipe pattern g_unix_signal_add() uses
 * underneath, minus the main loop). The process signal mask is untouched,
 * so neither the threads GStreamer creates nor the children it spawns (the
 * plugin scanner, anything a pipeline executes) are affected.
 *
 * The first signal quits the main loop and, unless --shutdown-timeout is 0,
 * starts a deadline after which the process exits regardless, because
 * stopping the IPC layer waits for request handlers that may be stuck in
 * the same state change. A second signal exits at once. */
typedef struct
{
  GMainLoop *main_loop;
  guint timeout;                /* seconds; 0 waits forever */
  gint fds[2];                  /* the handler writes, the thread reads */

  GMutex lock;
  GCond cond;
  gboolean done;                /* main() finished the clean path */
  GThread *deadline;            /* started on the first signal */
} GstdShutdown;

#define GSTD_SHUTDOWN_TIMEOUT_DEFAULT 5

/* gstd -k waits this much longer than the daemon's own deadline, so a
 * daemon that needs the bounded exit is gone before -k gives up on it */
#define GSTD_KILL_WAIT_MARGIN 2
/* With --shutdown-timeout 0 the daemon waits forever; -k cannot, so a day */
#define GSTD_KILL_WAIT_FOREVER (24 * 60 * 60)

static GstdShutdown shutdown_state = { NULL, 0, {-1, -1}, };

static void print_header ();

static void
print_header (void)
{
  g_print (HEADER);
}

static void
gstd_shutdown_signal_handler (gint signum)
{
  const guint8 byte = (guint8) signum;
  const gint saved_errno = errno;
  G_GNUC_UNUSED gssize written;

  /* Only write() is async-signal-safe here. A full pipe means earlier
   * signals are still queued, which is the same outcome. */
  written = write (shutdown_state.fds[1], &byte, sizeof (byte));
  errno = saved_errno;
}

static gboolean
gstd_shutdown_wait_signal (gint * signum)
{
  guint8 byte = 0;
  gssize n;

  do {
    n = read (shutdown_state.fds[0], &byte, sizeof (byte));
  } while (n < 0 && errno == EINTR);

  if (n != sizeof (byte)) {
    return FALSE;
  }

  *signum = byte;
  return TRUE;
}

static gboolean
gstd_shutdown_quit (gpointer user_data)
{
  g_main_loop_quit ((GMainLoop *) user_data);
  return G_SOURCE_REMOVE;
}

static void
gstd_shutdown_exit_now (const gchar * reason)
{
  /* Nothing below us can be waited for any longer, so leave without
   * running the remaining cleanup. The log line is the only trace. */
  g_printerr ("%s, exiting now\n", reason);
  GST_ERROR ("%s, exiting now", reason);
  _exit (EXIT_FAILURE);
}

/* With nobody reading the pipe the handlers would swallow every signal.
 * The default action (terminate at once, no cleanup) keeps gstd stoppable. */
static void
gstd_shutdown_restore_default (const gchar * why)
{
  const gchar *reason = g_strerror (errno);

  g_printerr ("%s: %s. SIGINT/SIGTERM will now terminate gstd without "
      "cleanup\n", why, reason);
  GST_ERROR ("%s: %s. SIGINT/SIGTERM will now terminate gstd without "
      "cleanup", why, reason);
  signal (SIGINT, SIG_DFL);
  signal (SIGTERM, SIG_DFL);
}

static gpointer
gstd_shutdown_deadline_thread (gpointer user_data)
{
  const guint timeout = shutdown_state.timeout;
  const gint64 end =
      g_get_monotonic_time () + (gint64) timeout * G_USEC_PER_SEC;
  gboolean done;

  g_mutex_lock (&shutdown_state.lock);
  while (!shutdown_state.done
      && g_cond_wait_until (&shutdown_state.cond, &shutdown_state.lock, end)) {
    /* woken before the deadline; re-check */
  }
  done = shutdown_state.done;
  g_mutex_unlock (&shutdown_state.lock);

  if (!done) {
    gchar *reason =
        g_strdup_printf ("Shutdown did not complete within %u s", timeout);
    gstd_shutdown_exit_now (reason);
  }

  return NULL;
}

static gpointer
gstd_shutdown_thread (gpointer user_data)
{
  gint signum = 0;
  gboolean done;

  if (!gstd_shutdown_wait_signal (&signum)) {
    gstd_shutdown_restore_default ("Unable to wait for shutdown signals");
    return NULL;
  }

  GST_INFO ("Signal %d received, shutting down...", signum);
  g_print ("\n");

  /* Through an idle source rather than g_main_loop_quit() directly: a quit
   * that lands before g_main_loop_run() is discarded, and the signal may
   * have arrived before the loop was entered. The idle waits for it. */
  g_idle_add_full (G_PRIORITY_HIGH, gstd_shutdown_quit,
      g_main_loop_ref (shutdown_state.main_loop),
      (GDestroyNotify) g_main_loop_unref);

  g_mutex_lock (&shutdown_state.lock);
  if (shutdown_state.timeout > 0 && !shutdown_state.done) {
    shutdown_state.deadline =
        g_thread_new ("gstd-deadline", gstd_shutdown_deadline_thread, NULL);
  }
  g_mutex_unlock (&shutdown_state.lock);

  /* A second signal ends the wait, however long the timeout. Once the
   * clean path has finished the process is already exiting, and the signal
   * is ignored rather than turned into a failure. */
  while (gstd_shutdown_wait_signal (&signum)) {
    g_mutex_lock (&shutdown_state.lock);
    done = shutdown_state.done;
    g_mutex_unlock (&shutdown_state.lock);

    if (!done) {
      gstd_shutdown_exit_now ("Second signal received before shutdown "
          "completed");
    }
  }

  gstd_shutdown_restore_default ("Unable to wait for shutdown signals");
  return NULL;
}

/* Routes SIGINT/SIGTERM to the shutdown thread. Returns FALSE, with the
 * signal dispositions untouched, if the pipe or the thread cannot be made. */
static gboolean
gstd_shutdown_start (GMainLoop * main_loop, guint timeout)
{
  struct sigaction action;
  GError *error = NULL;
  GThread *thread;

  shutdown_state.main_loop = main_loop;
  shutdown_state.timeout = timeout;

  if (!g_unix_open_pipe (shutdown_state.fds, FD_CLOEXEC, &error)) {
    g_printerr ("Unable to create the shutdown pipe: %s\n", error->message);
    g_error_free (error);
    return FALSE;
  }

  thread = g_thread_try_new ("gstd-shutdown", gstd_shutdown_thread, NULL,
      &error);
  if (!thread) {
    g_printerr ("Unable to create the shutdown thread: %s\n", error->message);
    g_error_free (error);
    return FALSE;
  }
  /* Never joined: it blocks on the pipe until the process exits */
  g_thread_unref (thread);

  memset (&action, 0, sizeof (action));
  action.sa_handler = gstd_shutdown_signal_handler;
  sigemptyset (&action.sa_mask);
  action.sa_flags = SA_RESTART;
  sigaction (SIGINT, &action, NULL);
  sigaction (SIGTERM, &action, NULL);

  return TRUE;
}

/* Marks the clean path finished and reaps the deadline thread, so it cannot
 * wake after main() has returned and fail a shutdown that completed. */
static void
gstd_shutdown_finish (void)
{
  GThread *deadline;

  g_mutex_lock (&shutdown_state.lock);
  shutdown_state.done = TRUE;
  deadline = shutdown_state.deadline;
  shutdown_state.deadline = NULL;
  g_cond_broadcast (&shutdown_state.cond);
  g_mutex_unlock (&shutdown_state.lock);

  if (deadline) {
    g_thread_join (deadline);
  }
}

gint
main (gint argc, gchar * argv[])
{
  GMainLoop *main_loop;
  gboolean version = FALSE;
  gboolean kill = FALSE;
  gboolean daemon = FALSE;
  gboolean quiet = FALSE;
  const gchar *gstdlogfile = NULL;
  const gchar *gstlogfile = NULL;
  gchar *pidfile = NULL;
  GError *error = NULL;
  GOptionContext *context = NULL;
  gint ret = EXIT_SUCCESS;
  gchar *current_filename = NULL;
  gchar *filename = NULL;
  gboolean nolog = FALSE;
  gboolean parent = FALSE;
  gint max_pipelines = -1;      /* -1: option absent, keep env/default */
  gint shutdown_timeout = GSTD_SHUTDOWN_TIMEOUT_DEFAULT;
  gint kill_wait;

  GstD *gstd = NULL;

  GOptionEntry entries[] = {
    {"version", 'v', 0, G_OPTION_ARG_NONE, &version,
        "Print current gstd version and exit", NULL}
    ,
    {"kill", 'k', 0, G_OPTION_ARG_NONE, &kill,
        "Kill a running gstd, if any", NULL}
    ,
    {"quiet", 'q', 0, G_OPTION_ARG_NONE, &quiet,
        "Don't print any startup message", NULL}
    ,
    {"daemon", 'e', 0, G_OPTION_ARG_NONE, &daemon,
        "Detach into a daemon", NULL}
    ,
    {"pid-path", 'f', 0, G_OPTION_ARG_FILENAME, &pidfile,
        "Create gstd.pid file into path", NULL}
    ,
    {"gstd-log-filename", 'l', 0, G_OPTION_ARG_FILENAME, &gstdlogfile,
        "Create gstd.log file to path", NULL}
    ,
    {"gst-log-filename", 'd', 0, G_OPTION_ARG_FILENAME, &gstlogfile,
        "Create gst.log file to path", NULL}
    ,
    {"no-log", 'L', 0, G_OPTION_ARG_NONE, &nolog,
          "Disable file logging when gstd is running in daemon mode. Takes precedence over -l and -d.",
        NULL}
    ,
    {"max-pipelines", 0, 0, G_OPTION_ARG_INT, &max_pipelines,
          "Maximum number of simultaneous pipelines, as a resource-exhaustion "
          "guard. 0 means unlimited (default; overrides GSTD_MAX_PIPELINES, "
          "so an explicit 0 clears an environment-applied cap)",
        "count"}
    ,
    {"shutdown-timeout", 0, 0, G_OPTION_ARG_INT, &shutdown_timeout,
          "Seconds to wait for a clean shutdown after SIGINT/SIGTERM before "
          "exiting regardless (default 5; 0 waits forever). With -k, how "
          "long to wait for the daemon, plus 2",
        "seconds"}
    ,
    {NULL}
  };

  /* Initialize default */
  context = g_option_context_new (" - gst-launch under steroids");
  g_option_context_add_main_entries (context, entries, NULL);

  /* Initialize GStreamer */
  gstd_new (&gstd, 0, NULL);
  gstd_context_add_group (gstd, context);

  /* Parse the options before starting */
  if (!g_option_context_parse (context, &argc, &argv, &error)) {
    g_printerr ("%s\n", error->message);
    g_error_free (error);
    return EXIT_FAILURE;
  }
  g_option_context_free (context);

  /* Before daemonizing, so the message still reaches the terminal */
  if (shutdown_timeout < 0) {
    g_printerr ("Ignoring invalid --shutdown-timeout %d\n", shutdown_timeout);
    shutdown_timeout = GSTD_SHUTDOWN_TIMEOUT_DEFAULT;
  }

  if (!quiet && !kill) {
    print_header ();
  }

  if (version) {
    goto out;
  }

  /* If we need to daemonize or interact with the daemon (like killing
   * it, for example) we need to initialize the daemon subsystem.
   */
  if (daemon || kill) {

    /* Initialize the file logging only if:
     * - the user didn't explicitly request it by setting --no-log
     * - the user didn't invoke gstd to kill the daemon
     */
    if (!nolog && !kill) {
      if (!gstd_log_init (gstdlogfile, gstlogfile)) {
        ret = EXIT_FAILURE;
        goto out;
      }
    }

    if (!gstd_daemon_init (argc, argv, pidfile)) {
      ret = EXIT_FAILURE;
      goto out;
    }
  }

  gstd_debug_init ();

  if (kill) {
    if (shutdown_timeout == 0) {
      kill_wait = GSTD_KILL_WAIT_FOREVER;
    } else {
      kill_wait = MIN (shutdown_timeout, G_MAXINT - GSTD_KILL_WAIT_MARGIN)
          + GSTD_KILL_WAIT_MARGIN;
    }
    if (gstd_daemon_stop (kill_wait)) {
      GST_INFO ("Gstd successfully stopped");
    }
    goto out;
  }

  if (daemon) {
    if (!gstd_daemon_start (&parent)) {
      goto error;
    }

    /* Parent fork ends here */
    if (parent) {
      if (!quiet) {
        filename = gstd_log_get_current_gstd ();
        if (nolog) {
          g_print ("Log traces have been disabled.\n");
        } else {
          g_print ("Log traces will be saved to %s.\n", filename);
        }
        g_print ("Detaching from parent process.\n");
        g_free (filename);
      }
      goto out;
    }
  }

  /* 0 is meaningful: it clears a cap applied via GSTD_MAX_PIPELINES */
  if (max_pipelines >= 0) {
    gstd_set_max_pipelines (gstd, (guint) max_pipelines);
  } else if (max_pipelines < -1) {
    g_printerr ("Ignoring invalid --max-pipelines %d\n", max_pipelines);
  }

  /* Starting the application's main loop, necessary for 
     messaging and signaling subsystem */
  main_loop = g_main_loop_new (NULL, FALSE);

  /* Receive SIGINT/SIGTERM off the main loop, and bound the shutdown.
   * Before the IPC layer accepts connections, so a signal that arrives
   * while it starts is held for the loop instead of killing the daemon. */
  if (!gstd_shutdown_start (main_loop, (guint) shutdown_timeout)) {
    g_printerr ("Falling back to main-loop signal handling\n");
    g_unix_signal_add (SIGINT, gstd_shutdown_quit, main_loop);
    g_unix_signal_add (SIGTERM, gstd_shutdown_quit, main_loop);
  }

  /* Start IPC subsystem */
  if (!gstd_start (gstd)) {
    g_main_loop_unref (main_loop);
    main_loop = NULL;
    goto error;
  }

  GST_INFO ("Gstd started");
  g_main_loop_run (main_loop);

  /* Application shut down */
  g_main_loop_unref (main_loop);
  main_loop = NULL;

  /* Stop any IPC array */
  gstd_stop (gstd);

  gstd_log_deinit ();

  goto out;

error:
  {
    current_filename = gstd_log_get_current_gstd ();
    GST_ERROR ("Unable to start Gstd. Check %s for more details.",
        current_filename);
    g_free (current_filename);
    ret = EXIT_FAILURE;

  }
out:
  {
    /* Release the session, and with it every pipeline, before deinitializing
     * GStreamer: gst_deinit() waits for the task pool's threads, and a
     * pipeline that is still PLAYING never returns its streaming threads. */
    gstd_free (gstd);
    gst_deinit ();
    gstd_shutdown_finish ();
    return ret;
  }
}
