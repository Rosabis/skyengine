#include <stdint.h>
#include <stdbool.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _MSC_VER
#include "./include/compat_msvc.h"
#include "./include/dirent_win.h"
#else
#include <dirent.h>
#include <unistd.h>
#endif

#include "./include/bridge.h"
#include "./include/e2e_control.h"
#include "./include/file_lib.h"
#include "./include/keyboard_input.h"
#include "./include/native_text_widget.h"
#include "./include/skyengine.h"
#include "./include/memory.h"

#ifdef _MSC_VER
#include <SDL.h>
#elif defined(_WIN32)
// #ifdef __x86_64__
// #include "./windows/SDL2-2.0.10/x86_64-w64-mingw32/include/SDL2/SDL.h"
// #elif __i386__
#include "./windows/SDL2-2.0.10/i686-w64-mingw32/include/SDL2/SDL.h"
// #endif
#elif defined(__ANDROID__)
/* Android 的 SDL 由 android/third_party/SDL2 子工程编译，头文件以 <SDL.h>
 * 暴露（非桌面<pkg>的 <SDL2/SDL.h>）；该分支仅影响 Android 构建。 */
#include <SDL.h>
#else
#include <SDL2/SDL.h>
#endif

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#if defined(__ANDROID__)
#include "android_host.h"
#endif

#define MOUSE_DOWN 2
#define MOUSE_UP 3
#define MOUSE_MOVE 12

// http://wiki.libsdl.org/Tutorials
// http://lazyfoo.net/tutorials/SDL/index.php

static SDL_TimerID timeId = 0;
static SDL_Window *window;
static bool isMouseDown = false;

/* PPM 截屏：收到 SIGUSR1 时将当前 SDL surface 转储为 PPM 文件，
 * 用于在无显示器环境下验证画面是否正常渲染。 */
static SDL_atomic_t guiDrawBitmapCount;
/*
 * E2E 输入同步把每次 timerStart 作为独立 generation。pending 标识仍可完成的
 * 那一代 timer，dispatched 只在主线程完整执行 timer() 后发布；三者共同避免
 * 把按键前已经排队的 timer 事件误认为按键后的 guest 调度边界。
 */
static SDL_atomic_t timerArmGeneration;
static SDL_atomic_t timerDispatchedGeneration;
static SDL_atomic_t timerPendingGeneration;
static SDL_atomic_t timerDispatchInProgress;
static SDL_atomic_t runtimeExited;

static const char *screen_dump_path(void) {
    const char *path = getenv("SKYENGINE_PPM_PATH");
    return (path && *path) ? path : "/tmp/skyengine_screen.ppm";
}

static int dump_screen_ppm(const char *path) {
    if (!window) return -1;
    SDL_Surface *surface = SDL_GetWindowSurface(window);
    if (!surface) return -1;
    /* E2E 截图通常位于用户 TEMP；主机接口在 Windows 用 _wfopen 保留
     * Node 传入的 UTF-8 路径，Linux 行为仍等价于 fopen。 */
    FILE *fp = skyengine_host_fopen(path, "wb");
    if (!fp) return -1;
    fprintf(fp, "P6\n%d %d\n255\n", surface->w, surface->h);
    if (SDL_MUSTLOCK(surface) && SDL_LockSurface(surface) != 0) {
        fclose(fp);
        return -1;
    }
    for (int y = 0; y < surface->h; y++) {
        for (int x = 0; x < surface->w; x++) {
            Uint32 px = *((Uint32 *)(((Uint8 *)surface->pixels) + surface->pitch * y) + x);
            Uint8 r, g, b;
            SDL_GetRGB(px, surface->format, &r, &g, &b);
            fputc(r, fp); fputc(g, fp); fputc(b, fp);
        }
    }
    if (SDL_MUSTLOCK(surface)) SDL_UnlockSurface(surface);
    int ret = ferror(fp) ? -1 : 0;
    fclose(fp);
    // printf("[PPM] dumped to %s (%dx%d)\n", path, surface->w, surface->h);
    return ret;
}

#define E2E_DRAW_FRAME_RING_CAP 64

typedef struct {
    int draw_count;
    int width;
    int height;
    size_t rgb_len;
    uint8_t *rgb;
} E2eDrawFrame;

static E2eDrawFrame e2eDrawFrames[E2E_DRAW_FRAME_RING_CAP];
static SDL_mutex *e2eDrawFrameMutex = NULL;

static int e2e_draw_frame_capture_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *socket = getenv("SKYENGINE_E2E_SOCKET");
        cached = (socket && *socket) ? 1 : 0;
    }
    return cached;
}

static int e2e_ensure_draw_frame_mutex(void) {
    if (e2eDrawFrameMutex) return 1;
    e2eDrawFrameMutex = SDL_CreateMutex();
    return e2eDrawFrameMutex != NULL;
}

static int write_ppm_rgb(const char *path, int width, int height,
                         const uint8_t *rgb, size_t rgb_len) {
    if (!path || !rgb || width <= 0 || height <= 0) return -1;
    size_t expected = (size_t)width * (size_t)height * 3u;
    if (rgb_len < expected) return -1;
    FILE *fp = skyengine_host_fopen(path, "wb");
    if (!fp) return -1;
    fprintf(fp, "P6\n%d %d\n255\n", width, height);
    fwrite(rgb, 1, expected, fp);
    int ret = ferror(fp) ? -1 : 0;
    fclose(fp);
    return ret;
}

static void e2e_record_draw_frame(int draw_count, SDL_Surface *surface) {
    if (!e2e_draw_frame_capture_enabled() || draw_count <= 0 || !surface)
        return;
    if (!e2e_ensure_draw_frame_mutex()) return;

    int width = surface->w;
    int height = surface->h;
    if (width <= 0 || height <= 0) return;
    size_t rgb_len = (size_t)width * (size_t)height * 3u;
    uint8_t *rgb = (uint8_t *)malloc(rgb_len);
    if (!rgb) return;

    if (SDL_MUSTLOCK(surface) && SDL_LockSurface(surface) != 0) {
        free(rgb);
        return;
    }
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            Uint32 px = *((Uint32 *)(((Uint8 *)surface->pixels) +
                                      surface->pitch * y) + x);
            Uint8 r, g, b;
            size_t out = ((size_t)y * (size_t)width + (size_t)x) * 3u;
            SDL_GetRGB(px, surface->format, &r, &g, &b);
            rgb[out] = r;
            rgb[out + 1] = g;
            rgb[out + 2] = b;
        }
    }
    if (SDL_MUSTLOCK(surface)) SDL_UnlockSurface(surface);

    SDL_LockMutex(e2eDrawFrameMutex);
    E2eDrawFrame *slot =
        &e2eDrawFrames[(uint32_t)draw_count % E2E_DRAW_FRAME_RING_CAP];
    free(slot->rgb);
    slot->rgb = rgb;
    slot->rgb_len = rgb_len;
    slot->width = width;
    slot->height = height;
    slot->draw_count = draw_count;
    SDL_UnlockMutex(e2eDrawFrameMutex);
}

static int dump_e2e_draw_frame_ppm(int draw_count, const char *path) {
    if (!path || draw_count <= 0 || !e2eDrawFrameMutex) return -1;
    uint8_t *rgb = NULL;
    size_t rgb_len = 0;
    int width = 0;
    int height = 0;

    SDL_LockMutex(e2eDrawFrameMutex);
    E2eDrawFrame *slot =
        &e2eDrawFrames[(uint32_t)draw_count % E2E_DRAW_FRAME_RING_CAP];
    if (slot->draw_count == draw_count && slot->rgb && slot->rgb_len) {
        width = slot->width;
        height = slot->height;
        rgb_len = slot->rgb_len;
        rgb = (uint8_t *)malloc(rgb_len);
        if (rgb) memcpy(rgb, slot->rgb, rgb_len);
    }
    SDL_UnlockMutex(e2eDrawFrameMutex);

    if (!rgb) return -1;
    int ret = write_ppm_rgb(path, width, height, rgb, rgb_len);
    free(rgb);
    return ret;
}

static void sigusr1_handler(int sig) {
    (void)sig;
    dump_screen_ppm(screen_dump_path());
}

static int e2e_dump_screen_ppm_hook(const char *path, void *userdata) {
    (void)userdata;
    return dump_screen_ppm(path);
}

/* MOTION 命令注入动感样本(主线程回投后执行,见 e2e_control.c)。
 * 返回 0=已上送至 guest,非 0=guest 未开启动感监听。 */
static int e2e_motion_input_hook(int32_t x, int32_t y, int32_t z, void *userdata) {
    (void)userdata;
    return skyengine_motion_input(x, y, z) == MR_SUCCESS ? 0 : 1;
}

static int e2e_dump_draw_frame_ppm_hook(int draw_count, const char *path,
                                        void *userdata) {
    (void)userdata;
    return dump_e2e_draw_frame_ppm(draw_count, path);
}

static const char *e2e_screen_dump_path_hook(void *userdata) {
    (void)userdata;
    return screen_dump_path();
}

static int e2e_draw_count_hook(void *userdata) {
    (void)userdata;
    return SDL_AtomicGet(&guiDrawBitmapCount);
}

static uint32_t e2e_timer_arm_generation_hook(void *userdata) {
    (void)userdata;
    return (uint32_t)SDL_AtomicGet(&timerArmGeneration);
}

