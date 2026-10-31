/*
 * descriptor_overlay — see descriptor_overlay.h and
 * docs/design/overlay-descriptors.md.
 */
#include "descriptor_overlay.h"
#include "../sim/sim_config_internal.h"   /* sim_config_yaml_to_cjson */
#include "sim_layer.h"
#include "cJSON.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_NODES   128                  /* SIM_EQ_MAX_NODES */
#define MAX_FIELDS  32
#define MAX_ACTIONS 16
#define REFRESH_NS  500000000LL          /* rebuild at most every 500 ms */
#define MAX_LISTS   4
#define MAX_ELEMS   32
#define MAX_LFIELDS 8

/* An array member read element by element (OpenThread's router table). */
typedef struct {
    const char *name, *at, *type;
    int count;                           /* the field holding its length */
    int nf;
    const char *fname[MAX_LFIELDS], *fpath[MAX_LFIELDS];
} list_t;

typedef uint32_t list_vals_t[MAX_LISTS][MAX_ELEMS][MAX_LFIELDS];

struct descriptor_overlay {
    rpl_overlay_host_t host;
    cJSON   *doc;
    char     path[512];
    const char *id, *title, *symbol, *type;
    int      nfields;
    const char *fname[MAX_FIELDS], *fpath[MAX_FIELDS];
    int      key;                        /* field index links point at, -1 = none */
    cJSON   *node, *links, *legend, *counts, *layouts;
    int64_t  last_ns;
    int      nslots;
    int      ids[MAX_NODES];
    bool     have[MAX_NODES];            /* the node's fields were read */
    bool     unknown[MAX_NODES];         /* the variable is there, its layout is not */
    uint32_t val[MAX_NODES][MAX_FIELDS];
    int      nlists;
    list_t   lists[MAX_LISTS];
    list_vals_t *lval;                   /* [MAX_NODES], when there are lists */
    uint8_t  nelem[MAX_NODES][MAX_LISTS];
    int      cur_list, cur_elem;         /* the element a link rule is looking at */
};

/* --- loading and checking ----------------------------------------------- */

static void fail(char *err, size_t sz, const char *path, const char *fmt, ...) {
    int n = snprintf(err, sz, "%s: ", path);
    va_list ap;
    va_start(ap, fmt);
    if (n >= 0 && (size_t)n < sz) vsnprintf(err + n, sz - (size_t)n, fmt, ap);
    va_end(ap);
}

static int field_index(const descriptor_overlay_t *o, const char *name) {
    for (int i = 0; i < o->nfields; i++)
        if (strcmp(o->fname[i], name) == 0) return i;
    return -1;
}

static int list_field(const list_t *l, const char *name) {
    for (int i = 0; i < l->nf; i++)
        if (strcmp(l->fname[i], name) == 0) return i;
    return -1;
}

/* A name a rule may use: a field, or a field of any list's elements. */
static bool known_name(const descriptor_overlay_t *o, const char *name) {
    if (field_index(o, name) >= 0) return true;
    for (int i = 0; i < o->nlists; i++)
        if (list_field(&o->lists[i], name) >= 0) return true;
    return false;
}

static bool only_keys(const cJSON *obj, const char *const *keys, const char **bad) {
    const cJSON *c;
    cJSON_ArrayForEach(c, obj) {
        bool ok = false;
        for (int i = 0; keys[i]; i++) ok |= strcmp(c->string, keys[i]) == 0;
        if (!ok) { *bad = c->string; return false; }
    }
    return true;
}

/* Every "field" (and a link's "from") names a field of the descriptor. */
static bool check_refs(const descriptor_overlay_t *o, const cJSON *v, const char **bad) {
    if (cJSON_IsObject(v)) {
        const cJSON *f = cJSON_GetObjectItemCaseSensitive(v, "field");
        if (f && (!cJSON_IsString(f) || !known_name(o, f->valuestring))) {
            *bad = cJSON_IsString(f) ? f->valuestring : "(not a name)";
            return false;
        }
    }
    const cJSON *c;
    if (cJSON_IsObject(v) || cJSON_IsArray(v))
        cJSON_ArrayForEach(c, v)
            if (!check_refs(o, c, bad)) return false;
    return true;
}

