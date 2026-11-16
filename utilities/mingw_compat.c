/*
 * MinGW POSIX compatibility. This file is part of Shairport Sync.
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

#include "utilities/mingw_compat.h"

// The Microsoft CRT has no message text for the POSIX supplement errno values (100 and up).
static const char *posix_supplement_error_text(int errnum) {
  switch (errnum) {
  case EADDRINUSE:
    return "Address already in use";
  case EADDRNOTAVAIL:
    return "Cannot assign requested address";
  case EAFNOSUPPORT:
    return "Address family not supported by protocol";
  case EALREADY:
    return "Operation already in progress";
  case EBADMSG:
    return "Bad message";
  case ECANCELED:
    return "Operation canceled";
  case ECONNABORTED:
    return "Software caused connection abort";
  case ECONNREFUSED:
    return "Connection refused";
  case ECONNRESET:
    return "Connection reset by peer";
  case EDESTADDRREQ:
    return "Destination address required";
  case EHOSTUNREACH:
    return "No route to host";
  case EIDRM:
    return "Identifier removed";
  case EINPROGRESS:
    return "Operation now in progress";
  case EISCONN:
    return "Transport endpoint is already connected";
  case ELOOP:
    return "Too many levels of symbolic links";
  case EMSGSIZE:
    return "Message too long";
  case ENETDOWN:
    return "Network is down";
  case ENETRESET:
    return "Network dropped connection on reset";
  case ENETUNREACH:
    return "Network is unreachable";
  case ENOBUFS:
    return "No buffer space available";
  case ENODATA:
    return "No data available";
  case ENOLINK:
    return "Link has been severed";
  case ENOMSG:
    return "No message of desired type";
  case ENOPROTOOPT:
    return "Protocol not available";
  case ENOSR:
    return "Out of streams resources";
  case ENOSTR:
    return "Device not a stream";
  case ENOTCONN:
    return "Transport endpoint is not connected";
  case ENOTRECOVERABLE:
    return "State not recoverable";
  case ENOTSOCK:
    return "Socket operation on non-socket";
  case ENOTSUP:
    return "Not supported";
  case EOPNOTSUPP:
    return "Operation not supported";
  case EOVERFLOW:
    return "Value too large for defined data type";
  case EOWNERDEAD:
    return "Owner died";
  case EPROTO:
    return "Protocol error";
  case EPROTONOSUPPORT:
    return "Protocol not supported";
  case EPROTOTYPE:
    return "Protocol wrong type for socket";
  case ETIME:
    return "Timer expired";
  case ETIMEDOUT:
    return "Connection timed out";
  case ETXTBSY:
    return "Text file busy";
  case EWOULDBLOCK:
    return "Operation would block";
  default:
    return NULL;
  }
}

int shairport_mingw_strerror_r(int errnum, char *buf, size_t buflen) {
  const char *text = posix_supplement_error_text(errnum);
  if (text == NULL)
    return strerror_s(buf, buflen, errnum);
  return strncpy_s(buf, buflen, text, _TRUNCATE) == STRUNCATE ? ERANGE : 0;
}

char *shairport_mingw_strerror(int errnum) {
  const char *text = posix_supplement_error_text(errnum);
  return text == NULL ? (strerror)(errnum) : (char *)text;
}

// winpthreads' nanosleep() and pthread_delay_np() return at once when cancellation is
// disabled and a cancel is pending, and MinGW's usleep() and sleep() are not cancellation
// points at all. This gives the POSIX behaviour: cancellable when enabled, full length when not.
int shairport_mingw_nanosleep(const struct timespec *request, struct timespec *remaining) {
  if ((request->tv_sec < 0) || (request->tv_nsec < 0) || (request->tv_nsec >= 1000000000L)) {
    errno = EINVAL;
    return -1;
  }

  int cancel_state, probe_state;
  pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cancel_state);
  pthread_setcancelstate(cancel_state, &probe_state);

  if (cancel_state == PTHREAD_CANCEL_ENABLE) {
    pthread_delay_np(request);
  } else {
    Sleep((DWORD)(request->tv_sec * 1000 + (request->tv_nsec + 999999) / 1000000));
  }

  if (remaining != NULL) {
    remaining->tv_sec = 0;
    remaining->tv_nsec = 0;
  }
  return 0;
}

int shairport_mingw_usleep(useconds_t usec) {
  struct timespec request = {.tv_sec = usec / 1000000, .tv_nsec = (long)(usec % 1000000) * 1000};
  return shairport_mingw_nanosleep(&request, NULL);
}

unsigned int shairport_mingw_sleep(unsigned int seconds) {
  struct timespec request = {.tv_sec = seconds, .tv_nsec = 0};
  shairport_mingw_nanosleep(&request, NULL);
  return 0;
}
