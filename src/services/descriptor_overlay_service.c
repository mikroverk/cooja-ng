/*
 * descriptor_overlay_service — the overlays a config lists under
 * `overlays:` (descriptor files, docs/design/overlay-descriptors.md), each
 * drawn as a layer panel.  Attached by the runner when the list is not
 * empty; a descriptor that does not load stops the run before it starts.
 */
#include "descriptor_overlay.h"
#include "descriptor_overlay_service.h"
#include "sim_config.h"
#include "sim_fwvar.h"
#include "sim_runtime.h"    /* sim_runtime_ui_publish_panel */
#include "sim_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

typedef struct {
    int n;
    descriptor_overlay_t *o[SIM_CONFIG_MAX_OVERLAYS];
} overlays_t;

/* The one instance (a run has one service host), for the UI's commands. */
static overlays_t *g_overlays;

static rpl_overlay_host_t kernel_host(void) {
    return (rpl_overlay_host_t){
        .publish    = sim_runtime_ui_publish_panel,
        .resolve    = sim_fwvar_resolve,
        .read       = sim_fwvar_read,
        .node_count = sim_fwvar_node_count,
        .node_id    = sim_fwvar_node_id,
    };
}

static void overlays_destroy(sim_runtime_t *sim, void *state) {
    (void)sim;
    overlays_t *s = state;
    if (!s) return;
    for (int i = 0; i < s->n; i++) descriptor_overlay_free(s->o[i]);
    if (g_overlays == s) g_overlays = NULL;
    free(s);
}

static int overlays_init(sim_runtime_t *sim, const void *cfg, void **state) {
    (void)sim;
    const sim_normalized_config_t *c = cfg;      /* NULL: none yet (the editor adds them) */
    overlays_t *s = calloc(1, sizeof(*s));
    if (!s) return -1;
    for (int i = 0; c && i < c->overlay_count; i++) {
        char err[512];
        descriptor_overlay_t *o = descriptor_overlay_load(c->overlays[i], kernel_host(), err, sizeof err);
        if (!o) {
            fprintf(stderr, "overlay: %s\n", err);
            overlays_destroy(NULL, s);
            return -1;
        }
        s->o[s->n++] = o;
    }
    g_overlays = s;
    *state = s;
    return 0;
}

int descriptor_overlays_apply(sim_runtime_t *sim, const char *text, char *id, size_t idsz,
                              char *err, size_t errsz) {
    if (!g_overlays && sim_service_attach(sim, &descriptor_overlay_service_ops, NULL) < 0) {
        snprintf(err, errsz, "no room for the overlay service");
        return -1;
    }
    descriptor_overlay_t *o = descriptor_overlay_load_text("overlay", text, strlen(text),
                                                           kernel_host(), err, errsz);
    if (!o) return -1;
    overlays_t *s = g_overlays;
    int at = -1;
    for (int i = 0; i < s->n; i++)
        if (strcmp(descriptor_overlay_id(s->o[i]), descriptor_overlay_id(o)) == 0) at = i;
    if (at < 0) {
        if (s->n >= SIM_CONFIG_MAX_OVERLAYS) {
            snprintf(err, errsz, "at most %d overlays", SIM_CONFIG_MAX_OVERLAYS);
            descriptor_overlay_free(o);
            return -1;
        }
        at = s->n++;
    } else {
        descriptor_overlay_free(s->o[at]);
    }
    s->o[at] = o;
    descriptor_overlay_refresh(o, sim, sim_runtime_now_ns(sim));   /* drawn now, paused or not */
    snprintf(id, idsz, "%s", descriptor_overlay_id(o));
    return 0;
}

int descriptor_overlays_remove(sim_runtime_t *sim, const char *id) {
    overlays_t *s = g_overlays;
    for (int i = 0; s && i < s->n; i++) {
        if (strcmp(descriptor_overlay_id(s->o[i]), id) != 0) continue;
        sim_runtime_ui_remove_panel(sim, id);
        descriptor_overlay_free(s->o[i]);
        for (int k = i + 1; k < s->n; k++) s->o[k - 1] = s->o[k];
        s->n--;
        return 0;
    }
    return -1;
}

char *descriptor_overlays_list_json(void) {
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; g_overlays && i < g_overlays->n; i++) {
        const descriptor_overlay_t *o = g_overlays->o[i];
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "id", descriptor_overlay_id(o));
        cJSON_AddStringToObject(e, "title", descriptor_overlay_title(o));
        cJSON_AddStringToObject(e, "yaml", descriptor_overlay_source(o) ? descriptor_overlay_source(o) : "");
        cJSON_AddItemToArray(arr, e);
    }
    char *txt = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return txt;
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
