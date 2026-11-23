// panel.h —— 面板窗口的消息处理与键盘输入。
//
// 这里是唯一知道「坐标落在哪个控件上」的地方：所有命中判定都用
// ui/layout 算出来的同一份 L，所以「看起来在哪」和「点起来在哪」不会分家。
//
// 键盘走两条路进来：
//   · 面板自己持有焦点时（用户点了搜索框），WM_KEYDOWN 直接到 PanelProc；
//   · 面板是非激活态时（常态），低级键盘钩子把键旁路转成 WM_APP_PANELKEY。
// 两条路最后都进 PanelKeyDown，行为不会不一致。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// 面板窗口过程（WinVClipPanelUI 类）
LRESULT CALLBACK PanelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

// 非激活态下的按键处理（由低级钩子转发过来）
void PanelKeyDown(int vk, bool ctrl);

// 字符输入（IME 上屏 / 直接输入的可见字符）
void PanelChar(wchar_t c);

// 这个裸键面板会不会消费。钩子是串行链，吞得越宽，别家的全局热键越哑，
// 所以只接管真正要用的键（见实现里的白名单）。
bool PanelConsumesBareKey(DWORD vk, wchar_t ch);
