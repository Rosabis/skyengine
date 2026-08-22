/* Linux 原生菜单栏。X11/Wayland 没有 Win32 SetMenu。
 * GtkSocket/XEmbed 要求子窗口实现 XEMBED,SDL 窗口没有,XFCE 上会嵌入失败、
 * 看起来像「装了 GTK 仍无菜单」。X11 改为 XReparentWindow 把 SDL 窗口挂到
 * GTK 菜单栏下面的 EventBox 里。Wayland 仍用贴顶工具条。
 * 菜单回调只 queue_cmd,不在 GTK 信号里重启引擎。 */
#include "./include/desktop_shell_internal.h"

#include <gtk/gtk.h>
#ifdef GDK_WINDOWING_X11
#include <X11/Xlib.h>
#include <gdk/gdkx.h>
#endif

#ifdef _MSC_VER
#include <SDL.h>
#include <SDL_syswm.h>
#else
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#endif

#include <stdio.h>
#include <string.h>

static SDL_Window *g_sdl;
static GtkWidget *g_gtk_win;
static GtkWidget *g_embed_area;
static GtkWidget *g_recent_menu;
static int g_has_menubar;
static int g_embedded; /* 1 = XReparent 嵌入,0 = 贴顶工具条 */
static GtkWidget *g_edit_dlg;
static GtkWidget *g_edit_entry;
static int g_edit_done;
static int g_edit_ok;
static char g_edit_text[1024];
void desktop_shell_gtk_edit_close(void);
static int g_bar_h = 28;
#ifdef GDK_WINDOWING_X11
static Display *g_x11_dpy;
static Window g_sdl_xid;
#endif

static void copy_str(char *dst, size_t n, const char *src) {
    if (!dst || n == 0) return;
    snprintf(dst, n, "%s", src ? src : "");
}

static void on_cmd(GtkMenuItem *item, gpointer data) {
    (void)item;
    desktop_shell_queue_cmd((int)(intptr_t)data);
}

static GtkWidget *add_item(GtkWidget *menu, const char *title, int cmd) {
    GtkWidget *item = gtk_menu_item_new_with_label(title);
    g_signal_connect(item, "activate", G_CALLBACK(on_cmd), (gpointer)(intptr_t)cmd);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    return item;
}

static GtkWidget *build_menubar(void) {
    GtkWidget *bar = gtk_menu_bar_new();
    GtkWidget *file_item = gtk_menu_item_new_with_label("文件");
    GtkWidget *file_menu = gtk_menu_new();
    GtkWidget *set_item = gtk_menu_item_new_with_label("设置");
    GtkWidget *set_menu = gtk_menu_new();
    GtkWidget *recent_item;

    add_item(file_menu, "选择 MRP 启动...", CMD_OPEN);
    add_item(file_menu, "启动 dsm_gm.mrp", CMD_DSM_GM);
    recent_item = gtk_menu_item_new_with_label("最近打开 MRP");
    g_recent_menu = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(recent_item), g_recent_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), recent_item);
    add_item(file_menu, "高级启动...", CMD_ADVANCED);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), gtk_separator_menu_item_new());
    add_item(file_menu, "重启模拟器", CMD_RESTART);

    add_item(set_menu, "设置运行模式...", CMD_PROFILE);
    add_item(set_menu, "设置屏幕分辨率...", CMD_SCREEN);
    add_item(set_menu, "设置应用可见内存...", CMD_MEMORY);
    add_item(set_menu, "设置应用可见设备日期...", CMD_DATE);
    add_item(set_menu, "设置运行和 MRP 文件系统的工作目录...", CMD_WORKDIR);
    add_item(set_menu, "设置域名替换规则...", CMD_DNS);
    add_item(set_menu, "选择 SoundFont (SF2)...", CMD_SF2);

    gtk_menu_item_set_submenu(GTK_MENU_ITEM(file_item), file_menu);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(set_item), set_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), file_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), set_item);
    return bar;
}

static gboolean on_gtk_delete(GtkWidget *w, GdkEvent *e, gpointer data) {
    SDL_Event ev;
    (void)w;
    (void)e;
    (void)data;
    memset(&ev, 0, sizeof(ev));
    ev.type = SDL_QUIT;
    SDL_PushEvent(&ev);
    return TRUE;
}

