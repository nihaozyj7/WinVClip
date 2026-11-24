// layout.cpp —— 实现见 layout.h。
#include "ui/layout.h"

#include <algorithm>
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
int  GridContentH();
int  GridScrollMax();
void ComputeScrollbar();

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

    // 行几何：行高恒为 ROW_H，列表按像素滚动（见 app_state.h 里 g_scrollY 的说明）。
    // 可视区高度一般不是 ROW_H 的整数倍，底部那半行**要照常画出来**：一条被下边
    // 沿切掉一半的行会让人看出「下面还有内容」，而按整行排完留出的空档正好相反，
    // 看起来像是已经到底了。裁切由 PaintPanel 的 body clip 负责，这里给完整行矩形。
    L.row.clear();
    L.nRow = 0;
    int rh = S(ROW_H);
    L.rowH = rh;
    if (g_page == PG_HISTORY || g_page == PG_FAV) {
        int top0 = L.body.top + S(8);
        int avail = (L.body.bottom - S(12)) - top0;
        int total = (int)g_view.size();
        int count = 0;
        if (avail > 0 && g_top < total) {
            count = (avail + g_rowOff + rh - 1) / rh;
            if (count > total - g_top) count = total - g_top;
            if (count < 0) count = 0;
        }
        L.nRow = count;
        L.row.resize((size_t)count);
        for (int i = 0; i < count; ++i) {
            int ry = top0 + i * rh - g_rowOff;   // 首个可见行上移 g_rowOff 像素
            L.row[(size_t)i] = {S(12), ry, W - S(12), ry + rh};
        }
    }

    // toast
    L.toast = {W - S(14) - S(150), S(56), W - S(14), S(56) + S(32)};

    ComputeScrollbar();
}

// ================================================================== 设置窗口
// 七行的几何。控件统一右对齐到窗口内边距上，行高按内容给：
// 「临时文件目录」那行要放下「标题 + 路径」两行字，所以单独高一档。
// 与内嵌弹层时期的区别只有一个：卡片就是设置窗口的整个客户区，不再在面板里居中，
// 所以也不再有「面板太矮就压缩行高」那套逻辑——窗口尺寸本来就是按内容推出来的。
// 行 → setSw 下标；不是开关行返回 -1。开关行不连续（0/1 在前，最后加的项在末尾），
// 所以不能按下标直接推。
int SetSwitchOfRow(int row) {
    switch (row) {
    case SET_ROW_AUTO:    return 0;
    case SET_ROW_ADMIN:   return 1;
    case SET_ROW_CONFIRM: return 2;
    default: return -1;
    }
}

// 每行的自然高度，顺序与 layout.h 的 SET_ROW_* 一一对应。
// 「临时文件目录」和「清空历史」要放下标题 + 一行说明，所以单独高一档。
// SettingsWindowH 与 ComputeSettings 都读它，两边不会漂。
static const int SET_ROW_PX[SET_ROW_COUNT] = {52, 52, 56, 64, 52, 52, 62};
static int SettingsHeadH() { return S(48); }         // 标题行
static int SettingsBodyH() {
    int h = 0;
    for (int i = 0; i < SET_ROW_COUNT; ++i) h += S(SET_ROW_PX[i]);
    return h;
}

int SettingsWindowW() { return S(420); }
// 底部留白：S(16) 页边距 + S(34) 提示条（toast 画在这一条里，见 PaintSettingsWindow）。
int SettingsWindowH() { return SettingsHeadH() + SettingsBodyH() + S(16) + S(34); }

