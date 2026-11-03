/*
 * Firmware-variable reads, canvas layers and the rpl overlay's packet
 * source — unit tests, no simulation run.
 *
 *  - sim_fwvar on a real Sky image loaded into an MSP430 (behind a mock
 *    sim_control bundle): resolve by symbol with its size and machine,
 *    read what is in memory, refuse unknown symbols, reads past memory,
 *    removed nodes and nodes with no emulated CPU;
 *  - memory-only reads on an ARM CPU: SRAM yes, a peripheral never;
 *  - sim_layer's JSON (it parses, strings are escaped) and struct_layout's
 *    pick-by-size;
 *  - the rpl overlay from packets alone: a DIO and a DAO captured from a
 *    Sky chain give the root, the rank and the parent link.
 * tools/check-plugin.sh runs the overlay end to end on Sky, CC2538 and
 * nRF52840 images.
 */
#include "sim_fwvar.h"
#include "sim_layer.h"
#include "struct_layout.h"
#include "sim_control.h"
#include "sim_mote.h"
#include "arm_cpu.h"
#include "msp430_platform.h"
#include "msp430_elf.h"
#include "elf_loader.h"
#include "cJSON.h"
#include "../src/services/rpl_overlay_engine.h"
#include "../src/services/rpl_layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, failures;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("  FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* --- a mock runner: slot 0 an MSP430 on a Sky image, slot 1 "native" --- */

#define IMG "firmware/sky/udp-client.sky"

static sim_runtime_t rt;
static sim_control_t ctl;
static msp430_platform_t plat;
static bool slot0_removed;

static int m_count(void *u) { (void)u; return 2; }
static bool m_describe(void *u, int idx, sim_control_node_info_t *o) {
    (void)u;
    if (idx < 0 || idx > 1) return false;
    memset(o, 0, sizeof(*o));
    o->index = idx;
    o->id = idx + 1;
    o->firmware = IMG;
    o->secure_firmware = "";
    o->removed = idx == 0 && slot0_removed;
    return true;
}
static void *m_iface(void *u, int idx, int iface) {
    (void)u;
    return idx == 0 && iface == SIM_MOTE_IFACE_MSP430_CPU ? &plat.cpu : NULL;
}

