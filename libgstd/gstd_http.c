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

#include <stdio.h>
#include <string.h>
#include <gst/gst.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

#include "gstd_http.h"
#include "gstd_parser.h"
#include "gstd_pipeline.h"
#include "gstd_session.h"

/* Gstd HTTP debugging category */
GST_DEBUG_CATEGORY_STATIC (gstd_http_debug);
#define GST_CAT_DEFAULT gstd_http_debug

#define GSTD_DEBUG_DEFAULT_LEVEL GST_LEVEL_INFO

#if SOUP_CHECK_VERSION(3,0,0)
typedef SoupServerMessage SoupMsg;
#else
typedef SoupMessage SoupMsg;
#endif

typedef struct _GstdHttpRequest
{
  SoupServer *server;
  SoupMsg *msg;
  GstdSession *session;
  const char *path;
  GHashTable *query;
  GMutex *mutex;
} GstdHttpRequest;

struct _GstdHttp
{
  GstdIpc parent;
  guint port;
  gchar *address;
  gint max_threads;
  SoupServer *server;
  GstdSession *session;
  GThreadPool *pool;
  GMutex mutex;
};

struct _GstdHttpClass
{
  GstdIpcClass parent_class;
};

G_DEFINE_TYPE (GstdHttp, gstd_http, GSTD_TYPE_IPC);

/* VTable */

static void gstd_http_finalize (GObject *);
static GstdReturnCode gstd_http_start (GstdIpc * base, GstdSession * session);
static GstdReturnCode gstd_http_stop (GstdIpc * base);
static gboolean gstd_http_init_get_option_group (GstdIpc * base,
    GOptionGroup ** group);
static SoupStatus get_status_code (GstdReturnCode ret);
static GstdReturnCode do_get (SoupServer * server, SoupMsg * msg,
    char **output, const char *path, GstdSession * session);
static GstdReturnCode do_post (SoupServer * server, SoupMsg * msg,
    char *name, char *description, char **output, const char *path,
    GstdSession * session);
static GstdReturnCode do_put (SoupServer * server, SoupMsg * msg,
    char *name, char **output, const char *path, GstdSession * session);
static GstdReturnCode do_delete (SoupServer * server, SoupMsg * msg,
    char *name, char **output, const char *path, GstdSession * session);
static void do_request (gpointer data_request, gpointer eval);
static void parse_json_body (SoupMsg *msg, gchar **out_name, gchar **out_desc);
#if SOUP_CHECK_VERSION(3,0,0)
static void server_callback (SoupServer * server, SoupMsg * msg,
    const char *path, GHashTable * query, gpointer data);
#else
static void server_callback (SoupServer * server, SoupMessage * msg,
    const char *path, GHashTable * query, SoupClientContext * context,
    gpointer data);
#endif

static void
gstd_http_class_init (GstdHttpClass * klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GstdIpcClass *gstdipc_class = GSTD_IPC_CLASS (klass);
  guint debug_color;

  gstdipc_class->get_option_group =
      GST_DEBUG_FUNCPTR (gstd_http_init_get_option_group);
  gstdipc_class->start = GST_DEBUG_FUNCPTR (gstd_http_start);
  object_class->finalize = gstd_http_finalize;
  gstdipc_class->stop = GST_DEBUG_FUNCPTR (gstd_http_stop);

  /* Initialize debug category with nice colors */
  debug_color = GST_DEBUG_FG_BLACK | GST_DEBUG_BOLD | GST_DEBUG_BG_WHITE;
  GST_DEBUG_CATEGORY_INIT (gstd_http_debug, "gstdhttp", debug_color,
      "Gstd HTTP category");
}

static void
gstd_http_init (GstdHttp * self)
{
  GST_INFO_OBJECT (self, "Initializing gstd Http");
  g_mutex_init (&self->mutex);
  self->port = GSTD_HTTP_DEFAULT_PORT;
  self->address = g_strdup (GSTD_HTTP_DEFAULT_ADDRESS);
  self->max_threads = GSTD_HTTP_DEFAULT_MAX_THREADS;
  self->server = NULL;
  self->session = NULL;
  self->pool = NULL;

}

