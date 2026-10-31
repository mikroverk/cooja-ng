/*
 * sim_fwvar — read named firmware variables of emulated nodes.
 *
 * A service (an overlay, the shell) names a symbol of a node's firmware —
 * `curr_instance`, `linkaddr_node_addr` — and reads its bytes out of the
 * emulated memory, without knowing which CPU the node has.  The node's CPU
 * is reached through sim_mote_ops_t::get_interface (ARM or MSP430), the
 * image through the runner's describe primitive (sim_control), so this
 * header names no chip type.
 *
 * Reads only, and memory only: ROM, flash and SRAM on ARM, the memory
 * array on MSP430.  A peripheral register is never touched — reading one
 * can clear it or pop a FIFO, which would change the run.  Native, JS,
 * external and Renode nodes have no emulated memory here: resolve and
 * read return false for them.
 *
 * `node` is the slot index — what observer events carry as mote_index —
 * not the Cooja node id; sim_fwvar_node_id() maps one to the other.
 *
 * Symbols are cached per image (path + mtime/size/inode, so a rebuild at
 * the same path is re-read): every node booted from one image shares its
 * entries, and a symbol is looked up in the file once, found or not.
 */
#ifndef SIM_FWVAR_H
#define SIM_FWVAR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sim_runtime sim_runtime_t;
struct arm_cpu;
struct msp430_cpu;

typedef struct sim_fwvar {
    uint32_t addr;
    uint32_t size;      /* the symbol's st_size (an object's sizeof), 0 if unrecorded */
    uint16_t machine;   /* e_machine of the image it was found in: 105 MSP430, 40 ARM */
    bool     valid;
} sim_fwvar_t;

/* Resolve `symbol` in node `node`'s firmware, then its Secure-world image
 * (TrustZone-M), if any.  False — and out->valid false — if neither has
 * it, or the node has no image. */
bool sim_fwvar_resolve(sim_runtime_t *sim, int node, const char *symbol,
                       sim_fwvar_t *out);

/* Read n bytes at var->addr + offset from node `node`'s memory, as stored
 * (little-endian on every supported CPU).  False if any byte is outside
 * memory, the node has no emulated CPU, or the slot is removed. */
bool sim_fwvar_read(sim_runtime_t *sim, int node, const sim_fwvar_t *var,
                    uint32_t offset, void *dst, size_t n);

/* Little-endian u8/u16/u32 at var + offset. */
bool sim_fwvar_read_u8 (sim_runtime_t *sim, int node, const sim_fwvar_t *var,
                        uint32_t offset, uint8_t *v);
bool sim_fwvar_read_u16(sim_runtime_t *sim, int node, const sim_fwvar_t *var,
                        uint32_t offset, uint16_t *v);
bool sim_fwvar_read_u32(sim_runtime_t *sim, int node, const sim_fwvar_t *var,
                        uint32_t offset, uint32_t *v);

/* Node slots in use (removed ones included), and a slot's Cooja node id
 * (-1 for an empty or removed slot). */
int  sim_fwvar_node_count(sim_runtime_t *sim);
int  sim_fwvar_node_id(sim_runtime_t *sim, int node);

/* --- the layer underneath, shared with the shell ---------------------- */

/* Look `name` up in `firmware`, then `secure_firmware` (either may be
 * NULL or ""), through the per-image cache.  Any of addr/size/machine may
 * be NULL.  A symbol at address 0 is found. */
bool sim_fwvar_symbol(const char *firmware, const char *secure_firmware,
                      const char *name, uint32_t *addr, uint32_t *size,
                      uint16_t *machine);

/* Entries cached for `path` (found and not found), or -1 if the image is
 * not in the cache.  For the unit tests. */
int  sim_fwvar_cache_count(const char *path);

/* True if `addr` is memory (not a peripheral) on this CPU; give exactly
 * one of arm/msp. */
bool sim_fwvar_is_memory(const struct arm_cpu *arm, const struct msp430_cpu *msp,
                         uint32_t addr);

/* Read n bytes of memory from a CPU; false if any byte is not memory. */
bool sim_fwvar_mem_read(struct arm_cpu *arm, const struct msp430_cpu *msp,
                        uint32_t addr, void *dst, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* SIM_FWVAR_H */
