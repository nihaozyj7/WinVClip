// capture.h —— 监听剪贴板变化并把内容存进历史。
//
// 几个刻意的取舍：
//   · 不立刻读，而是 SetTimer 延迟 160ms：剪贴板在 WM_CLIPBOARDUPDATE 到达时
//     常常还被占用（OpenClipboard 会失败或读到半截数据）；
//   · 「来源」由前台窗口标题猜，猜不出来就记「桌面」——这条信息只是给用户看的，
//     不参与任何逻辑判断，所以宁可粗糙也不要去查进程和文档名；
//   · 重复内容靠逐字节比较去重，不靠哈希（条目数不大，省一次碰撞处理）。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

// WM_CLIPBOARDUPDATE 的入口：只排一个延迟定时器，不在消息里直接读
void OnClipboardChanged();

// 猜当前前台窗口的「来源」（浏览器、资源管理器…），用于条目上的元信息
std::wstring ForegroundSource();

// 真正读取剪贴板并写入历史（由延迟定时器触发）
void CaptureClipboard();