static void
gstd_http_finalize (GObject * object)
{
  GstdHttp *self = GSTD_HTTP (object);
  GstdIpc *ipc = GSTD_IPC (object);

  GST_INFO_OBJECT (object, "Deinitializing gstd HTTP");

  if (ipc->enabled) {
    gstd_http_stop (ipc);
  }

  g_mutex_clear (&self->mutex);

  if (self->address) {
    g_free (self->address);
    self->address = NULL;
  }

  if (self->pool) {
    g_thread_pool_free (self->pool, FALSE, TRUE);
    self->pool = NULL;
  }

  G_OBJECT_CLASS (gstd_http_parent_class)->finalize (object);
}

static SoupStatus
get_status_code (GstdReturnCode ret)
{
  SoupStatus status = SOUP_STATUS_OK;

  if (ret == GSTD_EOK) {
    status = SOUP_STATUS_OK;
  } else if (ret == GSTD_BAD_COMMAND || ret == GSTD_NO_RESOURCE) {
    status = SOUP_STATUS_NOT_FOUND;
  } else if (ret == GSTD_EXISTING_RESOURCE) {
    status = SOUP_STATUS_CONFLICT;
  } else if (ret == GSTD_BAD_VALUE) {
    status = SOUP_STATUS_NO_CONTENT;
  } else {
    status = SOUP_STATUS_BAD_REQUEST;
  }

  return status;
}

static GstdReturnCode
do_get (SoupServer * server, SoupMsg * msg, char **output, const char *path,
    GstdSession * session)
{
  gchar *message = NULL;
  GstdReturnCode ret = GSTD_EOK;

  g_return_val_if_fail (server, GSTD_NULL_ARGUMENT);
  g_return_val_if_fail (msg, GSTD_NULL_ARGUMENT);
  g_return_val_if_fail (session, GSTD_NULL_ARGUMENT);
  g_return_val_if_fail (output, GSTD_NULL_ARGUMENT);
  g_return_val_if_fail (path, GSTD_NULL_ARGUMENT);

  message = g_strdup_printf ("read %s", path);
  ret = gstd_parser_parse_cmd (session, message, output);
  g_free (message);
  message = NULL;

  return ret;
}

static GstdReturnCode
do_post (SoupServer * server, SoupMsg * msg, char *name,
    char *description, char **output, const char *path, GstdSession * session)
{
  gchar *message = NULL;
  GstdReturnCode ret = GSTD_EOK;

  g_return_val_if_fail (server, GSTD_NULL_ARGUMENT);
  g_return_val_if_fail (msg, GSTD_NULL_ARGUMENT);
  g_return_val_if_fail (session, GSTD_NULL_ARGUMENT);
  g_return_val_if_fail (path, GSTD_NULL_ARGUMENT);
  g_return_val_if_fail (name, GSTD_NULL_ARGUMENT);
  g_return_val_if_fail (output, GSTD_NULL_ARGUMENT);

  if (!name) {
    ret = GSTD_BAD_VALUE;
    GST_ERROR_OBJECT (session,
        "Wrong query param provided, \"name\" doesn't exist");
    goto out;
  }

  if (description) {
    message = g_strdup_printf ("create %s %s %s", path, name, description);
  } else {
    message = g_strdup_printf ("create %s %s", path, name);
  }

  ret = gstd_parser_parse_cmd (session, message, output);
  g_free (message);
  message = NULL;

out:
  return ret;
}

