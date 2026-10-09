#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "rtcp.h"
#include "rtp.h"

static uint16_t g_nack_seqs[32];
static int g_nack_count = 0;

static void on_nack(uint16_t seq, void* user_data) {
  g_nack_seqs[g_nack_count++] = seq;
}

static void test_pli() {
  uint8_t packet[12];
  uint8_t expected[12] = {0x81, 206, 0x00, 0x02, 0, 0, 0, 1, 0x12, 0x34, 0x56, 0x78};

  assert(rtcp_get_pli(packet, sizeof(packet), 1, 0x12345678) == 12);
  assert(memcmp(packet, expected, sizeof(expected)) == 0);
  assert(rtcp_get_pli(packet, 8, 1, 0x12345678) == -1);
}

static void test_sr() {
  uint8_t packet[128];
  RtcpSenderInfo info = {1, 0xe0000000, 0x80000000, 90000, 10, 12000};
  RtcpHeader* header;
  uint32_t* words;
  int len = rtcp_get_sr(packet, sizeof(packet), &info, "libpeer");

  // SR 28 bytes + SDES 8 bytes + CNAME item 2 + 7 + null terminator padded to 20 bytes
  assert(len == 48);
  header = (RtcpHeader*)packet;
  assert(header->vprc == 0x80 && header->type == RTCP_SR && ntohs(header->length) == 6);
  words = (uint32_t*)(packet + 4);
  assert(ntohl(words[0]) == 1 && ntohl(words[1]) == 0xe0000000 && ntohl(words[3]) == 90000);
  assert(ntohl(words[4]) == 10 && ntohl(words[5]) == 12000);

  header = (RtcpHeader*)(packet + 28);
  assert(header->vprc == 0x81 && header->type == RTCP_SDES && ntohs(header->length) == 4);
  assert(packet[36] == 1 && packet[37] == 7 && memcmp(packet + 38, "libpeer", 7) == 0);
  assert(packet[45] == 0);

  assert(rtcp_get_sr(packet, 40, &info, "libpeer") == -1);
}

static void test_report_blocks() {
  uint8_t packet[32] = {0x81, RTCP_RR, 0x00, 0x07, 0, 0, 0, 2};
  RtcpReportBlock* blocks;

  // ssrc 1, 25% lost, 3 packets lost in total
  packet[11] = 1;
  packet[12] = 64;
  packet[15] = 3;
  assert(rtcp_get_report_blocks(packet, sizeof(packet), &blocks) == 1);
  assert(ntohl(blocks[0].ssrc) == 1);
  assert(ntohl(blocks[0].flcnpl) >> 24 == 64 && (ntohl(blocks[0].flcnpl) & 0xffffff) == 3);

  // truncated
  assert(rtcp_get_report_blocks(packet, 20, &blocks) == 0);
}

static void test_nack() {
  // PID 100, BLP lost 101 and 116
  uint8_t packet[16] = {0x81, RTCP_RTPFB, 0x00, 0x03, 0, 0, 0, 2, 0, 0, 0, 1, 0, 100, 0x80, 0x01};

  assert(rtcp_parse_nack(packet, sizeof(packet), on_nack, NULL) == 3);
  assert(g_nack_seqs[0] == 100 && g_nack_seqs[1] == 101 && g_nack_seqs[2] == 116);

  // wrap around
  g_nack_count = 0;
  packet[12] = 0xff;
  packet[13] = 0xff;
  packet[14] = 0;
  packet[15] = 1;
  assert(rtcp_parse_nack(packet, sizeof(packet), on_nack, NULL) == 2);
  assert(g_nack_seqs[0] == 65535 && g_nack_seqs[1] == 0);
}

