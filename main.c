/*
 * wheel-filter — remove "ticks fantasma" (inversões espúrias) da roda do mouse.
 *
 * Dois modos:
 *
 *   Replay (teste offline com log do evtest):
 *       ./wheel-filter [log.txt] [-c N] [-f ms] [-i ms]
 *
 *   Ao vivo (lê o mouse, filtra e recria via uinput; precisa de root):
 *       sudo ./wheel-filter /dev/input/by-id/...-event-mouse [-c N] [-f ms] [-i ms]
 *
 *   Ao vivo achando o mouse pelo nome (sobrevive a mudança de /dev/input/eventN):
 *       sudo ./wheel-filter -d G703
 *
 *   Observação (não interfere no mouse; só registra os ghosts detectados):
 *       ./wheel-filter /dev/input/by-id/...-event-mouse -n -l ghosts.log
 *
 *   -l arquivo grava cada tick descartado com o contexto dos ticks ao redor
 *   (funciona nos dois modos ao vivo). Sem -l, o registro vai para o stderr.
 *
 * Regra do filtro:
 *   Uma inversão de direção só é aceita se vier confirmada por pelo menos
 *   N ticks seguidos na nova direção (-c, padrão 2). Se, enquanto a inversão
 *   está "em espera", chegar um tick na direção antiga, os ticks em espera
 *   são descartados (era ruído). Se nada chegar em -f ms (padrão 350), a
 *   inversão é considerada real (um único clique lento para o outro lado).
 *   Depois de -i ms sem nenhum evento (padrão 500), o próximo tick passa
 *   direto, em qualquer direção.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LINE_SIZE   512
#define MAX_PENDING 16
#define MAX_OTHER   64
#define RING_SIZE   8

/* ------------------------------------------------------------------ */
/* Núcleo do filtro (independente de I/O)                             */
/* ------------------------------------------------------------------ */

typedef struct {
    double ts;      /* segundos (epoch) */
    int value;      /* REL_WHEEL (ou hi-res, se REL_WHEEL vier zerado) */
    int wheel;      /* valor de REL_WHEEL a reemitir */
    int hires;      /* valor de REL_WHEEL_HI_RES a reemitir */
    double gap_ms;  /* tempo desde o tick anterior (só para o log); <0 = n/d */
} tick_t;

typedef void (*verdict_fn)(void *ctx, const tick_t *t, int pass);

typedef struct {
    int confirm;
    double flush_s;
    double idle_s;

    int last_dir;
    double last_ts;

    tick_t pending[MAX_PENDING];
    int npending;

    verdict_fn cb;
    void *ctx;
} filter_t;

static int sign(int v) { return (v > 0) - (v < 0); }

static void release_pending(filter_t *f, int pass)
{
    int n = f->npending;
    f->npending = 0;
    for (int i = 0; i < n; i++) {
        f->cb(f->ctx, &f->pending[i], pass);
    }
}

/* Inversão em espera por tempo demais: era real, deixa passar. */
static void filter_expire(filter_t *f, double now)
{
    if (f->npending == 0 || now - f->pending[0].ts < f->flush_s) {
        return;
    }

    f->last_dir = sign(f->pending[0].value);
    release_pending(f, 1);
}

static void filter_feed(filter_t *f, const tick_t *t)
{
    filter_expire(f, t->ts);

    int dir = sign(t->value);
    double gap = t->ts - f->last_ts;
    f->last_ts = t->ts;

    if (dir == 0) {
        f->cb(f->ctx, t, 1);
        return;
    }

    if (f->last_dir == 0 || gap > f->idle_s) {
        f->last_dir = dir;
        f->cb(f->ctx, t, 1);
        return;
    }

    if (dir == f->last_dir) {
        release_pending(f, 0);   /* o que estava em espera era ruído */
        f->cb(f->ctx, t, 1);
        return;
    }

    /* direção oposta à aceita: segura até confirmar */
    if (f->npending < MAX_PENDING) {
        f->pending[f->npending++] = *t;
    }

    if (f->npending >= f->confirm || f->npending == MAX_PENDING) {
        f->last_dir = dir;
        release_pending(f, 1);
    }
}

