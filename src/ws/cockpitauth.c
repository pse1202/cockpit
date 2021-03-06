/*
 * This file is part of Cockpit.
 *
 * Copyright (C) 2013 Red Hat, Inc.
 *
 * Cockpit is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * Cockpit is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Cockpit; If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "cockpitauth.h"

#include "cockpitauthpipe.h"
#include "cockpitsshtransport.h"
#include "cockpitws.h"

#include "websocket/websocket.h"

#include "common/cockpitconf.h"
#include "common/cockpiterror.h"
#include "common/cockpithex.h"
#include "common/cockpitlog.h"
#include "common/cockpitjson.h"
#include "common/cockpitpipe.h"
#include "common/cockpitpipetransport.h"
#include "common/cockpitmemory.h"
#include "common/cockpitunixfd.h"
#include "common/cockpitsystem.h"
#include "common/cockpitwebserver.h"

#include <security/pam_appl.h>

#include <sys/socket.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#define ACTION_SPAWN_HEADER "spawn-login-with-header"
#define ACTION_SPAWN_DECODE "spawn-login-with-decoded"
#define ACTION_SSH "remote-login-ssh"
#define ACTION_LOGIN_REPLY "x-login-reply"
#define LOGIN_REPLY_HEADER "X-Login-Reply"
#define ACTION_NONE "none"

#define MAX_AUTH_TIMEOUT 900
#define MIN_AUTH_TIMEOUT 1

/* Timeout of authenticated session when no connections */
guint cockpit_ws_service_idle = 15;

/* Timeout of everything when noone is connected */
guint cockpit_ws_process_idle = 90;

/* The amount of time a spawned process has to complete authentication */
guint cockpit_ws_auth_process_timeout = 30;
guint cockpit_ws_auth_response_timeout = 60;

/* Maximum number of pending authentication requests */
const gchar *cockpit_ws_max_startups = NULL;

static guint max_startups = 10;

static guint sig__idling = 0;

static gboolean gssapi_not_avail = FALSE;

G_DEFINE_TYPE (CockpitAuth, cockpit_auth, G_TYPE_OBJECT)

typedef struct {
  gchar *cookie;
  CockpitAuth *auth;
  CockpitCreds *creds;
  CockpitWebService *service;
  guint timeout_tag;
  gulong idling_sig;
  gulong destroy_sig;
} CockpitAuthenticated;

static void
cockpit_authenticated_destroy (CockpitAuthenticated *authenticated)
{
  CockpitAuth *self = authenticated->auth;
  g_hash_table_remove (self->authenticated, authenticated->cookie);
}

static void
on_web_service_gone (gpointer data,
                     GObject *where_the_object_was)
{
  CockpitAuthenticated *authenticated = data;
  authenticated->service = NULL;
  cockpit_authenticated_destroy (authenticated);
}

static void
cockpit_authenticated_free (gpointer data)
{
  CockpitAuthenticated *authenticated = data;
  GObject *object;

  if (authenticated->timeout_tag)
    g_source_remove (authenticated->timeout_tag);

  g_free (authenticated->cookie);
  cockpit_creds_poison (authenticated->creds);
  cockpit_creds_unref (authenticated->creds);

  if (authenticated->service)
    {
      if (authenticated->idling_sig)
        g_signal_handler_disconnect (authenticated->service, authenticated->idling_sig);
      if (authenticated->destroy_sig)
        g_signal_handler_disconnect (authenticated->service, authenticated->destroy_sig);
      object = G_OBJECT (authenticated->service);
      g_object_weak_unref (object, on_web_service_gone, authenticated);
      g_object_run_dispose (object);
      g_object_unref (authenticated->service);
    }

  g_free (authenticated);
}

typedef struct {
  CockpitAuthPipe *auth_pipe;
  gchar *id;
  gchar *response_data;
  GSimpleAsyncResult *pending_result;

  gint refs;
  gpointer tag;
  gpointer user_data;
  GDestroyNotify destroy_func;

} AuthData;

static void
auth_data_free (gpointer data)
{
  AuthData *ad = data;

  g_return_if_fail (ad->pending_result == NULL);

  if (ad->auth_pipe) {
    g_signal_handlers_disconnect_by_data (ad->auth_pipe, ad);
    g_object_unref (ad->auth_pipe);
  }

  if (ad->destroy_func && ad->user_data)
    ad->destroy_func (ad->user_data);

  g_free (ad->id);
  g_free (ad->response_data);

  g_free (ad);
}

static void
auth_data_add_pending_result (AuthData *data,
                              GSimpleAsyncResult *result)
{
  g_return_if_fail (data->pending_result == NULL);
  data->pending_result = g_object_ref (result);
}

static void
auth_data_complete_result (AuthData *data,
                           GError *error)
{
  if (data->pending_result)
    {
      if (error)
        g_simple_async_result_set_from_error (data->pending_result, error);

      g_simple_async_result_complete_in_idle (data->pending_result);

      g_object_unref (data->pending_result);
      data->pending_result = NULL;
    }
  else if (error)
    {
      g_message ("Dropped authentication error: %s no pending request to respond to", error->message);
    }
  else
    {
      g_message ("Dropped authentication result, no pending request to respond to");
    }
}

static AuthData *
auth_data_ref (AuthData *data)
{
  g_return_val_if_fail (data != NULL, NULL);
  data->refs++;
  return data;
}

static void
auth_data_unref (gpointer data)
{
  AuthData *d = data;
  g_return_if_fail (data != NULL);
  d->refs--;
  if (d->refs == 0)
    auth_data_free (data);
}

static void
cockpit_auth_finalize (GObject *object)
{
  CockpitAuth *self = COCKPIT_AUTH (object);
  if (self->timeout_tag)
    g_source_remove (self->timeout_tag);
  g_bytes_unref (self->key);
  g_hash_table_destroy (self->authenticated);
  g_hash_table_destroy (self->authentication_pending);
  G_OBJECT_CLASS (cockpit_auth_parent_class)->finalize (object);
}

static gboolean
on_process_timeout (gpointer data)
{
  CockpitAuth *self = COCKPIT_AUTH (data);

  self->timeout_tag = 0;
  if (g_hash_table_size (self->authenticated) == 0 &&
      g_hash_table_size (self->authentication_pending) == 0)
    {
      g_debug ("web service is idle");
      g_signal_emit (self, sig__idling, 0);
    }

  return FALSE;
}

