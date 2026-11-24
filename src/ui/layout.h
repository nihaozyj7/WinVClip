// layout.h —— 面板布局：所有矩形都在这里算，绘制与命中判定都只读它。
//
// 为什么要把布局单独拆出来：
//   原来的 PanelProc 里有 36 处 PtIn(L.xxx, pt) 的线性判定，而 L 的每个字段
//   又是在 140 行的 ComputeLayout 里按「先算按钮、再算页签、最后算列表」的
//   顺序推出来的。这两件事都和绘制混在一个文件里，导致「改一个间距要读 500 行」。
//   现在：ComputeLayout 负责算，Paint* 只读，命中判定读同一份结果。
//
// 约定：
//   · L 是每帧重建的纯结果，不要往里面塞状态；状态在 app_state.h；
//   · 这里只做几何，不画、不改 g_* 里的任何东西（唯一例外是
//     「最近为空时把当前分类挪到第一个」，那是布局正确性的前提，见实现注释）。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>
#include <utility>
#include <vector>

// 设置卡片里的行号。用枚举而不是魔数：SetSwitchOfRow 要按行号认「哪几行是开关」，
// 新增一行时只要把新项追加在 SET_ROW_COUNT 之前，其余引用全都不用动。
// （定义在 Layout 之前：setRow 的数组长度就是 SET_ROW_COUNT。）
enum {
    SET_ROW_AUTO = 0,      // 开机自启动（开关）
    SET_ROW_ADMIN,         // 以管理员身份运行（开关）
    SET_ROW_THEME,         // 主题（三选）
    SET_ROW_TEMP,          // 临时文件目录（标题 + 路径 + 更改…）
    SET_ROW_LIMIT,         // 剪贴板记录上限（− 值 +）
    SET_ROW_CONFIRM,       // 删除前二次确认（开关）
    SET_ROW_CLEAR,         // 清空剪贴板历史（标题 + 副标题 + 清空按钮）
    SET_ROW_COUNT
};

struct Layout {
    int W, H;
    RECT head, tab[4], gear, closeB, pinB;
    RECT searchBox, filters, body, foot;
    RECT chip[8]; int chipCat[8] = {0}; int nChip = 0;
    RECT moreBtn;
    // 分类抽屉（打开时才有内容）
    RECT drawer, drawerHead, drawerClose;
    std::vector<RECT> dRect;
    std::vector<int>  dCat;
    std::vector<std::pair<RECT, std::wstring>> dSec;
    // 行数随面板高度变化，行高恒为 ROW_H（不拉伸）。nRow 含「底部被切掉一半」的
    // 那一行，且 row 里的矩形是完整的行矩形（可能越出 body 上下沿），裁切交给
    // PaintPanel 的 body clip。rowH 保留给滚动条等内容高度计算用。
    std::vector<RECT> row; int nRow = 0; int rowH = 0;
    // 滚动条：内容高 > 视口高时才有几何，否则两个矩形都是空。绘制与拖拽命中
    // 都读这一份，避免两边各算一遍导致「看得见滑块、按住却抓不到」。
    RECT scrollTrack, scrollThumb;
    int  scrollMax = 0;          // 可滚动的像素总数（0 = 不需要滚动条）
    RECT toast;
    // 设置窗口：卡片 = 整个客户区（见 ComputeSettings），与面板几何无关。
    // setTitle 是自绘标题行的可拖动区（窗口没有系统标题栏，拖动由窗口过程自己做）。
    RECT setCard, setClose, setTitle;
    RECT setRow[SET_ROW_COUNT];
    RECT setSw[3];              // 开机自启 / 管理员运行 / 删除前确认（开关行不连续，见 SetSwitchOfRow）
    RECT setSeg[3];             // 浅色 / 暗色 / 跟随系统
    RECT setBrowse;             // 「更改…」临时目录
    RECT setMinus, setValue, setPlus;   // 历史上限
    RECT setClear;              // 「清空」剪贴板历史
};

extern Layout L;

extern const wchar_t* TAB_NAME[4];
extern const wchar_t* FILTER_NAME[4];

// 算出一帧的全部几何（每帧 / 尺寸或页面变化时调用）
void ComputeLayout();

// 命中判定的最小单元：所有点击都先过它
bool PtIn(const RECT& r, POINT p);

// 分类抽屉
std::wstring CatFirstGlyph(int page, int idx);   // 分类图标位显示的首字形
void ComputeDrawer();
int  DrawerMaxScroll();
void ClampDrawerScroll();
int  DrawerHit(POINT p);       // 抽屉内命中的分类下标，-1 = 没命中
void ToggleDrawer(bool open);

