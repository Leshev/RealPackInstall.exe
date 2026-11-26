// ============================================================================
//  RealPack Installer
//  Modern dark UI (Win32 + GDI+) and fast install engine.
//  Author: Leshev
// ============================================================================
#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <windowsx.h>
#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>
#include <urlmon.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <strsafe.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "resources.h"

using namespace Gdiplus;
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
//  Basics
// ---------------------------------------------------------------------------
static HINSTANCE   g_inst = nullptr;
static HWND        g_hwnd = nullptr;
static ULONG_PTR   g_gdiToken = 0;
static float       g_scale = 1.f;
static std::wstring g_targetDir;
static Bitmap*     g_icon = nullptr;

static const wchar_t* kClassName = L"RealPackInstaller.UI";
static const wchar_t* kTitleName = L"RealPack Installer";
static const wchar_t* kZipUrl    = L"https://github.com/Leshev/realpack/raw/refs/heads/main/realpack.zip";
static const wchar_t* kVerUrl    = L"https://github.com/Leshev/realpack/raw/refs/heads/main/version.txt";
static const wchar_t* kAppVer    = L"3.0";

static const float WIN_W = 660.f;
static const float WIN_H = 532.f;

// Palette -------------------------------------------------------------------
static const Color kInk     (255, 235, 244, 250);
static const Color kInkDim  (255, 141, 164, 180);
static const Color kInkMuted(255, 104, 128, 146);
static const Color kMuted2  (255, 96, 120, 138);
static const Color kAccentA (255, 64, 226, 168);
static const Color kAccentB (255, 45, 190, 214);
static const Color kDanger  (255, 248, 113, 113);
static const Color kSuccess (255, 110, 231, 183);

// Layout --------------------------------------------------------------------
static inline RectF RCard()        { return RectF(32.f, 154.f, 596.f, 244.f); }
static inline RectF RInstall()     { return RectF(32.f, 418.f, 288.f, 56.f); }
static inline RectF RCheck()       { return RectF(332.f, 418.f, 232.f, 56.f); }
static inline RectF RInfo()        { return RectF(576.f, 418.f, 52.f, 56.f); }
static inline RectF RClose()       { return RectF(WIN_W - 44.f, 12.f, 30.f, 30.f); }
static inline RectF RMin()         { return RectF(WIN_W - 78.f, 12.f, 30.f, 30.f); }
static inline RectF RLink()        { return RectF(56.f, 354.f, 240.f, 18.f); }
static inline RectF ROverlay()     { return RectF(95.f, 88.f, 470.f, 356.f); }
static inline RectF ROverlayBtn()  { return RectF(123.f, 378.f, 180.f, 46.f); }
static inline RectF ROverlayClose(){ return RectF(521.f, 104.f, 28.f, 28.f); }

// Messages ------------------------------------------------------------------
#define WM_RP_LOG       (WM_APP + 1)
#define WM_RP_PROGRESS  (WM_APP + 2)
#define WM_RP_PHASE     (WM_APP + 3)
#define WM_RP_VERSIONS  (WM_APP + 4)
#define WM_RP_FILEPROG  (WM_APP + 5)
#define WM_RP_DONE      (WM_APP + 6)
#define WM_RP_ERROR     (WM_APP + 7)
#define WM_RP_INSTALLED (WM_APP + 8)
#define WM_RP_START     (WM_APP + 9)

// ---------------------------------------------------------------------------
//  State
// ---------------------------------------------------------------------------
enum Phase { PH_IDLE, PH_CHECK, PH_DOWNLOAD, PH_EXTRACT, PH_INSTALL, PH_SUCCESS, PH_ERROR };

struct VersionInfo {
    std::wstring local, remote;
    bool hasLocal = false, hasRemote = false, newer = false;
};

static struct AppState {
    Phase phase = PH_IDLE;
    bool  busy = false;
    bool  overlay = false;
    std::deque<std::wstring> log;

    std::wstring localVer, remoteVer, error, notice;
    bool  hasLocal = false, hasRemote = false, updateAvailable = false;

    int   fileDone = 0, fileTotal = 0;
    float progress = 0.f;     // target 0..1
    float shown = 0.f;        // eased value
    float shim = 0.f;         // animation phase
    bool  indeterminate = false;

    std::wstring primaryLabel = L"Установить";
} st;

enum Ctl {
    C_INSTALL = 0, C_CHECK, C_INFO, C_CLOSE, C_MIN, C_LINK,
    C_OVERLAY_BTN, C_OVERLAY_CLOSE, C_COUNT
};

struct HoverAnim { float v = 0.f; };
static HoverAnim g_hover[C_COUNT];
static int  g_hoverId = -1;
static int  g_pressId = -1;
static bool g_timerOn = false;

// ---------------------------------------------------------------------------
//  Small helpers
// ---------------------------------------------------------------------------
static inline float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static Color Blend(const Color& a, const Color& b, float t) {
    t = Clamp(t, 0.f, 1.f);
    return Color(
        (int)(a.GetA() + (b.GetA() - a.GetA()) * t),
        (int)(a.GetR() + (b.GetR() - a.GetR()) * t),
        (int)(a.GetG() + (b.GetG() - a.GetG()) * t),
        (int)(a.GetB() + (b.GetB() - a.GetB()) * t));
}

static std::wstring Format(const wchar_t* fmt, ...) {
    wchar_t buf[1024] = {};
    va_list ap;
    va_start(ap, fmt);
    StringCchVPrintfW(buf, _countof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

static std::wstring FormatVersion(float v) {
    wchar_t buf[32] = {};
    float tenths = v * 10.f;
    if (fabsf(tenths - floorf(tenths + .5f)) < 0.01f)
        StringCchPrintfW(buf, 32, L"%.1f", v);
    else
        StringCchPrintfW(buf, 32, L"%.2f", v);
    return buf;
}

static inline bool InRect(const RectF& r, float x, float y) {
    return x >= r.X && x <= r.X + r.Width && y >= r.Y && y <= r.Y + r.Height;
}

// ---------------------------------------------------------------------------
//  GDI+ drawing helpers
// ---------------------------------------------------------------------------
static void RoundPath(GraphicsPath& p, const RectF& r, float rad) {
    if (rad < 0.5f || r.Width <= 0.f || r.Height <= 0.f) { p.AddRectangle(r); return; }
    float d = rad * 2.f;
    if (d > r.Width)  d = r.Width;
    if (d > r.Height) d = r.Height;
    p.AddArc(r.X, r.Y, d, d, 180.f, 90.f);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270.f, 90.f);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0.f, 90.f);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90.f, 90.f);
    p.CloseFigure();
}

static void FillRound(Graphics& g, const RectF& r, float rad, const Color& c) {
    if (r.Width <= 0.f || r.Height <= 0.f) return;
    GraphicsPath p;
    RoundPath(p, r, rad);
    SolidBrush b(c);
    g.FillPath(&b, &p);
}

static void FillRoundBrush(Graphics& g, const RectF& r, float rad, Brush* b) {
    if (r.Width <= 0.f || r.Height <= 0.f) return;
    GraphicsPath p;
    RoundPath(p, r, rad);
    g.FillPath(b, &p);
}

static void StrokeRound(Graphics& g, const RectF& r, float rad, const Color& c, float w = 1.f) {
    GraphicsPath p;
    RoundPath(p, r, rad);
    Pen pen(c, w);
    g.DrawPath(&pen, &p);
}

