// hook.cpp —— 实现见 hook.h（那里写了为什么必须用钩子、为什么吞键要克制）。
#include "app/hook.h"

#include "core/app_state.h"   // g_hook / g_mouseHook / g_panel / g_lwin / g_vDown / g_injectMark …
#include "core/diag.h"        // Trace
#include "core/text_util.h"
#include "core/util.h"        // MarkMine
#include "ui/actions.h"       // SetPage
#include "ui/layout.h"        // L / PtIn / ComputeLayout
#include "ui/panel.h"         // PanelKeyDown / PanelChar / PanelConsumesBareKey
#include "ui/showhide.h"      // ShowPanel / HidePanel / PanelShouldStay

using namespace wv;

// ================================================================== 键盘钩子
// 面板是非激活窗口（WS_EX_NOACTIVATE），永远收不到 WM_KEYDOWN / WM_CHAR，
// 所以它需要的按键全部在这里截下来、直接喂给面板的窗口过程。这是系统 Win+V
// 面板同款结构：焦点留在目标窗口，键盘输入由低级钩子旁路转发。
//
// 为什么不干脆让面板拿到焦点？因为「键盘焦点」是单一资源：面板一 SetFocus，
// 目标窗口立刻收到 WM_KILLFOCUS 并在自己的处理里 DestroyCaret，
// 记事本里的光标就没了。这不是能靠事后补丁补回来的（早期版本试过用替身窗口
// 在插入符原位画一根假光标，兼容性一塌糊涂）。正解是根本不碰焦点。
wchar_t VkToChar(DWORD vk, DWORD scan) {
    // 只关心不带修饰键的可见字符。Shift 用自跟踪的状态，不能用 GetAsyncKeyState：
    // 钩子吞键之后系统的键盘状态不会更新（见 g_ctrlDown 的注释）。
    BYTE ks[256] = {};
    if (g_shiftDown) {
        ks[VK_SHIFT] = 0x80;
        ks[VK_LSHIFT] = 0x80;
    }
    ks[VK_CAPITAL] = (BYTE)(GetKeyState(VK_CAPITAL) & 1);
    wchar_t buf[8] = {};
    int n = ToUnicodeEx(vk, scan, ks, buf, 8, 0, GetKeyboardLayout(0));
    return (n == 1) ? buf[0] : 0;      // n<0：死键，n>1：多字符序列，都不处理
}

// 非激活态下这个裸键要不要从系统手里接管。
//
// 判据必须收得窄，这是硬约束：WH_KEYBOARD_LL 是一条串行链，回调里 return 1
// 不只挡住目标窗口，还会让排在后面的**所有**低级键盘钩子——别的软件的全局
// 热键、输入法、AutoHotkey / Quicker 这类按键工具——根本收不到这一次按键。
// 早期版本「面板可见就接管全部非修饰键」，于是面板一弹出来，别家的键盘快捷键
// 就全哑了。现在只接面板确实会消费的键：
//   · Esc（唯一的键盘关闭键）、Tab（切页签）；
//   · 列表页的方向/翻页/删除、以及确认键；
//   · 能打出可见字符的键（非激活态直接打字进搜索框，这是核心体验）。
// 其余一律放行：功能键、媒体键、PrintScreen、Home/End/Insert、面板不处理的
// 导航键。带 Ctrl 的组合也整个放行——Ctrl+S / Ctrl+F / Ctrl+C 属于前台程序，
// 面板不该抢（代价是非激活态的 Ctrl+1..4 / P / D / S 快捷键不再生效，只在
// 点过搜索框的输入态可用）。
bool PanelConsumesBareKey(DWORD vk, wchar_t ch) {
    if (vk == VK_ESCAPE) return true;
    if (vk == VK_TAB)    return true;
    if (g_modalOpen || g_menuOpen)
        return vk == VK_RETURN || vk == VK_UP || vk == VK_DOWN;
    if (vk == VK_RETURN) return g_page == PG_HISTORY || g_page == PG_FAV;
    if (vk == VK_BACK)   return !g_query.empty();
    if (vk == VK_UP || vk == VK_DOWN || vk == VK_PRIOR || vk == VK_NEXT || vk == VK_DELETE)
        return g_page == PG_HISTORY || g_page == PG_FAV;
    return ch != 0;
}

