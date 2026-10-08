#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include "agent.h"
#include "base64.h"
#include "ice.h"
#include "ports.h"
#include "socket.h"
#include "stun.h"
#include "utils.h"

#define AGENT_POLL_TIMEOUT 1
#define AGENT_CONNCHECK_MAX 1000
#define AGENT_CONNCHECK_PERIOD 100
#define AGENT_STUN_RECV_MAXTIMES 1000

void agent_clear_candidates(Agent* agent) {
  agent->local_candidates_count = 0;
  agent->remote_candidates_count = 0;
  agent->candidate_pairs_num = 0;
  // the pairs pointed to belong to the previous session
  agent->selected_pair = NULL;
  agent->nominated_pair = NULL;
  agent->binding_request_pending = 0;
}

int agent_create(Agent* agent) {
  int ret;
  if ((ret = udp_socket_open(&agent->udp_sockets[0], AF_INET, 0)) < 0) {
    LOGE("Failed to create UDP socket.");
    return ret;
  }
  LOGI("create IPv4 UDP socket: %d", agent->udp_sockets[0].fd);

#if CONFIG_IPV6
  if ((ret = udp_socket_open(&agent->udp_sockets[1], AF_INET6, 0)) < 0) {
    LOGE("Failed to create IPv6 UDP socket.");
    return ret;
  }
  LOGI("create IPv6 UDP socket: %d", agent->udp_sockets[1].fd);
#endif

  agent_clear_candidates(agent);
  memset(agent->remote_ufrag, 0, sizeof(agent->remote_ufrag));
  memset(agent->remote_upwd, 0, sizeof(agent->remote_upwd));
  return 0;
}

void agent_destroy(Agent* agent) {
  if (agent->udp_sockets[0].fd > 0) {
    udp_socket_close(&agent->udp_sockets[0]);
  }

#if CONFIG_IPV6
  if (agent->udp_sockets[1].fd > 0) {
    udp_socket_close(&agent->udp_sockets[1]);
  }
#endif
}

static int agent_socket_recv(Agent* agent, Address* addr, uint8_t* buf, int len) {
  int ret = -1;
  int i = 0;
  int maxfd = -1;
  fd_set rfds;
  struct timeval tv;
  int addr_type[] = {
    AF_INET,
#if CONFIG_IPV6
    AF_INET6,
#endif
  };

  tv.tv_sec = 0;
  tv.tv_usec = AGENT_POLL_TIMEOUT * 1000;
  FD_ZERO(&rfds);

  for (i = 0; i < sizeof(addr_type) / sizeof(addr_type[0]); i++) {
    if (agent->udp_sockets[i].fd > maxfd) {
      maxfd = agent->udp_sockets[i].fd;
    }
    if (agent->udp_sockets[i].fd >= 0) {
      FD_SET(agent->udp_sockets[i].fd, &rfds);
    }
  }

  ret = select(maxfd + 1, &rfds, NULL, NULL, &tv);
  if (ret < 0) {
    LOGE("select error");
  } else if (ret == 0) {
    // timeout
  } else {
    for (i = 0; i < 2; i++) {
      if (FD_ISSET(agent->udp_sockets[i].fd, &rfds)) {
        memset(buf, 0, len);
        ret = udp_socket_recvfrom(&agent->udp_sockets[i], addr, buf, len);
        break;
      }
    }
  }

  return ret;
}

// Connectivity checks from the peer may arrive on the same socket while gathering,
// so only accept the response to our own request.
static int agent_socket_recv_stun_response(Agent* agent, StunMessage* request, StunMessage* response) {
  int ret = -1;
  int i = 0;
  StunHeader* request_header = (StunHeader*)request->buf;
  StunHeader* response_header = (StunHeader*)response->buf;
  for (i = 0; i < AGENT_STUN_RECV_MAXTIMES; i++) {
    ret = agent_socket_recv(agent, NULL, response->buf, sizeof(response->buf));
    if (ret <= 0) {
      continue;
    }
    if (stun_probe(response->buf, ret) == 0 &&
        memcmp(response_header->transaction_id, request_header->transaction_id,
               sizeof(request_header->transaction_id)) == 0) {
      response->size = ret;
      return ret;
    }
    LOGD("Ignore unexpected packet while waiting for STUN response");
  }
  return -1;
}