static void test_fwvar_msp430(void) {
    FILE *f = fopen(IMG, "rb");
    if (!f) { printf("  (skipped: %s not found)\n", IMG); return; }
    fclose(f);
    msp430_platform_init(&plat, msp430_platform_find("sky"));
    CHECK(msp430_load_elf(&plat.cpu, IMG) == 0, "load %s", IMG);
    sim_runtime_init(&rt);
    sim_control_ops_t ops = { .node_count = m_count, .describe = m_describe, .get_interface = m_iface };
    sim_control_init(&ctl, &rt, &ops);
    CHECK(rt.control == &ctl, "sim_control_init hands the runtime its control bundle");
    CHECK(sim_fwvar_node_count(&rt) == 2 && sim_fwvar_node_id(&rt, 1) == 2, "node count and ids");

    sim_fwvar_t v;
    uint32_t addr = 0, size = 0;
    CHECK(sim_fwvar_resolve(&rt, 0, "linkaddr_node_addr", &v) && v.valid, "resolve linkaddr_node_addr");
    CHECK(elf_lookup_symbol_sized(IMG, "linkaddr_node_addr", &addr, &size) && v.addr == addr && v.size == 8,
          "the symbol's address and size (0x%x, %u)", v.addr, v.size);
    CHECK(v.machine == 105, "an MSP430 image (machine %u)", v.machine);

    /* What is in memory comes back as stored, little-endian for the u16. */
    const uint8_t want[8] = { 0x00, 0x12, 0x74, 0x07, 0x00, 0x07, 0x07, 0x07 };
    memcpy(plat.cpu.memory + v.addr, want, 8);
    uint8_t got[8] = {0};
    uint16_t w = 0;
    CHECK(sim_fwvar_read(&rt, 0, &v, 0, got, 8) && !memcmp(got, want, 8), "read the bytes in memory");
    CHECK(sim_fwvar_read_u16(&rt, 0, &v, 2, &w) && w == 0x0774, "u16 is little-endian (0x%04x)", w);

    CHECK(!sim_fwvar_resolve(&rt, 0, "no_such_symbol_here", &v) && !v.valid, "an unknown symbol is not found");
    sim_fwvar_t far = { .addr = plat.cpu.max_mem - 2, .size = 4, .valid = true };
    CHECK(!sim_fwvar_read(&rt, 0, &far, 0, got, 4), "a read past the end of memory is refused");
    sim_fwvar_t low = { .addr = 0x1100, .size = 2, .valid = true };   /* + offset = 0x100 after wrapping */
    CHECK(!sim_fwvar_read(&rt, 0, &low, 0xfffff000u, got, 2), "an offset that wraps the address round is refused");
    CHECK(sim_fwvar_resolve(&rt, 1, "linkaddr_node_addr", &v) && !sim_fwvar_read(&rt, 1, &v, 0, got, 8),
          "a node with no emulated CPU resolves but reads nothing");
    slot0_removed = true;
    CHECK(!sim_fwvar_resolve(&rt, 0, "linkaddr_node_addr", &v) && sim_fwvar_node_id(&rt, 0) == -1,
          "a removed node resolves nothing");
    slot0_removed = false;
    CHECK(sim_fwvar_cache_count(IMG) >= 2, "one image, its entries cached once for both slots");

    /* The image's curr_instance picks its layout by machine and size. */
    CHECK(sim_fwvar_resolve(&rt, 0, "curr_instance", &v), "resolve curr_instance");
    const char *lay = struct_layout_pick(rpl_layout_rows, sizeof(rpl_layout_rows) / sizeof(rpl_layout_rows[0]),
                                         v.machine, "rpl_instance_t", v.size);
    CHECK(lay && !strcmp(lay, "contiki-ng-msp430"), "the Sky image's RPL layout (%s)", lay ? lay : "none");
    CHECK(!struct_layout_pick(rpl_layout_rows, sizeof(rpl_layout_rows) / sizeof(rpl_layout_rows[0]),
                              v.machine, "rpl_instance_t", v.size + 2), "another size picks no layout");
    uint32_t off = 0, sz = 0;
    CHECK(struct_layout_field(rpl_layout_rows, sizeof(rpl_layout_rows) / sizeof(rpl_layout_rows[0]),
                              lay ? lay : "", "rpl_instance_t", "dag.rank", &off, &sz) && sz == 2,
          "a field's offset and size");
    msp430_platform_destroy(&plat);
    rt.control = NULL;
}

static void test_arm_memory_only(void) {
    static uint8_t sram[64];
    arm_cpu_t *cpu = calloc(1, sizeof(*cpu));
    if (!cpu) return;
    cpu->sram = sram;
    cpu->sram_base = 0x20000000u;
    cpu->sram_end = 0x20000000u + sizeof(sram);
    sram[4] = 0x5a;
    uint8_t b = 0;
    CHECK(sim_fwvar_mem_read(cpu, NULL, 0x20000004u, &b, 1) && b == 0x5a, "ARM SRAM reads");
    CHECK(!sim_fwvar_mem_read(cpu, NULL, 0x2000003fu, &b, 2), "a read running off SRAM is refused");
    CHECK(!sim_fwvar_is_memory(cpu, NULL, 0x40000000u) && !sim_fwvar_mem_read(cpu, NULL, 0x40000000u, &b, 1),
          "a peripheral register is never read");
    free(cpu);
}

