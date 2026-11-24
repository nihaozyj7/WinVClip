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

#include "core/model.h"   // ClipItem（DropItemTemp 的参数）

// WM_CLIPBOARDUPDATE 的入口：只排一个延迟定时器，不在消息里直接读
void OnClipboardChanged();

// 猜当前前台窗口的「来源」（浏览器、资源管理器…），用于条目上的元信息
std::wstring ForegroundSource();

// 真正读取剪贴板并写入历史（由延迟定时器触发）。
// 返回 true 表示历史发生了变化——调用方据此决定要不要重建视图/重绘。
bool CaptureClipboard();

// 历史上限（设置里可改）：按当前 g_cfg.historyLimit 裁掉最旧的条目。
// 改设置后立刻调用一次，用户才看得见「500 条变成 200 条」的效果。
void ApplyHistoryLimit();

// 删除一条图片条目的临时副本（条目被删/被顶替时调用，别把像素文件留成垃圾）
void DropItemTemp(const ClipItem& it);

// 剪贴板里现在装的是不是这一条（比文本正文 / 文件路径列表；图片不比较，见实现）。
// 用途只有一个：删除一条时若它还躺在剪贴板里，就把当前序列号记进 g_delClipSeq，
// 让下一次 CaptureClipboard 别再把它捡回来。
bool ClipboardHoldsItem(const ClipItem& it);
