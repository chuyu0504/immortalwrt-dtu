#include "edge_external_io.h"
#include <string.h>

bool edge_external_io_read(const char *socket_path, edge_external_io_snapshot *snapshot) {
    (void)socket_path;
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->supported = true;
    snapshot->count = 1;
    strcpy(snapshot->channels[0].channel, "di0");
    snapshot->channels[0].kind = iot_edge_v1_ExternalIoKind_EXTERNAL_IO_DI;
    snapshot->channels[0].available = true;
    snapshot->channels[0].value = 1;
    return true;
}

bool edge_external_io_write(const char *socket_path, const char *channel, bool value,
                            edge_external_io_permit_write permit, void *context, bool *started, bool *actual) {
    (void)socket_path; (void)channel;
    if (!permit(context)) { *started = false; return false; }
    *started = true;
    *actual = value;
    return true;
}