static void filter_init(filter_t *f, int confirm, double flush_ms,
                        double idle_ms, verdict_fn cb, void *ctx)
{
    memset(f, 0, sizeof(*f));
    f->confirm = confirm < 1 ? 1 : confirm;
    f->flush_s = flush_ms / 1000.0;
    f->idle_s  = idle_ms / 1000.0;
    if (f->idle_s < f->flush_s) {
        f->idle_s = f->flush_s;
    }
    f->cb = cb;
    f->ctx = ctx;
}

/* ------------------------------------------------------------------ */
/* Modo replay                                                        */
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
           f.confirm, flush_ms, idle_ms);

    return EXIT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Modo ao vivo (evdev -> filtro -> uinput)                           */
/* ------------------------------------------------------------------ */

static volatile sig_atomic_t running = 1;

static void on_signal(int sig)
{
    (void)sig;
    running = 0;
}

static double now_realtime(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void emit_ev(int fd, int type, int code, int value)
{
    if (fd < 0) {
        return;
    }

    struct input_event e;
    memset(&e, 0, sizeof(e));
    e.type = type;
    e.code = code;
    e.value = value;

    if (write(fd, &e, sizeof(e)) < 0 && errno != EAGAIN) {
        perror("write uinput");
    }
}

typedef struct {
    int out_fd;                 /* -1 no modo observação */
    FILE *logf;
    tick_t ring[RING_SIZE];     /* últimos ticks recebidos (antes do filtro) */
    long ring_n;                /* total de ticks já recebidos */
} live_ctx_t;

static void fmt_time(double ts, char *buf, size_t n)
{
    time_t sec = (time_t)ts;
    struct tm tm;

    localtime_r(&sec, &tm);
    size_t len = strftime(buf, n, "%Y-%m-%d %H:%M:%S", &tm);
    snprintf(buf + len, n - len, ".%03d", (int)((ts - (double)sec) * 1000.0));
}

static void log_drop(live_ctx_t *c, const tick_t *t)
{
    char when[48];
    fmt_time(t->ts, when, sizeof(when));

    fprintf(c->logf, "[%s] ghost descartado: %+d\n", when, t->value);

    int count = c->ring_n < RING_SIZE ? (int)c->ring_n : RING_SIZE;

    for (int i = 0; i < count; i++) {
        const tick_t *r = &c->ring[(c->ring_n - count + i) % RING_SIZE];

        if (i == 0) {
            fprintf(c->logf, "    %.6f  %+d  delta=     --- ms", r->ts, r->value);
        } else {
            const tick_t *p = &c->ring[(c->ring_n - count + i - 1) % RING_SIZE];
            fprintf(c->logf, "    %.6f  %+d  delta=%8.2f ms",
                    r->ts, r->value, (r->ts - p->ts) * 1000.0);
        }

        fprintf(c->logf, "%s\n", r->ts == t->ts ? "  <- ghost" : "");
    }

    fflush(c->logf);
}

static void live_cb(void *ctx, const tick_t *t, int pass)
{
    live_ctx_t *c = ctx;

    if (!pass) {
        log_drop(c, t);
        return;
    }

    int fd = c->out_fd;

    if (t->wheel) {
        emit_ev(fd, EV_REL, REL_WHEEL, t->wheel);
    }
    if (t->hires) {
        emit_ev(fd, EV_REL, REL_WHEEL_HI_RES, t->hires);
    }
    emit_ev(fd, EV_SYN, SYN_REPORT, 0);
}

static void copy_bits(int in, int out, int type, int max, unsigned long ui_set)
{
    unsigned char bits[KEY_MAX / 8 + 1];
    memset(bits, 0, sizeof(bits));

    if (ioctl(in, EVIOCGBIT(type, sizeof(bits)), bits) < 0) {
        return;
    }

    ioctl(out, UI_SET_EVBIT, type);

    for (int c = 0; c <= max; c++) {
        if (bits[c / 8] & (1 << (c % 8))) {
            ioctl(out, ui_set, c);
        }
    }
}

static int create_uinput(int in)
{
    int out = open("/dev/uinput", O_WRONLY | O_NONBLOCK);

    if (out < 0) {
        perror("open /dev/uinput");
        return -1;
    }

    copy_bits(in, out, EV_KEY, KEY_MAX, UI_SET_KEYBIT);
    copy_bits(in, out, EV_REL, REL_MAX, UI_SET_RELBIT);
    copy_bits(in, out, EV_MSC, MSC_MAX, UI_SET_MSCBIT);

    struct input_id id;
    memset(&id, 0, sizeof(id));
    ioctl(in, EVIOCGID, &id);

    struct uinput_setup setup;
    memset(&setup, 0, sizeof(setup));
    setup.id = id;
    snprintf(setup.name, UINPUT_MAX_NAME_SIZE, "wheel-filter virtual mouse");

    if (ioctl(out, UI_DEV_SETUP, &setup) < 0 ||
        ioctl(out, UI_DEV_CREATE) < 0) {
        perror("criar dispositivo uinput");
        close(out);
        return -1;
    }

    return out;
}

/* Procura /dev/input/event* cujo nome contém `needle` e que tenha REL_WHEEL. */
static int find_device_by_name(const char *needle, char *path, size_t size)
{
    DIR *dir = opendir("/dev/input");

    if (!dir) {
        perror("opendir /dev/input");
        return -1;
    }

    int found = -1;
    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "event", 5) != 0) {
            continue;
        }

        char candidate[300];
        snprintf(candidate, sizeof(candidate), "/dev/input/%s", entry->d_name);

        int fd = open(candidate, O_RDONLY);

        if (fd < 0) {
            continue;
        }

        char name[256] = "";
        unsigned char rel[REL_MAX / 8 + 1];
        memset(rel, 0, sizeof(rel));

        ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel)), rel);
        close(fd);

        int has_wheel = rel[REL_WHEEL / 8] & (1 << (REL_WHEEL % 8));

        if (has_wheel && strstr(name, needle) &&
            !strstr(name, "wheel-filter")) {
            snprintf(path, size, "%s", candidate);
            fprintf(stderr, "Dispositivo encontrado: %s (%s)\n", candidate, name);
            found = 0;
            break;
        }
    }

    closedir(dir);
    return found;
}

