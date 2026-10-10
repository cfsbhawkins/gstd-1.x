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
 * Tests for the CPU utilization clamp a session holds while it has
 * pipelines (GSTD_PIPELINE_UTIL_CLAMP_MIN). Setting a clamp needs a
 * kernel with utilization clamping and, on most kernels, CAP_SYS_NICE;
 * where either is missing the tests check that pipelines still work
 * unclamped.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <gst/check/gstcheck.h>

#include "gstd_list.h"
#include "gstd_session.h"
#include "gstd_util_clamp.h"

#ifdef __linux__
#include <stdint.h>
#include <sys/syscall.h>
#include <unistd.h>

struct test_sched_attr
{
  uint32_t size;
  uint32_t sched_policy;
  uint64_t sched_flags;
  int32_t sched_nice;
  uint32_t sched_priority;
  uint64_t sched_runtime;
  uint64_t sched_deadline;
  uint64_t sched_period;
  uint32_t sched_util_min;
  uint32_t sched_util_max;
};

/* The minimum clamp of the calling thread, or -1 if it cannot be read */
static gint
current_util_min (void)
{
  struct test_sched_attr attr = { 0 };

  if (syscall (SYS_sched_getattr, 0, &attr, sizeof (attr), 0) != 0)
    return -1;
  return (gint) attr.sched_util_min;
}
#else
static gint
current_util_min (void)
{
  return -1;
}
#endif

static gpointer
read_util_min_thread (gpointer data)
{
  return GINT_TO_POINTER (current_util_min ());
}

/* The minimum clamp of a thread started now, from this one */
static gint
new_thread_util_min (void)
{
  GThread *thread = g_thread_new ("clamp-probe", read_util_min_thread, NULL);
  return GPOINTER_TO_INT (g_thread_join (thread));
}

/* Whether this process may clamp at all. Leaves every thread at the
 * kernel default afterwards. */
static gboolean
clamp_supported (void)
{
  guint updated = 0;

  if (gstd_util_clamp_raise (512, &updated) != 0 || updated == 0)
    return FALSE;
  fail_unless_equals_int (0, gstd_util_clamp_release (512, NULL));
  return TRUE;
}

static GstdObject *
pipelines_of (GstdSession * session)
{
  GstdObject *node = NULL;

  fail_if (gstd_get_by_uri (session, "/pipelines", &node));
  fail_if (NULL == node);
  return node;
}

static void
set_state (GstdSession * session, const gchar * pipeline, const gchar * state)
{
  GstdObject *node = NULL;
  gchar *uri = g_strdup_printf ("/pipelines/%s/state", pipeline);

  fail_if (gstd_get_by_uri (session, uri, &node));
  fail_if (GSTD_EOK != gstd_object_update (node, state));
  gst_object_unref (node);
  g_free (uri);
}

/* The clamp seen by the thread that streams into a fakesink */
static gint streaming_util_min = -2;
static GMutex streaming_mutex;
static GCond streaming_cond;

static gboolean
handoff_hook (GSignalInvocationHint * hint, guint n_values,
    const GValue * values, gpointer data)
{
  g_mutex_lock (&streaming_mutex);
  if (streaming_util_min == -2) {
    streaming_util_min = current_util_min ();
    g_cond_signal (&streaming_cond);
  }
  g_mutex_unlock (&streaming_mutex);
  return TRUE;
}

