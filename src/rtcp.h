#ifndef RTCP_H_
#define RTCP_H_

#include <stddef.h>
#include <stdint.h>

typedef enum RtcpType {

  RTCP_FIR = 192,
  RTCP_SR = 200,
  RTCP_RR = 201,
  RTCP_SDES = 202,
  RTCP_BYE = 203,
  RTCP_APP = 204,
  RTCP_RTPFB = 205,
  RTCP_PSFB = 206,
  RTCP_XR = 207,

} RtcpType;

// feedback message types (FMT) carried in the report count field
typedef enum RtcpFeedbackType {
  RTCP_RTPFB_NACK = 1,
  RTCP_PSFB_PLI = 1,
  RTCP_PSFB_FIR = 4,
  // application layer feedback, used by REMB
  RTCP_PSFB_AFB = 15,
} RtcpFeedbackType;

typedef struct RtcpHeader {
  uint8_t vprc; /* Version, Padding, Report count/Feedback message type */
  uint8_t type; /* Packet Type */
  uint16_t length;

} RtcpHeader;

static inline void rtcp_header_init(RtcpHeader* header, uint8_t type, uint8_t rc) {
  header->vprc = 0x80U | (rc & 0x1fU);
  header->type = type;
}

static inline uint8_t rtcp_header_version(const RtcpHeader* header) {
  return (header->vprc >> 6) & 0x03U;
}

static inline uint8_t rtcp_header_padding(const RtcpHeader* header) {
  return (header->vprc >> 5) & 0x01U;
}

static inline uint8_t rtcp_header_rc(const RtcpHeader* header) {
  return header->vprc & 0x1fU;
}

// all fields are in network byte order
typedef struct RtcpReportBlock {
  uint32_t ssrc;
  uint32_t flcnpl;
  uint32_t ehsnr;
  uint32_t jitter;
  uint32_t lsr;
  uint32_t dlsr;

} RtcpReportBlock;

typedef struct RtcpFir {
  uint32_t ssrc;
  uint32_t seqnr;

} RtcpFir;

typedef struct RtcpFb {
  RtcpHeader header;
  uint32_t ssrc;
  uint32_t media;
  char fci[1];

} RtcpFb;

// sender information of a SR, all fields are in host byte order
typedef struct RtcpSenderInfo {
  uint32_t ssrc;
  uint32_t ntp_seconds;
  uint32_t ntp_fraction;
  uint32_t rtp_timestamp;
  uint32_t packet_count;
  uint32_t octet_count;

} RtcpSenderInfo;

typedef void (*RtcpNackHandler)(uint16_t seq, void* user_data);

int rtcp_probe(uint8_t* packet, size_t size);

int rtcp_get_pli(uint8_t* packet, int len, uint32_t sender_ssrc, uint32_t media_ssrc);

int rtcp_get_fir(uint8_t* packet, int len, int* seqnr);

/**
 * @brief build a compound RTCP packet with a SR and a SDES CNAME
 * @return size of the packet, -1 if the buffer is too small
 */
int rtcp_get_sr(uint8_t* packet, int len, const RtcpSenderInfo* info, const char* cname);

/**
 * @brief locate the report blocks of a single SR or RR packet
 * @param[in] packet a single RTCP packet, not a compound one
 * @param[in] len length of the packet
 * @param[out] blocks first report block
 * @return number of report blocks
 */
int rtcp_get_report_blocks(uint8_t* packet, size_t len, RtcpReportBlock** blocks);

/**
 * @brief call handler for every sequence number requested by a generic NACK (RFC 4585 6.2.1)
 * @param[in] packet a single RTPFB packet with FMT 1
 * @return number of requested sequence numbers
 */
int rtcp_parse_nack(const uint8_t* packet, size_t len, RtcpNackHandler handler, void* user_data);

/**
 * @brief read the sender information of a single SR packet
 * @return 0 on success, -1 if it is not a valid SR
 */
int rtcp_get_sender_info(const uint8_t* packet, size_t len, RtcpSenderInfo* info);

/**
 * @brief build a RR packet
 * @param[in] blocks report blocks in network byte order
 * @return size of the packet, -1 if the buffer is too small
 */
int rtcp_get_rr(uint8_t* packet, int len, uint32_t sender_ssrc, const RtcpReportBlock* blocks, int count);

/**
 * @brief build a generic NACK (RFC 4585 6.2.1)
 * @param[in] seqs requested sequence numbers in ascending order
 * @return size of the packet, -1 if the buffer is too small
 */
int rtcp_get_nack(uint8_t* packet, int len, uint32_t sender_ssrc, uint32_t media_ssrc, const uint16_t* seqs, int count);

/**
 * @brief build a REMB (draft-alvestrand-rmcat-remb) telling the sender the maximum bitrate to use
 * @return size of the packet, -1 if the buffer is too small
 */
int rtcp_get_remb(uint8_t* packet, int len, uint32_t sender_ssrc, uint32_t bitrate_bps, const uint32_t* ssrcs, int count);

#endif  // RTCP_H_
