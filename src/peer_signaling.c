#ifndef DISABLE_PEER_SIGNALING
#include <assert.h>
#include <cJSON.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include <core_http_client.h>
#include <core_mqtt.h>

#include "base64.h"
#include "config.h"
#include "peer_signaling.h"
#include "ports.h"
#include "ssl_transport.h"
#include "utils.h"

#define KEEP_ALIVE_TIMEOUT_SECONDS 60
#define CONNACK_RECV_TIMEOUT_MS 1000

#define PATH_MAX_LEN 128
#define HOST_MAX_LEN 64
#define URL_MAX_LEN (PATH_MAX_LEN + HOST_MAX_LEN + 8)
#define TOPIC_MAX_LEN URL_MAX_LEN
#define TOKEN_MAX_LEN 256

#define RPC_VERSION "2.0"
#define RPC_METHOD_STATE "state"
#define RPC_METHOD_OFFER "offer"
#define RPC_METHOD_ANSWER "answer"
#define RPC_METHOD_CLOSE "close"
#define RPC_METHOD_CANDIDATE "candidate"

#define RPC_ERROR_PARSE_ERROR "{\"code\":-32700,\"message\":\"Parse error\"}"
#define RPC_ERROR_INVALID_REQUEST "{\"code\":-32600,\"message\":\"Invalid Request\"}"
#define RPC_ERROR_METHOD_NOT_FOUND "{\"code\":-32601,\"message\":\"Method not found\"}"
#define RPC_ERROR_INVALID_PARAMS "{\"code\":-32602,\"message\":\"Invalid params\"}"
#define RPC_ERROR_INTERNAL_ERROR "{\"code\":-32603,\"message\":\"Internal error\"}"
#define RPC_ERROR_BUSY "{\"code\":-32000,\"message\":\"Too many connections\"}"

#define SESSION_ID_MAX_LEN 32
// "<path>/invoke" without a session id, used by clients that do not support sessions
#define LEGACY_SESSION_ID ""

// A browser picks a random session id and talks on "<path>/<session id>/invoke" and
// "<path>/<session id>/result", so that every browser gets its own peer connection.
typedef struct PeerSignalingSession {
  int bound;
  char id[SESSION_ID_MAX_LEN];
  PeerConnection* pc;
  uint32_t bound_time;
  int rpc_id;  // id of the pending offer request
} PeerSignalingSession;

typedef struct PeerSignaling {
  MQTTContext_t mqtt_ctx;
  MQTTFixedBuffer_t mqtt_fixed_buf;

  TransportInterface_t transport;
  NetworkContext_t net_ctx;

  uint8_t mqtt_buf[CONFIG_MQTT_BUFFER_SIZE];
  uint8_t http_buf[CONFIG_HTTP_BUFFER_SIZE];

  char subtopic[TOPIC_MAX_LEN];
  char session_subtopic[TOPIC_MAX_LEN];

  uint16_t packet_id;

  int proto;  // 0: MQTT, 1: HTTP
  int port;
  char host[HOST_MAX_LEN];
  char path[PATH_MAX_LEN];
  char token[TOKEN_MAX_LEN];
  char client_id[32];

  PeerSignalingSession sessions[CONFIG_SIGNALING_MAX_SESSIONS];
  int sessions_count;
  // the session whose peer connection is creating a description, callbacks of the peer
  // connection are invoked synchronously inside peer_connection_create_offer()
  PeerSignalingSession* current;

} PeerSignaling;

static PeerSignaling g_ps = {0};

static int peer_signaling_resolve_token(const char* token, char* username, char* password) {
  char plaintext[TOKEN_MAX_LEN] = {0};
  char* colon;

  if (token == NULL || strlen(token) == 0) {
    LOGW("Invalid token");
    return -1;
  }
  base64_decode(token, strlen(token), (unsigned char*)plaintext, sizeof(plaintext));
  colon = strchr(plaintext, ':');
  if (colon == NULL) {
    LOGW("Invalid token: %s", token);
    return -1;
  }

  strncpy(username, plaintext, colon - plaintext);
  strncpy(password, colon + 1, strlen(colon + 1));
  LOGD("Username: %s, Password: %s", username, password);
  return 0;
}

