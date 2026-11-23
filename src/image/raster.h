// raster.h —— 剪贴板原始图像 -> 统一 BGRA 像素块，以及缩略图用的降采样与圆角蒙版。
//
// 这个模块只处理「像素」，不碰剪贴板、不碰窗口，输入输出都是内存数据，
// 所以可以单独跑、也最容易写单元测试。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d2d1.h>

#include <vector>

#include "core/io_png.h"   // Rgba

// PNG（CF_PNG）经 WIC 解码成 BGRA。有 64M 像素上限，挡住解压炸弹。
bool PngToRgba(const std::vector<BYTE>& png, wv::Rgba& out);

// CF_DIB 交 GDI 统一成 32bpp 自上而下（省掉位深/调色板/上下翻转的解析）
bool DibToRgba(const std::vector<BYTE>& dib, wv::Rgba& out);

// 盒式下采样：面积平均，缩略图场景比双线性更干净（不会在细线处振铃）
void BoxDownsample(const wv::Rgba& s, int tw, int th, std::vector<BYTE>& dst);

// 单位方格内圆角覆盖比例，用来给圆角边缘做抗锯齿
float RoundCoverage(float x, float y, float w, float h, float rad);

// 把 BGRA 传成 D2D 位图（调用方负责 Release）
ID2D1Bitmap* MakeCompatBitmap(int w, int h, const std::vector<BYTE>& bgra);

// 只读 PNG 头拿尺寸，不解码整张图（用于拼「1200×800」这类元信息）
void ReadPngSize(const std::vector<BYTE>& b, int& w, int& h);

// 逐字节比较两份位图（剪贴板重复内容去重）
bool SameBits(const std::vector<BYTE>& a, const std::vector<BYTE>& b);
