/*
 * Ícone na bandeja (StatusNotifierItem / AppIndicator). No GNOME o ícone
 * aparece no painel se o suporte a AppIndicator/KStatusNotifierItem
 * estiver ativo. Fechar a janela não encerra o processo.
 */
#include "tray.h"

#include <gio/gio.h>
#include <libdbusmenu-glib/server.h>
#include <libdbusmenu-glib/menuitem.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *SNI_XML =
    "<node>"
    "  <interface name='org.kde.StatusNotifierItem'>"
    "    <property name='Category' type='s' access='read'/>"
    "    <property name='Id' type='s' access='read'/>"
    "    <property name='Title' type='s' access='read'/>"
    "    <property name='Status' type='s' access='read'/>"
    "    <property name='WindowId' type='i' access='read'/>"
    "    <property name='IconName' type='s' access='read'/>"
    "    <property name='OverlayIconName' type='s' access='read'/>"
    "    <property name='AttentionIconName' type='s' access='read'/>"
    "    <property name='AttentionMovieName' type='s' access='read'/>"
    "    <property name='ToolTip' type='(sa(iiay)ss)' access='read'/>"
    "    <property name='ItemIsMenu' type='b' access='read'/>"
    "    <property name='Menu' type='o' access='read'/>"
    "    <property name='IconThemePath' type='s' access='read'/>"
    "    <method name='ContextMenu'>"
    "      <arg type='i' name='x' direction='in'/>"
    "      <arg type='i' name='y' direction='in'/>"
    "    </method>"
    "    <method name='Activate'>"
    "      <arg type='i' name='x' direction='in'/>"
    "      <arg type='i' name='y' direction='in'/>"
    "    </method>"
    "    <method name='SecondaryActivate'>"
    "      <arg type='i' name='x' direction='in'/>"
    "      <arg type='i' name='y' direction='in'/>"
    "    </method>"
    "    <method name='Scroll'>"
    "      <arg type='i' name='delta' direction='in'/>"
    "      <arg type='s' name='orientation' direction='in'/>"
    "    </method>"
    "    <signal name='NewTitle'/>"
    "    <signal name='NewIcon'/>"
    "    <signal name='NewAttentionIcon'/>"
    "    <signal name='NewOverlayIcon'/>"
    "    <signal name='NewToolTip'/>"
    "    <signal name='NewStatus'><arg type='s' name='status'/></signal>"
    "  </interface>"
    "</node>";

enum {
    ACT_SHOW = 1,
    ACT_TOGGLE,
    ACT_QUIT,
    ACT_DEV,
    ACT_CONFIRM,
    ACT_FLUSH,
    ACT_IDLE
};

struct tray {
    tray_cbs_t cbs;
    GDBusConnection *bus;
    guint owner_id;
    guint sni_reg;
    guint watcher_watch;
    gboolean registered;
    char *bus_name;
    char *tooltip;

    DbusmenuServer *menu;
    DbusmenuMenuitem *root;

    GPtrArray *devs;
    int selected;
    int confirm;
    int flush_ms;
    int idle_ms;
    gboolean running;
    gboolean can_start;
};

static const int FLUSH_PRESETS[] = {150, 250, 350, 500, 800};
static const int IDLE_PRESETS[] = {300, 500, 700, 1000};

static DbusmenuMenuitem *menu_item(const char *label)
{
    DbusmenuMenuitem *it = dbusmenu_menuitem_new();
    dbusmenu_menuitem_property_set(it, DBUSMENU_MENUITEM_PROP_LABEL, label);
    return it;
}

static DbusmenuMenuitem *menu_sep(void)
{
    DbusmenuMenuitem *it = dbusmenu_menuitem_new();
    dbusmenu_menuitem_property_set(it, DBUSMENU_MENUITEM_PROP_TYPE, "separator");
    return it;
}