static int peer_signaling_resolve_url(const char* url, char* host, int* port, char* path) {
  char *port_start, *path_start;
  int proto = 0;

  if (url == NULL || strlen(url) == 0) {
    LOGW("Invalid URL");
    return -1;
  }

  if (strncmp(url, "mqtts://", 8) == 0) {
    *port = 8883;
    url += 8;
  } else if (strncmp(url, "https://", 8) == 0) {
    *port = 443;
    url += 8;
    proto = 1;
  } else if (strncmp(url, "mqtt://", 7) == 0) {
    *port = 1883;
    url += 7;
  } else if (strncmp(url, "http://", 7) == 0) {
    *port = 80;
    url += 7;
    proto = 1;
  } else {
    LOGW("Invalid URL: %s", url);
    return -1;
  }

  port_start = strchr(url, ':');
  path_start = strchr(url, '/');

  if (port_start != NULL && path_start != NULL && port_start < path_start) {
    strncpy(host, url, port_start - url);
    strncpy(path, path_start, strlen(path_start));
    *port = atoi(port_start + 1);
  } else if (port_start == NULL && path_start != NULL) {
    strncpy(host, url, path_start - url);
    strncpy(path, path_start, strlen(path_start));
  } else if (port_start != NULL && path_start == NULL) {
    strncpy(host, url, port_start - url);
    *port = atoi(port_start + 1);
  } else {
    strncpy(host, url, strlen(url));
  }

  LOGI("Host: %s, Port: %d, Path: %s", host, *port, path);
  return proto;
}

static void peer_signaling_pubtopic(const char* session_id, char* topic, size_t size) {
  if (strlen(session_id) == 0) {
    snprintf(topic, size, "%s/result", g_ps.path);
  } else {
    snprintf(topic, size, "%s/%s/result", g_ps.path, session_id);
  }
}

static void peer_signaling_mqtt_publish(MQTTContext_t* mqtt_ctx, const char* session_id, const char* message) {
  MQTTStatus_t status;
  MQTTPublishInfo_t pub_info;
  char topic[TOPIC_MAX_LEN + SESSION_ID_MAX_LEN];

  peer_signaling_pubtopic(session_id, topic, sizeof(topic));
  memset(&pub_info, 0, sizeof(pub_info));

  pub_info.qos = MQTTQoS0;
  pub_info.retain = false;
  pub_info.pTopicName = topic;
  pub_info.topicNameLength = strlen(topic);
  pub_info.pPayload = message;
  pub_info.payloadLength = strlen(message);

  status = MQTT_Publish(mqtt_ctx, &pub_info, MQTT_GetPacketId(mqtt_ctx));
  if (status != MQTTSuccess) {
    LOGE("MQTT_Publish failed: Status=%s.", MQTT_Status_strerror(status));
  } else {
    LOGD("MQTT_Publish succeeded.");
  }
}

static PeerSignalingSession* peer_signaling_find_session(const char* session_id) {
  int i;

  for (i = 0; i < g_ps.sessions_count; i++) {
    if (g_ps.sessions[i].bound && strcmp(g_ps.sessions[i].id, session_id) == 0) {
      return &g_ps.sessions[i];
    }
  }
  return NULL;
}

static int peer_signaling_session_is_idle(PeerSignalingSession* session) {
  switch (peer_connection_get_state(session->pc)) {
    case PEER_CONNECTION_NEW:
      // an offer was sent but the answer never came back
      return !session->bound ||
             (uint32_t)(ports_get_epoch_time() - session->bound_time) >= CONFIG_SIGNALING_SESSION_TIMEOUT;
    case PEER_CONNECTION_DISCONNECTED:
    case PEER_CONNECTION_FAILED:
    case PEER_CONNECTION_CLOSED:
      return 1;
    default:
      return 0;
  }
}

