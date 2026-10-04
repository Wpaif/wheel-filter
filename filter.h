/*
 * Motor do filtro: decide, tick a tick, o que é rolagem real e o que é
 * "ghost" (inversão espúria). Não depende de nenhuma E/S.
 *
 * Além da regra de confirmação temporal, aprende em quais setores do
 * leito da roda (detentes) o ghost se concentra e fica mais rigoroso
 * nesses ângulos.
 */
#ifndef WHEEL_FILTER_H
#define WHEEL_FILTER_H

#include <stdio.h>

#define MAX_PENDING 16
#define MAX_SECTORS 36
#define GHOST_LOG 48
#define DEFAULT_PERIOD 24
#define REV_SAMPLES 32          /* gaps de inversões reais lembrados */
#define DEFAULT_HUMAN_MIN 0.050 /* menor tempo humano plausível p/ inverter (s) */

typedef struct {
    double ts;      /* segundos (epoch) */
    int value;      /* direção/valor usado na decisão */
    int wheel;      /* valor de REL_WHEEL a reemitir */
    int hires;      /* valor de REL_WHEEL_HI_RES a reemitir */
    double gap_ms;  /* tempo desde o tick anterior; < 0 = desconhecido */
    int sector;     /* detente atual 0..period-1 no momento do veredito */
    int period;     /* quantos detentes o filtro está usando */
} tick_t;

/* Última inversão aceita, acompanhada para rotular o resultado depois:
 * se a direção antiga voltar logo, era um ghost que passou (vazamento). */
typedef struct {
    int active;
    int from, to;       /* direções antes/depois */
    int ticks;          /* ticks aceitos na nova direção até agora */
    int sector;         /* detente em que ocorreu */
    int abs;            /* abs_pos no início da inversão */
    double ts;
    double gap;         /* tempo entre o último tick antigo e a inversão */
} reversal_t;

typedef void (*verdict_fn)(void *ctx, const tick_t *t, int pass);

typedef struct {
    int confirm;
    double flush_s;
    double idle_s;

    int last_dir;
    double last_ts;

    tick_t pending[MAX_PENDING];
    int npending;

    int period;                 /* detentes por volta (16..36, padrão 24) */
    int pos;                    /* setor atual 0..period-1 */
    int abs_pos;                /* posição acumulada só com ticks aceitos */
    unsigned long hits[MAX_SECTORS];
    unsigned long ghosts[MAX_SECTORS];
    int ghost_log[GHOST_LOG];   /* abs_pos no momento de cada ghost */
    int nghost_log;
    int since_tune;

    /* rolagem contínua: sequência recente de ticks aceitos na mesma direção */
    int acc_dir;                /* direção do último tick aceito (0 = nenhum) */
    double acc_ts;
    int run;                    /* ticks seguidos no ritmo de rolagem */
    double ewma_gap;            /* intervalo típico entre ticks (s) */

    /* aprendizado do usuário */
    reversal_t rev;
    double rev_gaps[REV_SAMPLES];   /* gaps de inversões reais confirmadas */
    int nrev_gaps, rev_head;
    double human_min;           /* piso aprendido do gap de inversão real (s) */
    unsigned long leaks;        /* ghosts que passaram e foram rotulados depois */

    /* tabela de setores salva de outra sessão, ainda sem alinhar à roda */
    int have_prior;
    int prior_period;
    unsigned long prior_hits[MAX_SECTORS];
    unsigned long prior_ghosts[MAX_SECTORS];

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

/* Rolagem contínua (ex.: ler um PDF) em andamento no instante ts? */
int filter_in_cruise(const filter_t *f, double ts);

/* Estado aprendido (setores, piso humano, ritmo) em arquivo texto.
 * load devolve 1 se leu um estado válido, 0 caso contrário. */
void filter_save(const filter_t *f, FILE *fp);
int filter_load(filter_t *f, FILE *fp);
/* Tenta casar a tabela salva com os ghosts desta sessão (automático). */
void filter_align_prior(filter_t *f);

int filter_sector_confirm(const filter_t *f);
int filter_sector_hot(const filter_t *f, int sector);

#endif
