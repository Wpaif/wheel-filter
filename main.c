/*
 * wheel-filter — remove "ticks fantasma" (inversões espúrias) da roda do mouse.
 *
 * Modos:
 *   wheel-filter --gui                       janela gráfica
 *   wheel-filter [log.txt] [opções]          replay offline de um log do evtest
 *   sudo wheel-filter /dev/input/... [op.]   filtro ao vivo
 *   sudo wheel-filter -d G703 [opções]       ao vivo, achando o mouse pelo nome
 *   wheel-filter --engine <dispositivo> ...  uso interno da GUI (protocolo em texto)
 *
 * Regra do filtro:
 *   Uma inversão de direção só é aceita se vier confirmada por pelo menos
 *   N ticks seguidos na nova direção (-c, padrão 2). Se, enquanto a inversão
 *   está "em espera", chegar um tick na direção antiga, os ticks em espera
 *   são descartados (era ruído). Se nada chegar em -f ms (padrão 350), a
 *   inversão é considerada real. Depois de -i ms sem nenhum evento (padrão
 *   500), o próximo tick passa direto, em qualquer direção.
 *
 * Além disso o filtro aprende com o uso (ver filter.h): durante uma rolagem
 * contínua exige mais confirmações para inverter, descobre o menor tempo
 * humano de inversão, rotula retroativamente os ghosts que passaram e guarda
 * tudo em disco entre sessões.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "filter.h"
#include "io.h"

#ifdef WITH_GUI
#include "gui.h"
#endif

#define LINE_SIZE 512

/* ------------------------------------------------------------------ */
/* Replay                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    long passed;
    long dropped;
} stats_t;

static int parse_wheel_event(const char *line, double *timestamp, int *value)
{
    if (strstr(line, "REL_WHEEL)") == NULL) {
        return 0;
    }

    double ts;
    int val;

    int result = sscanf(
        line,
        "Event: time %lf, type 2 (EV_REL), code 8 (REL_WHEEL), value %d",
        &ts,
        &val
    );

    if (result != 2) {
        return 0;
    }

    *timestamp = ts;
    *value = val;

    return 1;
}

static void replay_cb(void *ctx, const tick_t *t, int pass)
{
    stats_t *s = ctx;

    if (pass) {
        s->passed++;
    } else {
        s->dropped++;
    }

    if (t->gap_ms < 0) {
        printf("%.6f  %+d  delta=     --- ms  %s\n",
               t->ts, t->value, pass ? "pass" : "DROP");
    } else {
        printf("%.6f  %+d  delta=%8.2f ms  %s\n",
               t->ts, t->value, t->gap_ms, pass ? "pass" : "DROP");
    }
}

static int run_replay(const char *filename, int confirm, double flush_ms,
                      double idle_ms)
{
    FILE *file = fopen(filename, "r");

    if (!file) {
        perror("Erro ao abrir arquivo");
        return EXIT_FAILURE;
    }

    stats_t stats = {0, 0};
    filter_t f;
    filter_init(&f, confirm, flush_ms, idle_ms, replay_cb, &stats);

    char line[LINE_SIZE];
    double prev_ts = 0.0;
    int have_prev = 0;

    while (fgets(line, sizeof(line), file)) {
        tick_t t;

        if (!parse_wheel_event(line, &t.ts, &t.value)) {
            continue;
        }

        t.wheel = t.value;
        t.hires = t.value * 120;
        t.gap_ms = have_prev ? (t.ts - prev_ts) * 1000.0 : -1.0;

        filter_feed(&f, &t);

        prev_ts = t.ts;
        have_prev = 1;
    }

    filter_expire(&f, 1e18);   /* descarrega o que sobrou em espera */
    fclose(file);

    printf("\n%ld ticks: %ld passaram, %ld descartados (-c %d -f %.0f -i %.0f)\n",
           stats.passed + stats.dropped, stats.passed, stats.dropped,
           f.confirm, f.flush_s * 1000.0, f.idle_s * 1000.0);

    return EXIT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Ao vivo (linha de comando)                                          */
/* ------------------------------------------------------------------ */

static void cli_state(void *user, const char *state, const char *msg)
{
    (void)user;

    char when[48];
    fmt_time(now_realtime(), when, sizeof(when));
    fprintf(stderr, "[%s] %s: %s\n", when, state, msg);
}

/* ------------------------------------------------------------------ */
/* Modo --engine: protocolo em linhas separadas por TAB (usado pela GUI) */
/* ------------------------------------------------------------------ */