static uint32_t e2e_timer_dispatched_generation_hook(void *userdata) {
    (void)userdata;
    return (uint32_t)SDL_AtomicGet(&timerDispatchedGeneration);
}

static uint32_t e2e_timer_pending_generation_hook(void *userdata) {
    (void)userdata;
    return (uint32_t)SDL_AtomicGet(&timerPendingGeneration);
}

static int e2e_timer_dispatch_in_progress_hook(void *userdata) {
    (void)userdata;
    return SDL_AtomicGet(&timerDispatchInProgress);
}

static int e2e_runtime_exited_hook(void *userdata) {
    (void)userdata;
    return SDL_AtomicGet(&runtimeExited);
}

static void e2e_publish_runtime_exit(void) {
    if (skyengine_is_exited()) SDL_AtomicSet(&runtimeExited, 1);
}

static void e2e_publish_timer_dispatch(uint32_t generation) {
    int current = SDL_AtomicGet(&timerDispatchedGeneration);
    /* Removed timers can already have queued callbacks; never let a stale event
     * move the published completion generation backwards. */
    while ((int32_t)(generation - (uint32_t)current) > 0 &&
           !SDL_AtomicCAS(&timerDispatchedGeneration, current, (int)generation)) {
        current = SDL_AtomicGet(&timerDispatchedGeneration);
    }
}

/* timerCb 在 SDL 定时器线程中触发，直接调用 timer() 会与主线程的 event()
 * 同时访问同一个 Unicorn ARM 引擎，引发竞态崩溃。改为向 SDL 事件队列推送
 * 自定义事件，由主循环统一调度 timer()，保证单线程串行执行。 */
static Uint32 timerEventType = 0;
static Uint32 e2eEventType = (Uint32)-1;
static E2eControl *e2eControl = NULL;
static bool isEditMode = false;
static int32_t editMaxSize = 0;
static char *holdEditText = NULL;
static uint32_t clickSeq = 0;

static SkyEngineKeyLatch keyLatch = SKYENGINE_KEY_LATCH_INITIALIZER;

#if defined(__ANDROID__)
/* Android 全屏窗口要先把 guest 画面按比例居中放进游戏区 gameDst，再把触屏
 * 坐标从窗口回映射到 guest 逻辑分辨率。窗口尺寸在创建/旋转后由
 * SDL_WINDOWEVENT_SIZE_CHANGED 刷新（见 loop()）。rotation 由新引擎在
 * skyengine_display_width/height 内自管，指针映射不额外叠加旋转。 */
static SDL_Rect g_game_dst = {0, 0, 0, 0};
static void update_game_dst(void) {
    int winW = 0, winH = 0;
    if (!window) return;
    SDL_GetWindowSize(window, &winW, &winH);
    android_compute_layout(winW, winH, skyengine_display_width(),
                           skyengine_display_height(), &g_game_dst);
}
static int map_mouse(int px, int py, int *lx, int *ly) {
    return android_map_pointer(px, py, &g_game_dst, 0, 0,
                               skyengine_display_width(),
                               skyengine_display_height(), lx, ly);
}
#else
static int map_mouse(int px, int py, int *lx, int *ly) {
    /* 桌面按 1:1 直通，保持既有行为。 */
    *lx = px;
    *ly = py;
    return 1;
}
#endif

#ifndef __EMSCRIPTEN__
static SDL_Window *edit_win = NULL;
static SDL_Renderer *edit_ren = NULL;
static char edit_buf[512];
static int  edit_cursor = 0;
static int  edit_scroll = 0;
static bool edit_confirm = false;
static bool edit_cancel = false;
static bool edit_nav_lock = false;
static char edit_title[128];
#define EDIT_VISIBLE_LINES 6
#define EDIT_MAXCOL 26
typedef struct { int start; } EditRow;
static EditRow edit_rows[256];
static int edit_nrows = 0;
#endif

#ifndef __EMSCRIPTEN__
static const unsigned char edit_font[128][8] = {
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, { 0x18, 0x3C, 0x3C, 0x18, 0x18, 0x00, 0x18, 0x00},
    { 0x36, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, { 0x36, 0x36, 0x7F, 0x36, 0x7F, 0x36, 0x36, 0x00},
    { 0x0C, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x0C, 0x00}, { 0x00, 0x63, 0x33, 0x18, 0x0C, 0x66, 0x63, 0x00},
    { 0x1C, 0x36, 0x1C, 0x6E, 0x3B, 0x33, 0x6E, 0x00}, { 0x06, 0x06, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00},
    { 0x18, 0x0C, 0x06, 0x06, 0x06, 0x0C, 0x18, 0x00}, { 0x06, 0x0C, 0x18, 0x18, 0x18, 0x0C, 0x06, 0x00},
    { 0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00}, { 0x00, 0x0C, 0x0C, 0x3F, 0x0C, 0x0C, 0x00, 0x00},
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x06}, { 0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x00, 0x00},
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x00}, { 0x60, 0x30, 0x18, 0x0C, 0x06, 0x03, 0x01, 0x00},
    { 0x3E, 0x63, 0x73, 0x7B, 0x6F, 0x67, 0x3E, 0x00}, { 0x0C, 0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x3F, 0x00},
    { 0x1E, 0x33, 0x30, 0x1C, 0x06, 0x33, 0x3F, 0x00}, { 0x1E, 0x33, 0x30, 0x1C, 0x30, 0x33, 0x1E, 0x00},
    { 0x38, 0x3C, 0x36, 0x33, 0x7F, 0x30, 0x78, 0x00}, { 0x3F, 0x03, 0x1F, 0x30, 0x30, 0x33, 0x1E, 0x00},
    { 0x1C, 0x06, 0x03, 0x1F, 0x33, 0x33, 0x1E, 0x00}, { 0x3F, 0x33, 0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x00},
    { 0x1E, 0x33, 0x33, 0x1E, 0x33, 0x33, 0x1E, 0x00}, { 0x1E, 0x33, 0x33, 0x3E, 0x30, 0x18, 0x0E, 0x00},
    { 0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x00}, { 0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x06},
    { 0x18, 0x0C, 0x06, 0x03, 0x06, 0x0C, 0x18, 0x00}, { 0x00, 0x00, 0x3F, 0x00, 0x00, 0x3F, 0x00, 0x00},
    { 0x06, 0x0C, 0x18, 0x30, 0x18, 0x0C, 0x06, 0x00}, { 0x1E, 0x33, 0x30, 0x18, 0x0C, 0x00, 0x0C, 0x00},
    { 0x3E, 0x63, 0x7B, 0x7B, 0x7B, 0x03, 0x1E, 0x00}, { 0x0C, 0x1E, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x00},
    { 0x3F, 0x66, 0x66, 0x3E, 0x66, 0x66, 0x3F, 0x00}, { 0x3C, 0x66, 0x03, 0x03, 0x03, 0x66, 0x3C, 0x00},
    { 0x1F, 0x36, 0x66, 0x66, 0x66, 0x36, 0x1F, 0x00}, { 0x7F, 0x46, 0x16, 0x1E, 0x16, 0x46, 0x7F, 0x00},
    { 0x7F, 0x46, 0x16, 0x1E, 0x16, 0x06, 0x0F, 0x00}, { 0x3C, 0x66, 0x03, 0x03, 0x73, 0x66, 0x7C, 0x00},
    { 0x33, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x33, 0x00}, { 0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},
    { 0x78, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E, 0x00}, { 0x67, 0x66, 0x36, 0x1E, 0x36, 0x66, 0x67, 0x00},
    { 0x0F, 0x06, 0x06, 0x06, 0x46, 0x66, 0x7F, 0x00}, { 0x63, 0x77, 0x7F, 0x7F, 0x6B, 0x63, 0x63, 0x00},
    { 0x63, 0x67, 0x6F, 0x7B, 0x73, 0x63, 0x63, 0x00}, { 0x1C, 0x36, 0x63, 0x63, 0x63, 0x36, 0x1C, 0x00},
    { 0x3F, 0x66, 0x66, 0x3E, 0x06, 0x06, 0x0F, 0x00}, { 0x1E, 0x33, 0x33, 0x33, 0x3B, 0x1E, 0x38, 0x00},
    { 0x3F, 0x66, 0x66, 0x3E, 0x36, 0x66, 0x67, 0x00}, { 0x1E, 0x33, 0x07, 0x0E, 0x38, 0x33, 0x1E, 0x00},
    { 0x3F, 0x2D, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00}, { 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x3F, 0x00},
    { 0x33, 0x33, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00}, { 0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00},
    { 0x63, 0x63, 0x36, 0x1C, 0x1C, 0x36, 0x63, 0x00}, { 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x0C, 0x1E, 0x00},
    { 0x7F, 0x63, 0x31, 0x18, 0x4C, 0x66, 0x7F, 0x00}, { 0x1E, 0x06, 0x06, 0x06, 0x06, 0x06, 0x1E, 0x00},
    { 0x03, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x40, 0x00}, { 0x1E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x1E, 0x00},
    { 0x08, 0x1C, 0x36, 0x63, 0x00, 0x00, 0x00, 0x00}, { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF},
    { 0x0C, 0x0C, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00}, { 0x00, 0x00, 0x1E, 0x30, 0x3E, 0x33, 0x6E, 0x00},
    { 0x07, 0x06, 0x06, 0x3E, 0x66, 0x66, 0x3B, 0x00}, { 0x00, 0x00, 0x1E, 0x33, 0x03, 0x33, 0x1E, 0x00},
    { 0x38, 0x30, 0x30, 0x3e, 0x33, 0x33, 0x6E, 0x00}, { 0x00, 0x00, 0x1E, 0x33, 0x3f, 0x03, 0x1E, 0x00},
    { 0x1C, 0x36, 0x06, 0x0f, 0x06, 0x06, 0x0F, 0x00}, { 0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x1F},
    { 0x07, 0x06, 0x36, 0x6E, 0x66, 0x66, 0x67, 0x00}, { 0x0C, 0x00, 0x0E, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},
    { 0x30, 0x00, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E}, { 0x07, 0x06, 0x66, 0x36, 0x1E, 0x36, 0x67, 0x00},
    { 0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00}, { 0x00, 0x00, 0x33, 0x7F, 0x7F, 0x6B, 0x63, 0x00},
    { 0x00, 0x00, 0x1F, 0x33, 0x33, 0x33, 0x33, 0x00}, { 0x00, 0x00, 0x1E, 0x33, 0x33, 0x33, 0x1E, 0x00},
    { 0x00, 0x00, 0x3B, 0x66, 0x66, 0x3E, 0x06, 0x0F}, { 0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x78},
    { 0x00, 0x00, 0x3B, 0x6E, 0x66, 0x06, 0x0F, 0x00}, { 0x00, 0x00, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x00},
    { 0x08, 0x0C, 0x3E, 0x0C, 0x0C, 0x2C, 0x18, 0x00}, { 0x00, 0x00, 0x33, 0x33, 0x33, 0x33, 0x6E, 0x00},
    { 0x00, 0x00, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00}, { 0x00, 0x00, 0x63, 0x6B, 0x7F, 0x7F, 0x36, 0x00},
    { 0x00, 0x00, 0x63, 0x36, 0x1C, 0x36, 0x63, 0x00}, { 0x00, 0x00, 0x33, 0x33, 0x33, 0x3E, 0x30, 0x1F},
    { 0x00, 0x00, 0x3F, 0x19, 0x0C, 0x26, 0x3F, 0x00}, { 0x38, 0x0C, 0x0C, 0x07, 0x0C, 0x0C, 0x38, 0x00},
    { 0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x18, 0x00}, { 0x07, 0x0C, 0x0C, 0x38, 0x0C, 0x0C, 0x07, 0x00},
    { 0x6E, 0x3B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}
};

