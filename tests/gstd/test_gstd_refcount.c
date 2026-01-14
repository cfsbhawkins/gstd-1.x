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
 * Tests for refcount and thread safety fixes:
 * - State refcount operations under concurrent access
 * - Pipeline refcount operations
 * - Session singleton thread safety
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <gst/check/gstcheck.h>
#include <pthread.h>

#include "gstd_session.h"
#include "gstd_pipeline.h"

#define NUM_THREADS 4
#define NUM_ITERATIONS 100

static GstdSession *shared_session = NULL;
static GstdObject *shared_state_node = NULL;
static volatile gboolean test_running = FALSE;

/*
 * Thread function for concurrent state changes
 */
static void *
state_change_thread (void *data)
{
  int thread_id = GPOINTER_TO_INT (data);
  int i;
  GstdReturnCode ret;
  const gchar *states[] = { "playing", "paused", "ready", "null" };

  while (!test_running) {
    /* Busy wait for test to start */
    g_usleep (100);
  }

  for (i = 0; i < NUM_ITERATIONS && test_running; i++) {
    const gchar *state = states[(thread_id + i) % 4];
    ret = gstd_object_update (shared_state_node, state);
    /* State change may fail due to race, but should not crash */
    (void) ret;
    g_usleep (100);  /* Small delay to increase race likelihood */
  }

  return NULL;
}

/*
 * Test: Concurrent state changes from multiple threads
 * This tests thread safety of state refcount operations
 */
GST_START_TEST (test_concurrent_state_changes)
{
  GstdObject *node;
  GstdReturnCode ret;
  pthread_t threads[NUM_THREADS];
  int i;

  shared_session = gstd_session_new ("Test Session");
  fail_if (NULL == shared_session);

  /* Create a pipeline */
  ret = gstd_get_by_uri (shared_session, "/pipelines", &node);
  fail_if (ret);
  ret = gstd_object_create (node, "p0", "fakesrc ! fakesink");
  fail_if (ret);
  gst_object_unref (node);

  /* Get state node for all threads to share */
  ret = gstd_get_by_uri (shared_session, "/pipelines/p0/state", &shared_state_node);
  fail_if (ret);
  fail_if (NULL == shared_state_node);

  /* Do one state change first to ensure GType registration is complete
   * before threads start (avoids race in g_enum_register_static) */
  ret = gstd_object_update (shared_state_node, "playing");
  fail_if (ret != GSTD_EOK);

  /* Start threads */
  test_running = TRUE;
  for (i = 0; i < NUM_THREADS; i++) {
    pthread_create (&threads[i], NULL, state_change_thread, GINT_TO_POINTER (i));
  }

  /* Wait for threads to complete */
  for (i = 0; i < NUM_THREADS; i++) {
    pthread_join (threads[i], NULL);
  }

  test_running = FALSE;

  /* Cleanup - should not crash even after concurrent access */
  gst_object_unref (shared_state_node);
  shared_state_node = NULL;
  gst_object_unref (shared_session);
  shared_session = NULL;
}
GST_END_TEST;

/*
 * Test: Invalid state string handling doesn't leak memory
 * This tests the GValue unset fix in gstd_state_update()
 */
GST_START_TEST (test_invalid_state_no_leak)
{
  GstdObject *node;
  GstdReturnCode ret;
  GstdSession *test_session = gstd_session_new ("Test Session");
  int i;

  /* Create a pipeline */
  ret = gstd_get_by_uri (test_session, "/pipelines", &node);
  fail_if (ret);
  ret = gstd_object_create (node, "p0", "fakesrc ! fakesink");
  fail_if (ret);
  gst_object_unref (node);

  /* Get state object */
  ret = gstd_get_by_uri (test_session, "/pipelines/p0/state", &node);
  fail_if (ret);

  /* Repeatedly try invalid states - should not leak GValue */
  for (i = 0; i < 1000; i++) {
    ret = gstd_object_update (node, "not_a_valid_state");
    fail_if (ret != GSTD_BAD_VALUE);
  }

  gst_object_unref (node);
  gst_object_unref (test_session);
}
GST_END_TEST;

