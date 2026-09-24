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
 * Tests for the pipeline count limit (GstdList max-children), the
 * resource-exhaustion guard behind --max-pipelines / GSTD_MAX_PIPELINES.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <gst/check/gstcheck.h>

#include "gstd_list.h"
#include "gstd_session.h"

GST_START_TEST (test_pipeline_limit_enforced)
{
  GstdObject *node;
  GstdReturnCode ret;
  GstdSession *test_session = gstd_session_new ("Limit_session");

  ret = gstd_get_by_uri (test_session, "/pipelines", &node);
  fail_if (ret);
  fail_if (NULL == node);

  g_object_set (node, "max-children", 2, NULL);

  ret = gstd_object_create (node, "p0", "fakesrc ! fakesink");
  fail_if (GSTD_EOK != ret);

  ret = gstd_object_create (node, "p1", "fakesrc ! fakesink");
  fail_if (GSTD_EOK != ret);

  /* Third pipeline exceeds the cap */
  ret = gstd_object_create (node, "p2", "fakesrc ! fakesink");
  fail_if (GSTD_MAX_LIMIT_REACHED != ret,
      "Expected GSTD_MAX_LIMIT_REACHED, got %d", ret);

  /* Deleting frees a slot */
  ret = gstd_object_delete (node, "p0");
  fail_if (GSTD_EOK != ret);

  ret = gstd_object_create (node, "p2", "fakesrc ! fakesink");
  fail_if (GSTD_EOK != ret);

  gst_object_unref (node);
  gst_object_unref (test_session);
}

GST_END_TEST;

GST_START_TEST (test_pipeline_limit_unlimited_by_default)
{
  GstdObject *node;
  GstdReturnCode ret;
  guint max = 12345;
  gint i;
  gchar *name;
  GstdSession *test_session = gstd_session_new ("Unlimited_session");

  ret = gstd_get_by_uri (test_session, "/pipelines", &node);
  fail_if (ret);
  fail_if (NULL == node);

  g_object_get (node, "max-children", &max, NULL);
  fail_if (0 != max, "max-children should default to 0 (unlimited)");

  /* Well past any accidental small default */
  for (i = 0; i < 20; i++) {
    name = g_strdup_printf ("p%d", i);
    ret = gstd_object_create (node, name, "fakesrc ! fakesink");
    fail_if (GSTD_EOK != ret, "Creation %d failed with %d", i, ret);
    g_free (name);
  }

  gst_object_unref (node);
  gst_object_unref (test_session);
}

GST_END_TEST;

GST_START_TEST (test_pipeline_limit_zero_disables)
{
  GstdObject *node;
  GstdReturnCode ret;
  GstdSession *test_session = gstd_session_new ("Reset_session");

  ret = gstd_get_by_uri (test_session, "/pipelines", &node);
  fail_if (ret);
  fail_if (NULL == node);

  g_object_set (node, "max-children", 1, NULL);

  ret = gstd_object_create (node, "p0", "fakesrc ! fakesink");
  fail_if (GSTD_EOK != ret);

  ret = gstd_object_create (node, "p1", "fakesrc ! fakesink");
  fail_if (GSTD_MAX_LIMIT_REACHED != ret);

  /* Setting the cap back to 0 lifts the limit */
  g_object_set (node, "max-children", 0, NULL);

  ret = gstd_object_create (node, "p1", "fakesrc ! fakesink");
  fail_if (GSTD_EOK != ret);

  gst_object_unref (node);
  gst_object_unref (test_session);
}

GST_END_TEST;

static Suite *
gstd_pipeline_limit_suite (void)
{
  Suite *suite = suite_create ("gstd_pipeline_limit");
  TCase *tc = tcase_create ("general");

  suite_add_tcase (suite, tc);
  tcase_set_timeout (tc, 30);

  tcase_add_test (tc, test_pipeline_limit_enforced);
  tcase_add_test (tc, test_pipeline_limit_unlimited_by_default);
  tcase_add_test (tc, test_pipeline_limit_zero_disables);

  return suite;
}

GST_CHECK_MAIN (gstd_pipeline_limit);
