#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <commdlg.h>
#include <shlobj.h>
#include <direct.h>
#endif

#include "./include/desktop_shell.h"
#include "./include/desktop_shell_internal.h"
#include "./include/file_lib.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifdef _MSC_VER
#include <SDL.h>
#include <SDL_syswm.h>
#elif defined(_WIN32)
#include "./windows/SDL2-2.0.10/i686-w64-mingw32/include/SDL2/SDL.h"
#include "./windows/SDL2-2.0.10/i686-w64-mingw32/include/SDL2/SDL_syswm.h"
#else
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#endif

#define RECENT_MAX DESKTOP_SHELL_RECENT_MAX

static DesktopShellHost g_host;
static int g_enabled;
static int g_pending_cmd;
static int g_pending_recent;
static char g_recent[RECENT_MAX][PATH_MAX];
static int g_recent_count;
static char g_last_ext[256];
static char g_last_entry[256];
static Uint32 g_shell_event;
static int g_native_menubar;

#ifdef _WIN32
static HWND g_hwnd;
static HMENU g_menu_bar;
static HMENU g_menu_recent;
static ATOM g_dlg_atom;
static int g_com_inited;
static WNDPROC g_sdl_wndproc;
#endif

static const char *env_nonempty(const char *a, const char *b) {
    const char *v = getenv(a);
    if (v && *v) return v;
    v = getenv(b);
    if (v && *v) return v;
    return NULL;
}

static void copy_str(char *dst, size_t dst_sz, const char *src) {
    if (!dst || dst_sz == 0) return;
    if (!src) src = "";
    snprintf(dst, dst_sz, "%s", src);
}

static void trim_inplace(char *s);

void desktop_shell_queue_cmd(int cmd) {
    SDL_Event ev;
    if (!g_enabled || cmd == CMD_NONE) return;
    if (g_shell_event == 0 || g_shell_event == (Uint32)-1) {
        g_pending_cmd = cmd;
        if (cmd >= CMD_RECENT_BASE && cmd < CMD_RECENT_BASE + RECENT_MAX) {
            g_pending_recent = cmd - CMD_RECENT_BASE;
        }
        return;
    }
    memset(&ev, 0, sizeof(ev));
    ev.type = g_shell_event;
    ev.user.code = cmd;
    SDL_PushEvent(&ev);
}

int desktop_shell_recent_count(void) {
    return g_recent_count;
}

const char *desktop_shell_recent_at(int index) {
    if (index < 0 || index >= g_recent_count) return NULL;
    return g_recent[index];
}

const char *desktop_shell_last_ext(void) {
    return g_last_ext[0] ? g_last_ext : "start.mr";
}

const char *desktop_shell_last_entry(void) {
    return g_last_entry;
}

const char *desktop_shell_current_mrp(void) {
    return g_host.args ? g_host.args->mrp_path : "";
}

extern int32 my_configureDnsMap(const char *map);
void native_audio_set_sf2(const char *path);

void desktop_dns_format(const DesktopDnsRule *rule, char *out, size_t n) {
    if (!out || n == 0) return;
    if (!rule) {
        out[0] = '\0';
        return;
    }
    snprintf(out, n, "%s -> %s", rule->from, rule->to);
}

int desktop_dns_parse(const char *map, DesktopDnsRule *rules, int max) {
    char buf[SKYENGINE_DNS_MAP_LIMIT];
    char *entry;
    char *next;
    int n = 0;
    if (!rules || max <= 0) return 0;
    if (!map || !*map) return 0;
    copy_str(buf, sizeof(buf), map);
    entry = buf;
    while (entry && *entry && n < max) {
        char *sep;
        next = entry;
        while (*next && *next != ',' && *next != ';' && *next != '\n' && *next != '\r') {
            next++;
        }
        if (*next) {
            *next = '\0';
            next++;
        } else {
            next = NULL;
        }
        trim_inplace(entry);
        if (!*entry) {
            entry = next;
            continue;
        }
        sep = strstr(entry, "->");
        if (sep) {
            *sep = '\0';
            trim_inplace(entry);
            trim_inplace(sep + 2);
            if (entry[0] && sep[2]) {
                copy_str(rules[n].from, sizeof(rules[n].from), entry);
                copy_str(rules[n].to, sizeof(rules[n].to), sep + 2);
                n++;
            }
        } else {
            sep = strchr(entry, '=');
            if (sep) {
                *sep = '\0';
                trim_inplace(entry);
                trim_inplace(sep + 1);
                if (entry[0] && sep[1]) {
                    copy_str(rules[n].from, sizeof(rules[n].from), entry);
                    copy_str(rules[n].to, sizeof(rules[n].to), sep + 1);
                    n++;
                }
            }
        }
        entry = next;
    }
    return n;
}

int desktop_dns_serialize(const DesktopDnsRule *rules, int count, char *out, size_t n) {
    int i;
    size_t used = 0;
    if (!out || n == 0) return MR_FAILED;
    out[0] = '\0';
    if (!rules || count <= 0) return MR_SUCCESS;
    for (i = 0; i < count; i++) {
        char piece[DESKTOP_DNS_HOST_MAX * 2 + 8];
        size_t plen;
        if (!rules[i].from[0] || !rules[i].to[0]) continue;
        snprintf(piece, sizeof(piece), "%s%s->%s", used ? ";" : "", rules[i].from, rules[i].to);
        plen = strlen(piece);
        if (used + plen >= n) return MR_FAILED;
        memcpy(out + used, piece, plen + 1);
        used += plen;
    }
    return MR_SUCCESS;
}

static void trim_inplace(char *s) {
    char *end;
    if (!s) return;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
        memmove(s, s + 1, strlen(s));
    }
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
        *--end = '\0';
    }
}

static const char *path_basename(const char *path) {
    const char *slash;
    if (!path || !*path) return path;
    slash = strrchr(path, '/');
#ifdef _WIN32
    {
        const char *bslash = strrchr(path, '\\');
        if (bslash && (!slash || bslash > slash)) slash = bslash;
    }
#endif
    return slash ? slash + 1 : path;
}

static void shell_error(const char *utf8) {
    if (!utf8) utf8 = "操作失败";
#ifdef _WIN32
    {
        int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
        wchar_t *wide = NULL;
        if (n > 0) wide = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
        if (wide && MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, n) > 0) {
            MessageBoxW(g_hwnd, wide, L"SkyEngine", MB_OK | MB_ICONERROR);
            free(wide);
            return;
        }
        free(wide);
    }
#endif
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "SkyEngine", utf8, g_host.window);
}

#ifdef _WIN32
static wchar_t *utf8_to_wide(const char *text) {
    int len;
    wchar_t *wide;
    if (!text) text = "";
    len = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    if (len <= 0) return NULL;
    wide = (wchar_t *)malloc((size_t)len * sizeof(wchar_t));
    if (!wide) return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, len) <= 0) {
        free(wide);
        return NULL;
    }
    return wide;
}

static int wide_to_utf8(const wchar_t *wide, char *out, size_t out_sz) {
    if (!wide || !out || out_sz == 0) return MR_FAILED;
    if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, (int)out_sz, NULL, NULL) <= 0) {
        return MR_FAILED;
    }
    return MR_SUCCESS;
}
#endif

static int config_path(char *out, size_t out_sz) {
#ifdef _WIN32
    wchar_t wdir[MAX_PATH];
    wchar_t wpath[MAX_PATH];
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, SHGFP_TYPE_CURRENT, wdir))) {
        return MR_FAILED;
    }
    _snwprintf(wpath, MAX_PATH, L"%s\\SkyEngine", wdir);
    wpath[MAX_PATH - 1] = L'\0';
    CreateDirectoryW(wpath, NULL);
    _snwprintf(wpath, MAX_PATH, L"%s\\SkyEngine\\ui.cfg", wdir);
    wpath[MAX_PATH - 1] = L'\0';
    return wide_to_utf8(wpath, out, out_sz);
#elif defined(__APPLE__)
    const char *home = getenv("HOME");
    char dir[PATH_MAX];
    if (!home || !*home) return MR_FAILED;
    snprintf(dir, sizeof(dir), "%s/Library/Application Support/SkyEngine", home);
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) return MR_FAILED;
    snprintf(out, out_sz, "%s/ui.cfg", dir);
    return MR_SUCCESS;
#else
    const char *home = getenv("XDG_CONFIG_HOME");
    char dir[PATH_MAX];
    if (home && *home) {
        snprintf(dir, sizeof(dir), "%s/skyengine", home);
    } else {
        home = getenv("HOME");
        if (!home || !*home) return MR_FAILED;
        snprintf(dir, sizeof(dir), "%s/.config/skyengine", home);
    }
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        return MR_FAILED;
    }
    snprintf(out, out_sz, "%s/ui.cfg", dir);
    return MR_SUCCESS;
#endif
}

static void recent_add(const char *path) {
    int i;
    char copy[PATH_MAX];
    if (!path || !*path) return;
    copy_str(copy, sizeof(copy), path);
    for (i = 0; i < g_recent_count; i++) {
        if (strcmp(g_recent[i], copy) == 0) {
            memmove(&g_recent[i], &g_recent[i + 1],
                    (size_t)(g_recent_count - i - 1) * sizeof(g_recent[0]));
            g_recent_count--;
            break;
        }
    }
    if (g_recent_count >= RECENT_MAX) g_recent_count = RECENT_MAX - 1;
    memmove(&g_recent[1], &g_recent[0], (size_t)g_recent_count * sizeof(g_recent[0]));
    copy_str(g_recent[0], sizeof(g_recent[0]), copy);
    g_recent_count++;
}

