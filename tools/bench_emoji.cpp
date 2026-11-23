// 彩色 emoji 光栅化耗时基准：对比「逐字形独立 RT」「整屏一次 DrawTextW」
// 「缩小尺寸」三种软件路径，用于决定 winvclip 的 emoji 缓存架构。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <cstdio>
#include <string>
#include <initializer_list>

static double NowMs() {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

// 63 个常见 emoji（都存在于 Segoe UI Emoji）
static const wchar_t* EMO =
    L"😀😃😄😁😆😅😂🤣🥲😊😇🙂🙃😉😌😍🥰😘😗😙😚😋😛😝😜🤪🤨🧐🤓😎🥸🤩🥳😏😒😞😔😟😕"
    L"🙁😣😖😫😩🥺😢😭😤😠😡🤬🤯😳🥵🥶😱😨😰😥😓🤗🤔🤭🤫🤥😶";

// 逐个取码点（处理代理对）
static int CollectCodepoints(const wchar_t* s, std::wstring* out) {
    int n = 0;
    for (const wchar_t* p = s; *p;) {
        wchar_t c = *p;
        if (!c) break;
        out[n] = std::wstring();
        size_t len = 1;
        unsigned cp = (unsigned)c;
        if (c >= 0xD800 && c <= 0xDBFF && p[1]) {
            unsigned lo = (unsigned)p[1];
            if (lo >= 0xDC00 && lo <= 0xDFFF) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); len = 2; }
        }
        out[n].assign(p, len);
        p += len;
        ++n;
        if (n >= 64) break;
    }
    return n;
}

