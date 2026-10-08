#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "agent.h"
#include "config.h"
#include "dtls_srtp.h"
#include "peer_connection.h"
#include "ports.h"
#include "rtcp.h"
#include "rtp.h"
#include "sctp.h"
#include "sdp.h"

#define STATE_CHANGED(pc, curr_state)                                 \
  if (pc->oniceconnectionstatechange && pc->state != curr_state) {    \
    pc->oniceconnectionstatechange(curr_state, pc->config.user_data); \
    pc->state = curr_state;                                           \
  }

// RTCP packets built locally, without SRTCP index and authentication tag
#define RTCP_BUFFER_SIZE 128

typedef struct RtpSenderCounters {
  uint32_t packet_count;
  uint32_t octet_count;
  uint32_t rtp_timestamp;
  uint32_t last_packet_ms;
} RtpSenderCounters;

typedef struct RtpSenderStats {
  uint32_t ssrc;
  uint32_t clock_rate;
  int is_video;

  // written by the thread sending media and read by the thread running
  // peer_connection_loop, an odd seqlock means the counters are being updated
  uint32_t seqlock;
  RtpSenderCounters counters;

  // owned by the thread running peer_connection_loop
  uint32_t last_sr_ms;
  uint32_t report_ms;
  uint32_t report_octet_count;
  uint32_t send_bitrate_bps;
} RtpSenderStats;

struct PeerConnection {
  PeerConfiguration config;
  PeerConnectionState state;
  Agent agent;
  DtlsSrtp dtls_srtp;
  Sctp sctp;
  DtlsSrtpRole role;
  char remote_fingerprint[DTLS_SRTP_FINGERPRINT_LENGTH];

  char sdp[CONFIG_SDP_BUFFER_SIZE];

  void (*onicecandidate)(char* sdp, void* user_data);
  void (*onlocalcandidate)(char* candidate, void* user_data);
  void (*oniceconnectionstatechange)(PeerConnectionState state, void* user_data);
  void (*on_connected)(void* userdata);
  void (*on_receiver_packet_loss)(float fraction_loss, uint32_t total_loss, void* user_data);
  void (*on_receiver_report)(const PeerReceiverReport* report, void* user_data);

  uint8_t temp_buf[CONFIG_MTU];
  uint8_t agent_buf[CONFIG_MTU];
  int agent_ret;
  uint8_t rtcp_buf[RTCP_BUFFER_SIZE + SRTP_MAX_TRAILER_LEN];
  uint8_t rtx_buf[CONFIG_MTU + 32];

  RtpEncoder artp_encoder;
  RtpEncoder vrtp_encoder;
  RtpDecoder vrtp_decoder;
  RtpDecoder artp_decoder;

  RtpSenderStats astats;
  RtpSenderStats vstats;
  // sent SRTP video packets for NACK retransmission
  RtpHistory* video_history;
  int pli_pending;

  uint32_t remote_assrc;
  uint32_t remote_vssrc;

  // Trickled candidates come from the signaling thread while the agent is owned by the
  // thread running peer_connection_loop, so they are passed through a single-producer
  // single-consumer queue.
  IceCandidate remote_candidate_queue[AGENT_MAX_CANDIDATES];
  int remote_candidate_queue_head;
  int remote_candidate_queue_tail;
};

static void rtp_sender_stats_init(RtpSenderStats* stats, uint32_t ssrc, uint32_t clock_rate, int is_video) {
  memset(stats, 0, sizeof(*stats));
  stats->ssrc = ssrc;
  stats->clock_rate = clock_rate;
  stats->is_video = is_video;
}

static void rtp_sender_stats_update(RtpSenderStats* stats, size_t payload_size, uint32_t rtp_timestamp) {
  uint32_t seqlock = stats->seqlock;

  __atomic_store_n(&stats->seqlock, seqlock + 1, __ATOMIC_RELAXED);
  __atomic_thread_fence(__ATOMIC_RELEASE);
  stats->counters.packet_count++;
  stats->counters.octet_count += payload_size;
  stats->counters.rtp_timestamp = rtp_timestamp;
  stats->counters.last_packet_ms = ports_get_epoch_time();
  __atomic_store_n(&stats->seqlock, seqlock + 2, __ATOMIC_RELEASE);
}

static void rtp_sender_stats_read(RtpSenderStats* stats, RtpSenderCounters* counters) {
  uint32_t seqlock;

  do {
    seqlock = __atomic_load_n(&stats->seqlock, __ATOMIC_ACQUIRE);
    *counters = stats->counters;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
  } while ((seqlock & 1) || seqlock != __atomic_load_n(&stats->seqlock, __ATOMIC_RELAXED));
}

static uint32_t peer_connection_clock_rate(MediaCodec codec) {
  switch (codec) {
    case CODEC_OPUS:
      return 48000;
    case CODEC_PCMA:
    case CODEC_PCMU:
      return 8000;
    default:
      return 90000;
  }
}

