// showhide.cpp —— 实现见 showhide.h（那里写了三条必须遵守的时序约定）。
#include "ui/showhide.h"

#include <algorithm>
#include <cmath>

#include "clipboard/capture.h"   // CaptureClipboard
#include "clipboard/paste.h"     // RestoreClipboard
#include "core/app_state.h"
#include "core/diag.h"   // Trace
#include "core/theme.h"
#include "core/util.h"          // S
#include "model/clip_model.h"    // BuildView
#include "ui/actions.h"         // Toast
#include "ui/caret.h"           // ProbeCaret / CaretInfo / BoxSaneRect
#include "ui/layout.h"          // L / ComputeLayout / Redraw
#include "ui/panel.h"

using namespace wv;


// 面板落点：贴着插入符弹出，并且尽量不压住输入框。
// 只算「下方放不下就翻到上方」是不够的：面板高 620、屏幕 1440，输入框在屏幕中部时
// 下方放不下面板，翻到上方又会被工作区上边界钳回来，钳完正好压在输入框上。
// 这里把所有候选位置都算出来打分：不遮挡 > 完整落在屏幕内 > 遮挡面积小 > 依次偏好
// （下方、上方、右侧、左侧、贴底、贴顶）。夹取到工作区之后才评分，所以钳取带来的
// 遮挡也会被计入。
POINT PlacePanel(const RECT& work, int w, int h, const RECT& caret, const RECT* avoid) {
    const int m = S(8), gap = S(8);
    const RECT& av = avoid ? *avoid : caret;
    struct Cand { int x, y; bool fits; long long ov; };
    Cand best{work.left, work.top, false, 0};
    bool have = false;

    auto consider = [&](int x, int y) {
        bool fits = x >= work.left + m && x + w <= work.right - m &&
                    y >= work.top + m && y + h <= work.bottom - m;
        int cx = x, cy = y;
        if (cx > work.right - m - w) cx = work.right - m - w;
        if (cy > work.bottom - m - h) cy = work.bottom - m - h;
        if (cx < work.left + m) cx = work.left + m;
        if (cy < work.top + m) cy = work.top + m;
        int ox = (std::min)((int)av.right, cx + w) - (std::max)((int)av.left, cx);
        int oy = (std::min)((int)av.bottom, cy + h) - (std::max)((int)av.top, cy);
        long long ov = (ox > 0 && oy > 0) ? (long long)ox * oy : 0;
        Cand c{cx, cy, fits, ov};
        auto better = [](const Cand& a, const Cand& b) {
            if ((a.ov > 0) != (b.ov > 0)) return a.ov == 0;      // 不遮挡优先
            if (a.fits != b.fits) return a.fits;                 // 完整在屏幕内优先
            return a.ov < b.ov;                                  // 遮挡面积小优先
        };
        if (!have || better(c, best)) { best = c; have = true; }
    };

    int x0 = caret.left - S(2);
    consider(x0, av.bottom + gap);                  // 下方（默认）
    consider(x0, av.top - gap - h);                 // 上方
    consider(av.right + gap, caret.top - S(30));    // 右侧
    consider(av.left - gap - w, caret.top - S(30)); // 左侧
    consider(x0, work.bottom - m - h);              // 极限贴底：重叠最小
    consider(x0, work.top + m);                     // 极限贴顶
    return POINT{best.x, best.y};
}

