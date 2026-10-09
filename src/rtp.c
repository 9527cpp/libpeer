#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "address.h"
#include "config.h"
#include "peer_connection.h"
#include "rtp.h"
#include "utils.h"

typedef enum RtpH264Type {

  NALU = 23,
  STAP_A = 24,
  FU_A = 28,

} RtpH264Type;

typedef struct NaluHeader {
  uint8_t type : 5;
  uint8_t nri : 2;
  uint8_t f : 1;
} NaluHeader;

typedef struct FuHeader {
  uint8_t type : 5;
  uint8_t r : 1;
  uint8_t e : 1;
  uint8_t s : 1;
} FuHeader;

#define RTP_PAYLOAD_SIZE (CONFIG_MTU - sizeof(RtpHeader))
#define FU_PAYLOAD_SIZE (CONFIG_MTU - sizeof(RtpHeader) - sizeof(FuHeader) - sizeof(NaluHeader))
#define NALU_START_CODE_SIZE 4
#define NALU_HEADER_SIZE 1
#define FU_HEADER_SIZE 1
#define FU_HEADER_START 0x80
#define FU_HEADER_END 0x40
#define NALU_TYPE_IDR 5
#define NALU_TYPE_SPS 7

// a packet with SRTP authentication tag
#define RTP_HISTORY_PACKET_SIZE (CONFIG_MTU + 32)
#define RTP_HISTORY_SEQ_WRITING UINT32_MAX

typedef struct RtpHistorySlot {
  uint32_t seq;  // RTP_HISTORY_SEQ_WRITING while the packet is being replaced
  uint16_t size;
  uint8_t packet[RTP_HISTORY_PACKET_SIZE];
} RtpHistorySlot;

struct RtpHistory {
  int capacity;
  RtpHistorySlot slots[];
};

RtpHistory* rtp_history_create(int capacity) {
  RtpHistory* history;
  int i;

  if (capacity <= 0) {
    return NULL;
  }

  history = malloc(sizeof(RtpHistory) + capacity * sizeof(RtpHistorySlot));
  if (history == NULL) {
    LOGW("Failed to allocate RTP history of %d packets", capacity);
    return NULL;
  }

  history->capacity = capacity;
  for (i = 0; i < capacity; i++) {
    history->slots[i].seq = RTP_HISTORY_SEQ_WRITING;
  }
  return history;
}

void rtp_history_destroy(RtpHistory* history) {
  free(history);
}

// seqlock: the reader detects a slot overwritten during its copy by checking seq again
void rtp_history_put(RtpHistory* history, uint16_t seq, const uint8_t* packet, size_t size) {
  RtpHistorySlot* slot;

  if (history == NULL || size > RTP_HISTORY_PACKET_SIZE) {
    return;
  }

  slot = &history->slots[seq % history->capacity];
  __atomic_store_n(&slot->seq, RTP_HISTORY_SEQ_WRITING, __ATOMIC_RELAXED);
  __atomic_thread_fence(__ATOMIC_RELEASE);
  memcpy(slot->packet, packet, size);
  slot->size = size;
  __atomic_store_n(&slot->seq, seq, __ATOMIC_RELEASE);
}

int rtp_history_get(RtpHistory* history, uint16_t seq, uint8_t* buf, size_t len) {
  RtpHistorySlot* slot;
  uint32_t seq_before;
  uint16_t size;

  if (history == NULL) {
    return -1;
  }

  slot = &history->slots[seq % history->capacity];
  seq_before = __atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE);
  if (seq_before != seq) {
    return -1;
  }

  size = slot->size;
  if (size > len) {
    return -1;
  }
  memcpy(buf, slot->packet, size);
  __atomic_thread_fence(__ATOMIC_ACQUIRE);

  if (__atomic_load_n(&slot->seq, __ATOMIC_RELAXED) != seq_before) {
    return -1;
  }
  return size;
}

