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

struct Layout {
    int W, H;
    RECT head, logo, tab[4], gear, closeB, pinB;
    RECT searchBox, filters, body, foot;
    RECT chip[8]; int chipCat[8] = {0}; int nChip = 0;
    RECT moreBtn;
    // 分类抽屉（打开时才有内容）
    RECT drawer, drawerHead, drawerClose;
    std::vector<RECT> dRect;
    std::vector<int>  dCat;
    std::vector<std::pair<RECT, std::wstring>> dSec;
    std::vector<RECT> row; int nRow = 0;    // 行数随面板高度变化，不再固定
    RECT toast;
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
int  GridContentH();
void ClampGridScroll();

// 字形网格
int  GlyphCols();
RECT GlyphCellRect(int idx);

extern const int MENU_SEP_BEFORE;
extern const int MENU_SEP_H;

// 由窗口/绘制层提供：布局改了可视状态后请求一次重绘
void Redraw(const RECT* r = nullptr);

// 右键菜单项下标（MenuHit 返回它；键盘上下也按它轮转）
enum { MI_PIN = 0, MI_SAVE, MI_DELETE, MI_COUNT };

// 弹层
RECT MenuRect();
RECT MenuItemRect(int i);
int  MenuHit(POINT p);
RECT ModalRect();
RECT CenterIcon(const RECT& r, int size);
