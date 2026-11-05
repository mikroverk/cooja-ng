# examples/rpl-udp with the MAC chosen at build time: udp-client.c and
# udp-server.c are verbatim copies of Contiki-NG's examples/rpl-udp, and the
# two builds differ only in MAKE_MAC.  Over TSCH (the default here) the
# 6TiSCH minimal schedule is used — one shared cell in a 7-slot slotframe —
# and the server, as RPL root, becomes the TSCH coordinator (tsch-rpl.c).
#   make TARGET=nrf52840 BOARD=dk CONTIKI=... udp-client udp-server                       # TSCH
#   make TARGET=nrf52840 BOARD=dk CONTIKI=... MAKE_MAC=MAKE_MAC_CSMA udp-client udp-server # CSMA
# (remove build/ between the two).  The energy ladder demo
# (examples/energy-ladder-nrf52840.yaml) runs both; provenance in
# firmware/nrf52840-dk/PROVENANCE.md.
CONTIKI_PROJECT = udp-client udp-server
all: $(CONTIKI_PROJECT)
CONTIKI ?= ../../../../contiki-ng
MAKE_MAC ?= MAKE_MAC_TSCH
include $(CONTIKI)/Makefile.include
