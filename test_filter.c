#include <stdio.h>
#include <string.h>

#include "filter.h"

#define MAX_OUT 64

typedef struct {
    int n;
    double ts[MAX_OUT];
    int value[MAX_OUT];
    int pass[MAX_OUT];
} rec_t;

static int failures = 0;

static void rec_cb(void *ctx, const tick_t *t, int pass)
{
    rec_t *r = ctx;

    if (r->n < MAX_OUT) {
        r->ts[r->n] = t->ts;
        r->value[r->n] = t->value;
        r->pass[r->n] = pass;
        r->n++;
    }
}

static void feed(filter_t *f, double ts, int value)
{
    tick_t t = { ts, value, value, value * 120, -1.0 };
    filter_feed(f, &t);
}

/* Confere a sequência de vereditos: string com 'p' (passou) e 'd' (descartado) */
static void expect(const char *name, const rec_t *r, const char *verdicts)
{
    char got[MAX_OUT + 1];

    for (int i = 0; i < r->n; i++) {
        got[i] = r->pass[i] ? 'p' : 'd';
    }
    got[r->n] = '\0';

    if (strcmp(got, verdicts) != 0) {
        printf("FALHOU  %-34s esperado=%s obtido=%s\n", name, verdicts, got);
        failures++;
    } else {
        printf("ok      %s\n", name);
    }
}

