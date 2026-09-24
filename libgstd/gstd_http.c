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

/* Upper bound on the request body we are willing to buffer. Pipeline
 * descriptions and property values are small, so a generous cap is safe for
 * every client while bounding the memory a hostile request can pin. It is
 * enforced while the body is received (see request_started_cb), before any
 * handler runs. */
#define GSTD_HTTP_MAX_BODY_SIZE (8 * 1024 * 1024)

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
  /* Optional bearer token required on every request except /health and
   * CORS preflights. NULL disables authentication (the default). */
  gchar *api_token;
  /* Origin allowed in CORS response headers. NULL (the default) emits no
   * CORS headers at all, so browsers refuse cross-origin reads. */
  gchar *cors_origin;
  SoupServer *server;
  GstdSession *session;
  GThreadPool *pool;
  GMutex mutex;
};

enum
{
  PROP_0,
  PROP_PORT,
  PROP_ADDRESS,
  PROP_MAX_THREADS,
  PROP_API_TOKEN,
  PROP_CORS_ORIGIN,
  N_PROPERTIES
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
static gchar *json_escape_string (const gchar * s);
#if SOUP_CHECK_VERSION(3,0,0)
static void server_callback (SoupServer * server, SoupMsg * msg,
    const char *path, GHashTable * query, gpointer data);
#else
static void server_callback (SoupServer * server, SoupMessage * msg,
    const char *path, GHashTable * query, SoupClientContext * context,
    gpointer data);
#endif

static void gstd_http_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec);
static void gstd_http_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec);

static void
gstd_http_class_init (GstdHttpClass * klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GstdIpcClass *gstdipc_class = GSTD_IPC_CLASS (klass);
  GParamSpec *properties[N_PROPERTIES] = { NULL, };
  guint debug_color;

  gstdipc_class->get_option_group =
      GST_DEBUG_FUNCPTR (gstd_http_init_get_option_group);
  gstdipc_class->start = GST_DEBUG_FUNCPTR (gstd_http_start);
  object_class->finalize = gstd_http_finalize;
  object_class->set_property = gstd_http_set_property;
  object_class->get_property = gstd_http_get_property;
  gstdipc_class->stop = GST_DEBUG_FUNCPTR (gstd_http_stop);

  properties[PROP_PORT] =
      g_param_spec_uint ("port", "Port",
      "The port the HTTP server listens on",
      0, G_MAXUINT16, GSTD_HTTP_DEFAULT_PORT, G_PARAM_READWRITE);

  properties[PROP_ADDRESS] =
      g_param_spec_string ("address", "Address",
      "The address the HTTP server binds to",
      NULL, G_PARAM_READWRITE);

  properties[PROP_MAX_THREADS] =
      g_param_spec_int ("max-threads", "Max threads",
      "Max number of threads processing simultaneous requests (-1 unlimited)",
      -1, G_MAXINT, GSTD_HTTP_DEFAULT_MAX_THREADS, G_PARAM_READWRITE);

  properties[PROP_API_TOKEN] =
      g_param_spec_string ("api-token", "API token",
      "Bearer token required on every request except GET/HEAD /health "
      "(NULL disables authentication; an empty string is rejected)",
      NULL, G_PARAM_READWRITE);

  properties[PROP_CORS_ORIGIN] =
      g_param_spec_string ("cors-origin", "CORS origin",
      "Origin allowed in CORS response headers: one http(s)://host[:port] "
      "origin, never \"*\" (NULL emits no CORS headers)",
      NULL, G_PARAM_READWRITE);

  g_object_class_install_properties (object_class, N_PROPERTIES, properties);

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
  /* Left NULL until first use: the --http-address option overwrites the
   * pointer without freeing it, so a preallocated default would leak. */
  self->address = NULL;
  self->max_threads = GSTD_HTTP_DEFAULT_MAX_THREADS;
  self->api_token = NULL;
  self->cors_origin = NULL;
  self->server = NULL;
  self->session = NULL;
  self->pool = NULL;

}

/* NULL disables authentication; any other value must be a usable token */
static gboolean
api_token_is_valid (const gchar * token)
{
  return token == NULL || token[0] != '\0';
}

/*
 * A CORS origin must be exactly one concrete serialized origin, as a
 * browser sends it in the Origin header and compares it byte for byte
 * against Access-Control-Allow-Origin: http or https, a lowercase host
 * (DNS name, IPv4, or bracketed IPv6) and an optional non-default port,
 * nothing else. That rules out "*", "null", paths (including a trailing
 * slash), queries, fragments, credentials, and lists. NULL disables CORS.
 */