void ComputeSettings(int w, int h) {
    int headH = SettingsHeadH();
    if (w < S(260)) w = S(260);
    if (h < headH + S(40)) h = headH + S(40);

    L.setCard = {0, 0, w, h};
    L.setClose = {w - S(34), S(8), w - S(10), S(32)};
    // 窗口没有系统标题栏，标题行（含标题字与关闭按钮）整条都是拖动抓取区；
    // 关闭按钮自己先被 SettingsHit 命中，所以点它仍是关闭，不会变成拖动。
    L.setTitle = {0, 0, w, headH};

    int right = w - S(18);
    int cy = headH;
    int rowH[SET_ROW_COUNT];
    for (int i = 0; i < SET_ROW_COUNT; ++i) {
        rowH[i] = S(SET_ROW_PX[i]);
        L.setRow[i] = {S(18), cy, right, cy + rowH[i]};
        cy += rowH[i];
    }

    // 开关行：SET_ROW_AUTO / SET_ROW_ADMIN / SET_ROW_CONFIRM
    for (int i = 0; i < SET_ROW_COUNT; ++i) {
        int si = SetSwitchOfRow(i);
        if (si < 0) continue;
        int sh = S(22), sw = S(42);
        int ry = L.setRow[i].top + (rowH[i] - sh) / 2;
        L.setSw[si] = {right - sw, ry, right, ry + sh};
    }

    // 主题三选（宽度不够时按可用宽度均分，文字得放得下「跟随系统」）
    {
        const RECT& row = L.setRow[SET_ROW_THEME];
        const wchar_t* names[3] = {L"浅色", L"暗色", L"跟随系统"};
        int segW = 0;
        for (auto* n : names) {
            int t = TextW(g_fChip, n) + S(18);
            if (t > segW) segW = t;
        }
        int gap = S(6);
        int total = segW * 3 + gap * 2;
        int avail = row.right - row.left - S(96);
        if (total > avail) {
            segW = (avail - gap * 2) / 3;
            if (segW < S(40)) segW = S(40);
        }
        int sh = S(28);
        int ry = row.top + (rowH[SET_ROW_THEME] - sh) / 2;
        int sx = right - (segW * 3 + gap * 2);
        for (int i = 0; i < 3; ++i) {
            L.setSeg[i] = {sx, ry, sx + segW, ry + sh};
            sx += segW + gap;
        }
    }

    // 临时文件目录（标题 + 路径），右侧「更改…」
    {
        const RECT& row = L.setRow[SET_ROW_TEMP];
        int bh = S(30), bw = S(72);
        int ry = row.top + (rowH[SET_ROW_TEMP] - bh) / 2;
        L.setBrowse = {right - bw, ry, right, ry + bh};
    }

    // 记录上限 [−] [500] [+]
    {
        const RECT& row = L.setRow[SET_ROW_LIMIT];
        int sh = S(28), vw = S(56);
        int ry = row.top + (rowH[SET_ROW_LIMIT] - sh) / 2;
        L.setPlus = {right - sh, ry, right, ry + sh};
        L.setValue = {L.setPlus.left - S(6) - vw, ry, L.setPlus.left - S(6), ry + sh};
        L.setMinus = {L.setValue.left - S(6) - sh, ry, L.setValue.left - S(6), ry + sh};
    }

    // 清空剪贴板历史：标题 + 一行说明（保留置顶与收藏），右侧「清空」按钮
    {
        const RECT& row = L.setRow[SET_ROW_CLEAR];
        int bh = S(30), bw = S(64);
        int ry = row.top + (rowH[SET_ROW_CLEAR] - bh) / 2;
        L.setClear = {right - bw, ry, right, ry + bh};
    }
}