static void edit_draw_char(SDL_Renderer *ren, int x, int y, int scale, unsigned char c, int r, int g, int b) {
    const unsigned char *glyph = edit_font[c & 0x7F];
    SDL_SetRenderDrawColor(ren, r, g, b, 255);
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
            if (glyph[row] & (0x01 << col)) {
                SDL_Rect rc = { x + col * scale, y + row * scale, scale, scale };
                SDL_RenderFillRect(ren, &rc);
            }
        }
    }
}

static void edit_draw_text(SDL_Renderer *ren, int x, int y, int scale, const char *text, int r, int g, int b) {
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        edit_draw_char(ren, x, y, scale, *p, r, g, b);
        x += 8 * scale + 1;
    }
}

#define EDIT_BTN_H 28
#define EDIT_BTN_Y 178
#define EDIT_BTN_OK_X 120
#define EDIT_BTN_CANCEL_X 260
#define EDIT_BTN_W 100

static void edit_draw_button(SDL_Renderer *ren, int x, int y, int w, int h, const char *label, int r, int g, int b) {
    SDL_SetRenderDrawColor(ren, 0x3a, 0x41, 0x52, 255);
    SDL_Rect bg = { x, y, w, h };
    SDL_RenderFillRect(ren, &bg);
    SDL_SetRenderDrawColor(ren, 0x8a, 0x95, 0xb0, 255);
    SDL_RenderDrawRect(ren, &bg);
    int lw = (int)strlen(label) * (8 * 2 + 1) - 1;
    int lx = x + (w - lw) / 2;
    int ly = y + (h - 8 * 2) / 2;
    edit_draw_text(ren, lx, ly, 2, label, r, g, b);
}

static int utf8_prev_index(const char *s, int pos);
static int utf8_next_index(const char *s, int pos);
static void edit_insert_at(const char *ins, int add);
static void edit_build_rows(void);
static int edit_row_content_end(int r);
static int edit_row_count(void);
static void edit_pos_to_visual(int pos, int *row, int *col);
static int edit_visual_to_pos(int row, int col);
static void edit_ensure_visible(void);

static void edit_render(void) {
    if (!edit_win || !edit_ren) return;
    edit_build_rows();
    if (!edit_nav_lock) edit_ensure_visible();
    SDL_SetRenderDrawColor(edit_ren, 0x22, 0x26, 0x30, 255);
    SDL_RenderClear(edit_ren);
    char caption[64];
    snprintf(caption, sizeof(caption), "%s",
             (edit_title[0] ? edit_title : "input"));
    int capIsAscii = 1;
    for (const unsigned char *cc = (const unsigned char *)caption; *cc; cc++)
        if (*cc >= 0x80) { capIsAscii = 0; break; }
    if (capIsAscii) {
        size_t capLen = strlen(caption);
        if (capLen > 24) { caption[24] = '\0'; capLen = 24; }
        edit_draw_text(edit_ren, 12, 8, 2, caption, 0xff, 0xd0, 0x60);
    }
    SDL_Rect box = { 12, 34, 456, 136 };
    SDL_SetRenderDrawColor(edit_ren, 0x0e, 0x10, 0x18, 255);
    SDL_RenderFillRect(edit_ren, &box);
    SDL_SetRenderDrawColor(edit_ren, 0x60, 0x6c, 0x88, 255);
    SDL_RenderDrawRect(edit_ren, &box);
    const int lineh = 8 * 2 + 4;
    int tx = 20, ty = 40;
    int totalRows = edit_row_count();
    if (edit_scroll < 0) edit_scroll = 0;
    if (edit_scroll > totalRows - 1) edit_scroll = totalRows - 1;
    for (int r = edit_scroll; r < edit_scroll + EDIT_VISIBLE_LINES && r < totalRows; r++) {
        int rs = edit_rows[r].start;
        int ce = edit_row_content_end(r);
        char line[256];
        int n = ce - rs;
        if (n > (int)sizeof(line) - 1) n = (int)sizeof(line) - 1;
        memcpy(line, edit_buf + rs, n);
        line[n] = '\0';
        edit_draw_text(edit_ren, tx, ty, 2, line, 0xf0, 0xf0, 0xf0);
        ty += lineh;
    }
    {
        int crow, ccol;
        edit_pos_to_visual(edit_cursor, &crow, &ccol);
        int caretX, caretY;
        if (crow >= edit_scroll && crow < edit_scroll + EDIT_VISIBLE_LINES) {
            caretX = tx + ccol * (8 * 2 + 1);
            caretY = 40 + (crow - edit_scroll) * lineh;
        } else {
            caretX = tx; caretY = 40;
        }
        SDL_SetRenderDrawColor(edit_ren, 0xf0, 0xf0, 0xf0, 255);
        SDL_Rect caret = { caretX, caretY, 2, 16 };
        SDL_RenderFillRect(edit_ren, &caret);
    }
    if (totalRows > EDIT_VISIBLE_LINES && edit_ren) {
        const int sbX = 462, sbTop = 34, sbH = 136, sbW = 4;
        SDL_SetRenderDrawColor(edit_ren, 0x3a, 0x41, 0x52, 255);
        SDL_Rect track = { sbX, sbTop, sbW, sbH };
        SDL_RenderFillRect(edit_ren, &track);
        int thumbH = sbH * EDIT_VISIBLE_LINES / totalRows;
        if (thumbH < 12) thumbH = 12;
        int maxScroll = totalRows - EDIT_VISIBLE_LINES;
        int thumbY = sbTop + (maxScroll > 0 ? (sbH - thumbH) * edit_scroll / maxScroll : 0);
        SDL_SetRenderDrawColor(edit_ren, 0x8a, 0x95, 0xb0, 255);
        SDL_Rect thumb = { sbX, thumbY, sbW, thumbH };
        SDL_RenderFillRect(edit_ren, &thumb);
    }
    edit_draw_button(edit_ren, EDIT_BTN_OK_X, EDIT_BTN_Y, EDIT_BTN_W, EDIT_BTN_H, "OK", 0x9f, 0xe0, 0x9f);
    edit_draw_button(edit_ren, EDIT_BTN_CANCEL_X, EDIT_BTN_Y, EDIT_BTN_W, EDIT_BTN_H, "Cancel", 0xf0, 0x9f, 0x9f);
    SDL_RenderPresent(edit_ren);
}

static void edit_close_win(void);

static int utf8_prev_index(const char *s, int pos) {
    if (pos <= 0) return 0;
    int i = pos - 1;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    return i;
}

static int utf8_next_index(const char *s, int pos) {
    int len = (int)strlen(s);
    if (pos >= len) return len;
    int i = pos + 1;
    while (i < len && ((unsigned char)s[i] & 0xC0) == 0x80) i++;
    return i;
}

