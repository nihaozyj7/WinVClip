// app_state.h —— 全部「可变全局状态」的唯一定义处。
//
// 为什么要有这个文件：
//   拆分 main.cpp 之前，这些变量都是 static（内部链接），别的 .cpp 根本看不见，
//   于是任何模块都拆不出去——这是整个重构的地基。
//   现在改成 extern，定义统一放在 app_state.cpp，其余模块 include 本文件即可。
//
// 变量名一个都没有改。P2b 是纯物理搬运，调用点零改动，
// 目的是让「谁拥有状态」这件事第一次看得见，而不是急着改代码。
//
// 使用约定（按组写在下面）：
//   · 「写者」一栏标明*唯一*允许写这组的模块。paint / layout 这类模块只读。
//   · 跨组共享的状态（如 g_view 与 g_sel）不在这里强制，但 P3/P4 会把
//     它们的不变量收拢到唯一的写入口，届时以那里的注释为准。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>   // NOTIFYICONDATAW

#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#include <d2d1.h>
#include <dwrite.h>

#include "core/model.h"      // ClipItem / GItem / GCat / RecentGlyph

// ---------------------------------------------------------------- 页面 / 筛选
enum { PG_HISTORY = 0, PG_FAV, PG_EMOJI, PG_SYMBOL, PG_COUNT };
enum { FL_ALL = 0, FL_TEXT, FL_IMAGE, FL_OTHER, FL_COUNT };

// ---------------------------------------------------------------- 窗口与钩子
// 写者：main.cpp（创建/销毁）、hook.cpp（钩子句柄）
extern HHOOK  g_hook;        // 低级键盘：Win+V 热键 + 面板按键转发
extern HHOOK  g_mouseHook;   // 低级鼠标：点面板外部即收起
extern HWND   g_main;
extern HWND   g_panel;
extern HWND   g_target;
extern HWND   g_pasteTarget;
// 唤起面板时，目标窗口内部那个有焦点的控件（地址栏的编辑框、搜索框…）。
// SetForegroundWindow 只还原顶层窗口，控件级焦点得靠它自己处理：记事本能找回，
// 资源管理器的地址栏 / 搜索框不能——那样粘过去的 Ctrl+V 会落到一个没有焦点的
// 窗口上，表现就是「面板选完条目，输入框里什么都没出现」。
extern HWND   g_targetFocus;

// ---------------------------------------------------------------- 键盘状态
// 写者：hook.cpp 独有。别处只读，且不要试图用 GetAsyncKeyState 交叉验证。
extern bool   g_lwin, g_rwin;
// 修饰键状态必须自己跟踪：钩子吞掉按键后系统不会更新全局键盘状态，
// GetAsyncKeyState / GetKeyState 在回调里都会读到「没按下」，Ctrl+1 因此会被
// 误当成字符 '1' 打进搜索框（早期实测踩过）。
extern bool   g_ctrlDown, g_shiftDown;
extern bool   g_swallowVUp;
extern bool   g_vDown;       // V 当前是否处于按下（用来识别长按产生的自动重复键）
extern DWORD  g_vDownAt;
// 已经吞下 keydown 的键，等它的 keyup 一起吞。不配对的话目标窗口会收到一个
// 没有 down 的 up，个别程序会把它当成一次组合键的结束。面板收起时清空。
extern bool   g_swallowed[256];

// 自己注入的按键统一打上这个标记。钩子靠它区分「本程序注入」和「其他来源的注入」：
// 前者必须放行（否则固定态连着插表情时，自己注入的字符会被自己的钩子吞掉），
// 后者照常转发给面板，跟真人按键一视同仁——自动化测试也正是靠这一点才能驱动面板。
extern const ULONG_PTR g_injectMark;   // 'WINV'

// ---------------------------------------------------------------- 剪贴板历史模型
// 写者：clip_model.cpp（采集、去重、置顶/收藏、删除）。paint 只读。
extern std::deque<ClipItem> g_items;

// ---------------------------------------------------------------- 视图状态
// 写者：clip_model.cpp（搜索/筛选重建 g_view）与 panel.cpp（选中、滚动、悬停）。
// 不变式（P3 会收拢到唯一写入口）：
//   · g_view 是 g_items 下标的过滤结果，g_sel 是 g_view 的偏移，不是 g_items 下标；
//   · g_top 是 g_view 的滚动首行，且 0 <= g_top <= max(0, |g_view|-可见行数)；
//   · 任何改动 g_query/g_filter/g_page 之后必须重建 g_view 并把 g_sel/g_top 夹回范围。
extern int    g_page;
extern int    g_filter;
extern std::wstring g_query;
extern std::wstring g_comp;      // IME 组合串（尚未上屏）：只用于显示与光标定位
extern std::vector<int> g_view;
extern int    g_sel, g_top, g_hoverRow, g_hoverChip;
extern int    g_hoverCell;
// 0 = 「最近使用」，n≥1 对应 g_emoCats / g_symCats[n-1]
extern int    g_emojiCat, g_symCat;

// ---------------------------------------------------------------- toast
extern std::wstring g_toast;
extern bool   g_toastOn, g_toastInfo;

// ---------------------------------------------------------------- 菜单 / 弹层
// 写者：panel.cpp
extern bool   g_menuOpen;
extern int    g_menuRow, g_menuHover;
extern int    g_menuItem;      // 呼出菜单时锁定的条目在 g_items 中的绝对下标
extern POINT  g_menuPt;        // 右键呼出菜单时的光标位置（客户区坐标）
extern bool   g_dialogOpen;    // 文件对话框打开期间禁止「失焦即隐藏」
extern bool   g_modalOpen;
extern int    g_modalRow;

