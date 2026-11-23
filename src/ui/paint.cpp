// paint.cpp —— 实现见 paint.h。
//
// 阅读顺序建议：PaintPanel（缓冲与脏区）-> PaintHead/PaintFilters/PaintRow（各区域）
// -> PaintMenu/PaintModal（弹层）-> PaintToast。字形页的三块（PaintEmoji /
// PaintSymbols / PaintGlyphEmpty）依赖 emoji 图集，图集本身在 glyph/ 下。
#include "ui/paint.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "clipboard/capture.h"
#include "core/app_state.h"    // 全部视图状态与设备对象
#include "core/diag.h"
#include "core/model.h"
#include "core/text_measure.h" // Fmt / TextW
#include "core/text_util.h"
#include "core/theme.h"
#include "core/util.h"         // S / RelTime / SizeText
#include "glyph/emoji_atlas.h"
#include "glyph/glyph.h"
#include "image/raster.h"
#include "image/thumb.h"
#include "model/clip_model.h"
#include "render/dc.h"
#include "render/fonts.h"
#include "render/icons.h"
#include "render/path.h"
#include "ui/layout.h"

using namespace wv;

// ================================================================== 绘制
// 左上、右上为圆角、底部为直角的矩形填充。
// 页签的形状是 CSS 的 border-radius: 6px 6px 0 0——底部要和内容区连成一片，
// 所以底角必须保持直角；直接用四角圆角的 FillRound 会在页签下沿露出标题栏底色。
void FillTopRound(HDC dc, const RECT& r, float rad, COLORREF c) {
    (void)dc;
    if (!g_rt || !g_painting) return;
    if (rad < 0.1f) { FillBox(dc, r, c); return; }
    float l = (float)r.left, t = (float)r.top, rr = (float)r.right, b = (float)r.bottom;
    if (rad > (rr - l) / 2.0f) rad = (rr - l) / 2.0f;
    if (rad > b - t) rad = b - t;
    const float k = 0.5522847498f * rad;
    PathB p;
    if (!p.ok()) { FillBox(dc, r, c); return; }
    p.MoveFill(l + rad, t);
    p.Line(rr - rad, t);
    p.sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(rr - rad + k, t),
                                          D2D1::Point2F(rr, t + rad - k),
                                          D2D1::Point2F(rr, t + rad)));
    p.Line(rr, b);
    p.Line(l, b);
    p.Line(l, t + rad);
    p.sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(l, t + rad - k),
                                          D2D1::Point2F(l + rad - k, t),
                                          D2D1::Point2F(l + rad, t)));
    p.EndClosed();
    p.Fill(c);
}

void PaintHead(HDC dc) {
    int headBot = L.head.bottom - 1;
    RECT hb{0, 0, L.W, headBot};
    FillBox(dc, hb, C_SUBTLE);

    // 页签。活动页签是顶部两角圆、底边与内容区连通的卡片（6px 6px 0 0）：
    // 圆角连边框一起弯，所以先按边框色铺一层，再内缩 1px 铺底色；底边不内缩，
    // 底色一直落到底线上，才和下面的内容区接得上。
    float tabRad = (float)S(6);
    for (int i = 0; i < 4; ++i) {
        RECT t = L.tab[i];
        bool active = (i == g_page);
        RECT rr = t; rr.bottom = headBot;
        if (active) {
            FillTopRound(dc, rr, tabRad, C_LINE);
            RECT inner{rr.left + 1, rr.top + 1, rr.right - 1, rr.bottom};
            FillTopRound(dc, inner, tabRad - 1, C_SURFACE);
            // 顶部橙色条（覆盖边框线，与 CSS 的 ::after 一致）
            RECT bar{rr.left + S(12), rr.top, rr.right - S(12), rr.top + S(2)};
            FillBox(dc, bar, C_VOLT);
        }
        TextOutF(dc, g_fTab, active ? C_INK : C_MUTED, TAB_NAME[i], rr,
                 DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    }

    // 右上三个按钮的高亮与图标在 PaintHeadButtons 里画（放在外框线之后，
    // 这样悬停/固定态的高亮块能贴到窗口边缘，不会被 1px 外框线隔出一条浅边）

    HLine(dc, 0, L.W, headBot, C_LINE);
    // 活动页签底部与内容区连通（对应 CSS 的 border-bottom-color: surface）
    {
        RECT t = L.tab[g_page];
        RECT gap{t.left + 1, headBot, t.right - 1, headBot + 1};
        FillBox(dc, gap, C_SURFACE);
    }
}

// 右上角为圆角、其余为直角的矩形填充。贴窗口右上角的按钮高亮块必须这样做：
// 窗口靠 SetWindowRgn 裁圆角，若高亮块是直角矩形，右上角会被硬裁出一个缺口，
// 缺口露出后面的桌面，看起来就是「按钮右上方一圈白」。
void FillTRRound(HDC dc, const RECT& r, float rad, COLORREF c) {
    (void)dc;
    if (!g_rt || !g_painting) return;
    if (rad < 0.1f) { FillBox(dc, r, c); return; }
    float l = (float)r.left, t = (float)r.top, rr = (float)r.right, b = (float)r.bottom;
    if (rad > rr - l) rad = rr - l;
    if (rad > b - t) rad = b - t;
    const float k = 0.5522847498f * rad;
    PathB p;
    if (!p.ok()) { FillBox(dc, r, c); return; }
    p.MoveFill(l, t);
    p.Line(rr - rad, t);
    p.sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(rr - rad + k, t),
                                          D2D1::Point2F(rr, t + rad - k),
                                          D2D1::Point2F(rr, t + rad)));
    p.Line(rr, b);
    p.Line(l, b);
    p.EndClosed();
    p.Fill(c);
}

