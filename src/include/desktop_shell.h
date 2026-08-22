#ifndef SKYENGINE_DESKTOP_SHELL_H
#define SKYENGINE_DESKTOP_SHELL_H

#include "skyengine_args.h"

struct SDL_Window;
union SDL_Event;

/*
 * 桌面宿主菜单(文件/设置),外观对齐 KEmulator 的窗口菜单栏。
 *
 * 必须画在 SDL 客户区之外:e2e PPM 截的是 SDL_GetWindowSurface(),若把
 * 菜单画进 240x320 画面会污染像素断言。因此 Windows 用 HMENU,Linux 用
 * F10/右键弹出系统对话框,两者都不改 framebuffer。
 *
 * Android / Emscripten / E2E socket / dummy 视频驱动一律不启用。
 */

typedef struct DesktopShellHost {
    struct SDL_Window *window;
    SkyEngineArgs *args;
    void (*stop_timer)(void);
    /* 使用 host->args 停引擎再启动;成功返回 0。 */
    int (*restart_engine)(void);
} DesktopShellHost;

int desktop_shell_enabled(void);
void desktop_shell_init(const DesktopShellHost *host);
void desktop_shell_shutdown(void);
/* 返回 1 表示事件已由宿主菜单消费,不要再交给 MRP。 */
int desktop_shell_handle_event(const union SDL_Event *ev);
/* 处理排队的菜单命令。必须在当前 SDL 事件处理完之后调用,避免在
 * WM_COMMAND 里直接弹模态框重入 SDL 窗口过程。 */
void desktop_shell_pump(void);
void desktop_shell_refresh(void);

#endif