static void
cockpit_auth_init (CockpitAuth *self)
{
  self->key = cockpit_system_random_nonce (128);
  if (!self->key)
    g_error ("couldn't read random key, startup aborted");

  self->authenticated = g_hash_table_new_full (g_str_hash, g_str_equal,
                                               NULL, cockpit_authenticated_free);

  self->authentication_pending = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                        NULL, auth_data_unref);

  self->timeout_tag = g_timeout_add_seconds (cockpit_ws_process_idle,
                                             on_process_timeout, self);

  self->startups = 0;
  self->max_startups = max_startups;
  self->max_startups_begin = max_startups;
  self->max_startups_rate = 100;
}

gchar *
cockpit_auth_nonce (CockpitAuth *self)
{
  const guchar *key;
  gsize len;
  guint64 seed;

  seed = self->nonce_seed++;
  key = g_bytes_get_data (self->key, &len);
  return g_compute_hmac_for_data (G_CHECKSUM_SHA256, key, len,
                                  (guchar *)&seed, sizeof (seed));
}

static void
purge_auth_id (CockpitAuthPipe *auth_pipe,
               GError *error,
               gpointer user_data)
{
  CockpitAuth *self = user_data;
  const gchar *id = cockpit_auth_pipe_get_id (auth_pipe);
  g_hash_table_remove (self->authentication_pending, id);
}

static void
cockpit_auth_prepare_login_reply (CockpitAuth *self,
                                  JsonObject *prompt_data,
                                  GHashTable *headers,
                                  AuthData *ad)
{
  const gchar *prompt;
  gchar *encoded_data = NULL;

  g_return_if_fail (ad->pending_result == NULL);

  // Will fail if prompt is not present
  prompt = json_object_get_string_member (prompt_data, "prompt");
  encoded_data = g_base64_encode ((guint8 *)prompt, strlen (prompt));

  g_hash_table_replace (headers, g_strdup ("WWW-Authenticate"),
                        g_strdup_printf ("%s %s %s", LOGIN_REPLY_HEADER,
                                         ad->id, encoded_data));

  g_hash_table_insert (self->authentication_pending, ad->id, auth_data_ref (ad));
  g_signal_connect (ad->auth_pipe, "close", G_CALLBACK (purge_auth_id), self);

  json_object_remove_member (prompt_data, "prompt");
  g_free (encoded_data);
}

static inline gchar *
str_skip (gchar *v,
          gchar c)
{
  while (v[0] == c)
    v++;
  return v;
}

static void
clear_free_authorization (gpointer data)
{
  cockpit_secclear (data, strlen (data));
  g_free (data);
}

static const gchar *
parse_basic_auth_password (GBytes *input,
                           gchar **user)
{
  const gchar *password;
  const gchar *data;

  data = g_bytes_get_data (input, NULL);

  /* password is null terminated, see below */
  password = strchr (data, ':');
  if (password != NULL)
    {
      if (user)
        *user = g_strndup (data, password - data);
      password++;
    }

  return password;
}

/*
 * Returns the Authorization type from the headers
 * Does not modify the header hashtable.
 */
gchar *
cockpit_auth_parse_authorization_type (GHashTable *headers)
{
  gchar *line;
  gchar *type;
  gchar *next;
  gpointer key;
  gsize i;

  /* Avoid copying as it can contain passwords */
  if (!g_hash_table_lookup_extended (headers, "Authorization", &key, (gpointer *)&line))
    return NULL;

  line = str_skip (line, ' ');
  next = strchr (line, ' ');

  if (!next)
    return NULL;

  type = g_strndup (line, next - line);
  for (i = 0; type[i] != '\0'; i++)
    type[i] = g_ascii_tolower (type[i]);

  return type;
}

/*
 * Returns contents of Authorization header from the headers
 * Removes the Authorization header from the hashtable.
 */
GBytes *
cockpit_auth_parse_authorization (GHashTable *headers,
                                  gboolean base64_decode)
{
  gchar *line;
  gchar *next;
  gchar *contents;
  gsize length;
  gpointer key;

  /* Avoid copying as it can contain passwords */
  if (!g_hash_table_lookup_extended (headers, "Authorization", &key, (gpointer *)&line))
    return NULL;

  g_hash_table_steal (headers, "Authorization");
  g_free (key);

  line = str_skip (line, ' ');
  next = strchr (line, ' ');
  if (!next)
    {
      g_free (line);
      return NULL;
    }

  contents = str_skip (next, ' ');
  if (base64_decode)
    {
      if (g_base64_decode_inplace (contents, &length) == NULL)
        {
          g_free (line);
          return NULL;
        }

      /* Null terminate for convenience, but null count not included in GBytes */
      contents[length] = '\0';
    }
  else
    {
      length = strlen (contents);
    }

  /* Avoid copying by using the line directly */
  return g_bytes_new_with_free_func (contents, length, clear_free_authorization, line);
}

static const gchar *
type_option (const gchar *type,
             const gchar *option,
             const gchar *default_str)
{
  if (type && cockpit_conf_string (type, option))
    return cockpit_conf_string (type, option);

  return default_str;
}

static guint
timeout_option (const gchar *name,
                const gchar *type,
                guint default_value)
{
  guint timeout = default_value;
  guint64 conf_timeout;
  const gchar* conf = type_option (type, name, NULL);

  if (conf)
    {
      conf_timeout = g_ascii_strtoull (conf, NULL, 10);
      if (errno == ERANGE || errno == EINVAL)
        timeout = default_value;
      else if (conf_timeout > MAX_AUTH_TIMEOUT || conf_timeout > UINT_MAX)
        timeout = (MAX_AUTH_TIMEOUT > UINT_MAX) ? UINT_MAX : MAX_AUTH_TIMEOUT;
      else if (conf_timeout < MIN_AUTH_TIMEOUT)
        timeout = MIN_AUTH_TIMEOUT;
      else
        timeout = (guint)conf_timeout;

      if (conf_timeout != timeout)
        g_message ("Invalid %s timeout value '%s', setting to %u", type, conf, timeout);
    }

  return timeout;
}

/* ------------------------------------------------------------------------
 *  Login by spawning a new command
 */

typedef struct {
  gint process_in;
  gint process_out;
  GPid process_pid;

  GBytes *authorization;
  gchar *remote_peer;
  gchar *auth_type;
  gchar *application;
  const gchar *command;

} SpawnLoginData;