static void DrawShadow(Graphics& g, const RectF& r, float rad, int alpha) {
    for (int i = 4; i >= 1; --i) {
        RectF rr(r.X - i, r.Y + i * 0.7f, r.Width + 2.f * i, r.Height + 2.f * i);
        FillRound(g, rr, rad + i, Color(alpha / (i * 2), 0, 0, 0));
    }
}

static void DrawStr(Graphics& g, const std::wstring& s, Font* f, const RectF& r, const Color& c,
                    StringAlignment ha = StringAlignmentNear, StringAlignment va = StringAlignmentNear,
                    bool wrap = false, StringTrimming trim = StringTrimmingNone) {
    if (s.empty() || !f) return;
    StringFormat fmt;
    fmt.SetAlignment(ha);
    fmt.SetLineAlignment(va);
    if (!wrap) fmt.SetFormatFlags(StringFormatFlagsNoWrap);
    if (trim != StringTrimmingNone) fmt.SetTrimming(trim);
    SolidBrush brush(c);
    g.DrawString(s.c_str(), (INT)s.size(), f, r, &fmt, &brush);
}

static float MeasureW(Graphics& g, const std::wstring& s, Font* f) {
    if (!f) return 0.f;
    StringFormat fmt;
    fmt.SetFormatFlags(StringFormatFlagsNoWrap);
    RectF box;
    g.MeasureString(s.c_str(), (INT)s.size(), f, RectF(0, 0, 2000, 200), &fmt, &box);
    return box.Width;
}

static void GradientText(Graphics& g, const std::wstring& s, const FontFamily* fam, float em,
                         const RectF& r, const Color& c1, const Color& c2,
                         StringAlignment ha = StringAlignmentNear) {
    if (!fam || s.empty()) return;
    GraphicsPath path;
    StringFormat fmt;
    fmt.SetAlignment(ha);
    fmt.SetLineAlignment(StringAlignmentNear);
    fmt.SetFormatFlags(StringFormatFlagsNoWrap);
    path.AddString(s.c_str(), (INT)s.size(), fam, FontStyleRegular, em, r, &fmt);
    LinearGradientBrush b(r, c1, c2, LinearGradientModeHorizontal);
    g.FillPath(&b, &path);
}

static void DrawGlow(Graphics& g, float cx, float cy, float rx, float ry, const Color& c) {
    GraphicsPath p;
    p.AddEllipse(cx - rx, cy - ry, rx * 2.f, ry * 2.f);
    PathGradientBrush b(&p);
    b.SetCenterColor(c);
    Color edge(0, c.GetR(), c.GetG(), c.GetB());
    int count = 1;
    b.SetSurroundColors(&edge, &count);
    g.FillPath(&b, &p);
}

// ---------------------------------------------------------------------------
//  Fonts
// ---------------------------------------------------------------------------
static FontFamily* g_famUiOwn = nullptr;
static FontFamily* g_famSemiOwn = nullptr;
static FontFamily* g_famMonoOwn = nullptr;
static const FontFamily* g_famUi = nullptr;
static const FontFamily* g_famSemi = nullptr;
static const FontFamily* g_famMono = nullptr;

static Font* g_fBrand = nullptr;
static Font* g_fTbTitle = nullptr;
static Font* g_fTbSub = nullptr;
static Font* g_fH1 = nullptr;
static Font* g_fSub = nullptr;
static Font* g_fChip = nullptr;
static Font* g_fChipV = nullptr;
static Font* g_fPct = nullptr;
static Font* g_fMono = nullptr;
static Font* g_fBtn = nullptr;
static Font* g_fTiny = nullptr;
static Font* g_fLink = nullptr;
static Font* g_fInfoTitle = nullptr;
static Font* g_fInfoBody = nullptr;

static void InitFonts() {
    g_famUiOwn = new FontFamily(L"Segoe UI");
    if (g_famUiOwn->GetLastStatus() != Ok) { delete g_famUiOwn; g_famUiOwn = nullptr; }
    g_famUi = g_famUiOwn ? (const FontFamily*)g_famUiOwn : FontFamily::GenericSansSerif();

    g_famSemiOwn = new FontFamily(L"Segoe UI Semibold");
    if (g_famSemiOwn->GetLastStatus() != Ok) { delete g_famSemiOwn; g_famSemiOwn = nullptr; }
    g_famSemi = g_famSemiOwn ? (const FontFamily*)g_famSemiOwn : g_famUi;

    g_famMonoOwn = new FontFamily(L"Consolas");
    if (g_famMonoOwn->GetLastStatus() != Ok) { delete g_famMonoOwn; g_famMonoOwn = nullptr; }
    g_famMono = g_famMonoOwn ? (const FontFamily*)g_famMonoOwn : FontFamily::GenericMonospace();

    g_fBrand     = new Font(g_famSemi, 44.f, FontStyleRegular, UnitPixel);
    g_fTbTitle   = new Font(g_famSemi, 12.5f, FontStyleRegular, UnitPixel);
    g_fTbSub     = new Font(g_famUi, 9.5f, FontStyleRegular, UnitPixel);
    g_fH1        = new Font(g_famSemi, 17.5f, FontStyleRegular, UnitPixel);
    g_fSub       = new Font(g_famUi, 11.5f, FontStyleRegular, UnitPixel);
    g_fChip      = new Font(g_famUi, 10.5f, FontStyleRegular, UnitPixel);
    g_fChipV     = new Font(g_famSemi, 11.f, FontStyleRegular, UnitPixel);
    g_fPct       = new Font(g_famSemi, 26.f, FontStyleRegular, UnitPixel);
    g_fMono      = new Font(g_famMono, 10.f, FontStyleRegular, UnitPixel);
    g_fBtn       = new Font(g_famSemi, 14.f, FontStyleRegular, UnitPixel);
    g_fTiny      = new Font(g_famUi, 9.5f, FontStyleRegular, UnitPixel);
    g_fLink      = new Font(g_famUi, 10.5f, FontStyleRegular, UnitPixel);
    g_fInfoTitle = new Font(g_famSemi, 21.f, FontStyleRegular, UnitPixel);
    g_fInfoBody  = new Font(g_famUi, 12.f, FontStyleRegular, UnitPixel);
}

static void FreeFonts() {
    Font* fonts[] = { g_fBrand, g_fTbTitle, g_fTbSub, g_fH1, g_fSub, g_fChip, g_fChipV,
                      g_fPct, g_fMono, g_fBtn, g_fTiny, g_fLink, g_fInfoTitle, g_fInfoBody };
    for (Font* f : fonts) delete f;
    delete g_famUiOwn;
    delete g_famSemiOwn;
    delete g_famMonoOwn;
}

