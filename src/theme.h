// 视觉常量：配色、尺寸、窗口类名。
//
// 这些值原先散落在 main.cpp 顶部，与绘制代码相隔上千行。单独成文是为了：
//   · 调色/调尺寸只改一个文件，不必在 5000 行里找常量；
//   · 绘制路径（paint）只依赖这里和 state，不依赖具体业务逻辑。
//
// 尺寸是「逻辑像素」，实际落笔前一律经 S(v) = MulDiv(v, dpi, 96) 缩放。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace wv {

// ---------------------------------------------------------------- 窗口类名
inline const wchar_t* const MAIN_CLASS  = L"WinVClipMain";
inline const wchar_t* const PANEL_CLASS = L"WinVClipPanelUI";

// ---------------------------------------------------------------- 主题色
// 原设计稿是 CSS 变量，这里一一对应，注释里保留变量名便于对照。
inline constexpr COLORREF C_CANVAS   = RGB(244, 242, 237);   // --canvas  #F4F2ED
inline constexpr COLORREF C_SURFACE  = RGB(255, 253, 248);   // --surface #FFFDF8
inline constexpr COLORREF C_SUBTLE   = RGB(231, 227, 219);   // --subtle  #E7E3DB
inline constexpr COLORREF C_INK      = RGB(32, 37, 43);      // --ink     #20252B
inline constexpr COLORREF C_MUTED    = RGB(116, 122, 125);   // --muted   #747A7D
inline constexpr COLORREF C_LINE     = RGB(213, 208, 200);   // --line    #D5D0C8
inline constexpr COLORREF C_VOLT     = RGB(255, 107, 53);    // --voltage #FF6B35
inline constexpr COLORREF C_VOLTSOFT = RGB(255, 224, 210);   // --voltage-soft #FFE0D2
inline constexpr COLORREF C_SUCCESS  = RGB(47, 155, 104);    // --success #2F9B68
inline constexpr COLORREF C_DANGER   = RGB(216, 58, 58);     // --danger  #D83A3A
inline constexpr COLORREF C_RINGSOFT = RGB(255, 232, 221);   // 焦点光晕 rgba(255,107,53,.14)

// ---------------------------------------------------------------- 尺寸
inline constexpr int P_W         = 560;
inline constexpr int P_H         = 620;
inline constexpr int HEAD_H      = 34;
inline constexpr int SEARCH_TOP  = 12;
inline constexpr int SEARCH_H    = 40;
inline constexpr int SEARCH_PADX = 14;
inline constexpr int SEARCH_BOT  = 10;
inline constexpr int FILTER_H    = 46;
inline constexpr int FOOT_H      = 38;
inline constexpr int ROW_H       = 56;
inline constexpr int RADIUS      = 12;
// 面板尺寸可调：最小尺寸由「页签 + 三个按钮排得开」和「至少露出两行」定。
// 宽度下限压到 360 后页签放不下自然宽度，ComputeLayout 里会按可用宽度均分。
inline constexpr int MIN_W       = 360;
inline constexpr int MIN_H       = 340;
inline constexpr int RESIZE_EDGE = 5;     // 边缘可抓取的带宽（px）

}  // namespace wv