static void
spawn_login_data_free (gpointer data)
{
  SpawnLoginData *sl = data;

  if (sl->process_in != -1)
    close (sl->process_in);
  if (sl->process_out != -1)
    close (sl->process_out);

  if (sl->process_pid != 0)
    {
      g_child_watch_add (sl->process_pid, (GChildWatchFunc)g_spawn_close_pid, NULL);
      kill (sl->process_pid, SIGTERM);
    }

  if (sl->authorization)
    g_bytes_unref (sl->authorization);

  g_free (sl->auth_type);
  g_free (sl->application);
  g_free (sl->remote_peer);
  g_free (sl);
}

static void
spawn_child_setup (gpointer data)
{
  gint auth_fd = GPOINTER_TO_INT (data);
  if (cockpit_unix_fd_close_all (3, auth_fd) < 0)
    {
      g_printerr ("couldn't close file descriptors: %m");
      _exit (127);
    }

  /* When running a spawn login command fd 3 is always auth */
  if (dup2 (auth_fd, 3) < 0)
    {
      g_printerr ("couldn't dup file descriptor: %m");
      _exit (127);
    }

  close (auth_fd);
}


static void
build_gssapi_output_header (GHashTable *headers,
                            JsonObject *results)
{
  gchar *encoded;
  const gchar *output = NULL;
  gchar *value = NULL;
  gpointer data;
  gsize length;

  if (results)
    {
      if (!cockpit_json_get_string (results, "gssapi-output", NULL, &output))
        {
          g_warning ("received invalid gssapi-output field");
          return;
        }
    }

  if (!output)
    return;

  data = cockpit_hex_decode (output, &length);
  if (!data)
    {
      g_warning ("received invalid gssapi-output field");
      return;
    }
  if (length)
    {
      encoded = g_base64_encode (data, length);
      value = g_strdup_printf ("Negotiate %s", encoded);
      g_free (encoded);
    }
  else
    {
      value = g_strdup ("Negotiate");
    }
  g_free (data);

  g_hash_table_replace (headers, g_strdup ("WWW-Authenticate"), value);
  g_debug ("gssapi: WWW-Authenticate: %s", value);
}

static const gchar *
auth_parse_application (const gchar *path,
                        gchar **memory)
{
  const gchar *pos;

  g_return_val_if_fail (path != NULL, NULL);
  g_return_val_if_fail (path[0] == '/', NULL);

  path += 1;

  /* We are being embedded as a specific application */
  if (g_str_has_prefix (path, "cockpit+") && path[8] != '\0')
    {
      pos = strchr (path, '/');
      if (pos)
        {
          *memory = g_strndup (path, pos - path);
          return *memory;
        }
      else
        {
          return path;
        }
    }
  else
    {
      return "cockpit";
    }
}

static CockpitCreds *
create_creds_for_spawn_authenticated (CockpitAuth *self,
                                      const gchar *user,
                                      SpawnLoginData *sl,
                                      JsonObject *results,
                                      const gchar *raw_data)
{
  const gchar *password = NULL;
  const gchar *gssapi_creds = NULL;
  CockpitCreds *creds = NULL;
  gchar *csrf_token;

  /*
   * Dig the password out of the authorization header, rather than having
   * passing it back and forth possibly leaking it.
   */

  if (g_str_equal (sl->auth_type, "basic"))
    password = parse_basic_auth_password (sl->authorization, NULL);

  if (!cockpit_json_get_string (results, "gssapi-creds", NULL, &gssapi_creds))
    {
      g_warning ("received bad gssapi-creds from %s", sl->command);
      gssapi_creds = NULL;
    }

  csrf_token = cockpit_auth_nonce (self);

  creds = cockpit_creds_new (user,
                             sl->application,
                             COCKPIT_CRED_LOGIN_DATA, raw_data,
                             COCKPIT_CRED_PASSWORD, password,
                             COCKPIT_CRED_RHOST, sl->remote_peer,
                             COCKPIT_CRED_GSSAPI, gssapi_creds,
                             COCKPIT_CRED_CSRF_TOKEN, csrf_token,
                             NULL);

  g_free (csrf_token);
  return creds;
}

static CockpitCreds *
parse_cockpit_spawn_results (CockpitAuth *self,
                             SpawnLoginData *sl,
                             gchar *response_data,
                             GHashTable *headers,
                             JsonObject **prompt_data,
                             GError **error)
{
  CockpitCreds *creds = NULL;
  GError *json_error = NULL;
  JsonObject *results = NULL;
  const gchar *user;
  const gchar *error_str;
  const gchar *prompt;
  const gchar *message;

  g_debug ("%s says: %s", sl->command, response_data);

  if (response_data)
    {
      results = cockpit_json_parse_object (response_data,
                                           strlen (response_data),
                                           &json_error);
    }

  if (g_error_matches (json_error, JSON_PARSER_ERROR, JSON_PARSER_ERROR_INVALID_DATA))
    {
      g_message ("got non-utf8 user name from %s", sl->command);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Login user name is not UTF8 encoded");
      g_error_free (json_error);
    }
  else if (!results)
    {
      g_warning ("couldn't parse %s auth output: %s",
                 sl->command,
                 json_error ? json_error->message : NULL);
      g_error_free (json_error);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Authentication failed: no results");
    }
  else if (!cockpit_json_get_string (results, "error", NULL, &error_str) ||
           !cockpit_json_get_string (results, "message", NULL, &message) ||
           !cockpit_json_get_string (results, "prompt", NULL, &prompt))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Authentication failed: invalid results");
    }
  else
    {
      if (prompt && prompt_data)
        {
          *prompt_data = json_object_ref (results);
          g_set_error (error, COCKPIT_ERROR, COCKPIT_ERROR_AUTHENTICATION_FAILED,
                       "X-Login-Reply needed");
        }
      else if (!error_str)
        {
          if (!cockpit_json_get_string (results, "user", NULL, &user) || !user)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "Authentication failed: missing user");
            }
          else
            {
              g_debug ("user authenticated as %s", user);
              creds = create_creds_for_spawn_authenticated (self, user, sl,
                                                            results,
                                                            response_data);
            }
        }
      else
        {
          if (g_strcmp0 (error_str, "authentication-unavailable") == 0 &&
              g_str_equal (sl->auth_type, "negotiate"))
            {
              gssapi_not_avail = TRUE;
              g_debug ("negotiate auth is not available, disabling");
              g_set_error (error, COCKPIT_ERROR, COCKPIT_ERROR_AUTHENTICATION_FAILED,
                           "Negotiate authentication not available");
            }
          else if (g_strcmp0 (error_str, "authentication-failed") == 0 ||
                   g_strcmp0 (error_str, "authentication-unavailable") == 0)
            {
              g_debug ("%s %s", error_str, message);
              g_set_error (error, COCKPIT_ERROR, COCKPIT_ERROR_AUTHENTICATION_FAILED,
                           "Authentication failed");
            }
          else if (g_strcmp0 (error_str, "permission-denied") == 0)
            {
              g_debug ("permission denied %s", message);
              g_set_error (error, COCKPIT_ERROR, COCKPIT_ERROR_PERMISSION_DENIED,
                           "Permission denied");
            }
          else
            {
              g_debug ("error from %s: %s: %s", sl->command,
                       error_str, message);
              g_set_error (error, COCKPIT_ERROR, COCKPIT_ERROR_FAILED,
                           "Authentication failed: %s: %s",
                           error_str,
                           message);
            }
        }
    }

  build_gssapi_output_header (headers, results);

  if (results)
    json_object_unref (results);

  return creds;
}