/*
 * Test: Pipeline play/stop refcount cycles
 * Tests that pipeline_play_ref and pipeline_stop_ref are balanced
 */
GST_START_TEST (test_pipeline_refcount_balance)
{
  GstdObject *node;
  GstdObject *state_node;
  GstdReturnCode ret;
  GstdSession *test_session = gstd_session_new ("Test Session");
  int i;

  /* Create a pipeline */
  ret = gstd_get_by_uri (test_session, "/pipelines", &node);
  fail_if (ret);
  ret = gstd_object_create (node, "p0", "fakesrc ! fakesink");
  fail_if (ret);
  gst_object_unref (node);

  /* Get state object */
  ret = gstd_get_by_uri (test_session, "/pipelines/p0/state", &node);
  fail_if (ret);

  /* Cycle through play/stop multiple times */
  for (i = 0; i < 50; i++) {
    ret = gstd_object_update (node, "playing");
    fail_if (ret != GSTD_EOK);
    ret = gstd_object_update (node, "null");
    fail_if (ret != GSTD_EOK);
  }

  /* Final state should be null */
  gst_object_unref (node);

  /* Pipeline should be deletable (refcount balanced) */
  ret = gstd_get_by_uri (test_session, "/pipelines", &node);
  fail_if (ret);
  ret = gstd_object_delete (node, "p0");
  fail_if (ret);

  gst_object_unref (node);
  gst_object_unref (test_session);
}
GST_END_TEST;

/*
 * Test: Session singleton behavior
 * Tests that multiple session requests return same instance
 */
GST_START_TEST (test_session_singleton)
{
  GstdSession *session1;
  GstdSession *session2;
  GstdSession *session3;

  session1 = gstd_session_new ("Session 1");
  fail_if (NULL == session1);

  session2 = gstd_session_new ("Session 2");
  fail_if (NULL == session2);

  /* Both should be the same singleton instance */
  fail_unless (session1 == session2);

  /* Unref one, other should still be valid */
  gst_object_unref (session1);

  session3 = gstd_session_new ("Session 3");
  fail_if (NULL == session3);
  fail_unless (session2 == session3);

  gst_object_unref (session2);
  gst_object_unref (session3);
}
GST_END_TEST;

/*
 * Thread function for concurrent session access
 */
static void *
session_access_thread (void *data)
{
  int i;
  GstdSession *session;

  for (i = 0; i < NUM_ITERATIONS; i++) {
    session = gstd_session_new ("Thread Session");
    if (session) {
      gst_object_unref (session);
    }
    g_usleep (10);
  }

  return NULL;
}

/*
 * Test: Concurrent session creation/destruction
 * Tests thread safety of session singleton pattern
 */
GST_START_TEST (test_concurrent_session_access)
{
  pthread_t threads[NUM_THREADS];
  int i;

  /* Start threads that all try to get/release the session */
  for (i = 0; i < NUM_THREADS; i++) {
    pthread_create (&threads[i], NULL, session_access_thread, NULL);
  }

  /* Wait for all threads */
  for (i = 0; i < NUM_THREADS; i++) {
    pthread_join (threads[i], NULL);
  }

  /* Should complete without crash or deadlock */
}
GST_END_TEST;

static Suite *
gstd_refcount_suite (void)
{
  Suite *suite = suite_create ("gstd_refcount");
  TCase *tc = tcase_create ("general");

  suite_add_tcase (suite, tc);
  tcase_set_timeout (tc, 60);

  tcase_add_test (tc, test_concurrent_state_changes);
  tcase_add_test (tc, test_invalid_state_no_leak);
  tcase_add_test (tc, test_pipeline_refcount_balance);
  tcase_add_test (tc, test_session_singleton);
  tcase_add_test (tc, test_concurrent_session_access);

  return suite;
}

GST_CHECK_MAIN (gstd_refcount);
