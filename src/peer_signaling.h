#ifndef PEER_SIGNALING_H_
#define PEER_SIGNALING_H_

#include "peer_connection.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DISABLE_PEER_SIGNALING

int peer_signaling_connect(const char* url, const char* token, PeerConnection* pc);

/**
 * @brief serve multiple remote peers, each of them gets one of the given peer connections.
 * With MQTT, a remote peer picks a random session id and talks on "<path>/<session id>/invoke"
 * and "<path>/<session id>/result". "<path>/invoke" and "<path>/result" are served as one more
 * session for clients without session support. A peer connection is reused once it is
 * disconnected, closed or failed. HTTP signaling only uses the first peer connection.
 * @param[in] signaling URL
 * @param[in] token for authentication
 * @param[in] peer connections, owned by the caller
 * @param[in] number of peer connections, at most CONFIG_SIGNALING_MAX_SESSIONS
 */
int peer_signaling_connect_multi(const char* url, const char* token, PeerConnection** pcs, int count);

void peer_signaling_disconnect();

int peer_signaling_loop();

#endif  // DISABLE_PEER_SIGNALING

#ifdef __cplusplus
}
#endif

#endif  // PEER_SIGNALING_H_
