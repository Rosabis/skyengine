#include "./include/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#define getcwd _getcwd
#else
#include <unistd.h>
#endif

#define LOG_FILE "log.txt"

static FILE *g_log = NULL;

int skyengine_log_active(void) {
    return g_log != NULL;
}

void skyengine_log_msg(const char *fmt, ...) {
    va_list ap;
    if (fmt == NULL) return;
    /* stderr 已被 skyengine_log_init 重定向到 log.txt,直接写 stderr 即落盘,
     * 同时保留一份 stdout 出口由调用方决定。 */
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);
}

void skyengine_log_init(void) {
    FILE *f;
    time_t now;
    struct tm *tm;
    char cwd[512];

    if (g_log != NULL) {
        return;
    }
    f = fopen(LOG_FILE, "w");
    if (f == NULL) {
        return;
    }
    fprintf(f, "=== skyengine log ===\n");
    now = time(NULL);
    tm = localtime(&now);
    if (tm != NULL) {
        fprintf(f, "time : %04d-%02d-%02d %02d:%02d:%02d\n",
                tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                tm->tm_hour, tm->tm_min, tm->tm_sec);
    }
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        fprintf(f, "cwd  : %s\n", cwd);
    }
    fflush(f);
    fclose(f);

    /* 打开后再把 stderr 重定向到同一文件(追加),纳入引擎/DSM/SDL 的诊断输出。 */
    if (freopen(LOG_FILE, "a", stderr) == NULL) {
        return;
    }
    g_log = stderr;
}

void skyengine_log_shutdown(void) {
    FILE *f = g_log;
    g_log = NULL;
    if (f != NULL) {
        fflush(f);
    }
}