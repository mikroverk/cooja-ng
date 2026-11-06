/*
 * cngsim — Cooja-NG for people.
 *
 *   cngsim run <sim.yaml | example | firmware...> [options]
 *   cngsim new [template] [options]
 *   cngsim check <sim.yaml>
 *   cngsim examples | boards | convert <sim.csc> | help [command]
 *
 * A frontend, not a second simulator: `run` calls the same entry point as
 * `test_runner test` (run_mixed_multinode_test), with the same arguments
 * spelled for people, so a simulation cannot differ between the two
 * binaries.  What differs is what reaches the terminal.  test_runner
 * prints everything (it is what the determinism gates diff); cngsim runs
 * the simulation with its own stdout and stderr fed through a filter that
 * keeps the node consoles, the checks and a one-line verdict, and drops
 * the radio traces, ELF patch details and chip counters unless --trace or
 * -v asks for them.  With --shell or --script the output is the session
 * itself and passes through untouched.
 *
 * Bundled examples and firmware come from the data directory: $CNGSIM_DATA,
 * else <prefix>/share/cooja-ng next to an installed bin/cngsim, else the
 * checkout that build/cngsim was built in.
 *
 * docs/quick-start.md is the user guide; the plan behind it is
 * docs/design/release-0.3.1-usability-plan.md.
 */
#include "csim_version.h"
#include "sim_board.h"
#include "sim_config.h"
#include "sim_fwvar.h"
#include "sim_registry.h"
#include "cJSON.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__FreeBSD__)
#include <sys/sysctl.h>
#endif

extern char **environ;

/* The runner (test/test_mixed_multinode.c) and its two cngsim hooks. */
extern int run_mixed_multinode_test(int argc, char **argv);
extern int csim_cli_enter_resumes;
extern int csim_cli_pause_at_end;
extern int csim_cli_hold_verdict;
extern volatile sig_atomic_t csim_cli_stop;
extern int64_t csim_cli_simulated_ns;
extern double csim_cli_paused_ms;
extern int csim_cli_run_complete;

#define EXIT_INVALID 2
#define EXIT_INTERRUPTED 130    /* 128 + SIGINT: the shell's convention for Ctrl-C */

static void fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("cngsim: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static int has_suffix(const char *s, const char *suf) {
    size_t n = strlen(s), m = strlen(suf);
    return n >= m && strcmp(s + n - m, suf) == 0;
}

static int is_config_path(const char *p) {
    return has_suffix(p, ".yaml") || has_suffix(p, ".yml") || has_suffix(p, ".json");
}

static int is_file(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static int is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/* "8080": all digits, 1..65535 (so `--ui 2node.yaml` is not a port). */
static int is_port(const char *s) {
    if (!*s) return 0;
    for (const char *c = s; *c; c++)
        if (!isdigit((unsigned char)*c)) return 0;
    long v = strtol(s, NULL, 10);
    return v >= 1 && v <= 65535 && strlen(s) <= 5;
}

/* ---- data directory ------------------------------------------------------ */

static int exe_path(char *out, size_t n, const char *argv0) {
    char buf[PATH_MAX];
#if defined(__APPLE__)
    uint32_t sz = sizeof(buf);
    if (_NSGetExecutablePath(buf, &sz) == 0 && realpath(buf, out)) return 0;
#elif defined(__linux__)
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        snprintf(out, n, "%s", buf);
        return 0;
    }
#elif defined(__FreeBSD__)
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1 };
    size_t sz = sizeof(buf);
    if (sysctl(mib, 4, buf, &sz, NULL, 0) == 0) {
        snprintf(out, n, "%s", buf);
        return 0;
    }
#endif
    if (strchr(argv0, '/') && realpath(argv0, buf)) {
        snprintf(out, n, "%s", buf);
        return 0;
    }
    (void)n;
    return -1;
}

static char g_argv0[PATH_MAX];
static char g_data[PATH_MAX];

/* The directory holding examples/ and firmware/, or NULL. */
static const char *data_dir(void) {
    if (g_data[0]) return g_data;
    const char *env = getenv("CNGSIM_DATA");
    if (env && *env) {
        snprintf(g_data, sizeof(g_data), "%s", env);
        return g_data;
    }
    char exe[PATH_MAX];
    if (exe_path(exe, sizeof(exe), g_argv0) != 0) return NULL;
    char *slash = strrchr(exe, '/');
    if (!slash) return NULL;
    *slash = '\0';                                   /* exe = its directory */
    const char *cands[] = { "/../share/cooja-ng", "/.." };
    for (size_t i = 0; i < sizeof(cands) / sizeof(cands[0]); i++) {
        char d[PATH_MAX], ex[PATH_MAX + 16];
        snprintf(d, sizeof(d), "%s%s", exe, cands[i]);
        snprintf(ex, sizeof(ex), "%s/examples", d);
        if (is_dir(ex) && realpath(d, g_data)) return g_data;
    }
    return NULL;
}

static int example_path(const char *name, char *out, size_t n) {
    const char *d = data_dir();
    if (!d || strchr(name, '/')) return -1;
    snprintf(out, n, "%s/examples/%s.yaml", d, name);
    return is_file(out) ? 0 : -1;
}

/* ---- examples, boards ---------------------------------------------------- */

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static int cmd_examples(void) {
    const char *d = data_dir();
    char dir[PATH_MAX + 16];
    if (!d) {
        fail("no examples found: set CNGSIM_DATA to the directory that holds "
             "examples/ and firmware/");
        return EXIT_INVALID;
    }
    snprintf(dir, sizeof(dir), "%s/examples", d);
    DIR *dp = opendir(dir);
    if (!dp) {
        fail("cannot read %s", dir);
        return EXIT_INVALID;
    }
    char *names[256];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(dp)) && n < 256) {
        if (e->d_name[0] != '.' && has_suffix(e->d_name, ".yaml"))
            names[n++] = strdup(e->d_name);
    }
    closedir(dp);
    qsort(names, (size_t)n, sizeof(names[0]), cmp_str);
    printf("Examples (cngsim run <name>, add --ui to watch it in the browser):\n\n");
    for (int i = 0; i < n; i++) {
        char path[PATH_MAX + 300];
        snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        names[i][strlen(names[i]) - 5] = '\0';
        static sim_normalized_config_t cfg;
        const char *title = "";
        if (sim_config_load(&cfg, path) == 0) title = cfg.title;
        printf("  %-22s %s\n", names[i], title);
        sim_config_free(&cfg);
        free(names[i]);
    }
    printf("\nThe files are in %s: copy one to start your own.\n", dir);
    return 0;
}

/* Registry row i if it is the first row of its board (".nrf52840" and
 * ".nrf52840-dk" are one board), else NULL; *end set past the table. */
static const sim_board_desc_t *board_row(int i, int *end) {
    const sim_board_desc_t *b = sim_board_at(i);
    if (!b) { *end = 1; return NULL; }
    if (!b->name) return NULL;                    /* host-process kinds */
    for (int j = 0; j < i; j++) {
        const sim_board_desc_t *o = sim_board_at(j);
        if (o->name && strcmp(o->name, b->name) == 0) return NULL;
    }
    return b;
}

/* "nrf52840dk/nrf52840", with a * when no bundled image of that system
 * has run on the board here; "-" when it has no such name. */
static const char *os_cell(const char *name, unsigned runs, unsigned bit, char *buf, size_t n) {
    if (!name) return "-";
    snprintf(buf, n, "%s%s", name, (runs & bit) ? "" : " *");
    return buf;
}

static int cmd_boards(int argc, char **argv) {
    int os = 0;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--os") == 0) os = 1;
        else {
            fail("boards: unknown option '%s' (cngsim help boards)", argv[i]);
            return EXIT_INVALID;
        }
    }
    int end = 0;
    if (os) {
        printf("%-20s %-32s %-17s %-12s %s\n", "board", "Zephyr (west build -b)",
               "RIOT (BOARD=)", "OpenThread", "Contiki-NG");
        for (int i = 0; !end; i++) {
            const sim_board_desc_t *b = board_row(i, &end);
            if (!b) continue;
            char z[64], r[64], o[64];
            printf("%-20s %-32s %-17s %-12s %s\n", b->name,
                   os_cell(b->zephyr, b->runs, SIM_OS_ZEPHYR, z, sizeof(z)),
                   os_cell(b->riot, b->runs, SIM_OS_RIOT, r, sizeof(r)),
                   os_cell(b->openthread, b->runs, SIM_OS_OPENTHREAD, o, sizeof(o)),
                   b->build);
        }
        printf("\n--board and a mote type's  board:  take any of these names, and also:");
        for (int i = 0;; i++) {
            const sim_board_alias_t *a = sim_board_alias_at(i);
            if (!a) break;
            printf("%s%s", i == 0 ? "\n  " : i % 3 ? ", " : ",\n  ", a->alias);
        }
        printf(".\n* the board has that name there, but no image built with it has run\n"
               "  here yet.  The dongle's console is USB, which is not modelled: Zephyr\n"
               "  and RIOT print over it by default.  cc1312r7_lp is a board of this\n"
               "  repository (tools/zephyr-boards), not one in Zephyr's tree.\n"
               "OpenThread: ot-cli-ftd from that port, unmodified (docs/openthread.md).\n");
        return 0;
    }
    printf("%-20s %-24s %-19s %-27s %s\n", "board", "", "cpu", "radio",
           "build with (Contiki-NG)");
    for (int i = 0; !end; i++) {
        const sim_board_desc_t *b = board_row(i, &end);
        if (!b) continue;
        printf("%-20s %-24s %-19s %-27s %s\n", b->name, b->title, b->cpu,
               b->radio, b->build);
    }
    printf("\nAn image named <app>.<board> runs as that board.  For any other name\n"
           "(.elf, .nrf, .simplelink, .zoul ...), add  board: <board>  to its mote\n"
           "type.  Native Cooja motes (TARGET=cooja, .cooja) run too.  Zephyr, RIOT\n"
           "and OpenThread names for the boards (accepted as well): cngsim boards --os\n");
    return 0;
}

/* ---- output filter (run) -------------------------------------------------- */

enum { TRACE_NONE = 0, TRACE_RADIO = 1, TRACE_BOOT = 2, TRACE_STATS = 4,
       TRACE_ALL = 7 };

typedef struct {
    int fd;                 /* read end */
    int out;                /* where filtered lines go */
    int is_stdout;
} filt_arg_t;

static struct {
    int quiet, trace, ui, paused_ui, enter_ok, no_open;
    int running;            /* "--- Simulation running ---" seen */
    char title[160];
    int nodes, seed;
    double duration_s;
    char verdict[16];
    char fail_reason[400];
    double test_end_s, wall_s, sim_s, speed;
    int have_test_end, have_wall, have_sim, have_speed;
} F;

static void out_line(int fd, const char *s) {
    size_t n = strlen(s);
    while (n > 0) {
        ssize_t w = write(fd, s, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return;
        s += w;
        n -= (size_t)w;
    }
}

/* Formatted, whole: a console line can be as long as the filter's line
 * buffer, and a cut one would differ from what test_runner prints. */
static void out_fmt(int fd, const char *fmt, ...) {
    char buf[2048];
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n >= (int)sizeof(buf)) {
        char *big = malloc((size_t)n + 1);
        if (big) {
            vsnprintf(big, (size_t)n + 1, fmt, ap2);
            out_line(fd, big);
            free(big);
            va_end(ap2);
            return;
        }
    }
    va_end(ap2);
    if (n >= 0) out_line(fd, buf);
}

static void open_browser(const char *url) {
#if defined(__APPLE__)
    const char *opener = "open";
#else
    const char *opener = "xdg-open";
#endif
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    char *argv[] = { (char *)opener, (char *)url, NULL };
    pid_t pid;
    if (posix_spawnp(&pid, opener, &fa, NULL, argv, environ) == 0)
        (void)pid;            /* reaped with the others when cngsim exits */
    posix_spawn_file_actions_destroy(&fa);
}

static int skip_ws(const char *s) {
    int i = 0;
    while (s[i] == ' ' || s[i] == '\t') i++;
    return i;
}

/* "   12.345 [Node 2/MSP430] ..." */
static int is_console_line(const char *s) {
    int i = skip_ws(s);
    if (!isdigit((unsigned char)s[i])) return 0;
    while (isdigit((unsigned char)s[i])) i++;
    if (s[i++] != '.') return 0;
    for (int k = 0; k < 3; k++)
        if (!isdigit((unsigned char)s[i++])) return 0;
    return strncmp(s + i, " [Node ", 7) == 0;
}

/* ---- console lines: the board and OS in the label, no ANSI escapes ------ */

/* test_runner labels a console line with the CPU family ("[Node 2/ARM]"),
 * and the determinism gates diff that.  cngsim's terminal shows the board,
 * and the OS when the image is not Contiki-NG: "[Node 2/nrf52840-dk Zephyr]".
 * Nodes it cannot place (added at run time, host-process motes) keep the
 * runner's label. */
static struct { int id; char label[64]; } g_label[MAX_SIM_NODES];
/* Each test step's node filter (-1 = any node), so a step line can say
 * "on any node" for a count gathered over several nodes. */
static int g_step_node[MAX_TEST_STEPS];
static int g_nstep;
static int g_nlabel;

/* The OS an image was built with, from a symbol each one's start-up code
 * defines; NULL for Contiki-NG and for anything unrecognised. */
