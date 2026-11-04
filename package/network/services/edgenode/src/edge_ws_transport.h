#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <ev.h>
#include <libwebsockets.h>

typedef struct edge_ws_bridge edge_ws_bridge;
typedef struct {
    edge_ws_bridge *worker;
} edge_ws_transport;

/* Each platform owns a network worker; callbacks run only on the caller loop.
 * DNS/TLS/compression and socket servicing never execute in the main process. */
bool edge_ws_transport_connect(edge_ws_transport *transport, struct ev_loop *loop,
                               const char *url, size_t message_limit, void *user,
                               void (*opened)(void *),
                               void (*received)(void *, void *, size_t, bool),
                               void (*closed)(void *, int, const char *));
bool edge_ws_transport_send(edge_ws_transport *transport, const void *data, size_t size);
/* Accepted bytes stay outstanding across IPC until LWS accepts the write. */
size_t edge_ws_transport_queued_bytes(const edge_ws_transport *transport);
/* Borrowed socket, valid only during opened(); used for traffic accounting. */
int edge_ws_transport_socket_fd(const edge_ws_transport *transport);
void edge_ws_transport_close(edge_ws_transport *transport, uint16_t code, const char *reason);
void edge_ws_transport_destroy(edge_ws_transport *transport);