static int run_live(const char *device, int confirm, double flush_ms,
                    double idle_ms, int observe, const char *logpath)
{
    int in = open(device, O_RDONLY);

    if (in < 0) {
        perror("open dispositivo");
        return EXIT_FAILURE;
    }

    live_ctx_t lc;
    memset(&lc, 0, sizeof(lc));
    lc.out_fd = -1;
    lc.logf = stderr;

    if (logpath) {
        lc.logf = fopen(logpath, "a");

        if (!lc.logf) {
            perror("abrir arquivo de log");
            close(in);
            return EXIT_FAILURE;
        }
    }

    if (!observe) {
        /* captura antes de criar o virtual: nunca há dois mouses ao mesmo tempo */
        if (ioctl(in, EVIOCGRAB, 1) < 0) {
            perror("EVIOCGRAB");
            if (logpath) {
                fclose(lc.logf);
            }
            close(in);
            return EXIT_FAILURE;
        }

        lc.out_fd = create_uinput(in);

        if (lc.out_fd < 0) {
            ioctl(in, EVIOCGRAB, 0);
            if (logpath) {
                fclose(lc.logf);
            }
            close(in);
            return EXIT_FAILURE;
        }

        /* pequena pausa para o sistema registrar o dispositivo virtual */
        usleep(200000);
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    filter_t f;
    filter_init(&f, confirm, flush_ms, idle_ms, live_cb, &lc);

    {
        char when[48];
        fmt_time(now_realtime(), when, sizeof(when));
        fprintf(lc.logf, "[%s] %s %s (-c %d -f %.0f -i %.0f)\n", when,
                observe ? "observando (sem filtrar)" : "filtrando",
                device, f.confirm, flush_ms, idle_ms);
        fflush(lc.logf);
    }

    if (lc.logf != stderr) {
        fprintf(stderr, "%s %s. Registro em %s. Ctrl+C para sair.\n",
                observe ? "Observando" : "Filtrando", device, logpath);
    }

    struct input_event other[MAX_OTHER];
    int nother = 0;
    int has_wheel = 0, w_val = 0, w_hi = 0;

    struct pollfd pfd = { .fd = in, .events = POLLIN };

    while (running) {
        int timeout = -1;

        if (f.npending > 0) {
            double left = f.flush_s - (now_realtime() - f.pending[0].ts);
            timeout = left <= 0 ? 0 : (int)(left * 1000.0) + 1;
        }

        int r = poll(&pfd, 1, timeout);

        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            break;
        }

        if (r == 0) {
            filter_expire(&f, now_realtime());
            continue;
        }

        struct input_event ev;
        ssize_t n = read(in, &ev, sizeof(ev));

        if (n != (ssize_t)sizeof(ev)) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
                continue;
            }
            fprintf(stderr, "Dispositivo desconectado.\n");
            break;
        }

        if (ev.type == EV_REL &&
            (ev.code == REL_WHEEL || ev.code == REL_WHEEL_HI_RES)) {
            has_wheel = 1;
            if (ev.code == REL_WHEEL) {
                w_val = ev.value;
            } else {
                w_hi = ev.value;
            }
            continue;
        }

        if (!(ev.type == EV_SYN && ev.code == SYN_REPORT)) {
            if (nother < MAX_OTHER) {
                other[nother++] = ev;
            }
            continue;
        }

        /* fim do frame */
        if (has_wheel) {
            tick_t t;
            t.ts = ev.time.tv_sec + ev.time.tv_usec / 1e6;
            t.wheel = w_val;
            t.hires = w_hi ? w_hi : w_val * 120;
            t.value = w_val ? w_val : w_hi;
            t.gap_ms = -1.0;

            lc.ring[lc.ring_n % RING_SIZE] = t;
            lc.ring_n++;

            filter_feed(&f, &t);
        }

        if (nother > 0) {
            for (int i = 0; i < nother; i++) {
                emit_ev(lc.out_fd, other[i].type, other[i].code, other[i].value);
            }
            emit_ev(lc.out_fd, EV_SYN, SYN_REPORT, 0);
        }

        nother = 0;
        has_wheel = 0;
        w_val = 0;
        w_hi = 0;
    }

    if (!observe) {
        ioctl(in, EVIOCGRAB, 0);
        ioctl(lc.out_fd, UI_DEV_DESTROY);
        close(lc.out_fd);
    }

    if (logpath) {
        fclose(lc.logf);
    }

    close(in);

    return EXIT_SUCCESS;
}