int rtp_packet_validate(uint8_t* packet, size_t size) {
  if (size < 12)
    return 0;

  RtpHeader* rtp_header = (RtpHeader*)packet;
  uint8_t type = rtp_header_type(rtp_header);
  return ((type < 64) || (type >= 96));
}

uint32_t rtp_get_ssrc(uint8_t* packet) {
  RtpHeader* rtp_header = (RtpHeader*)packet;
  return ntohl(rtp_header->ssrc);
}

int rtp_get_payload(const uint8_t* packet, size_t size, const uint8_t** payload) {
  size_t offset = sizeof(RtpHeader);
  size_t padding = 0;

  if (size < offset || (packet[0] >> 6) != 2) {
    return -1;
  }

  // CSRC count
  offset += 4 * (packet[0] & 0x0f);

  if (packet[0] & 0x10) {
    // header extension: profile (16 bits), length in 32-bit words (16 bits)
    if (offset + 4 > size) {
      return -1;
    }
    offset += 4 + 4 * ((packet[offset + 2] << 8) | packet[offset + 3]);
  }

  if (packet[0] & 0x20) {
    // the last octet is the padding count
    padding = packet[size - 1];
  }

  if (offset + padding > size) {
    return -1;
  }

  *payload = packet + offset;
  return (int)(size - offset - padding);
}

static int rtp_encoder_encode_h264_single(RtpEncoder* rtp_encoder, uint8_t* buf, size_t size) {
  RtpPacket* rtp_packet = (RtpPacket*)rtp_encoder->buf;

  rtp_header_init(&rtp_packet->header, rtp_encoder->type);
  rtp_packet->header.seq_number = htons(rtp_encoder->seq_number);
  rtp_encoder->seq_number++;
  rtp_packet->header.timestamp = htonl(rtp_encoder->timestamp);
  rtp_packet->header.ssrc = htonl(rtp_encoder->ssrc);

  // I frame and P frame
  if ((*buf & 0x1f) == 0x05 || (*buf & 0x1f) == 0x01) {
    rtp_header_set_marker(&rtp_packet->header);
    rtp_encoder->timestamp += rtp_encoder->timestamp_increment;
  }
#if 0
  LOGI("markbit: %d, timestamp: %d, nalu type: %d", rtp_packet->header.markerbit, rtp_encoder->timestamp, buf[0] & 0x1f);
#endif

  memcpy(rtp_packet->payload, buf, size);
  rtp_encoder->on_packet(rtp_encoder->buf, size + sizeof(RtpHeader), rtp_encoder->user_data);
  return 0;
}

static int rtp_encoder_encode_h264_fu_a(RtpEncoder* rtp_encoder, uint8_t* buf, size_t size) {
  RtpPacket* rtp_packet = (RtpPacket*)rtp_encoder->buf;

  rtp_header_init(&rtp_packet->header, rtp_encoder->type);
  rtp_packet->header.timestamp = htonl(rtp_encoder->timestamp);
  rtp_packet->header.ssrc = htonl(rtp_encoder->ssrc);
  uint8_t type = buf[0] & 0x1f;
  uint8_t nri = (buf[0] & 0x60) >> 5;
  buf = buf + 1;
  size = size - 1;

  // increase timestamp if I, P frame
  if (type == 0x05 || type == 0x01) {
    rtp_encoder->timestamp += rtp_encoder->timestamp_increment;
  }

  NaluHeader* fu_indicator = (NaluHeader*)rtp_packet->payload;
  FuHeader* fu_header = (FuHeader*)rtp_packet->payload + sizeof(NaluHeader);
  fu_header->s = 1;

  while (size > 0) {
    fu_indicator->type = FU_A;
    fu_indicator->nri = nri;
    fu_indicator->f = 0;
    fu_header->type = type;
    fu_header->r = 0;
    rtp_packet->header.seq_number = htons(rtp_encoder->seq_number);
    rtp_encoder->seq_number++;

    if (size <= FU_PAYLOAD_SIZE) {
      fu_header->e = 1;
      rtp_header_set_marker(&rtp_packet->header);
      memcpy(rtp_packet->payload + sizeof(NaluHeader) + sizeof(FuHeader), buf, size);
      rtp_encoder->on_packet(rtp_encoder->buf, size + sizeof(RtpHeader) + sizeof(NaluHeader) + sizeof(FuHeader), rtp_encoder->user_data);
      break;
    }

    fu_header->e = 0;

    memcpy(rtp_packet->payload + sizeof(NaluHeader) + sizeof(FuHeader), buf, FU_PAYLOAD_SIZE);
    rtp_encoder->on_packet(rtp_encoder->buf, CONFIG_MTU, rtp_encoder->user_data);
    size -= FU_PAYLOAD_SIZE;
    buf += FU_PAYLOAD_SIZE;

    fu_header->s = 0;
  }
  return 0;
}

