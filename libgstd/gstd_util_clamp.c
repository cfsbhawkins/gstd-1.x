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

#ifdef __linux__
#include <dirent.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

/* A thread created during a scan copies its parent's clamp before it is
 * listed, and a thread exiting can make the listing skip a sibling, so
 * scans repeat until one changes nothing, up to this many times. */
#define GSTD_UTIL_CLAMP_MAX_PASSES 4

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
  return error == ENOSYS || error == E2BIG || error == EINVAL
      || error == EOPNOTSUPP || error == ENOTSUP;
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

/* Since Linux 5.11 this clamp value returns a thread to the kernel
 * default, including the real-time boost; older kernels reject it. */
#define GSTD_UTIL_CLAMP_RESET ((uint32_t) -1)

typedef enum
{
  GSTD_UTIL_CLAMP_SKIPPED,
  GSTD_UTIL_CLAMP_CHANGED,
  GSTD_UTIL_CLAMP_FAILED,
} GstdUtilClampResult;

/* Changes only the minimum clamp of one thread: policy, priority, nice
 * and the maximum clamp are kept as they are. A raise skips a thread
 * already at or above \p value; a release skips one no longer at it. */
static GstdUtilClampResult
gstd_util_clamp_thread (pid_t tid, guint value, gboolean raise, gint * error)
{
  struct gstd_sched_attr attr = { 0 };

  if (syscall (SYS_sched_getattr, tid, &attr, sizeof (attr), 0) != 0)
    goto failed;

  if (raise ? attr.sched_util_min >= value : attr.sched_util_min != value)
    return GSTD_UTIL_CLAMP_SKIPPED;

  attr.size = sizeof (attr);
  attr.sched_flags = GSTD_SCHED_FLAG_KEEP_POLICY | GSTD_SCHED_FLAG_KEEP_PARAMS
      | GSTD_SCHED_FLAG_UTIL_CLAMP_MIN;
  attr.sched_util_min = raise ? value : GSTD_UTIL_CLAMP_RESET;

  if (syscall (SYS_sched_setattr, tid, &attr, 0) == 0)
    return GSTD_UTIL_CLAMP_CHANGED;

  if (!raise && errno == EINVAL) {
    attr.sched_util_min = 0;
    if (syscall (SYS_sched_setattr, tid, &attr, 0) == 0)
      return GSTD_UTIL_CLAMP_CHANGED;
  }

failed:
  *error = errno;
  return GSTD_UTIL_CLAMP_FAILED;
}

static gint
gstd_util_clamp_all (guint value, gboolean raise, guint * updated)
{
  gint first_error = 0;
  guint changed_total = 0;
  guint pass;

  for (pass = 0; pass < GSTD_UTIL_CLAMP_MAX_PASSES; pass++) {
    DIR *tasks;
    struct dirent *entry;
    guint changed = 0;

    tasks = opendir ("/proc/self/task");
    if (!tasks) {
      if (first_error == 0)
        first_error = errno;
      break;
    }

    while ((entry = readdir (tasks)) != NULL) {
      gchar *end = NULL;
      guint64 tid;
      gint error = 0;

      if (!g_ascii_isdigit (entry->d_name[0]))
        continue;
      tid = g_ascii_strtoull (entry->d_name, &end, 10);
      if (!end || *end != '\0' || tid == 0)
        continue;

      switch (gstd_util_clamp_thread ((pid_t) tid, value, raise, &error)) {
        case GSTD_UTIL_CLAMP_CHANGED:
          changed++;
          break;
        case GSTD_UTIL_CLAMP_FAILED:
          /* A thread that exited since the list was read needs nothing */
          if (error != ESRCH && first_error == 0)
            first_error = error;
          break;
        case GSTD_UTIL_CLAMP_SKIPPED:
          break;
      }
    }
    closedir (tasks);

    changed_total += changed;
    /* A failing thread fails the same way on every pass */
    if (changed == 0 || first_error != 0)
      break;
  }

  if (updated)
    *updated = changed_total;
  return first_error;
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
