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

#include "gstd_util_clamp.h"

#include <errno.h>
#include <string.h>

#ifdef __linux__
#include <dirent.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

/* Scheduling policies that carry their own utilization boost */
#define GSTD_SCHED_FIFO 1
#define GSTD_SCHED_RR 2
#define GSTD_SCHED_DEADLINE 6

/* A thread created during a scan copies its parent's clamp before it is
 * listed, and a thread exiting can make the listing skip a sibling, so
 * scans repeat until the thread set is stable and a pass changes nothing,
 * up to this many times. */
#define GSTD_UTIL_CLAMP_MAX_PASSES 8

gboolean
gstd_util_clamp_parse (const gchar * str, guint * value)
{
  gchar *end = NULL;
  guint64 parsed;

  g_return_val_if_fail (value, FALSE);

  if (!str || !g_ascii_isdigit (str[0]))
    return FALSE;

  parsed = g_ascii_strtoull (str, &end, 10);
  /* The whole string must be the number: "512abc" is a typo, not 512 */
  if (!end || *end != '\0' || parsed < 1 || parsed > GSTD_UTIL_CLAMP_SCALE)
    return FALSE;

  *value = (guint) parsed;
  return TRUE;
}

gboolean
gstd_util_clamp_unsupported (gint error)
{
  return error == ENOSYS || error == E2BIG || error == EOPNOTSUPP
      || error == ENOTSUP;
}

#if defined(__linux__) && defined(SYS_sched_setattr) && defined(SYS_sched_getattr)

/* The kernel's struct sched_attr, as of Linux 5.3. libc headers do not all
 * carry it, and the clamp fields need the 5.3 layout. */
struct gstd_sched_attr
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

#define GSTD_SCHED_FLAG_KEEP_POLICY 0x08
#define GSTD_SCHED_FLAG_KEEP_PARAMS 0x10
#define GSTD_SCHED_FLAG_UTIL_CLAMP_MIN 0x20

/* The kernel's own default since Linux 5.11, where the sysctl appeared */
#define GSTD_UTIL_CLAMP_RT_DEFAULT_FALLBACK GSTD_UTIL_CLAMP_SCALE

static guint
gstd_util_clamp_kernel_rt_default (void)
{
  gchar *contents = NULL;
  guint64 value = GSTD_UTIL_CLAMP_RT_DEFAULT_FALLBACK;

  if (g_file_get_contents ("/proc/sys/kernel/sched_util_clamp_min_rt_default",
          &contents, NULL, NULL)) {
    gchar *end = NULL;
    guint64 parsed = g_ascii_strtoull (g_strstrip (contents), &end, 10);

    if (end && *end == '\0' && parsed <= GSTD_UTIL_CLAMP_SCALE)
      value = parsed;
  }
  g_free (contents);
  return (guint) value;
}

static gint
gstd_util_clamp_kernel_get_attr (gint tid, struct gstd_sched_attr *attr)
{
  memset (attr, 0, sizeof (*attr));
  if (syscall (SYS_sched_getattr, (pid_t) tid, attr, sizeof (*attr), 0) != 0)
    return errno;
  return 0;
}

static gint
gstd_util_clamp_kernel_get (gint tid, GstdUtilClampThread * thread)
{
  struct gstd_sched_attr attr;
  gint error = gstd_util_clamp_kernel_get_attr (tid, &attr);

  if (error != 0)
    return error;

  thread->policy = attr.sched_policy;
  thread->util_min = attr.sched_util_min;
  /* Before Linux 5.3 the kernel fills neither clamp */
  thread->util_max = attr.size >= sizeof (attr) ? attr.sched_util_max
      : GSTD_UTIL_CLAMP_SCALE;
  return 0;
}

/* Changes only the minimum clamp: policy, priority, nice and the maximum
 * clamp are kept as they are. */
