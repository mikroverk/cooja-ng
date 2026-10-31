/*
 * struct_layout — pick a firmware struct's field offsets at run time.
 *
 * tools/gen-struct-layout.py reads struct layouts from the DWARF of a
 * debug build and writes them as rows into an overlay's layout header
 * (src/services/rpl_layout.h).  Rows come in groups ("layouts"), one per
 * distinct layout; images with the same layout share one.  An overlay picks
 * the group for a node by the image's machine and the size its symbol
 * table records for a variable of the first struct (curr_instance is a
 * rpl_instance_t): another configuration, another size, no group — and the
 * overlay says "layout unknown" instead of reading the wrong bytes.
 *
 * Header-only and libc-free, so an out-of-tree plugin can use it too.
 */
#ifndef STRUCT_LAYOUT_H
#define STRUCT_LAYOUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct struct_layout_row {
    const char *layout;     /* the group                                    */
    uint16_t    machine;    /* ELF e_machine: 105 MSP430, 40 ARM            */
    const char *type;       /* the struct, as the spec named it             */
    const char *field;      /* member path; "" is the struct's sizeof       */
    uint32_t    offset, size;
    uint8_t     bit, bits;  /* a bitfield: `bits` wide, `bit` up from the LSB of
                             * the byte at offset (size bytes hold it); 0 bits
                             * = an ordinary member                          */
} struct_layout_row_t;

/* The layout for `machine` whose sizeof(`type`) is `size`, or NULL. */
static inline const char *struct_layout_pick(const struct_layout_row_t *rows, size_t n,
                                             uint16_t machine, const char *type,
                                             uint32_t size) {
    for (size_t i = 0; i < n; i++)
        if (rows[i].machine == machine && rows[i].field[0] == '\0' &&
            rows[i].size == size && strcmp(rows[i].type, type) == 0)
            return rows[i].layout;
    return NULL;
}

/* A field's row of `type` in `layout`, or NULL. */
static inline const struct_layout_row_t *struct_layout_row(const struct_layout_row_t *rows,
                                                           size_t n, const char *layout,
                                                           const char *type, const char *field) {
    for (size_t i = 0; i < n; i++)
        if (strcmp(rows[i].layout, layout) == 0 && strcmp(rows[i].type, type) == 0 &&
            strcmp(rows[i].field, field) == 0)
            return &rows[i];
    return NULL;
}

/* A field of `type` in `layout`: true with its offset and size, or false. */
static inline bool struct_layout_field(const struct_layout_row_t *rows, size_t n,
                                       const char *layout, const char *type,
                                       const char *field, uint32_t *offset,
                                       uint32_t *size) {
    const struct_layout_row_t *r = struct_layout_row(rows, n, layout, type, field);
    if (!r) return false;
    *offset = r->offset;
    *size = r->size;
    return true;
}

/* A field's value from the bytes read at its offset (little-endian, up to
 * 4 bytes), its bitfield bits picked out. */
static inline uint32_t struct_layout_value(const struct_layout_row_t *r, const uint8_t *b) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < r->size && i < 4; i++) v |= (uint32_t)b[i] << (8 * i);
    if (r->bits) v = (v >> r->bit) & ((r->bits >= 32) ? 0xFFFFFFFFu : ((1u << r->bits) - 1));
    return v;
}

#endif /* STRUCT_LAYOUT_H */
