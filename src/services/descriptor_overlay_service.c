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

/* The live instance, for the UI's commands.  The service is attached once
 * (by the runner for a config's overlays, or by the first Apply) and stays
 * attached across restarts, so the overlays drawn now (the config's, as the
 * editor may have changed them, and any added) carry on into the new run:
 * on_restart tells each one. */
static overlays_t *g_overlays;

/* The panel ids the built-in (C) overlays and panels publish under: a
 * descriptor with one of these would overwrite theirs every refresh. */
static const char *const builtin_panels[] = { "rpl", "rpl-packets", "thread", "thread-packets",
                                              "energest", NULL };

static bool builtin_panel(const char *id) {
    for (int i = 0; builtin_panels[i]; i++)
        if (strcmp(builtin_panels[i], id) == 0) return true;
    return false;
}

static int index_of(const overlays_t *s, const char *id) {
    for (int i = 0; s && i < s->n; i++)
        if (strcmp(descriptor_overlay_id(s->o[i]), id) == 0) return i;
    return -1;
}

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
        const char *id = o ? descriptor_overlay_id(o) : NULL;
        if (o && builtin_panel(id))
            snprintf(err, sizeof err, "%s: overlay id '%s' is a built-in panel's", c->overlays[i], id);
        else if (o && index_of(s, id) >= 0)
            snprintf(err, sizeof err, "%s: overlay id '%s' is taken by another descriptor in the config", c->overlays[i], id);
        else if (o) { s->o[s->n++] = o; continue; }
        descriptor_overlay_free(o);
        fprintf(stderr, "overlay: %s\n", err);
        overlays_destroy(NULL, s);
        return -1;
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
    const char *oid = descriptor_overlay_id(o);
    int at = index_of(s, oid);
    if (at < 0) {
        /* A new panel: its id must be free, and the page's table must have
         * room (a publish to a full one is dropped, nothing drawn). */
        int why = builtin_panel(oid) || sim_runtime_ui_has_panel(sim, oid) ? 1
                : !sim_runtime_ui_panel_room(sim) ? 2 : s->n >= SIM_CONFIG_MAX_OVERLAYS ? 3 : 0;
        if (why) {
            if (why == 1) snprintf(err, errsz, "overlay '%s': the id is another panel's", oid);
            else if (why == 2) snprintf(err, errsz, "overlay '%s' has no room: the page shows at most %d panels; Remove one first",
                                        oid, SIM_RUNTIME_MAX_PANELS);
            else snprintf(err, errsz, "overlay '%s' has no room: at most %d overlays", oid, SIM_CONFIG_MAX_OVERLAYS);
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
    int i = index_of(s, id);
    if (i < 0) return -1;
    sim_runtime_ui_remove_panel(sim, id);
    descriptor_overlay_free(s->o[i]);
    for (int k = i + 1; k < s->n; k++) s->o[k - 1] = s->o[k];
    s->n--;
    return 0;
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

static void overlays_on_restart(sim_runtime_t *sim, void *state) {
    overlays_t *s = state;
    for (int i = 0; i < s->n; i++) descriptor_overlay_restart(s->o[i], sim);
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
    .on_restart = overlays_on_restart,
};