#ifdef GDK_WINDOWING_X11
static int try_x11_embed(GtkWidget *bar, int w, int h) {
    SDL_SysWMinfo info;
    GtkWidget *vbox;
    GtkWidget *area;
    GdkWindow *gdkwin;
    Window parent_xid;

    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(g_sdl, &info) || info.subsystem != SDL_SYSWM_X11) {
        fprintf(stderr, "[desktop_shell] GTK: SDL 不是 X11 窗口,无法嵌入\n");
        return -1;
    }

    /* EventBox 有自己的 X window,才能当 XReparent 的父窗口。
     * GtkSocket 要 XEMBED,SDL 不实现该协议,Debian/XFCE 上会嵌入失败。 */
    g_gtk_win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(g_gtk_win), "SkyEngine");
    vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    area = gtk_event_box_new();
    gtk_widget_set_size_request(area, w, h);
    gtk_widget_set_hexpand(area, TRUE);
    gtk_widget_set_vexpand(area, TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), bar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), area, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(g_gtk_win), vbox);
    g_signal_connect(g_gtk_win, "delete-event", G_CALLBACK(on_gtk_delete), NULL);
    gtk_widget_show_all(g_gtk_win);
    while (gtk_events_pending()) {
        gtk_main_iteration_do(FALSE);
    }
    gdkwin = gtk_widget_get_window(area);
    if (!gdkwin) {
        fprintf(stderr, "[desktop_shell] GTK: EventBox 尚未 realize,放弃嵌入\n");
        gtk_widget_destroy(g_gtk_win);
        g_gtk_win = NULL;
        return -1;
    }
    parent_xid = gdk_x11_window_get_xid(gdkwin);
    g_x11_dpy = info.info.x11.display;
    g_sdl_xid = info.info.x11.window;
    SDL_SetWindowBordered(g_sdl, SDL_FALSE);
    XReparentWindow(g_x11_dpy, g_sdl_xid, parent_xid, 0, 0);
    XResizeWindow(g_x11_dpy, g_sdl_xid, (unsigned)w, (unsigned)h);
    XMapWindow(g_x11_dpy, g_sdl_xid);
    XSync(g_x11_dpy, False);
    g_embed_area = area;
    g_embedded = 1;
    g_has_menubar = 1;
    fprintf(stderr, "[desktop_shell] GTK menubar: X11 reparent %dx%d\n", w, h);
    return 0;
}
#endif

static void dock_bar(void) {
    int x = 0, y = 0, w = 0, h = 0;
    if (!g_sdl || !g_gtk_win || g_embedded) return;
    SDL_GetWindowPosition(g_sdl, &x, &y);
    SDL_GetWindowSize(g_sdl, &w, &h);
    (void)h;
    gtk_window_resize(GTK_WINDOW(g_gtk_win), w > 0 ? w : 240, g_bar_h);
    /* SDL_GetWindowPosition 是客户区左上;贴在客户区上方,不盖住游戏像素。 */
    gtk_window_move(GTK_WINDOW(g_gtk_win), x, y - g_bar_h);
}

static int try_docked_bar(GtkWidget *bar, int w) {
    int top = 0, left = 0, bottom = 0, right = 0;
    (void)bottom;
    (void)right;
    g_gtk_win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(g_gtk_win), "SkyEngine 菜单");
    gtk_window_set_decorated(GTK_WINDOW(g_gtk_win), FALSE);
    gtk_window_set_keep_above(GTK_WINDOW(g_gtk_win), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(g_gtk_win), FALSE);
    gtk_container_add(GTK_CONTAINER(g_gtk_win), bar);
    gtk_widget_set_size_request(g_gtk_win, w > 0 ? w : 240, g_bar_h);
    g_signal_connect(g_gtk_win, "delete-event", G_CALLBACK(on_gtk_delete), NULL);
    gtk_widget_show_all(g_gtk_win);
    g_embedded = 0;
    g_has_menubar = 1;
    SDL_GetWindowBordersSize(g_sdl, &top, &left, &bottom, &right);
    (void)left;
    (void)top;
    dock_bar();
    fprintf(stderr, "[desktop_shell] GTK menubar: docked bar (driver=%s)\n",
            SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?");
    return 0;
}

