#ifndef WHEEL_FILTER_TRAY_H
#define WHEEL_FILTER_TRAY_H

#include <glib.h>

#include "devices.h"

typedef struct tray tray_t;

typedef struct {
    void (*show_window)(void *user);
    void (*quit)(void *user);
    void (*select_device)(void *user, int idx);
    void (*set_confirm)(void *user, int v);
    void (*set_flush)(void *user, int v);
    void (*set_idle)(void *user, int v);
    void (*toggle_filter)(void *user);
    void *user;
} tray_cbs_t;

tray_t *tray_start(const tray_cbs_t *cbs);
void tray_update(tray_t *t, GPtrArray *devs, int selected, int confirm,
                 int flush_ms, int idle_ms, gboolean running,
                 gboolean can_start);
void tray_set_status(tray_t *t, const char *tooltip);
gboolean tray_is_registered(const tray_t *t);
void tray_stop(tray_t *t);

#endif
