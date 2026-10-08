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

int main(int argc, char* argv[]) {
  test_pli();
  test_sr();
  test_report_blocks();
  test_nack();
  test_history();
  printf("All RTCP tests passed\n");
  return 0;
}
