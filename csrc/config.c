/* 配置 / 日志 / WinHTTP 查余额 / 倒计时 / 自启动
 *
 * 配置沿用 Python 版那份 config.json（同一个 schema），两个版本可以互相读。
 */
#include "config.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <winhttp.h>
#include <wincrypt.h>

Config  g_cfg;
wchar_t g_dirw[MAX_PATH] = {0};
char    g_dir[MAX_PATH * 2] = {0};

#define DEF_API_BASE "https://api.deepseek.com"
#ifndef CW_SETTINGS_KEY
#define CW_SETTINGS_KEY L"Software\\DeepSeekWidget"
#endif

static int settings_read_failed = 0;

/* ---------------------------------------------------------------- 基础工具 */
char *cw_u8_from_w(const wchar_t *w)
{
    if (!w) return NULL;
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    char *out = (char *)malloc((size_t)n);
    if (!out) return NULL;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, n, NULL, NULL);
    return out;
}

wchar_t *cw_w_from_u8(const char *s)
{
    if (!s) return NULL;
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *out = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!out) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, out, n);
    return out;
}

void cw_dir_of_exe(void)
{
    DWORD n = GetModuleFileNameW(NULL, g_dirw, MAX_PATH);
    if (n == 0) { wcscpy(g_dirw, L"."); }
    wchar_t *slash = wcsrchr(g_dirw, L'\\');
    if (slash) *slash = 0;
    char *u8 = cw_u8_from_w(g_dirw);
    if (u8) { lstrcpynA(g_dir, u8, sizeof(g_dir)); free(u8); }
}

void cw_full_path(wchar_t *out, size_t cap, const wchar_t *file)
{
    _snwprintf(out, cap, L"%s\\%s", g_dirw, file);
    out[cap - 1] = 0;
}

void cw_log(const char *fmt, ...) { (void)fmt; }

void cw_hex_to_rgb(const char *hex, BYTE *r, BYTE *g, BYTE *b,
                   BYTE def_r, BYTE def_g, BYTE def_b)
{
    *r = def_r; *g = def_g; *b = def_b;
    if (!hex) return;
    const char *p = (*hex == '#') ? hex + 1 : hex;
    if (strlen(p) != 6) return;
    unsigned int v = 0;
    if (sscanf(p, "%x", &v) != 1) return;
    *r = (BYTE)((v >> 16) & 0xFF);
    *g = (BYTE)((v >> 8) & 0xFF);
    *b = (BYTE)(v & 0xFF);
}

/* ---------------------------------------------------------------- JSON 取值 */
static const char *skip_ws(const char *p)
{
    while (p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    return p;
}

const char *cw_jfind(const char *json, const char *key)
{
    if (!json || !key) return NULL;
    char pat[128];
    _snprintf(pat, sizeof(pat), "\"%s\"", key);
    pat[sizeof(pat) - 1] = 0;
    const char *p = strstr(json, pat);
    if (!p) return NULL;
    p = strchr(p + strlen(pat), ':');
    if (!p) return NULL;
    return skip_ws(p + 1);
}

int cw_jstr(const char *json, const char *key, char *out, int cap)
{
    out[0] = 0;
    const char *p = cw_jfind(json, key);
    if (!p) return 0;
    if (*p == '"') p++;
    int i = 0;
    while (*p && *p != '"' && i < cap - 1) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n': out[i++] = '\n'; break;
            case 't': out[i++] = '\t'; break;
            case 'r': out[i++] = '\r'; break;
            case 'u': out[i++] = '?'; p += 4; break;   /* 本程序不写 \u，够用 */
            default:  out[i++] = *p; break;           /* \\ 和 \" */
            }
            p++;
        } else {
            out[i++] = *p++;
        }
    }
    out[i] = 0;
    return i > 0;
}

int cw_jint(const char *json, const char *key, int def)
{
    const char *p = cw_jfind(json, key);
    if (!p) return def;
    return (int)strtol(p, NULL, 10);
}

