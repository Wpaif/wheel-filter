#define _GNU_SOURCE
#include "io.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define RING_SIZE 8
#define MAX_OTHER 64
#define VIRTUAL_PREFIX "Wheel Filter"

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

double now_realtime(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

void fmt_time(double ts, char *buf, size_t n)
{
    time_t sec = (time_t)ts;
    struct tm tm;

    localtime_r(&sec, &tm);
    size_t len = strftime(buf, n, "%Y-%m-%d %H:%M:%S", &tm);
    snprintf(buf + len, n - len, ".%03d", (int)((ts - (double)sec) * 1000.0));
}

typedef struct {
    const session_cfg_t *cfg;
    filter_t f;
    int in;
    int out;                    /* -1 no modo observação */

    tick_t ring[RING_SIZE];     /* últimos ticks recebidos (antes do filtro) */
    long ring_n;
    double prev_rx;
    int have_prev;

    char ctl_buf[256];
    size_t ctl_len;
    int stop;
    char last_state[16];
} sess_t;

/* ------------------------------------------------------------------ */
/* estado e comandos                                                   */
/* ------------------------------------------------------------------ */

static void set_state(sess_t *s, const char *state, const char *msg)
{
    snprintf(s->last_state, sizeof(s->last_state), "%s", state);

    if (s->cfg->on_state) {
        s->cfg->on_state(s->cfg->user, state, msg);
    }
}

static void ctl_process_line(sess_t *s, const char *line)
{
    if (strcmp(line, "STOP") == 0) {
        s->stop = 1;
        return;
    }

    int c;
    double fl, id;

    if (sscanf(line, "SET %d %lf %lf", &c, &fl, &id) == 3) {
        filter_set_params(&s->f, c, fl, id);
    }
}

static void ctl_read(sess_t *s)
{
    if (s->cfg->ctl_fd < 0) {
        return;
    }

    ssize_t n = read(s->cfg->ctl_fd, s->ctl_buf + s->ctl_len,
                     sizeof(s->ctl_buf) - s->ctl_len - 1);

    if (n <= 0) {
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
            return;
        }
        s->stop = 1;            /* EOF: quem controlava sumiu */
        return;
    }

    s->ctl_len += (size_t)n;
    s->ctl_buf[s->ctl_len] = '\0';

    char *start = s->ctl_buf;
    char *nl;

    while ((nl = strchr(start, '\n')) != NULL) {
        *nl = '\0';
        ctl_process_line(s, start);
        start = nl + 1;
    }

    size_t rest = strlen(start);
    memmove(s->ctl_buf, start, rest + 1);
    s->ctl_len = rest;

    if (s->ctl_len >= sizeof(s->ctl_buf) - 1) {
        s->ctl_len = 0;
    }
}

static void wait_a_bit(sess_t *s, int ms)
{
    struct pollfd p = { .fd = s->cfg->ctl_fd, .events = POLLIN };
    int r = poll(&p, s->cfg->ctl_fd >= 0 ? 1 : 0, ms);

    if (r > 0 && (p.revents & (POLLIN | POLLHUP))) {
        ctl_read(s);
    }
}

/* ------------------------------------------------------------------ */
/* saída: uinput e log                                                 */
/* ------------------------------------------------------------------ */

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

static void log_drop(sess_t *s, const tick_t *t)
{
    FILE *out = s->cfg->logf;
    char when[48];

    fmt_time(t->ts, when, sizeof(when));
    fprintf(out, "[%s] ghost descartado: %+d\n", when, t->value);

    int count = s->ring_n < RING_SIZE ? (int)s->ring_n : RING_SIZE;

    for (int i = 0; i < count; i++) {
        const tick_t *r = &s->ring[(s->ring_n - count + i) % RING_SIZE];

        if (i == 0) {
            fprintf(out, "    %.6f  %+d  delta=     --- ms", r->ts, r->value);
        } else {
            const tick_t *p = &s->ring[(s->ring_n - count + i - 1) % RING_SIZE];
            fprintf(out, "    %.6f  %+d  delta=%8.2f ms",
                    r->ts, r->value, (r->ts - p->ts) * 1000.0);
        }

        fprintf(out, "%s\n", r->ts == t->ts ? "  <- ghost" : "");
    }

    fflush(out);
}

