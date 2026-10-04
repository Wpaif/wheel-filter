/*
 * Interface gráfica (GTK 4) do Wheel Filter.
 *
 * A janela roda sem privilégios. O filtro em si roda como root num processo
 * separado ("wheel-filter --engine ..."), iniciado via pkexec, e conversa com
 * a janela por texto (linhas STATE/TICK no stdout; STOP/SET no stdin). Se a
 * janela fechar ou travar, o stdin do motor fecha e ele solta o mouse sozinho.
 */

#include <adwaita.h>
#include <gtk/gtk.h>
#include <gio/gio.h>
#include <glib/gstdio.h>

#include <math.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "devices.h"
#include "filter.h"
#include "gui.h"
#include "tray.h"

static const char *APP_CSS =
    "label.success { color: @success_color; }\n"
    "label.warning { color: @warning_color; }\n"
    "label.error { color: @error_color; }\n"
    ".dim-label { opacity: 0.55; }\n"
    ".stat-tile { padding: 12px 14px; border-radius: 14px; }\n"
    ".stat-value { font-size: 26px; font-weight: 800;\n"
    "              font-feature-settings: 'tnum'; transition: color 450ms ease; }\n"
    ".stat-value.flash { color: @error_color; transition: color 60ms ease; }\n"
    ".ring-pct { font-size: 28px; font-weight: 800; }\n"
    ".status-pill { padding: 3px 12px; border-radius: 999px;\n"
    "               background: alpha(currentColor, 0.08); }\n"
    ".banner { padding: 8px 12px; border-radius: 12px;\n"
    "          background: alpha(@warning_color, 0.15); }\n"
    ".log-card { border-radius: 14px; padding: 2px; }\n"
    ".log-card textview, .log-card text { background: transparent; }\n"
    "button.start-btn { padding: 6px 18px; min-height: 0; }\n";

#define APP_ID "org.wheelfilter.App"
#define LOG_MAX (4 * 1024 * 1024)
#define MON_MAX_LINES 600
#define MON_TRIM_LINES 200

typedef struct {
    GtkApplication *gapp;
    GtkWidget *win;
    char *self_path;

    /* dispositivos */
    GPtrArray *devs;
    char *dev_signature;
    GtkWidget *dev_box;
    GtkWidget *dev_grid;
    int selected;
    char *selected_stable;
    char *last_key;                 /* "vvvv:pppp" da última escolha salva */
    GtkWidget *sel_name;
    GtkWidget *sel_path;
    GtkWidget *sel_wheel;

    /* configuração */
    GtkWidget *sp_confirm;
    GtkWidget *sp_flush;
    GtkWidget *sp_idle;
    GtkWidget *chk_observe;
    gboolean loading;

    /* controle */
    GtkWidget *status;
    GtkWidget *btn_start;
    GtkWidget *service_warn;

    /* monitor */
    GtkWidget *tile[5];             /* recebidos, liberados, filtrados, taxa, tempo */
    GtkWidget *ring_lbl;
    double pulse;                   /* 1 -> 0 depois de cada tick */
    gint64 pulse_t;
    guint flash_id;
    GtkWidget *monitor;
    GtkTextBuffer *buf;
    GtkTextMark *end_mark;
    GtkWidget *chk_ghosts;
    int mon_lines;

    /* sessão */
    GSubprocess *proc;
    GDataInputStream *out;
    GOutputStream *in;
    gboolean running;
    long rx;
    long pass;
    long ghosts;
    gint64 t0;
    gint64 last_secs;
    guint uptime_id;
    char *last_error;
    GString *log;

    /* config em disco */
    GKeyFile *cfg;
    char *cfg_path;

    tray_t *tray;
    GtkWidget *tray_warn;
    GtkWidget *sec_draw;
    GtkWidget *sec_label;
    int sec_period;
    int sec_current;
    unsigned long sec_hits[MAX_SECTORS];
    unsigned long sec_ghosts[MAX_SECTORS];
    GPtrArray *dev_radios;
} App;

static void select_device(App *a, int idx);
static void update_buttons(App *a);
static void tray_sync(App *a);

/* ------------------------------------------------------------------ */
/* utilidades                                                          */
/* ------------------------------------------------------------------ */

static void set_status(App *a, const char *text, const char *css)
{
    static const char *classes[] = { "success", "warning", "error", NULL };

    gtk_label_set_text(GTK_LABEL(a->status), text);

    for (int i = 0; classes[i]; i++) {
        gtk_widget_remove_css_class(a->status, classes[i]);
    }
    if (css) {
        gtk_widget_add_css_class(a->status, css);
    }
}

static GtkWidget *make_label(const char *text, const char *css)
{
    GtkWidget *l = gtk_label_new(text);

    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    if (css) {
        gtk_widget_add_css_class(l, css);
    }
    return l;
}

static void fmt_clock(double ts, char *buf, size_t n)
{
    time_t sec = (time_t)ts;
    struct tm tm;

    localtime_r(&sec, &tm);
    size_t len = strftime(buf, n, "%H:%M:%S", &tm);
    snprintf(buf + len, n - len, ".%03d", (int)((ts - (double)sec) * 1000.0));
}

static mouse_info_t *selected_mouse(App *a)
{
    if (a->devs && a->selected >= 0 && a->selected < (int)a->devs->len) {
        return g_ptr_array_index(a->devs, a->selected);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* configuração persistente (~/.config/wheel-filter/config.ini)         */
/* ------------------------------------------------------------------ */

static void cfg_load(App *a)
{
    a->cfg = g_key_file_new();
    a->cfg_path = g_build_filename(g_get_user_config_dir(), "wheel-filter",
                                   "config.ini", NULL);

    char *dir = g_path_get_dirname(a->cfg_path);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);

    g_key_file_load_from_file(a->cfg, a->cfg_path, G_KEY_FILE_NONE, NULL);
    a->last_key = g_key_file_get_string(a->cfg, "general", "last_mouse", NULL);
}

static void cfg_save(App *a)
{
    gchar *data = g_key_file_to_data(a->cfg, NULL, NULL);

    if (data) {
        g_file_set_contents(a->cfg_path, data, -1, NULL);
        g_free(data);
    }
}

static char *profile_group(const mouse_info_t *m)
{
    return g_strdup_printf("mouse %04x:%04x", m->vendor, m->product);
}

static char *profile_key(const mouse_info_t *m)
{
    return g_strdup_printf("%04x:%04x", m->vendor, m->product);
}

static void profile_apply(App *a, const mouse_info_t *m)
{
    char *g = profile_group(m);
    int c = 2, f = 350, i = 500;
    gboolean obs = FALSE;

    if (g_key_file_has_key(a->cfg, g, "confirm", NULL)) {
        c = g_key_file_get_integer(a->cfg, g, "confirm", NULL);
    }
    if (g_key_file_has_key(a->cfg, g, "flush_ms", NULL)) {
        f = g_key_file_get_integer(a->cfg, g, "flush_ms", NULL);
    }
    if (g_key_file_has_key(a->cfg, g, "idle_ms", NULL)) {
        i = g_key_file_get_integer(a->cfg, g, "idle_ms", NULL);
    }
    if (g_key_file_has_key(a->cfg, g, "observe", NULL)) {
        obs = g_key_file_get_boolean(a->cfg, g, "observe", NULL);
    }
    g_free(g);

    a->loading = TRUE;
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->sp_confirm), c);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->sp_flush), f);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->sp_idle), i);
    gtk_switch_set_active(GTK_SWITCH(a->chk_observe), obs);
    a->loading = FALSE;
}

