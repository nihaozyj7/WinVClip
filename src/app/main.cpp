// WinVClip —— 接管 Win+V，弹出自定义剪贴板面板
// 视觉稿：Clipboard Reliquary · PC Panel · 560 × 620（1:1 还原 index.html）
// 构建：cmake -G Ninja -B build && cmake --build build
// 用法：winvclip.exe            常驻托盘，Win+V 弹面板
//       winvclip.exe --show     启动后立刻弹一次面板
//
// 原理：
//   1) WH_KEYBOARD_LL 全局钩子：Win 按下 + V 按下 -> 吞掉该键，同步弹面板
//      面板非激活态要用的按键也由它旁路转发，但只接面板会消费的键
//      （见 PanelConsumesBareKey：钩子是串行链，吞得越宽，别家的全局热键越哑）
//   2) AddClipboardFormatListener 监听 WM_CLIPBOARDUPDATE，延迟读取剪贴板存历史
//   3) 自绘 popup 面板；回车单击 = 写回剪贴板 + 向原窗口发 Ctrl+V

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <deque>
#include <algorithm>
#include <cwctype>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <d2d1.h>
#include <dwrite.h>
#include <initguid.h>   // 让 wincodec.h 把 PNG 编码器/像素格式的 GUID 定义成实体
#include <wincodec.h>
#include <uiautomation.h>   // 浏览器/Electron 的插入符只能从 UIA 文本模式读到（见 UiaCaretRect）
#include <tlhelp32.h>       // 焦点元素可能是浏览器的渲染子进程，判归属要查进程树
#include <commdlg.h>
#include <imm.h>       // 搜索框要能吃中文：IME 组合串自己收、自己画
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <atomic>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <cstring>
#include "app/resource.h"
#include "glyph/glyph_data.h"

// ================================================================== 常量
// 视觉常量（配色 / 尺寸 / 窗口类名）集中在 theme.h，改配色只看那一个文件。
#include "core/theme.h"
using namespace wv;
#include "core/text_util.h"
#include "core/diag.h"
#include "core/io_png.h"
#include "core/model.h"
#include "core/app_state.h"
#include "glyph/glyph.h"
#include "core/text_measure.h"
#include "core/util.h"
#include "render/dc.h"
#include "render/icons.h"
#include "render/fonts.h"
#include "render/path.h"
#include "image/raster.h"
#include "image/thumb.h"
#include "glyph/emoji_atlas.h"
#include "clipboard/capture.h"
#include "clipboard/paste.h"
#include "ui/layout.h"
#include "ui/paint.h"
#include "ui/caret.h"
#include "ui/showhide.h"
#include "app/tray.h"
#include "app/hook.h"
#include "ui/panel.h"
#include "ui/actions.h"
#include "model/clip_model.h"

// ================================================================== 数据

// Thumb / ClipItem / GItem / GCat / RecentGlyph 见 model.h

// 自己注入的按键统一打上标记（g_injectMark 见 app_state.h）：
// 钩子靠它区分「本程序注入」与「其他来源的注入」——后者要照常转发给面板，
// 自动化测试正是靠这一点才能驱动面板。

// ================================================================== 交互
// ---- 诊断日志（默认关闭）：排查前台/焦点问题时设 WV_TRACE=1 启动，
// 记录写到 %TEMP%\winvclip-trace.txt。默认路径上只有一次 getenv，开销可忽略。
// Trace 见 diag.cpp（WV_TRACE=1 时写 %TEMP%\winvclip-trace.txt）



