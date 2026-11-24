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

// 删除单条历史（row = 相对当前视图顶部的行号）。按设置决定直接删还是先弹确认卡片，
// 三个入口（Del 键、右键菜单、卡片确认）都收敛到这两个函数。
void RequestDeleteRow(int row);
void ConfirmDelete();

// ---------------------------------------------------------------- 设置窗口
// 打开/关闭设置窗口（独立窗口，见 ui/settings_win.h；打开时面板会被收起）
void OpenSettings();
void CloseSettings();
// which: 0 = 开机自启动，1 = 以管理员身份运行，2 = 删除前二次确认
void SettingsToggle(int which);
// mode 见 core/settings.h 的 THEME_*
void SettingsSetTheme(int mode);
// 历史上限按步长增减（delta = ±HIST_STEP）
void SettingsStepHistory(int delta);
// 选临时文件目录（系统文件夹选择器）
void SettingsPickTempDir();

// 清空剪贴板历史（设置窗口里的危险操作：确认卡片也画在设置窗口里）。
// 只删「既不置顶也不收藏」的记录：置顶与收藏是用户显式标记过的数据，不与历史互通
// ——清空不动它们，历史上限裁剪也不动它们（见 clipboard/capture.cpp 的
// ApplyHistoryLimit）。先弹确认卡片，确认后立刻落盘。
void RequestClearHistory();
void ConfirmClearHistory();
// 这次清空会删掉几条（卡片文案要显示具体数字）；置顶/收藏不计入
int  ClearHistoryCount();