static void profile_store(App *a)
{
    mouse_info_t *m = selected_mouse(a);

    if (!m) {
        return;
    }

    char *g = profile_group(m);
    g_key_file_set_integer(a->cfg, g, "confirm",
                           gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(a->sp_confirm)));
    g_key_file_set_integer(a->cfg, g, "flush_ms",
                           gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(a->sp_flush)));
    g_key_file_set_integer(a->cfg, g, "idle_ms",
                           gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(a->sp_idle)));
    g_key_file_set_boolean(a->cfg, g, "observe",
                           gtk_switch_get_active(GTK_SWITCH(a->chk_observe)));
    g_free(g);
    cfg_save(a);
}

/* ------------------------------------------------------------------ */
/* estatísticas e monitor                                              */
/* ------------------------------------------------------------------ */

static void update_stats(App *a)
{
    if (a->running) {
        a->last_secs = (g_get_monotonic_time() - a->t0) / G_USEC_PER_SEC;
    }

    long h = (long)(a->last_secs / 3600);
    long m = (long)((a->last_secs / 60) % 60);
    long s = (long)(a->last_secs % 60);
    double pct = a->rx ? 100.0 * (double)a->ghosts / (double)a->rx : 0.0;

    char *v[5] = {
        g_strdup_printf("%ld", a->rx),
        g_strdup_printf("%ld", a->pass),
        g_strdup_printf("%ld", a->ghosts),
        g_strdup_printf("%.1f%%", pct),
        g_strdup_printf("%02ld:%02ld:%02ld", h, m, s),
    };

    for (int i = 0; i < 5; i++) {
        gtk_label_set_text(GTK_LABEL(a->tile[i]), v[i]);
    }
    if (a->ring_lbl) {
        gtk_label_set_text(GTK_LABEL(a->ring_lbl), v[3]);
    }
    for (int i = 0; i < 5; i++) {
        g_free(v[i]);
    }

    if (a->sec_label) {
        int hot = 0;
        int p = a->sec_period > 0 ? a->sec_period : DEFAULT_PERIOD;

        for (int i = 0; i < p && i < MAX_SECTORS; i++) {
            if (a->sec_hits[i] >= 8 &&
                (double)a->sec_ghosts[i] / (double)a->sec_hits[i] >= 0.12) {
                hot++;
            }
        }

        char *s = g_strdup_printf(
            "%d detentes · setor atual %d\n%d %s",
            p, a->sec_current, hot, hot == 1 ? "setor instável" : "setores instáveis");
        gtk_label_set_text(GTK_LABEL(a->sec_label), s);
        g_free(s);
    }

    if (a->sec_draw) {
        gtk_widget_queue_draw(a->sec_draw);
    }
}

static void monitor_append(App *a, const char *text)
{
    GtkTextIter end;

    gtk_text_buffer_get_end_iter(a->buf, &end);
    gtk_text_buffer_insert(a->buf, &end, text, -1);
    a->mon_lines++;

    if (a->mon_lines > MON_MAX_LINES) {
        GtkTextIter start, cut;

        gtk_text_buffer_get_start_iter(a->buf, &start);
        gtk_text_buffer_get_iter_at_line(a->buf, &cut, MON_TRIM_LINES);
        gtk_text_buffer_delete(a->buf, &start, &cut);
        a->mon_lines -= MON_TRIM_LINES;
    }

    gtk_text_buffer_get_end_iter(a->buf, &end);
    gtk_text_buffer_move_mark(a->buf, a->end_mark, &end);
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(a->monitor), a->end_mark,
                                 0.0, FALSE, 0.0, 0.0);
}

static void log_append(App *a, const char *text)
{
    g_string_append(a->log, text);

    if (a->log->len > LOG_MAX) {
        g_string_erase(a->log, 0, LOG_MAX / 4);
    }
}

static gboolean unflash_cb(gpointer data)
{
    App *a = data;

    a->flash_id = 0;
    gtk_widget_remove_css_class(a->tile[2], "flash");
    gtk_widget_remove_css_class(a->tile[3], "flash");
    return G_SOURCE_REMOVE;
}

static void flash_ghost(App *a)
{
    gtk_widget_add_css_class(a->tile[2], "flash");
    gtk_widget_add_css_class(a->tile[3], "flash");
    if (a->flash_id) {
        g_source_remove(a->flash_id);
    }
    a->flash_id = g_timeout_add(350, unflash_cb, a);
}

