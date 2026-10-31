/*
 * sim_layer — see include/sim/sim_layer.h.  libc only, on purpose.
 */
#include "sim_layer.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char *s; size_t len, cap; bool oom; } sbuf_t;

struct sim_layer {
    sbuf_t title, nodes, links, legend, note;
};

static void sb_put(sbuf_t *b, const char *s, size_t n) {
    if (b->oom) return;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (b->len + n + 1 > cap) cap *= 2;
        char *p = realloc(b->s, cap);
        if (!p) { b->oom = true; return; }
        b->s = p; b->cap = cap;
    }
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = '\0';
}

static void sb_str(sbuf_t *b, const char *s) { sb_put(b, s, strlen(s)); }

static void sb_fmt(sbuf_t *b, const char *fmt, ...) {
    char tmp[64];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) sb_put(b, tmp, (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1);
}

/* A JSON string literal, quotes included. */
static void sb_json(sbuf_t *b, const char *s) {
    sb_str(b, "\"");
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        if (*p == '"' || *p == '\\') { char e[2] = { '\\', (char)*p }; sb_put(b, e, 2); }
        else if (*p == '\n') sb_str(b, "\\n");
        else if (*p < 0x20) sb_fmt(b, "\\u%04x", *p);
        else sb_put(b, (const char *)p, 1);
    }
    sb_str(b, "\"");
}

/* ,"key":"value" (or {"key" when first), for a non-NULL value. */
static void sb_member(sbuf_t *b, bool *first, const char *key, const char *val) {
    if (!val) return;
    sb_str(b, *first ? "" : ",");
    *first = false;
    sb_json(b, key);
    sb_str(b, ":");
    sb_json(b, val);
}

sim_layer_t *sim_layer_begin(const char *title) {
    sim_layer_t *l = calloc(1, sizeof(*l));
    if (l) sb_json(&l->title, title ? title : "");
    return l;
}

void sim_layer_node(sim_layer_t *l, int id, const sim_layer_style_t *st) {
    if (!l || !st) return;
    if (l->nodes.len) sb_str(&l->nodes, ",");
    sb_fmt(&l->nodes, "\"%d\":{", id);
    bool first = true;
    sb_member(&l->nodes, &first, "glyph", st->glyph);
    sb_member(&l->nodes, &first, "fill", st->fill);
    sb_member(&l->nodes, &first, "ring", st->ring);
    if (st->ring_dash) { sb_str(&l->nodes, first ? "" : ","); first = false; sb_str(&l->nodes, "\"ring_dash\":true"); }
    sb_member(&l->nodes, &first, "badge", st->badge);
    sb_member(&l->nodes, &first, "label", st->label);
    if (st->actions && st->actions[0] && st->actions[1]) {
        sb_str(&l->nodes, first ? "\"actions\":[" : ",\"actions\":[");
        first = false;
        for (int i = 0; st->actions[i] && st->actions[i + 1]; i += 2) {
            sb_str(&l->nodes, i ? ",[" : "[");
            sb_json(&l->nodes, st->actions[i]);
            sb_str(&l->nodes, ",");
            sb_json(&l->nodes, st->actions[i + 1]);
            sb_str(&l->nodes, "]");
        }
        sb_str(&l->nodes, "]");
    }
    sb_str(&l->nodes, "}");
}

void sim_layer_link(sim_layer_t *l, int from, int to, const char *style,
                    const char *color) {
    if (!l) return;
    if (l->links.len) sb_str(&l->links, ",");
    sb_fmt(&l->links, "{\"from\":%d,\"to\":%d", from, to);
    bool first = false;
    sb_member(&l->links, &first, "style", style ? style : "plain");
    sb_member(&l->links, &first, "color", color);
    sb_str(&l->links, "}");
}

void sim_layer_legend(sim_layer_t *l, const char *sample, const char *text) {
    if (!l) return;
    if (l->legend.len) sb_str(&l->legend, ",");
    sb_str(&l->legend, "[");
    sb_json(&l->legend, sample);
    sb_str(&l->legend, ",");
    sb_json(&l->legend, text);
    sb_str(&l->legend, "]");
}

void sim_layer_note(sim_layer_t *l, const char *text) {
    if (!l || !text) return;
    if (l->note.len) sb_str(&l->note, "\n");
    sb_str(&l->note, text);
}

static void sb_free(sbuf_t *b) { free(b->s); }

char *sim_layer_end(sim_layer_t *l) {
    if (!l) return NULL;
    sbuf_t out = {0};
    sb_str(&out, "{\"title\":");
    sb_str(&out, l->title.s ? l->title.s : "\"\"");
    if (l->note.len) { sb_str(&out, ",\"text\":"); sb_json(&out, l->note.s); }
    sb_str(&out, ",\"layer\":{\"nodes\":{");
    if (l->nodes.s) sb_str(&out, l->nodes.s);
    sb_str(&out, "},\"links\":[");
    if (l->links.s) sb_str(&out, l->links.s);
    sb_str(&out, "],\"legend\":[");
    if (l->legend.s) sb_str(&out, l->legend.s);
    sb_str(&out, "]}}");
    bool oom = out.oom || l->title.oom || l->nodes.oom || l->links.oom ||
               l->legend.oom || l->note.oom;
    sb_free(&l->title); sb_free(&l->nodes); sb_free(&l->links);
    sb_free(&l->legend); sb_free(&l->note);
    free(l);
    if (oom) { free(out.s); return NULL; }
    return out.s;
}