static void peer_connection_outgoing_rtp_packet(uint8_t* data, size_t size, void* user_data) {
  PeerConnection* pc = (PeerConnection*)user_data;
  RtpHeader* header = (RtpHeader*)data;
  uint32_t ssrc = ntohl(header->ssrc);
  uint16_t seq = ntohs(header->seq_number);
  uint32_t timestamp = ntohl(header->timestamp);
  RtpSenderStats* stats = NULL;
  int packet_len = (int)size;

  if (dtls_srtp_encrypt_rtp_packet(&pc->dtls_srtp, data, &packet_len) != 0) {
    return;
  }

  if (ssrc == pc->vstats.ssrc) {
    stats = &pc->vstats;
    rtp_history_put(pc->video_history, seq, data, packet_len);
  } else if (ssrc == pc->astats.ssrc) {
    stats = &pc->astats;
  }

  agent_send(&pc->agent, data, packet_len);

  if (stats) {
    rtp_sender_stats_update(stats, size - sizeof(RtpHeader), timestamp);
  }
}

static int peer_connection_send_rtcp(PeerConnection* pc, uint8_t* packet, int len) {
  if (dtls_srtp_encrypt_rtcp_packet(&pc->dtls_srtp, packet, &len) != 0) {
    LOGW("Failed to encrypt RTCP packet");
    return -1;
  }
  return agent_send(&pc->agent, packet, len);
}

static void peer_connection_send_sr(PeerConnection* pc, RtpSenderStats* stats, uint32_t now) {
  RtpSenderCounters counters;
  RtcpSenderInfo info;
  int len;

  if (stats->ssrc == 0 || (uint32_t)(now - stats->last_sr_ms) < CONFIG_RTCP_SR_INTERVAL) {
    return;
  }

  rtp_sender_stats_read(stats, &counters);
  if (counters.packet_count == 0) {
    return;
  }
  stats->last_sr_ms = now;

  info.ssrc = stats->ssrc;
  ports_get_ntp_time(&info.ntp_seconds, &info.ntp_fraction);
  // the RTP timestamp corresponding to the NTP timestamp, extrapolated from the last packet
  info.rtp_timestamp = counters.rtp_timestamp +
                       (uint32_t)((uint64_t)(uint32_t)(now - counters.last_packet_ms) * stats->clock_rate / 1000);
  info.packet_count = counters.packet_count;
  info.octet_count = counters.octet_count;

  len = rtcp_get_sr(pc->rtcp_buf, RTCP_BUFFER_SIZE, &info, SDP_CNAME);
  if (len > 0 && peer_connection_send_rtcp(pc, pc->rtcp_buf, len) > 0) {
    LOGD("Sent SR ssrc=%" PRIu32 " packets=%" PRIu32 " octets=%" PRIu32,
         info.ssrc, info.packet_count, info.octet_count);
  }
}

static void peer_connection_send_pli(PeerConnection* pc) {
  int len;

  if (pc->remote_vssrc == 0) {
    return;
  }

  len = rtcp_get_pli(pc->rtcp_buf, RTCP_BUFFER_SIZE, pc->vstats.ssrc, pc->remote_vssrc);
  if (len > 0 && peer_connection_send_rtcp(pc, pc->rtcp_buf, len) > 0) {
    LOGI("Sent PLI to ssrc=%" PRIu32, pc->remote_vssrc);
  }
}

static uint32_t peer_connection_target_bitrate(uint32_t send_bitrate_bps, float fraction_lost) {
  if (fraction_lost < 0.02f) {
    return (uint32_t)(send_bitrate_bps * 1.08f);
  } else if (fraction_lost <= 0.1f) {
    return send_bitrate_bps;
  }
  return (uint32_t)(send_bitrate_bps * (1.0f - 0.5f * fraction_lost));
}

