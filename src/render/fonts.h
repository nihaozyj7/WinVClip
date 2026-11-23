// fonts.h —— 字体与 Direct2D/DWrite 设备相关的初始化。
//
// HFONT 在本项目里是双重身份：既是 GDI 句柄，也是 D2D 绘制时的「字体令牌」
// （text_measure.cpp 用它查对应的 IDWriteTextFormat）。两者必须成对创建，
// 所以字体只在这里产生，别处不要自己 CreateFontW。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <dwrite.h>

// 按 px 与字重创建 DWrite 文本格式（已设好不换行）
IDWriteTextFormat* MkDW(int px, int weight, const wchar_t* face);

// 创建 HFONT 并登记它对应的 DWrite 格式
HFONT MkFont(int px, int weight, const wchar_t* face);

// DPI 变化或启动时重建全部字体（会让字形可画性缓存失效，见 GlyphDrawable）
void MakeFonts();

// 创建 D2D 工厂、DCRenderTarget、椭圆 trimming sign 等一次性设备对象
void InitD2D();

// 覆盖度探测用的 DWrite 字体对象：判定某个字形在指定字体里到底画不画得出来。
// 实现在 glyph 绘制段（它属于「字形可画性」这件事，不属于字体创建）。
IDWriteFont* MakeProbe(const wchar_t* family);
