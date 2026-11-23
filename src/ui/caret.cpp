// caret.cpp —— 实现见 caret.h。
//
// 这里全部是「读别人的窗口」的代码：GetGUIThreadInfo、UIA、进程树。
// 唯一要记住的取舍是——所有外部调用都必须带上超时/尺寸上限，
// 因为目标是别的进程（甚至可能是无响应的程序），不能让一次探测把面板拖住。
#include "ui/caret.h"

#include <tlhelp32.h>
#include <uiautomation.h>

#include "core/app_state.h"   // g_panel / g_main
#include "core/util.h"         // S
#include <cmath>

#include "core/diag.h"        // Trace / S

using namespace wv;

// ================================================================== 目标窗口的文本插入符
// 两个用途：
//   1. 面板贴着「文本插入符」弹出，而不是跟着鼠标——用户唤起面板时光标往往并不在
//      鼠标指针那儿；没有插入符可用时退回屏幕居中。
//   2. 表情/符号直插之前判断目标窗口到底有没有文本输入区。资源管理器的文件列表、
//      桌面这类地方没有插入符，把字符注入进去什么也插不进，还会触发系统提示音。
bool CaretRectOf(HWND wnd, RECT* out) {
    if (!wnd || !IsWindow(wnd)) { Trace("  caret: no target wnd (%p)", (void*)wnd); return false; }
    if (wnd == g_panel || wnd == g_main) return false;
    DWORD tid = GetWindowThreadProcessId(wnd, nullptr);
    if (!tid) return false;
    GUITHREADINFO gi{}; gi.cbSize = sizeof(gi);
    if (!GetGUIThreadInfo(tid, &gi)) { Trace("  caret: GetGUIThreadInfo fail tid=%lu", (unsigned long)tid); return false; }
    if (!gi.hwndCaret || !IsWindow(gi.hwndCaret)) {
        Trace("  caret: hwndCaret=%p focus=%p wnd=%p tid=%lu", (void*)gi.hwndCaret, (void*)gi.hwndFocus,
              (void*)wnd, (unsigned long)tid);
        return false;
    }

    RECT rc = gi.rcCaret;
    // rcCaret 的坐标基准在文档与实测之间不一致（一部分窗口给的是客户区坐标），
    // 拿 caret 所在窗口的屏幕矩形做判别，否则面板会跑到屏幕外面去。
    RECT wr{}; GetWindowRect(gi.hwndCaret, &wr);
    POINT o{0, 0}; ClientToScreen(gi.hwndCaret, &o);
    RECT conv{rc.left + o.x, rc.top + o.y, rc.right + o.x, rc.bottom + o.y};
    auto inside = [](const RECT& r, const RECT& b) {
        return r.left >= b.left - 2 && r.top >= b.top - 2 &&
               r.right <= b.right + 2 && r.bottom <= b.bottom + 2;
    };
    if (inside(conv, wr)) *out = conv;
    else if (inside(rc, wr)) *out = rc;
    else { Trace("  caret: rect outside wnd rc=%ld,%ld,%ld,%ld conv=%ld,%ld wr=%ld,%ld,%ld,%ld",
                 rc.left, rc.top, rc.right, rc.bottom, conv.left, conv.top,
                 wr.left, wr.top, wr.right, wr.bottom); return false; }

    if (out->right - out->left < 1) out->right = out->left + 1;
    if (out->bottom - out->top < 1) out->bottom = out->top + S(16);   // 折叠成一点的 caret
    return true;
}

// 原生通道的「输入框矩形」：焦点控件（Edit / RichEdit / 记事本的编辑区…）的窗口矩形。
// 只当它是控件、而不是整窗时才认（见 ShowPanel 里的合理性判断）。
bool FocusBoxOf(HWND wnd, RECT* out) {
    DWORD tid = GetWindowThreadProcessId(wnd, nullptr);
    if (!tid) return false;
    GUITHREADINFO gi{}; gi.cbSize = sizeof(gi);
    if (!GetGUIThreadInfo(tid, &gi)) return false;
    if (!gi.hwndFocus || !IsWindow(gi.hwndFocus)) return false;
    return GetWindowRect(gi.hwndFocus, out) != 0;
}

