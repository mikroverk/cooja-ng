/*
 * abi_probe — a test plugin for the v4 service members (tools/check-plugin.sh).
 *
 * It registers two services that count frames and restarts and print the
 * counts at teardown:
 *   probe-v1  through register_service, with on_restart and wants_frames set
 *             in its ops: the host must ignore both (it copies the five v1
 *             members only), so it sees no restart and no headless frame;
 *   probe-v4  through register_service_v4, with on_restart, and wants_frames
 *             when ABI_PROBE_FRAMES=1 is in the environment.
 */
#include "csim_plugin.h"

#include <stdio.h>
#include <stdlib.h>

typedef struct { const char *name; long frames, restarts; } probe_t;

static int probe_init(sim_runtime_t *sim, const void *cfg, void **state);

static void probe_on_event(sim_runtime_t *sim, void *state, const sim_observer_event_t *ev) {
    (void)sim;
    if (ev->kind == SIM_OBS_PACKET_FRAME) ((probe_t *)state)->frames++;
}

static void probe_on_restart(sim_runtime_t *sim, void *state) {
    (void)sim;
    ((probe_t *)state)->restarts++;
}

static void probe_destroy(sim_runtime_t *sim, void *state) {
    (void)sim;
    probe_t *p = state;
    printf("abi-probe: %s %ld frames, %ld restarts\n", p->name, p->frames, p->restarts);
    free(p);
}

static sim_service_ops_t v1_ops = {
    .name = "probe-v1", .init = probe_init, .destroy = probe_destroy, .on_event = probe_on_event,
    .on_restart = probe_on_restart, .wants_frames = true,
};
static sim_service_ops_t v4_ops = {
    .name = "probe-v4", .init = probe_init, .destroy = probe_destroy, .on_event = probe_on_event,
    .on_restart = probe_on_restart,
};

/* The host calls init through its own copy of the ops, so tell the two
 * services apart by a counter: they are attached in registration order. */
static int probe_init(sim_runtime_t *sim, const void *cfg, void **state) {
    (void)sim; (void)cfg;
    static int n;
    probe_t *p = calloc(1, sizeof *p);
    if (!p) return -1;
    p->name = n++ == 0 ? "probe-v1" : "probe-v4";
    *state = p;
    return 0;
}

int csim_plugin_init(const csim_api_t *api) {
    if (!api || api->version < 4u || !api->registry || !api->registry->register_service_v4)
        return -1;
    const char *f = getenv("ABI_PROBE_FRAMES");
    v4_ops.wants_frames = f && f[0] == '1';
    if (api->registry->register_service(api->reg, &v1_ops) < 0) return -1;
    return api->registry->register_service_v4(api->reg, &v4_ops) < 0 ? -1 : 0;
}