static void peer_connection_incoming_report_block(PeerConnection* pc, RtcpReportBlock* block) {
  uint32_t ssrc = ntohl(block->ssrc);
  uint32_t flcnpl = ntohl(block->flcnpl);
  uint32_t lsr = ntohl(block->lsr);
  uint32_t dlsr = ntohl(block->dlsr);
  uint32_t now = ports_get_epoch_time();
  uint32_t ntp_seconds, ntp_fraction, rtt;
  RtpSenderStats* stats;
  RtpSenderCounters counters;
  PeerReceiverReport report;

  if (ssrc == pc->vstats.ssrc) {
    stats = &pc->vstats;
  } else if (ssrc == pc->astats.ssrc) {
    stats = &pc->astats;
  } else {
    return;
  }

  memset(&report, 0, sizeof(report));
  report.ssrc = ssrc;
  report.is_video = stats->is_video;
  report.fraction_lost = (flcnpl >> 24) / 256.0f;
  // 24 bits signed integer
  report.cumulative_lost = (int32_t)(flcnpl << 8) >> 8;
  report.jitter_ms = (uint32_t)((uint64_t)ntohl(block->jitter) * 1000 / stats->clock_rate);

  report.rtt_ms = -1;
  if (lsr != 0) {
    // middle 32 bits of the NTP timestamp, in units of 1/65536 seconds
    ports_get_ntp_time(&ntp_seconds, &ntp_fraction);
    rtt = ((ntp_seconds << 16) | (ntp_fraction >> 16)) - lsr - dlsr;
    report.rtt_ms = (int32_t)rtt < 0 ? 0 : (int32_t)(((uint64_t)rtt * 1000) >> 16);
  }

  // receivers may send reports along with every feedback, measure over a reasonable period
  rtp_sender_stats_read(stats, &counters);
  if (stats->report_ms == 0) {
    stats->report_ms = now;
    stats->report_octet_count = counters.octet_count;
  } else if ((uint32_t)(now - stats->report_ms) >= 500) {
    stats->send_bitrate_bps = (uint32_t)((uint64_t)(uint32_t)(counters.octet_count - stats->report_octet_count) * 8 * 1000 /
                                         (uint32_t)(now - stats->report_ms));
    stats->report_ms = now;
    stats->report_octet_count = counters.octet_count;
  }
  report.send_bitrate_bps = stats->send_bitrate_bps;
  report.target_bitrate_bps = peer_connection_target_bitrate(stats->send_bitrate_bps, report.fraction_lost);

  LOGD("RTCP report ssrc=%" PRIu32 " lost=%.3f/%" PRId32 " jitter=%" PRIu32 "ms rtt=%" PRId32 "ms bitrate=%" PRIu32 " target=%" PRIu32,
       report.ssrc, report.fraction_lost, report.cumulative_lost, report.jitter_ms, report.rtt_ms,
       report.send_bitrate_bps, report.target_bitrate_bps);

  if (pc->on_receiver_report) {
    pc->on_receiver_report(&report, pc->config.user_data);
  }

  if (pc->on_receiver_packet_loss && report.fraction_lost > 0) {
    pc->on_receiver_packet_loss(report.fraction_lost, (uint32_t)report.cumulative_lost, pc->config.user_data);
  }
}

static void peer_connection_retransmit_rtp_packet(uint16_t seq, void* user_data) {
  PeerConnection* pc = (PeerConnection*)user_data;
  int len = rtp_history_get(pc->video_history, seq, pc->rtx_buf, sizeof(pc->rtx_buf));

  if (len < 0) {
    LOGD("NACK seq=%u is not in history", seq);
    return;
  }

  // the packet was protected already, resending it as is avoids SRTP replay protection
  agent_send(&pc->agent, pc->rtx_buf, len);
  LOGD("Retransmitted seq=%u", seq);
}

static int peer_connection_dtls_srtp_recv(void* ctx, unsigned char* buf, size_t len) {
  int recv_max = 0;
  int ret = -1;
  DtlsSrtp* dtls_srtp = (DtlsSrtp*)ctx;
  PeerConnection* pc = (PeerConnection*)dtls_srtp->user_data;

  if (pc->agent_ret > 0 && pc->agent_ret <= len) {
    memcpy(buf, pc->agent_buf, pc->agent_ret);
    return pc->agent_ret;
  }

  while (recv_max < CONFIG_TLS_READ_TIMEOUT && pc->state == PEER_CONNECTION_CHECKING) {
    ret = agent_recv(&pc->agent, buf, len);

    if (ret > 0) {
      break;
    }

    recv_max++;
  }
  return ret;
}

static int peer_connection_dtls_srtp_send(void* ctx, const uint8_t* buf, size_t len) {
  DtlsSrtp* dtls_srtp = (DtlsSrtp*)ctx;
  PeerConnection* pc = (PeerConnection*)dtls_srtp->user_data;

  // LOGD("send %.4x %.4x, %ld", *(uint16_t*)buf, *(uint16_t*)(buf + 2), len);
  return agent_send(&pc->agent, buf, len);
}

static void peer_connection_incoming_rtcp(PeerConnection* pc, uint8_t* buf, size_t len) {
  RtcpHeader* rtcp_header;
  RtcpReportBlock* blocks;
  RtcpFb* fb;
  size_t pos = 0;
  size_t packet_len;
  int i, count, fmt;

  while (pos + sizeof(RtcpHeader) <= len) {
    rtcp_header = (RtcpHeader*)(buf + pos);
    packet_len = 4 * ((size_t)ntohs(rtcp_header->length) + 1);
    if (rtcp_header_version(rtcp_header) != 2 || pos + packet_len > len) {
      LOGW("Invalid RTCP packet, type=%d offset=%zu len=%zu", rtcp_header->type, pos, len);
      break;
    }

    fmt = rtcp_header_rc(rtcp_header);
    switch (rtcp_header->type) {
      case RTCP_SR:
      case RTCP_RR:
        count = rtcp_get_report_blocks(buf + pos, packet_len, &blocks);
        for (i = 0; i < count; i++) {
          peer_connection_incoming_report_block(pc, &blocks[i]);
        }
        break;
      case RTCP_RTPFB:
        fb = (RtcpFb*)rtcp_header;
        if (fmt == RTCP_RTPFB_NACK && packet_len >= 12 && ntohl(fb->media) == pc->vstats.ssrc && pc->video_history) {
          count = rtcp_parse_nack(buf + pos, packet_len, peer_connection_retransmit_rtp_packet, pc);
          LOGD("RTCP NACK %d packets", count);
        }
        break;
      case RTCP_PSFB:
        LOGD("RTCP_PSFB %d", fmt);
        if ((fmt == RTCP_PSFB_PLI || fmt == RTCP_PSFB_FIR) && pc->config.on_request_keyframe) {
          pc->config.on_request_keyframe(pc->config.user_data);
        }
        break;
      default:
        break;
    }

    pos += packet_len;
  }
}

