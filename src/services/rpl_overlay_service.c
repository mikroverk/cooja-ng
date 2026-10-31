/*
 * rpl_overlay_service — the RPL overlay as a COMPILED-IN plugin.
 *
 * Registered in builtin_services.c, so a config selects it by name —
 * "plugins": ["rpl"] — and the web UI draws the DODAG on the canvas.
 * "rpl-packets" is the same overlay with the firmware reads left out: what
 * the packets alone tell (a sniffer's view), and a check on that source.  All
 * the logic lives in rpl_overlay_engine; this file hands it the kernel's
 * sinks directly (a built-in may name kernel symbols), where a .so would
 * hand it the v3 ui and v4 fw vtables of its csim_api.
 */
#include "rpl_overlay_engine.h"
#include "sim_fwvar.h"
#include "sim_runtime.h"    /* sim_runtime_ui_publish_panel */
#include "sim_service.h"

static int start(void **state, bool firmware) {
    rpl_overlay_host_t host = {
        .panel_id   = firmware ? "rpl" : "rpl-packets",
        .title      = firmware ? "RPL" : "RPL (from packets)",
        .publish    = sim_runtime_ui_publish_panel,
        .resolve    = firmware ? sim_fwvar_resolve : NULL,
        .read       = firmware ? sim_fwvar_read : NULL,
        .node_count = sim_fwvar_node_count,
        .node_id    = sim_fwvar_node_id,
    };
    rpl_overlay_t *o = rpl_overlay_create(host);
    if (!o) return -1;
    *state = o;
    return 0;
}

static int rpl_bi_init(sim_runtime_t *sim, const void *cfg, void **state) {
    (void)sim; (void)cfg;
    return start(state, true);
}

static int rpl_packets_bi_init(sim_runtime_t *sim, const void *cfg, void **state) {
    (void)sim; (void)cfg;
    return start(state, false);
}

static void rpl_bi_on_event(sim_runtime_t *sim, void *state,
                            const sim_observer_event_t *ev) {
    rpl_overlay_on_event((rpl_overlay_t *)state, sim, ev);
}

static void rpl_bi_destroy(sim_runtime_t *sim, void *state) {
    (void)sim;
    rpl_overlay_free((rpl_overlay_t *)state);
}

const sim_service_ops_t rpl_overlay_service_ops = {
    .name     = "rpl",
    .init     = rpl_bi_init,
    .destroy  = rpl_bi_destroy,
    .on_event = rpl_bi_on_event,
};

const sim_service_ops_t rpl_packets_overlay_service_ops = {
    .name     = "rpl-packets",
    .init     = rpl_packets_bi_init,
    .destroy  = rpl_bi_destroy,
    .on_event = rpl_bi_on_event,
};