// ================================================================== 主窗口
static LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_APP_HOTKEY:
        if (IsWindowVisible(g_panel)) HidePanel();
        else ShowPanel();
        return 0;

    case WM_TIMER:
        if (wp == TIMER_WARM) {
            KillTimer(hwnd, TIMER_WARM);
            StartEmojiWarm();
            if (g_panel) SetTimer(g_panel, TIMER_EMOJI, 8, nullptr);
        } else if (wp == TIMER_CLIP) {
            KillTimer(hwnd, TIMER_CLIP);
            CaptureClipboard();
            BuildView();
            ComputeLayout();
            if (g_panel && IsWindowVisible(g_panel)) Redraw();
        } else if (wp == TIMER_PASTE) {
            KillTimer(hwnd, TIMER_PASTE);
            HWND target = g_pasteTarget;
            g_pasteTarget = nullptr;
            std::wstring pending = g_typePending;
            g_typePending.clear();
            if (target && IsWindow(target)) {
                // 非激活态下目标窗口一直持有焦点，这里一般什么都不用做。
                // 只有用户点过搜索框（输入态）时才需要把前台交还回去。
                if (GetForegroundWindow() != target) {
                    ForceForeground(target);
                    if (GetForegroundWindow() != target) {
                        Toast(L"目标窗口没有获得焦点，已取消");
                        Redraw();
                        return 0;
                    }
                    // 顶层窗口回来了还不够，地址栏/搜索框那类内部焦点得手动还回去
                    RestoreTargetFocus(target, g_targetFocus);
                }
                if (!pending.empty()) {
                    TypeUnicode(pending);            // 表情/符号：直插，不动剪贴板
                } else {
                    INPUT in[4]{};
                    in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = VK_CONTROL;
                    in[1].type = INPUT_KEYBOARD; in[1].ki.wVk = 'V';
                    in[2].type = INPUT_KEYBOARD; in[2].ki.wVk = 'V'; in[2].ki.dwFlags = KEYEVENTF_KEYUP;
                    in[3].type = INPUT_KEYBOARD; in[3].ki.wVk = VK_CONTROL; in[3].ki.dwFlags = KEYEVENTF_KEYUP;
                    for (auto& x : in) MarkMine(x);
                    SendInput(4, in, sizeof(INPUT));
                    // 借来的剪贴板要还：给目标一点时间真正读出剪贴板内容再还原，
                    // 太早还原会让部分程序（异步读剪贴板的）粘到旧内容。
                    if (g_borrowedClip) SetTimer(g_main, TIMER_CLIPRESTORE, 600, nullptr);
                }
                g_inputMode = false;   // 注入完回到非激活态，焦点继续留在目标窗口
            }
        } else if (wp == TIMER_CLIPRESTORE) {
            KillTimer(hwnd, TIMER_CLIPRESTORE);
            RestoreClipboard();
        }
        return 0;

    case WM_CLIPBOARDUPDATE:
        OnClipboardChanged();
        return 0;
    case WM_APP_TRAY:
        if (LOWORD(lp) == WM_LBUTTONUP) {
            if (IsWindowVisible(g_panel)) HidePanel(); else ShowPanel();
        } else if (LOWORD(lp) == WM_RBUTTONUP) {
            POINT pt; GetCursorPos(&pt);
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING, 1, L"打开面板");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(m, MF_STRING, 2, L"退出");
            SetForegroundWindow(hwnd);
            int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                          pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(m);
            if (cmd == 1) ShowPanel();
            else if (cmd == 2) DestroyWindow(hwnd);
        }
        return 0;

    case WM_DESTROY:
        RemoveTray();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ================================================================== 静态数据
// 字形库在启动时只解析分类表（几十行），条目索引按分类惰性建立。
// 默认分类沿用参考页面：符号停在「常用」（quick 大类），表情停在「情绪」。
static void InitData() {
    GlyphParseTable(glyphdata::kSymBlob, true);
    GlyphParseTable(glyphdata::kEmoBlob, false);

    g_symCat = GlyphCatIndexByKey(PG_SYMBOL, "quick");   // 「常用」大类（原 hot 精选）
    if (g_symCat <= 0) g_symCat = g_symCats.empty() ? 0 : 1;
    g_emojiCat = GlyphCatIndexByKey(PG_EMOJI, "smileys");
    if (g_emojiCat <= 0) g_emojiCat = g_emoCats.empty() ? 0 : 1;

}

