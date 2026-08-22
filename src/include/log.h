#ifndef __VMRP_LOG_H__
#define __VMRP_LOG_H__

/*
 * 桌面版运行时日志:启动时在工作目录生成 log.txt。
 *
 * 设计:
 *  - skyengine_log_init() 打开/截断 log.txt,写入带时间戳的横幅,并把 stderr
 *    重定向到该文件(追加模式)。引擎/DSM/SDL 所有写 stderr 的诊断
 *    (如 [_mr_intra_start] mrp_open FAILED)都会落入 log.txt,stdout 仍保留在
 *    终端,便于同时观察控制台进度。
 *  - 只在桌面(SDL 可执行文件)启用;Android 走 logcat、Emscripten 无落盘不调用。
 */

/* 打开 log.txt 并重定向 stderr。失败不致命,静默返回。 */
void skyengine_log_init(void);

/* 冲刷并关闭日志文件。可用于 atexit 或显式收尾。 */
void skyengine_log_shutdown(void);

/* 日志是否已初始化(stderr 已重定向到 log.txt)。 */
int skyengine_log_active(void);

/* 写一条结构化日志:落盘 stderr(即 log.txt)。由应用生命周期主动调用,
 * 不依赖 SKYENGINE_LOG 环境变量。 */
void skyengine_log_msg(const char *fmt, ...);

#endif /* __VMRP_LOG_H__ */