double cw_jdbl(const char *json, const char *key, double def)
{
    const char *p = cw_jfind(json, key);
    if (!p) return def;
    return strtod(p, NULL);
}

int cw_jbool(const char *json, const char *key, int def)
{
    const char *p = cw_jfind(json, key);
    if (!p) return def;
    if (!_strnicmp(p, "true", 4)) return 1;
    if (!_strnicmp(p, "false", 5)) return 0;
    return def;
}

void cw_jesc(char *out, int cap, const char *in)
{
    int i = 0;
    for (const char *p = in ? in : ""; *p && i < cap - 2; p++) {
        if (*p == '\\' || *p == '"') out[i++] = '\\';
        out[i++] = *p;
    }
    out[i] = 0;
}

/* ---------------------------------------------------------------- 读配置 */
static void defaults(void)
{
    memset(&g_cfg, 0, sizeof(g_cfg));
    lstrcpynA(g_cfg.api_base, DEF_API_BASE, sizeof(g_cfg.api_base));
    g_cfg.use_proxy = 1;
    g_cfg.refresh_seconds = 30;
    g_cfg.show_delta = 1;
    g_cfg.delta_hold_seconds = 600;
    g_cfg.show_status_line = 1;
    g_cfg.show_balance_detail = 1;
    g_cfg.show_only_charged = 1;
    g_cfg.charged_hold_minutes = 60;
    g_cfg.provider_count = 1;
    memset(&g_cfg.providers[0], 0, sizeof(Provider));
    lstrcpynA(g_cfg.providers[0].name, "DeepSeek", sizeof(g_cfg.providers[0].name));
    lstrcpynA(g_cfg.providers[0].type, "deepseek", sizeof(g_cfg.providers[0].type));
    g_cfg.providers[0].enabled = 1;
    g_cfg.w = 360;
    g_cfg.h = 112;
    g_cfg.opacity = 0.95;
    g_cfg.rounded = 20;
    g_cfg.topmost = 1;
    g_cfg.scrim_alpha = 110;
    lstrcpynA(g_cfg.font_family, "Microsoft YaHei UI", sizeof(g_cfg.font_family));
    g_cfg.value_size = 30;
    g_cfg.label_size = 12;
    g_cfg.detail_size = 12;
    lstrcpynA(g_cfg.label_color, "#8FA6BF", sizeof(g_cfg.label_color));
    lstrcpynA(g_cfg.value_color, "#FFFFFF", sizeof(g_cfg.value_color));
    lstrcpynA(g_cfg.detail_color, "#A8B8C8", sizeof(g_cfg.detail_color));
    lstrcpynA(g_cfg.accent_color, "#4C8DFF", sizeof(g_cfg.accent_color));
    lstrcpynA(g_cfg.exams[0].name, "CET-4", sizeof(g_cfg.exams[0].name));
    lstrcpynA(g_cfg.exams[0].datetime, "2026-12-12 09:00", sizeof(g_cfg.exams[0].datetime));
    lstrcpynA(g_cfg.exams[1].name, "CET-4", sizeof(g_cfg.exams[1].name));
    lstrcpynA(g_cfg.exams[1].datetime, "2027-06-12 09:00", sizeof(g_cfg.exams[1].datetime));
    g_cfg.exam_count = 2;
    g_cfg.bg_x = g_cfg.bg_y = 0.5;
}

static void parse_datetime(const char *s, SYSTEMTIME *out)
{
    int Y = 0, M = 0, D = 0, h = 0, mi = 0;
    memset(out, 0, sizeof(*out));
    int n = sscanf(s, "%d-%d-%d %d:%d", &Y, &M, &D, &h, &mi);
    if (n < 3) return;
    out->wYear = (WORD)Y;
    out->wMonth = (WORD)M;
    out->wDay = (WORD)D;
    out->wHour = (WORD)h;
    out->wMinute = (WORD)mi;
}

