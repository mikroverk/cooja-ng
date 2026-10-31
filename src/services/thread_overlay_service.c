/*
 * thread_overlay_service — the Thread overlay as a COMPILED-IN plugin.
 *
 * "plugins": ["thread"] draws an OpenThread network on the canvas (roles,
 * partitions, RLOC16s, parent and router links), read out of each node's
 * memory; "thread-packets" draws what the MAC headers alone tell.  All the
 * logic lives in thread_overlay_engine.
 */
#include "thread_overlay_engine.h"
#include "sim_fwvar.h"
#include "sim_runtime.h"    /* sim_runtime_ui_publish_panel */
#include "sim_service.h"

static int start(void **state, bool firmware) {
    thread_overlay_host_t host = {
        .panel_id   = firmware ? "thread" : "thread-packets",
        .title      = firmware ? "Thread" : "Thread (from packets)",
        .publish    = sim_runtime_ui_publish_panel,
        .resolve    = firmware ? sim_fwvar_resolve : NULL,
        .read       = firmware ? sim_fwvar_read : NULL,
        .node_count = sim_fwvar_node_count,
        .node_id    = sim_fwvar_node_id,
    };
    thread_overlay_t *o = thread_overlay_create(host);
    if (!o) return -1;
    *state = o;
    return 0;
}

static int thread_bi_init(sim_runtime_t *sim, const void *cfg, void **state) {
    (void)sim; (void)cfg;
    return start(state, true);
}

static int thread_packets_bi_init(sim_runtime_t *sim, const void *cfg, void **state) {
    (void)sim; (void)cfg;
    return start(state, false);
}

static void thread_bi_on_event(sim_runtime_t *sim, void *state,
                               const sim_observer_event_t *ev) {
    thread_overlay_on_event((thread_overlay_t *)state, sim, ev);
}

static void thread_bi_destroy(sim_runtime_t *sim, void *state) {
    (void)sim;
    thread_overlay_free((thread_overlay_t *)state);
}

const sim_service_ops_t thread_overlay_service_ops = {
    .name     = "thread",
    .init     = thread_bi_init,
    .destroy  = thread_bi_destroy,
    .on_event = thread_bi_on_event,
};

const sim_service_ops_t thread_packets_overlay_service_ops = {
    .name     = "thread-packets",
    .init     = thread_packets_bi_init,
    .destroy  = thread_bi_destroy,
    .on_event = thread_bi_on_event,
};
