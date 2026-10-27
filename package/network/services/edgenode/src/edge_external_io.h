#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "edge.pb.h"

/* Values are acquired by dtu.io, the sole owner of GPIO and ADC hardware. */
typedef struct {
    char channel[17];
    iot_edge_v1_ExternalIoKind kind;
    bool available;
    bool controllable;
    int64_t value;
} edge_external_io_channel;

typedef struct {
    bool supported;
    size_t count;
    edge_external_io_channel channels[15];
} edge_external_io_snapshot;

bool edge_external_io_read(const char *socket_path, edge_external_io_snapshot *snapshot);
/* Once started is true, a failure may mean the physical write occurred. */
typedef bool (*edge_external_io_permit_write)(void *context);
bool edge_external_io_write(const char *socket_path, const char *channel, bool value,
                            edge_external_io_permit_write permit, void *context,
                            bool *started, bool *actual);
