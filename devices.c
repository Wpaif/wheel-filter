#define _GNU_SOURCE
#include "devices.h"

#include <limits.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VIRTUAL_PREFIX "Wheel Filter"

static void find_stable_path(mouse_info_t *m)
{
    const char *dirs[] = { "/dev/input/by-id", "/dev/input/by-path", NULL };
    char best[320] = "";
    const char *best_kind = "";
    int best_score = 0;

    for (int d = 0; dirs[d]; d++) {
        GDir *dir = g_dir_open(dirs[d], 0, NULL);

        if (!dir) {
            continue;
        }

        const char *name;

        while ((name = g_dir_read_name(dir)) != NULL) {
            char full[300];
            snprintf(full, sizeof(full), "%s/%s", dirs[d], name);

            char *real = realpath(full, NULL);

            if (!real) {
                continue;
            }

            int match = strcmp(real, m->event_path) == 0;
            free(real);

            if (!match) {
                continue;
            }

            int score = 1;

            if (g_str_has_suffix(name, "-event-mouse")) {
                score = 3;
            } else if (strstr(name, "event-mouse")) {
                score = 2;
            }

            if (d == 0) {
                score += 10;    /* by-id é o mais estável */
            }

            if (score > best_score) {
                best_score = score;
                best_kind = d == 0 ? "by-id" : "by-path";
                snprintf(best, sizeof(best), "%s", full);
            }
        }

        g_dir_close(dir);
    }

    if (best_score > 0) {
        snprintf(m->stable_path, sizeof(m->stable_path), "%s", best);
        snprintf(m->stable_kind, sizeof(m->stable_kind), "%s", best_kind);
    } else {
        snprintf(m->stable_path, sizeof(m->stable_path), "%s", m->event_path);
        snprintf(m->stable_kind, sizeof(m->stable_kind), "event");
    }
}

static void finish_block(GPtrArray *out, mouse_info_t *cur, int have_event,
                         unsigned long long rel)
{
    int has_x = (rel >> REL_X) & 1;
    int has_y = (rel >> REL_Y) & 1;

    if (!have_event || !has_x || !has_y) {
        return;
    }

    if (strncmp(cur->name, VIRTUAL_PREFIX, strlen(VIRTUAL_PREFIX)) == 0) {
        return;                 /* nosso próprio mouse virtual */
    }

    cur->has_wheel = (rel >> REL_WHEEL) & 1;
    cur->has_hires = (rel >> REL_WHEEL_HI_RES) & 1;
    find_stable_path(cur);

    mouse_info_t *copy = g_new(mouse_info_t, 1);
    *copy = *cur;
    g_ptr_array_add(out, copy);
}

GPtrArray *devices_list(void)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    gchar *text = NULL;

    /* WHEEL_FILTER_DEVICES_FILE existe só para testes */
    const char *src = g_getenv("WHEEL_FILTER_DEVICES_FILE");

    if (!g_file_get_contents(src ? src : "/proc/bus/input/devices", &text,
                             NULL, NULL)) {
        return out;
    }

    gchar **lines = g_strsplit(text, "\n", -1);
    mouse_info_t cur;
    int have_event = 0;
    unsigned long long rel = 0;

    memset(&cur, 0, sizeof(cur));

    for (int i = 0; lines[i]; i++) {
        const char *l = lines[i];

        if (l[0] == '\0') {
            finish_block(out, &cur, have_event, rel);
            memset(&cur, 0, sizeof(cur));
            have_event = 0;
            rel = 0;
        } else if (g_str_has_prefix(l, "I: ")) {
            sscanf(l, "I: Bus=%*x Vendor=%x Product=%x", &cur.vendor,
                   &cur.product);
        } else if (g_str_has_prefix(l, "N: Name=")) {
            const char *a = strchr(l, '"');
            const char *b = strrchr(l, '"');

            if (a && b && b > a) {
                g_strlcpy(cur.name, a + 1,
                          MIN((gsize)(b - a), sizeof(cur.name)));
            }
        } else if (g_str_has_prefix(l, "H: Handlers=")) {
            const char *e = strstr(l, "event");
            int n;

            if (e && sscanf(e, "event%d", &n) == 1) {
                snprintf(cur.event_path, sizeof(cur.event_path),
                         "/dev/input/event%d", n);
                have_event = 1;
            }
        } else if (g_str_has_prefix(l, "B: REL=")) {
            const char *val = l + strlen("B: REL=");
            const char *last = strrchr(val, ' ');   /* palavra menos significativa */
            rel = strtoull(last ? last + 1 : val, NULL, 16);
        }
    }

    finish_block(out, &cur, have_event, rel);

    g_strfreev(lines);
    g_free(text);
    return out;
}
