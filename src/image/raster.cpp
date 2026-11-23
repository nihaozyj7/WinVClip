// raster.cpp —— 把「剪贴板里的原始图像数据」变成统一的 BGRA 像素块。
//
// 两条入口：
//   · PngToRgba：PNG 交给 WIC 解码（剪贴板里常见 CF_PNG，浏览器截图就是这个）
//   · DibToRgba：CF_DIB 交给 GDI 转 32bpp —— 自己解析各种位深/调色板/行序
//     是纯粹的体力活且容易出错，让 GDI 做一次 StretchBlt 反而更省事
//
// 之后统一下采样到缩略图尺寸，再在 CPU 上做圆角抗锯齿蒙版并预乘 alpha。
// 为什么要预乘：DCRenderTarget 不支持 PushLayer 的几何遮罩，只有预乘之后
// DrawBitmap 的 source-over 才能得到平滑圆角（否则角上是脏的）。
#include "image/raster.h"

#include <cmath>
#include <cstring>

#include "core/io_png.h"       // Rgba / InitWic / g_wic / kWicFmt32BGRA
#include "core/app_state.h"    // g_rt

using namespace wv;

bool PngToRgba(const std::vector<BYTE>& png, Rgba& out) {
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
bool DibToRgba(const std::vector<BYTE>& dib, Rgba& out) {
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
void BoxDownsample(const Rgba& s, int tw, int th, std::vector<BYTE>& dst) {
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
float RoundCoverage(float x, float y, float w, float h, float rad) {
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

ID2D1Bitmap* MakeCompatBitmap(int w, int h, const std::vector<BYTE>& bgra) {
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
void ReadPngSize(const std::vector<BYTE>& b, int& w, int& h) {
    w = h = 0;
    if (b.size() < 24) return;
    if (!(b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G')) return;
    w = (b[16] << 24) | (b[17] << 16) | (b[18] << 8) | b[19];
    h = (b[20] << 24) | (b[21] << 16) | (b[22] << 8) | b[23];
}

bool SameBits(const std::vector<BYTE>& a, const std::vector<BYTE>& b) {
    return a.size() == b.size() && (a.empty() || memcmp(a.data(), b.data(), a.size()) == 0);
}