static void load_ui_config(SkyEngineArgs *args) {
    char path[PATH_MAX];
    char line[SKYENGINE_DNS_MAP_LIMIT + 64];
    FILE *fp;
    if (config_path(path, sizeof(path)) != MR_SUCCESS) return;
    fp = skyengine_host_fopen(path, "r");
    if (!fp) return;
    while (fgets(line, sizeof(line), fp)) {
        char *eq;
        char *key;
        char *val;
        trim_inplace(line);
        if (!line[0] || line[0] == '#') continue;
        eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        key = line;
        val = eq + 1;
        trim_inplace(key);
        if (strcmp(key, "screen") == 0 && !(args->sourced & SKYENGINE_SRC_SCREEN)) {
            int w = 0, h = 0;
            if (skyengine_args_parse_screen(val, &w, &h) == MR_SUCCESS) {
                args->screen_width = w;
                args->screen_height = h;
            }
        } else if (strcmp(key, "memory") == 0 && !(args->sourced & SKYENGINE_SRC_MEMORY)) {
            int mb = 0;
            if (skyengine_args_parse_memory(val, &mb) == MR_SUCCESS) args->memory_mb = mb;
        } else if (strcmp(key, "device_date") == 0 && !(args->sourced & SKYENGINE_SRC_DATE)) {
            int y = 0, m = 0, d = 0;
            if (skyengine_args_parse_device_date(val, &y, &m, &d) == MR_SUCCESS) {
                args->device_year = y;
                args->device_month = m;
                args->device_day = d;
            }
        } else if (strcmp(key, "work_dir") == 0 && !(args->sourced & SKYENGINE_SRC_WORKDIR)) {
            char resolved[PATH_MAX];
            if (val[0] && skyengine_args_resolve_dir(val, resolved, sizeof(resolved)) == MR_SUCCESS) {
                copy_str(args->work_dir, sizeof(args->work_dir), resolved);
            }
        } else if (strcmp(key, "dns_map") == 0 && !(args->sourced & SKYENGINE_SRC_DNS)) {
            copy_str(args->dns_map, sizeof(args->dns_map), val);
        } else if (strcmp(key, "sf2") == 0 && !(args->sourced & SKYENGINE_SRC_SF2)) {
            copy_str(args->sf2_path, sizeof(args->sf2_path), val);
        } else if (strcmp(key, "profile") == 0 && !(args->sourced & SKYENGINE_SRC_PROFILE)) {
            /* 桌面「设置运行模式」:CLI/--profile/SKYENGINE_PROFILE 优先于 ui.cfg。 */
            int profile = SKYENGINE_PROFILE_SPEED;
            if (skyengine_args_parse_profile(val, &profile) == MR_SUCCESS) {
                args->compat_priority = profile;
            }
        } else if (strcmp(key, "last_ext") == 0) {
            copy_str(g_last_ext, sizeof(g_last_ext), val);
        } else if (strcmp(key, "last_entry") == 0) {
            copy_str(g_last_entry, sizeof(g_last_entry), val);
        } else if (strncmp(key, "recent", 6) == 0 && g_recent_count < RECENT_MAX && val[0]) {
            copy_str(g_recent[g_recent_count], sizeof(g_recent[0]), val);
            g_recent_count++;
        }
    }
    fclose(fp);
}

static void save_ui_config(void) {
    char path[PATH_MAX];
    char date[32];
    FILE *fp;
    int i;
    SkyEngineArgs *args = g_host.args;
    if (!args) return;
    if (config_path(path, sizeof(path)) != MR_SUCCESS) return;
    fp = skyengine_host_fopen(path, "w");
    if (!fp) return;
    skyengine_args_format_device_date(args, date, sizeof(date));
    fprintf(fp, "screen=%dx%d\n", args->screen_width, args->screen_height);
    fprintf(fp, "memory=%dM\n", args->memory_mb > 0 ? args->memory_mb : DEFAULT_MEMORY_MB);
    fprintf(fp, "device_date=%s\n", date);
    fprintf(fp, "work_dir=%s\n", args->work_dir);
    fprintf(fp, "dns_map=%s\n", args->dns_map);
    fprintf(fp, "sf2=%s\n", args->sf2_path);
    fprintf(fp, "profile=%s\n", args->compat_priority ? "compat" : "speed");
    fprintf(fp, "last_ext=%s\n", g_last_ext[0] ? g_last_ext : "start.mr");
    fprintf(fp, "last_entry=%s\n", g_last_entry);
    for (i = 0; i < g_recent_count; i++) {
        fprintf(fp, "recent%d=%s\n", i, g_recent[i]);
    }
    fclose(fp);
}

static void resolve_ext_choice(const char *input, char *out, size_t out_sz) {
    const char *env;
    if (!input) input = "";
    if (!input[0] || strcmp(input, "VMRP_EXT") == 0) {
        env = env_nonempty("VMRP_EXT", "SKYENGINE_EXT");
        copy_str(out, out_sz, env ? env : "start.mr");
        return;
    }
    copy_str(out, out_sz, input);
}

static void resolve_entry_choice(const char *input, char *out, size_t out_sz) {
    const char *env;
    if (!input) input = "";
    if (strcmp(input, "VMRP_ENTRY") == 0) {
        env = env_nonempty("VMRP_ENTRY", "SKYENGINE_ENTRY");
        copy_str(out, out_sz, env ? env : "");
        return;
    }
    if (strcmp(input, "空") == 0 || strcmp(input, "（空）") == 0) {
        out[0] = '\0';
        return;
    }
    copy_str(out, out_sz, input);
}

static int find_dsm_gm(char *out, size_t out_sz) {
    /* startEngine 会 chdir 到 work_dir,所以这里用短相对路径,避开 128 字节限制。 */
    if (my_info("dsm_gm.mrp") == MR_IS_FILE) {
        copy_str(out, out_sz, "dsm_gm.mrp");
        return MR_SUCCESS;
    }
    if (my_info("mythroad/dsm_gm.mrp") == MR_IS_FILE) {
        copy_str(out, out_sz, "mythroad/dsm_gm.mrp");
        return MR_SUCCESS;
    }
    return MR_FAILED;
}

static int launch_mrp(const char *path, const char *ext, const char *entry) {
    SkyEngineArgs *args = g_host.args;
    if (!args || !path || !*path) return MR_FAILED;
    if (strlen(path) >= VMRP_MRP_NAME_LIMIT) {
        shell_error("MRP 路径过长（Mythroad 限制 128 字节），请把文件移到更短的位置。");
        return MR_FAILED;
    }
    copy_str(args->mrp_path, sizeof(args->mrp_path), path);
    copy_str(args->ext_name, sizeof(args->ext_name), ext && *ext ? ext : "start.mr");
    copy_str(args->entry, sizeof(args->entry), entry ? entry : "");
    copy_str(g_last_ext, sizeof(g_last_ext), args->ext_name);
    copy_str(g_last_entry, sizeof(g_last_entry), args->entry);
    recent_add(path);
    save_ui_config();
    if (g_host.stop_timer) g_host.stop_timer();
    if (!g_host.restart_engine || g_host.restart_engine() != 0) {
        shell_error("启动失败，请查看 log.txt");
        return MR_FAILED;
    }
    desktop_shell_refresh();
    return MR_SUCCESS;
}

static int apply_settings_and_restart(void) {
    save_ui_config();
    if (g_host.stop_timer) g_host.stop_timer();
    if (!g_host.restart_engine || g_host.restart_engine() != 0) {
        shell_error("重新启动失败，请查看 log.txt");
        return MR_FAILED;
    }
    desktop_shell_refresh();
    return MR_SUCCESS;
}

#ifdef _WIN32
static void apply_font(HWND hwnd) {
    SendMessageW(hwnd, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
}

/* SDL_WaitEvent 默认不会因为 Win32 菜单 WM_COMMAND 醒来。把 SDL 窗口过程
 * 链起来,把菜单命令转成 SDL 用户事件,主循环才能在 pump() 里弹对话框。 */
static LRESULT CALLBACK shell_sdl_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_COMMAND && g_shell_event != (Uint32)-1) {
        int cmd = (int)LOWORD(wparam);
        if (cmd) {
            SDL_Event ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = g_shell_event;
            ev.user.code = cmd;
            SDL_PushEvent(&ev);
        }
        return 0;
    }
    if (g_sdl_wndproc) {
        return CallWindowProcW(g_sdl_wndproc, hwnd, msg, wparam, lparam);
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static int win_pick_mrp(char *out, size_t out_sz) {
    wchar_t file[MAX_PATH];
    OPENFILENAMEW ofn;
    file[0] = L'\0';
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = L"MRP 文件 (*.mrp)\0*.mrp\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"选择要启动的 MRP 文件";
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
                OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) return MR_FAILED;
    return wide_to_utf8(file, out, out_sz);
}

static int win_pick_dir(char *out, size_t out_sz) {
    BROWSEINFOW bi;
    LPITEMIDLIST pidl;
    wchar_t folder[MAX_PATH];
    memset(&bi, 0, sizeof(bi));
    bi.hwndOwner = g_hwnd;
    bi.lpszTitle = L"设置运行和 MRP 文件系统的工作目录";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return MR_FAILED;
    folder[0] = L'\0';
    if (!SHGetPathFromIDListW(pidl, folder)) {
        CoTaskMemFree(pidl);
        return MR_FAILED;
    }
    CoTaskMemFree(pidl);
    return wide_to_utf8(folder, out, out_sz);
}

#define DLG_ADVANCED 1
#define DLG_PROMPT 2
#define DLG_DNS 3
#define DLG_DNS_PAIR 4
#define IDC_DNS_LIST 300
#define IDC_DNS_ADD 301
#define IDC_DNS_DEL 302

