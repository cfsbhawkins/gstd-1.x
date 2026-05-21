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
 * Regression tests for GstChildProxy property access.
 *
 * gstd enumerates the children of a GstChildProxy element (e.g. the
 * request sink pads of "compositor") and registers their properties under
 * prefixed names such as "sink_0::alpha". GETs have always worked because
 * the read path uses the stored pspec. These tests assert that WRITES also
 * work through to the underlying pad: a PUT on "sink_0::alpha" must return
 * GSTD_EOK and be observable on a subsequent GET.
 *
 * They also exercise a plain (non-child-proxy) element property to confirm
 * the regular property path is unaffected.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <gst/check/gstcheck.h>

#include "gstd_session.h"
#include "gstd_parser.h"

static GstdSession *test_session = NULL;

/* compositor lives in gst-plugins-base and videotestsrc in -base as well;
 * skip the suite gracefully if a minimal build lacks them. */
static gboolean
required_elements_available (void)
{
  GstElementFactory *comp = gst_element_factory_find ("compositor");
  GstElementFactory *vts = gst_element_factory_find ("videotestsrc");
  gboolean available = (comp != NULL) && (vts != NULL);

  if (comp)
    gst_object_unref (comp);
  if (vts)
    gst_object_unref (vts);

  return available;
}

static void
setup (void)
{
  test_session = gstd_session_new ("ChildProxy Property Test Session");
  fail_if (NULL == test_session);
}

static void
teardown (void)
{
  if (test_session) {
    g_object_unref (test_session);
    test_session = NULL;
  }
}

static GstdReturnCode
create_compositor_pipe (const gchar * name)
{
  gchar *cmd;
  gchar *output = NULL;
  GstdReturnCode ret;

  cmd = g_strdup_printf ("pipeline_create %s videotestsrc ! "
      "compositor name=mix ! fakesink", name);
  ret = gstd_parser_parse_cmd (test_session, cmd, &output);
  g_free (cmd);
  g_free (output);
  return ret;
}

static void
delete_pipe (const gchar * name)
{
  gchar *cmd;
  gchar *output = NULL;

  cmd = g_strdup_printf ("pipeline_delete %s", name);
  gstd_parser_parse_cmd (test_session, cmd, &output);
  g_free (output);
}

/*
 * Test: the prefixed child-proxy properties are enumerated on the element.
 */
GST_START_TEST (test_childproxy_properties_listed)
{
  GstdReturnCode ret;
  gchar *output = NULL;

  if (!required_elements_available ())
    return;

  ret = create_compositor_pipe ("cp_list");
  fail_if (ret != GSTD_EOK, "Pipeline create failed with code %d", ret);

  ret = gstd_parser_parse_cmd (test_session,
      "read /pipelines/cp_list/elements/mix/properties", &output);
  fail_if (ret != GSTD_EOK, "Reading mix properties failed with code %d", ret);
  fail_if (NULL == output);

  fail_unless (NULL != g_strstr_len (output, -1, "sink_0::alpha"),
      "sink_0::alpha not enumerated");
  fail_unless (NULL != g_strstr_len (output, -1, "sink_0::xpos"),
      "sink_0::xpos not enumerated");
  fail_unless (NULL != g_strstr_len (output, -1, "sink_0::width"),
      "sink_0::width not enumerated");
  fail_unless (NULL != g_strstr_len (output, -1, "sink_0::height"),
      "sink_0::height not enumerated");
  fail_unless (NULL != g_strstr_len (output, -1, "sink_0::zorder"),
      "sink_0::zorder not enumerated");

  g_free (output);
  delete_pipe ("cp_list");
}
GST_END_TEST;

/*
 * Test: writing a double child-proxy property (sink_0::alpha). This is the
 * regression case -- before the read path used the stored pspec, the write
 * path used the prefixed name and silently failed.
 */