static const char *image_os(const char *fw) {
    uint32_t a;
    if (sim_fwvar_symbol(fw, NULL, "z_cstart", &a, NULL, NULL)) return "Zephyr";
    if (sim_fwvar_symbol(fw, NULL, "kernel_init", &a, NULL, NULL) &&
        sim_fwvar_symbol(fw, NULL, "sched_run", &a, NULL, NULL)) return "RIOT";
    if (sim_fwvar_symbol(fw, NULL, "otInstanceInitSingle", &a, NULL, NULL)) return "OpenThread";
    return NULL;
}

static void add_label(int id, const char *fw, const char *board, const char *meta) {
    if (g_nlabel >= MAX_SIM_NODES || !fw || !fw[0]) return;
    char err[256];
    const sim_board_desc_t *b = sim_board_resolve(fw, board && board[0] ? board : NULL,
                                                  err, sizeof(err));
    if (!b || !b->name) return;               /* host-process kinds: as the runner has it */
    const char *os = NULL;
    char meta_os[32] = "";
    cJSON *m = meta && meta[0] ? cJSON_Parse(meta) : NULL;
    cJSON *o = m ? cJSON_GetObjectItemCaseSensitive(m, "OS") : NULL;
    if (cJSON_IsString(o) && o->valuestring[0])
        snprintf(meta_os, sizeof(meta_os), "%s", o->valuestring);
    cJSON_Delete(m);
    os = meta_os[0] ? meta_os : image_os(fw);
    if (os && strcmp(os, "Contiki-NG") == 0) os = NULL;
    g_label[g_nlabel].id = id;
    snprintf(g_label[g_nlabel].label, sizeof(g_label[0].label), "%s%s%s", b->name,
             os ? " " : "", os ? os : "");
    g_nlabel++;
}

static void labels_from_config(const sim_normalized_config_t *cfg) {
    g_nstep = cfg->test.step_count < MAX_TEST_STEPS ? cfg->test.step_count : MAX_TEST_STEPS;
    for (int i = 0; i < g_nstep; i++) g_step_node[i] = cfg->test.steps[i].node;
    for (int i = 0; i < cfg->node_count; i++) {
        const sim_node_config_t *nd = &cfg->nodes[i];
        const char *fw = nd->firmware, *board = NULL;
        for (int t = 0; nd->type_name[0] && t < cfg->mote_type_count; t++) {
            const sim_mote_type_t *mt = &cfg->mote_types[t];
            if (strcmp(mt->name, nd->type_name) != 0) continue;
            fw = mt->firmware[0] ? mt->firmware : cfg->mote_type_firmware[t];
            board = mt->board;
        }
        add_label(nd->id, fw, board, nd->meta);
    }
}

/* Render a line's ANSI escapes the way a terminal would leave the line
 * (a Zephyr shell redraws its prompt: ESC[1;32m uart:~$ ESC[m ESC[8D ESC[J,
 * then its log line): colours dropped, the cursor moved left (ESC[nD) and
 * right (ESC[nC), a backspace stepping back; text after a move overwrites.
 * ESC[K and ESC[J (on this one line the same) erase: with no number or 0
 * to the end, 1 from the start to the cursor, 2 (J also 3) the whole line,
 * erased cells before the cursor left blank.  Any other escape is dropped
 * whole, in ECMA-48's forms as the shell's cmd drops them: ESC, bytes
 * 0x20-0x2f and one more (ESC ( B, ESC ) 0, ESC # 8); ESC ] P X ^ _ and a
 * string up to BEL or ESC \ (OSC, a terminal title; DCS), or to the next
 * escape; else ESC and one byte (ESC =, ESC 7).  One cut off at the end of
 * the line takes the rest.  In place; returns whether the line had any. */
static int render_ansi(char *s) {
    size_t len = 0, cur = 0;
    int had = 0;
    for (char *r = s; *r; r++) {
        if (*r == '\b') { if (cur) cur--; had = 1; continue; }
        if (*r != 0x1b) {
            s[cur++] = *r;                 /* cur <= r - s: never ahead of the reader */
            if (cur > len) len = cur;
            continue;
        }
        had = 1;
        if (r[1] >= 0x20 && r[1] <= 0x2f) {            /* ESC ( B */
            r++;
            while (r[1] >= 0x20 && r[1] <= 0x2f) r++;
            if (r[1]) r++;                             /* its final byte */
            continue;
        }
        if (r[1] == ']' || r[1] == 'P' || r[1] == 'X' || r[1] == '^' || r[1] == '_') {
            r += 2;                                    /* a string: OSC, DCS ... */
            while (*r && *r != 0x07 && *r != 0x1b) r++;
            if (!*r) break;
            if (*r == 0x1b) {
                if (r[1] == '\\') r++;                /* ESC \ ends it */
                else r--;                              /* another escape: read it next */
            }
            continue;
        }
        if (r[1] != '[') { if (r[1]) r++; continue; }
        r += 2;
        long n = 0;
        int have_n = 0;
        while (*r && !(*r >= 0x40 && *r <= 0x7e)) {
            if (isdigit((unsigned char)*r)) { n = n * 10 + (*r - '0'); have_n = 1; if (n > 4096) n = 4096; }
            r++;
        }
        if (!*r) break;
        if (!have_n) n = 1;
        if (*r == 'D') cur = (size_t)n >= cur ? 0 : cur - (size_t)n;
        else if (*r == 'C') { cur += (size_t)n; if (cur > len) cur = len; }
        else if (*r == 'J' || *r == 'K') {
            long how = have_n ? n : 0;
            if (how == 0) {
                len = cur;                               /* to the end */
            } else if (how == 1) {                       /* the start to the cursor */
                for (size_t k = 0; k <= cur && k < len; k++) s[k] = ' ';
            } else if (how == 2 || (how == 3 && *r == 'J')) {
                for (size_t k = 0; k < cur; k++) s[k] = ' ';
                len = cur;
            }
        }
    }
    s[len] = '\0';
    return had;
}

/* A console line as the terminal shows it, into `out`; 0 to show nothing
 * (a line that was nothing but escapes, and spaces). */
static int console_text(const char *line, char *out, size_t n) {
    snprintf(out, n, "%s", line);
    char *lb = strstr(out, " [Node ");
    if (!lb) return 1;
    char *num = lb + 7, *slash = num, *end;
    while (isdigit((unsigned char)*slash)) slash++;
    end = *slash == '/' ? strchr(slash, ']') : NULL;
    if (!end) return 1;
    int id = atoi(num);
    char *text = end + 1;
    int had = render_ansi(text);
    if (had && text[strspn(text, " ")] == '\0') return 0;   /* nothing left to see */
    for (int k = 0; k < g_nlabel; k++) {
        if (g_label[k].id != id) continue;
        char rest[16384];
        snprintf(rest, sizeof(rest), "%s", end);
        snprintf(slash + 1, n - (size_t)(slash + 1 - out), "%s%s", g_label[k].label, rest);
        break;
    }
    return 1;
}

static void console_out(int out, const char *line) {
    static char buf[16384 + 128];
    if (console_text(line, buf, sizeof(buf))) out_fmt(out, "%s\n", buf);
}

static void header(int out) {
    static int done;
    if (F.quiet || done) return;
    done = 1;
    char buf[512];
    int n = snprintf(buf, sizeof(buf), "cooja-ng %s", CSIM_VERSION);
    if (F.title[0]) n += snprintf(buf + n, sizeof(buf) - (size_t)n, " · %s", F.title);
    if (F.nodes > 0)
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, " · %d node%s", F.nodes,
                      F.nodes == 1 ? "" : "s");
    if (F.seed) n += snprintf(buf + n, sizeof(buf) - (size_t)n, " · seed %d", F.seed);
    if (F.duration_s > 0 && !F.ui)
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, " · %g s", F.duration_s);
    snprintf(buf + n, sizeof(buf) - (size_t)n, "\n");
    out_line(out, buf);
}

static void stdout_line(int out, char *line) {
    const char *t = line + skip_ws(line);
    if (strncmp(t, "WebSocket UI server listening on ", 33) == 0) {
        char url[256];
        snprintf(url, sizeof(url), "%s", t + 33);
        char *sp = strchr(url, ' ');
        if (sp) *sp = '\0';
        header(out);
        out_fmt(out, "Web UI: %s\n", url);
        if (F.paused_ui)
            out_fmt(out, "Paused: press Play (or Space) in the browser%s to start.  "
                         "Ctrl-C quits.\n", F.enter_ok ? ", or Enter here," : "");
        else
            out_fmt(out, "Ctrl-C quits.\n");
        if (strstr(t, "NOT loopback")) out_fmt(out, "%s\n", t);
        if (!F.no_open) open_browser(url);
        return;
    }
    if (!F.running) {
        if (strncmp(t, "Config: ", 8) == 0) {
            snprintf(F.title, sizeof(F.title), "%s", t + 8);
        } else if (strncmp(t, "--- Simulation running ---", 26) == 0) {
            F.running = 1;
            header(out);
        } else if (is_console_line(line)) {
            /* A node can print while it is set up (a JS mote's init()). */
            header(out);
            if (!F.quiet) console_out(out, line);
        } else if (F.trace & TRACE_BOOT) {
            out_fmt(out, "%s\n", line);
        } else if (strstr(t, "rror") || strstr(t, "annot ") || strstr(t, "ailed") ||
                   strstr(t, "arning")) {
            out_fmt(out, "%s\n", t);
        }
        return;
    }
    if (is_console_line(line)) {
        if (!F.quiet) console_out(out, line);
        return;
    }
    /* The energest plugin's end-of-run report: the setup asked for it. */
    if (strncmp(t, "energest: ", 10) == 0) {
        if (!F.quiet) out_fmt(out, "%s\n", t);
        return;
    }
    /*   TEST step 2 PASS: "Received response" matched 3/3 (node 2, 37083 ms) */
    {
        int step;
        char ok[8], pat[256];
        int got, want, node;
        long ms;
        if (sscanf(t, "TEST step %d %7[A-Z]: \"%255[^\"]\" matched %d/%d (node %d, %ld ms)",
                   &step, ok, pat, &got, &want, &node, &ms) == 7) {
            if (!F.quiet) {
                char times[32] = "";
                char where[48];
                if (want != 1) snprintf(times, sizeof(times), " ×%d", want);
                /* A count over any node can be gathered from several: the
                 * runner names only the node of the last match. */
                if (want != 1 && step >= 0 && step < g_nstep && g_step_node[step] < 0)
                    snprintf(where, sizeof(where), "on any node, the last on node %d", node);
                else
                    snprintf(where, sizeof(where), "on node %d", node);
                out_fmt(out, "  %s \"%s\"%s %s (%.3f s)\n",
                        strcmp(ok, "PASS") == 0 ? "✓" : "✗", pat, times, where,
                        (double)ms / 1000.0);
            }
            return;
        }
    }
    /*   Step 1 [FAIL]: wait "Received response" node=2   (the end-of-run list) */
    if (strncmp(t, "Step ", 5) == 0 && strstr(t, "]: ")) {
        if (strstr(t, "[FAIL]") || strstr(t, "[PENDING]") || strstr(t, "[TIMEOUT]"))
            out_fmt(out, "  ✗ %s\n", strstr(t, "]: ") + 3);
        return;
    }
    /*   Validator [PASS]: "Received request" matched 46/1 node=1 */
    if (strncmp(t, "Validator [", 11) == 0) {
        char ok[8], pat[256];
        int got, want, node = 0;
        int k = sscanf(t, "Validator [%7[A-Z]]: \"%255[^\"]\" matched %d/%d node=%d",
                       ok, pat, &got, &want, &node);
        if (k >= 4 && !F.quiet) {
            char where[32] = "";
            if (k == 5) snprintf(where, sizeof(where), " on node %d", node);
            out_fmt(out, "  %s \"%s\"%s (seen %d, needed %d)\n",
                    strcmp(ok, "PASS") == 0 ? "✓" : "✗", pat, where, got, want);
        }
        return;
    }
    if (strncmp(t, "TEST PASSED", 11) == 0) {
        snprintf(F.verdict, sizeof(F.verdict), "PASSED");
        long ms;
        if (sscanf(t, "TEST PASSED (%ld ms simulated)", &ms) == 1) {
            F.test_end_s = (double)ms / 1000.0;
            F.have_test_end = 1;
        }
        return;
    }
    if (strncmp(t, "TEST FAILED", 11) == 0) {
        snprintf(F.verdict, sizeof(F.verdict), "FAILED");
        const char *why = t + 11;
        while (*why == ':' || *why == ' ') why++;
        snprintf(F.fail_reason, sizeof(F.fail_reason), "%s", why);
        return;
    }
    if (strncmp(t, "Wall-clock time:", 16) == 0) {
        double ms, s;
        if (sscanf(t, "Wall-clock time: %lf ms (%lf s)", &ms, &s) == 2) {
            F.wall_s = ms / 1000.0;
            F.have_wall = 1;
        }
    } else if (strncmp(t, "Simulated time:", 15) == 0) {
        long ms;
        if (sscanf(t, "Simulated time: %ld ms", &ms) == 1) {
            F.sim_s = (double)ms / 1000.0;
            F.have_sim = 1;
        }
    } else if (strncmp(t, "Speed ratio:", 12) == 0) {
        if (sscanf(t, "Speed ratio: %lfx", &F.speed) == 1) F.have_speed = 1;
    } else if (strncmp(t, "Paused at the end", 17) == 0 || strncmp(t, "Checks ", 7) == 0 ||
               strncmp(t, "--wall-timeout", 14) == 0 || strstr(t, "rror") ||
               strstr(t, "arning")) {
        out_fmt(out, "%s\n", t);
        return;
    }
    if (F.trace & TRACE_STATS) out_fmt(out, "%s\n", line);
}