int desktop_shell_gtk_init(struct SDL_Window *window, int w, int h) {
    GtkWidget *bar;
    const char *driver;
    if (!window) return -1;
    g_sdl = window;
    gtk_init(NULL, NULL);
    bar = build_menubar();
    desktop_shell_gtk_refresh_recents();

    driver = SDL_GetCurrentVideoDriver();
    fprintf(stderr, "[desktop_shell] GTK init, SDL driver='%s'\n",
            driver ? driver : "(null)");
#ifdef GDK_WINDOWING_X11
    if (driver && strcmp(driver, "x11") == 0) {
        if (try_x11_embed(bar, w, h) == 0) return 0;
        bar = build_menubar();
        desktop_shell_gtk_refresh_recents();
    }
#else
    (void)h;
#endif
    return try_docked_bar(bar, w);
}

void desktop_shell_gtk_shutdown(void) {
    desktop_shell_gtk_edit_close();
    if (g_gtk_win) {
        gtk_widget_destroy(g_gtk_win);
        g_gtk_win = NULL;
    }
    g_embed_area = NULL;
#ifdef GDK_WINDOWING_X11
    g_x11_dpy = NULL;
    g_sdl_xid = 0;
#endif
    g_recent_menu = NULL;
    g_has_menubar = 0;
    g_sdl = NULL;
}

void desktop_shell_gtk_idle(void) {
    while (gtk_events_pending()) {
        gtk_main_iteration_do(FALSE);
    }
}

int desktop_shell_gtk_needs_idle(void) {
    return g_has_menubar || g_edit_dlg != NULL;
}

int desktop_shell_gtk_has_menubar(void) {
    return g_has_menubar;
}

void desktop_shell_gtk_set_title(const char *title) {
    if (g_gtk_win && g_embedded && title) {
        gtk_window_set_title(GTK_WINDOW(g_gtk_win), title);
    }
    if (g_sdl && title) SDL_SetWindowTitle(g_sdl, title);
}

void desktop_shell_gtk_refresh_recents(void) {
    GList *children, *it;
    int n, i;
    if (!g_recent_menu) return;
    children = gtk_container_get_children(GTK_CONTAINER(g_recent_menu));
    for (it = children; it; it = it->next) {
        gtk_widget_destroy(GTK_WIDGET(it->data));
    }
    g_list_free(children);
    n = desktop_shell_recent_count();
    if (n <= 0) {
        GtkWidget *empty = gtk_menu_item_new_with_label("（空）");
        gtk_widget_set_sensitive(empty, FALSE);
        gtk_menu_shell_append(GTK_MENU_SHELL(g_recent_menu), empty);
        gtk_widget_show_all(g_recent_menu);
        return;
    }
    for (i = 0; i < n; i++) {
        const char *path = desktop_shell_recent_at(i);
        add_item(g_recent_menu, path ? path : "", CMD_RECENT_BASE + i);
    }
    gtk_widget_show_all(g_recent_menu);
}

void desktop_shell_gtk_sync_window(void) {
    int w = 0, h = 0;
    if (!g_sdl) return;
    SDL_GetWindowSize(g_sdl, &w, &h);
    if (g_embedded && g_embed_area) {
        gtk_widget_set_size_request(g_embed_area, w, h);
        if (g_gtk_win) gtk_window_resize(GTK_WINDOW(g_gtk_win), w, h + g_bar_h);
#ifdef GDK_WINDOWING_X11
        if (g_x11_dpy && g_sdl_xid) {
            XResizeWindow(g_x11_dpy, g_sdl_xid, (unsigned)w, (unsigned)h);
            XSync(g_x11_dpy, False);
        }
#endif
    } else {
        dock_bar();
    }
}

int desktop_shell_gtk_handle_window_event(const union SDL_Event *ev) {
    if (!ev || ev->type != SDL_WINDOWEVENT) return 0;
    switch (ev->window.event) {
        case SDL_WINDOWEVENT_MOVED:
        case SDL_WINDOWEVENT_RESIZED:
        case SDL_WINDOWEVENT_SIZE_CHANGED:
        case SDL_WINDOWEVENT_SHOWN:
        case SDL_WINDOWEVENT_EXPOSED:
            desktop_shell_gtk_sync_window();
            break;
        default:
            break;
    }
    return 0;
}

static GtkWindow *dialog_parent(void) {
    return g_gtk_win ? GTK_WINDOW(g_gtk_win) : NULL;
}