typedef struct {
    int kind;
    int result; /* 1 ok, -1 cancel, 0 running */
    HWND hwnd;
    HWND mrp;
    HWND ext;
    HWND entry;
    char mrp_utf8[PATH_MAX];
    char ext_utf8[256];
    char entry_utf8[256];
} AdvancedDlg;

typedef struct {
    int kind;
    int result;
    HWND hwnd;
    HWND edit;
    HWND combo;
    int use_combo;
    char text_utf8[SKYENGINE_DNS_MAP_LIMIT];
} PromptDlg;

typedef struct {
    int kind;
    int result;
    HWND hwnd;
    HWND list;
    DesktopDnsRule rules[DESKTOP_DNS_RULE_MAX];
    int count;
    char map_utf8[SKYENGINE_DNS_MAP_LIMIT];
} DnsDlg;

typedef struct {
    int kind;
    int result;
    HWND hwnd;
    HWND from;
    HWND to;
    char from_utf8[DESKTOP_DNS_HOST_MAX];
    char to_utf8[DESKTOP_DNS_HOST_MAX];
} DnsPairDlg;

static HWND add_label(HWND parent, const wchar_t *text, int x, int y, int w, int h) {
    HWND hwnd = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE,
                              x, y, w, h, parent, NULL, GetModuleHandleW(NULL), NULL);
    apply_font(hwnd);
    return hwnd;
}

static void dlg_capture_window_text(HWND field, char *out, size_t out_sz) {
    wchar_t wbuf[SKYENGINE_DNS_MAP_LIMIT];
    if (!field || !out || out_sz == 0) return;
    wbuf[0] = L'\0';
    GetWindowTextW(field, wbuf, (int)(sizeof(wbuf) / sizeof(wbuf[0])));
    wide_to_utf8(wbuf, out, out_sz);
    trim_inplace(out);
}