static GstdReturnCode
do_put (SoupServer * server, SoupMsg * msg, char *name, char **output,
    const char *path, GstdSession * session)
{
  gchar *message = NULL;
  GstdReturnCode ret = GSTD_EOK;

  g_return_val_if_fail (server, GSTD_NULL_ARGUMENT);
  g_return_val_if_fail (msg, GSTD_NULL_ARGUMENT);
  g_return_val_if_fail (session, GSTD_NULL_ARGUMENT);
  g_return_val_if_fail (name, GSTD_NULL_ARGUMENT);
  g_return_val_if_fail (output, GSTD_NULL_ARGUMENT);
  g_return_val_if_fail (path, GSTD_NULL_ARGUMENT);

  if (!name) {
    ret = GSTD_BAD_VALUE;
    GST_ERROR_OBJECT (session,
        "Wrong query param provided, \"name\" doesn't exist");
    goto out;
  }

  message = g_strdup_printf ("update %s %s", path, name);
  ret = gstd_parser_parse_cmd (session, message, output);
  g_free (message);
  message = NULL;

out:
  return ret;
}

static GstdReturnCode
do_delete (SoupServer * server, SoupMsg * msg, char *name,
    char **output, const char *path, GstdSession * session)
{
  gchar *message = NULL;
  GstdReturnCode ret = GSTD_EOK;

  g_return_val_if_fail (server, GSTD_NULL_ARGUMENT);
  g_return_val_if_fail (msg, GSTD_NULL_ARGUMENT);
  g_return_val_if_fail (session, GSTD_NULL_ARGUMENT);
  g_return_val_if_fail (name, GSTD_NULL_ARGUMENT);
  g_return_val_if_fail (output, GSTD_NULL_ARGUMENT);
  g_return_val_if_fail (path, GSTD_NULL_ARGUMENT);

  if (!name) {
    ret = GSTD_BAD_VALUE;
    GST_ERROR_OBJECT (session,
        "Wrong query param provided, \"name\" doesn't exist");
    goto out;
  }

  message = g_strdup_printf ("delete %s %s", path, name);
  ret = gstd_parser_parse_cmd (session, message, output);
  g_free (message);
  message = NULL;

out:
  return ret;
}

static void
do_request (gpointer data_request, gpointer eval)
{
  gchar *response = NULL;
  gchar *name = NULL;
  gchar *description_pipe = NULL;
  GstdReturnCode ret = GSTD_BAD_COMMAND;
  gchar *output = NULL;
  const gchar *description = NULL;
  SoupStatus status = SOUP_STATUS_OK;
  SoupServer *server = NULL;
  SoupMsg *msg = NULL;
  GstdSession *session = NULL;
  const char *path = NULL;
  GHashTable *query = NULL;
  GstdHttpRequest *data_request_local = NULL;
  const char *method;

  g_return_if_fail (data_request);

  data_request_local = (GstdHttpRequest *) data_request;

  /*
   * Extract all fields from the request struct atomically.
   * The struct may be accessed from multiple threads, so we need
   * to copy everything we need under the lock.
   */
  g_mutex_lock (data_request_local->mutex);
  server = data_request_local->server;
  msg = data_request_local->msg;
  session = data_request_local->session;
  path = data_request_local->path;
  query = data_request_local->query;
  g_mutex_unlock (data_request_local->mutex);

  parse_json_body (msg, &name, &description_pipe);

  if (!name && query) {
    name = g_strdup (g_hash_table_lookup (query, "name"));
  }
  if (!description_pipe && query) {
    description_pipe = g_strdup (g_hash_table_lookup (query, "description"));
  }
#if SOUP_CHECK_VERSION(3,0,0)
  method = soup_server_message_get_method (msg);
#else
  method = msg->method;
#endif
  if (method == SOUP_METHOD_GET) {
    ret = do_get (server, msg, &output, path, session);
  } else if (method == SOUP_METHOD_POST) {
    ret = do_post (server, msg, name, description_pipe, &output, path, session);
  } else if (method == SOUP_METHOD_PUT) {
    ret = do_put (server, msg, name, &output, path, session);
  } else if (method == SOUP_METHOD_DELETE) {
    ret = do_delete (server, msg, name, &output, path, session);
  } else if (method == SOUP_METHOD_OPTIONS) {
    ret = GSTD_EOK;
  }
  g_free (name);
  g_free (description_pipe);
  name = NULL;
  description_pipe = NULL;

  description = gstd_return_code_to_string (ret);
  response =
      g_strdup_printf
      ("{\n  \"code\" : %d,\n  \"description\" : \"%s\",\n  \"response\" : %s\n}",
      ret, description, output ? output : "null");
  g_free (output);
  output = NULL;

#if SOUP_CHECK_VERSION(3,0,0)
  soup_server_message_set_response (msg, "application/json", SOUP_MEMORY_COPY,
      response, strlen (response));
#else
  soup_message_set_response (msg, "application/json", SOUP_MEMORY_COPY,
      response, strlen (response));
#endif
  g_free (response);
  response = NULL;

  status = get_status_code (ret);

#if SOUP_CHECK_VERSION(3,0,0)
  soup_server_message_set_status (msg, status, NULL);
#else
  soup_message_set_status (msg, status);
#endif

  g_mutex_lock (data_request_local->mutex);

#if SOUP_CHECK_VERSION(3,2,0)
  soup_server_message_unpause (msg);
#else
  soup_server_unpause_message (server, msg);
#endif
  g_mutex_unlock (data_request_local->mutex);

  if (query != NULL) {
    g_hash_table_unref (query);
  }
  g_free (data_request);
  data_request = NULL;

  return;
}

