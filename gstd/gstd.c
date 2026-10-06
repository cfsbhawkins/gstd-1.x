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

#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#include "gstd.h"
#include "gstd_daemon.h"
#include "gstd_log.h"

#define HEADER \
      "\nGstD version " PACKAGE_VERSION "\n" \
      "Copyright (C) 2015-2021 RidgeRun (https://www.ridgerun.com)\n\n"

/* Shutdown is driven by a dedicated thread that waits for SIGINT/SIGTERM
 * with sigwait(). A GLib signal source would need the main loop to dispatch
 * it, and the main loop is exactly what a stuck pipeline can block: a state
 * change that never returns holds a lock the main thread needs for its own
 * requests, and then a daemon that only quits from the main loop never quits.
 * The thread also bounds the shutdown itself, because stopping the IPC layer
 * waits for request handlers that may be stuck in the same state change. */
typedef struct
{
  GMainLoop *main_loop;
  guint timeout;
  sigset_t signals;
} GstdShutdown;

#define GSTD_SHUTDOWN_TIMEOUT_DEFAULT 5

static void print_header ();

static void
print_header (void)
{
  g_print (HEADER);
}

static gpointer
gstd_shutdown_thread (gpointer user_data)
{
  GstdShutdown *shutdown = (GstdShutdown *) user_data;
  gint signum = 0;

  if (sigwait (&shutdown->signals, &signum) != 0) {
    return NULL;
  }

  GST_INFO ("Signal %d received, shutting down...", signum);
  g_print ("\n");
  g_main_loop_quit (shutdown->main_loop);

  if (shutdown->timeout == 0) {
    return NULL;
  }

  g_usleep ((gulong) shutdown->timeout * G_USEC_PER_SEC);

  /* Still here: a pipeline teardown or request handler is not returning.
   * Nothing below us can be waited for any longer, so leave without
   * running the remaining cleanup. The log line is the only trace. */
  g_printerr ("Shutdown did not complete within %u s, exiting now\n",
      shutdown->timeout);
  GST_ERROR ("Shutdown did not complete within %u s, exiting now",
      shutdown->timeout);
  _exit (EXIT_FAILURE);
  return NULL;
}

/* Block the shutdown signals in the calling thread. Every thread created
 * afterwards inherits the mask, which leaves sigwait() in the shutdown
 * thread as their only receiver. Must run before any thread exists. */
static void
gstd_shutdown_block_signals (GstdShutdown * shutdown)
{
  sigemptyset (&shutdown->signals);
  sigaddset (&shutdown->signals, SIGINT);
  sigaddset (&shutdown->signals, SIGTERM);
  pthread_sigmask (SIG_BLOCK, &shutdown->signals, NULL);
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
  GstdShutdown shutdown = { NULL, 0, };
  GThread *shutdown_thread = NULL;

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
          "exiting regardless (default 5; 0 waits forever)",
        "seconds"}
    ,
    {NULL}
  };

  /* Shutdown signals go to the dedicated thread; see gstd_shutdown_thread.
   * This has to happen before gstd_new() creates the first thread. */
  gstd_shutdown_block_signals (&shutdown);

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
    if (gstd_daemon_stop ()) {
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

  /* Start IPC subsystem */
  if (!gstd_start (gstd)) {
    goto error;
  }

  /* Starting the application's main loop, necessary for 
     messaging and signaling subsystem */
  main_loop = g_main_loop_new (NULL, FALSE);

  /* Wait for SIGINT/SIGTERM off the main loop, and bound the shutdown */
  if (shutdown_timeout < 0) {
    g_printerr ("Ignoring invalid --shutdown-timeout %d\n", shutdown_timeout);
    shutdown_timeout = GSTD_SHUTDOWN_TIMEOUT_DEFAULT;
  }
  shutdown.main_loop = main_loop;
  shutdown.timeout = (guint) shutdown_timeout;
  shutdown_thread =
      g_thread_new ("gstd-shutdown", gstd_shutdown_thread, &shutdown);

  GST_INFO ("Gstd started");
  g_main_loop_run (main_loop);

  /* Application shut down. The shutdown thread is still sleeping towards
   * its deadline, or already gone; either way it must not be joined. */
  g_thread_unref (shutdown_thread);
  shutdown_thread = NULL;
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
    return ret;
  }
}
