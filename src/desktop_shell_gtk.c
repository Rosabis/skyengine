/* Linux 原生菜单栏。X11/Wayland 没有 Win32 SetMenu。
 *
 * 不要 GtkSocket/XEmbed:SDL 不实现 XEMBED。
 * 不要 XReparentWindow:XFCE 合成器仍把 SDL 当顶层窗,拖外框画面不动。
 * 不要把 SDL 叠在 GTK EventBox 上:点菜单会把空框抬到最前(变白),
 * 还要把 SDL 按 EventBox 分配尺寸去 SetWindowSize,换 MRP 后画面被裁。
 *
 * X11:GTK 窗口只含标题栏+菜单栏,无游戏客户区;无边框 SDL 贴在它正下方,
 * 两者不重叠。拖 GTK 时按菜单栏底边对齐 SDL。SDL 尺寸只跟 --screen。
 * Wayland 没有全局坐标,仍用贴顶工具条。
 * 菜单回调只 queue_cmd,不在 GTK 信号里重启引擎。 */
#include "./include/desktop_shell_internal.h"

#include <gtk/gtk.h>
#ifdef GDK_WINDOWING_X11
#include <X11/Xlib.h>
#include <X11/Xatom.h>
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
static GtkWidget *g_recent_menu;
static int g_has_menubar;
static int g_embedded; /* 1 = GTK 只做标题+菜单,SDL 贴在下方;0 = 贴顶工具条 */
static int g_overlay_lock; /* SDL_SetWindowPosition 时忽略 WINDOWEVENT,避免回环 */
static int g_last_ox = -1, g_last_oy = -1, g_last_bar_h = -1;
static int g_game_w = 240, g_game_h = 320;
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
static void overlay_sdl(void);

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

/* 无边框 SDL 贴在 GTK 菜单栏客户区正下方。不改 SDL 尺寸(那是 --screen),也不盖住菜单。 */
static void overlay_sdl(void) {
    int ox = 0, oy = 0, bar_h;
    GdkWindow *gdkwin;
    if (!g_sdl || !g_gtk_win || !g_embedded || g_overlay_lock) return;
    gdkwin = gtk_widget_get_window(g_gtk_win);
    if (!gdkwin) return;
    gdk_window_get_origin(gdkwin, &ox, &oy);
    bar_h = gtk_widget_get_allocated_height(g_gtk_win);
    if (bar_h <= 0) bar_h = g_bar_h;
    oy += bar_h;
    if (ox == g_last_ox && oy == g_last_oy && bar_h == g_last_bar_h) return;
    g_overlay_lock = 1;
    SDL_SetWindowPosition(g_sdl, ox, oy);
    g_last_ox = ox;
    g_last_oy = oy;
    g_last_bar_h = bar_h;
    g_overlay_lock = 0;
}

#ifdef GDK_WINDOWING_X11
/* skip-taskbar + utility + transient-for:任务栏只留 GTK 标题这一条。
 * SetWindowBordered 会 remap,每次 map 后都要重写 hint。 */
