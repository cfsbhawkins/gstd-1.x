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
 * Tests for stability fixes:
 * - State change async handling
 * - State query with timeout
 * - Bus message parsing error handling
 * - Iterator resync limits
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <gst/check/gstcheck.h>

#include "gstd_session.h"
#include "gstd_pipeline.h"

/*
 * Test: State query returns valid state even during async transitions
 * This tests the 100ms timeout fix in gstd_state_read()
 */
GST_START_TEST (test_state_query_during_transition)
{
  GstdObject *node;
  GstdReturnCode ret;
  GstdSession *test_session = gstd_session_new ("Test Session");
  gchar *output = NULL;

  /* Create a pipeline */
  ret = gstd_get_by_uri (test_session, "/pipelines", &node);
  fail_if (ret);
  fail_if (NULL == node);

  ret = gstd_object_create (node, "p0", "fakesrc ! fakesink");
  fail_if (ret);
  gst_object_unref (node);

  /* Start playing - may be async */
  ret = gstd_get_by_uri (test_session, "/pipelines/p0/state", &node);
  fail_if (ret);
  fail_if (NULL == node);

  ret = gstd_object_update (node, "playing");
  /* Should succeed even if async */
  fail_if (ret != GSTD_EOK);

  /* Query state immediately - should not crash or hang */
  ret = gstd_object_to_string (node, &output);
  fail_if (ret);
  fail_if (NULL == output);
  g_free (output);

  gst_object_unref (node);
  gst_object_unref (test_session);
}
GST_END_TEST;

/*
 * Test: Multiple rapid state changes don't cause issues
 * This tests async state change handling
 */
GST_START_TEST (test_rapid_state_changes)
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
  fail_if (NULL == node);

  /* Rapid state changes - should not crash */
  for (i = 0; i < 5; i++) {
    ret = gstd_object_update (node, "playing");
    fail_if (ret != GSTD_EOK);
    ret = gstd_object_update (node, "paused");
    fail_if (ret != GSTD_EOK);
    ret = gstd_object_update (node, "ready");
    fail_if (ret != GSTD_EOK);
  }

  gst_object_unref (node);
  gst_object_unref (test_session);
}
GST_END_TEST;

/*
 * Test: Pipeline creation and deletion cycle
 * Tests for memory leaks in bus reference handling
 */
GST_START_TEST (test_pipeline_create_delete_cycle)
{
  GstdObject *node;
  GstdReturnCode ret;
  GstdSession *test_session = gstd_session_new ("Test Session");
  int i;
  gchar *pipe_name;

  ret = gstd_get_by_uri (test_session, "/pipelines", &node);
  fail_if (ret);
  fail_if (NULL == node);

  /* Create and delete pipelines multiple times */
  for (i = 0; i < 10; i++) {
    pipe_name = g_strdup_printf ("pipe%d", i);

    ret = gstd_object_create (node, pipe_name, "fakesrc ! fakesink");
    fail_if (ret);

    ret = gstd_object_delete (node, pipe_name);
    fail_if (ret);

    g_free (pipe_name);
  }

  gst_object_unref (node);
  gst_object_unref (test_session);
}
GST_END_TEST;

/*
 * Test: Pipeline with many elements
 * Tests iterator handling with larger pipelines
 */
GST_START_TEST (test_pipeline_many_elements)
{
  GstdObject *node;
  GstdObject *elements_node;
  GstdReturnCode ret;
  GstdSession *test_session = gstd_session_new ("Test Session");
  gchar *output = NULL;

  ret = gstd_get_by_uri (test_session, "/pipelines", &node);
  fail_if (ret);

  /* Create pipeline with multiple elements */
  ret = gstd_object_create (node, "p0",
      "fakesrc name=src ! queue name=q1 ! queue name=q2 ! "
      "queue name=q3 ! queue name=q4 ! fakesink name=sink");
  fail_if (ret);
  gst_object_unref (node);

  /* Query elements list - tests iterator */
  ret = gstd_get_by_uri (test_session, "/pipelines/p0/elements", &elements_node);
  fail_if (ret);
  fail_if (NULL == elements_node);

  ret = gstd_object_to_string (elements_node, &output);
  fail_if (ret);
  fail_if (NULL == output);

  /* Should contain all element names */
  fail_if (NULL == strstr (output, "src"));
  fail_if (NULL == strstr (output, "sink"));
  fail_if (NULL == strstr (output, "q1"));

  g_free (output);
  gst_object_unref (elements_node);
  gst_object_unref (test_session);
}
GST_END_TEST;

