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

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>

#include "gstd_list.h"
#include "gstd_list_reader.h"
#include "gstd_pipeline_creator.h"
#include "gstd_pipeline_deleter.h"
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

/* The minimum clamp of thread \p tid, or -1 if it cannot be read */
static gint
util_min_of (gint tid)
{
#ifdef __linux__
  struct test_sched_attr attr = { 0 };

  if (syscall (SYS_sched_getattr, (pid_t) tid, &attr, sizeof (attr), 0) != 0)
    return -1;
  return (gint) attr.sched_util_min;
#else
  (void) tid;
  return -1;
#endif
}

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
  GstdUtilClamp *clamp = gstd_util_clamp_new (512);
  guint updated = 0;
  gboolean supported;

  supported = gstd_util_clamp_raise (clamp, &updated) == 0 && updated > 0;
  fail_unless_equals_int (0, gstd_util_clamp_release (clamp, NULL));
  gstd_util_clamp_free (clamp);
  return supported;
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
  GstdUtilClamp *other;

  if (!clamp_supported ()) {
    GST_INFO ("Utilization clamping unavailable; nothing to preserve");
    return;
  }

  /* Something else asked for more than gstd will */
  other = gstd_util_clamp_new (1024);
  fail_unless_equals_int (0, gstd_util_clamp_raise (other, NULL));

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

  fail_unless_equals_int (0, gstd_util_clamp_release (other, NULL));
  gstd_util_clamp_free (other);
  g_unsetenv ("GSTD_PIPELINE_UTIL_CLAMP_MIN");
}

GST_END_TEST;

/* A thread that switches itself to SCHED_FIFO on request, then waits */
typedef struct
{
  GMutex mutex;
  GCond cond;
  gint tid;
  gboolean promote;
  gint promoted;
  gboolean demote;
  gint demoted;
  gboolean finish;
} PromotedThread;

static gpointer
promoted_thread (gpointer data)
{
  PromotedThread *job = data;

  g_mutex_lock (&job->mutex);
#ifdef __linux__
  job->tid = (gint) syscall (SYS_gettid);
#endif
  g_cond_broadcast (&job->cond);
  while (!job->promote)
    g_cond_wait (&job->cond, &job->mutex);
  {
    struct sched_param param = { 0 };

    param.sched_priority = sched_get_priority_min (SCHED_FIFO);
    job->promoted = pthread_setschedparam (pthread_self (), SCHED_FIFO,
        &param) == 0 ? 1 : -1;
  }
  g_cond_broadcast (&job->cond);
  while (!job->demote)
    g_cond_wait (&job->cond, &job->mutex);
  {
    struct sched_param param = { 0 };

    job->demoted = pthread_setschedparam (pthread_self (), SCHED_OTHER,
        &param) == 0 ? 1 : -1;
  }
  g_cond_broadcast (&job->cond);
  while (!job->finish)
    g_cond_wait (&job->cond, &job->mutex);
  g_mutex_unlock (&job->mutex);
  return NULL;
}

/* On a real kernel: a thread raised under normal scheduling that switches
 * to SCHED_FIFO keeps the clamp; the release returns it to the kernel's
 * real-time default, and to 0 once it is back on SCHED_OTHER */
