/*
 * builtin_services — csim_register_builtin_services (include/sim/sim_registry.h).
 *
 * The services register themselves, the way boards do in sim_board.c and
 * mote kinds in mote_kinds.c: the kernel's registry owns the catalog but
 * names no service, so src/sim does not depend on src/services.
 */
#include "sim_registry.h"

#include "timeline_service.h"     /* timeline_service_ops   */
#include "pcap_service.h"         /* pcap_service_ops       */
#include "progress_service.h"     /* progress_service_ops   */
#include "json_test_service.h"    /* json_test_service_ops  */
#include "js_test_service.h"      /* js_test_service_ops    */
#include "gdb_service.h"          /* gdb_service_ops        */

/* Compiled-in example "plugin": the energy estimator as a built-in service,
 * selectable by config name (its engine is shared with plugins/energest.so). */
extern const sim_service_ops_t energest_service_ops;
extern const sim_service_ops_t shell_service_ops;
/* Network overlays drawn on the web UI's canvas: "plugins": ["rpl"], and
 * "rpl-packets" for the view from the packets alone. */
extern const sim_service_ops_t rpl_overlay_service_ops;
extern const sim_service_ops_t rpl_packets_overlay_service_ops;
/* "thread" / "thread-packets": OpenThread roles, partitions and links. */
extern const sim_service_ops_t thread_overlay_service_ops;
extern const sim_service_ops_t thread_packets_overlay_service_ops;

/* Renode co-simulation (csim as clock slave).  Normally selected by the
 * runner's --renode flag, which hands over its own config; also reachable
 * as a config plugins[] name, in which case it reads CSIM_RENODE. */
extern const sim_service_ops_t renode_cosim_service_ops;

/* Register the built-in library services that ship as exported ops structs.
 * The runner registers its two file-local serial-socket services
 * (serial-bridge, external-command) on top of these.  Registration order is
 * not significant — the catalog is keyed by name, and attach order (= the
 * service host's fan-out order) is decided at the attach sites, not here. */
void csim_register_builtin_services(sim_registry_t *r) {
    if (!r) return;
    sim_registry_register_service(r, &timeline_service_ops);
    sim_registry_register_service(r, &pcap_service_ops);
    sim_registry_register_service(r, &progress_service_ops);
    sim_registry_register_service(r, &json_test_service_ops);
    sim_registry_register_service(r, &js_test_service_ops);
    sim_registry_register_service(r, &gdb_service_ops);
    sim_registry_register_service(r, &energest_service_ops);
    sim_registry_register_service(r, &rpl_overlay_service_ops);
    sim_registry_register_service(r, &rpl_packets_overlay_service_ops);
    sim_registry_register_service(r, &thread_overlay_service_ops);
    sim_registry_register_service(r, &thread_packets_overlay_service_ops);
    sim_registry_register_service(r, &renode_cosim_service_ops);
    sim_registry_register_service(r, &shell_service_ops);
}