static void
parse_json_body (SoupMsg *msg, gchar **out_name, gchar **out_desc)
{
  const char *content_type = NULL;
  JsonParser *parser = NULL;
  JsonNode *root = NULL;
  GError *err = NULL;
  const char *body_data = NULL;
  gsize body_length = 0;
  SoupMessageBody *request_body = NULL;
  SoupMessageHeaders *request_headers = NULL;
#if SOUP_CHECK_VERSION(3,0,0)
  GBytes *body_bytes = NULL;
#else
  SoupBuffer *body_buffer = NULL;
#endif

  g_return_if_fail (msg);
  g_return_if_fail (out_name);
  g_return_if_fail (out_desc);

  *out_name = NULL;
  *out_desc = NULL;

#if SOUP_CHECK_VERSION(3,0,0)
  request_body = soup_server_message_get_request_body (msg);
  request_headers = soup_server_message_get_request_headers (msg);
#else
  request_body = msg->request_body;
  request_headers = msg->request_headers;
#endif

  if (!request_body) {
    return;
  }

#if SOUP_CHECK_VERSION(3,0,0)
  /* libsoup3: use GBytes API for body access */
  body_bytes = soup_message_body_flatten (request_body);
  if (!body_bytes) {
    return;
  }
  body_data = g_bytes_get_data (body_bytes, &body_length);
  if (body_length == 0) {
    g_bytes_unref (body_bytes);
    return;
  }
#else
  /* libsoup2: flatten returns SoupBuffer, access via buffer */
  body_buffer = soup_message_body_flatten (request_body);
  if (!body_buffer) {
    return;
  }
  body_data = body_buffer->data;
  body_length = body_buffer->length;
  if (body_length == 0) {
    soup_buffer_free (body_buffer);
    return;
  }
#endif

  content_type = soup_message_headers_get_content_type (request_headers, NULL);

  if (!content_type || !g_str_has_prefix (content_type, "application/json")) {
    goto out;
  }

  parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, body_data, body_length, &err)) {
    g_clear_error (&err);
    g_object_unref (parser);
    goto out;
  }

  root = json_parser_get_root (parser);
  if (JSON_NODE_HOLDS_OBJECT (root)) {
    JsonObject *obj = json_node_get_object (root);
    if (json_object_has_member (obj, "name")) {
      const char *value = json_object_get_string_member (obj, "name");
      if (value) *out_name = g_strdup (value);
    }
    if (json_object_has_member (obj, "description")) {
      const char *value = json_object_get_string_member (obj, "description");
      if (value) *out_desc = g_strdup (value);
    }
  }
  g_object_unref (parser);

out:
#if SOUP_CHECK_VERSION(3,0,0)
  if (body_bytes) {
    g_bytes_unref (body_bytes);
  }