GST_START_TEST (test_clamp_released_after_promotion)
{
  GstdObject *node;
  GstdSession *test_session;
  PromotedThread job = { 0 };
  GThread *thread;
  gint rt_default = 1024;
  gchar *contents = NULL;

  if (!clamp_supported ()) {
    GST_INFO ("Utilization clamping unavailable; nothing to release");
    return;
  }
  if (g_file_get_contents ("/proc/sys/kernel/sched_util_clamp_min_rt_default",
          &contents, NULL, NULL))
    rt_default = atoi (contents);
  g_free (contents);

  g_mutex_init (&job.mutex);
  g_cond_init (&job.cond);
  thread = g_thread_new ("promoted", promoted_thread, &job);
  g_mutex_lock (&job.mutex);
  while (job.tid == 0)
    g_cond_wait (&job.cond, &job.mutex);
  g_mutex_unlock (&job.mutex);

  g_setenv ("GSTD_PIPELINE_UTIL_CLAMP_MIN", "512", TRUE);
  test_session = gstd_session_new ("Promoted_clamp_session");
  node = pipelines_of (test_session);
  fail_if (GSTD_EOK != gstd_object_create (node, "p0", "fakesrc ! fakesink"));
  fail_unless_equals_int (512, util_min_of (job.tid));

  g_mutex_lock (&job.mutex);
  job.promote = TRUE;
  g_cond_broadcast (&job.cond);
  while (job.promoted == 0)
    g_cond_wait (&job.cond, &job.mutex);
  g_mutex_unlock (&job.mutex);

  if (job.promoted > 0) {
    /* Promotion keeps a user-defined clamp */
    fail_unless_equals_int (512, util_min_of (job.tid));
    fail_if (GSTD_EOK != gstd_object_delete (node, "p0"));
    fail_unless_equals_int (rt_default, util_min_of (job.tid));

    /* Released, not overwritten: back on SCHED_OTHER it has no floor */
    g_mutex_lock (&job.mutex);
    job.demote = TRUE;
    g_cond_broadcast (&job.cond);
    while (job.demoted == 0)
      g_cond_wait (&job.cond, &job.mutex);
    g_mutex_unlock (&job.mutex);
    fail_unless_equals_int (1, job.demoted);
    fail_unless_equals_int (0, util_min_of (job.tid));
  } else {
    GST_INFO ("Cannot switch to SCHED_FIFO here; promotion not tested");
    fail_if (GSTD_EOK != gstd_object_delete (node, "p0"));
  }

  g_mutex_lock (&job.mutex);
  job.demote = TRUE;
  job.finish = TRUE;
  g_cond_broadcast (&job.cond);
  g_mutex_unlock (&job.mutex);
  g_thread_join (thread);
  g_mutex_clear (&job.mutex);
  g_cond_clear (&job.cond);

  gst_object_unref (node);
  gst_object_unref (test_session);
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

/*
 * A fake kernel. Every thread starts as a normal thread with no clamp;
 * tests mark threads real-time or capped, and make set_min fail for one
 * thread or for everyone. GSTD_UTIL_CLAMP_RESET is recorded as 0.
 */
#ifdef __linux__
#define FAKE_ANY_TID -1
#define FAKE_SCHED_FIFO 1

static GHashTable *fake_threads = NULL;
static GMutex fake_mutex;
typedef struct
{
  gint tid;
  guint value;
  gint error;
  gint times;
} FakeFault;

/* Up to two armed faults, matched in order */
static FakeFault fake_faults[2];
static gboolean fake_no_reset = FALSE;
/* set_min reports success without changing anything */
static gboolean fake_forget = FALSE;
static guint fake_rt_default_value = GSTD_UTIL_CLAMP_SCALE;
static guint fake_set_calls = 0;

#define FAKE_SCHED_OTHER 0

/* A thread as the kernel keeps it: the requested clamp, and whether it
 * was set by hand (user-defined) or is the default for the policy */
typedef struct
{
  GstdUtilClampThread state;
  gboolean user_defined;
  /* Changes when the tid is given to a new thread */
  guint64 start;
} FakeThread;

static FakeThread *
fake_entry (gint tid)
{
  FakeThread *thread =
      g_hash_table_lookup (fake_threads, GINT_TO_POINTER (tid));

  if (!thread) {
    thread = g_new0 (FakeThread, 1);
    thread->state.util_max = GSTD_UTIL_CLAMP_SCALE;
    g_hash_table_insert (fake_threads, GINT_TO_POINTER (tid), thread);
  }
  return thread;
}

static GstdUtilClampThread *
fake_thread (gint tid)
{
  return &fake_entry (tid)->state;
}

/* What sched_setscheduler() does to the clamp: a user-defined one is kept
 * across the change, a default one becomes the new policy's default */
static void
fake_set_policy (gint tid, guint policy)
{
  FakeThread *thread;

  g_mutex_lock (&fake_mutex);
  thread = fake_entry (tid);
  thread->state.policy = policy;
  if (!thread->user_defined)
    thread->state.util_min = policy == FAKE_SCHED_FIFO
        ? fake_rt_default_value : 0;
  g_mutex_unlock (&fake_mutex);
}

static gint
fake_get (gint tid, GstdUtilClampThread * out)
{
  g_mutex_lock (&fake_mutex);
  *out = *fake_thread (tid);
  g_mutex_unlock (&fake_mutex);
  return 0;
}

/* Fails when the tid and the value match an armed fault, a set number of
 * times (-1: always). A value of 0 in a fault matches any value. */
static gint
fake_set_min (gint tid, guint util_min)
{
  gint error = 0;
  guint i;

  g_mutex_lock (&fake_mutex);
  fake_set_calls++;
  for (i = 0; i < G_N_ELEMENTS (fake_faults) && error == 0; i++) {
    FakeFault *fault = &fake_faults[i];

    if (fault->error != 0 && fault->times != 0
        && (fault->tid == FAKE_ANY_TID || fault->tid == tid)
        && (fault->value == 0 || fault->value == util_min)) {
      error = fault->error;
      if (fault->times > 0)
        fault->times--;
    }
  }
  if (error == 0 && util_min == GSTD_UTIL_CLAMP_RESET && fake_no_reset)
    error = EINVAL;
  if (error == 0 && !fake_forget) {
    FakeThread *thread = fake_entry (tid);

    /* A written value is user-defined; the reset clears that and gives
     * the policy's default */
    thread->user_defined = util_min != GSTD_UTIL_CLAMP_RESET;
    if (thread->user_defined)
      thread->state.util_min = util_min;
    else
      thread->state.util_min = thread->state.policy == FAKE_SCHED_FIFO
          ? fake_rt_default_value : 0;
  }
  g_mutex_unlock (&fake_mutex);
  return error;
}

static gint
fake_start_time (gint tid, guint64 * start)
{
  g_mutex_lock (&fake_mutex);
  *start = fake_entry (tid)->start;
  g_mutex_unlock (&fake_mutex);
  return 0;
}

static guint
fake_rt_default (void)
{
  return fake_rt_default_value;
}

static const GstdUtilClampBackend fake_backend = {
  fake_get, fake_set_min, fake_start_time, fake_rt_default
};

static gint
own_tid (void)
{
  return (gint) syscall (SYS_gettid);
}

static guint
fake_util_min (gint tid)
{
  guint value;

  g_mutex_lock (&fake_mutex);
  value = fake_thread (tid)->util_min;
  g_mutex_unlock (&fake_mutex);
  return value;
}

/* A clamp set by something other than gstd */
static void
fake_set_util_min (gint tid, guint util_min)
{
  FakeThread *thread;

  g_mutex_lock (&fake_mutex);
  thread = fake_entry (tid);
  thread->state.util_min = util_min;
  thread->user_defined = TRUE;
  g_mutex_unlock (&fake_mutex);
}

/* The thread exits and the kernel gives its tid to a new thread, which
 * inherits \p util_min from the thread that created it */
static void
fake_reuse_tid (gint tid, guint util_min)
{
  FakeThread *thread;

  g_mutex_lock (&fake_mutex);
  thread = fake_entry (tid);
  thread->start++;
  thread->state.util_min = util_min;
  thread->user_defined = TRUE;
  g_mutex_unlock (&fake_mutex);
}

static gboolean
fake_user_defined (gint tid)
{
  gboolean user_defined;

  g_mutex_lock (&fake_mutex);
  user_defined = fake_entry (tid)->user_defined;
  g_mutex_unlock (&fake_mutex);
  return user_defined;
}

static void
fake_arm_slot (guint slot, gint tid, guint value, gint error, gint times)
{
  g_mutex_lock (&fake_mutex);
  fake_faults[slot].tid = tid;
  fake_faults[slot].value = value;
  fake_faults[slot].error = error;
  fake_faults[slot].times = times;
  g_mutex_unlock (&fake_mutex);
}

/* Arms one fault and clears the other; all zeros clears both */
static void
fake_arm (gint tid, guint value, gint error, gint times)
{
  fake_arm_slot (0, tid, value, error, times);
  fake_arm_slot (1, 0, 0, 0, 0);
}

/* A second thread that stays alive, and listed, for the whole test */
static GMutex parked_mutex;
static GCond parked_cond;
static gboolean parked_release = FALSE;
static gint parked_tid = 0;

static gpointer
parked_thread (gpointer data)
{
  g_mutex_lock (&parked_mutex);
  parked_tid = own_tid ();
  g_cond_broadcast (&parked_cond);
  while (!parked_release)
    g_cond_wait (&parked_cond, &parked_mutex);
  g_mutex_unlock (&parked_mutex);
  return NULL;
}

static GThread *parked = NULL;

static void
fake_setup (void)
{
  fake_threads = g_hash_table_new_full (NULL, NULL, NULL, g_free);
  fake_arm (0, 0, 0, 0);
  fake_no_reset = FALSE;
  fake_forget = FALSE;
  fake_rt_default_value = GSTD_UTIL_CLAMP_SCALE;
  fake_set_calls = 0;
  gstd_util_clamp_set_backend (&fake_backend);

  parked_release = FALSE;
  parked_tid = 0;
  parked = g_thread_new ("parked", parked_thread, NULL);
  g_mutex_lock (&parked_mutex);
  while (parked_tid == 0)
    g_cond_wait (&parked_cond, &parked_mutex);
  g_mutex_unlock (&parked_mutex);

  g_setenv ("GSTD_PIPELINE_UTIL_CLAMP_MIN", "512", TRUE);
}

static void
fake_teardown (void)
{
  g_mutex_lock (&parked_mutex);
  parked_release = TRUE;
  g_cond_broadcast (&parked_cond);
  g_mutex_unlock (&parked_mutex);
  g_thread_join (parked);

  gstd_util_clamp_set_backend (NULL);
  g_hash_table_unref (fake_threads);
  fake_threads = NULL;
  g_unsetenv ("GSTD_PIPELINE_UTIL_CLAMP_MIN");
}

GST_START_TEST (test_fake_partial_raise_retried)
{
  GstdSession *test_session = gstd_session_new ("Fake_partial_raise_session");
  GstdObject *node = pipelines_of (test_session);

  /* This thread refuses the raise; the parked one takes it */
  fake_arm (own_tid (), 512, EBUSY, -1);
  fail_if (GSTD_EOK != gstd_object_create (node, "p0", "fakesrc ! fakesink"));
  fail_unless_equals_int (512, fake_util_min (parked_tid));
  fail_unless_equals_int (0, fake_util_min (own_tid ()));

  /* The next pipeline finishes the raise */
  fake_arm (0, 0, 0, 0);
  fail_if (GSTD_EOK != gstd_object_create (node, "p1", "fakesrc ! fakesink"));
  fail_unless_equals_int (512, fake_util_min (own_tid ()));

  fail_if (GSTD_EOK != gstd_object_delete (node, "p0"));
  fail_if (GSTD_EOK != gstd_object_delete (node, "p1"));
  fail_unless_equals_int (0, fake_util_min (own_tid ()));
  fail_unless_equals_int (0, fake_util_min (parked_tid));

  gst_object_unref (node);
  gst_object_unref (test_session);
}

GST_END_TEST;

GST_START_TEST (test_fake_partial_release_retried)
{
  GstdSession *test_session = gstd_session_new ("Fake_partial_session");
  GstdObject *node = pipelines_of (test_session);

  fail_if (GSTD_EOK != gstd_object_create (node, "p0", "fakesrc ! fakesink"));
  fail_unless_equals_int (512, fake_util_min (parked_tid));
  fail_unless_equals_int (512, fake_util_min (own_tid ()));

  /* This thread refuses the release; the parked one has already gone */
  fake_arm (own_tid (), GSTD_UTIL_CLAMP_RESET, EBUSY, -1);
  fail_if (GSTD_EOK != gstd_object_delete (node, "p0"));
  fail_unless_equals_int (512, fake_util_min (own_tid ()));
  fail_unless_equals_int (0, fake_util_min (parked_tid));

  /* The next pipeline raises the floor again where it was released */
  fail_if (GSTD_EOK != gstd_object_create (node, "p1", "fakesrc ! fakesink"));
  fail_unless_equals_int (512, fake_util_min (parked_tid));

  /* And the retry at the next empty list releases everything */
  fake_arm (0, 0, 0, 0);
  fail_if (GSTD_EOK != gstd_object_delete (node, "p1"));
  fail_unless_equals_int (0, fake_util_min (own_tid ()));
  fail_unless_equals_int (0, fake_util_min (parked_tid));

  gst_object_unref (node);
  gst_object_unref (test_session);
}

GST_END_TEST;

GST_START_TEST (test_fake_dispose_after_failed_release)
{
  GstdSession *test_session = gstd_session_new ("Fake_failed_release_session");
  GstdObject *node = pipelines_of (test_session);

  fail_if (GSTD_EOK != gstd_object_create (node, "p0", "fakesrc ! fakesink"));

  /* The release reaches the parked thread but not this one */
  fake_arm (own_tid (), GSTD_UTIL_CLAMP_RESET, EBUSY, -1);
  fail_if (GSTD_EOK != gstd_object_delete (node, "p0"));
  fail_unless_equals_int (512, fake_util_min (own_tid ()));
  fail_unless_equals_int (0, fake_util_min (parked_tid));

  /* Disposing straight away must still take the floor down */
  fake_arm (0, 0, 0, 0);
  gst_object_unref (node);
  gst_object_unref (test_session);
  fail_unless_equals_int (0, fake_util_min (own_tid ()));
}

GST_END_TEST;

GST_START_TEST (test_fake_equal_clamp_preserved)
{
  GstdSession *test_session = gstd_session_new ("Fake_equal_session");
  GstdObject *node = pipelines_of (test_session);

  /* Something else already holds the parked thread at gstd's value */
  fake_set_util_min (parked_tid, 512);

  fail_if (GSTD_EOK != gstd_object_create (node, "p0", "fakesrc ! fakesink"));
  fail_unless_equals_int (512, fake_util_min (own_tid ()));

  fail_if (GSTD_EOK != gstd_object_delete (node, "p0"));
  fail_unless_equals_int (0, fake_util_min (own_tid ()));
  fail_unless_equals_int (512, fake_util_min (parked_tid));

  /* Also after another round */
  fail_if (GSTD_EOK != gstd_object_create (node, "p1", "fakesrc ! fakesink"));
  gst_object_unref (node);
  gst_object_unref (test_session);
  fail_unless_equals_int (0, fake_util_min (own_tid ()));
  fail_unless_equals_int (512, fake_util_min (parked_tid));
}

GST_END_TEST;

GST_START_TEST (test_fake_reused_tid_released)
{
  GstdSession *test_session = gstd_session_new ("Fake_reused_session");
  GstdObject *node = pipelines_of (test_session);

  fake_set_util_min (parked_tid, 512);
  fail_if (GSTD_EOK != gstd_object_create (node, "p0", "fakesrc ! fakesink"));

  /* The preserved thread exits; a thread gstd's clamp reached takes its
   * tid */
  fake_reuse_tid (parked_tid, 512);
  fail_if (GSTD_EOK != gstd_object_delete (node, "p0"));
  fail_unless_equals_int (0, fake_util_min (parked_tid));

  gst_object_unref (node);
  gst_object_unref (test_session);
}

GST_END_TEST;

GST_START_TEST (test_fake_unsettled_scan_fails)
{
  GstdUtilClamp *clamp = gstd_util_clamp_new (512);

  /* Every pass changes something, so no pass is clean */
  fake_forget = TRUE;
  fail_unless_equals_int (EAGAIN, gstd_util_clamp_raise (clamp, NULL));
  fail_unless (gstd_util_clamp_is_held (clamp));

  fake_forget = FALSE;
  fail_unless_equals_int (0, gstd_util_clamp_release (clamp, NULL));
  fail_if (gstd_util_clamp_is_held (clamp));
  gstd_util_clamp_free (clamp);
}

GST_END_TEST;

/* A list set up the way the session sets up its own */
static GstdList *
new_pipeline_list (void)
{
  GstdList *list =
      GSTD_LIST (g_object_new (GSTD_TYPE_LIST, "name", "pipelines",
          "node-type", GSTD_TYPE_PIPELINE, "flags",
          GSTD_PARAM_CREATE | GSTD_PARAM_READ | GSTD_PARAM_UPDATE |
          GSTD_PARAM_DELETE, NULL));

  gstd_object_set_creator (GSTD_OBJECT (list),
      g_object_new (GSTD_TYPE_PIPELINE_CREATOR, NULL));
  gstd_object_set_reader (GSTD_OBJECT (list),
      g_object_new (GSTD_TYPE_LIST_READER, NULL));
  gstd_object_set_deleter (GSTD_OBJECT (list),
      g_object_new (GSTD_TYPE_PIPELINE_DELETER, NULL));
  return list;
}

GST_START_TEST (test_fake_replaced_list_followed)
{
  GstdSession *test_session = gstd_session_new ("Fake_replaced_session");
  GstdObject *old_list = pipelines_of (test_session);
  GstdList *list = new_pipeline_list ();

  /* The new list already has a pipeline: replacing raises the floor */
  fail_if (GSTD_EOK != gstd_object_create (GSTD_OBJECT (list), "p0",
          "fakesrc ! fakesink"));
  g_object_set (test_session, "pipelines", list, NULL);
  fail_unless_equals_int (512, fake_util_min (parked_tid));

  /* The old list no longer counts */
  fail_if (GSTD_EOK != gstd_object_create (old_list, "old",
          "fakesrc ! fakesink"));
  fail_if (GSTD_EOK != gstd_object_delete (GSTD_OBJECT (list), "p0"));
  fail_unless_equals_int (0, fake_util_min (parked_tid));

  fail_if (GSTD_EOK != gstd_object_create (GSTD_OBJECT (list), "p1",
          "fakesrc ! fakesink"));
  fail_unless_equals_int (512, fake_util_min (parked_tid));
  fail_if (GSTD_EOK != gstd_object_delete (GSTD_OBJECT (list), "p1"));
  fail_unless_equals_int (0, fake_util_min (parked_tid));

  gst_object_unref (list);
  gst_object_unref (old_list);
  gst_object_unref (test_session);
}

GST_END_TEST;

GST_START_TEST (test_fake_realtime_threads_untouched)
{
  GstdSession *test_session;
  GstdObject *node;
  guint calls;

  /* A real-time thread under the kernel's default boost, and a kernel
   * without the reset value (before 5.11) */
  fake_set_policy (own_tid (), FAKE_SCHED_FIFO);
  fake_no_reset = TRUE;
  g_setenv ("GSTD_PIPELINE_UTIL_CLAMP_MIN", "1024", TRUE);

  test_session = gstd_session_new ("Fake_rt_session");
  node = pipelines_of (test_session);

  fail_if (GSTD_EOK != gstd_object_create (node, "p0", "fakesrc ! fakesink"));
  fail_unless_equals_int (1024, fake_util_min (parked_tid));

  calls = fake_set_calls;
  fail_if (GSTD_EOK != gstd_object_delete (node, "p0"));
  /* The normal thread falls back to 0; the real-time one is never written */
  fail_unless_equals_int (0, fake_util_min (parked_tid));
  fail_unless_equals_int (1024, fake_util_min (own_tid ()));
  fail_unless (fake_set_calls > calls);

  gst_object_unref (node);
  gst_object_unref (test_session);
  fail_unless_equals_int (1024, fake_util_min (own_tid ()));
  /* Still the kernel's boost, not a pinned value it would keep after
   * leaving real-time */
  fail_if (fake_user_defined (own_tid ()));
}

GST_END_TEST;

/* A thread raised under normal scheduling that then switches to a
 * real-time policy keeps gstd's value. The release must take it back, so
 * that the thread drops to 0 when it leaves real-time again; before 5.11
 * the explicit write keeps the real-time default instead. */
static void
check_promoted_thread_released (const gchar * value, guint rt_default,
    gboolean no_reset, guint after_demotion)
{
  GstdSession *test_session;
  GstdObject *node;
  guint raised = (guint) atoi (value);

  fake_no_reset = no_reset;
  fake_rt_default_value = rt_default;
  g_setenv ("GSTD_PIPELINE_UTIL_CLAMP_MIN", value, TRUE);
  test_session = gstd_session_new ("Fake_promoted_session");
  node = pipelines_of (test_session);

  fail_if (GSTD_EOK != gstd_object_create (node, "p0", "fakesrc ! fakesink"));
  fail_unless_equals_int (raised, fake_util_min (parked_tid));

  /* The policy changes; the user-defined clamp stays */
  fake_set_policy (parked_tid, FAKE_SCHED_FIFO);
  fail_unless_equals_int (raised, fake_util_min (parked_tid));

  fail_if (GSTD_EOK != gstd_object_delete (node, "p0"));
  fail_unless_equals_int (rt_default, fake_util_min (parked_tid));
  fail_unless_equals_int (0, fake_util_min (own_tid ()));

  fake_set_policy (parked_tid, FAKE_SCHED_OTHER);
  fail_unless_equals_int (after_demotion, fake_util_min (parked_tid));

  gst_object_unref (node);
  gst_object_unref (test_session);
}

GST_START_TEST (test_fake_promoted_thread_released)
{
  check_promoted_thread_released ("512", 1024, FALSE, 0);
}

GST_END_TEST;

GST_START_TEST (test_fake_promoted_thread_released_without_reset)
{
  /* Without the reset the number is written, and outlives the policy */
  check_promoted_thread_released ("512", 1024, TRUE, 1024);
}

GST_END_TEST;

GST_START_TEST (test_fake_promoted_thread_released_at_rt_default)
{
  /* gstd's value is the real-time default: indistinguishable while the
   * thread is real-time, but it must still lose the floor when it leaves */
  check_promoted_thread_released ("1024", 1024, FALSE, 0);
}

GST_END_TEST;

GST_START_TEST (test_fake_capped_thread_skipped)
{
  GstdSession *test_session;
  GstdObject *node;

  /* Its maximum clamp is below the floor; asking would be EINVAL */
  g_mutex_lock (&fake_mutex);
  fake_thread (own_tid ())->util_max = 256;
  g_mutex_unlock (&fake_mutex);

  test_session = gstd_session_new ("Fake_capped_session");
  node = pipelines_of (test_session);

  fail_if (GSTD_EOK != gstd_object_create (node, "p0", "fakesrc ! fakesink"));
  fail_unless_equals_int (0, fake_util_min (own_tid ()));
  fail_unless_equals_int (512, fake_util_min (parked_tid));

  fail_if (GSTD_EOK != gstd_object_delete (node, "p0"));
  fail_unless_equals_int (0, fake_util_min (parked_tid));

  gst_object_unref (node);
  gst_object_unref (test_session);
}

GST_END_TEST;

GST_START_TEST (test_fake_transient_error_retried)
{
  GstdSession *test_session = gstd_session_new ("Fake_transient_session");
  GstdObject *node = pipelines_of (test_session);

  fake_arm (FAKE_ANY_TID, 0, EINVAL, -1);
  fail_if (GSTD_EOK != gstd_object_create (node, "p0", "fakesrc ! fakesink"));
  fail_unless_equals_int (0, fake_util_min (parked_tid));
  fail_if (GSTD_EOK != gstd_object_delete (node, "p0"));

  /* Not "unsupported": the next pipeline tries again */
  fake_arm (0, 0, 0, 0);
  fail_if (GSTD_EOK != gstd_object_create (node, "p1", "fakesrc ! fakesink"));
  fail_unless_equals_int (512, fake_util_min (parked_tid));

  gst_object_unref (node);
  gst_object_unref (test_session);
}

GST_END_TEST;

GST_START_TEST (test_fake_unsupported_kernel_disables)
{
  GstdSession *test_session = gstd_session_new ("Fake_unsupported_session");
  GstdObject *node = pipelines_of (test_session);
  guint calls;

  fake_arm (FAKE_ANY_TID, 0, EOPNOTSUPP, -1);
  fail_if (GSTD_EOK != gstd_object_create (node, "p0", "fakesrc ! fakesink"));
  fail_if (GSTD_EOK != gstd_object_delete (node, "p0"));

  /* Never asked again for the life of the session */
  fake_arm (0, 0, 0, 0);
  calls = fake_set_calls;
  fail_if (GSTD_EOK != gstd_object_create (node, "p1", "fakesrc ! fakesink"));
  fail_unless_equals_int (calls, fake_set_calls);
  fail_unless_equals_int (0, fake_util_min (parked_tid));

  gst_object_unref (node);
  gst_object_unref (test_session);
}

GST_END_TEST;

GST_START_TEST (test_fake_dispose_retries_release)
{
  GstdSession *test_session = gstd_session_new ("Fake_dispose_session");
  GstdObject *node = pipelines_of (test_session);

  fail_if (GSTD_EOK != gstd_object_create (node, "p0", "fakesrc ! fakesink"));
  gst_object_unref (node);

  /* The first release attempt fails on this thread only */
  fake_arm (own_tid (), GSTD_UTIL_CLAMP_RESET, EBUSY, 1);
  gst_object_unref (test_session);
  fail_unless_equals_int (0, fake_util_min (own_tid ()));
  fail_unless_equals_int (0, fake_util_min (parked_tid));
}

GST_END_TEST;
#endif

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
  tcase_add_test (tc, test_clamp_released_after_promotion);
  tcase_add_test (tc, test_clamp_off_by_default);
  tcase_add_test (tc, test_clamp_invalid_value_ignored);

#ifdef __linux__
  {
    TCase *fake = tcase_create ("fake kernel");

    suite_add_tcase (suite, fake);
    tcase_set_timeout (fake, 30);
    tcase_add_checked_fixture (fake, fake_setup, fake_teardown);
    tcase_add_test (fake, test_fake_partial_raise_retried);
    tcase_add_test (fake, test_fake_partial_release_retried);
    tcase_add_test (fake, test_fake_dispose_after_failed_release);
    tcase_add_test (fake, test_fake_equal_clamp_preserved);
    tcase_add_test (fake, test_fake_reused_tid_released);
    tcase_add_test (fake, test_fake_unsettled_scan_fails);
    tcase_add_test (fake, test_fake_replaced_list_followed);
    tcase_add_test (fake, test_fake_realtime_threads_untouched);
    tcase_add_test (fake, test_fake_promoted_thread_released);
    tcase_add_test (fake, test_fake_promoted_thread_released_without_reset);
    tcase_add_test (fake, test_fake_promoted_thread_released_at_rt_default);
    tcase_add_test (fake, test_fake_capped_thread_skipped);
    tcase_add_test (fake, test_fake_transient_error_retried);
    tcase_add_test (fake, test_fake_unsupported_kernel_disables);
    tcase_add_test (fake, test_fake_dispose_retries_release);
  }
#endif

  return suite;
}

GST_CHECK_MAIN (gstd_util_clamp);
