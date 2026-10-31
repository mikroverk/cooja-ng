/*
 * sim_layer — build a UI panel that draws on the network canvas.
 *
 * A "layer" panel styles nodes (glyph, fill, ring, badge, label) and adds
 * typed links between them; the web UI draws it over the topology, and the
 * panel box shows the title, the legend and any note.  The vocabulary is
 * in docs/design/ui-plugins.md.  The builder produces the JSON for
 * publish_panel (v3 csim_ui_ops, or sim_runtime_ui_publish_panel):
 *
 *     sim_layer_t *l = sim_layer_begin("RPL");
 *     sim_layer_node(l, 1, &(sim_layer_style_t){ .glyph = "square", .fill = "#2a6", .label = "rank 128" });
 *     sim_layer_link(l, 4, 1, "parent", NULL);
 *     sim_layer_legend(l, "square", "DODAG root");
 *     char *json = sim_layer_end(l);
 *     publish(sim, "rpl", json);
 *     free(json);
 *
 * It names no host symbol (libc only), so an out-of-tree plugin can
 * compile src/sim/sim_layer.c into itself.  Strings are JSON-escaped.
 * Node ids are Cooja node ids, the ones the canvas labels "Node N".
 */
#ifndef SIM_LAYER_H
#define SIM_LAYER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sim_layer sim_layer_t;

/* Any member may be NULL (left to the canvas's default). */
typedef struct sim_layer_style {
    const char *glyph;      /* "circle" (default) | "hex" | "square" | "diamond" */
    const char *fill;       /* CSS colour of the glyph                           */
    const char *ring;       /* CSS colour of a 2-3 px outline                    */
    bool        ring_dash;  /* draw the ring dashed (a sleepy node)              */
    const char *badge;      /* up to 2 characters drawn in the glyph             */
    const char *label;      /* short text under the node                         */
} sim_layer_style_t;

/* Start a layer panel; NULL on OOM (every call below accepts NULL). */
sim_layer_t *sim_layer_begin(const char *title);
/* Style node `id`.  A second call for the same id adds a second entry; the
 * canvas uses the last. */
void sim_layer_node(sim_layer_t *l, int id, const sim_layer_style_t *st);
/* A link from node `from` to node `to`: style "parent" (arrow to the
 * parent) | "router" | "dashed" | "plain"; `color` NULL = the style's own. */
void sim_layer_link(sim_layer_t *l, int from, int to, const char *style,
                    const char *color);
/* One legend row: a sample (a glyph name or a few words) and its meaning. */
void sim_layer_legend(sim_layer_t *l, const char *sample, const char *text);
/* A free-text note shown in the panel box (lines joined with '\n'). */
void sim_layer_note(sim_layer_t *l, const char *text);
/* Finish: a malloc'd JSON object string (caller frees), or NULL on OOM.
 * Frees the builder either way. */
char *sim_layer_end(sim_layer_t *l);

#ifdef __cplusplus
}
#endif

#endif /* SIM_LAYER_H */
