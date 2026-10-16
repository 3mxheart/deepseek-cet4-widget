/* 悬浮窗本体：GDI+ 逐像素透明渲染 + 窗口 / 菜单 / 交互 / 后台刷新线程
 *
 * 说明：Windows 的 GDI+ 官方接口是 C++ 类（gdiplus.h），所以本文件用 C++ 编译，
 *       但整体按 C 风格写：不用 STL / 异常 / RTTI，产出仍是小体积原生 exe。
 */
#include <windows.h>
#include <gdiplus.h>
#include <commdlg.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "config.h"

using namespace Gdiplus;

#define WM_CW_BALANCE (WM_APP + 1)
#define TIMER_TICK    1

enum {
    IDM_REFRESH = 1001, IDM_SETKEY, IDM_OPENFOLDER, IDM_EDITCFG, IDM_PICKBG,
    IDM_CLEARBG, IDM_OPACITY_UP, IDM_OPACITY_DOWN, IDM_LOCK, IDM_STARTUP, IDM_EXIT,
    IDM_HIDE,
    IDM_FREQ_BASE = 1100,
    IDM_IDC_KEY_OK = 1200, IDM_IDC_KEY_CANCEL
};

static const int kFreqs[] = { 10, 15, 30, 60, 300, 600 };
#define FREQ_COUNT ((int)(sizeof(kFreqs) / sizeof(kFreqs[0])))

/* ---------------------------------------------------------------- 全局状态 */
static HINSTANCE  g_inst;
static HWND       g_hwnd;
static HANDLE     g_thread, g_ev_refresh, g_ev_stop;
static CRITICAL_SECTION g_lock;
static ULONG_PTR  g_gdiplus;

static char       g_currency[16] = "CNY";
static char       g_total[64] = "";
static char       g_granted[32] = "";
static char       g_topped[32] = "";
static char       g_error[512] = "";
static int        g_has_balance = 0;
static int        g_loading = 1;
static int        g_has_updated = 0, g_has_attempt = 0;
static SYSTEMTIME g_updated_at, g_attempt_at;
static double     g_delta = 0;
static int        g_has_delta = 0;
static SYSTEMTIME g_delta_at;

static HBITMAP    g_dib = NULL;
static void      *g_bits = NULL;
static HDC        g_memdc = NULL;
static int        g_bmp_w = 0, g_bmp_h = 0;
static wchar_t    g_font_family[64] = L"Microsoft YaHei UI";
static char       g_last_key[1024] = "";
static HWND       g_keydlg = NULL;
static int        g_key_changed = 0;

/* 托盘图标 */
#define WM_CW_TRAY (WM_APP + 2)
#define TRAY_ID    1
static NOTIFYICONDATAW g_nid;
static int        g_hidden = 0;

/* ---------------------------------------------------------------- 小工具 */
static void cw_fmt(char *out, int cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(out, (size_t)cap, fmt, ap);
    va_end(ap);
    out[cap - 1] = 0;
}

static double parse_total(const char *s)
{
    if (!s || !s[0]) return -1;
    char *end = NULL;
    double v = strtod(s, &end);
    if (end == s) return -1;
    return v;
}

static void hhmmss(const SYSTEMTIME *st, char *out, int cap)
{
    cw_fmt(out, cap, "%02d:%02d:%02d", st->wHour, st->wMinute, st->wSecond);
}

static void interval_text(char *out, int cap)
{
    int s = g_cfg.refresh_seconds;
    if (s >= 60 && s % 60 == 0) cw_fmt(out, cap, "每 %d 分钟", s / 60);
    else                        cw_fmt(out, cap, "每 %d 秒", s);
}

static long long seconds_between(const SYSTEMTIME *a, const SYSTEMTIME *b)
{
    struct tm ta, tb;
    memset(&ta, 0, sizeof(ta));
    memset(&tb, 0, sizeof(tb));
    ta.tm_year = a->wYear - 1900; ta.tm_mon = a->wMonth - 1; ta.tm_mday = a->wDay;
    ta.tm_hour = a->wHour; ta.tm_min = a->wMinute; ta.tm_sec = a->wSecond;
    tb.tm_year = b->wYear - 1900; tb.tm_mon = b->wMonth - 1; tb.tm_mday = b->wDay;
    tb.tm_hour = b->wHour; tb.tm_min = b->wMinute; tb.tm_sec = b->wSecond;
    return (long long)difftime(mktime(&tb), mktime(&ta));
}

/* ---------------------------------------------------------------- 文案组装 */
typedef struct {
    char value[64];
    char detail[512];
    char cd_value[32];
    char cd_detail[80];
    char status[192];
    char delta[24];
    int  delta_up;
    int  value_gray;
} Texts;

