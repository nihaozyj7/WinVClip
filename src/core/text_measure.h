// text_measure.h —— 文字度量。
//
// 布局（layout）和绘制（paint）都要「这段文字有多宽」，而且量的是同一批固定文案
// （页签、chip、页脚、行内大小/时间）。所以这层单独成文并带缓存：
//   · 量宽优先走 DirectWrite，保证与真正渲染的宽度完全一致；
//   · DWrite 未就绪时回退 GDI 的 GetTextExtentPoint32W；
//   · 结果按 (字体, 文本) 缓存，缓存满 600 条整表清空（简单且足够，别做 LRU）。
//
// 依赖方向：只依赖 app_state.h 里的 g_dw / g_ref / g_tfmt，不依赖布局与业务。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

#include <dwrite.h>

// HFONT → DWrite 文排格式（未登记时返回 nullptr，调用方回退 GDI）
IDWriteTextFormat* Fmt(HFONT f);

// 文字像素宽度（已缓存）
int TextW(HFONT f, const std::wstring& s);

// 文字像素宽度（**不进缓存**）。给「一次算几十个中间串」的场景用：
// 每次调用都自己建一次 TextLayout，所以只适合单次会话里少量调用（如截断的二分查找）。
int TextWNoCache(HFONT f, const std::wstring& s);

// 字体环境变化后作废宽度缓存。
void ClearTextWidthCache();
