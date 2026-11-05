/*
 * energest_engine — the host-agnostic core of the energest plugin.
 *
 * It integrates, per node and per component ("lane": cpu, radio, each LED,
 * timer, clock gate ...), the exact time in each state from the power-state
 * streams the models emit (SIM_OBS_CPU_TRANSITION, SIM_OBS_RADIO_STATE,
 * SIM_OBS_COMPONENT_STATE), puts a datasheet power model on top
 * (energest_model.h), and reports:
 *   - an end-of-run summary table (time in state, energy, average current,
 *     battery lifetime) with its cross-checks (states sum to the node's
 *     lifetime; CPU low-power time vs the core's own lpm_ns; TX time vs the
 *     bus's frame air time),
 *   - an optional CSV (node x component x state),
 *   - a live UI table panel ("energest", the latest value), and
 *   - UI frames ("energest", sent once each): binned timelines plus
 *     cumulative totals, the format in docs/energest.md "UI frames".
 *
 * It names no host symbol: output goes through the caller's sink, so the
 * same logic runs compiled in (energest_service.c) or behind a .so wrapper.
 */
#ifndef ENERGEST_ENGINE_H
#define ENERGEST_ENGINE_H

#include "sim_observer.h"   /* sim_observer_event_t */
#include "sim_service.h"    /* sim_runtime_t (opaque forward decl) */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What the engine needs to know of a node slot. */
typedef struct energest_node_info {
    int         id;          /* Cooja node id                              */
    const char *board;       /* board registry name ("sky"), or NULL       */
    int64_t     start_ns;    /* when the node starts executing             */
} energest_node_info_t;

/* Where the engine sends output and asks about nodes.  Any member may be
 * NULL: publish (no live panel), push_frame (no UI frames), log (stdout),
 * describe (node id = slot index + 1, no board, start 0), board_known. */
typedef struct energest_sink {
    void (*publish)(sim_runtime_t *sim, const char *id, const char *json);
    bool (*push_frame)(sim_runtime_t *sim, const char *id, const char *json);
    void (*log)(const char *line);
    bool (*describe)(sim_runtime_t *sim, int idx, energest_node_info_t *out);
    /* Is `board` a board name (the `currents` keys are checked with it)?
     * NULL = any name is accepted. */
    bool (*board_known)(const char *board);
} energest_sink_t;

typedef struct energest_engine energest_engine_t;

/* Create/free the engine.  create returns NULL on OOM. */
energest_engine_t *energest_engine_create(energest_sink_t sink);
void               energest_engine_free(energest_engine_t *e);

/* Apply the config's `energest:` section (compact JSON; NULL or "" = the
 * defaults).  Returns 0, or -1 with a reason in err: an unknown key, a
 * current for an unknown component or state, a bad number, a csv, frames
 * or trace file that cannot be created (the CSV is written at the
 * report). */
int energest_engine_configure(energest_engine_t *e, const char *json,
                              char *err, size_t errsz);

/* Forget everything counted so far (a restarted run); options stay. */
void energest_engine_reset(energest_engine_t *e);

/* Feed one observer event.  `sim` is passed through to the sink. */
void energest_engine_on_event(energest_engine_t *e, sim_runtime_t *sim,
                              const sim_observer_event_t *ev);

/* Close every lane at the end of the run, emit the last frames, print the
 * summary through sink.log and write the CSV.  Returns 0, or -1 when a
 * cross-check failed or a file could not be written (also printed): the
 * host fails the run (energest_service.c: exit 1). */
int energest_engine_report(energest_engine_t *e, sim_runtime_t *sim);

#ifdef __cplusplus
}
#endif

#endif /* ENERGEST_ENGINE_H */