static void verdict(void *ctx, const tick_t *t, int pass)
{
    sess_t *s = ctx;

    if (pass) {
        if (t->wheel) {
            emit_ev(s->out, EV_REL, REL_WHEEL, t->wheel);
        }
        if (t->hires) {
            emit_ev(s->out, EV_REL, REL_WHEEL_HI_RES, t->hires);
        }
        emit_ev(s->out, EV_SYN, SYN_REPORT, 0);
    } else if (s->cfg->logf) {
        log_drop(s, t);
    }

    if (s->cfg->on_tick) {
        s->cfg->on_tick(s->cfg->user, t, pass);
    }
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

static int create_uinput(int in, const char *devname)
{
    int out = open("/dev/uinput", O_WRONLY | O_NONBLOCK);

    if (out < 0) {
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
    snprintf(setup.name, UINPUT_MAX_NAME_SIZE, "%s - %s", VIRTUAL_PREFIX,
             devname);

    if (ioctl(out, UI_DEV_SETUP, &setup) < 0 ||
        ioctl(out, UI_DEV_CREATE) < 0) {
        int saved = errno;
        close(out);
        errno = saved;
        return -1;
    }

    return out;
}

/* ------------------------------------------------------------------ */
/* localizar e abrir o mouse                                           */
/* ------------------------------------------------------------------ */

/* Procura /dev/input/event* cujo nome contém `needle` e que tenha REL_WHEEL.
 * Em falha, errno = ENOENT (não achou) ou EACCES (sem permissão). */
static int find_device_by_name(const char *needle, char *path, size_t size)
{
    DIR *dir = opendir("/dev/input");

    if (!dir) {
        return -1;
    }

    int found = -1;
    int saw_eacces = 0;
    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "event", 5) != 0) {
            continue;
        }

        char candidate[300];
        snprintf(candidate, sizeof(candidate), "/dev/input/%s", entry->d_name);

        int fd = open(candidate, O_RDONLY);

        if (fd < 0) {
            if (errno == EACCES) {
                saw_eacces = 1;
            }
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
            strncmp(name, VIRTUAL_PREFIX, strlen(VIRTUAL_PREFIX)) != 0) {
            snprintf(path, size, "%s", candidate);
            found = 0;
            break;
        }
    }

    closedir(dir);

    if (found < 0) {
        errno = saw_eacces ? EACCES : ENOENT;
    }

    return found;
}

static int open_target(const session_cfg_t *cfg, char *resolved, size_t size)
{
    if (cfg->path) {
        snprintf(resolved, size, "%s", cfg->path);
    } else if (find_device_by_name(cfg->name, resolved, size) < 0) {
        return -1;
    }

    return open(resolved, O_RDONLY);
}

/* ------------------------------------------------------------------ */
/* atendimento de uma conexão                                          */
/* ------------------------------------------------------------------ */

