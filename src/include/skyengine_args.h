#ifndef __VMRP_ARGS_H__
#define __VMRP_ARGS_H__

#include <limits.h>
#include <stddef.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define DEFAULT_SCREEN_WIDTH 240
#define DEFAULT_SCREEN_HEIGHT 320
#define DEFAULT_MEMORY_MB 1
/* Archived MRP software expects a handset-era RTC unless the caller opts
 * into the host wall clock with --device-date host. */
#define DEFAULT_DEVICE_YEAR 2011
#define DEFAULT_DEVICE_MONTH 1
#define DEFAULT_DEVICE_DAY 1
#define VMRP_MRP_NAME_LIMIT 128
#define SKYENGINE_DNS_MAP_LIMIT 2048

/* CLI 或环境变量已经写过对应字段。桌面 UI 配置文件不得覆盖这些来源,
 * 否则 e2e/`--screen` 会被上次 GUI 保存的分辨率悄悄改掉。 */
#define SKYENGINE_SRC_SCREEN   (1u << 0)
#define SKYENGINE_SRC_MEMORY   (1u << 1)
#define SKYENGINE_SRC_DATE     (1u << 2)
#define SKYENGINE_SRC_WORKDIR  (1u << 3)
#define SKYENGINE_SRC_DNS      (1u << 4)
#define SKYENGINE_SRC_MRP      (1u << 5)
#define SKYENGINE_SRC_EXT      (1u << 6)
#define SKYENGINE_SRC_ENTRY    (1u << 7)
#define SKYENGINE_SRC_SF2      (1u << 8)
#define SKYENGINE_SRC_PROFILE  (1u << 9)

/* 0=速度优先(默认,接近 old fork 的稀钩子);1=兼容优先(宽 R9/GOT/屏写钩)。 */
#define SKYENGINE_PROFILE_SPEED 0
#define SKYENGINE_PROFILE_COMPAT 1

typedef struct SkyEngineArgs {
    int screen_width;
    int screen_height;
    int memory_mb; /* 应用可见内存(MB):1/2/4/6/8/16 */
    int compat_priority; /* SKYENGINE_PROFILE_SPEED/COMPAT */
    int device_year;  /* 0 表示直接使用宿主日期 */
    int device_month;
    int device_day;
    char work_dir[PATH_MAX];
    char mrp_path[PATH_MAX];
    char ext_name[256];
    char entry[256];
    char dns_map[SKYENGINE_DNS_MAP_LIMIT];
    /* 非空时使用 TinySoundFont 渲染 MIDI(SF2 音色库路径)；
     * 为空则回退到内置波形合成。仅桌面端经 --sf2/环境变量注入。 */
    char sf2_path[PATH_MAX];
    unsigned sourced;
} SkyEngineArgs;

SkyEngineArgs skyengine_args_default(void);
/* Shared calendar validation for CLI and embedding API configuration. */
int skyengine_args_parse_device_date(const char *str, int *year, int *month, int *day);
int skyengine_args_parse_screen(const char *str, int *w, int *h);
int skyengine_args_parse_memory(const char *str, int *mb);
int skyengine_args_parse_profile(const char *str, int *out);
void skyengine_args_format_device_date(const SkyEngineArgs *args, char *out, size_t out_sz);
int skyengine_args_resolve_mrp_path(const char *input, char *output, size_t output_size);
int skyengine_args_resolve_dir(const char *input, char *output, size_t output_size);
int skyengine_args_parse(int argc, char *argv[], SkyEngineArgs *out);
void skyengine_args_print_usage(const char *program);

#endif