static void edit_insert_at(const char *ins, int add) {
    int cur = edit_cursor;
    if (cur < 0) cur = 0;
    if (cur > (int)strlen(edit_buf)) cur = (int)strlen(edit_buf);
    if (add > 0 && add + (int)strlen(edit_buf) >= (int)sizeof(edit_buf)) return;
    memmove(edit_buf + cur + add, edit_buf + cur, strlen(edit_buf) - cur + 1);
    memcpy(edit_buf + cur, ins, add);
    edit_cursor = cur + add;
}

static void edit_build_rows(void) {
    edit_nrows = 0;
    int len = (int)strlen(edit_buf);
    int i = 0;
    while (i <= len) {
        if (edit_nrows >= 256) break;
        edit_rows[edit_nrows].start = i;
        edit_nrows++;
        if (i >= len) break;
        int col = 0;
        while (i < len && col < EDIT_MAXCOL) {
            if (edit_buf[i] == '\n') { i++; break; }
            i = utf8_next_index(edit_buf, i);
            col++;
        }
    }
}

static int edit_row_content_end(int r) {
    if (r + 1 < edit_nrows) {
        int nx = edit_rows[r + 1].start;
        if (nx > edit_rows[r].start && edit_buf[nx - 1] == '\n') return nx - 1;
        return nx;
    }
    return (int)strlen(edit_buf);
}

static int edit_row_count(void) { return edit_nrows; }

static void edit_pos_to_visual(int pos, int *row, int *col) {
    int len = (int)strlen(edit_buf);
    if (pos < 0) pos = 0;
    if (pos > len) pos = len;
    for (int r = 0; r < edit_nrows; r++) {
        int ce = edit_row_content_end(r);
        if (pos <= ce) {
            int c = 0, i = edit_rows[r].start;
            while (i < pos) { i = utf8_next_index(edit_buf, i); c++; }
            *row = r; *col = c;
            return;
        }
    }
    *row = edit_nrows > 0 ? edit_nrows - 1 : 0;
    *col = 0;
}

static int edit_visual_to_pos(int row, int col) {
    if (row < 0) row = 0;
    if (row >= edit_nrows) row = edit_nrows - 1;
    int ce = edit_row_content_end(row);
    int i = edit_rows[row].start;
    while (col > 0 && i < ce) { i = utf8_next_index(edit_buf, i); col--; }
    return i;
}

static void edit_ensure_visible(void) {
    int row, col;
    edit_pos_to_visual(edit_cursor, &row, &col);
    int total = edit_row_count();
    if (total <= 0 || edit_nrows <= 0) { edit_scroll = 0; return; }
    if (edit_scroll < 0) edit_scroll = 0;
    if (edit_scroll > total - 1) edit_scroll = total - 1;
    if (row < edit_scroll) edit_scroll = row;
    if (row >= edit_scroll + EDIT_VISIBLE_LINES) edit_scroll = row - EDIT_VISIBLE_LINES + 1;
    if (edit_scroll < 0) edit_scroll = 0;
}

static void edit_open_win(const char *title) {
    edit_close_win();
    if (title && title[0]) {
        snprintf(edit_title, sizeof(edit_title), "%s", title);
    } else {
        snprintf(edit_title, sizeof(edit_title), "input");
    }
    edit_win = SDL_CreateWindow(edit_title,
                                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                480, 220, SDL_WINDOW_SHOWN | SDL_WINDOW_ALWAYS_ON_TOP);
    if (!edit_win) {
        SDL_Log("create input window failed: %s", SDL_GetError());
        return;
    }
    edit_ren = SDL_CreateRenderer(edit_win, -1, SDL_RENDERER_SOFTWARE);
    edit_render();
}

static void edit_close_win(void) {
    if (edit_ren) { SDL_DestroyRenderer(edit_ren); edit_ren = NULL; }
    if (edit_win) { SDL_DestroyWindow(edit_win); edit_win = NULL; }
}

static bool edit_handle_event(const SDL_Event *ev) {
    if (!edit_win) return false;
    Uint32 wid = SDL_GetWindowID(edit_win);
    switch (ev->type) {
        case SDL_TEXTINPUT:
            if (ev->text.windowID != wid) return false;
            edit_insert_at(ev->text.text, (int)strlen(ev->text.text));
            edit_render();
            return true;
        case SDL_MOUSEBUTTONDOWN:
            if (ev->button.windowID != wid) return false;
            if (ev->button.button == SDL_BUTTON_LEFT) {
                int mx = ev->button.x;
                int my = ev->button.y;
                if (mx >= EDIT_BTN_OK_X && mx < EDIT_BTN_OK_X + EDIT_BTN_W &&
                    my >= EDIT_BTN_Y && my < EDIT_BTN_Y + EDIT_BTN_H) {
                    edit_confirm = true;
                } else if (mx >= EDIT_BTN_CANCEL_X && mx < EDIT_BTN_CANCEL_X + EDIT_BTN_W &&
                           my >= EDIT_BTN_Y && my < EDIT_BTN_Y + EDIT_BTN_H) {
                    edit_cancel = true;
                }
            }
            if (edit_confirm || edit_cancel) edit_close_win();
            return true;
        case SDL_MOUSEWHEEL:
            if (ev->wheel.windowID != wid) return false;
            edit_nav_lock = true;
            edit_scroll -= ev->wheel.y;
            if (edit_scroll < 0) edit_scroll = 0;
            if (edit_scroll > edit_row_count() - 1) edit_scroll = edit_row_count() - 1;
            edit_render();
            edit_nav_lock = false;
            return true;
        case SDL_KEYDOWN:
            if (ev->key.windowID != wid) return false;
            if (ev->key.keysym.sym == SDLK_RETURN || ev->key.keysym.sym == SDLK_KP_ENTER) {
                edit_insert_at("\n", 1);
                edit_render();
            } else if (ev->key.keysym.sym == SDLK_ESCAPE) {
                edit_cancel = true;
                edit_close_win();
            } else if (ev->key.keysym.sym == SDLK_BACKSPACE) {
                int pos = edit_cursor;
                if (pos <= 0) {
                    edit_render();
                } else {
                    int prev = utf8_prev_index(edit_buf, pos);
                    memmove(edit_buf + prev, edit_buf + pos, strlen(edit_buf) - pos + 1);
                    edit_cursor = prev;
                    edit_render();
                }
            } else if (ev->key.keysym.sym == SDLK_DELETE) {
                int pos = edit_cursor;
                int len = (int)strlen(edit_buf);
                if (pos < len) {
                    int nxt = utf8_next_index(edit_buf, pos);
                    memmove(edit_buf + pos, edit_buf + nxt, strlen(edit_buf) - nxt + 1);
                    edit_render();
                }
            } else if (ev->key.keysym.sym == SDLK_LEFT) {
                if (edit_cursor > 0) edit_cursor = utf8_prev_index(edit_buf, edit_cursor);
                edit_render();
            } else if (ev->key.keysym.sym == SDLK_RIGHT) {
                if (edit_cursor < (int)strlen(edit_buf)) edit_cursor = utf8_next_index(edit_buf, edit_cursor);
                edit_render();
            } else if (ev->key.keysym.sym == SDLK_UP) {
                int row, col;
                edit_pos_to_visual(edit_cursor, &row, &col);
                if (row > 0) edit_cursor = edit_visual_to_pos(row - 1, col);
                edit_render();
            } else if (ev->key.keysym.sym == SDLK_DOWN) {
                int row, col;
                edit_pos_to_visual(edit_cursor, &row, &col);
                if (row < edit_row_count() - 1) edit_cursor = edit_visual_to_pos(row + 1, col);
                edit_render();
            } else if (ev->key.keysym.sym == SDLK_PAGEUP) {
                edit_scroll -= EDIT_VISIBLE_LINES;
                if (edit_scroll < 0) edit_scroll = 0;
                edit_render();
            } else if (ev->key.keysym.sym == SDLK_PAGEDOWN) {
                edit_scroll += EDIT_VISIBLE_LINES;
                if (edit_scroll > edit_row_count() - 1) edit_scroll = edit_row_count() - 1;
                edit_render();
            } else if (ev->key.keysym.sym == SDLK_HOME) {
                edit_cursor = 0;
                edit_render();
            } else if (ev->key.keysym.sym == SDLK_END) {
                edit_cursor = (int)strlen(edit_buf);
                edit_render();
            } else if (ev->key.keysym.sym == SDLK_v && (SDL_GetModState() & KMOD_CTRL)) {
                char *s = SDL_GetClipboardText();
                if (s && *s) edit_insert_at(s, (int)strlen(s));
                if (s) SDL_free(s);
                edit_render();
            }
            return true;
        case SDL_WINDOWEVENT:
            if (ev->window.windowID != wid) return false;
            if (ev->window.event == SDL_WINDOWEVENT_CLOSE) {
                edit_cancel = true;
                edit_close_win();
            }
            return true;
        default:
            return false;
    }
}
#endif

void saveEditText(char *str) {
    uint8_t *utf8Str = (uint8_t *)str;
    int32_t n = 0;
    while (*utf8Str && (n < editMaxSize)) {
        if (*utf8Str < 0x80) {  // 1 Byte
            utf8Str += 1;
        } else if ((*utf8Str & 0xe0) == 0xc0) {  // 2 Bytes
            utf8Str += 2;
        } else if ((*utf8Str & 0xf0) == 0xe0) {  // 3 Bytes
            utf8Str += 3;
        } else {
            break;
        }
        n++;
    }
    if (holdEditText != NULL) {
        my_freeExt(holdEditText);
        holdEditText = NULL;
    }
    uint32_t len = (uint32_t)((uintptr_t)utf8Str - (uintptr_t)str);
    holdEditText = my_mallocExt(len + 1);
    memcpy(holdEditText, str, len);
    holdEditText[len] = '\0';
}