static void on_tick(App *a, char **f)
{
    double ts = g_ascii_strtod(f[1], NULL);
    int value = atoi(f[2]);
    gboolean ghost = strcmp(f[3], "GHOST") == 0;
    double gap = g_ascii_strtod(f[4], NULL);
    char when[32];

    fmt_clock(ts, when, sizeof(when));

    a->rx++;
    a->pulse = 1.0;
    a->pulse_t = g_get_monotonic_time();
    if (ghost) {
        a->ghosts++;
        flash_ghost(a);
    } else {
        a->pass++;
    }

    if (g_strv_length(f) >= 7) {
        int sector = atoi(f[5]);
        int period = atoi(f[6]);

        if (period > 0 && period <= MAX_SECTORS) {
            a->sec_period = period;
        }
        if (sector >= 0 && sector < MAX_SECTORS) {
            a->sec_current = sector;
            a->sec_hits[sector]++;
            if (ghost) {
                a->sec_ghosts[sector]++;
            }
        }
    }

    char *gap_txt = gap < 0 ? g_strdup("---") : g_strdup_printf("%.2f", gap);
    char *log_line = g_strdup_printf("%s  %+d  delta=%8s ms  %s\n", when, value,
                                     gap_txt, ghost ? "GHOST" : "pass");
    log_append(a, log_line);
    g_free(log_line);
    g_free(gap_txt);

    gboolean only_ghosts =
        gtk_check_button_get_active(GTK_CHECK_BUTTON(a->chk_ghosts));

    if (!only_ghosts || ghost) {
        char *line = g_strdup_printf("%s    %+d    %s\n", when, value,
                                     ghost ? "possível ghost" : "normal");
        monitor_append(a, line);
        g_free(line);
    }

    update_stats(a);
}

static void on_state(App *a, const char *state, const char *msg)
{
    char *text = NULL;

    if (strcmp(state, "filtering") == 0) {
        text = g_strdup_printf("● Filtrando — %s", msg);
        set_status(a, text, "success");
    } else if (strcmp(state, "observing") == 0) {
        text = g_strdup_printf("● Observando (sem filtrar) — %s", msg);
        set_status(a, text, "success");
    } else if (strcmp(state, "waiting") == 0) {
        text = g_strdup_printf("⚠ Mouse desconectado — %s", msg);
        set_status(a, text, "warning");
    } else if (strcmp(state, "error") == 0) {
        g_free(a->last_error);
        a->last_error = g_strdup_printf("✖ %s", msg);
        set_status(a, a->last_error, "error");
    } else {
        text = g_strdup(msg);
        set_status(a, text, NULL);
    }

    char *log_line = g_strdup_printf("# %s: %s\n", state, msg);
    log_append(a, log_line);
    g_free(log_line);
    g_free(text);
    tray_set_status(a->tray, gtk_label_get_text(GTK_LABEL(a->status)));
}

static void handle_line(App *a, const char *line)
{
    gchar **f = g_strsplit(line, "\t", -1);
    guint n = g_strv_length(f);

    if (n >= 3 && strcmp(f[0], "STATE") == 0) {
        on_state(a, f[1], f[2]);
    } else if (n >= 5 && strcmp(f[0], "TICK") == 0) {
        on_tick(a, f);
    } else if (line[0] != '\0') {
        /* qualquer outra coisa (ex.: mensagem do pkexec) */
        g_free(a->last_error);
        a->last_error = g_strdup(line);
        set_status(a, line, "error");
    }

    g_strfreev(f);
}

/* ------------------------------------------------------------------ */
/* ciclo de vida do motor                                              */
/* ------------------------------------------------------------------ */

static void send_line(App *a, const char *line)
{
    if (!a->in) {
        return;
    }

    g_output_stream_write_all(a->in, line, strlen(line), NULL, NULL, NULL);
    g_output_stream_flush(a->in, NULL, NULL);
}

static void send_params(App *a)
{
    char *l = g_strdup_printf(
        "SET %d %d %d\n",
        gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(a->sp_confirm)),
        gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(a->sp_flush)),
        gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(a->sp_idle)));

    send_line(a, l);
    g_free(l);
}

static void read_next_line(App *a);

static void on_line(GObject *src, GAsyncResult *res, gpointer data)
{
    App *a = data;
    GError *err = NULL;
    gsize len = 0;
    char *line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(src),
                                                      res, &len, &err);

    if (err) {
        g_error_free(err);
    }
    if (!line) {
        return;                 /* EOF: on_exit_cb finaliza */
    }

    handle_line(a, line);
    g_free(line);

    if ((GObject *)a->out == src) {
        read_next_line(a);
    }
}

static void read_next_line(App *a)
{
    g_data_input_stream_read_line_async(a->out, G_PRIORITY_DEFAULT, NULL,
                                        on_line, a);
}

static gboolean uptime_cb(gpointer data)
{
    App *a = data;

    if (!a->running) {
        a->uptime_id = 0;
        return G_SOURCE_REMOVE;
    }

    update_stats(a);
    return G_SOURCE_CONTINUE;
}

static void finish_session(App *a, int status)
{
    a->running = FALSE;
    update_stats(a);

    g_clear_object(&a->proc);
    g_clear_object(&a->in);
    g_clear_object(&a->out);

    if (a->uptime_id) {
        g_source_remove(a->uptime_id);
        a->uptime_id = 0;
    }

    if (a->last_error) {
        set_status(a, a->last_error, "error");
        tray_set_status(a->tray, a->last_error);
    } else if (status == 126 || status == 127) {
        set_status(a, "✖ Autenticação cancelada ou negada.", "error");
        tray_set_status(a->tray, "Autenticação cancelada");
    } else if (status != 0) {
        char *t = g_strdup_printf("✖ O filtro terminou com erro (código %d).", status);
        set_status(a, t, "error");
        tray_set_status(a->tray, t);
        g_free(t);
    } else {
        set_status(a, "● Filtro parado — o mouse voltou ao normal", NULL);
        tray_set_status(a->tray, "Filtro parado");
    }

    log_append(a, "# sessão encerrada\n");
    update_buttons(a);
}

static void on_exit_cb(GObject *src, GAsyncResult *res, gpointer data)
{
    App *a = data;
    GSubprocess *p = G_SUBPROCESS(src);

    g_subprocess_wait_finish(p, res, NULL);

    if (p != a->proc) {
        return;                 /* sessão antiga */
    }

    int status = g_subprocess_get_if_exited(p)
                     ? g_subprocess_get_exit_status(p)
                     : -1;
    finish_session(a, status);
}

