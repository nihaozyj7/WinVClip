// actions.cpp —— 实现见 actions.h。
#include "ui/actions.h"

#include <commdlg.h>
#include <shlobj.h>
#include <shobjidl.h>   // IFileOpenDialog（选临时目录）
#include <deque>
#include <vector>

#include "app/startup.h"       // SetAutostart / RelaunchElevated / IsProcessElevated
#include "clipboard/capture.h"
#include "clipboard/paste.h"
#include "core/app_state.h"    // 全部视图状态
#include "core/diag.h"
#include "core/history_store.h" // HistoryTouch / HistoryMigrateFrom（历史跨进程保留）
#include "core/model.h"
#include "core/settings.h"     // g_cfg / SettingsSave / ApplyThemeFromSettings
#include "core/text_util.h"
#include "core/theme.h"
#include "core/util.h"
#include "glyph/emoji_atlas.h"
#include "glyph/glyph.h"
#include "image/raster.h"
#include "model/clip_model.h"
#include "ui/caret.h"
#include "ui/layout.h"
#include "ui/settings_win.h"
#include "ui/showhide.h"

using namespace wv;


void Toast(const std::wstring& msg, bool info) {
    g_toast = msg;
    g_toastInfo = info;
    g_toastOn = true;
    // 只有「正在被谁看」的那个窗口才该起消隐定时器：面板隐藏时它的定时器照样会触发，
    // 会把设置窗口里的提示提前清掉（实测提示还没看清就没了）。
    if (g_panel && IsWindowVisible(g_panel)) {
        SetTimer(g_panel, TIMER_TOAST, 1500, nullptr);
        InvalidateRect(g_panel, nullptr, FALSE);
    }
    // 设置是独立窗口：改设置后的反馈（尤其失败原因）必须能在那个窗口里看见，
    // 不能指望面板——打开设置时面板是隐藏的。
    if (SettingsWindowVisible()) {
        SetTimer(g_setWin, TIMER_TOAST, 3000, nullptr);        InvalidateRect(g_setWin, nullptr, FALSE);
    }
}

void Redraw(const RECT* r) {
    if (g_panel) InvalidateRect(g_panel, r, FALSE);
    // r 是面板坐标，设置窗口整窗重绘（内容小，代价可以忽略）
    if (SettingsWindowVisible()) InvalidateRect(g_setWin, nullptr, FALSE);
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
    g_sel = 0; ResetListScroll(); g_hoverRow = -1; g_hoverCell = -1;
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
    HistoryTouch();
    Toast(it.pinned ? L"已置顶" : L"已取消置顶", true);
    BuildView();
    ComputeLayout();
    Redraw();
}

void ToggleFav(int vi) {
    if (vi < 0 || vi >= (int)g_view.size()) return;
    ClipItem& it = g_items[g_view[vi]];
    it.fav = !it.fav;
    HistoryTouch();
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
    if (it.kind == 2) EnsureItemBits(it);   // 像素可能只在临时目录里
    if (it.kind == 0) EnsureItemText(it);   // 超长正文可能只在数据库里

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
        RequestDeleteRow(row);
        break;
    default:
        Redraw();
        break;
    }
}

