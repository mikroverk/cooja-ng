/*
 * descriptor_overlay — see descriptor_overlay.h and
 * docs/design/overlay-descriptors.md.
 */
#include "descriptor_overlay.h"
#include "../sim/sim_config_internal.h"   /* sim_config_yaml_to_cjson */
#include "sim_layer.h"
#include "cJSON.h"

#include <ctype.h>
#include <math.h>
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
#define MAX_LINKS   16
#define ID_MAX      31                   /* SIM_RUNTIME_PANEL_ID_LEN - 1 */

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
    char    *source;                     /* the text it was loaded from */
    char     path[512];
    const char *id, *title, *symbol, *type;
    int      nfields;
    const char *fname[MAX_FIELDS], *fpath[MAX_FIELDS];
    int      key;                        /* field index links point at, -1 = none */
    int      scope;                      /* a field both ends of a link share, -1 = none */
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
    uint8_t (*pairs)[MAX_NODES][MAX_NODES / 8];   /* per link rule: pairs drawn (once) */
};

/* --- loading and checking ----------------------------------------------- */

static void fail(char *err, size_t sz, const char *path, const char *fmt, ...) {
    int n = snprintf(err, sz, "%s: ", path);
    va_list ap;
    va_start(ap, fmt);
    if (n >= 0 && (size_t)n < sz) vsnprintf(err + n, sz - (size_t)n, fmt, ap);
    va_end(ap);
}

static bool word_char(char c) { return isalnum((unsigned char)c) || c == '_' || c == '.' || c == '-'; }

/* The line of the first `word` in `text` standing alone (as a key: followed
 * by ':', a closing quote allowed in between), or 0.  The parsed tree keeps
 * no lines, so a schema error finds its place in the text this way. */
static int line_of(const char *text, const char *word, bool key) {
    size_t wl = word ? strlen(word) : 0;
    if (!text || !wl) return 0;
    int line = 1;
    for (const char *p = text; *p; p++) {
        if (*p == '\n') { line++; continue; }
        if (strncmp(p, word, wl) || (p > text && word_char(p[-1])) || word_char(p[wl])) continue;
        if (!key) return line;
        const char *q = p + wl;
        if (*q == '"' || *q == '\'') q++;
        while (*q == ' ' || *q == '\t') q++;
        if (*q == ':') return line;
    }
    return 0;
}

