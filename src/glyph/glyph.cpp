// glyph.cpp —— 字形库的解析、索引与可画性判定（声明与边界见 glyph.h）。
//
// 数据来源是 glyph_data.cpp 里的静态 blob，格式为 tab 分隔：
//   C<TAB>key<TAB>中文名        分类头
//   I<TAB>字形<TAB>中文名<TAB>英文名   条目（英文名可省）
// 全流程不为条目分配宽字符串：只在要画到屏幕时才转，见 model.h 的说明。
#include "glyph/glyph.h"

#include <cctype>
#include <cstring>
#include <unordered_map>

#include "glyph/glyph_data.h"
#include "core/text_util.h"

using namespace wv;

// ---------------------------------------------------------------- 字形可画性
// 一个字形最多测 4 个候选字体，结果按字形缓存，实际只算一次。
static std::unordered_map<std::string, bool> g_drawCache;

static bool ProbeCovers(IDWriteFont* f, const std::wstring& s) {
    if (!f || s.empty()) return false;
    for (int i = 0, n = (int)s.size(); i < n; ) {
        UINT32 cp = NextCp(s.c_str(), i, n);
        if (cp == 0xFE0F || cp == 0xFE0E || cp == 0x200D) continue;  // 变体选择符 / ZWJ
        BOOL has = FALSE;
        if (FAILED(f->HasCharacter(cp, &has)) || !has) return false;
    }
    return true;
}

// 表情只认 Segoe UI Emoji，符号按 4 个候选字体依次测。
// 探测对象没建起来（DWrite 不可用）时一律放行，否则整页会变成空白。
bool GlyphDrawable(int page, const char* g, int gl) {
    if (!g || gl <= 0) return true;
    std::string key;
    key.reserve((size_t)gl + 1);
    key.push_back(page == PG_EMOJI ? 'E' : 'S');
    key.append(g, (size_t)gl);
    auto it = g_drawCache.find(key);
    if (it != g_drawCache.end()) return it->second;

    std::wstring w = U8N(g, gl);
    bool ok;
    if (page == PG_EMOJI) {
        ok = !g_probe[4] || ProbeCovers(g_probe[4], w);
    } else {
        ok = !g_probe[0] || ProbeCovers(g_probe[0], w) || ProbeCovers(g_probe[1], w) ||
             ProbeCovers(g_probe[2], w) || ProbeCovers(g_probe[3], w);
    }
    g_drawCache[key] = ok;
    return ok;
}

// 字体换了，缓存的可画性作废。
void ClearDrawCache() { g_drawCache.clear(); }

// 单个字体能否画出整串：PickSymFont 用它挑符号兜底字体。
bool GlyphFontCovers(IDWriteFont* f, const std::wstring& s) { return ProbeCovers(f, s); }

// ---------------------------------------------------------------- 字形库：解析
static std::string Str8(const char* s, int n) { return std::string(s, (size_t)n); }

// 按 tab 切成字段（不修改 blob，返回各字段的指针/长度）
static int SplitTabs(const char* p, const char* lineEnd, const char** f, int* fl, int maxf) {
    int n = 0;
    const char* s = p;
    while (n < maxf) {
        const char* t = s;
        while (t < lineEnd && *t != '\t') ++t;
        f[n] = s; fl[n] = (int)(t - s); ++n;
        if (t >= lineEnd) break;
        s = t + 1;
    }
    return n;
}

static const char* NextLine(const char* p, const char* end) {
    while (p < end && *p != '\n') ++p;
    return (p < end) ? p + 1 : end;
}

// 只解析分类表：分类条目仅记下范围与数量，条目索引留到真正要显示时再建
void GlyphParseTable(const char* blob, bool sym) {
    std::vector<GCat>& cats = sym ? g_symCats : g_emoCats;
    cats.clear();

    const char* p = blob;
    const char* end = blob + strlen(blob);
    while (p < end) {
        const char* le = p;
        while (le < end && *le != '\n') ++le;
        const char* f[4]; int fl[4];
        int n = SplitTabs(p, le, f, fl, 4);
        if (n >= 2 && fl[0] == 1) {
            if (f[0][0] == 'C') {
                GCat c;
                c.key  = Str8(f[1], fl[1]);
                c.name = Str8(f[2], fl[2]);
                c.raw = (le < end) ? le + 1 : end;
                cats.push_back(std::move(c));
            } else if (f[0][0] == 'I' && !cats.empty()) {
                ++cats.back().count;
            }
        }
        p = NextLine(p, end);
    }
    // 每个分类的条目范围止于下一个分类的起点
    for (size_t i = 0; i < cats.size(); ++i)
        cats[i].rawEnd = (i + 1 < cats.size()) ? cats[i + 1].raw : end;
}