static void apply_sdl_x11_hints(void) {
    GdkWindow *gw;
    Window gtk_xid;
    Atom state, skip_taskbar, skip_pager, bypass, wtype, utility;
    XEvent ev;
    unsigned long bypass_val = 0;
    if (!g_x11_dpy || !g_sdl_xid || !g_gtk_win) return;
    gw = gtk_widget_get_window(g_gtk_win);
    if (!gw) return;
    gtk_xid = gdk_x11_window_get_xid(gw);
    XSetTransientForHint(g_x11_dpy, g_sdl_xid, gtk_xid);

    skip_taskbar = XInternAtom(g_x11_dpy, "_NET_WM_STATE_SKIP_TASKBAR", False);
    skip_pager = XInternAtom(g_x11_dpy, "_NET_WM_STATE_SKIP_PAGER", False);
    state = XInternAtom(g_x11_dpy, "_NET_WM_STATE", False);
    bypass = XInternAtom(g_x11_dpy, "_NET_WM_BYPASS_COMPOSITOR", False);
    wtype = XInternAtom(g_x11_dpy, "_NET_WM_WINDOW_TYPE", False);
    utility = XInternAtom(g_x11_dpy, "_NET_WM_WINDOW_TYPE_UTILITY", False);

    {
        Atom atoms[2];
        atoms[0] = skip_taskbar;
        atoms[1] = skip_pager;
        XChangeProperty(g_x11_dpy, g_sdl_xid, state, XA_ATOM, 32,
                        PropModeReplace, (unsigned char *)atoms, 2);
        XChangeProperty(g_x11_dpy, g_sdl_xid, wtype, XA_ATOM, 32,
                        PropModeReplace, (unsigned char *)&utility, 1);
    }

    memset(&ev, 0, sizeof(ev));
    ev.xclient.type = ClientMessage;
    ev.xclient.window = g_sdl_xid;
    ev.xclient.message_type = state;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = 1; /* _NET_WM_STATE_ADD */
    ev.xclient.data.l[1] = (long)skip_taskbar;
    ev.xclient.data.l[2] = (long)skip_pager;
    ev.xclient.data.l[3] = 1;
    XSendEvent(g_x11_dpy, DefaultRootWindow(g_x11_dpy), False,
               SubstructureNotifyMask | SubstructureRedirectMask, &ev);
    XChangeProperty(g_x11_dpy, g_sdl_xid, bypass, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)&bypass_val, 1);
    XSync(g_x11_dpy, False);
}

static gboolean on_gtk_configure(GtkWidget *w, GdkEventConfigure *e, gpointer data) {
    (void)w;
    (void)e;
    (void)data;
    overlay_sdl();
    return FALSE;
}

static gboolean on_gtk_map(GtkWidget *w, GdkEventAny *e, gpointer data) {
    (void)w;
    (void)e;
    (void)data;
    /* 第一次 gtk_widget_show_all 时 g_embedded 还是 0,不要把仍带边框的 SDL 再亮出来。 */
    if (!g_embedded || !g_sdl) return FALSE;
    SDL_ShowWindow(g_sdl);
    apply_sdl_x11_hints();
    g_last_ox = -1;
    overlay_sdl();
    return FALSE;
}

static gboolean on_gtk_unmap(GtkWidget *w, GdkEventAny *e, gpointer data) {
    (void)w;
    (void)e;
    (void)data;
    /* GTK 最小化/切工作区时把游戏窗一起藏起来,否则画面会留在原地。 */
    if (!g_embedded || !g_sdl) return FALSE;
    SDL_HideWindow(g_sdl);
    g_last_ox = -1;
    return FALSE;
}

static int try_x11_embed(GtkWidget *bar, int w, int h) {
    SDL_SysWMinfo info;
    int sx = 0, sy = 0;

    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(g_sdl, &info) || info.subsystem != SDL_SYSWM_X11) {
        fprintf(stderr, "[desktop_shell] GTK: SDL 不是 X11 窗口,无法贴菜单栏\n");
        return -1;
    }

    g_game_w = w > 0 ? w : 240;
    g_game_h = h > 0 ? h : 320;
    g_gtk_win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(g_gtk_win), "SkyEngine");
    gtk_window_set_resizable(GTK_WINDOW(g_gtk_win), FALSE);
    /* 只有菜单栏,不要 EventBox:那个空客户区就是点菜单变白的根因。 */
    gtk_widget_set_size_request(bar, g_game_w, -1);
    gtk_container_add(GTK_CONTAINER(g_gtk_win), bar);
    g_signal_connect(g_gtk_win, "delete-event", G_CALLBACK(on_gtk_delete), NULL);
    g_signal_connect(g_gtk_win, "configure-event", G_CALLBACK(on_gtk_configure), NULL);
    g_signal_connect(g_gtk_win, "map-event", G_CALLBACK(on_gtk_map), NULL);
    g_signal_connect(g_gtk_win, "unmap-event", G_CALLBACK(on_gtk_unmap), NULL);

    SDL_GetWindowPosition(g_sdl, &sx, &sy);
    gtk_window_move(GTK_WINDOW(g_gtk_win), sx, sy);

    gtk_widget_show_all(g_gtk_win);
    while (gtk_events_pending()) {
        gtk_main_iteration_do(FALSE);
    }
    if (!gtk_widget_get_window(g_gtk_win)) {
        fprintf(stderr, "[desktop_shell] GTK: 菜单窗尚未 realize,放弃贴靠\n");
        gtk_widget_destroy(g_gtk_win);
        g_gtk_win = NULL;
        return -1;
    }
    {
        int bh = gtk_widget_get_allocated_height(bar);
        if (bh > 0) g_bar_h = bh;
    }
    g_x11_dpy = info.info.x11.display;
    g_sdl_xid = info.info.x11.window;
    g_embedded = 1;
    g_has_menubar = 1;
    g_last_ox = -1;
