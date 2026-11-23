// WinVClip —— 接管 Win+V，弹出自定义剪贴板面板
// 视觉稿：Clipboard Reliquary · PC Panel · 560 × 620（1:1 还原 index.html）
// 构建：cmake -G Ninja -B build && cmake --build build
// 用法：winvclip.exe            常驻托盘，Win+V 弹面板
//       winvclip.exe --show     启动后立刻弹一次面板
//
// 原理：
//   1) WH_KEYBOARD_LL 全局钩子：Win 按下 + V 按下 -> 吞掉该键，同步弹面板
//      面板非激活态要用的按键也由它旁路转发，但只接面板会消费的键
//      （见 PanelConsumesBareKey：钩子是串行链，吞得越宽，别家的全局热键越哑）
//   2) AddClipboardFormatListener 监听 WM_CLIPBOARDUPDATE，延迟读取剪贴板存历史
//   3) 自绘 popup 面板；回车单击 = 写回剪贴板 + 向原窗口发 Ctrl+V

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <deque>
#include <algorithm>
#include <cwctype>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <d2d1.h>
#include <dwrite.h>
#include <initguid.h>   // 让 wincodec.h 把 PNG 编码器/像素格式的 GUID 定义成实体
#include <wincodec.h>
#include <uiautomation.h>   // 浏览器/Electron 的插入符只能从 UIA 文本模式读到（见 UiaCaretRect）
#include <tlhelp32.h>       // 焦点元素可能是浏览器的渲染子进程，判归属要查进程树
#include <commdlg.h>
#include <imm.h>       // 搜索框要能吃中文：IME 组合串自己收、自己画
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <atomic>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <cstring>
#include "glyph_data.h"

// ================================================================== 常量
// 视觉常量（配色 / 尺寸 / 窗口类名）集中在 theme.h，改配色只看那一个文件。
#include "theme.h"
using namespace wv;
#include "text_util.h"
#include "diag.h"
#include "io_png.h"
#include "model.h"
#include "app_state.h"
#include "glyph.h"

enum { WM_APP_HOTKEY = WM_APP + 1, WM_APP_TRAY = WM_APP + 2, WM_APP_DEFER_HIDE = WM_APP + 10,
       WM_APP_HIDE_OUTSIDE = WM_APP + 11 };
enum { TIMER_CLIP = 0x101, TIMER_PASTE = 0x102, TIMER_TOAST = 0x103, TIMER_CARET = 0x104,
       TIMER_FOCUS = 0x106, TIMER_EMOJI = 0x107, TIMER_WARM = 0x108, TIMER_CLIPRESTORE = 0x109,
       TIMER_REPROBE = 0x10A, TIMER_DEFERHIDE = 0x10B };

// ================================================================== 数据

// Thumb / ClipItem / GItem / GCat / RecentGlyph 见 model.h

// 自己注入的按键统一打上标记（g_injectMark 见 app_state.h）：
// 钩子靠它区分「本程序注入」与「其他来源的注入」——后者要照常转发给面板，
// 自动化测试正是靠这一点才能驱动面板。
static void MarkMine(INPUT& in) { in.ki.dwExtraInfo = g_injectMark; }

static int S(int v) { return MulDiv(v, g_dpi, 96); }

static IDWriteFont* MakeProbe(const wchar_t* family);   // 实现在字形绘制段

// ================================================================== 工具
// 字符串 / 编码转换见 text_util.h（Lower / U8N / NextCp / W2U8 / FileNameOf / StampName）

// 字形库（解析 / 索引 / 搜索 / 可画性）见 glyph.h / glyph.cpp

static std::wstring FileBase(const std::wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? p : p.substr(i + 1);
}

static IDWriteTextFormat* Fmt(HFONT f) {
    auto it = g_tfmt.find(f);
    return it == g_tfmt.end() ? nullptr : it->second;
}

// 量文字宽度：优先 DWrite（与渲染完全一致），未就绪时回退 GDI
// 量宽要建 IDWriteTextLayout（一次 COM 分配 + 字形 shaping），而布局与绘制会
// 对同一批固定文案反复量（页签、chip、页脚、行内大小/时间）。这里按字体分组缓存。
static std::unordered_map<HFONT, std::unordered_map<std::wstring, int>> g_twCache;
static int g_twCacheN = 0;

static int TextWUncached(HFONT f, const std::wstring& s) {
    IDWriteTextFormat* fmt = Fmt(f);
    if (fmt && g_dw) {
        IDWriteTextLayout* lay = nullptr;
        if (SUCCEEDED(g_dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), fmt,
                                             4096.0f, 512.0f, &lay)) && lay) {
            DWRITE_TEXT_METRICS m{};
            float w = 0;
            if (SUCCEEDED(lay->GetMetrics(&m))) w = m.widthIncludingTrailingWhitespace;
            lay->Release();
            if (w > 0) return (int)ceilf(w);
            return 0;
        }
    }
    if (!g_ref) return (int)s.size() * 8;
    HFONT old = (HFONT)SelectObject(g_ref, f);
    SIZE sz{};
    GetTextExtentPoint32W(g_ref, s.c_str(), (int)s.size(), &sz);
    SelectObject(g_ref, old);
    return sz.cx;
}

static int TextW(HFONT f, const std::wstring& s) {
    auto it = g_twCache.find(f);
    if (it != g_twCache.end()) {
        auto j = it->second.find(s);
        if (j != it->second.end()) return j->second;
    }
    int v = TextWUncached(f, s);
    if (g_twCacheN >= 600) { g_twCache.clear(); g_twCacheN = 0; }
    g_twCache[f][s] = v;
    ++g_twCacheN;
    return v;
}

static std::wstring RelTime(const SYSTEMTIME& st) {
    FILETIME ft{}, ftNow{};
    SystemTimeToFileTime(&st, &ft);
    SYSTEMTIME now{};
    GetLocalTime(&now);
    SystemTimeToFileTime(&now, &ftNow);
    ULARGE_INTEGER a{}, b{};
    a.LowPart = ft.dwLowDateTime;  a.HighPart = ft.dwHighDateTime;
    b.LowPart = ftNow.dwLowDateTime; b.HighPart = ftNow.dwHighDateTime;
    long long sec = (long long)((b.QuadPart - a.QuadPart) / 10000000ULL);
    if (sec < 0) sec = 0;
    if (sec < 3600)       return std::to_wstring(sec < 60 ? 1 : sec / 60) + L"分钟前";
    if (sec < 86400)      return std::to_wstring(sec / 3600) + L"小时前";
    if (sec < 172800)     return L"昨天";
    wchar_t buf[16];
    swprintf(buf, 16, L"%02d-%02d", (int)st.wMonth, (int)st.wDay);
    return buf;
}

// 字节数 -> 可读大小（默认 KB，进到 1000KB 以上换 MB）
static std::wstring SizeText(size_t bytes) {
    double kb = (double)bytes / 1024.0;
    if (kb < 1.0) kb = 1.0;
    wchar_t buf[32];
    int ikb = (int)(kb + 0.5);
    if (ikb < 1000) swprintf(buf, 32, L"%dKB", ikb);
    else            swprintf(buf, 32, L"%.1fMB", kb / 1024.0);
    return buf;
}

// 文本按 UTF-8 编码后的字节数（与剪贴板上实际占用的量一致）
static size_t Utf8Bytes(const std::wstring& s) {
    size_t n = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned c = (unsigned)s[i];
        if (c < 0x80) n += 1;
        else if (c < 0x800) n += 2;
        else if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.size()) { n += 4; ++i; }
        else n += 3;
    }
    return n;
}

// 单个文件大小（目录和取不到的算 0）
static size_t PathBytes(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fad)) return 0;
    if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return 0;
    return ((size_t)fad.nFileSizeHigh << 32) | (size_t)fad.nFileSizeLow;
}

// ================================================================== 绘制后端（Direct2D）
static D2D1_COLOR_F Col(COLORREF c, float a = 1.0f) {
    return D2D1::ColorF(GetRValue(c) / 255.0f, GetGValue(c) / 255.0f,
                        GetBValue(c) / 255.0f, a);
}

static void Brush(COLORREF c, float a = 1.0f) {
    if (g_brush) g_brush->SetColor(Col(c, a));
}

// 一次绘制批次：绑定内存 DC，整帧只在 Begin/End 之间发起 D2D 调用
static bool g_dirtyClip = false;
static bool g_needBind = true;      // 缓冲换了才需要重新绑定 DC

static void PaintBegin(HDC dc, int w, int h, const RECT* dirty = nullptr) {
    if (!g_rt || !g_brush) return;
    // BindDC 只做一次：软件 DCRenderTarget 每次绑定都要重建内部位图包装，
    // 每帧调它实测吃掉 2ms 左右，而缓冲尺寸不变时完全没有必要
    if (g_needBind) {
        RECT br{0, 0, w, h};
        if (FAILED(g_rt->BindDC(dc, &br))) return;
        g_rt->SetDpi(96.0f, 96.0f);   // BindDC 会继承 DC 的 DPI，这里强制 1 DIP = 1px
        g_needBind = false;
    }
    g_rt->SetTransform(D2D1::Matrix3x2F::Identity());
    g_rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    g_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    g_rt->BeginDraw();
    g_painting = true;
    g_dirtyClip = false;
    if (dirty && !IsRectEmpty(dirty)) {
        g_rt->PushAxisAlignedClip(
            D2D1::RectF((float)dirty->left, (float)dirty->top,
                        (float)dirty->right, (float)dirty->bottom),
            D2D1_ANTIALIAS_MODE_ALIASED);
        g_dirtyClip = true;
    }
}

static void PaintEnd() {
    if (!g_painting) return;
    if (g_dirtyClip) { g_rt->PopAxisAlignedClip(); g_dirtyClip = false; }
    g_painting = false;
    g_rt->EndDraw();
}

static void FillRound(HDC dc, const RECT& r, int rad, COLORREF fill, COLORREF border, int bw = 1) {
    (void)dc;
    if (!g_rt || !g_painting) return;
    float w = (float)(r.right - r.left), h = (float)(r.bottom - r.top);
    float rr = rad / 2.0f;                       // GDI 传直径，D2D 要半径
    if (rr > w / 2.0f) rr = w / 2.0f;
    if (rr > h / 2.0f) rr = h / 2.0f;
    D2D1_ROUNDED_RECT box = D2D1::RoundedRect(
        D2D1::RectF((float)r.left, (float)r.top, (float)r.right, (float)r.bottom), rr, rr);
    Brush(fill);
    g_rt->FillRoundedRectangle(box, g_brush);
    if (bw > 0) {
        // 描边压在填充上并内缩半个线宽，避免圆角边缘发胖
        Brush(border);
        g_rt->DrawRoundedRectangle(box, g_brush, (float)bw);
    }
}

static void FillBox(HDC dc, const RECT& r, COLORREF c) {
    (void)dc;
    if (!g_rt || !g_painting) return;
    Brush(c);
    g_rt->FillRectangle(D2D1::RectF((float)r.left, (float)r.top,
                                    (float)r.right, (float)r.bottom), g_brush);
}

static void HLine(HDC dc, int x1, int x2, int y, COLORREF c) {
    RECT r{x1, y, x2, y + 1};
    FillBox(dc, r, c);
}

static void TextOutF(HDC dc, HFONT f, COLORREF c, const std::wstring& s, RECT r, UINT flags) {
    IDWriteTextFormat* fmt = Fmt(f);
    if (fmt && g_rt && g_painting) {
        fmt->SetTextAlignment((flags & DT_CENTER) ? DWRITE_TEXT_ALIGNMENT_CENTER
                            : (flags & DT_RIGHT)  ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                                  : DWRITE_TEXT_ALIGNMENT_LEADING);
        fmt->SetParagraphAlignment((flags & DT_VCENTER) ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER
                                                        : DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        if (flags & DT_END_ELLIPSIS) {
            DWRITE_TRIMMING tm{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            fmt->SetTrimming(&tm, g_ellipsis);
        } else {
            DWRITE_TRIMMING tm{DWRITE_TRIMMING_GRANULARITY_NONE, 0, 0};
            fmt->SetTrimming(&tm, nullptr);
        }
        Brush(c);
        g_rt->DrawTextW(s.c_str(), (UINT32)s.size(), fmt,
                        D2D1::RectF((float)r.left, (float)r.top, (float)r.right, (float)r.bottom),
                        g_brush, D2D1_DRAW_TEXT_OPTIONS_NONE);
        return;
    }
    // 兜底：D2D 尚未就绪时仍用 GDI
    HFONT old = (HFONT)SelectObject(dc, f);
    int oldMode = SetBkMode(dc, TRANSPARENT);
    COLORREF oc = SetTextColor(dc, c);
    DrawTextW(dc, s.c_str(), (int)s.size(), &r, flags | DT_NOPREFIX);
    SetTextColor(dc, oc);
    SetBkMode(dc, oldMode);
    SelectObject(dc, old);
}

// ------------------------------------------------------------------ 路径构建器
struct PathB {
    ID2D1PathGeometry* geo = nullptr;
    ID2D1GeometrySink* sink = nullptr;
    bool used = false;

    PathB() {
        if (g_d2d && SUCCEEDED(g_d2d->CreatePathGeometry(&geo)) && geo && SUCCEEDED(geo->Open(&sink)))
            sink->SetFillMode(D2D1_FILL_MODE_WINDING);   // 让反向绕序的子路径成为镂空
    }
    ~PathB() {
        if (sink) sink->Release();
        if (geo) geo->Release();
    }
    bool ok() const { return geo && sink; }

    void Move(float x, float y) {
        if (!sink) return;
        sink->BeginFigure(D2D1::Point2F(x, y), D2D1_FIGURE_BEGIN_HOLLOW);
        used = true;
    }
    void Line(float x, float y) { if (sink) sink->AddLine(D2D1::Point2F(x, y)); }
    void EndOpen()   { if (sink) sink->EndFigure(D2D1_FIGURE_END_OPEN); }
    void EndClosed() { if (sink) sink->EndFigure(D2D1_FIGURE_END_CLOSED); }

    // 四段三次贝塞尔近似整圆（比 ArcSegment 更稳，不受角度限制）
    void Circle(float cx, float cy, float r, bool cw = true) {
        if (!sink) return;
        const float k = 0.5522847498f * r;
        if (cw) {
            Move(cx, cy - r);
            sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(cx + k, cy - r),
                                                D2D1::Point2F(cx + r, cy - k),
                                                D2D1::Point2F(cx + r, cy)));
            sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(cx + r, cy + k),
                                                D2D1::Point2F(cx + k, cy + r),
                                                D2D1::Point2F(cx, cy + r)));
            sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(cx - k, cy + r),
                                                D2D1::Point2F(cx - r, cy + k),
                                                D2D1::Point2F(cx - r, cy)));
            sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(cx - r, cy - k),
                                                D2D1::Point2F(cx - k, cy - r),
                                                D2D1::Point2F(cx, cy - r)));
        } else {
            Move(cx, cy - r);
            sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(cx - k, cy - r),
                                                D2D1::Point2F(cx - r, cy - k),
                                                D2D1::Point2F(cx - r, cy)));
            sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(cx - r, cy + k),
                                                D2D1::Point2F(cx - k, cy + r),
                                                D2D1::Point2F(cx, cy + r)));
            sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(cx + k, cy + r),
                                                D2D1::Point2F(cx + r, cy + k),
                                                D2D1::Point2F(cx + r, cy)));
            sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(cx + r, cy - k),
                                                D2D1::Point2F(cx + k, cy - r),
                                                D2D1::Point2F(cx, cy - r)));
        }
        EndClosed();
    }

    void RoundRect(float l, float t, float rr, float b, float rad) {
        if (!sink) return;
        float r = rad;
        if (r > (rr - l) / 2.0f) r = (rr - l) / 2.0f;
        if (r > (b - t) / 2.0f) r = (b - t) / 2.0f;
        if (r < 0.1f) {
            Move(l, t); Line(rr, t); Line(rr, b); Line(l, b); EndClosed();
            return;
        }
        const float k = 0.5522847498f * r;
        Move(l + r, t);
        Line(rr - r, t);
        sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(rr - r + k, t),
                                            D2D1::Point2F(rr, t + r - k),
                                            D2D1::Point2F(rr, t + r)));
        Line(rr, b - r);
        sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(rr, b - r + k),
                                            D2D1::Point2F(rr - r + k, b),
                                            D2D1::Point2F(rr - r, b)));
        Line(l + r, b);
        sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(l + r - k, b),
                                            D2D1::Point2F(l, b - r + k),
                                            D2D1::Point2F(l, b - r)));
        Line(l, t + r);
        sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(l, t + r - k),
                                            D2D1::Point2F(l + r - k, t),
                                            D2D1::Point2F(l + r, t)));
        EndClosed();
    }

    void Stroke(COLORREF c, float width) {
        if (!ok() || !used) return;
        sink->Close();
        sink->Release();
        sink = nullptr;
        Brush(c);
        g_rt->DrawGeometry(geo, g_brush, width < 1.0f ? 1.0f : width, g_roundStroke);
    }

    void Fill(COLORREF c) {
        if (!ok() || !used) return;
        sink->Close();
        sink->Release();
        sink = nullptr;
        Brush(c);
        g_rt->FillGeometry(geo, g_brush);
    }
};

// ------------------------------------------------------------------ 图标
enum IconId { IC_SEARCH, IC_GEAR, IC_X, IC_CLIP, IC_TEXT, IC_CODE, IC_IMAGE,
              IC_FILE, IC_LINK, IC_PIN, IC_STAR, IC_MORE, IC_TRASH, IC_CHECK, IC_COPY,
              IC_SAVE };

// 在 box 内按 24×24 坐标系描边绘制线性图标，stroke 为线宽
// 在 box 内按 24×24 坐标系绘制线性图标，stroke 为 24 坐标系下的线宽
static void DrawIcon(HDC dc, IconId id, RECT box, COLORREF c, float stroke) {
    (void)dc;
    if (!g_rt || !g_painting) return;
    float w = (float)(box.right - box.left), h = (float)(box.bottom - box.top);
    float sz = w < h ? w : h;
    float k = sz / 24.0f;
    float ox = (float)box.left + (w - sz) / 2.0f;
    float oy = (float)box.top + (h - sz) / 2.0f;
    auto X = [&](double v) { return ox + (float)(v * k); };
    auto Y = [&](double v) { return oy + (float)(v * k); };
    float px = stroke * k;               // 线宽按尺寸折算，不取整

    PathB s, f;                          // s = 描边路径，f = 填充路径
    switch (id) {
    case IC_SEARCH:
        s.Circle(X(11), Y(11), 7 * k);
        s.Move(X(20), Y(20)); s.Line(X(16.2), Y(16.2)); s.EndOpen();
        break;
    case IC_GEAR: {
        // 线性齿轮：外圈 + 8 根短齿 + 中心轴孔（与其它图标同风格，走描边路径）
        s.Circle(X(12), Y(12), 6.2 * k);
        s.Circle(X(12), Y(12), 2.5 * k);
        for (int i = 0; i < 8; ++i) {
            double a = i * 3.14159265 / 4.0;
            s.Move(X(12 + 6.4 * cos(a)), Y(12 + 6.4 * sin(a)));
            s.Line(X(12 + 9.3 * cos(a)), Y(12 + 9.3 * sin(a)));
            s.EndOpen();
        }
        break; }
    case IC_X:
        s.Move(X(6), Y(6));  s.Line(X(18), Y(18)); s.EndOpen();
        s.Move(X(18), Y(6)); s.Line(X(6), Y(18));  s.EndOpen();
        break;
    case IC_CLIP:
        s.RoundRect(X(5), Y(4), X(19), Y(22), 2 * k);
        s.RoundRect(X(8.5), Y(2), X(15.5), Y(6), 1 * k);
        break;
    case IC_TEXT:
        s.Move(X(4), Y(6));  s.Line(X(20), Y(6));  s.EndOpen();
        s.Move(X(4), Y(12)); s.Line(X(15), Y(12)); s.EndOpen();
        s.Move(X(4), Y(18)); s.Line(X(11), Y(18)); s.EndOpen();
        break;
    case IC_CODE:
        s.Move(X(9), Y(6));  s.Line(X(3), Y(12));  s.Line(X(9), Y(18));  s.EndOpen();
        s.Move(X(15), Y(6)); s.Line(X(21), Y(12)); s.Line(X(15), Y(18)); s.EndOpen();
        break;
    case IC_IMAGE:
        s.RoundRect(X(3), Y(3), X(21), Y(21), 2 * k);
        s.Circle(X(9), Y(9), 2 * k);
        s.Move(X(20), Y(14)); s.Line(X(15), Y(9)); s.Line(X(4), Y(20)); s.EndOpen();
        break;
    case IC_FILE:
        s.Move(X(14), Y(2));
        s.Line(X(6), Y(2));  s.Line(X(4), Y(4));  s.Line(X(4), Y(20));
        s.Line(X(6), Y(22)); s.Line(X(18), Y(22)); s.Line(X(20), Y(20));
        s.Line(X(20), Y(8)); s.Line(X(14), Y(2));
        s.EndOpen();
        s.Move(X(14), Y(2)); s.Line(X(14), Y(8)); s.Line(X(20), Y(8)); s.EndOpen();
        break;
    case IC_LINK:
        s.RoundRect(X(2), Y(9), X(13), Y(15), 3 * k);
        s.RoundRect(X(11), Y(9), X(22), Y(15), 3 * k);
        break;
    case IC_PIN:
        s.Move(X(12), Y(16)); s.Line(X(12), Y(22)); s.EndOpen();
        s.Move(X(7), Y(11));  s.Line(X(17), Y(11)); s.EndOpen();
        s.Move(X(9.5), Y(2));
        s.Line(X(14.5), Y(2)); s.Line(X(15), Y(6)); s.Line(X(15), Y(11));
        s.EndOpen();
        s.Move(X(9), Y(11));  s.Line(X(9), Y(6)); s.Line(X(9.5), Y(2)); s.EndOpen();
        break;
    case IC_STAR: {
        double a0 = -3.14159265 / 2;
        s.Move(X(12 + 9.2 * cos(a0)), Y(12 + 9.2 * sin(a0)));
        for (int i = 1; i < 10; ++i) {
            double a = a0 + i * 3.14159265 / 5;
            double rr = (i % 2 == 0) ? 9.2 : 3.9;
            s.Line(X(12 + rr * cos(a)), Y(12 + rr * sin(a)));
        }
        s.EndClosed();
        break; }
    case IC_MORE:
        for (int i = 0; i < 3; ++i)
            s.Circle(X(12), Y(5 + i * 7), 0.55 * k);
        break;
    case IC_TRASH:
        s.Move(X(3), Y(6));  s.Line(X(21), Y(6)); s.EndOpen();
        s.Move(X(8), Y(6));  s.Line(X(8), Y(3)); s.Line(X(16), Y(3)); s.Line(X(16), Y(6));
        s.EndOpen();
        s.Move(X(19), Y(6)); s.Line(X(18), Y(21)); s.Line(X(6), Y(21)); s.Line(X(5), Y(6));
        s.EndOpen();
        break;
    case IC_CHECK:
        s.Move(X(20), Y(6)); s.Line(X(9), Y(17)); s.Line(X(4), Y(12)); s.EndOpen();
        break;
    case IC_COPY:
        s.RoundRect(X(9), Y(9), X(21), Y(21), 1.5 * k);
        s.Move(X(5), Y(15));
        s.Line(X(4), Y(15)); s.Line(X(3), Y(13)); s.Line(X(3), Y(4));
        s.Line(X(5), Y(3));  s.Line(X(14), Y(3)); s.Line(X(15), Y(4));
        s.Line(X(15), Y(5));
        s.EndOpen();
        break;
    case IC_SAVE:
        // 下载式「另存为」：向下的箭头落进托盘
        s.Move(X(12), Y(3));  s.Line(X(12), Y(15)); s.EndOpen();
        s.Move(X(7.5), Y(10.5)); s.Line(X(12), Y(15)); s.Line(X(16.5), Y(10.5)); s.EndOpen();
        s.Move(X(4), Y(17.5)); s.Line(X(4), Y(20)); s.Line(X(20), Y(20));
        s.Line(X(20), Y(17.5)); s.EndOpen();
        break;
    }

    f.Fill(c);
    s.Stroke(c, px);
}

static IconId ItemIcon(const ClipItem& it) {
    if (it.kind == 2) return IC_IMAGE;
    if (it.kind == 1) return IC_FILE;
    if (it.isLink) return IC_LINK;
    if (it.isCode) return IC_CODE;
    return IC_TEXT;
}

// ================================================================== 字体
static IDWriteTextFormat* MkDW(int px, int weight, const wchar_t* face) {
    if (!g_dw) return nullptr;
    IDWriteTextFormat* tf = nullptr;
    if (FAILED(g_dw->CreateTextFormat(face, nullptr, (DWRITE_FONT_WEIGHT)weight,
                                      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                      (float)S(px), L"zh-cn", &tf)) || !tf)
        return nullptr;
    tf->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    return tf;
}

// HFONT 在这里既是 GDI 句柄也是 D2D 绘制时的「字体令牌」，两者一一对应
static HFONT MkFont(int px, int weight, const wchar_t* face) {
    HFONT hf = CreateFontW(-S(px), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET,
                           OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, face);
    IDWriteTextFormat* tf = MkDW(px, weight, face);
    if (hf && tf) g_tfmt[hf] = tf;
    return hf;
}

static void MakeFonts() {
    HFONT* all[] = {&g_fTitle, &g_fMeta, &g_fTab, &g_fChip, &g_fSearch, &g_fMono,
                    &g_fEmoji, &g_fSym, &g_fKbd, &g_fBig, &g_fToast, &g_fSymName,
                    &g_fSymFallback[0], &g_fSymFallback[1], &g_fSymFallback[2]};
    for (auto* f : all) if (*f) { DeleteObject(*f); *f = nullptr; }
    g_symFontSlot.clear();
    for (auto& kv : g_tfmt) if (kv.second) kv.second->Release();
    g_tfmt.clear();

    const wchar_t* sans  = L"Microsoft YaHei UI";
    const wchar_t* mono  = L"Consolas";
    g_fTitle    = MkFont(13, FW_SEMIBOLD, sans);
    g_fMeta     = MkFont(11, FW_NORMAL,   sans);
    g_fTab      = MkFont(13, FW_SEMIBOLD, sans);
    g_fChip     = MkFont(11, FW_SEMIBOLD, sans);
    g_fSearch   = MkFont(14, FW_NORMAL,   sans);
    g_fMono     = MkFont(12, FW_NORMAL,   mono);
    g_fEmoji    = MkFont(21, FW_NORMAL,   L"Segoe UI Emoji");
    g_fSym      = MkFont(24, FW_NORMAL,   L"Segoe UI Symbol");
    g_fSymName  = MkFont(11, FW_NORMAL,   sans);
    g_fKbd      = MkFont(10, FW_NORMAL,   mono);
    g_fBig      = MkFont(15, FW_SEMIBOLD, sans);
    g_fToast    = MkFont(12, FW_SEMIBOLD, sans);

    // 符号字体兜底：Segoe UI Symbol 没有数学字母(𝒜)、古文字等区块，
    // 缺字会画成方框，所以按字形再往后找两三个能画的字体
    g_fSymFallback[0] = MkFont(24, FW_NORMAL, L"Cambria Math");
    g_fSymFallback[1] = MkFont(24, FW_NORMAL, L"Segoe UI Historic");
    g_fSymFallback[2] = MkFont(24, FW_NORMAL, L"SimSun");

    // 覆盖度探测用的 DWrite 字体对象（判定见 GlyphDrawable）
    for (auto& p : g_probe) { if (p) { p->Release(); p = nullptr; } }
    ClearDrawCache();             // 字体换了，缓存的可画性作废
    g_probe[0] = MakeProbe(L"Segoe UI Symbol");
    g_probe[1] = MakeProbe(L"Cambria Math");
    g_probe[2] = MakeProbe(L"Segoe UI Historic");
    g_probe[3] = MakeProbe(L"SimSun");
    g_probe[4] = MakeProbe(L"Segoe UI Emoji");
}

// ------------------------------------------------------------------ D2D/DWrite：彩色 emoji
static void InitD2D() {
    // MinGW 下避免 __uuidof，直接给 IID
    static const GUID iidD2D = {0x06152247, 0x6f50, 0x465a,
                                {0x92, 0x45, 0x11, 0x8b, 0xfd, 0x3b, 0x60, 0x07}};
    static const GUID iidDW  = {0xb859ee5a, 0xd838, 0x4b5b,
                                {0xa2, 0xe8, 0x1a, 0xdc, 0x7d, 0x93, 0xdb, 0x48}};

    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, iidD2D, nullptr,
                                 (void**)&g_d2d)) || !g_d2d) return;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, iidDW,
                                   (IUnknown**)&g_dw)) || !g_dw) return;

    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
        96.0f, 96.0f);
    if (FAILED(g_d2d->CreateDCRenderTarget(&props, &g_rt)) || !g_rt) return;

    if (FAILED(g_dw->CreateTextFormat(L"Segoe UI Emoji", nullptr,
                                      DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                      DWRITE_FONT_STRETCH_NORMAL, (float)S(21), L"",
                                      &g_emojiFmt)) || !g_emojiFmt) return;
    g_emojiFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    g_emojiFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    g_rt->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &g_brush);

    // 圆头圆角笔触：图标线条与圆角框都靠它拿到平滑收尾
    D2D1_STROKE_STYLE_PROPERTIES ss = D2D1::StrokeStyleProperties(
        D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
        D2D1_LINE_JOIN_ROUND, 1.0f, D2D1_DASH_STYLE_SOLID, 0.0f);
    g_d2d->CreateStrokeStyle(ss, nullptr, 0, &g_roundStroke);

    g_dw->CreateEllipsisTrimmingSign(nullptr, &g_ellipsis);
}

