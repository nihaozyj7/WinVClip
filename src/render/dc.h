// dc.h —— Direct2D 绘制原语。
//
// 全部绘制都在一个 batch 内：PaintBegin 把内存 DC 绑到 DCRenderTarget，
// 中间的 FillRound / TextOutF / DrawIcon 只是往这里追加 D2D 调用，
// PaintEnd 才真正 EndDraw 提交一次。
//
// 为什么不用 GDI：整个界面要抗锯齿圆角、跨字体回退和渐变描边，
// GDI 做圆角要么糊要么锯齿，而 D2D 的 DCRenderTarget 能直接画到
// 我们已经有的后备缓冲上，不需要另开窗口。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d2d1.h>

#include <string>

D2D1_COLOR_F Col(COLORREF c, float a = 1.0f);
void Brush(COLORREF c, float a = 1.0f);

// 后备缓冲换过之后必须重新 BindDC 一次，否则 D2D 还在往旧位图上画
extern bool g_needBind;

// dirty 非空时只裁剪出这块区域提交，配合双缓冲做局部重绘
void PaintBegin(HDC dc, int w, int h, const RECT* dirty = nullptr);
void PaintEnd();

// rad 是圆角「直径」（沿用 GDI RoundRect 的语义），内部折半给 D2D
void FillRound(HDC dc, const RECT& r, int rad, COLORREF fill, COLORREF border, int bw = 1);
void FillBox(HDC dc, const RECT& r, COLORREF c);
void HLine(HDC dc, int x1, int x2, int y, COLORREF c);

// 统一的文字出口：HFONT 只是「字体令牌」，真正的排版走 DWrite（见 text_measure）
void TextOutF(HDC dc, HFONT f, COLORREF c, const std::wstring& s, RECT r, UINT flags);