GST_START_TEST (test_util_clamp_parse)
{
  guint value = 0;

  fail_unless (gstd_util_clamp_parse ("1", &value));
  fail_unless_equals_int (1, value);
  fail_unless (gstd_util_clamp_parse ("512", &value));
  fail_unless_equals_int (512, value);
  fail_unless (gstd_util_clamp_parse ("1024", &value));
  fail_unless_equals_int (1024, value);

  value = 7;
  fail_if (gstd_util_clamp_parse (NULL, &value));
  fail_if (gstd_util_clamp_parse ("", &value));
  fail_if (gstd_util_clamp_parse ("0", &value));
  fail_if (gstd_util_clamp_parse ("1025", &value));
  fail_if (gstd_util_clamp_parse ("-1", &value));
  fail_if (gstd_util_clamp_parse ("+512", &value));
  fail_if (gstd_util_clamp_parse (" 512", &value));
  fail_if (gstd_util_clamp_parse ("512abc", &value));
  fail_if (gstd_util_clamp_parse ("99999999999999999999", &value));
  fail_unless_equals_int (7, value);
}

GST_END_TEST;

static void
count_notified (GObject * object, GParamSpec * pspec, gpointer data)
{
  guint *notifications = data;
  (*notifications)++;
}

GST_START_TEST (test_list_notifies_count)
{
  GstdObject *node;
  GstdReturnCode ret;
  guint notifications = 0;
  GstdSession *test_session = gstd_session_new ("Count_session");

  ret = gstd_get_by_uri (test_session, "/pipelines", &node);
  fail_if (ret);
  g_signal_connect (node, "notify::count", G_CALLBACK (count_notified),
      &notifications);

  ret = gstd_object_create (node, "p0", "fakesrc ! fakesink");
  fail_if (GSTD_EOK != ret);
  fail_unless_equals_int (1, notifications);

  /* A failed create changes nothing, so notifies nothing */
  ret = gstd_object_create (node, "bad", "nosuchelement_xyz ! fakesink");
  fail_if (GSTD_EOK == ret);
  fail_unless_equals_int (1, notifications);

  ret = gstd_object_delete (node, "p0");
  fail_if (GSTD_EOK != ret);
  fail_unless_equals_int (2, notifications);

  g_signal_handlers_disconnect_by_func (node, count_notified, &notifications);
  gst_object_unref (node);
  gst_object_unref (test_session);
}

GST_END_TEST;

GST_START_TEST (test_clamp_follows_pipelines)
{
  GstdObject *node;
  GstdReturnCode ret;
  GstdSession *test_session;
  gboolean supported;

  supported = clamp_supported ();

  g_setenv ("GSTD_PIPELINE_UTIL_CLAMP_MIN", "512", TRUE);
  test_session = gstd_session_new ("Clamp_session");
  ret = gstd_get_by_uri (test_session, "/pipelines", &node);
  fail_if (ret);

  ret = gstd_object_create (node, "p0", "fakesrc ! fakesink");
  fail_if (GSTD_EOK != ret);
  ret = gstd_object_create (node, "p1", "fakesrc ! fakesink");
  fail_if (GSTD_EOK != ret);

  if (supported) {
    fail_unless_equals_int (512, current_util_min ());
    /* Threads started after the pipeline exists inherit the clamp */
    fail_unless_equals_int (512, new_thread_util_min ());
  } else {
    GST_INFO ("Utilization clamping unavailable; checking pipelines only");
  }

  /* Still held while one pipeline remains */
  ret = gstd_object_delete (node, "p0");
  fail_if (GSTD_EOK != ret);
  if (supported)
    fail_unless_equals_int (512, current_util_min ());

  ret = gstd_object_delete (node, "p1");
  fail_if (GSTD_EOK != ret);
  if (supported) {
    fail_unless_equals_int (0, current_util_min ());
    fail_unless_equals_int (0, new_thread_util_min ());
  }

  /* And raised again for the next pipeline */
  ret = gstd_object_create (node, "p2", "fakesrc ! fakesink");
  fail_if (GSTD_EOK != ret);
  if (supported)
    fail_unless_equals_int (512, current_util_min ());

  gst_object_unref (node);
  gst_object_unref (test_session);

  /* Releasing the session releases the clamp */
  if (supported)
    fail_unless_equals_int (0, current_util_min ());
  g_unsetenv ("GSTD_PIPELINE_UTIL_CLAMP_MIN");
}

