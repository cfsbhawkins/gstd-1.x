/*
 * This file is part of GStreamer Daemon
 * Copyright 2015-2022 Ridgerun, LLC (http://www.ridgerun.com)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <string.h>
#include <gst/check/gstcheck.h>

#include "gstd_parser.h"
#include "gstd_session.h"

/*
 * The "metadata" property is an opaque string a client keeps with a
 * pipeline. These tests drive it through the same parser commands the
 * IPC front ends use, so they cover the property reader and updater
 * as well as the property itself.
 */

static GstdSession *
session_with_pipeline (const gchar * name)
{
  GstdObject *node;
  GstdReturnCode ret;
  GstdSession *session = gstd_session_new ("Metadata test session");

  ret = gstd_get_by_uri (session, "/pipelines", &node);
  fail_if (ret);
  fail_if (NULL == node);
  ret = gstd_object_create (node, name, "fakesrc ! fakesink");
  fail_if (ret);
  gst_object_unref (node);

  return session;
}

static gchar *
read_metadata (GstdSession * session, const gchar * pipeline)
{
  gchar *command;
  gchar *output = NULL;
  GstdReturnCode ret;

  command = g_strdup_printf ("read /pipelines/%s/metadata", pipeline);
  ret = gstd_parser_parse_cmd (session, command, &output);
  g_free (command);
  fail_if (ret, "read of metadata failed with %d", ret);
  fail_if (NULL == output);

  return output;
}

GST_START_TEST (test_metadata_defaults_to_empty)
{
  GstdSession *session = session_with_pipeline ("p0");
  gchar *output = read_metadata (session, "p0");

  fail_if (strstr (output, "\"name\" : \"metadata\"") == NULL,
      "Expected the metadata property in: %s", output);
  fail_if (strstr (output, "\"value\" : \"\"") == NULL,
      "Expected an empty default value in: %s", output);

  g_free (output);
  output = NULL;
  gst_object_unref (session);
}

GST_END_TEST;

GST_START_TEST (test_metadata_update_and_read_back)
{
  GstdSession *session = session_with_pipeline ("p0");
  gchar *output = NULL;
  GstdReturnCode ret;

  ret = gstd_parser_parse_cmd (session,
      "update /pipelines/p0/metadata eyJrZXkiOiJ2YWx1ZSJ9", &output);
  fail_if (ret, "update of metadata failed with %d", ret);
  g_free (output);
  output = NULL;

  output = read_metadata (session, "p0");
  fail_if (strstr (output, "eyJrZXkiOiJ2YWx1ZSJ9") == NULL,
      "Expected the stored value in: %s", output);
  g_free (output);
  output = NULL;

  /* A later update replaces the value rather than appending to it */
  ret = gstd_parser_parse_cmd (session,
      "update /pipelines/p0/metadata second", &output);
  fail_if (ret);
  g_free (output);
  output = NULL;

  output = read_metadata (session, "p0");
  fail_if (strstr (output, "second") == NULL,
      "Expected the replaced value in: %s", output);
  fail_if (strstr (output, "eyJrZXkiOiJ2YWx1ZSJ9") != NULL,
      "The previous value must be gone: %s", output);
  g_free (output);
  output = NULL;

  gst_object_unref (session);
}

GST_END_TEST;

GST_START_TEST (test_metadata_keeps_spaces)
{
  GstdSession *session = session_with_pipeline ("p0");
  gchar *output = NULL;
  GstdReturnCode ret;

  /* The parser splits the resource from the value on the first space
   * only; the rest of the line is the value verbatim. It reads back in
   * gst_value_serialize form, which escapes the spaces. */
  ret = gstd_parser_parse_cmd (session,
      "update /pipelines/p0/metadata one two  three", &output);
  fail_if (ret);
  g_free (output);
  output = NULL;

  output = read_metadata (session, "p0");
  fail_if (strstr (output, "one\\\\ two\\\\ \\\\ three") == NULL,
      "Expected the value with its spaces in: %s", output);
  g_free (output);
  output = NULL;

  gst_object_unref (session);
}

GST_END_TEST;