static void
on_auth_pipe_result (CockpitAuthPipe *auth_pipe,
                     GBytes *message,
                     gpointer user_data)
{
  AuthData *ad = user_data;
  gsize len;

  len = g_bytes_get_size (message);
  g_return_if_fail (ad->response_data == NULL);
  ad->response_data = g_strndup (g_bytes_get_data(message, NULL), len);
  auth_data_complete_result (ad, NULL);
}

static void
on_spawn_auth_pipe_close (CockpitAuthPipe *auth_pipe,
                          GError *error,
                          gpointer user_data)
{
  AuthData *ad = user_data;
  /* Only report errors */
  if (error || ad->pending_result)
    auth_data_complete_result (ad, error);
}


static void
cockpit_auth_spawn_login_async (CockpitAuth *self,
                                const gchar *application,
                                const gchar *type,
                                gboolean decode_header,
                                GHashTable *headers,
                                const gchar *remote_peer,
                                GAsyncReadyCallback callback,
                                gpointer user_data)
{
  GSimpleAsyncResult *result;
  SpawnLoginData *sl;
  AuthData *ad = NULL;
  gint child_pd;

  GBytes *input = NULL;
  const gchar *command;
  GError *error = NULL;

  const gchar *argv[] = {
      "command",
      type,
      remote_peer ? remote_peer : "",
      NULL,
  };

  result = g_simple_async_result_new (G_OBJECT (self), callback, user_data,
                                      cockpit_auth_spawn_login_async);

  command = type_option (type, "command", cockpit_ws_session_program);

  input = cockpit_auth_parse_authorization (headers, decode_header);
  if (!input && !gssapi_not_avail && g_strcmp0 (type, "negotiate") == 0)
    input = g_bytes_new_static ("", 0);

  if (input && application)
    {
      ad = g_new0 (AuthData, 1);
      ad->refs = 1;
      ad->id = cockpit_auth_nonce (self);
      ad->pending_result = NULL;
      ad->response_data = NULL;
      ad->tag = cockpit_auth_spawn_login_async;
      ad->destroy_func = spawn_login_data_free;
      ad->user_data = NULL;
      ad->auth_pipe = g_object_new (COCKPIT_TYPE_AUTH_PIPE,
                                    "pipe-timeout", timeout_option ("timeout", type,
                                                                    cockpit_ws_auth_process_timeout),
                                    "idle-timeout", timeout_option ("response-timeout", type,
                                                                    cockpit_ws_auth_response_timeout),
                                    "id", ad->id,
                                    "logname", command,
                                    NULL);

      sl = g_new0 (SpawnLoginData, 1);
      sl->remote_peer = g_strdup (remote_peer);
      sl->auth_type = g_strdup (type);
      sl->authorization = g_bytes_ref (input);
      sl->application = g_strdup (application);
      argv[0] = sl->command = command;
      sl->process_in = -1;
      sl->process_out = -1;

      ad->user_data = sl;

      child_pd = cockpit_auth_pipe_steal_fd (ad->auth_pipe);
      g_simple_async_result_set_op_res_gpointer (result,
                                                 auth_data_ref (ad), auth_data_unref);

      g_debug ("spawning %s", argv[0]);

      if (g_spawn_async_with_pipes (NULL, (gchar **) argv, NULL,
                                     G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_LEAVE_DESCRIPTORS_OPEN,
                                     spawn_child_setup, GINT_TO_POINTER (child_pd),
                                     &sl->process_pid, &sl->process_in, &sl->process_out, NULL, &error))
        {
          auth_data_add_pending_result (ad, result);
          g_signal_connect (ad->auth_pipe, "message",
                            G_CALLBACK (on_auth_pipe_result),
                            ad);
          g_signal_connect (ad->auth_pipe, "close",
                            G_CALLBACK (on_spawn_auth_pipe_close),
                            ad);
          cockpit_auth_pipe_answer (ad->auth_pipe, input);
        }
      else
        {
          g_warning ("failed to start %s: %s", argv[0], error->message);
          g_error_free (error);

          g_simple_async_result_set_error (result, COCKPIT_ERROR, COCKPIT_ERROR_FAILED,
                                           "Internal error starting %s", command);
          g_simple_async_result_complete_in_idle (result);
        }

      /* Child process end of pipe */
      close (child_pd);
    }
  else
    {
      g_simple_async_result_set_error (result, COCKPIT_ERROR, COCKPIT_ERROR_AUTHENTICATION_FAILED,
                                       "Authentication required");
      g_simple_async_result_complete_in_idle (result);
    }

  if (input)
    g_bytes_unref (input);

  if (ad)
    auth_data_unref (ad);

  g_object_unref (result);
}

static CockpitCreds *
cockpit_auth_spawn_login_finish (CockpitAuth *self,
                                 GAsyncResult *result,
                                 GHashTable *headers,
                                 JsonObject **prompt_data,
                                 CockpitTransport **transport,
                                 GError **error)
{
  CockpitCreds *creds;
  SpawnLoginData *sl;
  AuthData *ad;

  CockpitPipe *pipe;

  g_return_val_if_fail (g_simple_async_result_is_valid (result, G_OBJECT (self),
                        cockpit_auth_spawn_login_async), NULL);

  ad = g_simple_async_result_get_op_res_gpointer (G_SIMPLE_ASYNC_RESULT (result));

  if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (result), error))
    return NULL;

  sl = ad->user_data;
  creds = parse_cockpit_spawn_results (self, sl, ad->response_data,
                                       headers, prompt_data, error);

  if (creds)
    {
      if (transport)
        {
          pipe = g_object_new (COCKPIT_TYPE_PIPE,
                               "name", "localhost",
                               "pid", sl->process_pid,
                               "in-fd", sl->process_out,
                               "out-fd", sl->process_in,
                               NULL);
          sl->process_pid = 0;
          sl->process_out = -1;
          sl->process_in = -1;

          *transport = cockpit_pipe_transport_new (pipe);
          g_object_unref (pipe);
        }
    }
  else
    {
      if (prompt_data && *prompt_data)
        {
          cockpit_auth_prepare_login_reply (self, *prompt_data, headers, ad);
        }
      else if (sl->process_pid > 0)
        {
          kill (sl->process_pid, SIGTERM);
          sl->process_pid = 0;
        }
    }

  g_free (ad->response_data);
  ad->response_data = NULL;

  return creds;
}


