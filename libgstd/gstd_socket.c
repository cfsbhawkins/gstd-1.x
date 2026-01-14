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

#include <string.h>

#include "gstd_parser.h"

#include "gstd_socket.h"

/* Gstd SOCKET debugging category */
GST_DEBUG_CATEGORY_STATIC (gstd_socket_debug);
#define GST_CAT_DEFAULT gstd_socket_debug

#define GSTD_DEBUG_DEFAULT_LEVEL GST_LEVEL_INFO

G_DEFINE_TYPE (GstdSocket, gstd_socket, GSTD_TYPE_IPC);

/* VTable */

static gboolean
gstd_socket_callback (GSocketService * service,
    GSocketConnection * connection,
    GObject * source_object, gpointer user_data);
static void gstd_socket_dispose (GObject *);
static GstdReturnCode gstd_socket_start (GstdIpc * base, GstdSession * session);
static GstdReturnCode gstd_socket_stop (GstdIpc * base);

static void
gstd_socket_class_init (GstdSocketClass * klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GstdIpcClass *gstdipc_class = GSTD_IPC_CLASS (klass);
  guint debug_color;
  gstdipc_class->start = GST_DEBUG_FUNCPTR (gstd_socket_start);
  gstdipc_class->stop = GST_DEBUG_FUNCPTR (gstd_socket_stop);
  object_class->dispose = gstd_socket_dispose;

  /* Initialize debug category with nice colors */
  debug_color = GST_DEBUG_FG_BLACK | GST_DEBUG_BOLD | GST_DEBUG_BG_WHITE;
  GST_DEBUG_CATEGORY_INIT (gstd_socket_debug, "gstdsocket", debug_color,
      "Gstd SOCKET category");
}

static void
gstd_socket_init (GstdSocket * self)
{
  GstdIpc *base = GSTD_IPC (self);
  GST_INFO_OBJECT (self, "Initializing gstd Socket");
  self->service = NULL;
  base->enabled = FALSE;
}

static void
gstd_socket_dispose (GObject * object)
{
  GST_INFO_OBJECT (object, "Deinitializing gstd SOCKET");

  G_OBJECT_CLASS (gstd_socket_parent_class)->dispose (object);
}