static gboolean
cors_origin_is_valid (const gchar * origin)
{
  const gchar *host = NULL;
  const gchar *host_end = NULL;
  const gchar *port = NULL;
  const gchar *c = NULL;
  gchar *literal = NULL;
  gboolean https = FALSE;
  gboolean valid = FALSE;
  guint64 port_value = 0;

  if (!origin) {
    return TRUE;
  }

  if (g_str_has_prefix (origin, "http://")) {
    host = origin + strlen ("http://");
  } else if (g_str_has_prefix (origin, "https://")) {
    host = origin + strlen ("https://");
    https = TRUE;
  } else {
    return FALSE;
  }

  if (host[0] == '[') {
    /* IPv6 literal */
    host_end = strchr (host, ']');
    if (!host_end) {
      return FALSE;
    }
    literal = g_strndup (host + 1, host_end - host - 1);
    valid = strchr (literal, ':') != NULL;
    for (c = literal; valid && *c; c++) {
      valid = g_ascii_isdigit (*c) || (*c >= 'a' && *c <= 'f')
          || *c == ':' || *c == '.';
    }
    valid = valid && g_hostname_is_ip_address (literal);
    g_free (literal);
    if (!valid) {
      return FALSE;
    }
    port = host_end + 1;
  } else {
    for (c = host; *c && *c != ':'; c++) {
      if (!g_ascii_islower (*c) && !g_ascii_isdigit (*c) && *c != '-'
          && *c != '.') {
        return FALSE;
      }
    }
    host_end = c;
    if (host_end == host || host[0] == '.' || host[0] == '-'
        || host_end[-1] == '.' || host_end[-1] == '-'
        || g_strstr_len (host, host_end - host, "..")) {
      return FALSE;
    }
    port = host_end;
  }

  if (port[0] == '\0') {
    return TRUE;
  }
  if (port[0] != ':') {
    return FALSE;
  }
  port++;

  /* Serialized ports have no leading zero and omit the scheme default */
  if (port[0] == '\0' || port[0] == '0' || strlen (port) > 5) {
    return FALSE;
  }
  for (c = port; *c; c++) {
    if (!g_ascii_isdigit (*c)) {
      return FALSE;
    }
  }
  port_value = g_ascii_strtoull (port, NULL, 10);
  if (port_value > G_MAXUINT16 || (https && port_value == 443)
      || (!https && port_value == 80)) {
    return FALSE;
  }

  return TRUE;
}

#define GSTD_HTTP_CORS_ORIGIN_HINT \
  "a single origin such as https://ui.example.com or " \
  "http://127.0.0.1:8080 (lowercase, no default port, no \"*\", path, " \
  "trailing slash, query, fragment, credentials, or list)"