// ---------------------------------------------------------------------------
//  Status text
// ---------------------------------------------------------------------------
static void StatusText(std::wstring& head, std::wstring& sub, Color& col) {
    head.clear(); sub.clear();
    switch (st.phase) {
    case PH_CHECK:
        head = L"Проверяем обновления";
        sub = L"Сверяем локальную версию с сервером…";
        col = kInk;
        break;
    case PH_DOWNLOAD:
        head = L"Скачиваем архив";
        sub = L"Загрузка с GitHub — обычно пара секунд";
        col = kAccentA;
        break;
    case PH_EXTRACT:
        head = L"Распаковываем архив";
        sub = L"Готовим файлы плагина";
        col = kAccentA;
        break;
    case PH_INSTALL:
        head = L"Обновляем файлы";
        sub = st.fileTotal > 0 ? Format(L"Файл %d из %d", st.fileDone, st.fileTotal)
                               : std::wstring(L"Заменяем файлы плагина");
        col = kAccentA;
        break;
    case PH_SUCCESS:
        head = L"Установка завершена";
        sub = st.hasLocal ? (L"RealPack " + st.localVer + L" готов к работе")
                          : std::wstring(L"Файлы плагина обновлены");
        col = kSuccess;
        break;
    case PH_ERROR:
        head = L"Не удалось завершить";
        sub = st.error.empty() ? std::wstring(L"Попробуйте ещё раз") : st.error;
        col = kDanger;
        break;
    default:
        if (st.updateAvailable) {
            head = L"Доступно обновление";
            sub = L"В сети версия " + st.remoteVer + L" — нажмите «Обновить»";
            col = kAccentA;
        } else if (st.hasLocal) {
            head = L"Установлена актуальная версия";
            sub = L"RealPack " + st.localVer + L" · можно переустановить";
            col = kInk;
        } else {
            head = L"Готово к установке";
            sub = L"Нажмите «Установить», чтобы поставить RealPack";
            col = kInk;
        }
        if (!st.notice.empty()) sub = st.notice;
        break;
    }
}

// ---------------------------------------------------------------------------
//  Drawing
// ---------------------------------------------------------------------------
static void DrawCaptionButton(Graphics& g, const RectF& r, int id, bool close) {
    float t = g_hover[id].v;
    float cx = r.X + r.Width * .5f;
    float cy = r.Y + r.Height * .5f;
    if (t > .01f) {
        Color fill = close ? Color((int)(185 * t), 232, 78, 82)
                           : Color((int)(85 * t), 92, 122, 146);
        SolidBrush b(fill);
        g.FillEllipse(&b, cx - 14.f, cy - 14.f, 28.f, 28.f);
    }
    Color glyph = close ? Color(255, 152, 174, 188) : Color(255, 139, 162, 178);
    if (t > .01f) glyph = close ? Color(255, 255, 246, 246) : Color(255, 224, 238, 247);
    Pen pen(glyph, 1.6f);
    if (close) {
        g.DrawLine(&pen, cx - 4.5f, cy - 4.5f, cx + 4.5f, cy + 4.5f);
        g.DrawLine(&pen, cx + 4.5f, cy - 4.5f, cx - 4.5f, cy + 4.5f);
    } else {
        g.DrawLine(&pen, cx - 5.f, cy + .5f, cx + 5.f, cy + .5f);
    }
}

static void DrawTitleBar(Graphics& g) {
    if (g_icon) g.DrawImage(g_icon, RectF(24.f, 12.f, 28.f, 28.f));
    DrawStr(g, L"RealPack", g_fTbTitle, RectF(61.f, 8.f, 240.f, 20.f), kInk);
    DrawStr(g, L"installer · " + std::wstring(kAppVer), g_fTbSub, RectF(61.f, 27.f, 240.f, 12.f), kMuted2);
    DrawCaptionButton(g, RMin(), C_MIN, false);
    DrawCaptionButton(g, RClose(), C_CLOSE, true);
}

static void DrawHero(Graphics& g) {
    GradientText(g, L"RealPack", g_famSemi, 44.f, RectF(32.f, 56.f, 300.f, 64.f),
                 Color(255, 244, 252, 249), Color(255, 78, 222, 172));
    DrawStr(g, L"Плагин-пак для Minecraft · установка и автообновление", g_fSub,
            RectF(33.f, 123.f, 580.f, 18.f), kInkDim);
}

static float DrawChip(Graphics& g, float x, float y, const std::wstring& label,
                      const std::wstring& value, const Color& dot) {
    const float h = 26.f;
    StringFormat fmt;
    fmt.SetFormatFlags(StringFormatFlagsNoWrap);
    fmt.SetLineAlignment(StringAlignmentCenter);
    RectF lb, vb;
    g.MeasureString(label.c_str(), (INT)label.size(), g_fChip, RectF(0, 0, 400, h), &fmt, &lb);
    g.MeasureString(value.c_str(), (INT)value.size(), g_fChipV, RectF(0, 0, 400, h), &fmt, &vb);

    float w = 12.f + 7.f + 6.f + lb.Width + 6.f + vb.Width + 12.f;
    RectF chip(x, y, w, h);
    FillRound(g, chip, h * .5f, Color(255, 20, 33, 44));
    StrokeRound(g, chip, h * .5f, Color(255, 38, 55, 70));

    SolidBrush db(dot);
    g.FillEllipse(&db, x + 13.f, y + h * .5f - 3.5f, 7.f, 7.f);

    DrawStr(g, label, g_fChip, RectF(x + 24.f, y, lb.Width + 2.f, h), Color(255, 124, 148, 166),
            StringAlignmentNear, StringAlignmentCenter);
    DrawStr(g, value, g_fChipV, RectF(x + 24.f + lb.Width + 6.f, y, vb.Width + 2.f, h),
            Color(255, 226, 238, 247), StringAlignmentNear, StringAlignmentCenter);
    return w;
}