void LoadCat(GCat& c, int page) {
    if (c.loaded) return;
    c.loaded = true;
    c.items.reserve((size_t)c.count);
    const char* p = c.raw;
    while (p < c.rawEnd) {
        const char* le = p;
        while (le < c.rawEnd && *le != '\n') ++le;
        const char* f[4]; int fl[4];
        int n = SplitTabs(p, le, f, fl, 4);
        // 系统字体画不出来的字形（方框）不列出来，见 GlyphDrawable
        if (n >= 3 && fl[0] == 1 && f[0][0] == 'I' && GlyphDrawable(page, f[1], fl[1])) {
            GItem it;
            it.g = f[1]; it.gl = fl[1];
            it.n = f[2]; it.nl = fl[2];
            if (n >= 4) { it.n2 = f[3]; it.n2l = fl[3]; }
            c.items.push_back(it);
        }
        p = NextLine(p, c.rawEnd);
    }
    // 计数改成「真正能画出来的条目数」：未加载的分类还沿用 blob 里的原始数量，
    // 首次打开该类时校正（这样启动不必把 9000 多条全测一遍覆盖度）
    c.count = (int)c.items.size();
}

void RebuildRecentItems(bool sym) {
    const std::vector<RecentGlyph>& src = sym ? g_recentSym : g_recentEmo;
    std::vector<GItem>& dst = sym ? g_recentSymItems : g_recentEmoItems;
    dst.clear();
    dst.reserve(src.size());
    for (const auto& r : src) {
        // 最近使用里的条目同样过滤：换了字体环境后可能已经画不出来
        if (!GlyphDrawable(sym ? PG_SYMBOL : PG_EMOJI, r.g.data(), (int)r.g.size())) continue;
        GItem it;
        it.g = r.g.data(); it.gl = (int)r.g.size();
        it.n = r.n.data(); it.nl = (int)r.n.size();
        dst.push_back(it);
    }
    g_recentCount[sym ? 1 : 0] = (int)dst.size();
}

// ---------------------------------------------------------------- 分类查询
int GlyphCatTotal(int page) {
    return page == PG_EMOJI ? (int)g_emoCats.size() : (int)g_symCats.size();
}

// 分类下标 → 分类对象（0 是动态的「最近使用」，因此整体错位 1）
GCat& GlyphCat(int page, int idx) {
    std::vector<GCat>& cats = page == PG_EMOJI ? g_emoCats : g_symCats;
    if (idx < 1) idx = 1;
    if (idx > (int)cats.size()) idx = (int)cats.size();
    return cats[(size_t)idx - 1];
}

int GlyphCatIndexByKey(int page, const char* key) {
    std::vector<GCat>& cats = page == PG_EMOJI ? g_emoCats : g_symCats;
    for (size_t i = 0; i < cats.size(); ++i)
        if (cats[i].key == key) return (int)i + 1;
    return -1;
}

std::wstring GlyphCatName(int page, int idx) {
    if (idx <= 0) return L"最近";
    return U8N(GlyphCat(page, idx).name.data(), (int)GlyphCat(page, idx).name.size());
}

int GlyphCatCount(int page, int idx) {
    if (idx <= 0) {
        RebuildRecentItems(page == PG_SYMBOL);
        return (int)(page == PG_EMOJI ? g_recentEmoItems.size() : g_recentSymItems.size());
    }
    return GlyphCat(page, idx).count;
}

// 「最近」是动态分类：一条都还没用过时它是空的。这种时候它整条都不该出现
// （分类条 chip / 抽屉 / 角标 / 抽屉标题的计数一起隐藏），否则面板一打开就是
// 一个空的「还没有最近」页面。
bool RecentEmpty(int page) {
    bool sym = (page == PG_SYMBOL);
    const std::vector<RecentGlyph>& src = sym ? g_recentSym : g_recentEmo;
    if (!src.empty() && (sym ? g_recentSymItems : g_recentEmoItems).empty())
        RebuildRecentItems(sym);          // 有原始记录但索引还没建
    return g_recentCount[sym ? 1 : 0] == 0;
}

