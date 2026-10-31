/*
 * descriptor_overlay — an overlay written as a file, not as C.
 *
 * A descriptor (YAML or JSON) names a firmware variable and the fields of
 * it to read, and maps their values onto the canvas vocabulary of
 * docs/design/ui-plugins.md: glyph, fill, ring, dashed ring, badge, label,
 * the node's right-click actions, links between nodes, a legend and counts.
 * A config lists descriptors under `overlays:`; each becomes a layer panel.
 * docs/design/overlay-descriptors.md is the reference; in short:
 *
 *   overlay: thread-lite                 # the panel id
 *   title: "Thread (from a descriptor)"
 *   symbol: _ZN2ot12gInstanceRawE        # the variable (mangled for C++)
 *   struct: Instance                     # its type; the layouts are for it
 *   fields: { role: mMle.mRole, rloc16: mMle.mRloc16, parent: mMle.mParent.mRloc16 }
 *   key: rloc16                          # what links point at
 *   node:
 *     show:  { field: role, ne: 0 }
 *     glyph: { field: role, map: { 3: hex, 4: hex }, default: circle }
 *     fill:  { field: partition, color: id }
 *     label: "{rloc16:04X}"
 *   links:
 *     - { from: parent, style: parent, color: "#8bc34a", when: { field: role, eq: 2 } }
 *   layouts: [...]    # generated: tools/gen-struct-layout.py --update FILE IMAGE
 *
 * The layouts carry each field's offset (and bitfield position) per image,
 * picked like the C overlays' rows: by the image's machine and the size its
 * symbol table records for the variable.  What a descriptor cannot say yet
 * — arrays (a router table), following pointers (RPL's preferred parent) —
 * is what the C overlays (rpl, thread) are for.
 */
#ifndef DESCRIPTOR_OVERLAY_H
#define DESCRIPTOR_OVERLAY_H

#include <stddef.h>

#include "rpl_overlay_engine.h"   /* rpl_overlay_host_t: the same sinks */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct descriptor_overlay descriptor_overlay_t;

/* Load and check the descriptor at `path`.  NULL with a message in `err`
 * if it cannot be read or does not follow the schema. */
descriptor_overlay_t *descriptor_overlay_load(const char *path, rpl_overlay_host_t host,
                                              char *err, size_t errsz);
void descriptor_overlay_free(descriptor_overlay_t *o);
void descriptor_overlay_on_event(descriptor_overlay_t *o, sim_runtime_t *sim,
                                 const sim_observer_event_t *ev);
void descriptor_overlay_refresh(descriptor_overlay_t *o, sim_runtime_t *sim, int64_t now_ns);

#ifdef __cplusplus
}
#endif

#endif /* DESCRIPTOR_OVERLAY_H */