static gint
gstd_util_clamp_kernel_set_min (gint tid, guint util_min)
{
  struct gstd_sched_attr attr;
  gint error = gstd_util_clamp_kernel_get_attr (tid, &attr);

  if (error != 0)
    return error;

  attr.size = sizeof (attr);
  attr.sched_flags = GSTD_SCHED_FLAG_KEEP_POLICY | GSTD_SCHED_FLAG_KEEP_PARAMS
      | GSTD_SCHED_FLAG_UTIL_CLAMP_MIN;
  attr.sched_util_min = util_min;

  if (syscall (SYS_sched_setattr, (pid_t) tid, &attr, 0) != 0)
    return errno;
  return 0;
}

static const GstdUtilClampBackend gstd_util_clamp_kernel = {
  gstd_util_clamp_kernel_get,
  gstd_util_clamp_kernel_set_min,
  gstd_util_clamp_kernel_rt_default,
};

static const GstdUtilClampBackend *backend = &gstd_util_clamp_kernel;

void
gstd_util_clamp_set_backend (const GstdUtilClampBackend * replacement)
{
  backend = replacement ? replacement : &gstd_util_clamp_kernel;
}

typedef enum
{
  GSTD_UTIL_CLAMP_SKIPPED,
  GSTD_UTIL_CLAMP_CHANGED,
  GSTD_UTIL_CLAMP_FAILED,
} GstdUtilClampResult;

static gboolean
gstd_util_clamp_is_realtime (guint policy)
{
  return policy == GSTD_SCHED_FIFO || policy == GSTD_SCHED_RR;
}

static GstdUtilClampResult
gstd_util_clamp_thread (gint tid, guint value, gboolean raise, gint * error)
{
  GstdUtilClampThread thread = { 0 };

  *error = backend->get (tid, &thread);
  if (*error != 0)
    return GSTD_UTIL_CLAMP_FAILED;

  /* Deadline threads are scheduled by bandwidth, not utilization */
  if (thread.policy == GSTD_SCHED_DEADLINE)
    return GSTD_UTIL_CLAMP_SKIPPED;

  if (gstd_util_clamp_is_realtime (thread.policy)) {
    guint rt_default;

    /* Writing a clamp makes it user-defined and opts a real-time thread out
     * of the kernel's boost for good, so a raise never touches one */
    if (raise)
      return GSTD_UTIL_CLAMP_SKIPPED;

    /* A real-time thread at exactly gstd's value was clamped before it
     * switched policy, or holds the kernel's own boost when that value is
     * the real-time default. The reset is right for both: it clears the
     * user-defined flag, so a thread that later leaves real-time drops to
     * 0 instead of keeping gstd's floor. */
    if (thread.util_min != value)
      return GSTD_UTIL_CLAMP_SKIPPED;

    *error = backend->set_min (tid, GSTD_UTIL_CLAMP_RESET);
    if (*error == EINVAL) {
      /* Before Linux 5.11 there is no reset, and writing the number sets
       * the flag. At the real-time default that would pin a thread that
       * merely had the kernel's boost, so leave it; otherwise write the
       * default explicitly. */
      rt_default = backend->rt_default ();
      if (value == rt_default) {
        *error = 0;
        return GSTD_UTIL_CLAMP_SKIPPED;
      }
      *error = backend->set_min (tid, rt_default);
    }
  } else if (raise) {
    /* Already enough, or capped below the floor by its maximum clamp */
    if (thread.util_min >= value || thread.util_max < value)
      return GSTD_UTIL_CLAMP_SKIPPED;
    *error = backend->set_min (tid, value);
  } else {
    /* Another clamp has replaced gstd's since */
    if (thread.util_min != value)
      return GSTD_UTIL_CLAMP_SKIPPED;
    *error = backend->set_min (tid, GSTD_UTIL_CLAMP_RESET);
    /* Before Linux 5.11 there is no reset; 0 is the default here */
    if (*error == EINVAL)
      *error = backend->set_min (tid, 0);
  }

  return *error == 0 ? GSTD_UTIL_CLAMP_CHANGED : GSTD_UTIL_CLAMP_FAILED;
}