GST_END_TEST;

GST_START_TEST (test_clamp_off_by_default)
{
  GstdObject *node;
  GstdReturnCode ret;
  GstdSession *test_session;
  gint before = current_util_min ();

  test_session = gstd_session_new ("Unclamped_session");
  ret = gstd_get_by_uri (test_session, "/pipelines", &node);
  fail_if (ret);

  ret = gstd_object_create (node, "p0", "fakesrc ! fakesink");
  fail_if (GSTD_EOK != ret);
  fail_unless_equals_int (before, current_util_min ());

  gst_object_unref (node);
  gst_object_unref (test_session);
}

GST_END_TEST;

GST_START_TEST (test_clamp_invalid_value_ignored)
{
  GstdObject *node;
  GstdReturnCode ret;
  GstdSession *test_session;
  gint before = current_util_min ();

  g_setenv ("GSTD_PIPELINE_UTIL_CLAMP_MIN", "2048", TRUE);
  test_session = gstd_session_new ("Invalid_clamp_session");
  ret = gstd_get_by_uri (test_session, "/pipelines", &node);
  fail_if (ret);

  ret = gstd_object_create (node, "p0", "fakesrc ! fakesink");
  fail_if (GSTD_EOK != ret);
  fail_unless_equals_int (before, current_util_min ());

  gst_object_unref (node);
  gst_object_unref (test_session);
  g_unsetenv ("GSTD_PIPELINE_UTIL_CLAMP_MIN");
}

GST_END_TEST;

GST_START_TEST (test_clamp_reaches_streaming_thread)
{
  GstdObject *node;
  GstdSession *test_session;
  GType sink_type;
  guint signal_id;
  gulong hook;
  gint64 deadline;
  gint seen;
  gboolean supported = clamp_supported ();

  g_setenv ("GSTD_PIPELINE_UTIL_CLAMP_MIN", "512", TRUE);
  test_session = gstd_session_new ("Streaming_clamp_session");
  node = pipelines_of (test_session);

  fail_if (GSTD_EOK != gstd_object_create (node, "p0",
          "fakesrc is-live=true ! fakesink signal-handoffs=true"));
  sink_type = g_type_from_name ("GstFakeSink");
  fail_if (0 == sink_type);
  signal_id = g_signal_lookup ("handoff", sink_type);
  hook = g_signal_add_emission_hook (signal_id, 0, handoff_hook, NULL, NULL);

  streaming_util_min = -2;
  set_state (test_session, "p0", "playing");

  deadline = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;
  g_mutex_lock (&streaming_mutex);
  while (streaming_util_min == -2)
    if (!g_cond_wait_until (&streaming_cond, &streaming_mutex, deadline))
      break;
  seen = streaming_util_min;
  g_mutex_unlock (&streaming_mutex);

  set_state (test_session, "p0", "null");
  g_signal_remove_emission_hook (signal_id, hook);

  fail_if (seen == -2, "The pipeline never streamed");
  if (supported)
    fail_unless_equals_int (512, seen);

  fail_if (GSTD_EOK != gstd_object_delete (node, "p0"));
  gst_object_unref (node);
  gst_object_unref (test_session);
  g_unsetenv ("GSTD_PIPELINE_UTIL_CLAMP_MIN");
}

GST_END_TEST;