GST_START_TEST (test_childproxy_set_alpha_double)
{
  GstdReturnCode ret;
  gchar *output = NULL;

  if (!required_elements_available ())
    return;

  ret = create_compositor_pipe ("cp_alpha");
  fail_if (ret != GSTD_EOK, "Pipeline create failed with code %d", ret);

  /* Default alpha is 1.0 */
  ret = gstd_parser_parse_cmd (test_session,
      "element_get cp_alpha mix sink_0::alpha", &output);
  fail_if (ret != GSTD_EOK, "Reading alpha failed with code %d", ret);
  fail_unless (NULL != g_strstr_len (output, -1, "\"value\" : \"1\""),
      "alpha default was not 1: %s", output);
  g_free (output);
  output = NULL;

  /* Write 0.0 -- must succeed */
  ret = gstd_parser_parse_cmd (test_session,
      "element_set cp_alpha mix sink_0::alpha 0.0", &output);
  fail_if (ret != GSTD_EOK, "Setting alpha=0.0 failed with code %d", ret);
  g_free (output);
  output = NULL;

  /* Read back -- must now be 0 */
  ret = gstd_parser_parse_cmd (test_session,
      "element_get cp_alpha mix sink_0::alpha", &output);
  fail_if (ret != GSTD_EOK, "Reading alpha back failed with code %d", ret);
  fail_unless (NULL != g_strstr_len (output, -1, "\"value\" : \"0\""),
      "alpha was not updated to 0: %s", output);
  g_free (output);

  delete_pipe ("cp_alpha");
}
GST_END_TEST;

/*
 * Test: writing an integer child-proxy property (sink_0::xpos).
 */
GST_START_TEST (test_childproxy_set_xpos_int)
{
  GstdReturnCode ret;
  gchar *output = NULL;

  if (!required_elements_available ())
    return;

  ret = create_compositor_pipe ("cp_xpos");
  fail_if (ret != GSTD_EOK, "Pipeline create failed with code %d", ret);

  ret = gstd_parser_parse_cmd (test_session,
      "element_set cp_xpos mix sink_0::xpos 100", &output);
  fail_if (ret != GSTD_EOK, "Setting xpos=100 failed with code %d", ret);
  g_free (output);
  output = NULL;

  ret = gstd_parser_parse_cmd (test_session,
      "element_get cp_xpos mix sink_0::xpos", &output);
  fail_if (ret != GSTD_EOK, "Reading xpos back failed with code %d", ret);
  fail_unless (NULL != g_strstr_len (output, -1, "\"value\" : \"100\""),
      "xpos was not updated to 100: %s", output);
  g_free (output);

  delete_pipe ("cp_xpos");
}
GST_END_TEST;

/*
 * Test: a plain (non-child-proxy) property still works after the fix.
 */
GST_START_TEST (test_plain_property_unaffected)
{
  GstdReturnCode ret;
  gchar *output = NULL;

  if (!required_elements_available ())
    return;

  ret = create_compositor_pipe ("cp_plain");
  fail_if (ret != GSTD_EOK, "Pipeline create failed with code %d", ret);

  /* videotestsrc pattern 18 == "ball" */
  ret = gstd_parser_parse_cmd (test_session,
      "element_set cp_plain videotestsrc0 pattern 18", &output);
  fail_if (ret != GSTD_EOK, "Setting pattern failed with code %d", ret);
  g_free (output);
  output = NULL;

  ret = gstd_parser_parse_cmd (test_session,
      "element_get cp_plain videotestsrc0 pattern", &output);
  fail_if (ret != GSTD_EOK, "Reading pattern back failed with code %d", ret);
  fail_unless (NULL != g_strstr_len (output, -1, "ball"),
      "pattern was not updated to ball: %s", output);
  g_free (output);

  delete_pipe ("cp_plain");
}
GST_END_TEST;

static Suite *
gstd_childproxy_property_suite (void)
{
  Suite *suite = suite_create ("gstd_childproxy_property");
  TCase *tc = tcase_create ("general");

  suite_add_tcase (suite, tc);
  tcase_add_checked_fixture (tc, setup, teardown);

  tcase_add_test (tc, test_childproxy_properties_listed);
  tcase_add_test (tc, test_childproxy_set_alpha_double);
  tcase_add_test (tc, test_childproxy_set_xpos_int);
  tcase_add_test (tc, test_plain_property_unaffected);

  return suite;
}

GST_CHECK_MAIN (gstd_childproxy_property);