// ---------------------------------------------------------------- UIA 兜底：浏览器 / Electron
// Chromium / Electron（Edge、Chrome、VS Code、各种内嵌浏览器、Slack…）把文本插入符放在
// 渲染进程里，GetGUIThreadInfo 的 hwndCaret 恒为 0 ——「光标明明在闪，却读不到」。
// 它们的可访问性（UIA）文本模式能给出插入符 / 选区的屏幕矩形，是这类窗口唯一能让面板
// 贴着光标弹出的通道（早期版本只走 GetGUIThreadInfo，于是这些窗口一律退回屏幕居中）。
// 代价：第一次访问 Chromium 会唤醒它的可访问性树（几十~几百 ms），所以只在
// GetGUIThreadInfo 失败时才走这里，并且把 UIA 的连接超时压到 500ms（默认 2s）。
static IUIAutomation* g_uia = nullptr;
static bool           g_uiaTried = false;

IUIAutomation* UiaClient() {
    if (g_uiaTried) return g_uia;
    g_uiaTried = true;
    IUIAutomation* uia = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IUIAutomation, (void**)&uia);
    if (FAILED(hr) || !uia) { Trace("  uia: create fail hr=%08lx", (unsigned long)hr); return nullptr; }
    IUIAutomation2* uia2 = nullptr;
    if (SUCCEEDED(uia->QueryInterface(IID_IUIAutomation2, (void**)&uia2)) && uia2) {
        uia2->put_ConnectionTimeout(500);
        uia2->Release();
    }
    g_uia = uia;
    return g_uia;
}

// 取文本范围的包围矩形：SAFEARRAY 里每 4 个 double 是 (left, top, width, height)，
// 屏幕物理像素。多组时插入符一般只有一组，选区则按 last 取末尾那组（光标在选区末端）。
bool UiaRangeRect(IUIAutomationTextRange* range, bool last, RECT* out) {
    SAFEARRAY* sa = nullptr;
    if (FAILED(range->GetBoundingRectangles(&sa)) || !sa) return false;
    bool ok = false;
    double* d = nullptr;
    if (SUCCEEDED(SafeArrayAccessData(sa, (void**)&d)) && d) {
        long lo = 0, hi = -1;
        SafeArrayGetLBound(sa, 1, &lo);
        SafeArrayGetUBound(sa, 1, &hi);
        long n = (hi >= lo) ? (hi - lo + 1) / 4 : 0;
        if (n > 0) {
            long i = last ? n - 1 : 0;
            double x = d[i * 4], y = d[i * 4 + 1], w = d[i * 4 + 2], h = d[i * 4 + 3];
            if (h > 0.5) {
                if (w < 1.0) w = 1.0;
                out->left = (LONG)lround(x);
                out->top = (LONG)lround(y);
                out->right = (LONG)lround(x + w);
                out->bottom = (LONG)lround(y + h);
                ok = (out->right > out->left) && (out->bottom > out->top);
            }
        }
        SafeArrayUnaccessData(sa);
    }
    SafeArrayDestroy(sa);
    return ok;
}

// 焦点元素是不是「目标窗口自己的东西」。
// Chromium / Electron 把页面内容放在渲染子进程里：网页输入框的焦点元素 pid 是那个子进程，
// 和顶层窗口的 pid 不同。早期这里按 pid 严格判等，结果网页里的输入框一律被当成
// 「别人的窗口」静默拒掉（地址栏属于浏览器进程，反而能过）——这就是「浏览器输入框读不到」。
// 现在认「同进程 或 目标进程的子孙进程」；面板自己、输入法候选窗这类旁支仍然被挡在外面。
bool ProcessIsSelfOrDescendant(DWORD pid, DWORD ancestor) {
    if (!pid || !ancestor) return false;
    if (pid == ancestor) return true;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    DWORD cur = pid;
    for (int depth = 0; depth < 6 && cur; depth++) {
        PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
        DWORD parent = 0;
        if (Process32FirstW(snap, &pe)) {
            do {
                if (pe.th32ProcessID == cur) { parent = pe.th32ParentProcessID; break; }
            } while (Process32NextW(snap, &pe));
        }
        if (!parent) break;
        if (parent == ancestor) { found = true; break; }
        cur = parent;
    }
    CloseHandle(snap);
    return found;
}

