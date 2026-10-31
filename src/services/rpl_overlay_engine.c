/*
 * rpl_overlay_engine — see rpl_overlay_engine.h.
 */
#include "rpl_overlay_engine.h"
#include "rpl_layout.h"
#include "packet_analyzer.h"
#include "sim_layer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_NODES      128                 /* SIM_EQ_MAX_NODES */
#define REFRESH_NS     500000000LL         /* rebuild at most every 500 ms */
#define CONTIKI_ROOT_RANK 128              /* RPL_MIN_HOPRANKINC's default */

/* RPL Lite's enum rpl_dag_state. */
enum { DAG_INITIALIZED = 0, DAG_JOINED, DAG_REACHABLE, DAG_POISONING };

/* What a node's transmissions have told us. */
typedef struct {
    uint8_t  mac[8];          /* its link-layer address, Contiki byte order */
    int      mac_len;         /* 0 = not seen yet                            */
    bool     dio;             /* a DIO was seen                              */
    uint16_t rank, minhop;    /* minhop 0 = no DAG configuration option yet  */
    uint8_t  dodag[16], version, mop;
    uint8_t  parent[8];       /* link destination of its last unicast DAO    */
    int      parent_len;
} pkt_view_t;

/* One node's RPL view for a refresh. */
typedef struct {
    int      id;              /* Cooja node id, -1 = no node in the slot     */
    enum { SRC_NONE, SRC_FW, SRC_PKT } src;
    bool     joined, root;
    uint16_t rank;
    uint8_t  dodag[16];
    uint8_t  parent[8];
    int      parent_len;      /* 0 = no parent                               */
    uint8_t  mac[8];
    int      mac_len;
    bool     layout_unknown;  /* RPL Lite is there, its layout is not ours  */
} node_view_t;

struct rpl_overlay {
    rpl_overlay_host_t host;
    pkt_view_t pkt[MAX_NODES];
    node_view_t view[MAX_NODES];
    int64_t last_ns;
    bool    published;
};

rpl_overlay_t *rpl_overlay_create(rpl_overlay_host_t host) {
    rpl_overlay_t *o = calloc(1, sizeof(*o));
    if (o) o->host = host;
    return o;
}

void rpl_overlay_free(rpl_overlay_t *o) { free(o); }

/* --- packets ----------------------------------------------------------- */

/* An 802.15.4 address as Contiki stores a linkaddr: the frame carries it
 * byte-reversed. */
static int canon_addr(int mode, const uint8_t ext[8], uint16_t shrt, uint8_t out[8]) {
    if (mode == 3) { for (int i = 0; i < 8; i++) out[i] = ext[7 - i]; return 8; }
    if (mode == 2 && shrt != 0xFFFF) { out[0] = (uint8_t)(shrt >> 8); out[1] = (uint8_t)shrt; return 2; }
    return 0;
}

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

static void learn_frame(rpl_overlay_t *o, int slot, const uint8_t *bytes, int len) {
    if (slot < 0 || slot >= MAX_NODES || !bytes || len < 5 || len > 127) return;
    uint8_t buf[128];
    buf[0] = (uint8_t)len;                   /* pkt_analyze wants the PHY length first */
    memcpy(buf + 1, bytes, (size_t)len);
    pkt_info_t pi;
    if (pkt_analyze(buf, len + 1, &pi) != 0 || pi.frame_type != PKT_802154_DATA) return;
    pkt_view_t *p = &o->pkt[slot];
    uint8_t a[8];
    int alen = canon_addr(pi.src_mode, pi.src_ext, pi.src_short, a);
    if (alen) { memcpy(p->mac, a, (size_t)alen); p->mac_len = alen; }
    if (!pi.has_icmpv6 || pi.icmpv6_type != ICMPV6_RPL) return;
    const uint8_t *m = buf + pi.icmpv6_offset;
    const uint8_t *end = buf + 1 + len - 2;  /* the FCS is not payload */
    if (pi.icmpv6_code == RPL_DIO && m + 4 + 24 <= end) {
        const uint8_t *b = m + 4;
        p->dio = true;
        p->version = b[1];
        p->rank = be16(b + 2);
        p->mop = (uint8_t)((b[4] >> 3) & 7);
        memcpy(p->dodag, b + 8, 16);
        for (const uint8_t *opt = b + 24; opt < end; ) {
            if (opt[0] == 0) { opt++; continue; }            /* Pad1 */
            if (opt + 2 > end || opt + 2 + opt[1] > end) break;
            if (opt[0] == 0x04 && opt[1] >= 8) p->minhop = be16(opt + 8);  /* DODAG configuration */
            opt += 2 + opt[1];
        }
    } else if (pi.icmpv6_code == RPL_DAO) {
        int plen = canon_addr(pi.dst_mode, pi.dst_ext, pi.dst_short, a);
        if (plen && !(plen == 2 && a[0] == 0xFF && a[1] == 0xFF)) {
            memcpy(p->parent, a, (size_t)plen);
            p->parent_len = plen;
        }
    }
}

