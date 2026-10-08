# libpeer - Portable WebRTC Library for IoT/Embedded Device

![build](https://github.com/sepfy/pear/actions/workflows/build.yml/badge.svg)

libpeer is a WebRTC implementation written in C, developed with BSD socket. The library aims to integrate IoT/Embedded device video/audio streaming with WebRTC, such as ESP32 and Raspberry Pi

### Features

- Video/Audio Codec
  - H264
  - G.711 PCM (A-law)
  - G.711 PCM (µ-law)
  - OPUS
- DataChannel
- STUN/TURN
- IPV4/IPV6
- Signaling
  - [WHIP](https://www.ietf.org/archive/id/draft-ietf-wish-whip-01.html)
  - MQTT

### Dependencies

* [mbedtls](https://github.com/Mbed-TLS/mbedtls)
* [libsrtp](https://github.com/cisco/libsrtp)
* [usrsctp](https://github.com/sctplab/usrsctp)
* [cJSON](https://github.com/DaveGamble/cJSON.git)
* [coreHTTP](https://github.com/FreeRTOS/coreHTTP)
* [coreMQTT](https://github.com/FreeRTOS/coreMQTT)

### Getting Started with Generic Example
- Copy URL from the test [website](https://9527cpp.github.io/libpeer)
- Build and run the example
```bash
$ sudo apt -y install git cmake wget ffmpeg
$ git clone --recursive https://github.com/sepfy/libpeer
$ cd libpeer
$ cmake -S . -B build && cmake --build build
$ wget -O sample.mp4 \
    https://download.samplelib.com/mp4/sample-30s.mp4
$ ffmpeg -i sample.mp4 \
    -map 0:v:0 -vf fps=25 -c:v libx264 -profile:v baseline -pix_fmt yuv420p \
    -x264-params bframes=0:keyint=25:min-keyint=25:scenecut=0:repeat-headers=1 \
    -f h264 test.264 \
    -map 0:a:0 -ac 1 -ar 8000 -c:a pcm_alaw -f wav test.wav
$ ./build/examples/generic/sample -u <URL> [-n <max viewers>]
```
- Click Connect button on the website, up to `-n` browsers (default 1) can watch at the same time

### Examples for Platforms
- [ESP32](https://github.com/sepfy/libpeer/tree/main/examples/esp32): MJPEG over datachannel
- [PICO](https://github.com/sepfy/libpeer/tree/main/examples/pico): Ping pong with datachannel
- [Raspberry Pi](https://github.com/sepfy/libpeer/tree/main/examples/raspberrypi): Video and two-way audio stream
