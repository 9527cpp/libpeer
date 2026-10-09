#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "rtp.h"
#include "rtp_receiver.h"

#define NALU_SPS 0x67
#define NALU_PPS 0x68
#define NALU_IDR 0x65
#define NALU_P 0x41

static uint8_t g_nalus[16][64];
static size_t g_nalu_sizes[16];
static int g_nalu_count = 0;

static void on_nalu(uint8_t* data, size_t size, void* user_data) {
  assert(g_nalu_count < 16 && size <= sizeof(g_nalus[0]));
  memcpy(g_nalus[g_nalu_count], data, size);
  g_nalu_sizes[g_nalu_count++] = size;
}

static size_t make_packet(uint8_t* packet, uint16_t seq, const uint8_t* payload, size_t size) {
  RtpHeader* header = (RtpHeader*)packet;

  memset(header, 0, sizeof(*header));
  rtp_header_init(header, PT_H264);
  header->seq_number = htons(seq);
  header->timestamp = htonl(seq * 3000);
  header->ssrc = htonl(1234);
  memcpy(packet + sizeof(RtpHeader), payload, size);
  return sizeof(RtpHeader) + size;
}

static void put(RtpReceiver* receiver, uint16_t seq, uint8_t nalu_type, uint32_t now) {
  uint8_t packet[64];
  uint8_t payload[2] = {nalu_type, (uint8_t)seq};

  rtp_receiver_put(receiver, packet, make_packet(packet, seq, payload, sizeof(payload)), now);
}

// the second byte of every NAL unit is the sequence number of its packet
static int delivered(int index, uint16_t seq) {
  return g_nalu_sizes[index] == 6 && g_nalus[index][5] == (uint8_t)seq;
}

static void setup(RtpReceiver* receiver, RtpDecoder* decoder, int buffered) {
  g_nalu_count = 0;
  rtp_decoder_init(decoder, CODEC_H264, on_nalu, NULL);
  rtp_receiver_init(receiver, decoder, buffered);
  rtp_receiver_reset(receiver, 1234, 90000);
}

static void teardown(RtpReceiver* receiver, RtpDecoder* decoder) {
  rtp_receiver_deinit(receiver);
  rtp_decoder_deinit(decoder);
}

static void test_payload() {
  uint8_t packet[64] = {0};
  const uint8_t* payload;

  // CSRC count 1, extension of one word, 3 bytes of padding
  packet[0] = 0x80 | 0x20 | 0x10 | 0x01;
  packet[16 + 2] = 0;
  packet[16 + 3] = 1;
  packet[40 - 1] = 3;
  assert(rtp_get_payload(packet, 40, &payload) == 40 - 12 - 4 - 8 - 3);
  assert(payload == packet + 24);

  // padding longer than the packet
  packet[40 - 1] = 30;
  assert(rtp_get_payload(packet, 40, &payload) == -1);
  // truncated extension
  assert(rtp_get_payload(packet, 18, &payload) == -1);
}

static void test_wait_keyframe() {
  RtpDecoder decoder;
  RtpReceiver receiver;

  setup(&receiver, &decoder, 1);
  put(&receiver, 1, NALU_P, 0);
  put(&receiver, 2, NALU_SPS, 0);
  put(&receiver, 3, NALU_PPS, 0);
  put(&receiver, 4, NALU_IDR, 0);
  assert(g_nalu_count == 3 && delivered(0, 2) && delivered(1, 3) && delivered(2, 4));
  assert(memcmp(g_nalus[0], "\x00\x00\x00\x01\x67", 5) == 0);
  teardown(&receiver, &decoder);
}