static void on_item(DbusmenuMenuitem *mi, guint ts, gpointer data)
{
    tray_t *t = data;
    int act = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(mi), "act"));
    int val = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(mi), "val"));
    (void)ts;

    if (!t->cbs.user) {
        return;
    }

    switch (act) {
    case ACT_SHOW:
        if (t->cbs.show_window) {
            t->cbs.show_window(t->cbs.user);
        }
        break;
    case ACT_TOGGLE:
        if (t->cbs.toggle_filter) {
            t->cbs.toggle_filter(t->cbs.user);
        }
        break;
    case ACT_QUIT:
        if (t->cbs.quit) {
            t->cbs.quit(t->cbs.user);
        }
        break;
    case ACT_DEV:
        if (t->cbs.select_device) {
            t->cbs.select_device(t->cbs.user, val);
        }
        break;
    case ACT_CONFIRM:
        if (t->cbs.set_confirm) {
            t->cbs.set_confirm(t->cbs.user, val);
        }
        break;
    case ACT_FLUSH:
        if (t->cbs.set_flush) {
            t->cbs.set_flush(t->cbs.user, val);
        }
        break;
    case ACT_IDLE:
        if (t->cbs.set_idle) {
            t->cbs.set_idle(t->cbs.user, val);
        }
        break;
    default:
        break;
    }
}

static void bind_item(tray_t *t, DbusmenuMenuitem *it, int act, int val)
{
    g_object_set_data(G_OBJECT(it), "act", GINT_TO_POINTER(act));
    g_object_set_data(G_OBJECT(it), "val", GINT_TO_POINTER(val));
    g_signal_connect(it, DBUSMENU_MENUITEM_SIGNAL_ITEM_ACTIVATED,
                     G_CALLBACK(on_item), t);
}

static DbusmenuMenuitem *radio_item(tray_t *t, const char *label, int act,
                                    int val, gboolean on)
{
    DbusmenuMenuitem *it = menu_item(label);
    dbusmenu_menuitem_property_set(it, DBUSMENU_MENUITEM_PROP_TOGGLE_TYPE,
                                   DBUSMENU_MENUITEM_TOGGLE_RADIO);
    dbusmenu_menuitem_property_set_int(
        it, DBUSMENU_MENUITEM_PROP_TOGGLE_STATE,
        on ? DBUSMENU_MENUITEM_TOGGLE_STATE_CHECKED
           : DBUSMENU_MENUITEM_TOGGLE_STATE_UNCHECKED);
    bind_item(t, it, act, val);
    return it;
}

static void tray_rebuild_menu(tray_t *t)
{
    DbusmenuMenuitem *root = dbusmenu_menuitem_new();
    dbusmenu_menuitem_set_root(root, TRUE);

    DbusmenuMenuitem *show = menu_item("Mostrar janela");
    bind_item(t, show, ACT_SHOW, 0);
    dbusmenu_menuitem_child_append(root, show);

    DbusmenuMenuitem *toggle = menu_item(
        t->running ? "Parar filtro" : "Iniciar filtro");
    dbusmenu_menuitem_property_set_bool(
        toggle, DBUSMENU_MENUITEM_PROP_ENABLED,
        t->running || t->can_start);
    bind_item(t, toggle, ACT_TOGGLE, 0);
    dbusmenu_menuitem_child_append(root, toggle);

    dbusmenu_menuitem_child_append(root, menu_sep());

    DbusmenuMenuitem *devs = menu_item("Dispositivo");
    dbusmenu_menuitem_property_set(devs, DBUSMENU_MENUITEM_PROP_CHILD_DISPLAY,
                                   "submenu");
    if (!t->devs || t->devs->len == 0) {
        DbusmenuMenuitem *empty = menu_item("Nenhum mouse");
        dbusmenu_menuitem_property_set_bool(
            empty, DBUSMENU_MENUITEM_PROP_ENABLED, FALSE);
        dbusmenu_menuitem_child_append(devs, empty);
    } else {
        for (guint i = 0; i < t->devs->len; i++) {
            const mouse_info_t *m = g_ptr_array_index(t->devs, i);
            if (!m->has_wheel) {
                continue;
            }
            DbusmenuMenuitem *it = radio_item(t, m->name, ACT_DEV, (int)i,
                                              (int)i == t->selected);
            dbusmenu_menuitem_child_append(devs, it);
        }
    }
    dbusmenu_menuitem_child_append(root, devs);

    DbusmenuMenuitem *conf = menu_item("Ticks para confirmar");
    dbusmenu_menuitem_property_set(conf, DBUSMENU_MENUITEM_PROP_CHILD_DISPLAY,
                                   "submenu");
    for (int c = 1; c <= 8; c++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", c);
        dbusmenu_menuitem_child_append(
            conf, radio_item(t, buf, ACT_CONFIRM, c, c == t->confirm));
    }
    dbusmenu_menuitem_child_append(root, conf);

    DbusmenuMenuitem *flush = menu_item("Tempo de confirmação (ms)");
    dbusmenu_menuitem_property_set(flush, DBUSMENU_MENUITEM_PROP_CHILD_DISPLAY,
                                   "submenu");
    for (size_t i = 0; i < G_N_ELEMENTS(FLUSH_PRESETS); i++) {
        int v = FLUSH_PRESETS[i];
        char buf[32];
        snprintf(buf, sizeof(buf), "%d ms", v);
        dbusmenu_menuitem_child_append(
            flush, radio_item(t, buf, ACT_FLUSH, v, v == t->flush_ms));
    }
    dbusmenu_menuitem_child_append(root, flush);

    DbusmenuMenuitem *idle = menu_item("Tempo de inatividade (ms)");
    dbusmenu_menuitem_property_set(idle, DBUSMENU_MENUITEM_PROP_CHILD_DISPLAY,
                                   "submenu");
    for (size_t i = 0; i < G_N_ELEMENTS(IDLE_PRESETS); i++) {
        int v = IDLE_PRESETS[i];
        char buf[32];
        snprintf(buf, sizeof(buf), "%d ms", v);
        dbusmenu_menuitem_child_append(
            idle, radio_item(t, buf, ACT_IDLE, v, v == t->idle_ms));
    }
    dbusmenu_menuitem_child_append(root, idle);

    dbusmenu_menuitem_child_append(root, menu_sep());

    DbusmenuMenuitem *quit = menu_item("Sair");
    bind_item(t, quit, ACT_QUIT, 0);
    dbusmenu_menuitem_child_append(root, quit);

    dbusmenu_server_set_root(t->menu, root);
    if (t->root) {
        g_object_unref(t->root);
    }
    t->root = root;
}

