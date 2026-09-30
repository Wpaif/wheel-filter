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

    printf("%s\n", failures ? "HÁ FALHAS" : "todos os testes passaram");
    return failures ? 1 : 0;
}
