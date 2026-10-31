/*
 * sim_fwvar — see include/sim/sim_fwvar.h.
 */
#include "sim_fwvar.h"
#include "sim_control.h"
#include "sim_mote.h"
#include "arm_cpu.h"
#include "msp430_cpu.h"
#include "elf_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* --- the per-image symbol cache ------------------------------------------ */

#define FWVAR_IMAGES   16          /* images cached, replaced round-robin   */
#define FWVAR_SYMS     64          /* entries per image, replaced round-robin */
#define FWVAR_PATH_MAX 512

/* By-node lookups re-check the file at most this often in sim time: an
 * overlay resolving a handful of symbols for every node, every refresh,
 * would otherwise stat the image thousands of times a wall second. */
#define FWVAR_RESTAT_NS 1000000000LL

typedef struct {
    char     path[FWVAR_PATH_MAX];
    int64_t  stamp[3];             /* mtime (ns), size, inode                */
    int64_t  checked_ns;           /* sim time of the last stat (by node)    */
    uint16_t machine;
    int      count, next;
    struct { char name[64]; uint32_t addr, size; bool found; } e[FWVAR_SYMS];
} fwvar_image_t;

static fwvar_image_t *g_images[FWVAR_IMAGES];
static int g_image_next;

/* The file behind an image path: mtime (ns, so a rebuild within the same
 * second is seen), size and inode, or zeros. */
static void image_stamp(const char *path, int64_t stamp[3]) {
    struct stat st;
    if (path[0] && stat(path, &st) == 0) {
#ifdef __APPLE__
        stamp[0] = (int64_t)st.st_mtimespec.tv_sec * 1000000000LL + st.st_mtimespec.tv_nsec;
#else
        stamp[0] = (int64_t)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
#endif
        stamp[1] = (int64_t)st.st_size; stamp[2] = (int64_t)st.st_ino;
    } else {
        stamp[0] = stamp[1] = stamp[2] = 0;
    }
}

static fwvar_image_t *find_image(const char *path) {
    for (int i = 0; i < FWVAR_IMAGES; i++)
        if (g_images[i] && strcmp(g_images[i]->path, path) == 0) return g_images[i];
    return NULL;
}

/* The cache entry for `path`, emptied if the file changed; NULL for a path
 * too long to key on (the caller then reads the file uncached).  `now_ns`
 * < 0 checks the file every time (the shell); otherwise at most once per
 * FWVAR_RESTAT_NS of sim time. */
static fwvar_image_t *image_entry(const char *path, int64_t now_ns) {
    if (strlen(path) >= FWVAR_PATH_MAX) return NULL;
    fwvar_image_t *im = find_image(path);
    if (im && now_ns >= 0 && now_ns >= im->checked_ns &&
        now_ns - im->checked_ns < FWVAR_RESTAT_NS)
        return im;
    int64_t st[3];
    image_stamp(path, st);
    if (!im) {                     /* take the next slot, round-robin */
        int k = g_image_next;
        g_image_next = (k + 1) % FWVAR_IMAGES;
        if (!g_images[k]) g_images[k] = calloc(1, sizeof(fwvar_image_t));
        im = g_images[k];
        if (!im) return NULL;
        im->path[0] = '\0';        /* not this image: emptied below */
    }
    if (strcmp(im->path, path) != 0 || memcmp(im->stamp, st, sizeof(st)) != 0) {
        memset(im, 0, sizeof(*im));
        snprintf(im->path, sizeof(im->path), "%s", path);
        memcpy(im->stamp, st, sizeof(st));
        im->machine = elf_machine(path);
    }
    im->checked_ns = now_ns >= 0 ? now_ns : 0;
    return im;
}

static bool lookup_one(const char *path, const char *name, int64_t now_ns,
                       uint32_t *addr, uint32_t *size, uint16_t *machine) {
    fwvar_image_t *im = image_entry(path, now_ns);
    if (im)
        for (int i = 0; i < im->count; i++)
            if (strcmp(im->e[i].name, name) == 0) {
                if (!im->e[i].found) return false;
                *addr = im->e[i].addr; *size = im->e[i].size; *machine = im->machine;
                return true;
            }
    uint32_t a = 0, sz = 0;
    bool found = elf_lookup_symbol_sized(path, name, &a, &sz);
    if (im && strlen(name) < sizeof(im->e[0].name)) {
        int k = im->next;
        snprintf(im->e[k].name, sizeof(im->e[k].name), "%s", name);
        im->e[k].addr = a; im->e[k].size = sz; im->e[k].found = found;
        im->next = (k + 1) % FWVAR_SYMS;
        if (im->count < FWVAR_SYMS) im->count++;
    }
    if (!found) return false;
    *addr = a; *size = sz;
    *machine = im ? im->machine : elf_machine(path);
    return true;
}

