/* 余额服务商层
 * ------------------------------------------------------------------
 * 内置预置：deepseek / moonshot / siliconflow / openrouter
 * 通用类型：http —— 自己填 url / headers / json_path，任何返回 JSON 的接口都能接
 *
 * 只负责“查一次余额并解析”，不关心界面怎么显示。
 */
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <winhttp.h>

#define DEF_API_BASE "https://api.deepseek.com"

static void set_err(char *err, int cap, const char *msg)
{
    if (err && cap > 0) lstrcpynA(err, msg, cap);
}

/* 从 p 指向的位置读一个 JSON 值（字符串或数字）到 out */
static int read_value_at(const char *p, char *out, int cap)
{
    out[0] = 0;
    while (p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    if (!p || !*p) return 0;
    int quoted = (*p == '"');
    if (quoted) p++;
    int i = 0;
    while (*p && i < cap - 1) {
        if (quoted) {
            if (*p == '"') break;
        } else if (*p == ',' || *p == '}' || *p == ']' || *p == ' ' ||
                   *p == '\r' || *p == '\n' || *p == '\t') {
            break;
        }
        out[i++] = *p++;
    }
    out[i] = 0;
    return i > 0;
}

/* 按点号路径取字段，例如 "data.balance" */
int cw_json_path(const char *json, const char *path, char *out, int cap)
{
    if (out && cap > 0) out[0] = 0;
    if (!json || !path || !*path) return 0;

    const char *p = json;
    const char *seg = path;
    for (;;) {
        const char *dot = strchr(seg, '.');
        int n = dot ? (int)(dot - seg) : (int)strlen(seg);
        if (n <= 0 || n >= 64) return 0;
        char key[64];
        memcpy(key, seg, (size_t)n);
        key[n] = 0;
        p = cw_jfind(p, key);
        if (!p) return 0;
        if (!dot) break;
        seg = dot + 1;
    }
    return read_value_at(p, out, cap);
}

/* DeepSeek 的余额是数组，优先取 CNY 那一段 */
static int pick_deepseek_str(const char *body, const char *field, char *out, int cap)
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
    return read_value_at(p + 1, out, cap);
}

int cw_http_get(const char *url, const char *extra_headers, int use_proxy,
                char *out, int cap, char *err, int cap_err)
{
    if (out && cap > 0) out[0] = 0;
    if (!url || !url[0]) { set_err(err, cap_err, "接口地址为空"); return 0; }

    wchar_t *wurl = cw_w_from_u8(url);
    if (!wurl) { set_err(err, cap_err, "接口地址编码失败"); return 0; }

    URL_COMPONENTS uc;
    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {0};
    wchar_t path[2048] = {0};
    wchar_t query[1024] = {0};
    uc.lpszHostName = host;      uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;       uc.dwUrlPathLength = 2047;
    uc.lpszExtraInfo = query;    uc.dwExtraInfoLength = 1023;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) {
        set_err(err, cap_err, "接口地址格式不对");
        free(wurl);
        return 0;
    }
    free(wurl);
    int secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);

    wchar_t full[3200];
    _snwprintf(full, 3200, L"%s%s", path, query);
    full[3199] = 0;

    HINTERNET ses = WinHttpOpen(L"DeepSeekWidget/1.1",
                                use_proxy ? WINHTTP_ACCESS_TYPE_DEFAULT_PROXY
                                          : WINHTTP_ACCESS_TYPE_NO_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) { set_err(err, cap_err, "WinHTTP 初始化失败"); return 0; }
    WinHttpSetTimeouts(ses, 15000, 15000, 15000, 20000);

    HINTERNET con = WinHttpConnect(ses, host, uc.nPort, 0);
    if (!con) {
        set_err(err, cap_err, "连不上服务器（检查网络或代理）");
        WinHttpCloseHandle(ses);
        return 0;
    }
    HINTERNET req = WinHttpOpenRequest(con, L"GET", full, NULL, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       secure ? WINHTTP_FLAG_SECURE : 0);
    int ok = 0;
    if (!req) {
        set_err(err, cap_err, "创建请求失败");
    } else {
        WinHttpAddRequestHeaders(req, L"Accept: application/json", -1L,
                                 WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
        /* 额外请求头用 | 分隔 */
        if (extra_headers && extra_headers[0]) {
            char tmp[1200];
            lstrcpynA(tmp, extra_headers, sizeof(tmp));
            char *save = NULL;
            for (char *line = strtok_s(tmp, "|", &save); line; line = strtok_s(NULL, "|", &save)) {
                while (*line == ' ') line++;
                wchar_t *wl = cw_w_from_u8(line);
                if (wl) {
                    WinHttpAddRequestHeaders(req, wl, -1L,
                                             WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
                    free(wl);
                }
            }
        }
        if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
            set_err(err, cap_err, "请求发送失败（网络不通或代理异常）");
        } else if (!WinHttpReceiveResponse(req, NULL)) {
            set_err(err, cap_err, "没收到响应（超时，或被代理拦下）");
        } else {
            DWORD status = 0, slen = sizeof(status);
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &slen,
                                WINHTTP_NO_HEADER_INDEX);
            DWORD used = 0;
            for (;;) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
                if (used + avail > (DWORD)cap - 1) avail = (DWORD)cap - 1 - used;
                DWORD got = 0;
                if (!WinHttpReadData(req, out + used, avail, &got) || got == 0) break;
                used += got;
                if (used >= (DWORD)cap - 1) break;
            }
            out[used] = 0;
            if (status == 200) {
                ok = 1;
            } else if (status == 401 || status == 403) {
                char msg[160];
                _snprintf(msg, sizeof(msg), "接口返回 %lu（Key 无效或没有权限）",
                          (unsigned long)status);
                msg[sizeof(msg) - 1] = 0;
                set_err(err, cap_err, msg);
            } else {
                char msg[128];
                _snprintf(msg, sizeof(msg), "接口返回 HTTP %lu", (unsigned long)status);
                msg[sizeof(msg) - 1] = 0;
                set_err(err, cap_err, msg);
            }
        }
        WinHttpCloseHandle(req);
    }
    WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return ok;
}