static void DrawCard(Graphics& g) {
    RectF card = RCard();
    DrawShadow(g, card, 16.f, 120);
    FillRound(g, card, 16.f, Color(255, 17, 27, 36));
    StrokeRound(g, card, 16.f, Color(255, 33, 49, 62));
    FillRound(g, RectF(card.X + 26.f, card.Y + 1.f, card.Width - 52.f, 1.f), .5f,
              Color(30, 96, 240, 205));

    float x = 56.f;
    x += DrawChip(g, x, 174.f, L"Локально", st.hasLocal ? st.localVer : L"—",
                  st.hasLocal ? kAccentA : kMuted2);
    x += 10.f;
    DrawChip(g, x, 174.f, L"В сети", st.hasRemote ? st.remoteVer : L"—",
             st.hasRemote ? kAccentB : kMuted2);

    bool showPct = (st.phase == PH_DOWNLOAD || st.phase == PH_INSTALL || st.phase == PH_SUCCESS);
    if (showPct) {
        int pct = (int)Clamp(st.shown * 100.f + .5f, 0.f, 100.f);
        wchar_t b[16] = {};
        StringCchPrintfW(b, 16, L"%d%%", pct);
        DrawStr(g, b, g_fPct, RectF(452.f, 163.f, 152.f, 38.f), kAccentA,
                StringAlignmentFar, StringAlignmentCenter);
    }

    std::wstring head, sub;
    Color headCol = kInk, dotCol = kAccentA;
    StatusText(head, sub, headCol);
    if (st.phase == PH_IDLE && !st.updateAvailable) dotCol = Color(255, 92, 160, 190);
    if (st.phase == PH_SUCCESS) dotCol = kSuccess;
    if (st.phase == PH_ERROR) dotCol = kDanger;
    if (st.busy) {
        float p = .5f + .5f * sinf(st.shim * 6.2831853f * 2.f);
        int a = (int)(110.f + 130.f * p);
        dotCol = Color(a, dotCol.GetR(), dotCol.GetG(), dotCol.GetB());
    }
    {
        SolidBrush db(dotCol);
        g.FillEllipse(&db, 56.f, 218.f, 8.f, 8.f);
    }
    DrawStr(g, head, g_fH1, RectF(72.f, 208.f, 520.f, 26.f), headCol,
            StringAlignmentNear, StringAlignmentCenter);
    DrawStr(g, sub, g_fSub, RectF(72.f, 232.f, 520.f, 18.f), kInkDim,
            StringAlignmentNear, StringAlignmentCenter, false, StringTrimmingEllipsisCharacter);

    // progress
    RectF track(56.f, 258.f, 548.f, 8.f);
    if (st.busy || st.shown > .002f) {
        FillRound(g, track, 4.f, Color(255, 25, 38, 50));
        if (st.indeterminate && st.busy) {
            float segW = 170.f;
            float pos = st.shim * (track.Width + segW) - segW;
            RectF seg(track.X + pos, track.Y, segW, track.Height);
            GraphicsState gs = g.Save();
            GraphicsPath clip;
            RoundPath(clip, track, 4.f);
            g.SetClip(&clip);
            LinearGradientBrush b(seg, Color(0, 90, 230, 180), Color(255, 90, 230, 180),
                                  LinearGradientModeHorizontal);
            g.FillRectangle(&b, seg);
            g.Restore(gs);
        } else {
            float w = track.Width * Clamp(st.shown, 0.f, 1.f);
            if (w > 1.f) {
                RectF fr(track.X, track.Y, w, track.Height);
                LinearGradientBrush b(fr, kAccentA, kAccentB, LinearGradientModeHorizontal);
                FillRoundBrush(g, fr, 4.f, &b);
            }
        }
    }

    // log
    int n = (int)st.log.size();
    int show = n < 4 ? n : 4;
    for (int i = 0; i < show; ++i) {
        const std::wstring& line = st.log[n - show + i];
        float top = 332.f - (float)(show - 1 - i) * 18.f;
        int age = show - 1 - i;
        Color c(255, 120 - age * 18, 146 - age * 18, 164 - age * 18);
        if (age == 0) c = Color(255, 158, 184, 200);
        DrawStr(g, line, g_fMono, RectF(56.f, top, 548.f, 16.f), c,
                StringAlignmentNear, StringAlignmentCenter, false, StringTrimmingEllipsisCharacter);
    }

    // folder link
    if (!g_targetDir.empty()) {
        float t = g_hover[C_LINK].v;
        Color c = Blend(kMuted2, kAccentA, t);
        const std::wstring label = L"Открыть папку плагина";
        DrawStr(g, label, g_fLink, RLink(), c, StringAlignmentNear, StringAlignmentCenter);
        if (t > .05f) {
            float w = MeasureW(g, label, g_fLink);
            Pen pen(Color((int)(150 * t), c.GetR(), c.GetG(), c.GetB()), 1.f);
            g.DrawLine(&pen, 56.f, 369.f, 56.f + w - 4.f, 369.f);
        }
    }
}

static void DrawPrimary(Graphics& g, RectF r, const std::wstring& label, float t,
                        bool enabled, float load, bool pressed) {
    if (pressed) r.Y += 1.f;
    if (enabled) {
        FillRound(g, RectF(r.X + 6.f, r.Y + 10.f, r.Width - 12.f, r.Height - 4.f), 16.f,
                  Color(22, 48, 214, 168));
        Color c1 = Blend(Color(255, 62, 222, 170), Color(255, 88, 240, 188), t);
        Color c2 = Blend(Color(255, 39, 175, 207), Color(255, 62, 208, 232), t);
        if (pressed) {
            c1 = Blend(c1, Color(255, 0, 0, 0), .10f);
            c2 = Blend(c2, Color(255, 0, 0, 0), .10f);
        }
        LinearGradientBrush bg(r, c1, c2, LinearGradientModeHorizontal);
        FillRoundBrush(g, r, 14.f, &bg);
        FillRound(g, RectF(r.X + 1.f, r.Y + 1.f, r.Width - 2.f, r.Height * .5f), 13.f,
                  Color(24, 255, 255, 255));
    } else {
        FillRound(g, r, 14.f, Color(255, 20, 32, 43));
        StrokeRound(g, r, 14.f, Color(255, 34, 50, 64));
        if (load > .01f) {
            GraphicsState gs = g.Save();
            GraphicsPath cp;
            RoundPath(cp, r, 14.f);
            g.SetClip(&cp);
            SolidBrush sb(Color(48, 70, 225, 175));
            g.FillRectangle(&sb, RectF(r.X, r.Y, r.Width * Clamp(load, 0.f, 1.f), r.Height));
            g.Restore(gs);
        }
    }
    Color txt = enabled ? Color(255, 7, 27, 23) : Color(255, 112, 136, 154);
    DrawStr(g, label, g_fBtn, r, txt, StringAlignmentCenter, StringAlignmentCenter);
}

static void DrawSecondary(Graphics& g, RectF r, const std::wstring& label, float t,
                          bool enabled, bool pressed) {
    if (pressed) r.Y += 1.f;
    Color fill = enabled ? Blend(Color(255, 17, 28, 38), Color(255, 24, 39, 51), t)
                         : Color(255, 16, 25, 34);
    FillRound(g, r, 14.f, fill);
    StrokeRound(g, r, 14.f, enabled ? Blend(Color(255, 40, 58, 72), Color(255, 66, 100, 122), t)
                                    : Color(255, 30, 44, 56));
    Color txt = enabled ? Blend(Color(255, 198, 216, 230), Color(255, 236, 248, 255), t)
                        : Color(255, 92, 114, 130);
    DrawStr(g, label, g_fBtn, r, txt, StringAlignmentCenter, StringAlignmentCenter);
}

static void DrawButtons(Graphics& g) {
    float ti = g_hover[C_INSTALL].v;
    float tc = g_hover[C_CHECK].v;
    float tf = g_hover[C_INFO].v;

    DrawPrimary(g, RInstall(), st.primaryLabel, ti, !st.busy, st.shown, g_pressId == C_INSTALL);
    DrawSecondary(g, RCheck(), L"Проверить обновления", tc, !st.busy, g_pressId == C_CHECK);

    RectF ri = RInfo();
    FillRound(g, ri, 14.f, Blend(Color(255, 18, 30, 40), Color(255, 26, 42, 55), tf));
    StrokeRound(g, ri, 14.f, Blend(Color(255, 38, 55, 69), Color(255, 58, 88, 108), tf));
    DrawStr(g, L"i", g_fBtn, ri, Blend(kInkDim, kInk, tf), StringAlignmentCenter, StringAlignmentCenter);
}

static void DrawFooter(Graphics& g) {
    std::wstring path = g_targetDir.empty() ? L"Папка плагина не определена" : g_targetDir;
    DrawStr(g, path, g_fTiny, RectF(32.f, 486.f, 400.f, 14.f), Color(255, 73, 95, 111),
            StringAlignmentNear, StringAlignmentNear, false, StringTrimmingEllipsisPath);
    DrawStr(g, L"RealPack " + std::wstring(kAppVer) + L" · Leshev", g_fTiny,
            RectF(452.f, 486.f, 176.f, 14.f), Color(255, 73, 95, 111),
            StringAlignmentFar, StringAlignmentNear);
}

