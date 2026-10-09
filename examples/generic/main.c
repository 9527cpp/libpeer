#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "peer.h"
#include "reader.h"

#define MAX_PEERS 8
#define MAX_ICE_SERVERS (int)(sizeof(((PeerConfiguration*)0)->ice_servers) / sizeof(IceServer))
#define DEFAULT_ICE_SERVER "stun:stun.l.google.com:19302"

typedef struct Peer {
  int index;
  PeerConnection* pc;
  pthread_t thread;
  FILE* video_file;
  size_t video_bytes;
} Peer;

int g_interrupted = 0;
Peer g_peers[MAX_PEERS];
int g_peers_count = 1;
int g_record = 0;
IceServer g_ice_servers[MAX_ICE_SERVERS];
int g_ice_servers_count = -1;  // -1: not set on the command line

static void onconnectionstatechange(PeerConnectionState state, void* user_data) {
  Peer* peer = (Peer*)user_data;
  printf("[peer %d] state is changed: %s\n", peer->index, peer_connection_state_to_string(state));
}

static void onopen(void* user_data) {
}

static void onclose(void* user_data) {
}

static void onmessage(char* msg, size_t len, void* user_data, uint16_t sid) {
  Peer* peer = (Peer*)user_data;
  printf("[peer %d] on message: %d %.*s", peer->index, sid, (int)len, msg);

  if (strncmp(msg, "ping", 4) == 0) {
    printf(", send pong\n");
    peer_connection_datachannel_send(peer->pc, "pong", 4);
  }
}

// called with one Annex B NAL unit at a time
static void onvideotrack(uint8_t* data, size_t size, void* user_data) {
  Peer* peer = (Peer*)user_data;
  char path[32];

  if (!g_record) {
    return;
  }
  if (peer->video_file == NULL) {
    snprintf(path, sizeof(path), "recv_peer%d.h264", peer->index);
    if ((peer->video_file = fopen(path, "wb")) == NULL) {
      return;
    }
    printf("[peer %d] recording received video to %s\n", peer->index, path);
  }
  fwrite(data, 1, size, peer->video_file);
  peer->video_bytes += size;
}

static void onrequestkeyframe(void* user_data) {
  Peer* peer = (Peer*)user_data;
  printf("[peer %d] remote requests a key frame\n", peer->index);
}

static void onreceiverreport(const PeerReceiverReport* report, void* user_data) {
  Peer* peer = (Peer*)user_data;
  printf("[peer %d] %s report: lost %.1f%% (%d), jitter %ums, rtt %dms, bitrate %u, target %u bps\n",
         peer->index, report->is_video ? "video" : "audio", report->fraction_lost * 100, report->cumulative_lost,
         report->jitter_ms, report->rtt_ms, report->send_bitrate_bps, report->target_bitrate_bps);
}

static void signal_handler(int signal) {
  g_interrupted = 1;
}

static void* peer_singaling_task(void* data) {
  while (!g_interrupted) {
    peer_signaling_loop();
    usleep(1000);
  }

  pthread_exit(NULL);
}

// each peer connection runs in its own thread so that a DTLS handshake does not stall the others
static void* peer_connection_task(void* data) {
  Peer* peer = (Peer*)data;

  while (!g_interrupted) {
    peer_connection_loop(peer->pc);
    usleep(1000);
  }

  pthread_exit(NULL);
}

static uint64_t get_timestamp() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

void print_usage(const char* prog_name) {
  printf("Usage: %s -u <url> [-t <token>] [-n <max peers, 1 ~ %d>] [-s <ice server>]... [-r]\n", prog_name, MAX_PEERS);
  printf("  -u  signaling URL, mqtts://<host>[:<port>]/<path> (MQTT over TLS) or http(s)://... (WHIP)\n");
  printf("  -s  stun:<host>:<port> or turn:<host>:<port>,<username>,<credential>, repeat up to %d times,\n",
         MAX_ICE_SERVERS);
  printf("      \"none\" for host candidates only, default %s\n", DEFAULT_ICE_SERVER);
  printf("  -r  save the video received from peer N to recv_peerN.h264\n");
}

// "<url>[,<username>,<credential>]", the argument is split in place
static int parse_ice_server(char* arg, IceServer* server) {
  char* sep;

  memset(server, 0, sizeof(*server));
  server->urls = arg;
  if ((sep = strchr(arg, ',')) != NULL) {
    *sep = '\0';
    server->username = sep + 1;
    if ((sep = strchr(sep + 1, ',')) == NULL) {
      return -1;
    }
    *sep = '\0';
    server->credential = sep + 1;
  }

  if (strncmp(arg, "stun:", 5) == 0) {
    return 0;
  }
  if (strncmp(arg, "turn:", 5) == 0 && server->username != NULL) {
    return 0;
  }
  return -1;
}