static void test_layer_json(void) {
    sim_layer_t *l = sim_layer_begin("R\"P\\L");
    sim_layer_node(l, 1, &(sim_layer_style_t){ .glyph = "square", .fill = "#2a6", .badge = "R", .label = "rank 128" });
    sim_layer_node(l, 4, &(sim_layer_style_t){ .ring = "#888", .ring_dash = true, .label = "a\nb" });
    sim_layer_link(l, 4, 1, "parent", NULL);
    sim_layer_link(l, 2, 1, NULL, "#f00");
    sim_layer_legend(l, "square", "DODAG root");
    sim_layer_note(l, "first");
    sim_layer_note(l, "second");
    char *s = sim_layer_end(l);
    cJSON *j = s ? cJSON_Parse(s) : NULL;
    CHECK(j, "the layer is JSON: %s", s ? s : "(null)");
    if (j) {
        cJSON *layer = cJSON_GetObjectItem(j, "layer");
        cJSON *n1 = cJSON_GetObjectItem(cJSON_GetObjectItem(layer, "nodes"), "1");
        cJSON *n4 = cJSON_GetObjectItem(cJSON_GetObjectItem(layer, "nodes"), "4");
        cJSON *links = cJSON_GetObjectItem(layer, "links");
        CHECK(!strcmp(cJSON_GetObjectItem(j, "title")->valuestring, "R\"P\\L"), "the title survives escaping");
        CHECK(!strcmp(cJSON_GetObjectItem(j, "text")->valuestring, "first\nsecond"), "notes join with newlines");
        CHECK(n1 && !strcmp(cJSON_GetObjectItem(n1, "glyph")->valuestring, "square") &&
              !cJSON_GetObjectItem(n1, "ring"), "a node carries only the members it was given");
        CHECK(n4 && cJSON_IsTrue(cJSON_GetObjectItem(n4, "ring_dash")) &&
              !strcmp(cJSON_GetObjectItem(n4, "label")->valuestring, "a\nb"), "ring_dash, an escaped label");
        CHECK(cJSON_GetArraySize(links) == 2 &&
              !strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(links, 1), "style")->valuestring, "plain") &&
              !strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(links, 1), "color")->valuestring, "#f00"),
              "a link with no style is plain, its colour kept");
        cJSON_Delete(j);
    }
    free(s);
    char *empty = sim_layer_end(sim_layer_begin(NULL));
    cJSON *e = empty ? cJSON_Parse(empty) : NULL;
    CHECK(e && cJSON_GetArraySize(cJSON_GetObjectItem(cJSON_GetObjectItem(e, "layer"), "links")) == 0,
          "an empty layer is JSON too");
    cJSON_Delete(e);
    free(empty);
}

/* --- the rpl overlay from packets ---------------------------------------- */

static char *last_panel;
static void cap_publish(sim_runtime_t *sim, const char *id, const char *json) {
    (void)sim;
    if (strcmp(id, "rpl-test")) return;
    free(last_panel);
    last_panel = strdup(json);
}
static int two(sim_runtime_t *sim) { (void)sim; return 2; }
static int id_of(sim_runtime_t *sim, int node) { (void)sim; return node + 1; }

static int unhex(const char *h, uint8_t *out) {
    int n = 0;
    for (; h[0] && h[1]; h += 2) { unsigned v; sscanf(h, "%2x", &v); out[n++] = (uint8_t)v; }
    return n;
}

static void tx(rpl_overlay_t *o, int slot, int64_t t, const char *hex) {
    uint8_t b[128];
    int n = unhex(hex, b);
    sim_observer_event_t ev = { .kind = SIM_OBS_PACKET_FRAME, .time_ns = t, .mote_index = slot };
    ev.u.frame.is_tx = true;
    ev.u.frame.bytes = b;
    ev.u.frame.bytes_len = n;
    rpl_overlay_on_event(o, NULL, &ev);
}

