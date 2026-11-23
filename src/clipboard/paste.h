// paste.h —— 把条目内容送回目标窗口的两条通道。
//
// 通道一（直插）：目标有可读插入符时，用 SendInput 注入 Unicode 字符，
//   全程不碰剪贴板，所以用户原来的剪贴板内容不会被改掉。
// 通道二（借剪贴板）：读不到插入符（浏览器渲染进程、资源管理器地址栏这类）时，
//   临时把自己的内容写进剪贴板 + 注入 Ctrl+V，粘完用序列号确认再还原。
//
// 为什么不能只用一条：
//   通道一要求目标能接收逐字符注入，个别程序（部分 Electron、UWP）不接受；
//   通道二会改动用户的剪贴板，必须备份还原，且还原有时序风险
//   （粘完 600ms 内用户自己复制了东西，就不能还原）。
// 两条通道按「插入符是否可读」分流，判断在 ui/caret 里做。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

#include "core/model.h"   // ClipItem

// 把条目写回剪贴板（文本/图片）
void PutToClipboard(const ClipItem& it);

// 逐字符注入 Unicode（KEYEVENTF_UNICODE），每个事件都打上本程序标记
void TypeUnicode(const std::wstring& s);

// 把字符串写进剪贴板（不含图片）
void PutText(const std::wstring& s);

// 通道二的准备与收尾：备份用户剪贴板 -> 写入我们的内容 -> 粘贴 -> 还原
void BorrowClipboard();
void RestoreClipboard();