const char* peer_connection_state_to_string(PeerConnectionState state) {
  switch (state) {
    case PEER_CONNECTION_NEW:
      return "new";
    case PEER_CONNECTION_CHECKING:
      return "checking";
    case PEER_CONNECTION_CONNECTED:
      return "connected";
    case PEER_CONNECTION_FAILED:
      return "failed";
    case PEER_CONNECTION_CLOSED:
      return "closed";
    case PEER_CONNECTION_DISCONNECTED:
      return "disconnected";
    default:
      return "unknown";
  }
}

PeerConnectionState peer_connection_get_state(PeerConnection* pc) {
  return pc->state;
}

void* peer_connection_get_sctp(PeerConnection* pc) {
  return &pc->sctp;
}

PeerConnection* peer_connection_create(PeerConfiguration* config) {
  PeerConnection* pc = calloc(1, sizeof(PeerConnection));
  if (!pc) {
    return NULL;
  }

  memcpy(&pc->config, config, sizeof(PeerConfiguration));

  agent_create(&pc->agent);

  memset(&pc->sctp, 0, sizeof(pc->sctp));

  if (pc->config.audio_codec) {
    rtp_encoder_init(&pc->artp_encoder, pc->config.audio_codec,
                     peer_connection_outgoing_rtp_packet, (void*)pc);

    rtp_decoder_init(&pc->artp_decoder, pc->config.audio_codec,
                     pc->config.onaudiotrack, pc->config.user_data);

    rtp_sender_stats_init(&pc->astats, pc->artp_encoder.ssrc, peer_connection_clock_rate(pc->config.audio_codec), 0);
  }

  if (pc->config.video_codec) {
    rtp_encoder_init(&pc->vrtp_encoder, pc->config.video_codec,
                     peer_connection_outgoing_rtp_packet, (void*)pc);

    rtp_decoder_init(&pc->vrtp_decoder, pc->config.video_codec,
                     pc->config.onvideotrack, pc->config.user_data);

    rtp_sender_stats_init(&pc->vstats, pc->vrtp_encoder.ssrc, peer_connection_clock_rate(pc->config.video_codec), 1);
    pc->video_history = rtp_history_create(CONFIG_RTP_HISTORY_SIZE);
  }

  return pc;
}

void peer_connection_destroy(PeerConnection* pc) {
  if (pc) {
    sctp_destroy_association(&pc->sctp);
    dtls_srtp_deinit(&pc->dtls_srtp);
    agent_destroy(&pc->agent);
    rtp_history_destroy(pc->video_history);
    free(pc);
    pc = NULL;
  }
}

void peer_connection_close(PeerConnection* pc) {
  pc->state = PEER_CONNECTION_CLOSED;
}

int peer_connection_send_audio(PeerConnection* pc, const uint8_t* buf, size_t len) {
  if (pc->state != PEER_CONNECTION_CONNECTED) {
    // LOGE("dtls_srtp not connected");
    return -1;
  }
  return rtp_encoder_encode(&pc->artp_encoder, buf, len);
}

int peer_connection_send_video(PeerConnection* pc, const uint8_t* buf, size_t len) {
  if (pc->state != PEER_CONNECTION_CONNECTED) {
    // LOGE("dtls_srtp not connected");
    return -1;
  }
  return rtp_encoder_encode(&pc->vrtp_encoder, buf, len);
}

int peer_connection_datachannel_send(PeerConnection* pc, char* message, size_t len) {
  return peer_connection_datachannel_send_sid(pc, message, len, 0);
}

int peer_connection_datachannel_send_sid(PeerConnection* pc, char* message, size_t len, uint16_t sid) {
  if (!sctp_is_connected(&pc->sctp)) {
    LOGE("sctp not connected");
    return -1;
  }
  if (pc->config.datachannel == DATA_CHANNEL_STRING)
    return sctp_outgoing_data(&pc->sctp, message, len, PPID_STRING, sid);
  else
    return sctp_outgoing_data(&pc->sctp, message, len, PPID_BINARY, sid);
}

int peer_connection_create_datachannel(PeerConnection* pc, DecpChannelType channel_type, uint16_t priority, uint32_t reliability_parameter, char* label, char* protocol) {
  return peer_connection_create_datachannel_sid(pc, channel_type, priority, reliability_parameter, label, protocol, 0);
}