int cw_provider_fetch(const Provider *p, int use_proxy,
                      char *total, int cap_total,
                      char *prefix, int cap_prefix,
                      char *extra, int cap_extra,
                      char *err, int cap_err)
{
    if (total && cap_total) total[0] = 0;
    if (prefix && cap_prefix) prefix[0] = 0;
    if (extra && cap_extra) extra[0] = 0;
    if (!p) { set_err(err, cap_err, "服务商配置为空"); return 0; }

    const char *type = p->type[0] ? p->type : "http";
    const char *key = p->api_key;
    int is_http = (lstrcmpiA(type, "http") == 0);

    /* http 类型允许不填 Key（有些接口只认自定义请求头） */
    if (!is_http && (!key || !key[0])) {
        set_err(err, cap_err, "还没有填 API Key");
        return 0;
    }

    char url[1024] = {0};
    if (lstrcmpiA(type, "deepseek") == 0) {
        const char *base = g_cfg.api_base[0] ? g_cfg.api_base : DEF_API_BASE;
        size_t n = strlen(base);
        while (n > 0 && base[n - 1] == '/') n--;
        _snprintf(url, sizeof(url), "%.*s/user/balance", (int)n, base);
        url[sizeof(url) - 1] = 0;
    } else if (lstrcmpiA(type, "moonshot") == 0) {
        lstrcpynA(url, "https://api.moonshot.cn/v1/users/me/balance", sizeof(url));
    } else if (lstrcmpiA(type, "siliconflow") == 0) {
        lstrcpynA(url, "https://api.siliconflow.cn/v1/user/info", sizeof(url));
    } else if (lstrcmpiA(type, "openrouter") == 0) {
        lstrcpynA(url, "https://openrouter.ai/api/v1/credits", sizeof(url));
    } else {
        if (!p->url[0]) { set_err(err, cap_err, "http 类型需要填 url"); return 0; }
        lstrcpynA(url, p->url, sizeof(url));
    }

    char hdrs[1200] = {0};
    if (p->url[0]) lstrcpynA(url, p->url, sizeof(url));
    if (key && key[0]) {
        _snprintf(hdrs, sizeof(hdrs), "Authorization: Bearer %s", key);
        hdrs[sizeof(hdrs) - 1] = 0;
    }
    if (p->headers[0]) {
        strncat(hdrs, "|", sizeof(hdrs) - strlen(hdrs) - 1);
        strncat(hdrs, p->headers, sizeof(hdrs) - strlen(hdrs) - 1);
    }

    char *body = (char *)malloc(65536);
    if (!body) { set_err(err, cap_err, "内存不足"); return 0; }
    int ok = cw_http_get(url, hdrs, use_proxy, body, 65536, err, cap_err);
    if (!ok) { free(body); return 0; }

    ok = 0;
    if (lstrcmpiA(type, "deepseek") == 0) {
        char cur[16] = {0}, granted[32] = {0}, topped[32] = {0};
        if (pick_deepseek_str(body, "total_balance", total, cap_total)) {
            pick_deepseek_str(body, "currency", cur, sizeof(cur));
            pick_deepseek_str(body, "granted_balance", granted, sizeof(granted));
            pick_deepseek_str(body, "topped_up_balance", topped, sizeof(topped));
            if (prefix && cap_prefix)
                lstrcpynA(prefix, lstrcmpiA(cur, "USD") == 0 ? "$" : "¥", cap_prefix);
            if (extra && cap_extra) {
                if (granted[0] && strcmp(granted, "0.00") != 0 && strcmp(granted, "0") != 0)
                    _snprintf(extra, cap_extra, "赠送 %s", granted);
                else if (topped[0] && strcmp(topped, "0.00") != 0)
                    _snprintf(extra, cap_extra, "充值 %s", topped);
                extra[cap_extra - 1] = 0;
            }
            ok = 1;
        } else {
            set_err(err, cap_err, "接口返回里没有 total_balance");
        }
    } else if (lstrcmpiA(type, "moonshot") == 0) {
        if (cw_json_path(body, "data.available_balance", total, cap_total)) {
            if (prefix && cap_prefix) lstrcpynA(prefix, "¥", cap_prefix);
            if (extra && cap_extra) lstrcpynA(extra, "可用余额", cap_extra);
            ok = 1;
        } else {
            set_err(err, cap_err, "接口返回里没有 data.available_balance");
        }
    } else if (lstrcmpiA(type, "siliconflow") == 0) {
        char cash[32] = {0}, gift[32] = {0};
        if (cw_json_path(body, "data.totalBalance", total, cap_total)) {
            cw_json_path(body, "data.chargeBalance", cash, sizeof(cash));
            cw_json_path(body, "data.balance", gift, sizeof(gift));
            if (prefix && cap_prefix) lstrcpynA(prefix, "¥", cap_prefix);
            if (extra && cap_extra) {
                if (cash[0] && gift[0]) _snprintf(extra, cap_extra, "充值 %s · 赠送 %s", cash, gift);
                else lstrcpynA(extra, "账户余额", cap_extra);
                extra[cap_extra - 1] = 0;
            }
            ok = 1;
        } else {
            set_err(err, cap_err, "接口返回里没有 data.totalBalance");
        }
    } else if (lstrcmpiA(type, "openrouter") == 0) {
        char credits[64] = {0}, usage[64] = {0};
        if (cw_json_path(body, "data.total_credits", credits, sizeof(credits)) &&
            cw_json_path(body, "data.total_usage", usage, sizeof(usage))) {
            double left = atof(credits) - atof(usage);
            _snprintf(total, cap_total, "%.2f", left);
            total[cap_total - 1] = 0;
            if (prefix && cap_prefix) lstrcpynA(prefix, "$", cap_prefix);
            if (extra && cap_extra) {
                _snprintf(extra, cap_extra, "已用 %.2f", atof(usage));
                extra[cap_extra - 1] = 0;
            }
            ok = 1;
        } else {
            set_err(err, cap_err, "接口返回里没有 credits / usage");
        }
    } else {
        if (!p->json_path[0]) {
            set_err(err, cap_err, "http 类型需要填 json_path，例如 data.balance");
        } else if (cw_json_path(body, p->json_path, total, cap_total)) {
            if (prefix && cap_prefix)
                lstrcpynA(prefix, p->prefix[0] ? p->prefix : "¥", cap_prefix);
            ok = 1;
        } else {
            char msg[160];
            _snprintf(msg, sizeof(msg), "返回里没找到字段 %s（检查 json_path）", p->json_path);
            msg[sizeof(msg) - 1] = 0;
            set_err(err, cap_err, msg);
        }
    }
    free(body);
    return ok;
}
