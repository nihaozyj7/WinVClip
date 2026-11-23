// actions.h —— 用户动作的执行：把「点击/按键」翻译成对模型与剪贴板的操作。
//
// 这一层的每个函数都对应一个明确的用户意图（粘贴、置顶、收藏、另存为、切页…），
// 消息处理（ui/panel）只负责判断「点到了哪里、按了什么键」，
// 具体做什么全在这里，顺序都是：改模型 -> 重建视图 -> 重绘。
//
// 注意：这些函数都会直接改 g_* 状态并自己触发重绘，调用方不要再补一次。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

#include "core/model.h"   // ClipItem / GItem

// 底部悬浮提示（info=true 用蓝色，否则绿色）
void Toast(const std::wstring& msg, bool info = false);

// 切换页面（0..PG_COUNT-1），会重置滚动并重建视图
void SetPage(int p);

// 临时固定：面板暂时不因失焦而收起（再按一次解除）
void ToggleTemporaryPin();

// 粘贴 g_view[vi] 指向的条目
void PasteItem(int vi);

// 把字形（表情/符号）送进目标窗口：有插入符直插，否则借剪贴板
void PasteGlyph(int page, const GItem& it);

// 置顶 / 收藏。参数是不同坐标系的下标：
//   TogglePinItem 收 g_items 的绝对下标（右键菜单用），
//   ToggleFav     收 g_view 的偏移（收藏按钮用）。
void TogglePinItem(int idx);
void ToggleFav(int vi);

// 当前选中的视图下标 / 条目下标（无选中返回 -1）
int SelView();
int SelItem();

// 另存为：弹系统对话框选路径，把条目内容写成文件
void SaveItemAs(int idx);

// 右键菜单项的执行入口（下标见 layout.h 的 MI_*）
void RunMenuItem(int hit);