// 标题栏右上三个按钮：临时固定 / 设置 / 关闭
// 放在外框线之后绘制——高亮块要盖住那 1px 外框线，否则悬停时按钮边缘会多出一道浅边
void PaintHeadButtons(HDC dc) {
    bool hp = (g_hoverChip == 902), hg = (g_hoverChip == 900), hc = (g_hoverChip == 901);
    float rad = (float)S(RADIUS) / 2.0f;
    if (g_pinned)     FillTRRound(dc, L.pinB, 0, C_VOLTSOFT);
    else if (hp)      FillTRRound(dc, L.pinB, 0, C_SURFACE);
    if (hg)           FillTRRound(dc, L.gear, 0, C_SURFACE);
    if (hc)           FillTRRound(dc, L.closeB, rad, C_DANGER);

    RECT pi = CenterIcon(L.pinB, S(17));
    RECT gi = CenterIcon(L.gear, S(17));
    RECT ci = CenterIcon(L.closeB, S(17));
    DrawIcon(dc, IC_PIN, pi, g_pinned ? C_VOLT : (hp ? C_INK : C_MUTED), g_pinned ? 2.6f : 1.9f);
    DrawIcon(dc, IC_GEAR, gi, hg ? C_INK : C_MUTED, 1.9f);
    DrawIcon(dc, IC_X, ci, hc ? C_SURFACE : C_MUTED, 2.0f);
}

