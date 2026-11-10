# examples/rpl-udp with the MAC chosen at build time: udp-client.c and
# udp-server.c are verbatim copies of Contiki-NG's examples/rpl-udp, and the
# two builds differ only in MAKE_MAC.  Over TSCH (the default here) the
# 6TiSCH minimal schedule is used — one shared cell in a 7-slot slotframe —
# and the server, as RPL root, becomes the TSCH coordinator (tsch-rpl.c).
#   make TARGET=nrf52840 BOARD=dk CONTIKI=... udp-client udp-server                       # TSCH
#   make TARGET=nrf52840 BOARD=dk CONTIKI=... MAKE_MAC=MAKE_MAC_CSMA udp-client udp-server # CSMA
#   make TARGET=nrf52840 BOARD=dk CONTIKI=... MAKE_WITH_ORCHESTRA=1 udp-client udp-server  # TSCH, Orchestra
# (remove build/ between them).  Orchestra replaces the minimal schedule
# with its default rules, as examples/6tisch/simple-node builds them: EBs
# per time source, unicast per neighbour (RPL non-storing), a common
# shared slotframe; each on its own channel offset.  The energy ladder demo
# (examples/energy-ladder-nrf52840.yaml) runs both; provenance in
# firmware/nrf52840-dk/PROVENANCE.md.
CONTIKI_PROJECT = udp-client udp-server
all: $(CONTIKI_PROJECT)
CONTIKI ?= ../../../../contiki-ng
MAKE_MAC ?= MAKE_MAC_TSCH
MAKE_WITH_ORCHESTRA ?= 0
include $(CONTIKI)/Makefile.dir-variables
ifeq ($(MAKE_WITH_ORCHESTRA),1)
  MODULES += $(CONTIKI_NG_SERVICES_DIR)/orchestra
  CFLAGS += -DORCHESTRA_CONF_RULES="{&eb_per_time_source,&unicast_per_neighbor_rpl_ns,&default_common}"
endif
include $(CONTIKI)/Makefile.include
