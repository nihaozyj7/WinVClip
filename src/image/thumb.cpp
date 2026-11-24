// thumb.cpp —— 图片条目的缩略图（惰性构建 + 缓存 + 绘制）。
//
// 关键约定：it.thumb 一旦赋值就不再重试。失败也保留一个空 Thumb，
// 否则每一帧都会重新解码同一张坏图，滚动时直接卡死。
#include "image/thumb.h"

#include <algorithm>
#include <cmath>

#include "core/app_state.h"    // g_rt / g_brush
#include "core/theme.h"        // C_SUBTLE / C_LINE
#include "core/util.h"         // S
#include "image/raster.h"      // PngToThumb / DibToThumb / MakeCompatBitmap
#include "render/dc.h"         // Brush

using namespace wv;

// ================================================================== 图片缩略图
// 思路：PNG 走 WIC 解码、CF_DIB 交给 GDI 转 32bpp（省掉各种位深/调色板的解析），
// 统一得到「原始尺寸的 BGRA」后自己盒式下采样到 36px，最后在 CPU 上做圆角抗锯齿
// 蒙版并预乘 alpha —— 预乘之后 DrawBitmap 的 source-over 就能得到平滑圆角，
// 不需要 D2D 的 layer（DC render target 不支持 PushLayer 的几何遮罩）。

// Rgba 与 WIC 像素格式 GUID 见 io_png.h

// g_wic / InitWic 见 io_png.cpp

// 惰性构建 + 缓存：it.thumb 一旦赋值就不再重试（失败永远是空位图，不会每帧重解码）
Thumb* GetThumb(ClipItem& it) {
    if (it.thumb) return it.thumb.get();
    if (it.kind != 2 || !g_rt) return nullptr;

    ThumbRef t = std::make_shared<Thumb>();
    it.thumb = t;

    // 先挂上空的 Thumb 再取内容：像素可能要从未命中的临时文件读回，
    // 失败时保持空位图，不会每帧重试一遍。
    if (!EnsureItemBits(it)) return t.get();

    Rgba src;
    // box 尺寸要在解码前就知道：WIC 的 scaler / StretchDIBits 都按它直接出像素，
    // 于是这一条路径上不会出现「整张 6000×4000 的全尺寸 BGRA」那种临时缓冲。
    const int box = S(36);
    bool ok = it.isPng ? PngToThumb(it.bits, box, src) : DibToThumb(it.bits, box, src);
    if (!ok || src.w <= 0 || src.h <= 0) return t.get();

    int tw = src.w, th = src.h;
    std::vector<BYTE> small;
    small.swap(src.px);                              // 已经是缩略图尺寸，不需要再降采样

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
bool DrawThumb(HDC dc, ClipItem& it, const RECT& iconBox) {
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