// ---------------------------------------------------------------- 拖动 / 缩放
// 写者：panel.cpp（WM_LBUTTONDOWN + SetCapture，不用系统模态循环）
extern bool   g_dragging;
extern POINT  g_dragFrom;      // 按下时的屏幕坐标
extern RECT   g_dragRect;      // 按下时的窗口位置
extern int    g_resizing;
extern POINT  g_rsFrom;
extern RECT   g_rsRect;
enum { RS_NONE = 0, RS_L, RS_R, RS_T, RS_B, RS_TL, RS_TR, RS_BL, RS_BR };

// 「点外面收起」这条路上不许抢前台：用户点的窗口正等着激活，HidePanel 若趁机
// SetForegroundWindow(g_target) 就会把前台拽回去（前台锁会让用户那次点击落空）。
extern bool   g_hideNoRestore;
// 临时固定：本次显示期间不失焦隐藏 + 置顶，关闭后自动取消
extern bool   g_pinned;
extern std::wstring g_typePending;   // 待注入的文本（表情/符号直插，不经剪贴板）

// 面板是否处于「输入态」：见 main.cpp 开头焦点模型的说明
extern bool   g_inputMode;
// 钩子转发给面板的按键消息（wParam=vk，lParam=MAKELPARAM(ctrl, ch)）
enum { WM_APP_PANELKEY = WM_APP + 3 };

// ---------------------------------------------------------------- 剪贴板借用
// 写者：paste.cpp。给不接受字符注入的目标窗口准备的粘贴通道：
// 借用期间把原剪贴板内容备份下来，粘完还原；借用产生的剪贴板变化不进历史。
struct ClipBak {
    bool valid = false;
    bool hadText = false;
    std::wstring text;
    std::vector<std::wstring> files;
    std::vector<BYTE> png, dib;
};
extern ClipBak g_clipBak;
extern bool    g_borrowedClip;   // 正在借用中（备份已做、尚未还原）
// 我方最后一次写剪贴板产生的序列号：既用于跳过「自己写的」捕获，也用于还原前的安全检查
extern DWORD   g_selfClipSeq;

// ---------------------------------------------------------------- 尺寸 / 定位
// 写者：panel.cpp。g_panelW/H 是本次显示的实际值，g_savedW/H 是上次调整后落盘的值
extern int    g_panelW, g_panelH;
extern int    g_savedW, g_savedH;
extern bool   g_caretOn;
extern int    g_gridScroll;    // 表情/符号页的纵向滚动（像素）
// 用户自己拖过面板 / 改过尺寸：这次的自动定位就作废，别再把他摆好的面板挪走
extern bool   g_posUserSet;
// 首次探测不到插入符时的补探测次数（Chromium 的可访问性树是懒建的，第一次问常常还没建好）
extern int    g_reprobeLeft;

// ---------------------------------------------------------------- 设备与字体资源
// 写者：main.cpp 初始化 / paint.cpp 使用时读取。g_dpi 变更要重建全部字体。
extern int    g_dpi;
extern HFONT  g_fTitle, g_fMeta, g_fTab, g_fChip, g_fSearch, g_fMono, g_fEmoji, g_fSym,
              g_fKbd, g_fBig, g_fToast, g_fSymName;
extern HDC    g_ref;           // 量文字用的屏幕 DC
extern NOTIFYICONDATAW g_nid;
extern bool   g_tray;
extern UINT   g_cfPng;         // 注册的 PNG 剪贴板格式

// ---------------------------------------------------------------- D2D 资源
// 写者：paint.cpp（懒创建、随 DPI 重建）。生命周期 = 进程。
extern ID2D1Factory*          g_d2d;
extern IDWriteFactory*        g_dw;
extern ID2D1DCRenderTarget*   g_rt;
extern HFONT                  g_fSymFallback[3];      // 符号字体兜底（见 PickSymFont）
extern std::unordered_map<std::wstring, int> g_symFontSlot;
extern IDWriteFont*           g_probe[5];             // 0-3 符号候选，4 = Segoe UI Emoji
extern IDWriteTextFormat*     g_emojiFmt;
extern ID2D1SolidColorBrush*  g_brush;                // 全局面刷（换色复用）
extern ID2D1StrokeStyle*      g_roundStroke;          // 圆头圆角笔触
extern IDWriteInlineObject*   g_ellipsis;             // 省略号 trimming sign
extern bool                   g_painting;             // 是否处于 D2D batch 内
extern std::unordered_map<HFONT, IDWriteTextFormat*> g_tfmt;   // HFONT → DWrite 格式

// ---------------------------------------------------------------- 字形库（表情 / 符号）
// 写者：glyph.cpp（分类表、最近使用）与 emoji_atlas.cpp（命中缓存、抽屉）。
extern std::vector<GCat>          g_symCats, g_emoCats;
extern std::vector<RecentGlyph>   g_recentSym, g_recentEmo;
extern std::vector<GItem>         g_recentSymItems, g_recentEmoItems;
extern int                        g_recentCount[2];      // [0]=表情 [1]=符号
extern std::vector<GItem>         g_glyphHits;           // 跨分类搜索结果
extern std::wstring               g_hitsQuery;           // 命中所对应的查询串
extern int                        g_hitsPage;

// 分类抽屉
extern bool   g_drawerOpen;
extern int    g_drawerScroll, g_drawerHover, g_drawerContentH;
