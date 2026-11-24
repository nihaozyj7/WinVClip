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

// 设备是否还活着（闲置回收后为 false，绘制前要靠 EnsureD2D 重建）
bool D2DAlive();

// 保证设备可用：已释放就重建（含字体与 DWrite 格式）。返回是否可用。
bool EnsureD2D();

// 释放全部 D2D/DWrite 设备对象与字体格式。
// 实测一套「D2D 工厂 + DCRenderTarget + 首次绘制」在进程里会独占一块 25~32MB 的
// PAGE_NOCACHE 私有内存池，且只有把工厂一起放掉才会归还；面板隐藏期间没必要留着。
// 重建代价约 60ms（DLL 已加载的前提下），所以只在闲置回收路径上调用。
void ReleaseD2D();

