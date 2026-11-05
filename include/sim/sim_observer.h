/*
 * Observer events — Phase 1 milestone 6 of the refactor.
 *
 * The kernel emits observer events at well-defined points (mote add/remove,
 * UART byte/line, LED change, radio TX/RX boundary, packet frame, sim
 * start/stop).  Services subscribe via `sim_runtime_subscribe()` and
 * receive a `const sim_observer_event_t *` on every emission.
 *
 * Services must not:
 *   - call back into kernel APIs that re-enter dispatch (use
 *     sim_runtime_request_stop() for stop, schedule a future event for
 *     deferred work).
 *   - block.  Observer callbacks run on the dispatch thread.
 *   - retain the event pointer past the callback — payloads are
 *     stack-allocated by the emitter.
 *
 * See docs/design/refactor-plan.md §3.11.
 */
#ifndef SIM_OBSERVER_H
#define SIM_OBSERVER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum sim_observer_kind {
    SIM_OBS_MOTE_ADDED         = 1,  /* a node booted into the slot while the run
                                    * goes on (rebooted, added); not the
                                    * configured nodes' boot at the start or
                                    * at a restart (on_restart says that)  */
    SIM_OBS_MOTE_REMOVED,          /* a node left the slot (removed, or about
                                    * to reboot)                           */
    SIM_OBS_MOTE_UART_BYTE,    /* raw byte off the console UART          */
    SIM_OBS_MOTE_LOG_LINE,     /* line-assembled "\n"-terminated text    */
    SIM_OBS_LED_CHANGED,
    SIM_OBS_RADIO_TX_START,
    SIM_OBS_RADIO_TX_END,
    SIM_OBS_RADIO_RX_START,
    SIM_OBS_RADIO_RX_END,
    SIM_OBS_RADIO_INTERFERENCE,
    SIM_OBS_PACKET_FRAME,
    SIM_OBS_SIM_START,
    SIM_OBS_SIM_STOP,
    /* A per-node radio-state transition (OFF/ON/TX/RX/INTF).  Carries the new
     * state in u.radio_state.state (values match ui/sim_state.h's
     * sim_radio_state_t: 0=OFF 1=ON 2=TX 3=RX 4=INTF — kept as a plain int so
     * this kernel header stays independent of the UI header).  Emitted only
     * while radio-state tracking is on (the live UI, or any loaded plugin —
     * see sim_runtime_set_radio_state_tracking()); a duty-cycle / energy
     * service accumulates time-in-state from this stream. */
    SIM_OBS_RADIO_STATE,
    /* A per-mote CPU power-time snapshot: cumulative active and low-power-mode
     * (LPM/WFI) nanoseconds in u.cpu.  Emitted at end-of-run (one per mote) for
     * an energy service; carries totals, not a transition, so the last snapshot
     * is exact.  Only when radio-state tracking is on. */
    SIM_OBS_CPU_STATE,
    /* The power-state streams below are emitted only while power-state
     * tracking is on (sim_runtime_set_power_states(): the energest plugin
     * asks for it); with it off, no model stamps anything and the run is
     * byte-identical.  Every stamp comes from the node's own cycle clock
     * at the instruction boundary where the change took effect, clamped
     * monotone per node (docs/energest.md "Clocks"). */
    /* A per-mote CPU power-mode transition.  u.cpu_mode.mode is
     * SIM_CPU_MODE_*: 0 = active, 1..5 = MSP430 LPM0..LPM4, 6 = a Cortex-M
     * WFI/WFE sleep, 7..9 = a CC2538 deep sleep (SCR.SLEEPDEEP with
     * SYS_CTRL_PMCTL = PM1..PM3).  The end-of-run SIM_OBS_CPU_STATE totals remain the
     * cross-check. */
    SIM_OBS_CPU_TRANSITION,
    /* A per-mote peripheral ("component") state change: one generic event
     * for every lane a chip model can state honestly (an LED, a timer
     * running, a clock gate open).  u.component: kind (SIM_COMP_*), index
     * (unique per node and kind), state (0 = off/idle, 1 = on; kinds with
     * more states say so), name (a static string, e.g. "led1", "gpt0"). */
    SIM_OBS_COMPONENT_STATE,
    /* A transmitted frame's air time, as the radio bus timed it: from
     * u.airtime.start_ns to time_ns, u.airtime.psdu_len bytes of PSDU in
     * u.airtime.air_bytes bytes on air (SHR + PHR + PSDU) at
     * u.airtime.byte_ns per byte.  Auto-ACKs are reported too (is_ack). */
    SIM_OBS_RADIO_AIRTIME,
} sim_observer_kind_t;

/* SIM_OBS_CPU_TRANSITION modes. */
#define SIM_CPU_MODE_ACTIVE 0
#define SIM_CPU_MODE_LPM0   1      /* LPMn = SIM_CPU_MODE_LPM0 + n (MSP430) */
#define SIM_CPU_MODE_LPM4   5
#define SIM_CPU_MODE_SLEEP  6      /* Cortex-M WFI / WFE (CC2538: PM0) */
#define SIM_CPU_MODE_PM1    7      /* CC2538 deep sleep, PMCTL = PM1..PM3 */
#define SIM_CPU_MODE_PM3    9
#define SIM_CPU_MODE_COUNT  10

/* SIM_OBS_COMPONENT_STATE kinds.  The state is 0 = off, 1 = on for all. */
#define SIM_COMP_LED        1      /* a board LED lit (polarity applied)     */
#define SIM_COMP_TIMER      2      /* a hardware timer counting (MC != stop,
                                    * TASKS_START..STOP)                     */
#define SIM_COMP_CLOCK_GATE 3      /* a peripheral's run-mode clock enabled
                                    * (CC2538 RCGCx)                         */

typedef struct sim_observer_event {
    sim_observer_kind_t kind;
    int64_t  time_ns;
    int      mote_index;     /* -1 for sim-wide events                    */
    int      radio_idx;      /* -1 for non-radio events                   */
    union {
        struct { uint8_t  byte; } uart;
        struct {
            const char *line;
            int len;
            int node_id;   /* Cooja node ID (≠ mote_index) — testlog/JS
                            * consumers key on this, matching Cooja's
                            * log.log(time + " " + id + " " + msg) */
        } log_line;
        struct { int led_index; bool on; } led;
        struct {
            const uint8_t *data;
            int len;
            int channel;
            int8_t rssi;
        } radio;
        struct {
            bool is_tx;             /* true = sender side, false = receiver side */
            const char *summary;    /* packet-analyzer summary text; NULL if none */
            const uint8_t *bytes;   /* the MAC frame and its FCS, as a pcap
                                     * records it; NULL if none */
            int bytes_len;
        } frame;
        struct { int state; } radio_state;  /* SIM_OBS_RADIO_STATE: new state */
        struct { int64_t active_ns, lpm_ns; } cpu; /* SIM_OBS_CPU_STATE totals */
        struct { int mode; } cpu_mode;      /* SIM_OBS_CPU_TRANSITION: new mode */
        struct {                            /* SIM_OBS_COMPONENT_STATE */
            int kind, index, state;
            const char *name;
        } component;
        struct {                            /* SIM_OBS_RADIO_AIRTIME */
            int64_t start_ns;
            int64_t byte_ns;
            int psdu_len, air_bytes;
            bool is_ack;
        } airtime;
    } u;
} sim_observer_event_t;

typedef void (*sim_observer_callback_t)(void *user,
                                        const sim_observer_event_t *ev);

#ifdef __cplusplus
}
#endif

#endif /* SIM_OBSERVER_H */
