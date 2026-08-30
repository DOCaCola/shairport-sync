#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>

#define restrict
int eintr_checked_accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
ssize_t socket_read(int sockfd, void *buf, size_t count);
ssize_t socket_write(int sockfd, const void *buf, size_t count);
int socket_set_errno(void);
int socket_set_timeout_ms(int sockfd, int option_name, uint32_t timeout_ms);

int _safe_socket_close(const char *filename, const int linenumber, int *sockfd);

#define safe_socket_close(sockfd) _safe_socket_close(__FILE__, __LINE__, sockfd)