descriptor_overlay_t *descriptor_overlay_load(const char *path, rpl_overlay_host_t host,
                                              char *err, size_t errsz) {
    FILE *f = fopen(path, "rb");
    if (!f) { fail(err, errsz, path, "cannot open"); return NULL; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0 || len > 1024 * 1024) { fclose(f); fail(err, errsz, path, "not a descriptor (size)"); return NULL; }
    char *text = malloc((size_t)len + 1);
    if (!text) { fclose(f); fail(err, errsz, path, "out of memory"); return NULL; }
    size_t got = fread(text, 1, (size_t)len, f);
    fclose(f);
    text[got] = '\0';
    size_t pl = strlen(path);
    bool json = pl > 5 && strcmp(path + pl - 5, ".json") == 0;
    cJSON *doc = json ? cJSON_Parse(text) : sim_config_yaml_to_cjson(text, got, path);
    free(text);
    if (!cJSON_IsObject(doc)) {
        cJSON_Delete(doc);
        fail(err, errsz, path, "not a YAML/JSON mapping");
        return NULL;
    }
    descriptor_overlay_t *o = calloc(1, sizeof(*o));
    if (!o) { cJSON_Delete(doc); fail(err, errsz, path, "out of memory"); return NULL; }
    o->doc = doc;
    o->host = host;
    o->key = -1;
    snprintf(o->path, sizeof(o->path), "%s", path);

    static const char *const top[] = { "overlay", "title", "symbol", "struct", "fields", "key",
                                       "lists", "node", "links", "legend", "counts", "layouts", NULL };
    static const char *const list_keys[] = { "at", "count", "struct", "fields", NULL };
    static const char *const node_keys[] = { "show", "glyph", "fill", "ring", "ring_dash",
                                             "badge", "label", "actions", NULL };
    static const char *const link_keys[] = { "from", "list", "to", "once", "style", "color", "when", NULL };
    const char *bad = NULL;
    if (!only_keys(doc, top, &bad)) { fail(err, errsz, path, "unknown key '%s'", bad); goto bad; }
    const cJSON *v;
#define STR(key, dst, required) \
    v = cJSON_GetObjectItemCaseSensitive(doc, key); \
    if (v && !cJSON_IsString(v)) { fail(err, errsz, path, "'%s' must be a string", key); goto bad; } \
    if (!v && required) { fail(err, errsz, path, "'%s' is missing", key); goto bad; } \
    dst = v ? v->valuestring : NULL;
    STR("overlay", o->id, true);
    STR("title", o->title, false);
    STR("symbol", o->symbol, true);
    STR("struct", o->type, true);
#undef STR
    const cJSON *fields = cJSON_GetObjectItemCaseSensitive(doc, "fields");
    if (!cJSON_IsObject(fields) || !fields->child) { fail(err, errsz, path, "'fields' must map names to member paths"); goto bad; }
    cJSON_ArrayForEach(v, fields) {
        if (!cJSON_IsString(v)) { fail(err, errsz, path, "field '%s': a member path (string)", v->string); goto bad; }
        if (o->nfields >= MAX_FIELDS) { fail(err, errsz, path, "more than %d fields", MAX_FIELDS); goto bad; }
        o->fname[o->nfields] = v->string;
        o->fpath[o->nfields++] = v->valuestring;
    }
    v = cJSON_GetObjectItemCaseSensitive(doc, "key");
    if (v) {
        if (!cJSON_IsString(v) || (o->key = field_index(o, v->valuestring)) < 0) {
            fail(err, errsz, path, "'key' must name a field"); goto bad;
        }
    }
    o->cur_list = -1;
    const cJSON *lists = cJSON_GetObjectItemCaseSensitive(doc, "lists");
    if (lists) {
        if (!cJSON_IsObject(lists)) { fail(err, errsz, path, "'lists' must map names to lists"); goto bad; }
        cJSON_ArrayForEach(v, lists) {
            if (o->nlists >= MAX_LISTS) { fail(err, errsz, path, "more than %d lists", MAX_LISTS); goto bad; }
            list_t *L = &o->lists[o->nlists];
            const cJSON *at = cJSON_GetObjectItemCaseSensitive(v, "at");
            const cJSON *cnt = cJSON_GetObjectItemCaseSensitive(v, "count");
            const cJSON *ty = cJSON_GetObjectItemCaseSensitive(v, "struct");
            const cJSON *lf = cJSON_GetObjectItemCaseSensitive(v, "fields"), *e;
            if (!cJSON_IsObject(v) || !only_keys(v, list_keys, &bad)) { fail(err, errsz, path, "list %s: unknown key '%s'", v->string, bad ? bad : "?"); goto bad; }
            if (!cJSON_IsString(at) || !cJSON_IsString(ty) || !cJSON_IsObject(lf)) { fail(err, errsz, path, "list %s needs 'at', 'struct' and 'fields'", v->string); goto bad; }
            if (!cJSON_IsString(cnt) || (L->count = field_index(o, cnt->valuestring)) < 0) { fail(err, errsz, path, "list %s: 'count' must name a field", v->string); goto bad; }
            L->name = v->string; L->at = at->valuestring; L->type = ty->valuestring;
            cJSON_ArrayForEach(e, lf) {
                if (!cJSON_IsString(e) || L->nf >= MAX_LFIELDS) { fail(err, errsz, path, "list %s: fields are up to %d member paths", v->string, MAX_LFIELDS); goto bad; }
                L->fname[L->nf] = e->string;
                L->fpath[L->nf++] = e->valuestring;
            }
            o->nlists++;
        }
        o->lval = calloc(MAX_NODES, sizeof(list_vals_t));
        if (!o->lval) { fail(err, errsz, path, "out of memory"); goto bad; }
    }
    o->node = cJSON_GetObjectItemCaseSensitive(doc, "node");
    if (o->node && (!cJSON_IsObject(o->node) || !only_keys(o->node, node_keys, &bad))) {
        fail(err, errsz, path, "node: unknown key '%s'", bad ? bad : "?"); goto bad;
    }
    o->links = cJSON_GetObjectItemCaseSensitive(doc, "links");
    if (o->links) {
        if (!cJSON_IsArray(o->links)) { fail(err, errsz, path, "'links' must be a list"); goto bad; }
        cJSON_ArrayForEach(v, o->links) {
            const cJSON *from = cJSON_GetObjectItemCaseSensitive(v, "from");
            if (!cJSON_IsObject(v) || !only_keys(v, link_keys, &bad)) { fail(err, errsz, path, "link: unknown key '%s'", bad ? bad : "?"); goto bad; }
            const cJSON *lst = cJSON_GetObjectItemCaseSensitive(v, "list"), *to = cJSON_GetObjectItemCaseSensitive(v, "to");
            if (lst) {
                int li = -1;
                for (int i = 0; cJSON_IsString(lst) && i < o->nlists; i++)
                    if (strcmp(o->lists[i].name, lst->valuestring) == 0) li = i;
                if (li < 0) { fail(err, errsz, path, "link: 'list' must name a list"); goto bad; }
                if (!cJSON_IsString(to) || list_field(&o->lists[li], to->valuestring) < 0) { fail(err, errsz, path, "link: 'to' must name a field of list %s", o->lists[li].name); goto bad; }
            } else if (!cJSON_IsString(from) || field_index(o, from->valuestring) < 0) {
                fail(err, errsz, path, "link: 'from' must name a field (or give 'list' and 'to')"); goto bad;
            }
            if (o->key < 0) { fail(err, errsz, path, "links need a 'key' field to point at"); goto bad; }
        }
    }
    o->legend = cJSON_GetObjectItemCaseSensitive(doc, "legend");
    o->counts = cJSON_GetObjectItemCaseSensitive(doc, "counts");
    o->layouts = cJSON_GetObjectItemCaseSensitive(doc, "layouts");
    if (o->layouts && !cJSON_IsArray(o->layouts)) { fail(err, errsz, path, "'layouts' must be a list"); goto bad; }
    if (!check_refs(o, doc, &bad)) { fail(err, errsz, path, "no field named '%s'", bad); goto bad; }
    return o;
bad:
    descriptor_overlay_free(o);
    return NULL;
}

