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

static void HidePanel();
static bool PanelShouldStay(HWND hwnd);
static void RestoreTargetFocus(HWND target, HWND focus);
static void ShowPanel();
static void PasteItem(int vi);
static void ForceForeground(HWND hwnd);
static bool CaretRectOf(HWND wnd, RECT* out);
// 插入符探测结果：caret 是插入符本身，box（可选）是它所在的输入控件矩形。
// box 只用来「别挡住输入框」，因此宁可缺也不能错——整窗/整页的矩形会在 ShowPanel 里被否掉。
struct CaretInfo {
    RECT caret{};
    RECT box{};
    bool hasBox = false;
    // 插入符位置是「框内近似」而非真读到的：Chromium 对空文本框只回整框，
    // 这时用框内首行位置顶上，仍值得补探一次更准的（见 UiaCaretRect / TIMER_REPROBE）。
    bool approx = false;
};
static bool ProbeCaret(HWND target, CaretInfo* ci);
static void MovePanelToCaret(const CaretInfo& ci);

static void Toast(const std::wstring& msg, bool info = false) {
    g_toast = msg;
    g_toastInfo = info;
    g_toastOn = true;
    if (g_panel) {
        SetTimer(g_panel, TIMER_TOAST, 1500, nullptr);
        InvalidateRect(g_panel, nullptr, FALSE);
    }
}

void Redraw(const RECT* r) {
    if (g_panel) InvalidateRect(g_panel, r, FALSE);
}