static GVariant *sni_get_prop(GDBusConnection *c, const gchar *sender,
                              const gchar *path, const gchar *iface,
                              const gchar *name, GError **err, gpointer data)
{
    tray_t *t = data;
    (void)c;
    (void)sender;
    (void)path;
    (void)iface;
    (void)err;

    if (!strcmp(name, "Category")) {
        return g_variant_new_string("Hardware");
    }
    if (!strcmp(name, "Id")) {
        return g_variant_new_string("wheel-filter");
    }
    if (!strcmp(name, "Title")) {
        return g_variant_new_string("Wheel Filter");
    }
    if (!strcmp(name, "Status")) {
        return g_variant_new_string("Active");
    }
    if (!strcmp(name, "WindowId")) {
        return g_variant_new_int32(0);
    }
    if (!strcmp(name, "IconName")) {
        return g_variant_new_string("input-mouse");
    }
    if (!strcmp(name, "OverlayIconName") ||
        !strcmp(name, "AttentionIconName") ||
        !strcmp(name, "AttentionMovieName") ||
        !strcmp(name, "IconThemePath")) {
        return g_variant_new_string("");
    }
    if (!strcmp(name, "ItemIsMenu")) {
        return g_variant_new_boolean(FALSE);
    }
    if (!strcmp(name, "Menu")) {
        return g_variant_new_object_path("/MenuBar");
    }
    if (!strcmp(name, "ToolTip")) {
        GVariantBuilder b;
        const char *tip = t->tooltip ? t->tooltip : "Wheel Filter";

        g_variant_builder_init(&b, G_VARIANT_TYPE("a(iiay)"));
        return g_variant_new("(sa(iiay)ss)", "", &b, "Wheel Filter", tip);
    }
    return NULL;
}

static void sni_method(GDBusConnection *c, const gchar *sender,
                       const gchar *path, const gchar *iface,
                       const gchar *method, GVariant *params,
                       GDBusMethodInvocation *inv, gpointer data)
{
    tray_t *t = data;
    (void)c;
    (void)sender;
    (void)path;
    (void)iface;
    (void)params;

    if (!strcmp(method, "Activate") || !strcmp(method, "SecondaryActivate")) {
        if (t->cbs.show_window) {
            t->cbs.show_window(t->cbs.user);
        }
    }

    g_dbus_method_invocation_return_value(inv, NULL);
}