#else
  if (body_buffer) {
    soup_buffer_free (body_buffer);
  }
#endif
}

static void
#if SOUP_CHECK_VERSION(3,0,0)
handle_health_request (SoupServer * server, SoupMsg * msg)
#else
handle_health_request (SoupServer * server, SoupMessage * msg)
#endif
{
  /* Simple liveness check - if HTTP server responds, gstd is alive.
   * Avoids GStreamer calls that could hang and trigger container restarts. */
  static const char *health_response =
      "{\n  \"code\" : 0,\n  \"description\" : \"OK\",\n  \"response\" : {\"status\": \"healthy\"}\n}";
  SoupMessageHeaders *response_headers = NULL;

#if SOUP_CHECK_VERSION(3,0,0)
  response_headers = soup_server_message_get_response_headers (msg);
#else
  response_headers = msg->response_headers;
#endif

  soup_message_headers_append (response_headers,
      "Access-Control-Allow-Origin", "*");
  soup_message_headers_append (response_headers,
      "Access-Control-Allow-Headers", "origin,range,content-type");
  soup_message_headers_append (response_headers,
      "Access-Control-Allow-Methods", "GET");

#if SOUP_CHECK_VERSION(3,0,0)
  soup_server_message_set_response (msg, "application/json", SOUP_MEMORY_STATIC,
      health_response, strlen (health_response));
  soup_server_message_set_status (msg, SOUP_STATUS_OK, NULL);
#else
  soup_message_set_response (msg, "application/json", SOUP_MEMORY_STATIC,
      health_response, strlen (health_response));
  soup_message_set_status (msg, SOUP_STATUS_OK);
#endif
}

/*
 * Fast-path handler for pipeline status polling.
 * This bypasses the thread pool to avoid contention during frequent
 * monitoring requests. Returns a lightweight JSON with pipeline names
 * and states only.
 */
static void
#if SOUP_CHECK_VERSION(3,0,0)
handle_pipelines_status (SoupServer * server, SoupMsg * msg,
    GstdSession * session)
#else
handle_pipelines_status (SoupServer * server, SoupMessage * msg,
    GstdSession * session)
#endif
{
  GString *json;
  GList *pipelines;
  GList *iter;
  gboolean first = TRUE;
  SoupMessageHeaders *response_headers = NULL;

#if SOUP_CHECK_VERSION(3,0,0)
  response_headers = soup_server_message_get_response_headers (msg);
#else
  response_headers = msg->response_headers;
#endif

  soup_message_headers_append (response_headers,
      "Access-Control-Allow-Origin", "*");
  soup_message_headers_append (response_headers,
      "Access-Control-Allow-Headers", "origin,range,content-type");
  soup_message_headers_append (response_headers,
      "Access-Control-Allow-Methods", "GET");

  json = g_string_new ("{\n  \"code\" : 0,\n  \"description\" : \"OK\",\n");
  g_string_append (json, "  \"response\" : {\n    \"pipelines\": [");

  /* Lock the list while iterating to prevent concurrent modification */
  GST_OBJECT_LOCK (session->pipelines);
  pipelines = session->pipelines->list;

  for (iter = pipelines; iter != NULL; iter = g_list_next (iter)) {
    GstdPipeline *pipeline = GSTD_PIPELINE (iter->data);
    const gchar *name;
    GstState current_state = GST_STATE_NULL;

    name = GSTD_OBJECT_NAME (pipeline);

    /* Get current pipeline state directly from GStreamer.
     * Must ref the element to prevent use-after-free if pipeline
     * is deleted by another thread while we're querying state. */
    GstElement *element = gstd_pipeline_get_element (pipeline);
    if (element) {
      gst_object_ref (element);
      gst_element_get_state (element, &current_state, NULL, 0);
      gst_object_unref (element);
    }

    if (!first) {
      g_string_append (json, ",");
    }
    first = FALSE;

    g_string_append_printf (json,
        "\n      {\"name\": \"%s\", \"state\": \"%s\"}",
        name,
        gst_element_state_get_name (current_state));
  }

  GST_OBJECT_UNLOCK (session->pipelines);

  g_string_append (json, "\n    ],\n    \"count\": ");
  g_string_append_printf (json, "%u", session->pipelines->count);
  g_string_append (json, "\n  }\n}");

#if SOUP_CHECK_VERSION(3,0,0)
  soup_server_message_set_response (msg, "application/json", SOUP_MEMORY_COPY,
      json->str, json->len);
  soup_server_message_set_status (msg, SOUP_STATUS_OK, NULL);
#else
  soup_message_set_response (msg, "application/json", SOUP_MEMORY_COPY,
      json->str, json->len);
  soup_message_set_status (msg, SOUP_STATUS_OK);
#endif

  g_string_free (json, TRUE);
}