static void do_start(App *a)
{
    mouse_info_t *m = selected_mouse(a);

    if (!m || a->running) {
        return;
    }

    if (!g_find_program_in_path("pkexec")) {
        set_status(a, "✖ pkexec (polkit) não encontrado; instale o polkit.", "error");
        return;
    }

    gboolean observe = gtk_switch_get_active(GTK_SWITCH(a->chk_observe));

    GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(argv, g_strdup("pkexec"));
    g_ptr_array_add(argv, g_strdup(a->self_path));
    g_ptr_array_add(argv, g_strdup("--engine"));
    g_ptr_array_add(argv, g_strdup("-c"));
    g_ptr_array_add(argv, g_strdup_printf("%d",
        gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(a->sp_confirm))));
    g_ptr_array_add(argv, g_strdup("-f"));
    g_ptr_array_add(argv, g_strdup_printf("%d",
        gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(a->sp_flush))));
    g_ptr_array_add(argv, g_strdup("-i"));
    g_ptr_array_add(argv, g_strdup_printf("%d",
        gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(a->sp_idle))));
    if (observe) {
        g_ptr_array_add(argv, g_strdup("-n"));
    }
    g_ptr_array_add(argv, g_strdup(m->stable_path));
    g_ptr_array_add(argv, NULL);

    GError *err = NULL;
    GSubprocess *p = g_subprocess_newv((const gchar * const *)argv->pdata,
                                       G_SUBPROCESS_FLAGS_STDIN_PIPE |
                                       G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                       G_SUBPROCESS_FLAGS_STDERR_MERGE,
                                       &err);
    g_ptr_array_unref(argv);

    if (!p) {
        char *t = g_strdup_printf("✖ Não foi possível iniciar: %s", err->message);
        set_status(a, t, "error");
        g_free(t);
        g_error_free(err);
        return;
    }

    a->proc = p;
    a->in = g_object_ref(g_subprocess_get_stdin_pipe(p));
    a->out = g_data_input_stream_new(g_subprocess_get_stdout_pipe(p));

    g_free(a->last_error);
    a->last_error = NULL;
    a->running = TRUE;
    a->rx = a->pass = a->ghosts = 0;
    a->t0 = g_get_monotonic_time();
    a->last_secs = 0;
    a->sec_period = DEFAULT_PERIOD;
    a->sec_current = 0;
    memset(a->sec_hits, 0, sizeof(a->sec_hits));
    memset(a->sec_ghosts, 0, sizeof(a->sec_ghosts));

    char *hdr = g_strdup_printf("# %s — %s (%s)  -c %d -f %d -i %d%s\n", m->name,
        m->stable_path, m->stable_kind,
        gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(a->sp_confirm)),
        gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(a->sp_flush)),
        gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(a->sp_idle)),
        observe ? "  (observação)" : "");
    log_append(a, hdr);
    g_free(hdr);

    set_status(a, "Iniciando… (pode pedir sua senha)", NULL);
    update_stats(a);
    update_buttons(a);

    a->uptime_id = g_timeout_add_seconds(1, uptime_cb, a);
    read_next_line(a);
    g_subprocess_wait_async(p, NULL, on_exit_cb, a);
}

static void stop_engine(App *a)
{
    if (!a->running) {
        return;
    }

    set_status(a, "Parando…", NULL);
    gtk_widget_set_sensitive(a->btn_start, FALSE);
    send_line(a, "STOP\n");
    if (a->in) {
        g_output_stream_close(a->in, NULL, NULL);
    }
}

#if GTK_CHECK_VERSION(4, 10, 0)
static void on_confirm_done(GObject *src, GAsyncResult *res, gpointer data)
{
    App *a = data;
    GError *err = NULL;
    int r = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, &err);

    if (err) {
        g_error_free(err);
        return;
    }
    if (r == 1) {
        do_start(a);
    }
}
#endif

static void on_start_clicked(GtkButton *b, gpointer data)
{
    App *a = data;
    (void)b;

    if (a->running) {
        stop_engine(a);
        return;
    }

    mouse_info_t *m = selected_mouse(a);

    if (!m) {
        return;
    }

    if (gtk_switch_get_active(GTK_SWITCH(a->chk_observe))) {
        do_start(a);            /* observação não captura nada */
        return;
    }

#if GTK_CHECK_VERSION(4, 10, 0)
    char *detail = g_strdup_printf(
        "O filtro será aplicado ao mouse:\n\n%s\n\n"
        "O dispositivo será temporariamente capturado e seus eventos serão "
        "redirecionados pelo filtro. Só este mouse é afetado. Para devolver o "
        "controle, clique em «Parar filtro» ou feche esta janela.", m->name);

    GtkAlertDialog *dlg = gtk_alert_dialog_new("%s", "Iniciar o filtro?");
    gtk_alert_dialog_set_detail(dlg, detail);

    const char *buttons[] = { "Cancelar", "Iniciar", NULL };
    gtk_alert_dialog_set_buttons(dlg, buttons);
    gtk_alert_dialog_set_cancel_button(dlg, 0);
    gtk_alert_dialog_set_default_button(dlg, 1);
    gtk_alert_dialog_choose(dlg, GTK_WINDOW(a->win), NULL, on_confirm_done, a);

    g_object_unref(dlg);
    g_free(detail);
#else
    do_start(a);
#endif
}

static void tray_sync(App *a)
{
    if (!a->tray || !a->sp_confirm) {
        return;
    }

    tray_update(
        a->tray, a->devs, a->selected,
        gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(a->sp_confirm)),
        gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(a->sp_flush)),
        gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(a->sp_idle)),
        a->running, selected_mouse(a) != NULL);
}

static void tray_on_show(void *user)
{
    App *a = user;
    gtk_window_present(GTK_WINDOW(a->win));
}

static void tray_on_quit(void *user)
{
    App *a = user;

    if (a->running) {
        send_line(a, "STOP\n");
    }
    if (a->proc) {
        g_subprocess_send_signal(a->proc, SIGTERM);
    }
    g_application_quit(G_APPLICATION(a->gapp));
}

static void tray_on_device(void *user, int idx)
{
    App *a = user;

    if (a->running) {
        return;
    }
    if (a->dev_radios && idx >= 0 && idx < (int)a->dev_radios->len) {
        gtk_check_button_set_active(
            GTK_CHECK_BUTTON(g_ptr_array_index(a->dev_radios, idx)), TRUE);
        return;
    }
    select_device(a, idx);
}

static void tray_on_confirm(void *user, int v)
{
    App *a = user;
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->sp_confirm), v);
}

static void tray_on_flush(void *user, int v)
{
    App *a = user;
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->sp_flush), v);
}

static void tray_on_idle(void *user, int v)
{
    App *a = user;
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->sp_idle), v);
}

static void tray_on_toggle(void *user)
{
    App *a = user;

    gtk_window_present(GTK_WINDOW(a->win));
    on_start_clicked(NULL, a);
}

/* ------------------------------------------------------------------ */
/* dispositivos                                                        */
/* ------------------------------------------------------------------ */