/* ------------------------------------------------------------------------
 * Remote login by using ssh even locally
 */

typedef struct {
  CockpitCreds *creds;
  CockpitTransport *transport;
  gboolean has_transport_result;
} RemoteLoginData;

static void
remote_login_data_free (gpointer data)
{
  RemoteLoginData *rl = data;
  if (rl->creds)
    cockpit_creds_unref (rl->creds);
  if (rl->transport)
    g_object_unref (rl->transport);
  g_free (rl);
}

static void
on_remote_login_done (CockpitSshTransport *transport,
                      const gchar *problem,
                      gpointer user_data)
{
  AuthData *ad = user_data;
  GHashTable *results = NULL;
  const gchar *pw_result;
  GError *error = NULL;
  RemoteLoginData *rl = ad->user_data;

  if (problem)
    {
      if (g_str_equal (problem, "authentication-failed"))
        {
          results = cockpit_ssh_transport_get_auth_method_results (transport);
          pw_result = g_hash_table_lookup (results, "password");
          if (!pw_result || g_strcmp0 (pw_result, "no-server-support") == 0)
            {
              g_set_error (&error, COCKPIT_ERROR,
                           COCKPIT_ERROR_AUTHENTICATION_FAILED,
                           "Authentication failed: authentication-not-supported");
            }
          else
            {
              g_set_error (&error, COCKPIT_ERROR,
                           COCKPIT_ERROR_AUTHENTICATION_FAILED,
                           "Authentication failed");
            }
        }
      else if (g_str_equal (problem, "terminated"))
        {
              g_set_error (&error, COCKPIT_ERROR,
                                               COCKPIT_ERROR_AUTHENTICATION_FAILED,
                                               "Authentication failed: terminated");
        }
      else
        {
          g_set_error (&error, COCKPIT_ERROR, COCKPIT_ERROR_FAILED,
                       "Couldn't connect or authenticate: %s", problem);
        }
    }

  rl->has_transport_result = TRUE;
  auth_data_complete_result (ad, error);
  if (error)
    g_error_free (error);
}


static JsonObject *
parse_ssh_prompt_results (CockpitAuth *self,
                          gchar *response_data,
                          GError **error)
{
  GError *json_error = NULL;
  JsonObject *results = NULL;
  const gchar *prompt;
  gboolean success = FALSE;

  g_debug ("ssh auth says: %s", response_data);

  results = cockpit_json_parse_object (response_data,
                                       strlen (response_data),
                                       &json_error);

  if (g_error_matches (json_error, JSON_PARSER_ERROR, JSON_PARSER_ERROR_INVALID_DATA))
    {
      g_message ("got non-utf8 data from ssh connection");
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Data is not UTF8 encoded");
      g_error_free (json_error);
    }
  else if (!results)
    {
      g_warning ("couldn't parse ssh auth output: %s",
                 json_error ? json_error->message : NULL);
      g_error_free (json_error);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Authentication failed: no results");
    }
  else if (!cockpit_json_get_string (results, "prompt", NULL, &prompt))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Authentication failed: invalid results");
    }
  else if (!prompt)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Authentication failed: missing prompt");
    }
  else
    {
      g_set_error (error, COCKPIT_ERROR, COCKPIT_ERROR_AUTHENTICATION_FAILED,
                   "X-Login-Reply needed");
      success = TRUE;
    }

  if (!success && results)
    {
      json_object_unref (results);
      results = NULL;
    }

  return results;
}

static void
cockpit_auth_remote_login_async (CockpitAuth *self,
                                 const gchar *application,
                                 const gchar *type,
                                 GHashTable *headers,
                                 const gchar *remote_peer,
                                 GAsyncReadyCallback callback,
                                 gpointer user_data)
{
  GSimpleAsyncResult *task;
  CockpitCreds *creds = NULL;
  AuthData *ad = NULL;
  RemoteLoginData *rl;
  const gchar *password;
  gchar *csrf_token;
  GBytes *input;
  gchar *user = NULL;

  task = g_simple_async_result_new (G_OBJECT (self), callback, user_data,
                                    cockpit_auth_remote_login_async);

  input = cockpit_auth_parse_authorization (headers, TRUE);

  if (application && type && input && g_str_equal (type, "basic"))
    {
      password = parse_basic_auth_password (input, &user);
      if (password && user)
        {
          csrf_token = cockpit_auth_nonce (self);

          creds = cockpit_creds_new (user,
                                     application,
                                     COCKPIT_CRED_PASSWORD, password,
                                     COCKPIT_CRED_RHOST, remote_peer,
                                     COCKPIT_CRED_CSRF_TOKEN, csrf_token,
                                     NULL);

          g_free (csrf_token);
        }
      g_free (user);
    }

  if (creds)
    {
      ad = g_new0 (AuthData, 1);
      ad->refs = 1;
      ad->id = cockpit_auth_nonce (self);
      ad->pending_result = NULL;
      ad->response_data = NULL;
      ad->tag = cockpit_auth_remote_login_async;
      ad->destroy_func = remote_login_data_free;
      ad->user_data = NULL;
      ad->auth_pipe = g_object_new (COCKPIT_TYPE_AUTH_PIPE,
                                    "pipe-timeout", timeout_option ("timeout", type,
                                                                    cockpit_ws_auth_process_timeout),
                                    "idle-timeout", timeout_option ("response-timeout", type,
                                                                    cockpit_ws_auth_response_timeout),
                                    "id", ad->id,
                                    "logname", "ssh (localhost)",
                                    NULL);

      rl = g_new0 (RemoteLoginData, 1);
      rl->creds = creds;
      rl->has_transport_result = FALSE;
      rl->transport = g_object_new (COCKPIT_TYPE_SSH_TRANSPORT,
                                    "host", type_option (ACTION_SSH, "host", "127.0.0.1"),
                                    "port", cockpit_ws_specific_ssh_port,
                                    "command", cockpit_ws_bridge_program,
                                    "creds", creds,
                                    "ignore-key", TRUE,
                                    "auth-pipe", ad->auth_pipe,
                                    NULL);
      ad->user_data = rl;

      g_simple_async_result_set_op_res_gpointer (task, auth_data_ref (ad), auth_data_unref);
      auth_data_add_pending_result (ad, task);
      g_signal_connect (rl->transport, "result", G_CALLBACK (on_remote_login_done), ad);
      g_signal_connect (ad->auth_pipe, "message", G_CALLBACK (on_auth_pipe_result), ad);
    }
  else
    {
      g_simple_async_result_set_error (task, COCKPIT_ERROR, COCKPIT_ERROR_AUTHENTICATION_FAILED,
                                       "Basic authentication required");
      g_simple_async_result_complete_in_idle (task);
    }

  if (ad)
    auth_data_unref (ad);

  g_object_unref (task);
}