void descriptor_overlay_free(descriptor_overlay_t *o) {
    if (!o) return;
    free(o->lval);
    cJSON_Delete(o->doc);
    free(o);
}

/* --- reading the fields --------------------------------------------------- */

static uint32_t num(const cJSON *v) {
    if (cJSON_IsNumber(v)) return (uint32_t)v->valuedouble;
    if (cJSON_IsString(v)) return (uint32_t)strtoul(v->valuestring, NULL, 0);
    if (cJSON_IsTrue(v)) return 1;
    return 0;
}

/* The layout for this image: its machine and the variable's size. */
static const cJSON *pick_layout(const descriptor_overlay_t *o, const sim_fwvar_t *v) {
    const cJSON *l;
    cJSON_ArrayForEach(l, o->layouts)
        if (num(cJSON_GetObjectItemCaseSensitive(l, "machine")) == v->machine &&
            num(cJSON_GetObjectItemCaseSensitive(l, "size")) == v->size)
            return l;
    return NULL;
}

/* One field per its row [offset, size, bit, bits], `base` bytes further
 * into the variable: 1 = read, 0 = unreadable, -1 = no such row. */
static int read_row(descriptor_overlay_t *o, sim_runtime_t *sim, int slot, const sim_fwvar_t *var,
                    const cJSON *r, uint32_t base, uint32_t *out) {
    if (!cJSON_IsArray(r) || cJSON_GetArraySize(r) < 2) return -1;
    uint32_t off = num(cJSON_GetArrayItem(r, 0)), size = num(cJSON_GetArrayItem(r, 1));
    uint32_t bit = num(cJSON_GetArrayItem(r, 2)), bits = num(cJSON_GetArrayItem(r, 3));
    uint8_t b[4] = {0};
    if (size == 0 || !o->host.read(sim, slot, var, base + off, b, size > 4 ? 4 : size)) return 0;
    uint32_t x = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    if (bits) x = (x >> bit) & (bits >= 32 ? 0xFFFFFFFFu : ((1u << bits) - 1));
    *out = x;
    return 1;
}