static void on_adv_browse(GtkButton *button, gpointer data) {
    char path[4096];
    (void)button;
    if (desktop_shell_gtk_pick_mrp(path, sizeof(path)) == 0) {
        gtk_entry_set_text(GTK_ENTRY(data), path);
    }
}

int desktop_shell_gtk_pick_mrp(char *out, size_t n) {
    GtkWidget *dlg;
    gint resp;
    int rc = -1;
    dlg = gtk_file_chooser_dialog_new("选择要启动的 MRP 文件", dialog_parent(),
                                      GTK_FILE_CHOOSER_ACTION_OPEN,
                                      "取消", GTK_RESPONSE_CANCEL,
                                      "打开", GTK_RESPONSE_ACCEPT, NULL);
    {
        GtkFileFilter *filter = gtk_file_filter_new();
        gtk_file_filter_set_name(filter, "MRP");
        gtk_file_filter_add_pattern(filter, "*.mrp");
        gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), filter);
    }
    resp = gtk_dialog_run(GTK_DIALOG(dlg));
    if (resp == GTK_RESPONSE_ACCEPT) {
        char *fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        if (fn) {
            copy_str(out, n, fn);
            g_free(fn);
            rc = 0;
        }
    }
    gtk_widget_destroy(dlg);
    desktop_shell_gtk_idle();
    return rc;
}

int desktop_shell_gtk_pick_dir(char *out, size_t n) {
    GtkWidget *dlg;
    gint resp;
    int rc = -1;
    dlg = gtk_file_chooser_dialog_new("设置运行和 MRP 文件系统的工作目录",
                                      dialog_parent(),
                                      GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                                      "取消", GTK_RESPONSE_CANCEL,
                                      "选择", GTK_RESPONSE_ACCEPT, NULL);
    resp = gtk_dialog_run(GTK_DIALOG(dlg));
    if (resp == GTK_RESPONSE_ACCEPT) {
        char *fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        if (fn) {
            copy_str(out, n, fn);
            g_free(fn);
            rc = 0;
        }
    }
    gtk_widget_destroy(dlg);
    desktop_shell_gtk_idle();
    return rc;
}

int desktop_shell_gtk_advanced(char *mrp, size_t mrp_n, char *ext, size_t ext_n,
                               char *entry, size_t entry_n) {
    GtkWidget *dlg, *content, *grid, *mrp_entry, *browse, *ext_combo, *entry_combo;
    gint resp;
    int rc = -1;
    const char *init_mrp = (mrp && mrp[0]) ? mrp : desktop_shell_current_mrp();

    dlg = gtk_dialog_new_with_buttons("高级启动", dialog_parent(),
                                      GTK_DIALOG_MODAL,
                                      "确定", GTK_RESPONSE_OK,
                                      "取消", GTK_RESPONSE_CANCEL, NULL);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 520, 220);
    content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 12);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("选择要启动的 MRP 文件"), 0, 0, 2, 1);
    mrp_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(mrp_entry), init_mrp ? init_mrp : "");
    gtk_widget_set_hexpand(mrp_entry, TRUE);
    browse = gtk_button_new_with_label("浏览...");
    gtk_grid_attach(GTK_GRID(grid), mrp_entry, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), browse, 1, 1, 1, 1);
    g_signal_connect(browse, "clicked", G_CALLBACK(on_adv_browse), mrp_entry);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("EXT_NAME（VMRP_EXT 或 start.mr）"), 0, 2, 2, 1);
    ext_combo = gtk_combo_box_text_new_with_entry();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ext_combo), "VMRP_EXT");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ext_combo), "start.mr");
    gtk_entry_set_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(ext_combo))),
                       desktop_shell_last_ext());

    gtk_grid_attach(GTK_GRID(grid), ext_combo, 0, 3, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("ENTRY（VMRP_ENTRY 或空）"), 0, 4, 2, 1);
    entry_combo = gtk_combo_box_text_new_with_entry();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(entry_combo), "VMRP_ENTRY");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(entry_combo), "（空）");
    {
        const char *le = desktop_shell_last_entry();
        gtk_entry_set_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(entry_combo))),
                           (le && le[0]) ? le : "（空）");
    }
    gtk_grid_attach(GTK_GRID(grid), entry_combo, 0, 5, 2, 1);
    gtk_container_add(GTK_CONTAINER(content), grid);
    gtk_widget_show_all(dlg);

    resp = gtk_dialog_run(GTK_DIALOG(dlg));
    if (resp == GTK_RESPONSE_OK) {
        copy_str(mrp, mrp_n, gtk_entry_get_text(GTK_ENTRY(mrp_entry)));
        copy_str(ext, ext_n, gtk_entry_get_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(ext_combo)))));
        copy_str(entry, entry_n,
                 gtk_entry_get_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(entry_combo)))));
        rc = 0;
    }
    gtk_widget_destroy(dlg);
    desktop_shell_gtk_idle();
    return rc;
}

