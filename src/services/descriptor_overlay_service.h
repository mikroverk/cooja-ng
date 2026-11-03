/*
 * descriptor_overlay_service — the overlays a run draws from descriptors:
 * those its config lists (`overlays:`) and those the web UI's overlay
 * editor sends while it runs (docs/design/overlay-descriptors.md).
 */
#ifndef DESCRIPTOR_OVERLAY_SERVICE_H
#define DESCRIPTOR_OVERLAY_SERVICE_H

#include <stddef.h>

#include "sim_service.h"   /* sim_runtime_t, sim_service_ops_t */

#ifdef __cplusplus
extern "C" {
#endif

extern const sim_service_ops_t descriptor_overlay_service_ops;

/* Load a descriptor's text and draw it now: a new overlay, or the one with
 * the same `overlay:` id replaced.  Attaches the service on first use.
 * 0 with its id in `id`, or -1 with the reason in `err`. */
int   descriptor_overlays_apply(sim_runtime_t *sim, const char *text, char *id, size_t idsz,
                                char *err, size_t errsz);
/* Take an overlay off (its panel too).  0, or -1 if there is none by `id`. */
int   descriptor_overlays_remove(sim_runtime_t *sim, const char *id);
/* [{"id","title","yaml"}, ...] of the overlays drawn now (caller frees). */
char *descriptor_overlays_list_json(void);

#ifdef __cplusplus
}
#endif

#endif /* DESCRIPTOR_OVERLAY_SERVICE_H */
