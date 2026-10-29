#include "edge_external_io.h"
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

unsigned edge_test_io_read_delay_ms;
const char *edge_test_io_read_signal_path;

bool edge_external_io_read(const char *socket_path, edge_external_io_snapshot *snapshot) {
    (void)socket_path;
    if (edge_test_io_read_signal_path) {
        const int signal = open(edge_test_io_read_signal_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (signal >= 0) close(signal);
    }
    if (edge_test_io_read_delay_ms) usleep(edge_test_io_read_delay_ms * 1000U);
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->supported = true;
    snapshot->count = 2;
    strcpy(snapshot->channels[0].channel, "di0");
    snapshot->channels[0].kind = iot_edge_v1_ExternalIoKind_EXTERNAL_IO_DI;
    snapshot->channels[0].available = true;
    snapshot->channels[0].value = 1;
    strcpy(snapshot->channels[1].channel,"do0");
    snapshot->channels[1].kind=iot_edge_v1_ExternalIoKind_EXTERNAL_IO_DO;
    snapshot->channels[1].available=true;
    snapshot->channels[1].controllable=true;
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
