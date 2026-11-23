// layout.cpp —— 实现见 layout.h。
#include "ui/layout.h"

#include "clipboard/capture.h"
#include "core/app_state.h"    // g_panelW/H / g_page / g_filter / g_query / g_*Cat / g_*Scroll
#include "core/text_measure.h" // TextW
#include "core/text_util.h"
#include "core/theme.h"
#include "core/util.h"         // S
#include "glyph/emoji_atlas.h" // EmojiStride
#include "glyph/glyph.h"       // RebuildGlyphHits / CurGlyphCat / GlyphCatTotal / RecentEmpty
#include "model/clip_model.h"  // PageCount

using namespace wv;

// ================================================================== 布局
Layout L;

void ComputeDrawer();

const wchar_t* TAB_NAME[4] = {L"剪贴板", L"收藏", L"表情", L"符号"};
const wchar_t* FILTER_NAME[4] = {L"全部", L"文本", L"图片", L"其他"};

void ComputeLayout() {
    RebuildGlyphHits();       // 关键词/页面一变就重新做一次跨分类搜索（未变时是字符串比较）
    // 「最近」为空时它不可见，当前分类就不能停在它上面：否则整页空白、分类条也没有高亮项。
    if ((g_page == PG_EMOJI || g_page == PG_SYMBOL) && CurGlyphCat(g_page) <= 0 && RecentEmpty(g_page)) {
        if (g_page == PG_EMOJI) g_emojiCat = GlyphCatTotal(g_page) > 0 ? 1 : 0;
        else                    g_symCat   = GlyphCatTotal(g_page) > 0 ? 1 : 0;
        g_gridScroll = 0;
    }
    int W = g_panelW ? g_panelW : S(P_W);
    int H = g_panelH ? g_panelH : S(P_H);
    L.W = W; L.H = H;

    L.head = {0, 0, W, S(HEAD_H)};

    // 右上三个按钮：临时固定 / 设置 / 关闭（与标题栏等高，关闭按钮贴右缘）
    // 先算按钮，页签才知道自己能用多宽
    // 宽高一致：按钮做成 HEAD_H×HEAD_H 的正方形（原来宽 30、高 34，略扁）
    int btnW = S(HEAD_H);
    L.closeB = {W - btnW, 0, W, S(HEAD_H)};
    L.gear   = {L.closeB.left - btnW, 0, L.closeB.left, S(HEAD_H)};
    L.pinB   = {L.gear.left - btnW, 0, L.gear.left, S(HEAD_H)};

    // 页签：从左边距起排，右边界顶到固定按钮左侧。
    // 宽度下限压到 360 后四个页签的自然宽度放不下，此时按可用宽度均分，
    // 否则「剪贴板」会顶到图钉按钮底下，或者页签互相压住。
    // 页签彼此紧邻、不留间隔（相邻页签直接共享一条边，活动页签的填充块
    // 与两侧页签贴齐，不再露出标题栏底色成缝）。
    int x0 = 0;   // 页签贴着面板左缘起排（原来是 S(12) 左边距）
    int avail = L.pinB.left - S(6) - x0;
    int nat[4], total = 0;
    for (int i = 0; i < 4; ++i) {
        nat[i] = TextW(g_fTab, TAB_NAME[i]) + S(20);
        total += nat[i];
    }
    int x = x0;
    if (total <= avail) {
        for (int i = 0; i < 4; ++i) {
            L.tab[i] = {x, 0, x + nat[i], S(HEAD_H)};
            x += nat[i];
        }
    } else {
        int each = avail / 4;
        if (each < S(30)) each = S(30);
        for (int i = 0; i < 4; ++i) {
            L.tab[i] = {x, 0, x + each, S(HEAD_H)};
            x += each;
        }
    }

    int sy = S(HEAD_H + SEARCH_TOP);
    L.searchBox = {S(SEARCH_PADX), sy, W - S(SEARCH_PADX), sy + S(SEARCH_H)};

    int fy = sy + S(SEARCH_H) + S(SEARCH_BOT);
    L.filters = {0, fy, W, fy + S(FILTER_H)};

    int by = L.filters.bottom;
    int bb = H - S(FOOT_H);
    L.body = {0, by, W, bb};
    L.foot = {0, bb, W, H};

    // 分类条：右侧 chip 组 + 「⋯」抽屉入口。
    // chip 数固定（最多 4 个），不随分类总数增长——分类再多也不重排分类条，
    // 全铺出来会把分类条撑爆，其余分类统一进抽屉。
    int cy = L.filters.top + (S(FILTER_H) - S(26)) / 2;
    int gap = S(8);
    int moreW = S(26);
    int right = W - S(16);
    L.nChip = 0;
    L.moreBtn = {0, 0, 0, 0};

    if (g_page == PG_EMOJI || g_page == PG_SYMBOL) {
        int page = g_page;
        int show[5], n = 0;
        if (!RecentEmpty(page)) show[n++] = 0;   // 「最近」为空时整条分类隐藏
        static const char* PINS_SYM[] = {"quick", "punct", "math"};
        static const char* PINS_EMO[] = {"smileys", "people", "nature"};
        const char* const* pins = (page == PG_SYMBOL) ? PINS_SYM : PINS_EMO;
        for (int k = 0; k < 3; ++k) {
            int i = GlyphCatIndexByKey(page, pins[k]);
            if (i > 0 && n < 4) show[n++] = i;
        }
        int cur = CurGlyphCat(page);
        bool has = false;
        for (int i = 0; i < n; ++i) if (show[i] == cur) has = true;
        if (!has) { if (n >= 4) --n; show[n++] = cur; }

        int cw[5];
        for (int i = 0; i < n; ++i) cw[i] = TextW(g_fChip, GlyphCatName(page, show[i])) + S(22);

        // 从右往左摆，放不下就丢掉最左边那个；当前分类不会是被丢的那个
        L.moreBtn = {right - moreW, cy, right, cy + S(26)};
        int x = L.moreBtn.left - gap;
        int labelRight = S(26) + S(84);
        RECT cr[5]; int cc[5]; int m = 0;
        for (int i = n - 1; i >= 0; --i) {
            if (x - cw[i] < labelRight) break;
            cr[m] = {x - cw[i], cy, x, cy + S(26)};
            cc[m] = show[i];
            ++m;
            x -= cw[i] + gap;
        }
        for (int i = 0; i < m; ++i) { L.chip[i] = cr[m - 1 - i]; L.chipCat[i] = cc[m - 1 - i]; }
        L.nChip = m;
    } else {
        int cw[4], total = 0;
        for (int i = 0; i < FL_COUNT; ++i) {
            cw[i] = TextW(g_fChip, FILTER_NAME[i]) + S(22);
            total += cw[i];
        }
        total += gap * (FL_COUNT - 1);
        int cx = right - total;
        for (int i = 0; i < FL_COUNT; ++i) {
            L.chip[i] = {cx, cy, cx + cw[i], cy + S(26)};
            L.chipCat[i] = i;
            cx += cw[i] + gap;
        }
        L.nChip = FL_COUNT;
    }
    if (g_drawerOpen) ComputeDrawer();

    // 行（高度可调后行数随面板走，不再有固定上限）
    L.row.clear();
    L.nRow = 0;
    if (g_page == PG_HISTORY || g_page == PG_FAV) {
        int avail = (L.body.bottom - S(12)) - (L.body.top + S(8));
        int fit = avail / S(ROW_H);
        if (fit < 1) fit = 1;
        int vis = (int)g_view.size() - g_top;
        if (vis > fit) vis = fit;
        if (vis < 0) vis = 0;
        L.nRow = vis;
        L.row.resize((size_t)vis);
        for (int i = 0; i < vis; ++i) {
            int ry = L.body.top + S(8) + i * S(ROW_H);
            L.row[(size_t)i] = {S(12), ry, W - S(12), ry + S(ROW_H)};
        }
    }

    // toast
    L.toast = {W - S(14) - S(150), S(56), W - S(14), S(56) + S(32)};
}