int peer_connection_create_datachannel_sid(PeerConnection* pc, DecpChannelType channel_type, uint16_t priority, uint32_t reliability_parameter, char* label, char* protocol, uint16_t sid) {
  int rtrn = -1;

  if (!sctp_is_connected(&pc->sctp)) {
    LOGE("sctp not connected");
    return rtrn;
  }

  //  0                   1                   2                   3
  //  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
  // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  // |  Message Type |  Channel Type |            Priority           |
  // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  // |                    Reliability Parameter                      |
  // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  // |         Label Length          |       Protocol Length         |
  // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  // |                                                               |
  // |                             Label                             |
  // |                                                               |
  // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  // |                                                               |
  // |                            Protocol                           |
  // |                                                               |
  // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  int msg_size = 12 + strlen(label) + strlen(protocol);
  uint16_t priority_big_endian = htons(priority);
  uint32_t reliability_big_endian = htonl(reliability_parameter);
  uint16_t label_length = htons(strlen(label));
  uint16_t protocol_length = htons(strlen(protocol));
  char* msg = calloc(1, msg_size);
  if (!msg) {
    return rtrn;
  }

  msg[0] = DATA_CHANNEL_OPEN;
  msg[1] = (uint8_t)channel_type;
  memcpy(msg + 2, &priority_big_endian, sizeof(uint16_t));
  memcpy(msg + 4, &reliability_big_endian, sizeof(uint32_t));
  memcpy(msg + 8, &label_length, sizeof(uint16_t));
  memcpy(msg + 10, &protocol_length, sizeof(uint16_t));
  memcpy(msg + 12, label, strlen(label));
  memcpy(msg + 12 + strlen(label), protocol, strlen(protocol));

  rtrn = sctp_outgoing_data(&pc->sctp, msg, msg_size, PPID_CONTROL, sid);
  free(msg);
  return rtrn;
}

static void peer_connection_gather_server_candidates(PeerConnection* pc) {
  int i, j, count;
  char candidate[256];

  for (i = 0; i < sizeof(pc->config.ice_servers) / sizeof(pc->config.ice_servers[0]); ++i) {
    if (!pc->config.ice_servers[i].urls) {
      continue;
    }
    LOGI("ice server: %s", pc->config.ice_servers[i].urls);
    count = pc->agent.local_candidates_count;
    agent_gather_candidate(&pc->agent, pc->config.ice_servers[i].urls, pc->config.ice_servers[i].username, pc->config.ice_servers[i].credential);

    if (!pc->onlocalcandidate) {
      continue;
    }
    for (j = count; j < pc->agent.local_candidates_count; j++) {
      ice_candidate_to_description(&pc->agent.local_candidates[j], candidate, sizeof(candidate));
      candidate[strcspn(candidate, "\r\n")] = '\0';
      // "a=candidate:..." in SDP, "candidate:..." when trickled
      pc->onlocalcandidate(candidate + strlen("a="), pc->config.user_data);
    }
  }
}

static char* peer_connection_dtls_role_setup_value(DtlsSrtpRole d) {
  return d == DTLS_SRTP_ROLE_SERVER ? "a=setup:passive" : "a=setup:active";
}

static void peer_connection_apply_remote_candidates(PeerConnection* pc) {
  int tail = __atomic_load_n(&pc->remote_candidate_queue_tail, __ATOMIC_RELAXED);

  while (tail != __atomic_load_n(&pc->remote_candidate_queue_head, __ATOMIC_ACQUIRE)) {
    agent_add_remote_candidate(&pc->agent, &pc->remote_candidate_queue[tail]);
    tail = (tail + 1) % AGENT_MAX_CANDIDATES;
    __atomic_store_n(&pc->remote_candidate_queue_tail, tail, __ATOMIC_RELEASE);
  }
}

