#include "filter.h"

#include <string.h>

static int sign(int v) { return (v > 0) - (v < 0); }

static void release_pending(filter_t *f, int pass)
{
    int n = f->npending;
    f->npending = 0;

    for (int i = 0; i < n; i++) {
        f->cb(f->ctx, &f->pending[i], pass);
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
    f->cb = cb;
    f->ctx = ctx;
}

void filter_reset(filter_t *f)
{
    f->last_dir = 0;
    f->last_ts = 0.0;
    f->npending = 0;
}

/* Inversão em espera por tempo demais: era real, deixa passar. */
void filter_expire(filter_t *f, double now)
{
    if (f->npending == 0 || now - f->pending[0].ts < f->flush_s) {
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
