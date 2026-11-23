// clip_model.h —— 剪贴板历史列表模型：过滤、搜索、排序。
//
// 这个模块只回答一个问题：给定「当前页面 / 筛选 / 搜索词」，
// 列表里应该显示哪些条目、按什么顺序。
//
// 核心不变式（改动这里之前先读）：
//   · g_view 存的是 g_items 的下标，不是条目本身——条目会被删除、置顶会换序，
//     存下标才能保证「按视图位置操作」时不会指到别人的数据上；
//   · g_sel 是 g_view 的偏移，不是 g_items 的下标，两者不能混用；
//   · 任何改动 g_query / g_filter / g_page 的地方，结束后都必须调 BuildView()，
//     并把 g_sel / g_top 夹回合法范围（BuildView 只夹 g_sel，滚动由调用方夹）。
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

// 标题/内容是否包含搜索词（q 已是小写）
bool MatchQuery(const ClipItem& it, const std::wstring& q);

// 是否属于当前筛选（全部/文本/图片/其他）
bool MatchFilter(const ClipItem& it);

// 按当前页面+筛选+搜索词重建 g_view，并把 g_sel 夹回范围
void BuildView();

// 当前页面的条目数（用于分类条上的计数；字形页取字形库总数）
int PageCount();
