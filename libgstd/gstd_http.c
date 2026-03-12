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
#include "gstd_list.h"
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
  guint count;
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

  /* Snapshot the pipeline list under the lock, then release the lock
   * before querying states. gst_element_get_state() can block if a
   * state change is in progress (it needs the element's state lock),
   * so holding the list lock during state queries can deadlock the
   * entire pipeline list — blocking create/delete/play on all pipelines. */
  GST_OBJECT_LOCK (session->pipelines);
  pipelines = g_list_copy (session->pipelines->list);
  count = session->pipelines->count;
  /* Ref each pipeline to prevent use-after-free after releasing the lock */
  for (iter = pipelines; iter != NULL; iter = g_list_next (iter)) {
    gst_object_ref (iter->data);
  }
  GST_OBJECT_UNLOCK (session->pipelines);

  for (iter = pipelines; iter != NULL; iter = g_list_next (iter)) {
    GstdPipeline *pipeline = GSTD_PIPELINE (iter->data);
    const gchar *name;
    GstState current_state = GST_STATE_NULL;

    name = GSTD_OBJECT_NAME (pipeline);

    /* Read the cached state without blocking.
     * gst_element_get_state() acquires the element's state lock, which
     * blocks if gst_element_set_state() is in progress on another thread.
     * Since this endpoint runs on the soup main thread, any blocking here
     * stalls all HTTP I/O. Use GST_STATE() for a lock-free read of the
     * last-known state instead. */
    GstElement *element = gstd_pipeline_get_element (pipeline);
    if (element) {
      gst_object_ref (element);
      current_state = GST_STATE (element);
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

    gst_object_unref (pipeline);
  }

  g_list_free (pipelines);

  g_string_append (json, "\n    ],\n    \"count\": ");
  g_string_append_printf (json, "%u", count);
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

/*
 * Escape a string for safe embedding inside a JSON quoted value.
 * Handles double-quote and backslash which would break JSON structure.
 * Caller must g_free() the result.
 */
static gchar *
json_escape_string (const gchar * s)
{
  GString *out = g_string_new (NULL);
  for (; *s; s++) {
    if (*s == '"' || *s == '\\')
      g_string_append_c (out, '\\');
    g_string_append_c (out, *s);
  }
  return g_string_free (out, FALSE);
}

/**
 * handle_clock_sync:
 * @server: the SoupServer handling the request
 * @msg: the HTTP message to respond to
 * @query: query parameters (requires "source" and "targets")
 * @session: the GstD session containing the pipeline list
 *
 * Fast-path handler for inter-pipeline clock synchronization (one-to-many).
 *
 * Copies the GstClock reference and base_time from a single source pipeline
 * to one or more target pipelines. This is required by the rsinter plugin
 * (intersink/intersrc) when consumer pipelines are rebuilt while the
 * producer keeps running.
 *
 * Without clock synchronization, rebuilt consumer pipelines get a default
 * base_time that doesn't match the producer's time domain, causing buffers
 * to appear far in the future. The sink then waits indefinitely, producing
 * frozen or extremely slow video.
 *
 * Multiple targets are specified as a comma-separated list. This allows a
 * single HTTP call to synchronize all consumer pipelines (output, preview,
 * framegrab, KLV, etc.) that share the same ingest pipeline.
 *
 * Targets that are not found are skipped (logged as warnings) rather than
 * failing the entire request. This is intentional — during pipeline rebuild,
 * some consumer pipelines may not exist yet or may have already been deleted.
 *
 * This runs on the soup main thread (fast-path, bypasses thread pool) for
 * minimal latency. The underlying operations (set_clock, set_base_time)
 * are lightweight pointer/value stores, but do acquire element object locks
 * briefly — acceptable for the typical target count (< 10).
 *
 * Reference: https://gstreamer.freedesktop.org/documentation/rsinter/intersrc.html
 *
 * HTTP: POST /pipelines/clock_sync?source=<pipeline>&targets=<pipeline>[,<pipeline>,...]
 *
 * Response (200): { "code": 0, "description": "Success",
 *                   "response": { "base_time": <uint64>,
 *                                 "synced": ["p1","p2"],
 *                                 "skipped": ["p3"] } }
 * Error (400): Missing or invalid query parameters
 * Error (404): Source pipeline not found
 * Error (405): Wrong HTTP method (only POST allowed)
 * Error (500): Source pipeline element not available
 *
 * Note: This endpoint is custom to this fork and not available in upstream gstd.
 */
static void
#if SOUP_CHECK_VERSION(3,0,0)
handle_clock_sync (SoupServer * server, SoupMsg * msg,
    GHashTable * query, GstdSession * session)
#else
handle_clock_sync (SoupServer * server, SoupMessage * msg,
    GHashTable * query, GstdSession * session)
#endif
{
  const gchar *source_name = NULL;
  const gchar *targets_csv = NULL;
  GstdObject *source_obj = NULL;
  GstElement *source_elem = NULL;
  GstClock *clock = NULL;
  GstClockTime base_time;
  gchar **target_names = NULL;
  const gchar *error_json = NULL;
  const gchar *method = NULL;
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
      "Access-Control-Allow-Methods", "POST");

  /* Allow CORS preflight through, reject non-POST for actual requests */
#if SOUP_CHECK_VERSION(3,0,0)
  method = soup_server_message_get_method (msg);
#else
  method = msg->method;
#endif
  if (method == SOUP_METHOD_OPTIONS) {
#if SOUP_CHECK_VERSION(3,0,0)
    soup_server_message_set_status (msg, SOUP_STATUS_OK, NULL);
#else
    soup_message_set_status (msg, SOUP_STATUS_OK);
#endif
    return;
  }
  if (method != SOUP_METHOD_POST) {
    error_json =
        "{ \"code\": 1, \"description\": \"Method not allowed:"
        " use POST\", \"response\": null }";
    goto error_405;
  }

  /* Validate query parameters */
  if (!query) {
    error_json =
        "{ \"code\": 1, \"description\": \"Missing query parameters:"
        " source and targets\", \"response\": null }";
    goto error_400;
  }

  source_name = g_hash_table_lookup (query, "source");
  targets_csv = g_hash_table_lookup (query, "targets");

  if (!source_name || !targets_csv || targets_csv[0] == '\0') {
    error_json =
        "{ \"code\": 1, \"description\": \"Required query parameters:"
        " source=<pipeline> and targets=<pipeline>[,<pipeline>,...]\","
        " \"response\": null }";
    goto error_400;
  }

  /* Find source pipeline */
  source_obj = gstd_list_find_child (session->pipelines, source_name);
  if (!source_obj) {
    error_json =
        "{ \"code\": 4, \"description\": \"Source pipeline not found\","
        " \"response\": null }";
    goto error_404;
  }

  /* Get underlying GstElement for the source pipeline.
   * gstd_pipeline_get_element returns a borrowed pointer (no ref added),
   * so ref it to prevent use-after-free if the pipeline is modified
   * concurrently. */
  source_elem = gstd_pipeline_get_element (GSTD_PIPELINE (source_obj));
  if (!source_elem) {
    g_object_unref (source_obj);
    error_json =
        "{ \"code\": 5, \"description\": \"Source pipeline element"
        " not available\", \"response\": null }";
    goto error_500;
  }
  gst_object_ref (source_elem);

  /* Read clock and base_time from source (once) */
  clock = gst_element_get_clock (source_elem);
  base_time = gst_element_get_base_time (source_elem);

  gst_object_unref (source_elem);

  /* Apply clock and base_time to each target pipeline */
  target_names = g_strsplit (targets_csv, ",", -1);

  {
    GString *synced_json = g_string_new ("[");
    GString *skipped_json = g_string_new ("[");
    gboolean first_synced = TRUE;
    gboolean first_skipped = TRUE;
    guint i;

    for (i = 0; target_names[i] != NULL; i++) {
      const gchar *name = g_strstrip (target_names[i]);
      GstdObject *target_obj;
      GstElement *target_elem;
      gchar *escaped;

      if (name[0] == '\0')
        continue;

      target_obj = gstd_list_find_child (session->pipelines, name);
      if (!target_obj) {
        GST_WARNING ("clock_sync: target pipeline '%s' not found, skipping",
            name);
        if (!first_skipped)
          g_string_append_c (skipped_json, ',');
        escaped = json_escape_string (name);
        g_string_append_printf (skipped_json, "\"%s\"", escaped);
        g_free (escaped);
        first_skipped = FALSE;
        continue;
      }

      target_elem = gstd_pipeline_get_element (GSTD_PIPELINE (target_obj));
      if (!target_elem) {
        GST_WARNING ("clock_sync: target pipeline '%s' element not available,"
            " skipping", name);
        g_object_unref (target_obj);
        if (!first_skipped)
          g_string_append_c (skipped_json, ',');
        escaped = json_escape_string (name);
        g_string_append_printf (skipped_json, "\"%s\"", escaped);
        g_free (escaped);
        first_skipped = FALSE;
        continue;
      }

      gst_object_ref (target_elem);

      if (clock) {
        gst_element_set_clock (target_elem, clock);
      }
      gst_element_set_base_time (target_elem, base_time);

      /* Propagate base_time to all descendant elements.
       *
       * gst_element_set_clock() already propagates recursively (GstBin
       * overrides it), but gst_element_set_base_time() does NOT — it is
       * a simple field setter with no virtual dispatch.
       *
       * Elements inside bins with locked-state=TRUE (e.g. rtspclientsink's
       * internal rtspbin/rtpbin/multiudpsink) get their own base_time
       * during their independent state transition. This causes buffers
       * from intersrc to land in the wrong time domain, blocking the
       * internal sync=TRUE sinks indefinitely.
       *
       * Recursively setting base_time ensures every element — including
       * those inside locked-state sub-bins — shares the producer's time
       * domain, which is exactly what inter-pipeline clock sync requires.
       */
      if (GST_IS_BIN (target_elem)) {
        GstIterator *it = gst_bin_iterate_recurse (GST_BIN (target_elem));
        GValue item = G_VALUE_INIT;
        GstIteratorResult res;

        while ((res = gst_iterator_next (it, &item)) != GST_ITERATOR_DONE) {
          if (res == GST_ITERATOR_OK) {
            GstElement *child = GST_ELEMENT (g_value_get_object (&item));
            gst_element_set_base_time (child, base_time);
            g_value_reset (&item);
          } else if (res == GST_ITERATOR_RESYNC) {
            gst_iterator_resync (it);
          }
        }
        g_value_unset (&item);
        gst_iterator_free (it);
      }

      gst_object_unref (target_elem);

      GST_INFO ("clock_sync: synced %s -> %s (base_time %" GST_TIME_FORMAT ")",
          source_name, name, GST_TIME_ARGS (base_time));

      g_object_unref (target_obj);

      if (!first_synced)
        g_string_append_c (synced_json, ',');
      escaped = json_escape_string (name);
      g_string_append_printf (synced_json, "\"%s\"", escaped);
      g_free (escaped);
      first_synced = FALSE;
    }

    g_string_append_c (synced_json, ']');
    g_string_append_c (skipped_json, ']');

    if (clock) {
      gst_object_unref (clock);
    }
    g_object_unref (source_obj);
    g_strfreev (target_names);

    {
      gchar *ok_json = g_strdup_printf (
          "{ \"code\": 0, \"description\": \"Success\","
          " \"response\": { \"base_time\": %" G_GUINT64_FORMAT ","
          " \"synced\": %s, \"skipped\": %s } }",
          (guint64) base_time, synced_json->str, skipped_json->str);

      g_string_free (synced_json, TRUE);
      g_string_free (skipped_json, TRUE);

#if SOUP_CHECK_VERSION(3,0,0)
      soup_server_message_set_response (msg, "application/json",
          SOUP_MEMORY_TAKE, ok_json, strlen (ok_json));
      soup_server_message_set_status (msg, SOUP_STATUS_OK, NULL);
#else
      soup_message_set_response (msg, "application/json",
          SOUP_MEMORY_TAKE, ok_json, strlen (ok_json));
      soup_message_set_status (msg, SOUP_STATUS_OK);
#endif
    }
  }
  return;

error_400:
#if SOUP_CHECK_VERSION(3,0,0)
  soup_server_message_set_response (msg, "application/json",
      SOUP_MEMORY_STATIC, error_json, strlen (error_json));
  soup_server_message_set_status (msg, SOUP_STATUS_BAD_REQUEST, NULL);
#else
  soup_message_set_response (msg, "application/json",
      SOUP_MEMORY_STATIC, error_json, strlen (error_json));
  soup_message_set_status (msg, SOUP_STATUS_BAD_REQUEST);
#endif
  return;

error_404:
#if SOUP_CHECK_VERSION(3,0,0)
  soup_server_message_set_response (msg, "application/json",
      SOUP_MEMORY_STATIC, error_json, strlen (error_json));
  soup_server_message_set_status (msg, SOUP_STATUS_NOT_FOUND, NULL);
#else
  soup_message_set_response (msg, "application/json",
      SOUP_MEMORY_STATIC, error_json, strlen (error_json));
  soup_message_set_status (msg, SOUP_STATUS_NOT_FOUND);
#endif
  return;

error_405:
#if SOUP_CHECK_VERSION(3,0,0)
  soup_server_message_set_response (msg, "application/json",
      SOUP_MEMORY_STATIC, error_json, strlen (error_json));
  soup_server_message_set_status (msg,
      SOUP_STATUS_METHOD_NOT_ALLOWED, NULL);
#else
  soup_message_set_response (msg, "application/json",
      SOUP_MEMORY_STATIC, error_json, strlen (error_json));
  soup_message_set_status (msg, SOUP_STATUS_METHOD_NOT_ALLOWED);
#endif
  return;

error_500:
#if SOUP_CHECK_VERSION(3,0,0)
  soup_server_message_set_response (msg, "application/json",
      SOUP_MEMORY_STATIC, error_json, strlen (error_json));
  soup_server_message_set_status (msg,
      SOUP_STATUS_INTERNAL_SERVER_ERROR, NULL);
#else
  soup_message_set_response (msg, "application/json",
      SOUP_MEMORY_STATIC, error_json, strlen (error_json));
  soup_message_set_status (msg, SOUP_STATUS_INTERNAL_SERVER_ERROR);
#endif
  return;
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

  /* Fast path for inter-pipeline clock synchronization - bypass thread pool.
   * Copies clock and base_time from a source pipeline to a target pipeline,
   * which is required when rebuilding a consumer pipeline that uses
   * intersrc/intersink while the producer pipeline keeps running.
   * See: https://gstreamer.freedesktop.org/documentation/rsinter/intersrc.html */
  if (g_strcmp0 (path, "/pipelines/clock_sync") == 0) {
    handle_clock_sync (server, msg, query, session);
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