static int agent_socket_send(Agent* agent, Address* addr, const uint8_t* buf, int len) {
  switch (addr->family) {
    case AF_INET6:
      return udp_socket_sendto(&agent->udp_sockets[1], addr, buf, len);
    case AF_INET:
    default:
      return udp_socket_sendto(&agent->udp_sockets[0], addr, buf, len);
  }
  return -1;
}

static int agent_create_host_addr(Agent* agent) {
  int i, j;
  const char* iface_prefx[] = {CONFIG_IFACE_PREFIX};
  IceCandidate* ice_candidate;
  int addr_type[] = {
    AF_INET,
#if CONFIG_IPV6
    AF_INET6,
#endif
  };

  for (i = 0; i < sizeof(addr_type) / sizeof(addr_type[0]); i++) {
    for (j = 0; j < sizeof(iface_prefx) / sizeof(iface_prefx[0]); j++) {
      ice_candidate = agent->local_candidates + agent->local_candidates_count;
      // only copy port and family to addr of ice candidate
      ice_candidate_create(ice_candidate, agent->local_candidates_count, ICE_CANDIDATE_TYPE_HOST,
                           &agent->udp_sockets[i].bind_addr);
      // if resolve host addr, add to local candidate
      if (ports_get_host_addr(&ice_candidate->addr, iface_prefx[j])) {
        agent->local_candidates_count++;
      }
    }
  }

  return 0;
}

static int agent_create_stun_addr(Agent* agent, Address* serv_addr) {
  int ret = -1;
  Address bind_addr;
  StunMessage send_msg;
  StunMessage recv_msg;
  memset(&send_msg, 0, sizeof(send_msg));
  memset(&recv_msg, 0, sizeof(recv_msg));

  stun_msg_create(&send_msg, STUN_CLASS_REQUEST | STUN_METHOD_BINDING);

  ret = agent_socket_send(agent, serv_addr, send_msg.buf, send_msg.size);

  if (ret == -1) {
    LOGE("Failed to send STUN Binding Request.");
    return ret;
  }

  ret = agent_socket_recv_stun_response(agent, &send_msg, &recv_msg);
  if (ret <= 0) {
    LOGD("Failed to receive STUN Binding Response.");
    return ret;
  }

  stun_parse_msg_buf(&recv_msg);
  memcpy(&bind_addr, &recv_msg.mapped_addr, sizeof(Address));
  IceCandidate* ice_candidate = agent->local_candidates + agent->local_candidates_count;
  ice_candidate_create(ice_candidate, agent->local_candidates_count++, ICE_CANDIDATE_TYPE_SRFLX, &bind_addr);
  return ret;
}

