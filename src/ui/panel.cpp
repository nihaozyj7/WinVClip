// panel.cpp —— 实现见 panel.h。
//
// PanelProc 有 490 行、20 多个 case，看着长，但它是一个 switch：
// 每个 case 的职责就是「判断点到了哪」然后调 ui/actions 里的一个函数。
// 真正的逻辑都在 actions / layout / showhide 里，这里保持扁平是有意的——
// 拆成十几个 WndProc 回调只会让「这个行为在哪处理」更难找。
#include "ui/panel.h"

#include <imm.h>
#include <windowsx.h>

#include "core/diag.h"   // Trace

#include "clipboard/capture.h"
#include "clipboard/paste.h"
#include "core/app_state.h"
#include "core/model.h"
#include "core/text_measure.h"
#include "core/text_util.h"
#include "core/theme.h"
#include "core/util.h"
#include "glyph/emoji_atlas.h"
#include "glyph/glyph.h"
#include "model/clip_model.h"
#include "ui/actions.h"
#include "ui/caret.h"
#include "ui/layout.h"
#include "ui/paint.h"
#include "ui/showhide.h"

using namespace wv;

// ---------------------------------------------------------------- 尺寸调整
// 不用系统的 HTLEFT/HTBOTTOMRIGHT：那是模态 sizing 循环，一进去面板会被判失活而自动收起
// （和标题栏拖动踩的是同一个坑）。所以自己 SetCapture 算差值。
void RoundCorners(HWND hwnd);

int HitResize(POINT p) {
    if (!g_panel) return RS_NONE;
    RECT rc; GetClientRect(g_panel, &rc);
    int e = S(RESIZE_EDGE);
    bool inL = p.x < e, inR = p.x >= rc.right - e;
    bool inT = p.y < e, inB = p.y >= rc.bottom - e;
    if (inT && inL) return RS_TL;
    if (inT && inR) return RS_TR;
    if (inB && inL) return RS_BL;
    if (inB && inR) return RS_BR;
    if (inL) return RS_L;
    if (inR) return RS_R;
    if (inT) return RS_T;
    if (inB) return RS_B;
    return RS_NONE;
}

// 夹最小尺寸时「正在拖的那条边不动、对面的边动」，否则拖到下限时
// 窗口会从另一侧往回缩，手感很怪
RECT ClampToMin(RECT r) {
    int minW = S(MIN_W), minH = S(MIN_H);
    bool leftSide = (g_resizing == RS_L || g_resizing == RS_TL || g_resizing == RS_BL);
    bool topSide  = (g_resizing == RS_T || g_resizing == RS_TL || g_resizing == RS_TR);
    if (r.right - r.left < minW) {
        if (leftSide) r.left = r.right - minW; else r.right = r.left + minW;
    }
    if (r.bottom - r.top < minH) {
        if (topSide) r.top = r.bottom - minH; else r.bottom = r.top + minH;
    }
    return r;
}

void ApplyResize(RECT r) {
    r = ClampToMin(r);
    HMONITOR mon = MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{}; mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi)) {
        if (r.left   < mi.rcWork.left)   r.left   = mi.rcWork.left;
        if (r.top    < mi.rcWork.top)    r.top    = mi.rcWork.top;
        if (r.right  > mi.rcWork.right)  r.right  = mi.rcWork.right;
        if (r.bottom > mi.rcWork.bottom) r.bottom = mi.rcWork.bottom;
        r = ClampToMin(r);
    }
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w == g_panelW && h == g_panelH) {
        RECT cur; GetWindowRect(g_panel, &cur);
        if (cur.left == r.left && cur.top == r.top) return;
    }
    SetWindowPos(g_panel, nullptr, r.left, r.top, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    g_panelW = w; g_panelH = h;
    RoundCorners(g_panel);        // 圆角 region 按新尺寸重做
    ComputeLayout();
    ClampGridScroll();
    ClampScroll();
    Redraw();
}

void WheelBody(int delta) {
    if (g_drawerOpen) {
        g_drawerScroll += (delta > 0) ? -S(48) : S(48);
        ClampDrawerScroll();
        ComputeLayout();
        Redraw();
        return;
    }
    if (g_page == PG_HISTORY || g_page == PG_FAV) {
        if (delta > 0) { if (g_top > 0) --g_top; }
        else { int maxTop = (int)g_view.size() - L.nRow; if (g_top < maxTop) ++g_top; }
    } else {
        int step = S(56);
        g_gridScroll += (delta > 0) ? -step : step;
        ClampGridScroll();
    }
    Redraw(&L.body);      // 滚动只影响列表区，外框/标题/页脚保持原样
}