/* 0 = pedido de parada, 1 = mouse desconectado, -1 = erro fatal */
static int serve(sess_t *s, int in, const char *resolved)
{
    const session_cfg_t *cfg = s->cfg;
    char msg[512];
    char devname[128] = "mouse";

    ioctl(in, EVIOCGNAME(sizeof(devname)), devname);

    s->in = in;
    s->out = -1;

    if (!cfg->observe) {
        /* captura antes de criar o virtual: nunca há dois mouses ao mesmo tempo */
        if (ioctl(in, EVIOCGRAB, 1) < 0) {
            snprintf(msg, sizeof(msg), "não foi possível capturar o mouse: %s%s",
                     strerror(errno),
                     errno == EBUSY
                         ? " (outro processo já o captura; o serviço wheel-filter está ativo?)"
                         : "");
            set_state(s, "error", msg);
            return -1;
        }

        s->out = create_uinput(in, devname);

        if (s->out < 0) {
            snprintf(msg, sizeof(msg),
                     "não foi possível criar o mouse virtual: %s "
                     "(módulo uinput carregado? precisa de root)",
                     strerror(errno));
            ioctl(in, EVIOCGRAB, 0);
            set_state(s, "error", msg);
            return -1;
        }

        /* pequena pausa para o sistema registrar o dispositivo virtual */
        usleep(200000);
    }

    filter_reset(&s->f);
    s->ring_n = 0;
    s->have_prev = 0;

    snprintf(msg, sizeof(msg), "%s (%s)", devname, resolved);
    set_state(s, cfg->observe ? "observing" : "filtering", msg);

    struct input_event other[MAX_OTHER];
    int nother = 0;
    int has_wheel = 0, w_val = 0, w_hi = 0;
    int result = 0;

    struct pollfd pfd[2];
    pfd[0].fd = in;
    pfd[0].events = POLLIN;
    pfd[1].fd = cfg->ctl_fd;
    pfd[1].events = POLLIN;
    nfds_t nfds = cfg->ctl_fd >= 0 ? 2 : 1;

    while (!g_stop && !s->stop) {
        int timeout = -1;

        if (s->f.npending > 0) {
            double left = s->f.flush_s - (now_realtime() - s->f.pending[0].ts);
            timeout = left <= 0 ? 0 : (int)(left * 1000.0) + 1;
        }

        int r = poll(pfd, nfds, timeout);

        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            result = 1;
            break;
        }

        if (r == 0) {
            filter_expire(&s->f, now_realtime());
            continue;
        }

        if (nfds == 2 && (pfd[1].revents & (POLLIN | POLLHUP))) {
            ctl_read(s);
            if (s->stop) {
                break;
            }
        }

        if (!(pfd[0].revents & (POLLIN | POLLERR | POLLHUP))) {
            continue;
        }

        struct input_event ev;
        ssize_t n = read(in, &ev, sizeof(ev));

        if (n != (ssize_t)sizeof(ev)) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
                continue;
            }
            result = 1;         /* ENODEV / EOF: mouse desconectado */
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
            t.gap_ms = s->have_prev ? (t.ts - s->prev_rx) * 1000.0 : -1.0;
            s->prev_rx = t.ts;
            s->have_prev = 1;

            s->ring[s->ring_n % RING_SIZE] = t;
            s->ring_n++;

            filter_feed(&s->f, &t);
        }

        if (nother > 0) {
            for (int i = 0; i < nother; i++) {
                emit_ev(s->out, other[i].type, other[i].code, other[i].value);
            }
            emit_ev(s->out, EV_SYN, SYN_REPORT, 0);
        }

        nother = 0;
        has_wheel = 0;
        w_val = 0;
        w_hi = 0;
    }

    if (!cfg->observe) {
        ioctl(in, EVIOCGRAB, 0);
        ioctl(s->out, UI_DEV_DESTROY);
        close(s->out);
        s->out = -1;
    }

    return result;
}

int session_run(const session_cfg_t *cfg)
{
    sess_t s;
    memset(&s, 0, sizeof(s));
    s.cfg = cfg;
    s.in = -1;
    s.out = -1;

    filter_init(&s.f, cfg->confirm, cfg->flush_ms, cfg->idle_ms, verdict, &s);

    g_stop = 0;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    int rc = EXIT_SUCCESS;

    while (!g_stop && !s.stop) {
        char resolved[300];
        int fd = open_target(cfg, resolved, sizeof(resolved));

        if (fd < 0) {
            if (errno != ENOENT && errno != ENODEV && errno != ENXIO) {
                char msg[200];
                snprintf(msg, sizeof(msg), "não foi possível abrir o mouse: %s%s",
                         strerror(errno),
                         errno == EACCES ? " (precisa de root)" : "");
                set_state(&s, "error", msg);
                rc = EXIT_FAILURE;
                break;
            }

            if (strcmp(s.last_state, "waiting") != 0) {
                set_state(&s, "waiting", "aguardando o mouse ser conectado");
            }

            wait_a_bit(&s, 1000);
            continue;
        }

        int r = serve(&s, fd, resolved);
        close(fd);
        s.in = -1;

        if (r < 0) {
            rc = EXIT_FAILURE;
            break;
        }

        if (r == 1 && !g_stop && !s.stop) {
            set_state(&s, "waiting", "mouse desconectado; aguardando reconexão");
            wait_a_bit(&s, 500);    /* evita laço apertado se o nó reaparecer e falhar */
        }
    }

    if (rc == EXIT_SUCCESS) {
        set_state(&s, "stopped", "filtro parado");
    }

    return rc;
}
