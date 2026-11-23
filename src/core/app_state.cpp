// app_state.cpp —— 全局状态的唯一定义处（声明与说明见 app_state.h）。
//
// 这里只放定义，不放任何逻辑。改动本文件时请同步 app_state.h 的「写者」注释：
// 一个变量只能有一个归属模块，否则 P3 之后的不变量检查会失准。
#include "core/app_state.h"

// ---------------------------------------------------------------- 窗口与钩子
HHOOK  g_hook  = nullptr;
HHOOK  g_mouseHook = nullptr;
HWND   g_main  = nullptr;
HWND   g_panel = nullptr;
HWND   g_target = nullptr;
HWND   g_pasteTarget = nullptr;
HWND   g_targetFocus = nullptr;

// ---------------------------------------------------------------- 键盘状态
bool   g_lwin = false, g_rwin = false;
bool   g_ctrlDown = false, g_shiftDown = false;
bool   g_swallowVUp = false;
bool   g_vDown = false;
DWORD  g_vDownAt = 0;
bool   g_swallowed[256] = {};
const ULONG_PTR g_injectMark = 0x57494E56;   // 'WINV'

// ---------------------------------------------------------------- 剪贴板历史模型
std::deque<ClipItem> g_items;

// ---------------------------------------------------------------- 视图状态
int    g_page = PG_HISTORY;
int    g_filter = FL_ALL;
std::wstring g_query;
std::wstring g_comp;
std::vector<int> g_view;
int    g_sel = 0, g_top = 0, g_hoverRow = -1, g_hoverChip = -1;
int    g_hoverCell = -1;
int    g_emojiCat = 0, g_symCat = 0;

// ---------------------------------------------------------------- toast
std::wstring g_toast;
bool   g_toastOn = false, g_toastInfo = false;

// ---------------------------------------------------------------- 菜单 / 弹层
bool   g_menuOpen = false;
int    g_menuRow = -1, g_menuHover = -1;
int    g_menuItem = -1;
POINT  g_menuPt{0, 0};
bool   g_dialogOpen = false;
bool   g_modalOpen = false;
int    g_modalRow = -1;

// ---------------------------------------------------------------- 拖动 / 缩放
bool   g_dragging = false;
POINT  g_dragFrom{0, 0};
RECT   g_dragRect{0, 0, 0, 0};
int    g_resizing = 0;
POINT  g_rsFrom{0, 0};
RECT   g_rsRect{0, 0, 0, 0};

bool   g_hideNoRestore = false;
bool   g_pinned = false;
std::wstring g_typePending;
bool   g_inputMode = false;

// ---------------------------------------------------------------- 剪贴板借用
ClipBak g_clipBak;
bool    g_borrowedClip = false;
DWORD   g_selfClipSeq = 0;

// ---------------------------------------------------------------- 尺寸 / 定位
int    g_panelW = 0, g_panelH = 0;
int    g_savedW = 0, g_savedH = 0;
bool   g_caretOn = true;
int    g_gridScroll = 0;
bool   g_posUserSet = false;
int    g_reprobeLeft = 0;

// ---------------------------------------------------------------- 设备与字体资源
int    g_dpi = 96;
HFONT  g_fTitle = nullptr, g_fMeta = nullptr, g_fTab = nullptr, g_fChip = nullptr,
       g_fSearch = nullptr, g_fMono = nullptr, g_fEmoji = nullptr, g_fSym = nullptr,
       g_fKbd = nullptr, g_fBig = nullptr, g_fToast = nullptr, g_fSymName = nullptr;
HDC    g_ref = nullptr;
NOTIFYICONDATAW g_nid{};
bool   g_tray = false;
UINT   g_cfPng = 0;

// ---------------------------------------------------------------- D2D 资源
ID2D1Factory*          g_d2d = nullptr;
IDWriteFactory*        g_dw = nullptr;
ID2D1DCRenderTarget*   g_rt = nullptr;
HFONT                  g_fSymFallback[3] = {};
std::unordered_map<std::wstring, int> g_symFontSlot;
IDWriteFont*           g_probe[5] = {};
IDWriteTextFormat*     g_emojiFmt = nullptr;
ID2D1SolidColorBrush*  g_brush = nullptr;
ID2D1StrokeStyle*      g_roundStroke = nullptr;
IDWriteInlineObject*   g_ellipsis = nullptr;
bool                   g_painting = false;
std::unordered_map<HFONT, IDWriteTextFormat*> g_tfmt;

// ---------------------------------------------------------------- 字形库
std::vector<GCat>          g_symCats, g_emoCats;
std::vector<RecentGlyph>   g_recentSym, g_recentEmo;
std::vector<GItem>         g_recentSymItems, g_recentEmoItems;
int                        g_recentCount[2] = {0, 0};
std::vector<GItem>         g_glyphHits;
std::wstring               g_hitsQuery;
int                        g_hitsPage = -1;

bool   g_drawerOpen = false;
int    g_drawerScroll = 0, g_drawerHover = -1, g_drawerContentH = 0;