static int is_radio_trace(const char *s) {
    const char *t = s + skip_ws(s);
    return strncmp(t, "[PKT]", 5) == 0 || strncmp(t, "[HEX]", 5) == 0 ||
           strncmp(t, "[RF]", 4) == 0 || strncmp(t, "[UIP]", 5) == 0;
}

/* A line on stderr that tells the user something went wrong.  Everything
 * else there is a developer trace: kept (the last ERR_KEEP lines) and shown
 * only if the run then fails without a verdict, where it may explain why.
 * The words are the ones the loader, the runner and the services use in
 * their messages to people.  Words that radio and CPU traces use ("abort",
 * "fault") are left out, or every such trace would reach the terminal; a
 * new message for people should use one of these words. */
static int looks_like_error(const char *s) {
    static const char *const words[] = {
        "rror", "annot ", "ailed", "arning", "nknown", "nvalid", "issing",
        "no board", "not found", "No such", "must ", "needs ", "too many",
        "refus", "denied", "not sent",
    };
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++)
        if (strstr(s, words[i])) return 1;
    return 0;
}

#define ERR_KEEP 30
static char err_ring[ERR_KEEP][512];
static int err_count;

static void stderr_line(int out, char *line) {
    if (is_radio_trace(line)) {
        if (F.trace & TRACE_RADIO) out_fmt(out, "%s\n", line);
        return;
    }
    if (looks_like_error(line) || (F.trace & TRACE_STATS)) {
        out_fmt(out, "%s\n", line);
        return;
    }
    snprintf(err_ring[err_count % ERR_KEEP], sizeof(err_ring[0]), "%s", line);
    err_count++;
}

static void *filter_thread(void *p) {
    filt_arg_t *a = p;
    char buf[8192];
    char line[16384];
    size_t len = 0;
    for (;;) {
        ssize_t n = read(a->fd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n' || len == sizeof(line) - 1) {
                line[len] = '\0';
                if (a->is_stdout) stdout_line(a->out, line);
                else stderr_line(a->out, line);
                len = 0;
                if (buf[i] == '\n') continue;
            }
            line[len++] = buf[i];
        }
    }
    if (len) {
        line[len] = '\0';
        if (a->is_stdout) stdout_line(a->out, line);
        else stderr_line(a->out, line);
    }
    close(a->fd);
    return NULL;
}

/* ---- run --------------------------------------------------------------- */

/* A board's RIOT name (BOARD=), one a registry row or a RIOT alias has. */
static int is_riot_board(const char *name) {
    for (int i = 0;; i++) {
        const sim_board_desc_t *b = sim_board_at(i);
        if (!b) break;
        if (b->riot && strcmp(b->riot, name) == 0) return 1;
    }
    for (int i = 0;; i++) {
        const sim_board_alias_t *a = sim_board_alias_at(i);
        if (!a) break;
        if (a->system && strncmp(a->system, "RIOT", 4) == 0 && strcmp(a->alias, name) == 0)
            return 1;
    }
    return 0;
}

/* The board a build directory names for an image whose file name does not:
 * a Zephyr build's .config next to zephyr.elf (build/zephyr/.config, its
 * CONFIG_BOARD_TARGET, "nrf52840dk/nrf52840", else CONFIG_BOARD), or a RIOT
 * build's bin/<BOARD>/ directory when <BOARD> is a RIOT name of a board
 * here (any other bin/<dir>/x.elf is not taken for a RIOT build: it gets
 * the usual error for its extension).  The name goes into `name`, where it
 * was found into `from`; 0 when neither is there.  A .config's name is not
 * checked here: the caller looks it up and refuses an unknown one. */
static int board_from_build(const char *image, char *name, size_t n,
                            char *from, size_t fn) {
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", image);
    char *sl = strrchr(dir, '/');
    if (sl) *sl = '\0'; else snprintf(dir, sizeof(dir), ".");
    char cfgp[PATH_MAX + 16];
    snprintf(cfgp, sizeof(cfgp), "%s/.config", dir);
    FILE *f = fopen(cfgp, "r");
    if (f) {
        char line[512], target[128] = "", board[128] = "";
        while (fgets(line, sizeof(line), f)) {
            char v[128];
            if (sscanf(line, "CONFIG_BOARD_TARGET=\"%127[^\"]\"", v) == 1)
                snprintf(target, sizeof(target), "%s", v);
            else if (sscanf(line, "CONFIG_BOARD=\"%127[^\"]\"", v) == 1)
                snprintf(board, sizeof(board), "%s", v);
        }
        fclose(f);
        /* "board@revision/qualifiers": the revision does not change the board */
        char *at = strchr(target, '@');
        if (at) {
            char *rest = strchr(at, '/');
            memmove(at, rest ? rest : at + strlen(at), strlen(rest ? rest : at + strlen(at)) + 1);
        }
        const char *pick = target[0] ? target : board;
        if (pick[0]) {
            snprintf(name, n, "%s", pick);
            snprintf(from, fn, "%s (%s)", cfgp, target[0] ? "CONFIG_BOARD_TARGET" : "CONFIG_BOARD");
            return 1;
        }
    }
    /* RIOT: <app>/bin/<BOARD>/<app>.elf */
    const char *b = strrchr(dir, '/');
    b = b ? b + 1 : dir;
    size_t bl = strlen(b);
    if (bl > 0 && bl < 64 && (size_t)(b - dir) >= 4 && strncmp(b - 4, "bin/", 4) == 0 &&
        (b - 4 == dir || b[-5] == '/') && is_riot_board(b)) {
        snprintf(name, n, "%s", b);
        snprintf(from, fn, "the RIOT build directory %s", dir);
        return 1;
    }
    return 0;
}


/* A simulated duration, which must carry its unit: "60s", "500ms", "2m".
 * A bare number is refused rather than guessed — test_runner's -t takes
 * milliseconds and --wall-timeout seconds, so "-t 10" would be 10 ms to one
 * reader and 10 s to the next.  -2 = a number without a unit; -3 = less
 * than 1 ms or more than the INT_MAX ms (24.8 days) a run can simulate. */
static int parse_duration_ms(const char *s, long *ms) {
    char *end;
    double v = strtod(s, &end);
    if (end == s || !(v >= 0) || v > 1e12) return -1;
    if (*end == '\0') return -2;
    double mult;
    if (strcmp(end, "ms") == 0) mult = 1.0;
    else if (strcmp(end, "s") == 0) mult = 1000.0;
    else if (strcmp(end, "m") == 0 || strcmp(end, "min") == 0) mult = 60000.0;
    else if (strcmp(end, "h") == 0) mult = 3600000.0;
    else return -1;
    double r = floor(v * mult + 0.5);
    if (r < 1 || r > INT_MAX) return -3;
    *ms = (long)r;
    return 0;
}

#define DURATION_RANGE "from 1ms to 596h (24.8 days)"

/* Ctrl-C: the first one ends the run the normal way (the verdict, the exit
 * code, --save-config); a second one, if that hangs, ends cngsim at once. */
static void on_sigint(int sig) {
    if (csim_cli_stop) {
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }
    csim_cli_stop = 1;
}

#define MAX_ARGS 320          /* 64 images, each with its own --board FILE=NAME */

static int cmd_run(int argc, char **argv) {
    char *rargv[MAX_ARGS];
    int rargc = 0;
    const char *targets[64];
    int ntargets = 0;
    int ui = 0, start = 0, verbose = 0, passthrough = 0, have_speed = 0;
    const char *port = "8080";
    long duration_ms = -1;          /* the last --duration given wins */
    static char duration_arg[32];
    const char *board = NULL;

    memset(&F, 0, sizeof(F));
#define PUSH(x) do { if (rargc >= MAX_ARGS - 1) { fail("too many arguments"); return EXIT_INVALID; } \
                     rargv[rargc++] = (char *)(x); } while (0)
#define NEED_VALUE() do { if (i + 1 >= argc) { fail("run: %s needs a value", a); return EXIT_INVALID; } } while (0)
    char *opts[MAX_ARGS];
    int nopts = 0;