static void dlg_finish(HWND hwnd, int result) {
    PromptDlg *any = (PromptDlg *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    AdvancedDlg *adv = (AdvancedDlg *)any;
    if (!any) {
        DestroyWindow(hwnd);
        return;
    }
    if (any->kind == DLG_ADVANCED && result == 1) {
        dlg_capture_window_text(adv->mrp, adv->mrp_utf8, sizeof(adv->mrp_utf8));
        dlg_capture_window_text(adv->ext, adv->ext_utf8, sizeof(adv->ext_utf8));
        dlg_capture_window_text(adv->entry, adv->entry_utf8, sizeof(adv->entry_utf8));
    } else if (any->kind == DLG_PROMPT && result == 1) {
        HWND field = any->use_combo ? any->combo : any->edit;
        dlg_capture_window_text(field, any->text_utf8, sizeof(any->text_utf8));
    } else if (any->kind == DLG_DNS) {
        DnsDlg *dns = (DnsDlg *)any;
        desktop_dns_serialize(dns->rules, dns->count, dns->map_utf8, sizeof(dns->map_utf8));
    } else if (any->kind == DLG_DNS_PAIR && result == 1) {
        DnsPairDlg *pair = (DnsPairDlg *)any;
        dlg_capture_window_text(pair->from, pair->from_utf8, sizeof(pair->from_utf8));
        dlg_capture_window_text(pair->to, pair->to_utf8, sizeof(pair->to_utf8));
    }
    any->result = result;
    DestroyWindow(hwnd);
}

static void dns_fill_listbox(DnsDlg *dns) {
    int i;
    if (!dns || !dns->list) return;
    SendMessageW(dns->list, LB_RESETCONTENT, 0, 0);
    for (i = 0; i < dns->count; i++) {
        char line[DESKTOP_DNS_HOST_MAX * 2 + 8];
        wchar_t *wide;
        desktop_dns_format(&dns->rules[i], line, sizeof(line));
        wide = utf8_to_wide(line);
        if (wide) {
            SendMessageW(dns->list, LB_ADDSTRING, 0, (LPARAM)wide);
            free(wide);
        }
    }
}

static int win_dns_pair_dialog(HWND owner, char *from, size_t from_n, char *to, size_t to_n) {
    DnsPairDlg dlg;
    RECT pr;
    MSG msg;
    HWND ok, cancel;
    HWND parent = owner ? owner : g_hwnd;
    int x = 0, y = 0;
    memset(&dlg, 0, sizeof(dlg));
    dlg.kind = DLG_DNS_PAIR;
    if (parent && GetWindowRect(parent, &pr)) {
        x = pr.left + 40;
        y = pr.top + 40;
    }
    dlg.hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"SkyEngineDlg", L"新增域名替换",
                               WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_VISIBLE,
                               x, y, 420, 200, parent, NULL, GetModuleHandleW(NULL), NULL);
    if (!dlg.hwnd) return MR_FAILED;
    SetWindowLongPtrW(dlg.hwnd, GWLP_USERDATA, (LONG_PTR)&dlg);
    add_label(dlg.hwnd, L"被替换值", 16, 12, 370, 20);
    dlg.from = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                               16, 36, 370, 24, dlg.hwnd, (HMENU)310, GetModuleHandleW(NULL), NULL);
    add_label(dlg.hwnd, L"替换值", 16, 68, 370, 20);
    dlg.to = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                             16, 92, 370, 24, dlg.hwnd, (HMENU)311, GetModuleHandleW(NULL), NULL);
    ok = CreateWindowW(L"BUTTON", L"确定",
                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                       206, 128, 90, 28, dlg.hwnd, (HMENU)IDOK, GetModuleHandleW(NULL), NULL);
    cancel = CreateWindowW(L"BUTTON", L"取消",
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                           306, 128, 90, 28, dlg.hwnd, (HMENU)IDCANCEL, GetModuleHandleW(NULL), NULL);
    apply_font(dlg.from);
    apply_font(dlg.to);
    apply_font(ok);
    apply_font(cancel);
    EnableWindow(parent, FALSE);
    while (dlg.result == 0 && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(dlg.hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(parent, TRUE);
    if (dlg.result == 1) {
        copy_str(from, from_n, dlg.from_utf8);
        copy_str(to, to_n, dlg.to_utf8);
        return (from[0] && to[0]) ? MR_SUCCESS : MR_FAILED;
    }
    return MR_FAILED;
}

static LRESULT CALLBACK shell_dlg_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    PromptDlg *any = (PromptDlg *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    AdvancedDlg *adv = (AdvancedDlg *)any;
    switch (msg) {
        case WM_COMMAND:
            if (!any) break;
            if (any->kind == DLG_DNS) {
                DnsDlg *dns = (DnsDlg *)any;
                if (LOWORD(wparam) == IDC_DNS_ADD) {
                    char from[DESKTOP_DNS_HOST_MAX];
                    char to[DESKTOP_DNS_HOST_MAX];
                    from[0] = '\0';
                    to[0] = '\0';
                    if (dns->count >= DESKTOP_DNS_RULE_MAX) {
                        shell_error("替换规则已满（最多 32 条）。");
                        return 0;
                    }
                    if (win_dns_pair_dialog(dns->hwnd, from, sizeof(from), to, sizeof(to)) == MR_SUCCESS) {
                        copy_str(dns->rules[dns->count].from, sizeof(dns->rules[0].from), from);
                        copy_str(dns->rules[dns->count].to, sizeof(dns->rules[0].to), to);
                        dns->count++;
                        dns_fill_listbox(dns);
                    }
                    return 0;
                }
                if (LOWORD(wparam) == IDC_DNS_DEL) {
                    int sel = (int)SendMessageW(dns->list, LB_GETCURSEL, 0, 0);
                    if (sel < 0 || sel >= dns->count) {
                        shell_error("请先选择要删除的规则。");
                        return 0;
                    }
                    if (sel + 1 < dns->count) {
                        memmove(&dns->rules[sel], &dns->rules[sel + 1],
                                (size_t)(dns->count - sel - 1) * sizeof(dns->rules[0]));
                    }
                    dns->count--;
                    dns_fill_listbox(dns);
                    return 0;
                }
            }
            if (LOWORD(wparam) == 201) {
                char path[PATH_MAX];
                wchar_t *wide;
                if (any->kind == DLG_ADVANCED) {
                    if (win_pick_mrp(path, sizeof(path)) == MR_SUCCESS) {
                        wide = utf8_to_wide(path);
                        if (wide) {
                            SetWindowTextW(adv->mrp, wide);
                            free(wide);
                        }
                    }
                } else if (win_pick_dir(path, sizeof(path)) == MR_SUCCESS) {
                    wide = utf8_to_wide(path);
                    if (wide) {
                        SetWindowTextW(any->edit, wide);
                        free(wide);
                    }
                }
                return 0;
            }
            if (LOWORD(wparam) == IDOK) {
                dlg_finish(hwnd, 1);
                return 0;
            }
            if (LOWORD(wparam) == IDCANCEL) {
                dlg_finish(hwnd, -1);
                return 0;
            }
            break;
        case WM_CLOSE:
            /* 域名列表没有取消:关掉窗口即采用当前增删结果。 */
            dlg_finish(hwnd, any && any->kind == DLG_DNS ? 1 : -1);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static int win_advanced_dialog(char *mrp, size_t mrp_sz, char *ext, size_t ext_sz,
                               char *entry, size_t entry_sz) {
    AdvancedDlg dlg;
    RECT pr;
    MSG msg;
    HWND browse, ok, cancel;
    wchar_t *init_mrp;
    const char *ext_env = env_nonempty("VMRP_EXT", "SKYENGINE_EXT");
    const char *entry_env = env_nonempty("VMRP_ENTRY", "SKYENGINE_ENTRY");
    int x = 0, y = 0;
    memset(&dlg, 0, sizeof(dlg));
    dlg.kind = DLG_ADVANCED;
    if (g_hwnd && GetWindowRect(g_hwnd, &pr)) {
        x = pr.left + 40;
        y = pr.top + 40;
    }
    dlg.hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"SkyEngineDlg", L"高级启动",
                               WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_VISIBLE,
                               x, y, 510, 340, g_hwnd, NULL, GetModuleHandleW(NULL), NULL);
    if (!dlg.hwnd) return MR_FAILED;
    SetWindowLongPtrW(dlg.hwnd, GWLP_USERDATA, (LONG_PTR)&dlg);
    add_label(dlg.hwnd, L"选择要启动的 MRP 文件", 16, 12, 460, 20);
    dlg.mrp = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                              16, 36, 360, 24, dlg.hwnd, (HMENU)200, GetModuleHandleW(NULL), NULL);
    browse = CreateWindowW(L"BUTTON", L"浏览...",
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                           386, 36, 90, 24, dlg.hwnd, (HMENU)201, GetModuleHandleW(NULL), NULL);
    add_label(dlg.hwnd, L"EXT_NAME（VMRP_EXT 或 start.mr）", 16, 72, 460, 20);
    dlg.ext = CreateWindowW(L"COMBOBOX", L"",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWN | WS_VSCROLL,
                            16, 96, 460, 160, dlg.hwnd, (HMENU)202, GetModuleHandleW(NULL), NULL);
    add_label(dlg.hwnd, L"ENTRY（VMRP_ENTRY 或空）", 16, 132, 460, 20);
    dlg.entry = CreateWindowW(L"COMBOBOX", L"",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWN | WS_VSCROLL,
                              16, 156, 460, 160, dlg.hwnd, (HMENU)203, GetModuleHandleW(NULL), NULL);
    ok = CreateWindowW(L"BUTTON", L"确定",
                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                       286, 214, 90, 28, dlg.hwnd, (HMENU)IDOK, GetModuleHandleW(NULL), NULL);
    cancel = CreateWindowW(L"BUTTON", L"取消",
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                           386, 214, 90, 28, dlg.hwnd, (HMENU)IDCANCEL, GetModuleHandleW(NULL), NULL);
    apply_font(dlg.mrp);
    apply_font(browse);
    apply_font(dlg.ext);
    apply_font(dlg.entry);
    apply_font(ok);
    apply_font(cancel);
    init_mrp = utf8_to_wide(mrp[0] ? mrp : (g_host.args ? g_host.args->mrp_path : ""));
    if (init_mrp) {
        SetWindowTextW(dlg.mrp, init_mrp);
        free(init_mrp);
    }
    SendMessageW(dlg.ext, CB_ADDSTRING, 0, (LPARAM)L"VMRP_EXT");
    SendMessageW(dlg.ext, CB_ADDSTRING, 0, (LPARAM)L"start.mr");
    if (ext_env && strcmp(ext_env, "start.mr") != 0) {
        wchar_t *wenv = utf8_to_wide(ext_env);
        if (wenv) {
            SendMessageW(dlg.ext, CB_ADDSTRING, 0, (LPARAM)wenv);
            free(wenv);
        }
    }
    {
        wchar_t *wext = utf8_to_wide(g_last_ext[0] ? g_last_ext : "start.mr");
        if (wext) {
            SetWindowTextW(dlg.ext, wext);
            free(wext);
        }
    }
    SendMessageW(dlg.entry, CB_ADDSTRING, 0, (LPARAM)L"VMRP_ENTRY");
    SendMessageW(dlg.entry, CB_ADDSTRING, 0, (LPARAM)L"（空）");
    if (entry_env) {
        wchar_t *wenv = utf8_to_wide(entry_env);
        if (wenv) {
            SendMessageW(dlg.entry, CB_ADDSTRING, 0, (LPARAM)wenv);
            free(wenv);
        }
    }
    {
        wchar_t *wentry = utf8_to_wide(g_last_entry[0] ? g_last_entry : "（空）");
        if (wentry) {
            SetWindowTextW(dlg.entry, wentry);
            free(wentry);
        } else {
            SetWindowTextW(dlg.entry, L"（空）");
        }
    }
    EnableWindow(g_hwnd, FALSE);
    while (dlg.result == 0 && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(dlg.hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (dlg.result == 1) {
        copy_str(mrp, mrp_sz, dlg.mrp_utf8);
        copy_str(ext, ext_sz, dlg.ext_utf8);
        copy_str(entry, entry_sz, dlg.entry_utf8);
    }
    EnableWindow(g_hwnd, TRUE);
    if (g_hwnd) SetForegroundWindow(g_hwnd);
    return dlg.result == 1 ? MR_SUCCESS : MR_FAILED;
}

static int win_prompt(const wchar_t *title, const wchar_t *label, const char *initial,
                      char *out, size_t out_sz, int multiline, int folder_browse,
                      const wchar_t **choices, int choice_count, int list_only) {
    PromptDlg dlg;
    RECT pr;
    MSG msg;
    HWND ok, cancel, browse = NULL;
    wchar_t *winit;
    int x = 0, y = 0;
    int edit_h = multiline ? 160 : 24;
    int dlg_h = multiline ? 340 : 190;
    int i;
    memset(&dlg, 0, sizeof(dlg));
    dlg.kind = DLG_PROMPT;
    dlg.use_combo = choices != NULL && choice_count > 0;
    if (g_hwnd && GetWindowRect(g_hwnd, &pr)) {
        x = pr.left + 40;
        y = pr.top + 40;
    }
    dlg.hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"SkyEngineDlg", title,
                               WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_VISIBLE,
                               x, y, 500, dlg_h, g_hwnd, NULL, GetModuleHandleW(NULL), NULL);
    if (!dlg.hwnd) return MR_FAILED;
    SetWindowLongPtrW(dlg.hwnd, GWLP_USERDATA, (LONG_PTR)&dlg);
    add_label(dlg.hwnd, label, 16, 12, 460, 36);
    if (dlg.use_combo) {
        DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                      (list_only ? CBS_DROPDOWNLIST : CBS_DROPDOWN);
        dlg.combo = CreateWindowW(L"COMBOBOX", L"", style,
                                  16, 56, folder_browse ? 360 : 450, 160,
                                  dlg.hwnd, (HMENU)200, GetModuleHandleW(NULL), NULL);
        apply_font(dlg.combo);
        for (i = 0; i < choice_count; i++) {
            SendMessageW(dlg.combo, CB_ADDSTRING, 0, (LPARAM)choices[i]);
        }
        winit = utf8_to_wide(initial ? initial : "");
        if (winit) {
            if (list_only) {
                int idx = (int)SendMessageW(dlg.combo, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)winit);
                if (idx < 0) idx = 0;
                SendMessageW(dlg.combo, CB_SETCURSEL, (WPARAM)idx, 0);
            } else {
                SetWindowTextW(dlg.combo, winit);
            }
            free(winit);
        }
    } else {
        DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | WS_BORDER;
        if (multiline) style |= ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL | WS_VSCROLL;
        dlg.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                   style, 16, 56, folder_browse ? 360 : 450, edit_h,
                                   dlg.hwnd, (HMENU)200, GetModuleHandleW(NULL), NULL);
        apply_font(dlg.edit);
        winit = utf8_to_wide(initial ? initial : "");
        if (winit) {
            SetWindowTextW(dlg.edit, winit);
            free(winit);
        }
    }
    if (folder_browse) {
        browse = CreateWindowW(L"BUTTON", L"浏览...",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                               386, 56, 90, 24, dlg.hwnd, (HMENU)201,
                               GetModuleHandleW(NULL), NULL);
        apply_font(browse);
    }
    ok = CreateWindowW(L"BUTTON", L"确定",
                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                       276, dlg_h - 70, 90, 28, dlg.hwnd, (HMENU)IDOK,
                       GetModuleHandleW(NULL), NULL);
    cancel = CreateWindowW(L"BUTTON", L"取消",
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                           376, dlg_h - 70, 90, 28, dlg.hwnd, (HMENU)IDCANCEL,
                           GetModuleHandleW(NULL), NULL);
    apply_font(ok);
    apply_font(cancel);
    EnableWindow(g_hwnd, FALSE);
    while (dlg.result == 0 && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(dlg.hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (dlg.result == 1) {
        copy_str(out, out_sz, dlg.text_utf8);
    }
    EnableWindow(g_hwnd, TRUE);
    if (g_hwnd) SetForegroundWindow(g_hwnd);
    return dlg.result == 1 ? MR_SUCCESS : MR_FAILED;
}

static int win_dns_editor(char *map, size_t map_n) {
    DnsDlg dlg;
    RECT pr;
    MSG msg;
    HWND add, del;
    int x = 0, y = 0;
    memset(&dlg, 0, sizeof(dlg));
    dlg.kind = DLG_DNS;
    dlg.count = desktop_dns_parse(map, dlg.rules, DESKTOP_DNS_RULE_MAX);
    if (g_hwnd && GetWindowRect(g_hwnd, &pr)) {
        x = pr.left + 40;
        y = pr.top + 40;
    }
    dlg.hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"SkyEngineDlg", L"域名替换规则",
                               WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_VISIBLE,
                               x, y, 520, 380, g_hwnd, NULL, GetModuleHandleW(NULL), NULL);
    if (!dlg.hwnd) return MR_FAILED;
    SetWindowLongPtrW(dlg.hwnd, GWLP_USERDATA, (LONG_PTR)&dlg);
    add_label(dlg.hwnd, L"现有替换（选择一条后可删除）", 16, 12, 470, 20);
    dlg.list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                                   LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                               16, 36, 470, 240, dlg.hwnd, (HMENU)IDC_DNS_LIST,
                               GetModuleHandleW(NULL), NULL);
    add = CreateWindowW(L"BUTTON", L"新增",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                        16, 292, 90, 28, dlg.hwnd, (HMENU)IDC_DNS_ADD,
                        GetModuleHandleW(NULL), NULL);
    del = CreateWindowW(L"BUTTON", L"删除",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        116, 292, 90, 28, dlg.hwnd, (HMENU)IDC_DNS_DEL,
                        GetModuleHandleW(NULL), NULL);
    apply_font(dlg.list);
    apply_font(add);
    apply_font(del);
    dns_fill_listbox(&dlg);
    EnableWindow(g_hwnd, FALSE);
    while (dlg.result == 0 && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(dlg.hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(g_hwnd, TRUE);
    if (g_hwnd) SetForegroundWindow(g_hwnd);
    if (dlg.result == 1) {
        copy_str(map, map_n, dlg.map_utf8);
        return MR_SUCCESS;
    }
    return MR_FAILED;
}

static int win_pick_sf2(char *out, size_t out_sz) {
    wchar_t file[MAX_PATH];
    OPENFILENAMEW ofn;
    file[0] = L'\0';
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = L"SoundFont (*.sf2)\0*.sf2\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"选择 SoundFont (SF2)";
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
                OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) return MR_FAILED;
    return wide_to_utf8(file, out, out_sz);
}

static void win_rebuild_recent(void) {
    int i;
    if (!g_menu_recent) return;
    while (GetMenuItemCount(g_menu_recent) > 0) {
        DeleteMenu(g_menu_recent, 0, MF_BYPOSITION);
    }
    if (g_recent_count <= 0) {
        AppendMenuW(g_menu_recent, MF_GRAYED | MF_STRING, 0, L"（空）");
        if (g_hwnd) DrawMenuBar(g_hwnd);
        return;
    }
    for (i = 0; i < g_recent_count; i++) {
        wchar_t *wide = utf8_to_wide(g_recent[i]);
        if (wide) {
            AppendMenuW(g_menu_recent, MF_STRING, (UINT)(CMD_RECENT_BASE + i), wide);
            free(wide);
        }
    }
    if (g_hwnd) DrawMenuBar(g_hwnd);
}

static int win_create_menu(void) {
    HMENU file_menu;
    HMENU settings_menu;
    SDL_SysWMinfo info;
    int w, h;
    WNDCLASSW wc;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(g_host.window, &info)) return MR_FAILED;
    g_hwnd = info.info.win.window;
    if (!g_hwnd) return MR_FAILED;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = shell_dlg_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"SkyEngineDlg";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    {
        WNDCLASSW existing;
        if (!GetClassInfoW(wc.hInstance, wc.lpszClassName, &existing)) {
            g_dlg_atom = RegisterClassW(&wc);
        } else {
            g_dlg_atom = 1;
        }
    }
    /* 不要改 SDL HWND 的 GWLP_USERDATA,SDL 自己可能占用该槽。 */

    g_menu_bar = CreateMenu();
    file_menu = CreatePopupMenu();
    settings_menu = CreatePopupMenu();
    g_menu_recent = CreatePopupMenu();
    AppendMenuW(file_menu, MF_STRING, CMD_OPEN, L"选择 MRP 启动...");
    AppendMenuW(file_menu, MF_STRING, CMD_DSM_GM, L"启动 dsm_gm.mrp");
    AppendMenuW(file_menu, MF_POPUP, (UINT_PTR)g_menu_recent, L"最近打开 MRP");
    AppendMenuW(file_menu, MF_STRING, CMD_ADVANCED, L"高级启动...");
    AppendMenuW(file_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file_menu, MF_STRING, CMD_RESTART, L"重启模拟器");
    AppendMenuW(settings_menu, MF_STRING, CMD_PROFILE, L"设置运行模式...");
    AppendMenuW(settings_menu, MF_STRING, CMD_SCREEN, L"设置屏幕分辨率...");
    AppendMenuW(settings_menu, MF_STRING, CMD_MEMORY, L"设置应用可见内存...");
    AppendMenuW(settings_menu, MF_STRING, CMD_DATE, L"设置应用可见设备日期...");
    AppendMenuW(settings_menu, MF_STRING, CMD_WORKDIR, L"设置运行和 MRP 文件系统的工作目录...");
    AppendMenuW(settings_menu, MF_STRING, CMD_DNS, L"设置域名替换规则...");
    AppendMenuW(settings_menu, MF_STRING, CMD_SF2, L"选择 SoundFont (SF2)...");
    AppendMenuW(g_menu_bar, MF_POPUP, (UINT_PTR)file_menu, L"文件");
    AppendMenuW(g_menu_bar, MF_POPUP, (UINT_PTR)settings_menu, L"设置");
    SetMenu(g_hwnd, g_menu_bar);
    g_sdl_wndproc = (WNDPROC)SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)shell_sdl_wndproc);
    win_rebuild_recent();
    /* SetMenu 会吃掉客户区高度;把客户区拉回 --screen 尺寸,避免 PPM 被裁。 */
    w = g_host.args ? g_host.args->screen_width : 240;
    h = g_host.args ? g_host.args->screen_height : 320;
    SDL_SetWindowSize(g_host.window, w, h);
    DrawMenuBar(g_hwnd);
    return MR_SUCCESS;
}
#endif /* _WIN32 */

