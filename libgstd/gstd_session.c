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

#include "gstd_session.h"
#include "gstd_list.h"
#include "gstd_tcp.h"
#include "gstd_pipeline_creator.h"
#include "gstd_property_reader.h"
#include "gstd_list_reader.h"
#include "gstd_pipeline_deleter.h"
#include "gstd_util_clamp.h"

#include <errno.h>

/* Gstd Session debugging category */
GST_DEBUG_CATEGORY_STATIC (gstd_session_debug);
#define GST_CAT_DEFAULT gstd_session_debug

#define GSTD_DEBUG_DEFAULT_LEVEL GST_LEVEL_INFO

GMutex singleton_mutex;

enum
{
  PROP_PIPELINES = 1,
  PROP_PID,
  PROP_DEBUG,
  N_PROPERTIES                  // NOT A PROPERTY
};

#define GSTD_SESSION_DEFAULT_PIPELINES NULL
#define GSTD_DEFAULT_PID -1

G_DEFINE_TYPE (GstdSession, gstd_session, GSTD_TYPE_OBJECT);

/* VTable */
static void
gstd_session_set_property (GObject *, guint, const GValue *, GParamSpec *);
static void gstd_session_get_property (GObject *, guint, GValue *,
    GParamSpec *);
static void gstd_session_dispose (GObject *);
static void gstd_session_finalize (GObject *);
static void gstd_session_on_pipeline_count (GObject *, GParamSpec *,
    gpointer);
static void gstd_session_update_util_clamp (GstdSession *, GObject *);

/* Singleton instance using thread-safe weak reference */
static GWeakRef the_session_ref;
static gboolean the_session_ref_initialized = FALSE;