int peer_connection_loop(PeerConnection* pc) {
  uint32_t ssrc = 0;
  int ret;
  char local_addr[ADDRSTRLEN];
  char remote_addr[ADDRSTRLEN];
  memset(pc->agent_buf, 0, sizeof(pc->agent_buf));
  pc->agent_ret = -1;
  switch (pc->state) {
    case PEER_CONNECTION_NEW:
      break;

    case PEER_CONNECTION_CHECKING:
      peer_connection_apply_remote_candidates(pc);
      if (pc->agent.selected_pair) {
        addr_to_string(&pc->agent.selected_pair->local->addr, local_addr, sizeof(local_addr));
        addr_to_string(&pc->agent.selected_pair->remote->addr, remote_addr, sizeof(remote_addr));
        LOGI("ICE binding succeeded, selected pair %s:%d > %s:%d, start DTLS handshake as %s",
             local_addr, pc->agent.selected_pair->local->addr.port,
             remote_addr, pc->agent.selected_pair->remote->addr.port,
             pc->role == DTLS_SRTP_ROLE_SERVER ? "server" : "client");
        // if ice candidate pass the connectivity check, then we can start DTLS-SRTP handshake
        ret = dtls_srtp_handshake(&pc->dtls_srtp, NULL,
                                  pc->remote_fingerprint);
        if (ret == 0) {
          LOGI("DTLS handshake succeeded");
        } else {
          LOGW("DTLS handshake failed: -0x%.4x, retry in next loop", (unsigned int)-ret);
        }
      } else {
        agent_connectivity_check(&pc->agent);
      }

      if (pc->dtls_srtp.state == DTLS_SRTP_STATE_CONNECTED) {
        LOGD("DTLS-SRTP handshake done");
        // media is not sent before CONNECTED, so nothing races with this thread here
        if (pc->astats.ssrc) {
          dtls_srtp_add_outbound_stream(&pc->dtls_srtp, pc->astats.ssrc);
          rtp_sender_stats_init(&pc->astats, pc->astats.ssrc, pc->astats.clock_rate, 0);
        }
        if (pc->vstats.ssrc) {
          dtls_srtp_add_outbound_stream(&pc->dtls_srtp, pc->vstats.ssrc);
          rtp_sender_stats_init(&pc->vstats, pc->vstats.ssrc, pc->vstats.clock_rate, 1);
        }
        __atomic_store_n(&pc->pli_pending, 0, __ATOMIC_RELAXED);
        if (pc->config.datachannel) {
          LOGI("Creating SCTP association");
          sctp_create_association(&pc->sctp, &pc->dtls_srtp);
          pc->sctp.userdata = pc->config.user_data;
        }

        STATE_CHANGED(pc, PEER_CONNECTION_CONNECTED);
      }
      break;
    case PEER_CONNECTION_CONNECTED:
      if ((pc->agent_ret = agent_recv(&pc->agent, pc->agent_buf, sizeof(pc->agent_buf))) > 0) {
        LOGD("agent_recv %d", pc->agent_ret);

        if (rtcp_probe(pc->agent_buf, pc->agent_ret)) {
          LOGD("Got RTCP packet");
          if ((ret = dtls_srtp_decrypt_rtcp_packet(&pc->dtls_srtp, pc->agent_buf, &pc->agent_ret)) != 0) {
            LOGW("Failed to decrypt RTCP packet: %d", ret);
          } else {
            peer_connection_incoming_rtcp(pc, pc->agent_buf, pc->agent_ret);
          }

        } else if (dtls_srtp_probe(pc->agent_buf)) {
          int ret = dtls_srtp_read(&pc->dtls_srtp, pc->temp_buf, sizeof(pc->temp_buf));
          LOGD("Got DTLS data %d", ret);

          if (ret > 0) {
            sctp_incoming_data(&pc->sctp, (char*)pc->temp_buf, ret);
          }

        } else if (rtp_packet_validate(pc->agent_buf, pc->agent_ret)) {
          LOGD("Got RTP packet");

          ssrc = rtp_get_ssrc(pc->agent_buf);
          if ((ret = dtls_srtp_decrypt_rtp_packet(&pc->dtls_srtp, pc->agent_buf, &pc->agent_ret)) != 0) {
            LOGD("Failed to decrypt RTP packet: %d", ret);
          } else if (ssrc == pc->remote_assrc) {
            rtp_decoder_decode(&pc->artp_decoder, pc->agent_buf, pc->agent_ret);
          } else if (ssrc == pc->remote_vssrc) {
            rtp_decoder_decode(&pc->vrtp_decoder, pc->agent_buf, pc->agent_ret);
          }

        } else {
          LOGW("Unknown data");
        }
      }

      if (__atomic_exchange_n(&pc->pli_pending, 0, __ATOMIC_RELAXED)) {
        peer_connection_send_pli(pc);
      }

      {
        uint32_t now = ports_get_epoch_time();
        peer_connection_send_sr(pc, &pc->vstats, now);
        peer_connection_send_sr(pc, &pc->astats, now);
      }

#if CONFIG_STUN_KEEPALIVE_INTERVAL > 0
      {
        uint32_t elapsed =
            (uint32_t)(ports_get_epoch_time() - pc->agent.binding_request_sent_time);

        if (pc->agent.binding_request_pending) {
          if (elapsed >= CONFIG_STUN_KEEPALIVE_TIMEOUT) {
            LOGW("STUN keepalive response timeout");
            STATE_CHANGED(pc, PEER_CONNECTION_CLOSED);
          }
        } else if (elapsed >= CONFIG_STUN_KEEPALIVE_INTERVAL) {
          if (agent_send_binding_request(&pc->agent) < 0) {
            LOGW("Failed to send STUN keepalive");
          } else {
            LOGD("Sent STUN keepalive");
          }
        }
      }
#endif

      break;
    case PEER_CONNECTION_FAILED:
      break;
    case PEER_CONNECTION_DISCONNECTED:
      break;
    case PEER_CONNECTION_CLOSED:
      break;
    default:
      break;
  }

  return 0;
}