int desktop_shell_gtk_prompt(const char *title, const char *label,
                             const char *initial, char *out, size_t n,
                             int multiline, const char **choices, int nchoices,
                             int list_only) {
    GtkWidget *dlg, *content, *box, *widget = NULL;
    gint resp;
    int rc = -1;
    int i;
    (void)list_only;

    dlg = gtk_dialog_new_with_buttons(title ? title : "SkyEngine", dialog_parent(),
                                      GTK_DIALOG_MODAL,
                                      "确定", GTK_RESPONSE_OK,
                                      "取消", GTK_RESPONSE_CANCEL, NULL);
    content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    if (label) gtk_box_pack_start(GTK_BOX(box), gtk_label_new(label), FALSE, FALSE, 0);

    if (choices && nchoices > 0) {
        GtkWidget *combo = gtk_combo_box_text_new();
        for (i = 0; i < nchoices; i++) {
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), choices[i]);
            if (initial && strcmp(choices[i], initial) == 0) {
                gtk_combo_box_set_active(GTK_COMBO_BOX(combo), i);
            }
        }
        if (gtk_combo_box_get_active(GTK_COMBO_BOX(combo)) < 0) {
            gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
        }
        widget = combo;
        gtk_box_pack_start(GTK_BOX(box), combo, FALSE, FALSE, 0);
    } else if (multiline) {
        GtkWidget *view = gtk_text_view_new();
        GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)),
                                 initial ? initial : "", -1);
        gtk_widget_set_size_request(scroll, 420, 160);
        gtk_container_add(GTK_CONTAINER(scroll), view);
        widget = view;
        gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);
    } else {
        GtkWidget *entry = gtk_entry_new();
        gtk_entry_set_text(GTK_ENTRY(entry), initial ? initial : "");
        widget = entry;
        gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 0);
    }
    gtk_container_add(GTK_CONTAINER(content), box);
    gtk_widget_show_all(dlg);
    resp = gtk_dialog_run(GTK_DIALOG(dlg));
    if (resp == GTK_RESPONSE_OK && widget) {
        if (GTK_IS_COMBO_BOX_TEXT(widget)) {
            gchar *text = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(widget));
            copy_str(out, n, text ? text : "");
            g_free(text);
            rc = 0;
        } else if (GTK_IS_TEXT_VIEW(widget)) {
            GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(widget));
            GtkTextIter start, end;
            gchar *text;
            gtk_text_buffer_get_start_iter(buf, &start);
            gtk_text_buffer_get_end_iter(buf, &end);
            text = gtk_text_buffer_get_text(buf, &start, &end, FALSE);
            copy_str(out, n, text ? text : "");
            g_free(text);
            rc = 0;
        } else if (GTK_IS_ENTRY(widget)) {
            copy_str(out, n, gtk_entry_get_text(GTK_ENTRY(widget)));
            rc = 0;
        }
    }
    gtk_widget_destroy(dlg);
    desktop_shell_gtk_idle();
    return rc;
}

static void gtk_dns_refill(GtkListStore *store, const DesktopDnsRule *rules, int count) {
    int i;
    gtk_list_store_clear(store);
    for (i = 0; i < count; i++) {
        GtkTreeIter iter;
        char line[DESKTOP_DNS_HOST_MAX * 2 + 8];
        desktop_dns_format(&rules[i], line, sizeof(line));
        gtk_list_store_append(store, &iter);
        gtk_list_store_set(store, &iter, 0, line, -1);
    }
}