// 立即删掉视图里第 row 行（相对 g_top 的行号），不弹确认。
// 视口保持不动：删一条之后 g_view 里后面的条目整体前移，原先的 g_sel 此刻指向的
// 确实已经换成了另一条，但那是「同一行位置上剩下的那条」——正是用户接下来要看的。
// 早先这里 g_sel = 0 + ResetListScroll()，结果每删一条视口就弹回顶部，清理历史时
// 得一次次重新滚回去，手感极差。
static bool DeleteRowNow(int row) {
    if (row < 0 || g_top + row >= (int)g_view.size()) return false;
    int idx = g_view[g_top + row];
    if (idx < 0 || idx >= (int)g_items.size()) return false;
    // 删的这条正好还躺在剪贴板里：记下当前序列号，否则下一次唤出面板时的补捕
    // （ShowPanel → CaptureClipboard）会把它重新捡回历史顶部——用户看到的就是
    // 「这一条怎么都删不掉」。见 app_state.h 的 g_delClipSeq。
    ClipItem& victim = g_items[idx];
    EnsureItemText(victim);            // 长文本正文只在库里，比剪贴板前要先取回来
    if (ClipboardHoldsItem(victim)) g_delClipSeq = GetClipboardSequenceNumber();
    DropItemTemp(victim);              // 图片像素副本一起删掉
    g_items.erase(g_items.begin() + idx);
    HistoryTouch();
    int keepScroll = g_scrollY;       // 删之前视线停在哪，删完还停在哪
    int keepSel = g_sel;
    BuildView();
    ComputeLayout();                  // ListScrollMax 依赖 L.body，得先有布局
    g_sel = (keepSel < (int)g_view.size()) ? keepSel : (int)g_view.size() - 1;
    if (g_sel < 0) g_sel = 0;
    SetListScroll(keepScroll, false); // 只夹回合法范围（内容变短时贴底），不回顶
    ComputeLayout();                  // SetListScroll 改了 g_top/g_rowOff，行矩形要跟着重算
    Redraw();
    return true;
}

// 「删除这一条」的唯一入口：Del 键、右键菜单、确认卡片都走这里。
// 是否先弹确认只在这一处判，别在 panel 里各写一遍。
void RequestDeleteRow(int row) {
    if (row < 0 || row >= (int)g_view.size()) return;
    if (g_cfg.confirmDelete) {
        g_modalOpen = true;
        g_modalKind = MODAL_DELETE_ONE;
        g_modalRow = row;
        Redraw();
        return;
    }
    DeleteRowNow(row);
    Toast(L"已删除", true);
}

// 确认卡片上点了「删除」（或按 Enter）
void ConfirmDelete() {
    g_modalOpen = false;
    g_modalKind = MODAL_DELETE_ONE;
    bool ok = DeleteRowNow(g_modalRow);
    g_modalRow = -1;
    Toast(ok ? L"已删除" : L"这条已经不在了", !ok);
    Redraw();
}

// ================================================================== 清空剪贴板历史
// 语义（用户明确要求，别自行简化）：置顶与收藏是用户显式标记过的数据，与历史是
// 两条独立的线——
//   · 清空历史不动它们；
//   · 剪贴板记录上限裁剪也不动它们（见 clipboard/capture.cpp 的 ApplyHistoryLimit
//     和 history_store.cpp 的 HistoryLoad：载入时也必须把它们全部取回来）。
// 收藏页把它们单独列出来，历史页里也照旧看得到（星标式，不是「移出历史」式）。
int ClearHistoryCount() {
    int n = 0;
    for (const auto& it : g_items) if (!it.pinned && !it.fav) ++n;
    return n;
}

int ClearHistoryKeepPinnedFav() {
    std::deque<ClipItem> keep;
    int removed = 0;
    bool clipHit = false;      // 剪贴板只会装一条，命中之后不必再逐条比
    for (auto& it : g_items) {
        if (it.pinned || it.fav) { keep.push_back(std::move(it)); continue; }
        // 清掉的那批里若有「正躺在剪贴板里」的，同样要挡住唤出面板时的补捕
        // （见 DeleteRowNow 的同一条注释）
        if (!clipHit) {
            EnsureItemText(it);
            if (ClipboardHoldsItem(it)) {
                g_delClipSeq = GetClipboardSequenceNumber();
                clipHit = true;
            }
        }
        DropItemTemp(it);      // 图片像素副本只被这一条引用，跟着一起删掉腾磁盘
        ++removed;
    }
    if (removed == 0) return 0;
    g_items.swap(keep);

    // 清空是不可撤销的显式动作：立刻落盘，不等 HistoryTouch 的 900ms 防抖——
    // 这段时间里被关机或任务管理器结束，「清空」就白做了。
    HistoryFlush();

    g_menuOpen = false;
    g_menuItem = -1;
    g_modalRow = -1;
    g_hoverRow = -1;
    BuildView();
    ResetListScroll();
    ComputeLayout();
    Redraw();
    return removed;
}

