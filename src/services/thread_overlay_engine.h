/*
 * thread_overlay_engine — an OpenThread network drawn on the canvas, in
 * OTNS's visual grammar so Thread people read it at once.
 *
 * Every 500 ms of sim time it publishes a "layer" panel
 * (docs/design/ui-plugins.md):
 *   leader   hexagon, red ring, badge L
 *   router   hexagon, badge R
 *   child    circle, badge C (a sleepy end device: dashed ring, badge S)
 *   detached grey ring, no fill; disabled: not drawn as Thread
 * filled with its partition's colour, labelled with its RLOC16, with a
 * green arrow from each child to its parent and blue links between routers
 * that have a valid link (router table, link quality > 0).
 *
 * Two sources, best first:
 *   firmware  OpenThread's one static instance (ot::gInstanceRaw): MLE role,
 *             device mode, RLOC16, leader data, parent, router table, read
 *             by symbol with offsets from thread_layout.h (picked by machine
 *             and sizeof ot::Instance).
 *   packets   the MAC headers it sends: after attaching, a Thread node sends
 *             its data frames from its RLOC16 (short address); a child ID
 *             of 0 is a router, any other a child whose parent is the router
 *             in its upper bits.  The leader and the partition travel inside
 *             encrypted MLE, so the packet view leaves them out.
 *
 * Each node also offers its console commands for a role change ("actions",
 * OpenThread CLI lines the page sends to the node): become leader, router
 * or child, detach, rejoin — only those that apply to its role and mode.
 *
 * Like rpl_overlay_engine it names no host symbol: the caller passes the
 * publish sink and the firmware reads.
 */
#ifndef THREAD_OVERLAY_ENGINE_H
#define THREAD_OVERLAY_ENGINE_H

#include "rpl_overlay_engine.h"   /* rpl_overlay_host_t: the same sinks */

#ifdef __cplusplus
extern "C" {
#endif

typedef rpl_overlay_host_t thread_overlay_host_t;
typedef struct thread_overlay thread_overlay_t;

thread_overlay_t *thread_overlay_create(thread_overlay_host_t host);
void              thread_overlay_free(thread_overlay_t *o);
void              thread_overlay_on_event(thread_overlay_t *o, sim_runtime_t *sim,
                                          const sim_observer_event_t *ev);
void              thread_overlay_refresh(thread_overlay_t *o, sim_runtime_t *sim,
                                         int64_t now_ns);

#ifdef __cplusplus
}
#endif

#endif /* THREAD_OVERLAY_ENGINE_H */