static void DrawOverlay(Graphics& g) {
    SolidBrush scrim(Color(216, 5, 9, 14));
    g.FillRectangle(&scrim, 0.f, 0.f, WIN_W, WIN_H);

    RectF c = ROverlay();
    DrawShadow(g, c, 18.f, 150);
    FillRound(g, c, 18.f, Color(255, 19, 30, 40));
    StrokeRound(g, c, 18.f, Color(255, 45, 65, 82));

    DrawStr(g, L"О программе", g_fInfoTitle, RectF(123.f, 112.f, 330.f, 32.f), kInk);
    DrawStr(g, L"RealPack Installer · сборка " + std::wstring(kAppVer), g_fTbSub,
            RectF(123.f, 146.f, 330.f, 16.f), kMuted2);
    FillRound(g, RectF(123.f, 174.f, 414.f, 1.f), .5f, Color(255, 38, 54, 69));

    std::wstring body =
        L"RealPack — плагин-пак для Minecraft и лаунчера MCGL.\n\n"
        L"«Установить» скачивает свежий архив с GitHub, распаковывает его и заменяет файлы плагина в вашем профиле.\n\n"
        L"«Проверить обновления» сверяет локальную версию с сервером.\n\n"
        L"Поддержка: Leshev на форуме MCGL.";
    DrawStr(g, body, g_fInfoBody, RectF(123.f, 190.f, 414.f, 176.f), kInkDim,
            StringAlignmentNear, StringAlignmentNear, true);

    DrawPrimary(g, ROverlayBtn(), L"Понятно", g_hover[C_OVERLAY_BTN].v, true, 0.f,
                g_pressId == C_OVERLAY_BTN);
    DrawCaptionButton(g, ROverlayClose(), C_OVERLAY_CLOSE, true);
}

static void DrawWindow(Graphics& g) {
    LinearGradientBrush bg(RectF(0.f, 0.f, WIN_W, WIN_H), Color(255, 8, 13, 19),
                           Color(255, 15, 25, 33), LinearGradientModeVertical);
    g.FillRectangle(&bg, 0.f, 0.f, WIN_W, WIN_H);

    DrawGlow(g, WIN_W - 40.f, -120.f, 300.f, 250.f, Color(34, 40, 214, 172));
    DrawGlow(g, 20.f, WIN_H + 60.f, 280.f, 200.f, Color(26, 36, 128, 205));

    StrokeRound(g, RectF(.5f, .5f, WIN_W - 1.f, WIN_H - 1.f), 18.f,
                Color(255, 30, 46, 58));

    DrawTitleBar(g);
    DrawHero(g);
    DrawCard(g);
    DrawButtons(g);
    DrawFooter(g);
    if (st.overlay) DrawOverlay(g);
}

// ---------------------------------------------------------------------------
//  Animation
// ---------------------------------------------------------------------------
static bool NeedAnim() {
    for (int i = 0; i < C_COUNT; ++i) {
        float target = (g_hoverId == i) ? 1.f : 0.f;
        if (fabsf(g_hover[i].v - target) > 0.004f) return true;
    }
    if (fabsf(st.shown - st.progress) > 0.002f) return true;
    if (st.busy) return true;
    return false;
}

static void EnsureTimer(HWND h) {
    if (!g_timerOn && h) {
        SetTimer(h, 1, 16, nullptr);
        g_timerOn = true;
    }
}

static void StopTimerIfIdle(HWND h) {
    if (g_timerOn && !NeedAnim()) {
        KillTimer(h, 1);
        g_timerOn = false;
    }
}

// ---------------------------------------------------------------------------
//  Install engine
// ---------------------------------------------------------------------------
static void PostLog(const std::wstring& s) {
    auto* p = new std::wstring(s);
    if (!PostMessageW(g_hwnd, WM_RP_LOG, 0, (LPARAM)p)) delete p;
}

static void PostPhase(Phase p) { PostMessageW(g_hwnd, WM_RP_PHASE, (WPARAM)p, 0); }

static void PostError(const std::wstring& s) {
    auto* p = new std::wstring(s);
    if (!PostMessageW(g_hwnd, WM_RP_ERROR, 0, (LPARAM)p)) delete p;
}

static std::wstring GetTargetDir() {
    wchar_t profile[MAX_PATH] = {};
    if (SHGetFolderPathW(nullptr, CSIDL_PROFILE, nullptr, SHGFP_TYPE_CURRENT, profile) != S_OK)
        return L"";
    std::wstring dir = std::wstring(profile) +
        L"\\AppData\\Roaming\\MCGL\\MinecraftLauncher2\\repository\\mclient\\plugins\\realpack";
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return L"";
    return dir;
}

static bool ReadVersion(const std::wstring& path, float& out) {
    std::ifstream f{ fs::path(path) };
    if (!f.is_open()) return false;
    std::string line;
    if (!std::getline(f, line)) return false;
    try {
        size_t pos = line.find_first_not_of(" \t\r\n");
        if (pos == std::string::npos) return false;
        out = std::stof(line.substr(pos));
        return true;
    } catch (...) {
        return false;
    }
}

class DlCallback : public IBindStatusCallback {
public:
    DlCallback(HWND h, DWORD timeoutMs, bool report)
        : hwnd_(h), timeout_(timeoutMs), report_(report), start_(GetTickCount64()) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IBindStatusCallback) {
            *ppv = static_cast<IBindStatusCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&refs_); }
    STDMETHODIMP_(ULONG) Release() override { return (ULONG)InterlockedDecrement(&refs_); }

    STDMETHODIMP OnStartBinding(DWORD, IBinding*) override { return S_OK; }
    STDMETHODIMP GetPriority(LONG* p) override { if (p) *p = THREAD_PRIORITY_NORMAL; return S_OK; }
    STDMETHODIMP OnLowResource(DWORD) override { return S_OK; }

    STDMETHODIMP OnProgress(ULONG cur, ULONG max, ULONG, LPCWSTR) override {
        if (timeout_ && GetTickCount64() - start_ > timeout_) return E_ABORT;
        if (!report_) return S_OK;
        if (max > 0) {
            int pm = (int)((unsigned long long)cur * 1000ull / max);
            if (pm != last_) {
                last_ = pm;
                PostMessageW(hwnd_, WM_RP_PROGRESS, (WPARAM)pm, 0);
            }
        } else if (!unknown_) {
            unknown_ = true;
            PostMessageW(hwnd_, WM_RP_PROGRESS, (WPARAM)-1, 0);
        }
        return S_OK;
    }

    STDMETHODIMP OnStopBinding(HRESULT, LPCWSTR) override { return S_OK; }
    STDMETHODIMP GetBindInfo(DWORD* f, BINDINFO* info) override {
        if (f) *f = 0;
        if (info) ZeroMemory(info, sizeof(*info));
        return S_OK;
    }
    STDMETHODIMP OnDataAvailable(DWORD, DWORD, FORMATETC*, STGMEDIUM*) override { return S_OK; }
    STDMETHODIMP OnObjectAvailable(REFIID, IUnknown*) override { return S_OK; }

private:
    LONG refs_ = 1;
    HWND hwnd_;
    DWORD timeout_;
    bool  report_;
    ULONGLONG start_;
    int   last_ = -1;
    bool  unknown_ = false;
};

static bool Download(const std::wstring& url, const std::wstring& path, DWORD timeoutMs,
                     bool report, std::wstring& err) {
    DeleteFileW(path.c_str());
    auto* cb = new DlCallback(g_hwnd, timeoutMs, report);
    cb->AddRef();
    HRESULT hr = URLDownloadToFileW(nullptr, url.c_str(), path.c_str(), 0, cb);
    cb->Release();
    cb->Release();

    if (FAILED(hr)) {
        if (hr == E_ABORT || hr == HRESULT_FROM_WIN32(ERROR_CANCELLED))
            err = L"Сервер не отвечает. Проверьте подключение к интернету";
        else
            err = Format(L"Ошибка загрузки (0x%08X)", (unsigned)hr);
        return false;
    }
    std::error_code ec;
    auto sz = fs::file_size(path, ec);
    if (ec || sz == 0) {
        err = L"Сервер вернул пустой файл";
        return false;
    }
    return true;
}