/* --- firmware ---------------------------------------------------------- */

static bool fw_field(const char *layout, const char *type, const char *field,
                     uint32_t *off, uint32_t *size) {
    return struct_layout_field(rpl_layout_rows, sizeof(rpl_layout_rows) / sizeof(rpl_layout_rows[0]),
                               layout, type, field, off, size);
}

/* Little-endian unsigned of 1, 2 or 4 bytes at var + off. */
static bool fw_uint(rpl_overlay_t *o, sim_runtime_t *sim, int slot, const sim_fwvar_t *v,
                    uint32_t off, uint32_t size, uint32_t *out) {
    uint8_t b[4] = {0};
    if (size == 0 || size > 4 || !o->host.read(sim, slot, v, off, b, size)) return false;
    *out = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return true;
}

static bool fw_own_addr(rpl_overlay_t *o, sim_runtime_t *sim, int slot, node_view_t *nv) {
    sim_fwvar_t v;
    if (!o->host.resolve(sim, slot, "linkaddr_node_addr", &v) || (v.size != 8 && v.size != 2))
        return false;
    if (!o->host.read(sim, slot, &v, 0, nv->mac, v.size)) return false;
    nv->mac_len = (int)v.size;
    return true;
}

/* The link address of the neighbour at `ptr` in RPL's neighbour table: its
 * index there is its index in the nbr-table keys, which hold the address. */
static bool fw_parent_addr(rpl_overlay_t *o, sim_runtime_t *sim, int slot, const char *layout,
                           uint32_t ptr, node_view_t *nv) {
    sim_fwvar_t mem, tab, keys;
    uint32_t isz_off, isz_size, key_size, ll_off, ll_size, unused;
    if (!o->host.resolve(sim, slot, "_rpl_neighbors_mem", &mem) ||
        !o->host.resolve(sim, slot, "rpl_neighbors_struct", &tab) ||
        !o->host.resolve(sim, slot, "neighbor_addr_mem_memb_mem", &keys) ||
        !fw_field(layout, "nbr_table_t", "item_size", &isz_off, &isz_size) ||
        !fw_field(layout, "nbr_table_key_t", "", &unused, &key_size) ||
        !fw_field(layout, "nbr_table_key_t", "lladdr", &ll_off, &ll_size) ||
        ll_size > sizeof(nv->parent))
        return false;
    uint32_t item;
    if (!fw_uint(o, sim, slot, &tab, isz_off, isz_size, &item) || item == 0 ||
        ptr < mem.addr || (ptr - mem.addr) % item != 0 ||
        (mem.size && ptr - mem.addr >= mem.size))
        return false;
    uint32_t idx = (ptr - mem.addr) / item;
    if (keys.size && (idx + 1) * key_size > keys.size) return false;
    if (!o->host.read(sim, slot, &keys, idx * key_size + ll_off, nv->parent, ll_size)) return false;
    nv->parent_len = (int)ll_size;
    return true;
}