/* 震动马达 bridge(mr_startShake/mr_stopShake):SDL 桌面前端没有振动器
 * 硬件,仅日志记录;真实震动由 Flutter 前端经 skyengine_api_take_shake 对接。 */
void guiStartShake(int32_t ms) {
    SDL_Log("guiStartShake(%d ms)", ms);
}

void guiStopShake(void) {
    SDL_Log("guiStopShake()");
}

int32_t editCreate(const char *title, const char *text, int32_t type, int32_t max_size) {
    isEditMode = true;
    editMaxSize = max_size;
    /* FLAG_USE_UTF8_EDIT 已启用:guest 的 UCS-2 大端字符串已由
     * mythroad/dsm.mr_editCreate 转成 UTF-8,这里 title/text 直接是 UTF-8。
     * 不能再用 ucs2be_to_utf8 处理,否则会二次转换导致标题乱码、输入变乱。 */
    SDL_Log("title: '%s', text: '%s', type: %d, max_size: %d",
            title ? title : "", text ? text : "", type, max_size);
#if defined(__ANDROID__)
    {
        char utf8_title[128] = {0};
        char utf8_input[256] = {0};
        if (title) snprintf(utf8_title, sizeof(utf8_title), "%s", title);
        if (text) snprintf(utf8_input, sizeof(utf8_input), "%s", text);
        saveEditText(utf8_input);
        android_start_edit(utf8_title, utf8_input, max_size);
    }
#elif !defined(__EMSCRIPTEN__)
    edit_buf[0] = '\0';
    edit_confirm = edit_cancel = false;
    if (text) snprintf(edit_buf, sizeof(edit_buf), "%s", text);
    edit_cursor = (int)strlen(edit_buf);
    edit_scroll = 0;
    edit_nav_lock = false;
    edit_open_win(title);
#else
    if (SDL_SetClipboardText(text ? text : "") == 0) {
        SDL_Log("编辑内容已复制到剪贴板，按ctrl+v输入内容，按ctrl+z取消");
    } else {
        SDL_Log("无法使用剪贴板");
    }
#endif
    return 1234;
}

int32 editRelease(int32 edit) {
    (void)edit;
    isEditMode = false;
#if defined(__ANDROID__)
    android_stop_edit();
#elif !defined(__EMSCRIPTEN__)
    edit_confirm = edit_cancel = false;
    edit_close_win();
#endif
    if (holdEditText != NULL) {
        my_freeExt(holdEditText);
        holdEditText = NULL;
    }
    return MR_SUCCESS;
}

char *editGetText(int32 edit) {
    (void)edit;
    SDL_Log("editGetText(): '%s'", holdEditText ? holdEditText : "(null)");
    /* FLAG_USE_UTF8_EDIT 已启用:这里直接返回 UTF-8,mythroad/dsm.mr_editGetText
     * 会把它 UTF-8 → GB → UCS2-BE 交还 guest。不能再返回 UCS2-BE,否则二次转换
     * 会让输入内容变成不可见的错误字符。 */
    return holdEditText;
}

void guiDrawBitmapWithStride(uint16_t *bmp, int32_t x, int32_t y,
                             int32_t w, int32_t h,
                             int32_t source_stride,
                             int32_t source_x,
                             int32_t source_y) {
    if (!bmp || source_stride <= 0 || w <= 0 || h <= 0) return;
    /* 平台文本框(native_text_widget):guest 帧先写入显示镜像;文本框显示
     * 期间该帧被平台窗口遮盖——不上屏、不计入 draw_count(可见帧计数),
     * 关闭文本框时由 widget 重推镜像恢复画面。文本框自身的上屏在 widget
     * 内部带 presenting 标记,不会被这里截留。 */
    if (native_text_widget_capture_frame(bmp, x, y, w, h,
                                         source_stride, source_x, source_y)) {
        return;
    }
    int draw_count = SDL_AtomicAdd(&guiDrawBitmapCount, 1) + 1;
    /* Dump after the bitmap is copied to the SDL surface.  Dumping before the
     * draw captures the previous frame, which makes VMRP_PPM misleading for
     * foreground handoff bugs where the last visible frame matters.  When
     * VMRP_PPM is set, the caller has explicitly requested verification, so
     * keep the configured PPM path equal to the most recent rendered frame. */
    int should_dump_ppm = getenv("SKYENGINE_PPM") || draw_count == 5;
    /* LCD 旋转(plat(101))后的横屏自动翻转:显示尺寸与窗口不一致时调整窗口。
     * VM 全部在 SDL 主循环线程执行(定时器回调仅 SDL_PushEvent),此处调
     * SDL_SetWindowSize 线程安全;resize 使旧 surface 失效,须在取 surface
     * 之前完成。rotation==0 时显示尺寸恒等于窗口创建尺寸,行为不变。 */
    int display_w = skyengine_display_width();
    int display_h = skyengine_display_height();
#if defined(__ANDROID__)
    /* Android 全屏窗口尺寸固定，不能调 SDL_SetWindowSize；改为每次上屏重算
     * 游戏区（含 guest 旋转后的显示尺寸变化），并把 guest 像素按最近邻缩放
     * 到 gameDst 居中区域。 */
    update_game_dst();
#else
    {
        int win_w = 0, win_h = 0;
        SDL_GetWindowSize(window, &win_w, &win_h);
        if (win_w != display_w || win_h != display_h) {
            SDL_SetWindowSize(window, display_w, display_h);
        }
    }
#endif
    SDL_Surface *surface = SDL_GetWindowSurface(window);
    if (!surface) return;
    if (SDL_MUSTLOCK(surface)) {
        if (SDL_LockSurface(surface) != 0) printf("SDL_LockSurface err\n");
    }
#if defined(__ANDROID__)
    if (g_game_dst.w > 0 && g_game_dst.h > 0) {
        int dstW = g_game_dst.w + g_game_dst.x;
        int dstH = g_game_dst.h + g_game_dst.y;
        float scl_x = (float)g_game_dst.w / (float)display_w;
        float scl_y = (float)g_game_dst.h / (float)display_h;
        for (int32_t j = 0; j < h; j++) {
            int32_t yy = y + j;
            if (yy < 0 || yy >= display_h) continue;
            for (int32_t i = 0; i < w; i++) {
                int32_t xx = x + i;
                if (xx < 0 || xx >= display_w) continue;
                int32_t sx = source_x + i;
                int32_t sy = source_y + j;
                if (sx < 0 || sy < 0 || sx >= source_stride) continue;
                uint16_t color = *(bmp + ((size_t)sy * (size_t)source_stride + (size_t)sx));
                int dx0 = g_game_dst.x + (int)((float)xx * scl_x);
                int dx1 = g_game_dst.x + (int)((float)(xx + 1) * scl_x);
                int dy0 = g_game_dst.y + (int)((float)yy * scl_y);
                int dy1 = g_game_dst.y + (int)((float)(yy + 1) * scl_y);
                if (dx1 <= dx0) dx1 = dx0 + 1;
                if (dy1 <= dy0) dy1 = dy0 + 1;
                if (dy0 < 0) dy0 = 0;
                if (dx0 < 0) dx0 = 0;
                if (dy1 > dstH) dy1 = dstH;
                if (dx1 > dstW) dx1 = dstW;
                for (int32_t dyy = dy0; dyy < dy1; dyy++) {
                    for (int32_t dxx = dx0; dxx < dx1; dxx++) {
                        Uint32 *p = (Uint32 *)(((Uint8 *)surface->pixels) +
                                                surface->pitch * dyy) + dxx;
                        *p = SDL_MapRGB(surface->format,
                                        PIXEL565R(color), PIXEL565G(color), PIXEL565B(color));
                    }
                }
            }
        }
    }
#else
    for (int32_t j = 0; j < h; j++) {
        for (int32_t i = 0; i < w; i++) {
            int32_t xx = x + i;
            int32_t yy = y + j;
            /* 裁剪按旋转后的显示尺寸(rotation==0 时即面板尺寸) */
            if (xx < 0 || yy < 0 || xx >= display_w || yy >= display_h) {
                continue;
            }
            int32_t sx = source_x + i;
            int32_t sy = source_y + j;
            if (sx < 0 || sy < 0 || sx >= source_stride) continue;
            uint16_t color = *(bmp + ((size_t)sy * (size_t)source_stride + (size_t)sx));
            Uint32 *p = (Uint32 *)(((Uint8 *)surface->pixels) + surface->pitch * yy) + xx;
            *p = SDL_MapRGB(surface->format, PIXEL565R(color), PIXEL565G(color), PIXEL565B(color));
        }
    }
#endif
    if (SDL_MUSTLOCK(surface)) SDL_UnlockSurface(surface);
    if (SDL_UpdateWindowSurface(window) != 0)
        printf("SDL_UpdateWindowSurface err\n");
    e2e_record_draw_frame(draw_count, surface);
    if (should_dump_ppm) {
        dump_screen_ppm(screen_dump_path());
    }
}

