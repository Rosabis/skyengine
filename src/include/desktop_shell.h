#ifndef SKYENGINE_DESKTOP_SHELL_H
#define SKYENGINE_DESKTOP_SHELL_H

#include "skyengine_args.h"

struct SDL_Window;
union SDL_Event;

/*
 * 桌面宿主菜单(文件/设置),外观对齐 KEmulator 的窗口菜单栏。
 *
 * 必须画在 SDL 客户区之外:e2e PPM 截的是 SDL_GetWindowSurface(),若把
 * 菜单画进 240x320 画面会污染像素断言。
 *
 * Windows:SetMenu 挂在 HWND 非客户区。
 * macOS:NSMenu 挂在屏幕顶部菜单栏(系统标准位置)。
 * Linux:X11/Wayland 没有窗口菜单 API;有 GTK3 时用 GtkMenuBar
 * (X11 为独立菜单窗口 + 无边框 SDL 跟随,不用 XReparent)。否则 F10/右键。
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
/* GTK 菜单需要与 SDL 分时抽事件。needs_idle 为真时主循环改用短超时 WaitEvent。 */
int desktop_shell_needs_idle(void);
void desktop_shell_idle(void);

/* 非阻塞原生输入框,替代 SDL 自绘 8x8 点阵窗口。成功返回 0。
 * E2E/dummy 不打开窗口,由 Ctrl+V/Z 路径提交。 */
int desktop_shell_edit_open(const char *title, const char *text, int type, int max_size);
void desktop_shell_edit_close(void);
/* 1=用户已确定/取消,*ok 非 0 为确定;0=仍在编辑或未打开原生框。 */
int desktop_shell_edit_poll(int *ok, char *out, size_t out_n);

#endif