#if !defined(_WIN32) && !defined(__APPLE__)
static int posix_run(char *const argv[], char *out, size_t out_sz, int *exit_code) {
    int pipefd[2];
    pid_t pid;
    ssize_t n;
    size_t used = 0;
    int st = 0;
    if (pipe(pipefd) != 0) return MR_FAILED;
    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return MR_FAILED;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        dup2(pipefd[1], STDOUT_FILENO);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2) close(devnull);
        }
        close(pipefd[0]);
        close(pipefd[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(pipefd[1]);
    if (out && out_sz) {
        out[0] = '\0';
        while (used + 1 < out_sz &&
               (n = read(pipefd[0], out + used, out_sz - used - 1)) > 0) {
            used += (size_t)n;
            out[used] = '\0';
        }
    } else {
        char sink[256];
        while (read(pipefd[0], sink, sizeof(sink)) > 0) {
        }
    }
    close(pipefd[0]);
    if (waitpid(pid, &st, 0) < 0) return MR_FAILED;
    if (exit_code) *exit_code = WIFEXITED(st) ? WEXITSTATUS(st) : 127;
    if (out) trim_inplace(out);
    return MR_SUCCESS;
}

static int posix_have_tool(const char *name) {
    char *argv[3];
    int code = 127;
    argv[0] = (char *)name;
    argv[1] = "--version";
    argv[2] = NULL;
    if (posix_run(argv, NULL, 0, &code) != MR_SUCCESS) return 0;
    return code != 127;
}

static int posix_pick_mrp(char *out, size_t out_sz) {
    int code = 1;
    if (posix_have_tool("zenity")) {
        char *argv[] = {
            "zenity", "--file-selection", "--title=选择要启动的 MRP 文件",
            "--file-filter=MRP | *.mrp", "--file-filter=All | *", NULL
        };
        if (posix_run(argv, out, out_sz, &code) == MR_SUCCESS && code == 0 && out[0]) {
            return MR_SUCCESS;
        }
        return MR_FAILED;
    }
    if (posix_have_tool("kdialog")) {
        char *argv[] = {
            "kdialog", "--getopenfilename", ".", "*.mrp", NULL
        };
        if (posix_run(argv, out, out_sz, &code) == MR_SUCCESS && code == 0 && out[0]) {
            return MR_SUCCESS;
        }
        return MR_FAILED;
    }
    shell_error("未找到 zenity/kdialog，无法打开文件对话框。");
    return MR_FAILED;
}

static int posix_pick_dir(char *out, size_t out_sz) {
    int code = 1;
    if (posix_have_tool("zenity")) {
        char *argv[] = {
            "zenity", "--file-selection", "--directory",
            "--title=设置运行和 MRP 文件系统的工作目录", NULL
        };
        if (posix_run(argv, out, out_sz, &code) == MR_SUCCESS && code == 0 && out[0]) {
            return MR_SUCCESS;
        }
        return MR_FAILED;
    }
    if (posix_have_tool("kdialog")) {
        char *argv[] = {"kdialog", "--getexistingdirectory", ".", NULL};
        if (posix_run(argv, out, out_sz, &code) == MR_SUCCESS && code == 0 && out[0]) {
            return MR_SUCCESS;
        }
        return MR_FAILED;
    }
    shell_error("未找到 zenity/kdialog，无法选择目录。");
    return MR_FAILED;
}

static int posix_entry(const char *title, const char *text, const char *initial,
                       char *out, size_t out_sz) {
    int code = 1;
    if (posix_have_tool("zenity")) {
        char *argv[] = {
            "zenity", "--entry", "--title", (char *)title, "--text", (char *)text,
            "--entry-text", (char *)(initial ? initial : ""), NULL
        };
        if (posix_run(argv, out, out_sz, &code) == MR_SUCCESS && code == 0) return MR_SUCCESS;
        return MR_FAILED;
    }
    if (posix_have_tool("kdialog")) {
        char *argv[] = {
            "kdialog", "--title", (char *)title, "--inputbox", (char *)text,
            (char *)(initial ? initial : ""), NULL
        };
        if (posix_run(argv, out, out_sz, &code) == MR_SUCCESS && code == 0) return MR_SUCCESS;
        return MR_FAILED;
    }
    shell_error("未找到 zenity/kdialog，无法显示输入框。");
    return MR_FAILED;
}

static int posix_list(const char *title, const char *text, char **items, int count,
                      char *out, size_t out_sz) {
    int code = 1;
    char *argv[64];
    int n = 0;
    int i;
    if (count + 8 >= (int)(sizeof(argv) / sizeof(argv[0]))) return MR_FAILED;
    if (posix_have_tool("zenity")) {
        argv[n++] = "zenity";
        argv[n++] = "--list";
        argv[n++] = "--title";
        argv[n++] = (char *)title;
        argv[n++] = "--text";
        argv[n++] = (char *)text;
        argv[n++] = "--column";
        argv[n++] = "选项";
        argv[n++] = "--hide-header";
        for (i = 0; i < count; i++) argv[n++] = items[i];
        argv[n] = NULL;
        if (posix_run(argv, out, out_sz, &code) == MR_SUCCESS && code == 0 && out[0]) {
            return MR_SUCCESS;
        }
        return MR_FAILED;
    }
    shell_error("未找到 zenity，无法显示列表。");
    return MR_FAILED;
}

static int posix_advanced_dialog(char *mrp, size_t mrp_sz, char *ext, size_t ext_sz,
                                 char *entry, size_t entry_sz) {
    char *ext_items[2];
    char *entry_items[2];
    if (posix_pick_mrp(mrp, mrp_sz) != MR_SUCCESS) return MR_FAILED;
    ext_items[0] = "VMRP_EXT";
    ext_items[1] = "start.mr";
    if (posix_list("EXT_NAME", "EXT_NAME（VMRP_EXT 或 start.mr）", ext_items, 2,
                   ext, ext_sz) != MR_SUCCESS) {
        return MR_FAILED;
    }
    entry_items[0] = "VMRP_ENTRY";
    entry_items[1] = "（空）";
    if (posix_list("ENTRY", "ENTRY（VMRP_ENTRY 或空）", entry_items, 2,
                   entry, entry_sz) != MR_SUCCESS) {
        return MR_FAILED;
    }
    return MR_SUCCESS;
}

static int posix_menu_pick(void) {
    char choice[128];
    char *items[] = {
        "选择 MRP 启动",
        "启动 dsm_gm.mrp",
        "最近打开 MRP",
        "高级启动",
        "重启模拟器",
        "设置运行模式",
        "设置屏幕分辨率",
        "设置应用可见内存",
        "设置应用可见设备日期",
        "设置运行和 MRP 文件系统的工作目录",
        "设置域名替换规则",
        "选择 SoundFont (SF2)"
    };
    if (posix_list("SkyEngine", "文件 / 设置", items, 12, choice, sizeof(choice)) != MR_SUCCESS) {
        return CMD_NONE;
    }
    if (strcmp(choice, items[0]) == 0) return CMD_OPEN;
    if (strcmp(choice, items[1]) == 0) return CMD_DSM_GM;
    if (strcmp(choice, items[2]) == 0) return CMD_RECENT_BASE;
    if (strcmp(choice, items[3]) == 0) return CMD_ADVANCED;
    if (strcmp(choice, items[4]) == 0) return CMD_RESTART;
    if (strcmp(choice, items[5]) == 0) return CMD_PROFILE;
    if (strcmp(choice, items[6]) == 0) return CMD_SCREEN;
    if (strcmp(choice, items[7]) == 0) return CMD_MEMORY;
    if (strcmp(choice, items[8]) == 0) return CMD_DATE;
    if (strcmp(choice, items[9]) == 0) return CMD_WORKDIR;
    if (strcmp(choice, items[10]) == 0) return CMD_DNS;
    if (strcmp(choice, items[11]) == 0) return CMD_SF2;
    return CMD_NONE;
}

static int posix_dns_pair(char *from, size_t from_n, char *to, size_t to_n) {
    if (posix_entry("新增域名替换", "被替换值", "", from, from_n) != MR_SUCCESS) {
        return MR_FAILED;
    }
    if (posix_entry("新增域名替换", "替换值", "", to, to_n) != MR_SUCCESS) {
        return MR_FAILED;
    }
    trim_inplace(from);
    trim_inplace(to);
    return (from[0] && to[0]) ? MR_SUCCESS : MR_FAILED;
}

static int posix_dns_editor(char *map, size_t map_n) {
    DesktopDnsRule rules[DESKTOP_DNS_RULE_MAX];
    int count = desktop_dns_parse(map, rules, DESKTOP_DNS_RULE_MAX);
    for (;;) {
        char *actions[] = {"新增", "删除", "完成"};
        char choice[32];
        if (posix_list("域名替换规则", count ? "现有替换见下一项操作" : "当前没有替换规则",
                       actions, 3, choice, sizeof(choice)) != MR_SUCCESS) {
            return MR_FAILED;
        }
        if (strcmp(choice, "完成") == 0) break;
        if (strcmp(choice, "新增") == 0) {
            char from[DESKTOP_DNS_HOST_MAX];
            char to[DESKTOP_DNS_HOST_MAX];
            if (count >= DESKTOP_DNS_RULE_MAX) {
                shell_error("替换规则已满（最多 32 条）。");
                continue;
            }
            if (posix_dns_pair(from, sizeof(from), to, sizeof(to)) == MR_SUCCESS) {
                copy_str(rules[count].from, sizeof(rules[0].from), from);
                copy_str(rules[count].to, sizeof(rules[0].to), to);
                count++;
            }
            continue;
        }
        if (strcmp(choice, "删除") == 0) {
            char *items[DESKTOP_DNS_RULE_MAX];
            char lines[DESKTOP_DNS_RULE_MAX][DESKTOP_DNS_HOST_MAX * 2 + 8];
            char selected[DESKTOP_DNS_HOST_MAX * 2 + 8];
            int i;
            if (count <= 0) {
                shell_error("没有可删除的规则。");
                continue;
            }
            for (i = 0; i < count; i++) {
                desktop_dns_format(&rules[i], lines[i], sizeof(lines[i]));
                items[i] = lines[i];
            }
            if (posix_list("删除域名替换", "选择要删除的规则", items, count,
                           selected, sizeof(selected)) != MR_SUCCESS) {
                continue;
            }
            for (i = 0; i < count; i++) {
                if (strcmp(selected, lines[i]) == 0) {
                    if (i + 1 < count) {
                        memmove(&rules[i], &rules[i + 1],
                                (size_t)(count - i - 1) * sizeof(rules[0]));
                    }
                    count--;
                    break;
                }
            }
        }
    }
    return desktop_dns_serialize(rules, count, map, map_n);
}

static int posix_pick_sf2(char *out, size_t out_sz) {
    int code = 1;
    if (posix_have_tool("zenity")) {
        char *argv[] = {
            "zenity", "--file-selection", "--title=选择 SoundFont (SF2)",
            "--file-filter=SF2 | *.sf2", "--file-filter=All | *", NULL
        };
        if (posix_run(argv, out, out_sz, &code) == MR_SUCCESS && code == 0 && out[0]) {
            return MR_SUCCESS;
        }
        return MR_FAILED;
    }
    if (posix_have_tool("kdialog")) {
        char *argv[] = {
            "kdialog", "--getopenfilename", ".", "*.sf2", NULL
        };
        if (posix_run(argv, out, out_sz, &code) == MR_SUCCESS && code == 0 && out[0]) {
            return MR_SUCCESS;
        }
        return MR_FAILED;
    }
    shell_error("未找到 zenity/kdialog，无法打开文件对话框。");
    return MR_FAILED;
}
#endif /* !WIN32 && !APPLE */

static int pick_mrp(char *out, size_t out_sz) {
#ifdef _WIN32
    return win_pick_mrp(out, out_sz);
#elif defined(__APPLE__)
    return desktop_shell_cocoa_pick_mrp(out, out_sz);
#else
#ifdef SKYENGINE_HAS_GTK
    if (desktop_shell_gtk_pick_mrp(out, out_sz) == 0) return MR_SUCCESS;
#endif
    return posix_pick_mrp(out, out_sz);
#endif
}

static int pick_dir(char *out, size_t out_sz) {
#ifdef _WIN32
    return win_pick_dir(out, out_sz);
#elif defined(__APPLE__)
    return desktop_shell_cocoa_pick_dir(out, out_sz);
#else
#ifdef SKYENGINE_HAS_GTK
    if (desktop_shell_gtk_pick_dir(out, out_sz) == 0) return MR_SUCCESS;
#endif
    return posix_pick_dir(out, out_sz);
#endif
}

static int plat_advanced(char *mrp, size_t mrp_n, char *ext, size_t ext_n,
                         char *entry, size_t entry_n) {
#ifdef _WIN32
    return win_advanced_dialog(mrp, mrp_n, ext, ext_n, entry, entry_n);
#elif defined(__APPLE__)
    return desktop_shell_cocoa_advanced(mrp, mrp_n, ext, ext_n, entry, entry_n);
#else
#ifdef SKYENGINE_HAS_GTK
    if (desktop_shell_gtk_advanced(mrp, mrp_n, ext, ext_n, entry, entry_n) == 0) {
        return MR_SUCCESS;
    }
#endif
    return posix_advanced_dialog(mrp, mrp_n, ext, ext_n, entry, entry_n);
#endif
}

#ifndef _WIN32
static int plat_prompt(const char *title, const char *label, const char *initial,
                       char *out, size_t n, int multiline, const char **choices,
                       int nchoices, int list_only) {
#if defined(__APPLE__)
    return desktop_shell_cocoa_prompt(title, label, initial, out, n, multiline,
                                      choices, nchoices, list_only);
#else
#ifdef SKYENGINE_HAS_GTK
    if (desktop_shell_gtk_prompt(title, label, initial, out, n, multiline,
                                 choices, nchoices, list_only) == 0) {
        return MR_SUCCESS;
    }
#endif
    (void)multiline;
    (void)list_only;
    if (choices && nchoices > 0) {
        char *items[16];
        int i;
        if (nchoices > 16) nchoices = 16;
        for (i = 0; i < nchoices; i++) items[i] = (char *)choices[i];
        return posix_list(title, label, items, nchoices, out, n);
    }
    return posix_entry(title, label, initial, out, n);
#endif
}
#endif

static int plat_dns_editor(char *map, size_t map_n) {
#ifdef _WIN32
    return win_dns_editor(map, map_n);
#elif defined(__APPLE__)
    return desktop_shell_cocoa_dns_editor(map, map_n);
#else
#ifdef SKYENGINE_HAS_GTK
    if (desktop_shell_gtk_dns_editor(map, map_n) == 0) return MR_SUCCESS;
#endif
    return posix_dns_editor(map, map_n);
#endif
}

static int pick_sf2(char *out, size_t out_sz) {
#ifdef _WIN32
    return win_pick_sf2(out, out_sz);
#elif defined(__APPLE__)
    return desktop_shell_cocoa_pick_sf2(out, out_sz);
#else
#ifdef SKYENGINE_HAS_GTK
    if (desktop_shell_gtk_pick_sf2(out, out_sz) == 0) return MR_SUCCESS;
#endif
    return posix_pick_sf2(out, out_sz);
#endif
}

static int run_open(void) {
    char path[PATH_MAX];
    char resolved[PATH_MAX];
    if (pick_mrp(path, sizeof(path)) != MR_SUCCESS) return MR_FAILED;
    if (skyengine_args_resolve_mrp_path(path, resolved, sizeof(resolved)) != MR_SUCCESS) {
        shell_error("无法打开所选 MRP 文件。路径必须存在且短于 128 字节。");
        return MR_FAILED;
    }
    return launch_mrp(resolved, "start.mr", "");
}

static int run_dsm_gm(void) {
    char path[PATH_MAX];
    if (find_dsm_gm(path, sizeof(path)) != MR_SUCCESS) {
        shell_error("当前工作目录下找不到 dsm_gm.mrp 或 mythroad/dsm_gm.mrp。");
        return MR_FAILED;
    }
    return launch_mrp(path, "start.mr", "");
}

static int run_recent(int idx) {
    char resolved[PATH_MAX];
    if (idx < 0 || idx >= g_recent_count) return MR_FAILED;
    if (skyengine_args_resolve_mrp_path(g_recent[idx], resolved, sizeof(resolved)) != MR_SUCCESS) {
        /* 相对路径(如 mythroad/dsm_gm.mrp)在已 chdir 的工作目录下仍可用。 */
        if (my_info(g_recent[idx]) == MR_IS_FILE) {
            return launch_mrp(g_recent[idx], "start.mr", "");
        }
        shell_error("最近打开的 MRP 已不存在。");
        return MR_FAILED;
    }
    return launch_mrp(resolved, "start.mr", "");
}

static int run_recent_picker(void) {
#if !defined(_WIN32) && !defined(__APPLE__)
    char choice[PATH_MAX];
    char *items[RECENT_MAX];
    int i;
    if (g_recent_count <= 0) {
        shell_error("没有最近打开的 MRP。");
        return MR_FAILED;
    }
    for (i = 0; i < g_recent_count; i++) items[i] = g_recent[i];
    if (posix_list("最近打开 MRP", "选择一个 MRP", items, g_recent_count, choice,
                   sizeof(choice)) != MR_SUCCESS) {
        return MR_FAILED;
    }
    for (i = 0; i < g_recent_count; i++) {
        if (strcmp(choice, g_recent[i]) == 0) return run_recent(i);
    }
#endif
    (void)path_basename;
    return MR_FAILED;
}

static int run_advanced(void) {
    char mrp[PATH_MAX];
    char ext_choice[256];
    char entry_choice[256];
    char resolved[PATH_MAX];
    char ext[256];
    char entry[256];
    mrp[0] = '\0';
    ext_choice[0] = '\0';
    entry_choice[0] = '\0';
    if (plat_advanced(mrp, sizeof(mrp), ext_choice, sizeof(ext_choice),
                      entry_choice, sizeof(entry_choice)) != MR_SUCCESS) {
        return MR_FAILED;
    }
    if (!mrp[0]) {
        shell_error("请选择要启动的 MRP 文件。");
        return MR_FAILED;
    }
    if (skyengine_args_resolve_mrp_path(mrp, resolved, sizeof(resolved)) != MR_SUCCESS) {
        shell_error("无法打开所选 MRP 文件。路径必须存在且短于 128 字节。");
        return MR_FAILED;
    }
    resolve_ext_choice(ext_choice, ext, sizeof(ext));
    resolve_entry_choice(entry_choice, entry, sizeof(entry));
    return launch_mrp(resolved, ext, entry);
}

static int run_restart(void) {
    if (!g_host.args || !g_host.args->mrp_path[0]) {
        shell_error("当前没有已加载的 MRP。");
        return MR_FAILED;
    }
    if (g_host.stop_timer) g_host.stop_timer();
    if (!g_host.restart_engine || g_host.restart_engine() != 0) {
        shell_error("重启失败，请查看 log.txt");
        return MR_FAILED;
    }
    desktop_shell_refresh();
    return MR_SUCCESS;
}

static int run_profile(void) {
    char cur[32];
    char next[32];
    int profile = SKYENGINE_PROFILE_SPEED;
    snprintf(cur, sizeof(cur), "%s",
             g_host.args->compat_priority ? "兼容性优先" : "速度优先");
#ifdef _WIN32
    {
        const wchar_t *choices[] = {L"速度优先", L"兼容性优先"};
        if (win_prompt(L"设置运行模式",
                       L"速度优先(默认,接近旧版稀钩子)或兼容性优先(宽 R9/GOT/屏写钩)。修改后重启生效。",
                       cur, next, sizeof(next), 0, 0, choices, 2, 1) != MR_SUCCESS) {
            return MR_FAILED;
        }
    }
#else
    {
        const char *items[] = {"速度优先", "兼容性优先"};
        if (plat_prompt("设置运行模式",
                        "速度优先(默认,接近旧版稀钩子)或兼容性优先(宽 R9/GOT/屏写钩)。修改后重启生效。",
                        cur, next, sizeof(next), 0, items, 2, 1) != MR_SUCCESS) {
            return MR_FAILED;
        }
    }
#endif
    if (skyengine_args_parse_profile(next, &profile) != MR_SUCCESS) {
        shell_error("无效运行模式，只接受速度优先或兼容性优先。");
        return MR_FAILED;
    }
    g_host.args->compat_priority = profile;
    return apply_settings_and_restart();
}

static int run_screen(void) {
    char cur[32];
    char next[64];
    int w = 0, h = 0;
    snprintf(cur, sizeof(cur), "%dx%d",
             g_host.args->screen_width, g_host.args->screen_height);
#ifdef _WIN32
    if (win_prompt(L"设置屏幕分辨率", L"屏幕分辨率，默认 240x320（格式 WxH）",
                   cur, next, sizeof(next), 0, 0, NULL, 0, 0) != MR_SUCCESS) {
        return MR_FAILED;
    }
#else
    if (plat_prompt("设置屏幕分辨率", "屏幕分辨率，默认 240x320（格式 WxH）",
                    cur, next, sizeof(next), 0, NULL, 0, 0) != MR_SUCCESS) {
        return MR_FAILED;
    }
#endif
    if (skyengine_args_parse_screen(next, &w, &h) != MR_SUCCESS) {
        shell_error("无效分辨率，请使用 WxH，例如 240x320。");
        return MR_FAILED;
    }
    g_host.args->screen_width = w;
    g_host.args->screen_height = h;
    return apply_settings_and_restart();
}

static int run_memory(void) {
    char cur[16];
    char next[16];
    int mb = 0;
    snprintf(cur, sizeof(cur), "%dM",
             g_host.args->memory_mb > 0 ? g_host.args->memory_mb : DEFAULT_MEMORY_MB);
#ifdef _WIN32
    {
        const wchar_t *choices[] = {L"1M", L"2M", L"4M", L"6M", L"8M", L"16M"};
        if (win_prompt(L"设置应用可见内存", L"只接受 1M、2M、4M、6M、8M、16M",
                       cur, next, sizeof(next), 0, 0, choices, 6, 1) != MR_SUCCESS) {
            return MR_FAILED;
        }
    }
#else
    {
        const char *items[] = {"1M", "2M", "4M", "6M", "8M", "16M"};
        if (plat_prompt("设置应用可见内存", "只接受 1M、2M、4M、6M、8M、16M",
                        cur, next, sizeof(next), 0, items, 6, 1) != MR_SUCCESS) {
            return MR_FAILED;
        }
    }
#endif
    if (skyengine_args_parse_memory(next, &mb) != MR_SUCCESS) {
        shell_error("无效内存档位，只接受 1M、2M、4M、6M、8M、16M。");
        return MR_FAILED;
    }
    g_host.args->memory_mb = mb;
    return apply_settings_and_restart();
}

static int run_date(void) {
    char cur[32];
    char next[32];
    int y = 0, m = 0, d = 0;
    skyengine_args_format_device_date(g_host.args, cur, sizeof(cur));
#ifdef _WIN32
    if (win_prompt(L"设置应用可见设备日期", L"接受 YYYY-MM-DD 或 host；默认 2011-01-01",
                   cur, next, sizeof(next), 0, 0, NULL, 0, 0) != MR_SUCCESS) {
        return MR_FAILED;
    }
#else
    if (plat_prompt("设置应用可见设备日期", "接受 YYYY-MM-DD 或 host；默认 2011-01-01",
                    cur, next, sizeof(next), 0, NULL, 0, 0) != MR_SUCCESS) {
        return MR_FAILED;
    }
#endif
    if (skyengine_args_parse_device_date(next, &y, &m, &d) != MR_SUCCESS) {
        shell_error("无效日期，请使用 YYYY-MM-DD 或 host。");
        return MR_FAILED;
    }
    g_host.args->device_year = y;
    g_host.args->device_month = m;
    g_host.args->device_day = d;
    return apply_settings_and_restart();
}

static int run_workdir(void) {
    char next[PATH_MAX];
    char resolved[PATH_MAX];
#ifdef _WIN32
    if (win_prompt(L"设置工作目录", L"设置运行和 MRP 文件系统的工作目录",
                   g_host.args->work_dir, next, sizeof(next), 0, 1, NULL, 0, 0) != MR_SUCCESS) {
        return MR_FAILED;
    }
#else
    if (pick_dir(next, sizeof(next)) != MR_SUCCESS) return MR_FAILED;
#endif
    if (skyengine_args_resolve_dir(next, resolved, sizeof(resolved)) != MR_SUCCESS) {
        shell_error("无效工作目录。");
        return MR_FAILED;
    }
    copy_str(g_host.args->work_dir, sizeof(g_host.args->work_dir), resolved);
    return apply_settings_and_restart();
}

static int run_dns(void) {
    char next[SKYENGINE_DNS_MAP_LIMIT];
    copy_str(next, sizeof(next), g_host.args->dns_map);
    if (plat_dns_editor(next, sizeof(next)) != MR_SUCCESS) return MR_FAILED;
    if (my_configureDnsMap(next) != MR_SUCCESS) {
        shell_error("域名替换规则无效。");
        return MR_FAILED;
    }
    copy_str(g_host.args->dns_map, sizeof(g_host.args->dns_map), next);
    save_ui_config();
    /* 映射表运行时可替换,不必重启引擎。 */
    return MR_SUCCESS;
}

static int run_sf2(void) {
    char path[PATH_MAX];
    if (pick_sf2(path, sizeof(path)) != MR_SUCCESS) return MR_FAILED;
    copy_str(g_host.args->sf2_path, sizeof(g_host.args->sf2_path), path);
    save_ui_config();
    native_audio_set_sf2(path);
    return MR_SUCCESS;
}

static void run_command(int cmd, int recent_idx) {
    switch (cmd) {
        case CMD_OPEN:
            run_open();
            break;
        case CMD_DSM_GM:
            run_dsm_gm();
            break;
        case CMD_ADVANCED:
            run_advanced();
            break;
        case CMD_RESTART:
            run_restart();
            break;
        case CMD_PROFILE:
            run_profile();
            break;
        case CMD_SCREEN:
            run_screen();
            break;
        case CMD_MEMORY:
            run_memory();
            break;
        case CMD_DATE:
            run_date();
            break;
        case CMD_WORKDIR:
            run_workdir();
            break;
        case CMD_DNS:
            run_dns();
            break;
        case CMD_SF2:
            run_sf2();
            break;
        default:
            if (cmd == CMD_RECENT_BASE && recent_idx < 0) {
                run_recent_picker();
            } else if (cmd >= CMD_RECENT_BASE && cmd < CMD_RECENT_BASE + RECENT_MAX) {
                run_recent(cmd - CMD_RECENT_BASE);
            }
            break;
    }
}

int desktop_shell_enabled(void) {
#if defined(__ANDROID__) || defined(__EMSCRIPTEN__)
    return 0;
#else
    const char *sock;
    const char *driver;
    sock = getenv("SKYENGINE_E2E_SOCKET");
    if (sock && *sock) return 0;
    driver = SDL_GetCurrentVideoDriver();
    if (driver && (strcmp(driver, "dummy") == 0 || strcmp(driver, "offscreen") == 0)) {
        return 0;
    }
    return 1;
#endif
}

void desktop_shell_init(const DesktopShellHost *host) {
    if (!host || !host->window || !host->args) return;
    g_host = *host;
    g_enabled = desktop_shell_enabled();
    g_pending_cmd = CMD_NONE;
    g_pending_recent = -1;
    g_recent_count = 0;
    copy_str(g_last_ext, sizeof(g_last_ext),
             host->args->ext_name[0] ? host->args->ext_name : "start.mr");
    copy_str(g_last_entry, sizeof(g_last_entry), host->args->entry);
    if (!g_enabled) return;
    load_ui_config(host->args);
    g_shell_event = SDL_RegisterEvents(1);
    g_native_menubar = 0;
#ifdef _WIN32
    {
        HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        g_com_inited = (hr == S_OK || hr == S_FALSE);
    }
    if (win_create_menu() != MR_SUCCESS) {
        g_enabled = 0;
        return;
    }
    g_native_menubar = 1;
#elif defined(__APPLE__)
    if (desktop_shell_cocoa_init() == 0) g_native_menubar = 1;
#elif defined(SKYENGINE_HAS_GTK)
    if (desktop_shell_gtk_init(host->window, host->args->screen_width,
                               host->args->screen_height) == 0) {
        g_native_menubar = desktop_shell_gtk_has_menubar();
    }
#else
    fprintf(stderr,
            "[desktop_shell] 本二进制未链接 GTK3。安装 libgtk-3-dev 后必须重新运行 cmake 再编译,只 apt 不会改已有 skyengine。\n");
    fprintf(stdout,
            "[desktop_shell] 未链接 GTK3,使用 F10/右键菜单。重新 cmake && 编译后才会出现原生菜单栏。\n");
    fflush(stdout);
#endif
    if (!g_native_menubar) {
        SDL_SetWindowTitle(host->window, "SkyEngine  [F10 或右键打开文件/设置]");
    }
    desktop_shell_refresh();
}

void desktop_shell_shutdown(void) {
    if (!g_enabled) return;
    save_ui_config();
#ifdef _WIN32
    if (g_hwnd && g_sdl_wndproc) {
        SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)g_sdl_wndproc);
        g_sdl_wndproc = NULL;
    }
    if (g_hwnd && g_menu_bar) {
        SetMenu(g_hwnd, NULL);
        DestroyMenu(g_menu_bar);
    }
    g_menu_bar = NULL;
    g_menu_recent = NULL;
    g_hwnd = NULL;
    if (g_com_inited) {
        CoUninitialize();
        g_com_inited = 0;
    }