static time_t st_to_time(const SYSTEMTIME *st)
{
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = st->wYear - 1900;
    tmv.tm_mon = st->wMonth - 1;
    tmv.tm_mday = st->wDay;
    tmv.tm_hour = st->wHour;
    tmv.tm_min = st->wMinute;
    tmv.tm_isdst = -1;
    return mktime(&tmv);
}

void cw_load_config(void)
{
    defaults();
    HKEY settings;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, CW_SETTINGS_KEY, 0, KEY_READ, &settings) == ERROR_SUCCESS) {
        DWORD size = 0, type = 0;
        if (RegQueryValueExW(settings, L"SettingsV1", NULL, &type, NULL, &size) == ERROR_SUCCESS &&
            type == REG_BINARY && size < 1024 * 1024) {
            BYTE *data = (BYTE *)malloc(size);
            if (data && RegQueryValueExW(settings,L"SettingsV1",NULL,&type,data,&size)==ERROR_SUCCESS) {
                DATA_BLOB in = {size,data}, out = {0};
                if (CryptUnprotectData(&in,NULL,NULL,NULL,NULL,CRYPTPROTECT_UI_FORBIDDEN,&out)) {
                    if (out.cbData == sizeof(Config)) {
                        memcpy(&g_cfg,out.pbData,sizeof(Config));
                        LocalFree(out.pbData); free(data); RegCloseKey(settings); return;
                    }
                    LocalFree(out.pbData);
                }
            }
            free(data);
        }
        RegCloseKey(settings);
        MessageBoxW(NULL,L"用户设置无法解密或版本不兼容。为保护原配置，本次不会覆盖设置。",L"配置读取失败",MB_OK|MB_ICONWARNING);
        settings_read_failed = 1;
        return;
    }
    for (int i = 0; i < g_cfg.exam_count; ++i) {
        SYSTEMTIME st;
        parse_datetime(g_cfg.exams[i].datetime, &st);
        g_cfg.exams[i].t = st_to_time(&st);
    }

    wchar_t path[MAX_PATH];
    cw_full_path(path, MAX_PATH, L"config.json");
    FILE *fp = _wfopen(path, L"rb");
    if (!fp) {
        DWORD attrs = GetFileAttributesW(path);
        if (attrs == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND) {
            cw_log("没有找到 config.json，使用默认配置并生成一份");
            cw_save_config();
        } else {
            cw_log("config.json 无法读取，保留原文件，本次不保存配置");
        }
        return;
    }
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (len <= 0 || len > 1024 * 1024) { fclose(fp); return; }
    char *buf = (char *)malloc((size_t)len + 1);
    if (!buf) { fclose(fp); return; }
    size_t got = fread(buf, 1, (size_t)len, fp);
    buf[got] = 0;
    int read_ok = !ferror(fp) && got == (size_t)len;
    fclose(fp);
    if (!read_ok) { free(buf); return; }

    cw_jstr(buf, "deepseek_api_key", g_cfg.api_key, sizeof(g_cfg.api_key));
    cw_jstr(buf, "api_base", g_cfg.api_base, sizeof(g_cfg.api_base));
    if (!g_cfg.api_base[0]) lstrcpynA(g_cfg.api_base, DEF_API_BASE, sizeof(g_cfg.api_base));
    g_cfg.use_proxy  = cw_jbool(buf, "use_system_proxy", g_cfg.use_proxy);
    g_cfg.refresh_seconds = cw_jint(buf, "balance_refresh_seconds", g_cfg.refresh_seconds);
    if (g_cfg.refresh_seconds < 10) g_cfg.refresh_seconds = 30;
    g_cfg.show_delta     = cw_jbool(buf, "show_delta", g_cfg.show_delta);
    g_cfg.delta_hold_seconds = cw_jint(buf, "delta_hold_seconds", g_cfg.delta_hold_seconds);
    g_cfg.show_status_line   = cw_jbool(buf, "show_status_line", g_cfg.show_status_line);
    g_cfg.show_balance_detail = cw_jbool(buf, "show_balance_detail", g_cfg.show_balance_detail);
    cw_jstr(buf, "background_image", g_cfg.bg_image, sizeof(g_cfg.bg_image));
    cw_jstr(buf, "tray_icon", g_cfg.tray_icon, sizeof(g_cfg.tray_icon));

    const char *win = cw_jfind(buf, "window");
    if (win) {
        g_cfg.w = cw_jint(win, "width", g_cfg.w);
        g_cfg.h = cw_jint(win, "height", g_cfg.h);
        g_cfg.x = cw_jint(win, "x", 0);
        g_cfg.y = cw_jint(win, "y", 0);
        const char *px = cw_jfind(win, "x");
        g_cfg.has_pos = (px && *px != 'n' && *px != 'N');   /* null 视为没存过 */
        g_cfg.opacity = cw_jdbl(win, "opacity", g_cfg.opacity);
        g_cfg.rounded = cw_jint(win, "rounded", g_cfg.rounded);
        g_cfg.topmost = cw_jbool(win, "topmost", g_cfg.topmost);
    }
    const char *app = cw_jfind(buf, "appearance");
    if (app) {
        g_cfg.scrim_alpha = cw_jint(app, "scrim_alpha", g_cfg.scrim_alpha);
        cw_jstr(app, "font_family", g_cfg.font_family, sizeof(g_cfg.font_family));
        g_cfg.value_size  = cw_jint(app, "value_size", g_cfg.value_size);
        g_cfg.label_size  = cw_jint(app, "label_size", g_cfg.label_size);
        g_cfg.detail_size = cw_jint(app, "detail_size", g_cfg.detail_size);
        cw_jstr(app, "label_color", g_cfg.label_color, sizeof(g_cfg.label_color));
        cw_jstr(app, "value_color", g_cfg.value_color, sizeof(g_cfg.value_color));
        cw_jstr(app, "detail_color", g_cfg.detail_color, sizeof(g_cfg.detail_color));
        cw_jstr(app, "accent_color", g_cfg.accent_color, sizeof(g_cfg.accent_color));
    }
    g_cfg.lock_position = cw_jbool(buf, "lock_position", 0);

    /* 多服务商：默认只显示“最近扣过钱”的那家 */
    g_cfg.show_only_charged    = cw_jbool(buf, "show_only_charged", g_cfg.show_only_charged);
    g_cfg.charged_hold_minutes = cw_jint(buf, "charged_hold_minutes", g_cfg.charged_hold_minutes);
    if (g_cfg.charged_hold_minutes < 0) g_cfg.charged_hold_minutes = 0;

    const char *pv = cw_jfind(buf, "providers");
    int have_providers = 0;
    if (pv && *pv == '[') {
        const char *arr_end = strchr(pv, ']');   /* 数组边界：别把后面的 exams/window 也吞了 */
        g_cfg.provider_count = 0;
        const char *pp = pv;
        while (*pp && g_cfg.provider_count < CW_MAX_PROVIDERS &&
               (!arr_end || pp < arr_end)) {
            const char *ob = strchr(pp, '{');
            if (!ob || (arr_end && ob >= arr_end)) break;
            const char *cb = strchr(ob, '}');
            if (!cb || (arr_end && cb > arr_end)) break;
            int blen = (int)(cb - ob);
            if (blen > 0 && blen < 4000) {
                char *block = (char *)malloc((size_t)blen + 1);
                if (!block) break;
                memcpy(block, ob, (size_t)blen);
                block[blen] = 0;
                Provider *pr = &g_cfg.providers[g_cfg.provider_count];
                memset(pr, 0, sizeof(*pr));
                cw_jstr(block, "name", pr->name, sizeof(pr->name));
                cw_jstr(block, "type", pr->type, sizeof(pr->type));
                cw_jstr(block, "api_key", pr->api_key, sizeof(pr->api_key));
                cw_jstr(block, "url", pr->url, sizeof(pr->url));
                cw_jstr(block, "headers", pr->headers, sizeof(pr->headers));
                cw_jstr(block, "json_path", pr->json_path, sizeof(pr->json_path));
                cw_jstr(block, "prefix", pr->prefix, sizeof(pr->prefix));
                pr->enabled = cw_jbool(block, "enabled", 1);
                free(block);
                if (!pr->type[0]) lstrcpynA(pr->type, "http", sizeof(pr->type));
                if (!pr->name[0]) lstrcpynA(pr->name, pr->type, sizeof(pr->name));
                g_cfg.provider_count++;
            }
            pp = cb + 1;
        }
        have_providers = (g_cfg.provider_count > 0);
    }
    /* 老配置兼容：没有 providers 就按 deepseek_api_key 造一个 */
    if (!have_providers && !(pv && *pv == '[')) {
        g_cfg.provider_count = 1;
        memset(&g_cfg.providers[0], 0, sizeof(Provider));
        lstrcpynA(g_cfg.providers[0].name, "DeepSeek", sizeof(g_cfg.providers[0].name));
        lstrcpynA(g_cfg.providers[0].type, "deepseek", sizeof(g_cfg.providers[0].type));
        lstrcpynA(g_cfg.providers[0].api_key, g_cfg.api_key,
                  sizeof(g_cfg.providers[0].api_key));
        g_cfg.providers[0].enabled = 1;
    }

    if (g_cfg.w < 160) g_cfg.w = 160;
    if (g_cfg.h < 72)  g_cfg.h = 72;
    if (g_cfg.opacity < 0.35) g_cfg.opacity = 0.35;
    if (g_cfg.opacity > 1.0)  g_cfg.opacity = 1.0;

    /* 考试列表 */
    const char *ex = cw_jfind(buf, "exams");
    if (ex && *ex == '[') {
        const char *arr_end = strchr(ex, ']');
        g_cfg.exam_count = 0;
        const char *p = ex;
        while (*p && g_cfg.exam_count < CW_MAX_EXAMS && (!arr_end || p < arr_end)) {
            const char *ob = strchr(p, '{');
            if (!ob || (arr_end && ob >= arr_end)) break;
            const char *cb = strchr(ob, '}');
            if (!cb || (arr_end && cb > arr_end)) break;
            int blen = (int)(cb - ob);
            if (blen > 0 && blen < 500) {
                char block[512];
                memcpy(block, ob, (size_t)blen);
                block[blen] = 0;
                Exam *e = &g_cfg.exams[g_cfg.exam_count];
                memset(e, 0, sizeof(*e));
                cw_jstr(block, "name", e->name, sizeof(e->name));
                cw_jstr(block, "datetime", e->datetime, sizeof(e->datetime));
                if (!e->name[0]) lstrcpynA(e->name, "CET-4", sizeof(e->name));
                SYSTEMTIME st;
                parse_datetime(e->datetime, &st);
                e->t = st_to_time(&st);
                if (st.wYear >= 2000) g_cfg.exam_count++;
            }
            p = cb + 1;
        }
    }
    /* 按时间排序（冒泡，最多 8 个） */
    for (int i = 0; i < g_cfg.exam_count; i++)
        for (int j = i + 1; j < g_cfg.exam_count; j++)
            if (g_cfg.exams[j].t < g_cfg.exams[i].t) {
                Exam tmp = g_cfg.exams[i];
                g_cfg.exams[i] = g_cfg.exams[j];
                g_cfg.exams[j] = tmp;
            }
    free(buf);
    cw_save_config(); /* One-time import of the old adjacent JSON. */
}