#define OPT(x) do { if (nopts >= MAX_ARGS - 1) { fail("too many arguments"); return EXIT_INVALID; } \
                    opts[nopts++] = (char *)(x); } while (0)
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--ui") == 0) {
            ui = 1;
            if (i + 1 < argc && is_port(argv[i + 1])) port = argv[++i];
        } else if (strcmp(a, "--no-open") == 0) {
            F.no_open = 1;
        } else if (strcmp(a, "--start") == 0) {
            start = 1;
        } else if (strcmp(a, "-q") == 0 || strcmp(a, "--quiet") == 0) {
            F.quiet = 1;
        } else if (strcmp(a, "-v") == 0 || strcmp(a, "--verbose") == 0) {
            verbose = 1;
        } else if (strcmp(a, "--trace") == 0) {
            NEED_VALUE();
            const char *k = argv[++i];
            if (strcmp(k, "radio") == 0 || strcmp(k, "packets") == 0) F.trace |= TRACE_RADIO;
            else if (strcmp(k, "boot") == 0) F.trace |= TRACE_BOOT;
            else if (strcmp(k, "stats") == 0) F.trace |= TRACE_STATS;
            else if (strcmp(k, "all") == 0) F.trace = TRACE_ALL;
            else {
                fail("run: --trace takes radio, boot, stats or all");
                return EXIT_INVALID;
            }
        } else if (strcmp(a, "--duration") == 0 || strcmp(a, "-t") == 0) {
            NEED_VALUE();
            int dr = parse_duration_ms(argv[++i], &duration_ms);
            if (dr == -2) {
                fail("run: %s %s: give the unit, e.g. %ss or %sms", a, argv[i],
                     argv[i], argv[i]);
                return EXIT_INVALID;
            }
            if (dr == -3) {
                fail("run: %s %s: a run simulates " DURATION_RANGE, a, argv[i]);
                return EXIT_INVALID;
            }
            if (dr != 0) {
                fail("run: bad duration '%s' (try 60s, 500ms, 2m)", argv[i]);
                return EXIT_INVALID;
            }
        } else if (strcmp(a, "--speed") == 0) {
            NEED_VALUE();
            have_speed = 1;
            OPT(a);
            OPT(argv[++i]);
        } else if (strcmp(a, "--seed") == 0 || strcmp(a, "--save-config") == 0 ||
                   strcmp(a, "--pcap") == 0 || strcmp(a, "--plugin") == 0 ||
                   strcmp(a, "--ui-record") == 0 || strcmp(a, "--ui-bind") == 0 ||
                   strcmp(a, "--wall-timeout") == 0 || strcmp(a, "-n") == 0 ||
                   strcmp(a, "--gdb") == 0 || strcmp(a, "--renode-freq") == 0) {
            NEED_VALUE();
            OPT(a);
            OPT(argv[++i]);
        } else if (strcmp(a, "--script") == 0) {
            NEED_VALUE();
            passthrough = 1;
            OPT(a);
            OPT(argv[++i]);
        } else if (strcmp(a, "--shell") == 0) {
            passthrough = 1;
            OPT(a);
        } else if (strcmp(a, "--renode") == 0) {
            OPT(a);           /* ADDR:MAIN:ASYNC, or bare: from CSIM_RENODE */
            if (i + 1 < argc && argv[i + 1][0] != '-' && strchr(argv[i + 1], ':') &&
                !is_file(argv[i + 1]))
                OPT(argv[++i]);
        } else if (strcmp(a, "--board") == 0) {
            NEED_VALUE();
            board = argv[++i];
        } else if (strcmp(a, "--paused") == 0 || strcmp(a, "--ui-edit") == 0 ||
                   strcmp(a, "--gdb-wait") == 0 || strcmp(a, "--realtime") == 0 ||
                   strcmp(a, "--idle-skip") == 0 || strcmp(a, "--no-idle-skip") == 0) {
            OPT(a);
        } else if (a[0] == '-') {
            fail("run: unknown option '%s' (cngsim help run)", a);
            return EXIT_INVALID;
        } else {
            if (ntargets >= 64) {
                fail("run: too many files");
                return EXIT_INVALID;
            }
            targets[ntargets++] = a;
        }
    }
    if (ntargets == 0) {
        fail("run: give a setup file, an example name (cngsim examples) or "
             "firmware images");
        return EXIT_INVALID;
    }

    /* One setup file, an example by name, or firmware images. */
    static char example[PATH_MAX + 64];
    static sim_normalized_config_t cfg;
    int have_cfg = 0;
    if (ntargets == 1 && (is_config_path(targets[0]) ||
                          (!is_file(targets[0]) &&
                           example_path(targets[0], example, sizeof(example)) == 0))) {
        const char *cfg_path = is_config_path(targets[0]) ? targets[0] : example;
        if (!is_file(cfg_path)) {
            fail("run: %s: no such file", cfg_path);
            return EXIT_INVALID;
        }
        if (sim_config_load(&cfg, cfg_path) != 0) return EXIT_INVALID;
        have_cfg = 1;
        if (board) {
            fail("run: --board is for firmware images on the command line; in a "
                 "setup file, put  board: %s  on the mote type", board);
            return EXIT_INVALID;
        }
        if (cfg.cwd_fallback[0])
            fprintf(stderr, "cngsim: note: %s is not next to %s, so it was taken "
                    "from the current directory; the setup works only when run from "
                    "here (name it relative to the setup file)\n",
                    cfg.cwd_fallback, cfg_path);
        if (cfg.bundled_fallback[0])
            fprintf(stderr, "cngsim: note: %s is not next to %s, so the bundled "
                    "firmware was used (as for the examples)\n",
                    cfg.bundled_fallback, cfg_path);
        PUSH(cfg_path);
        labels_from_config(&cfg);
        snprintf(F.title, sizeof(F.title), "%s", cfg.title);
        F.nodes = cfg.node_count;
        F.seed = cfg.seed;
        F.duration_s = cfg.timeout_ms / 1000.0;
    } else {
        for (int i = 0; i < ntargets; i++) {
            if (!is_file(targets[i])) {
                if (is_config_path(targets[i]) || !strchr(targets[i], '.'))
                    fail("run: '%s' is neither a file nor an example (cngsim examples)",
                         targets[i]);
                else
                    fail("run: %s: no such file", targets[i]);
                return EXIT_INVALID;
            }
            PUSH(targets[i]);
        }
        /* No --board, and an image whose name does not say: its build
         * directory may (a west build's .config, RIOT's bin/<BOARD>/).
         * That board is the image's own, --board FILE=NAME: another image
         * on the line may be named <app>.<board>, which a global --board
         * would contradict. */
        static char image_board_arg[64][PATH_MAX + 160];
        const char *image_board[64] = { 0 };
        for (int i = 0; !board && i < ntargets; i++) {
            if (sim_board_for_path(targets[i])) continue;
            char nm[128], from[PATH_MAX + 64];
            if (!board_from_build(targets[i], nm, sizeof(nm), from, sizeof(from))) continue;
            const sim_board_desc_t *fb = sim_board_find(nm);
            if (!fb || !fb->name) {
                char why[512];
                sim_board_unknown_error(nm, why, sizeof(why));
                fail("run: %s names board '%s': %s; give --board", from, nm, why);
                return EXIT_INVALID;
            }
            fprintf(stderr, "cngsim: note: board %s, from %s: runs as %s\n", nm, from,
                    fb->name);
            image_board[i] = fb->name;
            snprintf(image_board_arg[i], sizeof(image_board_arg[i]), "%s=%s", targets[i],
                     fb->name);
            PUSH("--board");
            PUSH(image_board_arg[i]);
        }
        if (board) {
            if (!sim_board_find(board)) {
                char why[512];
                sim_board_unknown_error(board, why, sizeof(why));
                fail("run: --board: %s", why);
                return EXIT_INVALID;
            }
            PUSH("--board");
            PUSH(board);
        }
        F.nodes = ntargets;
        F.duration_s = 20.0;
        /* Node i runs image i; with -n, the images repeat, which is not
         * worth guessing at: only one image, or one node per image, is
         * labelled. */
        int n_given = 0;
        for (int i = 0; i + 1 < nopts; i++)
            if (strcmp(opts[i], "-n") == 0) n_given = atoi(opts[i + 1]);
        for (int i = 0; i < ntargets && (ntargets == 1 || !n_given); i++)
            add_label(i + 1, targets[i], image_board[i] ? image_board[i] : board, "");
        if (ntargets == 1 && n_given)
            for (int k = 2; k <= n_given && k <= MAX_SIM_NODES; k++)
                add_label(k, targets[0], image_board[0] ? image_board[0] : board, "");
    }
    if (duration_ms >= 0) {
        F.duration_s = duration_ms / 1000.0;
        snprintf(duration_arg, sizeof(duration_arg), "%ld", duration_ms);
        PUSH("-t");
        PUSH(duration_arg);
    }
    for (int i = 0; i < nopts; i++) {
        if (strcmp(opts[i], "--seed") == 0 && i + 1 < nopts) F.seed = atoi(opts[i + 1]);
        if (strcmp(opts[i], "-n") == 0 && i + 1 < nopts) F.nodes = atoi(opts[i + 1]);
        PUSH(opts[i]);
    }

    if (ui) {
        PUSH("--ui");
        PUSH(port);
        if (!start) PUSH("--paused");
        /* Real time unless asked otherwise: the speed a person can follow. */
        if (!have_speed && !(have_cfg && cfg.speed > 0)) {
            PUSH("--speed");
            PUSH("1");
        }
        F.ui = 1;
        F.paused_ui = !start;
        F.enter_ok = !start && !passthrough && isatty(STDIN_FILENO);
        csim_cli_enter_resumes = F.enter_ok;
        csim_cli_pause_at_end = 1;
        csim_cli_hold_verdict = 1;
    }
    rargv[rargc] = NULL;
    if (!passthrough) {       /* the shell owns Ctrl-C in a session */
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_sigint;
        sigaction(SIGINT, &sa, NULL);
    }

    if (verbose || passthrough) {
        if (verbose && !passthrough) F.trace = TRACE_ALL;
        int vrc = run_mixed_multinode_test(rargc, rargv);
        if (!(csim_cli_stop && !csim_cli_run_complete)) return vrc;
        /* Unfiltered, the runner's own summary is the verdict, but an
         * interruption still says so, as the filtered run's last line does. */
        double sim_s = (double)csim_cli_simulated_ns / 1e9;
        printf(sim_s < 1.0 ? "INTERRUPTED · %.3f s simulated\n"
                           : "INTERRUPTED · %.1f s simulated\n", sim_s);
        return EXIT_INTERRUPTED;
    }

    /* The simulation's stdout and stderr go through the filter threads. */
    int po[2], pe[2];
    if (pipe(po) != 0 || pipe(pe) != 0) {
        perror("cngsim: pipe");
        return run_mixed_multinode_test(rargc, rargv);
    }
    fflush(stdout);
    fflush(stderr);
    int saved_out = dup(STDOUT_FILENO), saved_err = dup(STDERR_FILENO);
    dup2(po[1], STDOUT_FILENO);
    dup2(pe[1], STDERR_FILENO);
    close(po[1]);
    close(pe[1]);
    filt_arg_t fo = { po[0], saved_out, 1 }, fe = { pe[0], saved_err, 0 };
    pthread_t to, te;
    pthread_create(&to, NULL, filter_thread, &fo);
    pthread_create(&te, NULL, filter_thread, &fe);

    int rc = run_mixed_multinode_test(rargc, rargv);

    fflush(stdout);
    fflush(stderr);
    dup2(saved_out, STDOUT_FILENO);       /* closes the pipes' last write ends */
    dup2(saved_err, STDERR_FILENO);
    pthread_join(to, NULL);
    pthread_join(te, NULL);
    close(saved_out);
    close(saved_err);

    if (rc != 0 && !F.verdict[0] && err_count > 0) {
        /* Failed without a verdict: the held-back stderr may say why. */
        int from = err_count > ERR_KEEP ? err_count - ERR_KEEP : 0;
        fprintf(stderr, "(the last lines the simulator wrote to stderr:)\n");
        for (int k = from; k < err_count; k++)
            fprintf(stderr, "  %s\n", err_ring[k % ERR_KEEP]);
    }
    if (rc == EXIT_INVALID && !F.running) return rc;    /* the error said it all */
    /* Ctrl-C is the user ending the run, not a check failing: its own word
     * and exit code, whatever the unfinished checks reported.  (7 stays the
     * code for a cancellation through the protocol.) */
    int interrupted = csim_cli_stop && !csim_cli_run_complete;
    if (interrupted) rc = EXIT_INTERRUPTED;
    const char *word = interrupted ? "INTERRUPTED"
                     : F.verdict[0] ? F.verdict
                     : rc == 0 ? "DONE" : rc == 1 ? "FAILED"
                     : rc == 6 ? "WALL TIMEOUT" : rc == 7 ? "CANCELLED" : "ERROR";
    char summary[512];
    int n = snprintf(summary, sizeof(summary), "%s", word);
    if (!interrupted && F.verdict[0] && strcmp(F.verdict, "FAILED") == 0 && F.fail_reason[0])
        n += snprintf(summary + n, sizeof(summary) - (size_t)n, ": %s", F.fail_reason);
    /* How far the simulation got (the runner's "Simulated time" line is the
     * requested duration), and the wall time it ran, not the time it sat
     * paused waiting for Play. */
    double sim_s = F.have_test_end ? F.test_end_s : (double)csim_cli_simulated_ns / 1e9;
    double wall_s = F.wall_s - csim_cli_paused_ms / 1000.0;
    if (wall_s < 0) wall_s = 0;
    if ((F.have_test_end || F.have_sim) && F.have_wall) {
        n += snprintf(summary + n, sizeof(summary) - (size_t)n,
                      sim_s < 1.0 ? " · %.3f s simulated in %.2f s"
                                  : " · %.1f s simulated in %.2f s", sim_s, wall_s);
        if (csim_cli_paused_ms >= 1000.0)
            n += snprintf(summary + n, sizeof(summary) - (size_t)n, ", %.0f s paused",
                          csim_cli_paused_ms / 1000.0);
        /* One decimal below 10x: an OpenThread network runs slower than
         * real time, and "%.0f" made 0.4x read "0x". */
        if (wall_s > 0 && sim_s > 0) {
            double x = sim_s / wall_s;
            snprintf(summary + n, sizeof(summary) - (size_t)n,
                     x < 9.95 ? " (%.1fx real time)" : " (%.0fx real time)", x);
        }
    }
    printf("%s\n", summary);
    return rc;
}

/* ---- check ------------------------------------------------------------- */

/* A node id the setup has: configured, or created by an "add" action. */
static int node_known(const sim_normalized_config_t *cfg, int id) {
    for (int i = 0; i < cfg->node_count; i++)
        if (cfg->nodes[i].id == id) return 1;
    for (int k = 0; cfg->has_test && k < cfg->test.action_count; k++)
        if (cfg->test.actions[k].type == TEST_ACTION_ADD && cfg->test.actions[k].node == id)
            return 1;
    return 0;
}

static int cmd_check(int argc, char **argv) {
    if (argc != 1) {
        fail("check: give one setup file");
        return EXIT_INVALID;
    }
    char ex[PATH_MAX + 64];
    const char *path = argv[0];
    if (!is_file(path) && example_path(path, ex, sizeof(ex)) == 0) path = ex;
    static sim_normalized_config_t cfg;
    if (sim_config_load(&cfg, path) != 0) {
        printf("%s: INVALID\n", argv[0]);
        return EXIT_INVALID;
    }
    int bad = 0;
    for (int t = 0; t < cfg.mote_type_count; t++) {
        const sim_mote_type_t *mt = &cfg.mote_types[t];
        const char *fw = mt->firmware[0] ? mt->firmware : cfg.mote_type_firmware[t];
        if (!fw[0]) continue;
        char err[512];
        const sim_board_desc_t *b = sim_board_resolve(fw, mt->board, err, sizeof(err));
        if (!b) {
            fprintf(stderr, "%s: %s\n", argv[0], err);
            bad++;
        } else if (b->kind != SIM_BOARD_KIND_RENODE && access(fw, R_OK) != 0) {
            fprintf(stderr, "%s: mote type %s: cannot read %s\n", argv[0],
                    mt->name[0] ? mt->name : "?", fw);
            bad++;
        }
        if (mt->secure_firmware[0] && access(mt->secure_firmware, R_OK) != 0) {
            fprintf(stderr, "%s: cannot read %s\n", argv[0], mt->secure_firmware);
            bad++;
        }
    }
    if (cfg.version < 2) {   /* v1: each node names its firmware, mote_types or not */
        for (int i = 0; i < cfg.node_count; i++) {
            const sim_node_config_t *nd = &cfg.nodes[i];
            char err[512];
            const sim_board_desc_t *b = sim_board_resolve(nd->firmware, NULL, err, sizeof(err));
            if (!b) {
                fprintf(stderr, "%s: %s\n", argv[0], err);
                bad++;
            } else if (b->kind != SIM_BOARD_KIND_RENODE && access(nd->firmware, R_OK) != 0) {
                fprintf(stderr, "%s: node %d: cannot read %s\n", argv[0], nd->id,
                        nd->firmware);
                bad++;
            }
            if (nd->secure_firmware[0] && access(nd->secure_firmware, R_OK) != 0) {
                fprintf(stderr, "%s: node %d: cannot read %s\n", argv[0], nd->id,
                        nd->secure_firmware);
                bad++;
            }
        }
    }
    /* A check or an action that names a node the setup does not have would
     * wait for, or act on, nothing.  Nodes an "add" action creates count. */
    if (cfg.has_test) {
        const sim_test_config_t *tc = &cfg.test;
        int auto_add = 0;
        for (int k = 0; k < tc->action_count; k++)
            if (tc->actions[k].type == TEST_ACTION_ADD && tc->actions[k].node <= 0)
                auto_add = 1;          /* its id is chosen at run time */
#define CHECK_NODE(what, idx, id) do { \
            if ((id) > 0 && !auto_add && !node_known(&cfg, (id))) { \
                fprintf(stderr, "%s: test %s %d names node %d, which the setup " \
                        "does not have\n", argv[0], (what), (idx) + 1, (id)); \
                bad++; \
            } } while (0)
        for (int k = 0; k < tc->step_count; k++) CHECK_NODE("step", k, tc->steps[k].node);
        for (int k = 0; k < tc->validator_count; k++)
            CHECK_NODE("validator", k, tc->validators[k].node);
        for (int k = 0; k < tc->action_count; k++)
            if (tc->actions[k].type != TEST_ACTION_ADD &&
                tc->actions[k].type != TEST_ACTION_SEND_ALL)
                CHECK_NODE("action", k, tc->actions[k].node);
#undef CHECK_NODE
    }
    if (cfg.cwd_fallback[0])
        fprintf(stderr, "%s: note: %s is not next to the setup, so it was taken from "
                "the current directory; the setup works only when run from here\n",
                argv[0], cfg.cwd_fallback);
    if (cfg.bundled_fallback[0])
        fprintf(stderr, "%s: note: %s is not next to the setup, so the bundled "
                "firmware is used (as for the examples)\n", argv[0], cfg.bundled_fallback);
    /* Plugins as the runner resolves them: a path must be readable, a bare
     * or "builtin:" name must be a compiled-in service. */
    static sim_registry_t reg;
    if (!reg.service_count) csim_register_builtin_services(&reg);
    for (int p = 0; p < cfg.plugin_count; p++) {
        const char *entry = cfg.plugins[p];
        size_t len = strlen(entry);
        if (!len) continue;
        if (strchr(entry, '/') || (len > 3 && strcmp(entry + len - 3, ".so") == 0)) {
            if (access(entry, R_OK) != 0) {
                fprintf(stderr, "%s: cannot read plugin %s\n", argv[0], entry);
                bad++;
            }
        } else {
            const char *name = strncmp(entry, "builtin:", 8) == 0 ? entry + 8 : entry;
            if (!sim_registry_find_service(&reg, name)) {
                fprintf(stderr, "%s: unknown plugin '%s' (not a built-in service, "
                        "and not a path to a .so)\n", argv[0], name);
                bad++;
            }
        }
    }
    if (cfg.has_js_script && cfg.js_script_path[0] && access(cfg.js_script_path, R_OK) != 0) {
        fprintf(stderr, "%s: cannot read js_script %s\n", argv[0], cfg.js_script_path);
        bad++;
    }
    if (bad) {
        printf("%s: INVALID\n", argv[0]);
        sim_config_free(&cfg);
        return EXIT_INVALID;
    }
    printf("%s: OK · %d node%s, %d mote type%s, %g s", argv[0], cfg.node_count,
           cfg.node_count == 1 ? "" : "s", cfg.mote_type_count,
           cfg.mote_type_count == 1 ? "" : "s", cfg.timeout_ms / 1000.0);
    if (cfg.has_test)
        printf(", %d check%s", cfg.test.step_count + cfg.test.validator_count,
               cfg.test.step_count + cfg.test.validator_count == 1 ? "" : "s");
    else if (cfg.has_js_script)
        printf(", a JavaScript test");
    printf("\n");
    for (int t = 0; t < cfg.mote_type_count; t++) {
        const sim_mote_type_t *mt = &cfg.mote_types[t];
        /* a v1 config's mote types are only firmware, in the index table */
        const char *fw = mt->firmware[0] ? mt->firmware : cfg.mote_type_firmware[t];
        char err[512], name[64];
        snprintf(name, sizeof(name), "%s", mt->name[0] ? mt->name : "");
        if (!name[0]) snprintf(name, sizeof(name), "type %d", t);
        const sim_board_desc_t *b = sim_board_resolve(fw, mt->board, err, sizeof(err));
        int users = 0;
        for (int i = 0; i < cfg.node_count; i++)
            if (mt->name[0] ? strcmp(cfg.nodes[i].type_name, mt->name) == 0
                            : strcmp(cfg.nodes[i].firmware, fw) == 0) users++;
        printf("  %-12s %-20s %2d node%s  %s\n", name,
               b ? (b->name ? b->name : b->label) : "?", users, users == 1 ? " " : "s", fw);
    }
    sim_config_free(&cfg);
    return 0;
}