static int agent_create_turn_addr(Agent* agent, Address* serv_addr, const char* username, const char* credential) {
  int ret = -1;
  uint32_t attr = ntohl(0x11000000);
  Address turn_addr;
  StunMessage send_msg;
  StunMessage recv_msg;
  memset(&recv_msg, 0, sizeof(recv_msg));
  memset(&send_msg, 0, sizeof(send_msg));
  stun_msg_create(&send_msg, STUN_METHOD_ALLOCATE);
  stun_msg_write_attr(&send_msg, STUN_ATTR_TYPE_REQUESTED_TRANSPORT, sizeof(attr), (char*)&attr);  // UDP
  stun_msg_write_attr(&send_msg, STUN_ATTR_TYPE_USERNAME, strlen(username), (char*)username);

  ret = agent_socket_send(agent, serv_addr, send_msg.buf, send_msg.size);
  if (ret == -1) {
    LOGE("Failed to send TURN Binding Request.");
    return -1;
  }

  ret = agent_socket_recv_stun_response(agent, &send_msg, &recv_msg);
  if (ret <= 0) {
    LOGD("Failed to receive STUN Binding Response.");
    return ret;
  }

  stun_parse_msg_buf(&recv_msg);

  if (recv_msg.stunclass == STUN_CLASS_ERROR && recv_msg.stunmethod == STUN_METHOD_ALLOCATE) {
    memset(&send_msg, 0, sizeof(send_msg));
    stun_msg_create(&send_msg, STUN_METHOD_ALLOCATE);
    stun_msg_write_attr(&send_msg, STUN_ATTR_TYPE_REQUESTED_TRANSPORT, sizeof(attr), (char*)&attr);  // UDP
    stun_msg_write_attr(&send_msg, STUN_ATTR_TYPE_USERNAME, strlen(username), (char*)username);
    stun_msg_write_attr(&send_msg, STUN_ATTR_TYPE_NONCE, strlen(recv_msg.nonce), recv_msg.nonce);
    stun_msg_write_attr(&send_msg, STUN_ATTR_TYPE_REALM, strlen(recv_msg.realm), recv_msg.realm);
    stun_msg_finish(&send_msg, STUN_CREDENTIAL_LONG_TERM, credential, strlen(credential));
  } else {
    LOGE("Invalid TURN Binding Response.");
    return -1;
  }

  ret = agent_socket_send(agent, serv_addr, send_msg.buf, send_msg.size);
  if (ret < 0) {
    LOGE("Failed to send TURN Binding Request.");
    return -1;
  }

  ret = agent_socket_recv_stun_response(agent, &send_msg, &recv_msg);
  if (ret <= 0) {
    LOGD("Failed to receive TURN Binding Response.");
    return ret;
  }

  stun_parse_msg_buf(&recv_msg);
  memcpy(&turn_addr, &recv_msg.relayed_addr, sizeof(Address));
  IceCandidate* ice_candidate = agent->local_candidates + agent->local_candidates_count++;
  ice_candidate_create(ice_candidate, agent->local_candidates_count, ICE_CANDIDATE_TYPE_RELAY, &turn_addr);
  return ret;
}

void agent_gather_candidate(Agent* agent, const char* urls, const char* username, const char* credential) {
  char* pos;
  int port;
  char hostname[64];
  char addr_string[ADDRSTRLEN];
  int i;
  int addr_type[1] = {AF_INET};  // ipv6 no need stun
  Address resolved_addr;
  memset(hostname, 0, sizeof(hostname));

  if (urls == NULL) {
    agent_create_host_addr(agent);
    return;
  }

  if ((pos = strstr(urls + 5, ":")) == NULL) {
    LOGE("Invalid URL");
    return;
  }

  port = atoi(pos + 1);
  printf("port => %s\n", pos + 1);
  if (port <= 0) {
    LOGE("Cannot parse port");
    return;
  }

  snprintf(hostname, pos - urls - 5 + 1, "%s", urls + 5);

  for (i = 0; i < sizeof(addr_type) / sizeof(addr_type[0]); i++) {
    if (ports_resolve_addr(hostname, &resolved_addr) == 0) {
      addr_set_port(&resolved_addr, port);
      addr_to_string(&resolved_addr, addr_string, sizeof(addr_string));
      LOGI("Resolved stun/turn server %s:%d", addr_string, port);

      if (strncmp(urls, "stun:", 5) == 0) {
        LOGD("Create stun addr");
        agent_create_stun_addr(agent, &resolved_addr);
      } else if (strncmp(urls, "turn:", 5) == 0) {
        LOGD("Create turn addr");
        agent_create_turn_addr(agent, &resolved_addr, username, credential);
      }
    }
  }
}

