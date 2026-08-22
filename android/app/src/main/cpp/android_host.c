#include "android_host.h"

#include <jni.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>

#include <SDL_system.h>

#include "skyengine.h"
#include "skyengine_args.h"

static int g_keypad_h = 0;

static char g_storage_dir[1024];

void android_host_init(void) {
    /* 运行时固件/配置(vmrp.cfg、mythroad/…)都被 Java 侧解压到 app 内部
     * files 目录,而 Android 原生进程的 cwd 是系统根目录。这里把 cwd 切到
     * 内部存储,之后对 "vmrp.cfg"、"mythroad/dsm_gm.mrp" 等相对路径的访问
     * 才能命中解压产物;否则 apply_config_paths 默认切换到 "."(=/)导致
     * 目标文件找不到,startEngine 直接失败、Activity 启动即闪退。 */
    const char *storage = SDL_AndroidGetInternalStoragePath();
    if (storage && storage[0]) {
        snprintf(g_storage_dir, sizeof(g_storage_dir), "%s", storage);
        if (chdir(g_storage_dir) != 0) {
            fprintf(stderr, "[android_host] chdir('%s') failed: %s\n", g_storage_dir, strerror(errno));
        }
    } else {
        fprintf(stderr, "[android_host] SDL_AndroidGetInternalStoragePath() unavailable\n");
    }
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "Portrait");
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "1");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
}

void android_set_keypad_height(int height_px) {
    g_keypad_h = height_px > 0 ? height_px : 0;
}

int android_keypad_height(void) {
    return g_keypad_h;
}

void android_load_config(const char *path) {
    char line[256];
    FILE *fp = fopen(path ? path : "vmrp.cfg", "r");
    if (fp == NULL) {
        return;
    }
    while (fgets(line, sizeof(line), fp)) {
        char *nl = strchr(line, '\n');
        char *cr = strchr(line, '\r');
        char *eq;
        char *key;
        char *val;
        if (nl) {
            *nl = '\0';
        }
        if (cr) {
            *cr = '\0';
        }
        if (line[0] == '#' || line[0] == '\0') {
            continue;
        }
        eq = strchr(line, '=');
        if (eq == NULL) {
            continue;
        }
        *eq = '\0';
        key = line;
        val = eq + 1;
        if (strcmp(key, "width") == 0) {
            int w = atoi(val);
            if (w > 0) {
                skyengine_config.screen_width = w;
            }
        } else if (strcmp(key, "height") == 0) {
            int h = atoi(val);
            if (h > 0) {
                skyengine_config.screen_height = h;
            }
        } else if (strcmp(key, "firmware") == 0 ||
                   strcmp(key, "imei") == 0 ||
                   strcmp(key, "imsi") == 0 ||
                   strcmp(key, "manuf") == 0 ||
                   strcmp(key, "model") == 0) {
            /* 新引擎(重构后 src/)不再提供 vmrp_set_firmware/imei/imsi/
             * manufacturer/model 这些 setter —— 设备信息改由 DSM 侧自管或
             * 经 SkyEngineArgs 传入,旧的 vmrp.cfg 里这些键不再生效。这里
             * 仅读入并告警,避免行为不一致;如需再引入请在 src/include 增加
             * 对应配置字段后再接入。 */
            fprintf(stderr, "[android_host] config key '%s' no longer supported in new API, ignored\n", key);
        } else if (strcmp(key, "sf2") == 0) {
            /* SF2 音色库路径:非空时原生 MIDI 用 TinySoundFont 渲染,为空则
             * 回退到内置波形合成。路径相对内部存储 filesDir,或为绝对路径。 */
            snprintf(skyengine_config.sf2_path, sizeof(skyengine_config.sf2_path), "%s", val);
        }
    }
    fclose(fp);
}

void android_compute_layout(int winW, int winH, int screenW, int screenH, SDL_Rect *gameDst) {
    int reserved = g_keypad_h;
    int availH;
    float sx;
    float sy;
    float s;
    if (gameDst == NULL || winW <= 0 || winH <= 0 || screenW <= 0 || screenH <= 0) {
        return;
    }
    if (reserved <= 0) {
        reserved = winH * 28 / 100;
    }
    if (reserved > winH * 38 / 100) {
        reserved = winH * 38 / 100;
    }
    availH = winH - reserved;
    if (availH < 64) {
        availH = 64;
    }
    sx = (float)winW / (float)screenW;
    sy = (float)availH / (float)screenH;
    s = sx < sy ? sx : sy;
    if (s < 0.25f) {
        s = 0.25f;
    }
    gameDst->w = (int)(screenW * s + 0.5f);
    gameDst->h = (int)(screenH * s + 0.5f);
    gameDst->x = (winW - gameDst->w) / 2;
    gameDst->y = (availH - gameDst->h) / 2;
    if (gameDst->y < 0) {
        gameDst->y = 0;
    }
}

