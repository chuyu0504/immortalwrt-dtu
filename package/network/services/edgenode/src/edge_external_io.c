#include "edge_external_io.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <libubox/blobmsg.h>
#include <libubus.h>

enum { STATUS_SUPPORTED, STATUS_ERROR, STATUS_DI, STATUS_DO, STATUS_AI, STATUS_COUNT };
static const struct blobmsg_policy status_policy[STATUS_COUNT] = {
    [STATUS_SUPPORTED] = {.name = "supported", .type = BLOBMSG_TYPE_BOOL},
    [STATUS_ERROR] = {.name = "config_error", .type = BLOBMSG_TYPE_STRING},
    [STATUS_DI] = {.name = "di", .type = BLOBMSG_TYPE_ARRAY},
    [STATUS_DO] = {.name = "do", .type = BLOBMSG_TYPE_ARRAY},
    [STATUS_AI] = {.name = "ai", .type = BLOBMSG_TYPE_ARRAY},
};
enum { CHANNEL_ID, CHANNEL_NAME, CHANNEL_AVAILABLE, CHANNEL_CONTROLLABLE, CHANNEL_VALUE, CHANNEL_COUNT };
static const struct blobmsg_policy channel_policy[CHANNEL_COUNT] = {
    [CHANNEL_ID] = {.name = "id", .type = BLOBMSG_TYPE_STRING},
    [CHANNEL_NAME] = {.name = "name", .type = BLOBMSG_TYPE_STRING},
    [CHANNEL_AVAILABLE] = {.name = "available", .type = BLOBMSG_TYPE_BOOL},
    [CHANNEL_CONTROLLABLE] = {.name = "controllable", .type = BLOBMSG_TYPE_BOOL},
    [CHANNEL_VALUE] = {.name = "value", .type = BLOBMSG_TYPE_UNSPEC},
};

typedef struct {
    edge_external_io_snapshot snapshot;
    bool received;
    bool valid;
} read_result;

static bool read_channels(read_result *result, struct blob_attr *array,
                          iot_edge_v1_ExternalIoKind kind, bool config_valid) {
    struct blob_attr *entry;
    size_t remaining;
    if (!array) return false;
    blobmsg_for_each_attr(entry, array, remaining) {
        if (blobmsg_type(entry) != BLOBMSG_TYPE_TABLE || result->snapshot.count == 15U) return false;
        struct blob_attr *fields[CHANNEL_COUNT];
        blobmsg_parse(channel_policy, CHANNEL_COUNT, fields, blobmsg_data(entry), blobmsg_data_len(entry));
        struct blob_attr *id = kind == iot_edge_v1_ExternalIoKind_EXTERNAL_IO_AI ? fields[CHANNEL_NAME] : fields[CHANNEL_ID];
        if (!id || !fields[CHANNEL_AVAILABLE]) return false;
        const char *name = blobmsg_get_string(id);
        if (!*name || strlen(name) > 16U) return false;
        for (size_t i = 0; i < result->snapshot.count; ++i)
            if (!strcmp(result->snapshot.channels[i].channel, name)) return false;
        edge_external_io_channel *channel = &result->snapshot.channels[result->snapshot.count++];
        memcpy(channel->channel, name, strlen(name) + 1U);
        channel->kind = kind;
        channel->available = blobmsg_get_bool(fields[CHANNEL_AVAILABLE]);
        channel->controllable = kind == iot_edge_v1_ExternalIoKind_EXTERNAL_IO_DO && config_valid &&
            fields[CHANNEL_CONTROLLABLE] && blobmsg_get_bool(fields[CHANNEL_CONTROLLABLE]);
        if (!channel->available) continue;
        if (!fields[CHANNEL_VALUE]) return false;
        if (kind == iot_edge_v1_ExternalIoKind_EXTERNAL_IO_AI) {
            if (blobmsg_type(fields[CHANNEL_VALUE]) != BLOBMSG_TYPE_STRING) return false;
            const char *text = blobmsg_get_string(fields[CHANNEL_VALUE]);
            char *end = NULL;
            errno = 0;
            long long value = strtoll(text, &end, 10);
            if (errno || !*text || !end || *end) return false;
            channel->value = (int64_t)value;
        } else {
            if (blobmsg_type(fields[CHANNEL_VALUE]) != BLOBMSG_TYPE_BOOL || blobmsg_get_u8(fields[CHANNEL_VALUE]) > 1U) return false;
            channel->value = blobmsg_get_u8(fields[CHANNEL_VALUE]);
        }
    }
    return remaining == 0;
}