void agent_create_ice_credential(Agent* agent) {
  memset(agent->local_ufrag, 0, sizeof(agent->local_ufrag));
  memset(agent->local_upwd, 0, sizeof(agent->local_upwd));

  utils_random_string(agent->local_ufrag, 4);
  utils_random_string(agent->local_upwd, 24);
}

void agent_get_local_description(Agent* agent, char* description, int length) {
  for (int i = 0; i < agent->local_candidates_count; i++) {
    ice_candidate_to_description(&agent->local_candidates[i], description + strlen(description), length - strlen(description));
  }

  // remove last \n
  description[strlen(description)] = '\0';
  LOGD("local description:\n%s", description);
}

static void agent_pair_to_string(IceCandidatePair* pair, char* buf, size_t len) {
  char local_addr[ADDRSTRLEN];
  char remote_addr[ADDRSTRLEN];
  addr_to_string(&pair->local->addr, local_addr, sizeof(local_addr));
  addr_to_string(&pair->remote->addr, remote_addr, sizeof(remote_addr));
  snprintf(buf, len, "%s:%d > %s:%d", local_addr, pair->local->addr.port, remote_addr, pair->remote->addr.port);
}

int agent_send(Agent* agent, const uint8_t* buf, int len) {
  return agent_socket_send(agent, &agent->nominated_pair->remote->addr, buf, len);
}

static void agent_create_binding_response(Agent* agent, StunMessage* msg, Address* addr) {
  int size = 0;
  char username[584];
  char mapped_address[32];
  uint8_t mask[16];
  StunHeader* header;
  stun_msg_create(msg, STUN_CLASS_RESPONSE | STUN_METHOD_BINDING);
  header = (StunHeader*)msg->buf;
  memcpy(header->transaction_id, agent->transaction_id, sizeof(header->transaction_id));
  snprintf(username, sizeof(username), "%s:%s", agent->local_ufrag, agent->remote_ufrag);
  *((uint32_t*)mask) = htonl(MAGIC_COOKIE);
  memcpy(mask + 4, agent->transaction_id, sizeof(agent->transaction_id));
  size = stun_set_mapped_address(mapped_address, mask, addr);
  stun_msg_write_attr(msg, STUN_ATTR_TYPE_XOR_MAPPED_ADDRESS, size, mapped_address);
  stun_msg_write_attr(msg, STUN_ATTR_TYPE_USERNAME, strlen(username), username);
  stun_msg_finish(msg, STUN_CREDENTIAL_SHORT_TERM, agent->local_upwd, strlen(agent->local_upwd));
}

static void agent_create_binding_request(Agent* agent, StunMessage* msg) {
  uint64_t tie_breaker = 0;  // always be controlled
  // send binding request
  stun_msg_create(msg, STUN_CLASS_REQUEST | STUN_METHOD_BINDING);
  char username[584];
  memset(username, 0, sizeof(username));
  snprintf(username, sizeof(username), "%s:%s", agent->remote_ufrag, agent->local_ufrag);
  stun_msg_write_attr(msg, STUN_ATTR_TYPE_USERNAME, strlen(username), username);
  stun_msg_write_attr(msg, STUN_ATTR_TYPE_PRIORITY, 4, (char*)&agent->nominated_pair->priority);
  if (agent->mode == AGENT_MODE_CONTROLLING) {
    stun_msg_write_attr(msg, STUN_ATTR_TYPE_USE_CANDIDATE, 0, NULL);
    stun_msg_write_attr(msg, STUN_ATTR_TYPE_ICE_CONTROLLING, 8, (char*)&tie_breaker);
  } else {
    stun_msg_write_attr(msg, STUN_ATTR_TYPE_ICE_CONTROLLED, 8, (char*)&tie_breaker);
  }
  stun_msg_finish(msg, STUN_CREDENTIAL_SHORT_TERM, agent->remote_upwd, strlen(agent->remote_upwd));
}

