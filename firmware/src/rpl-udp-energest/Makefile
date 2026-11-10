# rpl-udp with the firmware's own energest accounting printed every 10 s —
# the firmware half of tools/check-energest-agreement.py (agent-sim M4).
# udp-client.c / udp-server.c are verbatim copies of examples/rpl-udp.
CONTIKI_PROJECT = udp-client udp-server
all: $(CONTIKI_PROJECT)
CONTIKI ?= ../../../../contiki-ng
MODULES += os/services/simple-energest
include $(CONTIKI)/Makefile.include
