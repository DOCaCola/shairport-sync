/*
MIT License

Copyright (c) 2023--2025 Mike Brady 4265913+mikebrady@users.noreply.github.com

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include "config.h"
#include "debug.h"
#include <ctype.h> // for isprint()
#include <unistd.h> // for usleep()
#include <inttypes.h>
#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#ifdef CONFIG_FOR_MINGW
#include <io.h>
#include <process.h>
#include <windows.h>
#endif

static int debuglev = 0;
int debugger_show_elapsed_time = 0;
int debugger_show_relative_time = 0;
int debugger_show_file_and_line = 1;

static uint64_t ns_time_at_startup = 0;
static uint64_t ns_time_at_last_debug_message;

void (*local_exit_requester)(const int exit_status) = NULL;

// always lock use this when accessing the ns_time_at_last_debug_message
static pthread_mutex_t debug_timing_lock = PTHREAD_MUTEX_INITIALIZER;

#ifdef CONFIG_FOR_MINGW
#define ASYNC_OUTPUT_QUEUE_CAPACITY 256
#define ASYNC_OUTPUT_MESSAGE_SIZE 4096

typedef struct {
  FILE *stream;
  char message[ASYNC_OUTPUT_MESSAGE_SIZE];
} async_output_message;

static INIT_ONCE async_output_init_once = INIT_ONCE_STATIC_INIT;
static SRWLOCK async_output_queue_lock = SRWLOCK_INIT;
static HANDLE async_output_queue_event = NULL;
static HANDLE async_output_thread = NULL;
static volatile LONG async_output_stopping = 0;
static volatile LONG async_output_messages_dropped = 0;
static int async_output_writer_available = 0;
static async_output_message async_output_queue[ASYNC_OUTPUT_QUEUE_CAPACITY];
static size_t async_output_queue_head = 0;
static size_t async_output_queue_tail = 0;
static size_t async_output_queue_count = 0;

static void async_output_write(FILE *stream, const char *message) {
  // Use the handle directly so a blocked writer cannot hold a CRT stream lock at exit.
  HANDLE handle = (HANDLE)_get_osfhandle(_fileno(stream));
  char line[ASYNC_OUTPUT_MESSAGE_SIZE + 2];
  size_t length = strlen(message);
  memcpy(line, message, length);
  line[length++] = '\r';
  line[length++] = '\n';
  size_t offset = 0;
  while (offset < length) {
    DWORD written;
    if (!WriteFile(handle, line + offset, (DWORD)(length - offset), &written, NULL) ||
        written == 0)
      break;
    offset += written;
  }
}

static unsigned __stdcall async_output_writer(void *arg) {
  (void)arg;
  async_output_message message;

  while (WaitForSingleObject(async_output_queue_event, INFINITE) == WAIT_OBJECT_0) {
    while (1) {
      AcquireSRWLockExclusive(&async_output_queue_lock);
      if (async_output_queue_count == 0) {
        ReleaseSRWLockExclusive(&async_output_queue_lock);
        if (InterlockedCompareExchange(&async_output_stopping, 0, 0))
          return 0;
        break;
      }

      message = async_output_queue[async_output_queue_head];
      async_output_queue_head =
          (async_output_queue_head + 1) % ASYNC_OUTPUT_QUEUE_CAPACITY;
      async_output_queue_count--;
      ReleaseSRWLockExclusive(&async_output_queue_lock);

      LONG dropped = InterlockedExchange(&async_output_messages_dropped, 0);
      if (dropped != 0) {
        char notice[128];
        snprintf(notice, sizeof(notice), "[shairport-sync] %ld log messages dropped.", dropped);
        async_output_write(message.stream, notice);
      }
      async_output_write(message.stream, message.message);
    }
  }

  return 0;
}

static void async_output_shutdown(void) {
  InterlockedExchange(&async_output_stopping, 1);
  SetEvent(async_output_queue_event);
  // A selected console or blocked pipe must not prevent process shutdown.
  WaitForSingleObject(async_output_thread, 500);
}

static BOOL CALLBACK async_output_init(PINIT_ONCE init_once, PVOID parameter, PVOID *context) {
  (void)init_once;
  (void)parameter;
  (void)context;

  async_output_queue_event = CreateEvent(NULL, FALSE, FALSE, NULL);
  if (async_output_queue_event != NULL) {
    uintptr_t thread = _beginthreadex(NULL, 0, async_output_writer, NULL, 0, NULL);
    if (thread != 0) {
      async_output_thread = (HANDLE)thread;
      async_output_writer_available = 1;
      atexit(async_output_shutdown);
    } else {
      CloseHandle(async_output_queue_event);
      async_output_queue_event = NULL;
    }
  }

  if (!async_output_writer_available) {
    fprintf(stderr, "Could not initialise asynchronous Windows logging.\n");
    ExitProcess(EXIT_FAILURE);
  }
  return TRUE;
}

static void async_output_output_init(void) {
  InitOnceExecuteOnce(&async_output_init_once, async_output_init, NULL, NULL);
}

static void async_output_enqueue(FILE *stream, const char *message) {
  if (async_output_writer_available == 0) {
    InterlockedIncrement(&async_output_messages_dropped);
    return;
  }

  // The lock only guards copying into the queue; console I/O happens outside it.
  AcquireSRWLockExclusive(&async_output_queue_lock);
  if (async_output_queue_count == ASYNC_OUTPUT_QUEUE_CAPACITY) {
    ReleaseSRWLockExclusive(&async_output_queue_lock);
    InterlockedIncrement(&async_output_messages_dropped);
    return;
  }

  async_output_message *entry = &async_output_queue[async_output_queue_tail];
  entry->stream = stream;
  size_t message_length = strlen(message);
  if (message_length < sizeof(entry->message)) {
    memcpy(entry->message, message, message_length + 1);
  } else {
    static const char truncation_marker[] = "... [truncated]";
    size_t marker_length = sizeof(truncation_marker) - 1;
    size_t copy_length = sizeof(entry->message) - marker_length - 1;
    memcpy(entry->message, message, copy_length);
    memcpy(entry->message + copy_length, truncation_marker, marker_length + 1);
  }

  async_output_queue_tail =
      (async_output_queue_tail + 1) % ASYNC_OUTPUT_QUEUE_CAPACITY;
  async_output_queue_count++;
  ReleaseSRWLockExclusive(&async_output_queue_lock);
  SetEvent(async_output_queue_event);
}
#endif

void debug_write_line(FILE *stream, const char *message) {
#ifdef CONFIG_FOR_MINGW
  async_output_output_init();
  if (stream == stderr || stream == stdout) {
    async_output_enqueue(stream, message);
    return;
  }
#endif
  fprintf(stream, "%s\n", message);
}

void debug_write_formatted_line(FILE *stream, const char *format, ...) {
  char message[1024 * 64];
  va_list args;
  va_start(args, format);
  vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  debug_write_line(stream, message);
}

uint64_t debug_get_absolute_time_in_ns() {
  uint64_t time_now_ns;
  struct timespec tn;
  // CLOCK_REALTIME because PTP uses it.
  clock_gettime(CLOCK_REALTIME, &tn);
  uint64_t tnnsec = tn.tv_sec;
  tnnsec = tnnsec * 1000000000;
  uint64_t tnjnsec = tn.tv_nsec;
  time_now_ns = tnnsec + tnjnsec;
  return time_now_ns;
}

void debug_init(int level, int show_elapsed_time, int show_relative_time, int show_file_and_line,
                void (*exit_requester)(const int exit_status)) {
#ifdef CONFIG_FOR_MINGW
  async_output_output_init();
#endif
  ns_time_at_startup = debug_get_absolute_time_in_ns();
  ns_time_at_last_debug_message = ns_time_at_startup;
  debuglev = level;
  debugger_show_elapsed_time = show_elapsed_time;
  debugger_show_relative_time = show_relative_time;
  debugger_show_file_and_line = show_file_and_line;
  local_exit_requester = exit_requester;
}

int debug_level() { return debuglev; };

void set_debug_level(int level) { debuglev = level; }

void increase_debug_level() {
  if (debuglev < 3)
    debuglev++;
}

void decrease_debug_level() {
  if (debuglev > 0)
    debuglev--;
}

int get_show_elapsed_time() { return debugger_show_elapsed_time; }
void set_show_elapsed_time(int setting) { debugger_show_elapsed_time = setting; }
int get_show_relative_timel() { return debugger_show_relative_time; }

void set_show_relative_time(int setting) { debugger_show_relative_time = setting; }

int get_show_file_and_line() { return debugger_show_file_and_line; }

void set_show_file_and_line(int setting) { debugger_show_file_and_line = setting; }

char *generate_preliminary_string(char *buffer, size_t buffer_length, double tss, double tsl,
                                  const char *filename, const int linenumber, const char *prefix) {
  size_t space_remaining = buffer_length;
  char *insertion_point = buffer;
  if (debugger_show_elapsed_time) {
    snprintf(insertion_point, space_remaining, "% 20.9f", tss);
    insertion_point = insertion_point + strlen(insertion_point);
    space_remaining = space_remaining - strlen(insertion_point);
  }
  if (debugger_show_relative_time) {
    snprintf(insertion_point, space_remaining, "% 20.9f", tsl);
    insertion_point = insertion_point + strlen(insertion_point);
    space_remaining = space_remaining - strlen(insertion_point);
  }
  if (debugger_show_file_and_line) {
    snprintf(insertion_point, space_remaining, " \"%s:%d\"", filename, linenumber);
    insertion_point = insertion_point + strlen(insertion_point);
    space_remaining = space_remaining - strlen(insertion_point);
  }
  if (prefix) {
    snprintf(insertion_point, space_remaining, "%s", prefix);
    insertion_point = insertion_point + strlen(insertion_point);
    space_remaining = space_remaining - strlen(insertion_point);
  }
  return insertion_point;
}

void _die(const char *filename, const int linenumber, const char *format, ...) {
  int oldState;
  pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldState);
  char b[1024];
  b[0] = 0;
  char *s;
  if (debuglev) {
    pthread_mutex_lock(&debug_timing_lock);
    uint64_t time_now = debug_get_absolute_time_in_ns();
    uint64_t time_since_start = time_now - ns_time_at_startup;
    uint64_t time_since_last_debug_message = time_now - ns_time_at_last_debug_message;
    ns_time_at_last_debug_message = time_now;
    pthread_mutex_unlock(&debug_timing_lock);
    s = generate_preliminary_string(b, sizeof(b), 1.0 * time_since_start / 1000000000,
                                    1.0 * time_since_last_debug_message / 1000000000, filename,
                                    linenumber, " *fatal error: ");
  } else {
    strncpy(b, "fatal error: ", sizeof(b));
    s = b + strlen(b);
  }
  va_list args;
  va_start(args, format);
  vsnprintf(s, sizeof(b) - (s - b), format, args);
  va_end(args);
  // syslog(LOG_ERR, "%s", b);
  debug_write_line(stderr, b);
  pthread_setcancelstate(oldState, NULL);
  if (local_exit_requester != NULL) {
    local_exit_requester(EXIT_FAILURE);
    // wait for the exit request to be honoured
    usleep(1000000);
    // if not, head for the hills
    debug_write_line(stderr, "fatal error: exit cleanup could not be completed.");
    _Exit(EXIT_FAILURE);
  }
}

void _warn(const char *filename, const int linenumber, const char *format, ...) {
  int oldState;
  pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldState);
  char b[1024];
  b[0] = 0;
  char *s;
  if (debuglev) {
    pthread_mutex_lock(&debug_timing_lock);
    uint64_t time_now = debug_get_absolute_time_in_ns();
    uint64_t time_since_start = time_now - ns_time_at_startup;
    uint64_t time_since_last_debug_message = time_now - ns_time_at_last_debug_message;
    ns_time_at_last_debug_message = time_now;
    pthread_mutex_unlock(&debug_timing_lock);
    s = generate_preliminary_string(b, sizeof(b), 1.0 * time_since_start / 1000000000,
                                    1.0 * time_since_last_debug_message / 1000000000, filename,
                                    linenumber, " *warning: ");
  } else {
    strncpy(b, "warning: ", sizeof(b));
    s = b + strlen(b);
  }
  va_list args;
  va_start(args, format);
  vsnprintf(s, sizeof(b) - (s - b), format, args);
  va_end(args);
  // syslog(LOG_WARNING, "%s", b);
  debug_write_line(stderr, b);
  pthread_setcancelstate(oldState, NULL);
}

void _debug(const char *filename, const int linenumber, int level, const char *format, ...) {
  if (level > debuglev)
    return;
  int oldState;
  pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldState);
  char b[1024 * 64];
  b[0] = 0;
  pthread_mutex_lock(&debug_timing_lock);
  uint64_t time_now = debug_get_absolute_time_in_ns();
  uint64_t time_since_start = time_now - ns_time_at_startup;
  uint64_t time_since_last_debug_message = time_now - ns_time_at_last_debug_message;
  ns_time_at_last_debug_message = time_now;
  pthread_mutex_unlock(&debug_timing_lock);
  char *s = generate_preliminary_string(b, sizeof(b), 1.0 * time_since_start / 1000000000,
                                        1.0 * time_since_last_debug_message / 1000000000, filename,
                                        linenumber, " ");
  va_list args;
  va_start(args, format);
  vsnprintf(s, sizeof(b) - (s - b), format, args);
  va_end(args);
  // syslog(LOG_DEBUG, "%s", b);
  debug_write_line(stderr, b);
  pthread_setcancelstate(oldState, NULL);
}

void _inform(const char *filename, const int linenumber, const char *format, ...) {
  int oldState;
  pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldState);
  char b[1024];
  b[0] = 0;
  char *s;
  if (debuglev) {
    pthread_mutex_lock(&debug_timing_lock);
    uint64_t time_now = debug_get_absolute_time_in_ns();
    uint64_t time_since_start = time_now - ns_time_at_startup;
    uint64_t time_since_last_debug_message = time_now - ns_time_at_last_debug_message;
    ns_time_at_last_debug_message = time_now;
    pthread_mutex_unlock(&debug_timing_lock);
    s = generate_preliminary_string(b, sizeof(b), 1.0 * time_since_start / 1000000000,
                                    1.0 * time_since_last_debug_message / 1000000000, filename,
                                    linenumber, " ");
  } else {
    s = b;
  }
  va_list args;
  va_start(args, format);
  vsnprintf(s, sizeof(b) - (s - b), format, args);
  va_end(args);
  // syslog(LOG_INFO, "%s", b);
  debug_write_line(stderr, b);
  pthread_setcancelstate(oldState, NULL);
}

void _debug_print_buffer(const char *thefilename, const int linenumber, int level, void *ibuf,
                         size_t buf_len) {

  if (level > debuglev)
    return;

  char *vbuf = (char *)ibuf; // this to stop the compiler complaining about indexing through a void
  unsigned int i;
  unsigned int hexdump_cols = 16;

  // 0x123456: <hexdump_cols * 3> <hexdump_cols>\n
  const size_t buffer_size = 2 + 6 + 2 + hexdump_cols * 3 + hexdump_cols + 1;
  char *buf = malloc(buffer_size);
  if (buf) {
    // char *bufp = buf;
    //  *buf = '\0';

    // if (msg)
    //   printf("%s", msg);

#define ADDR_PREFIX_LEN 10 // "0x000000: "

    // each complete row
    for (i = 0; i < (buf_len / hexdump_cols); i++) {
      char *bufp = buf;
      *buf = '\0';
      snprintf(bufp, ADDR_PREFIX_LEN + 1, "0x%06x  ", (i * hexdump_cols) & 0xFFFFFF);
      bufp += ADDR_PREFIX_LEN;

      unsigned int j;
      for (j = 0; j < hexdump_cols; j++) {
        snprintf(bufp, strlen("12 ") + 1, "%02x ", 0xFF & vbuf[i * hexdump_cols + j]);
        bufp += strlen("12 ");
      }

      for (j = 0; j < hexdump_cols; j++) {
        if (isprint(vbuf[i * hexdump_cols + j])) {
          *bufp = 0xFF & vbuf[i * hexdump_cols + j];
        } else {
          *bufp = '.';
        }
        bufp++;
      }
      *bufp = '\0';
      _debug(thefilename, linenumber, level, "%s", buf);
    }

    size_t remaining = buf_len % hexdump_cols;
    if (remaining != 0) {
      size_t starting_offset = buf_len - remaining;
      char *bufp = buf;
      *buf = '\0';
      snprintf(bufp, ADDR_PREFIX_LEN + 1, "0x%06zx  ", starting_offset & 0xFFFFFF);
      bufp += ADDR_PREFIX_LEN;

      unsigned int j;
      for (j = 0; j < hexdump_cols; j++) {
        if (j < remaining) {
          snprintf(bufp, strlen("12 ") + 1, "%02x ", 0xFF & vbuf[starting_offset + j]);
        } else {
          snprintf(bufp, strlen("   ") + 1, "   "); // fille with blanks
        }
        bufp += strlen("12 ");
      }

      for (j = 0; j < remaining; j++) {
        if (isprint(vbuf[i * hexdump_cols + j])) {
          *bufp = 0xFF & vbuf[starting_offset + j];
        } else {
          *bufp = '.';
        }
        bufp++;
      }
      *bufp = '\0';
      _debug(thefilename, linenumber, level, "%s", buf);
    }
    free(buf);
  }
}

/*
void _debug_print_buffer(const char *thefilename, const int linenumber, int level, void *vbuf,
                         size_t buf_len) {
  if (level > debuglev)
    return;
  new_debug_print_buffer(thefilename, linenumber, level, vbuf, buf_len);
  char *buf = (char *)vbuf;
  char *obf =
      malloc(buf_len * 4 + 1); // to be on the safe side -- 4 characters on average for each byte
  if (obf != NULL) {
    char *obfp = obf;
    unsigned int obfc;
    for (obfc = 0; obfc < buf_len; obfc++) {
      snprintf(obfp, 3, "%02X", (unsigned char)buf[obfc]);
      obfp += 2;
      if (obfc != buf_len - 1) {
        if (obfc % 32 == 31) {
          snprintf(obfp, 5, " || ");
          obfp += strlen(" || ");
        } else if (obfc % 16 == 15) {
          snprintf(obfp, 4, " | ");
          obfp += strlen(" | ");;
        } else if (obfc % 4 == 3) {
          snprintf(obfp, 2, " ");
          obfp += strlen(" ");;
        }
      }
    };
    *obfp = 0;
    _debug(thefilename, linenumber, level, "%s", obf);
    free(obf);
  }
}
*/