// ================================================================== 窗口过程
// 面板的按键处理。两个入口共用这一份逻辑：
//   · 输入态（用户点过搜索框，面板持有焦点）→ 系统投递的 WM_KEYDOWN；
//   · 非激活态（常态）→ 低级键盘钩子转发来的 WM_APP_PANELKEY。
void PanelKeyDown(int vk, bool ctrl) {

        // 输入法正在组合（g_comp 非空）：按键是给输入法的（空格上屏、Esc 取消、
        // 数字选候选），别让它们顺带触发面板的快捷键——尤其 Esc，否则想取消
        // 组合反而把面板关了。
        if (!g_comp.empty()) return;

        if (g_modalOpen) {
            if (vk == VK_ESCAPE) { g_modalOpen = false; Redraw(); return; }
            if (vk == VK_RETURN) {
                if (g_modalRow >= 0 && g_modalRow < L.nRow && g_top + g_modalRow < (int)g_view.size()) {
                    g_items.erase(g_items.begin() + g_view[g_top + g_modalRow]);
                    g_sel = 0; g_top = 0;
                }
                g_modalOpen = false;
                BuildView(); ComputeLayout();
                Toast(L"已删除", true);
                Redraw();
                return;
            }
            return;
        }
        if (g_menuOpen) {
            if (vk == VK_ESCAPE) { g_menuOpen = false; Redraw(); return; }
            if (vk == VK_DOWN || vk == VK_UP) {
                int d = (vk == VK_DOWN) ? 1 : MI_COUNT - 1;
                g_menuHover = (g_menuHover < 0) ? 0 : (g_menuHover + d) % MI_COUNT;
                Redraw();
                return;
            }
            if (vk == VK_RETURN) {
                RunMenuItem(g_menuHover < 0 ? 0 : g_menuHover);
                return;
            }
        }

        if (ctrl) {
            if (vk >= '1' && vk <= '4') { SetPage(vk - '1'); return; }
            if (vk == 'P') { TogglePinItem(SelItem()); return; }
            if (vk == 'D') { ToggleFav(SelView()); return; }
            if (vk == 'S') { SaveItemAs(SelItem()); return; }
            return;
        }

        switch (vk) {
        case VK_ESCAPE:
            if (g_drawerOpen) { ToggleDrawer(false); return; }
            HidePanel();
            return;
        case VK_RETURN:
            if (g_page == PG_HISTORY || g_page == PG_FAV) PasteItem(SelView());
            return;
        case VK_UP:
            if (g_page == PG_HISTORY || g_page == PG_FAV) {
                if (g_sel > 0) { --g_sel; ClampScroll(); Redraw(); }
            }
            return;
        case VK_DOWN:
            if (g_page == PG_HISTORY || g_page == PG_FAV) {
                if (g_sel + 1 < (int)g_view.size()) { ++g_sel; ClampScroll(); Redraw(); }
            }
            return;
        case VK_PRIOR:
            if (g_page == PG_HISTORY || g_page == PG_FAV) {
                g_sel = (std::max)(0, g_sel - L.nRow);
                ClampScroll(); Redraw();
            }
            return;
        case VK_NEXT:
            if (g_page == PG_HISTORY || g_page == PG_FAV) {
                g_sel = (std::min)((int)g_view.size() - 1, g_sel + L.nRow);
                ClampScroll(); Redraw();
            }
            return;
        case VK_BACK:
            if (!g_query.empty()) {
                g_query.pop_back();
                g_sel = 0; g_top = 0;
                BuildView(); ComputeLayout(); Redraw();
            }
            return;
        case VK_DELETE:
            if (g_page == PG_HISTORY || g_page == PG_FAV) {
                g_modalOpen = true;
                g_modalRow = g_sel - g_top;
                Redraw();
            }
            return;
        case VK_TAB:
            SetPage((g_page + 1) % PG_COUNT);
            return;
        }
}