static uint8_t* h264_find_nalu(uint8_t* buf_start, uint8_t* buf_end) {
  uint8_t* p = buf_start + 2;

  while (p < buf_end) {
    if (*(p - 2) == 0x00 && *(p - 1) == 0x00 && *p == 0x01)
      return p + 1;
    p++;
  }

  return buf_end;
}

static int rtp_encoder_encode_h264(RtpEncoder* rtp_encoder, uint8_t* buf, size_t size) {
  uint8_t* buf_end = buf + size;
  uint8_t *pstart, *pend;
  size_t nalu_size;

  for (pstart = h264_find_nalu(buf, buf_end); pstart < buf_end; pstart = pend) {
    pend = h264_find_nalu(pstart, buf_end);
    nalu_size = pend - pstart;

    if (pend != buf_end)
      nalu_size--;

    while (pstart[nalu_size - 1] == 0x00)
      nalu_size--;

    if (nalu_size <= RTP_PAYLOAD_SIZE) {
      rtp_encoder_encode_h264_single(rtp_encoder, pstart, nalu_size);

    } else {
      rtp_encoder_encode_h264_fu_a(rtp_encoder, pstart, nalu_size);
    }
  }

  return 0;
}

static int rtp_encoder_encode_generic(RtpEncoder* rtp_encoder, uint8_t* buf, size_t size) {
  RtpHeader* rtp_header = (RtpHeader*)rtp_encoder->buf;
  rtp_header_init(rtp_header, rtp_encoder->type);
  rtp_header->seq_number = htons(rtp_encoder->seq_number);
  rtp_encoder->seq_number++;
  rtp_header->timestamp = htonl(rtp_encoder->timestamp);
  rtp_encoder->timestamp += rtp_encoder->timestamp_increment;
  rtp_header->ssrc = htonl(rtp_encoder->ssrc);
  memcpy(rtp_encoder->buf + sizeof(RtpHeader), buf, size);

  rtp_encoder->on_packet(rtp_encoder->buf, size + sizeof(RtpHeader), rtp_encoder->user_data);

  return 0;
}

void rtp_encoder_init(RtpEncoder* rtp_encoder, MediaCodec codec, RtpOnPacket on_packet, void* user_data) {
  rtp_encoder->on_packet = on_packet;
  rtp_encoder->user_data = user_data;
  rtp_encoder->timestamp = 0;
  rtp_encoder->seq_number = 0;

  switch (codec) {
    case CODEC_H264:
      rtp_encoder->type = PT_H264;
      rtp_encoder->ssrc = SSRC_H264;
      rtp_encoder->timestamp_increment = 90000 / 30;  // 30 FPS.
      rtp_encoder->encode_func = rtp_encoder_encode_h264;
      break;
    case CODEC_PCMA:
      rtp_encoder->type = PT_PCMA;
      rtp_encoder->ssrc = SSRC_PCMA;
      rtp_encoder->timestamp_increment = CONFIG_AUDIO_DURATION * 8000 / 1000;
      rtp_encoder->encode_func = rtp_encoder_encode_generic;
      break;
    case CODEC_PCMU:
      rtp_encoder->type = PT_PCMU;
      rtp_encoder->ssrc = SSRC_PCMU;
      rtp_encoder->timestamp_increment = CONFIG_AUDIO_DURATION * 8000 / 1000;
      rtp_encoder->encode_func = rtp_encoder_encode_generic;
      break;
    case CODEC_OPUS:
      rtp_encoder->type = PT_OPUS;
      rtp_encoder->ssrc = SSRC_OPUS;
      rtp_encoder->timestamp_increment = CONFIG_AUDIO_DURATION * 48000 / 1000;
      rtp_encoder->encode_func = rtp_encoder_encode_generic;
      break;
    default:
      break;
  }
}

