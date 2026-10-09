#include <stdlib.h>
#include <string.h>

#include "address.h"
#include "config.h"
#include "rtp_receiver.h"
#include "utils.h"

// RFC 3550 A.1
#define MAX_DROPOUT 3000
#define MAX_MISORDER 100

// a lost packet is requested at most this many times
#define MAX_NACKS 3
#define MIN_NACK_INTERVAL 20

typedef enum RtpReceiverSlotState {
  SLOT_EMPTY = 0,
  SLOT_MISSING,
  SLOT_RECEIVED,
} RtpReceiverSlotState;

struct RtpReceiverSlot {
  uint16_t seq;
  uint8_t state;
  uint8_t nacks;
  uint16_t size;
  uint32_t missing_ms;  // when the packet was found missing
  uint32_t nack_ms;
  uint8_t packet[CONFIG_MTU];
};

#if CONFIG_RTP_JITTER_BUFFER_SIZE & (CONFIG_RTP_JITTER_BUFFER_SIZE - 1)
#error "CONFIG_RTP_JITTER_BUFFER_SIZE must be a power of 2"
#endif

static RtpReceiverSlot* rtp_receiver_slot(RtpReceiver* receiver, uint16_t seq) {
  // a power of 2 keeps the slots of consecutive sequence numbers apart across the wrap around
  return &receiver->slots[seq & (CONFIG_RTP_JITTER_BUFFER_SIZE - 1)];
}

static void rtp_receiver_decode(RtpReceiver* receiver, const uint8_t* packet, size_t size) {
  if (receiver->decoder) {
    rtp_decoder_decode(receiver->decoder, packet, size);
  }
}

static void rtp_receiver_drop(RtpReceiver* receiver) {
  receiver->lost_count++;
  if (receiver->decoder) {
    rtp_decoder_reset(receiver->decoder);
  }
}

// hand the buffered packets before until to the decoder, dropping the missing ones
static void rtp_receiver_advance(RtpReceiver* receiver, uint16_t until) {
  RtpReceiverSlot* slot;

  while (receiver->next_seq != until) {
    slot = rtp_receiver_slot(receiver, receiver->next_seq);
    if (slot->state == SLOT_RECEIVED) {
      rtp_receiver_decode(receiver, slot->packet, slot->size);
    } else if (slot->state == SLOT_MISSING) {
      receiver->missing--;
      rtp_receiver_drop(receiver);
    }
    slot->state = SLOT_EMPTY;
    receiver->next_seq++;
  }
}

// hand the packets received in order to the decoder
static void rtp_receiver_play(RtpReceiver* receiver) {
  RtpReceiverSlot* slot;

  while (receiver->next_seq != receiver->end_seq) {
    slot = rtp_receiver_slot(receiver, receiver->next_seq);
    if (slot->state != SLOT_RECEIVED) {
      break;
    }
    rtp_receiver_decode(receiver, slot->packet, slot->size);
    slot->state = SLOT_EMPTY;
    receiver->next_seq++;
  }
}

static void rtp_receiver_buffer(RtpReceiver* receiver, const uint8_t* packet, size_t size, uint16_t seq, uint32_t now) {
  RtpReceiverSlot* slot;
  uint16_t s;

  if ((uint16_t)(seq - receiver->next_seq) >= 65536 - MAX_DROPOUT) {
    // already given to the decoder or given up
    return;
  }

  if ((uint16_t)(seq - receiver->next_seq) >= MAX_DROPOUT) {
    // the stream restarted
    rtp_receiver_advance(receiver, receiver->end_seq);
    rtp_receiver_drop(receiver);
    receiver->next_seq = receiver->end_seq = seq;
  } else if ((uint16_t)(seq - receiver->next_seq) >= CONFIG_RTP_JITTER_BUFFER_SIZE) {
    // make room for the packet
    s = seq - CONFIG_RTP_JITTER_BUFFER_SIZE + 1;
    if ((int16_t)(s - receiver->end_seq) > 0) {
      rtp_receiver_advance(receiver, receiver->end_seq);
      rtp_receiver_drop(receiver);
      receiver->next_seq = receiver->end_seq = s;
    } else {
      rtp_receiver_advance(receiver, s);
    }
  }

  if ((int16_t)(seq - receiver->end_seq) >= 0) {
    for (s = receiver->end_seq; s != seq; s++) {
      slot = rtp_receiver_slot(receiver, s);
      slot->seq = s;
      slot->state = SLOT_MISSING;
      slot->nacks = 0;
      slot->missing_ms = now;
      receiver->missing++;
    }
    receiver->end_seq = seq + 1;
    slot = rtp_receiver_slot(receiver, seq);
  } else {
    slot = rtp_receiver_slot(receiver, seq);
    if (slot->state != SLOT_MISSING || slot->seq != seq) {
      // duplicated
      return;
    }
    receiver->missing--;
  }

  slot->seq = seq;
  slot->state = SLOT_RECEIVED;
  slot->size = size;
  memcpy(slot->packet, packet, size);

  rtp_receiver_play(receiver);
}