static void build_texts(Texts *t)
{
    memset(t, 0, sizeof(*t));
    SYSTEMTIME now;
    GetLocalTime(&now);

    EnterCriticalSection(&g_lock);
    int has = g_has_balance, loading = g_loading;
    char total[64], cur[16], gr[32], tp[32], err[512];
    lstrcpynA(total, g_total, sizeof(total));
    lstrcpynA(cur, g_currency, sizeof(cur));
    lstrcpynA(gr, g_granted, sizeof(gr));
    lstrcpynA(tp, g_topped, sizeof(tp));
    lstrcpynA(err, g_error, sizeof(err));
    int has_upd = g_has_updated, has_att = g_has_attempt, has_delta = g_has_delta;
    double delta = g_delta;
    SYSTEMTIME updated = g_updated_at, attempt = g_attempt_at, dat = g_delta_at;
    LeaveCriticalSection(&g_lock);

    if (has) {
        const char *sym = "\xc2\xa5";               /* ¥ (UTF-8) */
        if (lstrcmpiA(cur, "USD") == 0) sym = "$";
        else if (lstrcmpiA(cur, "EUR") == 0) sym = "\xe2\x82\xac";
        cw_fmt(t->value, sizeof(t->value), "%s%s", sym, total);
        char parts[256] = "";
        if (gr[0] && strcmp(gr, "0.00") != 0 && strcmp(gr, "0") != 0)
            cw_fmt(parts + strlen(parts), (int)sizeof(parts) - (int)strlen(parts), "赠送 %s", gr);
        if (tp[0] && strcmp(tp, "0.00") != 0 && strcmp(tp, "0") != 0) {
            if (parts[0]) strcat(parts, " · ");
            cw_fmt(parts + strlen(parts), (int)sizeof(parts) - (int)strlen(parts), "充值 %s", tp);
        }
        lstrcpynA(t->detail, parts[0] ? parts : "账户余额", sizeof(t->detail));
    } else if (loading) {
        lstrcpynA(t->value, "…", sizeof(t->value));
        lstrcpynA(t->detail, "正在查询余额…", sizeof(t->detail));
    } else {
        lstrcpynA(t->value, "--", sizeof(t->value));
        lstrcpynA(t->detail, err[0] ? err : "余额未知", sizeof(t->detail));
        t->value_gray = 1;
    }
    if (!g_cfg.show_balance_detail) t->detail[0] = 0;

    cw_countdown(t->cd_value, sizeof(t->cd_value), t->cd_detail, sizeof(t->cd_detail));

    if (has_delta && g_cfg.show_delta) {
        long long age = seconds_between(&dat, &now);
        if (g_cfg.delta_hold_seconds <= 0 || age <= g_cfg.delta_hold_seconds) {
            cw_fmt(t->delta, sizeof(t->delta), "刚 %+.2f", delta);
            t->delta_up = (delta > 0);
        }
    }

    if (g_cfg.show_status_line) {
        char every[32], t1[16];
        interval_text(every, sizeof(every));
        if (has && has_upd) {
            hhmmss(&updated, t1, sizeof(t1));
            cw_fmt(t->status, sizeof(t->status), "更新于 %s · %s刷新", t1, every);
        } else if (err[0] && has_att) {
            hhmmss(&attempt, t1, sizeof(t1));
            cw_fmt(t->status, sizeof(t->status), "刷新失败 %s · %s重试", t1, every);
        } else {
            cw_fmt(t->status, sizeof(t->status), "%s自动刷新", every);
        }
    }
}

/* ---------------------------------------------------------------- 背景 */
/* 背景图缓存：大照片只在“路径或尺寸变化”时解码并缩放一次。
   否则每 10 秒重绘都要重新解一张几 MB 的 JPG，既费 CPU 又拖慢启动。 */
static Bitmap *g_bg_scaled = NULL;
static char    g_bg_path[1024] = "";
static int     g_bg_w = 0, g_bg_h = 0;

static void free_bg_cache(void)
{
    if (g_bg_scaled) { delete g_bg_scaled; g_bg_scaled = NULL; }
    g_bg_path[0] = 0;
    g_bg_w = g_bg_h = 0;
}

static Bitmap *background_scaled(int W, int H)
{
    if (g_bg_scaled && g_bg_w == W && g_bg_h == H
        && lstrcmpA(g_bg_path, g_cfg.bg_image) == 0)
        return g_bg_scaled;
    if (g_bg_scaled) { delete g_bg_scaled; g_bg_scaled = NULL; }
    lstrcpynA(g_bg_path, g_cfg.bg_image, sizeof(g_bg_path));
    g_bg_w = W;
    g_bg_h = H;
    if (!g_bg_path[0]) return NULL;

    wchar_t *wp = cw_w_from_u8(g_bg_path);
    if (!wp) return NULL;
    Image src(wp);
    free(wp);
    if (src.GetLastStatus() != Ok || src.GetWidth() <= 0 || src.GetHeight() <= 0) {
        cw_log("背景图加载失败: %s", g_bg_path);
        return NULL;
    }
    Bitmap *out = new Bitmap(W, H, PixelFormat32bppPARGB);
    if (!out || out->GetLastStatus() != Ok) { delete out; return NULL; }
    {
        Graphics g(out);
        g.SetCompositingMode(CompositingModeSourceOver);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        g.Clear(Color(0, 0, 0, 0));
        float sw = (float)src.GetWidth(), sh = (float)src.GetHeight();
        float scale = ((float)W / sw > (float)H / sh) ? (float)W / sw : (float)H / sh;
        float dw = sw * scale, dh = sh * scale;
        float dx = ((float)W - dw) / 2.0f, dy = ((float)H - dh) / 2.0f;
        g.DrawImage(&src, RectF(dx, dy, dw, dh), 0, 0, sw, sh, UnitPixel);
    }
    g_bg_scaled = out;
    return g_bg_scaled;
}