static CockpitCreds *
cockpit_auth_remote_login_finish (CockpitAuth *self,
                                  GAsyncResult *result,
                                  GHashTable *headers,
                                  JsonObject **prompt_data,
                                  CockpitTransport **transport,
                                  GError **error)
{
  AuthData *ad;
  RemoteLoginData *rl;
  CockpitCreds *creds = NULL;
  JsonObject *results = NULL;

  g_return_val_if_fail (g_simple_async_result_is_valid (result, G_OBJECT (self),
                        cockpit_auth_remote_login_async), NULL);

  if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (result), error))
    return NULL;

  ad = g_simple_async_result_get_op_res_gpointer (G_SIMPLE_ASYNC_RESULT (result));
  rl = ad->user_data;

  if (rl->has_transport_result)
    {
      creds = cockpit_creds_ref (rl->creds);
      if (transport)
          *transport = g_object_ref (rl->transport);
    }
  else
    {
      results = parse_ssh_prompt_results (self, ad->response_data, error);
      if (!results)
        cockpit_transport_close (COCKPIT_TRANSPORT (rl->transport), "internal-error");
    }

  if (results)
    {
      if (prompt_data)
        {
          cockpit_auth_prepare_login_reply (self, results, headers, ad);
          *prompt_data = json_object_ref (results);
        }
      json_object_unref (results);
    }

  g_free (ad->response_data);
  ad->response_data = NULL;

  return creds;
}

/* ---------------------------------------------------------------------- */

static void
cockpit_auth_none_login_async (CockpitAuth *self,
                               GAsyncReadyCallback callback,
                               gpointer user_data)
{
  GSimpleAsyncResult *task;

  task = g_simple_async_result_new (G_OBJECT (self), callback, user_data,
                                    cockpit_auth_none_login_async);

  g_simple_async_result_set_error (task, COCKPIT_ERROR, COCKPIT_ERROR_AUTHENTICATION_FAILED,
                                   "Authentication disabled");
  g_simple_async_result_complete_in_idle (task);
  g_object_unref (task);
}


static CockpitCreds *
cockpit_auth_none_login_finish (CockpitAuth *self,
                                  GAsyncResult *result,
                                  GHashTable *headers,
                                  JsonObject **prompt_data,
                                  CockpitTransport **transport,
                                  GError **error)
{
  g_return_val_if_fail (g_simple_async_result_is_valid (result, G_OBJECT (self),
                        cockpit_auth_none_login_async), NULL);

  g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (result), error);
  return NULL;
}

/* ---------------------------------------------------------------------- */

static void
cockpit_auth_resume_async (CockpitAuth *self,
                           const gchar *application,
                           const gchar *type,
                           GHashTable *headers,
                           const gchar *remote_peer,
                           GAsyncReadyCallback callback,
                           gpointer user_data)
{
  AuthData *ad = NULL;
  GSimpleAsyncResult *task;
  GBytes *input = NULL;
  gchar **parts = NULL;
  gchar *header = NULL;
  gsize length;

  header = g_hash_table_lookup (headers, "Authorization");
  if (header)
    parts = g_strsplit (header, " ", 3);

  if (parts && g_strv_length (parts) == 3)
      ad = g_hash_table_lookup (self->authentication_pending, parts[1]);

  if (!ad)
    {
      task = g_simple_async_result_new (G_OBJECT (self), callback, user_data,
                                        cockpit_auth_none_login_async);

      g_simple_async_result_set_error (task, COCKPIT_ERROR,
                                       COCKPIT_ERROR_AUTHENTICATION_FAILED,
                                       "Invalid resume token");
      g_simple_async_result_complete_in_idle (task);
    }
  else
    {
      g_signal_handlers_disconnect_by_data (ad->auth_pipe, self);

      task = g_simple_async_result_new (G_OBJECT (self), callback, user_data,
                                        ad->tag);
      g_simple_async_result_set_op_res_gpointer (task, auth_data_ref (ad), auth_data_unref);
      g_hash_table_remove (self->authentication_pending, parts[1]);

      if (!g_base64_decode_inplace (parts[2], &length) || length < 1)
        {
          g_simple_async_result_set_error (task, COCKPIT_ERROR,
                                           COCKPIT_ERROR_AUTHENTICATION_FAILED,
                                           "Invalid resume token");
          g_simple_async_result_complete_in_idle (task);
        }
      else
        {
          input = g_bytes_new (parts[2], length);
          auth_data_add_pending_result (ad, task);
          cockpit_auth_pipe_answer (ad->auth_pipe, input);
          g_bytes_unref (input);
        }
    }

  if (parts)
    g_strfreev (parts);
  g_object_unref (task);
}

static const gchar *
action_for_type (const gchar *type,
                 gboolean force_ssh)
{
  const gchar *action;

  g_return_val_if_fail (type != NULL, NULL);

  if (g_strcmp0 (type, ACTION_LOGIN_REPLY) == 0)
    action = ACTION_LOGIN_REPLY;

  /* ssh only supports basic right now */
  else if (force_ssh && g_strcmp0 (type, "basic") == 0)
    action = ACTION_SSH;

  else if (type && cockpit_conf_string (type, "action"))
      action = cockpit_conf_string (type, "action");

  else if (g_strcmp0 (type, "basic") == 0 ||
           g_strcmp0 (type, "negotiate") == 0)
      action = ACTION_SPAWN_DECODE;

  else
      action = ACTION_NONE;

  return action;
}