// 插入符矩形是不是「控件整体矩形」。
// Chromium 系（Edge / WebView2 / Electron）在文本框内容为空、或同一文本视图里拿不到
// 字符布局时，会把整个控件框当成范围的包围盒返回：实测 WebView2 的聊天输入框
// （931x46）、Chromium 的空 textarea（660x300）都直接回整框，和真实插入符（1x17）
// 差着整块控件。采信它，落点算法就会把「整框」当成要避让的光标行，
// 面板被甩到框外远处——这就是「读到的明显是整个大输入框」。
bool CaretRectIsBox(const RECT& c, const RECT& b) {
    int cw = c.right - c.left, ch = c.bottom - c.top;
    int bw = b.right - b.left, bh = b.bottom - b.top;
    if (cw <= 0 || ch <= 0 || bw <= 0 || bh <= 0) return false;
    long long ca = (long long)cw * ch, ba = (long long)bw * bh;
    if (ca * 10 >= ba * 8) return true;                         // 面积占了框的 8 成
    if (bw >= 40 && cw * 10 >= bw * 8 && ch >= bh - 2) return true;  // 和框一样宽一样高
    return false;
}

// 框内近似的插入符：内容为空 / 布局不可得时，唯一还成立的几何事实是
// 「插入符在文本框首行的起始处」。按框内左上内缩一个 padding、行高 20dp 画一根，
// 误差十几个像素，而整框矩形错的是整块控件。
void ApproxCaretInBox(const RECT& box, RECT* out) {
    int pad = S(8);
    int bw = box.right - box.left, bh = box.bottom - box.top;
    if (pad * 3 > bw) pad = (std::max)(0, bw / 6);
    int lh = (std::min)(bh - pad * 2, S(20));
    if (lh < S(8)) lh = S(8);
    out->left = box.left + pad;
    out->right = out->left + 1;
    out->top = box.top + pad;
    out->bottom = out->top + lh;
}

bool BoxSaneRect(const RECT& b, const RECT& work, HWND target);