int SettingsHit(POINT p) {
    if (!g_settingsOpen) return SET_NONE;
    if (!PtIn(L.setCard, p)) return SET_NONE;
    if (PtIn(L.setClose, p)) return SET_CLOSE;
    for (int i = 0; i < 3; ++i) if (PtIn(L.setSw[i], p)) return SET_SW_AUTO + i;
    for (int i = 0; i < 3; ++i) if (PtIn(L.setSeg[i], p)) return SET_SEG_0 + i;
    if (PtIn(L.setBrowse, p)) return SET_BROWSE;
    if (PtIn(L.setMinus, p))  return SET_MINUS;
    if (PtIn(L.setPlus, p))   return SET_PLUS;
    if (PtIn(L.setClear, p))  return SET_CLEAR;
    // 整行可点：点标签也算点开关/三选，省得非要精准点到小控件上
    for (int i = 0; i < SET_ROW_COUNT; ++i) {
        if (!PtIn(L.setRow[i], p)) continue;
        int si = SetSwitchOfRow(i);
        if (si >= 0) return SET_SW_AUTO + si;
        if (i == SET_ROW_TEMP)  return SET_BROWSE;
        if (i == SET_ROW_CLEAR) return SET_CLEAR;
    }
    return SET_NONE;
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

// 菜单行内横向留白的唯一口径：外框→项、项→图标、图标宽、图标→文字、文字右留白。
// MenuRect（算宽）与 MenuItemIconRect / MenuItemTextRect（排版）全从这里取，
// paint 只调这两个 rect，别自己算偏移 —— 两边口径一旦漂开就是「一边贴边一边空」。
static const int MENU_PAD_X     = 6;
static const int MENU_ITEM_PADL = 9;
static const int MENU_ICON_W    = 15;
static const int MENU_ICON_GAP  = 9;
static const int MENU_TEXT_PADR = 8;

const wchar_t* MenuLabel(int idx, bool pinned) {
    switch (idx) {
    case MI_PIN:  return pinned ? L"取消置顶" : L"置顶";
    case MI_SAVE: return L"另存为…";
    default:      return L"删除";
    }
}

// 菜单宽度跟最长的一项标签走。原来写死 S(176)：11px 的「取消置顶」只有 40 多像素，
// 减掉图标和留白后右边要空出 70 多像素。
// 两种 pin 状态都量一遍取大 —— 否则点一次「置顶」，文案从「置顶」变「取消置顶」，
// 菜单宽度会当场跳一下。
static int MenuWidth() {
    int tw = 0;
    for (int i = 0; i < MI_COUNT; ++i) {
        int a = TextW(g_fChip, MenuLabel(i, false));
        int b = TextW(g_fChip, MenuLabel(i, true));
        if (a > tw) tw = a;
        if (b > tw) tw = b;
    }
    // 末位 S(4) 是量宽与真实排版的余量：DWrite 的 advance width 不含右 bearing，
    // 贴太紧会让最后一个字被裁一刀。
    return S(MENU_PAD_X) * 2 + S(MENU_ITEM_PADL) + S(MENU_ICON_W) + S(MENU_ICON_GAP)
           + tw + S(MENU_TEXT_PADR) + S(4);
}

RECT MenuRect() {
    int w = MenuWidth();
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
    return RECT{m.left + S(MENU_PAD_X), yy, m.right - S(MENU_PAD_X), yy + itemH};
}

// 菜单项的图标框与文字框：与 MenuWidth 用的是同一组留白常量，宽度才对得上。
RECT MenuItemIconRect(int idx) {
    RECT ir = MenuItemRect(idx);
    int side = S(MENU_ICON_W);
    int x = ir.left + S(MENU_ITEM_PADL);
    int y = ir.top + (ir.bottom - ir.top - side) / 2;
    return RECT{x, y, x + side, y + side};
}

RECT MenuItemTextRect(int idx) {
    RECT ir = MenuItemRect(idx);
    return RECT{MenuItemIconRect(idx).right + S(MENU_ICON_GAP), ir.top,
                ir.right - S(MENU_TEXT_PADR), ir.bottom};
}

// 命中测试：返回菜单项下标，未命中返回 -1
int MenuHit(POINT p) {
    for (int i = 0; i < MI_COUNT; ++i)
        if (PtIn(MenuItemRect(i), p)) return i;
    return -1;
}

// 确认弹层（删单条 / 清空历史共用同一份几何）。清空历史那张要多一行说明
// （会删几条 + 什么被保留），所以整体高一档。
RECT ModalRect() {
    int w = S(300);
    int h = (g_modalKind == MODAL_CLEAR_HIST) ? S(178) : S(146);
    int x = (L.W - w) / 2, y = (L.H - h) / 2;
    return RECT{x, y, x + w, y + h};
}

// 设置窗口里的同一张卡片：在设置窗口客户区（= L.setCard）里居中。
// 两个坐标系不能混用——设置窗口开着时面板是隐藏的，L.W/L.H 还是面板的旧尺寸。
RECT SettingsModalRect() {
    int w = S(300);
    int h = (g_modalKind == MODAL_CLEAR_HIST) ? S(178) : S(146);
    int cw = L.setCard.right - L.setCard.left;
    int ch = L.setCard.bottom - L.setCard.top;
    int x = L.setCard.left + (cw - w) / 2, y = L.setCard.top + (ch - h) / 2;
    return RECT{x, y, x + w, y + h};
}

// ================================================================== 绘制：组件
RECT CenterIcon(const RECT& b, int s) {
    int cx = (b.left + b.right) / 2, cy = (b.top + b.bottom) / 2;
    int x = cx - s / 2, y = cy - s / 2;
    return RECT{x, y, x + s, y + s};
}

// ================================================================== 列表滚动
// 主状态是 g_scrollY（像素，见 app_state.h），这里负责夹回合法区间、同步派生量
// g_top / g_rowOff，以及滚轮的缓动。为什么要缓动：滚轮一格正好是一行高，若一帧
// 就跳到位，眼睛看到的是「一批格子换成另一批」而不是内容在滚动——这正是之前
// 整行索引式滚动（g_top++）的观感问题，光改成像素偏移还不能解决。
int ListRowH() { return S(ROW_H); }

void SyncListScroll() {
    int rh = ListRowH();
    if (g_scrollY < 0) g_scrollY = 0;
    g_top = g_scrollY / rh;
    g_rowOff = g_scrollY % rh;
}

// 内容总高 - 可视高。依赖 L.body，调用方都已经先跑过 ComputeLayout。
int ListScrollMax() {
    int avail = (L.body.bottom - S(12)) - (L.body.top + S(8));
    int m = (int)g_view.size() * ListRowH() - avail;
    return m > 0 ? m : 0;
}

// 滚动的缓动：按时间插值 + smoothstep，而不是「每帧走剩余的一半」。
// 后者每帧的位移量差一个数量级（首帧直接走掉一半，末尾十几帧都在磨最后 1~2px），
// 眼睛看到的是「先跳一下、再慢慢蠕动」——用户说的「列表项跳动感」就来自这里。
// 现在固定时长、按经过时间取曲线值：起步和收尾都慢、中段快，帧间隔抖动也不影响速度。
static int       g_animFrom = 0;    // 本次动画的起始偏移
static int       g_animDur  = 0;    // 本次动画时长（ms），0 = 没有动画在跑
static ULONGLONG g_animT0   = 0;    // 起始时刻（GetTickCount64）
// 网格页（表情 / 符号）的滚动用同一套曲线和定时器，只是作用在 g_gridScroll 上。
// 早先网格页是「一格滚轮直接跳 52px」：一整排格子瞬间换掉，观感同样是不顺畅。
// g_gridAnchor 记住「上一次是动画自己写的那个值」：别处（切分类、改尺寸、
// 拖滚动条）直接改 g_gridScroll 时，动画自己让位，不会把视图往回拽。
static bool      g_animGrid = false;
static int       g_gridFrom = 0;
static int       g_gridTarget = 0;
static int       g_gridAnchor = 0;

// 距离越远走得越久（一格滚轮约 110ms），但有上下限：下限保证短距离也看得清过程，
// 上限避免连续快滚时拖出一条长尾巴。
static int ScrollAnimMs(int dist) {
    int ms = 60 + dist * 100 / 450;
    if (ms < 90) ms = 90;
    if (ms > 220) ms = 220;
    return ms;
}

void SetListScroll(int y, bool animate) {
    g_animGrid = false;                 // 列表动画接管：别让网格的动画状态串到这边
    int m = ListScrollMax();
    if (y < 0) y = 0;
    if (y > m) y = m;
    g_scrollTarget = y;
    if (!animate) {
        g_scrollY = y;
        if (g_animDur) {                 // 拖动/夹边要立刻到位：残留的动画必须停掉
            g_animDur = 0;
            if (g_panel) KillTimer(g_panel, TIMER_SCROLL);
        }
        SyncListScroll();
        return;
    }
    int dist = (y > g_scrollY) ? y - g_scrollY : g_scrollY - y;
    if (dist == 0) return;
    g_animFrom = g_scrollY;
    g_animDur  = ScrollAnimMs(dist);
    // 起点往前挪一帧：WheelBody 紧接着会调一次 TickListScroll 好让「第一次滚轮马上有
    // 位移」，若起点就是此刻，那一帧进度是 0，看起来像「滚了没反应、过一下才动」。
    g_animT0 = GetTickCount64() - 16;
    if (g_panel) SetTimer(g_panel, TIMER_SCROLL, 16, nullptr);
}

void ResetListScroll() {
    if (g_panel) KillTimer(g_panel, TIMER_SCROLL);
    g_animDur = 0;
    g_animGrid = false;
    g_scrollY = g_scrollTarget = 0;
    g_top = 0;
    g_rowOff = 0;
}

void ScrollListByPx(int dy) {
    if (dy == 0) return;
    // 从「目标」而不是「当前位置」起算：连滚时目标累加，不会把上一格还没走完的
    // 行程吃掉（否则连滚会比手的动作慢半拍）
    int base = (g_scrollTarget != g_scrollY) ? g_scrollTarget : g_scrollY;
    SetListScroll(base + dy, true);
}

// 网格页同理：从目标起算，连滚时不会吃掉上一格的行程。
void ScrollGridByPx(int dy) {
    if (dy == 0) return;
    int m = GridScrollMax();
    int base = (g_animGrid && g_gridTarget != g_gridScroll) ? g_gridTarget : g_gridScroll;
    int y = base + dy;
    if (y < 0) y = 0;
    if (y > m) y = m;
    g_gridTarget = y;
    g_gridAnchor = g_gridScroll;
    if (y == g_gridScroll) return;           // 已经贴边：不用再起动画
    g_gridFrom = g_gridScroll;
    g_animGrid = true;
    g_animDur = ScrollAnimMs(y > g_gridFrom ? y - g_gridFrom : g_gridFrom - y);
    g_animT0 = GetTickCount64() - 16;        // 往前挪一帧：第一次滚轮当场就有位移
    if (g_panel) SetTimer(g_panel, TIMER_SCROLL, 16, nullptr);
}

// 缓动一帧；返回 false = 已到位（调用方 KillTimer）。
// 列表与网格共用这一份：两者的差别只在「改哪个偏移量、要不要同步行派生量」。
bool TickListScroll() {
    int* cur = g_animGrid ? &g_gridScroll : &g_scrollY;
    int target = g_animGrid ? g_gridTarget : g_scrollTarget;
    int from   = g_animGrid ? g_gridFrom   : g_animFrom;
    if (g_animGrid && *cur != g_gridAnchor) {
        // 别处直接改过 g_gridScroll（切分类 / 改尺寸 / 拖滚动条）：动画让位，
        // 否则下一帧会把这个新位置按旧行程拽回去
        g_animDur = 0;
        if (g_panel) KillTimer(g_panel, TIMER_SCROLL);
        return false;
    }
    int dur = g_animDur > 0 ? g_animDur : 1;
    double t = (double)(GetTickCount64() - g_animT0) / (double)dur;
    if (t >= 1.0) {
        *cur = target;                              // 收尾必须严丝合缝落在目标上
    } else {
        double p = t * t * (3.0 - 2.0 * t);         // smoothstep
        double want = (double)from + (double)(target - from) * p;
        int y = (int)(want + (target >= from ? 0.5 : -0.5));
        if (y == *cur) return true;                 // 这一帧还不足 1px：继续跑，别提前收工
        *cur = y;
    }
    if (g_animGrid) g_gridAnchor = g_gridScroll;
    else            SyncListScroll();
    // 行矩形是 ComputeLayout 按 g_rowOff 生成的：只改 g_scrollY 不动布局，
    // 绘制仍拿旧矩形，表现就是「滚轮完全没反应」（实测踩过）。滚动条的
    // 几何（ComputeScrollbar）也挂在里面，网格页同理。
    ComputeLayout();
    return *cur != target;
}

void ClampScroll() {
    ComputeLayout();
    int rh = ListRowH();
    int avail = (L.body.bottom - S(12)) - (L.body.top + S(8));
    int full = (avail > 0) ? avail / rh : 0;   // 完全可见的行数（不含底部被切那半个）
    if (full < 1) full = 1;

    int y = g_scrollY;
    if (g_sel < g_top) {
        y = g_sel * rh;                        // 选中项在视口上方：让它成为首行
    } else if (g_sel >= g_top + full) {
        y = (g_sel + 1) * rh - avail;          // 在下方：让它的底边对齐视口底边
    }
    int m = ListScrollMax();
    if (y > m) y = m;
    if (y < 0) y = 0;
    // 键盘导航 / 重置必须即时到位，不能带惯性：否则连按 ↓ 时视口一直在追
    g_scrollTarget = y;
    g_scrollY = y;
    g_animDur = 0;
    if (g_panel) KillTimer(g_panel, TIMER_SCROLL);
    SyncListScroll();
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

// 网格页能滚多远（像素）。ClampGridScroll / 滚动条几何 / 缓动夹取都读这一份。
int GridScrollMax() {
    int maxS = GridContentH() - (L.body.bottom - L.body.top) + S(12);
    return maxS > 0 ? maxS : 0;
}

void ClampGridScroll() {
    int maxS = GridScrollMax();
    if (g_gridScroll > maxS) g_gridScroll = maxS;
    if (g_gridScroll < 0) g_gridScroll = 0;
    // 目标一起夹：内容变短（切换分类/搜索）后，残留的动画目标不能把视图拖出去
    if (g_gridTarget > maxS) g_gridTarget = maxS;
    if (g_gridTarget < 0) g_gridTarget = 0;
    if (!g_animGrid) g_gridAnchor = g_gridScroll;
}

// 滚动条几何。绘制（paint.cpp）与拖拽命中（panel.cpp）都读这一份：
// 两边各算一遍是「看得见滑块、按下去却抓不到」的经典来源。
void ComputeScrollbar() {
    L.scrollTrack = L.scrollThumb = RECT{0, 0, 0, 0};
    L.scrollMax = 0;
    int contentH = 0, offset = 0;
    if (g_page == PG_HISTORY || g_page == PG_FAV) {
        contentH = S(8) + (int)g_view.size() * L.rowH + S(12);
        offset = g_scrollY;      // 像素偏移，不是 g_top * 行高（后者只能整行对齐）
    } else if (g_page == PG_EMOJI || g_page == PG_SYMBOL) {
        contentH = GridContentH();
        offset = g_gridScroll;
    } else {
        return;
    }
    int viewH = L.body.bottom - L.body.top;
    if (contentH <= viewH) return;                 // 没得滚：不给几何，也就没有滚动条

    int trackTop = L.body.top + S(4);
    int trackBot = L.body.bottom - S(4);
    int trackH = trackBot - trackTop;
    if (trackH <= 0) return;
    int thumbH = (std::max)(S(28), (int)((long long)trackH * viewH / contentH));
    if (thumbH > trackH) thumbH = trackH;
    int maxOffset = contentH - viewH;
    if (maxOffset < 1) maxOffset = 1;
    int thumbTop = trackTop + (int)((long long)(trackH - thumbH) * offset / maxOffset);
    if (thumbTop < trackTop) thumbTop = trackTop;
    if (thumbTop + thumbH > trackBot) thumbTop = trackBot - thumbH;

    // 视觉上仍是贴右缘的 5px 细条，命中带向左侧放宽几像素：细条按不准，
    // 而右缘最外侧那 5px 是改尺寸的抓取带（HitResize 对列表区让位，见那里）
    L.scrollTrack = RECT{L.W - S(10), trackTop, L.W - S(1), trackBot};
    L.scrollThumb = RECT{L.W - S(8), thumbTop, L.W - S(3), thumbTop + thumbH};
    L.scrollMax = maxOffset;
}

// 滚动条拖拽 / 点轨道：把偏移直接设到 off（当前页面坐标系），立即到位、不走缓动。
// 拖动时内容必须严格跟手，带惯性就成了「拖一下自己滑一小段」。
void SetScrollOffsetNow(int off) {
    ComputeLayout();                  // 下面的夹取依赖 L.body
    if (g_page == PG_HISTORY || g_page == PG_FAV) {
        SetListScroll(off, false);
    } else {
        // 拖动要严格跟手：残留的网格动画必须停掉（否则下一帧会拽回旧行程）
        if (g_animGrid) {
            g_animDur = 0;
            if (g_panel) KillTimer(g_panel, TIMER_SCROLL);
        }
        g_animGrid = false;
        g_gridScroll = off;
        ClampGridScroll();
        g_gridTarget = g_gridScroll;
        g_gridAnchor = g_gridScroll;
    }
    ComputeLayout();                  // 改了 g_scrollY / g_gridScroll，几何要跟着重算
}