GST_START_TEST (test_clamp_keeps_higher_clamps)
{
  GstdObject *node;
  GstdSession *test_session;

  if (!clamp_supported ()) {
    GST_INFO ("Utilization clamping unavailable; nothing to preserve");
    return;
  }

  /* Something else asked for more than gstd will */
  fail_unless_equals_int (0, gstd_util_clamp_raise (1024, NULL));

  g_setenv ("GSTD_PIPELINE_UTIL_CLAMP_MIN", "512", TRUE);
  test_session = gstd_session_new ("Higher_clamp_session");
  node = pipelines_of (test_session);

  fail_if (GSTD_EOK != gstd_object_create (node, "p0", "fakesrc ! fakesink"));
  fail_unless_equals_int (1024, current_util_min ());

  /* Releasing gstd's clamp leaves the other one in place */
  fail_if (GSTD_EOK != gstd_object_delete (node, "p0"));
  fail_unless_equals_int (1024, current_util_min ());

  gst_object_unref (node);
  gst_object_unref (test_session);
  fail_unless_equals_int (1024, current_util_min ());

  fail_unless_equals_int (0, gstd_util_clamp_release (1024, NULL));
  g_unsetenv ("GSTD_PIPELINE_UTIL_CLAMP_MIN");
}

GST_END_TEST;

#define CHURN_THREADS 4
#define CHURN_ROUNDS 25

typedef struct
{
  GstdObject *pipelines;
  gint index;
} ChurnJob;

static gpointer
churn_thread (gpointer data)
{
  ChurnJob *job = data;
  gchar *name = g_strdup_printf ("churn%d", job->index);
  gint i;

  for (i = 0; i < CHURN_ROUNDS; i++) {
    fail_if (GSTD_EOK != gstd_object_create (job->pipelines, name,
            "fakesrc ! fakesink"));
    fail_if (GSTD_EOK != gstd_object_delete (job->pipelines, name));
  }

  g_free (name);
  return NULL;
}

GST_START_TEST (test_clamp_concurrent_churn_settles)
{
  GstdObject *node;
  GstdSession *test_session;
  GThread *threads[CHURN_THREADS];
  ChurnJob jobs[CHURN_THREADS];
  gboolean supported = clamp_supported ();
  gint i;

  g_setenv ("GSTD_PIPELINE_UTIL_CLAMP_MIN", "512", TRUE);
  test_session = gstd_session_new ("Churn_clamp_session");
  node = pipelines_of (test_session);

  for (i = 0; i < CHURN_THREADS; i++) {
    jobs[i].pipelines = node;
    jobs[i].index = i;
    threads[i] = g_thread_new ("churn", churn_thread, &jobs[i]);
  }
  for (i = 0; i < CHURN_THREADS; i++)
    g_thread_join (threads[i]);

  /* Every pipeline is gone, so the clamp must be too */
  if (supported) {
    fail_unless_equals_int (0, current_util_min ());
    fail_unless_equals_int (0, new_thread_util_min ());
  }

  fail_if (GSTD_EOK != gstd_object_create (node, "p0", "fakesrc ! fakesink"));
  if (supported)
    fail_unless_equals_int (512, current_util_min ());

  gst_object_unref (node);
  gst_object_unref (test_session);
  g_unsetenv ("GSTD_PIPELINE_UTIL_CLAMP_MIN");
}

GST_END_TEST;

static Suite *
gstd_util_clamp_suite (void)
{
  Suite *suite = suite_create ("gstd_util_clamp");
  TCase *tc = tcase_create ("general");

  /* gstd_session_init applies this env var; a developer shell exporting
   * it must not change what these tests observe */
  g_unsetenv ("GSTD_PIPELINE_UTIL_CLAMP_MIN");

  suite_add_tcase (suite, tc);
  tcase_set_timeout (tc, 30);

  tcase_add_test (tc, test_util_clamp_parse);
  tcase_add_test (tc, test_list_notifies_count);
  tcase_add_test (tc, test_clamp_follows_pipelines);
  tcase_add_test (tc, test_clamp_reaches_streaming_thread);
  tcase_add_test (tc, test_clamp_keeps_higher_clamps);
  tcase_add_test (tc, test_clamp_concurrent_churn_settles);
  tcase_add_test (tc, test_clamp_off_by_default);
  tcase_add_test (tc, test_clamp_invalid_value_ignored);

  return suite;
}

GST_CHECK_MAIN (gstd_util_clamp);