static void update_buttons(App *a)
{
    const char *label;

    if (a->running) {
        label = "Parar filtro";
    } else if (gtk_switch_get_active(GTK_SWITCH(a->chk_observe))) {
        label = "Iniciar observação";
    } else {
        label = "Iniciar filtro";
    }

    gtk_button_set_label(GTK_BUTTON(a->btn_start), label);
    if (a->running) {
        gtk_widget_remove_css_class(a->btn_start, "suggested-action");
        gtk_widget_add_css_class(a->btn_start, "destructive-action");
    } else {
        gtk_widget_remove_css_class(a->btn_start, "destructive-action");
        gtk_widget_add_css_class(a->btn_start, "suggested-action");
    }
    gtk_widget_set_sensitive(a->btn_start, a->running || selected_mouse(a) != NULL);
    gtk_widget_set_sensitive(a->chk_observe, !a->running);
    gtk_widget_set_sensitive(a->dev_box, !a->running);
    tray_sync(a);
}

static void select_device(App *a, int idx)
{
    a->selected = idx;

    mouse_info_t *m = selected_mouse(a);

    if (!m) {
        gtk_label_set_text(GTK_LABEL(a->sel_name), "Nenhum mouse selecionado");
        gtk_label_set_text(GTK_LABEL(a->sel_path), "");
        gtk_label_set_text(GTK_LABEL(a->sel_wheel), "");
        update_buttons(a);
        return;
    }

    g_free(a->selected_stable);
    a->selected_stable = g_strdup(m->stable_path);

    char *path = g_strdup_printf("%s  (%s)", m->stable_path, m->stable_kind);
    char *wheel = g_strdup_printf("Roda:  REL_WHEEL %s    REL_WHEEL_HI_RES %s",
                                  m->has_wheel ? "✓" : "—",
                                  m->has_hires ? "✓" : "—");

    gtk_label_set_text(GTK_LABEL(a->sel_name), m->name);
    gtk_label_set_text(GTK_LABEL(a->sel_path), path);
    gtk_label_set_text(GTK_LABEL(a->sel_wheel), wheel);
    g_free(path);
    g_free(wheel);

    profile_apply(a, m);

    char *key = profile_key(m);
    g_key_file_set_string(a->cfg, "general", "last_mouse", key);
    cfg_save(a);
    g_free(key);

    update_buttons(a);
}

static void on_radio_toggled(GtkCheckButton *btn, gpointer data)
{
    App *a = data;

    if (!gtk_check_button_get_active(btn)) {
        return;
    }

    select_device(a, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "idx")));
}

static char *devs_signature(GPtrArray *devs)
{
    GString *s = g_string_new(NULL);

    for (guint i = 0; i < devs->len; i++) {
        const mouse_info_t *m = g_ptr_array_index(devs, i);
        g_string_append_printf(s, "%s|%s|%d;", m->event_path, m->name, m->has_wheel);
    }

    return g_string_free(s, FALSE);
}

static void rebuild_devices(App *a, gboolean force)
{
    GPtrArray *fresh = devices_list();
    char *sig = devs_signature(fresh);

    if (!force && a->dev_signature && strcmp(sig, a->dev_signature) == 0) {
        g_ptr_array_unref(fresh);
        g_free(sig);
        return;
    }

    g_free(a->dev_signature);
    a->dev_signature = sig;

    if (a->devs) {
        g_ptr_array_unref(a->devs);
    }
    a->devs = fresh;

    if (a->dev_grid) {
        gtk_box_remove(GTK_BOX(a->dev_box), a->dev_grid);
    }

    GtkWidget *grid = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(grid), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(grid, "boxed-list");

    GPtrArray *radios = g_ptr_array_new();
    GtkWidget *first = NULL;
    int want = -1;
    int by_last = -1;

    for (guint i = 0; i < a->devs->len; i++) {
        const mouse_info_t *m = g_ptr_array_index(a->devs, i);
        GtkWidget *radio = gtk_check_button_new();

        if (first) {
            gtk_check_button_set_group(GTK_CHECK_BUTTON(radio),
                                       GTK_CHECK_BUTTON(first));
        } else {
            first = radio;
        }

        gtk_widget_set_valign(radio, GTK_ALIGN_CENTER);
        g_object_set_data(G_OBJECT(radio), "idx", GINT_TO_POINTER((int)i));
        g_ptr_array_add(radios, radio);

        GtkWidget *row = adw_action_row_new();
        char *sub = g_strdup_printf("%s%s", m->event_path,
                                    m->has_wheel ? "" : " · sem roda");
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), m->name);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), sub);
        g_free(sub);
        adw_action_row_add_prefix(ADW_ACTION_ROW(row), radio);
        adw_action_row_set_activatable_widget(ADW_ACTION_ROW(row), radio);
        gtk_widget_set_sensitive(row, m->has_wheel);
        gtk_list_box_append(GTK_LIST_BOX(grid), row);

        if (a->selected_stable && strcmp(m->stable_path, a->selected_stable) == 0) {
            want = (int)i;
        }

        if (a->last_key && m->has_wheel) {
            char *k = profile_key(m);
            if (strcmp(k, a->last_key) == 0 && by_last < 0) {
                by_last = (int)i;
            }
            g_free(k);
        }
    }

    if (a->devs->len == 0) {
        GtkWidget *row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                      "Nenhum mouse encontrado");
        gtk_widget_set_sensitive(row, FALSE);
        gtk_list_box_append(GTK_LIST_BOX(grid), row);
    }

    a->dev_grid = grid;
    gtk_box_append(GTK_BOX(a->dev_box), grid);

    for (guint i = 0; i < radios->len; i++) {
        g_signal_connect(g_ptr_array_index(radios, i), "toggled",
                         G_CALLBACK(on_radio_toggled), a);
    }

    if (a->dev_radios) {
        g_ptr_array_unref(a->dev_radios);
    }
    a->dev_radios = radios;

    if (want < 0 && !a->selected_stable) {
        want = by_last;         /* restaura a escolha explícita anterior */
    }

    if (want >= 0) {
        gtk_check_button_set_active(GTK_CHECK_BUTTON(g_ptr_array_index(radios, want)),
                                    TRUE);
    } else {
        select_device(a, -1);
    }
}

static void on_refresh_clicked(GtkButton *b, gpointer data)
{
    (void)b;
    rebuild_devices(data, TRUE);
}