static void read_node(descriptor_overlay_t *o, sim_runtime_t *sim, int slot) {
    o->have[slot] = o->unknown[slot] = false;
    if (!o->host.resolve || !o->host.read) return;
    sim_fwvar_t var;
    if (!o->host.resolve(sim, slot, o->symbol, &var)) return;
    const cJSON *lay = pick_layout(o, &var);
    const cJSON *rows = lay ? cJSON_GetObjectItemCaseSensitive(lay, "rows") : NULL;
    if (!rows) { o->unknown[slot] = true; return; }
    for (int i = 0; i < o->nfields; i++) {
        int rc = read_row(o, sim, slot, &var, cJSON_GetObjectItemCaseSensitive(rows, o->fpath[i]), 0, &o->val[slot][i]);
        if (rc < 0) { o->unknown[slot] = true; return; }
        if (rc == 0) return;
    }
    const cJSON *lists = cJSON_GetObjectItemCaseSensitive(lay, "lists");
    for (int li = 0; li < o->nlists; li++) {
        const list_t *L = &o->lists[li];
        const cJSON *ll = cJSON_GetObjectItemCaseSensitive(lists, L->name);
        const cJSON *at = cJSON_GetObjectItemCaseSensitive(rows, L->at);
        const cJSON *lrows = cJSON_GetObjectItemCaseSensitive(ll, "rows");
        uint32_t esize = num(cJSON_GetObjectItemCaseSensitive(ll, "size"));
        if (!lrows || !cJSON_IsArray(at) || !esize) { o->unknown[slot] = true; return; }
        uint32_t base = num(cJSON_GetArrayItem(at, 0)), n = o->val[slot][L->count];
        if (n > MAX_ELEMS) n = MAX_ELEMS;
        o->nelem[slot][li] = 0;
        for (uint32_t e = 0; e < n; e++) {
            for (int f = 0; f < L->nf; f++) {
                int rc = read_row(o, sim, slot, &var, cJSON_GetObjectItemCaseSensitive(lrows, L->fpath[f]),
                                  base + e * esize, &o->lval[slot][li][e][f]);
                if (rc < 0) { o->unknown[slot] = true; return; }
                if (rc == 0) return;
            }
            o->nelem[slot][li] = (uint8_t)(e + 1);
        }
    }
    o->have[slot] = true;
}