static PeerSignalingSession* peer_signaling_bind_session(const char* session_id) {
  PeerSignalingSession* session;
  int i;

  for (i = 0; i < g_ps.sessions_count; i++) {
    session = &g_ps.sessions[i];
    if (!session->bound || peer_signaling_session_is_idle(session)) {
      if (session->bound) {
        LOGI("Release session \"%s\" of peer connection %d", session->id, i);
      }
      session->bound = 1;
      snprintf(session->id, sizeof(session->id), "%s", session_id);
      session->bound_time = ports_get_epoch_time();
      LOGI("Bind session \"%s\" to peer connection %d", session_id, i);
      return session;
    }
  }
  return NULL;
}

static void peer_signaling_on_pub_event(const char* session_id, const char* msg, size_t size) {
  cJSON *req, *res, *item, *result, *error;
  int id = -1;
  char* payload = NULL;
  PeerSignalingSession* session;
  PeerConnectionState state;

  req = res = item = result = error = NULL;
  session = peer_signaling_find_session(session_id);
  do {
    req = cJSON_ParseWithLength(msg, size);
    if (!req) {
      error = cJSON_CreateRaw(RPC_ERROR_PARSE_ERROR);
      LOGW("Parse json failed");
      break;
    }

    item = cJSON_GetObjectItem(req, "method");
    if (!cJSON_IsString(item)) {
      error = cJSON_CreateRaw(RPC_ERROR_INVALID_REQUEST);
      LOGW("Cannot find method");
      break;
    }

    // trickled candidates are JSON-RPC notifications, which have no id and get no response
    if (strcmp(item->valuestring, RPC_METHOD_CANDIDATE) == 0) {
      item = cJSON_GetObjectItem(req, "params");
      if (session && cJSON_IsString(item) && strlen(item->valuestring) > 0) {
        LOGI("Received remote candidate from session \"%s\": %s", session_id, item->valuestring);
        peer_connection_add_ice_candidate(session->pc, item->valuestring);
      }
      break;
    }

    item = cJSON_GetObjectItem(req, "id");
    if (!cJSON_IsNumber(item)) {
      error = cJSON_CreateRaw(RPC_ERROR_INVALID_REQUEST);
      LOGW("Cannot find id");
      break;
    }

    id = item->valueint;
    item = cJSON_GetObjectItem(req, "method");

    if (strcmp(item->valuestring, RPC_METHOD_OFFER) == 0) {
      if (session == NULL) {
        session = peer_signaling_bind_session(session_id);
      } else if (!peer_signaling_session_is_idle(session) && peer_connection_get_state(session->pc) != PEER_CONNECTION_NEW) {
        // the session is negotiating or connected
        session = NULL;
      }

      if (session == NULL) {
        LOGW("No peer connection available for session \"%s\"", session_id);
        error = cJSON_CreateRaw(RPC_ERROR_BUSY);
        break;
      }

      session->rpc_id = id;
      session->bound_time = ports_get_epoch_time();
      g_ps.current = session;
      peer_connection_create_offer(session->pc);
      g_ps.current = NULL;

    } else if (strcmp(item->valuestring, RPC_METHOD_ANSWER) == 0) {
      item = cJSON_GetObjectItem(req, "params");
      if (!cJSON_IsString(item)) {
        error = cJSON_CreateRaw(RPC_ERROR_INVALID_PARAMS);
        LOGW("Cannot find params");
        break;
      }
      if (session == NULL) {
        error = cJSON_CreateRaw(RPC_ERROR_INVALID_REQUEST);
        LOGW("Answer from unknown session \"%s\"", session_id);
        break;
      }

      LOGI("Received remote SDP (answer) from session \"%s\":\n%s", session_id, item->valuestring);
      peer_connection_set_remote_description(session->pc, item->valuestring, SDP_TYPE_ANSWER);

    } else if (strcmp(item->valuestring, RPC_METHOD_STATE) == 0) {
      state = session ? peer_connection_get_state(session->pc) : PEER_CONNECTION_NEW;
      result = cJSON_CreateString(peer_connection_state_to_string(state));

    } else if (strcmp(item->valuestring, RPC_METHOD_CLOSE) == 0) {
      if (session) {
        peer_connection_close(session->pc);
        session->bound = 0;
      }
      result = cJSON_CreateString("");

    } else {
      error = cJSON_CreateRaw(RPC_ERROR_METHOD_NOT_FOUND);
      LOGW("Unsupport method");
    }

  } while (0);

  if (result || error) {
    res = cJSON_CreateObject();
    cJSON_AddStringToObject(res, "jsonrpc", RPC_VERSION);
    cJSON_AddNumberToObject(res, "id", id);

    if (result) {
      cJSON_AddItemToObject(res, "result", result);
    } else if (error) {
      cJSON_AddItemToObject(res, "error", error);
    }

    payload = cJSON_PrintUnformatted(res);

    if (payload) {
      peer_signaling_mqtt_publish(&g_ps.mqtt_ctx, session_id, payload);
      free(payload);
    }
    cJSON_Delete(res);
  }

  if (req) {
    cJSON_Delete(req);
  }
}