static gboolean auto_refresh_cb(gpointer data)
{
    rebuild_devices(data, FALSE);
    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* configuração, log e janela                                           */
/* ------------------------------------------------------------------ */

static void on_params_changed(GtkWidget *w, gpointer data)
{
    App *a = data;
    (void)w;

    if (a->loading) {
        return;
    }

    profile_store(a);
    update_buttons(a);

    if (a->running) {
        send_params(a);
    }
}

static void on_clear_clicked(GtkButton *b, gpointer data)
{
    App *a = data;
    (void)b;

    gtk_text_buffer_set_text(a->buf, "", 0);
    a->mon_lines = 0;
}

static void save_log_to(App *a, GFile *f)
{
    GError *err = NULL;

    if (g_file_replace_contents(f, a->log->str, a->log->len, NULL, FALSE,
                                G_FILE_CREATE_NONE, NULL, NULL, &err)) {
        char *p = g_file_get_path(f);
        char *t = g_strdup_printf("Log salvo em %s", p ? p : "(arquivo)");
        set_status(a, t, "success");
        g_free(t);
        g_free(p);
    } else {
        char *t = g_strdup_printf("✖ Não foi possível salvar: %s", err->message);
        set_status(a, t, "error");
        g_free(t);
        g_error_free(err);
    }
}

#if GTK_CHECK_VERSION(4, 10, 0)
static void on_save_done(GObject *src, GAsyncResult *res, gpointer data)
{
    App *a = data;
    GError *err = NULL;
    GFile *f = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, &err);

    if (!f) {
        g_clear_error(&err);    /* cancelado */
        return;
    }

    save_log_to(a, f);
    g_object_unref(f);
}
#endif

static void on_save_clicked(GtkButton *b, gpointer data)
{
    App *a = data;
    (void)b;

    if (a->log->len == 0) {
        set_status(a, "Nada para salvar ainda.", "warning");
        return;
    }

    char name[80];
    time_t now = time(NULL);
    struct tm tm;

    localtime_r(&now, &tm);
    strftime(name, sizeof(name), "wheel-filter-%Y%m%d-%H%M%S.log", &tm);

#if GTK_CHECK_VERSION(4, 10, 0)
    GtkFileDialog *dlg = gtk_file_dialog_new();

    gtk_file_dialog_set_initial_name(dlg, name);
    gtk_file_dialog_save(dlg, GTK_WINDOW(a->win), NULL, on_save_done, a);
    g_object_unref(dlg);
#else
    char *path = g_build_filename(g_get_home_dir(), name, NULL);
    GFile *f = g_file_new_for_path(path);

    save_log_to(a, f);
    g_object_unref(f);
    g_free(path);
#endif
}

static gboolean on_close_request(GtkWindow *w, gpointer data)
{
    App *a = data;
    (void)w;

    gtk_widget_set_visible(GTK_WIDGET(a->win), FALSE);
    return TRUE;
}

static void draw_sectors(GtkDrawingArea *area, cairo_t *cr, int width,
                         int height, gpointer data)
{
    App *a = data;
    int p = a->sec_period > 0 ? a->sec_period : DEFAULT_PERIOD;
    double cx = width / 2.0;
    double cy = height / 2.0;
    double r = (width < height ? width : height) / 2.0 - 4.0;
    double ri = r * 0.70;
    double gap = 0.035;
    GdkRGBA fg, ac;
    gboolean dark =
        adw_style_manager_get_dark(adw_style_manager_get_default());

    gtk_widget_get_color(GTK_WIDGET(area), &fg);
    GdkRGBA *acp =
        adw_style_manager_get_accent_color_rgba(adw_style_manager_get_default());
    ac = *acp;
    gdk_rgba_free(acp);

    for (int i = 0; i < p; i++) {
        double a0 = -G_PI / 2.0 + (2.0 * G_PI * i) / p + gap / 2;
        double a1 = -G_PI / 2.0 + (2.0 * G_PI * (i + 1)) / p - gap / 2;
        double rate = 0.0;
        gboolean cur = i == a->sec_current;

        if (i < MAX_SECTORS && a->sec_hits[i] > 0) {
            rate = (double)a->sec_ghosts[i] / (double)a->sec_hits[i];
        }
        rate = rate > 0.4 ? 1.0 : rate / 0.4;       /* 40% já é "quente" */

        double hot = dark ? 0.95 : 0.80;
        double base = 0.10 + 0.80 * rate;

        if (rate > 0.0) {
            cairo_set_source_rgba(cr, hot, 0.38 * (1.0 - rate) + 0.18, 0.25,
                                  base);
        } else if (a->sec_hits[i < MAX_SECTORS ? i : 0] > 0) {
            cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.22);
        } else {
            cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.10);
        }
        if (cur) {
            cairo_set_source_rgba(cr, ac.red, ac.green, ac.blue,
                                  0.55 + 0.45 * a->pulse);
        }

        double rr = cur ? r + 3.0 * a->pulse : r;
        cairo_new_path(cr);
        cairo_arc(cr, cx, cy, rr, a0, a1);
        cairo_arc_negative(cr, cx, cy, ri, a1, a0);
        cairo_close_path(cr);
        cairo_fill(cr);
    }
}

static gboolean pulse_tick(GtkWidget *w, GdkFrameClock *clock, gpointer data)
{
    App *a = data;
    (void)clock;

    if (a->pulse > 0.0) {
        double dt = (g_get_monotonic_time() - a->pulse_t) / 1e6;

        a->pulse = dt >= 0.6 ? 0.0 : 1.0 - dt / 0.6;
        gtk_widget_queue_draw(w);
    }
    return G_SOURCE_CONTINUE;
}

static GtkWidget *make_spin(App *a, double min, double max, double step, double val)
{
    GtkWidget *sp = gtk_spin_button_new_with_range(min, max, step);

    gtk_spin_button_set_value(GTK_SPIN_BUTTON(sp), val);
    gtk_widget_set_valign(sp, GTK_ALIGN_CENTER);
    g_signal_connect(sp, "value-changed", G_CALLBACK(on_params_changed), a);
    return sp;
}

static void on_observe_notify(GObject *o, GParamSpec *p, gpointer data)
{
    (void)p;
    on_params_changed(GTK_WIDGET(o), data);
}

