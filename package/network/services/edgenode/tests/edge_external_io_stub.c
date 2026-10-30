#include "edge_external_io.h"
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

unsigned edge_test_io_read_delay_ms;
const char *edge_test_io_read_signal_path;
int64_t edge_test_io_ai_microamps = 12345;

bool edge_external_io_read(const char *socket_path, edge_external_io_snapshot *snapshot) {
    (void)socket_path;
    if (edge_test_io_read_signal_path) {
        const int signal = open(edge_test_io_read_signal_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (signal >= 0) close(signal);
    }
    if (edge_test_io_read_delay_ms) usleep(edge_test_io_read_delay_ms * 1000U);
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->supported = true;
    snapshot->count = 3;
    strcpy(snapshot->channels[0].channel, "di0");
    snapshot->channels[0].kind = iot_edge_v1_ExternalIoKind_EXTERNAL_IO_DI;
    snapshot->channels[0].available = true;
    snapshot->channels[0].value = 1;
    strcpy(snapshot->channels[1].channel,"do0");
    snapshot->channels[1].kind=iot_edge_v1_ExternalIoKind_EXTERNAL_IO_DO;
    snapshot->channels[1].available=true;
    snapshot->channels[1].controllable=true;
    strcpy(snapshot->channels[2].channel, "AI0");
    snapshot->channels[2].kind = iot_edge_v1_ExternalIoKind_EXTERNAL_IO_AI;
    snapshot->channels[2].available = true;
    snapshot->channels[2].value = edge_test_io_ai_microamps;
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