HTTPResponse_t peer_signaling_http_request(const TransportInterface_t* transport_interface,
                                           const char* method,
                                           size_t method_len,
                                           const char* host,
                                           size_t host_len,
                                           const char* path,
                                           size_t path_len,
                                           const char* auth,
                                           size_t auth_len,
                                           const char* body,
                                           size_t body_len) {
  HTTPStatus_t status = HTTPSuccess;
  HTTPRequestInfo_t request_info = {0};
  HTTPResponse_t response = {0};
  HTTPRequestHeaders_t request_headers = {0};

  request_info.pMethod = method;
  request_info.methodLen = method_len;
  request_info.pHost = host;
  request_info.hostLen = host_len;
  request_info.pPath = path;
  request_info.pathLen = path_len;
  request_info.reqFlags = HTTP_REQUEST_KEEP_ALIVE_FLAG;

  request_headers.pBuffer = g_ps.http_buf;
  request_headers.bufferLen = sizeof(g_ps.http_buf);

  status = HTTPClient_InitializeRequestHeaders(&request_headers, &request_info);

  if (status == HTTPSuccess) {
    HTTPClient_AddHeader(&request_headers,
                         "Content-Type", strlen("Content-Type"), "application/sdp", strlen("application/sdp"));

    if (auth_len > 0) {
      HTTPClient_AddHeader(&request_headers,
                           "Authorization", strlen("Authorization"), auth, auth_len);
    }

    response.pBuffer = g_ps.http_buf;
    response.bufferLen = sizeof(g_ps.http_buf);

    status = HTTPClient_Send(transport_interface,
                             &request_headers, (uint8_t*)body, body ? body_len : 0, &response, 0);

  } else {
    LOGE("Failed to initialize HTTP request headers: Error=%s.", HTTPClient_strerror(status));
  }

  return response;
}

static int peer_signaling_http_post(const char* hostname, const char* path, int port, const char* auth, const char* body) {
  int ret = 0;
  TransportInterface_t trans_if = {0};
  NetworkContext_t net_ctx;
  HTTPResponse_t res;

  trans_if.recv = ssl_transport_recv;
  trans_if.send = ssl_transport_send;
  trans_if.pNetworkContext = &net_ctx;

  assert(port > 0);

  ret = ssl_transport_connect(&net_ctx, hostname, port, NULL);

  if (ret < 0) {
    LOGE("Failed to connect to %s:%d", hostname, port);
    return ret;
  }

  res = peer_signaling_http_request(&trans_if, "POST", 4, hostname, strlen(hostname), path,
                                    strlen(path), auth, strlen(auth), body, strlen(body));

  ssl_transport_disconnect(&net_ctx);

  if (res.pHeaders == NULL) {
    LOGE("Response headers are NULL");
    return -1;
  }

  if (res.pBody == NULL) {
    LOGE("Response body is NULL");
    return -1;
  }

  LOGI(
      "Received HTTP response from %s%s\n"
      "Response Headers: %s\nResponse Status: %u\nResponse Body: %s\n",
      hostname, path, res.pHeaders, res.statusCode, res.pBody);

  if (res.statusCode == 201) {
    peer_connection_set_remote_description(g_ps.sessions[0].pc, (const char*)res.pBody, SDP_TYPE_ANSWER);
  }
  return 0;
}

