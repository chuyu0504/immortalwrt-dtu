#include "edge_memory.h"
#include "edge_ws_transport.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    edge_ws_transport transport;
    edge_memory_outbox outbox;
    struct ev_loop *loop;
    struct ev_timer reconnect;
    const char *url;
    unsigned connections;
    bool failed;
    bool acknowledged;
} recovery_fixture;

static const uint8_t result_id[16] = {1U};
static const uint8_t result[] = "command-result";
static uint8_t filler[16384];

static void fail(recovery_fixture *fixture, const char *message) {
    fprintf(stderr, "%s\n", message);
    fixture->failed = true;
    ev_break(fixture->loop, EVBREAK_ALL);
}

static void opened(void *user) {
    recovery_fixture *fixture = user;
    ++fixture->connections;
    if (fixture->connections != 2U)
        return;
    const edge_memory_message *message = edge_memory_outbox_next(&fixture->outbox, 2U);
    if (message == NULL || !edge_ws_transport_send(&fixture->transport,
                                                    message->payload, message->payload_size))
        fail(fixture, "pending result was not replayed on the new connection");
}

static void received(void *user, void *data, size_t size, bool binary) {
    recovery_fixture *fixture = user;
    if (!binary) {
        fail(fixture, "unexpected text frame");
        return;
    }
    if (fixture->connections == 1U && size == 4U && !memcmp(data, "fill", 4U)) {
        if (!edge_memory_outbox_put_priority(&fixture->outbox, result_id, result,
                                             sizeof(result) - 1U, true) ||
            edge_memory_outbox_next(&fixture->outbox, 1U) == NULL) {
            fail(fixture, "cannot preserve the pending result");
            return;
        }
        for (unsigned index = 0; index < 15U; ++index) {
            if (!edge_ws_transport_send(&fixture->transport, filler, sizeof(filler))) {
                fail(fixture, "cannot fill the socket send window");
                return;
            }
        }
        if (!edge_ws_transport_send(&fixture->transport, result, sizeof(result) - 1U))
            fail(fixture, "cannot queue the initial result");
    } else if (fixture->connections == 2U && size == 3U && !memcmp(data, "ack", 3U)) {
        fixture->acknowledged = edge_memory_outbox_ack(&fixture->outbox, result_id);
        if (!fixture->acknowledged)
            fail(fixture, "replayed result could not be acknowledged");
        else
            edge_ws_transport_close(&fixture->transport, 1000, "verified");
    } else {
        fail(fixture, "unexpected recovery frame");
    }
}

static void closed(void *user, int code, const char *reason) {
    recovery_fixture *fixture = user;
    (void)reason;
    if (fixture->connections == 1U && code != 1000) {
        edge_memory_outbox_reset(&fixture->outbox);
        ev_timer_set(&fixture->reconnect, 0.01, 0.0);
        ev_timer_start(fixture->loop, &fixture->reconnect);
    } else if (fixture->connections == 2U && code == 1000 &&
               fixture->acknowledged && fixture->outbox.count == 0U) {
        ev_break(fixture->loop, EVBREAK_ALL);
    } else {
        fail(fixture, "unexpected connection close during recovery");
    }
}

static void reconnect(struct ev_loop *loop, struct ev_timer *timer, int events) {
    (void)loop;
    (void)events;
    recovery_fixture *fixture = timer->data;
    if (!edge_ws_transport_connect(&fixture->transport, fixture->loop, fixture->url,
                                   sizeof(filler), fixture, opened, received, closed))
        fail(fixture, "reconnection failed");
}

static void timed_out(struct ev_loop *loop, struct ev_timer *timer, int events) {
    (void)loop;
    (void)events;
    fail(timer->data, "blocked connection did not recover in time");
}

int main(int argc, char **argv) {
    if (argc != 2)
        return 2;
    recovery_fixture fixture = {.url = argv[1]};
    fixture.loop = ev_loop_new(0);
    if (fixture.loop == NULL)
        return 2;
    uint32_t random = 12345U;
    for (size_t index = 0; index < sizeof(filler); ++index) {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        filler[index] = (uint8_t)random;
    }
    edge_memory_outbox_init(&fixture.outbox, 1024U);
    ev_timer_init(&fixture.reconnect, reconnect, 0.0, 0.0);
    fixture.reconnect.data = &fixture;
    struct ev_timer deadline;
    ev_timer_init(&deadline, timed_out, 24.0, 0.0);
    deadline.data = &fixture;
    ev_timer_start(fixture.loop, &deadline);
    if (!edge_ws_transport_connect(&fixture.transport, fixture.loop, fixture.url,
                                   sizeof(filler), &fixture, opened, received, closed))
        fixture.failed = true;
    else
        ev_run(fixture.loop, 0);
    edge_ws_transport_destroy(&fixture.transport);
    ev_timer_stop(fixture.loop, &fixture.reconnect);
    ev_timer_stop(fixture.loop, &deadline);
    ev_run(fixture.loop, EVRUN_NOWAIT);
    ev_loop_destroy(fixture.loop);
    edge_memory_outbox_free(&fixture.outbox);
    return fixture.failed || !fixture.acknowledged ? 1 : 0;
}
