#ifndef CONFIG_H_
#define CONFIG_H_

// uncomment this if you want to handshake with a aiortc
// #define CONFIG_DTLS_USE_ECDSA 1

#define SCTP_MTU (1200)
#define CONFIG_MTU (1300)

#ifndef CONFIG_USE_ZEPHYR
#ifdef __ZEPHYR__
#define CONFIG_USE_ZEPHYR 1
#else
#define CONFIG_USE_ZEPHYR 0
#endif
#endif

#ifndef CONFIG_USE_LWIP
#define CONFIG_USE_LWIP 0
#endif

#ifndef CONFIG_MBEDTLS_DEBUG
#define CONFIG_MBEDTLS_DEBUG 0
#endif

#ifndef CONFIG_MBEDTLS_2_X
#define CONFIG_MBEDTLS_2_X 0
#endif

#if CONFIG_MBEDTLS_2_X
#define RSA_KEY_LENGTH 512
#else
#define RSA_KEY_LENGTH 1024
#endif

#ifndef CONFIG_DTLS_USE_ECDSA
#define CONFIG_DTLS_USE_ECDSA 0
#endif

#ifndef CONFIG_USE_USRSCTP
#define CONFIG_USE_USRSCTP 1
#endif

#ifndef CONFIG_SDP_BUFFER_SIZE
#define CONFIG_SDP_BUFFER_SIZE 8096
#endif

#ifndef CONFIG_MQTT_BUFFER_SIZE
#define CONFIG_MQTT_BUFFER_SIZE 4096
#endif

#ifndef CONFIG_HTTP_BUFFER_SIZE
#define CONFIG_HTTP_BUFFER_SIZE 4096
#endif

#ifndef CONFIG_TLS_READ_TIMEOUT
#define CONFIG_TLS_READ_TIMEOUT 3000
#endif

#ifndef CONFIG_STUN_KEEPALIVE_INTERVAL
#define CONFIG_STUN_KEEPALIVE_INTERVAL 0
#endif

#ifndef CONFIG_STUN_KEEPALIVE_TIMEOUT
#define CONFIG_STUN_KEEPALIVE_TIMEOUT 15000
#endif

#if CONFIG_STUN_KEEPALIVE_INTERVAL > 0 && \
    CONFIG_STUN_KEEPALIVE_TIMEOUT <= CONFIG_STUN_KEEPALIVE_INTERVAL
#error "CONFIG_STUN_KEEPALIVE_TIMEOUT must be greater than CONFIG_STUN_KEEPALIVE_INTERVAL"
#endif

#ifndef CONFIG_AUDIO_DURATION
#define CONFIG_AUDIO_DURATION 20
#endif

// a connected peer is considered gone when nothing is received from it for this long, 0 to disable.
// browsers send ICE consent checks every ~5s even when no media flows.
#ifndef CONFIG_PEER_CONNECTION_IDLE_TIMEOUT
#define CONFIG_PEER_CONNECTION_IDLE_TIMEOUT 15000
#endif

// maximum number of peer connections served by the signaling at the same time
#ifndef CONFIG_SIGNALING_MAX_SESSIONS
#define CONFIG_SIGNALING_MAX_SESSIONS 8
#endif

// a session that requested an offer but did not complete the negotiation is released after this time
#ifndef CONFIG_SIGNALING_SESSION_TIMEOUT
#define CONFIG_SIGNALING_SESSION_TIMEOUT 30000
#endif

// interval of RTCP sender reports for each outgoing stream
#ifndef CONFIG_RTCP_SR_INTERVAL
#define CONFIG_RTCP_SR_INTERVAL 1000
#endif

// number of sent video packets kept for NACK retransmission, 0 to disable.
// each slot takes about CONFIG_MTU bytes, 128 slots cover ~1s of 1Mbps video.
#ifndef CONFIG_RTP_HISTORY_SIZE
#define CONFIG_RTP_HISTORY_SIZE 128
#endif

// number of received video packets held to put them back in order and wait for the
// retransmission of lost ones, a power of 2, 0 to disable NACK. Each slot takes about
// CONFIG_MTU bytes and they are allocated when the remote peer starts sending video.
#ifndef CONFIG_RTP_JITTER_BUFFER_SIZE
#define CONFIG_RTP_JITTER_BUFFER_SIZE 128
#endif

// how long received video waits for a lost packet before giving it up and requesting a key frame
#ifndef CONFIG_RTP_JITTER_BUFFER_DELAY
#define CONFIG_RTP_JITTER_BUFFER_DELAY 200
#endif

// the highest bitrate announced to the remote peer by REMB for the video it sends
#ifndef CONFIG_REMB_MAX_BITRATE
#define CONFIG_REMB_MAX_BITRATE 2500000
#endif

#ifndef CONFIG_MAX_NALU_SIZE
#define CONFIG_MAX_NALU_SIZE (100 * 1024)  // 100KB
#endif

#define CONFIG_IPV6 0
// empty will use first active interface
#define CONFIG_IFACE_PREFIX ""

// #define LOG_LEVEL LEVEL_DEBUG
#ifndef LOG_REDIRECT
#define LOG_REDIRECT 0
#endif

// Disable MQTT and HTTP signaling
// #define DISABLE_PEER_SIGNALING 1

#endif  // CONFIG_H_
