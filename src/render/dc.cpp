// dc.cpp —— 实现见 dc.h。
#include "render/dc.h"

#include <string>

#include "core/app_state.h"     // g_rt / g_brush / g_painting / g_ellipsis
#include "core/text_measure.h"  // Fmt：HFONT -> IDWriteTextFormat
#include "core/util.h"          // S

// ================================================================== 绘制后端（Direct2D）
D2D1_COLOR_F Col(COLORREF c, float a) {
    return D2D1::ColorF(GetRValue(c) / 255.0f, GetGValue(c) / 255.0f,
                        GetBValue(c) / 255.0f, a);
}

void Brush(COLORREF c, float a) {
    if (g_brush) g_brush->SetColor(Col(c, a));
}

// 一次绘制批次：绑定内存 DC，整帧只在 Begin/End 之间发起 D2D 调用
static bool g_dirtyClip = false;
bool g_needBind = true;      // 缓冲换了才需要重新绑定 DC

void PaintBegin(HDC dc, int w, int h, const RECT* dirty) {
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

void PaintEnd() {
    if (!g_painting) return;
    if (g_dirtyClip) { g_rt->PopAxisAlignedClip(); g_dirtyClip = false; }
    g_painting = false;
    g_rt->EndDraw();
}

void FillRound(HDC dc, const RECT& r, int rad, COLORREF fill, COLORREF border, int bw) {
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

void FillBox(HDC dc, const RECT& r, COLORREF c) {
    (void)dc;
    if (!g_rt || !g_painting) return;
    Brush(c);
    g_rt->FillRectangle(D2D1::RectF((float)r.left, (float)r.top,
                                    (float)r.right, (float)r.bottom), g_brush);
}

void HLine(HDC dc, int x1, int x2, int y, COLORREF c) {
    RECT r{x1, y, x2, y + 1};
    FillBox(dc, r, c);
}

void TextOutF(HDC dc, HFONT f, COLORREF c, const std::wstring& s, RECT r, UINT flags) {
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