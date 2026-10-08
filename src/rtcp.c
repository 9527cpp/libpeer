#include <stdio.h>
#include <string.h>

#include "address.h"
#include "rtcp.h"
#include "rtp.h"

#define RTCP_SDES_CNAME 1

int rtcp_probe(uint8_t* packet, size_t size) {
  if (size < 8)
    return -1;

  RtpHeader* header = (RtpHeader*)packet;
  uint8_t type = rtp_header_type(header);
  return ((type >= 64) && (type < 96));
}

int rtcp_get_pli(uint8_t* packet, int len, uint32_t sender_ssrc, uint32_t media_ssrc) {
  RtcpFb* pli = (RtcpFb*)packet;

  if (packet == NULL || len < 12)
    return -1;

  memset(packet, 0, 12);
  rtcp_header_init(&pli->header, RTCP_PSFB, RTCP_PSFB_PLI);
  pli->header.length = htons((12 / 4) - 1);
  pli->ssrc = htonl(sender_ssrc);
  pli->media = htonl(media_ssrc);

  return 12;
}

int rtcp_get_fir(uint8_t* packet, int len, int* seqnr) {
  if (packet == NULL || len != 20 || seqnr == NULL)
    return -1;

  memset(packet, 0, len);
  RtcpHeader* rtcp = (RtcpHeader*)packet;
  *seqnr = *seqnr + 1;
  if (*seqnr < 0 || *seqnr >= 256)
    *seqnr = 0;

  rtcp_header_init(rtcp, RTCP_PSFB, RTCP_PSFB_FIR);
  rtcp->length = htons((len / 4) - 1);
  RtcpFb* rtcp_fb = (RtcpFb*)rtcp;
  RtcpFir* fir = (RtcpFir*)rtcp_fb->fci;
  fir->seqnr = htonl(*seqnr << 24);

  return 20;
}

int rtcp_get_sr(uint8_t* packet, int len, const RtcpSenderInfo* info, const char* cname) {
  RtcpHeader* header;
  uint32_t* words;
  uint8_t* sdes;
  size_t cname_len = strlen(cname);
  // SR: header + sender SSRC + 5 words of sender info, without report blocks
  int sr_size = 28;
  // SDES: header + SSRC + CNAME item (type, length, text) + null terminator, padded to 32 bits
  int sdes_size = (8 + 2 + cname_len + 1 + 3) & ~3;

  if (packet == NULL || cname_len > 255 || len < sr_size + sdes_size)
    return -1;

  memset(packet, 0, sr_size + sdes_size);

  header = (RtcpHeader*)packet;
  rtcp_header_init(header, RTCP_SR, 0);
  header->length = htons(sr_size / 4 - 1);
  words = (uint32_t*)(packet + sizeof(RtcpHeader));
  words[0] = htonl(info->ssrc);
  words[1] = htonl(info->ntp_seconds);
  words[2] = htonl(info->ntp_fraction);
  words[3] = htonl(info->rtp_timestamp);
  words[4] = htonl(info->packet_count);
  words[5] = htonl(info->octet_count);

  header = (RtcpHeader*)(packet + sr_size);
  rtcp_header_init(header, RTCP_SDES, 1);
  header->length = htons(sdes_size / 4 - 1);
  sdes = packet + sr_size + sizeof(RtcpHeader);
  *(uint32_t*)sdes = htonl(info->ssrc);
  sdes[4] = RTCP_SDES_CNAME;
  sdes[5] = (uint8_t)cname_len;
  memcpy(sdes + 6, cname, cname_len);

  return sr_size + sdes_size;
}

int rtcp_get_report_blocks(uint8_t* packet, size_t len, RtcpReportBlock** blocks) {
  RtcpHeader* header = (RtcpHeader*)packet;
  size_t offset;
  int count;

  if (len < sizeof(RtcpHeader)) {
    return 0;
  }

  switch (header->type) {
    case RTCP_SR:
      offset = 28;
      break;
    case RTCP_RR:
      offset = 8;
      break;
    default:
      return 0;
  }

  count = rtcp_header_rc(header);
  if (offset + count * sizeof(RtcpReportBlock) > len) {
    return 0;
  }

  *blocks = (RtcpReportBlock*)(packet + offset);
  return count;
}

int rtcp_parse_nack(const uint8_t* packet, size_t len, RtcpNackHandler handler, void* user_data) {
  const uint8_t* fci;
  uint16_t pid, blp;
  int i, count = 0;

  // header + sender SSRC + media SSRC, followed by FCIs of PID (16 bits) + BLP (16 bits)
  for (fci = packet + 12; fci + 4 <= packet + len; fci += 4) {
    pid = (fci[0] << 8) | fci[1];
    blp = (fci[2] << 8) | fci[3];
    handler(pid, user_data);
    count++;
    for (i = 0; i < 16; i++) {
      if (blp & (1 << i)) {
        handler((uint16_t)(pid + i + 1), user_data);
        count++;
      }
    }
  }
  return count;
}