// ------------------------------------------------ 彩色字形（后台光栅化 + 磁盘持久化缓存）
// 软件光栅化 COLRv1 彩色字形实测 2~5ms/格（见 tools/bench_emoji.cpp），一行 9 格就是
// 20~45ms，放在 UI 线程里必然卡顿，且与格子尺寸/像素格式无关 —— 那是软件光栅化的下限。
// 所以分成三层：
//   1) 磁盘缓存：全部 emoji 的成品位图落盘（%LOCALAPPDATA%\WinVClip），第二次启动整块
//      memcpy 进 atlas，一个字形都不用再光栅化；
//   2) 后台预热：首次运行（无缓存）时由工作线程把全部 emoji 渲染一遍并写盘；
//   3) 视口优先：当前可见格走高优先队列，预热任务只在 UI 空闲时推进。
// atlas 容量按「全部 emoji 条数」定，正常情况下不做淘汰 —— 淘汰意味着切回旧分类要重新
// 光栅化，那正是「切分类要等一两秒」的来源。LRU 只在图集被 cap 住时兜底。

struct EmoReq { std::string key; int slot = -1; };            // slot<0 = 只渲染不落槽
struct EmoDone { int slot = -1; std::string key; std::vector<uint32_t> px; };

static ID2D1Bitmap* g_emoAtlas = nullptr;                     // 主设备的图集（仅 UI 线程访问）
static int g_emoSz = 0;
static int g_emoAtlasCols = 0, g_emoAtlasSlots = 0, g_emoNextSlot = 0;
static std::unordered_map<std::string, int> g_emoSlot;        // 字形 -> atlas 槽位
static std::vector<std::string>   g_emoSlotKey;               // 槽位 -> 字形（淘汰/写盘用）
static std::vector<int>           g_emoSlotUsed;              // 槽位 -> 最近使用的帧号
static std::vector<unsigned char> g_emoReady;                 // 槽位 -> 像素是否已上传
static int g_emoFrame = 0;

// 预热 / 落盘状态
static std::vector<uint32_t> g_emoDisk;      // 预热期间的整块镜像，写完盘立刻释放
static std::vector<EmoReq>   g_emoWarmJobs;  // 全量预热清单（UI 一次性写入，worker 只读）
static std::atomic<int>      g_emoWarmIdx{0};
static bool g_emoWarmPosted = false;         // 是否已开始预热（或已从缓存恢复）
static bool g_emoWarmSaved  = false;         // 是否已把图集落盘
static int  g_emoWarmSlots  = 0;             // 预热涉及的槽数
static int  g_emoUpN        = 0;             // 已上传进 atlas 的槽数
static bool g_emoCacheTried = false;         // 是否已尝试读盘
static int  g_emoTotal      = 0;             // 全部 emoji 条数（决定图集容量）

static std::vector<std::thread> g_emoThr;    // 2 个 worker
static std::mutex              g_emoMx;
static std::condition_variable g_emoCv;
static std::vector<EmoReq>     g_emoReq;     // 视口请求（后进先出）
static std::unordered_set<std::string> g_emoPending;   // 视口请求：已投递未完成
static std::vector<EmoDone>    g_emoDone;    // 已完成
static bool g_emoQuit = false;
static int  g_emoNewN = 0, g_emoHitN = 0, g_emoFailN = 0;

static void CollectEmojiCells(std::vector<std::pair<RECT, int>>& out);  // 定义在绘制段
static int GlyphCols();   // 定义在布局段
static int EmojiStride() { return g_emoSz + S(4); }
static int EmojiSide()   { return g_emoAtlasCols * EmojiStride(); }

// 工作线程主体：自带一套 D2D/DWrite 设备，循环取请求、渲染、交出像素。
// 取任务顺序 = 视口请求（LIFO）-> 预热（顺序推进），保证切分类/滚动立刻可见。
static void EmojiWorkerMain(int sz) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const int stride = sz + S(4);

    ID2D1Factory* d2d = nullptr;
    IDWriteFactory* dwf = nullptr;
    ID2D1DCRenderTarget* rt = nullptr;
    IDWriteTextFormat* fmt = nullptr;
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    void* bits = nullptr;
    bool ready = false;

    do {
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d)) || !d2d) break;
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       (IUnknown**)&dwf)) || !dwf) break;
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);
        if (FAILED(d2d->CreateDCRenderTarget(&props, &rt)) || !rt) break;

        HDC scr = GetDC(nullptr);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = stride;
        bi.bmiHeader.biHeight = -stride;      // 自上而下
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        bmp = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        dc = CreateCompatibleDC(scr);
        ReleaseDC(nullptr, scr);
        if (!bmp || !dc || !bits) break;
        SelectObject(dc, bmp);
        RECT br{0, 0, stride, stride};
        rt->BindDC(dc, &br);
        rt->SetDpi(96.0f, 96.0f);

        if (FAILED(dwf->CreateTextFormat(L"Segoe UI Emoji", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                         (float)S(21), L"", &fmt)) || !fmt) break;
        fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        ready = true;
    } while (false);

    if (!ready) WarmLog("  warm: worker-not-ready sz=%d\n", sz, 0);

    std::vector<uint32_t> px((size_t)stride * stride);
    for (;;) {
        EmoReq r;
        {
            std::unique_lock<std::mutex> lk(g_emoMx);
            g_emoCv.wait_for(lk, std::chrono::milliseconds(20), [] {
                return g_emoQuit || !g_emoReq.empty() ||
                       g_emoWarmIdx.load() < (int)g_emoWarmJobs.size();
            });
            if (g_emoQuit) break;
            if (!g_emoReq.empty()) {
                r = std::move(g_emoReq.back());     // 后进先出：最近滚到的格子优先
                g_emoReq.pop_back();
            } else {
                int wi = g_emoWarmIdx.fetch_add(1);
                if (wi >= (int)g_emoWarmJobs.size()) continue;   // 预热已跑完
                r = g_emoWarmJobs[(size_t)wi];
            }
        }
        if (!ready) {
            std::lock_guard<std::mutex> lk(g_emoMx);
            g_emoPending.erase(r.key);
            continue;
        }
        std::wstring w = U8N(r.key.data(), (int)r.key.size());
        rt->BeginDraw();
        rt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
        rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        ID2D1SolidColorBrush* br = nullptr;
        if (SUCCEEDED(rt->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &br)) && br) {
            rt->DrawTextW(w.c_str(), (UINT32)w.size(), fmt,
                          D2D1::RectF(0.0f, 0.0f, (float)sz, (float)sz), br,
                          D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
            br->Release();
        }
        rt->EndDraw();
        std::memcpy(px.data(), bits, (size_t)stride * stride * 4);

        std::lock_guard<std::mutex> lk(g_emoMx);
        g_emoPending.erase(r.key);
        // 预热期间顺手把像素留在整块镜像里，全部完成后一次写盘
        if (r.slot >= 0 && !g_emoDisk.empty()) {
            size_t off = (size_t)r.slot * stride * stride;
            if (off + (size_t)stride * stride <= g_emoDisk.size())
                std::memcpy(g_emoDisk.data() + off, px.data(), (size_t)stride * stride * 4);
        }
        g_emoDone.push_back(EmoDone{r.slot, r.key, px});
    }

    if (fmt) fmt->Release();
    if (rt) rt->Release();
    if (dwf) dwf->Release();
    if (d2d) d2d->Release();
    if (bmp) DeleteObject(bmp);
    if (dc) DeleteDC(dc);
    CoUninitialize();
}

static void StopEmojiWorker() {
    {
        std::lock_guard<std::mutex> lk(g_emoMx);
        g_emoQuit = true;
    }
    g_emoCv.notify_all();
    for (auto& t : g_emoThr) if (t.joinable()) t.join();
    g_emoThr.clear();
}

static void ReleaseEmojiCache() {
    StopEmojiWorker();
    {
        std::lock_guard<std::mutex> lk(g_emoMx);
        g_emoReq.clear();
        g_emoPending.clear();
        g_emoDone.clear();
        g_emoWarmJobs.clear();
        g_emoDisk.clear();
        g_emoDisk.shrink_to_fit();
        g_emoQuit = false;
        g_emoSlot.clear();
        g_emoSlotKey.clear();
        g_emoSlotUsed.clear();
        g_emoReady.clear();
        g_emoFrame = 0;
    }
    g_emoWarmIdx.store(0);
    if (g_emoAtlas) { g_emoAtlas->Release(); g_emoAtlas = nullptr; }
    g_emoSz = 0; g_emoAtlasCols = 0; g_emoAtlasSlots = 0; g_emoNextSlot = 0;
    g_emoWarmPosted = false; g_emoWarmSaved = false; g_emoWarmSlots = 0; g_emoUpN = 0;
}

static bool EnsureEmojiCache(int sz) {
    if (g_emoAtlas && g_emoSz == sz) return true;
    ReleaseEmojiCache();
    if (!g_rt) return false;

    // 格子步长留 4px 透明边，线性采样不会串到邻格
    const int stride = sz + S(4);

    if (g_emoTotal <= 0)
        for (size_t i = 0; i < g_emoCats.size(); ++i) g_emoTotal += g_emoCats[i].count;

    // 容量按「全部 emoji 条数 + 8%」定：装得下就永不淘汰
    int need = g_emoTotal + g_emoTotal / 12 + 16;
    if (need < 256) need = 256;
    int aCols = 16;
    while (aCols * aCols < need) ++aCols;
    // 单张图集边长上限 4096（再大会拖慢整块拷贝与显存占用）
    int capCols = 4096 / (stride > 0 ? stride : 1);
    if (capCols < 16) capCols = 16;
    if (aCols > capCols) aCols = capCols;

    const int axis = aCols * stride;
    D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    if (FAILED(g_rt->CreateBitmap(D2D1::SizeU((UINT32)axis, (UINT32)axis), bp, &g_emoAtlas)) ||
        !g_emoAtlas) {
        g_emoAtlas = nullptr;
        return false;
    }
    g_emoSz = sz;
    g_emoAtlasCols = aCols;
    g_emoAtlasSlots = aCols * aCols;
    g_emoNextSlot = 0;
    g_emoFrame = 0;
    g_emoSlotKey.assign((size_t)g_emoAtlasSlots, std::string());
    g_emoSlotUsed.assign((size_t)g_emoAtlasSlots, 0);
    g_emoReady.assign((size_t)g_emoAtlasSlots, 0);

    std::lock_guard<std::mutex> lk(g_emoMx);
    g_emoQuit = false;
    // 单线程光栅化首屏 63 格要 300ms 左右，两个 worker 并行能压到一半
    for (int i = 0; i < 2; ++i) g_emoThr.emplace_back(EmojiWorkerMain, sz);
    return true;
}

// 分配一个图集槽：先用没占过的，用尽后淘汰「最久没出现在视口里」的那个。
// 只有图集被 cap 住、装不下全部字形时才会走到淘汰分支。
static int AllocEmojiSlot() {
    if (g_emoNextSlot < g_emoAtlasSlots) return g_emoNextSlot++;
    int best = -1, bestUsed = INT_MAX;
    for (size_t i = 0; i < g_emoSlotUsed.size(); ++i) {
        int used = g_emoSlotUsed[i];
        if (used < bestUsed) { bestUsed = used; best = (int)i; }
    }
    if (best < 0 || bestUsed >= g_emoFrame) return -1;   // 全部都在当前视口里
    g_emoSlot.erase(g_emoSlotKey[(size_t)best]);
    g_emoSlotKey[(size_t)best].clear();
    g_emoReady[(size_t)best] = 0;
    return best;
}

static D2D1_RECT_F EmojiSlotRect(int slot) {
    const int stride = EmojiStride();
    float x = (float)((slot % g_emoAtlasCols) * stride);
    float y = (float)((slot / g_emoAtlasCols) * stride);
    return D2D1::RectF(x, y, x + (float)g_emoSz, y + (float)g_emoSz);
}

// -------------------------------------------------------------- 磁盘缓存
static std::wstring EmojiCacheDir() {
    wchar_t buf[MAX_PATH] = L"";
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) GetTempPathW(MAX_PATH, buf);
    std::wstring d = std::wstring(buf) + L"\\WinVClip";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

static std::wstring EmojiCacheFile() {
    wchar_t nm[96];
    wsprintfW(nm, L"\\emoji-atlas-v2-%dpx-%d.bin", g_emoSz, g_emoAtlasCols);
    return EmojiCacheDir() + nm;
}

// 面板尺寸记忆：和字形缓存放同一目录，一行「宽 高」
static std::wstring ViewSizeFile() { return EmojiCacheDir() + L"\\view-size.txt"; }

static void LoadViewSize() {
    FILE* fp = _wfopen(ViewSizeFile().c_str(), L"rb");
    if (!fp) return;
    int w = 0, h = 0;
    if (fscanf(fp, "%d %d", &w, &h) == 2 && w > 0 && h > 0) { g_savedW = w; g_savedH = h; }
    fclose(fp);
}

static void SaveViewSize() {
    if (g_panelW <= 0 || g_panelH <= 0) return;
    g_savedW = g_panelW;
    g_savedH = g_panelH;
    FILE* fp = _wfopen(ViewSizeFile().c_str(), L"wb");
    if (!fp) return;
    fprintf(fp, "%d %d\n", g_savedW, g_savedH);
    fclose(fp);
}

// 从磁盘恢复整块图集：一次 CopyFromMemory 就把所有字形装好，一个都不用光栅化
static bool LoadEmojiCache() {
    if (!g_emoAtlas) return false;
    FILE* fp = _wfopen(EmojiCacheFile().c_str(), L"rb");
    if (!fp) return false;

    bool ok = false;
    unsigned int hdr[8] = {0};
    do {
        if (fread(hdr, sizeof(hdr), 1, fp) != 1) break;
        if (hdr[0] != 0x564F4D45u /* 'EMOV' */ || hdr[1] != 2u) break;
        const int sz = (int)hdr[2], stride = (int)hdr[3], cols = (int)hdr[4];
        const int slots = (int)hdr[5], count = (int)hdr[6], axis = (int)hdr[7];
        if (sz != g_emoSz || stride != EmojiStride() || cols != g_emoAtlasCols ||
            slots != g_emoAtlasSlots || axis != EmojiSide()) break;
        if (count <= 0 || count > slots) break;

        std::vector<std::pair<int, std::string>> idx;
        idx.reserve((size_t)count);
        bool io = true;
        for (int i = 0; i < count; ++i) {
            unsigned int s = 0, l = 0;
            if (fread(&s, 4, 1, fp) != 1 || fread(&l, 4, 1, fp) != 1) { io = false; break; }
            if (l == 0 || l > 96 || (int)s >= slots) { io = false; break; }
            std::string k(l, '\0');
            if (fread(&k[0], 1, l, fp) != l) { io = false; break; }
            idx.emplace_back((int)s, std::move(k));
        }
        if (!io) break;

        std::vector<uint32_t> px((size_t)slots * stride * stride, 0);
        if (fread(px.data(), 4, px.size(), fp) != px.size()) break;

        D2D1_RECT_U all{0, 0, (UINT32)axis, (UINT32)axis};
        HRESULT hr = g_emoAtlas->CopyFromMemory(&all, px.data(), (UINT32)(axis * 4));
        // 逐格再确认一遍：实测「一次性整块 CopyFromMemory」在软件 DCRenderTarget 上
        // 会返回 S_OK 但内容错位（每格都像被压扁的邻居），所以始终以逐格为准。
        for (auto& e : idx) {
            D2D1_RECT_U r;
            r.left = (UINT32)((e.first % cols) * stride);
            r.top = (UINT32)((e.first / cols) * stride);
            r.right = r.left + (UINT32)stride;
            r.bottom = r.top + (UINT32)stride;
            if (FAILED(g_emoAtlas->CopyFromMemory(&r, px.data() + (size_t)e.first * stride * stride,
                                                  (UINT32)(stride * 4)))) {
                WarmLog("  load: cell-copy-fail slot=%d hr=0x%x\n", e.first, (int)hr);
            }
        }
        WarmLog("  load: cells=%d bulk-hr=0x%x\n", count, (int)hr);

        for (auto& e : idx) {
            g_emoSlot[e.second] = e.first;
            if ((size_t)e.first < g_emoSlotKey.size()) g_emoSlotKey[(size_t)e.first] = e.second;
            if ((size_t)e.first < g_emoReady.size()) g_emoReady[(size_t)e.first] = 1;
        }
        g_emoWarmPosted = true;      // 全部就绪，不再预热
        g_emoWarmSaved = true;       // 已有缓存，不必再写
        g_emoNextSlot = slots;       // 新字形才继续分配（正常不会发生）
        g_emoUpN = count;
        ok = true;
    } while (false);
    fclose(fp);
    return ok;
}

// 把整块图集写盘。数据源是预热期间的镜像 g_emoDisk，写完即释放。
static void SaveEmojiCache() {
    if (g_emoWarmSaved || g_emoDisk.empty() || !g_emoAtlas) return;
    g_emoWarmSaved = true;

    const int stride = EmojiStride();
    const int axis = EmojiSide();
    int count = 0;
    for (auto& k : g_emoSlotKey) if (!k.empty()) ++count;
    if (count <= 0) return;

    FILE* fp = _wfopen(EmojiCacheFile().c_str(), L"wb");
    if (!fp) return;
    unsigned int hdr[8] = {0x564F4D45u, 2u, (unsigned)g_emoSz, (unsigned)stride,
                           (unsigned)g_emoAtlasCols, (unsigned)g_emoAtlasSlots,
                           (unsigned)count, (unsigned)axis};
    fwrite(hdr, sizeof(hdr), 1, fp);
    for (size_t i = 0; i < g_emoSlotKey.size(); ++i) {
        if (g_emoSlotKey[i].empty()) continue;
        unsigned int s = (unsigned)i, l = (unsigned)g_emoSlotKey[i].size();
        fwrite(&s, 4, 1, fp);
        fwrite(&l, 4, 1, fp);
        fwrite(g_emoSlotKey[i].data(), 1, l, fp);
    }
    fwrite(g_emoDisk.data(), 4, g_emoDisk.size(), fp);
    fclose(fp);
    WarmLog("  warm: saved slots=%d up=%d\n", g_emoAtlasSlots, g_emoUpN);

    g_emoDisk.clear();
    g_emoDisk.shrink_to_fit();
}

// 启动预热：读缓存命中就整块恢复，否则把全部 emoji 排进低优先队列
static void StartEmojiWarm() {
    if (g_emoWarmPosted) return;
    if (!EnsureEmojiCache(S(44))) { WarmLog("  warm: ensure-fail sz=%d dpi=%d\n", S(44), 0); return; }
    g_emoWarmPosted = true;
    if (LoadEmojiCache()) { WarmLog("  warm: cache-hit total=%d slots=%d\n", g_emoTotal, g_emoAtlasSlots); return; }

    std::vector<std::string> keys;
    keys.reserve((size_t)(g_emoTotal > 0 ? g_emoTotal : 2048));
    for (int i = 1; i <= GlyphCatTotal(PG_EMOJI); ++i) {
        GCat& c = GlyphCat(PG_EMOJI, i);
        LoadCat(c, PG_EMOJI);
        for (auto& it : c.items) keys.emplace_back(it.g, (size_t)it.gl);
    }
    if (keys.empty()) { g_emoWarmPosted = false; WarmLog("  warm: no-keys total=%d cats=%d\n", g_emoTotal, GlyphCatTotal(PG_EMOJI)); return; }

    const int stride = EmojiStride();
    // 整块镜像：预热期间每一格都留一份，全部完成后一次写盘
    g_emoDisk.assign((size_t)g_emoAtlasSlots * stride * stride, 0);

    std::vector<EmoReq> jobs;
    jobs.reserve(keys.size());
    for (auto& k : keys) {
        if (g_emoSlot.find(k) != g_emoSlot.end()) continue;   // 视口已经抢先渲染过
        int slot = AllocEmojiSlot();
        if (slot < 0) break;                                  // 图集被 cap 住且已满
        g_emoSlot[k] = slot;
        g_emoSlotKey[(size_t)slot] = k;
        jobs.push_back(EmoReq{k, slot});
    }
    if (jobs.empty()) { g_emoDisk.clear(); g_emoDisk.shrink_to_fit(); WarmLog("  warm: no-jobs keys=%d slots=%d\n", (int)keys.size(), g_emoAtlasSlots); return; }

    {
        std::lock_guard<std::mutex> lk(g_emoMx);
        g_emoWarmSlots = (int)jobs.size();
        g_emoWarmJobs = std::move(jobs);
        g_emoWarmIdx.store(0);
    }
    WarmLog("  warm: posted jobs=%d slots=%d\n", g_emoWarmSlots, g_emoAtlasSlots);
    g_emoCv.notify_all();
}

// 把工作线程做好的像素搬进 atlas，并在全量预热跑完后落盘。
// 与当前页面无关：预热期间面板可能没显示、也可能停在历史页，都得继续推进。
// 返回是否还有未完成的活（调用方据此续排定时器）。
static bool PumpEmojiUploads() {
    if (!g_emoAtlas) return false;
    const int stride = EmojiStride();
    const int cols = g_emoAtlasCols;

    std::vector<EmoDone> done;
    {
        std::lock_guard<std::mutex> lk(g_emoMx);
        done.swap(g_emoDone);
    }
    // 一帧最多 160 格，别让拷贝把单帧拖长（160 格约 1.5ms）
    int up = 0;
    size_t used = 0;
    for (; used < done.size() && up < 160; ++used) {
        EmoDone& d = done[used];
        if (d.slot < 0 || d.slot >= g_emoAtlasSlots ||
            (int)d.px.size() != stride * stride) { ++g_emoFailN; continue; }
        D2D1_RECT_U r;
        r.left = (UINT32)((d.slot % cols) * stride);
        r.top = (UINT32)((d.slot / cols) * stride);
        r.right = r.left + (UINT32)stride;
        r.bottom = r.top + (UINT32)stride;
        if (FAILED(g_emoAtlas->CopyFromMemory(&r, d.px.data(), (UINT32)(stride * 4)))) {
            ++g_emoFailN;
            continue;
        }
        g_emoReady[(size_t)d.slot] = 1;
        if ((size_t)d.slot < g_emoSlotUsed.size()) g_emoSlotUsed[(size_t)d.slot] = g_emoFrame;
        ++g_emoNewN;
        ++g_emoUpN;
        ++up;
    }
    if (used < done.size()) {          // 没消化完的留到下一帧
        std::lock_guard<std::mutex> lk(g_emoMx);
        done.erase(done.begin(), done.begin() + (ptrdiff_t)used);
        g_emoDone.insert(g_emoDone.end(),
                         std::make_move_iterator(done.begin()),
                         std::make_move_iterator(done.end()));
    }

    // 全部预热格都进 atlas 了：整块落盘，下次启动直接载入
    if (g_emoWarmSlots > 0 && !g_emoWarmSaved && g_emoUpN >= g_emoWarmSlots) SaveEmojiCache();

    std::lock_guard<std::mutex> lk(g_emoMx);
    return !g_emoPending.empty() || !g_emoDone.empty() ||
           g_emoWarmIdx.load() < (int)g_emoWarmJobs.size();
}

// UI 线程每帧调一次：上传已完成的像素 + 投递缺失格。绝不做光栅化。
static void PrepareEmojiBitmaps() {
    if (!g_rt || !g_emojiFmt) return;
    std::vector<std::pair<RECT, int>> cells;
    CollectEmojiCells(cells);
    if (cells.empty()) return;
    const std::vector<GItem>& list = GlyphItems(PG_EMOJI);
    if (!EnsureEmojiCache(S(44))) return;

    // 0) 首次进来：先看磁盘缓存（命中的话这一帧就是全彩的）
    if (!g_emoCacheTried) {
        g_emoCacheTried = true;
        StartEmojiWarm();
    }

    // 1) 先把工作线程做好的像素搬进 atlas（与当前页面无关，预热期间也要推进）
    bool more = PumpEmojiUploads();

    // 2) 刷新可见槽位的「最近使用」（淘汰兜底用）
    ++g_emoFrame;
    for (auto& c : cells) {
        const GItem& it = list[(size_t)c.second];
        std::string key(it.g, (size_t)it.gl);
        auto f = g_emoSlot.find(key);
        if (f != g_emoSlot.end() && (size_t)f->second < g_emoSlotUsed.size())
            g_emoSlotUsed[(size_t)f->second] = g_emoFrame;
    }

    // 3) 投递缺失格。队列是后进先出，所以「先投预取、后投视口内」，
    //    这样视口内的格子总排在前面先被渲染。
    bool posted = false;
    {
        std::lock_guard<std::mutex> lk(g_emoMx);
        auto post = [&](int idx) {
            if (idx < 0 || idx >= (int)list.size()) return;
            const GItem& it = list[(size_t)idx];
            std::string key(it.g, (size_t)it.gl);
            auto f = g_emoSlot.find(key);
            if (f != g_emoSlot.end()) {
                int s0 = f->second;
                if ((size_t)s0 < g_emoReady.size() && g_emoReady[(size_t)s0]) return;
            }
            if (g_emoPending.find(key) != g_emoPending.end()) return;
            int slot = AllocEmojiSlot();
            if (slot < 0) return;
            g_emoSlot[key] = slot;
            if ((size_t)slot < g_emoSlotKey.size()) g_emoSlotKey[(size_t)slot] = key;
            g_emoPending.insert(key);
            g_emoReq.push_back(EmoReq{std::move(key), slot});
            posted = true;
        };
        // 预取：视口上下各一行，滚动时下一屏通常已经就绪
        const int ec = GlyphCols();
        if (!cells.empty()) {
            int lo = cells.front().second - ec;
            int hi = cells.back().second + ec;
            for (int i = lo; i <= hi; ++i)
                if (i < cells.front().second || i > cells.back().second) post(i);
        }
        for (auto& c : cells) post(c.second);
    }
    if (posted) g_emoCv.notify_one();

    // 4) 统计可见格命中数
    {
        std::lock_guard<std::mutex> lk(g_emoMx);
        for (auto& c : cells) {
            const GItem& it = list[(size_t)c.second];
            std::string key(it.g, (size_t)it.gl);
            auto f = g_emoSlot.find(key);
            if (f != g_emoSlot.end() && (size_t)f->second < g_emoReady.size() &&
                g_emoReady[(size_t)f->second]) ++g_emoHitN;
        }
    }

    // 5) 还有格子在路上（或预热未完成）：排一个 8ms 的重绘，补齐后自然停下
    if (more && g_panel) SetTimer(g_panel, TIMER_EMOJI, 8, nullptr);
}

// ================================================================== 图片缩略图
// 思路：PNG 走 WIC 解码、CF_DIB 交给 GDI 转 32bpp（省掉各种位深/调色板的解析），
// 统一得到「原始尺寸的 BGRA」后自己盒式下采样到 36px，最后在 CPU 上做圆角抗锯齿
// 蒙版并预乘 alpha —— 预乘之后 DrawBitmap 的 source-over 就能得到平滑圆角，
// 不需要 D2D 的 layer（DC render target 不支持 PushLayer 的几何遮罩）。

// Rgba 与 WIC 像素格式 GUID 见 io_png.h

// g_wic / InitWic 见 io_png.cpp