static void read_reply(struct ubus_request *request, int type, struct blob_attr *message) {
    (void)type;
    read_result *result = request->priv;
    result->received = true;
    if (!message) return;
    struct blob_attr *fields[STATUS_COUNT];
    blobmsg_parse(status_policy, STATUS_COUNT, fields, blob_data(message), blob_len(message));
    if (!fields[STATUS_SUPPORTED]) return;
    result->snapshot.supported = blobmsg_get_bool(fields[STATUS_SUPPORTED]);
    if (!result->snapshot.supported) { result->valid = true; return; }
    const bool config_valid = !fields[STATUS_ERROR] || !blobmsg_get_string(fields[STATUS_ERROR])[0];
    result->valid = read_channels(result, fields[STATUS_DI], iot_edge_v1_ExternalIoKind_EXTERNAL_IO_DI, config_valid) &&
        read_channels(result, fields[STATUS_DO], iot_edge_v1_ExternalIoKind_EXTERNAL_IO_DO, config_valid) &&
        read_channels(result, fields[STATUS_AI], iot_edge_v1_ExternalIoKind_EXTERNAL_IO_AI, config_valid);
}

bool edge_external_io_read(const char *socket_path, edge_external_io_snapshot *snapshot) {
    if (!snapshot) return false;
    memset(snapshot, 0, sizeof(*snapshot));
    struct ubus_context *bus = ubus_connect(socket_path);
    if (!bus) return false;
    uint32_t id = 0;
    read_result result = {0};
    const int status = ubus_lookup_id(bus, "dtu.io", &id) ||
        ubus_invoke(bus, id, "status", NULL, read_reply, &result, 1000);
    ubus_free(bus);
    if (status || !result.received || !result.valid) return false;
    *snapshot = result.snapshot;
    return true;
}

typedef struct { bool received; bool value; } write_result;
static void write_reply(struct ubus_request *request, int type, struct blob_attr *message) {
    (void)type;
    static const struct blobmsg_policy policy = {.name = "value", .type = BLOBMSG_TYPE_BOOL};
    struct blob_attr *value = NULL;
    write_result *result = request->priv;
    if (!message) return;
    blobmsg_parse(&policy, 1, &value, blob_data(message), blob_len(message));
    if (value && blobmsg_get_u8(value) <= 1U) {
        result->received = true;
        result->value = blobmsg_get_bool(value);
    }
}

bool edge_external_io_write(const char *socket_path, const char *channel, bool value,
                            edge_external_io_permit_write permit, void *context,
                            bool *started, bool *actual) {
    if (!channel || !permit || !started || !actual) return false;
    *started = false;
    struct ubus_context *bus = ubus_connect(socket_path);
    if (!bus) return false;
    uint32_t id = 0;
    if (ubus_lookup_id(bus, "dtu.io", &id)) { ubus_free(bus); return false; }
    struct blob_buf request = {0};
    blob_buf_init(&request, 0);
    blobmsg_add_string(&request, "channel", channel);
    blobmsg_add_u8(&request, "value", value);
    write_result result = {0};
    if (!permit(context)) { blob_buf_free(&request); ubus_free(bus); return false; }
    *started = true;
    const int status = ubus_invoke(bus, id, "set_output", request.head, write_reply, &result, 1000);
    blob_buf_free(&request);
    ubus_free(bus);
    if (status || !result.received) return false;
    *actual = result.value;
    return result.value == value;
}