int agent_send_binding_request(Agent* agent) {
  StunMessage msg;
  StunHeader* header;
  int ret;

  if (agent->nominated_pair == NULL) {
    return -1;
  }

  memset(&msg, 0, sizeof(msg));
  agent_create_binding_request(agent, &msg);
  agent->binding_request_sent_time = ports_get_epoch_time();
  header = (StunHeader*)msg.buf;
  memcpy(agent->binding_request_transaction_id, header->transaction_id,
         sizeof(agent->binding_request_transaction_id));
  agent->binding_request_pending = 1;
  ret = agent_socket_send(agent, &agent->nominated_pair->remote->addr, msg.buf, msg.size);
  return ret;
}

// RFC 8445 7.3.1.3: learn the source of a valid binding request from an unknown address
// as a peer reflexive remote candidate, and pair it with a local candidate.
static IceCandidatePair* agent_add_peer_reflexive_candidate(Agent* agent, Address* addr) {
  IceCandidate* remote;
  IceCandidatePair* pair;
  int i;

  for (i = 0; i < agent->remote_candidates_count; i++) {
    if (addr_equal(&agent->remote_candidates[i].addr, addr)) {
      return NULL;
    }
  }

  if (agent->remote_candidates_count >= AGENT_MAX_CANDIDATES ||
      agent->candidate_pairs_num >= AGENT_MAX_CANDIDATE_PAIRS) {
    LOGW("No room for peer reflexive candidate");
    return NULL;
  }

  for (i = 0; i < agent->local_candidates_count; i++) {
    if (agent->local_candidates[i].addr.family == addr->family) {
      break;
    }
  }

  if (i == agent->local_candidates_count) {
    return NULL;
  }

  remote = &agent->remote_candidates[agent->remote_candidates_count];
  ice_candidate_create(remote, agent->remote_candidates_count, ICE_CANDIDATE_TYPE_PRFLX, addr);
  snprintf(remote->foundation, sizeof(remote->foundation), "prflx%d", agent->remote_candidates_count);
  agent->remote_candidates_count++;

  pair = &agent->candidate_pairs[agent->candidate_pairs_num++];
  pair->local = &agent->local_candidates[i];
  pair->remote = remote;
  pair->priority = pair->local->priority + remote->priority;
  pair->state = ICE_CANDIDATE_STATE_FROZEN;
  pair->conncheck = 0;
  return pair;
}

// RFC 8445 7.3.1.4: check the new peer reflexive pair right away instead of waiting in order.
static void agent_triggered_check(Agent* agent, IceCandidatePair* pair) {
  char pair_string[2 * ADDRSTRLEN + 16];

  if (agent->nominated_pair && agent->nominated_pair->state == ICE_CANDIDATE_STATE_INPROGRESS) {
    if (agent->nominated_pair->remote->type == ICE_CANDIDATE_TYPE_PRFLX) {
      return;
    }
    agent->nominated_pair->state = ICE_CANDIDATE_STATE_FROZEN;
  }

  agent->nominated_pair = pair;
  pair->conncheck = 0;
  pair->state = ICE_CANDIDATE_STATE_INPROGRESS;
  agent_pair_to_string(pair, pair_string, sizeof(pair_string));
  LOGI("Triggered check on peer reflexive pair [%d] %s", (int)(pair - agent->candidate_pairs), pair_string);
  agent_send_binding_request(agent);
}