static gboolean
gstd_socket_callback (GSocketService * service,
    GSocketConnection * connection, GObject * source_object, gpointer user_data)
{
  GstdSession *session;
  GInputStream *istream;
  GOutputStream *ostream;
  gint read;
  const guint size = 1024 * 1024;
  gchar *output = NULL;
  gchar *response = NULL;
  gchar *message;
  GstdReturnCode ret;
  const gchar *description = NULL;
  GError *error = NULL;
  GSocketAddress *remote_addr = NULL;
  gchar *client_info = NULL;
  guint command_count = 0;

  g_return_val_if_fail (service, FALSE);
  g_return_val_if_fail (connection, FALSE);
  g_return_val_if_fail (user_data, FALSE);

  session = GSTD_SESSION (user_data);
  g_return_val_if_fail (session, FALSE);

  /* Log client connection */
  remote_addr = g_socket_connection_get_remote_address (connection, NULL);
  if (remote_addr && G_IS_INET_SOCKET_ADDRESS (remote_addr)) {
    GInetAddress *inet_addr;
    gchar *addr_str;
    guint16 port;
    inet_addr = g_inet_socket_address_get_address (
        G_INET_SOCKET_ADDRESS (remote_addr));
    port = g_inet_socket_address_get_port (G_INET_SOCKET_ADDRESS (remote_addr));
    addr_str = g_inet_address_to_string (inet_addr);
    client_info = g_strdup_printf ("%s:%u", addr_str, port);
    g_free (addr_str);
    GST_DEBUG_OBJECT (session, "Client connected: %s", client_info);
    g_object_unref (remote_addr);
  } else {
    client_info = g_strdup ("unknown");
    GST_DEBUG_OBJECT (session, "Client connected (address unavailable)");
    if (remote_addr)
      g_object_unref (remote_addr);
  }

  istream = g_io_stream_get_input_stream (G_IO_STREAM (connection));
  ostream = g_io_stream_get_output_stream (G_IO_STREAM (connection));

  message = g_malloc (size);

  while (TRUE) {
    read = g_input_stream_read (istream, message, size, NULL, &error);

    /* Was connection closed or error? */
    if (read <= 0) {
      if (read < 0 && error) {
        GST_WARNING_OBJECT (session, "Read error from %s: %s",
            client_info, error->message);
        g_error_free (error);
        error = NULL;
      } else if (read == 0) {
        GST_DEBUG_OBJECT (session, "Client %s closed connection after %u commands",
            client_info, command_count);
      }
      break;
    }
    message[read] = '\0';
    command_count++;

    GST_DEBUG_OBJECT (session, "Received command from %s: %.80s%s",
        client_info, message, strlen (message) > 80 ? "..." : "");

    ret = gstd_parser_parse_cmd (session, message, &output);

    /* Log command result at appropriate level */
    if (ret != GSTD_EOK) {
      GST_WARNING_OBJECT (session, "Command from %s failed: %s (code %d)",
          client_info, gstd_return_code_to_string (ret), ret);
    } else {
      GST_DEBUG_OBJECT (session, "Command from %s succeeded", client_info);
    }

    /* Prepend the code to the output */
    description = gstd_return_code_to_string (ret);
    response =
        g_strdup_printf
        ("{\n  \"code\" : %d,\n  \"description\" : \"%s\",\n  \"response\" : %s\n}",
        ret, description, output ? output : "null");
    g_free (output);
    output = NULL;

    read =
        g_output_stream_write (ostream, response, strlen (response) + 1, NULL,
        &error);
    g_free (response);
    response = NULL;

    if (read < 0) {
      if (error) {
        GST_WARNING_OBJECT (session, "Write error to %s: %s",
            client_info, error->message);
        g_error_free (error);
        error = NULL;
      }
      break;
    }
  }

  g_free (message);

  /* Properly close the connection to release file descriptors */
  if (!g_io_stream_close (G_IO_STREAM (connection), NULL, &error)) {
    if (error) {
      GST_WARNING_OBJECT (session, "Error closing connection to %s: %s",
          client_info, error->message);
      g_error_free (error);
    }
  }

  GST_DEBUG_OBJECT (session, "Client disconnected: %s (processed %u commands)",
      client_info, command_count);
  g_free (client_info);

  return TRUE;
}

static GstdReturnCode
gstd_socket_start (GstdIpc * base, GstdSession * session)
{
  GstdSocket *self = GSTD_SOCKET (base);
  GSocketService *service;
  GstdReturnCode ret;

  GST_DEBUG_OBJECT (self, "Starting SOCKET");

  /* Close any existing connection */
  gstd_socket_stop (base);

  service = self->service;

  ret = GSTD_SOCKET_GET_CLASS (self)->create_socket_service (self, &service);

  if (ret != GSTD_EOK)
    return ret;

  /* listen to the 'incoming' signal */
  g_signal_connect (service, "run", G_CALLBACK (gstd_socket_callback), session);

  /* start the socket service */
  g_socket_service_start (service);

  return GSTD_EOK;
}

static GstdReturnCode
gstd_socket_stop (GstdIpc * base)
{
  GstdSocket *self = GSTD_SOCKET (base);
  GSocketService *service;
  GstdSession *session = base->session;
  GSocketListener *listener;

  g_return_val_if_fail (session, GSTD_NULL_ARGUMENT);

  GST_DEBUG_OBJECT (self, "Entering SOCKET stop ");
  if (self->service) {
    service = self->service;
    self->service = NULL;  /* Clear before cleanup to prevent double-free */
    listener = G_SOCKET_LISTENER (service);
    GST_INFO_OBJECT (session, "Closing SOCKET connection for %s",
        GSTD_OBJECT_NAME (session));
    g_socket_listener_close (listener);
    g_socket_service_stop (service);
    g_object_unref (service);
  }
  return GSTD_EOK;
}