/* --- rules ---------------------------------------------------------------- */

/* A name's value for the node — or for the list element a link rule is
 * looking at, whose fields come first. */
static bool lookup(const descriptor_overlay_t *o, int slot, const char *name, uint32_t *v) {
    if (o->cur_list >= 0) {
        int f = list_field(&o->lists[o->cur_list], name);
        if (f >= 0) { *v = o->lval[slot][o->cur_list][o->cur_elem][f]; return true; }
    }
    int i = field_index(o, name);
    if (i < 0) return false;
    *v = o->val[slot][i];
    return true;
}

static uint32_t value_of(const descriptor_overlay_t *o, int slot, const cJSON *rule) {
    const cJSON *f = cJSON_GetObjectItemCaseSensitive(rule, "field");
    uint32_t v = 0;
    if (!cJSON_IsString(f) || !lookup(o, slot, f->valuestring, &v)) return 0;
    const cJSON *mask = cJSON_GetObjectItemCaseSensitive(rule, "mask");
    return mask ? v & num(mask) : v;
}

/* A condition: { field, mask?, eq | ne | in | gt | lt }, or { all: [...] }
 * / { any: [...] } of conditions; none = true. */
static bool cond(const descriptor_overlay_t *o, int slot, const cJSON *c) {
    if (!c) return true;
    if (cJSON_IsBool(c)) return cJSON_IsTrue(c);
    const cJSON *all = cJSON_GetObjectItemCaseSensitive(c, "all");
    const cJSON *any = cJSON_GetObjectItemCaseSensitive(c, "any");
    if (cJSON_IsArray(all) || cJSON_IsArray(any)) {      /* { all: [...] } / { any: [...] } */
        const cJSON *e;
        if (cJSON_IsArray(all)) { cJSON_ArrayForEach(e, all) if (!cond(o, slot, e)) return false; return true; }
        cJSON_ArrayForEach(e, any) if (cond(o, slot, e)) return true;
        return false;
    }
    uint32_t v = value_of(o, slot, c);
    const cJSON *x;
    if ((x = cJSON_GetObjectItemCaseSensitive(c, "eq"))) return v == num(x);
    if ((x = cJSON_GetObjectItemCaseSensitive(c, "ne"))) return v != num(x);
    if ((x = cJSON_GetObjectItemCaseSensitive(c, "gt"))) return v > num(x);
    if ((x = cJSON_GetObjectItemCaseSensitive(c, "lt"))) return v < num(x);
    if ((x = cJSON_GetObjectItemCaseSensitive(c, "in"))) {
        const cJSON *e;
        cJSON_ArrayForEach(e, x) if (v == num(e)) return true;
        return false;
    }
    return v != 0;
}

/* "{name}" / "{name:04X}" replaced by the field's value. */
static void format(const descriptor_overlay_t *o, int slot, const char *fmt, char *out, size_t sz) {
    size_t n = 0;
    out[0] = '\0';
    for (const char *p = fmt; *p && n + 1 < sz; ) {
        if (*p != '{') { out[n++] = *p++; out[n] = '\0'; continue; }
        const char *end = strchr(p, '}');
        if (!end) { out[n++] = *p++; out[n] = '\0'; continue; }
        char name[48] = "", spec[16] = "u";
        const char *colon = memchr(p, ':', (size_t)(end - p));
        size_t nl = (size_t)((colon ? colon : end) - p - 1);
        if (nl < sizeof(name)) { memcpy(name, p + 1, nl); name[nl] = '\0'; }
        if (colon && (size_t)(end - colon - 1) < sizeof(spec) - 1) {
            size_t sl = (size_t)(end - colon - 1);
            memcpy(spec, colon + 1, sl); spec[sl] = '\0';
        }
        uint32_t fv = 0;
        bool have = lookup(o, slot, name, &fv);
        char conv = spec[0] ? spec[strlen(spec) - 1] : 'u';
        bool ok = strchr("diuxXo", conv) && strspn(spec, "0123456789") == strlen(spec) - 1;
        char pf[24];
        snprintf(pf, sizeof(pf), "%%%s", ok ? spec : "u");
        if (conv == 'd' || conv == 'i') pf[strlen(pf) - 1] = 'u';
        if (have) {
            int w = snprintf(out + n, sz - n, pf, (unsigned)fv);
            if (w > 0) n += (size_t)w < sz - n ? (size_t)w : sz - n - 1;
        }
        p = end + 1;
    }
}

