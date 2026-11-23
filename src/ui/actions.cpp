// actions.cpp —— 实现见 actions.h。
#include "ui/actions.h"

#include <commdlg.h>
#include <shlobj.h>
#include <vector>

#include "clipboard/capture.h"
#include "clipboard/paste.h"
#include "core/app_state.h"    // 全部视图状态
#include "core/diag.h"
#include "core/model.h"
#include "core/text_util.h"
#include "core/theme.h"
#include "core/util.h"
#include "glyph/emoji_atlas.h"
#include "glyph/glyph.h"
#include "image/raster.h"
#include "model/clip_model.h"
#include "ui/caret.h"
#include "ui/layout.h"
#include "ui/showhide.h"

using namespace wv;


void Toast(const std::wstring& msg, bool info) {
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
void ToggleTemporaryPin() {
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

void SetPage(int p) {
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

void PasteItem(int vi) {
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
void PasteGlyph(int page, const GItem& it) {
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
void TogglePinItem(int idx) {
    if (idx < 0 || idx >= (int)g_items.size()) return;
    ClipItem& it = g_items[idx];
    it.pinned = !it.pinned;
    Toast(it.pinned ? L"已置顶" : L"已取消置顶", true);
    BuildView();
    ComputeLayout();
    Redraw();
}

void ToggleFav(int vi) {
    if (vi < 0 || vi >= (int)g_view.size()) return;
    ClipItem& it = g_items[g_view[vi]];
    it.fav = !it.fav;
    Toast(it.fav ? L"已收藏" : L"已取消收藏", true);
    BuildView();
    ComputeLayout();
    Redraw();
}

// 当前选中的视图下标
int SelView() { return g_sel; }

// 当前选中项在 g_items 里的绝对下标
int SelItem() {
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
bool PickSavePath(const std::wstring& defName, const wchar_t* filter,
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

void SaveItemAs(int idx) {
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
void RunMenuItem(int hit) {
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