/* RPL Lite's curr_instance, when the image has it and we know its layout. */
static bool fw_view(rpl_overlay_t *o, sim_runtime_t *sim, int slot, node_view_t *nv) {
    if (!o->host.resolve || !o->host.read) return false;
    fw_own_addr(o, sim, slot, nv);
    sim_fwvar_t ci;
    if (!o->host.resolve(sim, slot, "curr_instance", &ci)) return false;   /* not RPL Lite */
    const char *layout = struct_layout_pick(rpl_layout_rows,
                                            sizeof(rpl_layout_rows) / sizeof(rpl_layout_rows[0]),
                                            ci.machine, "rpl_instance_t", ci.size);
    if (!layout) { nv->layout_unknown = true; return false; }
    uint32_t off, size, used, rank, minhop, state, ptr;
    if (!fw_field(layout, "rpl_instance_t", "used", &off, &size) ||
        !fw_uint(o, sim, slot, &ci, off, size, &used))
        return false;
    nv->src = SRC_FW;
    if (!used) return true;                                    /* not joined */
    if (!fw_field(layout, "rpl_instance_t", "dag.rank", &off, &size) ||
        !fw_uint(o, sim, slot, &ci, off, size, &rank) ||
        !fw_field(layout, "rpl_instance_t", "min_hoprankinc", &off, &size) ||
        !fw_uint(o, sim, slot, &ci, off, size, &minhop) ||
        !fw_field(layout, "rpl_instance_t", "dag.state", &off, &size) ||
        !fw_uint(o, sim, slot, &ci, off, size, &state) ||
        !fw_field(layout, "rpl_instance_t", "dag.dag_id", &off, &size) || size != 16 ||
        !o->host.read(sim, slot, &ci, off, nv->dodag, 16) ||
        !fw_field(layout, "rpl_instance_t", "dag.preferred_parent", &off, &size) ||
        !fw_uint(o, sim, slot, &ci, off, size, &ptr))
        return true;                                           /* used, unreadable */
    nv->rank = (uint16_t)rank;
    nv->joined = state >= DAG_JOINED && rank != 0xFFFF;
    nv->root = nv->joined && rank == minhop;
    if (nv->joined && !nv->root && ptr)
        fw_parent_addr(o, sim, slot, layout, ptr, nv);
    return true;
}

static void pkt_view(const pkt_view_t *p, node_view_t *nv) {
    if (!nv->mac_len && p->mac_len) { memcpy(nv->mac, p->mac, 8); nv->mac_len = p->mac_len; }
    if (!p->dio) return;
    nv->src = SRC_PKT;
    nv->rank = p->rank;
    nv->joined = p->rank != 0xFFFF;
    nv->root = nv->joined && p->rank == (p->minhop ? p->minhop : CONTIKI_ROOT_RANK);
    memcpy(nv->dodag, p->dodag, 16);
    if (nv->joined && !nv->root && p->parent_len) {
        memcpy(nv->parent, p->parent, 8);
        nv->parent_len = p->parent_len;
    }
}

/* --- the panel ---------------------------------------------------------- */

/* A DODAG's colour: its id hashed into a ten-colour palette. */
static const char *dodag_colour(const uint8_t id[16]) {
    static const char *const palette[10] = {
        "#4e79a7", "#59a14f", "#f28e2b", "#e15759", "#76b7b2",
        "#edc948", "#b07aa1", "#ff9da7", "#9c755f", "#bab0ac",
    };
    uint32_t h = 2166136261u;
    for (int i = 0; i < 16; i++) h = (h ^ id[i]) * 16777619u;
    return palette[h % 10];
}

static int slot_of_addr(const rpl_overlay_t *o, int n, const uint8_t *a, int len) {
    for (int s = 0; s < n; s++)
        if (o->view[s].id >= 0 && o->view[s].mac_len == len && memcmp(o->view[s].mac, a, (size_t)len) == 0)
            return s;
    return -1;
}

static void append_id(char *buf, size_t sz, int id) {
    size_t l = strlen(buf);
    if (l + 8 < sz) snprintf(buf + l, sz - l, "%s%d", l ? ", " : "", id);
}