static void
cockpit_auth_choose_login_async (CockpitAuth *self,
                                 const gchar *path,
                                 GHashTable *headers,
                                 const gchar *remote_peer,
                                 GAsyncReadyCallback callback,
                                 gpointer user_data)
{
  const gchar *action;
  gchar *application = NULL;
  gchar *type = NULL;

  application = cockpit_auth_parse_application (path);
  type = cockpit_auth_parse_authorization_type (headers);
  if (!type)
    type = g_strdup ("negotiate");

  action = action_for_type (type, self->login_loopback);
  if (g_strcmp0 (action, ACTION_SPAWN_HEADER) == 0)
    {
      cockpit_auth_spawn_login_async (self, application, type, FALSE,
                                      headers, remote_peer,
                                      callback, user_data);
    }
  else if (g_strcmp0 (action, ACTION_SPAWN_DECODE) == 0)
    {
      cockpit_auth_spawn_login_async (self, application, type, TRUE,
                                       headers, remote_peer,
                                       callback, user_data);
    }
  else if (g_strcmp0 (action, ACTION_SSH) == 0)
    {
      cockpit_auth_remote_login_async (self, application, type,
                                       headers, remote_peer,
                                       callback, user_data);
    }
  else if (g_strcmp0 (action, ACTION_LOGIN_REPLY) == 0)
    {
      cockpit_auth_resume_async (self, application, type,
                                 headers, remote_peer,
                                 callback, user_data);
    }
  else if (g_strcmp0 (action, ACTION_NONE) == 0)
    {
      cockpit_auth_none_login_async (self, callback, user_data);
    }
  else
    {
      g_message ("got unknown login action: %s", action);
      cockpit_auth_none_login_async (self, callback, user_data);
    }

  g_free (type);
  g_free (application);
}

static CockpitCreds *
cockpit_auth_choose_login_finish (CockpitAuth *self,
                                  GAsyncResult *result,
                                  GHashTable *headers,
                                  JsonObject **prompt_data,
                                  CockpitTransport **transport,
                                  GError **error)
{
  CockpitCreds *creds = NULL;

  if (g_simple_async_result_is_valid (result, G_OBJECT (self),
                                      cockpit_auth_spawn_login_async))
    {
      creds = cockpit_auth_spawn_login_finish (self, result, headers,
                                               prompt_data, transport, error);
    }
  else if (g_simple_async_result_is_valid (result, G_OBJECT (self),
                                           cockpit_auth_remote_login_async))
    {
      creds = cockpit_auth_remote_login_finish (self, result, headers,
                                                prompt_data, transport, error);
    }
  else if (g_simple_async_result_is_valid (result, G_OBJECT (self),
                                           cockpit_auth_none_login_async))
    {
      creds = cockpit_auth_none_login_finish (self, result, headers,
                                              prompt_data, transport, error);
    }
  else
    {
       g_critical ("Got invalid GAsyncResult. This is a programmer error.");
    }

    return creds;
}

