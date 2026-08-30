/*
 * Network Utilities. This file is part of Shairport Sync.
 * Copyright (c) Mike Brady 2026
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

#include "network_utilities.h"
#include "common.h"
#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>

#ifdef CONFIG_FOR_MINGW
#include <winsock2.h>
#endif

int socket_set_errno(void) {
#ifdef CONFIG_FOR_MINGW
  switch (WSAGetLastError()) {
  case WSAEINTR:
    errno = EINTR;
    break;
  case WSAEWOULDBLOCK:
    errno = EAGAIN;
    break;
  case WSAEADDRINUSE:
    errno = EADDRINUSE;
    break;
  case WSAECONNRESET:
    errno = ECONNRESET;
    break;
  case WSAETIMEDOUT:
    errno = ETIMEDOUT;
    break;
  case WSAECONNREFUSED:
    errno = ECONNREFUSED;
    break;
  default:
    errno = EIO;
    break;
  }
#endif
  return errno;
}

int socket_set_timeout_ms(int sockfd, int option_name, uint32_t timeout_ms) {
#ifdef CONFIG_FOR_MINGW
  DWORD timeout = timeout_ms;
  int response =
      setsockopt(sockfd, SOL_SOCKET, option_name, (const char *)&timeout, sizeof(timeout));
#else
  struct timeval timeout = {
      .tv_sec = timeout_ms / 1000,
      .tv_usec = (timeout_ms % 1000) * 1000,
  };
  int response = setsockopt(sockfd, SOL_SOCKET, option_name, &timeout, sizeof(timeout));
#endif
  if (response < 0)
    socket_set_errno();
  return response;
}

int eintr_checked_accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
  int response;
  do {
    response = accept(sockfd, addr, addrlen);

    if (response == -1) {
      socket_set_errno();
      char errorstring[1024];
      strerror_r(errno, (char *)errorstring, sizeof(errorstring));
      debug(1, "error %d accept()ing a socket %d: \"%s\". (Note: error %d will be ignored.)", errno,
            sockfd, errorstring, EINTR);
    }

  } while ((response == -1) && (errno == EINTR));
  return response;
}

pthread_mutex_t safe_socket_lock = PTHREAD_MUTEX_INITIALIZER;

ssize_t socket_read(int sockfd, void *buf, size_t count) {
#ifdef CONFIG_FOR_MINGW
  int response = recv(sockfd, buf, count, 0);
  if (response == SOCKET_ERROR)
    socket_set_errno();
  return response;
#else
  return read(sockfd, buf, count);
#endif
}

ssize_t socket_write(int sockfd, const void *buf, size_t count) {
#ifdef CONFIG_FOR_MINGW
  int response = send(sockfd, buf, count, 0);
  if (response == SOCKET_ERROR)
    socket_set_errno();
  return response;
#else
  return write(sockfd, buf, count);
#endif
}

int _safe_socket_close(const char *filename, const int linenumber, int *sockfd) {
  int result = 0;
  int oldstate;
  pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldstate);
  pthread_mutex_lock(&safe_socket_lock);
  if (*sockfd == 0) {
    _debug(filename, linenumber, 1, "_safe_socket_close: socket is zero!");
  }
  if ((*sockfd != -1) && (*sockfd != 0)) {
    _debug(filename, linenumber, 4, "_safe_socket_close: closing socket %d.", *sockfd);
#ifdef CONFIG_FOR_MINGW
    result = closesocket(*sockfd);
#else
    result = close(*sockfd);
#endif
    if (result == 0)
      *sockfd = -1;
  } else {
    _debug(filename, linenumber, 1, "_safe_socket_close: socket already closed!");
  }
  pthread_mutex_unlock(&safe_socket_lock);
  pthread_setcancelstate(oldstate, NULL);
  return result;
}