bool PtIn(const RECT& r, POINT p) {
    return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
}

// ---------------------------------------------------------------- 分类抽屉
// 参考页面的「全部分类」抽屉：从分类条下方展开、盖住列表区，按「域」分组，
// 几十个分类也不会失去结构；表情只有一个分组，省掉分组标题。
std::wstring CatFirstGlyph(int page, int idx) {
    if (idx <= 0) {
        const std::vector<RecentGlyph>& r = page == PG_EMOJI ? g_recentEmo : g_recentSym;
        return r.empty() ? L"↺" : U8N(r[0].g.data(), (int)r[0].g.size());
    }
    const GCat& c = GlyphCat(page, idx);
    // 直接读 blob 的第一条，避免为了一个字符把整类条目都建索引
    if (c.count > 0 && c.raw && c.rawEnd > c.raw + 3 && c.raw[0] == 'I') {
        const char* s = c.raw + 2;
        const char* t = s;
        while (t < c.rawEnd && *t != '\t' && *t != '\n') ++t;
        return U8N(s, (int)(t - s));
    }
    return L"·";
}

void ComputeDrawer() {
    L.dRect.clear(); L.dCat.clear(); L.dSec.clear();

    L.drawer = {0, L.filters.bottom, L.W, L.foot.top};
    L.drawerHead = {0, L.drawer.top, L.W, L.drawer.top + S(32)};
    int closeW = TextW(g_fChip, L"关闭") + S(10);
    L.drawerClose = {L.W - S(14) - closeW, L.drawerHead.top + S(6),
                     L.W - S(14), L.drawerHead.bottom - S(6)};

    const int page = (g_page == PG_EMOJI) ? PG_EMOJI : PG_SYMBOL;
    const int cols = 4, gapx = S(6), gapy = S(6), padx = S(12);
    const int cw = (L.W - padx * 2 - gapx * (cols - 1)) / cols;
    const int ch = S(30);
    const int startY = L.drawerHead.bottom + S(10);

    struct Sec { std::wstring name; std::vector<int> cats; };
    std::vector<Sec> secs;

    // 两页同构：只有一层分类（符号的大类＝数据里的「域」，已在生成期合并），
    // 因此不再有域分组标题，抽屉就是一片按钮。
    const std::vector<GCat>& cats = (page == PG_SYMBOL) ? g_symCats : g_emoCats;
    Sec s; 
    if (!RecentEmpty(page)) s.cats.push_back(0);   // 「最近」为空时抽屉里也没有它
    for (size_t i = 0; i < cats.size(); ++i) s.cats.push_back((int)i + 1);
    secs.push_back(std::move(s));

    int y = startY - g_drawerScroll;
    for (const auto& s : secs) {
        if (!s.name.empty()) {
            L.dSec.push_back({{padx, y, L.W - padx, y + S(14)}, s.name});
            y += S(14) + S(6);
        }
        for (size_t k = 0; k < s.cats.size(); ++k) {
            int x = padx + (int)(k % cols) * (cw + gapx);
            int yy = y + (int)(k / cols) * (ch + gapy);
            L.dRect.push_back({x, yy, x + cw, yy + ch});
            L.dCat.push_back(s.cats[k]);
        }
        int rows = ((int)s.cats.size() + cols - 1) / cols;
        y += rows * (ch + gapy) + S(11);
    }
    g_drawerContentH = (y + g_drawerScroll) - startY;
}