/* A style value: a string (formatted), or { field, map, default } or
 * { field, color: id | hash }, either preceded by `cases:`, a list of
 * { when, value } where the first condition that holds gives the value.
 * False = no value (leave it to the canvas). */
static bool style(const descriptor_overlay_t *o, int slot, const cJSON *r, char *out, size_t sz) {
    if (!r) return false;
    if (cJSON_IsString(r)) { format(o, slot, r->valuestring, out, sz); return true; }
    if (!cJSON_IsObject(r)) return false;
    const cJSON *c;
    cJSON_ArrayForEach(c, cJSON_GetObjectItemCaseSensitive(r, "cases"))   /* first match wins */
        if (cond(o, slot, cJSON_GetObjectItemCaseSensitive(c, "when")))
            return style(o, slot, cJSON_GetObjectItemCaseSensitive(c, "value"), out, sz);
    if (!cJSON_GetObjectItemCaseSensitive(r, "field")) {
        const cJSON *def = cJSON_GetObjectItemCaseSensitive(r, "default");
        return style(o, slot, def, out, sz);
    }
    uint32_t v = value_of(o, slot, r);
    const cJSON *color = cJSON_GetObjectItemCaseSensitive(r, "color");
    if (cJSON_IsString(color)) {
        if (strcmp(color->valuestring, "hash") == 0) {
            static const char *const palette[10] = {
                "#4e79a7", "#59a14f", "#f28e2b", "#76b7b2", "#edc948",
                "#b07aa1", "#ff9da7", "#9c755f", "#bab0ac", "#e15759",
            };
            uint32_t h = 2166136261u;
            for (int i = 0; i < 4; i++) h = (h ^ ((v >> (8 * i)) & 0xFF)) * 16777619u;
            snprintf(out, sz, "%s", palette[h % 10]);
        } else {
            snprintf(out, sz, "#%06x", (unsigned)(v & 0xFFFFFF));   /* the value as the colour */
        }
        return true;
    }
    const cJSON *map = cJSON_GetObjectItemCaseSensitive(r, "map"), *e;
    cJSON_ArrayForEach(e, map)
        if ((uint32_t)strtoul(e->string, NULL, 0) == v) {
            if (cJSON_IsString(e)) format(o, slot, e->valuestring, out, sz);
            else if (cJSON_IsNumber(e)) snprintf(out, sz, "%g", e->valuedouble);
            else return false;
            return true;
        }
    const cJSON *def = cJSON_GetObjectItemCaseSensitive(r, "default");
    if (cJSON_IsString(def)) { format(o, slot, def->valuestring, out, sz); return true; }
    return false;
}

/* The node's actions: a list of [label, command], or { field, map, default }
 * whose values are such lists. */
static const cJSON *actions_of(const descriptor_overlay_t *o, int slot, const cJSON *r) {
    if (!r || cJSON_IsArray(r)) return r;
    const cJSON *c;
    cJSON_ArrayForEach(c, cJSON_GetObjectItemCaseSensitive(r, "cases"))
        if (cond(o, slot, cJSON_GetObjectItemCaseSensitive(c, "when")))
            return actions_of(o, slot, cJSON_GetObjectItemCaseSensitive(c, "value"));
    uint32_t v = value_of(o, slot, r);
    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(r, "map"))
        if ((uint32_t)strtoul(e->string, NULL, 0) == v) return e;
    return cJSON_GetObjectItemCaseSensitive(r, "default");
}