void parse_arguments(int argc, char* argv[], const char** url, const char** token, int* count) {
  *token = NULL;
  *url = NULL;
  *count = 1;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-u") == 0 && (i + 1) < argc) {
      *url = argv[++i];
    } else if (strcmp(argv[i], "-t") == 0 && (i + 1) < argc) {
      *token = argv[++i];
    } else if (strcmp(argv[i], "-n") == 0 && (i + 1) < argc) {
      *count = atoi(argv[++i]);
    } else if (strcmp(argv[i], "-s") == 0 && (i + 1) < argc) {
      if (g_ice_servers_count < 0) {
        g_ice_servers_count = 0;
      }
      i++;
      if (strcmp(argv[i], "none") == 0) {
        continue;
      }
      if (g_ice_servers_count >= MAX_ICE_SERVERS || parse_ice_server(argv[i], &g_ice_servers[g_ice_servers_count]) != 0) {
        printf("Invalid or too many ICE servers: %s\n", argv[i]);
        print_usage(argv[0]);
        exit(1);
      }
      g_ice_servers_count++;
    } else if (strcmp(argv[i], "-r") == 0) {
      g_record = 1;
    } else {
      print_usage(argv[0]);
      exit(1);
    }
  }

  if (*url == NULL || *count < 1 || *count > MAX_PEERS) {
    print_usage(argv[0]);
    exit(1);
  }

  if (g_ice_servers_count < 0) {
    g_ice_servers[0].urls = DEFAULT_ICE_SERVER;
    g_ice_servers_count = 1;
  }
}

static int any_peer_connected() {
  for (int i = 0; i < g_peers_count; i++) {
    if (peer_connection_get_state(g_peers[i].pc) == PEER_CONNECTION_CONNECTED) {
      return 1;
    }
  }
  return 0;
}

int main(int argc, char* argv[]) {
  uint64_t curr_time, video_time = 0, audio_time = 0;
  uint8_t* buf = NULL;
  const char* url = NULL;
  const char* token = NULL;
  PeerConnection* pcs[MAX_PEERS];
  int size;
  int i;

  pthread_t peer_singaling_thread;

  parse_arguments(argc, argv, &url, &token, &g_peers_count);

  signal(SIGINT, signal_handler);

  PeerConfiguration config = {
      .datachannel = DATA_CHANNEL_STRING,
      .video_codec = CODEC_H264,
      .audio_codec = CODEC_PCMA,
      .onvideotrack = onvideotrack,
      .on_request_keyframe = onrequestkeyframe};
  memcpy(config.ice_servers, g_ice_servers, sizeof(g_ice_servers));

  printf("=========== Parsed Arguments ===========\n");
  printf(" %-5s : %s\n", "URL", url);
  printf(" %-5s : %s\n", "Token", token ? token : "");
  printf(" %-5s : %d\n", "Peers", g_peers_count);
  for (i = 0; i < g_ice_servers_count; i++) {
    printf(" %-5s : %s%s%s\n", "ICE", g_ice_servers[i].urls,
           g_ice_servers[i].username ? ", user " : "", g_ice_servers[i].username ? g_ice_servers[i].username : "");
  }
  if (g_ice_servers_count == 0) {
    printf(" %-5s : none\n", "ICE");
  }
  printf("========================================\n");

  peer_init();
  for (i = 0; i < g_peers_count; i++) {
    g_peers[i].index = i;
    config.user_data = &g_peers[i];
    g_peers[i].pc = pcs[i] = peer_connection_create(&config);
    peer_connection_oniceconnectionstatechange(pcs[i], onconnectionstatechange);
    peer_connection_ondatachannel(pcs[i], onmessage, onopen, onclose);
    peer_connection_on_receiver_report(pcs[i], onreceiverreport);
  }

  peer_signaling_connect_multi(url, token, pcs, g_peers_count);

  for (i = 0; i < g_peers_count; i++) {
    pthread_create(&g_peers[i].thread, NULL, peer_connection_task, &g_peers[i]);
  }
  pthread_create(&peer_singaling_thread, NULL, peer_singaling_task, NULL);

  reader_init();

  while (!g_interrupted) {
    if (any_peer_connected()) {
      curr_time = get_timestamp();

      // FPS 25, the same frame is sent to every connected peer
      if (curr_time - video_time > 40) {
        video_time = curr_time;
        if ((buf = reader_get_video_frame(&size)) != NULL) {
          for (i = 0; i < g_peers_count; i++) {
            peer_connection_send_video(g_peers[i].pc, buf, size);
          }
          // need to free the buffer
          free(buf);
          buf = NULL;
        }
      }

      if (curr_time - audio_time > 20) {
        if ((buf = reader_get_audio_frame(&size)) != NULL) {
          for (i = 0; i < g_peers_count; i++) {
            peer_connection_send_audio(g_peers[i].pc, buf, size);
          }
          buf = NULL;
        }
        audio_time = curr_time;
      }
    }
    usleep(1000);
  }

  pthread_join(peer_singaling_thread, NULL);
  for (i = 0; i < g_peers_count; i++) {
    pthread_join(g_peers[i].thread, NULL);
  }

  reader_deinit();

  peer_signaling_disconnect();
  for (i = 0; i < g_peers_count; i++) {
    peer_connection_destroy(g_peers[i].pc);
    if (g_peers[i].video_file) {
      fclose(g_peers[i].video_file);
    }
  }
  peer_deinit();

  return 0;
}
