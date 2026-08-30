/*
 * VBAN output driver. This file is part of Shairport Sync.
 *
 * Copyright (c) Mike Brady 2014--2025
 * Copyright (c) DOCa Cola 2026
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "audio.h"
#include "common.h"
#include "config.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef CONFIG_FOR_MINGW
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#endif

#define VBAN_DEFAULT_PORT 6980
#define VBAN_HEADER_SIZE 28
#define VBAN_STREAM_NAME_SIZE 16
#define VBAN_DATA_MAX_SIZE 1436
#define VBAN_SAMPLES_MAX_NB 256
#define VBAN_CHANNELS_MAX_NB 8

static const long vban_sample_rates[] = {6000,  12000, 24000, 48000,  96000,  192000, 384000,
                                         8000,  16000, 32000, 64000,  128000, 256000, 512000,
                                         11025, 22050, 44100, 88200,  176400, 352800, 705600};

static const char *destination = NULL;
static int port = VBAN_DEFAULT_PORT;
static char stream_name[VBAN_STREAM_NAME_SIZE] = {0};
static int stream_name_truncated = 0;

#ifdef CONFIG_FOR_MINGW
typedef SOCKET vban_socket_t;
#define VBAN_INVALID_SOCKET INVALID_SOCKET
#else
typedef int vban_socket_t;
#define VBAN_INVALID_SOCKET (-1)
#endif

static vban_socket_t fd = VBAN_INVALID_SOCKET;
static struct sockaddr_storage remote_address;
static socklen_t remote_address_length = 0;

static unsigned int configured_channels = 0;
static unsigned int configured_rate = 0;
static unsigned int configured_format = SPS_FORMAT_UNKNOWN;
static unsigned int bytes_per_frame = 0;
static unsigned int samples_per_packet = 0;
static uint8_t vban_sample_rate_index = 0;
static uint8_t vban_bit_format = 0;
static uint32_t frame_counter = 0;
static uint8_t buffered_audio[VBAN_DATA_MAX_SIZE];
static unsigned int buffered_samples = 0;
static uint64_t buffered_playtime = 0;
static int buffered_playtime_valid = 0;
static int buffered_contains_timed_samples = 0;
static uint64_t next_packet_time = 0;
static uint64_t pacing_remainder = 0;
static int next_packet_time_valid = 0;
static uint64_t failed_packets_in_burst = 0;

#ifdef CONFIG_FOR_MINGW
static HANDLE pacing_timer = NULL;
#endif

static int socket_errno(void) {
#ifdef CONFIG_FOR_MINGW
  return WSAGetLastError();
#else
  return errno;
#endif
}

static int socket_is_open(void) { return fd != VBAN_INVALID_SOCKET; }

static void close_socket(void) {
  if (socket_is_open()) {
#ifdef CONFIG_FOR_MINGW
    closesocket(fd);
#else
    close(fd);
#endif
    fd = VBAN_INVALID_SOCKET;
  }
}

static void reset_packetizer(void) {
  buffered_samples = 0;
  buffered_playtime = 0;
  buffered_playtime_valid = 0;
  buffered_contains_timed_samples = 0;
  next_packet_time = 0;
  pacing_remainder = 0;
  next_packet_time_valid = 0;
}

static uint64_t frames_to_ns(uint64_t frames) {
  return (frames * 1000000000ULL) / configured_rate;
}

static uint64_t advance_packet_time(uint64_t packet_time, unsigned int frames) {
  uint64_t numerator = frames * 1000000000ULL + pacing_remainder;
  packet_time += numerator / configured_rate;
  pacing_remainder = numerator % configured_rate;
  return packet_time;
}

static int wait_until(uint64_t target_time) {
  while (1) {
    uint64_t now = get_absolute_time_in_ns();
    if (now >= target_time)
      return 0;

    uint64_t wait_ns = target_time - now;
#ifdef CONFIG_FOR_MINGW
    LARGE_INTEGER due_time;
    due_time.QuadPart = -(LONGLONG)((wait_ns + 99) / 100);
    if (SetWaitableTimer(pacing_timer, &due_time, 0, NULL, NULL, FALSE) == 0) {
      warn("vban: error %lu setting the packet pacing timer.", GetLastError());
      return -1;
    }
    DWORD wait_result = WaitForSingleObject(pacing_timer, INFINITE);
    if (wait_result != WAIT_OBJECT_0) {
      warn("vban: error %lu waiting for the packet pacing timer.", GetLastError());
      return -1;
    }
#else
    struct timespec request = {.tv_sec = wait_ns / 1000000000ULL,
                               .tv_nsec = wait_ns % 1000000000ULL};
    int sleep_response;
    do {
      sleep_response = nanosleep(&request, &request);
    } while ((sleep_response != 0) && (errno == EINTR));
    if (sleep_response != 0) {
      warn("vban: error %d waiting for the packet pacing timer.", errno);
      return -1;
    }
#endif
  }
}

static int vban_rate_index(unsigned int rate) {
  for (unsigned int i = 0; i < sizeof(vban_sample_rates) / sizeof(vban_sample_rates[0]); i++)
    if ((unsigned int)vban_sample_rates[i] == rate)
      return (int)i;
  return -1;
}

static int rate_is_configured(unsigned int rate) {
  for (sps_rate_t r = SPS_RATE_LOWEST; r <= SPS_RATE_HIGHEST; r++)
    if (((config.rate_set & (1 << r)) != 0) && (sps_rate_actual_rate(r) == rate))
      return 1;
  return 0;
}

static sps_format_t native_format(sps_format_t format) {
  switch (format) {
  case SPS_FORMAT_S16:
    return config.endianness == SS_BIG_ENDIAN ? SPS_FORMAT_S16_BE : SPS_FORMAT_S16_LE;
  case SPS_FORMAT_S24:
    return config.endianness == SS_BIG_ENDIAN ? SPS_FORMAT_S24_3BE : SPS_FORMAT_S24_3LE;
  case SPS_FORMAT_S32:
    return config.endianness == SS_BIG_ENDIAN ? SPS_FORMAT_S32_BE : SPS_FORMAT_S32_LE;
  default:
    return format;
  }
}

static int vban_bit_format_for_sps_format(sps_format_t format, uint8_t *bit_format) {
  switch (format) {
  case SPS_FORMAT_S8:
  case SPS_FORMAT_U8:
    *bit_format = 0; // 8I
    return 0;
  case SPS_FORMAT_S16_LE:
    *bit_format = 1; // 16I
    return 0;
  case SPS_FORMAT_S24_3LE:
    *bit_format = 2; // 24I
    return 0;
  case SPS_FORMAT_S32_LE:
    *bit_format = 3; // 32I
    return 0;
  default:
    return -1;
  }
}

static int open_socket(void) {
  if (socket_is_open())
    return 0;

  char port_string[16];
  snprintf(port_string, sizeof(port_string), "%d", port);

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;

  struct addrinfo *info = NULL;
  int response = getaddrinfo(destination, port_string, &hints, &info);
  if (response != 0)
    die("vban: can not resolve destination \"%s\" port %d.", destination, port);

  for (struct addrinfo *p = info; p != NULL; p = p->ai_next) {
    fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (!socket_is_open())
      continue;

    memcpy(&remote_address, p->ai_addr, p->ai_addrlen);
    remote_address_length = p->ai_addrlen;
    break;
  }

  freeaddrinfo(info);

  if (!socket_is_open())
    die("vban: can not create UDP socket for destination \"%s\".", destination);

  return 0;
}

static void set_stream_name(const char *name) {
  memset(stream_name, 0, sizeof(stream_name));
  if (name == NULL)
    name = "Shairport Sync";

  size_t length = strlen(name);
  if (length > VBAN_STREAM_NAME_SIZE) {
    length = VBAN_STREAM_NAME_SIZE;
    stream_name_truncated = 1;
  }
  memcpy(stream_name, name, length);
}

static void help(void) {
  printf("    vban.destination: destination host or IP address.\n");
  printf("    vban.port: destination UDP port. The default is %d.\n", VBAN_DEFAULT_PORT);
  printf("    vban.stream_name: VBAN stream name. Defaults to the Shairport Sync service name and is truncated to %d bytes.\n",
         VBAN_STREAM_NAME_SIZE);
}

static int init(int argc, char **argv) {
  config.audio_backend_buffer_desired_length = 1.0;
  config.audio_backend_latency_offset = 0;

  uint32_t default_format_set = (1 << SPS_FORMAT_S8) | (1 << SPS_FORMAT_U8) |
                                (1 << SPS_FORMAT_S16_LE) | (1 << SPS_FORMAT_S24_3LE) |
                                (1 << SPS_FORMAT_S32_LE);
  parse_audio_options("vban", default_format_set, SPS_RATE_SET, (1 << 1) | (1 << 2) |
                                                           (1 << 3) | (1 << 4) |
                                                           (1 << 5) | (1 << 6) |
                                                           (1 << 7) | (1 << 8));

  if (config.cfg != NULL) {
    const char *str;
    int value;

    if (config_lookup_non_empty_string(config.cfg, "vban.destination", &str))
      destination = str;

    if (config_lookup_int(config.cfg, "vban.port", &value)) {
      if ((value <= 0) || (value > 65535))
        die("vban.port must be between 1 and 65535.");
      port = value;
    }

    if (config_lookup_non_empty_string(config.cfg, "vban.stream_name", &str))
      set_stream_name(str);
  }

  if (argc > 3)
    die("too many command-line arguments to vban");
  if (argc >= 1)
    destination = argv[0];
  if (argc >= 2) {
    port = atoi(argv[1]);
    if ((port <= 0) || (port > 65535))
      die("vban port must be between 1 and 65535.");
  }
  if (argc >= 3)
    set_stream_name(argv[2]);

  if (destination == NULL)
    die("vban.destination is required.");

  if (stream_name[0] == 0)
    set_stream_name(config.service_name);

  if (stream_name_truncated)
    warn("vban.stream_name is longer than %d bytes and has been truncated.", VBAN_STREAM_NAME_SIZE);

#ifdef CONFIG_FOR_MINGW
  pacing_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_ALL_ACCESS);
  if (pacing_timer == NULL)
    die("vban: can not create the high-resolution packet pacing timer: error %lu.",
        GetLastError());
#endif

  return open_socket();
}

static void deinit(void) {
  close_socket();
#ifdef CONFIG_FOR_MINGW
  if (pacing_timer != NULL) {
    CloseHandle(pacing_timer);
    pacing_timer = NULL;
  }
#endif
}

static int32_t get_configuration(unsigned int channels, unsigned int rate, unsigned int format) {
  sps_format_t selected_format = native_format((sps_format_t)format);

  if ((channels == 0) || (channels > VBAN_CHANNELS_MAX_NB))
    return 0;
  if ((config.channel_set & (1 << channels)) == 0)
    return 0;
  if ((vban_rate_index(rate) < 0) || (rate_is_configured(rate) == 0))
    return 0;
  if ((config.format_set & (1 << selected_format)) == 0)
    return 0;

  uint8_t bit_format;
  if (vban_bit_format_for_sps_format(selected_format, &bit_format) != 0)
    return 0;

  return CHANNELS_TO_ENCODED_FORMAT(channels) | RATE_TO_ENCODED_FORMAT(rate) |
         FORMAT_TO_ENCODED_FORMAT(selected_format);
}

static int configure(int32_t requested_encoded_format, __attribute__((unused)) char **channel_map) {
  configured_channels = CHANNELS_FROM_ENCODED_FORMAT(requested_encoded_format);
  configured_rate = RATE_FROM_ENCODED_FORMAT(requested_encoded_format);
  configured_format = FORMAT_FROM_ENCODED_FORMAT(requested_encoded_format);

  int rate_index = vban_rate_index(configured_rate);
  if (rate_index < 0)
    return EINVAL;

  if (vban_bit_format_for_sps_format((sps_format_t)configured_format, &vban_bit_format) != 0)
    return EINVAL;

  unsigned int bytes_per_sample = sps_format_sample_size((sps_format_t)configured_format);
  if ((bytes_per_sample == 0) || (configured_channels == 0))
    return EINVAL;

  bytes_per_frame = bytes_per_sample * configured_channels;
  samples_per_packet = VBAN_DATA_MAX_SIZE / bytes_per_frame;
  if (samples_per_packet > VBAN_SAMPLES_MAX_NB)
    samples_per_packet = VBAN_SAMPLES_MAX_NB;
  if (samples_per_packet == 0)
    return EINVAL;

  vban_sample_rate_index = (uint8_t)rate_index;
  frame_counter = 0;
  failed_packets_in_burst = 0;
  reset_packetizer();

  debug(1, "vban: setting output configuration to %s.", short_format_description(requested_encoded_format));
  return 0;
}

static void write_frame_counter(uint8_t *p, uint32_t value) {
  p[0] = (uint8_t)(value & 0xff);
  p[1] = (uint8_t)((value >> 8) & 0xff);
  p[2] = (uint8_t)((value >> 16) & 0xff);
  p[3] = (uint8_t)((value >> 24) & 0xff);
}

static int send_packet(const uint8_t *audio, unsigned int samples) {
  uint8_t packet[VBAN_HEADER_SIZE + VBAN_DATA_MAX_SIZE];
  size_t payload_size = samples * bytes_per_frame;

  packet[0] = 'V';
  packet[1] = 'B';
  packet[2] = 'A';
  packet[3] = 'N';
  packet[4] = vban_sample_rate_index;
  packet[5] = (uint8_t)(samples - 1);
  packet[6] = (uint8_t)(configured_channels - 1);
  packet[7] = vban_bit_format;
  memcpy(&packet[8], stream_name, VBAN_STREAM_NAME_SIZE);
  write_frame_counter(&packet[24], frame_counter++);
  memcpy(&packet[VBAN_HEADER_SIZE], audio, payload_size);

  int response = sendto(fd, (const char *)packet, VBAN_HEADER_SIZE + payload_size, 0,
                        (struct sockaddr *)&remote_address, remote_address_length);
  if (response < 0) {
    int error = socket_errno();
    if (failed_packets_in_burst == 0)
      warn("vban: error %d sending UDP packet; packet loss burst started.", error);
    failed_packets_in_burst++;
    return -1;
  }

  if (failed_packets_in_burst != 0) {
    warn("vban: UDP output recovered after %" PRIu64 " failed packets.",
         failed_packets_in_burst);
    failed_packets_in_burst = 0;
  }
  return response;
}

static int send_buffered_packet(void) {
  uint64_t packet_time;
  if (buffered_playtime_valid != 0) {
    packet_time = buffered_playtime;
    pacing_remainder = 0;
  } else if (next_packet_time_valid != 0) {
    packet_time = next_packet_time;
  } else {
    packet_time = get_absolute_time_in_ns();
  }

  int response = wait_until(packet_time);
  if (response == 0)
    response = send_packet(buffered_audio, buffered_samples);

  next_packet_time = advance_packet_time(packet_time, buffered_samples);
  next_packet_time_valid = 1;
  buffered_samples = 0;
  buffered_playtime = 0;
  buffered_playtime_valid = 0;
  buffered_contains_timed_samples = 0;
  return response < 0 ? -1 : 0;
}

static int play(void *buf, int samples, int sample_type,
                __attribute__((unused)) uint32_t timestamp, uint64_t playtime) {
  if ((!socket_is_open()) && (open_socket() != 0))
    return -1;
  if ((bytes_per_frame == 0) || (samples_per_packet == 0)) {
    debug(1, "vban: output format not configured before play().");
    return -1;
  }

  const uint8_t *audio = buf;
  int samples_remaining = samples;
  unsigned int samples_consumed = 0;
  int response = 0;

  while (samples_remaining > 0) {
    if (buffered_samples == 0) {
      if (sample_type == play_samples_are_timed) {
        buffered_playtime = playtime + frames_to_ns(samples_consumed);
        buffered_playtime_valid = 1;
        buffered_contains_timed_samples = 1;
      }
    } else if ((sample_type == play_samples_are_timed) &&
               (buffered_contains_timed_samples == 0)) {
      buffered_playtime =
          playtime + frames_to_ns(samples_consumed) - frames_to_ns(buffered_samples);
      buffered_playtime_valid = 1;
      buffered_contains_timed_samples = 1;
    }

    unsigned int space = samples_per_packet - buffered_samples;
    unsigned int chunk =
        samples_remaining > (int)space ? space : (unsigned int)samples_remaining;
    memcpy(buffered_audio + buffered_samples * bytes_per_frame, audio,
           chunk * bytes_per_frame);
    buffered_samples += chunk;
    audio += chunk * bytes_per_frame;
    samples_remaining -= chunk;
    samples_consumed += chunk;

    if ((buffered_samples == samples_per_packet) && (send_buffered_packet() != 0))
      response = -1;
  }

  return response;
}

static void flush(void) {
  frame_counter = 0;
  failed_packets_in_burst = 0;
  reset_packetizer();
}

audio_output audio_vban = {.name = "vban",
                           .help = &help,
                           .init = &init,
                           .deinit = &deinit,
                           .get_configuration = &get_configuration,
                           .configure = &configure,
                           .start = NULL,
                           .stop = NULL,
                           .is_running = NULL,
                           .flush = &flush,
                           .delay = NULL,
                           .stats = NULL,
                           .play = &play,
                           .volume = NULL,
                           .parameters = NULL,
                           .mute = NULL};