void agent_process_stun_request(Agent* agent, StunMessage* stun_msg, Address* addr) {
  StunMessage msg;
  StunHeader* header;
  IceCandidatePair* pair;
  char addr_string[ADDRSTRLEN];
  addr_to_string(addr, addr_string, sizeof(addr_string));
  switch (stun_msg->stunmethod) {
    case STUN_METHOD_BINDING:
      if (stun_msg_is_valid(stun_msg->buf, stun_msg->size, agent->local_upwd) == 0) {
        if (agent->selected_pair == NULL) {
          LOGI("Received binding request from %s:%d, send binding response", addr_string, addr->port);
        } else {
          LOGD("Received binding request from %s:%d, send binding response", addr_string, addr->port);
        }
        header = (StunHeader*)stun_msg->buf;
        memcpy(agent->transaction_id, header->transaction_id, sizeof(header->transaction_id));
        agent_create_binding_response(agent, &msg, addr);
        agent_socket_send(agent, addr, msg.buf, msg.size);

        if (agent->selected_pair == NULL && (pair = agent_add_peer_reflexive_candidate(agent, addr)) != NULL) {
          LOGI("Add peer reflexive remote candidate %s:%d", addr_string, addr->port);
          agent_triggered_check(agent, pair);
        }
      } else {
        LOGW("Invalid binding request from %s:%d, check remote ice-ufrag/ice-pwd", addr_string, addr->port);
      }
      break;
    default:
      break;
  }
}

void agent_process_stun_response(Agent* agent, StunMessage* stun_msg) {
  char pair_string[2 * ADDRSTRLEN + 16];
  switch (stun_msg->stunmethod) {
    case STUN_METHOD_BINDING:
      if (stun_msg_is_valid(stun_msg->buf, stun_msg->size, agent->remote_upwd) != 0) {
        LOGW("Invalid binding response, check remote ice-pwd");
      } else if (!agent->binding_request_pending ||
                 memcmp(((StunHeader*)stun_msg->buf)->transaction_id,
                        agent->binding_request_transaction_id,
                        sizeof(agent->binding_request_transaction_id)) != 0) {
        LOGD("Ignore binding response with unexpected transaction id");
      } else {
        agent->nominated_pair->state = ICE_CANDIDATE_STATE_SUCCEEDED;
        agent->binding_request_pending = 0;
        agent_pair_to_string(agent->nominated_pair, pair_string, sizeof(pair_string));
        LOGI("Received binding response, pair %s succeeded", pair_string);
      }
      break;
    default:
      break;
  }
}

int agent_recv(Agent* agent, uint8_t* buf, int len) {
  int ret = -1;
  StunMessage stun_msg;
  Address addr;
  if ((ret = agent_socket_recv(agent, &addr, buf, len)) > 0) {
    agent->last_recv_time = ports_get_epoch_time();
  }
  if (ret > 0 && stun_probe(buf, len) == 0) {
    memcpy(stun_msg.buf, buf, ret);
    stun_msg.size = ret;
    stun_parse_msg_buf(&stun_msg);
    switch (stun_msg.stunclass) {
      case STUN_CLASS_REQUEST:
        agent_process_stun_request(agent, &stun_msg, &addr);
        break;
      case STUN_CLASS_RESPONSE:
        agent_process_stun_response(agent, &stun_msg);
        break;
      case STUN_CLASS_ERROR:
        break;
      default:
        break;
    }
    ret = 0;
  }
  return ret;
}