/* ---- new --------------------------------------------------------------- */

struct new_spec;

typedef struct template {
    const char *name, *blurb;
    const char *roles[2], *images[2];
    const char *expect;                   /* console line the check waits for */
    int expect_radio;                     /* the check needs a radio path */
    int max_n;                            /* most nodes it works with, 0 = any */
    int check_node1;                      /* the check waits on node 1, not the farthest */
    int expect_others;                    /* ... for `expect` once from each of nodes 2..n */
    const char *fail_on, *fail_on_why;    /* a line that fails the run at once, or NULL */
    /* The console input it types at the nodes (OpenThread's dataset, RIOT's
     * RPL set-up), as test actions after the steps, and when the last of
     * them is sent (sp->hops deep, sp->n nodes); NULL for none. */
    void (*actions)(FILE *f, const struct new_spec *sp, int far);
    long (*last_ms)(const struct new_spec *sp);
    /* A network that needs time per hop before its check can pass: without
     * --duration, at least base_s + hop_s a hop (and never less than the
     * default); with one that is shorter, a warning.  0 = none. */
    int base_s, hop_s;
    const char *why_max;                  /* why max_n, for the refusal */
    const char *board;                    /* default --board: one it has images for */
    const char *note;                     /* printed after "Wrote", or NULL */
    int idle_skip;                        /* the setup turns on idle_skip (its
                                           * firmware polls; docs/idle-skip.md) */
    /* Whether the setup can be written for a board that has the images
     * (riot-rpl: its radio's interface number is known), and why not. */
    int (*board_ok)(const char *board);
    const char *why_board;
} template_t;

static void thread_actions(FILE *f, const struct new_spec *sp, int far);
/* The mesh-local prefix every node takes (OpenThread's default, set in the
 * dataset so the address below is plain to read), node 1's own address in
 * it, and that address as ot-cli-ftd's ping prints it.  THREAD_FIXED_ACTIONS
 * counts thread_actions' actions other than the two each of nodes 2..n has
 * (its `thread start` and its ping). */
#define THREAD_ML_PREFIX "fdde:ad00:beef:0::"
#define THREAD_NODE1 "fdde:ad00:beef:0::1"
#define THREAD_NODE1_PRINTED "fdde:ad00:beef:0:0:0:0:1"
#define THREAD_FIXED_ACTIONS 13
#define STR(x) #x
#define XSTR(x) STR(x)
static long thread_last_ms(const struct new_spec *sp);
static void riot_actions(FILE *f, const struct new_spec *sp, int far);
static long riot_last_ms(const struct new_spec *sp);
static int riot_board_ok(const char *board);

static const template_t templates[] = {
    /* The bundled udp-server is stock Contiki-NG: its routing table holds
     * 16 entries (NETSTACK_MAX_ROUTE_ENTRIES), and a 16th client is never
     * answered -- measured on Sky, CC2538 and nRF52840 images alike.  RPL
     * needs time per hop before the farthest client is answered: measured
     * on the bundled Sky images, a line of 7 hops passes at 119 s and one
     * of 15 at 250 s, so 60 s + 20 s a hop. */
    { .name = "rpl-udp",
      .blurb = "an RPL network: node 1 is the UDP server and the RPL root, the "
               "others are clients that send it requests",
      .roles = { "server", "client" }, .images = { "udp-server", "udp-client" },
      .expect = "Received response", .expect_radio = 1, .max_n = 16,
      .base_s = 60, .hop_s = 20,
      .why_max = "the bundled udp-server answers 15 clients, as Contiki-NG's routing "
                 "table holds 16 routes; for a larger network build the server with a "
                 "larger NETSTACK_MAX_ROUTE_ENTRIES",
      .board = "sky" },
    { .name = "nullnet",
      .blurb = "broadcast without IP: every node broadcasts and prints what it receives",
      .roles = { "node", NULL }, .images = { "nullnet-broadcast", NULL },
      .expect = "Received", .expect_radio = 1, .board = "sky" },
    { .name = "hello", .blurb = "hello world on every node",
      .roles = { "node", NULL }, .images = { "hello-world", NULL },
      .expect = "Hello, world", .board = "sky" },
    /* One network, not a reply: every other node pings an address only
     * node 1 has, so a node in a partition of its own (out of range, or
     * never attached) is not answered and the count falls short.  A ping
     * to ff03::1 was answered by any neighbour, and the leader ALOC by
     * each partition's own leader. */
    { .name = "thread",
      .blurb = "an OpenThread network: every node runs ot-cli-ftd and takes the same "
               "dataset at its console; node 1 starts first, leads and takes the "
               "address " THREAD_NODE1 ", the others attach a hop at a time and each "
               "pings that address: a reply on every one shows they are one network",
      .roles = { "ot", NULL }, .images = { "ot-cli-ftd", NULL },
      .expect = "bytes from " THREAD_NODE1_PRINTED, .expect_radio = 1, .expect_others = 1,
      .fail_on = "detached", .fail_on_why = "a node's `state` when it is in no network",
      .max_n = (MAX_TEST_ACTIONS - THREAD_FIXED_ACTIONS) / 2 + 1,
      .why_max = "its setup types `thread start` and a ping at each node as timed "
                 "actions of their own, and a setup holds at most " XSTR(MAX_TEST_ACTIONS),
      .actions = thread_actions, .last_ms = thread_last_ms, .board = "cc2538dk",
      /* Measured with cngsim new thread -n 6 --layout line, 67 s simulated,
       * on an M-series Mac: CC2538 14.6 s with idle_skip (88 s without),
       * nRF52840 DK 145 s either way (tools/check-idle-skip.sh, TIMING=1);
       * without idle_skip the CC2538 line ran at 0.2x on a 4-core x86-64
       * Linux box (gcc, Lightning). */
      .note = "OpenThread's main loop never sleeps; the setup turns on idle_skip,\n"
              "  which skips its idle passes and changes nothing else (docs/idle-skip.md).\n"
              "  How fast it runs depends on the host: on an M-series Mac, 6 nodes run\n"
              "  at about 4.6x real time on CC2538s (0.8x without idle_skip; 0.2x on a\n"
              "  4-core x86-64 Linux box) and 0.5x on nRF52840 DKs, whose loop it\n"
              "  cannot skip yet",
      .idle_skip = 1 },
    { .name = "zephyr-echo",
      .blurb = "Zephyr's echo_server and echo_client samples: node 1 echoes the UDP "
               "datagrams node 2 sends it",
      .roles = { "server", "client" }, .images = { "zephyr-echo-server", "zephyr-echo-client" },
      .expect = "Received and replied", .expect_radio = 1, .max_n = 2, .check_node1 = 1,
      .why_max = "the stock samples have fixed addresses, 2001:db8::1 and ::2, and no routing",
      .board = "nrf52840-dk" },
    { .name = "riot-rpl",
      .blurb = "a RIOT RPL network: every node runs RIOT's gnrc_networking; node 1 "
               "takes 2001:db8::1 and roots a DODAG from its shell, the others join, and "
               "the farthest node pings node 1",
      .roles = { "riot", NULL }, .images = { "riot-gnrc-networking", NULL },
      /* The whole line: steps match a substring, and "0% packet loss"
       * is also in the "100% packet loss" of a ping nobody answered. */
      .expect = "3 packets transmitted, 3 packets received, 0% packet loss",
      .expect_radio = 1, .actions = riot_actions, .last_ms = riot_last_ms,
      .board = "nrf52840-dk", .board_ok = riot_board_ok,
      .why_board = "its shell commands name the radio's interface number, which is the "
                   "build's own; add the board's to riot_ifaces[] in src/cli/cngsim.c" },
};

#define NTEMPLATES (sizeof(templates) / sizeof(templates[0]))

/* The templates' names, `sep` between them. */
static const char *template_names(const char *sep) {
    static char buf[256];
    buf[0] = '\0';
    for (size_t i = 0; i < NTEMPLATES; i++) {
        if (i) strncat(buf, sep, sizeof(buf) - strlen(buf) - 1);
        strncat(buf, templates[i].name, sizeof(buf) - strlen(buf) - 1);
    }
    return buf;
}

static const template_t *find_template(const char *name) {
    for (size_t i = 0; i < NTEMPLATES; i++)
        if (strcmp(templates[i].name, name) == 0) return &templates[i];
    return NULL;
}

/* The bundled image for one role of a template on a board, in `out`: the
 * board's UART-console build when it has one (the dongle's plain hello-world
 * prints over native USB, which the simulator does not model).  Returns 0 if
 * the board has no usable image. */
static int template_image(const char *data, const char *board, const template_t *tp,
                          int r, char *out, size_t outsz) {
    snprintf(out, outsz, "%s/firmware/%s/%s-uart.%s", data, board, tp->images[r], board);
    if (is_file(out)) return 1;
    snprintf(out, outsz, "%s/firmware/%s/%s.%s", data, board, tp->images[r], board);
    return is_file(out);
}

/* The boards a template comes with: every role's image bundled for it, and
 * a setup it can write there; its default first, ", " between them. */
static const char *template_boards(const char *data, const template_t *tp) {
    static char buf[512];
    buf[0] = '\0';
    int end = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; data && !(end = 0); i++) {
            const sim_board_desc_t *b = board_row(i, &end);
            if (end) break;
            if (!b || (strcmp(b->name, tp->board) == 0) != (pass == 0)) continue;
            int all = 1;
            for (int r = 0; r < 2 && tp->roles[r]; r++) {
                char p[PATH_MAX + 256];
                if (!template_image(data, b->name, tp, r, p, sizeof(p))) all = 0;
            }
            if (!all || (tp->board_ok && !tp->board_ok(b->name))) continue;
            if (buf[0]) strncat(buf, ", ", sizeof(buf) - strlen(buf) - 1);
            strncat(buf, b->name, sizeof(buf) - strlen(buf) - 1);
        }
    return buf;
}

/* What `new` writes: the template, the board's bundled images and the
 * layout, all decided before the file is written. */
