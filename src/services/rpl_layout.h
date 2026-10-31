/*
 * rpl_layout — Contiki-NG RPL Lite struct layouts for the rpl overlay.
 *
 * GENERATED rows: do not edit them by hand.  An image that carries DWARF
 * (the bundled Sky and nRF52840 RPL-UDP images do: build-device-firmware.sh
 * --debug) is added straight from itself:
 *
 *   tools/gen-struct-layout.py --update src/services/rpl_layout.h firmware/sky/udp-client.sky
 *
 * one without, through a DWARF build of its source (the image itself is not
 * rewritten): its own build command plus --layout src/services/rpl_layout.h.
 *
 * The overlay picks a layout by the image's machine and the size of its
 * curr_instance symbol (sizeof rpl_instance_t); see struct_layout.h.
 *
 * layout-spec: rpl_instance_t:used,instance_id,mop,min_hoprankinc,dag.dag_id,dag.preferred_parent,dag.rank,dag.version,dag.state nbr_table_t:item_size nbr_table_key_t:lladdr */
#ifndef RPL_LAYOUT_H
#define RPL_LAYOUT_H

#include "struct_layout.h"

static const struct_layout_row_t rpl_layout_rows[] = {
    /* rows:begin (tools/gen-struct-layout.py --update) */
    /* layout contiki-ng-msp430: udp-client.sky, udp-server.sky */
    { "contiki-ng-msp430", 105, "rpl_instance_t", "", 0, 254 },
    { "contiki-ng-msp430", 105, "rpl_instance_t", "used", 10, 1 },
    { "contiki-ng-msp430", 105, "rpl_instance_t", "instance_id", 11, 1 },
    { "contiki-ng-msp430", 105, "rpl_instance_t", "mop", 12, 1 },
    { "contiki-ng-msp430", 105, "rpl_instance_t", "min_hoprankinc", 20, 2 },
    { "contiki-ng-msp430", 105, "rpl_instance_t", "dag.dag_id", 26, 16 },
    { "contiki-ng-msp430", 105, "rpl_instance_t", "dag.preferred_parent", 64, 2 },
    { "contiki-ng-msp430", 105, "rpl_instance_t", "dag.rank", 72, 2 },
    { "contiki-ng-msp430", 105, "rpl_instance_t", "dag.version", 76, 1 },
    { "contiki-ng-msp430", 105, "rpl_instance_t", "dag.state", 86, 2 },
    { "contiki-ng-msp430", 105, "nbr_table_t", "", 0, 8 },
    { "contiki-ng-msp430", 105, "nbr_table_t", "item_size", 2, 2 },
    { "contiki-ng-msp430", 105, "nbr_table_key_t", "", 0, 10 },
    { "contiki-ng-msp430", 105, "nbr_table_key_t", "lladdr", 2, 8 },
    /* layout contiki-ng-cortex-m: udp-client.cc2538dk, udp-client.nrf52840-dk, udp-server.cc2538dk, udp-server.nrf52840-dk */
    { "contiki-ng-cortex-m", 40, "rpl_instance_t", "", 0, 352 },
    { "contiki-ng-cortex-m", 40, "rpl_instance_t", "used", 12, 1 },
    { "contiki-ng-cortex-m", 40, "rpl_instance_t", "instance_id", 13, 1 },
    { "contiki-ng-cortex-m", 40, "rpl_instance_t", "mop", 14, 1 },
    { "contiki-ng-cortex-m", 40, "rpl_instance_t", "min_hoprankinc", 22, 2 },
    { "contiki-ng-cortex-m", 40, "rpl_instance_t", "dag.dag_id", 28, 16 },
    { "contiki-ng-cortex-m", 40, "rpl_instance_t", "dag.preferred_parent", 68, 4 },
    { "contiki-ng-cortex-m", 40, "rpl_instance_t", "dag.rank", 78, 2 },
    { "contiki-ng-cortex-m", 40, "rpl_instance_t", "dag.version", 82, 1 },
    { "contiki-ng-cortex-m", 40, "rpl_instance_t", "dag.state", 92, 1 },
    { "contiki-ng-cortex-m", 40, "nbr_table_t", "", 0, 16 },
    { "contiki-ng-cortex-m", 40, "nbr_table_t", "item_size", 4, 4 },
    { "contiki-ng-cortex-m", 40, "nbr_table_key_t", "", 0, 12 },
    { "contiki-ng-cortex-m", 40, "nbr_table_key_t", "lladdr", 4, 8 },
    /* rows:end */
};

#endif /* RPL_LAYOUT_H */