// 临时固定：仅本次显示有效（置顶 + 不失焦隐藏），面板关闭后自动取消
static void ToggleTemporaryPin() {
    g_pinned = !g_pinned;
    if (g_pinned) {
        SetWindowPos(g_panel, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        Toast(L"已临时固定，可连续插入", true);
    } else {
        // 取消固定后立刻回到「前台不在目标窗口就收起」的规则。
        // 注意判据变了：非激活态下面板从不持有前台，所以不能再看「前台是不是面板」，
        // 要看「前台还是不是唤起时那个目标窗口」。
        if (!PanelShouldStay(g_panel)) { HidePanel(); return; }
        Toast(L"已取消临时固定", true);
    }
    Redraw();
}

static void SetPage(int p) {
    if (g_page == p) return;
    g_page = p;
    g_sel = 0; g_top = 0; g_hoverRow = -1; g_hoverCell = -1;
    g_gridScroll = 0;
    g_menuOpen = false; g_modalOpen = false;
    g_drawerOpen = false; g_drawerHover = -1; g_drawerScroll = 0;
    BuildView();
    ComputeLayout();
    Redraw();
}

static void PasteItem(int vi) {
    if (vi < 0 || vi >= (int)g_view.size()) return;
    ClipItem it = g_items[g_view[vi]];
    HWND target = g_target;
    if (!g_pinned) HidePanel();      // 固定态保留面板，便于连续粘贴
    PutToClipboard(it);
    if (target && IsWindow(target) && target != g_panel) {
        g_pasteTarget = target;
        // 非激活态下前台本来就是目标窗口，这行是空转；只有输入态才需要交还。
        if (GetForegroundWindow() != target) SetForegroundWindow(target);
        SetTimer(g_main, TIMER_PASTE, 90, nullptr);
    }
}

// 表情 / 符号点击：把字形送进目标窗口。有两条通道，选哪条看能不能读到目标的文本插入符。
// 以往这里是 PutText + toast「已复制到剪贴板」，每点一个字形就把用户的剪贴板顶掉，
// 历史里也被塞满一堆表情——所以能直插的场合一律不碰剪贴板。
static void PasteGlyph(int page, const GItem& it) {
    AddRecentGlyph(page, it);
    if (CurGlyphCat(page) <= 0) ComputeLayout();   // 「最近使用」页插入后重排

    std::wstring ch = U8N(it.g, it.gl);
    HWND target = g_target;
    if (target && IsWindow(target) && target != g_panel) {
        // 通道选择（这里是兼容性的关键，别再用「有没有插入符」当能不能插的判据）：
        //   A. 目标有文本插入符（记事本/Edit/RichEdit/Office 这类标准输入控件）
        //      → KEYEVENTF_UNICODE 直插，完全不碰剪贴板。
        //   B. 取不到插入符（Chromium/Electron、UWP、终端、shell 列表）
        //      → 借剪贴板 + Ctrl+V。这类程序的插入符在渲染进程里，GetGUIThreadInfo
        //        根本看不到，但「看不见插入符」不等于「不能粘」：Edge 地址栏就是典型，
        //        早期版本在这里直接拒绝，于是出现「系统剪贴板能粘、我们的表情点不动」。
        //        Ctrl+V 对所有这类目标都成立，系统 Win+V 面板走的也是这条路。
        // 资源管理器文件列表这类真的没有文本输入区的地方走 B 也不会响铃：
        // Ctrl+V 对文本剪贴板是无操作（而字符注入会被 Shell 当成键入查找，响一声提示音）。
        RECT cr{};
        bool canInject = CaretRectOf(target, &cr);
        bool usePaste = g_shiftDown || !canInject;   // Shift = 强制走粘贴，用户的手动兜底

        if (!g_pinned) HidePanel();      // 固定态保留面板，便于连续插入
        g_pasteTarget = target;
        if (usePaste) {
            g_typePending.clear();
            BorrowClipboard();
            PutText(ch);
            g_selfClipSeq = GetClipboardSequenceNumber();
        } else {
            g_typePending = ch;
        }
        if (GetForegroundWindow() != target) SetForegroundWindow(target);
        SetTimer(g_main, TIMER_PASTE, 90, nullptr);
    } else {
        // 没有可插入的目标窗口（比如在桌面上唤起面板）：退回复制，至少不会「点了没反应」
        PutText(ch);
        if (g_pinned) Redraw();
        Toast(L"没有可插入的窗口，已复制");
    }
}

// 置顶：按 g_items 的绝对下标操作（菜单与 Ctrl+P 共用）
static void TogglePinItem(int idx) {
    if (idx < 0 || idx >= (int)g_items.size()) return;
    ClipItem& it = g_items[idx];
    it.pinned = !it.pinned;
    Toast(it.pinned ? L"已置顶" : L"已取消置顶", true);
    BuildView();
    ComputeLayout();
    Redraw();
}

static void ToggleFav(int vi) {
    if (vi < 0 || vi >= (int)g_view.size()) return;
    ClipItem& it = g_items[g_view[vi]];
    it.fav = !it.fav;
    Toast(it.fav ? L"已收藏" : L"已取消收藏", true);
    BuildView();
    ComputeLayout();
    Redraw();
}

// 当前选中的视图下标
static int SelView() { return g_sel; }

// 当前选中项在 g_items 里的绝对下标
static int SelItem() {
    int vi = SelView();
    return (vi >= 0 && vi < (int)g_view.size()) ? g_view[vi] : -1;
}

// ================================================================== 另存为
// 文本存 UTF-8(BOM) 的 .txt；PNG 条目直接落盘（无损），CF_DIB 经 WIC 重新编码成 PNG；
// 文件条目复制原件（多个文件时选目标文件夹，重名自动加序号）。

// W2U8 / FileNameOf / StampName 见 text_util.h

// WriteAllBytes 见 io_png.cpp

// SaveRgbaAsPng 见 io_png.cpp

// 保存对话框：owner 是面板，期间要禁掉「失焦即隐藏」，否则刚弹出就被收起
static bool PickSavePath(const std::wstring& defName, const wchar_t* filter,
                         const wchar_t* defExt, std::wstring& out) {
    wchar_t buf[1024] = {};
    wcsncpy(buf, defName.c_str(), 1023);

    wchar_t desk[MAX_PATH] = {};
    bool hasDesk = SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, desk));

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = g_panel;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = (DWORD)ARRAYSIZE(buf);
    ofn.lpstrDefExt = defExt;
    ofn.lpstrInitialDir = hasDesk ? desk : nullptr;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;

    // 临时固定态下面板是 TOPMOST，会把对话框压住：先降一级，结束后恢复
    bool wasTop = g_pinned && g_panel;
    if (wasTop) SetWindowPos(g_panel, HWND_NOTOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    g_dialogOpen = true;
    BOOL ok = GetSaveFileNameW(&ofn);
    g_dialogOpen = false;

    if (wasTop) SetWindowPos(g_panel, HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (ok) out = buf;
    return ok != FALSE;
}

// UniquePath 见 io_png.cpp

static void SaveItemAs(int idx) {
    if (idx < 0 || idx >= (int)g_items.size()) return;
    ClipItem it = g_items[idx];      // 拷一份：对话框是模态的，历史可能在此期间被改动

    if (it.kind == 1) {
        if (it.files.empty()) { Toast(L"这条没有可保存的文件", true); return; }
        // 单文件：原样另存；多文件：用第一个文件确认目标位置，其余复制到同一目录
        std::wstring path;
        if (!PickSavePath(FileNameOf(it.files[0]), L"所有文件 (*.*)\0*.*\0", nullptr, path)) return;

        int saved = 0;
        if (CopyFileW(it.files[0].c_str(), path.c_str(), FALSE)) ++saved;
        if (it.files.size() > 1) {
            size_t slash = path.find_last_of(L"\\/");
            std::wstring dir = (slash == std::wstring::npos) ? std::wstring() : path.substr(0, slash);
            for (size_t i = 1; i < it.files.size(); ++i) {
                std::wstring dst = UniquePath(dir + L"\\" + FileNameOf(it.files[i]));
                if (CopyFileW(it.files[i].c_str(), dst.c_str(), FALSE)) ++saved;
            }
        }
        if (it.files.size() == 1)
            Toast(saved ? L"已保存：" + FileNameOf(path) : L"保存失败（原文件可能已被移动）", !saved);
        else
            Toast(L"已保存 " + std::to_wstring(saved) + L" / " +
                  std::to_wstring(it.files.size()) + L" 个文件", saved == 0);
        if (g_panel && IsWindowVisible(g_panel)) ForceForeground(g_panel);
        return;
    }

    std::wstring path;
    if (it.kind == 0) {
        std::wstring name = L"剪贴板文本_" + StampName(it.st) + L".txt";
        if (!PickSavePath(name, L"文本文件 (*.txt)\0*.txt\0所有文件 (*.*)\0*.*\0", L"txt", path)) return;
        std::string body = W2U8(it.text);
        std::vector<BYTE> buf{0xEF, 0xBB, 0xBF};    // BOM：记事本等能正确识别 UTF-8
        buf.insert(buf.end(), body.begin(), body.end());
        bool ok = WriteAllBytes(path, buf.data(), buf.size());
        Toast(ok ? L"已保存：" + FileNameOf(path) : L"保存失败", !ok);
    } else if (it.kind == 2) {
        std::wstring name = L"剪贴板图片_" + StampName(it.st) + L".png";
        if (!PickSavePath(name, L"PNG 图片 (*.png)\0*.png\0所有文件 (*.*)\0*.*\0", L"png", path)) return;
        bool ok = false;
        if (it.isPng) {
            ok = WriteAllBytes(path, it.bits.data(), it.bits.size());
        } else {
            Rgba img;
            if (DibToRgba(it.bits, img)) {
                // CF_DIB 没有 alpha 语义（32bpp 的 alpha 字节通常全是 0），
                // 原样编码会得到一张全透明 PNG，这里统一补成不透明
                for (size_t i = 3; i < img.px.size(); i += 4) img.px[i] = 255;
                ok = SaveRgbaAsPng(img, path);
            }
        }
        Toast(ok ? L"已保存：" + FileNameOf(path) : L"保存失败", !ok);
    } else {
        Toast(L"这条没有可保存的内容", true);
    }

    if (g_panel && IsWindowVisible(g_panel)) ForceForeground(g_panel);
}

// 执行菜单项（鼠标与键盘共用）
static void RunMenuItem(int hit) {
    int row = g_menuRow, item = g_menuItem;
    g_menuOpen = false;
    switch (hit) {
    case MI_PIN:  TogglePinItem(item); break;
    case MI_SAVE: SaveItemAs(item); break;
    case MI_DELETE:
        g_modalOpen = true;
        g_modalRow = row;
        Redraw();
        break;
    default:
        Redraw();
        break;
    }
}

// ---------------------------------------------------------------- 尺寸调整
// 不用系统的 HTLEFT/HTBOTTOMRIGHT：那是模态 sizing 循环，一进去面板会被判失活而自动收起
// （和标题栏拖动踩的是同一个坑）。所以自己 SetCapture 算差值。
static void RoundCorners(HWND hwnd);

static int HitResize(POINT p) {
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
static RECT ClampToMin(RECT r) {
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

static void ApplyResize(RECT r) {
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

static void WheelBody(int delta) {
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
static void PanelKeyDown(int vk, bool ctrl) {

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
static void PanelChar(wchar_t c) {
    if (c >= 32 && c != 127) {
        Trace("panel char '%c' query=%ls", (char)c, g_query.c_str());
        g_query.push_back(c);
        g_sel = 0; g_top = 0;
        BuildView(); ComputeLayout();
        Redraw();
    }
}

static LRESULT CALLBACK PanelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
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

// ================================================================== 目标窗口的文本插入符
// 两个用途：
//   1. 面板贴着「文本插入符」弹出，而不是跟着鼠标——用户唤起面板时光标往往并不在
//      鼠标指针那儿；没有插入符可用时退回屏幕居中。
//   2. 表情/符号直插之前判断目标窗口到底有没有文本输入区。资源管理器的文件列表、
//      桌面这类地方没有插入符，把字符注入进去什么也插不进，还会触发系统提示音。
static bool CaretRectOf(HWND wnd, RECT* out) {
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
static bool FocusBoxOf(HWND wnd, RECT* out) {
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

static IUIAutomation* UiaClient() {
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
static bool UiaRangeRect(IUIAutomationTextRange* range, bool last, RECT* out) {
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
static bool ProcessIsSelfOrDescendant(DWORD pid, DWORD ancestor) {
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
static bool CaretRectIsBox(const RECT& c, const RECT& b) {
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
static void ApproxCaretInBox(const RECT& box, RECT* out) {
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

static bool BoxSaneRect(const RECT& b, const RECT& work, HWND target);

static bool UiaCaretRect(HWND target, CaretInfo* info) {
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
static bool ProbeCaretRaw(HWND target, CaretInfo* ci) {
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

static bool ProbeCaret(HWND target, CaretInfo* ci) {
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
static bool BoxSaneRect(const RECT& b, const RECT& work, HWND target) {
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

// 面板落点：贴着插入符弹出，并且尽量不压住输入框。
// 只算「下方放不下就翻到上方」是不够的：面板高 620、屏幕 1440，输入框在屏幕中部时
// 下方放不下面板，翻到上方又会被工作区上边界钳回来，钳完正好压在输入框上。
// 这里把所有候选位置都算出来打分：不遮挡 > 完整落在屏幕内 > 遮挡面积小 > 依次偏好
// （下方、上方、右侧、左侧、贴底、贴顶）。夹取到工作区之后才评分，所以钳取带来的
// 遮挡也会被计入。
static POINT PlacePanel(const RECT& work, int w, int h, const RECT& caret, const RECT* avoid) {
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
static void MovePanelToCaret(const CaretInfo& ci) {
    if (!g_panel || !IsWindow(g_panel) || !g_panelW || !g_panelH) return;
    HMONITOR mon = MonitorFromPoint(POINT{ci.caret.left, ci.caret.top}, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{}; mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return;
    const RECT* avoid = (ci.hasBox && BoxSaneRect(ci.box, mi.rcWork, g_target)) ? &ci.box : nullptr;
    POINT p = PlacePanel(mi.rcWork, g_panelW, g_panelH, ci.caret, avoid);
    SetWindowPos(g_panel, HWND_TOPMOST, p.x, p.y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    Trace("  reposition -> %d,%d (caret %ld,%ld)", p.x, p.y, ci.caret.left, ci.caret.top);
}
static void RoundCorners(HWND hwnd) {
    RECT rc; GetClientRect(hwnd, &rc);
    HRGN rgn = CreateRoundRectRgn(0, 0, rc.right + 1, rc.bottom + 1, S(RADIUS), S(RADIUS));
    SetWindowRgn(hwnd, rgn, TRUE);
}

static void HidePanel() {
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
static void ForceForeground(HWND hwnd);

// 面板该不该继续留在屏幕上。取代了早期那套「失焦就抢回前台」的抢救逻辑——
// 那套是「面板抢前台」方案下的补丁，现在根本不需要抢，判据也就直白得多：
//   · 非激活态：面板从不改变前台，所以「前台仍是唤起时的目标窗口」就是正常状态；
//     前台换成了别的窗口 = 用户主动切走 = 收起。
//   · 输入态（点过搜索框）：前台就是面板自己，同样正常。
// 这也解释了为什么不再需要 FocusStillOurs：那条路会让窗口重新激活，
// 之后再也收不到失焦事件，面板会永久挂最上层（早期踩过的坑）。
static bool PanelShouldStay(HWND hwnd) {
    HWND fg = GetForegroundWindow();
    if (fg == hwnd) return true;
    if (!g_target || !IsWindow(g_target)) return true;
    return fg == g_target;
}

static void ForceForeground(HWND hwnd) {
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
static HWND FocusOfWindow(HWND wnd) {
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
static void RestoreTargetFocus(HWND target, HWND focus) {
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

static void ShowPanel() {
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

// ================================================================== 键盘钩子
// 面板是非激活窗口（WS_EX_NOACTIVATE），永远收不到 WM_KEYDOWN / WM_CHAR，
// 所以它需要的按键全部在这里截下来、直接喂给面板的窗口过程。这是系统 Win+V
// 面板同款结构：焦点留在目标窗口，键盘输入由低级钩子旁路转发。
//
// 为什么不干脆让面板拿到焦点？因为「键盘焦点」是单一资源：面板一 SetFocus，
// 目标窗口立刻收到 WM_KILLFOCUS 并在自己的处理里 DestroyCaret，
// 记事本里的光标就没了。这不是能靠事后补丁补回来的（早期版本试过用替身窗口
// 在插入符原位画一根假光标，兼容性一塌糊涂）。正解是根本不碰焦点。
static wchar_t VkToChar(DWORD vk, DWORD scan) {
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
static bool PanelConsumesBareKey(DWORD vk, wchar_t ch) {
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

static LRESULT CALLBACK LLKeyboard(int code, WPARAM wp, LPARAM lp) {
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
static LRESULT CALLBACK LLMouse(int code, WPARAM wp, LPARAM lp) {
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

// ================================================================== 托盘
static void AddTray() {
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_main;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy(g_nid.szTip, L"WinVClip —— Win+V 自定义剪贴板");
    g_tray = Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE;
}

static void RemoveTray() {
    if (g_tray) { Shell_NotifyIconW(NIM_DELETE, &g_nid); g_tray = false; }
}

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