static void rounded_path(GraphicsPath *p, float x, float y, float w, float h, float r)
{
    if (r <= 0.5f) { p->AddRectangle(RectF(x, y, w, h)); return; }
    float d = r * 2.0f;
    p->AddArc(x, y, d, d, 180.0f, 90.0f);
    p->AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    p->AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    p->AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    p->CloseFigure();
}

static void draw_background(Graphics *g, int W, int H)
{
    BYTE ar = 76, ag = 141, ab = 255;
    cw_hex_to_rgb(g_cfg.accent_color, &ar, &ag, &ab, 76, 141, 255);

    int drew_image = 0;
    Bitmap *bg = background_scaled(W, H);
    if (bg) {
        GraphicsPath clip;
        rounded_path(&clip, 0, 0, (float)W, (float)H, (float)g_cfg.rounded);
        g->SetClip(&clip);
        g->DrawImage(bg, 0, 0, W, H);
        int scrim = g_cfg.scrim_alpha;
        if (scrim > 255) scrim = 255;
        if (scrim > 0) {
            SolidBrush veil(Color((BYTE)scrim, 0, 0, 0));
            g->FillRectangle(&veil, RectF(0, 0, (float)W, (float)H));
        }
        g->ResetClip();
        drew_image = 1;
    }
    if (!drew_image) {
        LinearGradientBrush bg(Point(0, 0), Point(0, H),
                               Color(255, 18, 24, 34), Color(255, 8, 11, 16));
        GraphicsPath path;
        rounded_path(&path, 0, 0, (float)W, (float)H, (float)g_cfg.rounded);
        g->FillPath(&bg, &path);
        /* GDI+ 没有模糊，用多层同心椭圆叠出柔光 */
        for (int i = 10; i >= 1; i--) {
            float grow = (float)i * 6.0f;
            RectF e(-(float)W * 0.20f - grow, (float)H * 0.52f - grow,
                    (float)W * 0.95f + grow * 2.0f, (float)H * 1.10f + grow * 2.0f);
            SolidBrush soft(Color((BYTE)4, ar, ag, ab));
            g->FillEllipse(&soft, e);
        }
    }
}

/* ---------------------------------------------------------------- 文字 */
static void resolve_font_family(void)
{
    wchar_t *want = cw_w_from_u8(g_cfg.font_family);
    static const wchar_t *fallbacks[] = { L"Microsoft YaHei UI", L"Microsoft YaHei",
                                          L"SimHei", L"Arial" };
    if (want && want[0]) {
        FontFamily probe(want);
        if (probe.GetLastStatus() == Ok) {
            lstrcpynW(g_font_family, want, 64);
            free(want);
            return;
        }
    }
    free(want);
    for (int i = 0; i < 4; i++) {
        FontFamily probe(fallbacks[i]);
        if (probe.GetLastStatus() == Ok) {
            lstrcpynW(g_font_family, fallbacks[i], 64);
            return;
        }
    }
}

static float measure_text(Graphics *g, const char *utf8, float size, BOOL bold)
{
    wchar_t *w = cw_w_from_u8(utf8);
    if (!w || !w[0]) { free(w); return 0.0f; }
    Font font(g_font_family, size, bold ? FontStyleBold : FontStyleRegular, UnitPixel);
    StringFormat sf;
    sf.SetFormatFlags(StringFormatFlagsNoWrap);
    RectF bound;
    g->MeasureString(w, -1, &font, RectF(0, 0, 4000.0f, 200.0f), &sf, &bound);
    free(w);
    return bound.Width;
}

/* 画一行文字：字号自适应 + 淡投影 */
static void draw_line(Graphics *g, const char *utf8, const RectF *area, float size, BOOL bold,
                      Color color, StringAlignment align, BOOL shadow)
{
    wchar_t *w = cw_w_from_u8(utf8);
    if (!w || !w[0]) { free(w); return; }

    StringFormat sf;
    sf.SetAlignment(align);
    sf.SetLineAlignment(StringAlignmentNear);
    sf.SetFormatFlags(StringFormatFlagsNoWrap | StringFormatFlagsNoClip);

    float use = size;
    RectF bound;
    while (use > 7.0f) {
        Font probe(g_font_family, use, bold ? FontStyleBold : FontStyleRegular, UnitPixel);
        g->MeasureString(w, -1, &probe, *area, &sf, &bound);
        if (bound.Width <= area->Width + 1.0f) break;
        use -= 1.0f;
    }
    Font font(g_font_family, use, bold ? FontStyleBold : FontStyleRegular, UnitPixel);
    float lh = (float)font.GetHeight(g);
    RectF rc(area->X, area->Y - lh * 0.06f, area->Width, lh * 1.15f);
    if (shadow) {
        SolidBrush sh(Color(130, 0, 0, 0));
        RectF rcs(rc.X + 1.0f, rc.Y + 1.0f, rc.Width, rc.Height);
        g->DrawString(w, -1, &font, rcs, &sf, &sh);
    }
    SolidBrush brush(color);
    g->DrawString(w, -1, &font, rc, &sf, &brush);
    free(w);
}

