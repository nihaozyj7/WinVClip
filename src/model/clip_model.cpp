// clip_model.cpp —— 实现见 clip_model.h（那里写了三条不变式）。
#include "model/clip_model.h"

#include <algorithm>

#include "core/app_state.h"    // g_items / g_view / g_sel / g_page / g_filter / g_query
#include "core/history_store.h" // HistorySearchBodies（正文只在库里的条目）
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
    // 正文不在内存的条目（超长文本，见 core/history_store.h 的内存策略），
    // 内存里比不出结果，收集起来交给数据库一次补查——一次 SQL 循环绑定，
    // 而不是每条查一次。
    std::vector<int> missDbIds, missIdx;
    for (int i = 0; i < (int)g_items.size(); ++i) {
        const ClipItem& it = g_items[i];
        if (g_page == PG_FAV && !it.fav) continue;
        if (!MatchFilter(it)) continue;
        if (MatchQuery(it, q)) {
            tmp.push_back(i);
        } else if (!q.empty() && it.kind == 0 && it.text.empty() && it.dbId > 0) {
            missDbIds.push_back(it.dbId);
            missIdx.push_back(i);
        }
    }
    if (!missDbIds.empty()) {
        std::vector<int> hits;
        HistorySearchBodies(missDbIds, q, hits);
        for (int id : hits) {
            for (size_t k = 0; k < missDbIds.size(); ++k) {
                if (missDbIds[k] == id) { tmp.push_back(missIdx[k]); break; }
            }
        }
        std::sort(tmp.begin(), tmp.end());   // 补查到的条目要回到它原本的时间顺序上
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