// 字符输入：同样两个入口（WM_CHAR / 钩子转发的字符）
void PanelChar(wchar_t c) {
    if (c >= 32 && c != 127) {
        Trace("panel char '%c' query=%ls", (char)c, g_query.c_str());
        g_query.push_back(c);
        g_sel = 0; g_top = 0;
        BuildView(); ComputeLayout();
        Redraw();
    }
}

LRESULT CALLBACK PanelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        PaintPanel(hwnd);
        return 0;

    case WM_TIMER:
        if (wp == TIMER_TOAST) {
            KillTimer(hwnd, TIMER_TOAST);
            g_toastOn = false;
            Redraw();
        } else if (wp == TIMER_CARET) {
            g_caretOn = !g_caretOn;
            if (g_page == PG_HISTORY || g_page == PG_FAV || g_page == PG_EMOJI || g_page == PG_SYMBOL) {
                RECT r = L.searchBox;
                r.top -= S(5); r.bottom += S(5); r.left -= S(4); r.right += S(4);
                Redraw(&r);      // 光标只管搜索框那一块
            } else {
                Redraw();
            }
        } else if (wp == TIMER_EMOJI) {
            KillTimer(hwnd, TIMER_EMOJI);
            // 预热/补齐都要推进，哪怕面板此刻不在表情页、甚至不可见
            bool more = PumpEmojiUploads();
            if (g_page == PG_EMOJI) Redraw(&L.body);
            if (more) SetTimer(hwnd, TIMER_EMOJI, 8, nullptr);
        } else if (wp == TIMER_REPROBE) {
            // 补探测：首轮没拿到插入符时的第二次机会。用户动过面板就不再挪。
            KillTimer(hwnd, TIMER_REPROBE);
            if (g_posUserSet || g_pinned || g_dragging || !g_target || !IsWindow(g_target)) return 0;
            if (GetForegroundWindow() != g_target) return 0;
            CaretInfo ci;
            if (ProbeCaret(g_target, &ci) && !ci.approx) {
                MovePanelToCaret(ci);
                g_reprobeLeft = 0;
            } else if (--g_reprobeLeft > 0) {
                SetTimer(hwnd, TIMER_REPROBE, 160, nullptr);
            }
        } else if (wp == TIMER_FOCUS) {
            // 轮询前台窗口。非激活态下面板收不到 WA_INACTIVE（它本来就没被激活过），
            // 所以「用户有没有切走」只能靠轮询看前台是谁，见 PanelShouldStay。
            if (!g_pinned && !g_dragging && !g_dialogOpen) {
                if (!PanelShouldStay(hwnd)) {
                    g_menuOpen = false;
                    g_modalOpen = false;
                    HidePanel();
                } else if (g_inputMode && GetForegroundWindow() != hwnd) {
                    // 用户从输入态点回了目标窗口：退出输入态，回到钩子转发模式
                    g_inputMode = false;
                }
            }
        } else if (wp == TIMER_DEFERHIDE) {
            // WA_INACTIVE 那一瞬间前台还没落定，见 WM_ACTIVATE 里的说明：
            // 等它落定再判，否则收起时的「还前台」会和用户那次点击抢。
            KillTimer(hwnd, TIMER_DEFERHIDE);
            PostMessageW(hwnd, WM_APP_DEFER_HIDE, 0, 0);
        }
        return 0;

    case WM_KEYDOWN:
        PanelKeyDown((int)wp, (GetKeyState(VK_CONTROL) & 0x8000) != 0);
        return 0;

    // 非激活态下按键由钩子转发到这里（面板没有键盘焦点，见 LLKeyboard）
    case WM_APP_PANELKEY:
        Trace("panelkey vk=0x%02X ch=%04X", (unsigned)wp, (unsigned)HIWORD(lp));
        PanelKeyDown((int)wp, LOWORD(lp) != 0);
        if (HIWORD(lp)) PanelChar((wchar_t)HIWORD(lp));
        return 0;

    case WM_CHAR:
        PanelChar((wchar_t)wp);
        return 0;
    // ---------------------------------------------------------- 输入法
    // 搜索框是自绘的，系统不知道光标在哪：组合串自己收、自己画，
    // 并把候选窗钉到光标下方。不处理这些消息的话，中文一个字也打不进来。
    case WM_IME_SETCONTEXT:
        lp &= ~(LPARAM)ISC_SHOWUICOMPOSITIONWINDOW;   // 组合串由面板自己画
        break;                                        // 其余标志交默认处理
    case WM_IME_STARTCOMPOSITION:
        // 不能在这里 return 0 拦掉：DefWindowProc 要借此建立组合窗口，
        // 拦了以后输入法状态机不完整，连空格上屏都不认。
        // 显示已经在 WM_IME_SETCONTEXT 里屏蔽掉，画面由面板自己画。
        g_comp.clear();
        PositionImeCandidate(hwnd);
        break;
    case WM_IME_COMPOSITION: {
        HIMC himc = ImmGetContext(hwnd);
        if (himc) {
            if (lp & GCS_RESULTSTR) {                 // 上屏：并入查询串
                LONG n = ImmGetCompositionStringW(himc, GCS_RESULTSTR, nullptr, 0);
                if (n > 0) {
                    std::wstring s((size_t)n / sizeof(wchar_t), L'\0');
                    ImmGetCompositionStringW(himc, GCS_RESULTSTR, &s[0], n);
                    g_query += s;
                }
                g_comp.clear();
            }
            if (lp & GCS_COMPSTR) {                   // 仍在组合：只做预览
                LONG n = ImmGetCompositionStringW(himc, GCS_COMPSTR, nullptr, 0);
                if (n > 0) {
                    g_comp.assign((size_t)n / sizeof(wchar_t), L'\0');
                    ImmGetCompositionStringW(himc, GCS_COMPSTR, &g_comp[0], n);
                } else {
                    g_comp.clear();
                }
            }
            ImmReleaseContext(hwnd, himc);
        }
        g_sel = 0; g_top = 0;
        BuildView(); ComputeLayout();
        PositionImeCandidate(hwnd);
        Redraw();
        return 0;
    }
    case WM_IME_ENDCOMPOSITION:
        g_comp.clear();
        Redraw();
        return 0;

    // 边缘显示缩放光标；标题栏空白处显示四向箭头，暗示可拖动
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(hwnd, &p);
            switch (HitResize(p)) {
            case RS_L: case RS_R:
                SetCursor(LoadCursorW(nullptr, IDC_SIZEWE)); return TRUE;
            case RS_T: case RS_B:
                SetCursor(LoadCursorW(nullptr, IDC_SIZENS)); return TRUE;
            case RS_TL: case RS_BR:
                SetCursor(LoadCursorW(nullptr, IDC_SIZENWSE)); return TRUE;
            case RS_TR: case RS_BL:
                SetCursor(LoadCursorW(nullptr, IDC_SIZENESW)); return TRUE;
            default: break;
            }
            bool blank = PtIn(L.head, p) && !PtIn(L.gear, p) && !PtIn(L.closeB, p) &&
                         !PtIn(L.pinB, p);
            for (int i = 0; i < 4 && blank; ++i)
                if (PtIn(L.tab[i], p)) blank = false;
            SetCursor(LoadCursorW(nullptr, blank ? IDC_SIZEALL : IDC_ARROW));
            return TRUE;
        }
        break;

    case WM_LBUTTONUP:
        if (g_resizing) {
            g_resizing = RS_NONE;
            ReleaseCapture();
            SaveViewSize();      // 尺寸落盘，下次打开沿用（重启也记得）
            Redraw();
            return 0;
        }
        if (g_dragging) {
            g_dragging = false;
            ReleaseCapture();
            Redraw();
            return 0;
        }
        return 0;

    case WM_MOUSEMOVE: {
        // 调整尺寸中：屏幕坐标差直接改窗口矩形
        if (g_resizing) {
            POINT sc{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ClientToScreen(hwnd, &sc);
            int dx = sc.x - g_rsFrom.x, dy = sc.y - g_rsFrom.y;
            RECT r = g_rsRect;
            bool l = (g_resizing == RS_L || g_resizing == RS_TL || g_resizing == RS_BL);
            bool rr = (g_resizing == RS_R || g_resizing == RS_TR || g_resizing == RS_BR);
            bool t = (g_resizing == RS_T || g_resizing == RS_TL || g_resizing == RS_TR);
            bool b = (g_resizing == RS_B || g_resizing == RS_BL || g_resizing == RS_BR);
            if (l) r.left += dx;
            if (rr) r.right += dx;
            if (t) r.top += dy;
            if (b) r.bottom += dy;
            ApplyResize(r);
            return 0;
        }
        // 拖动中：屏幕坐标差直接搬窗口
        if (g_dragging) {
            POINT sc{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ClientToScreen(hwnd, &sc);
            int nx = g_dragRect.left + (sc.x - g_dragFrom.x);
            int ny = g_dragRect.top + (sc.y - g_dragFrom.y);
            SetWindowPos(hwnd, nullptr, nx, ny, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        int oldChip = g_hoverChip, oldRow = g_hoverRow, oldCell = g_hoverCell;

        g_hoverChip = -1; g_hoverRow = -1; g_hoverCell = -1;

        if (PtIn(L.closeB, POINT{x, y})) g_hoverChip = 901;
        else if (PtIn(L.gear, POINT{x, y})) g_hoverChip = 900;
        else if (PtIn(L.pinB, POINT{x, y})) g_hoverChip = 902;
        else {
            for (int i = 0; i < L.nChip; ++i)
                if (PtIn(L.chip[i], POINT{x, y})) { g_hoverChip = i; break; }
            if (g_hoverChip < 0 && L.moreBtn.right > L.moreBtn.left && PtIn(L.moreBtn, POINT{x, y}))
                g_hoverChip = 903;
        }

        if (PtIn(L.body, POINT{x, y})) {
            if (g_drawerOpen) {
                int oldDH = g_drawerHover;
                g_drawerHover = DrawerHit(POINT{x, y});
                if (oldDH != g_drawerHover) Redraw();
                return 0;
            }
            if (g_page == PG_HISTORY || g_page == PG_FAV) {
                for (int i = 0; i < L.nRow; ++i)
                    if (PtIn(L.row[i], POINT{x, y})) { g_hoverRow = g_top + i; break; }
            } else {
                // 表情与符号共用同一套网格几何
                const std::vector<GItem>& list = GlyphItems(g_page);
                // 从第一个可见行起查：滚到深处时不必从第 0 条空转
                int stepG = S(44) + S(5);
                int firstG = (g_gridScroll > S(8))
                             ? ((g_gridScroll - S(8)) / stepG) * GlyphCols() : 0;
                for (int i = firstG; i < (int)list.size(); ++i) {
                    RECT c = GlyphCellRect(i);
                    if (c.top > L.body.bottom) break;
                    if (PtIn(c, POINT{x, y})) { g_hoverCell = i; break; }
                }
            }
        }

        if (g_menuOpen) {
            int oldMH = g_menuHover;
            g_menuHover = MenuHit(POINT{x, y});
            if (oldMH != g_menuHover) Redraw();
        }

        if (oldChip != g_hoverChip || oldRow != g_hoverRow || oldCell != g_hoverCell) {
            if (oldRow != g_hoverRow && g_hoverRow >= 0) g_sel = g_hoverRow;
            // 悬停只改局部观感：分类条/标题按钮一行，或列表区一行，不必整窗重画
            if (oldRow != g_hoverRow || oldCell != g_hoverCell) Redraw(&L.body);
            if (oldChip != g_hoverChip) {
                RECT rr{0, L.head.top, L.W, L.filters.bottom};
                Redraw(&rr);
            }
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        g_hoverChip = g_hoverRow = g_hoverCell = -1;
        Redraw();
        return 0;

    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        POINT pt{x, y};
        ComputeLayout();

        // 弹层优先
        if (g_modalOpen) {
            RECT m = ModalRect();
            int bw = (m.right - m.left - S(40) - S(8)) / 2;
            int by = m.bottom - S(20) - S(38);
            RECT ok{m.left + S(20), by, m.left + S(20) + bw, by + S(38)};
            if (PtIn(ok, pt)) {
                if (g_modalRow >= 0 && g_modalRow < L.nRow && g_top + g_modalRow < (int)g_view.size())
                    g_items.erase(g_items.begin() + g_view[g_top + g_modalRow]);
                g_modalOpen = false;
                g_sel = 0; g_top = 0;
                BuildView(); ComputeLayout();
                Toast(L"已删除", true);
            } else {
                g_modalOpen = false;
            }
            Redraw();
            return 0;
        }

        if (g_menuOpen) {
            RunMenuItem(MenuHit(pt));
            return 0;
        }

        // 点搜索框 → 进入输入态。中文搜索要靠输入法，而输入法组合必须有真正的
        // 键盘焦点，只能把前台交给面板。这是唯一会让目标窗口光标暂时消失的入口，
        // 由用户主动点击触发；Esc 关掉面板即刻还原。
        // IsWindowVisible 兜住「面板已隐藏却仍收到投递消息」的情况，否则前台会停在
        // 一个不可见的窗口上，而隐藏之后定时器也停了，没人再把它收回来。
        if (IsWindowVisible(hwnd) && PtIn(L.searchBox, pt) && GetForegroundWindow() != hwnd) {
            g_inputMode = true;
            ForceForeground(hwnd);
            return 0;
        }

        // 分类抽屉：点条目选中分类、点关闭或点抽屉外收起
        if (g_drawerOpen && PtIn(L.drawer, pt)) {
            if (PtIn(L.drawerClose, pt)) { ToggleDrawer(false); return 0; }
            int hit = DrawerHit(pt);
            if (hit >= 0) {
                g_query.clear(); g_comp.clear();
                SetGlyphCat(g_page, L.dCat[hit]);
                g_drawerOpen = false;
                g_drawerHover = -1;
                BuildView(); ComputeLayout(); Redraw();
            }
            return 0;
        }
        // 抽屉开着时点别处：先收起抽屉。标题栏区域放行，否则要关个窗口得点两次
        if (g_drawerOpen && pt.y >= S(HEAD_H)) { ToggleDrawer(false); return 0; }

        // 窗口边缘/角落 → 抓取改变尺寸。必须排在标题栏拖动之前：
        // 顶部那条 5px 落在标题栏里，先判 resize 才不会误触发拖动
        int rs = HitResize(pt);
        if (rs != RS_NONE) {
            POINT sc = pt;
            ClientToScreen(hwnd, &sc);
            GetWindowRect(hwnd, &g_rsRect);
            g_rsFrom = sc;
            g_resizing = rs;
            g_posUserSet = true;      // 用户自己摆过位置，补探测不许再挪
            SetCapture(hwnd);
            return 0;
        }

        // 标题栏空白处按下 → 手动拖动（不走 HTCAPTION 模态循环：拖动中窗口会被判失活而自动收起）
        if (PtIn(L.head, pt) && !PtIn(L.gear, pt) && !PtIn(L.closeB, pt) && !PtIn(L.pinB, pt)) {
            bool onTab = false;
            for (int i = 0; i < 4; ++i)
                if (PtIn(L.tab[i], pt)) { onTab = true; break; }
            if (!onTab) {
                POINT sc = pt;
                ClientToScreen(hwnd, &sc);
                GetWindowRect(hwnd, &g_dragRect);
                g_dragFrom = sc;
                g_dragging = true;
                g_posUserSet = true;   // 同上：用户已经把面板摆到想要的地方了
                SetCapture(hwnd);
                return 0;
            }
        }

        // 窗口控制
        if (PtIn(L.closeB, pt)) { HidePanel(); return 0; }
        if (PtIn(L.gear, pt)) {
            // TODO: 设置弹层待实现（当前点击不做任何事）
            return 0;
        }
        if (PtIn(L.pinB, pt)) { ToggleTemporaryPin(); return 0; }

        // 页签
        for (int i = 0; i < 4; ++i) {
            if (PtIn(L.tab[i], pt)) { SetPage(i); return 0; }
        }

        // chips
        for (int i = 0; i < L.nChip; ++i) {
            if (PtIn(L.chip[i], pt)) {
                int cat = L.chipCat[i];
                if (g_page == PG_EMOJI || g_page == PG_SYMBOL) {
                    g_query.clear(); g_comp.clear();
                    SetGlyphCat(g_page, cat);
                    BuildView();
                } else {
                    g_filter = cat; g_sel = 0; g_top = 0;
                    BuildView();
                }
                ComputeLayout();
                Redraw();
                return 0;
            }
        }
        if (L.moreBtn.right > L.moreBtn.left && PtIn(L.moreBtn, pt)) {
            ToggleDrawer(!g_drawerOpen);
            return 0;
        }

        // 主体
        if (PtIn(L.body, pt)) {
            if (g_page == PG_HISTORY || g_page == PG_FAV) {
                for (int i = 0; i < L.nRow; ++i) {
                    if (PtIn(L.row[i], pt)) {
                        int vi = g_top + i;
                        g_sel = vi;
                        PasteItem(vi);
                        return 0;
                    }
                }
            } else {
                const std::vector<GItem>& list = GlyphItems(g_page);
                int stepG = S(44) + S(5);
                int firstG = (g_gridScroll > S(8))
                             ? ((g_gridScroll - S(8)) / stepG) * GlyphCols() : 0;
                for (int i = firstG; i < (int)list.size(); ++i) {
                    RECT c = GlyphCellRect(i);
                    if (c.top > pt.y) break;     // 行的 top 自上而下递增，越过光标就不必再看
                    if (PtIn(c, pt)) {
                        PasteGlyph(g_page, list[i]);
                        return 0;
                    }
                }
            }
        }
        return 0;
    }

    case WM_RBUTTONUP: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        POINT pt{x, y};
        ComputeLayout();
        if (g_modalOpen) return 0;
        if (PtIn(L.body, pt) && (g_page == PG_HISTORY || g_page == PG_FAV)) {
            for (int i = 0; i < L.nRow; ++i) {
                if (PtIn(L.row[i], pt)) {
                    g_sel = g_top + i;
                    g_menuPt = pt;
                    g_menuOpen = true;
                    g_menuRow = i;
                    g_menuItem = g_view[g_top + i];
                    g_menuHover = -1;
                    Redraw();
                    return 0;
                }
            }
        }
        return 0;
    }

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        WheelBody(delta);
        return 0;
    }

    // 面板是 WS_EX_NOACTIVATE，正常不会收到这些消息；只有「输入态」（用户点过
    // 搜索框，我们主动把前台交给面板）以及文件对话框关闭后才会走到这里。
    case WM_MOUSEACTIVATE:
        // 鼠标点面板不要激活它：焦点留在目标窗口，光标继续闪。
        return MA_NOACTIVATE;

    case WM_ACTIVATE:
        Trace("WM_ACTIVATE wp=%d fg=%p", (int)LOWORD(wp), (void*)GetForegroundWindow());
        if (LOWORD(wp) == WA_INACTIVE) {
            if (g_dialogOpen) return 0;     // 另存为对话框是面板的 owned window
            // 固定态：不失焦隐藏；顺手把新的前台窗口记为粘贴目标，方便换个窗口继续插
            if (g_pinned) {
                HWND fg = GetForegroundWindow();
                if (fg && fg != hwnd) g_target = fg;
                return 0;
            }
            // 输入态下失去前台：可能是用户点回了目标窗口（正常，继续留着），
            // 也可能是点去了别的窗口。统一交给 PanelShouldStay 判断，别在这里下结论。
            // 但前台切换此刻常常还没落定（GetForegroundWindow() 仍返回面板自己），
            // 这时收起会走「还前台」分支，把用户正要点的那次激活挤掉——实测点了
            // 另一个应用，前台却被拽回上一个窗口。所以先等落定再判。
            if (GetForegroundWindow() == hwnd) SetTimer(hwnd, TIMER_DEFERHIDE, 120, nullptr);
            else PostMessageW(hwnd, WM_APP_DEFER_HIDE, 0, 0);
            return 0;
        }
        return 0;

    case WM_APP_DEFER_HIDE:
        Trace("WM_APP_DEFER_HIDE fg=%p", (void*)GetForegroundWindow());
        if (g_pinned || g_dialogOpen || g_dragging) return 0;
        if (!PanelShouldStay(hwnd)) {
            g_menuOpen = false;
            g_modalOpen = false;
            HidePanel();
        }
        return 0;

    // 低级鼠标钩子看到「按下的点落在面板矩形之外」时投递过来（见 LLMouse）。
    // 光靠前台变化判不出来这种情况：非激活态下面板从不改变前台，用户在同一个
    // 窗口里点掉面板旁边的正文/地址栏时，GetForegroundWindow() 纹丝不动。
    case WM_APP_HIDE_OUTSIDE:
        Trace("WM_APP_HIDE_OUTSIDE fg=%p root=%p", (void*)GetForegroundWindow(), (void*)wp);
        if (g_pinned || g_dialogOpen || g_dragging || g_resizing) return 0;
        // 用户点的若不是唤起时的目标窗口（点了别的应用、桌面…），那个窗口马上会
        // 激活，收起时别去抢前台，否则它抢不过前台锁，用户会觉得「点了没反应」。
        g_hideNoRestore = ((HWND)wp != g_target);
        g_menuOpen = false;
        g_modalOpen = false;
        g_drawerOpen = false;
        g_drawerHover = -1;
        HidePanel();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}