static bool PngToRgba(const std::vector<BYTE>& png, Rgba& out) {
    InitWic();
    if (!g_wic || png.size() < 32) return false;
    IWICStream* st = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* fr = nullptr;
    IWICFormatConverter* cv = nullptr;
    bool ok = false;
    if (SUCCEEDED(g_wic->CreateStream(&st)) &&
        SUCCEEDED(st->InitializeFromMemory(const_cast<BYTE*>(png.data()), (DWORD)png.size())) &&
        SUCCEEDED(g_wic->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &fr)) &&
        SUCCEEDED(g_wic->CreateFormatConverter(&cv)) &&
        SUCCEEDED(cv->Initialize(fr, kWicFmt32BGRA, WICBitmapDitherTypeNone,
                                 nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        UINT w = 0, h = 0;
        if (SUCCEEDED(cv->GetSize(&w, &h)) && w > 0 && h > 0 &&
            (UINT64)w * h <= 64ull * 1024 * 1024) {
            out.w = (int)w; out.h = (int)h;
            out.px.resize((size_t)w * h * 4);
            if (SUCCEEDED(cv->CopyPixels(nullptr, w * 4, (UINT)out.px.size(), out.px.data())))
                ok = true;
        }
    }
    if (cv) cv->Release();
    if (fr) fr->Release();
    if (dec) dec->Release();
    if (st) st->Release();
    return ok;
}

// CF_DIB：让 GDI 替我们把各种位深/调色板/上下翻转统一成 32bpp，本身不缩放
static bool DibToRgba(const std::vector<BYTE>& dib, Rgba& out) {
    if (dib.size() < sizeof(BITMAPINFOHEADER)) return false;
    const BITMAPINFOHEADER* bi = (const BITMAPINFOHEADER*)dib.data();
    if (bi->biSize < sizeof(BITMAPINFOHEADER) || bi->biWidth <= 0) return false;
    int w = bi->biWidth;
    int h = bi->biHeight < 0 ? -bi->biHeight : bi->biHeight;
    if (w <= 0 || h <= 0 || (UINT64)w * h > 64ull * 1024 * 1024) return false;

    size_t off = bi->biSize;
    if (bi->biCompression == BI_BITFIELDS && bi->biSize == sizeof(BITMAPINFOHEADER))
        off += 12;                                   // 3 个 color mask 紧跟 40 字节头
    if (bi->biBitCount <= 8)
        off += (size_t)(bi->biClrUsed ? bi->biClrUsed : (1u << bi->biBitCount)) * 4;
    if (off >= dib.size()) return false;

    BITMAPINFO dst{};
    dst.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    dst.bmiHeader.biWidth       = w;
    dst.bmiHeader.biHeight      = -h;                 // 自上而下，与我们自己的行序一致
    dst.bmiHeader.biPlanes      = 1;
    dst.bmiHeader.biBitCount    = 32;
    dst.bmiHeader.biCompression = BI_RGB;

    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return false;
    void* bits = nullptr;
    HBITMAP bm = CreateDIBSection(dc, &dst, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bm || !bits) { if (bm) DeleteObject(bm); DeleteDC(dc); return false; }
    HGDIOBJ ob = SelectObject(dc, bm);
    int lines = StretchDIBits(dc, 0, 0, w, h, 0, 0, w, h, dib.data() + off,
                              (BITMAPINFO*)dib.data(), DIB_RGB_COLORS, SRCCOPY);
    bool ok = lines > 0;
    if (ok) {
        out.w = w; out.h = h;
        out.px.assign((BYTE*)bits, (BYTE*)bits + (size_t)w * h * 4);
        for (size_t i = 3; i < out.px.size(); i += 4) out.px[i] = 255;  // GDI 不给 alpha
    }
    SelectObject(dc, ob);
    DeleteObject(bm);
    DeleteDC(dc);
    return ok;
}

// 盒式平均下采样：把每个源像素按面积归到对应目标桶，一遍扫完
static void BoxDownsample(const Rgba& s, int tw, int th, std::vector<BYTE>& dst) {
    dst.assign((size_t)tw * th * 4, 0);
    if (s.w <= 0 || s.h <= 0) return;
    if (tw >= s.w || th >= s.h) {                    // 放大就最近邻，别做无意义平均
        for (int y = 0; y < th; ++y) {
            int sy = (int)((long long)y * s.h / th);
            for (int x = 0; x < tw; ++x) {
                int sx = (int)((long long)x * s.w / tw);
                memcpy(&dst[((size_t)y * tw + x) * 4], &s.px[((size_t)sy * s.w + sx) * 4], 4);
            }
        }
        return;
    }
    std::vector<unsigned long long> acc((size_t)tw * th * 4, 0);
    std::vector<unsigned> cnt((size_t)tw * th, 0);
    for (int sy = 0; sy < s.h; ++sy) {
        size_t ty = (size_t)((long long)sy * th / s.h);
        const BYTE* row = &s.px[(size_t)sy * s.w * 4];
        for (int sx = 0; sx < s.w; ++sx) {
            size_t ti = ty * tw + (size_t)((long long)sx * tw / s.w);
            const BYTE* p = row + (size_t)sx * 4;
            unsigned long long* a = &acc[ti * 4];
            a[0] += p[0]; a[1] += p[1]; a[2] += p[2]; a[3] += p[3];
            ++cnt[ti];
        }
    }
    for (size_t i = 0; i < cnt.size(); ++i) {
        unsigned n = cnt[i];
        if (!n) continue;
        for (int c = 0; c < 4; ++c)
            dst[i * 4 + c] = (BYTE)(acc[i * 4 + c] / n);
    }
}

// 圆角覆盖率：圆角矩形的有符号距离场，d 落在 ±0.5px 内给部分覆盖
static float RoundCoverage(float x, float y, float w, float h, float rad) {
    float hx = w / 2.0f, hy = h / 2.0f;
    float px = x - hx, py = y - hy;
    if (px < 0) px = -px;
    if (py < 0) py = -py;
    float qx = px - (hx - rad), qy = py - (hy - rad);
    if (qx < 0) qx = 0;
    if (qy < 0) qy = 0;
    float d = sqrtf(qx * qx + qy * qy) - rad;
    float cov = 0.5f - d;
    return cov < 0 ? 0.0f : (cov > 1 ? 1.0f : cov);
}

static ID2D1Bitmap* MakeCompatBitmap(int w, int h, const std::vector<BYTE>& bgra) {
    if (!g_rt || w <= 0 || h <= 0 || bgra.size() < (size_t)w * h * 4) return nullptr;
    D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    ID2D1Bitmap* bm = nullptr;
    if (FAILED(g_rt->CreateBitmap(D2D1::SizeU((UINT32)w, (UINT32)h), bgra.data(),
                                  (UINT32)w * 4, bp, &bm)) || !bm)
        return nullptr;
    return bm;
}

// 惰性构建 + 缓存：it.thumb 一旦赋值就不再重试（失败永远是空位图，不会每帧重解码）
static Thumb* GetThumb(ClipItem& it) {
    if (it.thumb) return it.thumb.get();
    if (it.kind != 2 || it.bits.empty() || !g_rt) return nullptr;

    ThumbRef t = std::make_shared<Thumb>();
    it.thumb = t;

    Rgba src;
    bool ok = it.isPng ? PngToRgba(it.bits, src) : DibToRgba(it.bits, src);
    if (!ok) return t.get();

    int box = S(36);
    double sc = (std::min)((double)box / src.w, (double)box / src.h);
    int tw = (std::max)(1, (int)lround(src.w * sc));
    int th = (std::max)(1, (int)lround(src.h * sc));

    std::vector<BYTE> small;
    BoxDownsample(src, tw, th, small);
    std::vector<BYTE>().swap(src.px);                // 原图缓冲立刻还回去，别和缩略图一起占着

    // 圆角 + 预乘
    float rad = (float)S(6) * (float)tw / (float)box;
    if (rad < 1.0f) rad = 1.0f;
    if (rad > (std::min)(tw, th) / 2.0f) rad = (std::min)(tw, th) / 2.0f;
    for (int y = 0; y < th; ++y) {
        for (int x = 0; x < tw; ++x) {
            BYTE* p = &small[((size_t)y * tw + x) * 4];
            float cov = RoundCoverage(x + 0.5f, y + 0.5f, (float)tw, (float)th, rad);
            unsigned a = (unsigned)(p[3] * cov + 0.5f);
            p[0] = (BYTE)(p[0] * a / 255);
            p[1] = (BYTE)(p[1] * a / 255);
            p[2] = (BYTE)(p[2] * a / 255);
            p[3] = (BYTE)a;
        }
    }

    t->bmp = MakeCompatBitmap(tw, th, small);
    t->w = tw; t->h = th;
    return t.get();
}

// 在列表行左侧画出缩略图（保持宽高比，居中于原来的 28px 图标块）
static bool DrawThumb(HDC dc, ClipItem& it, const RECT& iconBox) {
    (void)dc;
    if (!g_rt || !g_painting) return false;
    Thumb* t = GetThumb(it);
    if (!t || !t->bmp) return false;

    int cx = (iconBox.left + iconBox.right) / 2;
    int cy = (iconBox.top + iconBox.bottom) / 2;
    int l = cx - t->w / 2, tp = cy - t->h / 2;
    D2D1_RECT_F dst = D2D1::RectF((float)l, (float)tp, (float)(l + t->w), (float)(tp + t->h));

    // 浅底垫在透明区域下（PNG 也有全透明的地方）
    RECT bg{l, tp, l + t->w, tp + t->h};
    FillRound(dc, bg, S(6), C_SUBTLE, C_SUBTLE, 0);
    g_rt->DrawBitmap(t->bmp, dst, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);

    float rad = (float)S(6) / 2.0f;
    float maxRad = (float)(std::min)(t->w, t->h) / 2.0f;
    if (rad > maxRad) rad = maxRad;
    Brush(C_LINE);
    g_rt->DrawRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF((float)l + 0.5f, (float)tp + 0.5f,
                                      (float)(l + t->w) - 0.5f, (float)(tp + t->h) - 0.5f),
                          rad, rad),
        g_brush, 1.0f);
    return true;
}

// ================================================================== 剪贴板读取
static void ReadPngSize(const std::vector<BYTE>& b, int& w, int& h) {
    w = h = 0;
    if (b.size() < 24) return;
    if (!(b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G')) return;
    w = (b[16] << 24) | (b[17] << 16) | (b[18] << 8) | b[19];
    h = (b[20] << 24) | (b[21] << 16) | (b[22] << 8) | b[23];
}

static bool SameBits(const std::vector<BYTE>& a, const std::vector<BYTE>& b) {
    return a.size() == b.size() && (a.empty() || memcmp(a.data(), b.data(), a.size()) == 0);
}

static void CaptureClipboard();

static void OnClipboardChanged() { SetTimer(g_main, TIMER_CLIP, 160, nullptr); }

static std::wstring ForegroundSource() {
    HWND fg = GetForegroundWindow();
    if (!fg || fg == g_panel) return L"桌面";
    wchar_t title[256]{};
    GetWindowTextW(fg, title, 255);
    std::wstring s = title;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (s.empty() || s.size() > 24) {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (h) {
            wchar_t path[MAX_PATH]{};
            DWORD n = MAX_PATH;
            if (QueryFullProcessImageNameW(h, 0, path, &n)) {
                std::wstring base = FileBase(path);
                size_t dot = base.find_last_of(L'.');
                if (dot != std::wstring::npos) base.resize(dot);
                s = base;
            }
            CloseHandle(h);
        }
    }
    if (s.empty()) s = L"未知";
    if (s.size() > 20) s = s.substr(0, 20) + L"…";
    return s;
}

static void CaptureClipboard() {
    // 自己写进去的内容（借用剪贴板粘贴表情/符号、或还原备份）不进历史，
    // 否则每点一个表情，历史里就多一条孤零零的 emoji。
    {
        DWORD seq = GetClipboardSequenceNumber();
        if (g_selfClipSeq && seq == g_selfClipSeq) return;
    }
    if (!OpenClipboard(g_main)) { SetTimer(g_main, TIMER_CLIP, 160, nullptr); return; }

    ClipItem it;
    GetLocalTime(&it.st);
    it.source = ForegroundSource();

    HANDLE h = nullptr;
    if ((h = GetClipboardData(CF_UNICODETEXT)) != nullptr) {
        auto* p = (const wchar_t*)GlobalLock(h);
        if (p) { it.kind = 0; it.text = p; GlobalUnlock(h); }
    }
    if (it.text.empty() && (h = GetClipboardData(CF_HDROP)) != nullptr) {
        auto drop = (HDROP)h;
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        if (n > 0 && n < 1000) {
            it.kind = 1;
            for (UINT i = 0; i < n; ++i) {
                UINT len = DragQueryFileW(drop, i, nullptr, 0);
                std::wstring s(len + 1, L'\0');
                DragQueryFileW(drop, i, &s[0], len + 1);
                s.resize(len);
                it.files.push_back(s);
            }
        }
    }
    if (it.text.empty() && it.files.empty()) {
        if (g_cfPng && (h = GetClipboardData(g_cfPng)) != nullptr) {
            SIZE_T sz = GlobalSize(h);
            auto* p = (const BYTE*)GlobalLock(h);
            if (p && sz > 32 && sz < 64u * 1024 * 1024) {
                it.kind = 2; it.isPng = true;
                it.bits.assign(p, p + sz);
                ReadPngSize(it.bits, it.imgW, it.imgH);
            }
            if (p) GlobalUnlock(h);
        } else if ((h = GetClipboardData(CF_DIB)) != nullptr) {
            SIZE_T sz = GlobalSize(h);
            auto* p = (const BYTE*)GlobalLock(h);
            if (p && sz > sizeof(BITMAPINFOHEADER) && sz < 128u * 1024 * 1024) {
                auto* bi = (const BITMAPINFOHEADER*)p;
                if (bi->biWidth > 0 && bi->biWidth < 100000) {
                    it.kind = 2; it.isPng = false;
                    it.bits.assign(p, p + sz);
                    it.imgW = bi->biWidth;
                    it.imgH = bi->biHeight < 0 ? -bi->biHeight : bi->biHeight;
                }
            }
            if (p) GlobalUnlock(h);
        }
    }
    CloseClipboard();

    if (it.kind == 0) {
        it.bytes = Utf8Bytes(it.text);
    } else if (it.kind == 1) {
        for (auto& f : it.files) it.bytes += PathBytes(f);
    } else {
        it.bytes = it.bits.size();
    }

    if (it.text.empty() && it.files.empty() && it.kind != 2) return;

    if (it.kind == 0) {
        std::wstring t = it.text;
        size_t nl = t.find_first_of(L"\r\n");
        if (nl != std::wstring::npos) t.resize(nl);
        for (auto& c : t) if (c == L'\t') c = L' ';
        if (t.size() > 200) t = t.substr(0, 200) + L"…";
        if (t.empty()) t = L"(空白文本)";
        it.title = t;

        std::wstring lo = Lower(it.text);
        if (lo.rfind(L"http://", 0) == 0 || lo.rfind(L"https://", 0) == 0 ||
            lo.rfind(L"www.", 0) == 0) {
            it.isLink = true; it.tag = L"链接";
        } else if (it.text.find(L'{') != std::wstring::npos ||
                   it.text.find(L";\n") != std::wstring::npos ||
                   lo.rfind(L"git ", 0) == 0 || lo.rfind(L"npm ", 0) == 0 ||
                   lo.rfind(L"const ", 0) == 0) {
            it.isCode = true; it.tag = L"代码";
        } else {
            it.tag = L"文本";
        }
    } else if (it.kind == 1) {
        it.title = FileBase(it.files[0]);
        if (it.files.size() > 1)
            it.title += L"  等 " + std::to_wstring(it.files.size()) + L" 项";
        it.tag = L"文件";
    } else {
        it.title = L"图片 " + std::to_wstring(it.imgW) + L" × " + std::to_wstring(it.imgH);
        it.tag = L"图片";
    }

    // 去重
    if (!g_items.empty()) {
        const ClipItem& f = g_items.front();
        bool same = (f.kind == it.kind);
        if (same) {
            if (it.kind == 0) same = (f.text == it.text);
            else if (it.kind == 1) same = (f.files == it.files);
            else same = (f.isPng == it.isPng && SameBits(f.bits, it.bits));
        }
        if (same) return;
        for (size_t i = 1; i < g_items.size(); ++i) {
            ClipItem& o = g_items[i];
            bool dup = (o.kind == it.kind);
            if (dup) {
                if (it.kind == 0) dup = (o.text == it.text);
                else if (it.kind == 1) dup = (o.files == it.files);
                else dup = (o.isPng == it.isPng && SameBits(o.bits, it.bits));
            }
            if (dup) { g_items.erase(g_items.begin() + i); break; }
        }
    }
    g_items.push_front(std::move(it));
    while (g_items.size() > 60) g_items.pop_back();
}

// ================================================================== 写回剪贴板
static void PutToClipboard(const ClipItem& it) {
    if (!OpenClipboard(g_main)) return;
    EmptyClipboard();

    if (it.kind == 0) {
        size_t bytes = (it.text.size() + 1) * sizeof(wchar_t);
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (h) {
            void* p = GlobalLock(h);
            memcpy(p, it.text.c_str(), bytes);
            GlobalUnlock(h);
            if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
        }
    } else if (it.kind == 1) {
        size_t chrs = 1;
        for (auto& f : it.files) chrs += f.size() + 1;
        size_t total = sizeof(DROPFILES) + chrs * sizeof(wchar_t);
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, total);
        if (h) {
            auto* df = (DROPFILES*)GlobalLock(h);
            df->pFiles = sizeof(DROPFILES);
            df->fWide = TRUE;
            auto* p = (wchar_t*)((BYTE*)df + sizeof(DROPFILES));
            for (auto& f : it.files) {
                memcpy(p, f.c_str(), (f.size() + 1) * sizeof(wchar_t));
                p += f.size() + 1;
            }
            *p = 0;
            GlobalUnlock(h);
            if (!SetClipboardData(CF_HDROP, h)) GlobalFree(h);
        }
    } else if (it.kind == 2 && !it.bits.empty()) {
        UINT fmt = (it.isPng && g_cfPng) ? g_cfPng : CF_DIB;
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, it.bits.size());
        if (h) {
            void* p = GlobalLock(h);
            memcpy(p, it.bits.data(), it.bits.size());
            GlobalUnlock(h);
            if (!SetClipboardData(fmt, h)) GlobalFree(h);
        }
    }
    CloseClipboard();
}

// 直接把字符注入到焦点窗口（KEYEVENTF_UNICODE），完全不碰剪贴板：
// 点一个表情不该把用户自己的剪贴板内容和历史都顶掉。
// 代理对（emoji）按 UTF-16 码元逐个发，目标侧的 WM_CHAR 会自己合成。
static void TypeUnicode(const std::wstring& s) {
    if (s.empty()) return;
    std::vector<INPUT> in;
    in.reserve(s.size() * 2);
    for (wchar_t c : s) {
        INPUT a{};
        a.type = INPUT_KEYBOARD;
        a.ki.wVk = 0;
        a.ki.wScan = (WORD)c;
        a.ki.dwFlags = KEYEVENTF_UNICODE;
        MarkMine(a);
        in.push_back(a);
        a.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        in.push_back(a);
    }
    SendInput((UINT)in.size(), in.data(), sizeof(INPUT));
}

static void PutText(const std::wstring& s) {
    if (!OpenClipboard(g_main)) return;
    EmptyClipboard();
    size_t bytes = (s.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (h) {
        void* p = GlobalLock(h);
        memcpy(p, s.c_str(), bytes);
        GlobalUnlock(h);
        if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
    }
    CloseClipboard();
}

// 备份当前剪贴板（只备内容型格式：文本 / 文件列表 / PNG / DIB）。
// 目的：给「取不到插入符」的目标窗口走 Ctrl+V 通道时，粘完还能把用户原来的
// 剪贴板还回去 —— 点一个表情不该留下痕迹。
static void BorrowClipboard() {
    if (g_borrowedClip) return;      // 连续点多个字形：只备份第一次，别把上一次的临时内容当成原内容
    g_clipBak = ClipBak{};
    if (!OpenClipboard(g_main)) { g_borrowedClip = true; return; }

    HANDLE h = nullptr;
    if ((h = GetClipboardData(CF_UNICODETEXT)) != nullptr) {
        if (auto* p = (const wchar_t*)GlobalLock(h)) {
            g_clipBak.hadText = true;
            g_clipBak.text = p;
            GlobalUnlock(h);
        }
    }
    if (!g_clipBak.hadText && (h = GetClipboardData(CF_HDROP)) != nullptr) {
        UINT n = DragQueryFileW((HDROP)h, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n && n < 1000; ++i) {
            UINT len = DragQueryFileW((HDROP)h, i, nullptr, 0);
            std::wstring s(len + 1, L'\0');
            DragQueryFileW((HDROP)h, i, &s[0], len + 1);
            s.resize(len);
            g_clipBak.files.push_back(s);
        }
    }
    if (!g_clipBak.hadText && g_clipBak.files.empty()) {
        if (g_cfPng && (h = GetClipboardData(g_cfPng)) != nullptr) {
            SIZE_T sz = GlobalSize(h);
            if (auto* p = (const BYTE*)GlobalLock(h)) {
                if (sz > 32 && sz < 32u * 1024 * 1024) g_clipBak.png.assign(p, p + sz);
                GlobalUnlock(h);
            }
        } else if ((h = GetClipboardData(CF_DIB)) != nullptr) {
            SIZE_T sz = GlobalSize(h);
            if (auto* p = (const BYTE*)GlobalLock(h)) {
                if (sz > sizeof(BITMAPINFOHEADER) && sz < 32u * 1024 * 1024)
                    g_clipBak.dib.assign(p, p + sz);
                GlobalUnlock(h);
            }
        }
    }
    CloseClipboard();
    g_clipBak.valid = true;
    g_borrowedClip = true;
}

// 还原备份。安全前提：剪贴板里还是我们自己写的那份（序列号比对），
// 期间若有别人复制过东西就放弃还原，绝不覆盖新内容。
static void RestoreClipboard() {
    if (!g_borrowedClip) return;
    g_borrowedClip = false;
    if (g_clipBak.valid && (!g_selfClipSeq || GetClipboardSequenceNumber() == g_selfClipSeq) &&
        OpenClipboard(g_main)) {
        EmptyClipboard();
        if (g_clipBak.hadText) {
            size_t bytes = (g_clipBak.text.size() + 1) * sizeof(wchar_t);
            if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
                void* p = GlobalLock(h);
                memcpy(p, g_clipBak.text.c_str(), bytes);
                GlobalUnlock(h);
                if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
            }
        } else if (!g_clipBak.files.empty()) {
            size_t chrs = 1;
            for (auto& f : g_clipBak.files) chrs += f.size() + 1;
            size_t total = sizeof(DROPFILES) + chrs * sizeof(wchar_t);
            if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, total)) {
                auto* df = (DROPFILES*)GlobalLock(h);
                df->pFiles = sizeof(DROPFILES);
                df->fWide = TRUE;
                auto* p = (wchar_t*)((BYTE*)df + sizeof(DROPFILES));
                for (auto& f : g_clipBak.files) {
                    memcpy(p, f.c_str(), (f.size() + 1) * sizeof(wchar_t));
                    p += f.size() + 1;
                }
                *p = 0;
                GlobalUnlock(h);
                if (!SetClipboardData(CF_HDROP, h)) GlobalFree(h);
            }
        } else if (!g_clipBak.png.empty() && g_cfPng) {
            if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, g_clipBak.png.size())) {
                void* p = GlobalLock(h);
                memcpy(p, g_clipBak.png.data(), g_clipBak.png.size());
                GlobalUnlock(h);
                if (!SetClipboardData(g_cfPng, h)) GlobalFree(h);
            }
        } else if (!g_clipBak.dib.empty()) {
            if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, g_clipBak.dib.size())) {
                void* p = GlobalLock(h);
                memcpy(p, g_clipBak.dib.data(), g_clipBak.dib.size());
                GlobalUnlock(h);
                if (!SetClipboardData(CF_DIB, h)) GlobalFree(h);
            }
        }
        g_selfClipSeq = GetClipboardSequenceNumber();   // 还原动作本身也不该进历史
        CloseClipboard();
    }
    g_clipBak = ClipBak{};
}

// ================================================================== 视图
static bool MatchQuery(const ClipItem& it, const std::wstring& q) {
    if (q.empty()) return true;
    return Lower(it.title).find(q) != std::wstring::npos ||
           Lower(it.source).find(q) != std::wstring::npos ||
           Lower(it.tag).find(q) != std::wstring::npos ||
           Lower(it.text).find(q) != std::wstring::npos;
}

static bool MatchFilter(const ClipItem& it) {
    switch (g_filter) {
    case FL_TEXT:  return it.kind == 0;
    case FL_IMAGE: return it.kind == 2;
    case FL_OTHER: return it.kind == 1;
    default: return true;
    }
}

static void BuildView() {
    g_view.clear();
    std::wstring q = Lower(g_query);
    std::vector<int> tmp;
    for (int i = 0; i < (int)g_items.size(); ++i) {
        const ClipItem& it = g_items[i];
        if (g_page == PG_FAV && !it.fav) continue;
        if (!MatchFilter(it)) continue;
        if (!MatchQuery(it, q)) continue;
        tmp.push_back(i);
    }
    // 置顶优先（稳定排序：保持原相对顺序）
    for (int i : tmp) if (g_items[i].pinned) g_view.push_back(i);
    for (int i : tmp) if (!g_items[i].pinned) g_view.push_back(i);

    if (g_sel < 0) g_sel = 0;
    if (g_sel >= (int)g_view.size()) g_sel = (int)g_view.size() - 1;
    if (g_sel < 0) g_sel = 0;
}

// 页面上的条目数（用于分类条计数）
static int PageCount() {
    if (g_page == PG_EMOJI || g_page == PG_SYMBOL) return GlyphCount(g_page);
    return (int)g_view.size();
}

// ================================================================== 布局
struct Layout {
    int W, H;
    RECT head, logo, tab[4], gear, closeB, pinB;
    RECT searchBox, filters, body, foot;
    RECT chip[8]; int chipCat[8] = {0}; int nChip = 0;
    RECT moreBtn;
    // 分类抽屉（打开时才有内容）
    RECT drawer, drawerHead, drawerClose;
    std::vector<RECT> dRect;
    std::vector<int>  dCat;
    std::vector<std::pair<RECT, std::wstring>> dSec;
    std::vector<RECT> row; int nRow = 0;    // 行数随面板高度变化，不再固定
    RECT toast;
};

static Layout L;

static void ComputeDrawer();

static const wchar_t* TAB_NAME[4] = {L"剪贴板", L"收藏", L"表情", L"符号"};
static const wchar_t* FILTER_NAME[4] = {L"全部", L"文本", L"图片", L"其他"};

static void ComputeLayout() {
    RebuildGlyphHits();       // 关键词/页面一变就重新做一次跨分类搜索（未变时是字符串比较）
    // 「最近」为空时它不可见，当前分类就不能停在它上面：否则整页空白、分类条也没有高亮项。
    if ((g_page == PG_EMOJI || g_page == PG_SYMBOL) && CurGlyphCat(g_page) <= 0 && RecentEmpty(g_page)) {
        if (g_page == PG_EMOJI) g_emojiCat = GlyphCatTotal(g_page) > 0 ? 1 : 0;
        else                    g_symCat   = GlyphCatTotal(g_page) > 0 ? 1 : 0;
        g_gridScroll = 0;
    }
    int W = g_panelW ? g_panelW : S(P_W);
    int H = g_panelH ? g_panelH : S(P_H);
    L.W = W; L.H = H;

    L.head = {0, 0, W, S(HEAD_H)};
    L.logo = {S(12), S((HEAD_H - 26) / 2), S(12) + S(26), S((HEAD_H - 26) / 2) + S(26)};

    // 右上三个按钮：临时固定 / 设置 / 关闭（与标题栏等高，关闭按钮贴右缘）
    // 先算按钮，页签才知道自己能用多宽
    int btnW = S(30);
    L.closeB = {W - btnW, 0, W, S(HEAD_H)};
    L.gear   = {L.closeB.left - btnW, 0, L.closeB.left, S(HEAD_H)};
    L.pinB   = {L.gear.left - btnW, 0, L.gear.left, S(HEAD_H)};

    // 页签：紧接 logo，右边界顶到固定按钮左侧。
    // 宽度下限压到 360 后四个页签的自然宽度放不下，此时按可用宽度均分，
    // 否则「剪贴板」会顶到图钉按钮底下，或者页签互相压住。
    int tabGap = S(4);
    int x0 = S(12) + S(26) + S(10);
    int avail = L.pinB.left - S(6) - x0;
    int nat[4], total = tabGap * 3;
    for (int i = 0; i < 4; ++i) {
        nat[i] = TextW(g_fTab, TAB_NAME[i]) + S(20);
        total += nat[i];
    }
    int x = x0;
    if (total <= avail) {
        for (int i = 0; i < 4; ++i) {
            L.tab[i] = {x, 0, x + nat[i], S(HEAD_H)};
            x += nat[i] + tabGap;
        }
    } else {
        int each = (avail - tabGap * 3) / 4;
        if (each < S(30)) each = S(30);
        for (int i = 0; i < 4; ++i) {
            L.tab[i] = {x, 0, x + each, S(HEAD_H)};
            x += each + tabGap;
        }
    }

    int sy = S(HEAD_H + SEARCH_TOP);
    L.searchBox = {S(SEARCH_PADX), sy, W - S(SEARCH_PADX), sy + S(SEARCH_H)};

    int fy = sy + S(SEARCH_H) + S(SEARCH_BOT);
    L.filters = {0, fy, W, fy + S(FILTER_H)};

    int by = L.filters.bottom;
    int bb = H - S(FOOT_H);
    L.body = {0, by, W, bb};
    L.foot = {0, bb, W, H};

    // 分类条：右侧 chip 组 + 「⋯」抽屉入口。
    // chip 数固定（最多 4 个），不随分类总数增长——分类再多也不重排分类条，
    // 全铺出来会把分类条撑爆，其余分类统一进抽屉。
    int cy = L.filters.top + (S(FILTER_H) - S(26)) / 2;
    int gap = S(8);
    int moreW = S(26);
    int right = W - S(16);
    L.nChip = 0;
    L.moreBtn = {0, 0, 0, 0};

    if (g_page == PG_EMOJI || g_page == PG_SYMBOL) {
        int page = g_page;
        int show[5], n = 0;
        if (!RecentEmpty(page)) show[n++] = 0;   // 「最近」为空时整条分类隐藏
        static const char* PINS_SYM[] = {"quick", "punct", "math"};
        static const char* PINS_EMO[] = {"smileys", "people", "nature"};
        const char* const* pins = (page == PG_SYMBOL) ? PINS_SYM : PINS_EMO;
        for (int k = 0; k < 3; ++k) {
            int i = GlyphCatIndexByKey(page, pins[k]);
            if (i > 0 && n < 4) show[n++] = i;
        }
        int cur = CurGlyphCat(page);
        bool has = false;
        for (int i = 0; i < n; ++i) if (show[i] == cur) has = true;
        if (!has) { if (n >= 4) --n; show[n++] = cur; }

        int cw[5];
        for (int i = 0; i < n; ++i) cw[i] = TextW(g_fChip, GlyphCatName(page, show[i])) + S(22);

        // 从右往左摆，放不下就丢掉最左边那个；当前分类不会是被丢的那个
        L.moreBtn = {right - moreW, cy, right, cy + S(26)};
        int x = L.moreBtn.left - gap;
        int labelRight = S(26) + S(84);
        RECT cr[5]; int cc[5]; int m = 0;
        for (int i = n - 1; i >= 0; --i) {
            if (x - cw[i] < labelRight) break;
            cr[m] = {x - cw[i], cy, x, cy + S(26)};
            cc[m] = show[i];
            ++m;
            x -= cw[i] + gap;
        }
        for (int i = 0; i < m; ++i) { L.chip[i] = cr[m - 1 - i]; L.chipCat[i] = cc[m - 1 - i]; }
        L.nChip = m;
    } else {
        int cw[4], total = 0;
        for (int i = 0; i < FL_COUNT; ++i) {
            cw[i] = TextW(g_fChip, FILTER_NAME[i]) + S(22);
            total += cw[i];
        }
        total += gap * (FL_COUNT - 1);
        int cx = right - total;
        for (int i = 0; i < FL_COUNT; ++i) {
            L.chip[i] = {cx, cy, cx + cw[i], cy + S(26)};
            L.chipCat[i] = i;
            cx += cw[i] + gap;
        }
        L.nChip = FL_COUNT;
    }
    if (g_drawerOpen) ComputeDrawer();

    // 行（高度可调后行数随面板走，不再有固定上限）
    L.row.clear();
    L.nRow = 0;
    if (g_page == PG_HISTORY || g_page == PG_FAV) {
        int avail = (L.body.bottom - S(12)) - (L.body.top + S(8));
        int fit = avail / S(ROW_H);
        if (fit < 1) fit = 1;
        int vis = (int)g_view.size() - g_top;
        if (vis > fit) vis = fit;
        if (vis < 0) vis = 0;
        L.nRow = vis;
        L.row.resize((size_t)vis);
        for (int i = 0; i < vis; ++i) {
            int ry = L.body.top + S(8) + i * S(ROW_H);
            L.row[(size_t)i] = {S(12), ry, W - S(12), ry + S(ROW_H)};
        }
    }

    // toast
    L.toast = {W - S(14) - S(150), S(56), W - S(14), S(56) + S(32)};
}

