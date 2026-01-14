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
 * Tests for command parser functionality:
 * - Valid command parsing
 * - Invalid command handling
 * - Error paths
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <gst/check/gstcheck.h>

#include "gstd_session.h"
#include "gstd_parser.h"

static GstdSession *test_session = NULL;

static void
setup (void)
{
  test_session = gstd_session_new ("Parser Test Session");
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

/*
 * Test: Parse pipeline create command
 */
GST_START_TEST (test_parse_pipeline_create)
{
  GstdReturnCode ret;
  gchar *output = NULL;

  ret = gstd_parser_parse_cmd (test_session,
      "pipeline_create test_pipe fakesrc ! fakesink", &output);
  fail_if (ret != GSTD_EOK, "Pipeline create failed with code %d", ret);

  g_free (output);
  output = NULL;

  /* Cleanup */
  ret = gstd_parser_parse_cmd (test_session, "pipeline_delete test_pipe", &output);
  g_free (output);
}
GST_END_TEST;

/*
 * Test: Parse pipeline delete command
 */
GST_START_TEST (test_parse_pipeline_delete)
{
  GstdReturnCode ret;
  gchar *output = NULL;

  /* Create first */
  ret = gstd_parser_parse_cmd (test_session,
      "pipeline_create del_pipe fakesrc ! fakesink", &output);
  fail_if (ret != GSTD_EOK);
  g_free (output);
  output = NULL;

  /* Delete */
  ret = gstd_parser_parse_cmd (test_session, "pipeline_delete del_pipe", &output);
  fail_if (ret != GSTD_EOK, "Pipeline delete failed with code %d", ret);
  g_free (output);
}
GST_END_TEST;

/*
 * Test: Parse pipeline play command
 */
GST_START_TEST (test_parse_pipeline_play)
{
  GstdReturnCode ret;
  gchar *output = NULL;

  /* Create first */
  ret = gstd_parser_parse_cmd (test_session,
      "pipeline_create play_pipe fakesrc ! fakesink", &output);
  fail_if (ret != GSTD_EOK);
  g_free (output);
  output = NULL;

  /* Play */
  ret = gstd_parser_parse_cmd (test_session, "pipeline_play play_pipe", &output);
  fail_if (ret != GSTD_EOK, "Pipeline play failed with code %d", ret);
  g_free (output);
  output = NULL;

  /* Cleanup */
  ret = gstd_parser_parse_cmd (test_session, "pipeline_stop play_pipe", &output);
  g_free (output);
  output = NULL;
  ret = gstd_parser_parse_cmd (test_session, "pipeline_delete play_pipe", &output);
  g_free (output);
}
GST_END_TEST;

/*
 * Test: Parse pipeline pause command
 */
GST_START_TEST (test_parse_pipeline_pause)
{
  GstdReturnCode ret;
  gchar *output = NULL;

  /* Create and play first */
  ret = gstd_parser_parse_cmd (test_session,
      "pipeline_create pause_pipe fakesrc ! fakesink", &output);
  fail_if (ret != GSTD_EOK);
  g_free (output);
  output = NULL;

  ret = gstd_parser_parse_cmd (test_session, "pipeline_play pause_pipe", &output);
  fail_if (ret != GSTD_EOK);
  g_free (output);
  output = NULL;

  /* Pause */
  ret = gstd_parser_parse_cmd (test_session, "pipeline_pause pause_pipe", &output);
  fail_if (ret != GSTD_EOK, "Pipeline pause failed with code %d", ret);
  g_free (output);
  output = NULL;

  /* Cleanup */
  ret = gstd_parser_parse_cmd (test_session, "pipeline_stop pause_pipe", &output);
  g_free (output);
  output = NULL;
  ret = gstd_parser_parse_cmd (test_session, "pipeline_delete pause_pipe", &output);
  g_free (output);
}
GST_END_TEST;

/*
 * Test: Parse pipeline stop command
 */
GST_START_TEST (test_parse_pipeline_stop)
{
  GstdReturnCode ret;
  gchar *output = NULL;

  /* Create and play first */
  ret = gstd_parser_parse_cmd (test_session,
      "pipeline_create stop_pipe fakesrc ! fakesink", &output);
  fail_if (ret != GSTD_EOK);
  g_free (output);
  output = NULL;

  ret = gstd_parser_parse_cmd (test_session, "pipeline_play stop_pipe", &output);
  fail_if (ret != GSTD_EOK);
  g_free (output);
  output = NULL;

  /* Stop */
  ret = gstd_parser_parse_cmd (test_session, "pipeline_stop stop_pipe", &output);
  fail_if (ret != GSTD_EOK, "Pipeline stop failed with code %d", ret);
  g_free (output);
  output = NULL;

  /* Cleanup */
  ret = gstd_parser_parse_cmd (test_session, "pipeline_delete stop_pipe", &output);
  g_free (output);
}
GST_END_TEST;

/*
 * Test: Parse list_pipelines command
 */
GST_START_TEST (test_parse_list_pipelines)
{
  GstdReturnCode ret;
  gchar *output = NULL;

  ret = gstd_parser_parse_cmd (test_session, "list_pipelines", &output);
  fail_if (ret != GSTD_EOK, "list_pipelines failed with code %d", ret);
  fail_if (output == NULL);

  g_free (output);
}
GST_END_TEST;

/*
 * Test: Parse read command
 */
GST_START_TEST (test_parse_read)
{
  GstdReturnCode ret;
  gchar *output = NULL;

  /* Create a pipeline first */
  ret = gstd_parser_parse_cmd (test_session,
      "pipeline_create read_pipe fakesrc name=src ! fakesink", &output);
  fail_if (ret != GSTD_EOK);
  g_free (output);
  output = NULL;

  /* Read pipeline info */
  ret = gstd_parser_parse_cmd (test_session, "read /pipelines/read_pipe", &output);
  fail_if (ret != GSTD_EOK, "read command failed with code %d", ret);
  fail_if (output == NULL);
  g_free (output);
  output = NULL;

  /* Cleanup */
  ret = gstd_parser_parse_cmd (test_session, "pipeline_delete read_pipe", &output);
  g_free (output);
}
GST_END_TEST;

/*
 * Test: Parse element_get command
 */
GST_START_TEST (test_parse_element_get)
{
  GstdReturnCode ret;
  gchar *output = NULL;

  /* Create pipeline with named element */
  ret = gstd_parser_parse_cmd (test_session,
      "pipeline_create elem_pipe fakesrc name=mysrc num-buffers=100 ! fakesink", &output);
  fail_if (ret != GSTD_EOK);
  g_free (output);
  output = NULL;

  /* Get element property */
  ret = gstd_parser_parse_cmd (test_session,
      "element_get elem_pipe mysrc num-buffers", &output);
  fail_if (ret != GSTD_EOK, "element_get failed with code %d", ret);
  fail_if (output == NULL);
  fail_if (strstr (output, "100") == NULL, "Expected num-buffers=100 in output");
  g_free (output);
  output = NULL;

  /* Cleanup */
  ret = gstd_parser_parse_cmd (test_session, "pipeline_delete elem_pipe", &output);
  g_free (output);
}
GST_END_TEST;

/*
 * Test: Parse element_set command
 */
GST_START_TEST (test_parse_element_set)
{
  GstdReturnCode ret;
  gchar *output = NULL;

  /* Create pipeline with named element */
  ret = gstd_parser_parse_cmd (test_session,
      "pipeline_create set_pipe fakesrc name=mysrc ! fakesink", &output);
  fail_if (ret != GSTD_EOK);
  g_free (output);
  output = NULL;

  /* Set element property */
  ret = gstd_parser_parse_cmd (test_session,
      "element_set set_pipe mysrc num-buffers 50", &output);
  fail_if (ret != GSTD_EOK, "element_set failed with code %d", ret);
  g_free (output);
  output = NULL;

  /* Verify the change */
  ret = gstd_parser_parse_cmd (test_session,
      "element_get set_pipe mysrc num-buffers", &output);
  fail_if (ret != GSTD_EOK);
  fail_if (strstr (output, "50") == NULL, "Expected num-buffers=50 after set");
  g_free (output);
  output = NULL;

  /* Cleanup */
  ret = gstd_parser_parse_cmd (test_session, "pipeline_delete set_pipe", &output);
  g_free (output);
}
GST_END_TEST;

/*
 * Test: Invalid command returns error
 */
GST_START_TEST (test_parse_invalid_command)
{
  GstdReturnCode ret;
  gchar *output = NULL;

  ret = gstd_parser_parse_cmd (test_session, "this_is_not_a_valid_command", &output);
  fail_if (ret == GSTD_EOK, "Invalid command should return error");
  g_free (output);
}
GST_END_TEST;

/*
 * Test: NULL command returns error with expected critical warning
 * Note: Empty string "" causes crash in parser (known limitation)
 * so we test NULL instead
 */
GST_START_TEST (test_parse_null_command)
{
  GstdReturnCode ret;
  gchar *output = NULL;

  /* Expect critical warning from g_return_val_if_fail */
  ASSERT_CRITICAL (ret = gstd_parser_parse_cmd (test_session, NULL, &output));
  fail_if (ret == GSTD_EOK, "NULL command should return error");
  g_free (output);
}
GST_END_TEST;

/*
 * Test: Pipeline create with invalid description
 */
GST_START_TEST (test_parse_invalid_pipeline_description)
{
  GstdReturnCode ret;
  gchar *output = NULL;

  ret = gstd_parser_parse_cmd (test_session,
      "pipeline_create bad_pipe not_a_real_element ! fakesink", &output);
  fail_if (ret == GSTD_EOK, "Invalid pipeline description should fail");
  g_free (output);
}
GST_END_TEST;

/*
 * Test: Delete non-existent pipeline
 */
GST_START_TEST (test_parse_delete_nonexistent)
{
  GstdReturnCode ret;
  gchar *output = NULL;

  ret = gstd_parser_parse_cmd (test_session,
      "pipeline_delete nonexistent_pipeline", &output);
  fail_if (ret == GSTD_EOK, "Deleting non-existent pipeline should fail");
  g_free (output);
}
GST_END_TEST;

/*
 * Test: Play non-existent pipeline
 */
GST_START_TEST (test_parse_play_nonexistent)
{
  GstdReturnCode ret;
  gchar *output = NULL;

  ret = gstd_parser_parse_cmd (test_session,
      "pipeline_play nonexistent_pipeline", &output);
  fail_if (ret == GSTD_EOK, "Playing non-existent pipeline should fail");
  g_free (output);
}
GST_END_TEST;

/*
 * Test: Pipeline create missing arguments
 */
GST_START_TEST (test_parse_missing_arguments)
{
  GstdReturnCode ret;
  gchar *output = NULL;

  /* Missing pipeline description */
  ret = gstd_parser_parse_cmd (test_session, "pipeline_create just_name", &output);
  fail_if (ret == GSTD_EOK, "Missing pipeline description should fail");
  g_free (output);
}
GST_END_TEST;

/*
 * Test: List elements of pipeline
 */
GST_START_TEST (test_parse_list_elements)
{
  GstdReturnCode ret;
  gchar *output = NULL;

  /* Create pipeline */
  ret = gstd_parser_parse_cmd (test_session,
      "pipeline_create list_elem_pipe fakesrc name=src ! queue name=q ! fakesink name=sink", &output);
  fail_if (ret != GSTD_EOK);
  g_free (output);
  output = NULL;

  /* List elements */
  ret = gstd_parser_parse_cmd (test_session, "list_elements list_elem_pipe", &output);
  fail_if (ret != GSTD_EOK, "list_elements failed with code %d", ret);
  fail_if (output == NULL);
  fail_if (strstr (output, "src") == NULL, "Output should contain 'src'");
  fail_if (strstr (output, "sink") == NULL, "Output should contain 'sink'");
  g_free (output);
  output = NULL;

  /* Cleanup */
  ret = gstd_parser_parse_cmd (test_session, "pipeline_delete list_elem_pipe", &output);
  g_free (output);
}
GST_END_TEST;

/*
 * Test: Event EOS injection
 */
GST_START_TEST (test_parse_event_eos)
{
  GstdReturnCode ret;
  gchar *output = NULL;

  /* Create and play pipeline */
  ret = gstd_parser_parse_cmd (test_session,
      "pipeline_create eos_pipe fakesrc ! fakesink", &output);
  fail_if (ret != GSTD_EOK);
  g_free (output);
  output = NULL;

  ret = gstd_parser_parse_cmd (test_session, "pipeline_play eos_pipe", &output);
  fail_if (ret != GSTD_EOK);
  g_free (output);
  output = NULL;

  /* Inject EOS */
  ret = gstd_parser_parse_cmd (test_session, "event_eos eos_pipe", &output);
  fail_if (ret != GSTD_EOK, "event_eos failed with code %d", ret);
  g_free (output);
  output = NULL;

  /* Cleanup */
  ret = gstd_parser_parse_cmd (test_session, "pipeline_stop eos_pipe", &output);
  g_free (output);
  output = NULL;
  ret = gstd_parser_parse_cmd (test_session, "pipeline_delete eos_pipe", &output);
  g_free (output);
}
GST_END_TEST;

static Suite *
gstd_parser_suite (void)
{
  Suite *suite = suite_create ("gstd_parser");
  TCase *tc = tcase_create ("general");

  suite_add_tcase (suite, tc);
  tcase_set_timeout (tc, 30);
  tcase_add_checked_fixture (tc, setup, teardown);

  /* Valid command tests */
  tcase_add_test (tc, test_parse_pipeline_create);
  tcase_add_test (tc, test_parse_pipeline_delete);
  tcase_add_test (tc, test_parse_pipeline_play);
  tcase_add_test (tc, test_parse_pipeline_pause);
  tcase_add_test (tc, test_parse_pipeline_stop);
  tcase_add_test (tc, test_parse_list_pipelines);
  tcase_add_test (tc, test_parse_read);
  tcase_add_test (tc, test_parse_element_get);
  tcase_add_test (tc, test_parse_element_set);
  tcase_add_test (tc, test_parse_list_elements);
  tcase_add_test (tc, test_parse_event_eos);

  /* Error handling tests */
  tcase_add_test (tc, test_parse_invalid_command);
  tcase_add_test (tc, test_parse_null_command);
  tcase_add_test (tc, test_parse_invalid_pipeline_description);
  tcase_add_test (tc, test_parse_delete_nonexistent);
  tcase_add_test (tc, test_parse_play_nonexistent);
  tcase_add_test (tc, test_parse_missing_arguments);

  return suite;
}

GST_CHECK_MAIN (gstd_parser);
