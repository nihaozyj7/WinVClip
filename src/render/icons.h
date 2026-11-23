// icons.h —— 线性图标（24×24 设计坐标系，缩放后描边绘制）。
//
// 图标全部是自绘路径而不是资源位图：这样任意 DPI 下线宽都是清晰的，
// 也不会在 exe 里塞一堆 PNG。新增图标只需在 IconId 加一项、
// 在 DrawIcon 的 switch 里补一条 case。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "core/model.h"   // ClipItem

enum IconId { IC_SEARCH, IC_GEAR, IC_X, IC_CLIP, IC_TEXT, IC_CODE, IC_IMAGE,
              IC_FILE, IC_LINK, IC_PIN, IC_STAR, IC_MORE, IC_TRASH, IC_CHECK, IC_COPY,
              IC_SAVE };

// 在 box 内按 24×24 坐标系描边绘制线性图标，stroke 为 24 坐标系下的线宽
void DrawIcon(HDC dc, IconId id, RECT box, COLORREF c, float stroke);

// 按条目类型选图标（文本/代码/图片/文件/链接…）
IconId ItemIcon(const ClipItem& it);
