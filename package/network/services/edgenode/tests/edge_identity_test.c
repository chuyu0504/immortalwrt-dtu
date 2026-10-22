/* Exercise the real main-loop reload, with config storage and sessions stubbed. */
#include <assert.h>

#define main edgenode_main
#include "../src/main.c"
#undef main

static unsigned loads, starts, stops, progress;
static unsigned unavailable_reads;
static bool initially_ready, remain_empty;
static struct ev_timer finish;

bool edge_config_load(edge_app_config *config, char *error, size_t error_size) {
    (void)error;
    (void)error_size;
    memset(config, 0, sizeof(*config));
    ++loads;
    if (!initially_ready && loads > 1 && loads <= unavailable_reads + 1)
        return false;
    if (initially_ready || (!remain_empty && loads > unavailable_reads + 1))
        strcpy(config->imei, "490154203237518");
    config->platform_count = 2;
    config->platforms[0].id[0] = 1;
    config->platforms[1].id[0] = 2;
    return true;
}

bool edge_ws_app_init(edge_ws_app *app, struct ev_loop *loop,
                      const edge_app_config *config) {
    app->loop = loop;
    app->config = config;
    return true;
}

static void finish_loop(struct ev_loop *loop, struct ev_timer *timer, int events) {
    (void)timer;
    (void)events;
    if (remain_empty)
        raise(SIGTERM);
    else
        ev_break(loop, EVBREAK_ALL);
}

void edge_ws_app_start(edge_ws_app *app) {
    ++starts;
    assert(app->config->platform_count == 2);
    assert(app->config->platforms[0].id[0] == 1);
    assert(app->config->platforms[1].id[0] == 2);
    assert(app->config->imei[0]);
    ev_timer_init(&finish, finish_loop, .1, 0);
    ev_timer_start(app->loop, &finish);
}

void edge_ws_app_stop(edge_ws_app *app) {
    ++stops;
    ev_timer_stop(app->loop, &finish);
}

void edge_process_report_progress(void) {
    ++progress;
}

void edge_log_local_fault(const uint8_t platform_id[16], const uint8_t device_id[16],
                          unsigned protocol, edge_local_fault fault, unsigned reason) {
    (void)platform_id; (void)device_id; (void)protocol; (void)fault; (void)reason;
    assert(!"unexpected startup fault");
}

int main(void) {
    char *argv[] = {"edgenode", NULL};
    assert(edgenode_main(1, argv) == 0);
    assert(loads == 3 && starts == 1 && stops == 1 && progress >= 2);

    loads = starts = stops = 0;
    unavailable_reads = 1;
    assert(edgenode_main(1, argv) == 0);
    assert(loads == 4 && starts == 1 && stops == 1);

    loads = starts = stops = 0;
    initially_ready = true;
    assert(edgenode_main(1, argv) == 0);
    assert(loads == 1 && starts == 1 && stops == 1);

    loads = starts = stops = 0;
    initially_ready = false;
    remain_empty = true;
    unavailable_reads = 0;
    progress = 0;
    ev_timer_init(&finish, finish_loop, 1.2, 0);
    ev_timer_start(EV_DEFAULT, &finish);
    assert(edgenode_main(1, argv) == 0);
    ev_timer_stop(EV_DEFAULT, &finish);
    assert(loads == 2 && starts == 0 && stops == 0 && progress >= 2);
    puts("delayed identity, transient config error, existing identity and stoppable waiting passed");
    return 0;
}