void RequestClearHistory() {
    if (ClearHistoryCount() <= 0) {
        Toast(L"没有可清空的记录（置顶与收藏会保留）", true);
        Redraw();
        return;
    }
    g_modalOpen = true;
    g_modalKind = MODAL_CLEAR_HIST;
    g_modalRow = -1;
    Redraw();
}

void ConfirmClearHistory() {
    g_modalOpen = false;
    g_modalKind = MODAL_DELETE_ONE;   // 卡片复位成默认种类，别让下一次删单条画错文案
    int n = ClearHistoryKeepPinnedFav();
    Toast(n > 0 ? (L"已清空 " + std::to_wstring(n) + L" 条记录")
                : L"没有可清空的记录", true);
    Redraw();
}

// ================================================================== 设置窗口
// 每一项都是「改 g_cfg → 立刻生效 → 落盘」三步。不要攒到退出时统一写：
// 本程序是被关机/任务管理器结束也不会收到通知的常驻进程。
//
// 设置是独立窗口（见 ui/settings_win.h）：打开时先把面板收起——两者互斥，
// 面板那套「失焦即隐藏 + 按键靠钩子转发」的规则不能套在一个普通窗口上。
void OpenSettings() {
    g_menuOpen = false;
    g_modalOpen = false;
    g_modalKind = MODAL_DELETE_ONE;
    g_drawerOpen = false;
    if (g_panel && IsWindowVisible(g_panel)) HidePanel();
    ShowSettingsWindow();
}

void CloseSettings() {
    CloseSettingsWindow();
}

void SettingsToggle(int which) {
    if (which == 2) {
        g_cfg.confirmDelete = !g_cfg.confirmDelete;
        SettingsSave();
        Toast(g_cfg.confirmDelete ? L"删除前会先确认" : L"删除将立即执行，不再确认", true);
        Redraw();
        return;
    }
    if (which == 0) {
        g_cfg.autostart = !g_cfg.autostart;
        SettingsSave();
        std::wstring err;
        bool ok = SetAutostart(g_cfg.autostart, &err);   // 内部按「设置 + 账户权限」选 Run 项或计划任务
        if (!g_cfg.autostart) {
            Toast(ok ? L"已取消开机自启" : (L"取消开机自启失败：" + err), !ok);
        } else if (!ok) {
            Toast(L"设置开机自启失败：" + err, true);
        } else if (!err.empty()) {
            // 成功但走了退让路径（比如计划任务没建成、先退回普通自启）
            Toast(L"已设为开机自启（" + err + L"）", true);
        } else {
            bool hi = g_cfg.admin && IsUserInAdminGroup();
            Toast(hi ? L"已设为开机自启（登录时以管理员身份启动）" : L"已设为开机自启", true);
        }
        Redraw();
        return;
    }
    if (which != 1) return;

    if (g_cfg.admin) {
        // 关闭只影响下次启动：当前进程已经是管理员，降不回去。
        g_cfg.admin = false;
        SettingsSave();
        if (g_cfg.autostart) SetAutostart(true, nullptr);   // 自启从计划任务退回 Run 项
        Toast(L"下次启动起不再以管理员运行", true);
        Redraw();
        return;
    }

    // 标准用户既提不了权（UAC 上只能借别人的管理员凭据），也建不了「最高权限」的计划任务：
    // 直接挡在这里，别让开关点亮之后什么也没发生。
    if (!IsUserInAdminGroup()) {
        Toast(L"当前账户不在管理员组，无法以管理员身份运行", true);
        Redraw();
        return;
    }

    // 打开：先落盘再提权重启。提权后的新实例启动时会对账，把自启换成计划任务
    // （见 ReconcileAutostart）——只有管理员令牌能建 HighestAvailable 的任务。
    g_cfg.admin = true;
    SettingsSave();
    if (IsProcessElevated()) {
        if (g_cfg.autostart) SetAutostart(true, nullptr);
        Toast(L"已开启管理员模式", true);
        Redraw();
        return;
    }
    if (RelaunchElevated()) {
        if (g_main) DestroyWindow(g_main);     // 新实例接管，本进程退出
        return;
    }
    // 用户在 UAC 对话框上点了「否」
    g_cfg.admin = false;
    SettingsSave();
    Toast(L"未获得管理员权限，已取消", true);
    Redraw();
}

