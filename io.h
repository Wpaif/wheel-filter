/*
 * Sessão ao vivo: lê um mouse (evdev), passa a roda pelo filtro e recria o
 * mouse via uinput. Reconecta sozinha se o mouse sumir e voltar.
 */
#ifndef WHEEL_FILTER_IO_H
#define WHEEL_FILTER_IO_H

#include <stddef.h>
#include <stdio.h>

#include "filter.h"

typedef void (*tick_cb)(void *user, const tick_t *t, int pass);
/* state: "waiting", "filtering", "observing", "error", "stopped" */
typedef void (*state_cb)(void *user, const char *state, const char *msg);

typedef struct {
    const char *path;   /* caminho fixo (de preferência /dev/input/by-id/...) */
    const char *name;   /* ou trecho do nome do mouse (usado se path == NULL) */
    int confirm;
    double flush_ms;
    double idle_ms;
    int observe;        /* 1: só observa, não captura nem filtra */
    int ctl_fd;         /* -1, ou fd com comandos "STOP" e "SET c f i" */
    FILE *logf;         /* NULL ou arquivo com os ticks descartados */
    tick_cb on_tick;    /* pode ser NULL */
    state_cb on_state;  /* pode ser NULL */
    void *user;
} session_cfg_t;

/* Retorna EXIT_SUCCESS ao ser parada (sinal/STOP/EOF do ctl_fd) e
 * EXIT_FAILURE em erro fatal (sem permissão, mouse já capturado...). */
int session_run(const session_cfg_t *cfg);

double now_realtime(void);
void fmt_time(double ts, char *buf, size_t n);

#endif