static void test_reorder_and_nack() {
  RtpDecoder decoder;
  RtpReceiver receiver;
  uint16_t seqs[8];

  setup(&receiver, &decoder, 1);
  put(&receiver, 65534, NALU_SPS, 0);
  put(&receiver, 0, NALU_P, 10);
  put(&receiver, 2, NALU_P, 10);
  // 65535 and 1 are held back
  assert(g_nalu_count == 1);

  assert(rtp_receiver_poll(&receiver, 10, 50, seqs, 8) == 2);
  assert(seqs[0] == 65535 && seqs[1] == 1);
  // not again before a round trip
  assert(rtp_receiver_poll(&receiver, 40, 50, seqs, 8) == 0);
  assert(rtp_receiver_poll(&receiver, 60, 50, seqs, 8) == 2);

  put(&receiver, 65535, NALU_P, 70);
  assert(g_nalu_count == 3 && delivered(1, 65535) && delivered(2, 0));
  put(&receiver, 1, NALU_P, 80);
  assert(g_nalu_count == 5 && delivered(3, 1) && delivered(4, 2));
  // duplicated
  put(&receiver, 1, NALU_P, 90);
  assert(g_nalu_count == 5);
  assert(rtp_receiver_poll(&receiver, 100, 50, seqs, 8) == 0);
  assert(!decoder.wait_keyframe);
  teardown(&receiver, &decoder);
}

static void test_give_up() {
  RtpDecoder decoder;
  RtpReceiver receiver;
  uint16_t seqs[8];

  setup(&receiver, &decoder, 1);
  put(&receiver, 10, NALU_SPS, 0);
  put(&receiver, 12, NALU_P, 0);
  assert(rtp_receiver_poll(&receiver, CONFIG_RTP_JITTER_BUFFER_DELAY - 1, 50, seqs, 8) == 1);
  assert(g_nalu_count == 1);

  // 12 can not be decoded without 11
  rtp_receiver_poll(&receiver, CONFIG_RTP_JITTER_BUFFER_DELAY, 50, seqs, 8);
  assert(g_nalu_count == 1 && decoder.wait_keyframe && receiver.lost_count == 1);
  // too late
  put(&receiver, 11, NALU_P, CONFIG_RTP_JITTER_BUFFER_DELAY + 10);
  put(&receiver, 13, NALU_P, CONFIG_RTP_JITTER_BUFFER_DELAY + 10);
  assert(g_nalu_count == 1);
  put(&receiver, 14, NALU_SPS, CONFIG_RTP_JITTER_BUFFER_DELAY + 20);
  assert(g_nalu_count == 2 && delivered(1, 14) && !decoder.wait_keyframe);
  teardown(&receiver, &decoder);
}

static void test_overflow() {
  RtpDecoder decoder;
  RtpReceiver receiver;
  uint16_t seq;

  setup(&receiver, &decoder, 1);
  put(&receiver, 0, NALU_SPS, 0);
  // 1 never comes, the buffer is full when CONFIG_RTP_JITTER_BUFFER_SIZE + 1 arrives
  for (seq = 2; seq <= CONFIG_RTP_JITTER_BUFFER_SIZE + 1; seq++) {
    put(&receiver, seq, NALU_P, 0);
  }
  assert(g_nalu_count == 1 && decoder.wait_keyframe);
  put(&receiver, CONFIG_RTP_JITTER_BUFFER_SIZE + 2, NALU_SPS, 0);
  assert(g_nalu_count == 2 && delivered(1, CONFIG_RTP_JITTER_BUFFER_SIZE + 2));
  teardown(&receiver, &decoder);
}

static void test_fu_a() {
  RtpDecoder decoder;
  RtpReceiver receiver;
  uint8_t packet[64];
  uint8_t start[] = {0x7c, 0x85, 1, 2};
  uint8_t middle[] = {0x7c, 0x05, 3};
  uint8_t end[] = {0x7c, 0x45, 4};

  setup(&receiver, &decoder, 0);
  rtp_receiver_put(&receiver, packet, make_packet(packet, 1, start, sizeof(start)), 0);
  rtp_receiver_put(&receiver, packet, make_packet(packet, 2, middle, sizeof(middle)), 0);
  rtp_receiver_put(&receiver, packet, make_packet(packet, 3, end, sizeof(end)), 0);
  assert(g_nalu_count == 1 && g_nalu_sizes[0] == 9);
  assert(memcmp(g_nalus[0], "\x00\x00\x00\x01\x65\x01\x02\x03\x04", 9) == 0);

  // the first fragment is lost
  rtp_receiver_put(&receiver, packet, make_packet(packet, 5, middle, sizeof(middle)), 0);
  rtp_receiver_put(&receiver, packet, make_packet(packet, 6, end, sizeof(end)), 0);
  assert(g_nalu_count == 1 && decoder.wait_keyframe);
  teardown(&receiver, &decoder);
}