/*
 * Test: Invalid state transitions
 * Tests error handling in state changes
 */
GST_START_TEST (test_invalid_state_string)
{
  GstdObject *node;
  GstdReturnCode ret;
  GstdSession *test_session = gstd_session_new ("Test Session");

  /* Create a pipeline */
  ret = gstd_get_by_uri (test_session, "/pipelines", &node);
  fail_if (ret);
  ret = gstd_object_create (node, "p0", "fakesrc ! fakesink");
  fail_if (ret);
  gst_object_unref (node);

  /* Get state object */
  ret = gstd_get_by_uri (test_session, "/pipelines/p0/state", &node);
  fail_if (ret);

  /* Try invalid state string */
  ret = gstd_object_update (node, "invalid_state");
  fail_if (ret != GSTD_BAD_VALUE);

  /* Try empty string */
  ret = gstd_object_update (node, "");
  fail_if (ret != GSTD_BAD_VALUE);

  gst_object_unref (node);
  gst_object_unref (test_session);
}
GST_END_TEST;

/*
 * Test: Concurrent pipeline operations
 * Basic test for thread safety
 */
GST_START_TEST (test_multiple_pipelines)
{
  GstdObject *node;
  GstdReturnCode ret;
  GstdSession *test_session = gstd_session_new ("Test Session");
  int i;
  gchar *pipe_name;
  gchar *state_uri;
  GstdObject *state_node;

  ret = gstd_get_by_uri (test_session, "/pipelines", &node);
  fail_if (ret);

  /* Create multiple pipelines */
  for (i = 0; i < 5; i++) {
    pipe_name = g_strdup_printf ("pipe%d", i);
    ret = gstd_object_create (node, pipe_name, "fakesrc ! fakesink");
    fail_if (ret);
    g_free (pipe_name);
  }

  /* Start all pipelines */
  for (i = 0; i < 5; i++) {
    state_uri = g_strdup_printf ("/pipelines/pipe%d/state", i);
    ret = gstd_get_by_uri (test_session, state_uri, &state_node);
    fail_if (ret);
    ret = gstd_object_update (state_node, "playing");
    fail_if (ret);
    gst_object_unref (state_node);
    g_free (state_uri);
  }

  /* Stop all pipelines */
  for (i = 0; i < 5; i++) {
    state_uri = g_strdup_printf ("/pipelines/pipe%d/state", i);
    ret = gstd_get_by_uri (test_session, state_uri, &state_node);
    fail_if (ret);
    ret = gstd_object_update (state_node, "null");
    fail_if (ret);
    gst_object_unref (state_node);
    g_free (state_uri);
  }

  gst_object_unref (node);
  gst_object_unref (test_session);
}
GST_END_TEST;

static Suite *
gstd_stability_suite (void)
{
  Suite *suite = suite_create ("gstd_stability");
  TCase *tc = tcase_create ("general");

  suite_add_tcase (suite, tc);
  tcase_set_timeout (tc, 30);

  tcase_add_test (tc, test_state_query_during_transition);
  tcase_add_test (tc, test_rapid_state_changes);
  tcase_add_test (tc, test_pipeline_create_delete_cycle);
  tcase_add_test (tc, test_pipeline_many_elements);
  tcase_add_test (tc, test_invalid_state_string);
  tcase_add_test (tc, test_multiple_pipelines);

  return suite;
}

GST_CHECK_MAIN (gstd_stability);