static GtkWidget *param_row(const char *title, const char *sub,
                            const char *tip, GtkWidget *spin)
{
    GtkWidget *row = adw_action_row_new();

    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), sub);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), spin);
    gtk_widget_set_tooltip_text(row, tip);
    return row;
}

static GtkWidget *make_tile(App *a, int idx, const char *caption)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

    gtk_widget_add_css_class(box, "card");
    gtk_widget_add_css_class(box, "stat-tile");
    gtk_widget_set_hexpand(box, TRUE);

    a->tile[idx] = make_label("0", "stat-value");
    GtkWidget *cap = make_label(caption, "dim-label");
    gtk_widget_add_css_class(cap, "caption");

    gtk_box_append(GTK_BOX(box), a->tile[idx]);
    gtk_box_append(GTK_BOX(box), cap);
    return box;
}

static void build_ui(App *a)
{
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css, APP_CSS);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    a->win = GTK_WIDGET(adw_application_window_new(a->gapp));
    gtk_window_set_title(GTK_WINDOW(a->win), "Wheel Filter");
    gtk_window_set_default_size(GTK_WINDOW(a->win), 940, 600);
    g_signal_connect(a->win, "close-request", G_CALLBACK(on_close_request), a);

    /* --- barra superior: atualizar, estado, iniciar/parar --- */
    GtkWidget *header = adw_header_bar_new();

    GtkWidget *btn_refresh = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_set_tooltip_text(btn_refresh, "Atualizar dispositivos");
    g_signal_connect(btn_refresh, "clicked", G_CALLBACK(on_refresh_clicked), a);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), btn_refresh);

    a->status = make_label("● Filtro parado", "status-pill");
    gtk_label_set_ellipsize(GTK_LABEL(a->status), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(a->status), 48);
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), a->status);

    a->btn_start = gtk_button_new_with_label("Iniciar filtro");
    gtk_widget_add_css_class(a->btn_start, "suggested-action");
    gtk_widget_add_css_class(a->btn_start, "pill");
    gtk_widget_add_css_class(a->btn_start, "start-btn");
    gtk_widget_set_sensitive(a->btn_start, FALSE);
    g_signal_connect(a->btn_start, "clicked", G_CALLBACK(on_start_clicked), a);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), a->btn_start);

    GtkWidget *toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(a->win), toolbar);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    gtk_widget_set_margin_top(root, 6);
    gtk_widget_set_margin_bottom(root, 18);
    gtk_widget_set_margin_start(root, 18);
    gtk_widget_set_margin_end(root, 18);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), root);

    /* ============ coluna esquerda: mouse + filtro ============ */
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_set_size_request(left, 330, -1);

    a->tray_warn = make_label(
        "O ícone da bandeja precisa da extensão AppIndicator "
        "(KStatusNotifierItem) no GNOME. Sem ela o programa continua em "
        "segundo plano ao fechar a janela; reabra pelo Dash ou pela busca.",
        "dim-label");
    gtk_widget_add_css_class(a->tray_warn, "banner");
    gtk_label_set_wrap(GTK_LABEL(a->tray_warn), TRUE);
    gtk_widget_set_visible(a->tray_warn, FALSE);
    gtk_box_append(GTK_BOX(left), a->tray_warn);

    a->service_warn = make_label(
        "⚠ O serviço «wheel-filter» está ativo e já captura o mouse. "
        "Pare-o antes (sudo systemctl stop wheel-filter), senão o filtro "
        "daqui não conseguirá iniciar.", "warning");
    gtk_widget_add_css_class(a->service_warn, "banner");
    gtk_label_set_wrap(GTK_LABEL(a->service_warn), TRUE);
    gtk_widget_set_visible(a->service_warn, FALSE);
    gtk_box_append(GTK_BOX(left), a->service_warn);

    GtkWidget *g_mouse = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(g_mouse), "Mouse");
    a->dev_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g_mouse), a->dev_box);

    /* rótulos de seleção: mantidos para o estado interno, sem ocupar espaço */
    a->sel_name = make_label("", NULL);
    a->sel_path = make_label("", NULL);
    a->sel_wheel = make_label("", NULL);
    gtk_widget_set_visible(a->sel_name, FALSE);
    gtk_widget_set_visible(a->sel_path, FALSE);
    gtk_widget_set_visible(a->sel_wheel, FALSE);
    gtk_box_append(GTK_BOX(a->dev_box), a->sel_name);
    gtk_box_append(GTK_BOX(a->dev_box), a->sel_path);
    gtk_box_append(GTK_BOX(a->dev_box), a->sel_wheel);
    gtk_box_append(GTK_BOX(left), g_mouse);

    GtkWidget *g_cfg = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(g_cfg), "Filtro");

    a->sp_confirm = make_spin(a, 1, 8, 1, 2);
    a->sp_flush = make_spin(a, 50, 2000, 10, 350);
    a->sp_idle = make_spin(a, 100, 5000, 50, 500);

    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g_cfg), param_row(
        "Ticks para confirmar", "Seguidos na nova direção para inverter",
        "Quantos ticks seguidos na nova direção são necessários para aceitar uma "
        "inversão. Mais alto filtra mais, mas atrasa inversões reais.",
        a->sp_confirm));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g_cfg), param_row(
        "Tempo de confirmação", "ms até liberar uma inversão isolada",
        "Uma inversão isolada que não for seguida de nada nesse tempo é "
        "considerada real e liberada — salvo em um detente que o filtro já "
        "marcou como instável.",
        a->sp_flush));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g_cfg), param_row(
        "Inatividade", "ms sem eventos até passar direto",
        "Depois desse tempo sem eventos, o próximo tick passa direto em "
        "qualquer direção. Não use um valor menor que o tempo de confirmação.",
        a->sp_idle));

    a->chk_observe = gtk_switch_new();
    gtk_widget_set_valign(a->chk_observe, GTK_ALIGN_CENTER);
    g_signal_connect(a->chk_observe, "notify::active",
                     G_CALLBACK(on_observe_notify), a);
    GtkWidget *obs_row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(obs_row), "Modo observação");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(obs_row),
                                "Só registra os ghosts, sem capturar o mouse");
    adw_action_row_add_suffix(ADW_ACTION_ROW(obs_row), a->chk_observe);
    adw_action_row_set_activatable_widget(ADW_ACTION_ROW(obs_row), a->chk_observe);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g_cfg), obs_row);
    gtk_box_append(GTK_BOX(left), g_cfg);

    gtk_box_append(GTK_BOX(root), left);

    /* ============ coluna direita: números, anel e log ============ */
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_set_hexpand(right, TRUE);

    GtkWidget *tiles = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_set_homogeneous(GTK_BOX(tiles), TRUE);
    static const char *caps[5] = { "Recebidos", "Liberados", "Filtrados",
                                   "Taxa de ghost", "Tempo ativo" };
    for (int i = 0; i < 5; i++) {
        gtk_box_append(GTK_BOX(tiles), make_tile(a, i, caps[i]));
    }
    gtk_box_append(GTK_BOX(right), tiles);

    /* anel dos setores + legenda */
    GtkWidget *ring_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    gtk_widget_add_css_class(ring_row, "card");
    gtk_widget_add_css_class(ring_row, "stat-tile");

    a->sec_period = DEFAULT_PERIOD;
    a->sec_draw = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(a->sec_draw), 150);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(a->sec_draw), 150);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(a->sec_draw), draw_sectors,
                                   a, NULL);
    gtk_widget_add_tick_callback(a->sec_draw, pulse_tick, a, NULL);

    a->ring_lbl = make_label("0.0%", "ring-pct");
    gtk_widget_set_halign(a->ring_lbl, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(a->ring_lbl, GTK_ALIGN_CENTER);
    GtkWidget *ov = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(ov), a->sec_draw);
    gtk_overlay_add_overlay(GTK_OVERLAY(ov), a->ring_lbl);
    gtk_box_append(GTK_BOX(ring_row), ov);

    GtkWidget *rinfo = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_valign(rinfo, GTK_ALIGN_CENTER);
    GtkWidget *rt = make_label("Leito da roda", "heading");
    a->sec_label = make_label("", "dim-label");
    GtkWidget *legend = make_label(
        "Cada fatia é um detente. Quanto mais vermelha, mais ghosts o filtro "
        "viu ali; a fatia em destaque é a posição atual.", "dim-label");
    gtk_widget_add_css_class(legend, "caption");
    gtk_label_set_wrap(GTK_LABEL(legend), TRUE);
    gtk_box_append(GTK_BOX(rinfo), rt);
    gtk_box_append(GTK_BOX(rinfo), a->sec_label);
    gtk_box_append(GTK_BOX(rinfo), legend);
    gtk_widget_set_hexpand(rinfo, TRUE);
    gtk_box_append(GTK_BOX(ring_row), rinfo);
    gtk_box_append(GTK_BOX(right), ring_row);

    /* monitor */
    GtkWidget *mhead = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *mt = make_label("Monitor da roda", "heading");
    gtk_widget_set_hexpand(mt, TRUE);
    a->chk_ghosts = gtk_check_button_new_with_label("Só ghosts");
    GtkWidget *btn_clear = gtk_button_new_from_icon_name("edit-clear-all-symbolic");
    GtkWidget *btn_save = gtk_button_new_from_icon_name("document-save-symbolic");
    gtk_widget_set_tooltip_text(btn_clear, "Limpar");
    gtk_widget_set_tooltip_text(btn_save, "Salvar log…");
    gtk_widget_add_css_class(btn_clear, "flat");
    gtk_widget_add_css_class(btn_save, "flat");
    g_signal_connect(btn_clear, "clicked", G_CALLBACK(on_clear_clicked), a);
    g_signal_connect(btn_save, "clicked", G_CALLBACK(on_save_clicked), a);
    gtk_box_append(GTK_BOX(mhead), mt);
    gtk_box_append(GTK_BOX(mhead), a->chk_ghosts);
    gtk_box_append(GTK_BOX(mhead), btn_clear);
    gtk_box_append(GTK_BOX(mhead), btn_save);
    gtk_box_append(GTK_BOX(right), mhead);

    a->monitor = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(a->monitor), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(a->monitor), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(a->monitor), TRUE);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(a->monitor), 10);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(a->monitor), 6);
    a->buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(a->monitor));

    GtkTextIter it;
    gtk_text_buffer_get_end_iter(a->buf, &it);
    a->end_mark = gtk_text_buffer_create_mark(a->buf, "end", &it, FALSE);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), a->monitor);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 100);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_add_css_class(scroll, "card");
    gtk_widget_add_css_class(scroll, "log-card");
    gtk_box_append(GTK_BOX(right), scroll);

    gtk_box_append(GTK_BOX(root), right);

    update_stats(a);
}

