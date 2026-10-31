/*
 * rpl_overlay_engine — Contiki-NG RPL drawn on the network canvas.
 *
 * Every 500 ms of sim time it works out each node's RPL view — joined or
 * not, rank, DODAG, preferred parent, root — and publishes a "layer" panel
 * (docs/design/ui-plugins.md): the root a square, every node coloured by
 * its DODAG, labelled with its rank, and a green arrow to its preferred
 * parent.
 *
 * Two sources, best first:
 *   firmware  RPL Lite's curr_instance and neighbour table, read out of
 *             emulated memory by symbol (sim_fwvar).  Exact and immediate,
 *             but needs a known struct layout (rpl_layout.h, picked by the
 *             image's machine and sizeof rpl_instance_t).
 *   packets   what the node transmits: a DIO gives its rank and DODAG, a
 *             unicast DAO its preferred parent (the DAO goes to the parent
 *             in storing mode and up the default route — via the parent —
 *             in non-storing mode).  Works for any node that sends them —
 *             a stripped image, a layout we have no row for, a native or
 *             Renode node — but only knows what has been on the air.
 * The panel says which source each node's view came from.
 *
 * Like energest_engine it names no host symbol: the caller passes the
 * publish sink and the firmware reads (the v4 csim_fw_ops, or the kernel's
 * sim_fwvar_* directly), so the same engine serves a built-in and a .so.
 * rpl-classic (default_instance) is not read yet; such images fall back to
 * the packets.
 */
#ifndef RPL_OVERLAY_ENGINE_H
#define RPL_OVERLAY_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sim_fwvar.h"      /* sim_fwvar_t */
#include "sim_observer.h"   /* sim_observer_event_t */
#include "sim_service.h"    /* sim_runtime_t (opaque) */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rpl_overlay_host {
    /* The panel's id and title (NULL: "rpl", "RPL"). */
    const char *panel_id, *title;
    /* (sim, panel_id, json); NULL = nothing to publish to. */
    void (*publish)(sim_runtime_t *sim, const char *id, const char *json);
    /* Firmware reads; both NULL = packets only. */
    bool (*resolve)(sim_runtime_t *sim, int node, const char *symbol, sim_fwvar_t *out);
    bool (*read)(sim_runtime_t *sim, int node, const sim_fwvar_t *var,
                 uint32_t offset, void *dst, size_t n);
    /* The slots to draw and their node ids (the v4 fw ops' node_count and
     * node_id); without node_count nothing is drawn, without node_id a
     * slot's id is its index + 1. */
    int  (*node_count)(sim_runtime_t *sim);
    int  (*node_id)(sim_runtime_t *sim, int node);
} rpl_overlay_host_t;

typedef struct rpl_overlay rpl_overlay_t;

rpl_overlay_t *rpl_overlay_create(rpl_overlay_host_t host);
void           rpl_overlay_free(rpl_overlay_t *o);

/* Feed one observer event: transmitted frames teach the packet source,
 * and any event may trigger the throttled refresh. */
void rpl_overlay_on_event(rpl_overlay_t *o, sim_runtime_t *sim,
                          const sim_observer_event_t *ev);

/* Rebuild and publish the panel now (end of run). */
void rpl_overlay_refresh(rpl_overlay_t *o, sim_runtime_t *sim, int64_t now_ns);

#ifdef __cplusplus
}
#endif

#endif /* RPL_OVERLAY_ENGINE_H */