/* --- the panel ------------------------------------------------------------ */

static void append_id(char *buf, size_t sz, int id) {
    size_t l = strlen(buf);
    if (l + 8 < sz) snprintf(buf + l, sz - l, "%s%d", l ? ", " : "", id);
}

/* The node shown for slot whose key is `target`, or -1. */
static int node_with_key(const descriptor_overlay_t *o, int n, int except, uint32_t target,
                         const cJSON *show) {
    for (int t = 0; t < n; t++)
        if (t != except && o->ids[t] >= 0 && o->have[t] && o->val[t][o->key] == target && cond(o, t, show))
            return t;
    return -1;
}

/* A link per element of a list (`when` sees the element's fields): to the
 * node whose key is the element's `to` field; `once` draws each pair once. */
static void list_links(descriptor_overlay_t *o, sim_layer_t *l, int s, int n, const cJSON *lk) {
    const cJSON *lst = cJSON_GetObjectItemCaseSensitive(lk, "list");
    const cJSON *show = o->node ? cJSON_GetObjectItemCaseSensitive(o->node, "show") : NULL;
    const cJSON *sty = cJSON_GetObjectItemCaseSensitive(lk, "style");
    const cJSON *col = cJSON_GetObjectItemCaseSensitive(lk, "color");
    bool once = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(lk, "once"));
    int li = -1;
    for (int i = 0; i < o->nlists; i++)
        if (strcmp(o->lists[i].name, lst->valuestring) == 0) li = i;
    if (li < 0) return;
    int to = list_field(&o->lists[li], cJSON_GetObjectItemCaseSensitive(lk, "to")->valuestring);
    for (int e = 0; e < o->nelem[s][li]; e++) {
        o->cur_list = li; o->cur_elem = e;
        bool ok = cond(o, s, cJSON_GetObjectItemCaseSensitive(lk, "when"));
        o->cur_list = -1;
        if (!ok) continue;
        int t = node_with_key(o, n, s, o->lval[s][li][e][to], show);
        if (t < 0 || (once && o->ids[t] < o->ids[s])) continue;
        sim_layer_link(l, o->ids[s], o->ids[t], cJSON_IsString(sty) ? sty->valuestring : "plain",
                       cJSON_IsString(col) ? col->valuestring : NULL);
    }
}