int rtp_encoder_encode(RtpEncoder* rtp_encoder, const uint8_t* buf, size_t size) {
  return rtp_encoder->encode_func(rtp_encoder, (uint8_t*)buf, size);
}

static const uint8_t nalu_start_code[NALU_START_CODE_SIZE] = {0x00, 0x00, 0x00, 0x01};

// nalu begins with the start code
static void rtp_decode_h264_output(RtpDecoder* rtp_decoder, uint8_t* nalu, size_t size) {
  uint8_t type = nalu[NALU_START_CODE_SIZE] & 0x1f;

  if (rtp_decoder->wait_keyframe) {
    if (type != NALU_TYPE_SPS && type != NALU_TYPE_IDR) {
      return;
    }
    rtp_decoder->wait_keyframe = 0;
  }

  if (rtp_decoder->on_packet != NULL) {
    rtp_decoder->on_packet(nalu, size, rtp_decoder->user_data);
  }
}

static int rtp_decode_h264_single(RtpDecoder* rtp_decoder, const uint8_t* buf, size_t size) {
  if (size == 0 || NALU_START_CODE_SIZE + size > CONFIG_MAX_NALU_SIZE) {
    return -1;
  }

  memcpy(rtp_decoder->nalu_buf, nalu_start_code, NALU_START_CODE_SIZE);
  memcpy(rtp_decoder->nalu_buf + NALU_START_CODE_SIZE, buf, size);
  rtp_decode_h264_output(rtp_decoder, rtp_decoder->nalu_buf, NALU_START_CODE_SIZE + size);
  return 0;
}

static int rtp_decode_h264_stap_a(RtpDecoder* rtp_decoder, const uint8_t* buf, size_t size) {
  size_t pos = NALU_HEADER_SIZE;
  size_t nalu_size;

  while (pos + 2 <= size) {
    nalu_size = (buf[pos] << 8) | buf[pos + 1];
    pos += 2;

    if (nalu_size == 0 || pos + nalu_size > size) {
      LOGW("Invalid STAP-A packet: NALU length exceeds packet size");
      return -1;
    }

    rtp_decode_h264_single(rtp_decoder, buf + pos, nalu_size);
    pos += nalu_size;
  }
  return 0;
}