// "<path>/invoke" -> "", "<path>/<session id>/invoke" -> "<session id>", otherwise -1
static int peer_signaling_parse_session_id(const char* topic, size_t topic_len, char* session_id, size_t size) {
  size_t path_len = strlen(g_ps.path);
  size_t suffix_len = strlen("/invoke");
  size_t id_len;

  if (topic_len < path_len + suffix_len ||
      strncmp(topic, g_ps.path, path_len) != 0 ||
      strncmp(topic + topic_len - suffix_len, "/invoke", suffix_len) != 0) {
    return -1;
  }

  if (topic_len == path_len + suffix_len) {
    snprintf(session_id, size, "%s", LEGACY_SESSION_ID);
    return 0;
  }

  // "/<session id>"
  if (topic[path_len] != '/') {
    return -1;
  }
  id_len = topic_len - path_len - 1 - suffix_len;
  if (id_len == 0 || id_len >= size || memchr(topic + path_len + 1, '/', id_len) != NULL) {
    return -1;
  }
  memcpy(session_id, topic + path_len + 1, id_len);
  session_id[id_len] = '\0';
  return 0;
}

static void peer_signaling_mqtt_event_cb(MQTTContext_t* mqtt_ctx,
                                         MQTTPacketInfo_t* packet_info,
                                         MQTTDeserializedInfo_t* deserialized_info) {
  MQTTStatus_t status = MQTTSuccess;
  MQTTPublishInfo_t* pub_info;
  char session_id[SESSION_ID_MAX_LEN];

  switch (packet_info->type) {
    case MQTT_PACKET_TYPE_PUBLISH:
      pub_info = deserialized_info->pPublishInfo;
      LOGD("MQTT received message from %.*s: %.*s",
           pub_info->topicNameLength, pub_info->pTopicName,
           pub_info->payloadLength, (char*)pub_info->pPayload);
      if (peer_signaling_parse_session_id(pub_info->pTopicName, pub_info->topicNameLength,
                                          session_id, sizeof(session_id)) != 0) {
        LOGW("Ignore message from topic %.*s", pub_info->topicNameLength, pub_info->pTopicName);
        break;
      }
      peer_signaling_on_pub_event(session_id, pub_info->pPayload, pub_info->payloadLength);
      break;
    case MQTT_PACKET_TYPE_SUBACK: {
      size_t ncodes = 0;
      int i = 0;
      uint8_t* codes = NULL;
      status = MQTT_GetSubAckStatusCodes(packet_info, &codes, &ncodes);

      assert(status == MQTTSuccess);

      for (i = 0; i < ncodes; i++) {
        if (codes[i] == MQTTSubAckFailure) {
          LOGE("MQTT Subscription failed. Please check authorization");
          break;
        }
      }

      if (i == ncodes) {
        LOGI("MQTT Subscribe succeeded.");
      }
    } break;
    case MQTT_PACKET_TYPE_UNSUBACK:
      LOGI("MQTT Unsubscribe succeeded.");
      break;
    default:
      break;
  }
}

