/*
 * Motor do filtro: decide, tick a tick, o que é rolagem real e o que é
 * "ghost" (inversão espúria). Não depende de nenhuma E/S.
 */
#ifndef WHEEL_FILTER_H
#define WHEEL_FILTER_H

#define MAX_PENDING 16

typedef struct {
    double ts;      /* segundos (epoch) */
    int value;      /* direção/valor usado na decisão */
    int wheel;      /* valor de REL_WHEEL a reemitir */
    int hires;      /* valor de REL_WHEEL_HI_RES a reemitir */
    double gap_ms;  /* tempo desde o tick anterior; < 0 = desconhecido */
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

/* Parâmetros fora da faixa são ajustados: c 1..16, f 10..5000, i f..10000 */
void filter_init(filter_t *f, int confirm, double flush_ms, double idle_ms,
                 verdict_fn cb, void *ctx);
void filter_set_params(filter_t *f, int confirm, double flush_ms,
                       double idle_ms);
void filter_reset(filter_t *f);
void filter_feed(filter_t *f, const tick_t *t);
void filter_expire(filter_t *f, double now);

#endif