static int rtp_decode_h264_fu_a(RtpDecoder* rtp_decoder, const uint8_t* buf, size_t size) {
  uint8_t fu_indicator, fu_header;

  if (size < NALU_HEADER_SIZE + FU_HEADER_SIZE) {
    return -1;
  }

  fu_indicator = buf[0];
  fu_header = buf[1];
  buf += NALU_HEADER_SIZE + FU_HEADER_SIZE;
  size -= NALU_HEADER_SIZE + FU_HEADER_SIZE;

  if (fu_header & FU_HEADER_START) {
    memcpy(rtp_decoder->nalu_buf, nalu_start_code, NALU_START_CODE_SIZE);
    // F and NRI of the indicator with the type of the fragmented NAL unit
    rtp_decoder->nalu_buf[NALU_START_CODE_SIZE] = (fu_indicator & 0xe0) | (fu_header & 0x1f);
    rtp_decoder->nalu_size = NALU_START_CODE_SIZE + NALU_HEADER_SIZE;
  } else if (rtp_decoder->nalu_size == 0) {
    // the first fragment is missing
    rtp_decoder_reset(rtp_decoder);
    return -1;
  }

  if (rtp_decoder->nalu_size + size > CONFIG_MAX_NALU_SIZE) {
    LOGW("NAL unit exceeds %d bytes", CONFIG_MAX_NALU_SIZE);
    rtp_decoder_reset(rtp_decoder);
    return -1;
  }

  memcpy(rtp_decoder->nalu_buf + rtp_decoder->nalu_size, buf, size);
  rtp_decoder->nalu_size += size;

  if (fu_header & FU_HEADER_END) {
    rtp_decode_h264_output(rtp_decoder, rtp_decoder->nalu_buf, rtp_decoder->nalu_size);
    rtp_decoder->nalu_size = 0;
  }
  return 0;
}

static int rtp_decode_h264(RtpDecoder* rtp_decoder, const uint8_t* payload, size_t size) {
  uint8_t type;

  // padding only
  if (size == 0) {
    return 0;
  }

  if (rtp_decoder->nalu_buf == NULL && (rtp_decoder->nalu_buf = malloc(CONFIG_MAX_NALU_SIZE)) == NULL) {
    LOGW("Failed to allocate NALU buffer");
    return -1;
  }

  type = payload[0] & 0x1f;
  if (type != FU_A && rtp_decoder->nalu_size > 0) {
    // the last fragment is missing
    rtp_decoder_reset(rtp_decoder);
  }

  switch (type) {
    case STAP_A:
      return rtp_decode_h264_stap_a(rtp_decoder, payload, size);
    case FU_A:
      return rtp_decode_h264_fu_a(rtp_decoder, payload, size);
    default:
      return rtp_decode_h264_single(rtp_decoder, payload, size);
  }
}

static int rtp_decode_generic(RtpDecoder* rtp_decoder, const uint8_t* payload, size_t size) {
  if (rtp_decoder->on_packet != NULL && size > 0)
    rtp_decoder->on_packet((uint8_t*)payload, size, rtp_decoder->user_data);
  // even if there is no callback set, assume everything is ok for caller and do not return an error
  return (int)size;
}

void rtp_decoder_init(RtpDecoder* rtp_decoder, MediaCodec codec, RtpOnPacket on_packet, void* user_data) {
  memset(rtp_decoder, 0, sizeof(*rtp_decoder));
  rtp_decoder->on_packet = on_packet;
  rtp_decoder->user_data = user_data;

  switch (codec) {
    case CODEC_H264:
      rtp_decoder->decode_func = rtp_decode_h264;
      rtp_decoder->wait_keyframe = 1;
      break;
    case CODEC_PCMA:
    case CODEC_PCMU:
    case CODEC_OPUS:
      rtp_decoder->decode_func = rtp_decode_generic;
      break;
    default:
      break;
  }
}

void rtp_decoder_deinit(RtpDecoder* rtp_decoder) {
  free(rtp_decoder->nalu_buf);
  rtp_decoder->nalu_buf = NULL;
  rtp_decoder->nalu_size = 0;
}

void rtp_decoder_reset(RtpDecoder* rtp_decoder) {
  rtp_decoder->nalu_size = 0;
  if (rtp_decoder->decode_func == rtp_decode_h264) {
    rtp_decoder->wait_keyframe = 1;
  }
}

int rtp_decoder_decode(RtpDecoder* rtp_decoder, const uint8_t* packet, size_t size) {
  const uint8_t* payload;
  int payload_size;

  if (rtp_decoder->decode_func == NULL)
    return -1;

  if ((payload_size = rtp_get_payload(packet, size, &payload)) < 0) {
    return -1;
  }
  return rtp_decoder->decode_func(rtp_decoder, payload, payload_size);
}