LRESULT CALLBACK LLKeyboard(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        auto* k = (const KBDLLHOOKSTRUCT*)lp;
        DWORD vk = k->vkCode;
        bool down = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);
        bool up   = (wp == WM_KEYUP || wp == WM_SYSKEYUP);

        if (vk == VK_LWIN) g_lwin = down;
        if (vk == VK_RWIN) g_rwin = down;
        if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL) g_ctrlDown = down;
        if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) g_shiftDown = down;
        // 松开 Win 一并复位 V 的按下状态：万一某次 V 的 keyup 没被收到（UAC 安全桌面等），
        // 下一轮不会因为 g_vDown 卡在 true 而把正常按键当成重复键吞掉。
        if (up && (vk == VK_LWIN || vk == VK_RWIN)) g_vDown = false;

        if (down && vk == 'V' && (g_lwin || g_rwin)) {
            g_swallowVUp = true;
            // 长按 V 时系统每 ~30ms 重复送一次 keydown，而这个动作是 toggle（显示/隐藏）。
            // 不识别重复键的话，长按就变成 Hide/Show 高速交替，视觉上疯狂闪烁。
            // 只有「上一次 keyup 之后的第一次按下」才切换面板。
            // 时间窗 700ms > 系统最慢的重复速率(约 400ms/次)，同时兜住 keyup 丢失把 g_vDown 卡死的情况。
            DWORD now = GetTickCount();
            bool repeat = g_vDown && (now - g_vDownAt) <= 700;
            g_vDown = true;
            g_vDownAt = now;
            if (!repeat) {
                Trace("hook: downV -> toggle (fg=%p vis=%d)", (void*)GetForegroundWindow(), g_panel ? (int)IsWindowVisible(g_panel) : -1);
                if (g_panel && IsWindowVisible(g_panel)) HidePanel();
                else ShowPanel();
            }
            // 注入无害键：避免 Win 抬起时弹开始菜单，同时让 Win 抬起正常送达
            INPUT in[2]{};
            in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = VK_NONAME;
            in[1].type = INPUT_KEYBOARD; in[1].ki.wVk = VK_NONAME;
            in[1].ki.dwFlags = KEYEVENTF_KEYUP;
            MarkMine(in[0]); MarkMine(in[1]);
            SendInput(2, in, sizeof(INPUT));
            return 1;
        }
        if (up && vk == 'V') {
            g_vDown = false;
            if (g_swallowVUp) {
                g_swallowVUp = false;
                return 1;
            }
        }

        // ---- 面板打开期间的按键转发 ----
        // 命中条件：面板可见、前台不是面板自己（输入态下按键本来就直达面板）、
        // 并且这个键面板确实要消费（见 PanelConsumesBareKey，判据必须窄——
        // 多吞一个键，别家软件的全局热键就少收一个键）。
        // 修饰键一律放行，两个原因缺一不可：
        //   1) Win / Alt 组合要交给系统（Win+D、Alt+Tab、Alt+F4）——判据看自跟踪的
        //      g_lwin/g_rwin 与 LLKHF_ALTDOWN；
        //   2) **吞掉修饰键会让系统键盘状态卡死**。早期版本连 Win 的 keyup 一起吞了，
        //      系统以为 Win 一直按着，之后注入的 Ctrl+V 被解释成 Win+Ctrl+V，
        //      连按 Enter 都变成 Win 组合（实测直接弹出「快速设置」），
        //      而 GetAsyncKeyState(VK_LWIN) 全程返回「按下」。
        // 只放行「本程序自己注入的」按键（见 g_injectMark），其他来源的注入照常转发，
        // 与真人按键一致——自动化测试才能驱动面板。
        bool isModifier = (vk == VK_LWIN || vk == VK_RWIN ||
                           vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
                           vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ||
                           vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU);
        if (g_panel && IsWindowVisible(g_panel) && GetForegroundWindow() != g_panel &&
            k->dwExtraInfo != g_injectMark && !isModifier &&
            !(g_lwin || g_rwin) && !(k->flags & LLKHF_ALTDOWN)) {
            if (up) {
                // 吞了 down 就必须吞同一次按键的 up，目标窗口才收不到半截按键。
                if (g_swallowed[vk & 0xFF]) {
                    g_swallowed[vk & 0xFF] = false;
                    return 1;
                }
            } else if (down) {
                // 带 Ctrl 的组合整个放行：面板不抢前台程序的快捷键，也让排在
                // 钩子链后面的别家钩子能收到（代价见 PanelConsumesBareKey 的注释）。
                wchar_t ch = 0;
                if (!g_ctrlDown) {
                    if ((vk == 0 || vk == VK_PACKET) && (k->flags & LLKHF_INJECTED)) {
                        // KEYEVENTF_UNICODE 的注入事件：vk 被系统填成 VK_PACKET(0xE7)
                        // （个别路径是 0），码元在 scanCode 里。自动化脚本就是这样往
                        // 面板里打字的，真人键盘不会产生 VK_PACKET。
                        ch = (wchar_t)k->scanCode;
                    } else {
                        ch = VkToChar(vk, k->scanCode);
                    }
                }
                if (!g_ctrlDown && PanelConsumesBareKey(vk, ch)) {
                    g_swallowed[vk & 0xFF] = true;
                    // SendMessage 而不是 PostMessage：钩子就跑在本进程 UI 线程上，
                    // 同线程 SendMessage 是直接调用，没有跨线程等待，也就不会拖慢输入。
                    SendMessageW(g_panel, WM_APP_PANELKEY, (WPARAM)vk, MAKELPARAM(0, ch));
                    return 1;
                }
                // 面板不消费的键落到下面 CallNextHookEx：直接放行给目标窗口和
                // 后面的钩子，等于面板没打开时的行为。
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

// 点面板外部即收起。
// 为什么必须有这条：非激活态下面板从不夺取前台，所以「在同一个窗口里点掉面板」
// （最典型的是在浏览器里点正文/地址栏、在记事本里点正文后继续打字）根本不产生
// 前台变化——PanelShouldStay 的轮询看不到任何东西，面板就僵在屏幕上，直到用户
// 切到别的窗口或再按一次热键。系统 Win+V 面板没这问题是因为它自己是激活窗口，
// 一点别处就收到 WA_INACTIVE；我们保持不抢焦点，就得自己盯鼠标。
// 只看「按下的点是否在面板矩形内」，与前台轮询互补；自己注入的鼠标事件跳过。
LRESULT CALLBACK LLMouse(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && g_panel && IsWindowVisible(g_panel) &&
        !g_pinned && !g_dialogOpen && !g_dragging && !g_resizing) {
        bool down = (wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN || wp == WM_MBUTTONDOWN);
        if (down) {
            const MSLLHOOKSTRUCT* ms = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
            // 注意：不能用 LLMHF_INJECTED 过滤——自动化脚本（SetCursorPos + mouse_event）
            // 的点击也是注入事件，过滤掉就等于「脚本点别处不收起」。只认自己的魔数。
            if (ms && ms->dwExtraInfo != g_injectMark) {
                POINT pt = ms->pt;
                RECT r{};
                if (GetWindowRect(g_panel, &r) &&
                    !(pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom)) {
                    // 本进程自己的窗口（另存为对话框、消息框）不算「外部」，否则一开
                    // 对话框面板就被自己收掉了。
                    HWND at = WindowFromPoint(pt);
                    DWORD pid = 0;
                    if (at) GetWindowThreadProcessId(at, &pid);
                    if (!at || pid != GetCurrentProcessId()) {
                        HWND root = at ? GetAncestor(at, GA_ROOT) : nullptr;
                        HWND tgtRoot = (g_target && IsWindow(g_target))
                                       ? GetAncestor(g_target, GA_ROOT) : g_target;
                        bool other = (root && tgtRoot && root != tgtRoot);
                        // 输入态下面板自己是前台：这时点去别的窗口，收起必须让位给
                        // 那次点击的激活——我们先隐藏的话，系统会把前台随手交给
                        // Z 序下一个窗口（实测交给上一个记事本），用户点的窗口反而
                        // 抢不到前台。所以这种情形交给前台变化轮询去收（120ms 内），
                        // HidePanel 那时看到前台已不是自己，自然不会去抢。
                        if (other && GetForegroundWindow() == g_panel) {
                            Trace("hook: mouse down on other window (pt=%ld,%ld) -> defer to poll", pt.x, pt.y);
                            return CallNextHookEx(g_mouseHook, code, wp, lp);
                        }
                        Trace("hook: mouse down outside panel (pt=%ld,%ld) root=%p -> hide",
                              pt.x, pt.y, (void*)root);
                        // 不吞这次点击：用户点哪儿就是要操作哪儿，面板只是让开。
                        // 钩子跑在 UI 线程上，PostMessage 到同线程窗口不会重入。
                        // 点的不是唤起的那个窗口（或点空）时不许抢前台，见 g_hideNoRestore；
                        // 这里把点击处的顶层窗口带过去，由它和目标窗口比对。
                        PostMessageW(g_panel, WM_APP_HIDE_OUTSIDE, (WPARAM)root, 0);
                    }
                }
            }
        }
    }
    return CallNextHookEx(g_mouseHook, code, wp, lp);
}