// 滚动范围：把选中项滚进视野、把滚动量夹回合法区间
void ClampScroll();
// 列表滚动（像素级，主状态 g_scrollY，见 app_state.h）。
// 约定：只有这两个函数能改 g_scrollY —— 别处一律通过 SetListScroll / ScrollListByPx。
int  ListRowH();
void SyncListScroll();                    // 由 g_scrollY 重算派生量 g_top / g_rowOff
int  ListScrollMax();                     // 内容总高 - 可视高（需先 ComputeLayout）
void SetListScroll(int y, bool animate);  // animate=false 立即到位（重置/键盘导航）
void ResetListScroll();                   // 回到顶部并停掉缓动
void ScrollListByPx(int dy);              // 滚轮/拖动：设目标并启动缓动
bool TickListScroll();                    // 缓动一帧；false = 已到位（调用方 KillTimer）
int  GridContentH();
int  GridScrollMax();                     // 网格页能滚多远（像素）
void ScrollGridByPx(int dy);              // 网格页的滚轮：设目标并启动缓动（同上一条曲线）
void ClampGridScroll();
// 滚动条拖拽/点轨道用：把偏移直接设到 off（当前页面的滚动像素），立即到位、不走缓动
void SetScrollOffsetNow(int off);

// 字形网格
int  GlyphCols();
RECT GlyphCellRect(int idx);

extern const int MENU_SEP_BEFORE;
extern const int MENU_SEP_H;

// 由窗口/绘制层提供：布局改了可视状态后请求一次重绘
void Redraw(const RECT* r = nullptr);

// 右键菜单项下标（MenuHit 返回它；键盘上下也按它轮转）
enum { MI_PIN = 0, MI_SAVE, MI_DELETE, MI_COUNT };

// 设置窗口里的控件编码（SettingsHit / 悬停共用）。
// 行号见下面的 SET_ROW_*（行号即绘制顺序，只会往末尾追加：行号同时被这些编码、
//   PaintSettings 的 LABEL 和 SetSwitchOfRow 引用，插在中间会全部错位）。
enum {
    SET_NONE = -1,
    SET_SW_AUTO = 0,      // SET_ROW_AUTO 开关
    SET_SW_ADMIN = 1,     // SET_ROW_ADMIN 开关
    SET_SW_CONFIRM = 2,   // SET_ROW_CONFIRM 开关（= SET_SW_AUTO + setSw 下标）
    SET_SEG_0 = 10,       // SET_ROW_THEME 三选，+0/+1/+2 对应 THEME_*
    SET_BROWSE = 20,      // SET_ROW_TEMP 选目录
    SET_MINUS = 21,       // SET_ROW_LIMIT 减
    SET_PLUS = 22,        // SET_ROW_LIMIT 加
    SET_CLOSE = 30,       // 卡片右上关闭
    SET_CLEAR = 31,       // SET_ROW_CLEAR 清空历史
};

int  SetSwitchOfRow(int row);     // 行号 → setSw 下标；不是开关行返回 -1
int  SettingsHit(POINT p);     // 未命中返回 SET_NONE
// 设置窗口的客户区尺寸（由各行自然高度推出来，见 layout.cpp）。
// 设置窗口创建时用它们定大小；窗口过程每帧用实际客户区调 ComputeSettings。
int  SettingsWindowW();
int  SettingsWindowH();
// 算出设置窗口里全部控件的几何。w/h 是设置窗口的客户区尺寸
// （卡片 = 整个客户区，不再像内嵌弹层那样在面板里居中）。
void ComputeSettings(int w, int h);

// 弹层
RECT MenuRect();
RECT MenuItemRect(int i);
// 菜单项的图标框 / 文字框（宽度由标签实测决定，见 layout.cpp 的 MenuWidth）
RECT MenuItemIconRect(int i);
RECT MenuItemTextRect(int i);
// 菜单项文案（paint 与「按最长标签算宽」共用；pinned 决定显示「置顶」还是「取消置顶」）
const wchar_t* MenuLabel(int idx, bool pinned);
int  MenuHit(POINT p);
RECT ModalRect();
// 确认卡片在设置窗口里的几何（清空历史从这里发起）。与 ModalRect 同理，
// 只是在设置窗口的客户区里居中。
RECT SettingsModalRect();
RECT CenterIcon(const RECT& r, int size);
