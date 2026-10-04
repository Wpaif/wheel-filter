#include "filter.h"

#include <stdlib.h>
#include <string.h>

static int sign(int v) { return (v > 0) - (v < 0); }

static int wrap_pos(int pos, int period)
{
    if (period <= 0) {
        return 0;
    }

    int m = pos % period;
    return m < 0 ? m + period : m;
}

static int min_int(int a, int b) { return a < b ? a : b; }
static int max_int(int a, int b) { return a > b ? a : b; }
static double max_dbl(double a, double b) { return a > b ? a : b; }

#define CRUISE_RUN 4            /* ticks seguidos para valer como rolagem contínua */
#define CRUISE_GAP_S 0.150      /* intervalo máx. entre ticks dentro do ritmo */
#define CRUISE_HOLD_S 0.250     /* silêncio máx. para a rolagem ainda valer */
#define CRUISE_MULT 4.0         /* inversão em < 4x o intervalo típico é suspeita */
#define CRUISE_EXTRA 2          /* confirmações extras exigidas na rolagem contínua */
#define REV_BACK_S 0.5          /* a direção antiga voltou em tanto: era ghost */
#define SECTOR_DECAY 400        /* ao atingir, metade do histórico é esquecido */

/* Taxa de ghost do setor com prior Beta(0.2, 4.8): poucas amostras não
 * enganam, e a evidência acumulada domina. */
static double sector_rate(const filter_t *f, int s)
{
    return ((double)f->ghosts[s] + 0.2) / ((double)f->hits[s] + 5.0);
}

int filter_in_cruise(const filter_t *f, double ts)
{
    return f->acc_dir != 0 && f->run >= CRUISE_RUN &&
           ts - f->acc_ts < CRUISE_HOLD_S;
}

/* Uma inversão em rev_ts é rápida demais para ser humana? */
static int suspicious(const filter_t *f, double rev_ts)
{
    if (f->acc_dir == 0) {
        return 0;
    }

    double lim = f->human_min;

    if (filter_in_cruise(f, rev_ts)) {
        lim = max_dbl(lim, CRUISE_MULT * f->ewma_gap);
    }

    return rev_ts - f->acc_ts < lim;
}

int filter_sector_hot(const filter_t *f, int sector)
{
    if (sector < 0 || sector >= f->period) {
        return 0;
    }
    if (f->hits[sector] < 8) {
        return 0;
    }
    return sector_rate(f, sector) >= 0.12;
}

int filter_sector_confirm(const filter_t *f)
{
    int c = f->confirm;
    int s = f->pos;

    if (s < 0 || s >= f->period || f->hits[s] < 8) {
        return c;
    }

    double r = sector_rate(f, s);

    if (r >= 0.25) {
        return min_int(c + 2, MAX_PENDING);
    }
    if (r >= 0.10) {
        return min_int(c + 1, MAX_PENDING);
    }
    if (r < 0.03 && f->hits[s] >= 24) {
        return max_int(c - 1, 1);
    }
    return c;
}

static int score_period(const int *log, int n, int p)
{
    int bins[MAX_SECTORS];
    int max1 = 0, max2 = 0;

    memset(bins, 0, sizeof(bins));

    for (int i = 0; i < n; i++) {
        int b = wrap_pos(log[i], p);
        bins[b]++;
    }

    for (int i = 0; i < p; i++) {
        if (bins[i] > max1) {
            max2 = max1;
            max1 = bins[i];
        } else if (bins[i] > max2) {
            max2 = bins[i];
        }
    }

    /* premia concentração em poucos detentes */
    return max1 * 6 + max2 * 3 - p;
}

static void maybe_retune_period(filter_t *f)
{
    static const int candidates[] = {16, 18, 20, 24, 27, 36};
    int n = f->nghost_log;

    if (n < 12 || f->since_tune < 12) {
        return;
    }

    int best_p = f->period;
    int best = score_period(f->ghost_log, n, f->period);

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        int p = candidates[i];
        int s = score_period(f->ghost_log, n, p);

        if (s > best) {
            best = s;
            best_p = p;
        }
    }

    f->since_tune = 0;

    if (best_p == f->period) {
        return;
    }

    /* só troca se o padrão for claramente periódico */
    int uniform = (n / best_p) * 6;
    if (best < uniform + n) {
        return;
    }

    f->period = best_p;
    f->have_prior = 0;
    f->pos = wrap_pos(f->abs_pos, f->period);
    memset(f->hits, 0, sizeof(f->hits));
    memset(f->ghosts, 0, sizeof(f->ghosts));
}