static bool PtIn(const RECT& r, POINT p) {
    return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
}

static void Redraw(const RECT* r = nullptr);
static void ComputeLayout();

// ---------------------------------------------------------------- 分类抽屉
// 参考页面的「全部分类」抽屉：从分类条下方展开、盖住列表区，按「域」分组，
// 几十个分类也不会失去结构；表情只有一个分组，省掉分组标题。
static std::wstring CatFirstGlyph(int page, int idx) {
    if (idx <= 0) {
        const std::vector<RecentGlyph>& r = page == PG_EMOJI ? g_recentEmo : g_recentSym;
        return r.empty() ? L"↺" : U8N(r[0].g.data(), (int)r[0].g.size());
    }
    const GCat& c = GlyphCat(page, idx);
    // 直接读 blob 的第一条，避免为了一个字符把整类条目都建索引
    if (c.count > 0 && c.raw && c.rawEnd > c.raw + 3 && c.raw[0] == 'I') {
        const char* s = c.raw + 2;
        const char* t = s;
        while (t < c.rawEnd && *t != '\t' && *t != '\n') ++t;
        return U8N(s, (int)(t - s));
    }
    return L"·";
}

static void ComputeDrawer() {
    L.dRect.clear(); L.dCat.clear(); L.dSec.clear();

    L.drawer = {0, L.filters.bottom, L.W, L.foot.top};
    L.drawerHead = {0, L.drawer.top, L.W, L.drawer.top + S(32)};
    int closeW = TextW(g_fChip, L"关闭") + S(10);
    L.drawerClose = {L.W - S(14) - closeW, L.drawerHead.top + S(6),
                     L.W - S(14), L.drawerHead.bottom - S(6)};

    const int page = (g_page == PG_EMOJI) ? PG_EMOJI : PG_SYMBOL;
    const int cols = 4, gapx = S(6), gapy = S(6), padx = S(12);
    const int cw = (L.W - padx * 2 - gapx * (cols - 1)) / cols;
    const int ch = S(30);
    const int startY = L.drawerHead.bottom + S(10);

    struct Sec { std::wstring name; std::vector<int> cats; };
    std::vector<Sec> secs;

    // 两页同构：只有一层分类（符号的大类＝数据里的「域」，已在生成期合并），
    // 因此不再有域分组标题，抽屉就是一片按钮。
    const std::vector<GCat>& cats = (page == PG_SYMBOL) ? g_symCats : g_emoCats;
    Sec s; 
    if (!RecentEmpty(page)) s.cats.push_back(0);   // 「最近」为空时抽屉里也没有它
    for (size_t i = 0; i < cats.size(); ++i) s.cats.push_back((int)i + 1);
    secs.push_back(std::move(s));

    int y = startY - g_drawerScroll;
    for (const auto& s : secs) {
        if (!s.name.empty()) {
            L.dSec.push_back({{padx, y, L.W - padx, y + S(14)}, s.name});
            y += S(14) + S(6);
        }
        for (size_t k = 0; k < s.cats.size(); ++k) {
            int x = padx + (int)(k % cols) * (cw + gapx);
            int yy = y + (int)(k / cols) * (ch + gapy);
            L.dRect.push_back({x, yy, x + cw, yy + ch});
            L.dCat.push_back(s.cats[k]);
        }
        int rows = ((int)s.cats.size() + cols - 1) / cols;
        y += rows * (ch + gapy) + S(11);
    }
    g_drawerContentH = (y + g_drawerScroll) - startY;
}

static int DrawerMaxScroll() {
    int viewH = L.drawer.bottom - (L.drawerHead.bottom + S(10));
    int m = g_drawerContentH - viewH;
    return m > 0 ? m : 0;
}

static void ClampDrawerScroll() {
    int m = DrawerMaxScroll();
    if (g_drawerScroll > m) g_drawerScroll = m;
    if (g_drawerScroll < 0) g_drawerScroll = 0;
}

static int DrawerHit(POINT p) {
    for (size_t i = 0; i < L.dRect.size(); ++i)
        if (PtIn(L.dRect[i], p)) return (int)i;
    return -1;
}

static void ToggleDrawer(bool open) {
    g_drawerOpen = open;
    if (open) { g_drawerScroll = 0; g_drawerHover = -1; }
    ComputeLayout();
    Redraw();
}



// 字形格子：表情页与符号页共用同一套网格（9 列 × 44px），
// 两页都只画字形本身，不再有卡片、名称这类附加信息
static int GlyphCols() { return 9; }
static RECT GlyphCellRect(int i) {
    int cols = GlyphCols();
    int padL = S(12), padR = S(12), gap = S(5);
    int availW = L.W - padL - padR;
    int cw = (availW - gap * (cols - 1)) / cols;
    int ch = S(44);
    int row = i / cols, col = i % cols;
    int x = padL + col * (cw + gap);
    int y = L.body.top + S(8) + row * (ch + gap) - g_gridScroll;
    return RECT{x, y, x + cw, y + ch};
}

// 菜单：跟随右键位置弹出，越界时翻到光标另一侧、再不行就贴边
// 三项：置顶 / 另存为 /（分隔线）删除
enum { MI_PIN = 0, MI_SAVE, MI_DELETE, MI_COUNT };
static const int MENU_SEP_BEFORE = MI_DELETE;   // 分隔线画在该项之前
static const int MENU_SEP_H      = 9;

static RECT MenuRect() {
    int w = S(176);
    int itemH = S(34);
    int h = S(6) * 2 + itemH * MI_COUNT + S(MENU_SEP_H);

    int x = g_menuPt.x + S(2);
    int y = g_menuPt.y + S(2);
    if (x + w > L.W - S(8)) x = g_menuPt.x - S(2) - w;
    if (x < S(8)) x = S(8);
    if (x + w > L.W - S(8)) x = L.W - S(8) - w;

    int bodyBot = L.body.bottom - S(4);
    if (y + h > bodyBot) y = g_menuPt.y - S(2) - h;
    if (y < L.body.top) y = L.body.top;
    if (y + h > bodyBot) y = bodyBot - h;
    if (y < L.body.top) y = L.body.top;
    return RECT{x, y, x + w, y + h};
}

static RECT MenuItemRect(int idx) {
    RECT m = MenuRect();
    int itemH = S(34);
    int yy = m.top + S(6) + idx * itemH;
    if (idx >= MENU_SEP_BEFORE) yy += S(MENU_SEP_H);
    return RECT{m.left + S(6), yy, m.right - S(6), yy + itemH};
}

// 命中测试：返回菜单项下标，未命中返回 -1
static int MenuHit(POINT p) {
    for (int i = 0; i < MI_COUNT; ++i)
        if (PtIn(MenuItemRect(i), p)) return i;
    return -1;
}

// 删除确认弹层
static RECT ModalRect() {
    int w = S(300), h = S(146);
    int x = (L.W - w) / 2, y = (L.H - h) / 2;
    return RECT{x, y, x + w, y + h};
}

// ================================================================== 绘制：组件
static RECT CenterIcon(const RECT& b, int s) {
    int cx = (b.left + b.right) / 2, cy = (b.top + b.bottom) / 2;
    int x = cx - s / 2, y = cy - s / 2;
    return RECT{x, y, x + s, y + s};
}

static void PaintHead(HDC dc) {
    int headBot = L.head.bottom - 1;
    RECT hb{0, 0, L.W, headBot};
    FillBox(dc, hb, C_SUBTLE);

    // 品牌标
    FillRound(dc, L.logo, S(7), C_VOLT, C_VOLT, 0);
    RECT li = L.logo; InflateRect(&li, -S(5), -S(5));
    DrawIcon(dc, IC_CLIP, li, C_SURFACE, 2.0f);

    // 页签
    for (int i = 0; i < 4; ++i) {
        RECT t = L.tab[i];
        bool active = (i == g_page);
        RECT rr = t; rr.bottom = headBot;
        if (active) {
            FillBox(dc, rr, C_SURFACE);
            // 上边与左右边框
            HLine(dc, rr.left, rr.right, rr.top, C_LINE);
            RECT vl{rr.left, rr.top, rr.left + 1, rr.bottom};
            RECT vr{rr.right - 1, rr.top, rr.right, rr.bottom};
            FillBox(dc, vl, C_LINE); FillBox(dc, vr, C_LINE);
            // 顶部橙色条（覆盖边框线，与 CSS 的 ::after 一致）
            RECT bar{rr.left + S(12), rr.top, rr.right - S(12), rr.top + S(2)};
            FillBox(dc, bar, C_VOLT);
        }
        TextOutF(dc, g_fTab, active ? C_INK : C_MUTED, TAB_NAME[i], rr,
                 DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    }

    // 右上三个按钮的高亮与图标在 PaintHeadButtons 里画（放在外框线之后，
    // 这样悬停/固定态的高亮块能贴到窗口边缘，不会被 1px 外框线隔出一条浅边）

    HLine(dc, 0, L.W, headBot, C_LINE);
    // 活动页签底部与内容区连通（对应 CSS 的 border-bottom-color: surface）
    {
        RECT t = L.tab[g_page];
        RECT gap{t.left + 1, headBot, t.right - 1, headBot + 1};
        FillBox(dc, gap, C_SURFACE);
    }
}

// 右上角为圆角、其余为直角的矩形填充。贴窗口右上角的按钮高亮块必须这样做：
// 窗口靠 SetWindowRgn 裁圆角，若高亮块是直角矩形，右上角会被硬裁出一个缺口，
// 缺口露出后面的桌面，看起来就是「按钮右上方一圈白」。
static void FillTRRound(HDC dc, const RECT& r, float rad, COLORREF c) {
    (void)dc;
    if (!g_rt || !g_painting) return;
    if (rad < 0.1f) { FillBox(dc, r, c); return; }
    float l = (float)r.left, t = (float)r.top, rr = (float)r.right, b = (float)r.bottom;
    if (rad > rr - l) rad = rr - l;
    if (rad > b - t) rad = b - t;
    const float k = 0.5522847498f * rad;
    PathB p;
    if (!p.ok()) { FillBox(dc, r, c); return; }
    p.Move(l, t);
    p.Line(rr - rad, t);
    p.sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(rr - rad + k, t),
                                          D2D1::Point2F(rr, t + rad - k),
                                          D2D1::Point2F(rr, t + rad)));
    p.Line(rr, b);
    p.Line(l, b);
    p.EndClosed();
    p.Fill(c);
}

// 标题栏右上三个按钮：临时固定 / 设置 / 关闭
// 放在外框线之后绘制——高亮块要盖住那 1px 外框线，否则悬停时按钮边缘会多出一道浅边
static void PaintHeadButtons(HDC dc) {
    bool hp = (g_hoverChip == 902), hg = (g_hoverChip == 900), hc = (g_hoverChip == 901);
    float rad = (float)S(RADIUS) / 2.0f;
    if (g_pinned)     FillTRRound(dc, L.pinB, 0, C_VOLTSOFT);
    else if (hp)      FillTRRound(dc, L.pinB, 0, C_SURFACE);
    if (hg)           FillTRRound(dc, L.gear, 0, C_SURFACE);
    if (hc)           FillTRRound(dc, L.closeB, rad, C_DANGER);

    RECT pi = CenterIcon(L.pinB, S(17));
    RECT gi = CenterIcon(L.gear, S(17));
    RECT ci = CenterIcon(L.closeB, S(17));
    DrawIcon(dc, IC_PIN, pi, g_pinned ? C_VOLT : (hp ? C_INK : C_MUTED), g_pinned ? 2.6f : 1.9f);
    DrawIcon(dc, IC_GEAR, gi, hg ? C_INK : C_MUTED, 1.9f);
    DrawIcon(dc, IC_X, ci, hc ? C_SURFACE : C_MUTED, 2.0f);
}