void guiDrawBitmap(uint16_t *bmp, int32_t x, int32_t y, int32_t w, int32_t h) {
    /*
     * Mythroad _DispUpEx submits rectangles from mr_screenBuf, whose source
     * coordinates are absolute screen coordinates.  Keep this legacy full-screen
     * stride path and let ARM EXT local bitmap presents opt into
     * guiDrawBitmapWithStride().
     * 行宽取旋转后的显示宽度:plat(101) 横屏后全屏缓冲行宽即显示宽,
     * rotation==0 时与面板宽度相同。
     */
    guiDrawBitmapWithStride(bmp, x, y, w, h,
                            skyengine_display_width(), x, y);
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
void setEventEnable(int v) {
    int state = v ? SDL_ENABLE : SDL_DISABLE;
    SDL_EventState(SDL_TEXTINPUT, state);
    SDL_EventState(SDL_KEYDOWN, state);
    SDL_EventState(SDL_KEYUP, state);
    SDL_EventState(SDL_MOUSEMOTION, state);
    SDL_EventState(SDL_MOUSEBUTTONDOWN, state);
    SDL_EventState(SDL_MOUSEBUTTONUP, state);
}
#endif

uint32_t timerCb(uint32_t interval, void *param) {
    (void)interval; /* 签名由 SDL_AddTimer 回调 ABI 固定 */
    uint32_t generation = (uint32_t)(uintptr_t)param;
    /* 回调返回 0 已使 SDL timer 成为 one-shot；不在 timer 线程改写 timeId，
     * 否则一个刚被替换的旧 callback 可能清掉主线程保存的新 timer identity。 */
    SDL_Event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = timerEventType;
    ev.user.data1 = (void *)(uintptr_t)generation;
    if (SDL_PushEvent(&ev) != 1) {
        /* 只清除本代；较新的 timerStart 不能被较旧 callback 的失败覆盖。 */
        SDL_AtomicCAS(&timerPendingGeneration, (int)generation, 0);
    }
    return 0;
}

int32_t timerStart(uint16_t t) {
    uint32_t generation = (uint32_t)SDL_AtomicAdd(&timerArmGeneration, 1) + 1;
    /* 先发布身份，callback 即使立即运行也能针对同一 generation 清理失败状态。 */
    SDL_AtomicSet(&timerPendingGeneration, (int)generation);
    if (!timeId) {
        timeId = SDL_AddTimer(t, timerCb, (void *)(uintptr_t)generation);
    } else {
        SDL_RemoveTimer(timeId);
        timeId = SDL_AddTimer(t, timerCb, (void *)(uintptr_t)generation);
    }
    /* generation 即 timer 的身份；0 专门表示已停止或 arm 失败。 */
    if (!timeId) SDL_AtomicCAS(&timerPendingGeneration, (int)generation, 0);
    return MR_SUCCESS;
}

int32_t timerStop(void) {
    if (timeId) {
        SDL_RemoveTimer(timeId);
        timeId = 0;
    }
    SDL_AtomicSet(&timerPendingGeneration, 0);
    return MR_SUCCESS;
}

static SDL_Keycode keyboard_event_keycode(const SDL_KeyboardEvent *key) {
#ifdef _WIN32
    return (SDL_Keycode)skyengine_normalize_windows_keycode(
        (int32_t)key->keysym.sym, (int32_t)key->keysym.scancode,
        key->windowID == E2E_KEY_WINDOW_ID);
#else
    return key->keysym.sym;
#endif
}

static int dispatch_key_down(SDL_Keycode code, Uint8 repeat) {
    int32_t mr_key;
    if (!skyengine_key_latch_press(&keyLatch, (int32_t)code, &mr_key)) {
        /* Preserve the useful unknown-key diagnostic without allowing that key
         * to occupy the handset's single active-key slot. */
        if (!repeat && keyLatch.active_keycode == SDLK_UNKNOWN &&
            skyengine_mr_key_from_sdl_key((int32_t)code) == MR_KEY_NONE) {
            printf("key:%d\n", code);
        }
        return 0;
    }
    event(MR_KEY_PRESS, mr_key, 0);
    return 1;
}

static int dispatch_key_up(SDL_Keycode code) {
    int32_t mr_key;
    if (!skyengine_key_latch_release(&keyLatch, (int32_t)code, &mr_key)) return 0;
    event(MR_KEY_RELEASE, mr_key, 0);
    return 1;
}

static void complete_e2e_key_event(const SDL_KeyboardEvent *key, int delivered) {
    e2e_publish_runtime_exit();
    e2e_control_key_event_completed(
        e2eControl, key->type, key->keysym.sym,
        key->windowID, (uint32_t)key->keysym.scancode, delivered);
}

static void dispatch_e2e_key_up(int after_timer) {
    int32_t raw_code;
    uint32_t token;
    if (!e2e_control_take_key_up(
            e2eControl, after_timer, &raw_code, &token)) return;

    SDL_Keycode code = (SDL_Keycode)raw_code;
    int delivered;
    if (isEditMode) {
        /* 与 edit-mode SDL_KEYUP 分支一致：编辑器拥有输入，只清宿主按键锁。 */
        delivered = skyengine_key_latch_clear(&keyLatch, (int32_t)code);
    } else {
        delivered = dispatch_key_up(code);
    }
    e2e_publish_runtime_exit();
    e2e_control_key_event_completed(
        e2eControl, SDL_KEYUP, code,
        E2E_KEY_WINDOW_ID, token, delivered);
}

static void dispatch_mouse_down(int x, int y) {
    int lx, ly;
    if (!map_mouse(x, y, &lx, &ly)) return;
    uint32_t seq = ++clickSeq;
    printf("[CLICK] #%u down x=%d y=%d\n", seq, x, y);
    isMouseDown = true;
    int32_t ret = event(MR_MOUSE_DOWN, lx, ly);
    printf("[CLICK] #%u down ret=%d\n", seq, ret);
}

static void dispatch_mouse_up(int x, int y) {
    int lx, ly;
    uint32_t seq = clickSeq;
    if (!map_mouse(x, y, &lx, &ly)) {
        isMouseDown = false;
        return;
    }
    printf("[CLICK] #%u up x=%d y=%d\n", seq, x, y);
    isMouseDown = false;
    int32_t ret = event(MR_MOUSE_UP, lx, ly);
    printf("[CLICK] #%u up ret=%d\n", seq, ret);
}

/*
 * 自动点击注入：通过环境变量 VMRP_AUTO_CLICKS 触发一连串模拟点击，便于在没有
 * 真实交互的情况下复现 UI 路径上的 Bug。格式为 "x1,y1;x2,y2;..."，每个点击之间
 * 间隔 VMRP_AUTO_CLICK_DELAY_MS 毫秒（默认 800ms）。
 *
 * 在专用线程中调用 SDL_PushEvent，让事件像真实输入那样进入主循环。
 */
typedef struct {
    int x;
    int y;
    int delay_ms; /* 本次点击后等待的毫秒数，-1 表示使用全局默认值 */
} AutoClickPoint;

static AutoClickPoint *autoClickList = NULL;
static int autoClickCount = 0;

static int autoClickThread(void *data) {
    (void)data;
    const char *delay_env = getenv("SKYENGINE_AUTO_CLICK_DELAY_MS");
    Uint32 default_delay = delay_env ? (Uint32)atoi(delay_env) : 800;
    if (default_delay == 0) default_delay = 800;

    /* 先等一段时间让应用完成启动 */
    SDL_Delay(default_delay);

    for (int i = 0; i < autoClickCount; ++i) {
        /* 本次点击后的延迟：优先使用自定义值，否则用全局默认值 */
        Uint32 cur_delay = (autoClickList[i].delay_ms >= 0)
                           ? (Uint32)autoClickList[i].delay_ms : default_delay;
        /* 约定：x<0 表示发送一次按键（用 y 不解释）。-1=ESC, -2=否/SOFTRIGHT,
         * -3=是/SOFTLEFT, -4=SELECT/确认 */
        if (autoClickList[i].x < 0) {
            SDL_Keycode kc = SDLK_ESCAPE;
            if (autoClickList[i].x == -2) kc = SDLK_MINUS;      /* 否 */
            else if (autoClickList[i].x == -3) kc = SDLK_EQUALS; /* 是 */
            else if (autoClickList[i].x == -4) kc = SDLK_RETURN; /* 确认 */
            SDL_Event ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = SDL_KEYDOWN;
            ev.key.type = SDL_KEYDOWN;
            ev.key.state = SDL_PRESSED;
            ev.key.keysym.sym = kc;
            SDL_PushEvent(&ev);
            SDL_Delay(50);
            memset(&ev, 0, sizeof(ev));
            ev.type = SDL_KEYUP;
            ev.key.type = SDL_KEYUP;
            ev.key.state = SDL_RELEASED;
            ev.key.keysym.sym = kc;
            SDL_PushEvent(&ev);
            SDL_Delay(cur_delay);
            continue;
        }
        SDL_Event ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = SDL_MOUSEBUTTONDOWN;
        ev.button.type = SDL_MOUSEBUTTONDOWN;
        ev.button.button = SDL_BUTTON_LEFT;
        ev.button.state = SDL_PRESSED;
        ev.button.x = autoClickList[i].x;
        ev.button.y = autoClickList[i].y;
        SDL_PushEvent(&ev);

        SDL_Delay(500);

        memset(&ev, 0, sizeof(ev));
        ev.type = SDL_MOUSEBUTTONUP;
        ev.button.type = SDL_MOUSEBUTTONUP;
        ev.button.button = SDL_BUTTON_LEFT;
        ev.button.state = SDL_RELEASED;
        ev.button.x = autoClickList[i].x;
        ev.button.y = autoClickList[i].y;
        SDL_PushEvent(&ev);

        SDL_Delay(cur_delay);
    }
    return 0;
}

static void startAutoClicksIfRequested(void) {
    const char *env = getenv("SKYENGINE_AUTO_CLICKS");
    if (!env || !*env) return;
    /* 解析 "x1,y1[,delay1];x2,y2[,delay2];..." 第三个字段可选，单位 ms */
    int capacity = 8;
    autoClickList = (AutoClickPoint *)malloc(sizeof(AutoClickPoint) * capacity);
    autoClickCount = 0;
    const char *p = env;
    while (*p) {
        int x = 0, y = 0, d = -1;
        int n = sscanf(p, "%d,%d,%d", &x, &y, &d);
        if (n >= 2) {
            if (autoClickCount >= capacity) {
                capacity *= 2;
                autoClickList = (AutoClickPoint *)realloc(autoClickList, sizeof(AutoClickPoint) * capacity);
            }
            autoClickList[autoClickCount].x = x;
            autoClickList[autoClickCount].y = y;
            autoClickList[autoClickCount].delay_ms = (n >= 3) ? d : -1;
            autoClickCount++;
        }
        const char *semi = strchr(p, ';');
        if (!semi) break;
        p = semi + 1;
    }
    if (autoClickCount > 0) {
        printf("[AUTO_CLICK] scheduled %d clicks\n", autoClickCount);
        SDL_CreateThread(autoClickThread, "auto-click", NULL);
    }
}

void loop(void) {
    SDL_Event ev;
    bool isLoop = true;

    e2e_control_start_if_requested(e2eControl);
    startAutoClicksIfRequested();

#if defined(__EMSCRIPTEN__)
#else
    while (isLoop)
#endif
    {
#if defined(__EMSCRIPTEN__)
        while (SDL_PollEvent(&ev))
#else
        /* mr_menuShow 只显示平台菜单并立即返回；主循环继续处理 SDL/E2E/
         * timer，平台输入由 skyengine_runtime_event 的公共漏斗接管。 */
        while (SDL_WaitEvent(&ev))
#endif
        {
            if (skyengine_is_exited()) {
                isLoop = false;
                break;
            }
            if (ev.type == SDL_QUIT) {
                isLoop = false;
                // emscripten_cancel_main_loop();
                break;
            }
            if (ev.type == e2eEventType) {
                e2e_control_execute(e2eControl, &ev);
                continue;
            }
            if (ev.type == timerEventType) {
                uint32_t generation = (uint32_t)(uintptr_t)ev.user.data1;
                /* 真机 mr_timer 是单实例：timerStop/timerStart 之后，旧一代
                 * 定时器已不存在，它先前入队的到期事件不代表任何活动定时器，
                 * 不能再触发 guest tick。若照常分发，guest 在单次 tick 内多次
                 * stop/start（如 optwar 广告页 10ms 动画节拍）会让入队速度超过
                 * 分发速度，SDL 事件队列积压成百上千个陈旧 timer 事件，注入的
                 * 按键事件排在其后被拖延十几秒。仅当该事件仍是当前 pending 的
                 * 一代时才分发。 */
                if ((uint32_t)SDL_AtomicGet(&timerPendingGeneration) !=
                    generation) {
                    continue;
                }
                /* The SDL timer is one-shot and timer() rearms the guest's
                 * next tick.  Process it even while the platform editor owns
                 * keyboard input; dropping it there stops the guest scheduler
                 * permanently after a normal pause before Ctrl+V. */
                /* guest timer() 可能在一次调用内部先 stop 再 start；对控制线程
                 * 标记整个调用，避免把中途 pending=0 当成真正停止。 */
                SDL_AtomicSet(&timerDispatchInProgress, 1);
                timer();
                /*
                 * timer() 内可能已 arm 下一代。先发布本代完成，再仅在 pending
                 * 仍指向本代时清零，控制线程便不会看到 dispatch 中途的假停止。
                 * SDL 事件按入队顺序处理，generation 因而单调递增。
                */
                e2e_publish_timer_dispatch(generation);
                SDL_AtomicCAS(&timerPendingGeneration, (int)generation, 0);
                e2e_publish_runtime_exit();
                /* runtimeExited 必须先于 in-progress 清除发布，避免控制线程在
                 * 正常退出窗口中继续注入一个永远不会被主循环确认的按键。 */
                SDL_AtomicSet(&timerDispatchInProgress, 0);
                /* 默认 E2E 短按在这一拍结束时立即 release，不依赖控制线程调度。 */
                dispatch_e2e_key_up(1);
                if (skyengine_is_exited()) {
                    isLoop = false;
                    break;
                }
                continue;
            }
            if (isEditMode) {
#ifndef __EMSCRIPTEN__
                if (edit_handle_event(&ev)) {
                    if (edit_confirm) {
                        edit_confirm = false;
                        saveEditText(edit_buf);
                        event(MR_DIALOG_EVENT, 0, 0);
                    } else if (edit_cancel) {
                        edit_cancel = false;
                        event(MR_DIALOG_EVENT, 1, 0);
                    }
                    continue;
                }
#endif
#if defined(__ANDROID__)
                if (ev.type == VMRP_SDL_ANDROID_EDIT_EVENT) {
                    /* Java showEditDialog 完成时把结果回投到主线程 */
                    char *text = (char *)ev.user.data1;
                    if (ev.user.code) {
                        saveEditText(text ? text : "");
                        event(MR_DIALOG_EVENT, 0, 0);
                    } else {
                        event(MR_DIALOG_EVENT, 1, 0);
                    }
                    free(text);
                    continue;
                }
#endif
                switch (ev.type) {
                    case SDL_KEYUP: {
                        /* A key can open the editor from its KEYDOWN handler.
                         * Its matching KEYUP then arrives while edit mode owns
                         * input, so consume it without sending a Mythroad key
                         * release but clear the host key latch.  Otherwise the
                         * next physical keydown is rejected as a duplicate. */
                        SDL_Keycode code = keyboard_event_keycode(&ev.key);
                        int delivered = skyengine_key_latch_clear(
                            &keyLatch, (int32_t)code);
                        /* 编辑模式也完成了 release；按 token 通知对应的 E2E 命令。 */
                        complete_e2e_key_event(&ev.key, delivered);
                        continue;
                    }
                    case SDL_KEYDOWN: {
                        SDL_Keymod key_mod = (SDL_Keymod)(ev.key.keysym.mod | SDL_GetModState());
                        /* SDL_KEYDOWN carries the modifier state observed with
                         * the key event; use it so injected and physical Ctrl+V
                         * follow the same edit commit path. */
                        if (key_mod & KMOD_CTRL) {
                            if (ev.key.keysym.sym == SDLK_z) {  // 取消编辑框输入
                                // MR_DIALOG_KEY_CANCEL=1
                                event(MR_DIALOG_EVENT, 1, 0);
                                SDL_Log("取消输入");
                                complete_e2e_key_event(&ev.key, 1);
                                dispatch_e2e_key_up(0);
                                continue;
                            } else if (ev.key.keysym.sym == SDLK_v) {  // 编辑框输入
                                char *str = SDL_GetClipboardText();
                                saveEditText(str);
                                SDL_free(str);
                                // MR_DIALOG_KEY_OK=0
                                event(MR_DIALOG_EVENT, 0, 0);
                                complete_e2e_key_event(&ev.key, 1);
                                dispatch_e2e_key_up(0);
                                continue;
                            }
                        }
                    }
                    /* 非 Ctrl+V/Z 的按键与鼠标点击一样,只提示编辑操作方式 */
                    /* fall through */
                    case SDL_MOUSEBUTTONDOWN:
                        SDL_Log("ctrl+v输入内容，ctrl+z取消输入");
                }
                if (ev.type == SDL_KEYDOWN) {
                    /* 非编辑快捷键也已由 edit-mode 分支完整消费。 */
                    complete_e2e_key_event(&ev.key, 1);
                    dispatch_e2e_key_up(0);
                }
                continue;
            }
            switch (ev.type) {
                case SDL_KEYDOWN: {
                    SDL_Keycode code = keyboard_event_keycode(&ev.key);
                    complete_e2e_key_event(
                        &ev.key, dispatch_key_down(code, ev.key.repeat));
                    /* A guest with no pending timer still needs a deterministic
                     * short-key release at this same main-thread boundary. */
                    dispatch_e2e_key_up(0);
                    break;
                }
                case SDL_KEYUP: {
                    SDL_Keycode code = keyboard_event_keycode(&ev.key);
                    int delivered = dispatch_key_up(code);
                    /* dispatch 返回表示 guest release 回调已完成，可等待下一 timer epoch。 */
                    complete_e2e_key_event(&ev.key, delivered);
                    break;
                }
                case SDL_MOUSEMOTION:
                    if (isMouseDown) {
                        int lx, ly;
                        if (map_mouse(ev.motion.x, ev.motion.y, &lx, &ly)) {
                            event(MR_MOUSE_MOVE, lx, ly);
                        }
                    }
                    break;
                case SDL_MOUSEBUTTONDOWN:
                    dispatch_mouse_down(ev.button.x, ev.button.y);
                    break;
                case SDL_MOUSEBUTTONUP:
                    dispatch_mouse_up(ev.button.x, ev.button.y);
                    break;
#if defined(__ANDROID__)
                case VMRP_SDL_ANDROID_KEY_EVENT:
                    /* Java 虚拟按键：type/密钥 直接作为 MR_KEY_PRESS/RELEASE */
                    event((int32_t)ev.user.code, (int32_t)(intptr_t)ev.user.data1, 0);
                    break;
                case VMRP_SDL_ANDROID_EDIT_EVENT:
                    /* 非编辑目录下的残留编辑事件（理论上不会到这儿），仅释放内存 */
                    free(ev.user.data1);
                    break;
                case SDL_WINDOWEVENT:
                    if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                        ev.window.event == SDL_WINDOWEVENT_RESIZED) {
                        update_game_dst();
                    }
                    break;
#endif
            }
            if (skyengine_is_exited()) {
                isLoop = false;
                break;
            }
        }
    }
}

