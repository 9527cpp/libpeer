#ifndef RTP_RECEIVER_H_
#define RTP_RECEIVER_H_

#include <stddef.h>
#include <stdint.h>

#include "rtcp.h"
#include "rtp.h"

typedef struct RtpReceiverSlot RtpReceiverSlot;

// Receiving side of a remote stream. It keeps the statistics of receiver reports and,
// when buffered, puts packets back in sequence number order and finds the lost ones
// to request again. Only used by the thread running peer_connection_loop.
typedef struct RtpReceiver {
  uint32_t ssrc;
  uint32_t clock_rate;
  RtpDecoder* decoder;
  int buffered;

  // statistics of RFC 3550 A.1, A.3 and A.8
  int started;
  uint16_t max_seq;
  uint32_t cycles;
  uint32_t base_seq;
  uint32_t received;
  uint32_t expected_prior;
  uint32_t received_prior;
  uint32_t transit;
  uint32_t jitter;  // scaled by 16
  uint32_t last_sr;  // middle 32 bits of the NTP timestamp of the last SR
  uint32_t last_sr_ms;
  uint32_t octets;
  // packets found missing when a later one arrived, whether retransmitted afterwards or not
  uint32_t gaps;
  uint32_t gaps_prior;
  uint32_t network_expected_prior;

  // packets in [next_seq, end_seq) are held in slots until the missing ones arrive or time out
  RtpReceiverSlot* slots;
  uint16_t next_seq;
  uint16_t end_seq;
  int missing;
  uint32_t nack_count;
  uint32_t lost_count;
} RtpReceiver;

void rtp_receiver_init(RtpReceiver* receiver, RtpDecoder* decoder, int buffered);

void rtp_receiver_deinit(RtpReceiver* receiver);

/**
 * @brief start receiving a new stream
 * @param[in] ssrc SSRC of the remote stream, 0 if the remote peer does not send it
 */
void rtp_receiver_reset(RtpReceiver* receiver, uint32_t ssrc, uint32_t clock_rate);

/**
 * @brief handle a decrypted RTP packet of this stream
 */
void rtp_receiver_put(RtpReceiver* receiver, const uint8_t* packet, size_t size, uint32_t now);

/**
 * @brief give up the packets missing for too long and collect those to request again
 * @param[out] seqs sequence numbers to NACK, in ascending order
 * @return number of sequence numbers
 */
int rtp_receiver_poll(RtpReceiver* receiver, uint32_t now, uint32_t rtt_ms, uint16_t* seqs, int max);

void rtp_receiver_on_sr(RtpReceiver* receiver, const RtcpSenderInfo* info, uint32_t now);

/**
 * @brief fill a report block covering the packets received since the last one
 * @return 1 if the block is filled, 0 if nothing has been received yet
 */
int rtp_receiver_get_report_block(RtpReceiver* receiver, uint32_t now, RtcpReportBlock* block);

/**
 * @brief fraction of packets lost by the network since the last call. Unlike the fraction lost
 * of report blocks, packets recovered by retransmission are counted as lost.
 */
float rtp_receiver_get_network_loss(RtpReceiver* receiver);

#endif  // RTP_RECEIVER_H_
