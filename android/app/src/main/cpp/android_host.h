#ifndef VMRP_ANDROID_HOST_H
#define VMRP_ANDROID_HOST_H

#include <SDL.h>
#include <stdint.h>

/* Java 编辑框完成 / 虚拟按键，投递到 SDL 主线程 */
#define VMRP_SDL_ANDROID_EDIT_EVENT ((Uint32)(SDL_USEREVENT + 2))
#define VMRP_SDL_ANDROID_KEY_EVENT ((Uint32)(SDL_USEREVENT + 3))

void android_host_init(void);
void android_load_config(const char *path);
void android_set_keypad_height(int height_px);
int android_keypad_height(void);
void android_compute_layout(int winW, int winH, int screenW, int screenH, SDL_Rect *gameDst);
int android_map_pointer(int px, int py, const SDL_Rect *gameDst, int rot, int guest_rot,
                        int screenW, int screenH, int *lx, int *ly);
void android_start_edit(const char *titleUtf8, const char *textUtf8, int maxSize);
void android_stop_edit(void);

#endif