static int peer_signaling_mqtt_connect(const char* hostname, int port) {
  MQTTStatus_t status;
  MQTTConnectInfo_t conn_info;
  bool session_present;
  char username[TOKEN_MAX_LEN] = {0};
  char password[TOKEN_MAX_LEN] = {0};

  if (ssl_transport_connect(&g_ps.net_ctx, hostname, port, NULL) < 0) {
    LOGE("ssl transport connect failed");
    return -1;
  }

  g_ps.transport.recv = ssl_transport_recv;
  g_ps.transport.send = ssl_transport_send;
  g_ps.transport.pNetworkContext = &g_ps.net_ctx;
  g_ps.mqtt_fixed_buf.pBuffer = g_ps.mqtt_buf;
  g_ps.mqtt_fixed_buf.size = sizeof(g_ps.mqtt_buf);
  status = MQTT_Init(&g_ps.mqtt_ctx, &g_ps.transport,
                     ports_get_epoch_time, peer_signaling_mqtt_event_cb, &g_ps.mqtt_fixed_buf);

  memset(&conn_info, 0, sizeof(conn_info));

  conn_info.cleanSession = false;

  if (strlen(g_ps.token) > 0) {
    peer_signaling_resolve_token(g_ps.token, username, password);
    conn_info.pUserName = username;
    conn_info.userNameLength = strlen(username);
    conn_info.pPassword = password;
    conn_info.passwordLength = strlen(password);
  }

  if (strlen(g_ps.client_id) > 0) {
    conn_info.pClientIdentifier = g_ps.client_id;
    conn_info.clientIdentifierLength = strlen(g_ps.client_id);
  }

  conn_info.keepAliveSeconds = KEEP_ALIVE_TIMEOUT_SECONDS;

  status = MQTT_Connect(&g_ps.mqtt_ctx,
                        &conn_info, NULL, CONNACK_RECV_TIMEOUT_MS, &session_present);

  if (status != MQTTSuccess) {
    LOGE("MQTT_Connect failed: Status=%s.", MQTT_Status_strerror(status));
    return -1;
  }

  LOGI("MQTT_Connect succeeded.");
  return 0;
}

static int peer_signaling_mqtt_subscribe(int subscribed) {
  MQTTStatus_t status = MQTTSuccess;
  MQTTSubscribeInfo_t sub_info[2];

  uint16_t packet_id = MQTT_GetPacketId(&g_ps.mqtt_ctx);

  memset(sub_info, 0, sizeof(sub_info));
  sub_info[0].qos = MQTTQoS0;
  sub_info[0].pTopicFilter = g_ps.subtopic;
  sub_info[0].topicFilterLength = strlen(g_ps.subtopic);
  sub_info[1].qos = MQTTQoS0;
  sub_info[1].pTopicFilter = g_ps.session_subtopic;
  sub_info[1].topicFilterLength = strlen(g_ps.session_subtopic);

  if (subscribed) {
    LOGI("Subscribing topic %s and %s", g_ps.subtopic, g_ps.session_subtopic);
    status = MQTT_Subscribe(&g_ps.mqtt_ctx, sub_info, 2, packet_id);
  } else {
    status = MQTT_Unsubscribe(&g_ps.mqtt_ctx, sub_info, 2, packet_id);
  }
  if (status != MQTTSuccess) {
    LOGE("MQTT_Subscribe failed: Status=%s.", MQTT_Status_strerror(status));
    return -1;
  }

  status = MQTT_ProcessLoop(&g_ps.mqtt_ctx);

  if (status != MQTTSuccess) {
    LOGE("MQTT_ProcessLoop failed: Status=%s.", MQTT_Status_strerror(status));
    return -1;
  }

  return 0;
}

static void peer_signaling_onicecandidate(char* description, void* userdata) {
  cJSON* res;
  char* payload;
  PeerSignalingSession* session = g_ps.current;

  if (g_ps.proto == 0) {
    if (session == NULL) {
      LOGW("Drop local SDP created outside of the signaling");
      return;
    }
    LOGI("Sending local SDP (offer) to session \"%s\":\n%s", session->id, description);
    res = cJSON_CreateObject();
    cJSON_AddStringToObject(res, "jsonrpc", RPC_VERSION);
    cJSON_AddNumberToObject(res, "id", session->rpc_id);
    cJSON_AddStringToObject(res, "result", description);
    payload = cJSON_PrintUnformatted(res);
    if (payload) {
      peer_signaling_mqtt_publish(&g_ps.mqtt_ctx, session->id, payload);
      free(payload);
    }
    cJSON_Delete(res);
  } else {
    LOGI("Sending local SDP (offer) to %s:\n%s", g_ps.host, description);
    if (strlen(g_ps.token) > 0) {
      char cred[TOKEN_MAX_LEN + 10];
      memset(cred, 0, sizeof(cred));
      snprintf(cred, sizeof(cred), "Bearer %s", g_ps.token);
      peer_signaling_http_post(g_ps.host, g_ps.path, g_ps.port, cred, description);
    } else {
      peer_signaling_http_post(g_ps.host, g_ps.path, g_ps.port, "", description);
    }
  }
}

