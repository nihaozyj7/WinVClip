// thumb.h —— 图片条目的缩略图。
//
// 缓存策略写在 thumb.cpp 里：it.thumb 一旦赋值（哪怕是个空 Thumb）就不再重试，
// 否则每帧都会重新解码同一张坏图，滚动直接卡死。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "core/model.h"   // ClipItem / Thumb

// 惰性构建并缓存缩略图；数据不全、非图片或 D2D 未就绪时返回 nullptr
Thumb* GetThumb(ClipItem& it);

// 在 iconBox 里绘制缩略图（圆角与占位图标由调用方决定大小）
bool DrawThumb(HDC dc, ClipItem& it, const RECT& iconBox);