static void test_rpl_packets(void) {
    /* Captured from configs/test-rpl-overlay-chain-sky.yaml: the root's DIO
     * (rank 128, MinHopRankIncrease 128), and node 2's DAO to node 1 behind
     * a hop-by-hop RPL option.  Node 2's DIO is the root's with its own
     * source address and rank 256. */
    const char *dio1 = "41d8c6cdabffff01010100017412007a3b3a1a9b0116fe00f0008008f00000fd000000000000000212740100010101040e00080c00040000800001001e003c081e4040ffffffffffffffff00000000fd0000000000000000000000000000005351";
    const char *dio2 = "41d8c6cdabffff02020200027412007a3b3a1a9b0116fe00f0010008f00000fd000000000000000212740100010101040e00080c00040000800001001e003c081e4040ffffffffffffffff00000000fd0000000000000000000000000000005351";
    const char *dao2 = "61dc82cdab010101000174120002020200027412007ef700e03a066304000001499b0284fc008000f105120080fd00000000000000021274020002020206140000001efd00000000000000021274010001010150c9";
    rpl_overlay_t *o = rpl_overlay_create((rpl_overlay_host_t){
        .panel_id = "rpl-test", .publish = cap_publish, .node_count = two, .node_id = id_of });
    tx(o, 0, 100000000LL, dio1);
    tx(o, 1, 200000000LL, dio2);
    tx(o, 1, 300000000LL, dao2);
    rpl_overlay_refresh(o, NULL, 400000000LL);
    cJSON *j = last_panel ? cJSON_Parse(last_panel) : NULL;
    CHECK(j, "the overlay published a panel: %s", last_panel ? last_panel : "(none)");
    if (j) {
        cJSON *nodes = cJSON_GetObjectItem(cJSON_GetObjectItem(j, "layer"), "nodes");
        cJSON *links = cJSON_GetObjectItem(cJSON_GetObjectItem(j, "layer"), "links");
        cJSON *n1 = cJSON_GetObjectItem(nodes, "1"), *n2 = cJSON_GetObjectItem(nodes, "2");
        CHECK(n1 && !strcmp(cJSON_GetObjectItem(n1, "glyph")->valuestring, "square") &&
              !strcmp(cJSON_GetObjectItem(n1, "label")->valuestring, "rank 128*"),
              "node 1 is the root, from its DIO");
        CHECK(n2 && !strcmp(cJSON_GetObjectItem(n2, "glyph")->valuestring, "circle") &&
              !strcmp(cJSON_GetObjectItem(n2, "label")->valuestring, "rank 256*"), "node 2 at rank 256");
        CHECK(n1 && n2 && !strcmp(cJSON_GetObjectItem(n1, "fill")->valuestring,
                                  cJSON_GetObjectItem(n2, "fill")->valuestring), "one DODAG, one colour");
        cJSON *l0 = cJSON_GetArrayItem(links, 0);
        CHECK(cJSON_GetArraySize(links) == 1 && cJSON_GetObjectItem(l0, "from")->valueint == 2 &&
              cJSON_GetObjectItem(l0, "to")->valueint == 1 &&
              !strcmp(cJSON_GetObjectItem(l0, "style")->valuestring, "parent"),
              "node 2's DAO went to node 1: its preferred parent");
        cJSON_Delete(j);
    }
    /* Refreshes are throttled to every 500 ms of sim time. */
    free(last_panel); last_panel = NULL;
    tx(o, 0, 600000000LL, dio1);
    CHECK(!last_panel, "no refresh 200 ms after the last one");
    tx(o, 0, 900000000LL, dio1);
    CHECK(last_panel, "a refresh 500 ms after it");
    /* A node rebooted on its own (SIM_OBS_MOTE_REMOVED, then _ADDED):
     * what its packets said is forgotten, the other node's kept. */
    {
        sim_observer_event_t gone = { .kind = SIM_OBS_MOTE_REMOVED, .time_ns = 950000000LL, .mote_index = 1 };
        rpl_overlay_on_event(o, NULL, &gone);
        free(last_panel); last_panel = NULL;
        rpl_overlay_refresh(o, NULL, 960000000LL);
        CHECK(last_panel && strstr(last_panel, "rank 128*") && !strstr(last_panel, "rank 256") &&
              strstr(last_panel, "no RPL seen yet: node 2"),
              "a rebooted node's packets are forgotten: %s", last_panel ? last_panel : "(none)");
        tx(o, 1, 970000000LL, dio2);
        tx(o, 1, 980000000LL, dao2);
        rpl_overlay_refresh(o, NULL, 990000000LL);
    }
    /* A restart (the service's on_restart) replaces the old DODAG with an
     * empty layer at once, before any event of the new run. */
    free(last_panel); last_panel = NULL;
    rpl_overlay_restart(o, NULL);
    CHECK(last_panel && strstr(last_panel, "\"nodes\":{}") && strstr(last_panel, "\"links\":[]") &&
          strstr(last_panel, "restarted"), "a restart publishes an empty layer at once: %s",
          last_panel ? last_panel : "(none)");
    /* The new run's first event draws the nodes, with no 500 ms wait, and
     * the old run's packets are forgotten. */
    free(last_panel); last_panel = NULL;
    sim_observer_event_t boot = { .kind = SIM_OBS_MOTE_LOG_LINE, .time_ns = 1000, .mote_index = 0 };
    rpl_overlay_on_event(o, NULL, &boot);
    CHECK(last_panel && strstr(last_panel, "\"nodes\":{}") && strstr(last_panel, "no RPL seen yet: node 1, 2"),
          "after a restart nothing is known: %s", last_panel ? last_panel : "(none)");
    /* A restart early in a run (time never ran back by 500 ms) is a restart too. */
    tx(o, 0, 100000000LL, dio1);
    rpl_overlay_refresh(o, NULL, 200000000LL);
    CHECK(last_panel && strstr(last_panel, "rank 128*"), "the new run's DIO is drawn");
    rpl_overlay_restart(o, NULL);
    free(last_panel); last_panel = NULL;
    boot.time_ns = 150000000LL;
    rpl_overlay_on_event(o, NULL, &boot);
    CHECK(last_panel && strstr(last_panel, "\"nodes\":{}"),
          "a restart 200 ms in forgets the run too: %s", last_panel ? last_panel : "(none)");
    /* A No-Path DAO (path lifetime 0) goes to the parent being left: it is
     * not taken as the parent.  dao2 with its Transit Information's
     * lifetime (0x1e) zeroed, sent to node 3's address. */
    rpl_overlay_restart(o, NULL);
    tx(o, 0, 100000000LL, dio1);
    tx(o, 1, 200000000LL, dio2);
    tx(o, 1, 300000000LL, dao2);
    char nopath[512];
    snprintf(nopath, sizeof nopath, "%s", dao2);
    char *lt = strstr(nopath, "0614000000""1e");
    CHECK(lt != NULL, "the DAO's Transit Information option");
    if (lt) { lt[10] = '0'; lt[11] = '0'; }
    char *dst = strstr(nopath, "0101010001741200");   /* the link destination, node 1 */
    if (dst) memcpy(dst, "0303030003741200", 16);
    tx(o, 1, 400000000LL, nopath);
    free(last_panel); last_panel = NULL;
    rpl_overlay_refresh(o, NULL, 450000000LL);
    j = last_panel ? cJSON_Parse(last_panel) : NULL;
    if (j) {
        cJSON *links = cJSON_GetObjectItem(cJSON_GetObjectItem(j, "layer"), "links");
        cJSON *l0 = cJSON_GetArrayItem(links, 0);
        CHECK(cJSON_GetArraySize(links) == 1 && cJSON_GetObjectItem(l0, "to")->valueint == 1,
              "a No-Path DAO does not move the parent: %s", last_panel);
        cJSON_Delete(j);
    } else {
        CHECK(0, "the overlay published a panel");
    }
    free(last_panel); last_panel = NULL;
    rpl_overlay_free(o);
}