/* ---------------------------------------------------------------- 渲染 */
static void ensure_surface(int W, int H)
{
    if (g_bits && W == g_bmp_w && H == g_bmp_h) return;
    if (g_dib) { DeleteObject(g_dib); g_dib = NULL; }
    if (g_memdc) { DeleteDC(g_memdc); g_memdc = NULL; }
    g_bits = NULL;
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC screen = GetDC(NULL);
    g_dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &g_bits, NULL, 0);
    g_memdc = CreateCompatibleDC(screen);
    ReleaseDC(NULL, screen);
    if (!g_dib || !g_memdc || !g_bits) return;
    SelectObject(g_memdc, g_dib);
    memset(g_bits, 0, (size_t)W * H * 4);
    g_bmp_w = W;
    g_bmp_h = H;
}

static void render(void)
{
    int W = g_cfg.w, H = g_cfg.h;
    ensure_surface(W, H);
    if (!g_bits) return;

    Texts t;
    build_texts(&t);

    {
        Bitmap bmp(W, H, W * 4, PixelFormat32bppPARGB, (BYTE *)g_bits);
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);
        g.SetCompositingMode(CompositingModeSourceOver);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.Clear(Color(0, 0, 0, 0));

        draw_background(&g, W, H);

        BYTE lr, lg, lb, vr, vg, vb, dr, dg, db;
        cw_hex_to_rgb(g_cfg.label_color, &lr, &lg, &lb, 143, 166, 191);
        cw_hex_to_rgb(g_cfg.value_color, &vr, &vg, &vb, 255, 255, 255);
        cw_hex_to_rgb(g_cfg.detail_color, &dr, &dg, &db, 168, 184, 200);
        Color c_label(lr, lg, lb), c_value(vr, vg, vb), c_detail(dr, dg, db);
        if (t.value_gray) c_value = c_label;

        float pad = 16.0f, gap = 10.0f;
        float col_w = ((float)W - 2.0f * pad - gap) / 2.0f;
        float label_sz = (float)g_cfg.label_size;
        float value_sz = (float)g_cfg.value_size;
        float detail_sz = (float)g_cfg.detail_size;
        float label_h = label_sz + 4.0f, value_h = value_sz + 6.0f, detail_h = detail_sz + 4.0f;
        float vgap = 3.0f;
        float block_h = label_h + vgap + value_h + vgap + detail_h;
        float status_h = t.status[0] ? (detail_sz + 4.0f) : 0.0f;
        float top = (float)H - status_h - block_h;
        top = (top / 2.0f > pad * 0.4f) ? top / 2.0f : pad * 0.4f;
        float y_label = top;
        float y_value = y_label + label_h + vgap;
        float y_detail = y_value + value_h + vgap;

        /* 左栏：余额 */
        RectF r_label(pad, y_label, col_w, label_h);
        draw_line(&g, "DeepSeek 余额", &r_label, label_sz, FALSE, c_label,
                  StringAlignmentNear, TRUE);
        if (t.delta[0]) {
            float lw = measure_text(&g, "DeepSeek 余额", label_sz, FALSE);
            float dx = pad + lw + 2.0f;
            float room = col_w - (dx - pad);
            if (room > 10.0f) {
                Color c_delta = t.delta_up ? Color(126, 217, 160) : Color(255, 156, 156);
                RectF r_delta(dx, y_label, room, label_h);
                draw_line(&g, t.delta, &r_delta, label_sz, FALSE, c_delta,
                          StringAlignmentNear, TRUE);
            }
        }
        RectF r_value(pad, y_value, col_w, value_h);
        draw_line(&g, t.value, &r_value, value_sz, TRUE, c_value, StringAlignmentNear, TRUE);
        RectF r_detail(pad, y_detail, col_w, detail_h);
        draw_line(&g, t.detail, &r_detail, detail_sz, FALSE, c_detail,
                  StringAlignmentNear, TRUE);

        /* 右栏：倒计时 */
        float rx = pad + col_w + gap;
        char exam_label[64];
        cw_fmt(exam_label, sizeof(exam_label), "%s 倒计时",
               g_cfg.exam_count > 0 ? g_cfg.exams[0].name : "考试");
        RectF r2_label(rx, y_label, col_w, label_h);
        draw_line(&g, exam_label, &r2_label, label_sz, FALSE, c_label,
                  StringAlignmentNear, TRUE);
        RectF r2_value(rx, y_value, col_w, value_h);
        draw_line(&g, t.cd_value, &r2_value, value_sz, TRUE, c_value,
                  StringAlignmentNear, TRUE);
        RectF r2_detail(rx, y_detail, col_w, detail_h);
        draw_line(&g, t.cd_detail, &r2_detail, detail_sz, FALSE, c_detail,
                  StringAlignmentNear, TRUE);

        /* 分隔线 */
        float line_x = pad + col_w + gap / 2.0f;
        Pen pen(Color(38, 255, 255, 255), 1.0f);
        g.DrawLine(&pen, line_x, y_label, line_x, y_label + block_h);

        /* 底部状态行 */
        if (t.status[0]) {
            float ys = (float)H - pad * 0.45f - detail_sz;
            RectF r_status(pad, ys, (float)W - 2.0f * pad, detail_h);
            draw_line(&g, t.status, &r_status, detail_sz, FALSE, c_label,
                      StringAlignmentCenter, TRUE);
        }
        g.Flush();
    }

    RECT rc;
    GetWindowRect(g_hwnd, &rc);
    POINT src = { 0, 0 }, dst = { rc.left, rc.top };
    SIZE size = { W, H };
    BYTE alpha = (BYTE)(g_cfg.opacity * 255.0);
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA };
    HDC screen = GetDC(NULL);
    UpdateLayeredWindow(g_hwnd, screen, &dst, &size, g_memdc, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(NULL, screen);
}