static void
gstd_http_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  GstdHttp *self = GSTD_HTTP (object);

  switch (property_id) {
    case PROP_PORT:
      self->port = g_value_get_uint (value);
      break;
    case PROP_ADDRESS:
      g_free (self->address);
      self->address = g_value_dup_string (value);
      break;
    case PROP_MAX_THREADS:
      self->max_threads = g_value_get_int (value);
      break;
    case PROP_API_TOKEN:
      /* NULL is the only way to disable authentication: an empty token
       * would read as "configured" while matching an empty credential,
       * so refuse it and keep the current value */
      if (!api_token_is_valid (g_value_get_string (value))) {
        g_warning ("gstd: rejecting an empty HTTP API token; set NULL to "
            "disable authentication");
        break;
      }
      /* Swapped under the lock: soup threads read it per request */
      g_mutex_lock (&self->mutex);
      g_free (self->api_token);
      self->api_token = g_value_dup_string (value);
      g_mutex_unlock (&self->mutex);
      break;
    case PROP_CORS_ORIGIN:
      /* Refuse a wildcard or malformed origin and keep the current one */
      if (!cors_origin_is_valid (g_value_get_string (value))) {
        g_warning ("gstd: rejecting CORS origin \"%s\"; expected "
            GSTD_HTTP_CORS_ORIGIN_HINT, g_value_get_string (value));
        break;
      }
      g_mutex_lock (&self->mutex);
      g_free (self->cors_origin);
      self->cors_origin = g_value_dup_string (value);
      g_mutex_unlock (&self->mutex);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static void
gstd_http_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec)
{
  GstdHttp *self = GSTD_HTTP (object);

  switch (property_id) {
    case PROP_PORT:
      g_value_set_uint (value, self->port);
      break;
    case PROP_ADDRESS:
      g_value_set_string (value, self->address);
      break;
    case PROP_MAX_THREADS:
      g_value_set_int (value, self->max_threads);
      break;
    case PROP_API_TOKEN:
      g_mutex_lock (&self->mutex);
      g_value_set_string (value, self->api_token);
      g_mutex_unlock (&self->mutex);
      break;
    case PROP_CORS_ORIGIN:
      g_mutex_lock (&self->mutex);
      g_value_set_string (value, self->cors_origin);
      g_mutex_unlock (&self->mutex);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
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

  if (self->api_token) {
    g_free (self->api_token);
    self->api_token = NULL;
  }

  if (self->cors_origin) {
    g_free (self->cors_origin);
    self->cors_origin = NULL;
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
  } else if (ret == GSTD_MAX_LIMIT_REACHED) {
    /* 429 Too Many Requests; not all libsoup versions name it */
    status = (SoupStatus) 429;
  } else {
    /* Including GSTD_BAD_VALUE: a rejected value is a client error */
    status = SOUP_STATUS_BAD_REQUEST;
  }

  return status;
}

/* Refuse to hash absurdly long credentials */
#define GSTD_HTTP_MAX_TOKEN_LENGTH 1024

/*
 * Append CORS headers only when an allowed origin was configured.
 * With no origin configured (the default) no CORS headers are emitted,
 * so browsers refuse cross-origin access to the API.
 * The origin is copied under the lock: it may be swapped through the
 * GObject property while soup threads are serving requests.
 */
static void
add_cors_headers (GstdHttp * self, SoupMessageHeaders * response_headers,
    const gchar * methods)
{
  gchar *origin = NULL;

  if (!self) {
    return;
  }

  g_mutex_lock (&self->mutex);
  origin = g_strdup (self->cors_origin);
  g_mutex_unlock (&self->mutex);

  if (!origin || origin[0] == '\0') {
    g_free (origin);
    return;
  }

  soup_message_headers_append (response_headers,
      "Access-Control-Allow-Origin", origin);
  soup_message_headers_append (response_headers,
      "Access-Control-Allow-Headers",
      "origin,range,content-type,authorization");
  soup_message_headers_append (response_headers,
      "Access-Control-Allow-Methods", methods);
  /* The origin is always one concrete origin (a wildcard is refused at
   * configuration time), so caches must key on the request's Origin */
  soup_message_headers_append (response_headers, "Vary", "Origin");

  g_free (origin);
}

/*
 * Compare secrets in time independent of where they first differ.
 * Both inputs are hashed so the comparison runs over fixed-length
 * digests, and the digests are compared without an early exit; a
 * matching digest prefix reveals nothing about the token itself.
 */
static gboolean
token_equal (const gchar * expected, const gchar * provided)
{
  gchar *expected_digest = NULL;
  gchar *provided_digest = NULL;
  guchar diff = 0;
  gsize i;

  expected_digest =
      g_compute_checksum_for_string (G_CHECKSUM_SHA256, expected, -1);
  provided_digest =
      g_compute_checksum_for_string (G_CHECKSUM_SHA256, provided, -1);

  if (!expected_digest || !provided_digest
      || strlen (expected_digest) != strlen (provided_digest)) {
    diff = 1;
  } else {
    for (i = 0; expected_digest[i] != '\0'; i++) {
      diff |= (guchar) expected_digest[i] ^ (guchar) provided_digest[i];
    }
  }

  g_free (expected_digest);
  g_free (provided_digest);

  return diff == 0;
}

/*
 * Validate the Authorization header against the configured API token.
 * Returns TRUE when no token is configured (authentication disabled).
 * The token is copied under the lock: it may be swapped through the
 * GObject property while soup threads are serving requests.
 */
static gboolean
request_authorized (GstdHttp * self, SoupMsg * msg)
{
  SoupMessageHeaders *request_headers = NULL;
  const gchar *authorization = NULL;
  static const gchar bearer_prefix[] = "Bearer ";
  gchar *token = NULL;
  gboolean authorized = FALSE;

  g_mutex_lock (&self->mutex);
  token = g_strdup (self->api_token);
  g_mutex_unlock (&self->mutex);

  /* NULL is the only disabled state. An empty token is refused at
   * configuration time; should one ever get here, fail closed. */
  if (!token) {
    return TRUE;
  }
  if (token[0] == '\0') {
    goto out;
  }
#if SOUP_CHECK_VERSION(3,0,0)
  request_headers = soup_server_message_get_request_headers (msg);
#else
  request_headers = msg->request_headers;
#endif
  if (!request_headers) {
    goto out;
  }

  authorization = soup_message_headers_get_one (request_headers,
      "Authorization");
  if (!authorization
      || strlen (authorization) > GSTD_HTTP_MAX_TOKEN_LENGTH) {
    goto out;
  }

  if (g_ascii_strncasecmp (authorization, bearer_prefix,
          strlen (bearer_prefix)) != 0) {
    goto out;
  }

  authorized = token_equal (token, authorization + strlen (bearer_prefix));

out:
  g_free (token);
  return authorized;
}

static void
respond_unauthorized (GstdHttp * self, SoupMsg * msg)
{
  static const char *unauthorized =
      "{ \"code\": 1, \"description\": \"Unauthorized: missing or invalid"
      " API token\", \"response\": null }";
  SoupMessageHeaders *response_headers = NULL;

#if SOUP_CHECK_VERSION(3,0,0)
  response_headers = soup_server_message_get_response_headers (msg);
#else
  response_headers = msg->response_headers;
#endif
  soup_message_headers_append (response_headers, "WWW-Authenticate",
      "Bearer");
  /* With CORS configured, let the browser page read the 401 rather than
   * see an opaque network error */
  add_cors_headers (self, response_headers, "PUT, GET, POST, DELETE");

#if SOUP_CHECK_VERSION(3,0,0)
  soup_server_message_set_response (msg, "application/json",
      SOUP_MEMORY_STATIC, unauthorized, strlen (unauthorized));
  soup_server_message_set_status (msg, SOUP_STATUS_UNAUTHORIZED, NULL);
#else
  soup_message_set_response (msg, "application/json",
      SOUP_MEMORY_STATIC, unauthorized, strlen (unauthorized));
  soup_message_set_status (msg, SOUP_STATUS_UNAUTHORIZED);
#endif
}

/* 405 with the methods the endpoint accepts in Allow (RFC 9110 15.5.6) */
static void
respond_method_not_allowed (SoupMsg * msg, const gchar * allowed)
{
  static const char *method_error =
      "{ \"code\": 1, \"description\": \"Method not allowed\","
      " \"response\": null }";
  SoupMessageHeaders *response_headers = NULL;

#if SOUP_CHECK_VERSION(3,0,0)
  response_headers = soup_server_message_get_response_headers (msg);
#else
  response_headers = msg->response_headers;
#endif
  soup_message_headers_replace (response_headers, "Allow", allowed);

#if SOUP_CHECK_VERSION(3,0,0)
  soup_server_message_set_response (msg, "application/json",
      SOUP_MEMORY_STATIC, method_error, strlen (method_error));
  soup_server_message_set_status (msg, SOUP_STATUS_METHOD_NOT_ALLOWED, NULL);
#else
  soup_message_set_response (msg, "application/json",
      SOUP_MEMORY_STATIC, method_error, strlen (method_error));
  soup_message_set_status (msg, SOUP_STATUS_METHOD_NOT_ALLOWED);
#endif
}

/*
 * A resource name travels inside the space-separated parser command
 * language, so whitespace or control characters in it would be
 * re-tokenized as extra command arguments. Reject those up front;
 * legitimate object names never need them.
 */
static gboolean
is_valid_resource_name (const gchar * name)
{
  const gchar *c;

  if (!name || name[0] == '\0') {
    return FALSE;
  }

  for (c = name; *c; c++) {
    if ((guchar) * c <= 0x20 || (guchar) * c == 0x7f) {
      return FALSE;
    }
  }

  return TRUE;
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

  if (!is_valid_resource_name (name)) {
    ret = GSTD_BAD_VALUE;
    GST_ERROR_OBJECT (session,
        "Rejecting \"name\" with whitespace or control characters");
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

  if (!is_valid_resource_name (name)) {
    ret = GSTD_BAD_VALUE;
    GST_ERROR_OBJECT (session,
        "Rejecting \"name\" with whitespace or control characters");
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
  g_object_unref (msg);
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

  if (!request_body || request_body->length == 0) {
    return;
  }

  /* Only JSON bodies carry parameters; never copy anything else */
  content_type = soup_message_headers_get_content_type (request_headers, NULL);
  if (!content_type || !g_str_has_prefix (content_type, "application/json")) {
    return;
  }

  /* The limit is enforced while the body is received, so an oversized
   * request never reaches a handler; this guards the flatten below in
   * case that invariant is ever broken. */
  if (request_body->length > GSTD_HTTP_MAX_BODY_SIZE) {
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
handle_health_request (GstdHttp * self, SoupServer * server, SoupMsg * msg)
#else
handle_health_request (GstdHttp * self, SoupServer * server,
    SoupMessage * msg)
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

  add_cors_headers (self, response_headers, "GET, HEAD");

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
handle_pipelines_status (GstdHttp * self, SoupServer * server, SoupMsg * msg,
    GstdSession * session)
#else
handle_pipelines_status (GstdHttp * self, SoupServer * server,
    SoupMessage * msg, GstdSession * session)
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

  add_cors_headers (self, response_headers, "GET");

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
    gchar *escaped_name;
    GstState current_state = GST_STATE_NULL;
    GstElement *element;

    name = GSTD_OBJECT_NAME (pipeline);

    /* Read the cached state without blocking.
     * gst_element_get_state() acquires the element's state lock, which
     * blocks if gst_element_set_state() is in progress on another thread.
     * Since this endpoint runs on the soup main thread, any blocking here
     * stalls all HTTP I/O. Use GST_STATE() for a lock-free read of the
     * last-known state instead. */
    element = gstd_pipeline_get_element (pipeline);
    if (element) {
      gst_object_ref (element);
      current_state = GST_STATE (element);
      gst_object_unref (element);
    }

    if (!first) {
      g_string_append (json, ",");
    }
    first = FALSE;

    escaped_name = json_escape_string (name);
    g_string_append_printf (json,
        "\n      {\"name\": \"%s\", \"state\": \"%s\"}",
        escaped_name,
        gst_element_state_get_name (current_state));
    g_free (escaped_name);

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
    guchar c = (guchar) *s;
    switch (c) {
      case '"':
        g_string_append (out, "\\\"");
        break;
      case '\\':
        g_string_append (out, "\\\\");
        break;
      case '\b':
        g_string_append (out, "\\b");
        break;
      case '\f':
        g_string_append (out, "\\f");
        break;
      case '\n':
        g_string_append (out, "\\n");
        break;
      case '\r':
        g_string_append (out, "\\r");
        break;
      case '\t':
        g_string_append (out, "\\t");
        break;
      default:
        /* Escape remaining control characters per RFC 8259; leave raw UTF-8
         * bytes (>= 0x80) untouched so multibyte sequences pass through. */
        if (c < 0x20)
          g_string_append_printf (out, "\\u%04x", c);
        else
          g_string_append_c (out, (gchar) c);
        break;
    }
  }
  return g_string_free (out, FALSE);
}

/**
 * handle_element_check:
 * @server: the SoupServer handling the request
 * @msg: the HTTP message to respond to
 * @element_name: the GStreamer element factory name to look up
 *
 * Checks if a GStreamer element factory exists in the plugin registry.
 * Returns 200 with element metadata if found, 404 if not.
 *
 * Uses gst_element_factory_find() which is a thread-safe read from the
 * immutable plugin registry hash table — safe for the fast path.
 *
 * Lets a client probe element/hardware capabilities at startup without
 * creating a pipeline (e.g., checking that a hardware-accelerated converter
 * element is available before building a pipeline).
 *
 * HTTP: GET /elements/<element_name>
 *
 * Note: This endpoint is custom to this fork and not available in upstream gstd.
 */
static void
#if SOUP_CHECK_VERSION(3,0,0)
handle_element_check (GstdHttp * self, SoupServer * server, SoupMsg * msg,
    const gchar * element_name)
#else
handle_element_check (GstdHttp * self, SoupServer * server,
    SoupMessage * msg, const gchar * element_name)
#endif
{
  GstElementFactory *factory;
  SoupMessageHeaders *response_headers = NULL;
  const gchar *method = NULL;

#if SOUP_CHECK_VERSION(3,0,0)
  response_headers = soup_server_message_get_response_headers (msg);
#else
  response_headers = msg->response_headers;
#endif
  add_cors_headers (self, response_headers, "GET");

  /* Allow CORS preflight through */
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
  if (method != SOUP_METHOD_GET) {
    static const char *method_error =
        "{ \"code\": 1, \"description\": \"Method not allowed:"
        " use GET\", \"response\": null }";
#if SOUP_CHECK_VERSION(3,0,0)
    soup_server_message_set_response (msg, "application/json",
        SOUP_MEMORY_STATIC, method_error, strlen (method_error));
    soup_server_message_set_status (msg,
        SOUP_STATUS_METHOD_NOT_ALLOWED, NULL);
#else
    soup_message_set_response (msg, "application/json",
        SOUP_MEMORY_STATIC, method_error, strlen (method_error));
    soup_message_set_status (msg, SOUP_STATUS_METHOD_NOT_ALLOWED);
#endif
    return;
  }

  factory = gst_element_factory_find (element_name);
  if (factory) {
    const gchar *klass;
    const gchar *desc;
    const gchar *author;
    const gchar *license;
    GstPlugin *plugin;
    gchar *escaped_name;
    gchar *escaped_klass;
    gchar *escaped_desc;
    gchar *escaped_author;
    gchar *escaped_license;
    gchar *json;

    klass = gst_element_factory_get_metadata (factory,
        GST_ELEMENT_METADATA_KLASS);
    desc = gst_element_factory_get_metadata (factory,
        GST_ELEMENT_METADATA_DESCRIPTION);
    author = gst_element_factory_get_metadata (factory,
        GST_ELEMENT_METADATA_AUTHOR);

    /* License lives on the plugin, not the element factory */
    plugin = gst_plugin_feature_get_plugin (GST_PLUGIN_FEATURE (factory));
    license = plugin ? gst_plugin_get_license (plugin) : NULL;

    escaped_name = json_escape_string (element_name);
    escaped_klass = json_escape_string (klass ? klass : "");
    escaped_desc = json_escape_string (desc ? desc : "");
    escaped_author = json_escape_string (author ? author : "");
    escaped_license = json_escape_string (license ? license : "");

    json = g_strdup_printf (
        "{\n"
        "  \"code\" : 0,\n"
        "  \"description\" : \"Success\",\n"
        "  \"response\" : {\n"
        "    \"name\" : \"%s\",\n"
        "    \"available\" : true,\n"
        "    \"klass\" : \"%s\",\n"
        "    \"description\" : \"%s\",\n"
        "    \"author\" : \"%s\",\n"
        "    \"license\" : \"%s\"\n"
        "  }\n"
        "}",
        escaped_name, escaped_klass, escaped_desc,
        escaped_author, escaped_license);

    g_free (escaped_name);
    g_free (escaped_klass);
    g_free (escaped_desc);
    g_free (escaped_author);
    g_free (escaped_license);
    if (plugin)
      gst_object_unref (plugin);
    gst_object_unref (factory);

#if SOUP_CHECK_VERSION(3,0,0)
    soup_server_message_set_response (msg, "application/json",
        SOUP_MEMORY_TAKE, json, strlen (json));
    soup_server_message_set_status (msg, SOUP_STATUS_OK, NULL);
#else
    soup_message_set_response (msg, "application/json",
        SOUP_MEMORY_TAKE, json, strlen (json));
    soup_message_set_status (msg, SOUP_STATUS_OK);
#endif
  } else {
    gchar *escaped_name;
    gchar *json;

    escaped_name = json_escape_string (element_name);
    json = g_strdup_printf (
        "{\n"
        "  \"code\" : 1,\n"
        "  \"description\" : \"Element '%s' not found in registry\",\n"
        "  \"response\" : null\n"
        "}",
        escaped_name);

    g_free (escaped_name);

#if SOUP_CHECK_VERSION(3,0,0)
    soup_server_message_set_response (msg, "application/json",
        SOUP_MEMORY_TAKE, json, strlen (json));
    soup_server_message_set_status (msg, SOUP_STATUS_NOT_FOUND, NULL);
#else
    soup_message_set_response (msg, "application/json",
        SOUP_MEMORY_TAKE, json, strlen (json));
    soup_message_set_status (msg, SOUP_STATUS_NOT_FOUND);
#endif
  }
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
handle_clock_sync (GstdHttp * self, SoupServer * server, SoupMsg * msg,
    GHashTable * query, GstdSession * session)
#else
handle_clock_sync (GstdHttp * self, SoupServer * server, SoupMessage * msg,
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

  add_cors_headers (self, response_headers, "POST");

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
          } else {
            /* GST_ITERATOR_ERROR: stop instead of spinning forever */
            break;
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
  const gchar *method = NULL;

  g_return_if_fail (server);
  g_return_if_fail (msg);
  g_return_if_fail (data);

  self = GSTD_HTTP (data);
  session = self->session;

#if SOUP_CHECK_VERSION(3,0,0)
  method = soup_server_message_get_method (msg);
#else
  method = msg->method;
#endif

  /* Fast path for health checks - bypass thread pool. GET and HEAD are
   * exempt from authentication so container liveness probes need no
   * credentials; they expose nothing but the fact that the server
   * responds. Every other method is refused here, so the exemption never
   * extends to a write; OPTIONS falls through to the shared preflight. */
  if (g_strcmp0 (path, "/health") == 0 && method != SOUP_METHOD_OPTIONS) {
    if (method == SOUP_METHOD_GET || method == SOUP_METHOD_HEAD) {
      handle_health_request (self, server, msg);
    } else {
      respond_method_not_allowed (msg, "GET, HEAD");
    }
    return;
  }

  /* Answer CORS preflights centrally, before authentication (preflights
   * carry no credentials) and before any handler could run: an OPTIONS
   * request must never reach code that discloses state. */
  if (method == SOUP_METHOD_OPTIONS) {
#if SOUP_CHECK_VERSION(3,0,0)
    response_headers = soup_server_message_get_response_headers (msg);
#else
    response_headers = msg->response_headers;
#endif
    add_cors_headers (self, response_headers, "PUT, GET, POST, DELETE");
#if SOUP_CHECK_VERSION(3,0,0)
    soup_server_message_set_status (msg, SOUP_STATUS_OK, NULL);
#else
    soup_message_set_status (msg, SOUP_STATUS_OK);
#endif
    return;
  }

  /* When an API token is configured, every other endpoint requires it. */
  if (!request_authorized (self, msg)) {
    respond_unauthorized (self, msg);
    return;
  }

  /* The decoded request path is spliced into the same space-separated
   * parser command as ?name=, so whitespace or control characters in it
   * (e.g. an encoded %20) would be re-tokenized as extra command
   * arguments. Legitimate gstd paths never contain them. */
  if (!is_valid_resource_name (path)) {
    static const char *bad_path =
        "{ \"code\": 1, \"description\": \"Invalid characters in request"
        " path\", \"response\": null }";
#if SOUP_CHECK_VERSION(3,0,0)
    soup_server_message_set_response (msg, "application/json",
        SOUP_MEMORY_STATIC, bad_path, strlen (bad_path));
    soup_server_message_set_status (msg, SOUP_STATUS_BAD_REQUEST, NULL);
#else
    soup_message_set_response (msg, "application/json",
        SOUP_MEMORY_STATIC, bad_path, strlen (bad_path));
    soup_message_set_status (msg, SOUP_STATUS_BAD_REQUEST);
#endif
    return;
  }

  /* Fast path for pipeline status polling - bypass thread pool.
   * This endpoint is optimized for frequent monitoring requests. */
  if (g_strcmp0 (path, "/pipelines/status") == 0) {
    if (method != SOUP_METHOD_GET) {
      static const char *status_method_error =
          "{ \"code\": 1, \"description\": \"Method not allowed:"
          " use GET\", \"response\": null }";
#if SOUP_CHECK_VERSION(3,0,0)
      soup_server_message_set_response (msg, "application/json",
          SOUP_MEMORY_STATIC, status_method_error,
          strlen (status_method_error));
      soup_server_message_set_status (msg,
          SOUP_STATUS_METHOD_NOT_ALLOWED, NULL);
#else
      soup_message_set_response (msg, "application/json",
          SOUP_MEMORY_STATIC, status_method_error,
          strlen (status_method_error));
      soup_message_set_status (msg, SOUP_STATUS_METHOD_NOT_ALLOWED);
#endif
      return;
    }
    handle_pipelines_status (self, server, msg, session);
    return;
  }

  /* Fast path for inter-pipeline clock synchronization - bypass thread pool.
   * Copies clock and base_time from a source pipeline to a target pipeline,
   * which is required when rebuilding a consumer pipeline that uses
   * intersrc/intersink while the producer pipeline keeps running.
   * See: https://gstreamer.freedesktop.org/documentation/rsinter/intersrc.html */
  if (g_strcmp0 (path, "/pipelines/clock_sync") == 0) {
    handle_clock_sync (self, server, msg, query, session);
    return;
  }

  /* Fast path for element registry lookup - bypass thread pool.
   * Checks if a GStreamer element factory exists without creating a pipeline.
   * Lets a client probe element/hardware capabilities at startup (e.g.,
   * checking that a hardware-accelerated converter element is available
   * before building a pipeline).
   *
   * HTTP: GET /elements/<element_name>
   *
   * Note: This endpoint is custom to this fork and not available in upstream gstd. */
  if (g_str_has_prefix (path, "/elements/")) {
    const gchar *element_name = path + strlen ("/elements/");
    if (element_name[0] != '\0') {
      handle_element_check (self, server, msg, element_name);
    } else {
      /* /elements/ with no name — return 400 rather than falling through
       * to the thread pool where it would be misinterpreted as a gstd path */
      static const char *missing_name =
          "{ \"code\": 1, \"description\": \"Element name required:"
          " use /elements/<name>\", \"response\": null }";
#if SOUP_CHECK_VERSION(3,0,0)
      soup_server_message_set_response (msg, "application/json",
          SOUP_MEMORY_STATIC, missing_name, strlen (missing_name));
      soup_server_message_set_status (msg, SOUP_STATUS_BAD_REQUEST, NULL);
#else
      soup_message_set_response (msg, "application/json",
          SOUP_MEMORY_STATIC, missing_name, strlen (missing_name));
      soup_message_set_status (msg, SOUP_STATUS_BAD_REQUEST);
#endif
    }
    return;
  }

  /* Oversized bodies never get here: request_started_cb rejects them with
   * 413 while they are received, and libsoup skips every handler for a
   * message that already carries a status. */

  data_request = g_new0 (GstdHttpRequest, 1);

  /* The worker thread completes this message after the pause below,
   * possibly long after libsoup would have dropped its own references
   * on a client disconnect. Hold a reference until the worker is done;
   * it also keeps `path` valid, which is owned by the message's URI. */
  data_request->msg = g_object_ref (msg);
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
  add_cors_headers (self, response_headers, "PUT, GET, POST, DELETE");
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
    g_object_unref (data_request->msg);
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

/*
 * Reject a request whose body exceeds GSTD_HTTP_MAX_BODY_SIZE. Whatever
 * was buffered is released and the rest of the body is discarded as it
 * arrives instead of accumulated. Setting the status here makes libsoup
 * skip every handler, and the connection is closed after the response so
 * leftover body bytes can never be parsed as a new request.
 */
static void
reject_oversized_request (GstdHttp * self, SoupMsg * msg)
{
  static const char *too_large =
      "{ \"code\": 1, \"description\": \"Request body too large\","
      " \"response\": null }";
  SoupMessageBody *request_body = NULL;
  SoupMessageHeaders *response_headers = NULL;
  guint status = 0;

#if SOUP_CHECK_VERSION(3,0,0)
  request_body = soup_server_message_get_request_body (msg);
  response_headers = soup_server_message_get_response_headers (msg);
  status = soup_server_message_get_status (msg);
#else
  request_body = msg->request_body;
  response_headers = msg->response_headers;
  status = msg->status_code;
#endif

  soup_message_body_set_accumulate (request_body, FALSE);
  soup_message_body_truncate (request_body);

  /* libsoup may already have failed the request (e.g. a bad path); keep
   * its status, it only needed the buffering stopped */
  if (status != 0) {
    return;
  }

  GST_WARNING_OBJECT (self, "Rejecting request body larger than %d bytes",
      GSTD_HTTP_MAX_BODY_SIZE);

  soup_message_headers_replace (response_headers, "Connection", "close");
  add_cors_headers (self, response_headers, "PUT, GET, POST, DELETE");

#if SOUP_CHECK_VERSION(3,0,0)
  soup_server_message_set_response (msg, "application/json",
      SOUP_MEMORY_STATIC, too_large, strlen (too_large));
  soup_server_message_set_status (msg,
      SOUP_STATUS_REQUEST_ENTITY_TOO_LARGE, NULL);
#else
  soup_message_set_response (msg, "application/json",
      SOUP_MEMORY_STATIC, too_large, strlen (too_large));
  soup_message_set_status (msg, SOUP_STATUS_REQUEST_ENTITY_TOO_LARGE);
#endif
}

/* A declared Content-Length over the limit is rejected before any body
 * byte is read; with "Expect: 100-continue" libsoup then skips the body */
static void
request_got_headers_cb (SoupMsg * msg, gpointer data)
{
  SoupMessageHeaders *request_headers = NULL;

#if SOUP_CHECK_VERSION(3,0,0)
  request_headers = soup_server_message_get_request_headers (msg);
#else
  request_headers = msg->request_headers;
#endif

  if (soup_message_headers_get_encoding (request_headers) ==
      SOUP_ENCODING_CONTENT_LENGTH
      && soup_message_headers_get_content_length (request_headers) >
      GSTD_HTTP_MAX_BODY_SIZE) {
    reject_oversized_request (GSTD_HTTP (data), msg);
  }
}

/* Chunked (or otherwise unsized) bodies are counted as they arrive: the
 * body accumulates, so its length is the running total */
static void
#if SOUP_CHECK_VERSION(3,0,0)
request_got_chunk_cb (SoupMsg * msg, GBytes * chunk, gpointer data)
#else
request_got_chunk_cb (SoupMsg * msg, SoupBuffer * chunk, gpointer data)
#endif
{
  SoupMessageBody *request_body = NULL;

#if SOUP_CHECK_VERSION(3,0,0)
  request_body = soup_server_message_get_request_body (msg);
#else
  request_body = msg->request_body;
#endif

  if (request_body->length > GSTD_HTTP_MAX_BODY_SIZE) {
    reject_oversized_request (GSTD_HTTP (data), msg);
  }
}

/*
 * Runs for every request before libsoup reads it, so the body limit is
 * enforced for every method, content type, and endpoint (fast paths
 * included) before any handler dispatch or body flattening.
 */
static void
#if SOUP_CHECK_VERSION(3,0,0)
request_started_cb (SoupServer * server, SoupMsg * msg, gpointer data)
#else
request_started_cb (SoupServer * server, SoupMsg * msg,
    SoupClientContext * client, gpointer data)
#endif
{
  g_signal_connect (msg, "got-headers",
      G_CALLBACK (request_got_headers_cb), data);
  g_signal_connect (msg, "got-chunk", G_CALLBACK (request_got_chunk_cb), data);
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
  if (NULL == self->address)
    self->address = g_strdup (GSTD_HTTP_DEFAULT_ADDRESS);
  address = self->address;

  /* Environment fallbacks for settings not given on the command line.
   * The env var is the recommended way to pass the token, since command
   * line arguments are visible to other local processes. */
  g_mutex_lock (&self->mutex);
  if (NULL == self->api_token) {
    self->api_token = g_strdup (g_getenv ("GSTD_HTTP_API_TOKEN"));
  }
  if (NULL == self->cors_origin) {
    self->cors_origin = g_strdup (g_getenv ("GSTD_HTTP_CORS_ORIGIN"));
  }
  /* The command line writes the field directly, bypassing the property
   * check, and the environment is only read here: validate both. An
   * explicitly empty token must not start a server that looks
   * authenticated but is not. */
  if (!api_token_is_valid (self->api_token)) {
    g_mutex_unlock (&self->mutex);
    GST_ERROR_OBJECT (self, "Refusing to start with an empty API token");
    g_printerr ("gstd: The HTTP API token (--http-api-token or "
        "GSTD_HTTP_API_TOKEN) is set but empty; set a token, or unset it "
        "to run without authentication\n");
    return GSTD_BAD_VALUE;
  }
  if (!cors_origin_is_valid (self->cors_origin)) {
    g_mutex_unlock (&self->mutex);
    GST_ERROR_OBJECT (self, "Refusing to start with an invalid CORS origin");
    g_printerr ("gstd: Invalid HTTP CORS origin (--http-cors-origin or "
        "GSTD_HTTP_CORS_ORIGIN); expected " GSTD_HTTP_CORS_ORIGIN_HINT "\n");
    return GSTD_BAD_VALUE;
  }
  if (self->api_token) {
    GST_INFO_OBJECT (self, "HTTP API token authentication enabled");
  }
  g_mutex_unlock (&self->mutex);

  self->session = session;
  gstd_http_stop (base);

  GST_DEBUG_OBJECT (self, "Initializing HTTP server");
  self->server = soup_server_new ("server-header", "Gstd-1.0", NULL);
  if (!self->server) {
    goto noconnection;
  }
  g_signal_connect (self->server, "request-started",
      G_CALLBACK (request_started_cb), self);
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
    {"http-api-token", 0, 0, G_OPTION_ARG_STRING, &self->api_token,
          "Require this bearer token on every request except /health. "
          "Prefer the GSTD_HTTP_API_TOKEN environment variable: command "
          "line arguments are visible to other local processes. An empty "
          "value is rejected (default: authentication disabled)",
        "token"}
    ,
    {"http-cors-origin", 0, 0, G_OPTION_ARG_STRING, &self->cors_origin,
          "Origin allowed in CORS response headers, or GSTD_HTTP_CORS_ORIGIN "
          "env var. Exactly one http(s)://host[:port] origin; \"*\", paths, "
          "and lists are rejected (default: no CORS headers, cross-origin "
          "browser access disabled)",
        "origin"}
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

typedef struct _GstdHttpStopData
{
  SoupServer *server;
  GMutex mutex;
  GCond cond;
  gboolean done;
} GstdHttpStopData;

static gboolean
gstd_http_disconnect_cb (gpointer data)
{
  GstdHttpStopData *stop_data = (GstdHttpStopData *) data;

  soup_server_disconnect (stop_data->server);

  g_mutex_lock (&stop_data->mutex);
  stop_data->done = TRUE;
  g_cond_signal (&stop_data->cond);
  g_mutex_unlock (&stop_data->mutex);

  return G_SOURCE_REMOVE;
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
    GstdHttpStopData stop_data;

    stop_data.server = self->server;
    stop_data.done = FALSE;
    g_mutex_init (&stop_data.mutex);
    g_cond_init (&stop_data.cond);

    /* Disconnect on the context the server dispatches on: closing
     * listeners and connections from another thread races that
     * dispatch and triggers GLib criticals. When no loop is running
     * (daemon shutdown), invoke acquires the context and runs the
     * callback inline; when a loop thread owns it, the callback is
     * queued there and we wait for it. */
    g_main_context_invoke (NULL, gstd_http_disconnect_cb, &stop_data);

    g_mutex_lock (&stop_data.mutex);
    while (!stop_data.done) {
      g_cond_wait (&stop_data.cond, &stop_data.mutex);
    }
    g_mutex_unlock (&stop_data.mutex);

    g_mutex_clear (&stop_data.mutex);
    g_cond_clear (&stop_data.cond);

    g_object_unref (self->server);
  }
  self->server = NULL;

  return GSTD_EOK;
}
