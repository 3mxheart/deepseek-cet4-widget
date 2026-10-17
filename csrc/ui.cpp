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
    IDM_CLEARBG, IDM_PICKTRAY, IDM_DEFAULTTRAY,
    IDM_OPACITY_UP, IDM_OPACITY_DOWN, IDM_LOCK, IDM_STARTUP, IDM_EXIT,
    IDM_HIDE, IDM_ONLY_CHARGED,
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

/* 每个服务商的运行状态（配置在 g_cfg.providers，这里只放易变的读数） */
typedef struct {
    int        has_total;
    char       total[64];      /* 接口原样返回的金额字符串 */
    char       prefix[16];     /* ¥ / $ */
    char       extra[128];     /* 副标题：赠送 / 充值 / 已用 */
    int        has_error;
    char       error[256];
    int        has_updated;
    SYSTEMTIME updated_at;
    time_t     charged_at;     /* 最近一次“掉钱”的时间，0 = 没掉过 */
    double     last_delta;     /* 最近一次变化量（负数） */
    int        has_delta;
} PState;

static PState     g_ps[CW_MAX_PROVIDERS];
static int        g_rows[CW_MAX_PROVIDERS];
static int        g_row_count = 0;
static int        g_any_attempt = 0;
static SYSTEMTIME g_attempt_at;

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
#define CW_MAX_ROWS 4

typedef struct {
    char name[40];
    char value[64];      /* 已带币种前缀，如 "$2.11" */
    char delta[24];      /* "-0.07" */
    int  delta_up;
    char extra[128];
    int  gray;
} Row;

typedef struct {
    Row  rows[CW_MAX_ROWS];
    int  row_count;
    char cd_value[32];
    char cd_detail[80];
    char status[192];
} Texts;

/* 选出要显示哪几行：默认只显示“最近扣过钱”的服务商 */
static void pick_rows(const PState *ps, int count, time_t now, int rows[], int *row_count)
{
    *row_count = 0;
    if (!g_cfg.show_only_charged) {
        for (int i = 0; i < count && *row_count < CW_MAX_ROWS; i++)
            if (g_cfg.providers[i].enabled) rows[(*row_count)++] = i;
    } else {
        long hold = (long)g_cfg.charged_hold_minutes * 60;
        int picked[CW_MAX_PROVIDERS];
        long long age[CW_MAX_PROVIDERS];
        int n = 0;
        for (int i = 0; i < count && i < CW_MAX_PROVIDERS; i++) {
            if (!g_cfg.providers[i].enabled || ps[i].charged_at == 0) continue;
            long long a = (long long)(now - ps[i].charged_at);
            if (hold > 0 && a > hold) continue;
            picked[n] = i;
            age[n] = a;
            n++;
        }
        /* 最近扣的排最前（插入排序，最多 6 个） */
        for (int i = 1; i < n; i++) {
            int key = picked[i];
            long long ka = age[i];
            int j = i - 1;
            while (j >= 0 && age[j] > ka) {
                picked[j + 1] = picked[j];
                age[j + 1] = age[j];
                j--;
            }
            picked[j + 1] = key;
            age[j + 1] = ka;
        }
        for (int i = 0; i < n && *row_count < CW_MAX_ROWS; i++)
            rows[(*row_count)++] = picked[i];
    }
    /* 一家都没扣过钱：退回显示第一家，窗口不会空着 */
    if (*row_count == 0 && count > 0) {
        int first = -1;
        for (int i = 0; i < count; i++)
            if (g_cfg.providers[i].enabled) { first = i; break; }
        rows[(*row_count)++] = (first >= 0) ? first : 0;
    }
}