#ifdef _MSC_VER
static void abort_handler(int sig) {
    (void)sig;
    fflush(stdout);
    fprintf(stderr, "[CRASH] SIGABRT received - abort() was called\n");
    fflush(stderr);
    signal(SIGABRT, SIG_DFL);
    raise(SIGABRT);
}
static LONG WINAPI win_exception_filter(EXCEPTION_POINTERS *ep) {
    const EXCEPTION_RECORD *record = ep->ExceptionRecord;
    const CONTEXT *context = ep->ContextRecord;
    const void *image_base = (const void *)GetModuleHandleW(NULL);
    fprintf(stderr, "[CRASH] Unhandled exception: code=0x%08lX addr=%p\n",
        record->ExceptionCode, record->ExceptionAddress);
    /* ASLR makes a raw PC insufficient for matching optimized Windows builds to
     * dumpbin/PDB output. Keep the module-relative address and failed access in
     * the crash log so Release-only faults remain reproducible without a JIT debugger. */
    fprintf(stderr, "[CRASH] image_base=%p rva=0x%llX\n", image_base,
            (unsigned long long)((uintptr_t)record->ExceptionAddress -
                                 (uintptr_t)image_base));
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        record->NumberParameters >= 2) {
        const char *operation = record->ExceptionInformation[0] == 0 ? "read" :
                                record->ExceptionInformation[0] == 1 ? "write" :
                                record->ExceptionInformation[0] == 8 ? "execute" :
                                "unknown";
        fprintf(stderr, "[CRASH] access=%s target=%p\n", operation,
                (void *)record->ExceptionInformation[1]);
    }