static void PaintSearch(HDC dc) {
    RECT b = L.searchBox;
    FillRound(dc, b, S(8), C_CANVAS, C_LINE);
    {
        // focus ring：对应 CSS 的 box-shadow 0 0 0 2px rgba(255,107,53,.14)
        RECT ring = b; InflateRect(&ring, S(2), S(2));
        FillRound(dc, ring, S(10), C_RINGSOFT, C_RINGSOFT, 0);
        FillRound(dc, b, S(8), C_CANVAS, C_VOLT);
    }

    RECT si{b.left + S(11), b.top + (S(SEARCH_H) - S(17)) / 2,
            b.left + S(11) + S(17), b.top + (S(SEARCH_H) - S(17)) / 2 + S(17)};
    DrawIcon(dc, IC_SEARCH, si, C_MUTED, 2.0f);

    int tx = si.right + S(9);
    RECT tr{tx, b.top, b.right - S(11), b.bottom};
    // 组合串（IME 还没上屏的拼音）跟在查询串后面一起画，系统那张组合窗口已屏蔽
    std::wstring shown = g_query + g_comp;
    if (shown.empty()) {
        TextOutF(dc, g_fSearch, C_MUTED, L"搜索剪贴板内容", tr,
                 DT_SINGLELINE | DT_VCENTER | DT_LEFT);
    } else {
        TextOutF(dc, g_fSearch, C_INK, shown, tr, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        if (!g_comp.empty()) {
            int x1 = tx + TextW(g_fSearch, g_query);
            int x2 = x1 + TextW(g_fSearch, g_comp);
            int uy = b.bottom - S(7);
            HLine(dc, x1, (std::min)(x2, (int)tr.right), uy, C_VOLT);
        }
    }
    if (g_caretOn && IsWindowVisible(g_panel)) {
        int cx = tx + TextW(g_fSearch, shown) + S(1);
        if (cx > tr.right - S(2)) cx = tr.right - S(2);
        RECT cr{cx, b.top + S(11), cx + (std::max)(1, S(1)), b.bottom - S(11)};
        FillBox(dc, cr, C_INK);
    }
}

// IME 候选窗默认会飘到屏幕角落（窗口是自绘、没有系统 caret），
// 这里把它钉到搜索框里光标的正下方。
static void PositionImeCandidate(HWND hwnd) {
    HIMC himc = ImmGetContext(hwnd);
    if (!himc) return;
    RECT b = L.searchBox;
    int tx = b.left + S(11) + S(17) + S(9);
    int x = tx + TextW(g_fSearch, g_query + g_comp) + S(1);
    if (x > b.right - S(14)) x = b.right - S(14);
    POINT p{x, b.bottom + S(2)};
    ClientToScreen(hwnd, &p);
    CANDIDATEFORM cf{};
    cf.dwIndex = 0;
    cf.dwStyle = CFS_CANDIDATEPOS;
    cf.ptCurrentPos = p;
    ImmSetCandidateWindow(himc, &cf);
    ImmReleaseContext(hwnd, himc);
}

static void PaintFilters(HDC dc) {
    RECT f = L.filters;
    FillBox(dc, f, C_SURFACE);

    // 左侧「分组 · 数量」
    int cy = f.top + S(FILTER_H) / 2;
    RECT mk{S(16), cy - S(6), S(16) + S(2), cy + S(6)};
    FillRound(dc, mk, S(1), C_VOLT, C_VOLT, 0);

    const wchar_t* pageName = (g_page == PG_FAV) ? L"收藏"
                            : (g_page == PG_EMOJI) ? L"表情"
                            : (g_page == PG_SYMBOL) ? L"符号" : L"剪贴板";
    const wchar_t* grp = ((g_page == PG_EMOJI || g_page == PG_SYMBOL) && !g_query.empty())
                       ? L"搜索" : pageName;
    std::wstring label = std::wstring(grp) + L" · " + std::to_wstring(PageCount());
    RECT lr{S(26), f.top, S(26) + S(84), f.top + S(FILTER_H)};
    TextOutF(dc, g_fChip, C_MUTED, label, lr, DT_SINGLELINE | DT_VCENTER | DT_LEFT);

    // chips：下标 i 对应的分类取自 L.chipCat[i]（分类数远多于 chip 数，两者不再一一对应）
    int active = (g_page == PG_EMOJI || g_page == PG_SYMBOL) ? CurGlyphCat(g_page) : g_filter;
    for (int i = 0; i < L.nChip; ++i) {
        int cat = L.chipCat[i];
        bool on = (cat == active);
        RECT c = L.chip[i];
        COLORREF bg = on ? C_VOLTSOFT : (g_hoverChip == i ? C_SUBTLE : C_SURFACE);
        COLORREF bd = on ? C_VOLT : C_LINE;
        FillRound(dc, c, S(13), bg, bd);
        std::wstring nm = (g_page == PG_EMOJI || g_page == PG_SYMBOL)
                        ? GlyphCatName(g_page, cat) : FILTER_NAME[cat];
        TextOutF(dc, g_fChip, on ? C_INK : C_MUTED, nm, c, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    }

    // 「⋯」分类抽屉入口（角标显示分类总数）
    if (L.moreBtn.right > L.moreBtn.left) {
        RECT mb = L.moreBtn;
        bool on = g_drawerOpen;
        FillRound(dc, mb, S(13), on ? C_VOLTSOFT : (g_hoverChip == 903 ? C_SUBTLE : C_SURFACE),
                  on ? C_VOLT : C_LINE);
        TextOutF(dc, g_fChip, C_INK, L"⋯", mb, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        int badge = GlyphCatTotal(g_page) + (RecentEmpty(g_page) ? 0 : 1);   // +1 = 「最近」
        std::wstring bs = std::to_wstring(badge);
        int bw = (std::max)(S(15), TextW(g_fToast, bs) + S(5));
        // 角标压在按钮右上角外侧：不遮住「⋯」本身，也不越过窗口边（右缘还有 16px 余量）
        RECT br{mb.right + S(4) - bw, mb.top - S(7), mb.right + S(4), mb.top - S(7) + S(13)};
        FillRound(dc, br, S(7), C_VOLT, C_SURFACE, 1);
        TextOutF(dc, g_fKbd, C_SURFACE, bs, br, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    }

    HLine(dc, 0, L.W, f.bottom - 1, C_LINE);
}

static void PaintRow(HDC dc, int i) {
    int vi = g_top + i;
    if (vi >= (int)g_view.size()) return;
    ClipItem& it = g_items[g_view[vi]];
    RECT r = L.row[i];
    bool sel = (vi == g_sel);
    bool hov = (vi == g_hoverRow);

    if (sel) {
        FillRound(dc, r, S(8), C_VOLTSOFT, C_VOLT);
    } else if (hov) {
        FillRound(dc, r, S(8), C_CANVAS, C_CANVAS, 0);
    }

    // 类型徽标 / 图片缩略图
    RECT ic{r.left + S(10), r.top + (S(ROW_H) - S(28)) / 2,
            r.left + S(10) + S(28), r.top + (S(ROW_H) - S(28)) / 2 + S(28)};
    if (!DrawThumb(dc, it, ic)) {
        FillRound(dc, ic, S(6), C_SUBTLE, C_SUBTLE, 0);
        RECT ii = ic; InflateRect(&ii, -S(7), -S(7));
        DrawIcon(dc, ItemIcon(it), ii, C_MUTED, 2.0f);
    }

    // 主文本两行：标题 / 副行（左「类型 · 来源」，右「大小 - 时间」，两端对齐）
    int lx = ic.right + S(11);
    int midY = (r.top + r.bottom) / 2;
    int rowRight = r.right - S(10);
    RECT titleR{lx, midY - S(18), rowRight, midY - S(1)};
    RECT metaR{lx, midY + S(1), rowRight, midY + S(16)};

    // 副行右侧：大小 - 相对时间（右对齐到行右缘）
    std::wstring rt = SizeText(it.bytes) + L" - " + RelTime(it.st);
    int tw = TextW(g_fMeta, rt);
    RECT rtR{rowRight - tw, metaR.top, rowRight, metaR.bottom};
    TextOutF(dc, g_fMeta, C_MUTED, rt, rtR, DT_SINGLELINE | DT_VCENTER | DT_RIGHT);

    int tx = lx;
    if (it.pinned) {
        int s = S(13);
        RECT pr{lx, midY - S(18) + (S(19) - s) / 2, lx + s, midY - S(18) + (S(19) - s) / 2 + s};
        DrawIcon(dc, IC_PIN, pr, C_VOLT, 2.4f);
        tx += s + S(5);
    }
    RECT tt = titleR; tt.left = tx;
    HFONT tf = it.isCode ? g_fMono : g_fTitle;
    TextOutF(dc, tf, C_INK, it.title, tt, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_LEFT);

    std::wstring meta = it.tag + L" · 来源：" + it.source;
    RECT mR = metaR; mR.right = rtR.left - S(8);
    // 面板压到很窄时右侧「大小 - 时间」会吃掉大半行宽，左边界必须夹住，
    // 否则 right < left 交给 DirectWrite 会画出乱码般的裁切结果
    if (mR.right < lx + S(20)) mR.right = lx + S(20);
    TextOutF(dc, g_fMeta, C_MUTED, meta, mR, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_LEFT);
}

static void PaintEmpty(HDC dc) {
    int cx = L.W / 2;
    int top = L.body.top + (L.body.bottom - L.body.top) / 2 - S(90);
    RECT icx{cx - S(24), top, cx + S(24), top + S(48)};
    DrawIcon(dc, g_page == PG_FAV ? IC_STAR : IC_CLIP, icx, C_MUTED, 1.6f);

    const wchar_t* h3;
    const wchar_t* p;
    if (g_page == PG_FAV) {
        bool cleared = !g_query.empty() || g_filter != FL_ALL;
        h3 = cleared ? L"没有匹配的收藏" : L"还没有收藏";
        p  = cleared ? L"换个关键词，或调整筛选条件。"
                     : L"按 Ctrl+D 收藏当前条目，常用内容就会收进这里。";
    } else {
        h3 = L"没有匹配的剪贴板内容";
        p  = L"换个关键词，或调整筛选条件。";
    }
    RECT t1{0, top + S(62), L.W, top + S(62) + S(22)};
    TextOutF(dc, g_fBig, C_INK, h3, t1, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    RECT t2{0, t1.bottom + S(6), L.W, t1.bottom + S(6) + S(20)};
    TextOutF(dc, g_fMeta, C_MUTED, p, t2, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
}

// 符号字形的字体兜底：Segoe UI Symbol 覆盖不到数学字母、古文字等区块，
// 所以在候选字体里挑第一个能画的（可画性判定见 GlyphDrawable）。
static IDWriteFont* MakeProbe(const wchar_t* family) {
    if (!g_dw) return nullptr;
    IDWriteFontCollection* coll = nullptr;
    if (FAILED(g_dw->GetSystemFontCollection(&coll, FALSE)) || !coll) return nullptr;
    UINT32 idx = 0; BOOL exists = FALSE;
    IDWriteFont* out = nullptr;
    if (SUCCEEDED(coll->FindFamilyName(family, &idx, &exists)) && exists) {
        IDWriteFontFamily* fam = nullptr;
        if (SUCCEEDED(coll->GetFontFamily(idx, &fam)) && fam) {
            if (FAILED(fam->GetFont(0, &out))) out = nullptr;
            fam->Release();
        }
    }
    coll->Release();
    return out;
}

static HFONT PickSymFont(const std::wstring& g) {
    auto it = g_symFontSlot.find(g);
    int slot = (it != g_symFontSlot.end()) ? it->second : -1;
    if (slot < 0) {
        slot = 0;
        if (!GlyphFontCovers(g_probe[0], g)) {
            for (int i = 1; i < 4; ++i) {
                if (GlyphFontCovers(g_probe[i], g)) { slot = i; break; }
            }
        }
        g_symFontSlot[g] = slot;
    }
    return (slot == 0 || !g_fSymFallback[slot - 1]) ? g_fSym : g_fSymFallback[slot - 1];
}

static void PaintGlyphEmpty(HDC dc, const wchar_t* title, const wchar_t* sub) {
    int cy = (L.body.top + L.body.bottom) / 2;
    RECT t{0, cy - S(26), L.W, cy - S(6)};
    TextOutF(dc, g_fBig, C_INK, title, t, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    RECT s{0, cy - S(4), L.W, cy + S(16)};
    TextOutF(dc, g_fMeta, C_MUTED, sub, s, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
}

// 当前可见的 emoji 格子：滚动位置定位到行首，滚出视口就停
static void CollectEmojiCells(std::vector<std::pair<RECT, int>>& out) {
    out.clear();
    const std::vector<GItem>& list = GlyphItems(PG_EMOJI);
    if (list.empty()) return;
    const int cols = GlyphCols();
    const int step = S(44) + S(5);
    int firstRow = (g_gridScroll > S(8)) ? (g_gridScroll - S(8)) / step : 0;
    int start = firstRow * cols;
    if (start >= (int)list.size()) start = 0;
    out.reserve((size_t)((L.body.bottom - L.body.top) / step + 2) * (size_t)cols);
    for (int i = start; i < (int)list.size(); ++i) {
        RECT c = GlyphCellRect(i);
        if (c.top > L.body.bottom) break;
        if (c.bottom < L.body.top) continue;
        out.push_back({c, i});
    }
}

static void PaintEmoji(HDC dc) {
    const std::vector<GItem>& list = GlyphItems(PG_EMOJI);
    if (list.empty()) {
        bool recent = g_query.empty() && CurGlyphCat(PG_EMOJI) <= 0;
        PaintGlyphEmpty(dc, recent ? L"还没有最近" : L"没有匹配的结果",
                        recent ? L"点任意表情后，它会出现在这里" : L"换个关键词试试");
        return;
    }

    std::vector<std::pair<RECT, int>> cells;
    CollectEmojiCells(cells);
    for (auto& c : cells)
        if (c.second == g_hoverCell) FillRound(dc, c.first, S(7), C_CANVAS, C_CANVAS, 0);

    const int sz = S(44);
    if (g_rt && g_painting && g_emojiFmt) {
        for (auto& c : cells) {
            const GItem& it = list[(size_t)c.second];
            auto f = g_emoSlot.find(std::string(it.g, (size_t)it.gl));
            if (f != g_emoSlot.end() && g_emoAtlas &&
                (size_t)f->second < g_emoReady.size() && g_emoReady[(size_t)f->second]) {
                float x = (float)(c.first.left + c.first.right - sz) / 2.0f;
                float y = (float)c.first.top;
                D2D1_RECT_F sr = EmojiSlotRect(f->second);
                g_rt->DrawBitmap(g_emoAtlas, D2D1::RectF(x, y, x + (float)sz, y + (float)sz),
                                 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, &sr);
            } else {
                // 还没就绪：只给一层极淡的剪影。以前这里画的是实心黑字形，
                // 于是「黑色->彩色」的跳变非常刺眼；淡影过渡基本看不出来。
                Brush(C_MUTED, 0.22f);
                std::wstring w = U8N(it.g, it.gl);
                g_rt->DrawTextW(w.c_str(), (UINT32)w.size(), g_emojiFmt,
                                D2D1::RectF((float)c.first.left, (float)c.first.top,
                                            (float)c.first.right, (float)c.first.bottom),
                                g_brush, D2D1_DRAW_TEXT_OPTIONS_NONE);
            }
        }
        return;
    }
    for (auto& c : cells)
        TextOutF(dc, g_fEmoji, C_INK, U8N(list[(size_t)c.second].g, list[(size_t)c.second].gl),
                 c.first, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
}

static void PaintSymbols(HDC dc) {
    const std::vector<GItem>& list = GlyphItems(PG_SYMBOL);
    if (list.empty()) {
        bool recent = g_query.empty() && CurGlyphCat(PG_SYMBOL) <= 0;
        PaintGlyphEmpty(dc, recent ? L"还没有最近" : L"没有匹配的结果",
                        recent ? L"点任意符号后，它会出现在这里" : L"换个关键词或分类试试");
        return;
    }

    // 与表情页同一套网格：只画字形本身，不再有卡片底、边框和符号名称
    const int cols = GlyphCols();
    const int step = S(44) + S(5);
    int firstRow = (g_gridScroll > S(8)) ? (g_gridScroll - S(8)) / step : 0;
    int start = firstRow * cols;

    for (int i = start; i < (int)list.size(); ++i) {
        RECT c = GlyphCellRect(i);
        if (c.top > L.body.bottom) break;
        if (c.bottom < L.body.top) continue;
        if (i == g_hoverCell) FillRound(dc, c, S(7), C_CANVAS, C_CANVAS, 0);
        std::wstring g = U8N(list[i].g, list[i].gl);
        TextOutF(dc, PickSymFont(g), C_INK, g, c, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    }
}

// 细滚动条（对应 CSS 的 8px thumb）
static int GridContentH();

static void PaintScrollbar(HDC dc) {
    int contentH = 0, viewH = L.body.bottom - L.body.top, offset = 0;
    if (g_page == PG_HISTORY || g_page == PG_FAV) {
        contentH = S(8) + (int)g_view.size() * S(ROW_H) + S(12);
        offset = g_top * S(ROW_H);
    } else {
        contentH = GridContentH();
        offset = g_gridScroll;
    }
    if (contentH <= viewH) return;

    int trackTop = L.body.top + S(4);
    int trackBot = L.body.bottom - S(4);
    int trackH = trackBot - trackTop;
    int thumbH = (std::max)(S(28), (int)((long long)trackH * viewH / contentH));
    int maxOffset = contentH - viewH;
    if (maxOffset < 1) maxOffset = 1;
    int thumbTop = trackTop + (int)((long long)(trackH - thumbH) * offset / maxOffset);
    if (thumbTop < trackTop) thumbTop = trackTop;
    if (thumbTop + thumbH > trackBot) thumbTop = trackBot - thumbH;

    RECT tb{L.W - S(8), thumbTop, L.W - S(3), thumbTop + thumbH};
    FillRound(dc, tb, S(2), RGB(196, 190, 181), RGB(196, 190, 181), 0);
}

// ---------------------------------------------------------------- 分类抽屉绘制
static void PaintDrawer(HDC dc) {
    FillBox(dc, L.drawer, C_SURFACE);
    HLine(dc, 0, L.W, L.drawer.top, C_LINE);
    FillBox(dc, L.drawerHead, C_CANVAS);
    HLine(dc, 0, L.W, L.drawerHead.bottom - 1, C_LINE);

    int total = GlyphCatTotal(g_page) + (RecentEmpty(g_page) ? 0 : 1);   // +1 = 「最近」
    std::wstring title = L"全部分类 · " + std::to_wstring(total) + L" 类";
    RECT tr{S(16), L.drawerHead.top, L.W - S(70), L.drawerHead.bottom};
    TextOutF(dc, g_fChip, C_MUTED, title, tr, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
    TextOutF(dc, g_fChip, C_MUTED, L"关闭", L.drawerClose, DT_SINGLELINE | DT_VCENTER | DT_RIGHT);

    if (g_rt && g_painting)
        g_rt->PushAxisAlignedClip(
            D2D1::RectF(0.0f, (float)L.drawerHead.bottom, (float)L.W, (float)L.drawer.bottom),
            D2D1_ANTIALIAS_MODE_ALIASED);

    for (const auto& s : L.dSec) {
        if (s.first.top > L.drawer.bottom) break;
        if (s.first.bottom < L.drawerHead.bottom) continue;
        TextOutF(dc, g_fSymName, C_MUTED, s.second, s.first, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
    }

    int active = CurGlyphCat(g_page);
    for (size_t i = 0; i < L.dRect.size(); ++i) {
        RECT r = L.dRect[i];
        if (r.top > L.drawer.bottom) break;
        if (r.bottom < L.drawerHead.bottom) continue;
        int cat = L.dCat[i];
        bool on = (cat == active);
        bool hov = ((int)i == g_drawerHover);
        FillRound(dc, r, S(7), on ? C_VOLTSOFT : (hov ? C_CANVAS : C_SURFACE),
                  on ? C_VOLT : (hov ? C_MUTED : C_LINE));
        RECT gr{r.left + S(6), r.top, r.left + S(22), r.bottom};
        TextOutF(dc, g_fSymName, on ? C_INK : C_MUTED, CatFirstGlyph(g_page, cat), gr,
                 DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        RECT nr{r.left + S(24), r.top, r.right - S(6), r.bottom};
        TextOutF(dc, g_fChip, C_INK, GlyphCatName(g_page, cat), nr,
                 DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
    }

    if (g_rt && g_painting) g_rt->PopAxisAlignedClip();

    // 抽屉自己的滚动条
    int viewH = L.drawer.bottom - (L.drawerHead.bottom + S(10));
    if (g_drawerContentH > viewH) {
        int trackTop = L.drawerHead.bottom + S(6), trackBot = L.drawer.bottom - S(6);
        int trackH = trackBot - trackTop;
        int thumbH = (std::max)(S(28), (int)((long long)trackH * viewH / g_drawerContentH));
        int maxS = DrawerMaxScroll();
        int thumbTop = trackTop + (maxS > 0 ? (int)((long long)(trackH - thumbH) * g_drawerScroll / maxS) : 0);
        if (thumbTop + thumbH > trackBot) thumbTop = trackBot - thumbH;
        RECT tb{L.W - S(8), thumbTop, L.W - S(3), thumbTop + thumbH};
        FillRound(dc, tb, S(2), RGB(196, 190, 181), RGB(196, 190, 181), 0);
    }
}

static void PaintFoot(HDC dc) {
    FillBox(dc, L.foot, C_CANVAS);
    HLine(dc, 0, L.W, L.foot.top, C_LINE);

    struct H { const wchar_t* key; const wchar_t* label; };
    H hints[] = {{L"↑ ↓", L"选择"}, {L"Enter", L"粘贴"}, {L"P", L"置顶"}, {L"Esc", L"关闭"}};

    // 右侧计数先算出来，提示串才知道自己能用多宽
    std::wstring cnt;
    if (g_page == PG_EMOJI) {
        cnt = std::to_wstring(PageCount()) + L" 个表情";
    } else if (g_page == PG_SYMBOL) {
        cnt = std::to_wstring(PageCount()) + L" 个符号";
    } else {
        int pinned = 0, fav = 0;
        for (int i : g_view) { if (g_items[i].pinned) ++pinned; if (g_items[i].fav) ++fav; }
        cnt = std::to_wstring(g_view.size()) + L" 条";
        if (g_page == PG_HISTORY) {
            cnt += L" · " + std::to_wstring(pinned) + L" 置顶";
            if (fav) cnt += L" · " + std::to_wstring(fav) + L" 收藏";
        }
    }
    int cntW = TextW(g_fMeta, cnt);
    int limit = L.W - S(16) - cntW - S(12);   // 提示串右边界：留 12px 不和计数挨上

    int x = S(16);
    int cy = L.foot.top + S(FOOT_H) / 2;
    for (auto& h : hints) {
        int kw = TextW(g_fKbd, h.key) + S(10);
        int lw = TextW(g_fMeta, h.label);
        // 窄面板装不下全部提示，放不下的直接不画（右侧计数优先保留）
        if (x + kw + S(5) + lw + S(2) > limit) break;
        RECT kr{x, cy - S(9), x + kw, cy + S(9)};
        FillRound(dc, kr, S(4), C_SURFACE, C_LINE);
        TextOutF(dc, g_fKbd, C_INK, h.key, kr, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        int lx = kr.right + S(5);
        RECT lr{lx, L.foot.top, lx + lw + S(2), L.foot.bottom};
        TextOutF(dc, g_fMeta, C_MUTED, h.label, lr, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        x = lr.right + S(14);
    }

    RECT cr{S(16), L.foot.top, L.W - S(16), L.foot.bottom};
    TextOutF(dc, g_fMeta, C_MUTED, cnt, cr, DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
}

static void PaintToast(HDC dc) {
    if (!g_toastOn) return;
    int w = TextW(g_fToast, g_toast) + S(24) + S(22);
    RECT t{L.W - S(14) - w, S(56), L.W - S(14), S(56) + S(32)};
    FillRound(dc, t, S(8), C_INK, C_INK, 0);
    RECT ti{t.left + S(10), t.top + (S(32) - S(15)) / 2,
            t.left + S(10) + S(15), t.top + (S(32) - S(15)) / 2 + S(15)};
    DrawIcon(dc, g_toastInfo ? IC_MORE : IC_CHECK, ti, g_toastInfo ? C_VOLT : C_SUCCESS, 2.2f);
    RECT tt{ti.right + S(7), t.top, t.right - S(10), t.bottom};
    TextOutF(dc, g_fToast, C_SURFACE, g_toast, tt, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
}

// 菜单：置顶（文案随状态变）/ 另存为 /（分隔线）删除
static const wchar_t* MenuLabel(int idx, const ClipItem* it) {
    switch (idx) {
    case MI_PIN:  return (it && it->pinned) ? L"取消置顶" : L"置顶";
    case MI_SAVE: return L"另存为…";
    default:      return L"删除";
    }
}

static void PaintMenu(HDC dc) {
    if (!g_menuOpen) return;
    RECT m = MenuRect();
    FillRound(dc, m, S(8), C_SURFACE, C_LINE);

    // 分隔线落在「删除」上方的留白里
    int sepY = MenuItemRect(MENU_SEP_BEFORE).top - S(MENU_SEP_H / 2);
    HLine(dc, m.left + S(10), m.right - S(10), sepY, C_LINE);

    const ClipItem* it = (g_menuItem >= 0 && g_menuItem < (int)g_items.size())
                         ? &g_items[g_menuItem] : nullptr;
    for (int i = 0; i < MI_COUNT; ++i) {
        RECT ir = MenuItemRect(i);
        if (g_menuHover == i) FillRound(dc, ir, S(6), C_CANVAS, C_CANVAS, 0);
        RECT ii{ir.left + S(9), ir.top + (S(34) - S(15)) / 2,
                ir.left + S(9) + S(15), ir.top + (S(34) - S(15)) / 2 + S(15)};
        COLORREF tc = (i == MI_DELETE) ? C_DANGER : C_INK;
        IconId ic = (i == MI_PIN) ? IC_PIN : (i == MI_SAVE ? IC_SAVE : IC_TRASH);
        DrawIcon(dc, ic, ii, tc, 1.9f);
        RECT tr{ii.right + S(9), ir.top, ir.right - S(8), ir.bottom};
        TextOutF(dc, g_fChip, tc, MenuLabel(i, it), tr, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
    }
}

static void PaintModal(HDC dc) {
    if (!g_modalOpen) return;
    // 遮罩：rgba(32,37,43,.28) —— D2D 直接做 source-over，比手工拼预乘 DIB 更准
    if (g_rt && g_painting) {
        Brush(C_INK, 0.28f);
        g_rt->FillRectangle(D2D1::RectF(0, 0, (float)L.W, (float)L.H), g_brush);
    }

    RECT m = ModalRect();
    FillRound(dc, m, S(8), C_SURFACE, C_LINE);

    RECT h3{m.left + S(20), m.top + S(20), m.right - S(20), m.top + S(20) + S(22)};
    TextOutF(dc, g_fBig, C_INK, L"删除这条剪贴板项目？", h3,
             DT_SINGLELINE | DT_VCENTER | DT_LEFT);
    RECT p{m.left + S(20), h3.bottom + S(6), m.right - S(20), h3.bottom + S(6) + S(20)};
    TextOutF(dc, g_fMeta, C_MUTED, L"删除后无法恢复。", p, DT_SINGLELINE | DT_VCENTER | DT_LEFT);

    int bw = (m.right - m.left - S(40) - S(8)) / 2;
    int by = m.bottom - S(20) - S(38);
    RECT ok{m.left + S(20), by, m.left + S(20) + bw, by + S(38)};
    RECT cc{ok.right + S(8), by, ok.right + S(8) + bw, by + S(38)};
    FillRound(dc, ok, S(8), C_SURFACE, C_DANGER);
    FillRound(dc, cc, S(8), C_SURFACE, C_LINE);
    TextOutF(dc, g_fChip, C_DANGER, L"删除", ok, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    TextOutF(dc, g_fChip, C_INK, L"取消", cc, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
}

// PerfAppend / PerfInit 见 diag.cpp；下面是带业务语义的汇总输出。
static void PerfFlush() {
    if (g_perfAcc.n <= 0) return;
    char buf[512];
    int n = g_perfAcc.n;
    std::snprintf(buf, sizeof(buf),
                  "page=%d n=%d begin=%.2f prep=%.2f head=%.2f body=%.2f foot=%.2f end=%.2f "
                  "blit=%.2f total=%.2f max=%.2f ms\n",
                  g_page, n, PerfMs(g_perfAcc.begin) / n, PerfMs(g_perfAcc.prep) / n,
                  PerfMs(g_perfAcc.head) / n, PerfMs(g_perfAcc.body) / n,
                  PerfMs(g_perfAcc.foot) / n, PerfMs(g_perfAcc.end) / n,
                  PerfMs(g_perfAcc.blit) / n,
                  PerfMs(g_perfAcc.total) / n, PerfMs(g_perfAcc.maxTotal));
    PerfAppend(buf);
    {
        char b2[256];
        std::snprintf(b2, sizeof(b2),
                      "  emoji new=%d hit=%d fail=%d slots=%d/%d\n",
                      g_emoNewN, g_emoHitN, g_emoFailN, g_emoNextSlot, g_emoAtlasSlots);
        PerfAppend(b2);
    }
    g_emoNewN = g_emoHitN = g_emoFailN = 0;
    g_perfAcc = PerfAcc{};
}

// 离屏缓冲复用：每帧重建 560×620 的 DIBSection 是最大的固定开销（实测 2.5~3ms/帧），
// 尺寸不变就一直用同一块；DCRenderTarget 绑定的位图在 BeginDraw 时不会被清空，
// 所以保留上一帧内容还能配合「只重绘脏区」。
static HDC     g_bbDc = nullptr;
static HBITMAP g_bbBmp = nullptr;
static int     g_bbW = 0, g_bbH = 0;

static bool AcquireBackBuffer(HDC ref, int W, int H) {
    if (g_bbDc && g_bbW == W && g_bbH == H) return true;
    if (g_bbBmp) { DeleteObject(g_bbBmp); g_bbBmp = nullptr; }
    if (g_bbDc)  { DeleteDC(g_bbDc); g_bbDc = nullptr; }
    g_bbDc = CreateCompatibleDC(ref);
    if (!g_bbDc) return false;
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = W;
    bmi.bmiHeader.biHeight = -H;      // 自上而下，坐标与窗口一致
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    g_bbBmp = CreateDIBSection(ref, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!g_bbBmp) { DeleteDC(g_bbDc); g_bbDc = nullptr; return false; }
    SelectObject(g_bbDc, g_bbBmp);    // 自己的 DC，不用还原
    g_bbW = W; g_bbH = H;
    g_needBind = true;
    return true;
}

static bool RectsOverlap(const RECT& a, const RECT& b) {
    return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}

static void PaintPanel(HWND hwnd) {
    LONGLONG tb = g_perfOn ? PerfNow() : 0;
    PAINTSTRUCT ps;
    HDC dc0 = BeginPaint(hwnd, &ps);
    RECT rc; GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;
    LONGLONG t0 = g_perfOn ? PerfNow() : 0;

    // 新建缓冲时内容未定义，必须整窗重画
    bool fresh = !(g_bbDc && g_bbW == W && g_bbH == H);
    if (!AcquireBackBuffer(dc0, W, H)) { EndPaint(hwnd, &ps); return; }
    HDC dc = g_bbDc;
    RECT dirty = fresh ? rc : ps.rcPaint;
    if (IsRectEmpty(&dirty)) { EndPaint(hwnd, &ps); return; }

    // 缩略图必须在进 D2D 批次之前建好：CreateBitmap 是创建设备资源的操作，
    // 放在 BeginDraw/EndDraw 之间会触发目标内部的 flush
    if (g_rt && (g_page == PG_HISTORY || g_page == PG_FAV)) {
        for (int i = 0; i < L.nRow; ++i) {
            int vi = g_top + i;
            if (vi >= (int)g_view.size()) break;
            GetThumb(g_items[g_view[vi]]);
        }
    }
    // emoji 位图缓存同理：先补全可见格，再进 D2D 批次
    if (g_rt && g_page == PG_EMOJI && RectsOverlap(dirty, L.body)) PrepareEmojiBitmaps();

    PaintBegin(dc, W, H, &dirty);
    LONGLONG t1 = g_perfOn ? PerfNow() : 0;

    // 背景：只填脏区
    FillBox(dc, dirty, C_SURFACE);

    // 脏区之外的绘制整段跳过：脏区 clip 只能省光栅化，DWrite 的排版/字形解析
    // 在提交前就要做，跳过调用才是真省
    RECT topBand{0, 0, W, L.filters.bottom};
    if (RectsOverlap(dirty, topBand)) {
        PaintHead(dc);
        PaintSearch(dc);
        PaintFilters(dc);
    }
    LONGLONG t2 = g_perfOn ? PerfNow() : 0;

    if (RectsOverlap(dirty, L.body)) {
        // 主体（裁剪）
        if (g_rt && g_painting)
            g_rt->PushAxisAlignedClip(
                D2D1::RectF((float)L.body.left, (float)L.body.top,
                            (float)L.body.right, (float)L.body.bottom),
                D2D1_ANTIALIAS_MODE_ALIASED);
        FillBox(dc, L.body, C_SURFACE);

        if (g_page == PG_HISTORY || g_page == PG_FAV) {
            if (g_view.empty()) PaintEmpty(dc);
            else for (int i = 0; i < L.nRow; ++i) PaintRow(dc, i);
        } else if (g_page == PG_EMOJI) {
            PaintEmoji(dc);
        } else {
            PaintSymbols(dc);
        }
        PaintScrollbar(dc);
        if (g_rt && g_painting) g_rt->PopAxisAlignedClip();

        if (g_drawerOpen && (g_page == PG_EMOJI || g_page == PG_SYMBOL)) PaintDrawer(dc);
    }
    LONGLONG t3 = g_perfOn ? PerfNow() : 0;

    if (RectsOverlap(dirty, L.foot)) PaintFoot(dc);
    PaintToast(dc);
    PaintMenu(dc);
    PaintModal(dc);
    LONGLONG t4 = g_perfOn ? PerfNow() : 0;

    // 外边框（1px 线压在窗口内侧，对齐像素中心才不会糊）
    if (g_rt && g_painting) {
        float rad = S(RADIUS) / 2.0f;
        Brush(C_LINE);
        g_rt->DrawRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, (float)W - 0.5f, (float)H - 0.5f), rad, rad),
            g_brush, 1.0f);
    }

    PaintHeadButtons(dc);      // 压在外框线之上，让按钮高亮贴住窗口边缘

    PaintEnd();
    LONGLONG t5 = g_perfOn ? PerfNow() : 0;

    BitBlt(dc0, 0, 0, W, H, dc, 0, 0, SRCCOPY);
    LONGLONG t6 = g_perfOn ? PerfNow() : 0;

    EndPaint(hwnd, &ps);

    if (g_perfOn) {
        g_perfAcc.begin += t0 - tb;      // BeginPaint + GetClientRect + 缩略图预建
        g_perfAcc.prep += t1 - t0;
        g_perfAcc.head += t2 - t1;
        g_perfAcc.body += t3 - t2;
        g_perfAcc.foot += t4 - t3;
        g_perfAcc.end  += t5 - t4;
        g_perfAcc.blit += t6 - t5;
        LONGLONG tot = t6 - t0;
        g_perfAcc.total += tot;
        if (tot > g_perfAcc.maxTotal) g_perfAcc.maxTotal = tot;
        if (++g_perfAcc.n >= 30) PerfFlush();
    }
}

// ================================================================== 交互
// ---- 诊断日志（默认关闭）：排查前台/焦点问题时设 WV_TRACE=1 启动，
// 记录写到 %TEMP%\winvclip-trace.txt。默认路径上只有一次 getenv，开销可忽略。
// Trace 见 diag.cpp（WV_TRACE=1 时写 %TEMP%\winvclip-trace.txt）

static void HidePanel();
static bool PanelShouldStay(HWND hwnd);
static void RestoreTargetFocus(HWND target, HWND focus);
static void ShowPanel();
static void PasteItem(int vi);
static void ForceForeground(HWND hwnd);
static bool CaretRectOf(HWND wnd, RECT* out);
// 插入符探测结果：caret 是插入符本身，box（可选）是它所在的输入控件矩形。
// box 只用来「别挡住输入框」，因此宁可缺也不能错——整窗/整页的矩形会在 ShowPanel 里被否掉。
struct CaretInfo {
    RECT caret{};
    RECT box{};
    bool hasBox = false;
    // 插入符位置是「框内近似」而非真读到的：Chromium 对空文本框只回整框，
    // 这时用框内首行位置顶上，仍值得补探一次更准的（见 UiaCaretRect / TIMER_REPROBE）。
    bool approx = false;
};
static bool ProbeCaret(HWND target, CaretInfo* ci);
static void MovePanelToCaret(const CaretInfo& ci);

static void Toast(const std::wstring& msg, bool info = false) {
    g_toast = msg;
    g_toastInfo = info;
    g_toastOn = true;
    if (g_panel) {
        SetTimer(g_panel, TIMER_TOAST, 1500, nullptr);
        InvalidateRect(g_panel, nullptr, FALSE);
    }
}

static void Redraw(const RECT* r) {
    if (g_panel) InvalidateRect(g_panel, r, FALSE);
}

// 临时固定：仅本次显示有效（置顶 + 不失焦隐藏），面板关闭后自动取消
static void ToggleTemporaryPin() {
    g_pinned = !g_pinned;
    if (g_pinned) {
        SetWindowPos(g_panel, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        Toast(L"已临时固定，可连续插入", true);
    } else {
        // 取消固定后立刻回到「前台不在目标窗口就收起」的规则。
        // 注意判据变了：非激活态下面板从不持有前台，所以不能再看「前台是不是面板」，
        // 要看「前台还是不是唤起时那个目标窗口」。
        if (!PanelShouldStay(g_panel)) { HidePanel(); return; }
        Toast(L"已取消临时固定", true);
    }
    Redraw();
}

static void SetPage(int p) {
    if (g_page == p) return;
    g_page = p;
    g_sel = 0; g_top = 0; g_hoverRow = -1; g_hoverCell = -1;
    g_gridScroll = 0;
    g_menuOpen = false; g_modalOpen = false;
    g_drawerOpen = false; g_drawerHover = -1; g_drawerScroll = 0;
    BuildView();
    ComputeLayout();
    Redraw();
}

static void PasteItem(int vi) {
    if (vi < 0 || vi >= (int)g_view.size()) return;
    ClipItem it = g_items[g_view[vi]];
    HWND target = g_target;
    if (!g_pinned) HidePanel();      // 固定态保留面板，便于连续粘贴
    PutToClipboard(it);
    if (target && IsWindow(target) && target != g_panel) {
        g_pasteTarget = target;
        // 非激活态下前台本来就是目标窗口，这行是空转；只有输入态才需要交还。
        if (GetForegroundWindow() != target) SetForegroundWindow(target);
        SetTimer(g_main, TIMER_PASTE, 90, nullptr);
    }
}

// 表情 / 符号点击：把字形送进目标窗口。有两条通道，选哪条看能不能读到目标的文本插入符。
// 以往这里是 PutText + toast「已复制到剪贴板」，每点一个字形就把用户的剪贴板顶掉，
// 历史里也被塞满一堆表情——所以能直插的场合一律不碰剪贴板。
static void PasteGlyph(int page, const GItem& it) {
    AddRecentGlyph(page, it);
    if (CurGlyphCat(page) <= 0) ComputeLayout();   // 「最近使用」页插入后重排

    std::wstring ch = U8N(it.g, it.gl);
    HWND target = g_target;
    if (target && IsWindow(target) && target != g_panel) {
        // 通道选择（这里是兼容性的关键，别再用「有没有插入符」当能不能插的判据）：
        //   A. 目标有文本插入符（记事本/Edit/RichEdit/Office 这类标准输入控件）
        //      → KEYEVENTF_UNICODE 直插，完全不碰剪贴板。
        //   B. 取不到插入符（Chromium/Electron、UWP、终端、shell 列表）
        //      → 借剪贴板 + Ctrl+V。这类程序的插入符在渲染进程里，GetGUIThreadInfo
        //        根本看不到，但「看不见插入符」不等于「不能粘」：Edge 地址栏就是典型，
        //        早期版本在这里直接拒绝，于是出现「系统剪贴板能粘、我们的表情点不动」。
        //        Ctrl+V 对所有这类目标都成立，系统 Win+V 面板走的也是这条路。
        // 资源管理器文件列表这类真的没有文本输入区的地方走 B 也不会响铃：
        // Ctrl+V 对文本剪贴板是无操作（而字符注入会被 Shell 当成键入查找，响一声提示音）。
        RECT cr{};
        bool canInject = CaretRectOf(target, &cr);
        bool usePaste = g_shiftDown || !canInject;   // Shift = 强制走粘贴，用户的手动兜底

        if (!g_pinned) HidePanel();      // 固定态保留面板，便于连续插入
        g_pasteTarget = target;
        if (usePaste) {
            g_typePending.clear();
            BorrowClipboard();
            PutText(ch);
            g_selfClipSeq = GetClipboardSequenceNumber();
        } else {
            g_typePending = ch;
        }
        if (GetForegroundWindow() != target) SetForegroundWindow(target);
        SetTimer(g_main, TIMER_PASTE, 90, nullptr);
    } else {
        // 没有可插入的目标窗口（比如在桌面上唤起面板）：退回复制，至少不会「点了没反应」
        PutText(ch);
        if (g_pinned) Redraw();
        Toast(L"没有可插入的窗口，已复制");
    }
}

// 置顶：按 g_items 的绝对下标操作（菜单与 Ctrl+P 共用）
static void TogglePinItem(int idx) {
    if (idx < 0 || idx >= (int)g_items.size()) return;
    ClipItem& it = g_items[idx];
    it.pinned = !it.pinned;
    Toast(it.pinned ? L"已置顶" : L"已取消置顶", true);
    BuildView();
    ComputeLayout();
    Redraw();
}

static void ToggleFav(int vi) {
    if (vi < 0 || vi >= (int)g_view.size()) return;
    ClipItem& it = g_items[g_view[vi]];
    it.fav = !it.fav;
    Toast(it.fav ? L"已收藏" : L"已取消收藏", true);
    BuildView();
    ComputeLayout();
    Redraw();
}

// 当前选中的视图下标
static int SelView() { return g_sel; }

// 当前选中项在 g_items 里的绝对下标
static int SelItem() {
    int vi = SelView();
    return (vi >= 0 && vi < (int)g_view.size()) ? g_view[vi] : -1;
}

// ================================================================== 另存为
// 文本存 UTF-8(BOM) 的 .txt；PNG 条目直接落盘（无损），CF_DIB 经 WIC 重新编码成 PNG；
// 文件条目复制原件（多个文件时选目标文件夹，重名自动加序号）。

// W2U8 / FileNameOf / StampName 见 text_util.h

// WriteAllBytes 见 io_png.cpp

// SaveRgbaAsPng 见 io_png.cpp

// 保存对话框：owner 是面板，期间要禁掉「失焦即隐藏」，否则刚弹出就被收起
static bool PickSavePath(const std::wstring& defName, const wchar_t* filter,
                         const wchar_t* defExt, std::wstring& out) {
    wchar_t buf[1024] = {};
    wcsncpy(buf, defName.c_str(), 1023);

    wchar_t desk[MAX_PATH] = {};
    bool hasDesk = SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, desk));

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = g_panel;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = (DWORD)ARRAYSIZE(buf);
    ofn.lpstrDefExt = defExt;
    ofn.lpstrInitialDir = hasDesk ? desk : nullptr;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;

    // 临时固定态下面板是 TOPMOST，会把对话框压住：先降一级，结束后恢复
    bool wasTop = g_pinned && g_panel;
    if (wasTop) SetWindowPos(g_panel, HWND_NOTOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    g_dialogOpen = true;
    BOOL ok = GetSaveFileNameW(&ofn);
    g_dialogOpen = false;

    if (wasTop) SetWindowPos(g_panel, HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (ok) out = buf;
    return ok != FALSE;
}

// UniquePath 见 io_png.cpp

static void SaveItemAs(int idx) {
    if (idx < 0 || idx >= (int)g_items.size()) return;
    ClipItem it = g_items[idx];      // 拷一份：对话框是模态的，历史可能在此期间被改动

    if (it.kind == 1) {
        if (it.files.empty()) { Toast(L"这条没有可保存的文件", true); return; }
        // 单文件：原样另存；多文件：用第一个文件确认目标位置，其余复制到同一目录
        std::wstring path;
        if (!PickSavePath(FileNameOf(it.files[0]), L"所有文件 (*.*)\0*.*\0", nullptr, path)) return;

        int saved = 0;
        if (CopyFileW(it.files[0].c_str(), path.c_str(), FALSE)) ++saved;
        if (it.files.size() > 1) {
            size_t slash = path.find_last_of(L"\\/");
            std::wstring dir = (slash == std::wstring::npos) ? std::wstring() : path.substr(0, slash);
            for (size_t i = 1; i < it.files.size(); ++i) {
                std::wstring dst = UniquePath(dir + L"\\" + FileNameOf(it.files[i]));
                if (CopyFileW(it.files[i].c_str(), dst.c_str(), FALSE)) ++saved;
            }
        }
        if (it.files.size() == 1)
            Toast(saved ? L"已保存：" + FileNameOf(path) : L"保存失败（原文件可能已被移动）", !saved);
        else
            Toast(L"已保存 " + std::to_wstring(saved) + L" / " +
                  std::to_wstring(it.files.size()) + L" 个文件", saved == 0);
        if (g_panel && IsWindowVisible(g_panel)) ForceForeground(g_panel);
        return;
    }

    std::wstring path;
    if (it.kind == 0) {
        std::wstring name = L"剪贴板文本_" + StampName(it.st) + L".txt";
        if (!PickSavePath(name, L"文本文件 (*.txt)\0*.txt\0所有文件 (*.*)\0*.*\0", L"txt", path)) return;
        std::string body = W2U8(it.text);
        std::vector<BYTE> buf{0xEF, 0xBB, 0xBF};    // BOM：记事本等能正确识别 UTF-8
        buf.insert(buf.end(), body.begin(), body.end());
        bool ok = WriteAllBytes(path, buf.data(), buf.size());
        Toast(ok ? L"已保存：" + FileNameOf(path) : L"保存失败", !ok);
    } else if (it.kind == 2) {
        std::wstring name = L"剪贴板图片_" + StampName(it.st) + L".png";
        if (!PickSavePath(name, L"PNG 图片 (*.png)\0*.png\0所有文件 (*.*)\0*.*\0", L"png", path)) return;
        bool ok = false;
        if (it.isPng) {
            ok = WriteAllBytes(path, it.bits.data(), it.bits.size());
        } else {
            Rgba img;
            if (DibToRgba(it.bits, img)) {
                // CF_DIB 没有 alpha 语义（32bpp 的 alpha 字节通常全是 0），
                // 原样编码会得到一张全透明 PNG，这里统一补成不透明
                for (size_t i = 3; i < img.px.size(); i += 4) img.px[i] = 255;
                ok = SaveRgbaAsPng(img, path);
            }
        }
        Toast(ok ? L"已保存：" + FileNameOf(path) : L"保存失败", !ok);
    } else {
        Toast(L"这条没有可保存的内容", true);
    }

    if (g_panel && IsWindowVisible(g_panel)) ForceForeground(g_panel);
}

// 执行菜单项（鼠标与键盘共用）
static void RunMenuItem(int hit) {
    int row = g_menuRow, item = g_menuItem;
    g_menuOpen = false;
    switch (hit) {
    case MI_PIN:  TogglePinItem(item); break;
    case MI_SAVE: SaveItemAs(item); break;
    case MI_DELETE:
        g_modalOpen = true;
        g_modalRow = row;
        Redraw();
        break;
    default:
        Redraw();
        break;
    }
}

static void ClampScroll() {
    ComputeLayout();
    int fit = L.nRow;
    if (g_top > 0 && g_sel < g_top) g_top = g_sel;
    if (g_top + fit <= g_sel) g_top = g_sel - fit + 1;
    int maxTop = (int)g_view.size() - fit;
    if (maxTop < 0) maxTop = 0;
    if (g_top > maxTop) g_top = maxTop;
    if (g_top < 0) g_top = 0;
    ComputeLayout();
}

// 网格页内容总高（表情与符号同一套网格）
static int GridContentH() {
    if (g_page == PG_EMOJI || g_page == PG_SYMBOL) {
        int n = GlyphCount(g_page);
        int rows = (n + GlyphCols() - 1) / GlyphCols();
        return S(8) + rows * (S(44) + S(5));
    }
    return 0;
}

static void ClampGridScroll() {
    int maxS = GridContentH() - (L.body.bottom - L.body.top) + S(12);
    if (maxS < 0) maxS = 0;
    if (g_gridScroll > maxS) g_gridScroll = maxS;
    if (g_gridScroll < 0) g_gridScroll = 0;
}

// ---------------------------------------------------------------- 尺寸调整
// 不用系统的 HTLEFT/HTBOTTOMRIGHT：那是模态 sizing 循环，一进去面板会被判失活而自动收起
// （和标题栏拖动踩的是同一个坑）。所以自己 SetCapture 算差值。
static void RoundCorners(HWND hwnd);

static int HitResize(POINT p) {
    if (!g_panel) return RS_NONE;
    RECT rc; GetClientRect(g_panel, &rc);
    int e = S(RESIZE_EDGE);
    bool inL = p.x < e, inR = p.x >= rc.right - e;
    bool inT = p.y < e, inB = p.y >= rc.bottom - e;
    if (inT && inL) return RS_TL;
    if (inT && inR) return RS_TR;
    if (inB && inL) return RS_BL;
    if (inB && inR) return RS_BR;
    if (inL) return RS_L;
    if (inR) return RS_R;
    if (inT) return RS_T;
    if (inB) return RS_B;
    return RS_NONE;
}

// 夹最小尺寸时「正在拖的那条边不动、对面的边动」，否则拖到下限时
// 窗口会从另一侧往回缩，手感很怪
static RECT ClampToMin(RECT r) {
    int minW = S(MIN_W), minH = S(MIN_H);
    bool leftSide = (g_resizing == RS_L || g_resizing == RS_TL || g_resizing == RS_BL);
    bool topSide  = (g_resizing == RS_T || g_resizing == RS_TL || g_resizing == RS_TR);
    if (r.right - r.left < minW) {
        if (leftSide) r.left = r.right - minW; else r.right = r.left + minW;
    }
    if (r.bottom - r.top < minH) {
        if (topSide) r.top = r.bottom - minH; else r.bottom = r.top + minH;
    }
    return r;
}

static void ApplyResize(RECT r) {
    r = ClampToMin(r);
    HMONITOR mon = MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{}; mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi)) {
        if (r.left   < mi.rcWork.left)   r.left   = mi.rcWork.left;
        if (r.top    < mi.rcWork.top)    r.top    = mi.rcWork.top;
        if (r.right  > mi.rcWork.right)  r.right  = mi.rcWork.right;
        if (r.bottom > mi.rcWork.bottom) r.bottom = mi.rcWork.bottom;
        r = ClampToMin(r);
    }
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w == g_panelW && h == g_panelH) {
        RECT cur; GetWindowRect(g_panel, &cur);
        if (cur.left == r.left && cur.top == r.top) return;
    }
    SetWindowPos(g_panel, nullptr, r.left, r.top, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    g_panelW = w; g_panelH = h;
    RoundCorners(g_panel);        // 圆角 region 按新尺寸重做
    ComputeLayout();
    ClampGridScroll();
    ClampScroll();
    Redraw();
}

static void WheelBody(int delta) {
    if (g_drawerOpen) {
        g_drawerScroll += (delta > 0) ? -S(48) : S(48);
        ClampDrawerScroll();
        ComputeLayout();
        Redraw();
        return;
    }
    if (g_page == PG_HISTORY || g_page == PG_FAV) {
        if (delta > 0) { if (g_top > 0) --g_top; }
        else { int maxTop = (int)g_view.size() - L.nRow; if (g_top < maxTop) ++g_top; }
    } else {
        int step = S(56);
        g_gridScroll += (delta > 0) ? -step : step;
        ClampGridScroll();
    }
    Redraw(&L.body);      // 滚动只影响列表区，外框/标题/页脚保持原样
}

// ================================================================== 窗口过程
// 面板的按键处理。两个入口共用这一份逻辑：
//   · 输入态（用户点过搜索框，面板持有焦点）→ 系统投递的 WM_KEYDOWN；
//   · 非激活态（常态）→ 低级键盘钩子转发来的 WM_APP_PANELKEY。
static void PanelKeyDown(int vk, bool ctrl) {

        // 输入法正在组合（g_comp 非空）：按键是给输入法的（空格上屏、Esc 取消、
        // 数字选候选），别让它们顺带触发面板的快捷键——尤其 Esc，否则想取消
        // 组合反而把面板关了。
        if (!g_comp.empty()) return;

        if (g_modalOpen) {
            if (vk == VK_ESCAPE) { g_modalOpen = false; Redraw(); return; }
            if (vk == VK_RETURN) {
                if (g_modalRow >= 0 && g_modalRow < L.nRow && g_top + g_modalRow < (int)g_view.size()) {
                    g_items.erase(g_items.begin() + g_view[g_top + g_modalRow]);
                    g_sel = 0; g_top = 0;
                }
                g_modalOpen = false;
                BuildView(); ComputeLayout();
                Toast(L"已删除", true);
                Redraw();
                return;
            }
            return;
        }
        if (g_menuOpen) {
            if (vk == VK_ESCAPE) { g_menuOpen = false; Redraw(); return; }
            if (vk == VK_DOWN || vk == VK_UP) {
                int d = (vk == VK_DOWN) ? 1 : MI_COUNT - 1;
                g_menuHover = (g_menuHover < 0) ? 0 : (g_menuHover + d) % MI_COUNT;
                Redraw();
                return;
            }
            if (vk == VK_RETURN) {
                RunMenuItem(g_menuHover < 0 ? 0 : g_menuHover);
                return;
            }
        }

        if (ctrl) {
            if (vk >= '1' && vk <= '4') { SetPage(vk - '1'); return; }
            if (vk == 'P') { TogglePinItem(SelItem()); return; }
            if (vk == 'D') { ToggleFav(SelView()); return; }
            if (vk == 'S') { SaveItemAs(SelItem()); return; }
            return;
        }

        switch (vk) {
        case VK_ESCAPE:
            if (g_drawerOpen) { ToggleDrawer(false); return; }
            HidePanel();
            return;
        case VK_RETURN:
            if (g_page == PG_HISTORY || g_page == PG_FAV) PasteItem(SelView());
            return;
        case VK_UP:
            if (g_page == PG_HISTORY || g_page == PG_FAV) {
                if (g_sel > 0) { --g_sel; ClampScroll(); Redraw(); }
            }
            return;
        case VK_DOWN:
            if (g_page == PG_HISTORY || g_page == PG_FAV) {
                if (g_sel + 1 < (int)g_view.size()) { ++g_sel; ClampScroll(); Redraw(); }
            }
            return;
        case VK_PRIOR:
            if (g_page == PG_HISTORY || g_page == PG_FAV) {
                g_sel = (std::max)(0, g_sel - L.nRow);
                ClampScroll(); Redraw();
            }
            return;
        case VK_NEXT:
            if (g_page == PG_HISTORY || g_page == PG_FAV) {
                g_sel = (std::min)((int)g_view.size() - 1, g_sel + L.nRow);
                ClampScroll(); Redraw();
            }
            return;
        case VK_BACK:
            if (!g_query.empty()) {
                g_query.pop_back();
                g_sel = 0; g_top = 0;
                BuildView(); ComputeLayout(); Redraw();
            }
            return;
        case VK_DELETE:
            if (g_page == PG_HISTORY || g_page == PG_FAV) {
                g_modalOpen = true;
                g_modalRow = g_sel - g_top;
                Redraw();
            }
            return;
        case VK_TAB:
            SetPage((g_page + 1) % PG_COUNT);
            return;
        }
}

// 字符输入：同样两个入口（WM_CHAR / 钩子转发的字符）
static void PanelChar(wchar_t c) {
    if (c >= 32 && c != 127) {
        Trace("panel char '%c' query=%ls", (char)c, g_query.c_str());
        g_query.push_back(c);
        g_sel = 0; g_top = 0;
        BuildView(); ComputeLayout();
        Redraw();
    }
}

static LRESULT CALLBACK PanelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        PaintPanel(hwnd);
        return 0;

    case WM_TIMER:
        if (wp == TIMER_TOAST) {
            KillTimer(hwnd, TIMER_TOAST);
            g_toastOn = false;
            Redraw();
        } else if (wp == TIMER_CARET) {
            g_caretOn = !g_caretOn;
            if (g_page == PG_HISTORY || g_page == PG_FAV || g_page == PG_EMOJI || g_page == PG_SYMBOL) {
                RECT r = L.searchBox;
                r.top -= S(5); r.bottom += S(5); r.left -= S(4); r.right += S(4);
                Redraw(&r);      // 光标只管搜索框那一块
            } else {
                Redraw();
            }
        } else if (wp == TIMER_EMOJI) {
            KillTimer(hwnd, TIMER_EMOJI);
            // 预热/补齐都要推进，哪怕面板此刻不在表情页、甚至不可见
            bool more = PumpEmojiUploads();
            if (g_page == PG_EMOJI) Redraw(&L.body);
            if (more) SetTimer(hwnd, TIMER_EMOJI, 8, nullptr);
        } else if (wp == TIMER_REPROBE) {
            // 补探测：首轮没拿到插入符时的第二次机会。用户动过面板就不再挪。
            KillTimer(hwnd, TIMER_REPROBE);
            if (g_posUserSet || g_pinned || g_dragging || !g_target || !IsWindow(g_target)) return 0;
            if (GetForegroundWindow() != g_target) return 0;
            CaretInfo ci;
            if (ProbeCaret(g_target, &ci) && !ci.approx) {
                MovePanelToCaret(ci);
                g_reprobeLeft = 0;
            } else if (--g_reprobeLeft > 0) {
                SetTimer(hwnd, TIMER_REPROBE, 160, nullptr);
            }
        } else if (wp == TIMER_FOCUS) {
            // 轮询前台窗口。非激活态下面板收不到 WA_INACTIVE（它本来就没被激活过），
            // 所以「用户有没有切走」只能靠轮询看前台是谁，见 PanelShouldStay。
            if (!g_pinned && !g_dragging && !g_dialogOpen) {
                if (!PanelShouldStay(hwnd)) {
                    g_menuOpen = false;
                    g_modalOpen = false;
                    HidePanel();
                } else if (g_inputMode && GetForegroundWindow() != hwnd) {
                    // 用户从输入态点回了目标窗口：退出输入态，回到钩子转发模式
                    g_inputMode = false;
                }
            }
        } else if (wp == TIMER_DEFERHIDE) {
            // WA_INACTIVE 那一瞬间前台还没落定，见 WM_ACTIVATE 里的说明：
            // 等它落定再判，否则收起时的「还前台」会和用户那次点击抢。
            KillTimer(hwnd, TIMER_DEFERHIDE);
            PostMessageW(hwnd, WM_APP_DEFER_HIDE, 0, 0);
        }
        return 0;

    case WM_KEYDOWN:
        PanelKeyDown((int)wp, (GetKeyState(VK_CONTROL) & 0x8000) != 0);
        return 0;

    // 非激活态下按键由钩子转发到这里（面板没有键盘焦点，见 LLKeyboard）
    case WM_APP_PANELKEY:
        Trace("panelkey vk=0x%02X ch=%04X", (unsigned)wp, (unsigned)HIWORD(lp));
        PanelKeyDown((int)wp, LOWORD(lp) != 0);
        if (HIWORD(lp)) PanelChar((wchar_t)HIWORD(lp));
        return 0;

    case WM_CHAR:
        PanelChar((wchar_t)wp);
        return 0;
    // ---------------------------------------------------------- 输入法
    // 搜索框是自绘的，系统不知道光标在哪：组合串自己收、自己画，
    // 并把候选窗钉到光标下方。不处理这些消息的话，中文一个字也打不进来。
    case WM_IME_SETCONTEXT:
        lp &= ~(LPARAM)ISC_SHOWUICOMPOSITIONWINDOW;   // 组合串由面板自己画
        break;                                        // 其余标志交默认处理
    case WM_IME_STARTCOMPOSITION:
        // 不能在这里 return 0 拦掉：DefWindowProc 要借此建立组合窗口，
        // 拦了以后输入法状态机不完整，连空格上屏都不认。
        // 显示已经在 WM_IME_SETCONTEXT 里屏蔽掉，画面由面板自己画。
        g_comp.clear();
        PositionImeCandidate(hwnd);
        break;
    case WM_IME_COMPOSITION: {
        HIMC himc = ImmGetContext(hwnd);
        if (himc) {
            if (lp & GCS_RESULTSTR) {                 // 上屏：并入查询串
                LONG n = ImmGetCompositionStringW(himc, GCS_RESULTSTR, nullptr, 0);
                if (n > 0) {
                    std::wstring s((size_t)n / sizeof(wchar_t), L'\0');
                    ImmGetCompositionStringW(himc, GCS_RESULTSTR, &s[0], n);
                    g_query += s;
                }
                g_comp.clear();
            }
            if (lp & GCS_COMPSTR) {                   // 仍在组合：只做预览
                LONG n = ImmGetCompositionStringW(himc, GCS_COMPSTR, nullptr, 0);
                if (n > 0) {
                    g_comp.assign((size_t)n / sizeof(wchar_t), L'\0');
                    ImmGetCompositionStringW(himc, GCS_COMPSTR, &g_comp[0], n);
                } else {
                    g_comp.clear();
                }
            }
            ImmReleaseContext(hwnd, himc);
        }
        g_sel = 0; g_top = 0;
        BuildView(); ComputeLayout();
        PositionImeCandidate(hwnd);
        Redraw();
        return 0;
    }
    case WM_IME_ENDCOMPOSITION:
        g_comp.clear();
        Redraw();
        return 0;

    // 边缘显示缩放光标；标题栏空白处显示四向箭头，暗示可拖动
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(hwnd, &p);
            switch (HitResize(p)) {
            case RS_L: case RS_R:
                SetCursor(LoadCursorW(nullptr, IDC_SIZEWE)); return TRUE;
            case RS_T: case RS_B:
                SetCursor(LoadCursorW(nullptr, IDC_SIZENS)); return TRUE;
            case RS_TL: case RS_BR:
                SetCursor(LoadCursorW(nullptr, IDC_SIZENWSE)); return TRUE;
            case RS_TR: case RS_BL:
                SetCursor(LoadCursorW(nullptr, IDC_SIZENESW)); return TRUE;
            default: break;
            }
            bool blank = PtIn(L.head, p) && !PtIn(L.gear, p) && !PtIn(L.closeB, p) &&
                         !PtIn(L.pinB, p);
            for (int i = 0; i < 4 && blank; ++i)
                if (PtIn(L.tab[i], p)) blank = false;
            SetCursor(LoadCursorW(nullptr, blank ? IDC_SIZEALL : IDC_ARROW));
            return TRUE;
        }
        break;

    case WM_LBUTTONUP:
        if (g_resizing) {
            g_resizing = RS_NONE;
            ReleaseCapture();
            SaveViewSize();      // 尺寸落盘，下次打开沿用（重启也记得）
            Redraw();
            return 0;
        }
        if (g_dragging) {
            g_dragging = false;
            ReleaseCapture();
            Redraw();
            return 0;
        }
        return 0;

    case WM_MOUSEMOVE: {
        // 调整尺寸中：屏幕坐标差直接改窗口矩形
        if (g_resizing) {
            POINT sc{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ClientToScreen(hwnd, &sc);
            int dx = sc.x - g_rsFrom.x, dy = sc.y - g_rsFrom.y;
            RECT r = g_rsRect;
            bool l = (g_resizing == RS_L || g_resizing == RS_TL || g_resizing == RS_BL);
            bool rr = (g_resizing == RS_R || g_resizing == RS_TR || g_resizing == RS_BR);
            bool t = (g_resizing == RS_T || g_resizing == RS_TL || g_resizing == RS_TR);
            bool b = (g_resizing == RS_B || g_resizing == RS_BL || g_resizing == RS_BR);
            if (l) r.left += dx;
            if (rr) r.right += dx;
            if (t) r.top += dy;
            if (b) r.bottom += dy;
            ApplyResize(r);
            return 0;
        }
        // 拖动中：屏幕坐标差直接搬窗口
        if (g_dragging) {
            POINT sc{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ClientToScreen(hwnd, &sc);
            int nx = g_dragRect.left + (sc.x - g_dragFrom.x);
            int ny = g_dragRect.top + (sc.y - g_dragFrom.y);
            SetWindowPos(hwnd, nullptr, nx, ny, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        int oldChip = g_hoverChip, oldRow = g_hoverRow, oldCell = g_hoverCell;

        g_hoverChip = -1; g_hoverRow = -1; g_hoverCell = -1;

        if (PtIn(L.closeB, POINT{x, y})) g_hoverChip = 901;
        else if (PtIn(L.gear, POINT{x, y})) g_hoverChip = 900;
        else if (PtIn(L.pinB, POINT{x, y})) g_hoverChip = 902;
        else {
            for (int i = 0; i < L.nChip; ++i)
                if (PtIn(L.chip[i], POINT{x, y})) { g_hoverChip = i; break; }
            if (g_hoverChip < 0 && L.moreBtn.right > L.moreBtn.left && PtIn(L.moreBtn, POINT{x, y}))
                g_hoverChip = 903;
        }

        if (PtIn(L.body, POINT{x, y})) {
            if (g_drawerOpen) {
                int oldDH = g_drawerHover;
                g_drawerHover = DrawerHit(POINT{x, y});
                if (oldDH != g_drawerHover) Redraw();
                return 0;
            }
            if (g_page == PG_HISTORY || g_page == PG_FAV) {
                for (int i = 0; i < L.nRow; ++i)
                    if (PtIn(L.row[i], POINT{x, y})) { g_hoverRow = g_top + i; break; }
            } else {
                // 表情与符号共用同一套网格几何
                const std::vector<GItem>& list = GlyphItems(g_page);
                // 从第一个可见行起查：滚到深处时不必从第 0 条空转
                int stepG = S(44) + S(5);
                int firstG = (g_gridScroll > S(8))
                             ? ((g_gridScroll - S(8)) / stepG) * GlyphCols() : 0;
                for (int i = firstG; i < (int)list.size(); ++i) {
                    RECT c = GlyphCellRect(i);
                    if (c.top > L.body.bottom) break;
                    if (PtIn(c, POINT{x, y})) { g_hoverCell = i; break; }
                }
            }
        }

        if (g_menuOpen) {
            int oldMH = g_menuHover;
            g_menuHover = MenuHit(POINT{x, y});
            if (oldMH != g_menuHover) Redraw();
        }

        if (oldChip != g_hoverChip || oldRow != g_hoverRow || oldCell != g_hoverCell) {
            if (oldRow != g_hoverRow && g_hoverRow >= 0) g_sel = g_hoverRow;
            // 悬停只改局部观感：分类条/标题按钮一行，或列表区一行，不必整窗重画
            if (oldRow != g_hoverRow || oldCell != g_hoverCell) Redraw(&L.body);
            if (oldChip != g_hoverChip) {
                RECT rr{0, L.head.top, L.W, L.filters.bottom};
                Redraw(&rr);
            }
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        g_hoverChip = g_hoverRow = g_hoverCell = -1;
        Redraw();
        return 0;

    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        POINT pt{x, y};
        ComputeLayout();

        // 弹层优先
        if (g_modalOpen) {
            RECT m = ModalRect();
            int bw = (m.right - m.left - S(40) - S(8)) / 2;
            int by = m.bottom - S(20) - S(38);
            RECT ok{m.left + S(20), by, m.left + S(20) + bw, by + S(38)};
            if (PtIn(ok, pt)) {
                if (g_modalRow >= 0 && g_modalRow < L.nRow && g_top + g_modalRow < (int)g_view.size())
                    g_items.erase(g_items.begin() + g_view[g_top + g_modalRow]);
                g_modalOpen = false;
                g_sel = 0; g_top = 0;
                BuildView(); ComputeLayout();
                Toast(L"已删除", true);
            } else {
                g_modalOpen = false;
            }
            Redraw();
            return 0;
        }

        if (g_menuOpen) {
            RunMenuItem(MenuHit(pt));
            return 0;
        }

        // 点搜索框 → 进入输入态。中文搜索要靠输入法，而输入法组合必须有真正的
        // 键盘焦点，只能把前台交给面板。这是唯一会让目标窗口光标暂时消失的入口，
        // 由用户主动点击触发；Esc 关掉面板即刻还原。
        // IsWindowVisible 兜住「面板已隐藏却仍收到投递消息」的情况，否则前台会停在
        // 一个不可见的窗口上，而隐藏之后定时器也停了，没人再把它收回来。
        if (IsWindowVisible(hwnd) && PtIn(L.searchBox, pt) && GetForegroundWindow() != hwnd) {
            g_inputMode = true;
            ForceForeground(hwnd);
            return 0;
        }

        // 分类抽屉：点条目选中分类、点关闭或点抽屉外收起
        if (g_drawerOpen && PtIn(L.drawer, pt)) {
            if (PtIn(L.drawerClose, pt)) { ToggleDrawer(false); return 0; }
            int hit = DrawerHit(pt);
            if (hit >= 0) {
                g_query.clear(); g_comp.clear();
                SetGlyphCat(g_page, L.dCat[hit]);
                g_drawerOpen = false;
                g_drawerHover = -1;
                BuildView(); ComputeLayout(); Redraw();
            }
            return 0;
        }
        // 抽屉开着时点别处：先收起抽屉。标题栏区域放行，否则要关个窗口得点两次
        if (g_drawerOpen && pt.y >= S(HEAD_H)) { ToggleDrawer(false); return 0; }

        // 窗口边缘/角落 → 抓取改变尺寸。必须排在标题栏拖动之前：
        // 顶部那条 5px 落在标题栏里，先判 resize 才不会误触发拖动
        int rs = HitResize(pt);
        if (rs != RS_NONE) {
            POINT sc = pt;
            ClientToScreen(hwnd, &sc);
            GetWindowRect(hwnd, &g_rsRect);
            g_rsFrom = sc;
            g_resizing = rs;
            g_posUserSet = true;      // 用户自己摆过位置，补探测不许再挪
            SetCapture(hwnd);
            return 0;
        }

        // 标题栏空白处按下 → 手动拖动（不走 HTCAPTION 模态循环：拖动中窗口会被判失活而自动收起）
        if (PtIn(L.head, pt) && !PtIn(L.gear, pt) && !PtIn(L.closeB, pt) && !PtIn(L.pinB, pt)) {
            bool onTab = false;
            for (int i = 0; i < 4; ++i)
                if (PtIn(L.tab[i], pt)) { onTab = true; break; }
            if (!onTab) {
                POINT sc = pt;
                ClientToScreen(hwnd, &sc);
                GetWindowRect(hwnd, &g_dragRect);
                g_dragFrom = sc;
                g_dragging = true;
                g_posUserSet = true;   // 同上：用户已经把面板摆到想要的地方了
                SetCapture(hwnd);
                return 0;
            }
        }

        // 窗口控制
        if (PtIn(L.closeB, pt)) { HidePanel(); return 0; }
        if (PtIn(L.gear, pt)) {
            // TODO: 设置弹层待实现（当前点击不做任何事）
            return 0;
        }
        if (PtIn(L.pinB, pt)) { ToggleTemporaryPin(); return 0; }

        // 页签
        for (int i = 0; i < 4; ++i) {
            if (PtIn(L.tab[i], pt)) { SetPage(i); return 0; }
        }

        // chips
        for (int i = 0; i < L.nChip; ++i) {
            if (PtIn(L.chip[i], pt)) {
                int cat = L.chipCat[i];
                if (g_page == PG_EMOJI || g_page == PG_SYMBOL) {
                    g_query.clear(); g_comp.clear();
                    SetGlyphCat(g_page, cat);
                    BuildView();
                } else {
                    g_filter = cat; g_sel = 0; g_top = 0;
                    BuildView();
                }
                ComputeLayout();
                Redraw();
                return 0;
            }
        }
        if (L.moreBtn.right > L.moreBtn.left && PtIn(L.moreBtn, pt)) {
            ToggleDrawer(!g_drawerOpen);
            return 0;
        }

        // 主体
        if (PtIn(L.body, pt)) {
            if (g_page == PG_HISTORY || g_page == PG_FAV) {
                for (int i = 0; i < L.nRow; ++i) {
                    if (PtIn(L.row[i], pt)) {
                        int vi = g_top + i;
                        g_sel = vi;
                        PasteItem(vi);
                        return 0;
                    }
                }
            } else {
                const std::vector<GItem>& list = GlyphItems(g_page);
                int stepG = S(44) + S(5);
                int firstG = (g_gridScroll > S(8))
                             ? ((g_gridScroll - S(8)) / stepG) * GlyphCols() : 0;
                for (int i = firstG; i < (int)list.size(); ++i) {
                    RECT c = GlyphCellRect(i);
                    if (c.top > pt.y) break;     // 行的 top 自上而下递增，越过光标就不必再看
                    if (PtIn(c, pt)) {
                        PasteGlyph(g_page, list[i]);
                        return 0;
                    }
                }
            }
        }
        return 0;
    }

    case WM_RBUTTONUP: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        POINT pt{x, y};
        ComputeLayout();
        if (g_modalOpen) return 0;
        if (PtIn(L.body, pt) && (g_page == PG_HISTORY || g_page == PG_FAV)) {
            for (int i = 0; i < L.nRow; ++i) {
                if (PtIn(L.row[i], pt)) {
                    g_sel = g_top + i;
                    g_menuPt = pt;
                    g_menuOpen = true;
                    g_menuRow = i;
                    g_menuItem = g_view[g_top + i];
                    g_menuHover = -1;
                    Redraw();
                    return 0;
                }
            }
        }
        return 0;
    }

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        WheelBody(delta);
        return 0;
    }

    // 面板是 WS_EX_NOACTIVATE，正常不会收到这些消息；只有「输入态」（用户点过
    // 搜索框，我们主动把前台交给面板）以及文件对话框关闭后才会走到这里。
    case WM_MOUSEACTIVATE:
        // 鼠标点面板不要激活它：焦点留在目标窗口，光标继续闪。
        return MA_NOACTIVATE;

    case WM_ACTIVATE:
        Trace("WM_ACTIVATE wp=%d fg=%p", (int)LOWORD(wp), (void*)GetForegroundWindow());
        if (LOWORD(wp) == WA_INACTIVE) {
            if (g_dialogOpen) return 0;     // 另存为对话框是面板的 owned window
            // 固定态：不失焦隐藏；顺手把新的前台窗口记为粘贴目标，方便换个窗口继续插
            if (g_pinned) {
                HWND fg = GetForegroundWindow();
                if (fg && fg != hwnd) g_target = fg;
                return 0;
            }
            // 输入态下失去前台：可能是用户点回了目标窗口（正常，继续留着），
            // 也可能是点去了别的窗口。统一交给 PanelShouldStay 判断，别在这里下结论。
            // 但前台切换此刻常常还没落定（GetForegroundWindow() 仍返回面板自己），
            // 这时收起会走「还前台」分支，把用户正要点的那次激活挤掉——实测点了
            // 另一个应用，前台却被拽回上一个窗口。所以先等落定再判。
            if (GetForegroundWindow() == hwnd) SetTimer(hwnd, TIMER_DEFERHIDE, 120, nullptr);
            else PostMessageW(hwnd, WM_APP_DEFER_HIDE, 0, 0);
            return 0;
        }
        return 0;

    case WM_APP_DEFER_HIDE:
        Trace("WM_APP_DEFER_HIDE fg=%p", (void*)GetForegroundWindow());
        if (g_pinned || g_dialogOpen || g_dragging) return 0;
        if (!PanelShouldStay(hwnd)) {
            g_menuOpen = false;
            g_modalOpen = false;
            HidePanel();
        }
        return 0;

    // 低级鼠标钩子看到「按下的点落在面板矩形之外」时投递过来（见 LLMouse）。
    // 光靠前台变化判不出来这种情况：非激活态下面板从不改变前台，用户在同一个
    // 窗口里点掉面板旁边的正文/地址栏时，GetForegroundWindow() 纹丝不动。
    case WM_APP_HIDE_OUTSIDE:
        Trace("WM_APP_HIDE_OUTSIDE fg=%p root=%p", (void*)GetForegroundWindow(), (void*)wp);
        if (g_pinned || g_dialogOpen || g_dragging || g_resizing) return 0;
        // 用户点的若不是唤起时的目标窗口（点了别的应用、桌面…），那个窗口马上会
        // 激活，收起时别去抢前台，否则它抢不过前台锁，用户会觉得「点了没反应」。
        g_hideNoRestore = ((HWND)wp != g_target);
        g_menuOpen = false;
        g_modalOpen = false;
        g_drawerOpen = false;
        g_drawerHover = -1;
        HidePanel();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ================================================================== 目标窗口的文本插入符
// 两个用途：
//   1. 面板贴着「文本插入符」弹出，而不是跟着鼠标——用户唤起面板时光标往往并不在
//      鼠标指针那儿；没有插入符可用时退回屏幕居中。
//   2. 表情/符号直插之前判断目标窗口到底有没有文本输入区。资源管理器的文件列表、
//      桌面这类地方没有插入符，把字符注入进去什么也插不进，还会触发系统提示音。
static bool CaretRectOf(HWND wnd, RECT* out) {
    if (!wnd || !IsWindow(wnd)) { Trace("  caret: no target wnd (%p)", (void*)wnd); return false; }
    if (wnd == g_panel || wnd == g_main) return false;
    DWORD tid = GetWindowThreadProcessId(wnd, nullptr);
    if (!tid) return false;
    GUITHREADINFO gi{}; gi.cbSize = sizeof(gi);
    if (!GetGUIThreadInfo(tid, &gi)) { Trace("  caret: GetGUIThreadInfo fail tid=%lu", (unsigned long)tid); return false; }
    if (!gi.hwndCaret || !IsWindow(gi.hwndCaret)) {
        Trace("  caret: hwndCaret=%p focus=%p wnd=%p tid=%lu", (void*)gi.hwndCaret, (void*)gi.hwndFocus,
              (void*)wnd, (unsigned long)tid);
        return false;
    }

    RECT rc = gi.rcCaret;
    // rcCaret 的坐标基准在文档与实测之间不一致（一部分窗口给的是客户区坐标），
    // 拿 caret 所在窗口的屏幕矩形做判别，否则面板会跑到屏幕外面去。
    RECT wr{}; GetWindowRect(gi.hwndCaret, &wr);
    POINT o{0, 0}; ClientToScreen(gi.hwndCaret, &o);
    RECT conv{rc.left + o.x, rc.top + o.y, rc.right + o.x, rc.bottom + o.y};
    auto inside = [](const RECT& r, const RECT& b) {
        return r.left >= b.left - 2 && r.top >= b.top - 2 &&
               r.right <= b.right + 2 && r.bottom <= b.bottom + 2;
    };
    if (inside(conv, wr)) *out = conv;
    else if (inside(rc, wr)) *out = rc;
    else { Trace("  caret: rect outside wnd rc=%ld,%ld,%ld,%ld conv=%ld,%ld wr=%ld,%ld,%ld,%ld",
                 rc.left, rc.top, rc.right, rc.bottom, conv.left, conv.top,
                 wr.left, wr.top, wr.right, wr.bottom); return false; }

    if (out->right - out->left < 1) out->right = out->left + 1;
    if (out->bottom - out->top < 1) out->bottom = out->top + S(16);   // 折叠成一点的 caret
    return true;
}

// 原生通道的「输入框矩形」：焦点控件（Edit / RichEdit / 记事本的编辑区…）的窗口矩形。
// 只当它是控件、而不是整窗时才认（见 ShowPanel 里的合理性判断）。
static bool FocusBoxOf(HWND wnd, RECT* out) {
    DWORD tid = GetWindowThreadProcessId(wnd, nullptr);
    if (!tid) return false;
    GUITHREADINFO gi{}; gi.cbSize = sizeof(gi);
    if (!GetGUIThreadInfo(tid, &gi)) return false;
    if (!gi.hwndFocus || !IsWindow(gi.hwndFocus)) return false;
    return GetWindowRect(gi.hwndFocus, out) != 0;
}

// ---------------------------------------------------------------- UIA 兜底：浏览器 / Electron
// Chromium / Electron（Edge、Chrome、VS Code、各种内嵌浏览器、Slack…）把文本插入符放在
// 渲染进程里，GetGUIThreadInfo 的 hwndCaret 恒为 0 ——「光标明明在闪，却读不到」。
// 它们的可访问性（UIA）文本模式能给出插入符 / 选区的屏幕矩形，是这类窗口唯一能让面板
// 贴着光标弹出的通道（早期版本只走 GetGUIThreadInfo，于是这些窗口一律退回屏幕居中）。
// 代价：第一次访问 Chromium 会唤醒它的可访问性树（几十~几百 ms），所以只在
// GetGUIThreadInfo 失败时才走这里，并且把 UIA 的连接超时压到 500ms（默认 2s）。
static IUIAutomation* g_uia = nullptr;
static bool           g_uiaTried = false;

static IUIAutomation* UiaClient() {
    if (g_uiaTried) return g_uia;
    g_uiaTried = true;
    IUIAutomation* uia = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IUIAutomation, (void**)&uia);
    if (FAILED(hr) || !uia) { Trace("  uia: create fail hr=%08lx", (unsigned long)hr); return nullptr; }
    IUIAutomation2* uia2 = nullptr;
    if (SUCCEEDED(uia->QueryInterface(IID_IUIAutomation2, (void**)&uia2)) && uia2) {
        uia2->put_ConnectionTimeout(500);
        uia2->Release();
    }
    g_uia = uia;
    return g_uia;
}

// 取文本范围的包围矩形：SAFEARRAY 里每 4 个 double 是 (left, top, width, height)，
// 屏幕物理像素。多组时插入符一般只有一组，选区则按 last 取末尾那组（光标在选区末端）。
static bool UiaRangeRect(IUIAutomationTextRange* range, bool last, RECT* out) {
    SAFEARRAY* sa = nullptr;
    if (FAILED(range->GetBoundingRectangles(&sa)) || !sa) return false;
    bool ok = false;
    double* d = nullptr;
    if (SUCCEEDED(SafeArrayAccessData(sa, (void**)&d)) && d) {
        long lo = 0, hi = -1;
        SafeArrayGetLBound(sa, 1, &lo);
        SafeArrayGetUBound(sa, 1, &hi);
        long n = (hi >= lo) ? (hi - lo + 1) / 4 : 0;
        if (n > 0) {
            long i = last ? n - 1 : 0;
            double x = d[i * 4], y = d[i * 4 + 1], w = d[i * 4 + 2], h = d[i * 4 + 3];
            if (h > 0.5) {
                if (w < 1.0) w = 1.0;
                out->left = (LONG)lround(x);
                out->top = (LONG)lround(y);
                out->right = (LONG)lround(x + w);
                out->bottom = (LONG)lround(y + h);
                ok = (out->right > out->left) && (out->bottom > out->top);
            }
        }
        SafeArrayUnaccessData(sa);
    }
    SafeArrayDestroy(sa);
    return ok;
}

// 焦点元素是不是「目标窗口自己的东西」。
// Chromium / Electron 把页面内容放在渲染子进程里：网页输入框的焦点元素 pid 是那个子进程，
// 和顶层窗口的 pid 不同。早期这里按 pid 严格判等，结果网页里的输入框一律被当成
// 「别人的窗口」静默拒掉（地址栏属于浏览器进程，反而能过）——这就是「浏览器输入框读不到」。
// 现在认「同进程 或 目标进程的子孙进程」；面板自己、输入法候选窗这类旁支仍然被挡在外面。
static bool ProcessIsSelfOrDescendant(DWORD pid, DWORD ancestor) {
    if (!pid || !ancestor) return false;
    if (pid == ancestor) return true;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    DWORD cur = pid;
    for (int depth = 0; depth < 6 && cur; depth++) {
        PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
        DWORD parent = 0;
        if (Process32FirstW(snap, &pe)) {
            do {
                if (pe.th32ProcessID == cur) { parent = pe.th32ParentProcessID; break; }
            } while (Process32NextW(snap, &pe));
        }
        if (!parent) break;
        if (parent == ancestor) { found = true; break; }
        cur = parent;
    }
    CloseHandle(snap);
    return found;
}

// 插入符矩形是不是「控件整体矩形」。
// Chromium 系（Edge / WebView2 / Electron）在文本框内容为空、或同一文本视图里拿不到
// 字符布局时，会把整个控件框当成范围的包围盒返回：实测 WebView2 的聊天输入框
// （931x46）、Chromium 的空 textarea（660x300）都直接回整框，和真实插入符（1x17）
// 差着整块控件。采信它，落点算法就会把「整框」当成要避让的光标行，
// 面板被甩到框外远处——这就是「读到的明显是整个大输入框」。
static bool CaretRectIsBox(const RECT& c, const RECT& b) {
    int cw = c.right - c.left, ch = c.bottom - c.top;
    int bw = b.right - b.left, bh = b.bottom - b.top;
    if (cw <= 0 || ch <= 0 || bw <= 0 || bh <= 0) return false;
    long long ca = (long long)cw * ch, ba = (long long)bw * bh;
    if (ca * 10 >= ba * 8) return true;                         // 面积占了框的 8 成
    if (bw >= 40 && cw * 10 >= bw * 8 && ch >= bh - 2) return true;  // 和框一样宽一样高
    return false;
}

// 框内近似的插入符：内容为空 / 布局不可得时，唯一还成立的几何事实是
// 「插入符在文本框首行的起始处」。按框内左上内缩一个 padding、行高 20dp 画一根，
// 误差十几个像素，而整框矩形错的是整块控件。
static void ApproxCaretInBox(const RECT& box, RECT* out) {
    int pad = S(8);
    int bw = box.right - box.left, bh = box.bottom - box.top;
    if (pad * 3 > bw) pad = (std::max)(0, bw / 6);
    int lh = (std::min)(bh - pad * 2, S(20));
    if (lh < S(8)) lh = S(8);
    out->left = box.left + pad;
    out->right = out->left + 1;
    out->top = box.top + pad;
    out->bottom = out->top + lh;
}

static bool BoxSaneRect(const RECT& b, const RECT& work, HWND target);

static bool UiaCaretRect(HWND target, CaretInfo* info) {
    IUIAutomation* uia = UiaClient();
    if (!uia || !target || !IsWindow(target)) return false;

    DWORD pid = 0;
    GetWindowThreadProcessId(target, &pid);

    DWORD t0 = GetTickCount();
    IUIAutomationElement* el = nullptr;
    if (FAILED(uia->GetFocusedElement(&el)) || !el) { Trace("  uia: no focused element"); return false; }

    int ep = 0;
    if (FAILED(el->get_CurrentProcessId(&ep)) || !ProcessIsSelfOrDescendant((DWORD)ep, pid)) {
        // 焦点元素属于别的程序（面板自己、输入法候选窗…）：不是这次要贴的目标
        Trace("  uia: foreign focus elpid=%d targetpid=%lu", ep, (unsigned long)pid);
        el->Release();
        return false;
    }

    RECT* out = &info->caret;
    RECT rc{};
    bool ok = false;
    bool approx = false;

    // 焦点元素的控件类型决定「GetSelection 能不能信」。
    // Chromium 里的 input / textarea / contenteditable 分别是 Edit / Edit / Group，
    // 而焦点落在页面本身（body、页面级快捷键劫持）时是 Document：这种元素的
    // TextPattern 是整页文本视图，GetSelection 给的是页面里当前那块被选中/被跟踪的内容，
    // 与插入符无关（实测 bilibili 视频页给回一个 46x46 的方块），采信它面板会贴到乱处。
    CONTROLTYPEID ct = 0;
    el->get_CurrentControlType(&ct);
    bool isDocEl = (ct == UIA_DocumentControlTypeId);
    // 只对「看起来是文本控件」的类型采信 GetSelection 兜底：Edit / Document / Group / Pane /
    // Text / Custom（ct==0 表示没读出来，不排除）。列表项、按钮之类即便有 TextPattern 也不算。
    bool textish = (ct == 0) || ct == UIA_EditControlTypeId || ct == UIA_DocumentControlTypeId ||
                   ct == UIA_TextControlTypeId || ct == UIA_GroupControlTypeId ||
                   ct == UIA_PaneControlTypeId || ct == UIA_CustomControlTypeId;
    bool caretRangeOk = false;
    bool uiBadCaret = false;

    // 焦点控件整体矩形：既用来让面板别压住输入框本身，也用来识别「插入符矩形
    // 其实就是整框」这种假数据（见 CaretRectIsBox）。
    info->hasBox = false;
    RECT box{};
    bool boxOk = false;
    if (!isDocEl && SUCCEEDED(el->get_CurrentBoundingRectangle(&box)) &&
        box.right > box.left && box.bottom > box.top) {
        info->box = box;
        info->hasBox = true;
        boxOk = true;
    }

    // 1) TextPattern2::GetCaretRange —— 直接就是插入符，最准
    IUIAutomationTextPattern2* tp2 = nullptr;
    if (SUCCEEDED(el->GetCurrentPatternAs(UIA_TextPattern2Id, IID_IUIAutomationTextPattern2,
                                          (void**)&tp2)) && tp2) {
        BOOL active = FALSE;
        IUIAutomationTextRange* range = nullptr;
        if (SUCCEEDED(tp2->GetCaretRange(&active, &range)) && range) {
            if (active) {
                caretRangeOk = UiaRangeRect(range, false, &rc);
                ok = caretRangeOk;
                if (ok && boxOk && CaretRectIsBox(rc, box)) {
                    uiBadCaret = true;   // 回的是整框，不是插入符
                    ok = false;
                    caretRangeOk = false;
                }
            }
            range->Release();
        }
        tp2->Release();
    }

    // 2) TextPattern::GetSelection —— 没有 caret range 的控件（老实现、部分 Electron）
    //    用选区兜底：选区末尾就是插入符所在位置。文档级元素不适用，见上。
    if (!ok && textish && !isDocEl) {
        IUIAutomationTextPattern* tp = nullptr;
        if (SUCCEEDED(el->GetCurrentPatternAs(UIA_TextPatternId, IID_IUIAutomationTextPattern,
                                              (void**)&tp)) && tp) {
            IUIAutomationTextRangeArray* arr = nullptr;
            if (SUCCEEDED(tp->GetSelection(&arr)) && arr) {
                int n = 0;
                arr->get_Length(&n);
                if (n > 0) {
                    IUIAutomationTextRange* range = nullptr;
                    if (SUCCEEDED(arr->GetElement(n - 1, &range)) && range) {
                        ok = UiaRangeRect(range, true, &rc);
                        if (ok && boxOk && CaretRectIsBox(rc, box)) {
                            uiBadCaret = true;   // 同上：整框当插入符
                            ok = false;
                        }
                        range->Release();
                    }
                }
                arr->Release();
            }
            tp->Release();
        }
    }
    // 3) 通道都给不出可信插入符时，退到「框内近似」。
    //    内容为空的文本框是所有通道都失效的典型（Chromium 只回整框），但插入符
    //    必然在首行起始处；框本身可信（不是整窗、不是整页容器）才这么近似，
    //    否则宁可居中，免得把面板摆到整块控件外面老远。
    if (!ok && boxOk) {
        MONITORINFO mi{}; mi.cbSize = sizeof(mi);
        HMONITOR mon = MonitorFromRect(&box, MONITOR_DEFAULTTONEAREST);
        RECT work{};
        if (GetMonitorInfoW(mon, &mi)) work = mi.rcWork;
        if (work.right > work.left && BoxSaneRect(box, work, target)) {
            ApproxCaretInBox(box, &rc);
            ok = true;
            approx = true;
        }
    }
    el->Release();

    Trace("  uia: ok=%d ct=%d caret2=%d bad=%d approx=%d caret=%ld,%ld,%ld,%ld box=%d,%ld,%ld,%ld,%ld cost=%lums",
          (int)ok, (int)ct, (int)caretRangeOk, (int)uiBadCaret, (int)approx, rc.left, rc.top, rc.right,
          rc.bottom, (int)info->hasBox, info->box.left, info->box.top, info->box.right, info->box.bottom,
          (unsigned long)(GetTickCount() - t0));
    info->approx = approx;
    if (!ok) return false;
    if (rc.right - rc.left < 1) rc.right = rc.left + 1;
    *out = rc;
    return true;
}

// ================================================================== 显示 / 隐藏

// 两条通道依次试：原生插入符（GetGUIThreadInfo）→ UIA（浏览器 / Electron）。
static bool ProbeCaretRaw(HWND target, CaretInfo* ci) {
    *ci = CaretInfo{};
    if (!target || !IsWindow(target)) return false;
    if (CaretRectOf(target, &ci->caret)) {
        RECT box{};
        if (FocusBoxOf(target, &box)) { ci->box = box; ci->hasBox = true; }
        Trace("  caret: ok=%ld,%ld,%ld,%ld box=%d[%ld,%ld,%ld,%ld]", ci->caret.left, ci->caret.top,
              ci->caret.right, ci->caret.bottom, (int)ci->hasBox,
              ci->box.left, ci->box.top, ci->box.right, ci->box.bottom);
        return true;
    }
    return UiaCaretRect(target, ci);
}

static bool ProbeCaret(HWND target, CaretInfo* ci) {
    if (!ProbeCaretRaw(target, ci)) return false;
    // Chromium 第一次被 UIA 询问时常常只回「整框」或退化矩形（可访问性树刚被唤醒），
    // 紧接着再问一次通常就精确了——这次同步重试比等补探定时器好：面板不必先跳一下。
    if (ci->approx) {
        CaretInfo again;
        if (ProbeCaretRaw(target, &again) && !again.approx) *ci = again;
    }
    return true;
}

// 输入控件矩形可不可信。整窗（WPF / WinUI 自绘窗口的焦点控件就是顶层窗口）、
// 浏览器的整页 document 都会给出一个巨大矩形，拿它避让会把面板推到窗口外面去
// （实测 Win11 记事本：焦点控件矩形 = 整个窗口，面板于是被摆到窗口下方的空白区），
// 所以宁缺勿滥：太高的、又宽又高的、跟目标窗口几乎一样大的一律不认。
static bool BoxSaneRect(const RECT& b, const RECT& work, HWND target) {
    int bw = b.right - b.left, bh = b.bottom - b.top;
    if (bw <= 0 || bh <= 0) return false;
    int ww = work.right - work.left, wh = work.bottom - work.top;
    if (bh > wh * 2 / 3) return false;
    if (bw > ww * 3 / 4 && bh > wh / 3) return false;   // 又宽又高：多半是整页容器
    RECT tw{};
    if (target && IsWindow(target) && GetWindowRect(target, &tw)) {
        long long ta = (long long)(tw.right - tw.left) * (tw.bottom - tw.top);
        long long ba = (long long)bw * bh;
        bool covers = b.left <= tw.left + 2 && b.top <= tw.top + 2 &&
                      b.right >= tw.right - 2 && b.bottom >= tw.bottom - 2;
        if (covers || (ta > 0 && ba * 10 >= ta * 6)) return false;   // 就是整窗本身（6 成面积起算）
    }
    return true;
}

// 面板落点：贴着插入符弹出，并且尽量不压住输入框。
// 只算「下方放不下就翻到上方」是不够的：面板高 620、屏幕 1440，输入框在屏幕中部时
// 下方放不下面板，翻到上方又会被工作区上边界钳回来，钳完正好压在输入框上。
// 这里把所有候选位置都算出来打分：不遮挡 > 完整落在屏幕内 > 遮挡面积小 > 依次偏好
// （下方、上方、右侧、左侧、贴底、贴顶）。夹取到工作区之后才评分，所以钳取带来的
// 遮挡也会被计入。
static POINT PlacePanel(const RECT& work, int w, int h, const RECT& caret, const RECT* avoid) {
    const int m = S(8), gap = S(8);
    const RECT& av = avoid ? *avoid : caret;
    struct Cand { int x, y; bool fits; long long ov; };
    Cand best{work.left, work.top, false, 0};
    bool have = false;

    auto consider = [&](int x, int y) {
        bool fits = x >= work.left + m && x + w <= work.right - m &&
                    y >= work.top + m && y + h <= work.bottom - m;
        int cx = x, cy = y;
        if (cx > work.right - m - w) cx = work.right - m - w;
        if (cy > work.bottom - m - h) cy = work.bottom - m - h;
        if (cx < work.left + m) cx = work.left + m;
        if (cy < work.top + m) cy = work.top + m;
        int ox = (std::min)((int)av.right, cx + w) - (std::max)((int)av.left, cx);
        int oy = (std::min)((int)av.bottom, cy + h) - (std::max)((int)av.top, cy);
        long long ov = (ox > 0 && oy > 0) ? (long long)ox * oy : 0;
        Cand c{cx, cy, fits, ov};
        auto better = [](const Cand& a, const Cand& b) {
            if ((a.ov > 0) != (b.ov > 0)) return a.ov == 0;      // 不遮挡优先
            if (a.fits != b.fits) return a.fits;                 // 完整在屏幕内优先
            return a.ov < b.ov;                                  // 遮挡面积小优先
        };
        if (!have || better(c, best)) { best = c; have = true; }
    };

    int x0 = caret.left - S(2);
    consider(x0, av.bottom + gap);                  // 下方（默认）
    consider(x0, av.top - gap - h);                 // 上方
    consider(av.right + gap, caret.top - S(30));    // 右侧
    consider(av.left - gap - w, caret.top - S(30)); // 左侧
    consider(x0, work.bottom - m - h);              // 极限贴底：重叠最小
    consider(x0, work.top + m);                     // 极限贴顶
    return POINT{best.x, best.y};
}

// 首次探测不到插入符时的补定位：面板已经显示出来了，这里只挪位置，不碰前台。
static void MovePanelToCaret(const CaretInfo& ci) {
    if (!g_panel || !IsWindow(g_panel) || !g_panelW || !g_panelH) return;
    HMONITOR mon = MonitorFromPoint(POINT{ci.caret.left, ci.caret.top}, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{}; mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return;
    const RECT* avoid = (ci.hasBox && BoxSaneRect(ci.box, mi.rcWork, g_target)) ? &ci.box : nullptr;
    POINT p = PlacePanel(mi.rcWork, g_panelW, g_panelH, ci.caret, avoid);
    SetWindowPos(g_panel, HWND_TOPMOST, p.x, p.y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    Trace("  reposition -> %d,%d (caret %ld,%ld)", p.x, p.y, ci.caret.left, ci.caret.top);
}
static void RoundCorners(HWND hwnd) {
    RECT rc; GetClientRect(hwnd, &rc);
    HRGN rgn = CreateRoundRectRgn(0, 0, rc.right + 1, rc.bottom + 1, S(RADIUS), S(RADIUS));
    SetWindowRgn(hwnd, rgn, TRUE);
}

static void HidePanel() {
    Trace("HidePanel() from %p fg=%p", __builtin_return_address(0), (void*)GetForegroundWindow());
    bool noRestore = g_hideNoRestore;
    g_hideNoRestore = false;
    if (g_panel && IsWindowVisible(g_panel)) {
        // 只有输入态（用户点过搜索框）面板才持有前台，那时收起必须把焦点还回原窗口，
        // 否则前台会停在一个不可见的窗口上：原窗口失焦、键盘输入毫无反应。
        // 非激活态下我们从没抢过焦点，什么都不用还。
        bool hadFocus = (GetForegroundWindow() == g_panel);
        if (g_dragging) { g_dragging = false; ReleaseCapture(); }
        if (g_resizing) { g_resizing = RS_NONE; ReleaseCapture(); }
        KillTimer(g_panel, TIMER_CARET);
        KillTimer(g_panel, TIMER_FOCUS);
        KillTimer(g_panel, TIMER_EMOJI);
        KillTimer(g_panel, TIMER_REPROBE);
        KillTimer(g_panel, TIMER_DEFERHIDE);
        g_reprobeLeft = 0;
        ShowWindow(g_panel, SW_HIDE);
        // 面板收起后残留的「已吞 down」标记会让下一次显示吞掉一个孤立的 keyup。
        for (bool& b : g_swallowed) b = false;
        g_query.clear(); g_comp.clear();
        g_menuOpen = false;
        g_modalOpen = false;
        g_toastOn = false;
        g_pinned = false;        // 临时固定只在本次显示期间有效，关闭即取消
        g_inputMode = false;
        // 借用中的剪贴板要还回去（点完表情立刻关面板、没等到还原定时器的情况）
        if (g_borrowedClip) { KillTimer(g_main, TIMER_CLIPRESTORE); RestoreClipboard(); }
        BuildView();
        if (hadFocus && !noRestore && g_target && g_target != g_panel && IsWindow(g_target)) {
            SetForegroundWindow(g_target);
            RestoreTargetFocus(g_target, g_targetFocus);
        }
    }
}

// ---- 诊断日志（默认关闭，声明见文件头部）----
static void ForceForeground(HWND hwnd);

// 面板该不该继续留在屏幕上。取代了早期那套「失焦就抢回前台」的抢救逻辑——
// 那套是「面板抢前台」方案下的补丁，现在根本不需要抢，判据也就直白得多：
//   · 非激活态：面板从不改变前台，所以「前台仍是唤起时的目标窗口」就是正常状态；
//     前台换成了别的窗口 = 用户主动切走 = 收起。
//   · 输入态（点过搜索框）：前台就是面板自己，同样正常。
// 这也解释了为什么不再需要 FocusStillOurs：那条路会让窗口重新激活，
// 之后再也收不到失焦事件，面板会永久挂最上层（早期踩过的坑）。
static bool PanelShouldStay(HWND hwnd) {
    HWND fg = GetForegroundWindow();
    if (fg == hwnd) return true;
    if (!g_target || !IsWindow(g_target)) return true;
    return fg == g_target;
}

static void ForceForeground(HWND hwnd) {
    Trace("  ForceForeground(%p) enter fg=%p", (void*)hwnd, (void*)GetForegroundWindow());
    HWND fg = GetForegroundWindow();
    if (fg == hwnd) { SetFocus(hwnd); return; }
    DWORD fgTid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    DWORD myTid = GetCurrentThreadId();
    if (fgTid && fgTid != myTid) {
        AttachThreadInput(myTid, fgTid, TRUE);
        BringWindowToTop(hwnd);
        SetForegroundWindow(hwnd);
        SetActiveWindow(hwnd);
        SetFocus(hwnd);
        AttachThreadInput(myTid, fgTid, FALSE);
    } else {
        BringWindowToTop(hwnd);
        SetForegroundWindow(hwnd);
        SetActiveWindow(hwnd);
        SetFocus(hwnd);
    }
    if (GetForegroundWindow() != hwnd) {
        INPUT in[2]{};
        in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = VK_MENU;
        in[1].type = INPUT_KEYBOARD; in[1].ki.wVk = VK_MENU;
        in[1].ki.dwFlags = KEYEVENTF_KEYUP;
        MarkMine(in[0]); MarkMine(in[1]);
        SendInput(2, in, sizeof(INPUT));
        SetForegroundWindow(hwnd);
        SetFocus(hwnd);
    }
}

// 目标窗口内部当前有焦点的控件。
static HWND FocusOfWindow(HWND wnd) {
    if (!wnd || !IsWindow(wnd)) return nullptr;
    DWORD tid = GetWindowThreadProcessId(wnd, nullptr);
    if (!tid) return nullptr;
    GUITHREADINFO gi{}; gi.cbSize = sizeof(gi);
    if (!GetGUIThreadInfo(tid, &gi)) return nullptr;
    HWND f = gi.hwndFocus;
    if (!f || !IsWindow(f)) return nullptr;
    return f;
}

// 把控件级焦点还回目标窗口。
// SetForegroundWindow 只还原顶层窗口，窗口内部的焦点由它自己决定要不要恢复：
// 记事本会，资源管理器的地址栏 / 搜索框不会。不还的话，随后 SendInput 出去的那下
// Ctrl+V 会打在一个内部无焦点的窗口上——用户看到的就是「面板里选了条目，输入框没反应」。
static void RestoreTargetFocus(HWND target, HWND focus) {
    if (!target || !IsWindow(target) || !focus || !IsWindow(focus)) return;
    if (target == g_panel) return;
    DWORD tid = GetWindowThreadProcessId(target, nullptr);
    DWORD myTid = GetCurrentThreadId();
    if (!tid) return;
    if (tid == myTid) { SetFocus(focus); return; }
    AttachThreadInput(myTid, tid, TRUE);
    SetFocus(focus);
    AttachThreadInput(myTid, tid, FALSE);
    Trace("  RestoreTargetFocus: focus=%p -> %p", (void*)focus, (void*)GetFocus());
}

static void ShowPanel() {
    Trace("=== ShowPanel enter fg=%p vis=%d", (void*)GetForegroundWindow(), g_panel ? (int)IsWindowVisible(g_panel) : -1);
    g_query.clear(); g_comp.clear();
    g_sel = 0; g_top = 0;
    g_hoverRow = g_hoverChip = g_hoverCell = -1;
    g_gridScroll = 0;
    g_menuOpen = false;
    g_modalOpen = false;
    g_drawerOpen = false;      // 抽屉不跨次遗留：下次打开还是干净的分类条
    g_drawerHover = -1;
    g_drawerScroll = 0;
    g_caretOn = true;
    g_pinned = false;        // 每次打开都从未固定状态开始
    g_inputMode = false;     // 从非激活态开始：不碰焦点，目标窗口光标照常闪

    CaptureClipboard();

    HWND fg = GetForegroundWindow();
    if (fg && fg != g_panel) g_target = fg;
    // 必须趁目标窗口还持有焦点时读：抢完前台，它内部的焦点控件已经被清掉了。
    g_targetFocus = FocusOfWindow(g_target);

    // 位置：优先贴着目标窗口的文本插入符弹出；拿不到插入符（桌面、文件列表等
    // 没有文本输入区的地方）就居中于鼠标所在显示器。
    POINT pt; GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);

    CaretInfo ci;
    bool hasCaret = ProbeCaret(g_target, &ci);
    if (hasCaret) {
        POINT c{ci.caret.left, ci.caret.top};
        mon = MonitorFromPoint(c, MONITOR_DEFAULTTONEAREST);   // 光标可能跨屏
    }
    Trace("  place: target=%p hasCaret=%d", (void*)g_target, (int)hasCaret);

    MONITORINFO mi{}; mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    int workW = mi.rcWork.right - mi.rcWork.left;
    int workH = mi.rcWork.bottom - mi.rcWork.top;

    // 尺寸：用上次调好的（没有就用默认），夹在「最小尺寸」与当前工作区之间
    int w = g_savedW ? g_savedW : S(P_W);
    int h = g_savedH ? g_savedH : S(P_H);
    if (w < S(MIN_W)) w = S(MIN_W);
    if (h < S(MIN_H)) h = S(MIN_H);
    if (w > workW - S(16)) w = workW - S(16);
    if (h > workH - S(24)) h = workH - S(24);
    g_panelW = w;
    g_panelH = h;

    BuildView();
    ComputeLayout();

    g_posUserSet = false;
    int x = 0, y = 0;
    if (hasCaret) {
        const RECT* avoid = (ci.hasBox && BoxSaneRect(ci.box, mi.rcWork, g_target)) ? &ci.box : nullptr;
        POINT p = PlacePanel(mi.rcWork, w, h, ci.caret, avoid);
        x = p.x; y = p.y;
    } else {
        x = mi.rcWork.left + (w < workW ? (workW - w) / 2 : 0);
        y = mi.rcWork.top + (h < workH ? (workH - h) / 2 : 0);
    }

    int minX = mi.rcWork.left + S(8), minY = mi.rcWork.top + S(8);
    int maxX = mi.rcWork.right - S(8) - w, maxY = mi.rcWork.bottom - S(8) - h;
    if (x > maxX) x = maxX;
    if (y > maxY) y = maxY;
    if (x < minX) x = minX;
    if (y < minY) y = minY;

    // SWP_NOACTIVATE 是这套方案的关键：只把窗口摆上去并显示，不动前台。
    // 目标窗口因此一直持有键盘焦点，插入符一直闪——系统 Win+V 面板就是这么做的。
    // 面板要用到的按键由低级键盘钩子转发进来（见 LLKeyboard）。
    SetWindowPos(g_panel, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    Trace("  place x=%d y=%d w=%d h=%d caret=%ld,%ld avoid=%d", x, y, w, h, ci.caret.left, ci.caret.top,
          (int)(hasCaret && ci.hasBox && BoxSaneRect(ci.box, mi.rcWork, g_target)));
    Trace("  after SWP_SHOWWINDOW fg=%p vis=%d", (void*)GetForegroundWindow(), (int)IsWindowVisible(g_panel));
    RoundCorners(g_panel);
    SetTimer(g_panel, TIMER_CARET, 530, nullptr);
    SetTimer(g_panel, TIMER_FOCUS, 120, nullptr);
    // 缺插入符、或只拿到「框内近似」时补探：Chromium / Electron 的可访问性树是懒建的，
    // 刚被唤醒的头一次询问经常只回整框或退化矩形（网页输入框就是这样时有时无）。
    // 面板先按近似/居中显示，随后补探成功就挪到光标旁边——只挪位置，不碰前台。
    if ((!hasCaret || ci.approx) && g_target && IsWindow(g_target) && g_target != g_panel &&
        g_target != g_main) {
        g_reprobeLeft = 2;
        SetTimer(g_panel, TIMER_REPROBE, 140, nullptr);
    }
    Redraw();
}

// ================================================================== 键盘钩子
// 面板是非激活窗口（WS_EX_NOACTIVATE），永远收不到 WM_KEYDOWN / WM_CHAR，
// 所以它需要的按键全部在这里截下来、直接喂给面板的窗口过程。这是系统 Win+V
// 面板同款结构：焦点留在目标窗口，键盘输入由低级钩子旁路转发。
//
// 为什么不干脆让面板拿到焦点？因为「键盘焦点」是单一资源：面板一 SetFocus，
// 目标窗口立刻收到 WM_KILLFOCUS 并在自己的处理里 DestroyCaret，
// 记事本里的光标就没了。这不是能靠事后补丁补回来的（早期版本试过用替身窗口
// 在插入符原位画一根假光标，兼容性一塌糊涂）。正解是根本不碰焦点。
static wchar_t VkToChar(DWORD vk, DWORD scan) {
    // 只关心不带修饰键的可见字符。Shift 用自跟踪的状态，不能用 GetAsyncKeyState：
    // 钩子吞键之后系统的键盘状态不会更新（见 g_ctrlDown 的注释）。
    BYTE ks[256] = {};
    if (g_shiftDown) {
        ks[VK_SHIFT] = 0x80;
        ks[VK_LSHIFT] = 0x80;
    }
    ks[VK_CAPITAL] = (BYTE)(GetKeyState(VK_CAPITAL) & 1);
    wchar_t buf[8] = {};
    int n = ToUnicodeEx(vk, scan, ks, buf, 8, 0, GetKeyboardLayout(0));
    return (n == 1) ? buf[0] : 0;      // n<0：死键，n>1：多字符序列，都不处理
}

// 非激活态下这个裸键要不要从系统手里接管。
//
// 判据必须收得窄，这是硬约束：WH_KEYBOARD_LL 是一条串行链，回调里 return 1
// 不只挡住目标窗口，还会让排在后面的**所有**低级键盘钩子——别的软件的全局
// 热键、输入法、AutoHotkey / Quicker 这类按键工具——根本收不到这一次按键。
// 早期版本「面板可见就接管全部非修饰键」，于是面板一弹出来，别家的键盘快捷键
// 就全哑了。现在只接面板确实会消费的键：
//   · Esc（唯一的键盘关闭键）、Tab（切页签）；
//   · 列表页的方向/翻页/删除、以及确认键；
//   · 能打出可见字符的键（非激活态直接打字进搜索框，这是核心体验）。
// 其余一律放行：功能键、媒体键、PrintScreen、Home/End/Insert、面板不处理的
// 导航键。带 Ctrl 的组合也整个放行——Ctrl+S / Ctrl+F / Ctrl+C 属于前台程序，
// 面板不该抢（代价是非激活态的 Ctrl+1..4 / P / D / S 快捷键不再生效，只在
// 点过搜索框的输入态可用）。
static bool PanelConsumesBareKey(DWORD vk, wchar_t ch) {
    if (vk == VK_ESCAPE) return true;
    if (vk == VK_TAB)    return true;
    if (g_modalOpen || g_menuOpen)
        return vk == VK_RETURN || vk == VK_UP || vk == VK_DOWN;
    if (vk == VK_RETURN) return g_page == PG_HISTORY || g_page == PG_FAV;
    if (vk == VK_BACK)   return !g_query.empty();
    if (vk == VK_UP || vk == VK_DOWN || vk == VK_PRIOR || vk == VK_NEXT || vk == VK_DELETE)
        return g_page == PG_HISTORY || g_page == PG_FAV;
    return ch != 0;
}

static LRESULT CALLBACK LLKeyboard(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        auto* k = (const KBDLLHOOKSTRUCT*)lp;
        DWORD vk = k->vkCode;
        bool down = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);
        bool up   = (wp == WM_KEYUP || wp == WM_SYSKEYUP);

        if (vk == VK_LWIN) g_lwin = down;
        if (vk == VK_RWIN) g_rwin = down;
        if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL) g_ctrlDown = down;
        if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) g_shiftDown = down;
        // 松开 Win 一并复位 V 的按下状态：万一某次 V 的 keyup 没被收到（UAC 安全桌面等），
        // 下一轮不会因为 g_vDown 卡在 true 而把正常按键当成重复键吞掉。
        if (up && (vk == VK_LWIN || vk == VK_RWIN)) g_vDown = false;

        if (down && vk == 'V' && (g_lwin || g_rwin)) {
            g_swallowVUp = true;
            // 长按 V 时系统每 ~30ms 重复送一次 keydown，而这个动作是 toggle（显示/隐藏）。
            // 不识别重复键的话，长按就变成 Hide/Show 高速交替，视觉上疯狂闪烁。
            // 只有「上一次 keyup 之后的第一次按下」才切换面板。
            // 时间窗 700ms > 系统最慢的重复速率(约 400ms/次)，同时兜住 keyup 丢失把 g_vDown 卡死的情况。
            DWORD now = GetTickCount();
            bool repeat = g_vDown && (now - g_vDownAt) <= 700;
            g_vDown = true;
            g_vDownAt = now;
            if (!repeat) {
                Trace("hook: downV -> toggle (fg=%p vis=%d)", (void*)GetForegroundWindow(), g_panel ? (int)IsWindowVisible(g_panel) : -1);
                if (g_panel && IsWindowVisible(g_panel)) HidePanel();
                else ShowPanel();
            }
            // 注入无害键：避免 Win 抬起时弹开始菜单，同时让 Win 抬起正常送达
            INPUT in[2]{};
            in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = VK_NONAME;
            in[1].type = INPUT_KEYBOARD; in[1].ki.wVk = VK_NONAME;
            in[1].ki.dwFlags = KEYEVENTF_KEYUP;
            MarkMine(in[0]); MarkMine(in[1]);
            SendInput(2, in, sizeof(INPUT));
            return 1;
        }
        if (up && vk == 'V') {
            g_vDown = false;
            if (g_swallowVUp) {
                g_swallowVUp = false;
                return 1;
            }
        }

        // ---- 面板打开期间的按键转发 ----
        // 命中条件：面板可见、前台不是面板自己（输入态下按键本来就直达面板）、
        // 并且这个键面板确实要消费（见 PanelConsumesBareKey，判据必须窄——
        // 多吞一个键，别家软件的全局热键就少收一个键）。
        // 修饰键一律放行，两个原因缺一不可：
        //   1) Win / Alt 组合要交给系统（Win+D、Alt+Tab、Alt+F4）——判据看自跟踪的
        //      g_lwin/g_rwin 与 LLKHF_ALTDOWN；
        //   2) **吞掉修饰键会让系统键盘状态卡死**。早期版本连 Win 的 keyup 一起吞了，
        //      系统以为 Win 一直按着，之后注入的 Ctrl+V 被解释成 Win+Ctrl+V，
        //      连按 Enter 都变成 Win 组合（实测直接弹出「快速设置」），
        //      而 GetAsyncKeyState(VK_LWIN) 全程返回「按下」。
        // 只放行「本程序自己注入的」按键（见 g_injectMark），其他来源的注入照常转发，
        // 与真人按键一致——自动化测试才能驱动面板。
        bool isModifier = (vk == VK_LWIN || vk == VK_RWIN ||
                           vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
                           vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ||
                           vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU);
        if (g_panel && IsWindowVisible(g_panel) && GetForegroundWindow() != g_panel &&
            k->dwExtraInfo != g_injectMark && !isModifier &&
            !(g_lwin || g_rwin) && !(k->flags & LLKHF_ALTDOWN)) {
            if (up) {
                // 吞了 down 就必须吞同一次按键的 up，目标窗口才收不到半截按键。
                if (g_swallowed[vk & 0xFF]) {
                    g_swallowed[vk & 0xFF] = false;
                    return 1;
                }
            } else if (down) {
                // 带 Ctrl 的组合整个放行：面板不抢前台程序的快捷键，也让排在
                // 钩子链后面的别家钩子能收到（代价见 PanelConsumesBareKey 的注释）。
                wchar_t ch = 0;
                if (!g_ctrlDown) {
                    if ((vk == 0 || vk == VK_PACKET) && (k->flags & LLKHF_INJECTED)) {
                        // KEYEVENTF_UNICODE 的注入事件：vk 被系统填成 VK_PACKET(0xE7)
                        // （个别路径是 0），码元在 scanCode 里。自动化脚本就是这样往
                        // 面板里打字的，真人键盘不会产生 VK_PACKET。
                        ch = (wchar_t)k->scanCode;
                    } else {
                        ch = VkToChar(vk, k->scanCode);
                    }
                }
                if (!g_ctrlDown && PanelConsumesBareKey(vk, ch)) {
                    g_swallowed[vk & 0xFF] = true;
                    // SendMessage 而不是 PostMessage：钩子就跑在本进程 UI 线程上，
                    // 同线程 SendMessage 是直接调用，没有跨线程等待，也就不会拖慢输入。
                    SendMessageW(g_panel, WM_APP_PANELKEY, (WPARAM)vk, MAKELPARAM(0, ch));
                    return 1;
                }
                // 面板不消费的键落到下面 CallNextHookEx：直接放行给目标窗口和
                // 后面的钩子，等于面板没打开时的行为。
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

// 点面板外部即收起。
// 为什么必须有这条：非激活态下面板从不夺取前台，所以「在同一个窗口里点掉面板」
// （最典型的是在浏览器里点正文/地址栏、在记事本里点正文后继续打字）根本不产生
// 前台变化——PanelShouldStay 的轮询看不到任何东西，面板就僵在屏幕上，直到用户
// 切到别的窗口或再按一次热键。系统 Win+V 面板没这问题是因为它自己是激活窗口，
// 一点别处就收到 WA_INACTIVE；我们保持不抢焦点，就得自己盯鼠标。
// 只看「按下的点是否在面板矩形内」，与前台轮询互补；自己注入的鼠标事件跳过。
static LRESULT CALLBACK LLMouse(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && g_panel && IsWindowVisible(g_panel) &&
        !g_pinned && !g_dialogOpen && !g_dragging && !g_resizing) {
        bool down = (wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN || wp == WM_MBUTTONDOWN);
        if (down) {
            const MSLLHOOKSTRUCT* ms = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
            // 注意：不能用 LLMHF_INJECTED 过滤——自动化脚本（SetCursorPos + mouse_event）
            // 的点击也是注入事件，过滤掉就等于「脚本点别处不收起」。只认自己的魔数。
            if (ms && ms->dwExtraInfo != g_injectMark) {
                POINT pt = ms->pt;
                RECT r{};
                if (GetWindowRect(g_panel, &r) &&
                    !(pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom)) {
                    // 本进程自己的窗口（另存为对话框、消息框）不算「外部」，否则一开
                    // 对话框面板就被自己收掉了。
                    HWND at = WindowFromPoint(pt);
                    DWORD pid = 0;
                    if (at) GetWindowThreadProcessId(at, &pid);
                    if (!at || pid != GetCurrentProcessId()) {
                        HWND root = at ? GetAncestor(at, GA_ROOT) : nullptr;
                        HWND tgtRoot = (g_target && IsWindow(g_target))
                                       ? GetAncestor(g_target, GA_ROOT) : g_target;
                        bool other = (root && tgtRoot && root != tgtRoot);
                        // 输入态下面板自己是前台：这时点去别的窗口，收起必须让位给
                        // 那次点击的激活——我们先隐藏的话，系统会把前台随手交给
                        // Z 序下一个窗口（实测交给上一个记事本），用户点的窗口反而
                        // 抢不到前台。所以这种情形交给前台变化轮询去收（120ms 内），
                        // HidePanel 那时看到前台已不是自己，自然不会去抢。
                        if (other && GetForegroundWindow() == g_panel) {
                            Trace("hook: mouse down on other window (pt=%ld,%ld) -> defer to poll", pt.x, pt.y);
                            return CallNextHookEx(g_mouseHook, code, wp, lp);
                        }
                        Trace("hook: mouse down outside panel (pt=%ld,%ld) root=%p -> hide",
                              pt.x, pt.y, (void*)root);
                        // 不吞这次点击：用户点哪儿就是要操作哪儿，面板只是让开。
                        // 钩子跑在 UI 线程上，PostMessage 到同线程窗口不会重入。
                        // 点的不是唤起的那个窗口（或点空）时不许抢前台，见 g_hideNoRestore；
                        // 这里把点击处的顶层窗口带过去，由它和目标窗口比对。
                        PostMessageW(g_panel, WM_APP_HIDE_OUTSIDE, (WPARAM)root, 0);
                    }
                }
            }
        }
    }
    return CallNextHookEx(g_mouseHook, code, wp, lp);
}

// ================================================================== 托盘
static void AddTray() {
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_main;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy(g_nid.szTip, L"WinVClip —— Win+V 自定义剪贴板");
    g_tray = Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE;
}

static void RemoveTray() {
    if (g_tray) { Shell_NotifyIconW(NIM_DELETE, &g_nid); g_tray = false; }
}

// ================================================================== 主窗口
static LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_APP_HOTKEY:
        if (IsWindowVisible(g_panel)) HidePanel();
        else ShowPanel();
        return 0;

    case WM_TIMER:
        if (wp == TIMER_WARM) {
            KillTimer(hwnd, TIMER_WARM);
            g_emoCacheTried = true;
            StartEmojiWarm();
            if (g_panel) SetTimer(g_panel, TIMER_EMOJI, 8, nullptr);
        } else if (wp == TIMER_CLIP) {
            KillTimer(hwnd, TIMER_CLIP);
            CaptureClipboard();
            BuildView();
            ComputeLayout();
            if (g_panel && IsWindowVisible(g_panel)) Redraw();
        } else if (wp == TIMER_PASTE) {
            KillTimer(hwnd, TIMER_PASTE);
            HWND target = g_pasteTarget;
            g_pasteTarget = nullptr;
            std::wstring pending = g_typePending;
            g_typePending.clear();
            if (target && IsWindow(target)) {
                // 非激活态下目标窗口一直持有焦点，这里一般什么都不用做。
                // 只有用户点过搜索框（输入态）时才需要把前台交还回去。
                if (GetForegroundWindow() != target) {
                    ForceForeground(target);
                    if (GetForegroundWindow() != target) {
                        Toast(L"目标窗口没有获得焦点，已取消");
                        Redraw();
                        return 0;
                    }
                    // 顶层窗口回来了还不够，地址栏/搜索框那类内部焦点得手动还回去
                    RestoreTargetFocus(target, g_targetFocus);
                }
                if (!pending.empty()) {
                    TypeUnicode(pending);            // 表情/符号：直插，不动剪贴板
                } else {
                    INPUT in[4]{};
                    in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = VK_CONTROL;
                    in[1].type = INPUT_KEYBOARD; in[1].ki.wVk = 'V';
                    in[2].type = INPUT_KEYBOARD; in[2].ki.wVk = 'V'; in[2].ki.dwFlags = KEYEVENTF_KEYUP;
                    in[3].type = INPUT_KEYBOARD; in[3].ki.wVk = VK_CONTROL; in[3].ki.dwFlags = KEYEVENTF_KEYUP;
                    for (auto& x : in) MarkMine(x);
                    SendInput(4, in, sizeof(INPUT));
                    // 借来的剪贴板要还：给目标一点时间真正读出剪贴板内容再还原，
                    // 太早还原会让部分程序（异步读剪贴板的）粘到旧内容。
                    if (g_borrowedClip) SetTimer(g_main, TIMER_CLIPRESTORE, 600, nullptr);
                }
                g_inputMode = false;   // 注入完回到非激活态，焦点继续留在目标窗口
            }
        } else if (wp == TIMER_CLIPRESTORE) {
            KillTimer(hwnd, TIMER_CLIPRESTORE);
            RestoreClipboard();
        }
        return 0;

    case WM_CLIPBOARDUPDATE:
        OnClipboardChanged();
        return 0;
    case WM_APP_TRAY:
        if (LOWORD(lp) == WM_LBUTTONUP) {
            if (IsWindowVisible(g_panel)) HidePanel(); else ShowPanel();
        } else if (LOWORD(lp) == WM_RBUTTONUP) {
            POINT pt; GetCursorPos(&pt);
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING, 1, L"打开面板");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(m, MF_STRING, 2, L"退出");
            SetForegroundWindow(hwnd);
            int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                          pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(m);
            if (cmd == 1) ShowPanel();
            else if (cmd == 2) DestroyWindow(hwnd);
        }
        return 0;

    case WM_DESTROY:
        RemoveTray();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ================================================================== 静态数据
// 字形库在启动时只解析分类表（几十行），条目索引按分类惰性建立。
// 默认分类沿用参考页面：符号停在「常用」（quick 大类），表情停在「情绪」。
static void InitData() {
    GlyphParseTable(glyphdata::kSymBlob, true);
    GlyphParseTable(glyphdata::kEmoBlob, false);

    g_symCat = GlyphCatIndexByKey(PG_SYMBOL, "quick");   // 「常用」大类（原 hot 精选）
    if (g_symCat <= 0) g_symCat = g_symCats.empty() ? 0 : 1;
    g_emojiCat = GlyphCatIndexByKey(PG_EMOJI, "smileys");
    if (g_emojiCat <= 0) g_emojiCat = g_emoCats.empty() ? 0 : 1;

}

// ================================================================== 入口
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR cmdLine, int) {
    HANDLE mtx = CreateMutexW(nullptr, TRUE, L"WinVClip_SingleInstance");
    if (mtx && GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    SetProcessDPIAware();
    PerfInit();
    {
        HDC dc = GetDC(nullptr);
        g_dpi = GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(nullptr, dc);
    }
    g_ref = GetDC(nullptr);
    g_cfPng = RegisterClipboardFormatW(L"PNG");
    LoadViewSize();       // 上次调整好的面板尺寸
    InitData();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    InitD2D();
    MakeFonts();          // 必须在 InitD2D 之后：字体要同时注册到 DirectWrite

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpfnWndProc = MainProc;
    wc.lpszClassName = MAIN_CLASS;
    RegisterClassExW(&wc);

    wc.lpfnWndProc = PanelProc;
    wc.lpszClassName = PANEL_CLASS;
    wc.style = CS_DROPSHADOW;
    RegisterClassExW(&wc);

    g_main = CreateWindowExW(WS_EX_TOOLWINDOW, MAIN_CLASS, L"WinVClip", WS_POPUP, 0, 0, 0, 0,
                             nullptr, nullptr, hInst, nullptr);
    if (!g_main) return 1;

    // WS_EX_NOACTIVATE 是「不抢焦点」方案的地基：面板从不夺取前台，
    // 目标窗口的键盘焦点和文本插入符因此一直保留，光标照常闪烁。
    g_panel = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, PANEL_CLASS,
                              L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, hInst, nullptr);
    if (!g_panel) return 1;

    AddClipboardFormatListener(g_main);

    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, LLKeyboard, GetModuleHandleW(nullptr), 0);
    if (!g_hook) {
        MessageBoxW(nullptr, L"安装键盘钩子失败（可能被安全软件拦截），程序退出。",
                    L"WinVClip", MB_ICONERROR);
        return 1;
    }
    // 鼠标钩子只用来「点外面就收起」，装不上不影响主功能（面板仍能靠前台轮询，
    // 只是同窗口内点击收不起来），所以失败不报错。
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, LLMouse, GetModuleHandleW(nullptr), 0);

    AddTray();
    SetTimer(g_main, TIMER_WARM, 120, nullptr);   // 启动后就把字形图集准备好
    PostMessageW(g_main, WM_CLIPBOARDUPDATE, 0, 0);

    if (cmdLine && wcsstr(cmdLine, L"--show")) {
        PostMessageW(g_main, WM_APP_HOTKEY, 0, 0);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    StopEmojiWorker();        // 工作线程必须在 D2D/atlas 释放之前停掉
    ReleaseEmojiCache();
    UnhookWindowsHookEx(g_hook);
    if (g_mouseHook) UnhookWindowsHookEx(g_mouseHook);
    RemoveClipboardFormatListener(g_main);
    HFONT* all[] = {&g_fTitle, &g_fMeta, &g_fTab, &g_fChip, &g_fSearch, &g_fMono,
                    &g_fEmoji, &g_fSym, &g_fKbd, &g_fBig, &g_fToast, &g_fSymName};
    for (auto* f : all) if (*f) DeleteObject(*f);
    for (auto& kv : g_tfmt) if (kv.second) kv.second->Release();
    g_tfmt.clear();
    if (g_ellipsis)    g_ellipsis->Release();
    if (g_roundStroke) g_roundStroke->Release();
    if (g_brush)       g_brush->Release();
    for (auto& p : g_probe) if (p) p->Release();
    if (g_emojiFmt)    g_emojiFmt->Release();
    g_items.clear();          // 缩略图位图必须先于 render target 释放
    if (g_wic)         g_wic->Release();
    if (g_rt)         g_rt->Release();
    if (g_dw)         g_dw->Release();
    if (g_d2d)        g_d2d->Release();
    if (g_ref) ReleaseDC(nullptr, g_ref);
    return 0;
}
