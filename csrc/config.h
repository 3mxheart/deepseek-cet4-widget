/* DeepSeek 余额 + 四级倒计时 悬浮窗（原生 C 版）—— 配置 / 日志 / 网络 / 时间
 *
 * 目标是编译成一个不依赖 Python 的小 exe：
 *   config.h / config.c   配置读写（沿用同一份 config.json）、日志、WinHTTP 查余额、倒计时
 *   ui.c                  GDI+ 逐像素透明渲染 + 窗口 / 菜单 / 交互
 */
#ifndef CW_CONFIG_H
#define CW_CONFIG_H

#include <windows.h>
#include <time.h>

#define CW_MAX_EXAMS 8
#define CW_APP_NAME  L"DeepSeekWidget"
/* 注意：不要用 Global\ 前缀 —— 普通用户没有创建全局对象的权限，
   注册失败就失去了防重复启动的作用。会话内互斥量足够。 */
#define CW_MUTEX     L"DeepSeekWidgetMutex_c49731"
#define CW_RUN_KEY   L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define CW_RUN_VALUE L"DeepSeekWidget"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char       name[32];      /* "CET-4" */
    char       datetime[32];  /* "2026-12-12 09:00" */
    time_t     t;             /* 本地时间戳 */
} Exam;

typedef struct {
    char   api_key[512];
    char   api_base[256];
    int    use_proxy;
    int    refresh_seconds;
    int    show_delta;
    int    delta_hold_seconds;
    int    show_status_line;
    int    show_balance_detail;

    char   bg_image[1024];

    int    w, h;
    int    x, y;
    int    has_pos;
    double opacity;
    int    rounded;
    int    topmost;
    int    lock_position;

    int    scrim_alpha;
    char   font_family[64];
    int    value_size;
    int    label_size;
    int    detail_size;
    char   label_color[16];
    char   value_color[16];
    char   detail_color[16];
    char   accent_color[16];

    Exam   exams[CW_MAX_EXAMS];
    int    exam_count;
} Config;

extern Config  g_cfg;
extern wchar_t g_dirw[MAX_PATH];   /* exe 所在目录（宽字符） */
extern char    g_dir[MAX_PATH * 2];/* 同上，UTF-8 形式，用于日志 */

/* 基础工具 */
char    *cw_u8_from_w(const wchar_t *w);          /* 需 free() */
wchar_t *cw_w_from_u8(const char *s);             /* 需 free() */
void     cw_dir_of_exe(void);
void     cw_full_path(wchar_t *out, size_t cap, const wchar_t *file);
void     cw_log(const char *fmt, ...);
void     cw_hex_to_rgb(const char *hex, BYTE *r, BYTE *g, BYTE *b, BYTE def_r, BYTE def_g, BYTE def_b);

/* 配置 */
void     cw_load_config(void);
void     cw_save_config(void);

/* 极简 JSON 取值（只针对本程序自己的 config.json，够用不炫技） */
const char *cw_jfind(const char *json, const char *key);
int      cw_jstr(const char *json, const char *key, char *out, int cap);
int      cw_jint(const char *json, const char *key, int def);
double   cw_jdbl(const char *json, const char *key, double def);
int      cw_jbool(const char *json, const char *key, int def);
void     cw_jesc(char *out, int cap, const char *in);   /* 转义成 JSON 字符串内容 */

/* 余额查询：成功返回 1，失败返回 0 并填 err */
int      cw_fetch_balance(const char *key, const char *base, int use_proxy,
                          char *currency, int cap_cur,
                          char *total, int cap_total,
                          char *granted, int cap_granted,
                          char *topped, int cap_topped,
                          char *err, int cap_err);

/* 倒计时：返回 (大字, 小字) */
void     cw_countdown(char *value, int cap_v, char *detail, int cap_d);

/* 自启动开关（和 Python 版共用同一条注册表项） */
int      cw_autostart_get(wchar_t *out, int cap);
int      cw_autostart_set(int enable);

#ifdef __cplusplus
}
#endif

#endif /* CW_CONFIG_H */