static int gtk_dns_pair(char *from, size_t from_n, char *to, size_t to_n) {
    GtkWidget *dlg, *content, *grid, *from_e, *to_e;
    gint resp;
    int rc = -1;
    dlg = gtk_dialog_new_with_buttons("新增域名替换", dialog_parent(),
                                      GTK_DIALOG_MODAL,
                                      "确定", GTK_RESPONSE_OK,
                                      "取消", GTK_RESPONSE_CANCEL, NULL);
    content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 12);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("被替换值"), 0, 0, 1, 1);
    from_e = gtk_entry_new();
    gtk_widget_set_hexpand(from_e, TRUE);
    gtk_grid_attach(GTK_GRID(grid), from_e, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("替换值"), 0, 2, 1, 1);
    to_e = gtk_entry_new();
    gtk_grid_attach(GTK_GRID(grid), to_e, 0, 3, 1, 1);
    gtk_container_add(GTK_CONTAINER(content), grid);
    gtk_widget_show_all(dlg);
    resp = gtk_dialog_run(GTK_DIALOG(dlg));
    if (resp == GTK_RESPONSE_OK) {
        copy_str(from, from_n, gtk_entry_get_text(GTK_ENTRY(from_e)));
        copy_str(to, to_n, gtk_entry_get_text(GTK_ENTRY(to_e)));
        if (from[0] && to[0]) rc = 0;
    }
    gtk_widget_destroy(dlg);
    desktop_shell_gtk_idle();
    return rc;
}

int desktop_shell_gtk_dns_editor(char *map, size_t n) {
    DesktopDnsRule rules[DESKTOP_DNS_RULE_MAX];
    int count = desktop_dns_parse(map, rules, DESKTOP_DNS_RULE_MAX);
    GtkWidget *dlg, *content, *scroll, *view;
    GtkListStore *store;
    GtkCellRenderer *cell;
    GtkTreeViewColumn *col;
    gint resp;

    dlg = gtk_dialog_new_with_buttons("域名替换规则", dialog_parent(),
                                      GTK_DIALOG_MODAL,
                                      "新增", 1,
                                      "删除", 2,
                                      NULL);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 520, 360);
    content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    store = gtk_list_store_new(1, G_TYPE_STRING);
    view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    cell = gtk_cell_renderer_text_new();
    col = gtk_tree_view_column_new_with_attributes("现有替换", cell, "text", 0, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view), col);
    gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(view)),
                                GTK_SELECTION_SINGLE);
    gtk_dns_refill(store, rules, count);
    scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, 480, 240);
    gtk_container_add(GTK_CONTAINER(scroll), view);
    gtk_box_pack_start(GTK_BOX(content), scroll, TRUE, TRUE, 8);
    gtk_widget_show_all(dlg);

    for (;;) {
        resp = gtk_dialog_run(GTK_DIALOG(dlg));
        if (resp == 1) {
            char from[DESKTOP_DNS_HOST_MAX];
            char to[DESKTOP_DNS_HOST_MAX];
            if (count >= DESKTOP_DNS_RULE_MAX) continue;
            if (gtk_dns_pair(from, sizeof(from), to, sizeof(to)) == 0) {
                copy_str(rules[count].from, sizeof(rules[0].from), from);
                copy_str(rules[count].to, sizeof(rules[0].to), to);
                count++;
                gtk_dns_refill(store, rules, count);
            }
            continue;
        }
        if (resp == 2) {
            GtkTreeIter iter;
            GtkTreeModel *model = GTK_TREE_MODEL(store);
            GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
            GtkTreePath *path;
            gint *idx;
            if (!gtk_tree_selection_get_selected(sel, &model, &iter)) continue;
            path = gtk_tree_model_get_path(model, &iter);
            idx = gtk_tree_path_get_indices(path);
            if (idx && idx[0] >= 0 && idx[0] < count) {
                int i = idx[0];
                if (i + 1 < count) {
                    memmove(&rules[i], &rules[i + 1],
                            (size_t)(count - i - 1) * sizeof(rules[0]));
                }
                count--;
                gtk_dns_refill(store, rules, count);
            }
            gtk_tree_path_free(path);
            continue;
        }
        break;
    }
    gtk_widget_destroy(dlg);
    desktop_shell_gtk_idle();
    g_object_unref(store);
    return desktop_dns_serialize(rules, count, map, n);
}

