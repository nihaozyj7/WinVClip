// 复现 winvclip 的真实渲染结构，定位「当时序里夹了主帧后 emoji 渲染变慢」的原因。
// 用法: bench2.exe [0|1|2|3]
//   0 = 只渲染 emoji（无主帧、无 atlas copy）           —— 基线
//   1 = 渲染 + copy 进 atlas（无主帧）
//   2 = 渲染 + copy + 每帧主帧（63 次 DrawBitmap(atlas)）—— 近似真实程序
//   3 = 同 2，但主帧不画 atlas（只画几何/文本）
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <cstdio>
#include <string>
#include <vector>

static double NowMs() {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static const wchar_t* EMO =
    L"😀😃😄😁😆😅😂🤣🥲😊😇🙂🙃😉😌😍🥰😘😗😙😚😋😛😝😜🤪🤨🧐🤓😎🥸🤩🥳😏😒😞😔😟😕"
    L"🙁😣😖😫😩🥺😢😭😤😠😡🤬🤯😳🥵🥶😱😨😰😥😓🤗🤔🤭🤫🤥😶"
    L"🙄😯😦😧😮😲🥱😴🤤😪😵🤐🥴🤢🤮🤧😷🤒🤕🤕😈👿💩🤡👹👺💀☠️👻👽👾🤖😺😸";

int main(int argc, char** argv) {
    int mode = argc > 1 ? atoi(argv[1]) : 2;
    ID2D1Factory* d2d = nullptr;
    IDWriteFactory* dw = nullptr;
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&dw);

    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
    ID2D1DCRenderTarget* rt = nullptr;
    d2d->CreateDCRenderTarget(&props, &rt);

    HDC screen = GetDC(nullptr);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = 560;
    bmi.bmiHeader.biHeight = -620;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bb = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC bbdc = CreateCompatibleDC(screen);
    SelectObject(bbdc, bb);
    RECT br{0, 0, 560, 620};
    rt->BindDC(bbdc, &br);
    rt->SetDpi(96.0f, 96.0f);

    IDWriteTextFormat* fmt = nullptr;
    dw->CreateTextFormat(L"Segoe UI Emoji", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 21.0f, L"", &fmt);
    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    IDWriteTextFormat* fmt2 = nullptr;
    dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"", &fmt2);

    const int sz = 44, stride = sz + 4, cols = 9;
    D2D1_PIXEL_FORMAT pf = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                             D2D1_ALPHA_MODE_PREMULTIPLIED);
    // scratch：一屏 9 行
    D2D1_SIZE_F wsz = D2D1::SizeF((float)(cols * stride), (float)(9 * stride));
    ID2D1BitmapRenderTarget* scratch = nullptr;
    rt->CreateCompatibleRenderTarget(&wsz, nullptr, &pf,
                                     D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS_NONE, &scratch);
    ID2D1Bitmap* scratchBmp = nullptr;
    scratch->GetBitmap(&scratchBmp);

    ID2D1Bitmap* atlas = nullptr;
    int aCols = 20, side = aCols * stride;
    if (mode != 0)
        rt->CreateBitmap(D2D1::SizeU((UINT32)side, (UINT32)side),
                         D2D1::BitmapProperties(pf, 96.0f, 96.0f), &atlas);

    // 字形表
    std::vector<std::wstring> g;
    for (const wchar_t* p = EMO; *p;) {
        size_t len = 1;
        if (*p >= 0xD800 && *p <= 0xDBFF) len = 2;
        g.emplace_back(p, len);
        p += len;
    }
    std::printf("mode=%d glyphs=%d\n", mode, (int)g.size());

    int slot = 0, next = 0, frames = 30, perFrame = 9;
    double drawMs = 0, copyMs = 0, mainMs = 0;
    ID2D1Bitmap* atlasBmp = atlas;

    // 主帧用到的持久资源
    ID2D1SolidColorBrush* frameBrush = nullptr;
    if (mode >= 2) {
        rt->BeginDraw();
        rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &frameBrush);
        rt->EndDraw();
    }

    for (int f = 0; f < frames; ++f) {
        // ---- 渲染 perFrame 个新字形（一次 Begin/End）----
        double t0 = NowMs();
        scratch->BeginDraw();
        scratch->Clear(D2D1::ColorF(0, 0, 0, 0));
        ID2D1SolidColorBrush* br = nullptr;
        scratch->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &br);
        for (int k = 0; k < perFrame; ++k) {
            const std::wstring& w = g[(size_t)(next + k) % g.size()];
            float x = (float)((k % cols) * stride), y = (float)((k / cols) * stride);
            scratch->DrawTextW(w.c_str(), (UINT32)w.size(), fmt,
                               D2D1::RectF(x, y, x + sz, y + sz), br,
                               D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
        }
        br->Release();
        scratch->EndDraw();
        double t1 = NowMs();
        drawMs += t1 - t0;

        // ---- copy 进 atlas ----
        if (mode != 0 && atlas) {
            for (int k = 0; k < perFrame; ++k) {
                D2D1_POINT_2U dst = D2D1::Point2U(
                    (UINT32)((slot % aCols) * stride), (UINT32)((slot / aCols) * stride));
                D2D1_RECT_U src = D2D1::RectU((UINT32)((k % cols) * stride),
                                              (UINT32)((k / cols) * stride),
                                              (UINT32)((k % cols) * stride + stride),
                                              (UINT32)((k / cols) * stride + stride));
                atlas->CopyFromBitmap(&dst, scratchBmp, &src);
                ++slot;
            }
        }
        double t2 = NowMs();
        copyMs += t2 - t1;
        next += perFrame;

        // ---- 主帧 ----
        if (mode >= 2) {
            double t3 = NowMs();
            rt->BeginDraw();
            rt->SetTransform(D2D1::Matrix3x2F::Identity());
            ID2D1SolidColorBrush* fbr = nullptr;
            rt->CreateSolidColorBrush(D2D1::ColorF(1, 0.99f, 0.97f), &fbr);
            rt->FillRectangle(D2D1::RectF(0, 0, 560, 620), fbr);
            fbr->Release();
            if (mode == 2 && atlasBmp) {
                for (int i = 0; i < 63; ++i) {
                    int s = i % (slot ? slot : 1);
                    float x = (float)((s % aCols) * stride), y = (float)((s / aCols) * stride);
                    D2D1_RECT_F sr = D2D1::RectF(x, y, x + sz, y + sz);
                    float dx = (float)(20 + (i % 9) * 49), dy = (float)(120 + (i / 9) * 49);
                    rt->DrawBitmap(atlasBmp, D2D1::RectF(dx, dy, dx + sz, dy + sz),
                                   1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, &sr);
                }
            }
            // 一些文本与几何，模拟头部/搜索框/分类条
            ID2D1SolidColorBrush* tb = nullptr;
            rt->CreateSolidColorBrush(D2D1::ColorF(0.2f, 0.2f, 0.2f), &tb);
            for (int i = 0; i < 12; ++i) {
                const wchar_t* s = L"分类标签 剪贴板历史";
                rt->DrawTextW(s, (UINT32)wcslen(s), fmt2,
                              D2D1::RectF((float)(10 + i * 40), 90, (float)(60 + i * 40), 110), tb,
                              D2D1_DRAW_TEXT_OPTIONS_NONE);
            }
            tb->Release();
            rt->EndDraw();
            mainMs += NowMs() - t3;
        }
    }

    std::printf("draw=%.2f/格  copy=%.2f/格  main=%.2f/帧\n",
                drawMs / (frames * perFrame), copyMs / (frames * perFrame), mainMs / frames);

    if (frameBrush) frameBrush->Release();
    if (atlas) atlas->Release();
    if (scratchBmp) scratchBmp->Release();
    if (scratch) scratch->Release();
    fmt->Release();
    fmt2->Release();
    rt->Release();
    dw->Release();
    d2d->Release();
    return 0;
}