void agent_set_remote_description(Agent* agent, char* description) {
  /*
  a=ice-ufrag:Iexb
  a=ice-pwd:IexbSoY7JulyMbjKwISsG9
  a=candidate:1 1 UDP 1 36.231.28.50 38143 typ srflx
  */
  int i;

  LOGD("Set remote description:\n%s", description);

  char* line_start = description;
  char* line_end = NULL;

  while ((line_end = strstr(line_start, "\r\n")) != NULL) {
    if (strncmp(line_start, "a=ice-ufrag:", strlen("a=ice-ufrag:")) == 0) {
      strncpy(agent->remote_ufrag, line_start + strlen("a=ice-ufrag:"), line_end - line_start - strlen("a=ice-ufrag:"));

    } else if (strncmp(line_start, "a=ice-pwd:", strlen("a=ice-pwd:")) == 0) {
      strncpy(agent->remote_upwd, line_start + strlen("a=ice-pwd:"), line_end - line_start - strlen("a=ice-pwd:"));

    } else if (strncmp(line_start, "a=candidate:", strlen("a=candidate:")) == 0) {
      if (ice_candidate_from_description(&agent->remote_candidates[agent->remote_candidates_count], line_start, line_end) == 0) {
        for (i = 0; i < agent->remote_candidates_count; i++) {
          if (strcmp(agent->remote_candidates[i].foundation, agent->remote_candidates[agent->remote_candidates_count].foundation) == 0) {
            break;
          }
        }
        if (i == agent->remote_candidates_count) {
          agent->remote_candidates_count++;
        }
      }
    }

    line_start = line_end + 2;
  }

  LOGD("remote ufrag: %s", agent->remote_ufrag);
  LOGD("remote upwd: %s", agent->remote_upwd);
}

int agent_add_remote_candidate(Agent* agent, IceCandidate* candidate) {
  int i;
  IceCandidate* remote;
  IceCandidatePair* pair;
  char addr_string[ADDRSTRLEN];
  char pair_string[2 * ADDRSTRLEN + 16];

  for (i = 0; i < agent->remote_candidates_count; i++) {
    if (addr_equal(&agent->remote_candidates[i].addr, &candidate->addr)) {
      return 0;
    }
  }

  if (agent->remote_candidates_count >= AGENT_MAX_CANDIDATES) {
    LOGW("No room for remote candidate");
    return -1;
  }

  remote = &agent->remote_candidates[agent->remote_candidates_count++];
  memcpy(remote, candidate, sizeof(IceCandidate));

  addr_to_string(&remote->addr, addr_string, sizeof(addr_string));
  LOGI("Add remote candidate %s:%d", addr_string, remote->addr.port);

  // append pairs instead of rebuilding them, so the check in progress is kept
  for (i = 0; i < agent->local_candidates_count; i++) {
    if (agent->local_candidates[i].addr.family != remote->addr.family) {
      continue;
    }
    if (agent->candidate_pairs_num >= AGENT_MAX_CANDIDATE_PAIRS) {
      LOGW("No room for candidate pair");
      break;
    }
    pair = &agent->candidate_pairs[agent->candidate_pairs_num];
    pair->local = &agent->local_candidates[i];
    pair->remote = remote;
    pair->priority = pair->local->priority + remote->priority;
    pair->state = ICE_CANDIDATE_STATE_FROZEN;
    pair->conncheck = 0;
    agent_pair_to_string(pair, pair_string, sizeof(pair_string));
    LOGI("[%d] %s", agent->candidate_pairs_num, pair_string);
    agent->candidate_pairs_num++;
  }
  return 0;
}

void agent_update_candidate_pairs(Agent* agent) {
  int i, j;
  char local_addr_string[ADDRSTRLEN];
  char remote_addr_string[ADDRSTRLEN];
  int candidate_pairs_num = agent->candidate_pairs_num;
  // Please set gather candidates before set remote description
  agent->candidate_pairs_num = 0;
  for (i = 0; i < agent->local_candidates_count; i++) {
    for (j = 0; j < agent->remote_candidates_count; j++) {
      if (agent->local_candidates[i].addr.family == agent->remote_candidates[j].addr.family) {
        agent->candidate_pairs[agent->candidate_pairs_num].local = &agent->local_candidates[i];
        agent->candidate_pairs[agent->candidate_pairs_num].remote = &agent->remote_candidates[j];
        agent->candidate_pairs[agent->candidate_pairs_num].priority = agent->local_candidates[i].priority + agent->remote_candidates[j].priority;
        agent->candidate_pairs[agent->candidate_pairs_num].state = ICE_CANDIDATE_STATE_FROZEN;
        agent->candidate_pairs_num++;
      }
    }
  }

  if (candidate_pairs_num != agent->candidate_pairs_num) {
    LOGI("candidate pairs num %d:", agent->candidate_pairs_num);
    for (i = 0; i < agent->candidate_pairs_num; i++) {
      addr_to_string(&agent->candidate_pairs[i].local->addr, local_addr_string, sizeof(local_addr_string));
      addr_to_string(&agent->candidate_pairs[i].remote->addr, remote_addr_string, sizeof(remote_addr_string));
      LOGI("[%d] %s > %s", i, local_addr_string, remote_addr_string);
    }
  }
}