static bool RunAndWait(const std::wstring& cmdline) {
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    std::wstring cmd = cmdline;
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                        nullptr, nullptr, &si, &pi))
        return false;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}

static bool ExtractZip(const std::wstring& zip, const std::wstring& dest) {
    std::error_code ec;
    fs::create_directories(dest, ec);

    // Fast path: bsdtar ships with Windows 10 1803+ and unpacks zip natively.
    wchar_t sys[MAX_PATH] = {};
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring tar = std::wstring(sys) + L"\\tar.exe";
    if (GetFileAttributesW(tar.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (RunAndWait(L"\"" + tar + L"\" -xf \"" + zip + L"\" -C \"" + dest + L"\""))
            return true;
    }
    // Fallback: PowerShell Expand-Archive.
    std::wstring ps = L"powershell.exe -NoProfile -NonInteractive -WindowStyle Hidden -Command "
                      L"\"Expand-Archive -LiteralPath '" + zip + L"' -DestinationPath '" + dest +
                      L"' -Force\"";
    return RunAndWait(ps);
}

static int ReplaceFiles(const std::wstring& src, const std::wstring& dst, std::wstring& err) {
    std::vector<fs::path> srcFiles, dstFiles;
    std::error_code ec;

    try {
        for (const auto& e : fs::recursive_directory_iterator(
                 src, fs::directory_options::skip_permission_denied)) {
            if (e.is_regular_file(ec)) srcFiles.push_back(e.path());
        }
    } catch (...) {}

    if (srcFiles.empty()) {
        err = L"Архив пустой";
        return -1;
    }
    PostMessageW(g_hwnd, WM_RP_FILEPROG, 0, (LPARAM)srcFiles.size());

    try {
        for (const auto& e : fs::recursive_directory_iterator(
                 dst, fs::directory_options::skip_permission_denied)) {
            if (e.is_regular_file(ec)) dstFiles.push_back(e.path());
        }
    } catch (...) {}
    for (const auto& p : dstFiles) {
        std::error_code e2;
        fs::remove(p, e2);
    }

    int done = 0;
    for (const auto& f : srcFiles) {
        std::error_code e2;
        fs::path rel = fs::relative(f, src, e2);
        if (e2) {
            err = L"Не удалось вычислить путь файла";
            return -1;
        }
        fs::path target = fs::path(dst) / rel;
        fs::create_directories(target.parent_path(), e2);
        fs::copy_file(f, target, fs::copy_options::overwrite_existing, e2);
        if (e2) {
            err = L"Не удалось записать " + rel.wstring();
            return -1;
        }
        ++done;
        PostMessageW(g_hwnd, WM_RP_FILEPROG, (WPARAM)done, (LPARAM)srcFiles.size());
    }
    return done;
}

static bool DoCheck() {
    PostPhase(PH_CHECK);
    PostLog(L"Проверка обновлений…");

    std::wstring dir = GetTargetDir();
    if (dir.empty()) {
        PostError(L"Не удалось определить папку плагина");
        return false;
    }
    g_targetDir = dir;

    float local = 0.f;
    bool hasLocal = ReadVersion(dir + L"\\version.txt", local);
    PostLog(hasLocal ? (L"Локальная версия: " + FormatVersion(local))
                     : std::wstring(L"Плагин ещё не установлен"));

    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring path = std::wstring(tmp) + L"realpack_version_check.txt";
    std::wstring err;
    if (!Download(kVerUrl, path, 8000, false, err)) {
        PostError(err);
        return false;
    }

    float remote = 0.f;
    bool hasRemote = ReadVersion(path, remote);
    fs::remove(path);
    if (!hasRemote) {
        PostError(L"Не удалось прочитать версию на сервере");
        return false;
    }

    PostLog(L"Версия на сервере: " + FormatVersion(remote));

    auto* vi = new VersionInfo();
    vi->local = hasLocal ? FormatVersion(local) : L"";
    vi->remote = FormatVersion(remote);
    vi->hasLocal = hasLocal;
    vi->hasRemote = true;
    vi->newer = hasLocal && remote > local + 0.0005f;
    PostMessageW(g_hwnd, WM_RP_VERSIONS, 0, (LPARAM)vi);

    if (!hasLocal)
        PostLog(L"Доступна установка RealPack " + FormatVersion(remote));
    else if (remote > local + 0.0005f)
        PostLog(L"Доступно обновление " + FormatVersion(local) + L" → " + FormatVersion(remote));
    else if (local > remote + 0.0005f)
        PostLog(L"Локальная версия новее серверной");
    else
        PostLog(L"Установлена актуальная версия");
    return true;
}

static bool DoInstall() {
    PostPhase(PH_DOWNLOAD);
    std::wstring dir = GetTargetDir();
    if (dir.empty()) {
        PostError(L"Не удалось создать папку плагина");
        return false;
    }
    g_targetDir = dir;

    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring work = std::wstring(tmp) + L"realpack_setup";
    std::wstring zip = work + L"\\realpack.zip";
    std::wstring stage = work + L"\\payload";
    std::error_code ec;
    fs::remove_all(work, ec);
    fs::create_directories(work, ec);

    PostLog(L"Скачиваем realpack.zip…");
    std::wstring err;
    if (!Download(kZipUrl, zip, 0, true, err)) {
        fs::remove_all(work, ec);
        PostError(err);
        return false;
    }

    {
        char sig[2] = {};
        std::ifstream f(fs::path(zip), std::ios::binary);
        f.read(sig, 2);
        if (sig[0] != 'P' || sig[1] != 'K') {
            fs::remove_all(work, ec);
            PostError(L"Сервер вернул повреждённый архив");
            return false;
        }
    }

    PostPhase(PH_EXTRACT);
    PostLog(L"Распаковываем архив…");
    if (!ExtractZip(zip, stage)) {
        fs::remove_all(work, ec);
        PostError(L"Не удалось распаковать архив");
        return false;
    }

    PostPhase(PH_INSTALL);
    PostLog(L"Обновляем файлы плагина…");
    int copied = ReplaceFiles(stage, dir, err);
    fs::remove_all(work, ec);
    if (copied < 0) {
        PostError(err.empty() ? std::wstring(L"Не удалось заменить файлы") : err);
        return false;
    }
    PostLog(Format(L"Записано файлов: %d", copied));

    float v = 0.f;
    auto* installed = new std::wstring(ReadVersion(dir + L"\\version.txt", v)
                                           ? FormatVersion(v)
                                           : std::wstring(L""));
    PostMessageW(g_hwnd, WM_RP_INSTALLED, 0, (LPARAM)installed);
    return true;
}

static DWORD WINAPI WorkerProc(LPVOID param) {
    int mode = (int)(INT_PTR)param;
    bool ok = false;
    try {
        ok = (mode == 0) ? DoCheck() : DoInstall();
    } catch (...) {
        PostError(L"Непредвиденная ошибка");
        ok = false;
    }
    PostMessageW(g_hwnd, WM_RP_DONE, (WPARAM)mode, ok ? 1 : 0);
    return 0;
}

// ---------------------------------------------------------------------------
//  UI actions
// ---------------------------------------------------------------------------
static void RefreshPrimaryLabel() {
    if (st.busy) {
        switch (st.phase) {
        case PH_CHECK:    st.primaryLabel = L"Проверка…";    break;
        case PH_DOWNLOAD: st.primaryLabel = L"Скачивание…";  break;
        case PH_EXTRACT:  st.primaryLabel = L"Распаковка…";  break;
        case PH_INSTALL:  st.primaryLabel = L"Установка…";   break;
        default:          st.primaryLabel = L"Подождите…";   break;
        }
    } else if (st.phase == PH_ERROR) {
        st.primaryLabel = L"Повторить";
    } else if (st.updateAvailable) {
        st.primaryLabel = L"Обновить до " + st.remoteVer;
    } else if (st.hasLocal) {
        st.primaryLabel = L"Установить заново";
    } else {
        st.primaryLabel = L"Установить";
    }
}

static void StartWork(int mode) {
    if (st.busy || !g_hwnd) return;
    st.busy = true;
    st.error.clear();
    st.notice.clear();
    st.overlay = false;
    st.progress = 0.f;
    st.shown = 0.f;
    st.indeterminate = true;
    st.fileDone = 0;
    st.fileTotal = 0;
    st.phase = (mode == 0) ? PH_CHECK : PH_DOWNLOAD;
    RefreshPrimaryLabel();
    PostLog(mode == 0 ? L"Проверяем обновления…" : L"Запуск установки…");
    EnsureTimer(g_hwnd);
    InvalidateRect(g_hwnd, nullptr, FALSE);

    HANDLE t = CreateThread(nullptr, 0, WorkerProc, (LPVOID)(INT_PTR)mode, 0, nullptr);
    if (t) CloseHandle(t);
}

static void OpenPluginFolder() {
    if (g_targetDir.empty()) return;
    std::error_code ec;
    if (!fs::exists(g_targetDir, ec)) {
        PostLog(L"Папка плагина ещё не создана");
        return;
    }
    ShellExecuteW(g_hwnd, L"open", g_targetDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

static int HitTest(float x, float y) {
    if (st.overlay) {
        if (InRect(ROverlayClose(), x, y)) return C_OVERLAY_CLOSE;
        if (InRect(ROverlayBtn(), x, y)) return C_OVERLAY_BTN;
        return -1;
    }
    if (InRect(RClose(), x, y)) return C_CLOSE;
    if (InRect(RMin(), x, y)) return C_MIN;
    if (!st.busy) {
        if (InRect(RInstall(), x, y)) return C_INSTALL;
        if (InRect(RCheck(), x, y)) return C_CHECK;
    }
    if (InRect(RInfo(), x, y)) return C_INFO;
    if (!g_targetDir.empty() && InRect(RLink(), x, y)) return C_LINK;
    return -1;
}

static void DoAction(int id) {
    switch (id) {
    case C_INSTALL: StartWork(1); break;
    case C_CHECK:   StartWork(0); break;
    case C_INFO:
        st.overlay = !st.overlay;
        InvalidateRect(g_hwnd, nullptr, FALSE);
        break;
    case C_CLOSE:
        if (st.busy) {
            PostLog(L"Дождитесь завершения установки");
        } else {
            DestroyWindow(g_hwnd);
        }
        break;
    case C_MIN:
        ShowWindow(g_hwnd, SW_MINIMIZE);
        break;
    case C_LINK:
        OpenPluginFolder();
        break;
    case C_OVERLAY_BTN:
    case C_OVERLAY_CLOSE:
        st.overlay = false;
        InvalidateRect(g_hwnd, nullptr, FALSE);
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
//  Window plumbing
// ---------------------------------------------------------------------------
typedef UINT (WINAPI* FnGetDpiForSystem)();
typedef UINT (WINAPI* FnGetDpiForWindow)(HWND);
typedef BOOL (WINAPI* FnSetProcessDpiCtx)(HANDLE);

static FnGetDpiForSystem pGetDpiForSystem = nullptr;
static FnGetDpiForWindow pGetDpiForWindow = nullptr;

static UINT DetectDpi() {
    if (pGetDpiForSystem) return pGetDpiForSystem();
    HDC dc = GetDC(nullptr);
    UINT dpi = dc ? (UINT)GetDeviceCaps(dc, LOGPIXELSY) : 96u;
    if (dc) ReleaseDC(nullptr, dc);
    return dpi ? dpi : 96u;
}

static void UpdateWindowRegion(HWND h) {
    RECT rc = {};
    GetClientRect(h, &rc);
    int w = rc.right - rc.left;
    int hgt = rc.bottom - rc.top;
    if (w <= 0 || hgt <= 0) return;
    int rad = (int)(18.f * g_scale + .5f);
    HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, hgt + 1, rad, rad);
    if (rgn) SetWindowRgn(h, rgn, TRUE);
}

static void Render(HWND hWnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hWnd, &ps);
    RECT rc = {};
    GetClientRect(hWnd, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) { EndPaint(hWnd, &ps); return; }

    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    {
        Graphics g(mem);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.SetCompositingQuality(CompositingQualityHighQuality);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        g.ScaleTransform(g_scale, g_scale);
        DrawWindow(g);
    }
    BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hWnd, &ps);
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        g_hwnd = hWnd;
        HICON ico = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
                                      64, 64, LR_DEFAULTCOLOR);
        if (!ico) ico = LoadIconW(g_inst, MAKEINTRESOURCEW(IDI_APP_ICON));
        if (ico) {
            g_icon = Bitmap::FromHICON(ico);
            DestroyIcon(ico);
        }
        break;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        Render(hWnd);
        return 0;

    case WM_TIMER:
        if (wParam == 1) {
            for (int i = 0; i < C_COUNT; ++i) {
                float target = (g_hoverId == i) ? 1.f : 0.f;
                float d = target - g_hover[i].v;
                if (fabsf(d) > 0.004f) g_hover[i].v += d * 0.25f;
                else g_hover[i].v = target;
            }
            float pd = st.progress - st.shown;
            if (fabsf(pd) > 0.002f) st.shown += pd * 0.16f;
            else st.shown = st.progress;

            st.shim += 0.016f;
            if (st.shim > 1.f) st.shim -= 1.f;

            InvalidateRect(hWnd, nullptr, FALSE);
            StopTimerIfIdle(hWnd);
        }
        return 0;

    case WM_MOUSEMOVE: {
        float x = GET_X_LPARAM(lParam) / g_scale;
        float y = GET_Y_LPARAM(lParam) / g_scale;
        int id = HitTest(x, y);
        if (id != g_hoverId) {
            g_hoverId = id;
            EnsureTimer(hWnd);
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        SetCursor(LoadCursorW(nullptr, id >= 0 ? IDC_HAND : IDC_ARROW));
        TRACKMOUSEEVENT tme = {};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hWnd;
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        g_hoverId = -1;
        EnsureTimer(hWnd);
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;

    case WM_LBUTTONDOWN: {
        float x = GET_X_LPARAM(lParam) / g_scale;
        float y = GET_Y_LPARAM(lParam) / g_scale;
        int id = HitTest(x, y);
        if (st.overlay) {
            if (id < 0 && !InRect(ROverlay(), x, y)) {
                st.overlay = false;
                InvalidateRect(hWnd, nullptr, FALSE);
                return 0;
            }
        }
        g_pressId = id;
        if (id >= 0) SetCapture(hWnd);
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONUP: {
        int pressed = g_pressId;
        g_pressId = -1;
        if (GetCapture() == hWnd) ReleaseCapture();
        float x = GET_X_LPARAM(lParam) / g_scale;
        float y = GET_Y_LPARAM(lParam) / g_scale;
        if (pressed >= 0 && HitTest(x, y) == pressed) DoAction(pressed);
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    }
    case WM_CAPTURECHANGED:
        g_pressId = -1;
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            if (st.overlay) {
                st.overlay = false;
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            return 0;
        }
        if (wParam == VK_RETURN) {
            if (st.overlay) {
                st.overlay = false;
                InvalidateRect(hWnd, nullptr, FALSE);
            } else {
                StartWork(1);
            }
            return 0;
        }
        break;

    case WM_NCHITTEST: {
        if (st.overlay) return HTCLIENT;
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(hWnd, &pt);
        float x = pt.x / g_scale;
        float y = pt.y / g_scale;
        if (y < 52.f && !InRect(RClose(), x, y) && !InRect(RMin(), x, y))
            return HTCAPTION;
        return HTCLIENT;
    }

    case WM_DPICHANGED: {
        g_scale = (float)HIWORD(wParam) / 96.f;
        RECT* r = (RECT*)lParam;
        SetWindowPos(hWnd, nullptr, r->left, r->top,
                     (int)(WIN_W * g_scale + .5f), (int)(WIN_H * g_scale + .5f),
                     SWP_NOZORDER | SWP_NOACTIVATE);
        UpdateWindowRegion(hWnd);
        InvalidateRect(hWnd, nullptr, FALSE);
        break;
    }

    case WM_RP_START:
        StartWork(0);
        return 0;

    case WM_RP_LOG: {
        auto* p = (std::wstring*)lParam;
        if (p) {
            st.log.push_back(*p);
            delete p;
            if (st.log.size() > 40) st.log.pop_front();
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_RP_PROGRESS: {
        int pm = (int)wParam;
        if (pm < 0) {
            st.indeterminate = true;
        } else {
            st.indeterminate = false;
            st.progress = Clamp(pm / 1000.f, 0.f, 1.f);
        }
        EnsureTimer(hWnd);
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    }
    case WM_RP_PHASE: {
        st.phase = (Phase)wParam;
        if (st.phase == PH_DOWNLOAD || st.phase == PH_EXTRACT) {
            st.indeterminate = true;
            st.progress = 0.f;
            st.shown = 0.f;
        } else if (st.phase == PH_INSTALL) {
            st.indeterminate = false;
            st.progress = 0.f;
            st.shown = 0.f;
        }
        RefreshPrimaryLabel();
        EnsureTimer(hWnd);
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    }
    case WM_RP_VERSIONS: {
        auto* vi = (VersionInfo*)lParam;
        if (vi) {
            st.localVer = vi->local;
            st.hasLocal = vi->hasLocal;
            st.remoteVer = vi->remote;
            st.hasRemote = vi->hasRemote;
            st.updateAvailable = vi->newer;
            delete vi;
            RefreshPrimaryLabel();
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_RP_FILEPROG: {
        int done = (int)wParam;
        int total = (int)lParam;
        st.fileDone = done;
        st.fileTotal = total;
        if (total > 0) {
            st.progress = Clamp((float)done / (float)total, 0.f, 1.f);
            st.indeterminate = false;
        }
        EnsureTimer(hWnd);
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    }
    case WM_RP_ERROR: {
        auto* p = (std::wstring*)lParam;
        if (p) {
            st.error = *p;
            st.log.push_back(*p);
            if (st.log.size() > 40) st.log.pop_front();
            delete p;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_RP_INSTALLED: {
        auto* p = (std::wstring*)lParam;
        if (p) {
            if (!p->empty()) {
                st.localVer = *p;
                st.hasLocal = true;
                st.remoteVer = *p;
                st.hasRemote = true;
            }
            st.updateAvailable = false;
            delete p;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_RP_DONE: {
        int mode = (int)wParam;
        bool ok = lParam != 0;
        st.busy = false;
        st.indeterminate = false;
        if (mode == 0) {
            st.phase = PH_IDLE;
            if (!ok)
                st.notice = L"Не удалось проверить обновления — установка доступна вручную";
        } else if (ok) {
            st.phase = PH_SUCCESS;
            st.progress = 1.f;
            st.notice.clear();
            PostLog(L"Готово");
        } else {
            st.phase = PH_ERROR;
        }
        RefreshPrimaryLabel();
        EnsureTimer(hWnd);
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    }

    case WM_CLOSE:
        if (st.busy) {
            PostLog(L"Дождитесь завершения установки");
            return 0;
        }
        DestroyWindow(hWnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hWnd, 1);
        g_timerOn = false;
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hWnd, message, wParam, lParam);
}

// ---------------------------------------------------------------------------
//  Entry point
// ---------------------------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    g_inst = hInstance;

    // Crisp UI on every monitor.
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        auto setCtx = (FnSetProcessDpiCtx)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (setCtx) setCtx((HANDLE)(LONG_PTR)-4);
        else SetProcessDPIAware();
        pGetDpiForSystem = (FnGetDpiForSystem)GetProcAddress(user32, "GetDpiForSystem");
        pGetDpiForWindow = (FnGetDpiForWindow)GetProcAddress(user32, "GetDpiForWindow");
    }
    g_scale = (float)DetectDpi() / 96.f;

    // Single instance.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\RealPackInstaller.UI.SingleInstance");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND prev = FindWindowW(kClassName, nullptr);
        if (prev) {
            ShowWindow(prev, SW_RESTORE);
            SetForegroundWindow(prev);
        }
        CloseHandle(mutex);
        return 0;
    }

    GdiplusStartupInput gsi;
    if (GdiplusStartup(&g_gdiToken, &gsi, nullptr) != Ok) {
        MessageBoxW(nullptr, L"Не удалось инициализировать GDI+", L"RealPack", MB_ICONERROR);
        return 1;
    }
    InitFonts();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hIconSm = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClassName;

    if (!RegisterClassExW(&wc)) {
        MessageBoxW(nullptr, L"Ошибка регистрации окна", L"RealPack", MB_ICONERROR);
        return 1;
    }

    int w = (int)(WIN_W * g_scale + .5f);
    int h = (int)(WIN_H * g_scale + .5f);
    RECT wa = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int x = wa.left + ((wa.right - wa.left) - w) / 2;
    int y = wa.top + ((wa.bottom - wa.top) - h) / 2;

    HWND hWnd = CreateWindowExW(WS_EX_APPWINDOW, kClassName, kTitleName,
                                WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU,
                                x, y, w, h, nullptr, nullptr, hInstance, nullptr);
    if (!hWnd) {
        MessageBoxW(nullptr, L"Ошибка создания окна", L"RealPack", MB_ICONERROR);
        return 1;
    }

    UpdateWindowRegion(hWnd);
    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);
    SetForegroundWindow(hWnd);

    PostMessageW(hWnd, WM_RP_START, 0, 0);

    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_icon) { delete g_icon; g_icon = nullptr; }
    FreeFonts();
    GdiplusShutdown(g_gdiToken);
    if (mutex) CloseHandle(mutex);
    return (int)msg.wParam;
}