static void on_edit_response(GtkDialog *dialog, gint response, gpointer user) {
    (void)user;
    if (response == GTK_RESPONSE_OK && g_edit_entry) {
        const char *t = gtk_entry_get_text(GTK_ENTRY(g_edit_entry));
        copy_str(g_edit_text, sizeof(g_edit_text), t ? t : "");
        g_edit_ok = 1;
    } else {
        g_edit_ok = 0;
        g_edit_text[0] = '\0';
    }
    g_edit_done = 1;
    gtk_widget_hide(GTK_WIDGET(dialog));
    desktop_shell_wake();
}

int desktop_shell_gtk_edit_open(const char *title, const char *text, int type, int max_size) {
    GtkWidget *content;
    desktop_shell_gtk_edit_close();
    g_edit_done = 0;
    g_edit_ok = 0;
    g_edit_text[0] = '\0';
    g_edit_dlg = gtk_dialog_new_with_buttons(title && title[0] ? title : "编辑",
                                             dialog_parent(),
                                             GTK_DIALOG_DESTROY_WITH_PARENT,
                                             "确定", GTK_RESPONSE_OK,
                                             "取消", GTK_RESPONSE_CANCEL,
                                             NULL);
    gtk_window_set_default_size(GTK_WINDOW(g_edit_dlg), 420, 140);
    gtk_window_set_modal(GTK_WINDOW(g_edit_dlg), FALSE);
    content = gtk_dialog_get_content_area(GTK_DIALOG(g_edit_dlg));
    g_edit_entry = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(g_edit_entry), TRUE);
    if (max_size > 0) gtk_entry_set_max_length(GTK_ENTRY(g_edit_entry), max_size);
    if (type == 2) gtk_entry_set_visibility(GTK_ENTRY(g_edit_entry), FALSE);
    if (type == 1) gtk_entry_set_input_purpose(GTK_ENTRY(g_edit_entry), GTK_INPUT_PURPOSE_DIGITS);
    gtk_entry_set_text(GTK_ENTRY(g_edit_entry), text ? text : "");
    gtk_container_set_border_width(GTK_CONTAINER(content), 12);
    gtk_box_pack_start(GTK_BOX(content), g_edit_entry, TRUE, TRUE, 8);
    gtk_dialog_set_default_response(GTK_DIALOG(g_edit_dlg), GTK_RESPONSE_OK);
    g_signal_connect(g_edit_dlg, "response", G_CALLBACK(on_edit_response), NULL);
    gtk_widget_show_all(g_edit_dlg);
    gtk_widget_grab_focus(g_edit_entry);
    return 0;
}

void desktop_shell_gtk_edit_close(void) {
    if (g_edit_dlg) {
        gtk_widget_destroy(g_edit_dlg);
        g_edit_dlg = NULL;
        g_edit_entry = NULL;
    }
    g_edit_done = 0;
}

int desktop_shell_gtk_edit_poll(int *ok, char *out, size_t out_n) {
    if (!g_edit_done) return 0;
    if (ok) *ok = g_edit_ok;
    if (out && out_n) copy_str(out, out_n, g_edit_text);
    desktop_shell_gtk_edit_close();
    return 1;
}

int desktop_shell_gtk_edit_active(void) {
    return g_edit_dlg != NULL;
}

int desktop_shell_gtk_pick_sf2(char *out, size_t n) {
    GtkWidget *dlg;
    gint resp;
    int rc = -1;
    dlg = gtk_file_chooser_dialog_new("选择 SoundFont (SF2)", dialog_parent(),
                                      GTK_FILE_CHOOSER_ACTION_OPEN,
                                      "取消", GTK_RESPONSE_CANCEL,
                                      "打开", GTK_RESPONSE_ACCEPT, NULL);
    {
        GtkFileFilter *filter = gtk_file_filter_new();
        gtk_file_filter_set_name(filter, "SF2");
        gtk_file_filter_add_pattern(filter, "*.sf2");
        gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), filter);
    }
    resp = gtk_dialog_run(GTK_DIALOG(dlg));
    if (resp == GTK_RESPONSE_ACCEPT) {
        char *fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        if (fn) {
            copy_str(out, n, fn);
            g_free(fn);
            rc = 0;
        }
    }
    gtk_widget_destroy(dlg);
    desktop_shell_gtk_idle();
    return rc;
}
