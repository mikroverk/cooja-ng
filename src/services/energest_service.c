/*
 * energest_service — the energest plugin, COMPILED IN.
 *
 * Registered in the static registry, so a config selects it by bare name
 * ("plugins": ["energest"]); its options are the config's `energest:`
 * section (docs/energest.md).  All accounting lives in energest_engine.c;
 * this file is the sink: the kernel's panel and frame queues, the node
 * descriptions from the runner's control API, stdout for the summary.
 *
 * Attaching it turns on power-state tracking (sim_runtime_set_power_states):
 * the models then report their exact CPU, radio and peripheral states.  No
 * other service does, so runs without energest are unchanged.
 */
#include "energest_engine.h"
#include "sim_runtime.h"    /* sim_runtime_ui_publish_panel / push_frame */
#include "sim_service.h"
#include "sim_control.h"    /* sim_control_describe */
#include "sim_config.h"     /* sim_normalized_config_t (the energest section) */
#include "sim_board.h"      /* sim_board_find (currents keys) */

#include <stdio.h>
#include <stddef.h>
#include <string.h>

static bool energest_describe(sim_runtime_t *sim, int idx, energest_node_info_t *out) {
    sim_control_node_info_t info;
    if (!sim || !sim->control || !sim_control_describe(sim->control, idx, &info))
        return false;
    out->id = info.id;
    out->board = info.board;
    out->start_ns = info.start_ns == INT64_MAX ? -1 : info.start_ns;
    return true;
}

static int64_t energest_now(sim_runtime_t *sim) {
    return sim_runtime_now_ns(sim);
}

static bool energest_board_known(const char *board) {
    if (sim_board_find(board)) return true;
    /* boards a mote type can name that have no extension of their own */
    static const char *const extra[] = { "openmote", NULL };
    for (int i = 0; extra[i]; i++)
        if (strcmp(board, extra[i]) == 0) return true;
    return false;
}

/* cfg: the run's normalized config (the runner passes it to this built-in),
 * or NULL (no options: the defaults). */
static int energest_bi_init(sim_runtime_t *sim, const void *cfg, void **state) {
    energest_sink_t sink = {
        .publish     = sim_runtime_ui_publish_panel,  /* same signature */
        .push_frame  = sim_runtime_ui_push_frame,
        .log         = NULL,                          /* -> stdout */
        .describe    = energest_describe,
        .board_known = energest_board_known,
        .now         = energest_now,
    };
    energest_engine_t *e = energest_engine_create(sink);
    if (!e) return -1;
    const sim_normalized_config_t *c = cfg;
    char err[256] = "";
    if (energest_engine_configure(e, c ? c->energest_json : NULL, err, sizeof err) != 0) {
        fprintf(stderr, "%s\n", err);
        energest_engine_free(e);
        return SIM_SERVICE_REFUSED;     /* said why: no generic message */
    }
    sim_runtime_set_power_states(sim, true);
    *state = e;
    return 0;
}

static void energest_bi_on_event(sim_runtime_t *sim, void *state,
                                 const sim_observer_event_t *ev) {
    energest_engine_on_event((energest_engine_t *)state, sim, ev);
}

/* A restart is a new run: its energy starts from zero (the end-of-run
 * summary covers the run that ends, not the ones restarted before it). */
static void energest_bi_on_restart(sim_runtime_t *sim, void *state) {
    (void)sim;
    energest_engine_reset((energest_engine_t *)state);
}

/* A failed cross-check or an output file that could not be written fails
 * the run (the report printed which). */
static void energest_bi_destroy(sim_runtime_t *sim, void *state) {
    if (energest_engine_report((energest_engine_t *)state, sim) != 0)
        sim_runtime_note_service_failure(sim);
    /* The live stream's queue (a recording takes every frame). */
    if (sim && sim->ui_frames_dropped)
        printf("energest: note: %d UI frames dropped from the live stream (its queue "
               "of %d was full between two UI ticks; a larger frame_ms sends fewer)\n",
               sim->ui_frames_dropped, SIM_RUNTIME_MAX_FRAMES);
    energest_engine_free((energest_engine_t *)state);
}

const sim_service_ops_t energest_service_ops = {
    .name     = "energest",
    .init     = energest_bi_init,
    .destroy  = energest_bi_destroy,
    .on_event = energest_bi_on_event,
    .on_restart = energest_bi_on_restart,
};