int CurGlyphCat(int page) { return page == PG_EMOJI ? g_emojiCat : g_symCat; }

void SetGlyphCat(int page, int idx) {
    if (page == PG_EMOJI) g_emojiCat = idx; else g_symCat = idx;
    g_gridScroll = 0;
}

bool GlyphSearching(int page) {
    return (g_page == page) && !g_query.empty();
}

// 当前应当显示的字形列表：搜索命中优先，否则取当前分类（惰性建索引）
const std::vector<GItem>& GlyphItems(int page) {
    if (GlyphSearching(page)) return g_glyphHits;
    int idx = CurGlyphCat(page);
    if (idx <= 0) { RebuildRecentItems(page == PG_SYMBOL); return page == PG_EMOJI ? g_recentEmoItems : g_recentSymItems; }
    GCat& c = GlyphCat(page, idx);
    LoadCat(c, page);
    return c.items;
}

int GlyphCount(int page) {
    if (GlyphSearching(page)) return (int)g_glyphHits.size();
    return GlyphCatCount(page, CurGlyphCat(page));
}

// ---------------------------------------------------------------- 字形库：搜索
static bool ContainsCI(const char* p, int n, const std::string& q) {
    if ((int)q.size() > n) return false;
    for (int i = 0; i + (int)q.size() <= n; ++i) {
        int k = 0;
        while (k < (int)q.size() && tolower((unsigned char)p[i + k]) == (unsigned char)q[(size_t)k]) ++k;
        if (k == (int)q.size()) return true;
    }
    return false;
}

// 跨全部分类搜索：字形精确命中，或中文名/英文名包含关键词。
// 直接在 blob 上走一遍，不建索引也不分配字符串——7500 条一次扫描在微秒级。
static void GlyphSearchScan(bool sym, const std::string& q, std::vector<GItem>& out) {
    out.clear();
    const char* blob = sym ? glyphdata::kSymBlob : glyphdata::kEmoBlob;
    const char* end = blob + strlen(blob);
    const int kMax = 400;                 // 与参考页面的单次渲染上限一致
    const char* p = blob;
    while (p < end && (int)out.size() < kMax) {
        const char* le = p;
        while (le < end && *le != '\n') ++le;
        if (le - p > 2 && p[0] == 'I' && p[1] == '\t') {
            const char* f[4]; int fl[4];
            int n = SplitTabs(p, le, f, fl, 4);
            if (n >= 3) {
                bool hit = ((int)q.size() == fl[1] && memcmp(f[1], q.data(), q.size()) == 0)
                        || ContainsCI(f[2], fl[2], q)
                        || (n >= 4 && ContainsCI(f[3], fl[3], q));
                if (hit && GlyphDrawable(sym ? PG_SYMBOL : PG_EMOJI, f[1], fl[1])) {
                    GItem it;
                    it.g = f[1]; it.gl = fl[1];
                    it.n = f[2]; it.nl = fl[2];
                    if (n >= 4) { it.n2 = f[3]; it.n2l = fl[3]; }
                    out.push_back(it);
                }
            }
        }
        p = NextLine(p, end);
    }
}

static std::string LowerU8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], n, nullptr, nullptr);
    for (auto& c : out) c = (char)tolower((unsigned char)c);
    return out;
}

void RebuildGlyphHits() {
    if (g_page != PG_EMOJI && g_page != PG_SYMBOL) return;
    if (g_query == g_hitsQuery && g_hitsPage == g_page) return;
    g_hitsQuery = g_query;
    g_hitsPage = g_page;
    if (g_query.empty()) { g_glyphHits.clear(); return; }
    GlyphSearchScan(g_page == PG_SYMBOL, LowerU8(g_query), g_glyphHits);
}

void AddRecentGlyph(int page, const GItem& it) {
    std::vector<RecentGlyph>& list = page == PG_EMOJI ? g_recentEmo : g_recentSym;
    RecentGlyph r{std::string(it.g, (size_t)it.gl), std::string(it.n, (size_t)it.nl)};
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i].g == r.g) { list.erase(list.begin() + i); break; }
    }
    list.insert(list.begin(), std::move(r));
    if (list.size() > 80) list.resize(80);
}