/* A schema error, "path:LINE: why" when `word` is found in the text. */
static void fail_at(char *err, size_t sz, const char *path, const char *text, const char *word,
                    bool key, const char *fmt, ...) {
    int line = line_of(text, word, key);
    int n = line ? snprintf(err, sz, "%s:%d: ", path, line) : snprintf(err, sz, "%s: ", path);
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

/* --- checking the rules' types ---------------------------------------------
 * Everything a rule holds is checked at load, so the refresh never meets a
 * map that is a list, a number that is not one or a row that would shift
 * past 32 bits: a descriptor that is wrong is refused with the reason,
 * whether it comes from a config or the editor's Apply. */

typedef struct {
    const descriptor_overlay_t *o;
    char *err;
    size_t errsz;
} chk_t;

static bool no(chk_t *k, const char *word, bool key, const char *fmt, ...) {
    char why[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, sizeof why, fmt, ap);
    va_end(ap);
    fail_at(k->err, k->errsz, k->o->path, k->o->source, word, key, "%s", why);
    return false;
}

/* An integer a 32-bit field can be compared with: -2^31 .. 2^32-1 (a
 * negative one is its two's complement: -1 is 0xFFFFFFFF). */
static bool int_text(const char *t, double *out) {
    if (!t || !*t) return false;
    char *end;
    long long v = strtoll(t, &end, 0);
    if (*end || v < -2147483648LL || v > 4294967295LL) return false;
    *out = (double)v;
    return true;
}

static bool is_int(const cJSON *v) {
    double d;
    if (cJSON_IsNumber(v)) d = v->valuedouble;
    else if (!cJSON_IsString(v) || !int_text(v->valuestring, &d)) return false;
    return isfinite(d) && d == floor(d) && d >= -2147483648.0 && d <= 4294967295.0;
}

/* "{name}", "{name:04X}", "{name+1}": a known name, a width of up to two
 * digits (a wider one could pad a label to gigabytes), d i u x X o. */
static bool chk_format(chk_t *k, const char *fmt, const char *where) {
    for (const char *p = fmt; (p = strchr(p, '{')); p++) {
        const char *end = strchr(p, '}');
        if (!end) return true;                        /* a lone '{' is text */
        char name[48];
        size_t nl = strcspn(p + 1, "+-:}");
        if (nl == 0 || nl >= sizeof name) return no(k, where, true, "%s: '{...}' needs a field name", where);
        memcpy(name, p + 1, nl); name[nl] = '\0';
        if (!known_name(k->o, name)) return no(k, name, false, "no field named '%s'", name);
        const char *q = p + 1 + nl;
        if (*q == '+' || *q == '-') {
            q++;
            size_t dl = strspn(q, "0123456789");
            if (dl == 0 || dl > 9) return no(k, name, false, "%s: '{%s%c...}' needs a number", where, name, q[-1]);
            q += dl;
        }
        if (*q == ':') {
            q++;
            size_t dl = strspn(q, "0123456789");
            if (dl > 2 || !q[dl] || !strchr("diuxXo", q[dl]) || q + dl + 1 != end)
                return no(k, name, false, "%s: format '{%.*s}': a width of up to 2 digits and one of d i u x X o",
                          where, (int)(end - p - 1), p + 1);
            q += dl + 1;
        }
        if (q != end) return no(k, name, false, "%s: format '{%.*s}' is not {name}, {name:spec} or {name+N}",
                                where, (int)(end - p - 1), p + 1);
        p = end;
    }
    return true;
}

static bool chk_cond(chk_t *k, const cJSON *c, const char *where) {
    static const char *const keys[] = { "field", "mask", "eq", "ne", "gt", "lt", "in", "all", "any", NULL };
    if (!c || cJSON_IsBool(c)) return true;
    if (!cJSON_IsObject(c)) return no(k, where, true, "%s: a condition is { field, eq|ne|gt|lt|in }, { all } or { any }", where);
    const cJSON *e;
    cJSON_ArrayForEach(e, c) {
        bool ok = false;
        for (int i = 0; keys[i]; i++) ok |= strcmp(e->string, keys[i]) == 0;
        if (!ok) return no(k, e->string, true, "%s: unknown key '%s' in a condition", where, e->string);
        if (!strcmp(e->string, "all") || !strcmp(e->string, "any")) {
            if (!cJSON_IsArray(e)) return no(k, e->string, true, "%s: '%s' takes a list of conditions", where, e->string);
            const cJSON *x;
            cJSON_ArrayForEach(x, e) if (!chk_cond(k, x, where)) return false;
        } else if (!strcmp(e->string, "in")) {
            const cJSON *x;
            if (!cJSON_IsArray(e)) return no(k, "in", true, "%s: 'in' takes a list of numbers", where);
            cJSON_ArrayForEach(x, e) if (!is_int(x)) return no(k, "in", true, "%s: 'in' takes a list of numbers", where);
        } else if (!strcmp(e->string, "field")) {
            if (!cJSON_IsString(e)) return no(k, "field", true, "%s: 'field' must name a field", where);
        } else if (!is_int(e)) {
            return no(k, e->string, true, "%s: '%s' must be an integer (-2147483648 to 4294967295)", where, e->string);
        }
    }
    return true;
}

/* A map's keys are the field's values: integers, decimal or 0x hex. */
static bool chk_map_key(chk_t *k, const cJSON *e, const char *where) {
    double d;
    if (!e->string || !int_text(e->string, &d))
        return no(k, e->string, true, "%s: map key '%s' is not an integer", where, e->string ? e->string : "?");
    return true;
}

static bool chk_style(chk_t *k, const cJSON *r, const char *where) {
    static const char *const keys[] = { "cases", "field", "mask", "map", "default", "color", NULL };
    if (!r) return true;
    if (cJSON_IsString(r)) return chk_format(k, r->valuestring, where);
    if (cJSON_IsNull(r)) return true;                  /* a case giving no value */
    if (!cJSON_IsObject(r)) return no(k, where, true, "%s: a value is text, or { field, map | color, default } or { cases }", where);
    const cJSON *e, *f = cJSON_GetObjectItemCaseSensitive(r, "field");
    cJSON_ArrayForEach(e, r) {
        bool ok = false;
        for (int i = 0; keys[i]; i++) ok |= strcmp(e->string, keys[i]) == 0;
        if (!ok) return no(k, e->string, true, "%s: unknown key '%s'", where, e->string);
    }
    if (f && !cJSON_IsString(f)) return no(k, where, true, "%s: 'field' must name a field", where);
    const cJSON *m = cJSON_GetObjectItemCaseSensitive(r, "mask");
    if (m && !is_int(m)) return no(k, "mask", true, "%s: 'mask' must be an integer", where);
    const cJSON *cases = cJSON_GetObjectItemCaseSensitive(r, "cases");
    if (cases) {
        if (!cJSON_IsArray(cases)) return no(k, "cases", true, "%s: 'cases' is a list of { when, value }", where);
        cJSON_ArrayForEach(e, cases) {
            if (!cJSON_IsObject(e) || !cJSON_GetObjectItemCaseSensitive(e, "value") ||
                cJSON_GetArraySize(e) != 1 + (cJSON_GetObjectItemCaseSensitive(e, "when") != NULL))
                return no(k, "cases", true, "%s: 'cases' is a list of { when, value }", where);
            if (!chk_cond(k, cJSON_GetObjectItemCaseSensitive(e, "when"), where) ||
                !chk_style(k, cJSON_GetObjectItemCaseSensitive(e, "value"), where)) return false;
        }
    }
    const cJSON *col = cJSON_GetObjectItemCaseSensitive(r, "color");
    if (col && (!cJSON_IsString(col) || (strcmp(col->valuestring, "id") && strcmp(col->valuestring, "hash"))))
        return no(k, "color", true, "%s: 'color' is id or hash", where);
    const cJSON *map = cJSON_GetObjectItemCaseSensitive(r, "map");
    if (map) {
        if (!cJSON_IsObject(map)) return no(k, "map", true, "%s: 'map' maps values to results ({ 3: hex })", where);
        cJSON_ArrayForEach(e, map) {
            if (!chk_map_key(k, e, where)) return false;
            if (cJSON_IsString(e)) { if (!chk_format(k, e->valuestring, where)) return false; }
            else if (!cJSON_IsNumber(e)) return no(k, e->string, true, "%s: map value for %s must be text or a number", where, e->string);
        }
    }
    const cJSON *def = cJSON_GetObjectItemCaseSensitive(r, "default");
    if (def && f && !cJSON_IsString(def)) return no(k, "default", true, "%s: 'default' must be text", where);
    if (def && !f) return chk_style(k, def, where);
    if (def) return chk_format(k, def->valuestring, where);
    return true;
}

/* A list of [label, command] pairs, or { field, map, default } / cases of them. */
static bool chk_actions(chk_t *k, const cJSON *r) {
    static const char *const keys[] = { "cases", "field", "mask", "map", "default", NULL };
    const char *where = "actions";
    if (!r || cJSON_IsNull(r)) return true;
    const cJSON *e;
    if (cJSON_IsArray(r)) {
        cJSON_ArrayForEach(e, r) {
            if (!cJSON_IsArray(e) || cJSON_GetArraySize(e) != 2 || !cJSON_IsString(cJSON_GetArrayItem(e, 0)) ||
                !cJSON_IsString(cJSON_GetArrayItem(e, 1)))
                return no(k, where, true, "actions: each is [label, command]");
            if (!chk_format(k, cJSON_GetArrayItem(e, 0)->valuestring, where) ||
                !chk_format(k, cJSON_GetArrayItem(e, 1)->valuestring, where)) return false;
        }
        if (cJSON_GetArraySize(r) > MAX_ACTIONS) return no(k, where, true, "actions: at most %d per node", MAX_ACTIONS);
        return true;
    }
    if (!cJSON_IsObject(r)) return no(k, where, true, "actions: a list of [label, command], or { field, map } / { cases }");
    cJSON_ArrayForEach(e, r) {
        bool ok = false;
        for (int i = 0; keys[i]; i++) ok |= strcmp(e->string, keys[i]) == 0;
        if (!ok) return no(k, e->string, true, "actions: unknown key '%s'", e->string);
    }
    const cJSON *f = cJSON_GetObjectItemCaseSensitive(r, "field"), *m = cJSON_GetObjectItemCaseSensitive(r, "mask");
    if (f && !cJSON_IsString(f)) return no(k, where, true, "actions: 'field' must name a field");
    if (m && !is_int(m)) return no(k, "mask", true, "actions: 'mask' must be an integer");
    const cJSON *cases = cJSON_GetObjectItemCaseSensitive(r, "cases");
    if (cases) {
        if (!cJSON_IsArray(cases)) return no(k, "cases", true, "actions: 'cases' is a list of { when, value }");
        cJSON_ArrayForEach(e, cases) {
            if (!cJSON_IsObject(e) || !cJSON_GetObjectItemCaseSensitive(e, "value") ||
                cJSON_GetArraySize(e) != 1 + (cJSON_GetObjectItemCaseSensitive(e, "when") != NULL))
                return no(k, "cases", true, "actions: 'cases' is a list of { when, value }");
            if (!chk_cond(k, cJSON_GetObjectItemCaseSensitive(e, "when"), where) ||
                !chk_actions(k, cJSON_GetObjectItemCaseSensitive(e, "value"))) return false;
        }
    }
    const cJSON *map = cJSON_GetObjectItemCaseSensitive(r, "map");
    if (map) {
        if (!cJSON_IsObject(map)) return no(k, "map", true, "actions: 'map' maps values to lists of [label, command]");
        cJSON_ArrayForEach(e, map) {
            if (!chk_map_key(k, e, where)) return false;
            if (!cJSON_IsArray(e) && !cJSON_IsNull(e))
                return no(k, e->string, true, "actions: map value for %s must be a list of [label, command]", e->string);
            if (!chk_actions(k, e)) return false;
        }
    }
    return chk_actions(k, cJSON_GetObjectItemCaseSensitive(r, "default"));
}

/* A row [offset, size, bit, bits]: integers, the bits within 32. */
static bool chk_row(chk_t *k, const cJSON *row, const char *name) {
    if (!cJSON_IsArray(row) || cJSON_GetArraySize(row) < 2 || cJSON_GetArraySize(row) > 4)
        return no(k, name, true, "layouts: row %s is [offset, size, bit, bits]", name);
    const cJSON *x;
    cJSON_ArrayForEach(x, row)
        if (!cJSON_IsNumber(x) || x->valuedouble < 0 || x->valuedouble > 16777216.0 || x->valuedouble != floor(x->valuedouble))
            return no(k, name, true, "layouts: row %s holds non-negative integers", name);
    double bit = cJSON_GetArraySize(row) > 2 ? cJSON_GetArrayItem(row, 2)->valuedouble : 0;
    double bits = cJSON_GetArraySize(row) > 3 ? cJSON_GetArrayItem(row, 3)->valuedouble : 0;
    if (bits > 0 && bit + bits > 32)
        return no(k, name, true, "layouts: row %s: a bitfield past 32 bits (bit %g, %g bits) cannot be read", name, bit, bits);
    if (bits == 0 && bit != 0) return no(k, name, true, "layouts: row %s: a bit offset with no bits", name);
    return true;
}

static bool chk_layouts(chk_t *k, const cJSON *lays) {
    static const char *const keys[] = { "images", "machine", "size", "rows", "lists", NULL };
    const cJSON *l, *e, *r;
    cJSON_ArrayForEach(l, lays) {
        if (!cJSON_IsObject(l)) return no(k, "layouts", true, "layouts: each entry is { machine, size, rows }");
        cJSON_ArrayForEach(e, l) {
            bool ok = false;
            for (int i = 0; keys[i]; i++) ok |= strcmp(e->string, keys[i]) == 0;
            if (!ok) return no(k, e->string, true, "layouts: unknown key '%s'", e->string);
        }
        if (!is_int(cJSON_GetObjectItemCaseSensitive(l, "machine")) || !is_int(cJSON_GetObjectItemCaseSensitive(l, "size")) ||
            !cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(l, "rows")))
            return no(k, "layouts", true, "layouts: each entry is { machine, size, rows }");
        cJSON_ArrayForEach(r, cJSON_GetObjectItemCaseSensitive(l, "rows"))
            if (!chk_row(k, r, r->string)) return false;
        const cJSON *lists = cJSON_GetObjectItemCaseSensitive(l, "lists");
        if (lists && !cJSON_IsObject(lists)) return no(k, "lists", true, "layouts: 'lists' maps list names to { size, rows }");
        cJSON_ArrayForEach(e, lists) {
            if (!cJSON_IsObject(e) || !is_int(cJSON_GetObjectItemCaseSensitive(e, "size")) ||
                !cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(e, "rows")))
                return no(k, e->string, true, "layouts: list %s is { size, rows }", e->string);
            cJSON_ArrayForEach(r, cJSON_GetObjectItemCaseSensitive(e, "rows"))
                if (!chk_row(k, r, r->string)) return false;
        }
    }
    return true;
}