/* 文案没变就不重画，省电 */
static void render_if_changed(int force)
{
    Texts t;
    build_texts(&t);
    char key[1024];
    cw_fmt(key, sizeof(key), "%s|%s|%s|%s|%s|%s|%d|%d|%d|%d",
           t.value, t.detail, t.cd_value, t.cd_detail, t.status, t.delta,
           g_cfg.w, g_cfg.h, g_cfg.value_size, g_cfg.rounded);
    if (!force && strcmp(key, g_last_key) == 0) return;
    lstrcpynA(g_last_key, key, sizeof(g_last_key));
    render();
}

/* ---------------------------------------------------------------- 交互 */
static void save_pos(void)
{
    RECT rc;
    GetWindowRect(g_hwnd, &rc);
    g_cfg.x = rc.left;
    g_cfg.y = rc.top;
    g_cfg.has_pos = 1;
    cw_save_config();
}

static void bump_opacity(double d)
{
    g_cfg.opacity += d;
    if (g_cfg.opacity < 0.35) g_cfg.opacity = 0.35;
    if (g_cfg.opacity > 1.0) g_cfg.opacity = 1.0;
    cw_save_config();
    render_if_changed(1);
}

/* ---- 设置 API Key 的小对话框 ---- */
static HFONT ui_font(void)
{
    static HFONT f = NULL;
    if (!f)
        f = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        DEFAULT_PITCH, L"Microsoft YaHei UI");
    return f;
}

static LRESULT CALLBACK KeyDlgProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CREATE: {
        HWND lab = CreateWindowExW(0, L"STATIC",
                                   L"把 DeepSeek API Key 粘贴到这里（形如 sk-xxxxxxxx）",
                                   WS_CHILD | WS_VISIBLE, 16, 14, 430, 20, h, NULL, g_inst, NULL);
        wchar_t *cur = cw_w_from_u8(g_cfg.api_key);
        HWND ed = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", cur ? cur : L"",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_PASSWORD,
                                  16, 40, 430, 26, h, (HMENU)1, g_inst, NULL);
        free(cur);
        HWND hint = CreateWindowExW(0, L"STATIC",
                                    L"保存后会立刻查一次余额，结果显示在悬浮窗上",
                                    WS_CHILD | WS_VISIBLE, 16, 74, 430, 20, h, NULL, g_inst, NULL);
        HWND ok = CreateWindowExW(0, L"BUTTON", L"保存并测试",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                  226, 104, 104, 30, h, (HMENU)IDM_IDC_KEY_OK, g_inst, NULL);
        HWND cancel = CreateWindowExW(0, L"BUTTON", L"取消",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                      342, 104, 104, 30, h, (HMENU)IDM_IDC_KEY_CANCEL, g_inst, NULL);
        HWND all[5] = { lab, ed, hint, ok, cancel };
        for (int i = 0; i < 5; i++) SendMessageW(all[i], WM_SETFONT, (WPARAM)ui_font(), TRUE);
        SetFocus(ed);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(w) == IDM_IDC_KEY_OK) {
            HWND ed = GetDlgItem(h, 1);
            int len = GetWindowTextLengthW(ed);
            wchar_t *buf = (wchar_t *)malloc((size_t)(len + 2) * sizeof(wchar_t));
            if (buf) {
                GetWindowTextW(ed, buf, len + 1);
                char *u8 = cw_u8_from_w(buf);
                if (u8) {
                    while (*u8 == ' ') memmove(u8, u8 + 1, strlen(u8) + 1);
                    size_t n = strlen(u8);
                    while (n > 0 && (u8[n - 1] == ' ' || u8[n - 1] == '\r' || u8[n - 1] == '\n'))
                        u8[--n] = 0;
                    lstrcpynA(g_cfg.api_key, u8, sizeof(g_cfg.api_key));
                    free(u8);
                    g_key_changed = 1;
                }
                free(buf);
            }
            DestroyWindow(h);
            return 0;
        }
        if (LOWORD(w) == IDM_IDC_KEY_CANCEL) { DestroyWindow(h); return 0; }
        break;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

