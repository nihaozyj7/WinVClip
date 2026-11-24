// settings_win.cpp —— 实现见 settings_win.h。
#include "ui/settings_win.h"

#include <windowsx.h>

#include "app/resource.h"      // IDI_APP
#include "core/app_state.h"    // g_setWin / g_settingsOpen / g_setHover / g_modal*
#include "core/settings.h"     // g_cfg / THEME_* / HIST_STEP
#include "core/theme.h"
#include "core/util.h"         // S
#include "render/fonts.h"      // EnsureD2D
#include "ui/actions.h"        // SettingsToggle / SettingsSetTheme / …
#include "ui/layout.h"         // ComputeSettings / SettingsHit / SettingsModalRect
#include "ui/paint.h"          // PaintSettingsWindow
#include "ui/showhide.h"       // ForceForeground

using namespace wv;

bool SettingsWindowVisible() {
    return g_setWin && IsWindow(g_setWin) && IsWindowVisible(g_setWin);
}

// 拖动标题行：记下「按下时的屏幕坐标 + 窗口屏幕矩形」，移动时按差值 SetWindowPos。
// 不走 WM_NCHITTEST 返 HTCAPTION 的系统模态移动循环——那套循环会把消息从窗口自己的
// 循环里借走（面板踩过同一个坑，见 ui/panel.cpp 的标题栏拖动注释），而这里还要顺带
// 处理 hover 重绘与 Esc/Alt+F4，自己算差值最省心。
static bool  s_drag = false;
static POINT s_dragFrom{};
static RECT  s_dragRect{};

// 客户区是圆角卡片：无边框窗口的外形也得裁圆，否则四角会露出窗口的直角底色。
// 椭圆宽高 = 圆角半径 × 2（卡片 FillRound 用 S(12)，见 paint.cpp 的 PaintSettings）。
static void ShapeWindow(HWND hwnd) {
    RECT rc; GetClientRect(hwnd, &rc);
    int r = S(24);
    HRGN rgn = CreateRoundRectRgn(0, 0, rc.right + 1, rc.bottom + 1, r, r);
    if (rgn) SetWindowRgn(hwnd, rgn, TRUE);   // 成功后 rgn 归系统所有，别删
}

// 确认卡片里的两个目标：点在「清空」按钮上，还是点在卡片外（= 取消）
enum { MT_NONE = 0, MT_OK, MT_OUT };

static RECT SettingsModalOk() {
    RECT m = SettingsModalRect();
    int bw = (m.right - m.left - S(40) - S(8)) / 2;
    int by = m.bottom - S(20) - S(38);
    return RECT{m.left + S(20), by, m.left + S(20) + bw, by + S(38)};
}

static int ModalTarget(POINT p) {
    return PtIn(SettingsModalOk(), p) ? MT_OK : MT_OUT;
}

// 单击语义：按下只记「预备点的是哪个控件」，动作统一在松开时执行（见 WM_LBUTTONUP）。
// 拖动标题行是连续手势，仍然从按下开始。
static int  s_pressHit = SET_NONE;
static int  s_pressModal = MT_NONE;

void RegisterSettingsClass(HINSTANCE hInst) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, 0, 0,
                                 LR_DEFAULTSIZE | LR_SHARED);
    wc.hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                   LR_SHARED);
    wc.style = CS_DROPSHADOW;      // 没有系统边框就没有系统阴影，自己补一层
    wc.lpfnWndProc = SettingsProc;
    wc.lpszClassName = SETTINGS_CLASS;
    wc.lpszMenuName = nullptr;
    RegisterClassExW(&wc);
}

void ShowSettingsWindow() {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (g_setWin && IsWindow(g_setWin)) {
        ShowWindow(g_setWin, SW_SHOWNORMAL);
        ForceForeground(g_setWin);
        InvalidateRect(g_setWin, nullptr, FALSE);
        return;
    }

    EnsureD2D();      // 面板没显示时设备可能已经被闲置回收

    // 无边框窗口：客户区就是卡片本身，所以尺寸直接取内容尺寸（不必 AdjustWindowRectEx）
    int ww = SettingsWindowW(), wh = SettingsWindowH();

    POINT pt; GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{}; mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - ww) / 2;
    int y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - wh) / 2;

    g_settingsOpen = true;
    g_setHover = -1;
    g_toastOn = false;
    g_setWin = CreateWindowExW(0, SETTINGS_CLASS, L"WinVClip 设置",
                               WS_POPUP | WS_SYSMENU,    // 标题栏自绘，不留系统边框
                               x, y, ww, wh, nullptr, nullptr, inst, nullptr);
    if (!g_setWin) {
        g_settingsOpen = false;
        return;
    }
    ShapeWindow(g_setWin);
    ShowWindow(g_setWin, SW_SHOW);
    ForceForeground(g_setWin);
    InvalidateRect(g_setWin, nullptr, FALSE);
}

void CloseSettingsWindow() {
    if (g_setWin && IsWindow(g_setWin)) {
        DestroyWindow(g_setWin);      // WM_DESTROY 里统一清状态
        return;
    }
    g_setWin = nullptr;
    g_settingsOpen = false;
    g_setHover = -1;
}

