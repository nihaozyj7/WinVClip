// hook.h —— 全局低级钩子：面板的键盘与鼠标都由这里接管。
//
// 这个文件承载了项目里最难的两组约束，改动前务必先读：
//
// 一、为什么必须用钩子而不是给面板焦点
//   面板是 WS_EX_NOACTIVATE 的非激活窗口，永远收不到 WM_KEYDOWN / WM_CHAR。
//   不能靠「先抢焦点、完事再还」解决：键盘焦点是单一资源，面板一 SetFocus，
//   目标窗口立刻收到 WM_KILLFOCUS 并在自己的处理里 DestroyCaret，
//   记事本里的光标就没了——事后补不回来（试过用替身窗口画假光标，兼容性极差）。
//   所以正解是根本不碰焦点，按键由低级钩子旁路转发。
//
// 二、钩子是串行链，吞得越宽，别家越哑
//   WH_KEYBOARD_LL 的返回值 1 会让排在后面的钩子（AHK、Quicker、输入法热键）
//   收不到这个键。所以只接管面板真会消费的键，Ctrl 组合整段放行。
//
// 鼠标那条钩子只做一件事：判断「按下的点是否在面板矩形外」。
// 非激活面板只靠前台轮询有盲区——用户在同一个窗口里点掉面板时前台纹丝不动。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// 低级键盘钩子：Win+V 热键 + 面板按键转发
LRESULT CALLBACK LLKeyboard(int code, WPARAM wp, LPARAM lp);

// 低级鼠标钩子：按下点在面板外就收起
LRESULT CALLBACK LLMouse(int code, WPARAM wp, LPARAM lp);

// 这个裸键面板会不会消费。白名单在这里，不在调用点——钩子吞错一个键的代价
// 是别家全局热键直接失效，比漏掉一个键严重得多。
bool PanelConsumesBareKey(DWORD vk, wchar_t ch);