#if defined(_M_X64)
    fprintf(stderr,
            "[CRASH] rip=%016llX rsp=%016llX rbp=%016llX rbx=%016llX\n"
            "[CRASH] rcx=%016llX rdx=%016llX rsi=%016llX rdi=%016llX\n"
            "[CRASH] r8 =%016llX r9 =%016llX r10=%016llX r11=%016llX\n"
            "[CRASH] r12=%016llX r13=%016llX r14=%016llX r15=%016llX\n",
            context->Rip, context->Rsp, context->Rbp, context->Rbx,
            context->Rcx, context->Rdx, context->Rsi, context->Rdi,
            context->R8, context->R9, context->R10, context->R11,
            context->R12, context->R13, context->R14, context->R15);
#endif
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}
static void invalid_param_handler(const wchar_t *expr, const wchar_t *func,
                                   const wchar_t *file, unsigned int line,
                                   uintptr_t p) {
    (void)p;
    fflush(stdout);
    fprintf(stderr, "[CRASH] Invalid CRT parameter: expr=%ls func=%ls file=%ls line=%u\n",
            expr ? expr : L"(null)",
            func ? func : L"(null)",
            file ? file : L"(null)",
            line);
    fflush(stderr);
    abort();
}
#endif

int main(int argc, char *args[]) {
#ifdef _MSC_VER
    signal(SIGABRT, abort_handler);
    SetUnhandledExceptionFilter(win_exception_filter);
    _set_invalid_parameter_handler(invalid_param_handler);
#endif
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
    signal(SIGUSR1, sigusr1_handler);
#endif
    if (argc > 1 && (strcmp(args[1], "-h") == 0 || strcmp(args[1], "--help") == 0)) {
        skyengine_args_print_usage(args[0]);
        return 0;
    }
    SkyEngineArgs skyengine_args;
    if (skyengine_args_parse(argc, args, &skyengine_args) != MR_SUCCESS) {
        return -1;
    }

#ifdef __x86_64__
    printf("__x86_64__\n");
#elif __i386__
    printf("__i386__\n");
#endif

#if defined(__ANDROID__)
    android_host_init();
#endif

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_AUDIO) < 0) {
        printf("SDL could not initialize! SDL_Error: %s\n", SDL_GetError());
        return -1;
    }
    timerEventType = SDL_RegisterEvents(1);
    e2eEventType = SDL_RegisterEvents(1);
    if (e2eEventType == (Uint32)-1) {
        printf("SDL_RegisterEvents for E2E failed: %s\n", SDL_GetError());
        SDL_Quit();
        return -1;
    }
#ifndef _WIN32
    /* SDL_Init 会重置 SIGUSR1 为默认处理（终止进程），导致截屏信号反而杀掉
     * 进程。此处在 SDL_Init 之后重新安装 SIGUSR1 截屏处理，确保 PPM 转储可用。 */
    signal(SIGUSR1, sigusr1_handler);
#endif

    /* SDL 窗口必须按 --screen/环境变量解析出的分辨率创建。此前窗口用
     * skyengine_config 的编译期默认值(240x320),而 --screen 要到后面的
     * startVmrp() 才写入 skyengine_config,导致任何非默认分辨率都只显示
     * 240x320 窗口(gtcm --screen 480x320 只能看到左上裁切)。这里提前
     * 同步一次;startVmrp() 内部的赋值保持不变(共享库入口依赖它)。 */
    skyengine_config.screen_width = skyengine_args.screen_width;
    skyengine_config.screen_height = skyengine_args.screen_height;

#if defined(__ANDROID__)
    /* vmrp.cfg 覆盖 CLI 分辨率，写 skyengine_config.screen_*。放在上面同步
     * 之后、建窗之前，否则会被 CLI 值覆盖回 240x320。 */
    android_load_config("vmrp.cfg");
#endif

    /* guiDrawBitmap writes to SDL_GetWindowSurface(); avoiding an OpenGL window
     * lets the E2E pixel tests run under SDL's dummy video driver in CI. */
#if defined(__ANDROID__)
    window = SDL_CreateWindow("skyengine", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                              0, 0, SDL_WINDOW_FULLSCREEN | SDL_WINDOW_SHOWN);
#else
    window = SDL_CreateWindow("skyengine", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, skyengine_config.screen_width, skyengine_config.screen_height, 0);
#endif
    if (window == NULL) {
        printf("Window could not be created! SDL_Error: %s\n", SDL_GetError());
        SDL_Quit();
        return -1;
    }
#if defined(__ANDROID__)
    update_game_dst();
#endif
    VmrpE2eHooks e2e_hooks;
    memset(&e2e_hooks, 0, sizeof(e2e_hooks));
    SDL_AtomicSet(&timerArmGeneration, 0);
    SDL_AtomicSet(&timerDispatchedGeneration, 0);
    SDL_AtomicSet(&timerPendingGeneration, 0);
    SDL_AtomicSet(&timerDispatchInProgress, 0);
    SDL_AtomicSet(&runtimeExited, 0);
    e2e_hooks.dump_screen_ppm = e2e_dump_screen_ppm_hook;
    e2e_hooks.dump_draw_frame_ppm = e2e_dump_draw_frame_ppm_hook;
    e2e_hooks.screen_dump_path = e2e_screen_dump_path_hook;
    e2e_hooks.draw_count = e2e_draw_count_hook;
    e2e_hooks.timer_arm_generation = e2e_timer_arm_generation_hook;
    e2e_hooks.timer_dispatched_generation = e2e_timer_dispatched_generation_hook;
    e2e_hooks.timer_pending_generation = e2e_timer_pending_generation_hook;
    e2e_hooks.timer_dispatch_in_progress = e2e_timer_dispatch_in_progress_hook;
    e2e_hooks.runtime_exited = e2e_runtime_exited_hook;
    e2e_hooks.motion_input = e2e_motion_input_hook;
    e2eControl = e2e_control_create(e2eEventType, &e2e_hooks);

    if (startEngine(&skyengine_args) != MR_SUCCESS) {
        e2e_control_destroy(e2eControl);
        e2eControl = NULL;
        SDL_DestroyWindow(window);
        SDL_Quit();
        return -1;
    }
    if (skyengine_is_exited()) {
        stopEngine();
        e2e_control_destroy(e2eControl);
        e2eControl = NULL;
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 0;
    }

#if defined(__EMSCRIPTEN__)
    emscripten_set_main_loop(loop, 0, 1);
#else
    loop();
#endif
    e2e_control_destroy(e2eControl);
    e2eControl = NULL;
    stopEngine();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