static void
#if SOUP_CHECK_VERSION(3,0,0)
server_callback (SoupServer * server, SoupMsg * msg,
    const char *path, GHashTable * query, gpointer data)
#else
server_callback (SoupServer * server, SoupMessage * msg,
    const char *path, GHashTable * query,
    SoupClientContext * context, gpointer data)
#endif
{
  GstdSession *session = NULL;
  GstdHttp *self = NULL;
  GstdHttpRequest *data_request = NULL;
  SoupMessageHeaders *response_headers = NULL;

  g_return_if_fail (server);
  g_return_if_fail (msg);
  g_return_if_fail (data);

  /* Fast path for health checks - bypass thread pool */
  if (g_strcmp0 (path, "/health") == 0) {
    handle_health_request (server, msg);
    return;
  }

  self = GSTD_HTTP (data);
  session = self->session;

  /* Fast path for pipeline status polling - bypass thread pool.
   * This endpoint is optimized for frequent monitoring requests. */
  if (g_strcmp0 (path, "/pipelines/status") == 0) {
    handle_pipelines_status (server, msg, session);
    return;
  }

  data_request = g_new0 (GstdHttpRequest, 1);

  data_request->msg = msg;
  data_request->server = server;
  data_request->session = session;
  data_request->path = path;
  if (query) {
    data_request->query = g_hash_table_ref (query);
  } else {
    data_request->query = query;
  }
  data_request->mutex = &self->mutex;


#if SOUP_CHECK_VERSION(3,0,0)
  response_headers = soup_server_message_get_response_headers (msg);
#else
  response_headers = msg->response_headers;
#endif
  soup_message_headers_append (response_headers,
      "Access-Control-Allow-Origin", "*");
  soup_message_headers_append (response_headers,
      "Access-Control-Allow-Headers", "origin,range,content-type");
  soup_message_headers_append (response_headers,
      "Access-Control-Allow-Methods", "PUT, GET, POST, DELETE");
  g_mutex_lock (&self->mutex);
#if SOUP_CHECK_VERSION(3,2,0)
  soup_server_message_pause (msg);
#else
  soup_server_pause_message (server, msg);
#endif
  g_mutex_unlock (&self->mutex);
  if (!g_thread_pool_push (self->pool, (gpointer) data_request, NULL)) {
    GST_ERROR_OBJECT (self, "Thread pool push failed");
    /* Clean up the request that couldn't be queued */
    if (data_request->query) {
      g_hash_table_unref (data_request->query);
    }
    g_free (data_request);
    /* Unpause the message so libsoup can complete it with an error */
    g_mutex_lock (&self->mutex);
#if SOUP_CHECK_VERSION(3,0,0)
    soup_server_message_set_status (msg, SOUP_STATUS_SERVICE_UNAVAILABLE, NULL);
#else
    soup_message_set_status (msg, SOUP_STATUS_SERVICE_UNAVAILABLE);
#endif
#if SOUP_CHECK_VERSION(3,2,0)
    soup_server_message_unpause (msg);
#else
    soup_server_unpause_message (server, msg);
#endif
    g_mutex_unlock (&self->mutex);
  }

}

