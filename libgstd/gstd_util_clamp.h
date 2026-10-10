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

#ifndef __GSTD_UTIL_CLAMP_H__
#define __GSTD_UTIL_CLAMP_H__

#include <gst/gst.h>

G_BEGIN_DECLS

/**
 * Highest utilization clamp value the kernel accepts. A minimum clamp of
 * this value asks the CPU frequency governor for full speed whenever a
 * clamped thread runs.
 */
#define GSTD_UTIL_CLAMP_SCALE 1024

/**
 * Parses a minimum utilization clamp, as given to GSTD_PIPELINE_UTIL_CLAMP_MIN.
 *
 * \param str The value to parse
 * \param value Where to store the parsed clamp
 *
 * \return TRUE if the whole of \p str is a number from 1 to
 * GSTD_UTIL_CLAMP_SCALE, FALSE otherwise.
 */
gboolean gstd_util_clamp_parse (const gchar * str, guint * value);

/**
 * Raises the minimum utilization clamp of every thread in this process
 * that is below \p value to \p value. Threads already at or above it,
 * such as real-time threads under the kernel's default boost, are left
 * alone. Threads created afterwards inherit the clamp of the thread that
 * creates them.
 *
 * \param value The minimum clamp, from 1 to GSTD_UTIL_CLAMP_SCALE
 * \param updated (out) (optional) How many threads were changed
 *
 * \return 0 when every thread is at or above \p value, otherwise the errno
 * of the first thread that could not be changed.
 */
gint gstd_util_clamp_raise (guint value, guint * updated);

/**
 * Returns every thread whose minimum clamp is still exactly \p value to
 * the kernel default, undoing gstd_util_clamp_raise() without touching a
 * clamp something else has set since.
 *
 * \param value The value gstd_util_clamp_raise() set
 * \param updated (out) (optional) How many threads were changed
 *
 * \return 0 on success, otherwise the errno of the first thread that could
 * not be changed.
 */
gint gstd_util_clamp_release (guint value, guint * updated);

/**
 * \param error An errno returned by this module
 *
 * \return TRUE if \p error means the kernel cannot clamp utilization at
 * all: before Linux 5.3, or built without CONFIG_UCLAMP_TASK.
 */
gboolean gstd_util_clamp_unsupported (gint error);

G_END_DECLS
#endif //__GSTD_UTIL_CLAMP_H__