void filter_align_prior(filter_t *f)
{
    int p = f->period;

    if (!f->have_prior || f->prior_period != p || f->nghost_log < 10) {
        return;
    }

    unsigned long prior_total = 0;
    long bins[MAX_SECTORS];

    memset(bins, 0, sizeof(bins));
    for (int i = 0; i < p; i++) {
        prior_total += f->prior_ghosts[i];
    }
    for (int i = 0; i < f->nghost_log; i++) {
        bins[wrap_pos(f->ghost_log[i], p)]++;
    }
    if (prior_total < 10) {
        f->have_prior = 0;
        return;
    }

    /* acha a rotação que melhor casa os ghosts desta sessão com os salvos:
     * a posição inicial da roda é arbitrária a cada sessão */
    double best = -1.0, total = 0.0;
    int best_shift = 0;

    for (int sh = 0; sh < p; sh++) {
        double sc = 0.0;

        for (int n = 0; n < p; n++) {
            sc += (double)bins[n] * (double)f->prior_ghosts[(n + sh) % p];
        }
        total += sc;
        if (sc > best) {
            best = sc;
            best_shift = sh;
        }
    }

    double mean = total / p;

    if (best < 2.0 * mean) {
        return;     /* sem casamento claro: espera mais ghosts */
    }

    for (int n = 0; n < p; n++) {
        f->hits[n] += f->prior_hits[(n + best_shift) % p];
        f->ghosts[n] += f->prior_ghosts[(n + best_shift) % p];
    }
    f->have_prior = 0;
}

static void log_ghost(filter_t *f, int abs)
{
    if (f->nghost_log < GHOST_LOG) {
        f->ghost_log[f->nghost_log++] = abs;
    } else {
        memmove(f->ghost_log, f->ghost_log + 1, (GHOST_LOG - 1) * sizeof(int));
        f->ghost_log[GHOST_LOG - 1] = abs;
    }
    f->since_tune++;
    filter_align_prior(f);
    maybe_retune_period(f);
}