static gint
gstd_util_clamp_compare_tids (gconstpointer a, gconstpointer b)
{
  gint left = *(const gint *) a;
  gint right = *(const gint *) b;

  return left < right ? -1 : left > right;
}

/* The ids of this process's threads, sorted, or NULL with \p error set */
static GArray *
gstd_util_clamp_list_threads (gint * error)
{
  GArray *tids;
  DIR *tasks;
  struct dirent *entry;

  tasks = opendir ("/proc/self/task");
  if (!tasks) {
    *error = errno;
    return NULL;
  }

  tids = g_array_new (FALSE, FALSE, sizeof (gint));
  while ((entry = readdir (tasks)) != NULL) {
    gchar *end = NULL;
    guint64 tid;
    gint value;

    if (!g_ascii_isdigit (entry->d_name[0]))
      continue;
    tid = g_ascii_strtoull (entry->d_name, &end, 10);
    if (!end || *end != '\0' || tid == 0 || tid > G_MAXINT)
      continue;
    value = (gint) tid;
    g_array_append_val (tids, value);
  }
  closedir (tasks);

  g_array_sort (tids, gstd_util_clamp_compare_tids);
  return tids;
}

static gboolean
gstd_util_clamp_same_tids (GArray * a, GArray * b)
{
  return a && b && a->len == b->len
      && memcmp (a->data, b->data, a->len * sizeof (gint)) == 0;
}

static gint
gstd_util_clamp_all (guint value, gboolean raise, guint * updated)
{
  GArray *previous = NULL;
  gint error = 0;
  guint changed_total = 0;
  guint pass;

  for (pass = 0; pass < GSTD_UTIL_CLAMP_MAX_PASSES; pass++) {
    GArray *tids;
    guint changed = 0;
    guint i;

    error = 0;
    tids = gstd_util_clamp_list_threads (&error);
    if (!tids)
      break;

    for (i = 0; i < tids->len; i++) {
      gint thread_error = 0;

      switch (gstd_util_clamp_thread (g_array_index (tids, gint, i), value,
              raise, &thread_error)) {
        case GSTD_UTIL_CLAMP_CHANGED:
          changed++;
          break;
        case GSTD_UTIL_CLAMP_FAILED:
          /* A thread that exited since the list was read needs nothing */
          if (thread_error != ESRCH && error == 0)
            error = thread_error;
          break;
        case GSTD_UTIL_CLAMP_SKIPPED:
          break;
      }
    }

    changed_total += changed;
    /* Done once a pass saw the same threads as the last one and had
     * nothing left to change. A pass that only failed again is no
     * progress either. */
    if (changed == 0 && gstd_util_clamp_same_tids (previous, tids)) {
      g_array_unref (tids);
      break;
    }
    if (previous)
      g_array_unref (previous);
    previous = tids;
  }

  if (previous)
    g_array_unref (previous);
  if (updated)
    *updated = changed_total;
  return error;
}

gint
gstd_util_clamp_raise (guint value, guint * updated)
{
  g_return_val_if_fail (value >= 1 && value <= GSTD_UTIL_CLAMP_SCALE, EINVAL);
  return gstd_util_clamp_all (value, TRUE, updated);
}

gint
gstd_util_clamp_release (guint value, guint * updated)
{
  g_return_val_if_fail (value >= 1 && value <= GSTD_UTIL_CLAMP_SCALE, EINVAL);
  return gstd_util_clamp_all (value, FALSE, updated);
}

#else

void
gstd_util_clamp_set_backend (const GstdUtilClampBackend * replacement)
{
  (void) replacement;
}

gint
gstd_util_clamp_raise (guint value, guint * updated)
{
  (void) value;
  if (updated)
    *updated = 0;
  return ENOTSUP;
}

gint
gstd_util_clamp_release (guint value, guint * updated)
{
  (void) value;
  if (updated)
    *updated = 0;
  return ENOTSUP;
}

#endif
