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

// interval of RTCP sender reports for each outgoing stream
#ifndef CONFIG_RTCP_SR_INTERVAL
#define CONFIG_RTCP_SR_INTERVAL 1000
#endif

// number of sent video packets kept for NACK retransmission, 0 to disable.
// each slot takes about CONFIG_MTU bytes, 128 slots cover ~1s of 1Mbps video.
#ifndef CONFIG_RTP_HISTORY_SIZE
#define CONFIG_RTP_HISTORY_SIZE 128
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