static void test_stap_a() {
  RtpDecoder decoder;
  RtpReceiver receiver;
  uint8_t packet[64];
  uint8_t stap_a[] = {0x78, 0, 2, NALU_SPS, 1, 0, 2, NALU_PPS, 2};
  uint8_t invalid[] = {0x78, 0, 20, NALU_SPS, 1};

  setup(&receiver, &decoder, 0);
  rtp_receiver_put(&receiver, packet, make_packet(packet, 1, stap_a, sizeof(stap_a)), 0);
  assert(g_nalu_count == 2 && g_nalu_sizes[0] == 6 && g_nalu_sizes[1] == 6);
  assert(g_nalus[0][4] == NALU_SPS && g_nalus[1][4] == NALU_PPS);
  rtp_receiver_put(&receiver, packet, make_packet(packet, 2, invalid, sizeof(invalid)), 0);
  assert(g_nalu_count == 2);
  teardown(&receiver, &decoder);
}

static void test_report_block() {
  RtpDecoder decoder;
  RtpReceiver receiver;
  RtcpReportBlock block;
  RtcpSenderInfo info = {1234, 0x00010002, 0x00030000, 0, 0, 0};
  uint16_t seq;

  setup(&receiver, &decoder, 0);
  assert(rtp_receiver_get_report_block(&receiver, 0, &block) == 0);

  // 10 of 12 packets across the wrap around
  for (seq = 65530; seq != 6; seq++) {
    if (seq != 65535 && seq != 3) {
      put(&receiver, seq, NALU_P, 0);
    }
  }
  rtp_receiver_on_sr(&receiver, &info, 1000);
  assert(rtp_receiver_get_report_block(&receiver, 1500, &block) == 1);
  assert(ntohl(block.ssrc) == 1234);
  assert((ntohl(block.flcnpl) & 0xffffff) == 2 && (ntohl(block.flcnpl) >> 24) == 2 * 256 / 12);
  assert(ntohl(block.ehsnr) == 65536 + 5);
  assert(ntohl(block.lsr) == 0x00020003 && ntohl(block.dlsr) == 32768);
  assert(rtp_receiver_get_network_loss(&receiver) == 2.0f / 12);

  // nothing lost since the last report
  put(&receiver, 6, NALU_P, 0);
  assert(rtp_receiver_get_report_block(&receiver, 2000, &block) == 1);
  assert((ntohl(block.flcnpl) >> 24) == 0 && (ntohl(block.flcnpl) & 0xffffff) == 2);
  assert(rtp_receiver_get_network_loss(&receiver) == 0);

  // a retransmission recovers the packet in report blocks only
  put(&receiver, 8, NALU_P, 0);
  put(&receiver, 7, NALU_P, 0);
  assert(rtp_receiver_get_report_block(&receiver, 2500, &block) == 1);
  assert((ntohl(block.flcnpl) >> 24) == 0);
  assert(rtp_receiver_get_network_loss(&receiver) == 0.5f);
  teardown(&receiver, &decoder);
}

int main(int argc, char* argv[]) {
  test_payload();
  test_wait_keyframe();
  test_reorder_and_nack();
  test_give_up();
  test_overflow();
  test_fu_a();
  test_stap_a();
  test_report_block();
  printf("All RTP receiver tests passed\n");
  return 0;
}