static void register_with_watcher(tray_t *t)
{
    if (!t->bus || !t->bus_name) {
        return;
    }

    g_dbus_connection_call(
        t->bus, "org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
        "org.kde.StatusNotifierWatcher", "RegisterStatusNotifierItem",
        g_variant_new("(s)", t->bus_name), NULL, G_DBUS_CALL_FLAGS_NONE,
        -1, NULL, NULL, NULL);

    t->registered = TRUE;
}

static void on_watcher_appeared(GDBusConnection *c, const gchar *name,
                                const gchar *owner, gpointer data)
{
    (void)c;
    (void)name;
    (void)owner;
    register_with_watcher(data);
}

static void on_name_acquired(GDBusConnection *c, const gchar *name,
                             gpointer data)
{
    tray_t *t = data;
    (void)name;
    t->bus = g_object_ref(c);

    GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(SNI_XML, NULL);
    static const GDBusInterfaceVTable vt = {
        .method_call = sni_method,
        .get_property = sni_get_prop,
        .set_property = NULL,
    };

    t->sni_reg = g_dbus_connection_register_object(
        c, "/StatusNotifierItem", info->interfaces[0], &vt, t, NULL, NULL);
    g_dbus_node_info_unref(info);

    t->menu = dbusmenu_server_new("/MenuBar");
    tray_rebuild_menu(t);

    t->watcher_watch = g_bus_watch_name_on_connection(
        c, "org.kde.StatusNotifierWatcher", G_BUS_NAME_WATCHER_FLAGS_NONE,
        on_watcher_appeared, NULL, t, NULL);

    register_with_watcher(t);
}

static void on_name_lost(GDBusConnection *c, const gchar *name, gpointer data)
{
    (void)c;
    (void)name;
    (void)data;
}

tray_t *tray_start(const tray_cbs_t *cbs)
{
    tray_t *t = g_new0(tray_t, 1);
    t->cbs = *cbs;
    t->selected = -1;
    t->confirm = 2;
    t->flush_ms = 350;
    t->idle_ms = 500;
    t->tooltip = g_strdup("Wheel Filter");
    t->bus_name = g_strdup_printf("org.wheelfilter.StatusNotifierItem-%d",
                                  getpid());

    t->owner_id = g_bus_own_name(
        G_BUS_TYPE_SESSION, t->bus_name, G_BUS_NAME_OWNER_FLAGS_NONE,
        on_name_acquired, NULL, on_name_lost, t, NULL);

    return t;
}

void tray_update(tray_t *t, GPtrArray *devs, int selected, int confirm,
                 int flush_ms, int idle_ms, gboolean running,
                 gboolean can_start)
{
    if (!t) {
        return;
    }

    t->devs = devs;
    t->selected = selected;
    t->confirm = confirm;
    t->flush_ms = flush_ms;
    t->idle_ms = idle_ms;
    t->running = running;
    t->can_start = can_start;

    if (t->menu) {
        tray_rebuild_menu(t);
    }
}

void tray_set_status(tray_t *t, const char *tooltip)
{
    if (!t) {
        return;
    }

    g_free(t->tooltip);
    t->tooltip = g_strdup(tooltip ? tooltip : "Wheel Filter");

    if (t->bus) {
        g_dbus_connection_emit_signal(t->bus, NULL, "/StatusNotifierItem",
                                      "org.kde.StatusNotifierItem",
                                      "NewToolTip", NULL, NULL);
    }
}

gboolean tray_is_registered(const tray_t *t)
{
    return t && t->registered;
}

void tray_stop(tray_t *t)
{
    if (!t) {
        return;
    }

    if (t->watcher_watch) {
        g_bus_unwatch_name(t->watcher_watch);
    }
    if (t->sni_reg && t->bus) {
        g_dbus_connection_unregister_object(t->bus, t->sni_reg);
    }
    if (t->owner_id) {
        g_bus_unown_name(t->owner_id);
    }
    if (t->menu) {
        g_object_unref(t->menu);
    }
    if (t->root) {
        g_object_unref(t->root);
    }
    g_clear_object(&t->bus);
    g_free(t->bus_name);
    g_free(t->tooltip);
    g_free(t);
}