int DrawerMaxScroll() {
    int viewH = L.drawer.bottom - (L.drawerHead.bottom + S(10));
    int m = g_drawerContentH - viewH;
    return m > 0 ? m : 0;
}

void ClampDrawerScroll() {
    int m = DrawerMaxScroll();
    if (g_drawerScroll > m) g_drawerScroll = m;
    if (g_drawerScroll < 0) g_drawerScroll = 0;
}

int DrawerHit(POINT p) {
    for (size_t i = 0; i < L.dRect.size(); ++i)
        if (PtIn(L.dRect[i], p)) return (int)i;
    return -1;
}

void ToggleDrawer(bool open) {
    g_drawerOpen = open;
    if (open) { g_drawerScroll = 0; g_drawerHover = -1; }
    ComputeLayout();
    Redraw();
}



// 字形格子：表情页与符号页共用同一套网格（9 列 × 44px），
// 两页都只画字形本身，不再有卡片、名称这类附加信息
int GlyphCols() { return 9; }
RECT GlyphCellRect(int i) {
    int cols = GlyphCols();
    int padL = S(12), padR = S(12), gap = S(5);
    int availW = L.W - padL - padR;
    int cw = (availW - gap * (cols - 1)) / cols;
    int ch = S(44);
    int row = i / cols, col = i % cols;
    int x = padL + col * (cw + gap);
    int y = L.body.top + S(8) + row * (ch + gap) - g_gridScroll;
    return RECT{x, y, x + cw, y + ch};
}