/* --- the rpl overlay from memory, its parent from a DAO ------------------ */

/* Two nodes' memory, as a host's fw reads see it: curr_instance (the Sky
 * layout) and linkaddr_node_addr, but no neighbour table, so a parent
 * pointer cannot be looked up. */
static uint8_t fake_mem[2][512];
enum { FAKE_CI = 0, FAKE_LL = 300, FAKE_TAB = 320, FAKE_KEYS = 340, FAKE_NBRS = 500 };
/* Symbol sizes the layout checks compare (0 = the image has no such symbol). */
static uint32_t fake_ll_size = 8, fake_keys_size, fake_nbrs_size;
static bool fake_resolve(sim_runtime_t *sim, int node, const char *sym, sim_fwvar_t *out) {
    (void)sim; (void)node;
    memset(out, 0, sizeof(*out));
    if (!strcmp(sym, "curr_instance")) *out = (sim_fwvar_t){ FAKE_CI, 254, 105, true };
    else if (!strcmp(sym, "linkaddr_node_addr")) *out = (sim_fwvar_t){ FAKE_LL, fake_ll_size, 105, true };
    else if (!strcmp(sym, "neighbor_addr_mem_memb_mem") && fake_keys_size)
        *out = (sim_fwvar_t){ FAKE_KEYS, fake_keys_size, 105, true };
    else if (!strcmp(sym, "_rpl_neighbors_mem") && fake_nbrs_size)
        *out = (sim_fwvar_t){ FAKE_NBRS, fake_nbrs_size, 105, true };
    else if (!strcmp(sym, "rpl_neighbors_struct") && fake_nbrs_size)
        *out = (sim_fwvar_t){ FAKE_TAB, 8, 105, true };
    return out->valid;
}
static bool fake_read(sim_runtime_t *sim, int node, const sim_fwvar_t *v, uint32_t off, void *dst, size_t n) {
    (void)sim;
    if (node < 0 || node > 1 || v->addr + off + n > sizeof fake_mem[0]) return false;
    memcpy(dst, fake_mem[node] + v->addr + off, n);
    return true;
}
static void fake_put(int node, const char *field, uint32_t val, const uint8_t *bytes) {
    uint32_t off = 0, sz = 0;
    if (!struct_layout_field(rpl_layout_rows, sizeof(rpl_layout_rows) / sizeof(rpl_layout_rows[0]),
                             "contiki-ng-msp430", "rpl_instance_t", field, &off, &sz)) {
        CHECK(0, "no field %s", field);
        return;
    }
    if (bytes) memcpy(fake_mem[node] + FAKE_CI + off, bytes, sz);
    else for (uint32_t i = 0; i < sz; i++) fake_mem[node][FAKE_CI + off + i] = (uint8_t)(val >> (8 * i));
}

