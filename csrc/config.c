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

Config  g_cfg;
wchar_t g_dirw[MAX_PATH] = {0};
char    g_dir[MAX_PATH * 2] = {0};

#define DEF_API_BASE "https://api.deepseek.com"

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

void cw_log(const char *fmt, ...)
{
    wchar_t path[MAX_PATH];
    cw_full_path(path, MAX_PATH, L"widget.log");

    /* 超过 256KB 就截掉前面，避免无限增长 */
    HANDLE probe = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (probe != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size;
        if (GetFileSizeEx(probe, &size) && size.QuadPart > 256 * 1024) {
            CloseHandle(probe);
            DeleteFileW(path);
        } else {
            CloseHandle(probe);
        }
    }

    FILE *fp = _wfopen(path, L"ab");
    if (!fp) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(fp, "[%04d-%02d-%02d %02d:%02d:%02d] ", st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fprintf(fp, "\n");
    fclose(fp);
}

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

    wchar_t path[MAX_PATH];
    cw_full_path(path, MAX_PATH, L"config.json");
    FILE *fp = _wfopen(path, L"rb");
    if (!fp) {
        cw_log("没有找到 config.json，使用默认配置并生成一份");
        cw_save_config();
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
    fclose(fp);

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
    if (g_cfg.w < 160) g_cfg.w = 160;
    if (g_cfg.h < 72)  g_cfg.h = 72;
    if (g_cfg.opacity < 0.35) g_cfg.opacity = 0.35;
    if (g_cfg.opacity > 1.0)  g_cfg.opacity = 1.0;

    /* 考试列表 */
    const char *ex = cw_jfind(buf, "exams");
    if (ex && *ex == '[') {
        g_cfg.exam_count = 0;
        const char *p = ex;
        while (*p && g_cfg.exam_count < CW_MAX_EXAMS) {
            const char *ob = strchr(p, '{');
            const char *cb = ob ? strchr(ob, '}') : NULL;
            if (!ob || !cb) break;
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
}

void cw_save_config(void)
{
    wchar_t path[MAX_PATH];
    cw_full_path(path, MAX_PATH, L"config.json");
    FILE *fp = _wfopen(path, L"wb");
    if (!fp) { cw_log("配置保存失败（目录不可写？）"); return; }

    char key[1100], base[600], bg[2100], font[200];
    cw_jesc(key, sizeof(key), g_cfg.api_key);
    cw_jesc(base, sizeof(base), g_cfg.api_base);
    cw_jesc(bg, sizeof(bg), g_cfg.bg_image);
    cw_jesc(font, sizeof(font), g_cfg.font_family);

    fprintf(fp, "{\n");
    fprintf(fp, "  \"deepseek_api_key\": \"%s\",\n", key);
    fprintf(fp, "  \"api_base\": \"%s\",\n", base);
    fprintf(fp, "  \"use_system_proxy\": %s,\n", g_cfg.use_proxy ? "true" : "false");
    fprintf(fp, "  \"balance_refresh_seconds\": %d,\n", g_cfg.refresh_seconds);
    fprintf(fp, "  \"show_delta\": %s,\n", g_cfg.show_delta ? "true" : "false");
    fprintf(fp, "  \"delta_hold_seconds\": %d,\n", g_cfg.delta_hold_seconds);
    fprintf(fp, "  \"show_status_line\": %s,\n", g_cfg.show_status_line ? "true" : "false");
    fprintf(fp, "  \"exams\": [\n");
    for (int i = 0; i < g_cfg.exam_count; i++) {
        char nm[100], dt[100];
        cw_jesc(nm, sizeof(nm), g_cfg.exams[i].name);
        cw_jesc(dt, sizeof(dt), g_cfg.exams[i].datetime);
        fprintf(fp, "    { \"name\": \"%s\", \"datetime\": \"%s\" }%s\n", nm, dt,
                (i == g_cfg.exam_count - 1) ? "" : ",");
    }
    fprintf(fp, "  ],\n");
    fprintf(fp, "  \"background_image\": \"%s\",\n", bg);
    fprintf(fp, "  \"window\": {\n");
    fprintf(fp, "    \"width\": %d,\n    \"height\": %d,\n", g_cfg.w, g_cfg.h);
    if (g_cfg.has_pos) {
        fprintf(fp, "    \"x\": %d,\n    \"y\": %d,\n", g_cfg.x, g_cfg.y);
    } else {
        fprintf(fp, "    \"x\": null,\n    \"y\": null,\n");
    }
    fprintf(fp, "    \"opacity\": %.2f,\n    \"rounded\": %d,\n", g_cfg.opacity, g_cfg.rounded);
    fprintf(fp, "    \"topmost\": %s,\n", g_cfg.topmost ? "true" : "false");
    fprintf(fp, "    \"transparent\": true\n  },\n");
    fprintf(fp, "  \"appearance\": {\n");
    fprintf(fp, "    \"scrim_alpha\": %d,\n", g_cfg.scrim_alpha);
    fprintf(fp, "    \"text_shadow\": true,\n");
    fprintf(fp, "    \"font_family\": \"%s\",\n", font);
    fprintf(fp, "    \"value_size\": %d,\n    \"label_size\": %d,\n    \"detail_size\": %d,\n",
            g_cfg.value_size, g_cfg.label_size, g_cfg.detail_size);
    fprintf(fp, "    \"label_color\": \"%s\",\n", g_cfg.label_color);
    fprintf(fp, "    \"value_color\": \"%s\",\n", g_cfg.value_color);
    fprintf(fp, "    \"detail_color\": \"%s\",\n", g_cfg.detail_color);
    fprintf(fp, "    \"accent_color\": \"%s\"\n  },\n", g_cfg.accent_color);
    fprintf(fp, "  \"lock_position\": %s,\n", g_cfg.lock_position ? "true" : "false");
    fprintf(fp, "  \"show_balance_detail\": %s\n", g_cfg.show_balance_detail ? "true" : "false");
    fprintf(fp, "}\n");
    fclose(fp);
}

/* ---------------------------------------------------------------- 查余额 */
static void set_err(char *err, int cap, const char *msg)
{
    if (err && cap > 0) { lstrcpynA(err, msg, cap); }
}

/* 从响应体里取一个字符串字段（先找 CNY 那一段） */
static int pick_str(const char *body, const char *field, char *out, int cap)
{
    out[0] = 0;
    const char *zone = strstr(body, "\"CNY\"");
    if (!zone) zone = body;
    char pat[64];
    _snprintf(pat, sizeof(pat), "\"%s\"", field);
    pat[sizeof(pat) - 1] = 0;
    const char *p = strstr(zone, pat);
    if (!p) p = strstr(body, pat);
    if (!p) return 0;
    p = strchr(p + strlen(pat), ':');
    if (!p) return 0;
    p++;
    while (*p == ' ') p++;
    if (*p == '"') p++;
    int i = 0;
    while (*p && *p != '"' && *p != ',' && *p != '}' && i < cap - 1) out[i++] = *p++;
    out[i] = 0;
    return i > 0;
}

int cw_fetch_balance(const char *key, const char *base, int use_proxy,
                     char *currency, int cap_cur,
                     char *total, int cap_total,
                     char *granted, int cap_granted,
                     char *topped, int cap_topped,
                     char *err, int cap_err)
{
    if (!key || !key[0]) { set_err(err, cap_err, "API Key 为空"); return 0; }
    if (strncmp(key, "sk-", 3) != 0) {
        char msg[256];
        _snprintf(msg, sizeof(msg),
                  "API Key 格式不对：应以 sk- 开头（当前只有 %d 位）；请到 platform.deepseek.com/api_keys 复制完整 Key 再粘贴",
                  (int)strlen(key));
        msg[sizeof(msg) - 1] = 0;
        set_err(err, cap_err, msg);
        return 0;
    }
    if (!base || !base[0]) base = DEF_API_BASE;
    if (currency && cap_cur) currency[0] = 0;
    if (total && cap_total) total[0] = 0;

    wchar_t *wbase = cw_w_from_u8(base);
    wchar_t *wkey  = cw_w_from_u8(key);
    if (!wbase || !wkey) {
        free(wbase); free(wkey);
        set_err(err, cap_err, "参数编码失败");
        return 0;
    }

    URL_COMPONENTS uc;
    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {0};
    wchar_t path[1024] = {0};
    uc.lpszHostName = host;  uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;   uc.dwUrlPathLength = 1023;
    if (!WinHttpCrackUrl(wbase, 0, 0, &uc)) {
        set_err(err, cap_err, "api_base 地址无效");
        free(wbase); free(wkey);
        return 0;
    }
    int secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);

    /* 去掉尾部斜杠再拼 /user/balance */
    size_t plen = wcslen(path);
    while (plen > 0 && path[plen - 1] == L'/') path[--plen] = 0;
    wchar_t full[1200];
    _snwprintf(full, 1200, L"%s/user/balance", path);
    full[1199] = 0;

    HINTERNET ses = WinHttpOpen(L"DeepSeekWidget/1.0",
                                use_proxy ? WINHTTP_ACCESS_TYPE_DEFAULT_PROXY
                                          : WINHTTP_ACCESS_TYPE_NO_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) { set_err(err, cap_err, "WinHTTP 初始化失败"); free(wbase); free(wkey); return 0; }
    WinHttpSetTimeouts(ses, 15000, 15000, 15000, 20000);

    HINTERNET con = WinHttpConnect(ses, host, uc.nPort, 0);
    if (!con) {
        set_err(err, cap_err, "连不上服务器（检查网络/代理）");
        WinHttpCloseHandle(ses); free(wbase); free(wkey);
        return 0;
    }
    HINTERNET req = WinHttpOpenRequest(con, L"GET", full, NULL, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       secure ? WINHTTP_FLAG_SECURE : 0);
    int ok = 0;
    if (req) {
        wchar_t hdr[1200];
        _snwprintf(hdr, 1200, L"Authorization: Bearer %s", wkey);
        hdr[1199] = 0;
        WinHttpAddRequestHeaders(req, hdr, -1L,
                                 WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
        if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
            set_err(err, cap_err, "请求发送失败（网络不通或代理异常）");
        } else if (!WinHttpReceiveResponse(req, NULL)) {
            set_err(err, cap_err, "没收到响应（超时或被代理拦下）");
        } else {
            DWORD status = 0, slen = sizeof(status);
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &slen, WINHTTP_NO_HEADER_INDEX);
            char *body = (char *)malloc(65536);
            DWORD used = 0;
            if (body) {
                for (;;) {
                    DWORD avail = 0;
                    if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
                    if (used + avail > 65000) avail = 65000 - used;
                    DWORD got = 0;
                    if (!WinHttpReadData(req, body + used, avail, &got) || got == 0) break;
                    used += got;
                    if (used >= 65000) break;
                }
                body[used] = 0;
            }
            if (status == 200 && body) {
                if (pick_str(body, "currency", currency, cap_cur) &&
                    pick_str(body, "total_balance", total, cap_total)) {
                    pick_str(body, "granted_balance", granted, cap_granted);
                    pick_str(body, "topped_up_balance", topped, cap_topped);
                    if (!currency[0]) lstrcpynA(currency, "CNY", cap_cur);
                    ok = 1;
                } else {
                    set_err(err, cap_err, "接口未返回余额信息");
                }
            } else if (status == 401 || status == 403) {
                set_err(err, cap_err, "DeepSeek 拒绝了这个 API Key（401），请重新生成一个再粘贴");
            } else {
                char msg[128];
                _snprintf(msg, sizeof(msg), "接口返回 HTTP %lu", (unsigned long)status);
                msg[sizeof(msg) - 1] = 0;
                set_err(err, cap_err, msg);
            }
            free(body);
        }
        WinHttpCloseHandle(req);
    } else {
        set_err(err, cap_err, "创建请求失败");
    }
    WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    free(wbase);
    free(wkey);
    return ok;
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