#elif defined(__APPLE__)
    desktop_shell_cocoa_shutdown();
#elif defined(SKYENGINE_HAS_GTK)
    desktop_shell_gtk_shutdown();
#endif
    g_native_menubar = 0;
    g_enabled = 0;
}

int desktop_shell_handle_event(const union SDL_Event *ev) {
    if (!g_enabled || !ev) return 0;
    if (g_shell_event != 0 && g_shell_event != (Uint32)-1 && ev->type == g_shell_event) {
        int cmd = ev->user.code;
        if (cmd == 0) return 1;
        g_pending_cmd = cmd;
        if (cmd >= CMD_RECENT_BASE && cmd < CMD_RECENT_BASE + RECENT_MAX) {
            g_pending_recent = cmd - CMD_RECENT_BASE;
        }
        return 1;
    }
#ifdef SKYENGINE_HAS_GTK
    desktop_shell_gtk_handle_window_event(ev);
#endif
#if !defined(_WIN32) && !defined(__APPLE__)
    if (!g_native_menubar) {
        if (ev->type == SDL_KEYDOWN && ev->key.keysym.sym == SDLK_F10 && !ev->key.repeat) {
            g_pending_cmd = posix_menu_pick();
            return 1;
        }
        if (ev->type == SDL_KEYUP && ev->key.keysym.sym == SDLK_F10) {
            return 1;
        }
        if (ev->type == SDL_MOUSEBUTTONDOWN && ev->button.button == SDL_BUTTON_RIGHT) {
            g_pending_cmd = posix_menu_pick();
            return 1;
        }
        if (ev->type == SDL_MOUSEBUTTONUP && ev->button.button == SDL_BUTTON_RIGHT) {
            return 1;
        }
    }
#endif
    return 0;
}