void SettingsSetTheme(int mode) {
    if (mode < THEME_LIGHT || mode > THEME_SYSTEM) return;
    if (g_cfg.theme == mode) return;
    g_cfg.theme = mode;
    SettingsSave();
    ApplyThemeFromSettings();
    Redraw();
}

void SettingsStepHistory(int delta) {
    int v = g_cfg.historyLimit + delta;
    if (v < HIST_MIN) v = HIST_MIN;
    if (v > HIST_MAX) v = HIST_MAX;
    if (v == g_cfg.historyLimit) return;
    g_cfg.historyLimit = v;
    SettingsSave();
    ApplyHistoryLimit();
    HistoryTouch();          // 裁掉的条目也要从磁盘上消失
    BuildView();
    ComputeLayout();
    Redraw();
}

// 选目录用系统的文件夹选择器：自绘面板里再做一个路径输入框要处理 IME、
// 光标、粘贴、非法字符，成本远高于收益。
static bool PickFolder(HWND owner, const std::wstring& init, std::wstring& out) {
    IFileOpenDialog* dlg = nullptr;
    bool ok = false;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&dlg)))) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dlg->SetTitle(L"选择临时文件目录");
        if (!init.empty()) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(init.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
                dlg->SetFolder(item);
                item->Release();
            }
        }
        if (SUCCEEDED(dlg->Show(owner))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR p = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
                    out = p;
                    ok = true;
                    CoTaskMemFree(p);
                }
                item->Release();
            }
        }
        dlg->Release();
    }
    return ok;
}

void SettingsPickTempDir() {
    std::wstring cur = g_cfg.tempDir.empty() ? DefaultTempDir() : g_cfg.tempDir;
    std::wstring picked;

    // 与「另存为」同样的处理：临时固定态面板是 TOPMOST，会压住系统对话框
    bool wasTop = g_pinned && g_panel;
    if (wasTop) SetWindowPos(g_panel, HWND_NOTOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    g_dialogOpen = true;      // 对话框是模态的，期间禁止「失焦即隐藏」
    bool ok = PickFolder(g_panel, cur, picked);
    g_dialogOpen = false;
    if (wasTop) SetWindowPos(g_panel, HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (!ok || picked.empty()) return;

    // 换目录等于换历史库的位置：先把旧目录的数据取过来，再整体落一次盘。
    // 不这么做的话，用户改一次临时目录就会看到「历史全没了」——那是最容易被
    // 当成 bug 的体验，而实际数据一直躺在旧目录里。
    std::wstring oldHist = HistoryDir();
    g_cfg.tempDir = picked;
    SettingsSave();
    // 立刻验证一次：目录建不出来（无权限/盘符不存在）就退回默认并提示，
    // 不要让采集在接下来的每次复制里都失败一次。
    if (TempDir() != picked) {
        Toast(L"该目录不可写，已退回默认临时目录", true);
    } else {
        Toast(L"临时目录已更改", true);
    }
    HistoryMigrateFrom(oldHist);
    HistoryFlush();
    Redraw();
}