int agent_connectivity_check(Agent* agent) {
  char addr_string[ADDRSTRLEN];
  uint8_t buf[1400];

  if (agent_select_candidate_pair(agent) < 0) {
    // with trickle ICE there may be no pairs yet, keep answering the peer's checks
    // so that peer reflexive candidates can still be learned
    if (agent->candidate_pairs_num > 0) {
      LOGW("All %d candidate pairs failed, restart connectivity check", agent->candidate_pairs_num);
      agent_update_candidate_pairs(agent);
    }
    agent_recv(agent, buf, sizeof(buf));
    return -1;
  }
  if (agent->nominated_pair->state == ICE_CANDIDATE_STATE_SUCCEEDED) {
    agent->selected_pair = agent->nominated_pair;
    return 0;
  }
  if (agent->nominated_pair->state != ICE_CANDIDATE_STATE_INPROGRESS) {
    LOGI("nominated pair is not in progress");
    return -1;
  }

  if (agent->nominated_pair->conncheck % AGENT_CONNCHECK_PERIOD == 0) {
    addr_to_string(&agent->nominated_pair->remote->addr, addr_string, sizeof(addr_string));
    LOGD("send binding request to remote ip: %s, port: %d", addr_string, agent->nominated_pair->remote->addr.port);
    agent_send_binding_request(agent);
  }

  agent_recv(agent, buf, sizeof(buf));

  if (agent->nominated_pair->state == ICE_CANDIDATE_STATE_SUCCEEDED) {
    agent->selected_pair = agent->nominated_pair;
    return 0;
  }

  return -1;
}

int agent_select_candidate_pair(Agent* agent) {
  int i;
  char pair_string[2 * ADDRSTRLEN + 16];
  IceCandidatePair* pair = agent->nominated_pair;

  // keep checking the nominated pair first, it may be a triggered check out of array order
  if (pair != NULL && pair >= agent->candidate_pairs &&
      pair < agent->candidate_pairs + agent->candidate_pairs_num) {
    if (pair->state == ICE_CANDIDATE_STATE_SUCCEEDED) {
      return 0;
    } else if (pair->state == ICE_CANDIDATE_STATE_INPROGRESS) {
      pair->conncheck++;
      if (pair->conncheck < AGENT_CONNCHECK_MAX) {
        return 0;
      }
      pair->state = ICE_CANDIDATE_STATE_FAILED;
      agent_pair_to_string(pair, pair_string, sizeof(pair_string));
      LOGW("Candidate pair [%d] %s failed, no binding response after %d checks",
           (int)(pair - agent->candidate_pairs), pair_string, AGENT_CONNCHECK_MAX);
    }
  }

  for (i = 0; i < agent->candidate_pairs_num; i++) {
    if (agent->candidate_pairs[i].state == ICE_CANDIDATE_STATE_FROZEN) {
      // nominate this pair
      agent->nominated_pair = &agent->candidate_pairs[i];
      agent->candidate_pairs[i].conncheck = 0;
      agent->candidate_pairs[i].state = ICE_CANDIDATE_STATE_INPROGRESS;
      agent_pair_to_string(&agent->candidate_pairs[i], pair_string, sizeof(pair_string));
      LOGI("Checking candidate pair [%d] %s", i, pair_string);
      return 0;
    }
  }
  // all candidate pairs are failed
  return -1;
}