static GstdReturnCode
gstd_http_start (GstdIpc * base, GstdSession * session)
{
  GError *error = NULL;
  GSocketAddress *sa = NULL;
  GstdHttp *self = NULL;
  guint16 port = 0;
  gchar *address = NULL;

  g_return_val_if_fail (base, GSTD_NULL_ARGUMENT);
  g_return_val_if_fail (session, GSTD_NULL_ARGUMENT);

  self = GSTD_HTTP (base);
  port = self->port;
  address = self->address;

  self->session = session;
  gstd_http_stop (base);

  GST_DEBUG_OBJECT (self, "Initializing HTTP server");
  self->server = soup_server_new ("server-header", "Gstd-1.0", NULL);
  if (!self->server) {
    goto noconnection;
  }
  self->pool =
      g_thread_pool_new (do_request, NULL, self->max_threads, FALSE, &error);

  if (error) {
    goto noconnection;
  }

  sa = g_inet_socket_address_new_from_string (address, port);
  if (!sa) {
    g_printerr ("gstd: Invalid HTTP address: %s\n", address);
    goto noconnection;
  }

  soup_server_listen (self->server, sa, 0, &error);

  /* sa is no longer needed after soup_server_listen */
  g_object_unref (sa);
  sa = NULL;

  if (error) {
    goto noconnection;
  }

  GST_INFO_OBJECT (self, "HTTP server listening on %s:%u", address, port);

  soup_server_add_handler (self->server, NULL, server_callback, self, NULL);

  return GSTD_EOK;

noconnection:
  {
    if (error) {
      GST_ERROR_OBJECT (self, "%s", error->message);
      g_printerr ("%s\n", error->message);
      g_error_free (error);
      error = NULL;
    }
    if (self->pool) {
      g_thread_pool_free (self->pool, TRUE, FALSE);
      self->pool = NULL;
    }
    if (self->server) {
      g_object_unref (self->server);
      self->server = NULL;
    }
    return GSTD_NO_CONNECTION;
  }
}

static gboolean
gstd_http_init_get_option_group (GstdIpc * base, GOptionGroup ** group)
{
  GstdHttp *self = GSTD_HTTP (base);

  GOptionEntry http_args[] = {
    {"enable-http-protocol", 't', 0, G_OPTION_ARG_NONE, &base->enabled,
        "Enable attach the server through given HTTP ports ", NULL}
    ,
    {"http-address", 'a', 0, G_OPTION_ARG_STRING, &self->address,
          "Attach to the server through a given address (default 127.0.0.1)",
        "http-address"}
    ,
    {"http-port", 'p', 0, G_OPTION_ARG_INT, &self->port,
          "Attach to the server through a given port (default 5001)",
        "http-port"}
    ,
    {"http-max-threads", 'm', 0, G_OPTION_ARG_INT, &self->max_threads,
          "Max number of allowed threads to process simultaneous requests. -1 "
          "means unlimited (default -1)",
        "http-max-threads"}
    ,
    {NULL}
  };

  g_return_val_if_fail (base, FALSE);
  g_return_val_if_fail (group, FALSE);

  GST_DEBUG_OBJECT (self, "HTTP init group callback ");
  *group = g_option_group_new ("gstd-http", ("HTTP Options"),
      ("Show HTTP Options"), NULL, NULL);

  g_option_group_add_entries (*group, http_args);
  return TRUE;
}

static GstdReturnCode
gstd_http_stop (GstdIpc * base)
{
  GstdHttp *self = NULL;

  g_return_val_if_fail (base, GSTD_NULL_ARGUMENT);

  self = GSTD_HTTP (base);

  GST_DEBUG_OBJECT (self, "Stopping HTTP server");

  /* Wait for pending requests before destroying the pool */
  if (self->pool) {
    g_thread_pool_free (self->pool, FALSE, TRUE);  /* wait=TRUE for clean shutdown */
    self->pool = NULL;
  }

  if (self->server) {
    g_object_unref (self->server);
  }
  self->server = NULL;

  return GSTD_EOK;
}