// 菜单：跟随右键位置弹出，越界时翻到光标另一侧、再不行就贴边
// 三项：置顶 / 另存为 /（分隔线）删除
const int MENU_SEP_BEFORE = MI_DELETE;   // 分隔线画在该项之前
const int MENU_SEP_H      = 9;

RECT MenuRect() {
    int w = S(176);
    int itemH = S(34);
    int h = S(6) * 2 + itemH * MI_COUNT + S(MENU_SEP_H);

    int x = g_menuPt.x + S(2);
    int y = g_menuPt.y + S(2);
    if (x + w > L.W - S(8)) x = g_menuPt.x - S(2) - w;
    if (x < S(8)) x = S(8);
    if (x + w > L.W - S(8)) x = L.W - S(8) - w;

    int bodyBot = L.body.bottom - S(4);
    if (y + h > bodyBot) y = g_menuPt.y - S(2) - h;
    if (y < L.body.top) y = L.body.top;
    if (y + h > bodyBot) y = bodyBot - h;
    if (y < L.body.top) y = L.body.top;
    return RECT{x, y, x + w, y + h};
}

RECT MenuItemRect(int idx) {
    RECT m = MenuRect();
    int itemH = S(34);
    int yy = m.top + S(6) + idx * itemH;
    if (idx >= MENU_SEP_BEFORE) yy += S(MENU_SEP_H);
    return RECT{m.left + S(6), yy, m.right - S(6), yy + itemH};
}

// 命中测试：返回菜单项下标，未命中返回 -1
int MenuHit(POINT p) {
    for (int i = 0; i < MI_COUNT; ++i)
        if (PtIn(MenuItemRect(i), p)) return i;
    return -1;
}

// 删除确认弹层
RECT ModalRect() {
    int w = S(300), h = S(146);
    int x = (L.W - w) / 2, y = (L.H - h) / 2;
    return RECT{x, y, x + w, y + h};
}

// ================================================================== 绘制：组件
RECT CenterIcon(const RECT& b, int s) {
    int cx = (b.left + b.right) / 2, cy = (b.top + b.bottom) / 2;
    int x = cx - s / 2, y = cy - s / 2;
    return RECT{x, y, x + s, y + s};
}

void ClampScroll() {
    ComputeLayout();
    int fit = L.nRow;
    if (g_top > 0 && g_sel < g_top) g_top = g_sel;
    if (g_top + fit <= g_sel) g_top = g_sel - fit + 1;
    int maxTop = (int)g_view.size() - fit;
    if (maxTop < 0) maxTop = 0;
    if (g_top > maxTop) g_top = maxTop;
    if (g_top < 0) g_top = 0;
    ComputeLayout();
}

// 网格页内容总高（表情与符号同一套网格）
int GridContentH() {
    if (g_page == PG_EMOJI || g_page == PG_SYMBOL) {
        int n = GlyphCount(g_page);
        int rows = (n + GlyphCols() - 1) / GlyphCols();
        return S(8) + rows * (S(44) + S(5));
    }
    return 0;
}

void ClampGridScroll() {
    int maxS = GridContentH() - (L.body.bottom - L.body.top) + S(12);
    if (maxS < 0) maxS = 0;
    if (g_gridScroll > maxS) g_gridScroll = maxS;
    if (g_gridScroll < 0) g_gridScroll = 0;
}