int cw_save_config(void)
{
    if (settings_read_failed) return 0;
    DATA_BLOB in = {sizeof(Config), (BYTE *)&g_cfg}, out = {0};
    if (!CryptProtectData(&in,L"DeepSeekWidget",NULL,NULL,NULL,CRYPTPROTECT_UI_FORBIDDEN,&out)) return 0;
    HKEY key;
    LONG rc = RegCreateKeyExW(HKEY_CURRENT_USER,CW_SETTINGS_KEY,0,NULL,0,KEY_SET_VALUE,NULL,&key,NULL);
    if (rc == ERROR_SUCCESS) {
        rc = RegSetValueExW(key,L"SettingsV1",0,REG_BINARY,out.pbData,out.cbData);
        RegCloseKey(key);
    }
    LocalFree(out.pbData);
    return rc == ERROR_SUCCESS;
}


/* ---------------------------------------------------------------- 倒计时 */
void cw_countdown(char *value, int cap_v, char *detail, int cap_d)
{
    value[0] = 0; detail[0] = 0;
    if (g_cfg.exam_count <= 0) {
        lstrcpynA(value, "—", cap_v);
        lstrcpynA(detail, "config.json 里还没有考试日期", cap_d);
        return;
    }
    time_t now = time(NULL);
    Exam *pick = NULL;
    for (int i = 0; i < g_cfg.exam_count; i++) {
        if (now < g_cfg.exams[i].t + 3 * 3600) { pick = &g_cfg.exams[i]; break; }
    }
    if (!pick) {
        Exam *last = &g_cfg.exams[g_cfg.exam_count - 1];
        lstrcpynA(value, "已结束", cap_v);
        struct tm tmv;
        localtime_s(&tmv, &last->t);
        _snprintf(detail, cap_d, "%04d-%02d-%02d 场次已过", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
        return;
    }
    struct tm et;
    localtime_s(&et, &pick->t);
    double delta = difftime(pick->t, now);
    if (delta <= 0) {
        lstrcpynA(value, "考试中", cap_v);
        _snprintf(detail, cap_d, "本场 %02d:%02d 开考", et.tm_hour, et.tm_min);
    } else if (delta >= 86400) {
        _snprintf(value, cap_v, "%d 天", (int)(delta / 86400));
    } else if (delta >= 3600) {
        _snprintf(value, cap_v, "%d 时 %d 分", (int)(delta / 3600), (int)((long)delta % 3600) / 60);
    } else {
        _snprintf(value, cap_v, "%d 分 %d 秒", (int)(delta / 60), (int)((long)delta % 60));
    }
    struct tm nt;
    localtime_s(&nt, &now);
    if (et.tm_year == nt.tm_year)
        _snprintf(detail, cap_d, "%02d-%02d %02d:%02d 开考", et.tm_mon + 1, et.tm_mday, et.tm_hour, et.tm_min);
    else
        _snprintf(detail, cap_d, "%04d-%02d-%02d %02d:%02d 开考", et.tm_year + 1900, et.tm_mon + 1,
                  et.tm_mday, et.tm_hour, et.tm_min);
}

/* ---------------------------------------------------------------- 自启动 */
int cw_autostart_get(wchar_t *out, int cap)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, CW_RUN_KEY, 0, KEY_READ, &k) != ERROR_SUCCESS) return 0;
    wchar_t buf[1024];
    DWORD size = sizeof(buf), type = 0;
    LONG rc = RegQueryValueExW(k, CW_RUN_VALUE, NULL, &type, (BYTE *)buf, &size);
    RegCloseKey(k);
    if (rc != ERROR_SUCCESS) return 0;
    if (out && cap > 0) { lstrcpynW(out, buf, cap); }
    return 1;
}

int cw_autostart_set(int enable)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, CW_RUN_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL)
        != ERROR_SUCCESS) return 0;
    LONG rc;
    if (enable) {
        wchar_t exe[MAX_PATH + 2];
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        wchar_t value[MAX_PATH + 8];
        _snwprintf(value, MAX_PATH + 8, L"\"%s\"", exe);
        value[MAX_PATH + 7] = 0;
        rc = RegSetValueExW(k, CW_RUN_VALUE, 0, REG_SZ, (const BYTE *)value,
                            (DWORD)((wcslen(value) + 1) * sizeof(wchar_t)));
    } else {
        rc = RegDeleteValueW(k, CW_RUN_VALUE);
        if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;
    }
    RegCloseKey(k);
    return rc == ERROR_SUCCESS;
}