// 首次探测不到插入符时的补定位：面板已经显示出来了，这里只挪位置，不碰前台。
void MovePanelToCaret(const CaretInfo& ci) {
    if (!g_panel || !IsWindow(g_panel) || !g_panelW || !g_panelH) return;
    HMONITOR mon = MonitorFromPoint(POINT{ci.caret.left, ci.caret.top}, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{}; mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return;
    const RECT* avoid = (ci.hasBox && BoxSaneRect(ci.box, mi.rcWork, g_target)) ? &ci.box : nullptr;
    POINT p = PlacePanel(mi.rcWork, g_panelW, g_panelH, ci.caret, avoid);
    SetWindowPos(g_panel, HWND_TOPMOST, p.x, p.y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    Trace("  reposition -> %d,%d (caret %ld,%ld)", p.x, p.y, ci.caret.left, ci.caret.top);
}
void RoundCorners(HWND hwnd) {
    RECT rc; GetClientRect(hwnd, &rc);
    HRGN rgn = CreateRoundRectRgn(0, 0, rc.right + 1, rc.bottom + 1, S(RADIUS), S(RADIUS));
    SetWindowRgn(hwnd, rgn, TRUE);
}

void HidePanel() {
    Trace("HidePanel() from %p fg=%p", __builtin_return_address(0), (void*)GetForegroundWindow());
    bool noRestore = g_hideNoRestore;
    g_hideNoRestore = false;
    if (g_panel && IsWindowVisible(g_panel)) {
        // 只有输入态（用户点过搜索框）面板才持有前台，那时收起必须把焦点还回原窗口，
        // 否则前台会停在一个不可见的窗口上：原窗口失焦、键盘输入毫无反应。
        // 非激活态下我们从没抢过焦点，什么都不用还。
        bool hadFocus = (GetForegroundWindow() == g_panel);
        if (g_dragging) { g_dragging = false; ReleaseCapture(); }
        if (g_resizing) { g_resizing = RS_NONE; ReleaseCapture(); }
        KillTimer(g_panel, TIMER_CARET);
        KillTimer(g_panel, TIMER_FOCUS);
        KillTimer(g_panel, TIMER_EMOJI);
        KillTimer(g_panel, TIMER_REPROBE);
        KillTimer(g_panel, TIMER_DEFERHIDE);
        g_reprobeLeft = 0;
        ShowWindow(g_panel, SW_HIDE);
        // 面板收起后残留的「已吞 down」标记会让下一次显示吞掉一个孤立的 keyup。
        for (bool& b : g_swallowed) b = false;
        g_query.clear(); g_comp.clear();
        g_menuOpen = false;
        g_modalOpen = false;
        g_toastOn = false;
        g_pinned = false;        // 临时固定只在本次显示期间有效，关闭即取消
        g_inputMode = false;
        // 借用中的剪贴板要还回去（点完表情立刻关面板、没等到还原定时器的情况）
        if (g_borrowedClip) { KillTimer(g_main, TIMER_CLIPRESTORE); RestoreClipboard(); }
        BuildView();
        if (hadFocus && !noRestore && g_target && g_target != g_panel && IsWindow(g_target)) {
            SetForegroundWindow(g_target);
            RestoreTargetFocus(g_target, g_targetFocus);
        }
    }
}

// ---- 诊断日志（默认关闭，声明见文件头部）----
void ForceForeground(HWND hwnd);

// 面板该不该继续留在屏幕上。取代了早期那套「失焦就抢回前台」的抢救逻辑——
// 那套是「面板抢前台」方案下的补丁，现在根本不需要抢，判据也就直白得多：
//   · 非激活态：面板从不改变前台，所以「前台仍是唤起时的目标窗口」就是正常状态；
//     前台换成了别的窗口 = 用户主动切走 = 收起。
//   · 输入态（点过搜索框）：前台就是面板自己，同样正常。
// 这也解释了为什么不再需要 FocusStillOurs：那条路会让窗口重新激活，
// 之后再也收不到失焦事件，面板会永久挂最上层（早期踩过的坑）。
bool PanelShouldStay(HWND hwnd) {
    HWND fg = GetForegroundWindow();
    if (fg == hwnd) return true;
    if (!g_target || !IsWindow(g_target)) return true;
    return fg == g_target;
}

void ForceForeground(HWND hwnd) {
    Trace("  ForceForeground(%p) enter fg=%p", (void*)hwnd, (void*)GetForegroundWindow());
    HWND fg = GetForegroundWindow();
    if (fg == hwnd) { SetFocus(hwnd); return; }
    DWORD fgTid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    DWORD myTid = GetCurrentThreadId();
    if (fgTid && fgTid != myTid) {
        AttachThreadInput(myTid, fgTid, TRUE);
        BringWindowToTop(hwnd);
        SetForegroundWindow(hwnd);
        SetActiveWindow(hwnd);
        SetFocus(hwnd);
        AttachThreadInput(myTid, fgTid, FALSE);
    } else {
        BringWindowToTop(hwnd);
        SetForegroundWindow(hwnd);
        SetActiveWindow(hwnd);
        SetFocus(hwnd);
    }
    if (GetForegroundWindow() != hwnd) {
        INPUT in[2]{};
        in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = VK_MENU;
        in[1].type = INPUT_KEYBOARD; in[1].ki.wVk = VK_MENU;
        in[1].ki.dwFlags = KEYEVENTF_KEYUP;
        MarkMine(in[0]); MarkMine(in[1]);
        SendInput(2, in, sizeof(INPUT));
        SetForegroundWindow(hwnd);
        SetFocus(hwnd);
    }
}

// 目标窗口内部当前有焦点的控件。
HWND FocusOfWindow(HWND wnd) {
    if (!wnd || !IsWindow(wnd)) return nullptr;
    DWORD tid = GetWindowThreadProcessId(wnd, nullptr);
    if (!tid) return nullptr;
    GUITHREADINFO gi{}; gi.cbSize = sizeof(gi);
    if (!GetGUIThreadInfo(tid, &gi)) return nullptr;
    HWND f = gi.hwndFocus;
    if (!f || !IsWindow(f)) return nullptr;
    return f;
}

// 把控件级焦点还回目标窗口。
// SetForegroundWindow 只还原顶层窗口，窗口内部的焦点由它自己决定要不要恢复：
// 记事本会，资源管理器的地址栏 / 搜索框不会。不还的话，随后 SendInput 出去的那下
// Ctrl+V 会打在一个内部无焦点的窗口上——用户看到的就是「面板里选了条目，输入框没反应」。
void RestoreTargetFocus(HWND target, HWND focus) {
    if (!target || !IsWindow(target) || !focus || !IsWindow(focus)) return;
    if (target == g_panel) return;
    DWORD tid = GetWindowThreadProcessId(target, nullptr);
    DWORD myTid = GetCurrentThreadId();
    if (!tid) return;
    if (tid == myTid) { SetFocus(focus); return; }
    AttachThreadInput(myTid, tid, TRUE);
    SetFocus(focus);
    AttachThreadInput(myTid, tid, FALSE);
    Trace("  RestoreTargetFocus: focus=%p -> %p", (void*)focus, (void*)GetFocus());
}

void ShowPanel() {
    Trace("=== ShowPanel enter fg=%p vis=%d", (void*)GetForegroundWindow(), g_panel ? (int)IsWindowVisible(g_panel) : -1);
    g_query.clear(); g_comp.clear();
    g_sel = 0; g_top = 0;
    g_hoverRow = g_hoverChip = g_hoverCell = -1;
    g_gridScroll = 0;
    g_menuOpen = false;
    g_modalOpen = false;
    g_drawerOpen = false;      // 抽屉不跨次遗留：下次打开还是干净的分类条
    g_drawerHover = -1;
    g_drawerScroll = 0;
    g_caretOn = true;
    g_pinned = false;        // 每次打开都从未固定状态开始
    g_inputMode = false;     // 从非激活态开始：不碰焦点，目标窗口光标照常闪

    CaptureClipboard();

    HWND fg = GetForegroundWindow();
    if (fg && fg != g_panel) g_target = fg;
    // 必须趁目标窗口还持有焦点时读：抢完前台，它内部的焦点控件已经被清掉了。
    g_targetFocus = FocusOfWindow(g_target);

    // 位置：优先贴着目标窗口的文本插入符弹出；拿不到插入符（桌面、文件列表等
    // 没有文本输入区的地方）就居中于鼠标所在显示器。
    POINT pt; GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);

    CaretInfo ci;
    bool hasCaret = ProbeCaret(g_target, &ci);
    if (hasCaret) {
        POINT c{ci.caret.left, ci.caret.top};
        mon = MonitorFromPoint(c, MONITOR_DEFAULTTONEAREST);   // 光标可能跨屏
    }
    Trace("  place: target=%p hasCaret=%d", (void*)g_target, (int)hasCaret);

    MONITORINFO mi{}; mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    int workW = mi.rcWork.right - mi.rcWork.left;
    int workH = mi.rcWork.bottom - mi.rcWork.top;

    // 尺寸：用上次调好的（没有就用默认），夹在「最小尺寸」与当前工作区之间
    int w = g_savedW ? g_savedW : S(P_W);
    int h = g_savedH ? g_savedH : S(P_H);
    if (w < S(MIN_W)) w = S(MIN_W);
    if (h < S(MIN_H)) h = S(MIN_H);
    if (w > workW - S(16)) w = workW - S(16);
    if (h > workH - S(24)) h = workH - S(24);
    g_panelW = w;
    g_panelH = h;

    BuildView();
    ComputeLayout();

    g_posUserSet = false;
    int x = 0, y = 0;
    if (hasCaret) {
        const RECT* avoid = (ci.hasBox && BoxSaneRect(ci.box, mi.rcWork, g_target)) ? &ci.box : nullptr;
        POINT p = PlacePanel(mi.rcWork, w, h, ci.caret, avoid);
        x = p.x; y = p.y;
    } else {
        x = mi.rcWork.left + (w < workW ? (workW - w) / 2 : 0);
        y = mi.rcWork.top + (h < workH ? (workH - h) / 2 : 0);
    }

    int minX = mi.rcWork.left + S(8), minY = mi.rcWork.top + S(8);
    int maxX = mi.rcWork.right - S(8) - w, maxY = mi.rcWork.bottom - S(8) - h;
    if (x > maxX) x = maxX;
    if (y > maxY) y = maxY;
    if (x < minX) x = minX;
    if (y < minY) y = minY;

    // SWP_NOACTIVATE 是这套方案的关键：只把窗口摆上去并显示，不动前台。
    // 目标窗口因此一直持有键盘焦点，插入符一直闪——系统 Win+V 面板就是这么做的。
    // 面板要用到的按键由低级键盘钩子转发进来（见 LLKeyboard）。
    SetWindowPos(g_panel, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    Trace("  place x=%d y=%d w=%d h=%d caret=%ld,%ld avoid=%d", x, y, w, h, ci.caret.left, ci.caret.top,
          (int)(hasCaret && ci.hasBox && BoxSaneRect(ci.box, mi.rcWork, g_target)));
    Trace("  after SWP_SHOWWINDOW fg=%p vis=%d", (void*)GetForegroundWindow(), (int)IsWindowVisible(g_panel));
    RoundCorners(g_panel);
    SetTimer(g_panel, TIMER_CARET, 530, nullptr);
    SetTimer(g_panel, TIMER_FOCUS, 120, nullptr);
    // 缺插入符、或只拿到「框内近似」时补探：Chromium / Electron 的可访问性树是懒建的，
    // 刚被唤醒的头一次询问经常只回整框或退化矩形（网页输入框就是这样时有时无）。
    // 面板先按近似/居中显示，随后补探成功就挪到光标旁边——只挪位置，不碰前台。
    if ((!hasCaret || ci.approx) && g_target && IsWindow(g_target) && g_target != g_panel &&
        g_target != g_main) {
        g_reprobeLeft = 2;
        SetTimer(g_panel, TIMER_REPROBE, 140, nullptr);
    }
    Redraw();
}