typedef struct new_spec {
    const template_t *tp;
    const char *board, *layout, *outp;
    char (*fw)[PATH_MAX];
    int n, clients, columns, seed, hops;
    int hop_of[MAX_SIM_NODES + 1];      /* by node id: radio hops from node 1 */
    long duration_ms;
    double spacing, range, radius, side;
} new_spec_t;

/* OpenThread: the dataset every node takes (as thread-cc2538 types it), a
 * one-second router selection jitter so a node out of node 1's range finds
 * a router to attach to within seconds (OpenThread's default is 120 s),
 * then node 1 starts and leads, and the others start a hop at a time,
 * 10 s apart, each once a router is in its range: a node that starts with
 * none forms a partition of its own, and partitions take minutes to merge.
 * Then nodes 2..n ping node 1, one at a time, 500 ms apart: pinged at once,
 * their address queries collided and some nodes were never answered (16 on
 * a grid: 9 of 16; even 4 nRF52840s: 3 of 4). */
#define THREAD_PING_GAP_MS 500L
static long thread_start_ms(int hop) { return 12000L + 10000L * (hop > 1 ? hop - 1 : 0); }
static long thread_ping_ms(int hops) { return thread_start_ms(hops > 0 ? hops : 1) + 8000L; }
static long thread_last_ms(const new_spec_t *sp) {
    return thread_ping_ms(sp->hops) + THREAD_PING_GAP_MS * (sp->n > 2 ? sp->n - 2 : 0);
}

static void thread_actions(FILE *f, const new_spec_t *sp, int far) {
    static const char *const dataset[] = {
        "dataset networkkey 00112233445566778899aabbccddeeff", "dataset channel 15",
        "dataset panid 0x1234", "dataset extpanid dead00beef00cafe",
        "dataset networkname cooja-ng", ("dataset meshlocalprefix " THREAD_ML_PREFIX),
        "dataset activetimestamp 1", "dataset commit active", "routerselectionjitter 1",
        "ifconfig up",
    };
    (void)far;
    /* From 2 s: every node has booted (startup_delay_ms: 1000) and its
     * CLI is up; a line sent before is lost, and a node that misses the
     * network key forms a partition of its own. */
    fprintf(f, "  # What you would type at each ot-cli-ftd, at these times.\n  actions:\n");
    int t = 2000;
    for (size_t i = 0; i < sizeof(dataset) / sizeof(dataset[0]); i++, t += 100)
        fprintf(f, "    - { at_ms: %d, type: send_all, data: \"%s\\n\" }\n", t, dataset[i]);
    fprintf(f, "    - { at_ms: %d, type: send, node: 1, data: \"thread start\\n\" }\n", t);
    fprintf(f, "    - { at_ms: %d, type: send, node: 1, data: \"ipaddr add " THREAD_NODE1 "\\n\" }\n",
            t + 100);
    for (int h = 1; h <= (sp->hops > 0 ? sp->hops : 1); h++)
        for (int id = 2; id <= sp->n && id <= MAX_SIM_NODES; id++)
            if ((sp->hop_of[id] > 0 ? sp->hop_of[id] : 1) == h)
                fprintf(f, "    - { at_ms: %ld, type: send, node: %d, data: \"thread start\\n\" }\n",
                        thread_start_ms(h), id);
    long ping = thread_ping_ms(sp->hops);
    fprintf(f, "    - { at_ms: %ld, type: send_all, data: \"state\\n\" }\n", ping - 500);
    for (int id = 2; id <= sp->n && id <= MAX_SIM_NODES; id++)
        fprintf(f, "    - { at_ms: %ld, type: send, node: %d, data: \"ping " THREAD_NODE1 "\\n\" }\n",
                ping + THREAD_PING_GAP_MS * (id - 2), id);
}

/* RIOT: the radio's interface number, by board, which `ifconfig` lists
 * ("Iface  5  HWaddr: ...  PHY: O-QPSK" in the bundled nRF52840 DK build).
 * RIOT numbers interfaces by the build's network devices, so another
 * board's riot-gnrc-networking image, when one is bundled, needs its own
 * row here rather than this one's 5.  RIOT's default PAN ID is the same
 * on every node.  From 2 s, after every node has booted. */
static const struct { const char *board; int iface; } riot_ifaces[] = {
    { "nrf52840-dk", 5 },
};

static int riot_iface(const char *board) {
    for (size_t i = 0; i < sizeof(riot_ifaces) / sizeof(riot_ifaces[0]); i++)
        if (strcmp(riot_ifaces[i].board, board) == 0) return riot_ifaces[i].iface;
    return -1;
}

static int riot_board_ok(const char *board) { return riot_iface(board) >= 0; }

static long riot_last_ms(const new_spec_t *sp) {
    return 10000L + 5000L * (sp->hops > 0 ? sp->hops : 1);
}

static void riot_actions(FILE *f, const new_spec_t *sp, int far) {
    int ifc = riot_iface(sp->board);       /* cmd_new refused a board without one */
    fprintf(f, "  # What you would type at each RIOT shell, at these times; interface %d\n"
               "  # is the radio in this board's build.\n  actions:\n", ifc);
    fprintf(f, "    - { at_ms: 2000, type: send, node: 1, data: \"ifconfig %d add 2001:db8::1/64\\n\" }\n", ifc);
    fprintf(f, "    - { at_ms: 2200, type: send_all, data: \"rpl init %d\\n\" }\n", ifc);
    fprintf(f, "    - { at_ms: 2400, type: send, node: 1, data: \"rpl root 1 2001:db8::1\\n\" }\n");
    long ping = riot_last_ms(sp);
    fprintf(f, "    - { at_ms: %ld, type: send, node: %d, data: \"rpl\\n\" }\n", ping - 1000, far);
    fprintf(f, "    - { at_ms: %ld, type: send, node: %d, data: \"ping -c 3 -i 500 2001:db8::1\\n\" }\n",
            ping, far);
}

/* A YAML double-quoted scalar: any path survives (": ", " #", quotes). */
static void put_quoted(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') fputc('\\', f);
        fputc(*s, f);
    }
    fputc('"', f);
}

/* The whole setup file; with `far` > 0 also its check, which waits on that
 * node.  Returns 0, or -1 if the file cannot be written. */
static int write_setup(const char *path, const new_spec_t *sp, int far) {
    const template_t *tp = sp->tp;
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "# %c%s.\n", toupper((unsigned char)tp->blurb[0]), tp->blurb + 1);
    fprintf(f, "# Written by: cngsim new %s --board %s -n %d --layout %s\n", tp->name,
            sp->board, sp->n, sp->layout);
    fprintf(f, "# Run it:   cngsim run %s\n# Watch it: cngsim run %s --ui\n\n", sp->outp,
            sp->outp);
    char kv[128];
#define KV(comment, ...) do { snprintf(kv, sizeof(kv), __VA_ARGS__); \
        fprintf(f, "%-26s# %s\n", kv, comment); } while (0)
    fprintf(f, "version: 2\n");
    fprintf(f, "title: \"%s on %s, %d node%s (%s)\"\n", tp->name, sp->board, sp->n,
            sp->n == 1 ? "" : "s", sp->layout);
    KV("how long to simulate (simulated time, ms)", "timeout_ms: %ld", sp->duration_ms);
    KV("the same seed gives the same run; change it for another", "seed: %d", sp->seed);
    KV("nodes boot at random times within the first second", "startup_delay_ms: 1000");
    if (tp->idle_skip)
        KV("skip proven idle main-loop passes: same run, faster", "idle_skip: true");
    fprintf(f, "\nmedium:\n");
    KV("unit-disk graph, as in Cooja: within range means received", "  type: udgm");
    KV("radio range, metres", "  tx_range: %g", sp->range);
    KV("a sender this close disturbs reception", "  interference_range: %g", sp->range * 2);
#undef KV
    fprintf(f, "\n");
    fprintf(f, "# The firmware the nodes run, one entry per image: here the images that\n");
    fprintf(f, "# come with cooja-ng.  Your own can be named relative to this file.  An\n");
    fprintf(f, "# image not named <app>.<board> (an .elf, .nrf, .simplelink) also needs\n");
    fprintf(f, "# board: <name>  (cngsim boards lists them).\n");
    fprintf(f, "mote_types:\n");
    for (int r = 0; r < 2 && tp->roles[r]; r++) {
        fprintf(f, "  - { name: %s, firmware: ", tp->roles[r]);
        put_quoted(f, sp->fw[r]);
        fprintf(f, " }\n");
    }
    fprintf(f, "\n# Positions in metres.  One entry can place many nodes: count + layout\n");
    fprintf(f, "# (grid: columns, spacing; line: spacing; circle: radius; random: width,\n");
    fprintf(f, "# height), starting at x, y.  Or list nodes one by one with an id, x and y.\n");
    fprintf(f, "nodes:\n");
    const char *role = tp->roles[1] ? tp->roles[1] : tp->roles[0];
    double x0 = 0;
    int first_id = 1;
    if (tp->roles[1]) {
        fprintf(f, "  - { type: %s, id: 1, x: 0, y: 0 }\n", tp->roles[0]);
        x0 = sp->spacing;
        first_id = 2;
    }
    if (strcmp(sp->layout, "grid") == 0)
        fprintf(f, "  - { type: %s, id: %d, count: %d, layout: grid, columns: %d, spacing: %g, x: %g, y: 0 }\n",
                role, first_id, sp->clients, sp->columns, sp->spacing, x0);
    else if (strcmp(sp->layout, "line") == 0)
        fprintf(f, "  - { type: %s, id: %d, count: %d, layout: line, spacing: %g, x: %g, y: 0 }\n",
                role, first_id, sp->clients, sp->spacing, x0);
    else if (strcmp(sp->layout, "circle") == 0)
        fprintf(f, "  - { type: %s, id: %d, count: %d, layout: circle, radius: %g, x: 0, y: 0 }\n",
                role, first_id, sp->clients, floor(sp->radius * 10) / 10);
    else
        fprintf(f, "  - { type: %s, id: %d, count: %d, layout: random, width: %g, height: %g, x: 0, y: 0 }\n",
                role, first_id, sp->clients, sp->side, sp->side);
    if (far > 0) {
        fprintf(f, "\n# Checks.  The run passes when every step is seen, in order, and exits\n");
        fprintf(f, "# with 0; otherwise with 1.  test-format.md has more (validators, fail_on,\n");
        fprintf(f, "# Cooja JavaScript test scripts).\n");
        fprintf(f, "test:\n  steps:\n");
        if (tp->check_node1)
            fprintf(f, "    - { wait: \"%s\", node: 1 }\n", tp->expect);
        else if (tp->expect_others)
            fprintf(f, "    - { wait: \"%s\", count: %d }   # on nodes 2..%d: one network\n",
                    tp->expect, sp->n - 1, sp->n);
        else
            fprintf(f, "    - { wait: \"%s\", node: %d }%s\n", tp->expect, far,
                    tp->roles[1] ? "   # the farthest client got an answer" :
                    tp->actions ? "   # the farthest node's ping was answered" : "");
        if (tp->fail_on)
            fprintf(f, "  fail_on: [\"%s\"]   # %s\n", tp->fail_on, tp->fail_on_why);
        if (tp->actions) tp->actions(f, sp, far);
    }
    int bad = ferror(f);
    if (fclose(f) != 0) bad = 1;
    return bad ? -1 : 0;
}

/* The most radio hops (links shorter than `range`) any node is from the
 * first node, node 1, or -1 when some node has no path to it at all.  Each
 * node's own count goes into hop_of[id] when it is given. */
static int hops_to_node1(const sim_normalized_config_t *cfg, double range, int *hop_of) {
    int n = cfg->node_count, hops[MAX_SIM_NODES], queue[MAX_SIM_NODES];
    int head = 0, tail = 0, count = 1, deepest = 0;
    for (int i = 0; i < n; i++) hops[i] = -1;
    hops[0] = 0;
    queue[tail++] = 0;
    while (head < tail) {
        int a = queue[head++];
        for (int b = 0; b < n; b++) {
            if (hops[b] >= 0) continue;
            double dx = cfg->nodes[a].x - cfg->nodes[b].x, dy = cfg->nodes[a].y - cfg->nodes[b].y;
            if (dx * dx + dy * dy < range * range) {
                hops[b] = hops[a] + 1;
                if (hops[b] > deepest) deepest = hops[b];
                queue[tail++] = b;
                count++;
            }
        }
    }
    for (int i = 0; hop_of && i < n; i++)
        if (cfg->nodes[i].id > 0 && cfg->nodes[i].id <= MAX_SIM_NODES)
            hop_of[cfg->nodes[i].id] = hops[i];
    return count == n ? deepest : -1;
}

/* The id of the node farthest from node 1: the one the check waits on. */
static int farthest_from_node1(const sim_normalized_config_t *cfg, int skip_first) {
    int far = cfg->nodes[cfg->node_count - 1].id;
    double best = -1;
    for (int k = skip_first ? 1 : 0; k < cfg->node_count; k++) {
        double dx = cfg->nodes[k].x - cfg->nodes[0].x, dy = cfg->nodes[k].y - cfg->nodes[0].y;
        double d = dx * dx + dy * dy;
        if (d > best) { best = d; far = cfg->nodes[k].id; }
    }
    return far;
}

/* Ask, and take the answer into `out`; an empty answer keeps `dflt`.
 * `dflt` may be `out` itself (the callers pass the current value), so it is
 * copied before fgets overwrites it. */