// ================================================================== 入口
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR cmdLine, int) {
    HANDLE mtx = CreateMutexW(nullptr, TRUE, L"WinVClip_SingleInstance");
    if (mtx && GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    SetProcessDPIAware();
    PerfInit();
    {
        HDC dc = GetDC(nullptr);
        g_dpi = GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(nullptr, dc);
    }
    g_ref = GetDC(nullptr);
    g_cfPng = RegisterClipboardFormatW(L"PNG");
    LoadViewSize();       // 上次调整好的面板尺寸
    InitData();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    InitD2D();
    MakeFonts();          // 必须在 InitD2D 之后：字体要同时注册到 DirectWrite

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    // 两种尺寸各取一次：大图标给 Alt+Tab / 任务栏，小图标给标题栏与托盘，
    // 都带 LR_SHARED 由系统托管，不必自己 DestroyIcon。
    wc.hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, 0, 0,
                                LR_DEFAULTSIZE | LR_SHARED);
    wc.hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                  GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                  LR_SHARED);
    wc.lpfnWndProc = MainProc;
    wc.lpszClassName = MAIN_CLASS;
    RegisterClassExW(&wc);

    wc.lpfnWndProc = PanelProc;
    wc.lpszClassName = PANEL_CLASS;
    wc.style = CS_DROPSHADOW;
    RegisterClassExW(&wc);

    g_main = CreateWindowExW(WS_EX_TOOLWINDOW, MAIN_CLASS, L"WinVClip", WS_POPUP, 0, 0, 0, 0,
                             nullptr, nullptr, hInst, nullptr);
    if (!g_main) return 1;

    // WS_EX_NOACTIVATE 是「不抢焦点」方案的地基：面板从不夺取前台，
    // 目标窗口的键盘焦点和文本插入符因此一直保留，光标照常闪烁。
    g_panel = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, PANEL_CLASS,
                              L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, hInst, nullptr);
    if (!g_panel) return 1;

    AddClipboardFormatListener(g_main);

    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, LLKeyboard, GetModuleHandleW(nullptr), 0);
    if (!g_hook) {
        MessageBoxW(nullptr, L"安装键盘钩子失败（可能被安全软件拦截），程序退出。",
                    L"WinVClip", MB_ICONERROR);
        return 1;
    }
    // 鼠标钩子只用来「点外面就收起」，装不上不影响主功能（面板仍能靠前台轮询，
    // 只是同窗口内点击收不起来），所以失败不报错。
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, LLMouse, GetModuleHandleW(nullptr), 0);

    AddTray();
    SetTimer(g_main, TIMER_WARM, 120, nullptr);   // 启动后就把字形图集准备好
    PostMessageW(g_main, WM_CLIPBOARDUPDATE, 0, 0);

    if (cmdLine && wcsstr(cmdLine, L"--show")) {
        PostMessageW(g_main, WM_APP_HOTKEY, 0, 0);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    StopEmojiWorker();        // 工作线程必须在 D2D/atlas 释放之前停掉
    ReleaseEmojiCache();
    UnhookWindowsHookEx(g_hook);
    if (g_mouseHook) UnhookWindowsHookEx(g_mouseHook);
    RemoveClipboardFormatListener(g_main);
    HFONT* all[] = {&g_fTitle, &g_fMeta, &g_fTab, &g_fChip, &g_fSearch, &g_fMono,
                    &g_fEmoji, &g_fSym, &g_fKbd, &g_fBig, &g_fToast, &g_fSymName};
    for (auto* f : all) if (*f) DeleteObject(*f);
    for (auto& kv : g_tfmt) if (kv.second) kv.second->Release();
    g_tfmt.clear();
    if (g_ellipsis)    g_ellipsis->Release();
    if (g_roundStroke) g_roundStroke->Release();
    if (g_brush)       g_brush->Release();
    for (auto& p : g_probe) if (p) p->Release();
    if (g_emojiFmt)    g_emojiFmt->Release();
    g_items.clear();          // 缩略图位图必须先于 render target 释放
    if (g_wic)         g_wic->Release();
    if (g_rt)         g_rt->Release();
    if (g_dw)         g_dw->Release();
    if (g_d2d)        g_d2d->Release();
    if (g_ref) ReleaseDC(nullptr, g_ref);
    return 0;
}
