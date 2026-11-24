// paint.h —— 面板的全部绘制。
//
// 这一层是纯读者：只读 app_state 与 layout 的结果，不修改任何状态
// （唯一的例外是 PerfFlush 会清空性能累计计数）。
// 所以「画错了」和「算错了」在这里能立刻分清——渲染不对先看 layout.cpp。
//
// 双缓冲与脏区：PaintPanel 是唯一入口，它负责选缓冲、绑 DC、决定重绘范围；
// 下面的 PaintXxx 都在这一个 batch 里追加 D2D 调用。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>
#include <utility>
#include <vector>

// 整帧绘制入口（WM_PAINT / 主动 Redraw 都走它）
void PaintPanel(HWND hwnd);

// 设置窗口的整帧绘制入口（设置是独立窗口，见 ui/settings_win.h）
void PaintSettingsWindow(HWND hwnd);

// IME 候选窗跟随：中文组合串位置变化时把候选窗挪到组合串下面
void PositionImeCandidate(HWND hwnd);

// 当前视口内可见的 emoji 格子（emoji 图集据此排高优先的光栅化队列）
void CollectEmojiCells(std::vector<std::pair<RECT, int>>& out);

// 把累计的绘制耗时统计落一行到日志（调试用，不影响渲染）
void PerfFlush();