#ifdef SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
#endif
    /* 先藏起来再去边框,避免启动瞬间出现两个带框窗口。 */
    SDL_HideWindow(g_sdl);
    SDL_SetWindowBordered(g_sdl, SDL_FALSE);
    SDL_SetWindowSize(g_sdl, g_game_w, g_game_h);
    apply_sdl_x11_hints();
    overlay_sdl();
    SDL_ShowWindow(g_sdl);
    apply_sdl_x11_hints();
    fprintf(stderr, "[desktop_shell] GTK menubar: X11 chrome+game %dx%d\n",
            g_game_w, g_game_h);
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
    /* 先清 g_embedded,避免 destroy 触发 unmap 把还活着的 SDL 藏起来。 */
    g_embedded = 0;
    if (g_gtk_win) {
        gtk_widget_destroy(g_gtk_win);
        g_gtk_win = NULL;
    }
#ifdef GDK_WINDOWING_X11
    g_x11_dpy = NULL;
    g_sdl_xid = 0;
#endif
    g_recent_menu = NULL;
    g_has_menubar = 0;
    g_overlay_lock = 0;
    g_last_ox = g_last_oy = g_last_bar_h = -1;
    g_sdl = NULL;
}

void desktop_shell_gtk_idle(void) {
    while (gtk_events_pending()) {
        gtk_main_iteration_do(FALSE);
    }
    /* 拖动时 configure 可能被合成器合并,16ms idle 再对齐一次。 */
    if (g_embedded) overlay_sdl();
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
        /* 游戏窗无边框且 skip-taskbar,标题只写在 GTK 上,避免任务栏两条同名。 */
        return;
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
    if (!g_sdl || g_overlay_lock) return;
    SDL_GetWindowSize(g_sdl, &w, &h);
    if (g_embedded && g_gtk_win) {
        int bar_w = gtk_widget_get_allocated_width(g_gtk_win);
        if (w > 0) g_game_w = w;
        if (h > 0) g_game_h = h;
        /* 只让菜单栏跟屏幕宽度对齐,不要把游戏高度写进 GTK,否则又变回带白洞的大框。 */
        if (g_game_w > 0 && g_game_w != bar_w) {
            gtk_widget_set_size_request(g_gtk_win, g_game_w, -1);
            gtk_window_resize(GTK_WINDOW(g_gtk_win), g_game_w, g_bar_h);
            g_last_ox = -1;
        }
        overlay_sdl();
    } else {
        dock_bar();
    }
}

int desktop_shell_gtk_handle_window_event(const union SDL_Event *ev) {
    if (!ev || ev->type != SDL_WINDOWEVENT) return 0;
    if (g_overlay_lock) return 0;
    switch (ev->window.event) {
        case SDL_WINDOWEVENT_MOVED:
            /* 位置以 GTK 为准;SDL 被 WM 拖走就拉回菜单栏下面。 */
            if (g_embedded) overlay_sdl();
            else desktop_shell_gtk_sync_window();
            break;
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