static void check_service(App *a)
{
    gchar *out = NULL;

    if (g_spawn_command_line_sync("systemctl is-active wheel-filter", &out,
                                  NULL, NULL, NULL) &&
        out && g_str_has_prefix(out, "active")) {
        gtk_widget_set_visible(a->service_warn, TRUE);
    }

    g_free(out);
}

static void on_activate(GtkApplication *app, gpointer data)
{
    App *a = data;
    (void)app;

    if (a->win) {
        gtk_window_present(GTK_WINDOW(a->win));
        return;
    }

    cfg_load(a);
    build_ui(a);
    rebuild_devices(a, TRUE);
    g_timeout_add_seconds(3, auto_refresh_cb, a);
    check_service(a);
    update_buttons(a);
    g_application_hold(G_APPLICATION(a->gapp));

    tray_cbs_t cbs = {
        .show_window = tray_on_show,
        .quit = tray_on_quit,
        .select_device = tray_on_device,
        .set_confirm = tray_on_confirm,
        .set_flush = tray_on_flush,
        .set_idle = tray_on_idle,
        .toggle_filter = tray_on_toggle,
        .user = a,
    };
    a->tray = tray_start(&cbs);
    tray_sync(a);

    g_timeout_add_seconds(2, G_SOURCE_FUNC(NULL), NULL);
    gtk_window_present(GTK_WINDOW(a->win));
}

int gui_main(void)
{
    signal(SIGPIPE, SIG_IGN);

    App *a = g_new0(App, 1);

    a->selected = -1;
    a->log = g_string_new(NULL);
    a->self_path = g_file_read_link("/proc/self/exe", NULL);
    if (!a->self_path) {
        a->self_path = g_strdup("wheel-filter");
    }

    a->gapp = gtk_application_new(APP_ID, G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(a->gapp, "activate", G_CALLBACK(on_activate), a);

    char *argv[] = { "wheel-filter", NULL };
    int rc = g_application_run(G_APPLICATION(a->gapp), 1, argv);

    g_object_unref(a->gapp);
    return rc;
}
