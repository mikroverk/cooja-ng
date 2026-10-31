# Cooja-NG in the browser — the simulator compiled to WebAssembly.
#
#   source <emsdk>/emsdk_env.sh
#   make -C wasm          # dist/csim-cli.js + dist/web/
#   make -C wasm check    # native vs WASM parity
#   make -C wasm serve    # the site + live demos on :8765
#
# See README.md.
#
# Opt-in: nothing here is reachable from the top-level Makefile, and the
# native build is untouched.  The source lists come from the top-level
# Makefile so the two builds cannot drift; only the flags differ:
# interpreter only (no GNU Lightning), no host tuning, no LTO.

ROOT := $(abspath ..)

# The top-level Makefile, for its source lists and flags only: with
# CSIM_SOURCES_ONLY it defines no rules, so none of its targets (or its
# dependency files) leak in here.  It turns the JIT on when pkg-config finds
# Lightning; the browser build is interpreter only.
CSIM_SOURCES_ONLY := 1
override LIGHTNING_CFLAGS :=
override LIGHTNING_LIBS :=
include $(ROOT)/Makefile
.DEFAULT_GOAL := wasm

EMCC ?= emcc
WOUT := $(abspath build)
DIST := $(abspath dist)

# Every simulator source the native test_runner links, plus the runner itself
# (the scenario frontend lives in test/) and the browser adapter.
WASM_SOURCES := $(SOURCES) $(ARM_SOURCES) $(CHIPS_SOURCES) $(RISCV_SOURCES) \
                $(COMMON_SOURCES) $(SIM_SOURCES) $(SERVICES_SOURCES) \
                $(MOTES_SOURCES) $(NATIVE_SOURCES) $(UI_SOURCES) $(LIB_SOURCES) \
                $(TEST_SOURCES)
VENDOR_SOURCES := $(QUICKJS_SOURCES) $(YAML_SOURCES) $(LINENOISE_SOURCES)

WASM_OBJECTS := $(patsubst %.c,$(WOUT)/%.o,$(WASM_SOURCES)) \
                $(WOUT)/wasm/src/ui_page_none.o
VENDOR_OBJECTS := $(patsubst %.c,$(WOUT)/%.o,$(VENDOR_SOURCES))
# The browser build swaps the WebSocket server for the worker's message port
# (src/ws_server_worker.c implements the same ws_server.h), and gives the
# runner a main that hands its exit code on (src/web_main.c: under ASYNCIFY a
# returned value is lost).  Nothing else differs from the CLI build, and
# both carry an empty built-in page (src/ui_page_none.c): the site serves it.
WEB_OBJECTS := $(filter-out $(WOUT)/src/ui/ws_server.o $(WOUT)/test/test_main.o,$(WASM_OBJECTS)) \
               $(WOUT)/web/test/test_main.o \
               $(WOUT)/wasm/src/web_main.o \
               $(WOUT)/wasm/src/ws_server_worker.o

# Include paths made absolute, so that the dependency files name headers
# that exist from here (relative to the repository root they never do, and
# every object would always be out of date).
ABS_INCLUDES := $(foreach w,$(INCLUDES),$(if $(filter -%,$(w)),$(w),$(ROOT)/$(w)))
WASM_CFLAGS := -O3 $(WARNFLAGS) -std=c11 -ffp-contract=off -D_GNU_SOURCE \
               $(ABS_INCLUDES) -MMD -MP

COMMON_LDFLAGS := -lm -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=8MB

# A drop-in for build/test_runner under Node: same argv, reads the repo's
# files directly.  This is the parity oracle (`make check`).
CLI_LDFLAGS := $(COMMON_LDFLAGS) -sNODERAWFS=1 -sENVIRONMENT=node \
               -sEXIT_RUNTIME=1

# The browser build: runs in a Web Worker, firmware through MEMFS, and the
# runner loop yields to the worker's event loop (ASYNCIFY) so the page can
# pause, step and talk to the UI service while it runs.
WEB_LDFLAGS := $(COMMON_LDFLAGS) -sENVIRONMENT=worker -sMODULARIZE=1 \
               -sEXPORT_NAME=createCsim -sINVOKE_RUN=0 -sEXIT_RUNTIME=1 \
               -sASYNCIFY=1 -sASYNCIFY_STACK_SIZE=65536 -sASYNCIFY_IGNORE_INDIRECT=1 \
               -sEXPORTED_FUNCTIONS=_main,_malloc,_free,_csim_wasm_ui_recv \
               -sEXPORTED_RUNTIME_METHODS=callMain,FS,HEAPU8

.PHONY: wasm cli web site check serve profile clean-wasm

wasm: cli web

cli: $(DIST)/csim-cli.js
# dist/web drops into the GitHub Pages site as is: player.html (the web UI,
# as the site already ships it) runs a scenario live with ?wasm=NAME.
web: $(DIST)/web/csim-web.js $(DIST)/web/player.html $(DIST)/web/worker.js \
     $(DIST)/web/scenarios.json