int main() {
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
    bmi.bmiHeader.biWidth = 640;
    bmi.bmiHeader.biHeight = -640;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC dc = CreateCompatibleDC(screen);
    SelectObject(dc, bmp);
    RECT br{0, 0, 640, 640};
    rt->BindDC(dc, &br);
    rt->SetDpi(96.0f, 96.0f);

    std::wstring emo[64];
    int n = CollectCodepoints(EMO, emo);
    std::printf("codepoints=%d\n", n);

    // A: 逐字形独立 RT（当前实现）
    for (int sz : {44, 22}) {
        IDWriteTextFormat* fmt = nullptr;
        dw->CreateTextFormat(L"Segoe UI Emoji", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, (float)(sz / 2), L"", &fmt);
        fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        double t0 = NowMs();
        for (int k = 0; k < n; ++k) {
            ID2D1BitmapRenderTarget* sub = nullptr;
            rt->CreateCompatibleRenderTarget(D2D1::SizeF((float)sz, (float)sz), &sub);
            ID2D1SolidColorBrush* b = nullptr;
            sub->BeginDraw();
            sub->Clear(D2D1::ColorF(0, 0, 0, 0));
            sub->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &b);
            sub->DrawTextW(emo[k].c_str(), (UINT32)emo[k].size(), fmt,
                           D2D1::RectF(0, 0, (float)sz, (float)sz), b,
                           D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
            b->Release();
            sub->EndDraw();
            ID2D1Bitmap* out = nullptr;
            sub->GetBitmap(&out);
            if (out) out->Release();
            sub->Release();
        }
        double ms = NowMs() - t0;
        std::printf("A per-glyph sz=%d: total=%.1f per=%.2f ms\n", sz, ms, ms / n);

        // B: 整屏一次 DrawTextW（9 列，多行），同尺寸格子
        int cols = 9, rows = (n + cols - 1) / cols;
        std::wstring all;
        for (int k = 0; k < n; ++k) {
            all += emo[k];
            if ((k % cols) == cols - 1) all += L'\n';
        }
        double t1 = NowMs();
        ID2D1BitmapRenderTarget* big = nullptr;
        rt->CreateCompatibleRenderTarget(D2D1::SizeF((float)(cols * sz), (float)(rows * sz)), &big);
        ID2D1SolidColorBrush* b2 = nullptr;
        big->BeginDraw();
        big->Clear(D2D1::ColorF(0, 0, 0, 0));
        big->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &b2);
        big->DrawTextW(all.c_str(), (UINT32)all.size(), fmt,
                       D2D1::RectF(0, 0, (float)(cols * sz), (float)(rows * sz)), b2,
                       D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
        b2->Release();
        big->EndDraw();
        ID2D1Bitmap* out2 = nullptr;
        big->GetBitmap(&out2);
        if (out2) out2->Release();
        big->Release();
        double ms2 = NowMs() - t1;
        std::printf("B one-pass sz=%d: total=%.1f per=%.2f ms\n", sz, ms2, ms2 / n);

        // C: 复用同一个 RT（只建一次）
        ID2D1BitmapRenderTarget* re = nullptr;
        rt->CreateCompatibleRenderTarget(D2D1::SizeF((float)sz, (float)sz), &re);
        double t2 = NowMs();
        for (int k = 0; k < n; ++k) {
            ID2D1SolidColorBrush* b = nullptr;
            re->BeginDraw();
            re->Clear(D2D1::ColorF(0, 0, 0, 0));
            re->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &b);
            re->DrawTextW(emo[k].c_str(), (UINT32)emo[k].size(), fmt,
                          D2D1::RectF(0, 0, (float)sz, (float)sz), b,
                          D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
            b->Release();
            re->EndDraw();
            ID2D1Bitmap* o = nullptr;
            re->GetBitmap(&o);
            if (o) o->Release();
        }
        re->Release();
        double ms3 = NowMs() - t2;
        std::printf("C reuse-rt sz=%d: total=%.1f per=%.2f ms\n", sz, ms3, ms3 / n);

        // D: 无彩色字形选项（单色路径），看 COLR 占多少
        double t3 = NowMs();
        for (int k = 0; k < n; ++k) {
            ID2D1BitmapRenderTarget* sub = nullptr;
            rt->CreateCompatibleRenderTarget(D2D1::SizeF((float)sz, (float)sz), &sub);
            ID2D1SolidColorBrush* b = nullptr;
            sub->BeginDraw();
            sub->Clear(D2D1::ColorF(0, 0, 0, 0));
            sub->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &b);
            sub->DrawTextW(emo[k].c_str(), (UINT32)emo[k].size(), fmt,
                           D2D1::RectF(0, 0, (float)sz, (float)sz), b,
                           D2D1_DRAW_TEXT_OPTIONS_NONE);
            b->Release();
            sub->EndDraw();
            ID2D1Bitmap* o = nullptr;
            sub->GetBitmap(&o);
            if (o) o->Release();
            sub->Release();
        }
        double ms4 = NowMs() - t3;
        std::printf("D mono sz=%d: total=%.1f per=%.2f ms\n", sz, ms4, ms4 / n);

        // E: 持久 scratch RT，一个 Begin/End 内画满一整行（9 格）
        {
            int cols2 = 9;
            ID2D1BitmapRenderTarget* line = nullptr;
            rt->CreateCompatibleRenderTarget(D2D1::SizeF((float)(cols2 * sz), (float)sz), &line);
            ID2D1SolidColorBrush* b = nullptr;
            double te = NowMs();
            int lines = n / cols2;
            for (int r = 0; r < lines; ++r) {
                line->BeginDraw();
                line->Clear(D2D1::ColorF(0, 0, 0, 0));
                line->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &b);
                for (int c = 0; c < cols2; ++c) {
                    const std::wstring& g = emo[r * cols2 + c];
                    line->DrawTextW(g.c_str(), (UINT32)g.size(), fmt,
                                    D2D1::RectF((float)(c * sz), 0.0f, (float)((c + 1) * sz), (float)sz), b,
                                    D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
                }
                b->Release();
                line->EndDraw();
            }
            double mse = NowMs() - te;
            std::printf("E line-batch sz=%d: total=%.1f per-line=%.2f per-cell=%.2f ms\n",
                        sz, mse, mse / lines, mse / (lines * cols2));

            // F: 同上，另加 CopyFromBitmap 到 atlas
            ID2D1Bitmap* atlas = nullptr;
            rt->CreateBitmap(D2D1::SizeU(cols2 * sz, sz),
                             D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                                                      D2D1_ALPHA_MODE_PREMULTIPLIED)),
                             &atlas);
            ID2D1Bitmap* sb = nullptr;
            line->GetBitmap(&sb);
            double tf = NowMs();
            for (int r = 0; r < lines; ++r) {
                line->BeginDraw();
                line->Clear(D2D1::ColorF(0, 0, 0, 0));
                b = nullptr;
                line->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &b);
                for (int c = 0; c < cols2; ++c) {
                    const std::wstring& g = emo[r * cols2 + c];
                    line->DrawTextW(g.c_str(), (UINT32)g.size(), fmt,
                                    D2D1::RectF((float)(c * sz), 0.0f, (float)((c + 1) * sz), (float)sz), b,
                                    D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
                }
                b->Release();
                line->EndDraw();
                if (atlas && sb) {
                    D2D1_RECT_U src = D2D1::RectU(0, 0, (UINT32)(cols2 * sz), (UINT32)sz);
                    atlas->CopyFromBitmap(nullptr, sb, &src);
                }
            }
            double msf = NowMs() - tf;
            std::printf("F line+copy sz=%d: total=%.1f per-line=%.2f per-cell=%.2f ms\n",
                        sz, msf, msf / lines, msf / (lines * cols2));
            if (atlas) atlas->Release();
            if (sb) sb->Release();
            line->Release();
        }

        fmt->Release();
    }

    // G: 兼容 RT 的像素格式对渲染成本的影响（实际程序用了 PREMULTIPLIED）
    {
        IDWriteTextFormat* fmt = nullptr;
        dw->CreateTextFormat(L"Segoe UI Emoji", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 21.0f, L"", &fmt);
        fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        struct Case { const char* name; bool premul; bool inherit; int w, h; };
        Case cases[] = {
            {"inherit 44x44", false, true, 44, 44},
            {"premul  44x44", true, false, 44, 44},
            {"premul  48x48", true, false, 48, 48},
            {"premul 432x432", true, false, 432, 432},
            {"inherit 432x432", false, true, 432, 432},
        };
        for (const Case& c : cases) {
            D2D1_PIXEL_FORMAT pf = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                                     D2D1_ALPHA_MODE_PREMULTIPLIED);
            D2D1_SIZE_F wsz = D2D1::SizeF((float)c.w, (float)c.h);
            ID2D1BitmapRenderTarget* sub = nullptr;
            if (c.inherit) rt->CreateCompatibleRenderTarget(&wsz, nullptr, nullptr,
                                                            D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS_NONE, &sub);
            else rt->CreateCompatibleRenderTarget(&wsz, nullptr, &pf,
                                                  D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS_NONE, &sub);
            if (!sub) { std::printf("G %s: create failed\n", c.name); continue; }
            // 预热一次，再测 64 次
            for (int pass = 0; pass < 2; ++pass) {
                double t = NowMs();
                for (int k = 0; k < n; ++k) {
                    ID2D1SolidColorBrush* b = nullptr;
                    sub->BeginDraw();
                    sub->Clear(D2D1::ColorF(0, 0, 0, 0));
                    sub->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
                    sub->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &b);
                    sub->DrawTextW(emo[k].c_str(), (UINT32)emo[k].size(), fmt,
                                   D2D1::RectF(0, 0, 44.0f, 44.0f), b,
                                   D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
                    b->Release();
                    sub->EndDraw();
                }
                double ms = NowMs() - t;
                if (pass == 1) std::printf("G %-16s per=%.2f ms\n", c.name, ms / n);
            }
            sub->Release();
        }
        fmt->Release();
    }

    rt->Release();
    dw->Release();
    d2d->Release();
    return 0;
}