static void test_history() {
  RtpHistory* history = rtp_history_create(4);
  uint8_t packet[CONFIG_MTU + 32];
  uint8_t buf[CONFIG_MTU + 32];

  memset(packet, 0xab, sizeof(packet));
  rtp_history_put(history, 10, packet, 100);
  assert(rtp_history_get(history, 10, buf, sizeof(buf)) == 100);
  assert(memcmp(buf, packet, 100) == 0);
  assert(rtp_history_get(history, 11, buf, sizeof(buf)) == -1);
  assert(rtp_history_get(history, 10, buf, 50) == -1);

  // overwritten by seq 14 in the same slot
  rtp_history_put(history, 14, packet, 200);
  assert(rtp_history_get(history, 10, buf, sizeof(buf)) == -1);
  assert(rtp_history_get(history, 14, buf, sizeof(buf)) == 200);

  // too large
  rtp_history_put(history, 15, packet, sizeof(packet) + 1);
  assert(rtp_history_get(history, 15, buf, sizeof(buf)) == -1);

  rtp_history_destroy(history);
}

static void test_build_nack() {
  uint8_t packet[64];
  uint16_t seqs[] = {100, 101, 116, 117, 65535, 0};
  int len;

  // 100 with 101 and 116 in its BLP, then 117 and 65535 with 0 across the wrap around
  len = rtcp_get_nack(packet, sizeof(packet), 1, 2, seqs, 6);
  assert(len == 24);
  assert(packet[0] == 0x81 && packet[1] == RTCP_RTPFB && ntohs(*(uint16_t*)(packet + 2)) == 5);
  assert(ntohl(*(uint32_t*)(packet + 4)) == 1 && ntohl(*(uint32_t*)(packet + 8)) == 2);

  g_nack_count = 0;
  assert(rtcp_parse_nack(packet, len, on_nack, NULL) == 6);
  assert(g_nack_seqs[0] == 100 && g_nack_seqs[1] == 101 && g_nack_seqs[2] == 116);
  assert(g_nack_seqs[3] == 117 && g_nack_seqs[4] == 65535 && g_nack_seqs[5] == 0);

  // only the FCIs fitting in the buffer
  assert(rtcp_get_nack(packet, 16, 1, 2, seqs, 6) == 16);
}

static void test_rr() {
  uint8_t packet[64];
  RtcpReportBlock blocks[2] = {{htonl(3)}, {htonl(4)}};
  RtcpReportBlock* parsed;

  assert(rtcp_get_rr(packet, sizeof(packet), 1, blocks, 2) == 56);
  assert(packet[0] == 0x82 && packet[1] == RTCP_RR && ntohs(*(uint16_t*)(packet + 2)) == 13);
  assert(rtcp_get_report_blocks(packet, 56, &parsed) == 2);
  assert(ntohl(parsed[0].ssrc) == 3 && ntohl(parsed[1].ssrc) == 4);
  assert(rtcp_get_rr(packet, 40, 1, blocks, 2) == -1);
}

static void test_remb() {
  uint8_t packet[32];
  uint32_t ssrc = 0x12345678;
  uint32_t mantissa;

  assert(rtcp_get_remb(packet, sizeof(packet), 1, 2500000, &ssrc, 1) == 24);
  assert(packet[0] == 0x8f && packet[1] == RTCP_PSFB && ntohs(*(uint16_t*)(packet + 2)) == 5);
  assert(memcmp(packet + 12, "REMB", 4) == 0 && packet[16] == 1);
  // 2500000 = 156250 * 2^4
  mantissa = ((packet[17] & 0x03) << 16) | (packet[18] << 8) | packet[19];
  assert((packet[17] >> 2) == 4 && mantissa == 156250);
  assert(ntohl(*(uint32_t*)(packet + 20)) == ssrc);
}

static void test_sender_info() {
  uint8_t packet[128];
  RtcpSenderInfo info = {1, 0xe0000000, 0x80000000, 90000, 10, 12000};
  RtcpSenderInfo parsed;
  int len = rtcp_get_sr(packet, sizeof(packet), &info, "libpeer");

  assert(rtcp_get_sender_info(packet, len, &parsed) == 0);
  assert(memcmp(&parsed, &info, sizeof(info)) == 0);
  assert(rtcp_get_sender_info(packet, 20, &parsed) == -1);
}

int main(int argc, char* argv[]) {
  test_pli();
  test_sr();
  test_report_blocks();
  test_nack();
  test_history();
  test_build_nack();
  test_rr();
  test_remb();
  test_sender_info();
  printf("All RTCP tests passed\n");
  return 0;
}