static void test_rpl_fw_parent_fallback(void) {
    /* Node 1's link address is the DAO's destination (00:12:74:01:00:01:01:01). */
    static const uint8_t ll1[8] = { 0x00, 0x12, 0x74, 0x01, 0x00, 0x01, 0x01, 0x01 };
    static const uint8_t ll2[8] = { 0x00, 0x12, 0x74, 0x02, 0x00, 0x02, 0x02, 0x02 };
    static const uint8_t dodag[16] = { 0xfd, 0, 0, 0, 0, 0, 0, 0, 0x02, 0x12, 0x74, 0x01, 0, 0x01, 0x01, 0x01 };
    memset(fake_mem, 0, sizeof fake_mem);
    memcpy(fake_mem[0] + FAKE_LL, ll1, 8);
    memcpy(fake_mem[1] + FAKE_LL, ll2, 8);
    for (int nd = 0; nd < 2; nd++) {
        fake_put(nd, "used", 1, NULL);
        fake_put(nd, "min_hoprankinc", 128, NULL);
        fake_put(nd, "dag.state", 1, NULL);              /* DAG_JOINED */
        fake_put(nd, "dag.dag_id", 0, dodag);
        fake_put(nd, "dag.rank", nd ? 256 : 128, NULL);
    }
    fake_put(1, "dag.preferred_parent", 0x1234, NULL);    /* not in any table */
    const char *dao2 = "61dc82cdab010101000174120002020200027412007ef700e03a066304000001499b0284fc008000f105120080fd00000000000000021274020002020206140000001efd00000000000000021274010001010150c9";
    rpl_overlay_t *o = rpl_overlay_create((rpl_overlay_host_t){
        .panel_id = "rpl-test", .publish = cap_publish, .resolve = fake_resolve, .read = fake_read,
        .node_count = two, .node_id = id_of });
    rpl_overlay_refresh(o, NULL, 100000000LL);
    CHECK(last_panel && strstr(last_panel, "\"label\":\"rank 256\"") && strstr(last_panel, "\"links\":[]"),
          "from memory, no parent while the pointer cannot be looked up: %s", last_panel ? last_panel : "(none)");
    tx(o, 1, 200000000LL, dao2);
    rpl_overlay_refresh(o, NULL, 300000000LL);
    cJSON *j = last_panel ? cJSON_Parse(last_panel) : NULL;
    if (j) {
        cJSON *L = cJSON_GetObjectItem(j, "layer");
        cJSON *links = cJSON_GetObjectItem(L, "links"), *l0 = cJSON_GetArrayItem(links, 0);
        cJSON *n2 = cJSON_GetObjectItem(cJSON_GetObjectItem(L, "nodes"), "2");
        CHECK(cJSON_GetArraySize(links) == 1 && cJSON_GetObjectItem(l0, "from")->valueint == 2 &&
              cJSON_GetObjectItem(l0, "to")->valueint == 1,
              "the DAO-learned parent stands in for the one memory could not name: %s", last_panel);
        CHECK(n2 && !strcmp(cJSON_GetObjectItem(n2, "label")->valuestring, "rank 256*"),
              "and the node is marked as seen in packets");
        cJSON_Delete(j);
    } else {
        CHECK(0, "the overlay published a panel");
    }
    free(last_panel); last_panel = NULL;
    rpl_overlay_free(o);
}

