// fonts.cpp —— 实现见 fonts.h。
#include "render/fonts.h"

#include "core/app_state.h"   // g_dw / g_d2d / g_rt / g_tfmt / g_probe / g_f* 等
#include "core/util.h"        // S
#include "glyph/glyph.h"      // ClearDrawCache

// ================================================================== 字体
IDWriteTextFormat* MkDW(int px, int weight, const wchar_t* face) {
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
HFONT MkFont(int px, int weight, const wchar_t* face) {
    HFONT hf = CreateFontW(-S(px), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET,
                           OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, face);
    IDWriteTextFormat* tf = MkDW(px, weight, face);
    if (hf && tf) g_tfmt[hf] = tf;
    return hf;
}

void MakeFonts() {
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
void InitD2D() {
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

bool D2DAlive() { return g_rt && g_d2d && g_dw && g_brush; }

void ReleaseD2D() {
    // 顺序与进程退出路径一致：先放「依赖工厂的对象」，再放工厂本身。
    // 只放工厂而不放 target 的话，软件设备池不会归还（实测 private 只掉 3MB）。
    for (auto& kv : g_tfmt) if (kv.second) kv.second->Release();
    g_tfmt.clear();
    for (auto& p : g_probe) if (p) { p->Release(); p = nullptr; }
    if (g_emojiFmt)   { g_emojiFmt->Release();   g_emojiFmt = nullptr; }
    if (g_ellipsis)   { g_ellipsis->Release();   g_ellipsis = nullptr; }
    if (g_roundStroke){ g_roundStroke->Release();g_roundStroke = nullptr; }
    if (g_brush)      { g_brush->Release();      g_brush = nullptr; }
    if (g_rt)         { g_rt->Release();         g_rt = nullptr; }
    if (g_dw)         { g_dw->Release();         g_dw = nullptr; }
    if (g_d2d)        { g_d2d->Release();        g_d2d = nullptr; }
    g_painting = false;
}

bool EnsureD2D() {
    if (D2DAlive()) return true;
    InitD2D();
    if (!D2DAlive()) return false;
    MakeFonts();          // 字体格式挂在 DWrite 工厂上，工厂换了必须重建
    return true;
}