static void
gstd_session_class_init (GstdSessionClass * klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GParamSpec *properties[N_PROPERTIES] = { NULL, };
  guint debug_color;

  object_class->set_property = gstd_session_set_property;
  object_class->get_property = gstd_session_get_property;
  object_class->dispose = gstd_session_dispose;
  object_class->finalize = gstd_session_finalize;

  properties[PROP_PIPELINES] =
      g_param_spec_object ("pipelines",
      "Pipelines",
      "The pipelines created by the user",
      GSTD_TYPE_LIST,
      G_PARAM_READWRITE |
      G_PARAM_STATIC_STRINGS |
      GSTD_PARAM_CREATE | GSTD_PARAM_READ | GSTD_PARAM_DELETE);

  properties[PROP_PID] =
      g_param_spec_int ("pid",
      "PID",
      "The session process identifier",
      G_MININT,
      G_MAXINT, GSTD_DEFAULT_PID, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  properties[PROP_DEBUG] =
      g_param_spec_object ("debug",
      "Debug",
      "The debug object containing debug information",
      GSTD_TYPE_DEBUG, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPERTIES, properties);

  /* Initialize debug category with nice colors */
  debug_color = GST_DEBUG_FG_BLACK | GST_DEBUG_BOLD | GST_DEBUG_BG_WHITE;
  GST_DEBUG_CATEGORY_INIT (gstd_session_debug, "gstdsession", debug_color,
      "Gstd Session category");
}

static void
gstd_session_init (GstdSession * self)
{
  GST_INFO_OBJECT (self, "Initializing gstd session");

  gstd_object_set_reader (GSTD_OBJECT (self),
      g_object_new (GSTD_TYPE_PROPERTY_READER, NULL));

  self->pipelines =
      GSTD_LIST (g_object_new (GSTD_TYPE_LIST, "name", "pipelines", "node-type",
          GSTD_TYPE_PIPELINE, "flags",
          GSTD_PARAM_CREATE | GSTD_PARAM_READ | GSTD_PARAM_UPDATE |
          GSTD_PARAM_DELETE, NULL));

  gstd_object_set_creator (GSTD_OBJECT (self->pipelines),
      g_object_new (GSTD_TYPE_PIPELINE_CREATOR, NULL));

  gstd_object_set_reader (GSTD_OBJECT (self->pipelines),
      g_object_new (GSTD_TYPE_LIST_READER, NULL));

  gstd_object_set_deleter (GSTD_OBJECT (self->pipelines),
      g_object_new (GSTD_TYPE_PIPELINE_DELETER, NULL));

  /* Optional cap on simultaneous pipelines, as a resource-exhaustion
   * guard for the unauthenticated API. The --max-pipelines option
   * overrides this env var; both default to unlimited. */
  {
    const gchar *max_env = g_getenv ("GSTD_MAX_PIPELINES");
    if (max_env && max_env[0] != '\0') {
      gchar *end = NULL;
      guint64 max = g_ascii_strtoull (max_env, &end, 10);
      /* The whole string must be the number: "10abc" is a typo, not 10 */
      if (end && *end == '\0' && max > 0 && max <= G_MAXUINT) {
        g_object_set (self->pipelines, "max-children", (guint) max, NULL);
        GST_INFO_OBJECT (self, "Limiting pipelines to %u (GSTD_MAX_PIPELINES)",
            (guint) max);
      } else {
        GST_WARNING_OBJECT (self, "Ignoring invalid GSTD_MAX_PIPELINES \"%s\"",
            max_env);
      }
    }
  }

  /* Optional CPU clock floor while pipelines exist. A governor that
   * judges load per core can keep clocks low while a pipeline spread over
   * several threads falls behind real time. A minimum utilization clamp
   * asks a governor that honors it to clock up whenever gstd's threads
   * run. Without kernel support or the privilege to set it, gstd warns
   * once and runs unclamped. */
  g_mutex_init (&self->util_clamp_mutex);
  {
    const gchar *clamp_env = g_getenv ("GSTD_PIPELINE_UTIL_CLAMP_MIN");
    if (clamp_env && clamp_env[0] != '\0') {
      guint clamp = 0;
      if (gstd_util_clamp_parse (clamp_env, &clamp)) {
        self->util_clamp_min = clamp;
        self->util_clamp = gstd_util_clamp_new (clamp);
        self->util_clamp_settled = TRUE;
        g_signal_connect_object (self->pipelines, "notify::count",
            G_CALLBACK (gstd_session_on_pipeline_count), self, 0);
        GST_INFO_OBJECT (self, "Pipelines will hold a minimum CPU "
            "utilization clamp of %u/%u (GSTD_PIPELINE_UTIL_CLAMP_MIN)",
            clamp, GSTD_UTIL_CLAMP_SCALE);
      } else {
        GST_WARNING_OBJECT (self,
            "Ignoring invalid GSTD_PIPELINE_UTIL_CLAMP_MIN \"%s\" "
            "(expected 1 to %u)", clamp_env, GSTD_UTIL_CLAMP_SCALE);
      }
    }
  }

  self->debug =
      GSTD_DEBUG (g_object_new (GSTD_TYPE_DEBUG, "name", "Debug", NULL));

  self->pid = (GPid) getpid ();
}

static void
gstd_session_get_property (GObject * object,
    guint property_id, GValue * value, GParamSpec * pspec)
{
  GstdSession *self = GSTD_SESSION (object);

  switch (property_id) {
    case PROP_PIPELINES:
      GST_DEBUG_OBJECT (self, "Returning pipeline list %p", self->pipelines);
      g_value_set_object (value, self->pipelines);
      break;
    case PROP_PID:
      GST_DEBUG_OBJECT (self, "Returning pid %d", self->pid);
      g_value_set_int (value, self->pid);
      break;
    case PROP_DEBUG:
      GST_DEBUG_OBJECT (self, "Returning debug object %p", self->debug);
      g_value_set_object (value, self->debug);
      break;

    default:
      /* We don't have any other property... */
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static void
gstd_session_set_property (GObject * object,
    guint property_id, const GValue * value, GParamSpec * pspec)
{
  GstdSession *self = GSTD_SESSION (object);

  switch (property_id) {
    case PROP_PIPELINES:
    {
      GstdList *old;

      /* Swapped under the clamp's mutex, so a count change still in
       * flight from the old list can tell it is no longer the session's */
      g_mutex_lock (&self->util_clamp_mutex);
      old = self->pipelines;
      self->pipelines = g_value_dup_object (value);
      g_mutex_unlock (&self->util_clamp_mutex);
      GST_INFO_OBJECT (self, "Changed pipeline list to %p", self->pipelines);

      if (old) {
        g_signal_handlers_disconnect_by_func (old,
            gstd_session_on_pipeline_count, self);
        g_object_unref (old);
      }
      /* The clamp follows whichever list the session has, and no list
       * means no pipelines */
      if (self->util_clamp) {
        if (self->pipelines)
          g_signal_connect_object (self->pipelines, "notify::count",
              G_CALLBACK (gstd_session_on_pipeline_count), self, 0);
        gstd_session_update_util_clamp (self, NULL);
      }
      break;
    }
    case PROP_DEBUG:
      if (self->debug) {
        g_object_unref (self->debug);
      }
      self->debug = g_value_dup_object (value);
      GST_DEBUG_OBJECT (self, "Changing debug object to %p", self->debug);
      break;

    default:
      /* We don't have any other property... */
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static void
gstd_session_on_pipeline_count (GObject * list, GParamSpec * pspec,
    gpointer user_data)
{
  gstd_session_update_util_clamp (GSTD_SESSION (user_data), list);
}

/* Holds the clamp while the session has pipelines. Runs on whichever
 * thread created or deleted a pipeline, so it reconciles against the
 * current count under the mutex rather than trusting the order in which
 * concurrent notifications arrive. A transition that did not reach every
 * thread is retried at the next count change, except when the kernel has
 * no utilization clamping or refuses every thread, neither of which
 * changes while gstd runs. \p from is the list whose count changed, or
 * NULL when the session itself asks. */
static void
gstd_session_update_util_clamp (GstdSession * self, GObject * from)
{
  guint count = 0;
  guint updated = 0;
  gboolean want;
  gint error;

  g_mutex_lock (&self->util_clamp_mutex);

  if (self->util_clamp_min == 0)
    goto out;

  /* A list the session has since replaced */
  if (from && from != G_OBJECT (self->pipelines))
    goto out;

  /* Creates and deletes change the count under the list's lock. Take
   * only the snapshot under it; the scan below must not hold it. */
  if (self->pipelines) {
    GST_OBJECT_LOCK (self->pipelines);
    count = self->pipelines->count;
    GST_OBJECT_UNLOCK (self->pipelines);
  }

  want = count > 0;
  if (want == self->util_clamp_raised && self->util_clamp_settled)
    goto out;

  if (want) {
    error = gstd_util_clamp_raise (self->util_clamp, &updated);
    /* Nothing to undo, and nothing that would work next time */
    if (error != 0 && !gstd_util_clamp_is_held (self->util_clamp)
        && (gstd_util_clamp_unsupported (error) || error == EPERM)) {
      GST_WARNING_OBJECT (self, "Cannot clamp CPU utilization: %s (errno "
          "%d). Pipelines will run without a clock floor.%s",
          g_strerror (error), error, error == EPERM ?
          " Changing utilization clamps usually requires CAP_SYS_NICE." : "");
      self->util_clamp_min = 0;
      goto out;
    }
    if (error != 0) {
      GST_WARNING_OBJECT (self, "Raised the CPU utilization clamp on %u "
          "threads but not all: %s (errno %d); will retry", updated,
          g_strerror (error), error);
    }
  } else {
    error = gstd_util_clamp_release (self->util_clamp, &updated);
    /* The clamp stays held, so the next release or the session's
     * disposal still covers the threads this one missed */
    if (error != 0) {
      GST_WARNING_OBJECT (self, "Could not release the CPU utilization "
          "clamp on every thread: %s (errno %d); will retry",
          g_strerror (error), error);
    }
  }

  self->util_clamp_raised = want;
  self->util_clamp_settled = error == 0;
  if (error == 0) {
    GST_INFO_OBJECT (self, "%s the CPU utilization clamp on %u threads "
        "(%u pipelines)", want ? "Raised" : "Released", updated, count);
  }

out:
  g_mutex_unlock (&self->util_clamp_mutex);
}

static void
gstd_session_dispose (GObject * object)
{
  GstdSession *self = GSTD_SESSION (object);

  GST_INFO_OBJECT (object, "Deinitializing gstd session");

  if (self->pipelines) {
    GstdList *old;

    g_mutex_lock (&self->util_clamp_mutex);
    old = self->pipelines;
    self->pipelines = NULL;
    g_mutex_unlock (&self->util_clamp_mutex);

    g_signal_handlers_disconnect_by_func (old,
        gstd_session_on_pipeline_count, self);
    g_object_unref (old);
  }

  /* The session is the only owner of the clamp, and nothing will retry
   * once it is gone */
  g_mutex_lock (&self->util_clamp_mutex);
  if (self->util_clamp && gstd_util_clamp_is_held (self->util_clamp)) {
    gint error = 0;
    gint attempt;

    for (attempt = 0; attempt < 3; attempt++) {
      error = gstd_util_clamp_release (self->util_clamp, NULL);
      if (error == 0)
        break;
    }
    if (error != 0) {
      GST_WARNING_OBJECT (self, "Could not release the CPU utilization "
          "clamp on every thread: %s (errno %d)", g_strerror (error), error);
    }
  }
  gstd_util_clamp_free (self->util_clamp);
  self->util_clamp = NULL;
  self->util_clamp_min = 0;
  g_mutex_unlock (&self->util_clamp_mutex);

  if (self->debug) {
    g_object_unref (self->debug);
    self->debug = NULL;
  }

  G_OBJECT_CLASS (gstd_session_parent_class)->dispose (object);
}

static void
gstd_session_finalize (GObject * object)
{
  GstdSession *self = GSTD_SESSION (object);

  g_mutex_clear (&self->util_clamp_mutex);

  G_OBJECT_CLASS (gstd_session_parent_class)->finalize (object);
}

GstdSession *
gstd_session_new (const gchar * name)
{
  GstdSession *self = NULL;

  g_mutex_lock (&singleton_mutex);

  /* Initialize weak ref on first call */
  if (!the_session_ref_initialized) {
    g_weak_ref_init (&the_session_ref, NULL);
    the_session_ref_initialized = TRUE;
  }

  /* Try to get existing session - g_weak_ref_get is atomic and thread-safe */
  self = g_weak_ref_get (&the_session_ref);

  if (self == NULL) {
    /* Create new session */
    if (!name) {
      GPid tempPid = (GPid) getpid ();
      gchar *pid_name = g_strdup_printf ("Session %d", tempPid);
      self =
          GSTD_SESSION (g_object_new (GSTD_TYPE_SESSION, "name", pid_name, NULL));
      g_free (pid_name);
    } else {
      self = GSTD_SESSION (g_object_new (GSTD_TYPE_SESSION, "name", name, NULL));
    }
    /* Store in weak ref - automatically cleared when object is finalized */
    g_weak_ref_set (&the_session_ref, self);
  }
  /* Note: g_weak_ref_get already added a ref, so we don't need to ref again */

  g_mutex_unlock (&singleton_mutex);

  return self;
}

GstdReturnCode
gstd_get_by_uri (GstdSession * gstd, const gchar * uri, GstdObject ** node)
{
  GstdObject *parent, *child;
  gchar **nodes;
  gchar **it;
  GstdReturnCode ret;

  g_return_val_if_fail (GSTD_IS_SESSION (gstd), GSTD_NULL_ARGUMENT);
  g_return_val_if_fail (uri, GSTD_NULL_ARGUMENT);

  nodes = g_strsplit_set (uri, "/", -1);

  if (!nodes)
    goto badcommand;

  it = nodes;
  parent = g_object_ref (GSTD_OBJECT (gstd));

  while (*it) {
    // Empty slash, try no normalize
    if ('\0' == *it[0]) {
      ++it;
      continue;
    }

    ret = gstd_object_read (parent, *it, &child);
    g_object_unref (parent);

    if (ret)
      goto nonode;

    parent = child;
    ++it;
  }

  g_strfreev (nodes);
  *node = parent;
  return GSTD_EOK;

badcommand:
  {
    GST_ERROR_OBJECT (gstd, "Invalid command");
    return GSTD_BAD_COMMAND;
  }
nonode:
  {
    GST_ERROR_OBJECT (gstd, "Invalid node %s", *it);
    g_strfreev (nodes);
    return GSTD_BAD_COMMAND;
  }
}
