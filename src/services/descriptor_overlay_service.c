/*
 * descriptor_overlay_service — the overlays a config lists under
 * `overlays:` (descriptor files, docs/design/overlay-descriptors.md), each
 * drawn as a layer panel.  Attached by the runner when the list is not
 * empty; a descriptor that does not load stops the run before it starts.
 */
#include "descriptor_overlay.h"
#include "sim_config.h"
#include "sim_fwvar.h"
#include "sim_runtime.h"    /* sim_runtime_ui_publish_panel */
#include "sim_service.h"

#include <stdio.h>
#include <stdlib.h>

typedef struct {
    int n;
    descriptor_overlay_t *o[SIM_CONFIG_MAX_OVERLAYS];
} overlays_t;

static void overlays_destroy(sim_runtime_t *sim, void *state) {
    (void)sim;
    overlays_t *s = state;
    if (!s) return;
    for (int i = 0; i < s->n; i++) descriptor_overlay_free(s->o[i]);
    free(s);
}

static int overlays_init(sim_runtime_t *sim, const void *cfg, void **state) {
    (void)sim;
    const sim_normalized_config_t *c = cfg;
    overlays_t *s = calloc(1, sizeof(*s));
    if (!s || !c) { free(s); return -1; }
    rpl_overlay_host_t host = {
        .publish    = sim_runtime_ui_publish_panel,
        .resolve    = sim_fwvar_resolve,
        .read       = sim_fwvar_read,
        .node_count = sim_fwvar_node_count,
        .node_id    = sim_fwvar_node_id,
    };
    for (int i = 0; i < c->overlay_count; i++) {
        char err[512];
        descriptor_overlay_t *o = descriptor_overlay_load(c->overlays[i], host, err, sizeof err);
        if (!o) {
            fprintf(stderr, "overlay: %s\n", err);
            overlays_destroy(NULL, s);
            return -1;
        }
        s->o[s->n++] = o;
    }
    *state = s;
    return 0;
}

static void overlays_on_event(sim_runtime_t *sim, void *state, const sim_observer_event_t *ev) {
    overlays_t *s = state;
    for (int i = 0; i < s->n; i++) descriptor_overlay_on_event(s->o[i], sim, ev);
}

const sim_service_ops_t descriptor_overlay_service_ops = {
    .name     = "overlays",
    .init     = overlays_init,
    .destroy  = overlays_destroy,
    .on_event = overlays_on_event,
};