void desktop_shell_pump(void) {
    int cmd;
    int recent;
    if (!g_enabled || g_pending_cmd == CMD_NONE) return;
    cmd = g_pending_cmd;
    recent = g_pending_recent;
    g_pending_cmd = CMD_NONE;
    g_pending_recent = -1;
    run_command(cmd, recent);
}

void desktop_shell_refresh(void) {
    char title[512];
    const char *name;
    if (!g_enabled || !g_host.window || !g_host.args) return;
    name = path_basename(g_host.args->mrp_path);
    if (name && *name) {
        snprintf(title, sizeof(title), "SkyEngine - %s", name);
    } else {
        snprintf(title, sizeof(title), "SkyEngine");
    }
#ifdef _WIN32
    {
        wchar_t *wide = utf8_to_wide(title);
        if (wide && g_hwnd) {
            SetWindowTextW(g_hwnd, wide);
            free(wide);
        } else {
            SDL_SetWindowTitle(g_host.window, title);
        }
    }
    win_rebuild_recent();
#elif defined(__APPLE__)
    desktop_shell_cocoa_set_title(title);
    desktop_shell_cocoa_refresh_recents();
#elif defined(SKYENGINE_HAS_GTK)
    desktop_shell_gtk_set_title(title);
    desktop_shell_gtk_refresh_recents();
    desktop_shell_gtk_sync_window();
#else
    snprintf(title, sizeof(title), "SkyEngine - %s  [F10 或右键打开文件/设置]",
             name && *name ? name : "未载入");
    SDL_SetWindowTitle(g_host.window, title);
#endif
}

int desktop_shell_needs_idle(void) {
#ifdef SKYENGINE_HAS_GTK
    return desktop_shell_gtk_needs_idle();
#else
    return 0;
#endif
}

void desktop_shell_idle(void) {
#ifdef SKYENGINE_HAS_GTK
    desktop_shell_gtk_idle();
#endif
}
