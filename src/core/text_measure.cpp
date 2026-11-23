// text_measure.cpp —— 文字度量（说明见 text_measure.h）。
#include "core/text_measure.h"

#include <cmath>
#include <unordered_map>

#include "core/app_state.h"

IDWriteTextFormat* Fmt(HFONT f) {
    auto it = g_tfmt.find(f);
    return it == g_tfmt.end() ? nullptr : it->second;
}

// 量宽要建 IDWriteTextLayout（一次 COM 分配 + 字形 shaping），而布局与绘制会
// 对同一批固定文案反复量（页签、chip、页脚、行内大小/时间）。这里按字体分组缓存。
static std::unordered_map<HFONT, std::unordered_map<std::wstring, int>> g_twCache;
static int g_twCacheN = 0;

static int TextWUncached(HFONT f, const std::wstring& s) {
    IDWriteTextFormat* fmt = Fmt(f);
    if (fmt && g_dw) {
        IDWriteTextLayout* lay = nullptr;
        if (SUCCEEDED(g_dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), fmt,
                                             4096.0f, 512.0f, &lay)) && lay) {
            DWRITE_TEXT_METRICS m{};
            float w = 0;
            if (SUCCEEDED(lay->GetMetrics(&m))) w = m.widthIncludingTrailingWhitespace;
            lay->Release();
            if (w > 0) return (int)ceilf(w);
            return 0;
        }
    }
    if (!g_ref) return (int)s.size() * 8;
    HFONT old = (HFONT)SelectObject(g_ref, f);
    SIZE sz{};
    GetTextExtentPoint32W(g_ref, s.c_str(), (int)s.size(), &sz);
    SelectObject(g_ref, old);
    return sz.cx;
}

int TextW(HFONT f, const std::wstring& s) {
    auto it = g_twCache.find(f);
    if (it != g_twCache.end()) {
        auto j = it->second.find(s);
        if (j != it->second.end()) return j->second;
    }
    int v = TextWUncached(f, s);
    if (g_twCacheN >= 600) { g_twCache.clear(); g_twCacheN = 0; }
    g_twCache[f][s] = v;
    ++g_twCacheN;
    return v;
}

void ClearTextWidthCache() {
    g_twCache.clear();
    g_twCacheN = 0;
}