static void prompt(const char *q, const char *dflt, char *out, size_t n) {
    char keep[PATH_MAX];
    snprintf(keep, sizeof(keep), "%s", dflt);
    printf("%s [%s]: ", q, keep);
    fflush(stdout);
    if (!fgets(out, (int)n, stdin)) out[0] = '\0';
    out[strcspn(out, "\r\n")] = '\0';
    char *s = out;
    while (*s == ' ' || *s == '\t') s++;
    memmove(out, s, strlen(s) + 1);
    size_t len = strlen(out);
    while (len > 0 && (out[len - 1] == ' ' || out[len - 1] == '\t')) out[--len] = '\0';
    if (!out[0]) snprintf(out, n, "%s", keep);
}

static int cmd_new(int argc, char **argv) {
    char tname[64] = "rpl-udp", board[64] = "", layout[16] = "grid";
    char outp[PATH_MAX] = "sim.yaml", nbuf[16] = "4";
    int n = 4, seed = 42, force = 0, asked = 0;
    double spacing = 30.0;
    long duration_ms = 120000;
    int duration_given = 0, n_given = 0;
    int i = 0;
    if (argc > 0 && argv[0][0] != '-') snprintf(tname, sizeof(tname), "%s", argv[i++]);
    for (; i < argc; i++) {
        const char *a = argv[i];
        int more = i + 1 < argc;
        if (strcmp(a, "--force") == 0) { force = 1; continue; }
        if (!more) {
            fail("new: %s needs a value (cngsim help new)", a);
            return EXIT_INVALID;
        }
        const char *v = argv[++i];
        if (strcmp(a, "--board") == 0) snprintf(board, sizeof(board), "%s", v);   /* "" if empty: the default */
        else if (strcmp(a, "-n") == 0) { n = atoi(v); n_given = 1; }
        else if (strcmp(a, "--layout") == 0) snprintf(layout, sizeof(layout), "%s", v);
        else if (strcmp(a, "--spacing") == 0) spacing = atof(v);
        else if (strcmp(a, "-o") == 0) snprintf(outp, sizeof(outp), "%s", v);
        else if (strcmp(a, "--seed") == 0) seed = atoi(v);
        else if (strcmp(a, "--duration") == 0) {
            int dr = parse_duration_ms(v, &duration_ms);
            if (dr == -2) {
                fail("new: --duration %s: give the unit, e.g. %ss", v, v);
                return EXIT_INVALID;
            }
            if (dr == -3) {
                fail("new: --duration %s: a run simulates " DURATION_RANGE, v);
                return EXIT_INVALID;
            }
            if (dr != 0) {
                fail("new: bad duration '%s' (try 120s, 5m)", v);
                return EXIT_INVALID;
            }
            duration_given = 1;
        } else {
            fail("new: unknown option '%s' (cngsim help new)", a);
            return EXIT_INVALID;
        }
    }
    if (argc == 0 && isatty(STDIN_FILENO)) {
        asked = 1;
        printf("A new simulation.  Press Enter to take the value in brackets.\n");
        char q[300];
        snprintf(q, sizeof(q), "Template (%s)", template_names(", "));
        prompt(q, tname, tname, sizeof(tname));
        const template_t *t0 = find_template(tname);
        if (!board[0]) snprintf(board, sizeof(board), "%s", t0 ? t0->board : "sky");
        prompt("Board (cngsim boards lists them)", board, board, sizeof(board));
        /* Offer what the template takes: zephyr-echo's two, not the 4 the
         * others start from, which it would then refuse. */
        if (t0 && t0->max_n && n > t0->max_n && !n_given) n = t0->max_n;
        snprintf(nbuf, sizeof(nbuf), "%d", n);
        prompt("Number of nodes", nbuf, nbuf, sizeof(nbuf));
        n = atoi(nbuf);
        prompt("Layout (grid, line, circle, random)", layout, layout, sizeof(layout));
        prompt("Write it to", outp, outp, sizeof(outp));
    }
    const template_t *tp = find_template(tname);
    if (!tp) {
        fail("new: unknown template '%s' (%s)", tname, template_names(", "));
        return EXIT_INVALID;
    }
    /* No --board: the template's own, a board it has bundled images for
     * (sky for Contiki-NG; thread cc2538dk, zephyr-echo and riot-rpl
     * nrf52840-dk). */
    if (!board[0]) snprintf(board, sizeof(board), "%s", tp->board);
    /* A radio template needs a second node: its check waits for a line
     * that only traffic from another node prints. */
    int min_n = tp->expect_radio ? 2 : 1;
    if (n < min_n || n > MAX_SIM_NODES) {
        fail("new: %d nodes: give %d%s to %d", n, min_n,
             tp->roles[1] ? " (a server and a client)" : min_n > 1 ? " (one to send, one to receive)" : "",
             MAX_SIM_NODES);
        return EXIT_INVALID;
    }
    if (tp->max_n && n > tp->max_n && !n_given && !asked) n = tp->max_n;   /* its default */
    if (tp->max_n && n > tp->max_n) {
        fail("new: %d nodes: the %s template takes at most %d, so give -n %d or fewer (%s)",
             n, tp->name, tp->max_n, tp->max_n, tp->why_max ? tp->why_max : "");
        return EXIT_INVALID;
    }
    if (strcmp(layout, "grid") && strcmp(layout, "line") && strcmp(layout, "circle") &&
        strcmp(layout, "random")) {
        fail("new: unknown layout '%s' (grid, line, circle, random)", layout);
        return EXIT_INVALID;
    }
    if (spacing <= 0) {
        fail("new: --spacing must be > 0");
        return EXIT_INVALID;
    }
    const sim_board_desc_t *bd = sim_board_find(board);
    if (!bd || !bd->name) {
        char why[512];
        sim_board_unknown_error(board, why, sizeof(why));
        fail("new: %s", why);
        return EXIT_INVALID;
    }
    /* An alias (nrf52840dk/nrf52840, firefly) names the bundled images'
     * directory by the registry name. */
    snprintf(board, sizeof(board), "%s", bd->name);
    const char *data = data_dir();
    if (!data) {
        fail("new: no bundled firmware found: set CNGSIM_DATA to the directory "
             "that holds examples/ and firmware/");
        return EXIT_INVALID;
    }
    if (!force && access(outp, F_OK) == 0) {
        fail("new: %s exists (--force overwrites it)", outp);
        return EXIT_INVALID;
    }
    char outdir[PATH_MAX];
    snprintf(outdir, sizeof(outdir), "%s", outp);
    char *sl = strrchr(outdir, '/');
    if (sl) { if (sl == outdir) sl[1] = '\0'; else *sl = '\0'; }
    else snprintf(outdir, sizeof(outdir), ".");

    char fw[2][PATH_MAX];
    for (int r = 0; r < 2 && tp->roles[r]; r++) {
        char img[PATH_MAX + 256];
        if (!template_image(data, board, tp, r, img, sizeof(img))) {
            fail("new: no bundled %s image for %s; the %s template comes with: %s",
                 tp->images[r], board, tp->name, template_boards(data, tp));
            return EXIT_INVALID;
        }
        if (tp->board_ok && !tp->board_ok(board)) {
            fail("new: the %s template has no setup for %s yet: %s", tp->name, board,
                 tp->why_board);
            return EXIT_INVALID;
        }
        /* Bundled firmware is named by its absolute path, so the setup
         * still runs after it is moved; only the user's own files are
         * worth naming relative to the setup. */
        if (!realpath(img, fw[r])) snprintf(fw[r], sizeof(fw[r]), "%s", img);
    }

    new_spec_t sp = {
        .tp = tp, .board = board, .layout = layout, .outp = outp, .fw = fw,
        .n = n, .seed = seed, .duration_ms = duration_ms, .spacing = spacing,
    };
    sp.clients = tp->roles[1] ? n - 1 : n;
    sp.columns = (int)ceil(sqrt((double)(sp.clients > 0 ? sp.clients : 1)));
    sp.range = spacing * 1.5;
    /* circle: evenly spaced round the server, but never farther from it
     * than the radio reaches (from 11 nodes up spacing*clients/2pi would
     * be).  Without a server the ring itself carries the traffic, so its
     * neighbours are exactly `spacing` apart: the chord 2r sin(pi/n). */
    if (tp->roles[1]) {
        sp.radius = spacing * sp.clients / (2.0 * M_PI);
        if (sp.radius < spacing) sp.radius = spacing;
        if (sp.radius > 0.95 * sp.range) sp.radius = 0.95 * sp.range;
    } else {
        sp.radius = sp.clients > 1 ? spacing / (2.0 * sin(M_PI / sp.clients)) : spacing;
    }
    sp.side = round(spacing * sqrt((double)sp.clients) * 10) / 10;

    /* Built in a temporary file next to the output and renamed into place
     * once it loads, so a failure never leaves a partial setup behind. */
    char tmp[PATH_MAX + 32];
    snprintf(tmp, sizeof(tmp), "%s/.cngsim-new-XXXXXX.yaml", outdir);
    int tfd = mkstemps(tmp, 5);
    if (tfd < 0) {
        fail("new: cannot write in %s: %s", outdir, strerror(errno));
        return EXIT_INVALID;
    }
    close(tfd);

    /* Place the nodes, load what was written (the loader expands layouts),
     * and check every node has a radio path to node 1.  A random scatter
     * that leaves one out is drawn again in a smaller square: the same
     * draws, scaled, so it always ends up connected. */
    static sim_normalized_config_t cfg;
    int far = 0, hops = 0;
    for (int attempt = 0; attempt < 40; attempt++) {
        if (write_setup(tmp, &sp, 0) != 0 || sim_config_load(&cfg, tmp) != 0) {
            unlink(tmp);
            fail("new: the setup for %s does not load (a bug: please report it)", outp);
            return EXIT_INVALID;
        }
        hops = tp->expect_radio ? hops_to_node1(&cfg, sp.range, sp.hop_of) : 0;
        far = farthest_from_node1(&cfg, tp->roles[1] != NULL);
        sim_config_free(&cfg);
        if (hops >= 0 || strcmp(layout, "random") != 0) break;
        sp.side = round(sp.side * 0.9 * 10) / 10;
    }
    if (hops < 0) {
        unlink(tmp);
        fail("new: in this layout not every node has a radio path to node 1, so "
             "its check would fail; try a smaller --spacing or another layout");
        return EXIT_INVALID;
    }
    sp.hops = hops;
    if (tp->last_ms) {          /* its last console input, and time to answer it */
        long need_ms = tp->last_ms(&sp) + 5000;
        if (!duration_given) sp.duration_ms = need_ms;
        else if (need_ms > sp.duration_ms)
            fprintf(stderr, "cngsim: warning: the last console input is sent at %ld s; "
                    "%ld s is too short for its check (%ld s is enough)\n",
                    tp->last_ms(&sp) / 1000, sp.duration_ms / 1000, need_ms / 1000);
    }
    if (tp->hop_s && hops > 0) {
        long need_ms = (tp->base_s + (long)tp->hop_s * hops) * 1000;
        if (!duration_given && need_ms > sp.duration_ms)
            sp.duration_ms = need_ms;
        else if (duration_given && need_ms > sp.duration_ms)
            fprintf(stderr, "cngsim: warning: the farthest node is %d hops from node 1; "
                    "%ld s may be too short for its check to pass (about %ld s is "
                    "enough)\n", hops, sp.duration_ms / 1000, need_ms / 1000);
    }
    if (write_setup(tmp, &sp, far) != 0 || sim_config_load(&cfg, tmp) != 0) {
        unlink(tmp);
        fail("new: the setup for %s does not load (a bug: please report it)", outp);
        return EXIT_INVALID;
    }
    sim_config_free(&cfg);
    if (rename(tmp, outp) != 0) {
        fail("new: cannot write %s: %s", outp, strerror(errno));
        unlink(tmp);
        return EXIT_INVALID;
    }

    printf("Wrote %s: %d %s node%s, %s layout, %g m radio range.\n", outp, n, board,
           n == 1 ? "" : "s", layout, sp.range);
    printf("  Run it:   cngsim run %s\n  Watch it: cngsim run %s --ui\n", outp, outp);
    if (tp->note) printf("Note: %s.\n", tp->note);
    (void)asked;
    return 0;
}

/* ---- convert ----------------------------------------------------------- */