static int cmp_dbl(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* Inversão real confirmada: aprende o menor tempo humano de inversão. */
static void learn_human(filter_t *f, double gap)
{
    f->rev_gaps[f->rev_head] = gap;
    f->rev_head = (f->rev_head + 1) % REV_SAMPLES;
    if (f->nrev_gaps < REV_SAMPLES) {
        f->nrev_gaps++;
    }
    if (f->nrev_gaps < 8) {
        return;
    }

    double tmp[REV_SAMPLES];
    memcpy(tmp, f->rev_gaps, sizeof(tmp));
    qsort(tmp, (size_t)f->nrev_gaps, sizeof(double), cmp_dbl);

    double p5 = tmp[f->nrev_gaps / 20] * 0.8;
    f->human_min = p5 < 0.020 ? 0.020 : (p5 > 0.250 ? 0.250 : p5);
}

/* Acompanha os ticks que passaram: ritmo da rolagem, inversões aceitas e
 * rótulos retroativos. Chamado antes de abs_pos avançar. */
static void track_pass(filter_t *f, const tick_t *t, int dir)
{
    double ts = t->ts;

    /* após inatividade é um começo novo, não uma inversão */
    if (f->acc_dir != 0 && ts - f->acc_ts > f->idle_s) {
        f->acc_dir = 0;
        f->rev.active = 0;
    }

    if (f->acc_dir != 0 && dir != f->acc_dir) {
        if (f->rev.active && f->rev.ticks <= 2 && dir == f->rev.from &&
            ts - f->rev.ts < REV_BACK_S) {
            /* a direção antiga voltou logo: a inversão anterior era um
             * ghost que passou; desfaz o deslocamento que ele causou */
            f->abs_pos -= f->rev.to * f->rev.ticks;
            if (f->ghosts[f->rev.sector] < f->hits[f->rev.sector]) {
                f->ghosts[f->rev.sector]++;
            }
            f->leaks++;
            log_ghost(f, f->rev.abs);
        }
        f->rev = (reversal_t){ 1, f->acc_dir, dir, 1, f->pos, f->abs_pos, ts,
                               ts - f->acc_ts };
        f->run = 1;
    } else if (f->acc_dir == dir) {
        double g = ts - f->acc_ts;

        if (g < CRUISE_GAP_S) {
            f->run++;
            f->ewma_gap = f->ewma_gap > 0.0 ? 0.8 * f->ewma_gap + 0.2 * g : g;
        } else {
            f->run = 1;
        }
        if (f->rev.active && dir == f->rev.to && ++f->rev.ticks == 3) {
            learn_human(f, f->rev.gap);
        }
    } else {
        f->run = 1;
    }

    f->acc_dir = dir;
    f->acc_ts = ts;
}

static void emit_tick(filter_t *f, const tick_t *t, int pass)
{
    tick_t out = *t;
    int dir = sign(t->value);

    out.sector = f->pos;
    out.period = f->period;

    if (dir != 0 && f->period > 0 && f->pos >= 0 && f->pos < MAX_SECTORS) {
        f->hits[f->pos]++;
        if (f->hits[f->pos] >= SECTOR_DECAY) {
            f->hits[f->pos] /= 2;
            f->ghosts[f->pos] /= 2;
        }
        if (!pass) {
            f->ghosts[f->pos]++;
            log_ghost(f, f->abs_pos);
        } else {
            track_pass(f, t, dir);
            f->abs_pos += dir;
            f->pos = wrap_pos(f->abs_pos, f->period);
        }
    }

    f->cb(f->ctx, &out, pass);
}

static void release_pending(filter_t *f, int pass)
{
    int n = f->npending;
    f->npending = 0;

    for (int i = 0; i < n; i++) {
        emit_tick(f, &f->pending[i], pass);
    }
}

void filter_set_params(filter_t *f, int confirm, double flush_ms,
                       double idle_ms)
{
    if (confirm < 1) {
        confirm = 1;
    }
    if (confirm > MAX_PENDING) {
        confirm = MAX_PENDING;
    }
    if (flush_ms < 10.0) {
        flush_ms = 10.0;
    }
    if (flush_ms > 5000.0) {
        flush_ms = 5000.0;
    }
    if (idle_ms < flush_ms) {
        idle_ms = flush_ms;
    }
    if (idle_ms > 10000.0) {
        idle_ms = 10000.0;
    }

    f->confirm = confirm;
    f->flush_s = flush_ms / 1000.0;
    f->idle_s = idle_ms / 1000.0;
}

void filter_init(filter_t *f, int confirm, double flush_ms, double idle_ms,
                 verdict_fn cb, void *ctx)
{
    memset(f, 0, sizeof(*f));
    filter_set_params(f, confirm, flush_ms, idle_ms);
    f->period = DEFAULT_PERIOD;
    f->human_min = DEFAULT_HUMAN_MIN;
    f->cb = cb;
    f->ctx = ctx;
}

void filter_reset(filter_t *f)
{
    f->last_dir = 0;
    f->last_ts = 0.0;
    f->npending = 0;
    f->acc_dir = 0;
    f->acc_ts = 0.0;
    f->run = 0;
    f->rev.active = 0;
}

/* Inversão em espera por tempo demais: no setor limpo era real; no setor
 * quente (ghost recorrente naquele detente) descarta. */
void filter_expire(filter_t *f, double now)
{
    if (f->npending == 0 || now - f->pending[0].ts < f->flush_s) {
        return;
    }

    if (filter_sector_hot(f, f->pos) ||
        (f->confirm > 1 && suspicious(f, f->pending[0].ts))) {
        release_pending(f, 0);
        return;
    }

    f->last_dir = sign(f->pending[0].value);
    release_pending(f, 1);
}

void filter_feed(filter_t *f, const tick_t *t)
{
    filter_expire(f, t->ts);

    int dir = sign(t->value);
    double gap = t->ts - f->last_ts;
    f->last_ts = t->ts;

    if (dir == 0) {
        emit_tick(f, t, 1);
        return;
    }

    if (f->last_dir == 0 || gap > f->idle_s) {
        f->last_dir = dir;
        emit_tick(f, t, 1);
        return;
    }

    if (dir == f->last_dir) {
        release_pending(f, 0);   /* o que estava em espera era ruído */
        emit_tick(f, t, 1);
        return;
    }

    /* direção oposta à aceita: segura até confirmar (mais ticks se o
     * detente atual for um setor quente) */
    if (f->npending < MAX_PENDING) {
        f->pending[f->npending++] = *t;
    }

    int need = filter_sector_confirm(f);

    /* no meio de uma rolagem contínua, uma inversão rápida demais para um
     * humano exige mais confirmações */
    if (f->confirm > 1 && f->npending > 0 &&
        filter_in_cruise(f, f->pending[0].ts) &&
        suspicious(f, f->pending[0].ts)) {
        need = min_int(need + CRUISE_EXTRA, MAX_PENDING);
    }

    if (f->npending >= need || f->npending == MAX_PENDING) {
        f->last_dir = dir;
        release_pending(f, 1);
    }
}

/* ------------------------------------------------------------------ */
/* persistência                                                        */
/* ------------------------------------------------------------------ */

void filter_save(const filter_t *f, FILE *fp)
{
    /* sem alinhamento ainda, o que veio do arquivo vale mais que o parcial */
    int use_prior = f->have_prior;
    int p = use_prior ? f->prior_period : f->period;
    const unsigned long *h = use_prior ? f->prior_hits : f->hits;
    const unsigned long *g = use_prior ? f->prior_ghosts : f->ghosts;

    fprintf(fp, "wheel-filter-state 1\nperiod %d\nhuman_min %.6f\n"
                "ewma %.6f\nhits", p, f->human_min, f->ewma_gap);
    for (int i = 0; i < p; i++) {
        fprintf(fp, " %lu", h[i]);
    }
    fprintf(fp, "\nghosts");
    for (int i = 0; i < p; i++) {
        fprintf(fp, " %lu", g[i]);
    }
    fprintf(fp, "\ngaps %d", f->nrev_gaps);
    for (int i = 0; i < f->nrev_gaps; i++) {
        fprintf(fp, " %.6f", f->rev_gaps[i]);
    }
    fprintf(fp, "\n");
}

int filter_load(filter_t *f, FILE *fp)
{
    int ver = 0, p = 0, ng = 0;
    double hm = 0.0, ew = 0.0;
    unsigned long hits[MAX_SECTORS] = {0}, gh[MAX_SECTORS] = {0};
    double gaps[REV_SAMPLES] = {0};

    if (fscanf(fp, " wheel-filter-state %d", &ver) != 1 || ver != 1 ||
        fscanf(fp, " period %d", &p) != 1 || p < 1 || p > MAX_SECTORS ||
        fscanf(fp, " human_min %lf", &hm) != 1 ||
        fscanf(fp, " ewma %lf", &ew) != 1 || fscanf(fp, " hits") != 0) {
        return 0;
    }
    for (int i = 0; i < p; i++) {
        if (fscanf(fp, " %lu", &hits[i]) != 1) {
            return 0;
        }
    }
    if (fscanf(fp, " ghosts") != 0) {
        return 0;
    }
    for (int i = 0; i < p; i++) {
        if (fscanf(fp, " %lu", &gh[i]) != 1 || gh[i] > hits[i]) {
            return 0;
        }
    }
    if (fscanf(fp, " gaps %d", &ng) != 1 || ng < 0 || ng > REV_SAMPLES) {
        return 0;
    }
    for (int i = 0; i < ng; i++) {
        if (fscanf(fp, " %lf", &gaps[i]) != 1) {
            return 0;
        }
    }

    f->human_min = hm < 0.020 ? 0.020 : (hm > 0.250 ? 0.250 : hm);
    f->ewma_gap = (ew > 0.0 && ew < CRUISE_GAP_S) ? ew : 0.0;
    memcpy(f->rev_gaps, gaps, (size_t)ng * sizeof(double));
    f->nrev_gaps = ng;
    f->rev_head = ng % REV_SAMPLES;

    /* a posição inicial da roda muda a cada sessão: a tabela espera o
     * alinhamento por casamento dos ghosts novos */
    memset(f->hits, 0, sizeof(f->hits));
    memset(f->ghosts, 0, sizeof(f->ghosts));
    f->nghost_log = 0;
    f->abs_pos = 0;
    f->period = p;
    f->pos = 0;
    f->prior_period = p;
    memcpy(f->prior_hits, hits, sizeof(f->prior_hits));
    memcpy(f->prior_ghosts, gh, sizeof(f->prior_ghosts));
    f->have_prior = 1;
    return 1;
}
