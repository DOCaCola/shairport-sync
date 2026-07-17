/*
 * mDNS registration handler. This file is part of Shairport.
 * Copyright (c) Paul Lietar 2013
 * Copyright (c) Mike Brady 2014--2025
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

#include "mdns.h"
#include "common.h"
#ifdef CONFIG_DACP_CLIENT
#include "dacp.h"
#endif
#ifdef CONFIG_METADATA
#include "metadata/core.h"
#endif
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "tinysvcmdns.h"

struct mdns_tinysvcmdns_server {
  struct mdnsd *server;
  char *interface_name;
  struct mdns_tinysvcmdns_server *next;
};

static struct mdns_tinysvcmdns_server *servers = NULL;

static struct mdns_tinysvcmdns_server *find_server_for_interface(const char *interface_name) {
  struct mdns_tinysvcmdns_server *server = servers;
  while (server != NULL) {
    if (strcmp(server->interface_name, interface_name) == 0)
      return server;
    server = server->next;
  }
  return NULL;
}

#ifdef CONFIG_DACP_CLIENT
static pthread_mutex_t dacp_monitor_lock = PTHREAD_MUTEX_INITIALIZER;
static char *dacp_monitor_id = NULL;

static char *nlabel_to_str_no_trailing_dot(const uint8_t *name) {
  char *name_string = nlabel_to_str(name);
  if (name_string != NULL) {
    size_t len = strlen(name_string);
    if ((len > 0) && (name_string[len - 1] == '.'))
      name_string[len - 1] = '\0';
  }
  return name_string;
}

static int service_name_matches_dacp_id(const char *service_name, const char *dacp_id) {
  const char prefix[] = "iTunes_Ctrl_";
  const char suffix[] = "._dacp._tcp.local";
  size_t service_name_len = strlen(service_name);
  size_t prefix_len = strlen(prefix);
  size_t suffix_len = strlen(suffix);

  if ((dacp_id == NULL) || (service_name_len <= prefix_len + suffix_len) ||
      (strncmp(service_name, prefix, prefix_len) != 0) ||
      (strcmp(service_name + service_name_len - suffix_len, suffix) != 0))
    return 0;

  const char *service_dacp_id = service_name + prefix_len;
  size_t service_dacp_id_len = service_name_len - prefix_len - suffix_len;
  while ((service_dacp_id_len > 0) && (*service_dacp_id == '0')) {
    service_dacp_id++;
    service_dacp_id_len--;
  }

  return (strlen(dacp_id) == service_dacp_id_len) &&
         (strncmp(service_dacp_id, dacp_id, service_dacp_id_len) == 0);
}

static void mdns_tinysvcmdns_note_dacp_port(const char *dacp_id, uint16_t port) {
  dacp_monitor_port_update_callback(dacp_id, port);
#ifdef CONFIG_METADATA
  char port_in_chars[16];
  snprintf(port_in_chars, sizeof(port_in_chars), "%u", port);
  send_ssnc_metadata('dapo', port_in_chars, strlen(port_in_chars), 0);
#endif
}

static void process_dacp_record(struct rr_entry *rr, const char *dacp_id, struct mdnsd *server) {
  if (rr == NULL)
    return;

  if (rr->type == RR_PTR) {
    char *name = nlabel_to_str_no_trailing_dot(rr->name);
    if ((name != NULL) && (strcmp(name, "_dacp._tcp.local") == 0)) {
      char *service_name = nlabel_to_str_no_trailing_dot(MDNS_RR_GET_PTR_NAME(rr));
      if ((service_name != NULL) && service_name_matches_dacp_id(service_name, dacp_id)) {
        if (rr->ttl == 0) {
          mdns_tinysvcmdns_note_dacp_port(dacp_id, 0);
        } else if (server != NULL) {
          mdnsd_send_query(server, service_name, RR_SRV);
        }
      }
      free(service_name);
    }
    free(name);
  } else if (rr->type == RR_SRV) {
    char *service_name = nlabel_to_str_no_trailing_dot(rr->name);
    if ((service_name != NULL) && service_name_matches_dacp_id(service_name, dacp_id)) {
      mdns_tinysvcmdns_note_dacp_port(dacp_id, rr->ttl == 0 ? 0 : rr->data.SRV.port);
    }
    free(service_name);
  }
}

static void process_dacp_record_list(struct rr_list *records, const char *dacp_id,
                                     struct mdnsd *server) {
  for (; records != NULL; records = records->next)
    process_dacp_record(records->e, dacp_id, server);
}

static void mdns_tinysvcmdns_packet_callback(struct mdns_pkt *pkt, void *userdata) {
  struct mdnsd *server = userdata;
  char *dacp_id = NULL;
  pthread_mutex_lock(&dacp_monitor_lock);
  if (dacp_monitor_id != NULL)
    dacp_id = strdup(dacp_monitor_id);
  pthread_mutex_unlock(&dacp_monitor_lock);

  if (dacp_id == NULL)
    return;

  process_dacp_record_list(pkt->rr_ans, dacp_id, server);
  process_dacp_record_list(pkt->rr_auth, dacp_id, server);
  process_dacp_record_list(pkt->rr_add, dacp_id, server);

  free(dacp_id);
}
#endif

static int mdns_tinysvcmdns_register(char *ap1name, char *ap2name, int port, char **txt_records,
                                     char **secondary_txt_records) {
  struct ifaddrs *ifalist;
  struct ifaddrs *ifa;

  // Thanks to Paul Lietar for this
  // room for name + .local + NULL
  char hostname[100 + 6];
  gethostname(hostname, 99);
  // according to POSIX, this may be truncated without a final NULL !
  hostname[99] = 0;

  // will not work if the hostname doesn't end in .local
  char *hostend = hostname + strlen(hostname);
  if ((strlen(hostname) < strlen(".local")) || (strcmp(hostend - 6, ".local") != 0)) {
    strcat(hostname, ".local");
  }

  if (getifaddrs(&ifalist) < 0) {
    warn("tinysvcmdns: getifaddrs() failed");
    return -1;
  }

  // Use a separate responder for every interface so that host address records stay interface
  // scoped, as they are with Avahi.
  for (ifa = ifalist; ifa != NULL; ifa = ifa->ifa_next) {
    if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK) || (ifa->ifa_addr == NULL) ||
        (ifa->ifa_addr->sa_family != AF_INET))
      continue;
    if ((config.interface != NULL) && (strcmp(config.interface, ifa->ifa_name) != 0))
      continue;

    uint32_t interface_ip = ((struct sockaddr_in *)ifa->ifa_addr)->sin_addr.s_addr;
    struct mdns_tinysvcmdns_server *server = find_server_for_interface(ifa->ifa_name);
    if (server != NULL) {
      struct rr_entry *a_e =
          rr_create_a(create_nlabel(hostname), interface_ip); // TTL should be 120 seconds
      mdnsd_add_rr(server->server, a_e);
      continue;
    }

    struct mdnsd *mdns_server = mdnsd_start();
    if (mdns_server == NULL) {
      warn("tinysvcmdns: mdnsd_start() failed for interface \"%s\"", ifa->ifa_name);
      continue;
    }

    if (mdnsd_add_ipv4_interface(mdns_server, interface_ip) != 0) {
      char errorstring[1024];
      getErrorText(errorstring, sizeof(errorstring));
      warn("tinysvcmdns: could not join the mDNS group on interface \"%s\": %s",
           ifa->ifa_name, errorstring);
      mdnsd_stop(mdns_server);
      continue;
    }

    server = calloc(1, sizeof(*server));
    if (server == NULL) {
      mdnsd_stop(mdns_server);
      freeifaddrs(ifalist);
      return -1;
    }
    server->interface_name = strdup(ifa->ifa_name);
    if (server->interface_name == NULL) {
      free(server);
      mdnsd_stop(mdns_server);
      freeifaddrs(ifalist);
      return -1;
    }
    server->server = mdns_server;
    server->next = servers;
    servers = server;

    mdnsd_set_hostname(mdns_server, hostname, interface_ip); // TTL should be 120 seconds
  }

  if (servers == NULL) {
    warn("tinysvcmdns: no active non-loopback IPv4 interface found");
    freeifaddrs(ifalist);
    return -1;
  }

  for (ifa = ifalist; ifa != NULL; ifa = ifa->ifa_next) {
    if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK) || (ifa->ifa_addr == NULL) ||
        (ifa->ifa_addr->sa_family != AF_INET6))
      continue;
    if ((config.interface != NULL) && (strcmp(config.interface, ifa->ifa_name) != 0))
      continue;

    struct mdns_tinysvcmdns_server *server = find_server_for_interface(ifa->ifa_name);
    if (server != NULL) {
      struct in6_addr *addr = &((struct sockaddr_in6 *)ifa->ifa_addr)->sin6_addr;
      struct rr_entry *aaaa_e =
          rr_create_aaaa(create_nlabel(hostname), addr); // TTL should be 120 seconds
      mdnsd_add_rr(server->server, aaaa_e);
    }
  }

  freeifaddrs(ifalist);

  if (config.regtype == NULL)
    die("tinysvcmdns: regtype is null");

  char *extendedregtype = malloc(strlen(config.regtype) + strlen(".local") + 1);

  if (extendedregtype == NULL)
    die("tinysvcmdns: could not allocated memory to request a Zeroconf service");

  strcpy(extendedregtype, config.regtype);
  strcat(extendedregtype, ".local");

  char *secondary_extendedregtype = NULL;
  if ((ap2name != NULL) && (secondary_txt_records != NULL)) {
    if (config.regtype2 == NULL)
      die("tinysvcmdns: regtype2 is null");

    secondary_extendedregtype = malloc(strlen(config.regtype2) + strlen(".local") + 1);

    if (secondary_extendedregtype == NULL)
      die("tinysvcmdns: could not allocated memory to request a secondary Zeroconf service");

    strcpy(secondary_extendedregtype, config.regtype2);
    strcat(secondary_extendedregtype, ".local");
  }

  struct mdns_tinysvcmdns_server *server = servers;
  while (server != NULL) {
    struct mdns_service *svc = mdnsd_register_svc(
        server->server, ap1name, extendedregtype, port, NULL,
        (const char **)txt_records); // TTL should be 75 minutes, i.e. 4500 seconds
    mdns_service_destroy(svc);

    if (secondary_extendedregtype != NULL) {
      svc = mdnsd_register_svc(server->server, ap2name, secondary_extendedregtype, port, NULL,
                              (const char **)secondary_txt_records);
      mdns_service_destroy(svc);
    }
    server = server->next;
  }

  free(secondary_extendedregtype);
  free(extendedregtype);

  return 0;
}

static void mdns_tinysvcmdns_unregister(void) {
  while (servers != NULL) {
    struct mdns_tinysvcmdns_server *server = servers;
    servers = server->next;
    mdnsd_set_packet_callback(server->server, NULL, NULL);
    mdnsd_stop(server->server);
    free(server->interface_name);
    free(server);
  }
}

#ifdef CONFIG_DACP_CLIENT
static void mdns_tinysvcmdns_dacp_monitor_start(void) {
  struct mdns_tinysvcmdns_server *server = servers;
  while (server != NULL) {
    mdnsd_set_packet_callback(server->server, mdns_tinysvcmdns_packet_callback, server->server);
    server = server->next;
  }
}

static void mdns_tinysvcmdns_dacp_monitor_set_id(const char *dacp_id) {
  pthread_mutex_lock(&dacp_monitor_lock);
  free(dacp_monitor_id);
  dacp_monitor_id = dacp_id == NULL ? NULL : strdup(dacp_id);
  pthread_mutex_unlock(&dacp_monitor_lock);

  if ((dacp_id != NULL) && (strlen(dacp_id) > 0)) {
    struct mdns_tinysvcmdns_server *server = servers;
    while (server != NULL) {
      mdnsd_send_query(server->server, "_dacp._tcp.local", RR_PTR);
      server = server->next;
    }
  }
}

static void mdns_tinysvcmdns_dacp_monitor_stop(void) {
  struct mdns_tinysvcmdns_server *server = servers;
  while (server != NULL) {
    mdnsd_set_packet_callback(server->server, NULL, NULL);
    server = server->next;
  }

  pthread_mutex_lock(&dacp_monitor_lock);
  free(dacp_monitor_id);
  dacp_monitor_id = NULL;
  pthread_mutex_unlock(&dacp_monitor_lock);
}
#endif

mdns_backend mdns_tinysvcmdns = {.name = "tinysvcmdns",
                                 .mdns_register = mdns_tinysvcmdns_register,
                                 .mdns_unregister = mdns_tinysvcmdns_unregister,
#ifdef CONFIG_DACP_CLIENT
                                 .mdns_dacp_monitor_start =
                                     mdns_tinysvcmdns_dacp_monitor_start,
                                 .mdns_dacp_monitor_set_id =
                                     mdns_tinysvcmdns_dacp_monitor_set_id,
                                 .mdns_dacp_monitor_stop =
                                     mdns_tinysvcmdns_dacp_monitor_stop};
#else
                                 .mdns_dacp_monitor_start = NULL,
                                 .mdns_dacp_monitor_set_id = NULL,
                                 .mdns_dacp_monitor_stop = NULL};
#endif