static int cmd_convert(int argc, char **argv) {
    const char *in = NULL, *out = NULL;
    char *extra[32];
    int nextra = 0;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) out = argv[++i];
        else if ((strcmp(argv[i], "--contiki") == 0 || strcmp(argv[i], "--firmware-dir") == 0) &&
                 i + 1 < argc && nextra < 30) {
            extra[nextra++] = argv[i];
            extra[nextra++] = argv[++i];
        } else if (argv[i][0] != '-' && !in) in = argv[i];
        else {
            fail("convert: unknown option '%s' (cngsim help convert)", argv[i]);
            return EXIT_INVALID;
        }
    }
    if (!in || !has_suffix(in, ".csc")) {
        fail("convert: give a Cooja .csc file");
        return EXIT_INVALID;
    }
    char outbuf[PATH_MAX];
    if (!out) {
        snprintf(outbuf, sizeof(outbuf), "%.*s.yaml", (int)(strlen(in) - 4), in);
        out = outbuf;
    }
    const char *data = data_dir();
    char script[PATH_MAX + 64];
    snprintf(script, sizeof(script), "%s/tools/csc2json.py", data ? data : ".");
    if (!is_file(script)) {
        fail("convert: %s not found (set CNGSIM_DATA)", script);
        return EXIT_INVALID;
    }
    /* The converter writes its JSON to stdout, here a fresh file from
     * mkstemp (never a predictable name), and its errors to the terminal. */
    char tmp[PATH_MAX];
    const char *td = getenv("TMPDIR");
    snprintf(tmp, sizeof(tmp), "%s/cngsim-convert-XXXXXX.json", td && *td ? td : "/tmp");
    int tfd = mkstemps(tmp, 5);
    if (tfd < 0) {
        fail("convert: no temporary file: %s", strerror(errno));
        return EXIT_INVALID;
    }
    char *pargv[48];
    int pn = 0;
    pargv[pn++] = "python3";
    pargv[pn++] = script;
    pargv[pn++] = (char *)in;
    pargv[pn++] = "--js-native";
    for (int i = 0; i < nextra; i++) pargv[pn++] = extra[i];
    pargv[pn] = NULL;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, tfd, STDOUT_FILENO);
    pid_t pid;
    int sp_rc = posix_spawnp(&pid, "python3", &fa, NULL, pargv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(tfd);
    if (sp_rc != 0) {
        unlink(tmp);
        fail("convert: needs python3 on PATH");
        return EXIT_INVALID;
    }
    int st;
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        unlink(tmp);
        fail("convert: %s could not be converted (see above)", in);
        return EXIT_INVALID;
    }
    static sim_normalized_config_t cfg;
    int rc = sim_config_load(&cfg, tmp);
    unlink(tmp);
    if (rc != 0) return EXIT_INVALID;
    FILE *f = fopen(out, "w");
    if (!f) {
        fail("convert: cannot write %s: %s", out, strerror(errno));
        return EXIT_INVALID;
    }
    char outdir[PATH_MAX];
    snprintf(outdir, sizeof(outdir), "%s", out);
    char *sl = strrchr(outdir, '/');
    if (sl) *sl = '\0'; else snprintf(outdir, sizeof(outdir), ".");
    char hdr[PATH_MAX + 64];
    snprintf(hdr, sizeof(hdr), "converted from %s by cngsim %s", in, CSIM_VERSION);
    rc = sim_config_write_yaml(&cfg, f, hdr, outdir);
    if (fclose(f) != 0) rc = -1;
    if (rc != 0) {
        sim_config_free(&cfg);
        return EXIT_INVALID;
    }
    /* The firmware a .csc names is built from Contiki-NG sources, often not
     * yet: say which images run needs first, rather than "run it". */
    int missing = 0;
    for (int i = 0; i < cfg.node_count; i++) {
        const char *fw = cfg.nodes[i].firmware;
        if (!fw[0]) continue;
        const sim_board_desc_t *b = sim_board_for_path(fw);
        if (b && (b->kind == SIM_BOARD_KIND_RENODE || b->kind == SIM_BOARD_KIND_EXTERNAL ||
                  b->kind == SIM_BOARD_KIND_JS))
            continue;
        char at[PATH_MAX * 2];
        if (fw[0] == '/') snprintf(at, sizeof(at), "%s", fw);
        else snprintf(at, sizeof(at), "%s/%s", outdir, fw);
        int dup = 0;
        for (int j = 0; j < i; j++)
            if (strcmp(cfg.nodes[j].firmware, fw) == 0) dup = 1;
        if (dup || is_file(at) || (fw[0] != '/' && is_file(fw))) continue;
        if (!missing++)
            printf("Wrote %s.  It needs firmware that is not built yet:\n", out);
        printf("  %s\n", at);
    }
    sim_config_free(&cfg);
    if (missing)
        printf("Build %s (docs/contiki-ng-testing.md), then: cngsim run %s\n",
               missing == 1 ? "it" : "them", out);
    else
        printf("Wrote %s.  Run it: cngsim run %s\n", out, out);
    return 0;
}

/* ---- help -------------------------------------------------------------- */

static void help(FILE *f) {
    fprintf(f,
        "cngsim %s - run Cooja-NG network simulations\n"
        "\n"
        "  cngsim run <sim.yaml | example | firmware...> [options]   run a simulation\n"
        "  cngsim new [template] [options]                           write a new setup file\n"
        "  cngsim check <sim.yaml>                                   validate a setup, without running it\n"
        "  cngsim examples                                           list the bundled examples\n"
        "  cngsim boards [--os]                                      list the boards and how to build for them\n"
        "  cngsim convert <sim.csc> [-o sim.yaml]                    a Cooja .csc as YAML (needs python3)\n"
        "  cngsim help [command] | --version\n"
        "\n"
        "Start with:  cngsim examples   then   cngsim run <example> --ui\n"
        "Exit codes:  0 pass, 1 a check failed, 2 bad input, 6 wall-clock timeout,\n"
        "             7 cancelled (through the protocol), 130 interrupted (Ctrl-C)\n"
        "Guide:       docs/quick-start.md\n", CSIM_VERSION);
}

static void help_run(void) {
    printf(
        "cngsim run <sim.yaml | example | firmware...> [options]\n"
        "\n"
        "Runs a simulation headless, as fast as it goes, until its duration or its\n"
        "checks decide it, and prints the nodes' consoles, the checks and a verdict.\n"
        "A firmware image (or several) without a setup file runs one node each.\n"
        "\n"
        "  --ui [PORT]           watch it in the browser (http://127.0.0.1:8080/): it\n"
        "                        opens paused, at real time; --start runs at once,\n"
        "                        --no-open does not open the browser\n"
        "  --ui-bind ADDR        serve the UI on ADDR: whoever reaches it can pause,\n"
        "                        play, set the speed, restart and move nodes\n"
        "  --ui-edit             on --ui-bind, also add, remove and reboot nodes,\n"
        "                        send lines and save (always on, on loopback)\n"
        "  --paused              start paused (with --ui, --shell or --script)\n"
        "  --duration 60s, -t    simulated time, with its unit: 60s, 500ms, 2m\n"
        "  --seed N              another run, as reproducible as the first\n"
        "  --speed N|max|realtime  pacing (headless runs are unpaced); --realtime is 1x\n"
        "  --shell               interactive command line (docs/shell.md)\n"
        "  --script FILE         a command script with a pass/fail verdict\n"
        "  --save-config F.yaml  write the setup as it is at the end (moved/added nodes)\n"
        "  --pcap FILE           capture the radio traffic for Wireshark\n"
        "  --idle-skip           skip proven idle passes of a polling main loop on\n"
        "                        ARM nodes: the same run, faster (docs/idle-skip.md);\n"
        "                        --no-idle-skip turns a setup's idle_skip off\n"
        "  --ui-record FILE      record the run for replay in the browser\n"
        "  --plugin LIB.so       load a plugin (service or radio medium)\n"
        "  --wall-timeout DUR    give up after this much real time (exit 6)\n"
        "  --gdb [NODE:]PORT     a GDB server on a node; --gdb-wait waits for GDB\n"
        "  --renode [ADDR:MAIN:ASYNC]  run as Renode's clock slave; --renode-freq HZ\n"
        "  -n N                  with firmware files: the number of nodes\n"
        "  --board NAME          with firmware files: the board they run on, for an\n"
        "                        image not named <app>.<board> (cngsim boards)\n"
        "  -q / -v               the verdict only / everything (test_runner's output)\n"
        "  --trace radio|boot|stats|all   add the developer output, by kind\n");
}

/* `text` from column `col`, words wrapped before column 78, a new line
 * starting at column `indent`. */
static void put_wrapped(const char *text, int col, int indent) {
    const char *w = text;
    while (*w) {
        size_t len = strcspn(w, " ");
        if (col > indent && col + 1 + (int)len > 78) {
            printf("\n%*s", indent, "");
            col = indent;
        } else if (col > indent) {
            putchar(' ');
            col++;
        }
        printf("%.*s", (int)len, w);
        col += (int)len;
        w += len;
        while (*w == ' ') w++;
    }
    putchar('\n');
}

static void help_new(void) {
    const char *data = data_dir();
    printf("cngsim new [%s] [options]\n", template_names("|"));
    printf("\n"
           "Writes a commented setup file from a template and the bundled firmware.\n"
           "With no arguments on a terminal, it asks.  The templates, the images\n"
           "they run and the boards they come with (the first is the default):\n\n");
    /* From templates[] and the data directory, so it cannot drift. */
    for (size_t i = 0; i < NTEMPLATES; i++) {
        const template_t *tp = &templates[i];
        char line[1024];
        int n = snprintf(line, sizeof(line), "%s%s%s", tp->images[0],
                         tp->images[1] ? ", " : "", tp->images[1] ? tp->images[1] : "");
        int min_n = tp->expect_radio ? 2 : 1;      /* as cmd_new refuses fewer */
        if (tp->max_n)
            n += snprintf(line + n, sizeof(line) - (size_t)n, ", %s%d nodes",
                          tp->max_n == min_n ? "" : "up to ", tp->max_n);
        const char *bs = data ? template_boards(data, tp) : tp->board;
        snprintf(line + n, sizeof(line) - (size_t)n, ": %s", bs[0] ? bs : tp->board);
        printf("  %-12s ", tp->name);
        put_wrapped(line, 15, 15);
    }
    printf(
        "\n"
        "  --board NAME        the board (cngsim boards lists them; Zephyr and RIOT\n"
        "                      names work too); by default the template's first\n"
        "  -n N                the number of nodes (default 4, or a template's most\n"
        "                      when that is fewer)\n"
        "  --layout L          grid, line, circle or random (default grid)\n"
        "  --spacing M         metres between nodes (default 30; the range is 1.5x)\n"
        "  --duration D        simulated time, with its unit (default 120s, longer\n"
        "                      for a network many hops deep)\n"
        "  --seed N            (default 42)\n"
        "  -o FILE             (default sim.yaml; --force overwrites)\n");
}

static int cmd_help(int argc, char **argv) {
    if (argc > 0 && strcmp(argv[0], "run") == 0) help_run();
    else if (argc > 0 && strcmp(argv[0], "new") == 0) help_new();
    else if (argc > 0 && strcmp(argv[0], "check") == 0)
        printf("cngsim check <sim.yaml>\n\nLoads the setup the way run would: every key known, every value\n"
               "of the right type, every firmware image found and its board known,\n"
               "every plugin a built-in name or a readable file.\n"
               "Prints a summary and exits 0, or the problems and exits 2.\n");
    else if (argc > 0 && strcmp(argv[0], "convert") == 0)
        printf("cngsim convert <sim.csc> [-o sim.yaml] [--contiki DIR] [--firmware-dir DIR]\n\n"
               "Converts a Cooja simulation (Sky, Z1 and Cooja motes, UDGM, the test\n"
               "script) with tools/csc2json.py.  Needs python3.  The firmware must\n"
               "already be built; see docs/contiki-ng-testing.md.\n");
    else if (argc > 0 && strcmp(argv[0], "examples") == 0)
        printf("cngsim examples\n\nLists the bundled example simulations and where they are.  Run one\n"
               "by name (cngsim run rpl-udp-sky), or copy it and edit the copy.\n");
    else if (argc > 0 && strcmp(argv[0], "boards") == 0)
        printf("cngsim boards [--os]\n\nLists the boards, their CPU and radio, and the Contiki-NG TARGET/BOARD\n"
               "that builds for each.  An image named <app>.<board> runs as that board;\n"
               "for any other name, add  board: <board>  to its mote type.\n"
               "\n"
               "  --os   each board's name in Zephyr (west build -b), RIOT (BOARD=) and\n"
               "         OpenThread (the port); --board and board: take those names too,\n"
               "         e.g. --board nrf52840dk/nrf52840 for a west build's zephyr.elf\n");
    else help(stdout);
    return 0;
}

int main(int argc, char **argv) {
    /* Line-buffered from the start: `run` points fd 1 at a pipe to its
     * output filter, where stdio would otherwise buffer whole blocks and the
     * consoles would arrive in bursts.  setvbuf is only defined before the
     * stream's first use, so it is set here, not when the pipe goes in;
     * the FILE keeps its mode across the dup2. */
    setvbuf(stdout, NULL, _IOLBF, 0);
    snprintf(g_argv0, sizeof(g_argv0), "%s", argv[0]);
    /* A copied example names its firmware as ../firmware/..., relative to
     * examples/: let that still find the bundled images, as a last resort. */
    const char *data = data_dir();
    if (data) {
        char ex[PATH_MAX + 16];
        snprintf(ex, sizeof(ex), "%s/examples", data);
        sim_config_set_last_lookup_dir(ex);
    }
    if (argc < 2) {
        help(stderr);
        return EXIT_INVALID;
    }
    const char *cmd = argv[1];
    if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "-V") == 0) {
        printf("cngsim %s (Cooja-NG)\n", CSIM_VERSION);
        return 0;
    }
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0)
        return cmd_help(argc - 2, argv + 2);
    if (argc > 2 && (strcmp(argv[2], "--help") == 0 || strcmp(argv[2], "-h") == 0))
        return cmd_help(1, argv + 1);
    if (strcmp(cmd, "run") == 0) return cmd_run(argc - 2, argv + 2);
    if (strcmp(cmd, "new") == 0) return cmd_new(argc - 2, argv + 2);
    if (strcmp(cmd, "check") == 0) return cmd_check(argc - 2, argv + 2);
    if (strcmp(cmd, "examples") == 0) return cmd_examples();
    if (strcmp(cmd, "boards") == 0) return cmd_boards(argc - 2, argv + 2);
    if (strcmp(cmd, "convert") == 0) return cmd_convert(argc - 2, argv + 2);
    fail("unknown command '%s' (cngsim help)", cmd);
    return EXIT_INVALID;
}