static void build_texts(Texts *t)
{
    memset(t, 0, sizeof(*t));
    SYSTEMTIME now;
    GetLocalTime(&now);
    time_t nowt = time(NULL);

    PState ps[CW_MAX_PROVIDERS];
    int count = g_cfg.provider_count;
    if (count > CW_MAX_PROVIDERS) count = CW_MAX_PROVIDERS;
    EnterCriticalSection(&g_lock);
    memcpy(ps, g_ps, sizeof(g_ps));
    int any_attempt = g_any_attempt;
    SYSTEMTIME attempt = g_attempt_at;
    LeaveCriticalSection(&g_lock);

    pick_rows(ps, count, nowt, g_rows, &g_row_count);

    for (int r = 0; r < g_row_count; r++) {
        int i = g_rows[r];
        const Provider *p = &g_cfg.providers[i];
        Row *row = &t->rows[t->row_count++];
        lstrcpynA(row->name, p->name, sizeof(row->name));
        if (ps[i].has_total) {
            cw_fmt(row->value, sizeof(row->value), "%s%s", ps[i].prefix, ps[i].total);
            if (ps[i].has_error) lstrcpynA(row->extra, "刷新失败，显示上次读数", sizeof(row->extra));
            else                  lstrcpynA(row->extra, ps[i].extra, sizeof(row->extra));
        } else if (ps[i].has_error) {
            lstrcpynA(row->value, "--", sizeof(row->value));
            lstrcpynA(row->extra, ps[i].error, sizeof(row->extra));
            row->gray = 1;
        } else {
            lstrcpynA(row->value, "…", sizeof(row->value));
            lstrcpynA(row->extra, "正在查询…", sizeof(row->extra));
        }
        if (!g_cfg.show_balance_detail) row->extra[0] = 0;
        if (g_cfg.show_delta && ps[i].has_delta) {
            long long age = (long long)(nowt - ps[i].charged_at);
            if (g_cfg.delta_hold_seconds <= 0 || age <= g_cfg.delta_hold_seconds) {
                cw_fmt(row->delta, sizeof(row->delta), "%+.2f", ps[i].last_delta);
                row->delta_up = (ps[i].last_delta > 0);
            }
        }
    }

    cw_countdown(t->cd_value, sizeof(t->cd_value), t->cd_detail, sizeof(t->cd_detail));

    /* 底部状态行用“最新一次成功刷新”的时间 */
    int best = -1;
    long long best_age = 0;
    for (int i = 0; i < count; i++) {
        if (!ps[i].has_updated) continue;
        long long a = seconds_between(&ps[i].updated_at, &now);
        if (best < 0 || a < best_age) { best = i; best_age = a; }
    }
    if (g_cfg.show_status_line) {
        char every[32], t1[16];
        interval_text(every, sizeof(every));
        if (best >= 0) {
            hhmmss(&ps[best].updated_at, t1, sizeof(t1));
            cw_fmt(t->status, sizeof(t->status), "更新于 %s · %s刷新", t1, every);
        } else if (any_attempt) {
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
    Texts t;
    build_texts(&t);

    int W = g_cfg.w;
    int H = 84 + 28 * t.row_count;   /* 显示几行，窗口就多高（配置里的 height 作为下限） */
    if (H < g_cfg.h) H = g_cfg.h;

    RECT wrc;
    GetWindowRect(g_hwnd, &wrc);
    if ((wrc.bottom - wrc.top) != H || (wrc.right - wrc.left) != W)
        SetWindowPos(g_hwnd, NULL, 0, 0, W, H, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    ensure_surface(W, H);
    if (!g_bits) return;

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
        float pad = 16.0f;
        float label_sz = (float)g_cfg.label_size;
        float value_sz = (float)g_cfg.value_size;
        float detail_sz = (float)g_cfg.detail_size;
        float y = 8.0f;

        /* 服务商逐行：名字 + 变化量 在左，金额靠右 */
        for (int r = 0; r < t.row_count; r++) {
            Row *row = &t.rows[r];
            RectF rn(pad, y + 7.0f, 170.0f, label_sz + 8.0f);
            draw_line(&g, row->name, &rn, label_sz, FALSE, c_label,
                      StringAlignmentNear, TRUE);
            if (row->delta[0]) {
                float nw = measure_text(&g, row->name, label_sz, FALSE);
                float dx = pad + nw + 6.0f;
                if (dx < (float)W - 90.0f) {
                    Color c_delta = row->delta_up ? Color(126, 217, 160)
                                                  : Color(255, 156, 156);
                    RectF rd(dx, y + 7.0f, 86.0f, label_sz + 8.0f);
                    draw_line(&g, row->delta, &rd, label_sz, FALSE, c_delta,
                              StringAlignmentNear, TRUE);
                }
            }
            RectF rv(pad, y - 1.0f, (float)W - 2.0f * pad, value_sz + 10.0f);
            draw_line(&g, row->value, &rv, value_sz, TRUE,
                      row->gray ? c_label : c_value, StringAlignmentFar, TRUE);
            y += 28.0f;
        }

        /* 分隔线 */
        Pen pen(Color(38, 255, 255, 255), 1.0f);
        g.DrawLine(&pen, pad, y + 5.0f, (float)W - pad, y + 5.0f);

        /* 倒计时一行：左边标题+开考时间，右边大字 */
        char exam_label[64];
        cw_fmt(exam_label, sizeof(exam_label), "%s 倒计时",
               g_cfg.exam_count > 0 ? g_cfg.exams[0].name : "考试");
        float cy = y + 12.0f;
        RectF cl(pad, cy, 200.0f, label_sz + 8.0f);
        draw_line(&g, exam_label, &cl, label_sz, FALSE, c_label,
                  StringAlignmentNear, TRUE);
        if (t.cd_detail[0]) {
            RectF cd(pad, cy + 16.0f, 210.0f, detail_sz + 4.0f);
            draw_line(&g, t.cd_detail, &cd, detail_sz - 1.0f, FALSE, c_detail,
                      StringAlignmentNear, TRUE);
        }
        RectF cv(pad, cy - 5.0f, (float)W - 2.0f * pad, value_sz + 10.0f);
        draw_line(&g, t.cd_value, &cv, value_sz, TRUE, c_value,
                  StringAlignmentFar, TRUE);

        /* 底部状态行 */
        if (t.status[0]) {
            RectF rs(pad, (float)H - 12.0f - detail_sz, (float)W - 2.0f * pad,
                     detail_sz + 6.0f);
            draw_line(&g, t.status, &rs, detail_sz, FALSE, c_label,
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
    char key[2048];
    cw_fmt(key, sizeof(key), "%d|%s|%s|%s|%d|%d|%d",
           t.row_count, t.cd_value, t.cd_detail, t.status,
           g_cfg.w, g_cfg.h, g_cfg.value_size);
    for (int r = 0; r < t.row_count; r++) {
        size_t n = strlen(key);
        cw_fmt(key + n, (int)(sizeof(key) - n), "|%s=%s(%s)%s",
               t.rows[r].name, t.rows[r].value, t.rows[r].delta, t.rows[r].extra);
    }
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

/* 把下拉框里选中的服务商的 Key 读进输入框 */
static void dlg_load_key(HWND h)
{
    HWND combo = GetDlgItem(h, 2);
    int sel = combo ? (int)SendMessageW(combo, CB_GETCURSEL, 0, 0) : 0;
    if (sel < 0 || sel >= g_cfg.provider_count) sel = 0;
    wchar_t *kw = cw_w_from_u8(g_cfg.providers[sel].api_key);
    HWND ed = GetDlgItem(h, 1);
    if (ed) SetWindowTextW(ed, kw ? kw : L"");
    free(kw);
}

static LRESULT CALLBACK KeyDlgProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CREATE: {
        HWND lab1 = CreateWindowExW(0, L"STATIC", L"服务商",
                                    WS_CHILD | WS_VISIBLE, 16, 14, 60, 20, h, NULL, g_inst, NULL);
        HWND combo = CreateWindowExW(0, L"COMBOBOX", L"",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                                     CBS_DROPDOWNLIST,
                                     80, 10, 366, 240, h, (HMENU)2, g_inst, NULL);
        for (int i = 0; i < g_cfg.provider_count; i++) {
            wchar_t *wn = cw_w_from_u8(g_cfg.providers[i].name);
            SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)(wn ? wn : L"?"));
            free(wn);
        }
        SendMessageW(combo, CB_SETCURSEL, 0, 0);

        HWND lab2 = CreateWindowExW(0, L"STATIC", L"API Key",
                                    WS_CHILD | WS_VISIBLE, 16, 46, 60, 20, h, NULL, g_inst, NULL);
        HWND ed = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_PASSWORD,
                                  16, 68, 430, 26, h, (HMENU)1, g_inst, NULL);
        HWND hint = CreateWindowExW(0, L"STATIC",
                                    L"点“保存并测试”会立刻查一次；要加别的服务商，"
                                    L"在 config.json 的 providers 里添一项（type 可填 http）",
                                    WS_CHILD | WS_VISIBLE, 16, 100, 430, 34, h, NULL, g_inst, NULL);
        HWND ok = CreateWindowExW(0, L"BUTTON", L"保存并测试",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                  226, 140, 104, 30, h, (HMENU)IDM_IDC_KEY_OK, g_inst, NULL);
        HWND cancel = CreateWindowExW(0, L"BUTTON", L"取消",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                      342, 140, 104, 30, h, (HMENU)IDM_IDC_KEY_CANCEL, g_inst, NULL);
        HWND all[6] = { lab1, combo, lab2, ed, hint, ok };
        for (int i = 0; i < 6; i++) SendMessageW(all[i], WM_SETFONT, (WPARAM)ui_font(), TRUE);
        SendMessageW(cancel, WM_SETFONT, (WPARAM)ui_font(), TRUE);
        dlg_load_key(h);
        SetFocus(ed);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(w) == 2 && HIWORD(w) == CBN_SELCHANGE) {
            dlg_load_key(h);
            return 0;
        }
        if (LOWORD(w) == IDM_IDC_KEY_OK) {
            HWND combo = GetDlgItem(h, 2);
            int sel = (int)SendMessageW(combo, CB_GETCURSEL, 0, 0);
            if (sel < 0 || sel >= g_cfg.provider_count) sel = 0;
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
                    lstrcpynA(g_cfg.providers[sel].api_key, u8,
                              sizeof(g_cfg.providers[sel].api_key));
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
                               L"设置 API Key",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU,
                               rc.left - 60, rc.top + 40, 470, 215,
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

/* ---- 托盘图标本身：默认资源图标，也可指定 .ico / .png ---- */
static HICON g_icon_file = NULL;   /* 从文件加载的图标，需要 DestroyIcon */

/* PNG/BMP → 32×32 带 alpha 的 HICON */
static HICON icon_from_image(const wchar_t *path)
{
    Bitmap src(path);
    if (src.GetLastStatus() != Ok || src.GetWidth() <= 0) return NULL;

    const int S = 32;
    Bitmap scaled(S, S, PixelFormat32bppARGB);
    {
        Graphics g(&scaled);
        g.SetCompositingMode(CompositingModeSourceOver);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        g.Clear(Color(0, 0, 0, 0));
        g.DrawImage(&src, 0, 0, S, S);
    }

    Rect rc(0, 0, S, S);
    BitmapData bd;
    if (scaled.LockBits(&rc, ImageLockModeRead, PixelFormat32bppARGB, &bd) != Ok) return NULL;

    BITMAPV5HEADER bi;
    memset(&bi, 0, sizeof(bi));
    bi.bV5Size = sizeof(BITMAPV5HEADER);
    bi.bV5Width = S;
    bi.bV5Height = -S;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    HDC hdc = GetDC(NULL);
    void *bits = NULL;
    HBITMAP hbm = CreateDIBSection(hdc, (BITMAPINFO *)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HICON hIcon = NULL;
    if (hbm && bits) {
        for (int y = 0; y < S; y++)
            memcpy((BYTE *)bits + (size_t)y * S * 4,
                   (BYTE *)bd.Scan0 + (size_t)y * bd.Stride, (size_t)S * 4);
        HBITMAP hMask = CreateBitmap(S, S, 1, 1, NULL);
        ICONINFO ii;
        memset(&ii, 0, sizeof(ii));
        ii.fIcon = TRUE;
        ii.hbmMask = hMask;
        ii.hbmColor = hbm;
        hIcon = CreateIconIndirect(&ii);
        if (hMask) DeleteObject(hMask);
    }
    scaled.UnlockBits(&bd);
    if (hbm) DeleteObject(hbm);
    ReleaseDC(NULL, hdc);
    return hIcon;
}

static HICON build_tray_icon(void)
{
    if (g_icon_file) { DestroyIcon(g_icon_file); g_icon_file = NULL; }
    if (g_cfg.tray_icon[0]) {
        wchar_t *wp = cw_w_from_u8(g_cfg.tray_icon);
        if (wp) {
            HICON h = (HICON)LoadImageW(NULL, wp, IMAGE_ICON, 0, 0,
                                        LR_LOADFROMFILE | LR_DEFAULTSIZE);
            if (!h) h = icon_from_image(wp);
            if (h) g_icon_file = h;
            else   cw_log("托盘图标加载失败，改用内置图标: %s", g_cfg.tray_icon);
            free(wp);
        }
    }
    if (g_icon_file) return g_icon_file;
    HICON res = LoadIconW(g_inst, MAKEINTRESOURCEW(1));
    return res ? res : LoadIconW(NULL, IDI_APPLICATION);
}

static void tray_apply_icon(void)
{
    HICON h = build_tray_icon();
    if (!h || !g_nid.hWnd) return;
    g_nid.hIcon = h;
    g_nid.uFlags = NIF_ICON;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
}

static void pick_tray_icon(void)
{
    wchar_t file[MAX_PATH] = {0};
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"选择托盘图标（.ico 最清晰，.png 会缩放到 32×32）";
    ofn.lpstrFilter = L"图标 (*.ico;*.png;*.bmp)\0*.ico;*.png;*.bmp\0所有文件\0*.*\0\0";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) {
        char *u8 = cw_u8_from_w(file);
        if (u8) {
            lstrcpynA(g_cfg.tray_icon, u8, sizeof(g_cfg.tray_icon));
            free(u8);
            cw_save_config();
            tray_apply_icon();
            cw_log("托盘图标已更换: %s", g_cfg.tray_icon);
        }
    }
}

static void tray_add(void)
{
    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = TRAY_ID;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_CW_TRAY;
    g_nid.hIcon = build_tray_icon();
    lstrcpynW(g_nid.szTip, L"DeepSeek 余额 / CET-4 倒计时", 128);
    if (!Shell_NotifyIconW(NIM_ADD, &g_nid)) {
        cw_log("托盘图标添加失败: %lu", GetLastError());
        return;
    }
    g_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
    cw_log("托盘图标已添加（若任务栏没看到，点任务栏的 ^ 折叠区把它拖出来）");
}

static void tray_tip_text(const char *text)
{
    if (!g_nid.hWnd) return;
    wchar_t *w = cw_w_from_u8(text);
    lstrcpynW(g_nid.szTip, (w && w[0]) ? w : L"DeepSeek 余额", 128);
    free(w);
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
    AppendMenuW(menu, MF_STRING | (g_cfg.show_only_charged ? MF_CHECKED : 0),
                IDM_ONLY_CHARGED, L"只显示最近扣费的服务商");
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
    AppendMenuW(menu, MF_STRING, IDM_PICKTRAY, L"更换托盘图标…");
    AppendMenuW(menu, MF_STRING, IDM_DEFAULTTRAY, L"恢复默认托盘图标");
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
    case IDM_PICKTRAY:
        pick_tray_icon();
        break;
    case IDM_DEFAULTTRAY:
        g_cfg.tray_icon[0] = 0;
        cw_save_config();
        tray_apply_icon();
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
    case IDM_ONLY_CHARGED:
        g_cfg.show_only_charged = !g_cfg.show_only_charged;
        cw_save_config();
        render_if_changed(1);
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
        char tip[220] = "";
        for (int r = 0; r < t.row_count; r++) {
            size_t n = strlen(tip);
            cw_fmt(tip + n, (int)(sizeof(tip) - n), "%s%s %s",
                   r ? " · " : "", t.rows[r].name, t.rows[r].value);
        }
        {
            size_t n = strlen(tip);
            cw_fmt(tip + n, (int)(sizeof(tip) - n), "%s%s", n ? " · " : "", t.cd_value);
        }
        tray_tip_text(tip);
        if (!g_hidden) render_if_changed(0);
        return 0;
    }
    case WM_DESTROY:
        tray_remove();
        if (g_icon_file) { DestroyIcon(g_icon_file); g_icon_file = NULL; }
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
        g_any_attempt = 1;
        g_attempt_at = attempt;
        LeaveCriticalSection(&g_lock);

        /* 逐个服务商查，谁掉钱了谁就会被显示出来 */
        for (int i = 0; i < g_cfg.provider_count && i < CW_MAX_PROVIDERS; i++) {
            const Provider *p = &g_cfg.providers[i];
            if (!p->enabled) continue;

            char tot[64] = {0}, pfx[16] = {0}, extra[128] = {0}, err[256] = {0};
            int ok = cw_provider_fetch(p, g_cfg.use_proxy, tot, sizeof(tot),
                                       pfx, sizeof(pfx), extra, sizeof(extra),
                                       err, sizeof(err));
            EnterCriticalSection(&g_lock);
            PState *st = &g_ps[i];
            if (ok) {
                double nv = parse_total(tot);
                double ov = parse_total(st->total);
                if (st->has_total && nv >= 0 && ov >= 0 && ov - nv > 0.0049) {
                    st->last_delta = nv - ov;      /* 只有“掉钱”才算扣费 */
                    st->has_delta = 1;
                    st->charged_at = time(NULL);
                }
                lstrcpynA(st->total, tot, sizeof(st->total));
                lstrcpynA(st->prefix, pfx, sizeof(st->prefix));
                lstrcpynA(st->extra, extra, sizeof(st->extra));
                st->has_error = 0;
                st->has_total = 1;
                GetLocalTime(&st->updated_at);
                st->has_updated = 1;
            } else {
                lstrcpynA(st->error, err, sizeof(st->error));
                st->has_error = 1;
            }
            LeaveCriticalSection(&g_lock);

            if (ok) cw_log("余额刷新成功: %s %s%s", p->name, pfx, tot);
            else    cw_log("余额刷新失败: %s -> %s", p->name, err);
            PostMessageW(g_hwnd, WM_CW_BALANCE, 0, 0);
        }
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
    cw_log("服务商  : %d 个（%s）", g_cfg.provider_count,
           g_cfg.show_only_charged ? "只显示最近扣费的" : "显示全部");
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
    for (int i = 0; i < g_cfg.provider_count; i++) {
        const Provider *p = &g_cfg.providers[i];
        char tot[64] = {0}, pfx[16] = {0}, extra[128] = {0}, err[256] = {0};
        int ok = cw_provider_fetch(p, g_cfg.use_proxy, tot, sizeof(tot),
                                   pfx, sizeof(pfx), extra, sizeof(extra),
                                   err, sizeof(err));
        cw_log("余额测试: %s (%s) -> %s", p->name, p->type, ok ? "成功" : "失败");
        if (ok) cw_log("  %s%s  %s", pfx, tot, extra);
        else    cw_log("  %s", err);
    }
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