int android_map_pointer(int px, int py, const SDL_Rect *gameDst, int rot, int guest_rot,
                        int screenW, int screenH, int *lx, int *ly) {
    int X;
    int Y;
    int tx;
    int ty;
    int W = screenW;
    int H = screenH;
    if (gameDst == NULL || lx == NULL || ly == NULL || gameDst->w <= 0 || gameDst->h <= 0) {
        return 0;
    }
    if (px < gameDst->x || py < gameDst->y ||
        px >= gameDst->x + gameDst->w || py >= gameDst->y + gameDst->h) {
        return 0;
    }
    X = (px - gameDst->x) * W / gameDst->w;
    Y = (py - gameDst->y) * H / gameDst->h;
    if (X < 0) {
        X = 0;
    }
    if (Y < 0) {
        Y = 0;
    }
    if (X >= W) {
        X = W - 1;
    }
    if (Y >= H) {
        Y = H - 1;
    }
    switch (rot) {
        case 1:
            tx = (W - 1) - Y;
            ty = X;
            break;
        case 2:
            tx = (W - 1) - X;
            ty = (H - 1) - Y;
            break;
        case 3:
            tx = Y;
            ty = (H - 1) - X;
            break;
        default:
            tx = X;
            ty = Y;
            break;
    }
    switch (guest_rot) {
        case 1:
            *lx = ty;
            *ly = (W - 1) - tx;
            break;
        case 2:
            *lx = (W - 1) - tx;
            *ly = (H - 1) - ty;
            break;
        case 3:
            *lx = (H - 1) - ty;
            *ly = tx;
            break;
        default:
            *lx = tx;
            *ly = ty;
            break;
    }
    return 1;
}

void android_start_edit(const char *titleUtf8, const char *textUtf8, int maxSize) {
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject activity = (jobject)SDL_AndroidGetActivity();
    jclass cls;
    jmethodID mid;
    jstring jtitle;
    jstring jtext;
    if (env == NULL || activity == NULL) {
        return;
    }
    cls = (*env)->GetObjectClass(env, activity);
    if (cls == NULL) {
        (*env)->DeleteLocalRef(env, activity);
        return;
    }
    mid = (*env)->GetMethodID(env, cls, "showEditDialog", "(Ljava/lang/String;Ljava/lang/String;I)V");
    if (mid == NULL) {
        (*env)->DeleteLocalRef(env, cls);
        (*env)->DeleteLocalRef(env, activity);
        return;
    }
    jtitle = (*env)->NewStringUTF(env, titleUtf8 ? titleUtf8 : "");
    jtext = (*env)->NewStringUTF(env, textUtf8 ? textUtf8 : "");
    (*env)->CallVoidMethod(env, activity, mid, jtitle, jtext, maxSize);
    (*env)->DeleteLocalRef(env, jtitle);
    (*env)->DeleteLocalRef(env, jtext);
    (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, activity);
}

void android_stop_edit(void) {
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject activity = (jobject)SDL_AndroidGetActivity();
    jclass cls;
    jmethodID mid;
    if (env == NULL || activity == NULL) {
        return;
    }
    cls = (*env)->GetObjectClass(env, activity);
    if (cls == NULL) {
        (*env)->DeleteLocalRef(env, activity);
        return;
    }
    mid = (*env)->GetMethodID(env, cls, "dismissEditDialog", "()V");
    if (mid) {
        (*env)->CallVoidMethod(env, activity, mid);
    }
    (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, activity);
}

static void push_user_event(Uint32 type, Sint32 code, void *data1, void *data2) {
    SDL_Event e;
    SDL_zero(e);
    e.type = type;
    e.user.code = code;
    e.user.data1 = data1;
    e.user.data2 = data2;
    SDL_PushEvent(&e);
}

JNIEXPORT void JNICALL
Java_io_github_vmrp_VmrpActivity_nativeSetKeypadHeight(JNIEnv *env, jclass cls, jint height) {
    (void)env;
    (void)cls;
    android_set_keypad_height((int)height);
}

JNIEXPORT void JNICALL
Java_io_github_vmrp_VmrpActivity_nativeKey(JNIEnv *env, jclass cls, jint type, jint mrKey) {
    (void)env;
    (void)cls;
    push_user_event(VMRP_SDL_ANDROID_KEY_EVENT, type, (void *)(intptr_t)mrKey, NULL);
}

/* 不编 SDL HIDAPI；若仍链到官方 HID Java，这两个空实现避免启动即崩。 */
JNIEXPORT void JNICALL
Java_org_libsdl_app_HIDDeviceManager_HIDDeviceRegisterCallback(JNIEnv *env, jobject thiz) {
    (void)env;
    (void)thiz;
}

JNIEXPORT void JNICALL
Java_org_libsdl_app_HIDDeviceManager_HIDDeviceReleaseCallback(JNIEnv *env, jobject thiz) {
    (void)env;
    (void)thiz;
}

JNIEXPORT void JNICALL
Java_io_github_vmrp_VmrpActivity_nativeEditDone(JNIEnv *env, jclass cls, jstring text, jboolean ok) {
    char *copy = NULL;
    (void)cls;
    if (text != NULL) {
        const char *utf8 = (*env)->GetStringUTFChars(env, text, NULL);
        if (utf8) {
            size_t n = strlen(utf8);
            copy = (char *)malloc(n + 1);
            if (copy) {
                memcpy(copy, utf8, n + 1);
            }
            (*env)->ReleaseStringUTFChars(env, text, utf8);
        }
    }
    push_user_event(VMRP_SDL_ANDROID_EDIT_EVENT, ok ? 1 : 0, copy, NULL);
}