bool UiaCaretRect(HWND target, CaretInfo* info) {
    IUIAutomation* uia = UiaClient();
    if (!uia || !target || !IsWindow(target)) return false;

    DWORD pid = 0;
    GetWindowThreadProcessId(target, &pid);

    DWORD t0 = GetTickCount();
    IUIAutomationElement* el = nullptr;
    if (FAILED(uia->GetFocusedElement(&el)) || !el) { Trace("  uia: no focused element"); return false; }

    int ep = 0;
    if (FAILED(el->get_CurrentProcessId(&ep)) || !ProcessIsSelfOrDescendant((DWORD)ep, pid)) {
        // 焦点元素属于别的程序（面板自己、输入法候选窗…）：不是这次要贴的目标
        Trace("  uia: foreign focus elpid=%d targetpid=%lu", ep, (unsigned long)pid);
        el->Release();
        return false;
    }

    RECT* out = &info->caret;
    RECT rc{};
    bool ok = false;
    bool approx = false;

    // 焦点元素的控件类型决定「GetSelection 能不能信」。
    // Chromium 里的 input / textarea / contenteditable 分别是 Edit / Edit / Group，
    // 而焦点落在页面本身（body、页面级快捷键劫持）时是 Document：这种元素的
    // TextPattern 是整页文本视图，GetSelection 给的是页面里当前那块被选中/被跟踪的内容，
    // 与插入符无关（实测 bilibili 视频页给回一个 46x46 的方块），采信它面板会贴到乱处。
    CONTROLTYPEID ct = 0;
    el->get_CurrentControlType(&ct);
    bool isDocEl = (ct == UIA_DocumentControlTypeId);
    // 只对「看起来是文本控件」的类型采信 GetSelection 兜底：Edit / Document / Group / Pane /
    // Text / Custom（ct==0 表示没读出来，不排除）。列表项、按钮之类即便有 TextPattern 也不算。
    bool textish = (ct == 0) || ct == UIA_EditControlTypeId || ct == UIA_DocumentControlTypeId ||
                   ct == UIA_TextControlTypeId || ct == UIA_GroupControlTypeId ||
                   ct == UIA_PaneControlTypeId || ct == UIA_CustomControlTypeId;
    bool caretRangeOk = false;
    bool uiBadCaret = false;

    // 焦点控件整体矩形：既用来让面板别压住输入框本身，也用来识别「插入符矩形
    // 其实就是整框」这种假数据（见 CaretRectIsBox）。
    info->hasBox = false;
    RECT box{};
    bool boxOk = false;
    if (!isDocEl && SUCCEEDED(el->get_CurrentBoundingRectangle(&box)) &&
        box.right > box.left && box.bottom > box.top) {
        info->box = box;
        info->hasBox = true;
        boxOk = true;
    }

    // 1) TextPattern2::GetCaretRange —— 直接就是插入符，最准
    IUIAutomationTextPattern2* tp2 = nullptr;
    if (SUCCEEDED(el->GetCurrentPatternAs(UIA_TextPattern2Id, IID_IUIAutomationTextPattern2,
                                          (void**)&tp2)) && tp2) {
        BOOL active = FALSE;
        IUIAutomationTextRange* range = nullptr;
        if (SUCCEEDED(tp2->GetCaretRange(&active, &range)) && range) {
            if (active) {
                caretRangeOk = UiaRangeRect(range, false, &rc);
                ok = caretRangeOk;
                if (ok && boxOk && CaretRectIsBox(rc, box)) {
                    uiBadCaret = true;   // 回的是整框，不是插入符
                    ok = false;
                    caretRangeOk = false;
                }
            }
            range->Release();
        }
        tp2->Release();
    }

    // 2) TextPattern::GetSelection —— 没有 caret range 的控件（老实现、部分 Electron）
    //    用选区兜底：选区末尾就是插入符所在位置。文档级元素不适用，见上。
    if (!ok && textish && !isDocEl) {
        IUIAutomationTextPattern* tp = nullptr;
        if (SUCCEEDED(el->GetCurrentPatternAs(UIA_TextPatternId, IID_IUIAutomationTextPattern,
                                              (void**)&tp)) && tp) {
            IUIAutomationTextRangeArray* arr = nullptr;
            if (SUCCEEDED(tp->GetSelection(&arr)) && arr) {
                int n = 0;
                arr->get_Length(&n);
                if (n > 0) {
                    IUIAutomationTextRange* range = nullptr;
                    if (SUCCEEDED(arr->GetElement(n - 1, &range)) && range) {
                        ok = UiaRangeRect(range, true, &rc);
                        if (ok && boxOk && CaretRectIsBox(rc, box)) {
                            uiBadCaret = true;   // 同上：整框当插入符
                            ok = false;
                        }
                        range->Release();
                    }
                }
                arr->Release();
            }
            tp->Release();
        }
    }
    // 3) 通道都给不出可信插入符时，退到「框内近似」。
    //    内容为空的文本框是所有通道都失效的典型（Chromium 只回整框），但插入符
    //    必然在首行起始处；框本身可信（不是整窗、不是整页容器）才这么近似，
    //    否则宁可居中，免得把面板摆到整块控件外面老远。
    if (!ok && boxOk) {
        MONITORINFO mi{}; mi.cbSize = sizeof(mi);
        HMONITOR mon = MonitorFromRect(&box, MONITOR_DEFAULTTONEAREST);
        RECT work{};
        if (GetMonitorInfoW(mon, &mi)) work = mi.rcWork;
        if (work.right > work.left && BoxSaneRect(box, work, target)) {
            ApproxCaretInBox(box, &rc);
            ok = true;
            approx = true;
        }
    }
    el->Release();

    Trace("  uia: ok=%d ct=%d caret2=%d bad=%d approx=%d caret=%ld,%ld,%ld,%ld box=%d,%ld,%ld,%ld,%ld cost=%lums",
          (int)ok, (int)ct, (int)caretRangeOk, (int)uiBadCaret, (int)approx, rc.left, rc.top, rc.right,
          rc.bottom, (int)info->hasBox, info->box.left, info->box.top, info->box.right, info->box.bottom,
          (unsigned long)(GetTickCount() - t0));
    info->approx = approx;
    if (!ok) return false;
    if (rc.right - rc.left < 1) rc.right = rc.left + 1;
    *out = rc;
    return true;
}