static void show_key_dialog(void)
{
    RECT rc;
    GetWindowRect(g_hwnd, &rc);
    g_key_changed = 0;
    g_keydlg = CreateWindowExW(WS_EX_TOPMOST | WS_EX_DLGMODALFRAME, L"CWKeyDlg",
                               L"设置 DeepSeek API Key",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU,
                               rc.left - 60, rc.top + 40, 470, 180,
                               g_hwnd, NULL, g_inst, NULL);
    if (!g_keydlg) {
        cw_log("设置对话框创建失败: %lu", GetLastError());
        return;
    }
    ShowWindow(g_keydlg, SW_SHOW);
    UpdateWindow(g_keydlg);

    MSG msg;
    while (IsWindow(g_keydlg) && GetMessageW(&msg, NULL, 0, 0)) {
        if (!IsDialogMessageW(g_keydlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (g_key_changed) {
        cw_save_config();
        g_key_changed = 0;
        SetEvent(g_ev_refresh);
    }
}

static void pick_background(void)
{
    wchar_t file[MAX_PATH] = {0};
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"选择背景图片";
    ofn.lpstrFilter = L"图片\0*.png;*.jpg;*.jpeg;*.bmp;*.gif\0所有文件\0*.*\0\0";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) {
        char *u8 = cw_u8_from_w(file);
        if (u8) {
            lstrcpynA(g_cfg.bg_image, u8, sizeof(g_cfg.bg_image));
            free(u8);
            cw_save_config();
            render_if_changed(1);
        }
    }
}

/* ---- 托盘图标：显示/隐藏、右键菜单、鼠标悬停显示余额 ---- */
static void toggle_visible(void)
{
    g_hidden = !g_hidden;
    if (g_hidden) {
        ShowWindow(g_hwnd, SW_HIDE);
    } else {
        ShowWindow(g_hwnd, SW_SHOWNA);
        render_if_changed(1);
    }
    cw_log("悬浮窗 -> %s", g_hidden ? "隐藏" : "显示");
}

static void tray_add(void)
{
    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = TRAY_ID;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_CW_TRAY;
    g_nid.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(1));
    if (!g_nid.hIcon) g_nid.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    lstrcpynW(g_nid.szTip, L"DeepSeek 余额 / CET-4 倒计时", 128);
    if (!Shell_NotifyIconW(NIM_ADD, &g_nid)) {
        cw_log("托盘图标添加失败: %lu", GetLastError());
        return;
    }
    g_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
    cw_log("托盘图标已添加（若任务栏没看到，点任务栏的 ^ 折叠区把它拖出来）");
}

static void tray_tip(const char *value, const char *detail)
{
    if (!g_nid.hWnd) return;
    wchar_t *wv = cw_w_from_u8(value);
    wchar_t *wd = cw_w_from_u8(detail);
    _snwprintf(g_nid.szTip, 128, L"DeepSeek %s · %s", wv ? wv : L"--", wd ? wd : L"");
    g_nid.szTip[127] = 0;
    free(wv);
    free(wd);
    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
}

static void tray_remove(void)
{
    if (g_nid.hWnd) Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

/* trayMode: 1 = 托盘右键菜单（多了“显示/隐藏”），0 = 窗体右键菜单 */
static void show_menu(int trayMode)
{
    HMENU menu = CreatePopupMenu();
    HMENU sub = CreatePopupMenu();
    if (trayMode)
        AppendMenuW(menu, MF_STRING, IDM_HIDE, g_hidden ? L"显示悬浮窗" : L"隐藏悬浮窗");
    AppendMenuW(menu, MF_STRING, IDM_REFRESH, L"立即刷新余额（双击窗体同效）");
    AppendMenuW(menu, MF_STRING, IDM_SETKEY, L"设置 API Key…");
    for (int i = 0; i < FREQ_COUNT; i++) {
        char label[32];
        if (kFreqs[i] >= 60) cw_fmt(label, sizeof(label), "%d 分钟", kFreqs[i] / 60);
        else                 cw_fmt(label, sizeof(label), "%d 秒", kFreqs[i]);
        wchar_t *wl = cw_w_from_u8(label);
        AppendMenuW(sub, MF_STRING | (g_cfg.refresh_seconds == kFreqs[i] ? MF_CHECKED : 0),
                    (UINT_PTR)(IDM_FREQ_BASE + i), wl ? wl : L"?");
        free(wl);
    }
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)sub, L"刷新频率");
    if (!trayMode)
        AppendMenuW(menu, MF_STRING, IDM_HIDE, L"隐藏到托盘（托盘图标可叫回来）");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_PICKBG, L"更换背景图片…");
    AppendMenuW(menu, MF_STRING, IDM_CLEARBG, L"恢复默认背景");
    AppendMenuW(menu, MF_STRING, IDM_OPENFOLDER, L"打开所在文件夹");
    AppendMenuW(menu, MF_STRING, IDM_EDITCFG, L"编辑配置文件");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_OPACITY_UP, L"不透明度 +");
    AppendMenuW(menu, MF_STRING, IDM_OPACITY_DOWN, L"不透明度 −");
    AppendMenuW(menu, MF_STRING | (g_cfg.lock_position ? MF_CHECKED : 0), IDM_LOCK, L"锁定位置");
    wchar_t startup[1024];
    AppendMenuW(menu, MF_STRING | (cw_autostart_get(startup, 1024) ? MF_CHECKED : 0),
                IDM_STARTUP, L"开机自动启动");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"退出");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    int cmd = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                  pt.x, pt.y, 0, g_hwnd, NULL);
    DestroyMenu(menu);

    if (cmd >= IDM_FREQ_BASE && cmd < IDM_FREQ_BASE + FREQ_COUNT) {
        g_cfg.refresh_seconds = kFreqs[cmd - IDM_FREQ_BASE];
        cw_save_config();
        render_if_changed(1);
        SetEvent(g_ev_refresh);
        return;
    }
    switch (cmd) {
    case IDM_REFRESH:
        SetEvent(g_ev_refresh);
        break;
    case IDM_SETKEY:
        show_key_dialog();
        break;
    case IDM_PICKBG:
        pick_background();
        break;
    case IDM_CLEARBG:
        g_cfg.bg_image[0] = 0;
        cw_save_config();
        render_if_changed(1);
        break;
    case IDM_OPENFOLDER:
        ShellExecuteW(NULL, L"open", g_dirw, NULL, NULL, SW_SHOWNORMAL);
        break;
    case IDM_EDITCFG: {
        wchar_t path[MAX_PATH];
        cw_full_path(path, MAX_PATH, L"config.json");
        ShellExecuteW(NULL, L"open", path, NULL, g_dirw, SW_SHOWNORMAL);
        break;
    }
    case IDM_OPACITY_UP:
        bump_opacity(0.05);
        break;
    case IDM_OPACITY_DOWN:
        bump_opacity(-0.05);
        break;
    case IDM_LOCK:
        g_cfg.lock_position = !g_cfg.lock_position;
        cw_save_config();
        break;
    case IDM_STARTUP: {
        int on = cw_autostart_get(startup, 1024) ? 0 : 1;
        cw_autostart_set(on);
        cw_log("开机自启动 -> %s", on ? "开" : "关");
        break;
    }
    case IDM_HIDE:
        toggle_visible();
        break;
    case IDM_EXIT:
        DestroyWindow(g_hwnd);
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------------- 窗口过程 */
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    static int dragging = 0;
    static POINT drag_off;

    switch (m) {
    case WM_LBUTTONDOWN:
        if (!g_cfg.lock_position) {
            POINT pt;
            GetCursorPos(&pt);
            RECT rc;
            GetWindowRect(h, &rc);
            drag_off.x = pt.x - rc.left;
            drag_off.y = pt.y - rc.top;
            dragging = 1;
            SetCapture(h);
        }
        return 0;
    case WM_MOUSEMOVE:
        if (dragging) {
            POINT pt;
            GetCursorPos(&pt);
            SetWindowPos(h, NULL, pt.x - drag_off.x, pt.y - drag_off.y, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (dragging) {
            dragging = 0;
            ReleaseCapture();
            save_pos();
        }
        return 0;
    case WM_LBUTTONDBLCLK:
        SetEvent(g_ev_refresh);
        return 0;
    case WM_MOUSEWHEEL:
        bump_opacity(GET_WHEEL_DELTA_WPARAM(w) > 0 ? 0.04 : -0.04);
        return 0;
    case WM_RBUTTONUP:
        show_menu(0);
        return 0;
    case WM_CW_TRAY:
        switch (LOWORD(l)) {
        case WM_LBUTTONUP:
        case NIN_SELECT:
        case NIN_KEYSELECT:
            toggle_visible();
            break;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            show_menu(1);
            break;
        default:
            break;
        }
        return 0;
    case WM_TIMER:
        if (w == TIMER_TICK && !g_hidden) render_if_changed(0);
        return 0;
    case WM_CW_BALANCE: {
        Texts t;
        build_texts(&t);              /* 鼠标悬停托盘图标时也能看到余额 */
        tray_tip(t.value, t.cd_value);
        if (!g_hidden) render_if_changed(1);
        return 0;
    }
    case WM_DESTROY:
        tray_remove();
        KillTimer(h, TIMER_TICK);
        if (g_ev_stop) SetEvent(g_ev_stop);
        if (g_thread) {
            WaitForSingleObject(g_thread, 3000);
            CloseHandle(g_thread);
            g_thread = NULL;
        }
        save_pos();
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

/* ---------------------------------------------------------------- 后台线程 */
static DWORD WINAPI WorkerProc(LPVOID)
{
    HANDLE waits[2] = { g_ev_stop, g_ev_refresh };
    for (;;) {
        DWORD ms = (DWORD)g_cfg.refresh_seconds * 1000;
        if (ms < 5000) ms = 5000;
        DWORD r = WaitForMultipleObjects(2, waits, FALSE, ms);
        if (r == WAIT_OBJECT_0) break;

        SYSTEMTIME attempt;
        GetLocalTime(&attempt);
        EnterCriticalSection(&g_lock);
        g_has_attempt = 1;
        g_attempt_at = attempt;
        if (!g_has_balance) g_loading = 1;
        LeaveCriticalSection(&g_lock);

        char cur[16] = {0}, tot[64] = {0}, gr[32] = {0}, tp[32] = {0}, err[512] = {0};
        int ok = cw_fetch_balance(g_cfg.api_key, g_cfg.api_base, g_cfg.use_proxy,
                                  cur, sizeof(cur), tot, sizeof(tot),
                                  gr, sizeof(gr), tp, sizeof(tp), err, sizeof(err));
        EnterCriticalSection(&g_lock);
        g_loading = 0;
        if (ok) {
            double nv = parse_total(tot);
            double ov = parse_total(g_total);
            if (g_has_balance && nv >= 0 && ov >= 0 && (nv - ov > 1e-9 || ov - nv > 1e-9)) {
                g_delta = nv - ov;
                g_has_delta = 1;
                GetLocalTime(&g_delta_at);
            }
            lstrcpynA(g_currency, cur[0] ? cur : "CNY", sizeof(g_currency));
            lstrcpynA(g_total, tot, sizeof(g_total));
            lstrcpynA(g_granted, gr, sizeof(g_granted));
            lstrcpynA(g_topped, tp, sizeof(g_topped));
            g_error[0] = 0;
            g_has_balance = 1;
            GetLocalTime(&g_updated_at);
            g_has_updated = 1;
        } else {
            lstrcpynA(g_error, err, sizeof(g_error));
        }
        LeaveCriticalSection(&g_lock);

        if (ok) cw_log("余额刷新成功: %s %s", cur, tot);
        else    cw_log("余额刷新失败: %s", err);
        PostMessageW(g_hwnd, WM_CW_BALANCE, 0, 0);
    }
    return 0;
}

/* ---------------------------------------------------------------- 入口 */
static void set_dpi_aware(void)
{
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    typedef BOOL (WINAPI *SetCtxFn)(void *);
    SetCtxFn fn = u32 ? (SetCtxFn)GetProcAddress(u32, "SetProcessDpiAwarenessContext") : NULL;
    if (fn && fn((void *)-4)) return;      /* PER_MONITOR_AWARE_V2 */
    SetProcessDPIAware();
}

static void do_check(void)
{
    cw_log("== 自检（原生 C 版）==");
    cw_log("程序目录: %s", g_dir);
    cw_log("API Key : %s", g_cfg.api_key[0] ? "已设置" : "未设置");
    cw_log("背景图  : %s", g_cfg.bg_image[0] ? g_cfg.bg_image : "(默认渐变)");
    cw_log("刷新间隔: %d 秒", g_cfg.refresh_seconds);
    for (int i = 0; i < g_cfg.exam_count; i++) {
        struct tm tmv;
        localtime_s(&tmv, &g_cfg.exams[i].t);
        cw_log("考试    : %s %04d-%02d-%02d %02d:%02d",
               g_cfg.exams[i].name, tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
               tmv.tm_hour, tmv.tm_min);
    }
    wchar_t as[1024];
    cw_log("开机自启动: %s", cw_autostart_get(as, 1024) ? "已开启" : "未开启");
    char cur[16] = {0}, tot[64] = {0}, gr[32] = {0}, tp[32] = {0}, err[512] = {0};
    int ok = cw_fetch_balance(g_cfg.api_key, g_cfg.api_base, g_cfg.use_proxy,
                              cur, sizeof(cur), tot, sizeof(tot),
                              gr, sizeof(gr), tp, sizeof(tp), err, sizeof(err));
    if (ok) cw_log("余额测试: 成功 %s %s（赠送 %s / 充值 %s）", cur, tot, gr, tp);
    else    cw_log("余额测试: 失败 %s", err);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmdline, int)
{
    g_inst = inst;
    cw_dir_of_exe();

    HANDLE mtx = CreateMutexW(NULL, TRUE, CW_MUTEX);
    int already = (mtx && GetLastError() == ERROR_ALREADY_EXISTS);

    cw_load_config();
    resolve_font_family();

    if (cmdline && wcsstr(cmdline, L"--check")) {
        do_check();
        return 0;
    }
    if (already) {
        cw_log("已有实例在运行，本次退出");
        return 0;
    }

    set_dpi_aware();
    cw_log("启动（原生 C 版）：逐像素透明，刷新间隔 %d 秒", g_cfg.refresh_seconds);

    GdiplusStartupInput gsi;
    if (GdiplusStartup(&g_gdiplus, &gsi, NULL) != Ok) {
        cw_log("GDI+ 初始化失败");
        return 1;
    }
    InitializeCriticalSection(&g_lock);

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"CWWidgetClass";
    if (!RegisterClassExW(&wc)) cw_log("注册窗口类失败: %lu", GetLastError());

    WNDCLASSEXW dc;
    memset(&dc, 0, sizeof(dc));
    dc.cbSize = sizeof(dc);
    dc.lpfnWndProc = KeyDlgProc;
    dc.hInstance = inst;
    dc.hCursor = LoadCursor(NULL, IDC_ARROW);
    dc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    dc.lpszClassName = L"CWKeyDlg";
    if (!RegisterClassExW(&dc)) cw_log("注册对话框类失败: %lu", GetLastError());

    int W = g_cfg.w, H = g_cfg.h;
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    int x, y;
    if (g_cfg.has_pos) { x = g_cfg.x; y = g_cfg.y; }
    else               { x = sw - W - 32; y = 48; }
    if (x < 60 - W) x = 60 - W;
    if (x > sw - 60) x = sw - 60;
    if (y < 0) y = 0;
    if (y > sh - 60) y = sh - 60;

    DWORD exstyle = WS_EX_LAYERED | WS_EX_TOOLWINDOW;
    if (g_cfg.topmost) exstyle |= WS_EX_TOPMOST;
    g_hwnd = CreateWindowExW(exstyle, L"CWWidgetClass", CW_APP_NAME, WS_POPUP,
                             x, y, W, H, NULL, NULL, inst, NULL);
    if (!g_hwnd) {
        cw_log("窗口创建失败: %lu", GetLastError());
        return 1;
    }
    ShowWindow(g_hwnd, SW_SHOWNA);
    render();
    tray_add();
    SetTimer(g_hwnd, TIMER_TICK, 1000, NULL);

    g_ev_refresh = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_ev_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_thread = CreateThread(NULL, 0, WorkerProc, NULL, 0, NULL);
    SetEvent(g_ev_refresh);

    if (cmdline && wcsstr(cmdline, L"--keydlg")) show_key_dialog();

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_ev_stop) CloseHandle(g_ev_stop);
    if (g_ev_refresh) CloseHandle(g_ev_refresh);
    free_bg_cache();
    if (g_dib) DeleteObject(g_dib);
    if (g_memdc) DeleteDC(g_memdc);
    GdiplusShutdown(g_gdiplus);
    cw_log("悬浮窗已退出");
    return 0;
}