$(WOUT)/%.o: $(ROOT)/%.c
	@mkdir -p $(dir $@)
	$(EMCC) $(WASM_CFLAGS) -c $< -o $@

$(WOUT)/web/test/test_main.o: $(ROOT)/test/test_main.c
	@mkdir -p $(dir $@)
	$(EMCC) $(WASM_CFLAGS) -Dmain=csim_runner_main -c $< -o $@

# Vendored code: built quiet, like the native rules do.  QuickJS tests the
# pre-2.0 `EMSCRIPTEN` macro; current emcc defines only __EMSCRIPTEN__.
$(WOUT)/lib/quickjs/%.o: $(ROOT)/lib/quickjs/%.c
	@mkdir -p $(dir $@)
	$(EMCC) -O2 -std=c11 -ffp-contract=off -D_GNU_SOURCE -DEMSCRIPTEN -I $(ROOT)/lib/quickjs -DCONFIG_VERSION=\"2024-01-13\" -w -c $< -o $@
$(WOUT)/lib/yaml/%.o: $(ROOT)/lib/yaml/%.c
	@mkdir -p $(dir $@)
	$(EMCC) -O2 -std=c11 -D_GNU_SOURCE -I $(ROOT)/lib/yaml $(YAML_DEFS) -w -c $< -o $@
$(WOUT)/lib/linenoise/%.o: $(ROOT)/lib/linenoise/%.c
	@mkdir -p $(dir $@)
	$(EMCC) -O2 -std=c11 -D_GNU_SOURCE -I $(ROOT)/lib/linenoise -w -c $< -o $@

$(DIST)/csim-cli.js: $(WASM_OBJECTS) $(VENDOR_OBJECTS)
	@mkdir -p $(dir $@)
	$(EMCC) $^ -o $@ $(CLI_LDFLAGS)

$(DIST)/web/csim-web.js: $(WEB_OBJECTS) $(VENDOR_OBJECTS)
	@mkdir -p $(dir $@)
	$(EMCC) $^ -o $@ $(WEB_LDFLAGS)

$(DIST)/web/player.html: $(ROOT)/ui/index.html
	@mkdir -p $(dir $@)
	cp $< $@

$(DIST)/web/worker.js: web/worker.js
	@mkdir -p $(dir $@)
	cp $< $@

# The scenarios the page offers, and the firmware and configs they read,
# served as static files under files/ at their repository paths.  Firmware
# goes without its DWARF (tools/strip-debug.py: the same program, a quarter
# of the download); other files are copied as they are.
SCENARIO_FILES := $(shell python3 -c "import json;print(' '.join(sorted({f for v in json.load(open('web/scenarios.json')).values() for f in v['files']})))")
ifeq ($(strip $(SCENARIO_FILES)),)
$(error no scenario files: web/scenarios.json is missing, malformed or lists none)
endif

$(DIST)/web/scenarios.json: web/scenarios.json $(addprefix $(ROOT)/,$(SCENARIO_FILES)) $(ROOT)/tools/strip-debug.py
	@mkdir -p $(dir $@)
	cd $(ROOT) && for f in $(SCENARIO_FILES); do mkdir -p $(DIST)/web/files/$$(dirname $$f) && python3 tools/strip-debug.py $$f $(DIST)/web/files/$$f || exit 1; done
	cp $< $@

# The web build's objects and ASYNCIFY, run under Node: the second parity
# build (`make check`), and for measuring what the UI service and the yields
# cost without a browser in the way.
profile: $(DIST)/csim-ui-node.js

$(DIST)/csim-ui-node.js: $(WEB_OBJECTS) $(VENDOR_OBJECTS)
	@mkdir -p $(dir $@)
	$(EMCC) $^ -o $@ $(COMMON_LDFLAGS) -sNODERAWFS=1 -sENVIRONMENT=node \
	    -sEXIT_RUNTIME=1 -sASYNCIFY=1 -sASYNCIFY_STACK_SIZE=65536 -sASYNCIFY_IGNORE_INDIRECT=1 \
	    --profiling-funcs

# Both builds against native: the plain one, and the page's own objects with
# ASYNCIFY (csim-ui-node.js), so what is published is what was compared.
check: cli profile
	./check-parity.sh

# The landing page with the live build beside it, as the Pages workflow
# assembles it (without the recorded replays unless DEMOS=<dir> points at
# the output of tools/record-demos.sh).
site: web
	rm -rf $(DIST)/site && mkdir -p $(DIST)/site
	cp -r $(DIST)/web/. $(DIST)/site/
	cp -r $(ROOT)/site/. $(DIST)/site/
	$(if $(DEMOS),cp -r $(DEMOS) $(DIST)/site/demos)

PORT ?= 8765
serve: site
	cd $(DIST)/site && python3 -m http.server $(PORT) --bind 127.0.0.1

clean-wasm:
	rm -rf $(WOUT) $(DIST)

-include $(sort $(WASM_OBJECTS:.o=.d) $(WEB_OBJECTS:.o=.d))