// 点卡片里的一项。坐标必须来自消息本身（GET_X_LPARAM），不能读 GetCursorPos：
// 自动化测试用 PostMessage 投 WM_LBUTTONDOWN，真实光标并不在那儿。
static void HitSettings(POINT p) {
    switch (SettingsHit(p)) {
    case SET_CLOSE:      CloseSettingsWindow(); break;
    case SET_SW_AUTO:    SettingsToggle(0); break;
    case SET_SW_ADMIN:   SettingsToggle(1); break;
    case SET_SW_CONFIRM: SettingsToggle(2); break;
    case SET_SEG_0:      SettingsSetTheme(THEME_LIGHT); break;
    case SET_SEG_0 + 1:  SettingsSetTheme(THEME_DARK); break;
    case SET_SEG_0 + 2:  SettingsSetTheme(THEME_SYSTEM); break;
    case SET_BROWSE:     SettingsPickTempDir(); break;
    case SET_MINUS:      SettingsStepHistory(-HIST_STEP); break;
    case SET_PLUS:       SettingsStepHistory(HIST_STEP); break;
    case SET_CLEAR:      RequestClearHistory(); break;
    default: break;
    }
}

LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        PaintSettingsWindow(hwnd);
        return 0;

    case WM_SIZE:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_GETMINMAXINFO: {
        // 内容尺寸是固定的（行高之和），别让用户把窗口拖到看不全
        MINMAXINFO* mm = (MINMAXINFO*)lp;
        mm->ptMinTrackSize.x = SettingsWindowW();
        mm->ptMinTrackSize.y = SettingsWindowH();
        return 0;
    }

    case WM_TIMER:
        if (wp == TIMER_TOAST) {
            KillTimer(hwnd, TIMER_TOAST);
            g_toastOn = false;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_MOUSEMOVE: {
        RECT rc; GetClientRect(hwnd, &rc);
        ComputeSettings(rc.right, rc.bottom);
        if (s_drag) {
            // 光标可能已经移到窗口外（SetCapture 保证了消息还在，但 lp 是客户区坐标，
            // 拖动中不可靠），所以一律读 GetCursorPos 的屏幕坐标算差值。
            POINT p; GetCursorPos(&p);
            SetWindowPos(hwnd, nullptr,
                         s_dragRect.left + (p.x - s_dragFrom.x),
                         s_dragRect.top + (p.y - s_dragFrom.y),
                         0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int hit = SettingsHit(p);
        if (hit != g_setHover) {
            g_setHover = hit;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
        return 0;
    }

    case WM_MOUSELEAVE:
        if (g_setHover != -1) {
            g_setHover = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_LBUTTONDOWN: {
        RECT rc; GetClientRect(hwnd, &rc);
        ComputeSettings(rc.right, rc.bottom);
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};

        // 单击语义：按下只记「预备点的是哪个控件」，真正的动作在 WM_LBUTTONUP 里
        // 执行——按住后挪开再松开就是取消，不会误切一个开关。
        s_pressHit = SET_NONE;
        s_pressModal = MT_NONE;

        // 确认卡片优先（「清空剪贴板历史」从设置里点出来，卡片叠在最上层）
        if (g_modalOpen) { s_pressModal = ModalTarget(p); return 0; }

        // 标题行空白处 → 拖动窗口。必须排在控件命中之前：SettingsHit 会把
        // 「点标题栏」当成点空白，拖动就永远起不来。
        if (SettingsHit(p) == SET_NONE && PtIn(L.setTitle, p)) {
            POINT sc = p;
            ClientToScreen(hwnd, &sc);
            s_dragFrom = sc;
            GetWindowRect(hwnd, &s_dragRect);
            s_drag = true;
            SetCapture(hwnd);
            return 0;
        }

        s_pressHit = SettingsHit(p);
        return 0;
    }

    case WM_LBUTTONUP: {
        if (s_drag) {
            s_drag = false;
            ReleaseCapture();
            return 0;
        }
        RECT rc; GetClientRect(hwnd, &rc);
        ComputeSettings(rc.right, rc.bottom);
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};

        if (g_modalOpen) {
            int t = ModalTarget(p);
            // 按下与松开都落在「清空」上才算确认；点在卡片外（按下、松开都算）= 取消
            if (t == MT_OK && s_pressModal == MT_OK) {
                if (g_modalKind == MODAL_CLEAR_HIST) ConfirmClearHistory();
                else                                 ConfirmDelete();
            } else if (t == MT_OUT) {
                g_modalOpen = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            s_pressModal = MT_NONE;
            s_pressHit = SET_NONE;
            return 0;
        }

        // 松开时还落在按下时的同一个控件上才执行
        int hit = SettingsHit(p);
        int pressed = s_pressHit;
        s_pressHit = SET_NONE;
        if (hit != SET_NONE && hit == pressed) HitSettings(p);
        return 0;
    }

    // 拖动中途被夺走捕获（Alt+Tab、UAC 弹框）也必须结束拖动状态，
    // 否则下次鼠标一动窗口就自己跟着跑。
    case WM_CAPTURECHANGED:
        s_drag = false;
        return 0;

    // 标题行显示四向箭头，暗示这里能拖动
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(hwnd, &p);
            RECT rc; GetClientRect(hwnd, &rc);
            ComputeSettings(rc.right, rc.bottom);
            bool blank = PtIn(L.setTitle, p) && !PtIn(L.setClose, p);
            SetCursor(LoadCursorW(nullptr, blank ? IDC_SIZEALL : IDC_ARROW));
            return TRUE;
        }
        break;

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            if (g_modalOpen) {
                g_modalOpen = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            } else {
                CloseSettingsWindow();
            }
        }
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        // 窗口没了，状态必须一起复位：否则下一次读 g_settingsOpen 的代码会以为
        // 设置还开着（面板的按键分发、ShowPanel 的第一行都会看它）。
        g_setWin = nullptr;
        g_settingsOpen = false;
        g_setHover = -1;
        s_drag = false;
        s_pressHit = SET_NONE;
        s_pressModal = MT_NONE;
        g_modalOpen = false;
        g_modalKind = MODAL_DELETE_ONE;
        g_toastOn = false;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