void PaintSearch(HDC dc) {
    RECT b = L.searchBox;
    FillRound(dc, b, S(8), C_CANVAS, C_LINE);
    {
        // focus ring：对应 CSS 的 box-shadow 0 0 0 2px rgba(255,107,53,.14)
        RECT ring = b; InflateRect(&ring, S(2), S(2));
        FillRound(dc, ring, S(10), C_RINGSOFT, C_RINGSOFT, 0);
        FillRound(dc, b, S(8), C_CANVAS, C_VOLT);
    }

    RECT si{b.left + S(11), b.top + (S(SEARCH_H) - S(17)) / 2,
            b.left + S(11) + S(17), b.top + (S(SEARCH_H) - S(17)) / 2 + S(17)};
    DrawIcon(dc, IC_SEARCH, si, C_MUTED, 2.0f);

    int tx = si.right + S(9);
    RECT tr{tx, b.top, b.right - S(11), b.bottom};
    // 组合串（IME 还没上屏的拼音）跟在查询串后面一起画，系统那张组合窗口已屏蔽
    std::wstring shown = g_query + g_comp;
    if (shown.empty()) {
        TextOutF(dc, g_fSearch, C_MUTED, L"搜索剪贴板内容", tr,
                 DT_SINGLELINE | DT_VCENTER | DT_LEFT);
    } else {
        TextOutF(dc, g_fSearch, C_INK, shown, tr, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        if (!g_comp.empty()) {
            int x1 = tx + TextW(g_fSearch, g_query);
            int x2 = x1 + TextW(g_fSearch, g_comp);
            int uy = b.bottom - S(7);
            HLine(dc, x1, (std::min)(x2, (int)tr.right), uy, C_VOLT);
        }
    }
    if (g_caretOn && IsWindowVisible(g_panel)) {
        int cx = tx + TextW(g_fSearch, shown) + S(1);
        if (cx > tr.right - S(2)) cx = tr.right - S(2);
        RECT cr{cx, b.top + S(11), cx + (std::max)(1, S(1)), b.bottom - S(11)};
        FillBox(dc, cr, C_INK);
    }
}

// IME 候选窗默认会飘到屏幕角落（窗口是自绘、没有系统 caret），
// 这里把它钉到搜索框里光标的正下方。
void PositionImeCandidate(HWND hwnd) {
    HIMC himc = ImmGetContext(hwnd);
    if (!himc) return;
    RECT b = L.searchBox;
    int tx = b.left + S(11) + S(17) + S(9);
    int x = tx + TextW(g_fSearch, g_query + g_comp) + S(1);
    if (x > b.right - S(14)) x = b.right - S(14);
    POINT p{x, b.bottom + S(2)};
    ClientToScreen(hwnd, &p);
    CANDIDATEFORM cf{};
    cf.dwIndex = 0;
    cf.dwStyle = CFS_CANDIDATEPOS;
    cf.ptCurrentPos = p;
    ImmSetCandidateWindow(himc, &cf);
    ImmReleaseContext(hwnd, himc);
}

void PaintFilters(HDC dc) {
    RECT f = L.filters;
    FillBox(dc, f, C_SURFACE);

    // 左侧「分组 · 数量」
    int cy = f.top + S(FILTER_H) / 2;
    RECT mk{S(16), cy - S(6), S(16) + S(2), cy + S(6)};
    FillRound(dc, mk, S(1), C_VOLT, C_VOLT, 0);

    const wchar_t* pageName = (g_page == PG_FAV) ? L"收藏"
                            : (g_page == PG_EMOJI) ? L"表情"
                            : (g_page == PG_SYMBOL) ? L"符号" : L"剪贴板";
    const wchar_t* grp = ((g_page == PG_EMOJI || g_page == PG_SYMBOL) && !g_query.empty())
                       ? L"搜索" : pageName;
    std::wstring label = std::wstring(grp) + L" · " + std::to_wstring(PageCount());
    RECT lr{S(26), f.top, S(26) + S(84), f.top + S(FILTER_H)};
    TextOutF(dc, g_fChip, C_MUTED, label, lr, DT_SINGLELINE | DT_VCENTER | DT_LEFT);

    // chips：下标 i 对应的分类取自 L.chipCat[i]（分类数远多于 chip 数，两者不再一一对应）
    int active = (g_page == PG_EMOJI || g_page == PG_SYMBOL) ? CurGlyphCat(g_page) : g_filter;
    for (int i = 0; i < L.nChip; ++i) {
        int cat = L.chipCat[i];
        bool on = (cat == active);
        RECT c = L.chip[i];
        COLORREF bg = on ? C_VOLTSOFT : (g_hoverChip == i ? C_SUBTLE : C_SURFACE);
        COLORREF bd = on ? C_VOLT : C_LINE;
        FillRound(dc, c, S(13), bg, bd);
        std::wstring nm = (g_page == PG_EMOJI || g_page == PG_SYMBOL)
                        ? GlyphCatName(g_page, cat) : FILTER_NAME[cat];
        TextOutF(dc, g_fChip, on ? C_INK : C_MUTED, nm, c, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    }

    // 「⋯」分类抽屉入口（角标显示分类总数）
    if (L.moreBtn.right > L.moreBtn.left) {
        RECT mb = L.moreBtn;
        bool on = g_drawerOpen;
        FillRound(dc, mb, S(13), on ? C_VOLTSOFT : (g_hoverChip == 903 ? C_SUBTLE : C_SURFACE),
                  on ? C_VOLT : C_LINE);
        TextOutF(dc, g_fChip, C_INK, L"⋯", mb, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        int badge = GlyphCatTotal(g_page) + (RecentEmpty(g_page) ? 0 : 1);   // +1 = 「最近」
        std::wstring bs = std::to_wstring(badge);
        int bw = (std::max)(S(15), TextW(g_fToast, bs) + S(5));
        // 角标压在按钮右上角外侧：不遮住「⋯」本身，也不越过窗口边（右缘还有 16px 余量）
        RECT br{mb.right + S(4) - bw, mb.top - S(7), mb.right + S(4), mb.top - S(7) + S(13)};
        FillRound(dc, br, S(7), C_VOLT, C_SURFACE, 1);
        TextOutF(dc, g_fKbd, C_SURFACE, bs, br, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    }

    HLine(dc, 0, L.W, f.bottom - 1, C_LINE);
}

void PaintRow(HDC dc, int i) {
    int vi = g_top + i;
    if (vi >= (int)g_view.size()) return;
    ClipItem& it = g_items[g_view[vi]];
    RECT r = L.row[i];
    bool sel = (vi == g_sel);
    bool hov = (vi == g_hoverRow);

    if (sel) {
        FillRound(dc, r, S(8), C_VOLTSOFT, C_VOLT);
    } else if (hov) {
        FillRound(dc, r, S(8), C_CANVAS, C_CANVAS, 0);
    }

    // 类型徽标 / 图片缩略图
    RECT ic{r.left + S(10), r.top + (S(ROW_H) - S(28)) / 2,
            r.left + S(10) + S(28), r.top + (S(ROW_H) - S(28)) / 2 + S(28)};
    if (!DrawThumb(dc, it, ic)) {
        FillRound(dc, ic, S(6), C_SUBTLE, C_SUBTLE, 0);
        RECT ii = ic; InflateRect(&ii, -S(7), -S(7));
        DrawIcon(dc, ItemIcon(it), ii, C_MUTED, 2.0f);
    }

    // 主文本两行：标题 / 副行（左「类型 · 来源」，右「大小 - 时间」，两端对齐）
    int lx = ic.right + S(11);
    int midY = (r.top + r.bottom) / 2;
    int rowRight = r.right - S(10);
    RECT titleR{lx, midY - S(18), rowRight, midY - S(1)};
    RECT metaR{lx, midY + S(1), rowRight, midY + S(16)};

    // 副行右侧：大小 - 相对时间（右对齐到行右缘）
    std::wstring rt = SizeText(it.bytes) + L" - " + RelTime(it.st);
    int tw = TextW(g_fMeta, rt);
    RECT rtR{rowRight - tw, metaR.top, rowRight, metaR.bottom};
    TextOutF(dc, g_fMeta, C_MUTED, rt, rtR, DT_SINGLELINE | DT_VCENTER | DT_RIGHT);

    int tx = lx;
    if (it.pinned) {
        int s = S(13);
        RECT pr{lx, midY - S(18) + (S(19) - s) / 2, lx + s, midY - S(18) + (S(19) - s) / 2 + s};
        DrawIcon(dc, IC_PIN, pr, C_VOLT, 2.4f);
        tx += s + S(5);
    }
    RECT tt = titleR; tt.left = tx;
    HFONT tf = it.isCode ? g_fMono : g_fTitle;
    TextOutF(dc, tf, C_INK, it.title, tt, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_LEFT);

    std::wstring meta = it.tag + L" · 来源：" + it.source;
    RECT mR = metaR; mR.right = rtR.left - S(8);
    // 面板压到很窄时右侧「大小 - 时间」会吃掉大半行宽，左边界必须夹住，
    // 否则 right < left 交给 DirectWrite 会画出乱码般的裁切结果
    if (mR.right < lx + S(20)) mR.right = lx + S(20);
    TextOutF(dc, g_fMeta, C_MUTED, meta, mR, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_LEFT);
}

void PaintEmpty(HDC dc) {
    int cx = L.W / 2;
    int top = L.body.top + (L.body.bottom - L.body.top) / 2 - S(90);
    RECT icx{cx - S(24), top, cx + S(24), top + S(48)};
    DrawIcon(dc, g_page == PG_FAV ? IC_STAR : IC_CLIP, icx, C_MUTED, 1.6f);

    const wchar_t* h3;
    const wchar_t* p;
    if (g_page == PG_FAV) {
        bool cleared = !g_query.empty() || g_filter != FL_ALL;
        h3 = cleared ? L"没有匹配的收藏" : L"还没有收藏";
        p  = cleared ? L"换个关键词，或调整筛选条件。"
                     : L"按 Ctrl+D 收藏当前条目，常用内容就会收进这里。";
    } else {
        h3 = L"没有匹配的剪贴板内容";
        p  = L"换个关键词，或调整筛选条件。";
    }
    RECT t1{0, top + S(62), L.W, top + S(62) + S(22)};
    TextOutF(dc, g_fBig, C_INK, h3, t1, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    RECT t2{0, t1.bottom + S(6), L.W, t1.bottom + S(6) + S(20)};
    TextOutF(dc, g_fMeta, C_MUTED, p, t2, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
}
HFONT PickSymFont(const std::wstring& g) {
    auto it = g_symFontSlot.find(g);
    int slot = (it != g_symFontSlot.end()) ? it->second : -1;
    if (slot < 0) {
        slot = 0;
        if (!GlyphFontCovers(g_probe[0], g)) {
            for (int i = 1; i < 4; ++i) {
                if (GlyphFontCovers(g_probe[i], g)) { slot = i; break; }
            }
        }
        g_symFontSlot[g] = slot;
    }
    return (slot == 0 || !g_fSymFallback[slot - 1]) ? g_fSym : g_fSymFallback[slot - 1];
}

void PaintGlyphEmpty(HDC dc, const wchar_t* title, const wchar_t* sub) {
    int cy = (L.body.top + L.body.bottom) / 2;
    RECT t{0, cy - S(26), L.W, cy - S(6)};
    TextOutF(dc, g_fBig, C_INK, title, t, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    RECT s{0, cy - S(4), L.W, cy + S(16)};
    TextOutF(dc, g_fMeta, C_MUTED, sub, s, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
}

// 当前可见的 emoji 格子：滚动位置定位到行首，滚出视口就停
void CollectEmojiCells(std::vector<std::pair<RECT, int>>& out) {
    out.clear();
    const std::vector<GItem>& list = GlyphItems(PG_EMOJI);
    if (list.empty()) return;
    const int cols = GlyphCols();
    const int step = S(44) + S(5);
    int firstRow = (g_gridScroll > S(8)) ? (g_gridScroll - S(8)) / step : 0;
    int start = firstRow * cols;
    if (start >= (int)list.size()) start = 0;
    out.reserve((size_t)((L.body.bottom - L.body.top) / step + 2) * (size_t)cols);
    for (int i = start; i < (int)list.size(); ++i) {
        RECT c = GlyphCellRect(i);
        if (c.top > L.body.bottom) break;
        if (c.bottom < L.body.top) continue;
        out.push_back({c, i});
    }
}

void PaintEmoji(HDC dc) {
    const std::vector<GItem>& list = GlyphItems(PG_EMOJI);
    if (list.empty()) {
        bool recent = g_query.empty() && CurGlyphCat(PG_EMOJI) <= 0;
        PaintGlyphEmpty(dc, recent ? L"还没有最近" : L"没有匹配的结果",
                        recent ? L"点任意表情后，它会出现在这里" : L"换个关键词试试");
        return;
    }

    std::vector<std::pair<RECT, int>> cells;
    CollectEmojiCells(cells);
    for (auto& c : cells)
        if (c.second == g_hoverCell) FillRound(dc, c.first, S(7), C_CANVAS, C_CANVAS, 0);

    const int sz = S(44);
    if (g_rt && g_painting && g_emojiFmt) {
        for (auto& c : cells) {
            const GItem& it = list[(size_t)c.second];
            int slot = -1;
            if (EmojiSlotFor(std::string(it.g, (size_t)it.gl), &slot) &&
                g_emoAtlas && EmojiSlotReady(slot)) {
                float x = (float)(c.first.left + c.first.right - sz) / 2.0f;
                float y = (float)c.first.top;
                D2D1_RECT_F sr = EmojiSlotRect(slot);
                g_rt->DrawBitmap(g_emoAtlas, D2D1::RectF(x, y, x + (float)sz, y + (float)sz),
                                 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, &sr);
            } else {
                // 还没就绪：只给一层极淡的剪影。以前这里画的是实心黑字形，
                // 于是「黑色->彩色」的跳变非常刺眼；淡影过渡基本看不出来。
                Brush(C_MUTED, 0.22f);
                std::wstring w = U8N(it.g, it.gl);
                g_rt->DrawTextW(w.c_str(), (UINT32)w.size(), g_emojiFmt,
                                D2D1::RectF((float)c.first.left, (float)c.first.top,
                                            (float)c.first.right, (float)c.first.bottom),
                                g_brush, D2D1_DRAW_TEXT_OPTIONS_NONE);
            }
        }
        return;
    }
    for (auto& c : cells)
        TextOutF(dc, g_fEmoji, C_INK, U8N(list[(size_t)c.second].g, list[(size_t)c.second].gl),
                 c.first, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
}

void PaintSymbols(HDC dc) {
    const std::vector<GItem>& list = GlyphItems(PG_SYMBOL);
    if (list.empty()) {
        bool recent = g_query.empty() && CurGlyphCat(PG_SYMBOL) <= 0;
        PaintGlyphEmpty(dc, recent ? L"还没有最近" : L"没有匹配的结果",
                        recent ? L"点任意符号后，它会出现在这里" : L"换个关键词或分类试试");
        return;
    }

    // 与表情页同一套网格：只画字形本身，不再有卡片底、边框和符号名称
    const int cols = GlyphCols();
    const int step = S(44) + S(5);
    int firstRow = (g_gridScroll > S(8)) ? (g_gridScroll - S(8)) / step : 0;
    int start = firstRow * cols;

    for (int i = start; i < (int)list.size(); ++i) {
        RECT c = GlyphCellRect(i);
        if (c.top > L.body.bottom) break;
        if (c.bottom < L.body.top) continue;
        if (i == g_hoverCell) FillRound(dc, c, S(7), C_CANVAS, C_CANVAS, 0);
        std::wstring g = U8N(list[i].g, list[i].gl);
        TextOutF(dc, PickSymFont(g), C_INK, g, c, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    }
}

// 细滚动条（对应 CSS 的 8px thumb）
int GridContentH();

void PaintScrollbar(HDC dc) {
    int contentH = 0, viewH = L.body.bottom - L.body.top, offset = 0;
    if (g_page == PG_HISTORY || g_page == PG_FAV) {
        contentH = S(8) + (int)g_view.size() * S(ROW_H) + S(12);
        offset = g_top * S(ROW_H);
    } else {
        contentH = GridContentH();
        offset = g_gridScroll;
    }
    if (contentH <= viewH) return;

    int trackTop = L.body.top + S(4);
    int trackBot = L.body.bottom - S(4);
    int trackH = trackBot - trackTop;
    int thumbH = (std::max)(S(28), (int)((long long)trackH * viewH / contentH));
    int maxOffset = contentH - viewH;
    if (maxOffset < 1) maxOffset = 1;
    int thumbTop = trackTop + (int)((long long)(trackH - thumbH) * offset / maxOffset);
    if (thumbTop < trackTop) thumbTop = trackTop;
    if (thumbTop + thumbH > trackBot) thumbTop = trackBot - thumbH;

    RECT tb{L.W - S(8), thumbTop, L.W - S(3), thumbTop + thumbH};
    FillRound(dc, tb, S(2), RGB(196, 190, 181), RGB(196, 190, 181), 0);
}

// ---------------------------------------------------------------- 分类抽屉绘制
void PaintDrawer(HDC dc) {
    FillBox(dc, L.drawer, C_SURFACE);
    HLine(dc, 0, L.W, L.drawer.top, C_LINE);
    FillBox(dc, L.drawerHead, C_CANVAS);
    HLine(dc, 0, L.W, L.drawerHead.bottom - 1, C_LINE);

    int total = GlyphCatTotal(g_page) + (RecentEmpty(g_page) ? 0 : 1);   // +1 = 「最近」
    std::wstring title = L"全部分类 · " + std::to_wstring(total) + L" 类";
    RECT tr{S(16), L.drawerHead.top, L.W - S(70), L.drawerHead.bottom};
    TextOutF(dc, g_fChip, C_MUTED, title, tr, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
    TextOutF(dc, g_fChip, C_MUTED, L"关闭", L.drawerClose, DT_SINGLELINE | DT_VCENTER | DT_RIGHT);

    if (g_rt && g_painting)
        g_rt->PushAxisAlignedClip(
            D2D1::RectF(0.0f, (float)L.drawerHead.bottom, (float)L.W, (float)L.drawer.bottom),
            D2D1_ANTIALIAS_MODE_ALIASED);

    for (const auto& s : L.dSec) {
        if (s.first.top > L.drawer.bottom) break;
        if (s.first.bottom < L.drawerHead.bottom) continue;
        TextOutF(dc, g_fSymName, C_MUTED, s.second, s.first, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
    }

    int active = CurGlyphCat(g_page);
    for (size_t i = 0; i < L.dRect.size(); ++i) {
        RECT r = L.dRect[i];
        if (r.top > L.drawer.bottom) break;
        if (r.bottom < L.drawerHead.bottom) continue;
        int cat = L.dCat[i];
        bool on = (cat == active);
        bool hov = ((int)i == g_drawerHover);
        FillRound(dc, r, S(7), on ? C_VOLTSOFT : (hov ? C_CANVAS : C_SURFACE),
                  on ? C_VOLT : (hov ? C_MUTED : C_LINE));
        RECT gr{r.left + S(6), r.top, r.left + S(22), r.bottom};
        TextOutF(dc, g_fSymName, on ? C_INK : C_MUTED, CatFirstGlyph(g_page, cat), gr,
                 DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        RECT nr{r.left + S(24), r.top, r.right - S(6), r.bottom};
        TextOutF(dc, g_fChip, C_INK, GlyphCatName(g_page, cat), nr,
                 DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
    }

    if (g_rt && g_painting) g_rt->PopAxisAlignedClip();

    // 抽屉自己的滚动条
    int viewH = L.drawer.bottom - (L.drawerHead.bottom + S(10));
    if (g_drawerContentH > viewH) {
        int trackTop = L.drawerHead.bottom + S(6), trackBot = L.drawer.bottom - S(6);
        int trackH = trackBot - trackTop;
        int thumbH = (std::max)(S(28), (int)((long long)trackH * viewH / g_drawerContentH));
        int maxS = DrawerMaxScroll();
        int thumbTop = trackTop + (maxS > 0 ? (int)((long long)(trackH - thumbH) * g_drawerScroll / maxS) : 0);
        if (thumbTop + thumbH > trackBot) thumbTop = trackBot - thumbH;
        RECT tb{L.W - S(8), thumbTop, L.W - S(3), thumbTop + thumbH};
        FillRound(dc, tb, S(2), RGB(196, 190, 181), RGB(196, 190, 181), 0);
    }
}

void PaintFoot(HDC dc) {
    FillBox(dc, L.foot, C_CANVAS);
    HLine(dc, 0, L.W, L.foot.top, C_LINE);

    struct H { const wchar_t* key; const wchar_t* label; };
    H hints[] = {{L"↑ ↓", L"选择"}, {L"Enter", L"粘贴"}, {L"P", L"置顶"}, {L"Esc", L"关闭"}};

    // 右侧计数先算出来，提示串才知道自己能用多宽
    std::wstring cnt;
    if (g_page == PG_EMOJI) {
        cnt = std::to_wstring(PageCount()) + L" 个表情";
    } else if (g_page == PG_SYMBOL) {
        cnt = std::to_wstring(PageCount()) + L" 个符号";
    } else {
        int pinned = 0, fav = 0;
        for (int i : g_view) { if (g_items[i].pinned) ++pinned; if (g_items[i].fav) ++fav; }
        cnt = std::to_wstring(g_view.size()) + L" 条";
        if (g_page == PG_HISTORY) {
            cnt += L" · " + std::to_wstring(pinned) + L" 置顶";
            if (fav) cnt += L" · " + std::to_wstring(fav) + L" 收藏";
        }
    }
    int cntW = TextW(g_fMeta, cnt);
    int limit = L.W - S(16) - cntW - S(12);   // 提示串右边界：留 12px 不和计数挨上

    int x = S(16);
    int cy = L.foot.top + S(FOOT_H) / 2;
    for (auto& h : hints) {
        int kw = TextW(g_fKbd, h.key) + S(10);
        int lw = TextW(g_fMeta, h.label);
        // 窄面板装不下全部提示，放不下的直接不画（右侧计数优先保留）
        if (x + kw + S(5) + lw + S(2) > limit) break;
        RECT kr{x, cy - S(9), x + kw, cy + S(9)};
        FillRound(dc, kr, S(4), C_SURFACE, C_LINE);
        TextOutF(dc, g_fKbd, C_INK, h.key, kr, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        int lx = kr.right + S(5);
        RECT lr{lx, L.foot.top, lx + lw + S(2), L.foot.bottom};
        TextOutF(dc, g_fMeta, C_MUTED, h.label, lr, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        x = lr.right + S(14);
    }

    RECT cr{S(16), L.foot.top, L.W - S(16), L.foot.bottom};
    TextOutF(dc, g_fMeta, C_MUTED, cnt, cr, DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
}

void PaintToast(HDC dc) {
    if (!g_toastOn) return;
    int w = TextW(g_fToast, g_toast) + S(24) + S(22);
    RECT t{L.W - S(14) - w, S(56), L.W - S(14), S(56) + S(32)};
    FillRound(dc, t, S(8), C_INK, C_INK, 0);
    RECT ti{t.left + S(10), t.top + (S(32) - S(15)) / 2,
            t.left + S(10) + S(15), t.top + (S(32) - S(15)) / 2 + S(15)};
    DrawIcon(dc, g_toastInfo ? IC_MORE : IC_CHECK, ti, g_toastInfo ? C_VOLT : C_SUCCESS, 2.2f);
    RECT tt{ti.right + S(7), t.top, t.right - S(10), t.bottom};
    TextOutF(dc, g_fToast, C_SURFACE, g_toast, tt, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
}

// 菜单：置顶（文案随状态变）/ 另存为 /（分隔线）删除
const wchar_t* MenuLabel(int idx, const ClipItem* it) {
    switch (idx) {
    case MI_PIN:  return (it && it->pinned) ? L"取消置顶" : L"置顶";
    case MI_SAVE: return L"另存为…";
    default:      return L"删除";
    }
}

void PaintMenu(HDC dc) {
    if (!g_menuOpen) return;
    RECT m = MenuRect();
    FillRound(dc, m, S(8), C_SURFACE, C_LINE);

    // 分隔线落在「删除」上方的留白里
    int sepY = MenuItemRect(MENU_SEP_BEFORE).top - S(MENU_SEP_H / 2);
    HLine(dc, m.left + S(10), m.right - S(10), sepY, C_LINE);

    const ClipItem* it = (g_menuItem >= 0 && g_menuItem < (int)g_items.size())
                         ? &g_items[g_menuItem] : nullptr;
    for (int i = 0; i < MI_COUNT; ++i) {
        RECT ir = MenuItemRect(i);
        if (g_menuHover == i) FillRound(dc, ir, S(6), C_CANVAS, C_CANVAS, 0);
        RECT ii{ir.left + S(9), ir.top + (S(34) - S(15)) / 2,
                ir.left + S(9) + S(15), ir.top + (S(34) - S(15)) / 2 + S(15)};
        COLORREF tc = (i == MI_DELETE) ? C_DANGER : C_INK;
        IconId ic = (i == MI_PIN) ? IC_PIN : (i == MI_SAVE ? IC_SAVE : IC_TRASH);
        DrawIcon(dc, ic, ii, tc, 1.9f);
        RECT tr{ii.right + S(9), ir.top, ir.right - S(8), ir.bottom};
        TextOutF(dc, g_fChip, tc, MenuLabel(i, it), tr, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
    }
}

void PaintModal(HDC dc) {
    if (!g_modalOpen) return;
    // 遮罩：rgba(32,37,43,.28) —— D2D 直接做 source-over，比手工拼预乘 DIB 更准
    if (g_rt && g_painting) {
        Brush(C_INK, 0.28f);
        g_rt->FillRectangle(D2D1::RectF(0, 0, (float)L.W, (float)L.H), g_brush);
    }

    RECT m = ModalRect();
    FillRound(dc, m, S(8), C_SURFACE, C_LINE);

    RECT h3{m.left + S(20), m.top + S(20), m.right - S(20), m.top + S(20) + S(22)};
    TextOutF(dc, g_fBig, C_INK, L"删除这条剪贴板项目？", h3,
             DT_SINGLELINE | DT_VCENTER | DT_LEFT);
    RECT p{m.left + S(20), h3.bottom + S(6), m.right - S(20), h3.bottom + S(6) + S(20)};
    TextOutF(dc, g_fMeta, C_MUTED, L"删除后无法恢复。", p, DT_SINGLELINE | DT_VCENTER | DT_LEFT);

    int bw = (m.right - m.left - S(40) - S(8)) / 2;
    int by = m.bottom - S(20) - S(38);
    RECT ok{m.left + S(20), by, m.left + S(20) + bw, by + S(38)};
    RECT cc{ok.right + S(8), by, ok.right + S(8) + bw, by + S(38)};
    FillRound(dc, ok, S(8), C_SURFACE, C_DANGER);
    FillRound(dc, cc, S(8), C_SURFACE, C_LINE);
    TextOutF(dc, g_fChip, C_DANGER, L"删除", ok, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    TextOutF(dc, g_fChip, C_INK, L"取消", cc, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
}

// PerfAppend / PerfInit 见 diag.cpp；下面是带业务语义的汇总输出。
void PerfFlush() {
    if (g_perfAcc.n <= 0) return;
    char buf[512];
    int n = g_perfAcc.n;
    std::snprintf(buf, sizeof(buf),
                  "page=%d n=%d begin=%.2f prep=%.2f head=%.2f body=%.2f foot=%.2f end=%.2f "
                  "blit=%.2f total=%.2f max=%.2f ms\n",
                  g_page, n, PerfMs(g_perfAcc.begin) / n, PerfMs(g_perfAcc.prep) / n,
                  PerfMs(g_perfAcc.head) / n, PerfMs(g_perfAcc.body) / n,
                  PerfMs(g_perfAcc.foot) / n, PerfMs(g_perfAcc.end) / n,
                  PerfMs(g_perfAcc.blit) / n,
                  PerfMs(g_perfAcc.total) / n, PerfMs(g_perfAcc.maxTotal));
    PerfAppend(buf);
    {
        int newN = 0, hitN = 0, failN = 0, nextSlot = 0, slots = 0;
        EmojiPerfSnapshot(&newN, &hitN, &failN, &nextSlot, &slots);
        char b2[256];
        std::snprintf(b2, sizeof(b2), "  emoji new=%d hit=%d fail=%d slots=%d/%d\n",
                      newN, hitN, failN, nextSlot, slots);
        PerfAppend(b2);
        EmojiPerfReset();
    }
    g_perfAcc = PerfAcc{};
}

// 离屏缓冲复用：每帧重建 560×620 的 DIBSection 是最大的固定开销（实测 2.5~3ms/帧），
// 尺寸不变就一直用同一块；DCRenderTarget 绑定的位图在 BeginDraw 时不会被清空，
// 所以保留上一帧内容还能配合「只重绘脏区」。
static HDC     g_bbDc = nullptr;
static HBITMAP g_bbBmp = nullptr;
static int     g_bbW = 0, g_bbH = 0;

bool AcquireBackBuffer(HDC ref, int W, int H) {
    if (g_bbDc && g_bbW == W && g_bbH == H) return true;
    if (g_bbBmp) { DeleteObject(g_bbBmp); g_bbBmp = nullptr; }
    if (g_bbDc)  { DeleteDC(g_bbDc); g_bbDc = nullptr; }
    g_bbDc = CreateCompatibleDC(ref);
    if (!g_bbDc) return false;
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = W;
    bmi.bmiHeader.biHeight = -H;      // 自上而下，坐标与窗口一致
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    g_bbBmp = CreateDIBSection(ref, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!g_bbBmp) { DeleteDC(g_bbDc); g_bbDc = nullptr; return false; }
    SelectObject(g_bbDc, g_bbBmp);    // 自己的 DC，不用还原
    g_bbW = W; g_bbH = H;
    g_needBind = true;
    return true;
}

bool RectsOverlap(const RECT& a, const RECT& b) {
    return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}

void PaintPanel(HWND hwnd) {
    LONGLONG tb = g_perfOn ? PerfNow() : 0;
    PAINTSTRUCT ps;
    HDC dc0 = BeginPaint(hwnd, &ps);
    RECT rc; GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;
    LONGLONG t0 = g_perfOn ? PerfNow() : 0;

    // 新建缓冲时内容未定义，必须整窗重画
    bool fresh = !(g_bbDc && g_bbW == W && g_bbH == H);
    if (!AcquireBackBuffer(dc0, W, H)) { EndPaint(hwnd, &ps); return; }
    HDC dc = g_bbDc;
    RECT dirty = fresh ? rc : ps.rcPaint;
    if (IsRectEmpty(&dirty)) { EndPaint(hwnd, &ps); return; }

    // 缩略图必须在进 D2D 批次之前建好：CreateBitmap 是创建设备资源的操作，
    // 放在 BeginDraw/EndDraw 之间会触发目标内部的 flush
    if (g_rt && (g_page == PG_HISTORY || g_page == PG_FAV)) {
        for (int i = 0; i < L.nRow; ++i) {
            int vi = g_top + i;
            if (vi >= (int)g_view.size()) break;
            GetThumb(g_items[g_view[vi]]);
        }
    }
    // emoji 位图缓存同理：先补全可见格，再进 D2D 批次
    if (g_rt && g_page == PG_EMOJI && RectsOverlap(dirty, L.body)) PrepareEmojiBitmaps();

    PaintBegin(dc, W, H, &dirty);
    LONGLONG t1 = g_perfOn ? PerfNow() : 0;

    // 背景：只填脏区
    FillBox(dc, dirty, C_SURFACE);

    // 脏区之外的绘制整段跳过：脏区 clip 只能省光栅化，DWrite 的排版/字形解析
    // 在提交前就要做，跳过调用才是真省
    RECT topBand{0, 0, W, L.filters.bottom};
    if (RectsOverlap(dirty, topBand)) {
        PaintHead(dc);
        PaintSearch(dc);
        PaintFilters(dc);
    }
    LONGLONG t2 = g_perfOn ? PerfNow() : 0;

    if (RectsOverlap(dirty, L.body)) {
        // 主体（裁剪）
        if (g_rt && g_painting)
            g_rt->PushAxisAlignedClip(
                D2D1::RectF((float)L.body.left, (float)L.body.top,
                            (float)L.body.right, (float)L.body.bottom),
                D2D1_ANTIALIAS_MODE_ALIASED);
        FillBox(dc, L.body, C_SURFACE);

        if (g_page == PG_HISTORY || g_page == PG_FAV) {
            if (g_view.empty()) PaintEmpty(dc);
            else for (int i = 0; i < L.nRow; ++i) PaintRow(dc, i);
        } else if (g_page == PG_EMOJI) {
            PaintEmoji(dc);
        } else {
            PaintSymbols(dc);
        }
        PaintScrollbar(dc);
        if (g_rt && g_painting) g_rt->PopAxisAlignedClip();

        if (g_drawerOpen && (g_page == PG_EMOJI || g_page == PG_SYMBOL)) PaintDrawer(dc);
    }
    LONGLONG t3 = g_perfOn ? PerfNow() : 0;

    if (RectsOverlap(dirty, L.foot)) PaintFoot(dc);
    PaintToast(dc);
    PaintMenu(dc);
    PaintModal(dc);
    LONGLONG t4 = g_perfOn ? PerfNow() : 0;

    // 外边框（1px 线压在窗口内侧，对齐像素中心才不会糊）
    if (g_rt && g_painting) {
        float rad = S(RADIUS) / 2.0f;
        Brush(C_LINE);
        g_rt->DrawRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, (float)W - 0.5f, (float)H - 0.5f), rad, rad),
            g_brush, 1.0f);
    }

    PaintHeadButtons(dc);      // 压在外框线之上，让按钮高亮贴住窗口边缘

    PaintEnd();
    LONGLONG t5 = g_perfOn ? PerfNow() : 0;

    BitBlt(dc0, 0, 0, W, H, dc, 0, 0, SRCCOPY);
    LONGLONG t6 = g_perfOn ? PerfNow() : 0;

    EndPaint(hwnd, &ps);

    if (g_perfOn) {
        g_perfAcc.begin += t0 - tb;      // BeginPaint + GetClientRect + 缩略图预建
        g_perfAcc.prep += t1 - t0;
        g_perfAcc.head += t2 - t1;
        g_perfAcc.body += t3 - t2;
        g_perfAcc.foot += t4 - t3;
        g_perfAcc.end  += t5 - t4;
        g_perfAcc.blit += t6 - t5;
        LONGLONG tot = t6 - t0;
        g_perfAcc.total += tot;
        if (tot > g_perfAcc.maxTotal) g_perfAcc.maxTotal = tot;
        if (++g_perfAcc.n >= 30) PerfFlush();
    }
}