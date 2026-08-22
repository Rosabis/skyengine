#ifndef SKYENGINE_DESKTOP_SHELL_INTERNAL_H
#define SKYENGINE_DESKTOP_SHELL_INTERNAL_H

#include <stddef.h>

/* 桌面菜单内部接口:Windows / Cocoa / GTK 共用同一组命令码,经
 * desktop_shell_queue_cmd 投到 SDL 主循环再执行,避免在系统菜单回调里
 * 直接 stopEngine/startEngine。 */

#define DESKTOP_SHELL_RECENT_MAX 8
#define CMD_NONE 0
#define CMD_OPEN 1001
#define CMD_DSM_GM 1002
#define CMD_ADVANCED 1003
#define CMD_RESTART 1004
#define CMD_RECENT_BASE 1100
#define CMD_SCREEN 2001
#define CMD_MEMORY 2002
#define CMD_DATE 2003
#define CMD_WORKDIR 2004
#define CMD_DNS 2005

#ifdef __cplusplus
extern "C" {
#endif

void desktop_shell_queue_cmd(int cmd);
int desktop_shell_recent_count(void);
const char *desktop_shell_recent_at(int index);
const char *desktop_shell_last_ext(void);
const char *desktop_shell_last_entry(void);
const char *desktop_shell_current_mrp(void);

#if defined(__APPLE__)
int desktop_shell_cocoa_init(void);
void desktop_shell_cocoa_shutdown(void);
void desktop_shell_cocoa_set_title(const char *title);
void desktop_shell_cocoa_refresh_recents(void);
int desktop_shell_cocoa_pick_mrp(char *out, size_t n);
int desktop_shell_cocoa_pick_dir(char *out, size_t n);
int desktop_shell_cocoa_advanced(char *mrp, size_t mrp_n, char *ext, size_t ext_n,
                                 char *entry, size_t entry_n);
int desktop_shell_cocoa_prompt(const char *title, const char *label,
                               const char *initial, char *out, size_t n,
                               int multiline, const char **choices, int nchoices,
                               int list_only);
#endif

#ifdef SKYENGINE_HAS_GTK
struct SDL_Window;
union SDL_Event;
int desktop_shell_gtk_init(struct SDL_Window *window, int w, int h);
void desktop_shell_gtk_shutdown(void);
void desktop_shell_gtk_idle(void);
int desktop_shell_gtk_needs_idle(void);
int desktop_shell_gtk_has_menubar(void);
void desktop_shell_gtk_set_title(const char *title);
void desktop_shell_gtk_refresh_recents(void);
void desktop_shell_gtk_sync_window(void);
int desktop_shell_gtk_handle_window_event(const union SDL_Event *ev);
int desktop_shell_gtk_pick_mrp(char *out, size_t n);
int desktop_shell_gtk_pick_dir(char *out, size_t n);
int desktop_shell_gtk_advanced(char *mrp, size_t mrp_n, char *ext, size_t ext_n,
                               char *entry, size_t entry_n);
int desktop_shell_gtk_prompt(const char *title, const char *label,
                             const char *initial, char *out, size_t n,
                             int multiline, const char **choices, int nchoices,
                             int list_only);
#endif

#ifdef __cplusplus
}
#endif

#endif
