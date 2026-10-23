/*
 * web_main — main() for the ASYNCIFY builds (the page and its Node twin).
 *
 * There the runner's main is resumed from the event loop after its first
 * yield, so when it finally returns there is no caller left to take the
 * value: Emscripten drops it and exits with 0.  test/test_main.c is built
 * with -Dmain=csim_runner_main for these builds, and this main exits with
 * the runner's code explicitly, which is what reaches worker.js's onExit.
 */
#include <stdlib.h>

int csim_runner_main(int argc, char **argv);

int main(int argc, char **argv) {
    exit(csim_runner_main(argc, argv));
}