static bool symbol_at(const char *firmware, const char *secure_firmware,
                      const char *name, int64_t now_ns, uint32_t *addr,
                      uint32_t *size, uint16_t *machine) {
    uint32_t a = 0, sz = 0;
    uint16_t m = 0;
    bool found = (firmware && firmware[0] && lookup_one(firmware, name, now_ns, &a, &sz, &m)) ||
                 (secure_firmware && secure_firmware[0] &&
                  lookup_one(secure_firmware, name, now_ns, &a, &sz, &m));
    if (!found) return false;
    if (addr) *addr = a;
    if (size) *size = sz;
    if (machine) *machine = m;
    return true;
}

bool sim_fwvar_symbol(const char *firmware, const char *secure_firmware,
                      const char *name, uint32_t *addr, uint32_t *size,
                      uint16_t *machine) {
    return symbol_at(firmware, secure_firmware, name, -1, addr, size, machine);
}

int sim_fwvar_cache_count(const char *path) {
    const fwvar_image_t *im = find_image(path);
    return im ? im->count : -1;
}

/* --- memory --------------------------------------------------------------- */

bool sim_fwvar_is_memory(const struct arm_cpu *arm, const struct msp430_cpu *msp,
                         uint32_t a) {
    if (arm)
        return (arm->rom && a < arm->rom_size) ||
               (a >= arm->flash_base && a < arm->flash_end) ||
               (a >= arm->sram_base && a < arm->sram_end);
    return msp && a < msp->max_mem;
}

bool sim_fwvar_mem_read(struct arm_cpu *arm, const struct msp430_cpu *msp,
                        uint32_t addr, void *dst, size_t n) {
    uint8_t *out = dst;
    for (size_t i = 0; i < n; i++) {
        uint32_t a = addr + (uint32_t)i;
        if (a < addr || !sim_fwvar_is_memory(arm, msp, a)) return false;
        out[i] = arm ? arm_read8(arm, a) : msp->memory[a];
    }
    return true;
}

/* --- by node -------------------------------------------------------------- */

static bool node_info(sim_runtime_t *sim, int node, sim_control_node_info_t *info) {
    return sim && sim->control && node >= 0 &&
           sim_control_describe(sim->control, node, info) && !info->removed;
}

bool sim_fwvar_resolve(sim_runtime_t *sim, int node, const char *symbol,
                       sim_fwvar_t *out) {
    memset(out, 0, sizeof(*out));
    sim_control_node_info_t info;
    if (!symbol || !node_info(sim, node, &info)) return false;
    out->valid = symbol_at(info.firmware, info.secure_firmware, symbol,
                           sim_runtime_now_ns(sim), &out->addr, &out->size,
                           &out->machine);
    return out->valid;
}

bool sim_fwvar_read(sim_runtime_t *sim, int node, const sim_fwvar_t *var,
                    uint32_t offset, void *dst, size_t n) {
    sim_control_node_info_t info;
    if (!var || !var->valid || !node_info(sim, node, &info)) return false;
    const sim_control_ops_t *ops = &sim->control->ops;
    if (!ops->get_interface) return false;
    arm_cpu_t *arm = ops->get_interface(ops->user, node, SIM_MOTE_IFACE_ARM_CPU);
    msp430_cpu_t *msp = arm ? NULL : ops->get_interface(ops->user, node, SIM_MOTE_IFACE_MSP430_CPU);
    if (!arm && !msp) return false;
    return sim_fwvar_mem_read(arm, msp, var->addr + offset, dst, n);
}

bool sim_fwvar_read_u8(sim_runtime_t *sim, int node, const sim_fwvar_t *var,
                       uint32_t offset, uint8_t *v) {
    return sim_fwvar_read(sim, node, var, offset, v, 1);
}

bool sim_fwvar_read_u16(sim_runtime_t *sim, int node, const sim_fwvar_t *var,
                        uint32_t offset, uint16_t *v) {
    uint8_t b[2];
    if (!sim_fwvar_read(sim, node, var, offset, b, 2)) return false;
    *v = (uint16_t)(b[0] | (b[1] << 8));
    return true;
}

bool sim_fwvar_read_u32(sim_runtime_t *sim, int node, const sim_fwvar_t *var,
                        uint32_t offset, uint32_t *v) {
    uint8_t b[4];
    if (!sim_fwvar_read(sim, node, var, offset, b, 4)) return false;
    *v = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return true;
}

int sim_fwvar_node_count(sim_runtime_t *sim) {
    return sim && sim->control ? sim_control_node_count(sim->control) : 0;
}

int sim_fwvar_node_id(sim_runtime_t *sim, int node) {
    sim_control_node_info_t info;
    return node_info(sim, node, &info) ? info.id : -1;
}
