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

#include "gstd_icreator.h"
#include "gstd_list.h"
#include "gstd_session.h"

/*
 * A creator that records how many constructions run at once. It stands in
 * for the pipeline creator, whose cost (graph, memory, fds) is what the
 * cap is meant to bound, and holds each construction open long enough for
 * concurrent creates to overlap.
 */
#define TEST_CONSTRUCTION_US (200 * 1000)

typedef struct _TestCountingCreator
{
  GObject parent;
} TestCountingCreator;

typedef struct _TestCountingCreatorClass
{
  GObjectClass parent_class;
} TestCountingCreatorClass;

static GType test_counting_creator_get_type (void);
static void test_counting_creator_iface_init (GstdICreatorInterface * iface);

G_DEFINE_TYPE_WITH_CODE (TestCountingCreator, test_counting_creator,
    G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE (GSTD_TYPE_ICREATOR,
        test_counting_creator_iface_init));

static gint in_flight = 0;
static gint peak_in_flight = 0;
static gint constructions = 0;

static GstdReturnCode
test_counting_creator_create (GstdICreator * iface, const gchar * name,
    const gchar * description, GstdObject ** out)
{
  gint now;
  gint peak;

  *out = NULL;

  now = g_atomic_int_add (&in_flight, 1) + 1;
  g_atomic_int_inc (&constructions);
  do {
    peak = g_atomic_int_get (&peak_in_flight);
  } while (now > peak
      && !g_atomic_int_compare_and_exchange (&peak_in_flight, peak, now));

  g_usleep (TEST_CONSTRUCTION_US);
  g_atomic_int_add (&in_flight, -1);

  if (g_strcmp0 (description, "fail") == 0) {
    return GSTD_BAD_DESCRIPTION;
  }

  *out = g_object_new (GSTD_TYPE_OBJECT, "name", name, NULL);
  return GSTD_EOK;
}

static void
test_counting_creator_iface_init (GstdICreatorInterface * iface)
{
  iface->create = test_counting_creator_create;
}

static void
test_counting_creator_class_init (TestCountingCreatorClass * klass)
{
}

static void
test_counting_creator_init (TestCountingCreator * self)
{
}

static GstdList *
new_counting_list (guint max_children)
{
  GstdList *list = g_object_new (GSTD_TYPE_LIST, "name", "counted",
      "node-type", GSTD_TYPE_OBJECT, "max-children", max_children, NULL);

  gstd_object_set_creator (GSTD_OBJECT (list),
      GSTD_ICREATOR (g_object_new (test_counting_creator_get_type (), NULL)));

  g_atomic_int_set (&in_flight, 0);
  g_atomic_int_set (&peak_in_flight, 0);
  g_atomic_int_set (&constructions, 0);

  return list;
}

typedef struct
{
  GstdList *list;
  gchar *name;
  GMutex *gate_mutex;
  GCond *gate_cond;
  gboolean *gate_open;
  GstdReturnCode ret;
} CreateJob;

static gpointer
create_job_func (gpointer data)
{
  CreateJob *job = data;

  /* Start every create at the same moment */
  g_mutex_lock (job->gate_mutex);
  while (!*job->gate_open) {
    g_cond_wait (job->gate_cond, job->gate_mutex);
  }
  g_mutex_unlock (job->gate_mutex);

  job->ret = gstd_object_create (GSTD_OBJECT (job->list), job->name, "desc");
  return NULL;
}

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

/*
 * Test: parallel creates cannot all construct before the cap rejects
 * them. Capacity is reserved under the list lock before construction, so
 * no more than max-children constructions ever run at once and the
 * surplus is rejected without being built.
 */
#define CONCURRENT_CREATES 8
#define CONCURRENT_CAP 2