GST_START_TEST (test_metadata_survives_state_changes)
{
  GstdSession *session = session_with_pipeline ("p0");
  gchar *output = NULL;
  GstdReturnCode ret;

  ret = gstd_parser_parse_cmd (session,
      "update /pipelines/p0/metadata kept", &output);
  fail_if (ret);
  g_free (output);
  output = NULL;

  ret = gstd_parser_parse_cmd (session, "pipeline_play p0", &output);
  fail_if (ret, "play failed with %d", ret);
  g_free (output);
  output = NULL;

  output = read_metadata (session, "p0");
  fail_if (strstr (output, "kept") == NULL,
      "Expected the value while playing: %s", output);
  g_free (output);
  output = NULL;

  /* Updating while playing does not disturb the pipeline */
  ret = gstd_parser_parse_cmd (session,
      "update /pipelines/p0/metadata replaced-while-playing", &output);
  fail_if (ret);
  g_free (output);
  output = NULL;

  ret = gstd_parser_parse_cmd (session, "read /pipelines/p0/state", &output);
  fail_if (ret);
  fail_if (strstr (output, "playing") == NULL,
      "Pipeline should still be playing: %s", output);
  g_free (output);
  output = NULL;

  ret = gstd_parser_parse_cmd (session, "pipeline_stop p0", &output);
  fail_if (ret, "stop failed with %d", ret);
  g_free (output);
  output = NULL;

  output = read_metadata (session, "p0");
  fail_if (strstr (output, "replaced-while-playing") == NULL,
      "Expected the value after stop: %s", output);
  g_free (output);
  output = NULL;

  gst_object_unref (session);
}

GST_END_TEST;

GST_START_TEST (test_metadata_object_value_is_verbatim)
{
  GstdObject *node;
  GstdSession *session = session_with_pipeline ("p0");
  gchar *value = NULL;
  GstdReturnCode ret;

  ret = gstd_get_by_uri (session, "/pipelines/p0", &node);
  fail_if (ret);
  fail_if (NULL == node);

  /* At the object level the string is opaque: the text-protocol tokens
   * NULL and quotes mean nothing here */
  g_object_set (node, "metadata", "NULL", NULL);
  g_object_get (node, "metadata", &value, NULL);
  fail_if (g_strcmp0 (value, "NULL") != 0, "Expected NULL verbatim: %s", value);
  g_free (value);
  value = NULL;

  g_object_set (node, "metadata", "\"quoted\"", NULL);
  g_object_get (node, "metadata", &value, NULL);
  fail_if (g_strcmp0 (value, "\"quoted\"") != 0,
      "Expected the quotes kept: %s", value);
  g_free (value);
  value = NULL;

  /* An empty string unsets, which reads back as NULL */
  g_object_set (node, "metadata", "", NULL);
  g_object_get (node, "metadata", &value, NULL);
  fail_if (value != NULL, "Expected an unset value: %s", value);

  gst_object_unref (node);
  gst_object_unref (session);
}

GST_END_TEST;

GST_START_TEST (test_metadata_is_per_pipeline)
{
  GstdObject *node;
  GstdSession *session = session_with_pipeline ("p0");
  gchar *output = NULL;
  GstdReturnCode ret;

  ret = gstd_get_by_uri (session, "/pipelines", &node);
  fail_if (ret);
  ret = gstd_object_create (node, "p1", "fakesrc ! fakesink");
  fail_if (ret);
  gst_object_unref (node);

  ret = gstd_parser_parse_cmd (session,
      "update /pipelines/p0/metadata first", &output);
  fail_if (ret);
  g_free (output);
  output = NULL;

  output = read_metadata (session, "p1");
  fail_if (strstr (output, "first") != NULL,
      "The other pipeline must keep its own (empty) value: %s", output);
  g_free (output);
  output = NULL;

  gst_object_unref (session);
}

GST_END_TEST;

static Suite *
gstd_pipeline_metadata_suite (void)
{
  Suite *suite = suite_create ("gstd_pipeline_metadata");
  TCase *tc = tcase_create ("general");

  suite_add_tcase (suite, tc);
  tcase_add_test (tc, test_metadata_defaults_to_empty);
  tcase_add_test (tc, test_metadata_update_and_read_back);
  tcase_add_test (tc, test_metadata_keeps_spaces);
  tcase_add_test (tc, test_metadata_survives_state_changes);
  tcase_add_test (tc, test_metadata_object_value_is_verbatim);
  tcase_add_test (tc, test_metadata_is_per_pipeline);

  return suite;
}

GST_CHECK_MAIN (gstd_pipeline_metadata);