int main(void)
{
    rec_t r;
    filter_t f;

    memset(&r, 0, sizeof(r));
    filter_init(&f, 2, 350, 500, rec_cb, &r);
    feed(&f, 0.00, -1); feed(&f, 0.03, -1); feed(&f, 0.06, +1); feed(&f, 0.12, -1);
    expect("ghost isolado é descartado", &r, "ppdp");

    memset(&r, 0, sizeof(r));
    filter_init(&f, 2, 350, 500, rec_cb, &r);
    feed(&f, 0.00, -1); feed(&f, 0.03, -1); feed(&f, 0.06, +1); feed(&f, 0.09, +1);
    expect("inversão confirmada passa", &r, "pppp");
    if (r.n == 4 && (r.ts[2] != 0.06 || r.ts[3] != 0.09)) {
        printf("FALHOU  ordem dos ticks liberados\n");
        failures++;
    }

    memset(&r, 0, sizeof(r));
    filter_init(&f, 2, 350, 500, rec_cb, &r);
    feed(&f, 0.0, -1); feed(&f, 1.0, +1);
    expect("após inatividade passa direto", &r, "pp");

    memset(&r, 0, sizeof(r));
    filter_init(&f, 2, 350, 500, rec_cb, &r);
    feed(&f, 0.0, -1); feed(&f, 0.1, +1); filter_expire(&f, 0.5);
    expect("inversão isolada expira e passa", &r, "pp");

    memset(&r, 0, sizeof(r));
    filter_init(&f, 2, 350, 500, rec_cb, &r);
    feed(&f, 0.0, -1); feed(&f, 0.1, +1); filter_expire(&f, 0.3);
    expect("antes do prazo continua em espera", &r, "p");

    memset(&r, 0, sizeof(r));
    filter_init(&f, 1, 350, 500, rec_cb, &r);
    feed(&f, 0.0, -1); feed(&f, 0.03, +1); feed(&f, 0.06, -1);
    expect("confirm=1 não filtra nada", &r, "ppp");

    memset(&r, 0, sizeof(r));
    filter_init(&f, 3, 350, 500, rec_cb, &r);
    feed(&f, 0.0, -1); feed(&f, 0.03, -1); feed(&f, 0.06, +1); feed(&f, 0.09, +1);
    feed(&f, 0.12, -1);
    expect("rajada de 2 ghosts com confirm=3", &r, "ppddp");

    memset(&r, 0, sizeof(r));
    filter_init(&f, 2, 350, 500, rec_cb, &r);
    feed(&f, 0.0, +1); feed(&f, 0.03, +1); feed(&f, 0.06, -1); filter_expire(&f, 0.2);
    feed(&f, 0.25, +1);
    expect("ghost seguido de retorno (subindo)", &r, "ppdp");

    memset(&r, 0, sizeof(r));
    filter_init(&f, 999, -5, 99999, rec_cb, &r);
    if (f.confirm != MAX_PENDING || f.flush_s != 0.010 || f.idle_s != 10.0) {
        printf("FALHOU  validação de parâmetros\n");
        failures++;
    } else {
        printf("ok      parâmetros absurdos são ajustados\n");
    }

    memset(&r, 0, sizeof(r));
    filter_init(&f, 2, 350, 500, rec_cb, &r);
    feed(&f, 0.0, -1);
    f.hits[f.pos] = 20;
    f.ghosts[f.pos] = 10;
    feed(&f, 0.1, +1);
    filter_expire(&f, 0.5);
    expect("setor quente: inversão isolada cai", &r, "pd");

    memset(&r, 0, sizeof(r));
    filter_init(&f, 1, 350, 500, rec_cb, &r);
    feed(&f, 0.0, -1);
    f.hits[f.pos] = 30;
    f.ghosts[f.pos] = 12;
    feed(&f, 0.03, +1);
    expect("setor quente exige mais confirmação", &r, "p");

    memset(&r, 0, sizeof(r));
    filter_init(&f, 2, 350, 500, rec_cb, &r);
    feed(&f, 0.00, -1);
    feed(&f, 0.03, -1);
    feed(&f, 0.06, +1);
    feed(&f, 0.12, -1);
    if (f.ghosts[22] != 1 && f.period == 24) {
        /* pos após dois -1 a partir de 0 é 22; o ghost fica nesse detente */
        int found = 0;
        for (int i = 0; i < f.period; i++) {
            found += (int)f.ghosts[i];
        }
        if (found != 1) {
            printf("FALHOU  contagem de ghosts por setor (%d)\n", found);
            failures++;
        } else {
            printf("ok      ghost contabilizado no setor\n");
        }
    } else {
        printf("ok      ghost contabilizado no setor\n");
    }

    /* rolagem contínua: inversão rápida demais exige mais confirmações */
    memset(&r, 0, sizeof(r));
    filter_init(&f, 2, 350, 500, rec_cb, &r);
    for (int i = 0; i < 6; i++) {
        feed(&f, 0.03 * i, -1);
    }
    feed(&f, 0.18, +1); feed(&f, 0.21, +1);
    expect("rolagem contínua: 2 inversões não bastam", &r, "pppppp");
    if (f.npending != 2) {
        printf("FALHOU  inversão deveria seguir em espera\n");
        failures++;
    }

    memset(&r, 0, sizeof(r));
    filter_init(&f, 2, 350, 500, rec_cb, &r);
    for (int i = 0; i < 6; i++) {
        feed(&f, 0.03 * i, -1);
    }
    feed(&f, 0.18, +1);
    filter_expire(&f, 0.60);
    expect("rolagem contínua: ghost isolado cai", &r, "ppppppd");

    memset(&r, 0, sizeof(r));
    filter_init(&f, 2, 350, 500, rec_cb, &r);
    for (int i = 0; i < 6; i++) {
        feed(&f, 0.03 * i, -1);
    }
    feed(&f, 0.40, +1);
    filter_expire(&f, 0.80);
    expect("rolagem contínua: inversão após pausa passa", &r, "ppppppp");

    /* piso humano aprendido com inversões reais confirmadas */
    memset(&r, 0, sizeof(r));
    filter_init(&f, 2, 350, 500, rec_cb, &r);
    double t0 = 0.0;
    for (int k = 0; k < 10; k++) {
        feed(&f, t0, -1); feed(&f, t0 + 0.2, +1); feed(&f, t0 + 0.23, +1);
        feed(&f, t0 + 0.26, +1);
        t0 += 2.0;
    }
    if (f.human_min < 0.15 || f.human_min > 0.17) {
        printf("FALHOU  piso humano aprendido (%.3f)\n", f.human_min);
        failures++;
    } else {
        printf("ok      piso humano aprendido\n");
    }

    /* ghost que passou é rotulado quando a direção antiga volta */
    memset(&r, 0, sizeof(r));
    filter_init(&f, 1, 350, 500, rec_cb, &r);
    feed(&f, 0.00, -1); feed(&f, 0.05, -1); feed(&f, 0.10, +1);
    feed(&f, 0.15, -1);
    if (f.leaks != 1 || f.abs_pos != -3) {
        printf("FALHOU  vazamento rotulado (leaks=%lu abs=%d)\n", f.leaks,
               f.abs_pos);
        failures++;
    } else {
        printf("ok      ghost que passou é rotulado e abs_pos corrigido\n");
    }

    /* persistência: salvar, carregar e alinhar a rotação da roda */
    memset(&r, 0, sizeof(r));
    filter_init(&f, 2, 350, 500, rec_cb, &r);
    f.human_min = 0.120;
    for (int i = 0; i < 24; i++) {
        f.hits[i] = 40;
        f.ghosts[i] = (i == 5 || i == 6) ? 20 : 0;
    }
    FILE *fp = tmpfile();
    filter_save(&f, fp);
    rewind(fp);

    filter_t g;
    memset(&r, 0, sizeof(r));
    filter_init(&g, 2, 350, 500, rec_cb, &r);
    if (!filter_load(&g, fp) || g.human_min != 0.120 || !g.have_prior) {
        printf("FALHOU  carregar estado\n");
        failures++;
    } else {
        /* nesta sessão a roda começou 3 detentes adiante: ghosts em 2 e 3 */
        for (int i = 0; i < 12; i++) {
            g.ghost_log[g.nghost_log++] = (i % 2) ? 2 : 3;
        }
        g.since_tune = 0;
        g.ghost_log[g.nghost_log++] = 2;
        filter_align_prior(&g);
        if (g.have_prior || g.ghosts[2] != 20 || g.ghosts[3] != 20 ||
            g.ghosts[5] != 0) {
            printf("FALHOU  alinhamento da tabela salva\n");
            failures++;
        } else {
            printf("ok      estado salvo é carregado e alinhado\n");
        }
    }
    fclose(fp);

    printf("%s\n", failures ? "HÁ FALHAS" : "todos os testes passaram");
    return failures ? 1 : 0;
}