/* --- the layout checked against the image's symbol sizes ----------------- */

static void test_rpl_layout_checks(void) {
    /* The memory of test_rpl_fw_parent_fallback (both nodes joined, read
     * with the Sky layout: lladdr 8 bytes, a 10-byte key) and the tables'
     * item_size (8) in memory. */
    fake_mem[0][FAKE_TAB + 2] = fake_mem[1][FAKE_TAB + 2] = 8;
    rpl_overlay_t *o = rpl_overlay_create((rpl_overlay_host_t){
        .panel_id = "rpl-test", .publish = cap_publish, .resolve = fake_resolve, .read = fake_read,
        .node_count = two, .node_id = id_of });
    fake_keys_size = 160; fake_nbrs_size = 128;   /* 16 keys, 16 items: as on a Sky image */
    rpl_overlay_refresh(o, NULL, 100000000LL);
    CHECK(last_panel && strstr(last_panel, "\"label\":\"rank 256\"") && !strstr(last_panel, "layout unknown"),
          "sizes that fit the layout: read from memory: %s", last_panel ? last_panel : "(none)");
    struct { uint32_t ll, keys, nbrs; const char *what; } bad[] = {
        { 2, 160, 128, "linkaddr_node_addr of 2 bytes (LINKADDR_SIZE 2), the layout's lladdr 8" },
        { 8, 165, 128, "neighbor_addr_mem_memb_mem not a whole number of 10-byte keys" },
        { 8, 160, 132, "_rpl_neighbors_mem not a whole number of 8-byte items" },
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        fake_ll_size = bad[i].ll; fake_keys_size = bad[i].keys; fake_nbrs_size = bad[i].nbrs;
        free(last_panel); last_panel = NULL;
        rpl_overlay_refresh(o, NULL, 200000000LL + (int64_t)i);
        CHECK(last_panel && strstr(last_panel, "layout unknown (packets used): node 1, 2") &&
              strstr(last_panel, "no RPL seen yet: node 1, 2"),
              "%s: layout unknown, the packets used: %s", bad[i].what, last_panel ? last_panel : "(none)");
    }
    fake_ll_size = 8; fake_keys_size = fake_nbrs_size = 0;
    free(last_panel); last_panel = NULL;
    rpl_overlay_free(o);
}

/* A cut node list ends in "..." (the note's buffer holds about 30 ids). */
static int many(sim_runtime_t *sim) { (void)sim; return 100; }
static void test_rpl_long_note(void) {
    rpl_overlay_t *o = rpl_overlay_create((rpl_overlay_host_t){
        .panel_id = "rpl-test", .publish = cap_publish, .node_count = many, .node_id = id_of });
    rpl_overlay_refresh(o, NULL, 100000000LL);
    const char *t = last_panel ? strstr(last_panel, "no RPL seen yet: node 1, 2, 3") : NULL;
    const char *q = t ? strchr(t, '"') : NULL;
    CHECK(t && q && q - t > 4 && !strncmp(q - 4, " ...", 4) && !strstr(t, "100"),
          "a cut list of 100 silent nodes ends in \" ...\": %s", last_panel ? last_panel : "(none)");
    free(last_panel); last_panel = NULL;
    rpl_overlay_free(o);
}

int run_fwvar_tests(int verbose) {
    (void)verbose;
    printf("=== Firmware variables, canvas layers, rpl overlay ===\n");
    checks = failures = 0;
    test_fwvar_msp430();
    test_arm_memory_only();
    test_layer_json();
    test_rpl_packets();
    test_rpl_fw_parent_fallback();
    test_rpl_layout_checks();
    test_rpl_long_note();
    printf("  %d checks passed, %d failed\n", checks - failures, failures);
    return failures;
}
