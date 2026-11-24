// settings_win.h —— 设置窗口（独立窗口，不再盖在剪贴板面板上）。
//
// 为什么独立成窗口：设置是「慢交互」（点开关、选目录、等 UAC），而剪贴板面板是
// 「快交互」（非激活、失焦即隐藏、按键靠全局钩子转发）。把两者放同一个窗口里，
// 每次改设置都得应付面板那套「失焦就收起」的时序。分开之后：
//   · 设置窗口是普通可激活窗口，标题栏自绘（无系统边框），拖标题行移动、Alt+Tab 找得到；
//   · 面板的规则一条都不改，打开设置时把面板收起即可。
//
// 状态约定：g_settingsOpen 为真 ⇔ g_setWin 存在且可见（见 core/app_state.h）。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// 注册窗口类（wWinMain 里、创建任何窗口之前调用一次）
void RegisterSettingsClass(HINSTANCE hInst);

// 显示设置窗口（已开着就置前并重绘）。g_settingsOpen 由它置真。
void ShowSettingsWindow();

// 关掉设置窗口（销毁）。没有窗口时是空操作。
void CloseSettingsWindow();

// 设置窗口是否开着
bool SettingsWindowVisible();

// 设置窗口过程（WinVClipSettings 类）
LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