void peer_connection_set_remote_description(PeerConnection* pc, const char* sdp, SdpType type) {
  char* start = (char*)sdp;
  char* line = NULL;
  char buf[256];
  char* val_start = NULL;
  uint32_t* ssrc = NULL;
  DtlsSrtpRole role = DTLS_SRTP_ROLE_SERVER;
  int is_update = 0;
  Agent* agent = &pc->agent;

  while ((line = strstr(start, "\r\n"))) {
    line = strstr(start, "\r\n");
    strncpy(buf, start, line - start);
    buf[line - start] = '\0';

    if (strstr(buf, "a=setup:passive")) {
      role = DTLS_SRTP_ROLE_CLIENT;
    }

    if (strstr(buf, "a=fingerprint")) {
      const char* fingerprint = buf + 22;
      size_t fingerprint_len = strlen(fingerprint);

      if (fingerprint_len >= sizeof(pc->remote_fingerprint)) {
        LOGE("remote fingerprint is too long");
        return;
      }
      memcpy(pc->remote_fingerprint, fingerprint, fingerprint_len + 1);
    }

    if (strstr(buf, "a=ice-ufrag") &&
        strlen(agent->remote_ufrag) != 0 &&
        (strncmp(buf + strlen("a=ice-ufrag:"), agent->remote_ufrag, strlen(agent->remote_ufrag)) == 0)) {
      is_update = 1;
    }

    if (strstr(buf, "m=video")) {
      ssrc = &pc->remote_vssrc;
    } else if (strstr(buf, "m=audio")) {
      ssrc = &pc->remote_assrc;
    }

    if ((val_start = strstr(buf, "a=ssrc:")) && ssrc) {
      *ssrc = strtoul(val_start + 7, NULL, 10);
      LOGD("SSRC: %" PRIu32, *ssrc);
      ssrc = NULL;
    }

    start = line + 2;
  }

  if (is_update) {
    return;
  }

  agent_set_remote_description(&pc->agent, (char*)sdp);
  agent_update_candidate_pairs(&pc->agent);
  if (pc->state == PEER_CONNECTION_NEW) {
    STATE_CHANGED(pc, PEER_CONNECTION_CHECKING);
  }
}

void peer_connection_set_local_description(PeerConnection* pc, const char* sdp, SdpType sdp_type) {
  // just for gathering ICE candidates
  if (pc->state == PEER_CONNECTION_CONNECTED) {
    return;
  }
  pc->sctp.connected = 0;
  // drop candidates trickled for the previous session, the queue is not consumed outside CHECKING
  pc->remote_candidate_queue_tail = pc->remote_candidate_queue_head;

  dtls_srtp_deinit(&pc->dtls_srtp);
  memset(&pc->dtls_srtp, 0, sizeof(pc->dtls_srtp));

  switch (sdp_type) {
    case SDP_TYPE_OFFER:
      pc->role = DTLS_SRTP_ROLE_SERVER;
      agent_clear_candidates(&pc->agent);
      pc->agent.mode = AGENT_MODE_CONTROLLING;
      break;
    case SDP_TYPE_ANSWER:
      pc->role = DTLS_SRTP_ROLE_CLIENT;
      pc->agent.mode = AGENT_MODE_CONTROLLED;
      break;
    default:
      break;
  }

  if (dtls_srtp_init(&pc->dtls_srtp, pc->role, pc) != 0) {
    LOGE("dtls_srtp_init failed");
    STATE_CHANGED(pc, PEER_CONNECTION_FAILED);
    return;
  }
  pc->dtls_srtp.udp_recv = peer_connection_dtls_srtp_recv;
  pc->dtls_srtp.udp_send = peer_connection_dtls_srtp_send;

  agent_create_ice_credential(&pc->agent);
  agent_gather_candidate(&pc->agent, NULL, NULL, NULL);  // host address
  if (!pc->onlocalcandidate) {
    peer_connection_gather_server_candidates(pc);
  }
}

static const char* peer_connection_create_sdp(PeerConnection* pc, SdpType sdp_type) {
  char* description = (char*)pc->temp_buf;
  memset(pc->temp_buf, 0, sizeof(pc->temp_buf));
  memset(pc->sdp, 0, sizeof(pc->sdp));
  // TODO: check if we have video or audio codecs
  int sdp_audio = (pc->config.audio_codec != CODEC_NONE) && (sdp_type == SDP_TYPE_OFFER || pc->remote_assrc > 0);
  int sdp_video = (pc->config.video_codec != CODEC_NONE) && (sdp_type == SDP_TYPE_OFFER || pc->remote_vssrc > 0);

  sdp_create(pc->sdp, sdp_video, sdp_audio, pc->config.datachannel);
  if (pc->onlocalcandidate) {
    sdp_append(pc->sdp, "a=ice-options:trickle");
  }
  agent_get_local_description(&pc->agent, description, sizeof(pc->temp_buf));

  sdp_append(pc->sdp, "a=ice-ufrag:%s", pc->agent.local_ufrag);
  sdp_append(pc->sdp, "a=ice-pwd:%s", pc->agent.local_upwd);
  sdp_append(pc->sdp, "a=fingerprint:sha-256 %s", pc->dtls_srtp.local_fingerprint);
  sdp_append(pc->sdp, peer_connection_dtls_role_setup_value(pc->role));

  if (sdp_video) {
    switch (pc->config.video_codec) {
      case CODEC_H264:
        sdp_append_h264(pc->sdp);
        break;
      case CODEC_VP8:
        sdp_append_vp8(pc->sdp);
        break;
    }
    sdp_append(pc->sdp, description);
  }

  if (sdp_audio) {
    switch (pc->config.audio_codec) {
      case CODEC_PCMA:
        sdp_append_pcma(pc->sdp);
        break;
      case CODEC_PCMU:
        sdp_append_pcmu(pc->sdp);
        break;
      case CODEC_OPUS:
        sdp_append_opus(pc->sdp);
      default:
        break;
    }
    sdp_append(pc->sdp, description);
  }

  if (pc->config.datachannel) {
    sdp_append_datachannel(pc->sdp);
    sdp_append(pc->sdp, description);
  }

  if (pc->onicecandidate) {
    pc->onicecandidate(pc->sdp, pc->config.user_data);
  }
  return pc->sdp;
}

