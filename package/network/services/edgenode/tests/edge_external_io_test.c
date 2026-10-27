#include "edge_external_io.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <libubox/blobmsg.h>
#include <libubox/uloop.h>
#include <libubus.h>

static pid_t daemon_pid, service_pid;
static char socket_directory[] = "/tmp/edgenode-io-XXXXXX";
static char socket_path[128];
static void cleanup(void) {
    if (service_pid > 0) { kill(service_pid, SIGTERM); waitpid(service_pid, NULL, 0); }
    if (daemon_pid > 0) { kill(daemon_pid, SIGTERM); waitpid(daemon_pid, NULL, 0); }
    if (socket_path[0]) unlink(socket_path);
    if (socket_path[0]) rmdir(socket_directory);
}
static void require(bool condition, const char *message) {
    if (!condition) { fprintf(stderr, "external IO test: %s\n", message); exit(1); }
}
static bool output_value;
static struct blob_buf reply;

static int status_method(struct ubus_context *ctx, struct ubus_object *object,
                         struct ubus_request_data *request, const char *method,
                         struct blob_attr *message) {
    (void)object; (void)method; (void)message;
    blob_buf_init(&reply, 0);
    blobmsg_add_u8(&reply, "supported", true);
    blobmsg_add_string(&reply, "config_error", "");
    const char *groups[] = {"di", "do", "ai"};
    for (unsigned i = 0; i < 3; ++i) {
        void *array = blobmsg_open_array(&reply, groups[i]);
        void *item = blobmsg_open_table(&reply, NULL);
        blobmsg_add_string(&reply, i == 2 ? "name" : "id", i == 0 ? "di0" : i == 1 ? "do0" : "AI0");
        blobmsg_add_u8(&reply, "available", true);
        if (i == 1) blobmsg_add_u8(&reply, "controllable", true);
        if (i == 2) blobmsg_add_string(&reply, "value", "12500");
        else blobmsg_add_u8(&reply, "value", i == 0 ? true : output_value);
        blobmsg_close_table(&reply, item);
        blobmsg_close_array(&reply, array);
    }
    ubus_send_reply(ctx, request, reply.head);
    return UBUS_STATUS_OK;
}

static int set_method(struct ubus_context *ctx, struct ubus_object *object,
                      struct ubus_request_data *request, const char *method,
                      struct blob_attr *message) {
    (void)object; (void)method;
    static const struct blobmsg_policy policy[] = {
        {.name = "channel", .type = BLOBMSG_TYPE_STRING},
        {.name = "value", .type = BLOBMSG_TYPE_BOOL},
    };
    struct blob_attr *fields[2];
    blobmsg_parse(policy, 2, fields, blob_data(message), blob_len(message));
    if (!fields[0] || !fields[1] || strcmp(blobmsg_get_string(fields[0]), "do0")) return UBUS_STATUS_INVALID_ARGUMENT;
    output_value = blobmsg_get_bool(fields[1]);
    blob_buf_init(&reply, 0);
    blobmsg_add_u8(&reply, "value", output_value);
    ubus_send_reply(ctx, request, reply.head);
    return UBUS_STATUS_OK;
}

static const struct blobmsg_policy set_policy[] = {
    {.name = "channel", .type = BLOBMSG_TYPE_STRING},
    {.name = "value", .type = BLOBMSG_TYPE_BOOL},
};
static const struct ubus_method methods[] = {
    UBUS_METHOD_NOARG("status", status_method),
    UBUS_METHOD("set_output", set_method, set_policy),
};
static struct ubus_object_type object_type = UBUS_OBJECT_TYPE("dtu.io", methods);
static struct ubus_object object = {.name = "dtu.io", .type = &object_type, .methods = methods, .n_methods = 2};

static bool allow_write(void *context) { (void)context; return true; }
static bool deny_write(void *context) { (void)context; return false; }

int main(int argc, char **argv) {
    require(argc == 2, "expected ubusd binary");
    require(mkdtemp(socket_directory) != NULL, "cannot create temporary socket directory");
    snprintf(socket_path, sizeof(socket_path), "%s/ubus.sock", socket_directory);
    atexit(cleanup);
    daemon_pid = fork();
    require(daemon_pid >= 0, "cannot start ubusd");
    if (!daemon_pid) { execl(argv[1], argv[1], "-s", socket_path, (char *)NULL); _exit(127); }
    struct ubus_context *probe = NULL;
    for (unsigned i = 0; i < 100 && !probe; ++i) { usleep(20000); probe = ubus_connect(socket_path); }
    require(probe != NULL, "ubusd did not start");
    ubus_free(probe);
    int ready[2]; require(pipe(ready) == 0, "pipe failed");
    service_pid = fork();
    require(service_pid >= 0, "cannot start fake IO service");
    if (!service_pid) {
        close(ready[0]);
        if (uloop_init()) _exit(1);
        struct ubus_context *bus = ubus_connect(socket_path);
        if (!bus || ubus_add_object(bus, &object)) _exit(2);
        ubus_add_uloop(bus);
        if (write(ready[1], "1", 1) != 1) _exit(3);
        close(ready[1]);
        uloop_run();
        ubus_free(bus); uloop_done(); _exit(0);
    }
    close(ready[1]); char marker;
    require(read(ready[0], &marker, 1) == 1, "fake IO service did not start");
    close(ready[0]);
    edge_external_io_snapshot snapshot;
    require(edge_external_io_read(socket_path, &snapshot), "cannot read status");
    require(snapshot.supported && snapshot.count == 3 && snapshot.channels[0].value == 1 &&
            snapshot.channels[1].controllable && snapshot.channels[2].value == 12500,
            "channel values or capabilities changed");
    bool started = false, actual = false;
    require(!edge_external_io_write(socket_path, "do0", true, deny_write, NULL, &started, &actual) && !started,
            "revoked write must not be invoked");
    require(edge_external_io_write(socket_path, "do0", true, allow_write, NULL, &started, &actual) && started && actual,
            "output write/readback failed");
    require(edge_external_io_read(socket_path, &snapshot) && snapshot.channels[1].value == 1,
            "output status did not change");
    require(!edge_external_io_write(socket_path, "do7", false, allow_write, NULL, &started, &actual) && started,
            "invalid output must fail after invocation");
    puts("external IO ubus test passed");
    return 0;
}