void rtp_receiver_init(RtpReceiver* receiver, RtpDecoder* decoder, int buffered) {
  memset(receiver, 0, sizeof(*receiver));
  receiver->decoder = decoder;
  receiver->buffered = buffered && CONFIG_RTP_JITTER_BUFFER_SIZE > 0;
}

void rtp_receiver_deinit(RtpReceiver* receiver) {
  free(receiver->slots);
  receiver->slots = NULL;
}

void rtp_receiver_reset(RtpReceiver* receiver, uint32_t ssrc, uint32_t clock_rate) {
  RtpReceiverSlot* slots = receiver->slots;
  RtpDecoder* decoder = receiver->decoder;
  int buffered = receiver->buffered;
  int i;

  memset(receiver, 0, sizeof(*receiver));
  receiver->ssrc = ssrc;
  receiver->clock_rate = clock_rate;
  receiver->decoder = decoder;
  receiver->buffered = buffered;
  receiver->slots = slots;

  if (slots) {
    for (i = 0; i < CONFIG_RTP_JITTER_BUFFER_SIZE; i++) {
      slots[i].state = SLOT_EMPTY;
    }
  }
  if (decoder) {
    rtp_decoder_reset(decoder);
  }
}

void rtp_receiver_put(RtpReceiver* receiver, const uint8_t* packet, size_t size, uint32_t now) {
  const RtpHeader* header = (const RtpHeader*)packet;
  uint16_t seq, udelta;
  uint32_t arrival, transit, d;
  int in_order = 0;

  if (size < sizeof(RtpHeader) || size > CONFIG_MTU) {
    return;
  }

  seq = ntohs(header->seq_number);

  if (!receiver->started) {
    receiver->started = 1;
    receiver->base_seq = seq;
    receiver->max_seq = seq;
    receiver->next_seq = receiver->end_seq = seq;
    in_order = 1;
  } else {
    udelta = seq - receiver->max_seq;
    if (udelta > 0 && udelta < MAX_DROPOUT) {
      if (seq < receiver->max_seq) {
        receiver->cycles += 65536;
      }
      receiver->max_seq = seq;
      receiver->gaps += udelta - 1;
      in_order = 1;
    } else if (udelta >= MAX_DROPOUT && udelta <= 65536 - MAX_MISORDER) {
      // a very large jump, the sender restarted
      receiver->base_seq = seq;
      receiver->max_seq = seq;
      receiver->cycles = 0;
      receiver->received = 0;
      receiver->expected_prior = 0;
      receiver->received_prior = 0;
      receiver->network_expected_prior = 0;
      receiver->gaps_prior = receiver->gaps;
      in_order = 1;
    }
  }
  receiver->received++;
  receiver->octets += size;

  // interarrival jitter, retransmitted packets are not counted
  if (in_order && receiver->clock_rate) {
    arrival = (uint32_t)((uint64_t)now * receiver->clock_rate / 1000);
    transit = arrival - ntohl(header->timestamp);
    if (receiver->transit) {
      d = transit - receiver->transit;
      if ((int32_t)d < 0) {
        d = -d;
      }
      receiver->jitter += d - ((receiver->jitter + 8) >> 4);
    }
    receiver->transit = transit;
  }

  if (receiver->buffered && receiver->slots == NULL) {
    receiver->slots = calloc(CONFIG_RTP_JITTER_BUFFER_SIZE, sizeof(RtpReceiverSlot));
    if (receiver->slots == NULL) {
      LOGW("Failed to allocate %d slots for received packets", CONFIG_RTP_JITTER_BUFFER_SIZE);
      receiver->buffered = 0;
    }
  }

  if (receiver->slots) {
    rtp_receiver_buffer(receiver, packet, size, seq, now);
    return;
  }

  // decode right away, a packet out of order breaks the frame
  if (seq != receiver->next_seq && receiver->received > 1) {
    rtp_receiver_drop(receiver);
  }
  receiver->next_seq = seq + 1;
  rtp_receiver_decode(receiver, packet, size);
}