void rpl_overlay_refresh(rpl_overlay_t *o, sim_runtime_t *sim, int64_t now_ns) {
    o->last_ns = now_ns;
    if (!o->host.publish) return;
    int n = o->host.node_count ? o->host.node_count(sim) : 0;
    if (n <= 0) return;        /* no slots (the nodes are being torn down): keep the last panel */
    if (n > MAX_NODES) n = MAX_NODES;
    for (int s = 0; s < n; s++) {
        node_view_t *nv = &o->view[s];
        memset(nv, 0, sizeof(*nv));
        nv->id = o->host.node_id ? o->host.node_id(sim, s) : s + 1;
        if (nv->id < 0) continue;
        if (!fw_view(o, sim, s, nv)) pkt_view(&o->pkt[s], nv);
        else if (!nv->mac_len && o->pkt[s].mac_len) {
            memcpy(nv->mac, o->pkt[s].mac, 8);
            nv->mac_len = o->pkt[s].mac_len;
        }
    }

    sim_layer_t *l = sim_layer_begin(o->host.title ? o->host.title : "RPL");
    int roots = 0, joined = 0, waiting = 0, from_pkt = 0;
    char unknown[128] = "", silent[128] = "";
    for (int s = 0; s < n; s++) {
        const node_view_t *nv = &o->view[s];
        if (nv->id < 0) continue;
        if (nv->layout_unknown) append_id(unknown, sizeof unknown, nv->id);
        if (nv->src == SRC_NONE) { append_id(silent, sizeof silent, nv->id); continue; }
        if (nv->src == SRC_PKT) from_pkt++;
        char label[32];
        const char *mark = nv->src == SRC_PKT ? "*" : "";
        if (!nv->joined) {
            waiting++;
            snprintf(label, sizeof label, "not joined%s", mark);
            sim_layer_node(l, nv->id, &(sim_layer_style_t){ .glyph = "circle", .ring = "#888888",
                                                            .label = label });
            continue;
        }
        joined++;
        const char *fill = dodag_colour(nv->dodag);
        snprintf(label, sizeof label, "rank %u%s", (unsigned)nv->rank, mark);
        if (nv->root) roots++;
        sim_layer_node(l, nv->id, &(sim_layer_style_t){
            .glyph = nv->root ? "square" : "circle", .fill = fill,
            .ring = nv->root ? fill : NULL, .badge = nv->root ? "R" : NULL, .label = label });
        if (nv->parent_len) {
            int ps = slot_of_addr(o, n, nv->parent, nv->parent_len);
            if (ps >= 0) sim_layer_link(l, nv->id, o->view[ps].id, "parent", NULL);
        }
    }
    sim_layer_legend(l, "square", "DODAG root");
    sim_layer_legend(l, "colour", "DODAG (one colour per DODAG ID)");
    sim_layer_legend(l, "arrow", "preferred parent");
    if (from_pkt) sim_layer_legend(l, "*", "seen in packets (DIO, DAO), not read from memory");
    char note[256];
    snprintf(note, sizeof note, "%d root%s, %d joined, %d not joined", roots, roots == 1 ? "" : "s",
             joined - roots, waiting);
    sim_layer_note(l, note);
    if (unknown[0]) {
        snprintf(note, sizeof note, "layout unknown (packets used): node %s", unknown);
        sim_layer_note(l, note);
    }
    if (silent[0]) {
        snprintf(note, sizeof note, "no RPL seen yet: node %s", silent);
        sim_layer_note(l, note);
    }
    char *json = sim_layer_end(l);
    if (json) {
        o->host.publish(sim, o->host.panel_id ? o->host.panel_id : "rpl", json);
        o->published = true;
    }
    free(json);
}

void rpl_overlay_on_event(rpl_overlay_t *o, sim_runtime_t *sim,
                          const sim_observer_event_t *ev) {
    /* Time running back further than a frame's start can lag the event that
     * refreshed is a restart (the UI's or the shell's): a new run, so
     * forget what the old one's packets said. */
    if (ev->time_ns + REFRESH_NS < o->last_ns) {
        memset(o->pkt, 0, sizeof(o->pkt));
        o->last_ns = ev->time_ns;
    }
    if (ev->kind == SIM_OBS_PACKET_FRAME && ev->u.frame.is_tx)
        learn_frame(o, ev->mote_index, ev->u.frame.bytes, ev->u.frame.bytes_len);
    if (ev->time_ns - o->last_ns >= REFRESH_NS)
        rpl_overlay_refresh(o, sim, ev->time_ns);
}