static bool chk_all(chk_t *k) {
    const descriptor_overlay_t *o = k->o;
    const cJSON *e;
    if (o->node) {
        static const char *const styles[] = { "glyph", "fill", "ring", "badge", "label", NULL };
        for (int i = 0; styles[i]; i++)
            if (!chk_style(k, cJSON_GetObjectItemCaseSensitive(o->node, styles[i]), styles[i])) return false;
        if (!chk_cond(k, cJSON_GetObjectItemCaseSensitive(o->node, "show"), "show") ||
            !chk_cond(k, cJSON_GetObjectItemCaseSensitive(o->node, "ring_dash"), "ring_dash") ||
            !chk_actions(k, cJSON_GetObjectItemCaseSensitive(o->node, "actions"))) return false;
    }
    int nl = 0;
    cJSON_ArrayForEach(e, o->links) {
        if (++nl > MAX_LINKS) return no(k, "links", true, "links: at most %d", MAX_LINKS);
        const cJSON *st = cJSON_GetObjectItemCaseSensitive(e, "style"), *col = cJSON_GetObjectItemCaseSensitive(e, "color");
        const cJSON *once = cJSON_GetObjectItemCaseSensitive(e, "once");
        if ((st && !cJSON_IsString(st)) || (col && !cJSON_IsString(col)) || (once && !cJSON_IsBool(once)))
            return no(k, "links", true, "link: 'style' and 'color' are text, 'once' true or false");
        if (!chk_cond(k, cJSON_GetObjectItemCaseSensitive(e, "when"), "link")) return false;
    }
    if (o->legend) {
        if (!cJSON_IsArray(o->legend)) return no(k, "legend", true, "legend: a list of [key, text]");
        cJSON_ArrayForEach(e, o->legend)
            if (!cJSON_IsArray(e) || cJSON_GetArraySize(e) != 2 || !cJSON_IsString(cJSON_GetArrayItem(e, 0)) ||
                !cJSON_IsString(cJSON_GetArrayItem(e, 1)))
                return no(k, "legend", true, "legend: a list of [key, text]");
    }
    if (o->counts) {
        if (!cJSON_IsArray(o->counts)) return no(k, "counts", true, "counts: a list of { label, when }");
        cJSON_ArrayForEach(e, o->counts) {
            const cJSON *lab = cJSON_GetObjectItemCaseSensitive(e, "label");
            bool two = cJSON_IsArray(lab) && cJSON_GetArraySize(lab) == 2 && cJSON_IsString(cJSON_GetArrayItem(lab, 0)) &&
                       cJSON_IsString(cJSON_GetArrayItem(lab, 1));
            if (!cJSON_IsObject(e) || !(cJSON_IsString(lab) || two) ||
                cJSON_GetArraySize(e) != 1 + (cJSON_GetObjectItemCaseSensitive(e, "when") != NULL))
                return no(k, "counts", true, "counts: a list of { label: word or [one, many], when }");
            if (!chk_cond(k, cJSON_GetObjectItemCaseSensitive(e, "when"), "counts")) return false;
        }
    }
    return chk_layouts(k, o->layouts);
}

