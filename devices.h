/*
 * Descoberta de mouses (sem precisar de root): lê /proc/bus/input/devices e
 * resolve um caminho estável em /dev/input/by-id (ou by-path).
 */
#ifndef WHEEL_FILTER_DEVICES_H
#define WHEEL_FILTER_DEVICES_H

#include <glib.h>

typedef struct {
    char name[128];
    char event_path[64];     /* /dev/input/event7 */
    char stable_path[320];   /* /dev/input/by-id/...-event-mouse (ou o próprio event) */
    char stable_kind[16];    /* "by-id", "by-path" ou "event" */
    unsigned vendor;
    unsigned product;
    int has_wheel;           /* REL_WHEEL */
    int has_hires;           /* REL_WHEEL_HI_RES */
} mouse_info_t;

/* Lista dispositivos com REL_X e REL_Y (mouses). Libere com g_ptr_array_unref. */
GPtrArray *devices_list(void);

#endif
