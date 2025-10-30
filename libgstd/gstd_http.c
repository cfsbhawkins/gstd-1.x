// Replace the conflicted section with this:

static void
parse_json_body (SoupMsg *msg, gchar **out_name, gchar **out_desc)
{
  const char *content_type = NULL;
  JsonParser *parser = NULL;
  JsonNode *root = NULL;
  GError *err = NULL;
  SoupMessageBody *request_body = NULL;
  const char *body_data = NULL;
  gsize body_length = 0;

  *out_name = NULL;
  *out_desc = NULL;

#if SOUP_CHECK_VERSION(3,0,0)
  GBytes *request_body_bytes = soup_server_message_get_request_body (msg);
  if (!request_body_bytes)
    return;
  
  body_data = g_bytes_get_data (request_body_bytes, &body_length);
  if (body_length == 0)
    return;

  SoupMessageHeaders *request_headers = soup_server_message_get_request_headers (msg);
  content_type = soup_message_headers_get_content_type (request_headers, NULL);
#else
  soup_message_body_flatten (msg->request_body);
  if (!msg->request_body || msg->request_body->length == 0)
    return;

  body_data = msg->request_body->data;
  body_length = msg->request_body->length;
  content_type = soup_message_headers_get_content_type (msg->request_headers, NULL);
#endif

  if (!content_type || !g_str_has_prefix (content_type, "application/json"))
    return;

  parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, body_data, body_length, &err)) {
    g_clear_error (&err);
    g_object_unref (parser);
    return;
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

  self = GSTD_HTTP (data);
  session = self->session;

  data_request = (GstdHttpRequest *) malloc (sizeof (GstdHttpRequest));

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
  response_headers = soup_server_message_get_request_headers (msg);
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
    GST_ERROR_OBJECT (self->pool, "Thread pool push failed");
  }

}