const char* peer_connection_create_offer(PeerConnection* pc) {
  const char* sdp;
  peer_connection_set_local_description(pc, NULL, SDP_TYPE_OFFER);
  sdp = peer_connection_create_sdp(pc, SDP_TYPE_OFFER);
  if (pc->onlocalcandidate) {
    peer_connection_gather_server_candidates(pc);
  }
  return sdp;
}

const char* peer_connection_create_answer(PeerConnection* pc) {
  const char* sdp;
  peer_connection_set_local_description(pc, NULL, SDP_TYPE_ANSWER);
  sdp = peer_connection_create_sdp(pc, SDP_TYPE_ANSWER);
  if (pc->onlocalcandidate) {
    peer_connection_gather_server_candidates(pc);
  }
  return sdp;
}

int peer_connection_request_keyframe(PeerConnection* pc) {
  if (pc->remote_vssrc == 0) {
    return -1;
  }
  __atomic_store_n(&pc->pli_pending, 1, __ATOMIC_RELAXED);
  return 0;
}

// callbacks
void peer_connection_on_connected(PeerConnection* pc, void (*on_connected)(void* userdata)) {
  pc->on_connected = on_connected;
}

void peer_connection_on_receiver_packet_loss(PeerConnection* pc,
                                             void (*on_receiver_packet_loss)(float fraction_loss, uint32_t total_loss, void* userdata)) {
  pc->on_receiver_packet_loss = on_receiver_packet_loss;
}

void peer_connection_on_receiver_report(PeerConnection* pc,
                                        void (*on_receiver_report)(const PeerReceiverReport* report, void* userdata)) {
  pc->on_receiver_report = on_receiver_report;
}

void peer_connection_onicecandidate(PeerConnection* pc, void (*onicecandidate)(char* sdp, void* userdata)) {
  pc->onicecandidate = onicecandidate;
}

void peer_connection_onlocalcandidate(PeerConnection* pc, void (*onlocalcandidate)(char* candidate, void* userdata)) {
  pc->onlocalcandidate = onlocalcandidate;
}

void peer_connection_oniceconnectionstatechange(PeerConnection* pc,
                                                void (*oniceconnectionstatechange)(PeerConnectionState state, void* userdata)) {
  pc->oniceconnectionstatechange = oniceconnectionstatechange;
}

void peer_connection_ondatachannel(PeerConnection* pc,
                                   void (*onmessage)(char* msg, size_t len, void* userdata, uint16_t sid),
                                   void (*onopen)(void* userdata),
                                   void (*onclose)(void* userdata)) {
  if (pc) {
    sctp_onopen(&pc->sctp, onopen);
    sctp_onclose(&pc->sctp, onclose);
    sctp_onmessage(&pc->sctp, onmessage);
  }
}

int peer_connection_lookup_sid(PeerConnection* pc, const char* label, uint16_t* sid) {
  for (int i = 0; i < pc->sctp.stream_count; i++) {
    if (strncmp(pc->sctp.stream_table[i].label, label, sizeof(pc->sctp.stream_table[i].label)) == 0) {
      *sid = pc->sctp.stream_table[i].sid;
      return 0;
    }
  }
  return -1;  // Not found
}

char* peer_connection_lookup_sid_label(PeerConnection* pc, uint16_t sid) {
  for (int i = 0; i < pc->sctp.stream_count; i++) {
    if (pc->sctp.stream_table[i].sid == sid) {
      return pc->sctp.stream_table[i].label;
    }
  }
  return NULL;  // Not found
}

int peer_connection_add_ice_candidate(PeerConnection* pc, char* candidate) {
  int head = __atomic_load_n(&pc->remote_candidate_queue_head, __ATOMIC_RELAXED);
  int next = (head + 1) % AGENT_MAX_CANDIDATES;

  if (next == __atomic_load_n(&pc->remote_candidate_queue_tail, __ATOMIC_ACQUIRE)) {
    LOGW("Remote candidate queue is full, drop %s", candidate);
    return -1;
  }

  if (ice_candidate_from_description(&pc->remote_candidate_queue[head], candidate, candidate + strlen(candidate)) != 0) {
    return -1;
  }

  LOGD("Add candidate: %s", candidate);
  __atomic_store_n(&pc->remote_candidate_queue_head, next, __ATOMIC_RELEASE);
  return 0;
}