/* An id is a panel key: in the panels JSON, the page's lookups and the
 * recordings, unescaped, and at most 31 bytes in the runtime's table. */
static bool good_id(const char *id) {
    size_t n = strlen(id);
    if (n == 0 || n > ID_MAX) return false;
    for (size_t i = 0; i < n; i++)
        if (!isalnum((unsigned char)id[i]) && id[i] != '.' && id[i] != '_' && id[i] != '-') return false;
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
    descriptor_overlay_t *o = descriptor_overlay_load_text(path, text, got, host, err, errsz);
    free(text);
    return o;
}

const char *descriptor_overlay_id(const descriptor_overlay_t *o) { return o->id; }
const char *descriptor_overlay_title(const descriptor_overlay_t *o) { return o->title ? o->title : o->id; }
const char *descriptor_overlay_source(const descriptor_overlay_t *o) { return o->source; }

descriptor_overlay_t *descriptor_overlay_load_text(const char *path, const char *text, size_t got,
                                                   rpl_overlay_host_t host, char *err, size_t errsz) {
    if (got > 1024 * 1024) { fail(err, errsz, path, "not a descriptor (size)"); return NULL; }
    size_t i0 = 0;
    while (i0 < got && (text[i0] == ' ' || text[i0] == '\n' || text[i0] == '\t' || text[i0] == '\r')) i0++;
    bool json = i0 < got && text[i0] == '{';
    cJSON *doc = json ? cJSON_ParseWithLength(text, got) : sim_config_yaml_to_cjson(text, got, path);
    if (!cJSON_IsObject(doc)) {
        const char *at = json && !doc ? cJSON_GetErrorPtr() : NULL;
        cJSON_Delete(doc);
        const char *why = json ? "" : sim_config_yaml_last_error();
        if (why[0]) {
            snprintf(err, errsz, "%s", why);
        } else if (at && at >= text && at <= text + got) {     /* JSON: where it stopped */
            int line = 1, col = 1;
            for (const char *p = text; p < at; p++) { if (*p == '\n') { line++; col = 1; } else col++; }
            snprintf(err, errsz, "%s:%d:%d: not valid JSON here", path, line, col);
        } else {
            fail(err, errsz, path, "not a YAML/JSON mapping");
        }
        return NULL;
    }
    descriptor_overlay_t *o = calloc(1, sizeof(*o));
    if (!o) { cJSON_Delete(doc); fail(err, errsz, path, "out of memory"); return NULL; }
    o->doc = doc;
    o->source = malloc(got + 1);
    if (o->source) { memcpy(o->source, text, got); o->source[got] = '\0'; }
    o->host = host;
    o->key = o->scope = -1;
    snprintf(o->path, sizeof(o->path), "%s", path);

    static const char *const top[] = { "overlay", "title", "symbol", "struct", "fields", "key", "scope",
                                       "lists", "node", "links", "legend", "counts", "layouts", NULL };
    static const char *const list_keys[] = { "at", "count", "struct", "fields", NULL };
    static const char *const node_keys[] = { "show", "glyph", "fill", "ring", "ring_dash",
                                             "badge", "label", "actions", NULL };
    static const char *const link_keys[] = { "from", "list", "to", "once", "style", "color", "when", NULL };
    const char *bad = NULL;
    const char *src = o->source;
    if (!only_keys(doc, top, &bad)) { fail_at(err, errsz, path, src, bad, true, "unknown key '%s'", bad); goto bad; }
    const cJSON *v;
#define STR(key, dst, required) \
    v = cJSON_GetObjectItemCaseSensitive(doc, key); \
    if (v && !cJSON_IsString(v)) { fail_at(err, errsz, path, src, key, true, "'%s' must be a string", key); goto bad; } \
    if (!v && required) { fail(err, errsz, path, "'%s' is missing", key); goto bad; } \
    dst = v ? v->valuestring : NULL;
    STR("overlay", o->id, true);
    if (!good_id(o->id)) {
        fail_at(err, errsz, path, src, "overlay", true,
                "overlay id '%.40s%s': 1 to %d letters, digits, '.', '_' or '-' (it is the panel's key)",
                o->id, strlen(o->id) > 40 ? "..." : "", ID_MAX);
        goto bad;
    }
    STR("title", o->title, false);
    STR("symbol", o->symbol, true);
    STR("struct", o->type, true);
#undef STR
    const cJSON *fields = cJSON_GetObjectItemCaseSensitive(doc, "fields");
    if (!cJSON_IsObject(fields) || !fields->child) { fail_at(err, errsz, path, src, "fields", true, "'fields' must map names to member paths"); goto bad; }
    cJSON_ArrayForEach(v, fields) {
        if (!cJSON_IsString(v)) { fail_at(err, errsz, path, src, v->string, true, "field '%s': a member path (string)", v->string); goto bad; }
        if (o->nfields >= MAX_FIELDS) { fail_at(err, errsz, path, src, "fields", true, "more than %d fields", MAX_FIELDS); goto bad; }
        o->fname[o->nfields] = v->string;
        o->fpath[o->nfields++] = v->valuestring;
    }
    v = cJSON_GetObjectItemCaseSensitive(doc, "key");
    if (v) {
        if (!cJSON_IsString(v) || (o->key = field_index(o, v->valuestring)) < 0) {
            fail_at(err, errsz, path, src, "key", true, "'key' must name a field"); goto bad;
        }
    }
    v = cJSON_GetObjectItemCaseSensitive(doc, "scope");
    if (v) {
        if (!cJSON_IsString(v) || (o->scope = field_index(o, v->valuestring)) < 0) {
            fail_at(err, errsz, path, src, "scope", true, "'scope' must name a field"); goto bad;
        }
    }
    o->cur_list = -1;
    const cJSON *lists = cJSON_GetObjectItemCaseSensitive(doc, "lists");
    if (lists) {
        if (!cJSON_IsObject(lists)) { fail_at(err, errsz, path, src, "lists", true, "'lists' must map names to lists"); goto bad; }
        cJSON_ArrayForEach(v, lists) {
            if (o->nlists >= MAX_LISTS) { fail_at(err, errsz, path, src, "lists", true, "more than %d lists", MAX_LISTS); goto bad; }
            list_t *L = &o->lists[o->nlists];
            const cJSON *at = cJSON_GetObjectItemCaseSensitive(v, "at");
            const cJSON *cnt = cJSON_GetObjectItemCaseSensitive(v, "count");
            const cJSON *ty = cJSON_GetObjectItemCaseSensitive(v, "struct");
            const cJSON *lf = cJSON_GetObjectItemCaseSensitive(v, "fields"), *e;
            if (!cJSON_IsObject(v) || !only_keys(v, list_keys, &bad)) { fail_at(err, errsz, path, src, bad ? bad : v->string, true, "list %s: unknown key '%s'", v->string, bad ? bad : "?"); goto bad; }
            if (!cJSON_IsString(at) || !cJSON_IsString(ty) || !cJSON_IsObject(lf)) { fail_at(err, errsz, path, src, v->string, true, "list %s needs 'at', 'struct' and 'fields'", v->string); goto bad; }
            if (!cJSON_IsString(cnt) || (L->count = field_index(o, cnt->valuestring)) < 0) { fail_at(err, errsz, path, src, v->string, true, "list %s: 'count' must name a field", v->string); goto bad; }
            L->name = v->string; L->at = at->valuestring; L->type = ty->valuestring;
            cJSON_ArrayForEach(e, lf) {
                if (!cJSON_IsString(e) || L->nf >= MAX_LFIELDS) { fail_at(err, errsz, path, src, v->string, true, "list %s: fields are up to %d member paths", v->string, MAX_LFIELDS); goto bad; }
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
        fail_at(err, errsz, path, src, bad ? bad : "node", true, "node: unknown key '%s'", bad ? bad : "?"); goto bad;
    }
    o->links = cJSON_GetObjectItemCaseSensitive(doc, "links");
    if (o->links) {
        if (!cJSON_IsArray(o->links)) { fail_at(err, errsz, path, src, "links", true, "'links' must be a list"); goto bad; }
        cJSON_ArrayForEach(v, o->links) {
            const cJSON *from = cJSON_GetObjectItemCaseSensitive(v, "from");
            if (!cJSON_IsObject(v) || !only_keys(v, link_keys, &bad)) { fail_at(err, errsz, path, src, bad ? bad : "links", true, "link: unknown key '%s'", bad ? bad : "?"); goto bad; }
            const cJSON *lst = cJSON_GetObjectItemCaseSensitive(v, "list"), *to = cJSON_GetObjectItemCaseSensitive(v, "to");
            if (lst) {
                int li = -1;
                for (int i = 0; cJSON_IsString(lst) && i < o->nlists; i++)
                    if (strcmp(o->lists[i].name, lst->valuestring) == 0) li = i;
                if (li < 0) { fail_at(err, errsz, path, src, "list", true, "link: 'list' must name a list"); goto bad; }
                if (!cJSON_IsString(to) || list_field(&o->lists[li], to->valuestring) < 0) { fail_at(err, errsz, path, src, "to", true, "link: 'to' must name a field of list %s", o->lists[li].name); goto bad; }
            } else if (!cJSON_IsString(from) || field_index(o, from->valuestring) < 0) {
                fail_at(err, errsz, path, src, cJSON_IsString(from) ? from->valuestring : "links", !cJSON_IsString(from), "link: 'from' must name a field (or give 'list' and 'to')"); goto bad;
            }
            if (o->key < 0) { fail_at(err, errsz, path, src, "links", true, "links need a 'key' field to point at"); goto bad; }
        }
    }
    o->legend = cJSON_GetObjectItemCaseSensitive(doc, "legend");
    o->counts = cJSON_GetObjectItemCaseSensitive(doc, "counts");
    o->layouts = cJSON_GetObjectItemCaseSensitive(doc, "layouts");
    if (o->layouts && !cJSON_IsArray(o->layouts)) { fail_at(err, errsz, path, src, "layouts", true, "'layouts' must be a list"); goto bad; }
    if (!check_refs(o, doc, &bad)) { fail_at(err, errsz, path, src, bad, false, "no field named '%s'", bad); goto bad; }
    {
        chk_t k = { o, err, errsz };
        if (!chk_all(&k)) goto bad;
    }
    if (o->links) {
        o->pairs = calloc((size_t)cJSON_GetArraySize(o->links), sizeof *o->pairs);
        if (!o->pairs) { fail(err, errsz, path, "out of memory"); goto bad; }
    }
    return o;
bad:
    descriptor_overlay_free(o);
    return NULL;
}

void descriptor_overlay_free(descriptor_overlay_t *o) {
    if (!o) return;
    free(o->lval);
    free(o->pairs);
    free(o->source);
    cJSON_Delete(o->doc);
    free(o);
}

/* --- reading the fields --------------------------------------------------- */

/* A number as the 32-bit value it is compared with: a negative one as its
 * two's complement (-1 is 0xFFFFFFFF), the same on every host.  The load
 * checked the range; out of it (a row's own numbers) it is 0. */
static uint32_t num(const cJSON *v) {
    double d = 0;
    if (cJSON_IsNumber(v)) d = v->valuedouble;
    else if (cJSON_IsString(v)) d = (double)strtoll(v->valuestring, NULL, 0);
    else if (cJSON_IsTrue(v)) return 1;
    if (!isfinite(d) || d < -2147483648.0 || d > 4294967295.0) return 0;
    return d < 0 ? (uint32_t)(int32_t)d : (uint32_t)d;
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
    if (bits ? bit + bits > 32 : bit != 0) return -1;     /* checked at load: past 32 bits */
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

/* A map key ("3", "0x3", "-1") as the value it matches. */
static uint32_t num_key(const char *k) {
    long long v = strtoll(k, NULL, 0);
    return v < 0 ? (uint32_t)(int32_t)v : (uint32_t)v;
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

/* "{name}" / "{name:04X}" replaced by the field's value, "{name+1}" by
 * the value plus one (the load checked the names and specs). */
static void format(const descriptor_overlay_t *o, int slot, const char *fmt, char *out, size_t sz) {
    size_t n = 0;
    out[0] = '\0';
    for (const char *p = fmt; *p && n + 1 < sz; ) {
        if (*p != '{') { out[n++] = *p++; out[n] = '\0'; continue; }
        const char *end = strchr(p, '}');
        if (!end) { out[n++] = *p++; out[n] = '\0'; continue; }
        char name[48] = "", spec[16] = "u";
        const char *colon = memchr(p, ':', (size_t)(end - p));
        size_t nl = strcspn(p + 1, "+-:}");
        if (nl < sizeof(name)) { memcpy(name, p + 1, nl); name[nl] = '\0'; }
        long add = 0;
        if (p[1 + nl] == '+' || p[1 + nl] == '-') add = strtol(p + 1 + nl, NULL, 10);
        if (colon && (size_t)(end - colon - 1) < sizeof(spec) - 1) {
            size_t sl = (size_t)(end - colon - 1);
            memcpy(spec, colon + 1, sl); spec[sl] = '\0';
        }
        uint32_t fv = 0;
        bool have = lookup(o, slot, name, &fv);
        fv += (uint32_t)add;
        char conv = spec[0] ? spec[strlen(spec) - 1] : 'u';
        size_t digits = strspn(spec, "0123456789");
        bool ok = strchr("diuxXo", conv) && digits <= 2 && digits == strlen(spec) - 1;
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
        if (e->string && num_key(e->string) == v) {
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
        if (e->string && num_key(e->string) == v) return e;
    return cJSON_GetObjectItemCaseSensitive(r, "default");
}

/* --- the panel ------------------------------------------------------------ */

static void append_id(char *buf, size_t sz, int id) {
    size_t l = strlen(buf);
    if (l + 8 < sz) snprintf(buf + l, sz - l, "%s%d", l ? ", " : "", id);
}

/* The node shown whose key is `target` and, with a `scope`, whose scope
 * field equals slot `from`'s (an RLOC16 names a router only within its
 * partition), or -1. */
static int node_with_key(const descriptor_overlay_t *o, int n, int from, uint32_t target,
                         const cJSON *show) {
    for (int t = 0; t < n; t++)
        if (t != from && o->ids[t] >= 0 && o->have[t] && o->val[t][o->key] == target &&
            (o->scope < 0 || o->val[t][o->scope] == o->val[from][o->scope]) && cond(o, t, show))
            return t;
    return -1;
}

/* A link per element of a list (`when` sees the element's fields): to the
 * node whose key is the element's `to` field.  `once` draws each pair once,
 * from the lower id to the higher, whichever end lists it (two router
 * tables can disagree while a link comes up or ages out). */
static void list_links(descriptor_overlay_t *o, sim_layer_t *l, int s, int n, int rule, const cJSON *lk) {
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
        if (t < 0) continue;
        int from = o->ids[s], dest = o->ids[t];
        if (once) {
            int a = s < t ? s : t, b = s < t ? t : s;
            uint8_t *bits = &o->pairs[rule][a][b / 8];
            if (*bits & (1u << (b % 8))) continue;
            *bits |= (uint8_t)(1u << (b % 8));
            if (from > dest) { from = o->ids[t]; dest = o->ids[s]; }
        }
        sim_layer_link(l, from, dest, cJSON_IsString(sty) ? sty->valuestring : "plain",
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
    if (o->pairs) memset(o->pairs, 0, (size_t)cJSON_GetArraySize(o->links) * sizeof *o->pairs);
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
            char abuf[2 * MAX_ACTIONS][160];     /* each label and command, formatted */
            int na = 0;
            const cJSON *a, *list = actions_of(o, s, cJSON_GetObjectItemCaseSensitive(o->node, "actions"));
            cJSON_ArrayForEach(a, list) {
                const cJSON *lab = cJSON_GetArrayItem(a, 0), *cmd = cJSON_GetArrayItem(a, 1);
                if (cJSON_IsString(lab) && cJSON_IsString(cmd) && na < 2 * MAX_ACTIONS) {
                    format(o, s, lab->valuestring, abuf[na], sizeof abuf[na]);
                    acts[na] = abuf[na]; na++;
                    format(o, s, cmd->valuestring, abuf[na], sizeof abuf[na]);
                    acts[na] = abuf[na]; na++;
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
        int rule = 0;
        cJSON_ArrayForEach(lk, o->links) {
            const cJSON *lst = cJSON_GetObjectItemCaseSensitive(lk, "list");
            if (lst) { list_links(o, l, s, n, rule++, lk); continue; }
            rule++;
            if (!cond(o, s, cJSON_GetObjectItemCaseSensitive(lk, "when"))) continue;
            int from = field_index(o, cJSON_GetObjectItemCaseSensitive(lk, "from")->valuestring);
            int t = node_with_key(o, n, s, o->val[s][from], show);
            if (t < 0) continue;
            const cJSON *sty = cJSON_GetObjectItemCaseSensitive(lk, "style");
            const cJSON *col = cJSON_GetObjectItemCaseSensitive(lk, "color");
            sim_layer_link(l, o->ids[s], o->ids[t], cJSON_IsString(sty) ? sty->valuestring : "plain",
                           cJSON_IsString(col) ? col->valuestring : NULL);
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
            /* label: "leaders", or [singular, plural]: "1 leader, 2 routers" */
            const cJSON *word = cJSON_IsArray(label) ? cJSON_GetArrayItem(label, c == 1 ? 0 : 1) : label;
            size_t len = strlen(note);
            snprintf(note + len, sizeof note - len, "%s%d %s", len ? ", " : "", c,
                     cJSON_IsString(word) ? word->valuestring : "");
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

void descriptor_overlay_restart(descriptor_overlay_t *o) { o->last_ns = -REFRESH_NS; }

void descriptor_overlay_on_event(descriptor_overlay_t *o, sim_runtime_t *sim,
                                 const sim_observer_event_t *ev) {
    /* The clock more than one refresh behind the last refresh: a restart
     * (or events far out of order).  A descriptor keeps nothing from one
     * refresh to the next but its time, so either way refresh from here:
     * a false alarm costs one refresh. */
    if (ev->time_ns + REFRESH_NS < o->last_ns)
        o->last_ns = ev->time_ns - REFRESH_NS;
    if (ev->time_ns - o->last_ns >= REFRESH_NS)
        descriptor_overlay_refresh(o, sim, ev->time_ns);
}