GST_START_TEST (test_pipeline_limit_concurrent_creates_reserve)
{
  GstdList *list = new_counting_list (CONCURRENT_CAP);
  CreateJob jobs[CONCURRENT_CREATES];
  GThread *threads[CONCURRENT_CREATES];
  GMutex gate_mutex;
  GCond gate_cond;
  gboolean gate_open = FALSE;
  guint created = 0;
  guint rejected = 0;
  guint count = 0;
  gint i;

  g_mutex_init (&gate_mutex);
  g_cond_init (&gate_cond);

  for (i = 0; i < CONCURRENT_CREATES; i++) {
    jobs[i].list = list;
    jobs[i].name = g_strdup_printf ("c%d", i);
    jobs[i].gate_mutex = &gate_mutex;
    jobs[i].gate_cond = &gate_cond;
    jobs[i].gate_open = &gate_open;
    jobs[i].ret = GSTD_EOK;
    threads[i] = g_thread_new ("create", create_job_func, &jobs[i]);
  }

  g_mutex_lock (&gate_mutex);
  gate_open = TRUE;
  g_cond_broadcast (&gate_cond);
  g_mutex_unlock (&gate_mutex);

  for (i = 0; i < CONCURRENT_CREATES; i++) {
    g_thread_join (threads[i]);
    if (jobs[i].ret == GSTD_EOK) {
      created++;
    } else {
      fail_if (jobs[i].ret != GSTD_MAX_LIMIT_REACHED,
          "Create %d failed with %d, expected GSTD_MAX_LIMIT_REACHED", i,
          jobs[i].ret);
      rejected++;
    }
    g_free (jobs[i].name);
  }

  fail_if (g_atomic_int_get (&peak_in_flight) > CONCURRENT_CAP,
      "%d constructions ran at once with a cap of %d",
      g_atomic_int_get (&peak_in_flight), CONCURRENT_CAP);
  fail_if (g_atomic_int_get (&constructions) != CONCURRENT_CAP,
      "%d resources were constructed with a cap of %d",
      g_atomic_int_get (&constructions), CONCURRENT_CAP);
  fail_if (created != CONCURRENT_CAP, "%u creates succeeded, expected %d",
      created, CONCURRENT_CAP);
  fail_if (rejected != CONCURRENT_CREATES - CONCURRENT_CAP);

  g_object_get (list, "count", &count, NULL);
  fail_if (count != CONCURRENT_CAP, "List holds %u children", count);

  g_mutex_clear (&gate_mutex);
  g_cond_clear (&gate_cond);
  g_object_unref (list);
}

GST_END_TEST;

/*
 * Test: a failed construction or a duplicate name gives its reserved
 * slot back
 */
GST_START_TEST (test_pipeline_limit_failed_create_releases_slot)
{
  GstdList *list = new_counting_list (1);
  GstdReturnCode ret;

  ret = gstd_object_create (GSTD_OBJECT (list), "broken", "fail");
  fail_if (GSTD_BAD_DESCRIPTION != ret, "Expected a construction failure");

  ret = gstd_object_create (GSTD_OBJECT (list), "p0", "desc");
  fail_if (GSTD_EOK != ret, "A failed create kept its slot (%d)", ret);

  g_object_set (list, "max-children", 2, NULL);
  ret = gstd_object_create (GSTD_OBJECT (list), "p0", "desc");
  fail_if (GSTD_EXISTING_RESOURCE != ret, "Expected a duplicate (%d)", ret);

  ret = gstd_object_create (GSTD_OBJECT (list), "p1", "desc");
  fail_if (GSTD_EOK != ret, "A duplicate create kept its slot (%d)", ret);

  g_object_unref (list);
}

GST_END_TEST;

/*
 * Test: with the real pipeline creator, an unparsable description does
 * not consume capacity
 */
GST_START_TEST (test_pipeline_limit_bad_pipeline_releases_slot)
{
  GstdObject *node;
  GstdReturnCode ret;
  GstdSession *test_session = gstd_session_new ("Release_session");

  ret = gstd_get_by_uri (test_session, "/pipelines", &node);
  fail_if (ret);
  fail_if (NULL == node);

  g_object_set (node, "max-children", 1, NULL);

  ret = gstd_object_create (node, "bad", "nosuchelement_xyz ! fakesink");
  fail_if (GSTD_EOK == ret);

  ret = gstd_object_create (node, "good", "fakesrc ! fakesink");
  fail_if (GSTD_EOK != ret, "A failed build kept its slot (%d)", ret);

  gst_object_unref (node);
  gst_object_unref (test_session);
}

GST_END_TEST;

static Suite *
gstd_pipeline_limit_suite (void)
{
  Suite *suite = suite_create ("gstd_pipeline_limit");
  TCase *tc = tcase_create ("general");

  /* gstd_session_init applies this env var; a developer shell exporting
   * it must not change what these tests observe */
  g_unsetenv ("GSTD_MAX_PIPELINES");

  suite_add_tcase (suite, tc);
  tcase_set_timeout (tc, 30);

  tcase_add_test (tc, test_pipeline_limit_enforced);
  tcase_add_test (tc, test_pipeline_limit_unlimited_by_default);
  tcase_add_test (tc, test_pipeline_limit_zero_disables);
  tcase_add_test (tc, test_pipeline_limit_concurrent_creates_reserve);
  tcase_add_test (tc, test_pipeline_limit_failed_create_releases_slot);
  tcase_add_test (tc, test_pipeline_limit_bad_pipeline_releases_slot);

  return suite;
}

GST_CHECK_MAIN (gstd_pipeline_limit);