/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Uso:\n"
        "  %s [log.txt] [opções]              replay de um log do evtest\n"
        "  sudo %s /dev/input/... [opções]    filtro ao vivo\n\n"
        "Opções:\n"
        "  -c N   ticks seguidos necessários para aceitar inversão (padrão 2)\n"
        "  -f ms  tempo para considerar uma inversão isolada como real (350)\n"
        "  -i ms  inatividade após a qual o próximo tick sempre passa (500)\n"
        "  -d nome  acha o mouse pelo nome (ex.: -d G703) em vez do caminho\n"
        "  -n     só observar (ao vivo): não captura o mouse nem filtra\n"
        "  -l arq registra os ticks descartados em arq (ao vivo)\n",
        prog, prog);
}

int main(int argc, char *argv[])
{
    const char *target = "log.txt";
    int confirm = 2;
    double flush_ms = 350.0;
    double idle_ms = 500.0;
    int observe = 0;
    const char *logpath = NULL;
    const char *devname = NULL;
    char found_path[300];

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc) {
            confirm = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-f") && i + 1 < argc) {
            flush_ms = atof(argv[++i]);
        } else if (!strcmp(argv[i], "-i") && i + 1 < argc) {
            idle_ms = atof(argv[++i]);
        } else if (!strcmp(argv[i], "-d") && i + 1 < argc) {
            devname = argv[++i];
        } else if (!strcmp(argv[i], "-n")) {
            observe = 1;
        } else if (!strcmp(argv[i], "-l") && i + 1 < argc) {
            logpath = argv[++i];
        } else if (argv[i][0] == '-') {
            usage(argv[0]);
            return EXIT_FAILURE;
        } else {
            target = argv[i];
        }
    }

    if (devname) {
        if (find_device_by_name(devname, found_path, sizeof(found_path)) < 0) {
            fprintf(stderr, "Nenhum dispositivo com roda e nome contendo \"%s\".\n",
                    devname);
            return EXIT_FAILURE;
        }
        target = found_path;
    }

    if (strncmp(target, "/dev/", 5) == 0) {
        return run_live(target, confirm, flush_ms, idle_ms, observe, logpath);
    }

    return run_replay(target, confirm, flush_ms, idle_ms);
}
