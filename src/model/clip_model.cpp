// clip_model.cpp —— 实现见 clip_model.h（那里写了三条不变式）。
#include "model/clip_model.h"

#include "core/app_state.h"    // g_items / g_view / g_sel / g_page / g_filter / g_query
#include "core/text_util.h"    // Lower
#include "glyph/glyph.h"       // GlyphCount / GlyphItems

using namespace wv;

// ================================================================== 视图
bool MatchQuery(const ClipItem& it, const std::wstring& q) {
    if (q.empty()) return true;
    return Lower(it.title).find(q) != std::wstring::npos ||
           Lower(it.source).find(q) != std::wstring::npos ||
           Lower(it.tag).find(q) != std::wstring::npos ||
           Lower(it.text).find(q) != std::wstring::npos;
}

bool MatchFilter(const ClipItem& it) {
    switch (g_filter) {
    case FL_TEXT:  return it.kind == 0;
    case FL_IMAGE: return it.kind == 2;
    case FL_OTHER: return it.kind == 1;
    default: return true;
    }
}

void BuildView() {
    g_view.clear();
    std::wstring q = Lower(g_query);
    std::vector<int> tmp;
    for (int i = 0; i < (int)g_items.size(); ++i) {
        const ClipItem& it = g_items[i];
        if (g_page == PG_FAV && !it.fav) continue;
        if (!MatchFilter(it)) continue;
        if (!MatchQuery(it, q)) continue;
        tmp.push_back(i);
    }
    // 置顶优先（稳定排序：保持原相对顺序）
    for (int i : tmp) if (g_items[i].pinned) g_view.push_back(i);
    for (int i : tmp) if (!g_items[i].pinned) g_view.push_back(i);

    if (g_sel < 0) g_sel = 0;
    if (g_sel >= (int)g_view.size()) g_sel = (int)g_view.size() - 1;
    if (g_sel < 0) g_sel = 0;
}

// 页面上的条目数（用于分类条计数）
int PageCount() {
    if (g_page == PG_EMOJI || g_page == PG_SYMBOL) return GlyphCount(g_page);
    return (int)g_view.size();
}