void descriptor_overlay_refresh(descriptor_overlay_t *o, sim_runtime_t *sim, int64_t now_ns) {
    o->last_ns = now_ns;
    if (!o->host.publish) return;
    int n = o->host.node_count ? o->host.node_count(sim) : 0;
    if (n <= 0) return;        /* no slots (the nodes are being torn down): keep the last panel */
    if (n > MAX_NODES) n = MAX_NODES;
    o->nslots = n;
    for (int s = 0; s < n; s++) {
        o->ids[s] = o->host.node_id ? o->host.node_id(sim, s) : s + 1;
        if (o->ids[s] < 0) { o->have[s] = o->unknown[s] = false; continue; }
        read_node(o, sim, s);
    }
    const cJSON *show = o->node ? cJSON_GetObjectItemCaseSensitive(o->node, "show") : NULL;
    sim_layer_t *l = sim_layer_begin(o->title ? o->title : o->id);
    char unknown[128] = "";
    for (int s = 0; s < n; s++) {
        if (o->ids[s] < 0) continue;
        if (o->unknown[s]) append_id(unknown, sizeof unknown, o->ids[s]);
        if (!o->have[s] || !cond(o, s, show)) continue;
        char glyph[16], fill[16], ring[16], badge[8], label[48];
        sim_layer_style_t st = {0};
        if (o->node) {
#define S(member) if (style(o, s, cJSON_GetObjectItemCaseSensitive(o->node, #member), member, sizeof member)) st.member = member
            S(glyph); S(fill); S(ring); S(badge); S(label);
#undef S
            const cJSON *dash = cJSON_GetObjectItemCaseSensitive(o->node, "ring_dash");
            st.ring_dash = dash && cond(o, s, dash);
            const char *acts[2 * MAX_ACTIONS + 1];
            int na = 0;
            const cJSON *a, *list = actions_of(o, s, cJSON_GetObjectItemCaseSensitive(o->node, "actions"));
            cJSON_ArrayForEach(a, list) {
                const cJSON *lab = cJSON_GetArrayItem(a, 0), *cmd = cJSON_GetArrayItem(a, 1);
                if (cJSON_IsString(lab) && cJSON_IsString(cmd) && na < 2 * MAX_ACTIONS) {
                    acts[na++] = lab->valuestring;
                    acts[na++] = cmd->valuestring;
                }
            }
            acts[na] = NULL;
            st.actions = na ? acts : NULL;
            sim_layer_node(l, o->ids[s], &st);
        } else {
            sim_layer_node(l, o->ids[s], &st);
        }
        /* Links: this node's `from` field names the node whose key it is. */
        const cJSON *lk;
        cJSON_ArrayForEach(lk, o->links) {
            const cJSON *lst = cJSON_GetObjectItemCaseSensitive(lk, "list");
            if (lst) { list_links(o, l, s, n, lk); continue; }
            if (!cond(o, s, cJSON_GetObjectItemCaseSensitive(lk, "when"))) continue;
            int from = field_index(o, cJSON_GetObjectItemCaseSensitive(lk, "from")->valuestring);
            uint32_t target = o->val[s][from];
            for (int t = 0; t < n; t++) {
                if (t == s || o->ids[t] < 0 || !o->have[t] || o->val[t][o->key] != target) continue;
                if (!cond(o, t, show)) continue;
                const cJSON *sty = cJSON_GetObjectItemCaseSensitive(lk, "style");
                const cJSON *col = cJSON_GetObjectItemCaseSensitive(lk, "color");
                sim_layer_link(l, o->ids[s], o->ids[t], cJSON_IsString(sty) ? sty->valuestring : "plain",
                               cJSON_IsString(col) ? col->valuestring : NULL);
                break;
            }
        }
    }
    const cJSON *e;
    cJSON_ArrayForEach(e, o->legend) {
        const cJSON *a = cJSON_GetArrayItem(e, 0), *b = cJSON_GetArrayItem(e, 1);
        if (cJSON_IsString(a) && cJSON_IsString(b)) sim_layer_legend(l, a->valuestring, b->valuestring);
    }
    if (cJSON_IsArray(o->counts)) {
        char note[256] = "";
        cJSON_ArrayForEach(e, o->counts) {
            const cJSON *label = cJSON_GetObjectItemCaseSensitive(e, "label");
            const cJSON *when = cJSON_GetObjectItemCaseSensitive(e, "when");
            int c = 0;
            for (int s = 0; s < n; s++)
                if (o->ids[s] >= 0 && o->have[s] && cond(o, s, show) && cond(o, s, when)) c++;
            size_t len = strlen(note);
            snprintf(note + len, sizeof note - len, "%s%d %s", len ? ", " : "", c,
                     cJSON_IsString(label) ? label->valuestring : "");
        }
        sim_layer_note(l, note);
    }
    if (unknown[0]) {
        char note[192];
        snprintf(note, sizeof note, "layout unknown: node %s (tools/gen-struct-layout.py --update %s IMAGE)",
                 unknown, o->path);
        sim_layer_note(l, note);
    }
    char *json = sim_layer_end(l);
    if (json) o->host.publish(sim, o->id, json);
    free(json);
}

void descriptor_overlay_on_event(descriptor_overlay_t *o, sim_runtime_t *sim,
                                 const sim_observer_event_t *ev) {
    if (ev->time_ns < 2 * REFRESH_NS && o->last_ns > ev->time_ns + 10 * REFRESH_NS)
        o->last_ns = ev->time_ns;                         /* a restart: a new run */
    if (ev->time_ns - o->last_ns >= REFRESH_NS)
        descriptor_overlay_refresh(o, sim, ev->time_ns);
}