static void
cockpit_auth_class_init (CockpitAuthClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = cockpit_auth_finalize;

  klass->login_async = cockpit_auth_choose_login_async;
  klass->login_finish = cockpit_auth_choose_login_finish;

  sig__idling = g_signal_new ("idling", COCKPIT_TYPE_AUTH, G_SIGNAL_RUN_FIRST,
                              0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static char *
base64_decode_string (const char *enc)
{
  if (enc == NULL)
    return NULL;

  char *dec = g_strdup (enc);
  gsize len;
  g_base64_decode_inplace (dec, &len);
  dec[len] = '\0';
  return dec;
}

static CockpitAuthenticated *
authenticated_for_headers (CockpitAuth *self,
                           const gchar *path,
                           GHashTable *in_headers)
{
  gchar *cookie = NULL;
  gchar *raw = NULL;
  const char *prefix = "v=2;k=";
  CockpitAuthenticated *ret = NULL;
  const gchar *application;
  gchar *memory = NULL;

  g_return_val_if_fail (self != NULL, FALSE);
  g_return_val_if_fail (in_headers != NULL, FALSE);

  application = auth_parse_application (path, &memory);
  if (!application)
    return NULL;

  raw = cockpit_web_server_parse_cookie (in_headers, application);
  if (raw)
    {
      cookie = base64_decode_string (raw);
      if (cookie != NULL)
        {
          if (g_str_has_prefix (cookie, prefix))
            ret = g_hash_table_lookup (self->authenticated, cookie);
          else
            g_debug ("invalid or unsupported cookie: %s", cookie);
          g_free (cookie);
        }
      g_free (raw);
    }

  g_free (memory);
  return ret;
}

CockpitWebService *
cockpit_auth_check_cookie (CockpitAuth *self,
                           const gchar *path,
                           GHashTable *in_headers)
{
  CockpitAuthenticated *authenticated;

  authenticated = authenticated_for_headers (self, path, in_headers);
  if (authenticated)
    {
      g_debug ("received %s credential cookie for user '%s'",
               cockpit_creds_get_application (authenticated->creds),
               cockpit_creds_get_user (authenticated->creds));
      return g_object_ref (authenticated->service);
    }
  else
    {
      g_debug ("received unknown/invalid credential cookie");
      return NULL;
    }
}

/*
 * returns TRUE if auth can proceed, FALSE otherwise.
 * dropping starts at connection max_startups_begin with a probability
 * of (max_startups_rate/100). the probability increases linearly until
 * all connections are dropped for startups > max_startups
 */

static gboolean
can_start_auth (CockpitAuth *self)
{
  int p, r;

  /* 0 means unlimited */
  if (self->max_startups == 0)
    return TRUE;

  /* Under soft limit */
  if (self->startups <= self->max_startups_begin)
    return TRUE;

  /* Over hard limit */
  if (self->startups > self->max_startups)
    return FALSE;

  /* If rate is 100, soft limit is hard limit */
  if (self->max_startups_rate == 100)
    return FALSE;

  p = 100 - self->max_startups_rate;
  p *= self->startups - self->max_startups_begin;
  p /= self->max_startups - self->max_startups_begin;
  p += self->max_startups_rate;
  r = g_random_int_range (0, 100);

  g_debug ("calculating if auth can start: (%u:%u:%u): p %d, r %d",
           self->max_startups_begin, self->max_startups_rate,
           self->max_startups, p, r);
  return (r < p) ? FALSE : TRUE;
}

void
cockpit_auth_login_async (CockpitAuth *self,
                          const gchar *path,
                          GHashTable *headers,
                          const gchar *remote_peer,
                          GAsyncReadyCallback callback,
                          gpointer user_data)
{
  CockpitAuthClass *klass = COCKPIT_AUTH_GET_CLASS (self);
  GSimpleAsyncResult *result = NULL;

  g_return_if_fail (klass->login_async != NULL);

  self->startups++;
  if (can_start_auth (self))
    {
      klass->login_async (self, path, headers, remote_peer, callback, user_data);
    }
  else
    {
      g_message ("Request dropped; too many startup connections: %u", self->startups);
      result = g_simple_async_result_new_error (G_OBJECT (self), callback, user_data,
                                                COCKPIT_ERROR, COCKPIT_ERROR_FAILED,
                                               "Connection closed by host");
      g_simple_async_result_complete_in_idle (result);
    }

  if (result)
    g_object_unref (result);
}

static gboolean
on_authenticated_timeout (gpointer data)
{
  CockpitAuthenticated *authenticated = data;
  CockpitAuth *self = authenticated->auth;

  authenticated->timeout_tag = 0;

  if (cockpit_web_service_get_idling (authenticated->service))
    {
      g_info ("%s: timed out", cockpit_creds_get_user (authenticated->creds));
      g_hash_table_remove (self->authenticated, authenticated->cookie);
    }

  return FALSE;
}

static void
on_web_service_idling (CockpitWebService *service,
                       gpointer data)
{
  CockpitAuthenticated *authenticated = data;

  if (authenticated->timeout_tag)
    g_source_remove (authenticated->timeout_tag);

  g_debug ("%s: login is idle", cockpit_creds_get_user (authenticated->creds));

  /*
   * The minimum amount of time before a request uses this new web service,
   * otherwise it will just go away.
   */
  authenticated->timeout_tag = g_timeout_add_seconds (cockpit_ws_service_idle,
                                                      on_authenticated_timeout,
                                                      authenticated);

  /*
   * Also reset the timer which checks whether anything is going on in the
   * entire process or not.
   */
  if (authenticated->auth->timeout_tag)
    g_source_remove (authenticated->auth->timeout_tag);

  authenticated->auth->timeout_tag = g_timeout_add_seconds (cockpit_ws_process_idle,
                                                            on_process_timeout, authenticated->auth);
}

static void
on_web_service_destroy (CockpitWebService *service,
                        gpointer data)
{
  on_web_service_idling (service, data);
  cockpit_authenticated_destroy (data);
}

JsonObject *
cockpit_auth_login_finish (CockpitAuth *self,
                           GAsyncResult *result,
                           CockpitAuthFlags flags,
                           GHashTable *out_headers,
                           GError **error)
{
  CockpitAuthClass *klass = COCKPIT_AUTH_GET_CLASS (self);
  CockpitAuthenticated *authenticated;
  CockpitTransport *transport = NULL;
  JsonObject *prompt_data = NULL;
  CockpitCreds *creds;
  gchar *cookie_b64 = NULL;
  gchar *header;
  gchar *id;

  g_return_val_if_fail (klass->login_finish != NULL, FALSE);
  creds = klass->login_finish (self, result, out_headers,
                               &prompt_data, &transport, error);
  self->startups--;

  if (creds == NULL)
    return prompt_data;

  id = cockpit_auth_nonce (self);
  authenticated = g_new0 (CockpitAuthenticated, 1);
  authenticated->cookie = g_strdup_printf ("v=2;k=%s", id);
  authenticated->creds = creds;
  authenticated->service = cockpit_web_service_new (creds, transport);
  authenticated->auth = self;

  authenticated->idling_sig = g_signal_connect (authenticated->service, "idling",
                                                G_CALLBACK (on_web_service_idling), authenticated);
  authenticated->destroy_sig = g_signal_connect (authenticated->service, "destroy",
                                                G_CALLBACK (on_web_service_destroy), authenticated);

  if (transport)
    g_object_unref (transport);

  g_object_weak_ref (G_OBJECT (authenticated->service),
                     on_web_service_gone, authenticated);

  /* Start off in the idling state, and begin a timeout during which caller must do something else */
  on_web_service_idling (authenticated->service, authenticated);

  g_hash_table_insert (self->authenticated, authenticated->cookie, authenticated);

  g_debug ("sending %s credential id '%s' for user '%s'", id,
           cockpit_creds_get_application (creds),
           cockpit_creds_get_user (creds));

  g_free (id);

  if (out_headers)
    {
      gboolean force_secure = !(flags & COCKPIT_AUTH_COOKIE_INSECURE);
      cookie_b64 = g_base64_encode ((guint8 *)authenticated->cookie, strlen (authenticated->cookie));
      header = g_strdup_printf ("%s=%s; Path=/; %s HttpOnly",
                                cockpit_creds_get_application (creds),
                                cookie_b64, force_secure ? " Secure;" : "");
      g_free (cookie_b64);
      g_hash_table_insert (out_headers, g_strdup ("Set-Cookie"), header);
    }

  g_info ("logged in user: %s", cockpit_creds_get_user (authenticated->creds));

  return cockpit_creds_to_json (creds);
}

CockpitAuth *
cockpit_auth_new (gboolean login_loopback)
{
  CockpitAuth *self = g_object_new (COCKPIT_TYPE_AUTH, NULL);
  const gchar *max_startups_conf;
  gint count = 0;

  self->login_loopback = login_loopback;

  if (cockpit_ws_max_startups == NULL)
    max_startups_conf = cockpit_conf_string ("WebService", "MaxStartups");
  else
    max_startups_conf = cockpit_ws_max_startups;

  self->max_startups = max_startups;
  self->max_startups_begin = max_startups;
  self->max_startups_rate = 100;

  if (max_startups_conf)
    {
      count = sscanf (max_startups_conf, "%u:%u:%u",
                      &self->max_startups_begin,
                      &self->max_startups_rate,
                      &self->max_startups);

      /* If all three numbers are not given use the
       * first as a hard limit */
      if (count == 1 || count == 2)
        {
          self->max_startups = self->max_startups_begin;
          self->max_startups_rate = 100;
        }

      if (count < 1 || count > 3 ||
          self->max_startups_begin > self->max_startups ||
          self->max_startups_rate > 100 || self->max_startups_rate < 1)
        {
          g_warning ("Illegal MaxStartups spec: %s. Reverting to defaults", max_startups_conf);
          self->max_startups = max_startups;
          self->max_startups_begin = max_startups;
          self->max_startups_rate = 100;
        }
    }

  return self;
}

gchar *
cockpit_auth_parse_application (const gchar *path)
{
  const gchar *application;
  gchar *memory = NULL;

  application = auth_parse_application (path, &memory);
  if (!memory)
    memory = g_strdup (application);
  return memory;
}
