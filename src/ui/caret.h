// caret.h —— 目标窗口的文本插入符探测。
//
// 两个用途：
//   1. 面板贴着「文本插入符」弹出，而不是跟着鼠标——用户唤起面板时光标往往
//      并不在鼠标指针那儿；没有插入符可用时退回屏幕居中。
//   2. 表情/符号直插之前判断目标窗口到底有没有文本输入区。资源管理器的文件列表、
//      桌面这类地方没有插入符，把字符注入进去什么也插不进，还会触发系统提示音。
//
// 为什么不能只看 GetGUIThreadInfo：
//   Chromium / Electron 的插入符在渲染进程里，hwndCaret 恒为 0，只能走 UIA
//   (TextPattern::GetSelection)。三个必踩的坑写在 UiaCaretRect 里：
//   空文本框回整框、焦点元素 pid 是渲染子进程、焦点落在页面本身时是 Document。
//   所以「读不到插入符」不能推出「目标不能接收文本」，按通道分流（见 paste.h）。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// 插入符探测结果：caret 是插入符本身，box（可选）是它所在的输入控件矩形。
// box 只用来「别挡住输入框」，因此宁可缺也不能错——整窗/整页的矩形会被 BoxSaneRect 否掉。
struct CaretInfo {
    RECT caret{};
    RECT box{};
    bool hasBox = false;
    // 插入符位置是「框内近似」而非真读到的：Chromium 对空文本框只回整框，
    // 这时用框内首行位置顶上，仍值得补探一次更准的（见 UiaCaretRect / TIMER_REPROBE）。
    bool approx = false;
};

// 完整探测：先 GDI 线程信息，失败再走 UIA。返回 false 表示目标没有可用的插入符。
bool ProbeCaret(HWND target, CaretInfo* ci);

// 只读真插入符的屏幕矩形（不做 UIA 回退）
bool CaretRectOf(HWND wnd, RECT* out);

// 焦点元素的控件矩形（UIA），用于「别挡住输入框」
bool FocusBoxOf(HWND wnd, RECT* out);

// 输入控件矩形可不可信。整窗（WPF/WinUI 自绘的焦点控件就是顶层窗口）、
// 浏览器的整页 document 都会给出巨大矩形，拿它避让会把面板推到窗口外去，
// 所以宁缺勿滥：太高、又宽又高、跟目标窗口几乎一样大的，一律不认。
bool BoxSaneRect(const RECT& b, const RECT& work, HWND target);