// ================================================================== 显示 / 隐藏

// 两条通道依次试：原生插入符（GetGUIThreadInfo）→ UIA（浏览器 / Electron）。
bool ProbeCaretRaw(HWND target, CaretInfo* ci) {
    *ci = CaretInfo{};
    if (!target || !IsWindow(target)) return false;
    if (CaretRectOf(target, &ci->caret)) {
        RECT box{};
        if (FocusBoxOf(target, &box)) { ci->box = box; ci->hasBox = true; }
        Trace("  caret: ok=%ld,%ld,%ld,%ld box=%d[%ld,%ld,%ld,%ld]", ci->caret.left, ci->caret.top,
              ci->caret.right, ci->caret.bottom, (int)ci->hasBox,
              ci->box.left, ci->box.top, ci->box.right, ci->box.bottom);
        return true;
    }
    return UiaCaretRect(target, ci);
}

bool ProbeCaret(HWND target, CaretInfo* ci) {
    if (!ProbeCaretRaw(target, ci)) return false;
    // Chromium 第一次被 UIA 询问时常常只回「整框」或退化矩形（可访问性树刚被唤醒），
    // 紧接着再问一次通常就精确了——这次同步重试比等补探定时器好：面板不必先跳一下。
    if (ci->approx) {
        CaretInfo again;
        if (ProbeCaretRaw(target, &again) && !again.approx) *ci = again;
    }
    return true;
}

// 输入控件矩形可不可信。整窗（WPF / WinUI 自绘窗口的焦点控件就是顶层窗口）、
// 浏览器的整页 document 都会给出一个巨大矩形，拿它避让会把面板推到窗口外面去
// （实测 Win11 记事本：焦点控件矩形 = 整个窗口，面板于是被摆到窗口下方的空白区），
// 所以宁缺勿滥：太高的、又宽又高的、跟目标窗口几乎一样大的一律不认。
bool BoxSaneRect(const RECT& b, const RECT& work, HWND target) {
    int bw = b.right - b.left, bh = b.bottom - b.top;
    if (bw <= 0 || bh <= 0) return false;
    int ww = work.right - work.left, wh = work.bottom - work.top;
    if (bh > wh * 2 / 3) return false;
    if (bw > ww * 3 / 4 && bh > wh / 3) return false;   // 又宽又高：多半是整页容器
    RECT tw{};
    if (target && IsWindow(target) && GetWindowRect(target, &tw)) {
        long long ta = (long long)(tw.right - tw.left) * (tw.bottom - tw.top);
        long long ba = (long long)bw * bh;
        bool covers = b.left <= tw.left + 2 && b.top <= tw.top + 2 &&
                      b.right >= tw.right - 2 && b.bottom >= tw.bottom - 2;
        if (covers || (ta > 0 && ba * 10 >= ta * 6)) return false;   // 就是整窗本身（6 成面积起算）
    }
    return true;
}