static void engine_tick(void *user, const tick_t *t, int pass)
{
    (void)user;
    printf("TICK\t%.6f\t%d\t%s\t%.2f\t%d\t%d\n", t->ts, t->value,
           pass ? "PASS" : "GHOST", t->gap_ms, t->sector, t->period);
    fflush(stdout);
}

static void engine_state(void *user, const char *state, const char *msg)
{
    (void)user;
    printf("STATE\t%s\t%s\n", state, msg);
    fflush(stdout);
}

/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Uso:\n"
        "  %s --gui                          janela gráfica\n"
        "  %s [log.txt] [opções]             replay de um log do evtest\n"
        "  sudo %s /dev/input/... [opções]   filtro ao vivo\n"
        "  sudo %s -d G703 [opções]          ao vivo, achando o mouse pelo nome\n\n"
        "Opções:\n"
        "  -c N     ticks seguidos necessários para aceitar inversão (padrão 2)\n"
        "  -f ms    tempo para considerar uma inversão isolada como real (350)\n"
        "  -i ms    inatividade após a qual o próximo tick sempre passa (500)\n"
        "  -d nome  acha o mouse pelo nome (ex.: -d G703); espera se ele sumir\n"
        "  -s dir   onde guardar o que o filtro aprendeu (padrão: /var/lib/wheel-filter\n"
        "           como root; ~/.local/state/wheel-filter caso contrário)\n"
        "  -n       só observar (ao vivo): não captura o mouse nem filtra\n"
        "  -l arq   registra os ticks descartados em arq (ao vivo)\n",
        prog, prog, prog, prog);
}

int main(int argc, char *argv[])
{
    const char *target = "log.txt";
    int have_target = 0;
    int confirm = 2;
    double flush_ms = 350.0;
    double idle_ms = 500.0;
    int observe = 0;
    int engine = 0;
    int gui = 0;
    const char *logpath = NULL;
    const char *devname = NULL;
    const char *statedir = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc) {
            confirm = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-f") && i + 1 < argc) {
            flush_ms = atof(argv[++i]);
        } else if (!strcmp(argv[i], "-i") && i + 1 < argc) {
            idle_ms = atof(argv[++i]);
        } else if (!strcmp(argv[i], "-d") && i + 1 < argc) {
            devname = argv[++i];
        } else if (!strcmp(argv[i], "-l") && i + 1 < argc) {
            logpath = argv[++i];
        } else if (!strcmp(argv[i], "-s") && i + 1 < argc) {
            statedir = argv[++i];
        } else if (!strcmp(argv[i], "-n")) {
            observe = 1;
        } else if (!strcmp(argv[i], "--engine")) {
            engine = 1;
        } else if (!strcmp(argv[i], "--gui")) {
            gui = 1;
        } else if (argv[i][0] == '-') {
            usage(argv[0]);
            return EXIT_FAILURE;
        } else {
            target = argv[i];
            have_target = 1;
        }
    }

    if (gui) {
#ifdef WITH_GUI
        return gui_main();
#else
        fprintf(stderr, "Esta compilação não inclui a interface gráfica "
                        "(GTK 4 não foi encontrado). Veja o Makefile.\n");
        return EXIT_FAILURE;
#endif
    }

    int live = devname != NULL || strncmp(target, "/dev/", 5) == 0;

    if (!live && !engine) {
        return run_replay(target, confirm, flush_ms, idle_ms);
    }

    if (engine && !devname && !have_target) {
        fprintf(stderr, "--engine precisa de um dispositivo.\n");
        return EXIT_FAILURE;
    }

    session_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.path = devname ? NULL : target;
    cfg.name = devname;
    cfg.confirm = confirm;
    cfg.flush_ms = flush_ms;
    cfg.idle_ms = idle_ms;
    cfg.observe = observe;
    cfg.state_dir = statedir;
    cfg.ctl_fd = -1;

    if (engine) {
        setvbuf(stdout, NULL, _IOLBF, 0);
        cfg.ctl_fd = STDIN_FILENO;
        cfg.on_tick = engine_tick;
        cfg.on_state = engine_state;
    } else {
        cfg.on_state = cli_state;
        cfg.logf = stderr;

        if (logpath) {
            cfg.logf = fopen(logpath, "a");

            if (!cfg.logf) {
                perror("abrir arquivo de log");
                return EXIT_FAILURE;
            }
        }
    }

    int rc = session_run(&cfg);

    if (cfg.logf && cfg.logf != stderr) {
        fclose(cfg.logf);
    }

    return rc;
}