static void peer_signaling_onlocalcandidate(char* candidate, void* userdata) {
  cJSON* notification;
  char* payload;
  PeerSignalingSession* session = g_ps.current;

  if (session == NULL) {
    LOGW("Drop local candidate created outside of the signaling");
    return;
  }
  LOGI("Sending local candidate to session \"%s\": %s", session->id, candidate);
  notification = cJSON_CreateObject();
  cJSON_AddStringToObject(notification, "jsonrpc", RPC_VERSION);
  cJSON_AddStringToObject(notification, "method", RPC_METHOD_CANDIDATE);
  cJSON_AddStringToObject(notification, "params", candidate);
  payload = cJSON_PrintUnformatted(notification);
  if (payload) {
    peer_signaling_mqtt_publish(&g_ps.mqtt_ctx, session->id, payload);
    free(payload);
  }
  cJSON_Delete(notification);
}

int peer_signaling_connect(const char* url, const char* token, PeerConnection* pc) {
  return peer_signaling_connect_multi(url, token, &pc, 1);
}

int peer_signaling_connect_multi(const char* url, const char* token, PeerConnection** pcs, int count) {
  char* client_id;
  int i;

  if (count <= 0 || count > CONFIG_SIGNALING_MAX_SESSIONS) {
    LOGE("Invalid number of peer connections %d, the maximum is %d", count, CONFIG_SIGNALING_MAX_SESSIONS);
    return -1;
  }

  if ((g_ps.proto = peer_signaling_resolve_url(url, g_ps.host, &g_ps.port, g_ps.path)) < 0) {
    LOGE("Resolve URL failed");
  }

  if (token && strlen(token) > 0) {
    strncpy(g_ps.token, token, sizeof(g_ps.token));
  }

  memset(g_ps.sessions, 0, sizeof(g_ps.sessions));
  g_ps.sessions_count = count;
  for (i = 0; i < count; i++) {
    g_ps.sessions[i].pc = pcs[i];
    peer_connection_onicecandidate(pcs[i], peer_signaling_onicecandidate);
    if (g_ps.proto == 0) {
      // WHIP over HTTP has no channel for trickled candidates, so only MQTT enables it
      peer_connection_onlocalcandidate(pcs[i], peer_signaling_onlocalcandidate);
    }
  }

  switch (g_ps.proto) {
    case 0: {  // MQTT
      client_id = strrchr(g_ps.path, '/');
      snprintf(g_ps.client_id, sizeof(g_ps.client_id), "%s", client_id + 1);
      snprintf(g_ps.subtopic, sizeof(g_ps.subtopic), "%s/invoke", g_ps.path);
      snprintf(g_ps.session_subtopic, sizeof(g_ps.session_subtopic), "%s/+/invoke", g_ps.path);
      if (peer_signaling_mqtt_connect(g_ps.host, g_ps.port) == 0) {
        peer_signaling_mqtt_subscribe(1);
      }
    } break;
    case 1: {  // HTTP
      if (count > 1) {
        LOGW("Only the first peer connection is used with HTTP signaling");
      }
      g_ps.current = &g_ps.sessions[0];
      peer_connection_create_offer(pcs[0]);
      g_ps.current = NULL;
    } break;
    default: {
    } break;
  }

  return 0;
}

void peer_signaling_disconnect() {
  MQTTStatus_t status = MQTTSuccess;

  if (!g_ps.proto && !peer_signaling_mqtt_subscribe(0)) {
    status = MQTT_Disconnect(&g_ps.mqtt_ctx);
    if (status != MQTTSuccess) {
      LOGE("Failed to disconnect with broker: %s", MQTT_Status_strerror(status));
    }

    ssl_transport_disconnect(&g_ps.net_ctx);
  }
  LOGI("Disconnected");
}

int peer_signaling_loop() {
  MQTT_ProcessLoop(&g_ps.mqtt_ctx);
  return 0;
}
#endif  // DISABLE_PEER_SIGNALING