int rtp_receiver_poll(RtpReceiver* receiver, uint32_t now, uint32_t rtt_ms, uint16_t* seqs, int max) {
  RtpReceiverSlot* slot;
  uint32_t interval = rtt_ms > MIN_NACK_INTERVAL ? rtt_ms : MIN_NACK_INTERVAL;
  uint16_t s;
  int count = 0;

  if (receiver->slots == NULL) {
    return 0;
  }

  // give up the packets which did not come back in time
  while (receiver->missing > 0) {
    slot = rtp_receiver_slot(receiver, receiver->next_seq);
    if (slot->state != SLOT_MISSING || (uint32_t)(now - slot->missing_ms) < CONFIG_RTP_JITTER_BUFFER_DELAY) {
      break;
    }
    for (s = receiver->next_seq; s != receiver->end_seq; s++) {
      if (rtp_receiver_slot(receiver, s)->state != SLOT_MISSING) {
        break;
      }
    }
    LOGD("Gave up %d packets from seq=%u", (uint16_t)(s - receiver->next_seq), receiver->next_seq);
    rtp_receiver_advance(receiver, s);
    rtp_receiver_play(receiver);
  }

  if (receiver->missing == 0) {
    return 0;
  }

  for (s = receiver->next_seq; s != receiver->end_seq && count < max; s++) {
    slot = rtp_receiver_slot(receiver, s);
    if (slot->state != SLOT_MISSING || slot->nacks >= MAX_NACKS) {
      continue;
    }
    if (slot->nacks == 0 || (uint32_t)(now - slot->nack_ms) >= interval) {
      slot->nacks++;
      slot->nack_ms = now;
      seqs[count++] = s;
    }
  }
  receiver->nack_count += count;
  return count;
}

void rtp_receiver_on_sr(RtpReceiver* receiver, const RtcpSenderInfo* info, uint32_t now) {
  receiver->last_sr = (info->ntp_seconds << 16) | (info->ntp_fraction >> 16);
  receiver->last_sr_ms = now;
}

int rtp_receiver_get_report_block(RtpReceiver* receiver, uint32_t now, RtcpReportBlock* block) {
  uint32_t extended_max, expected, expected_interval, received_interval, fraction = 0, dlsr = 0;
  int32_t lost, lost_interval;

  if (!receiver->started || receiver->ssrc == 0) {
    return 0;
  }

  extended_max = receiver->cycles + receiver->max_seq;
  expected = extended_max - receiver->base_seq + 1;
  lost = (int32_t)(expected - receiver->received);
  // 24 bits signed integer
  if (lost > 0x7fffff) {
    lost = 0x7fffff;
  } else if (lost < -0x800000) {
    lost = -0x800000;
  }

  expected_interval = expected - receiver->expected_prior;
  received_interval = receiver->received - receiver->received_prior;
  receiver->expected_prior = expected;
  receiver->received_prior = receiver->received;
  lost_interval = (int32_t)(expected_interval - received_interval);
  if (expected_interval > 0 && lost_interval > 0) {
    fraction = ((uint32_t)lost_interval << 8) / expected_interval;
  }

  if (receiver->last_sr_ms) {
    // in units of 1/65536 seconds
    dlsr = (uint32_t)((uint64_t)(uint32_t)(now - receiver->last_sr_ms) * 65536 / 1000);
  }

  block->ssrc = htonl(receiver->ssrc);
  block->flcnpl = htonl((fraction << 24) | ((uint32_t)lost & 0xffffff));
  block->ehsnr = htonl(extended_max);
  block->jitter = htonl(receiver->jitter >> 4);
  block->lsr = htonl(receiver->last_sr);
  block->dlsr = htonl(dlsr);
  return 1;
}

float rtp_receiver_get_network_loss(RtpReceiver* receiver) {
  uint32_t expected = receiver->cycles + receiver->max_seq - receiver->base_seq + 1;
  uint32_t expected_interval = expected - receiver->network_expected_prior;
  uint32_t gaps_interval = receiver->gaps - receiver->gaps_prior;

  if (!receiver->started) {
    return 0;
  }

  receiver->network_expected_prior = expected;
  receiver->gaps_prior = receiver->gaps;
  if (expected_interval == 0 || gaps_interval > expected_interval) {
    return 0;
  }
  return (float)gaps_interval / expected_interval;
}
