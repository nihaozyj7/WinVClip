// showhide.h —— 面板的显示、隐藏与落点。
//
// 「失焦即隐藏」的整套机制都在这儿，也是整个项目最容易踩坑的地方：
//   · 不在失焦后去抢回前台（抢回来之后就再没有失活事件，面板会永久挂在最上层）；
//   · 显示期间用 ~150ms 定时器轮询前台窗口兜底（AttachThreadInput 抢来的前台
//     常常收不到 WA_INACTIVE）；
//   · 非激活面板还有个盲区：用户在同一个窗口里点掉面板时前台纹丝不动，
//     所以另用 WH_MOUSE_LL 判断「按下的点是否在面板矩形外」。
// 改动这里的任何时序之前，先读 test_noactivate.py / test_hide_outside.py。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "ui/caret.h"   // CaretInfo (MovePanelToCaret 的入参)

// 弹出面板（先探测目标窗口的插入符，再决定落点）
void ShowPanel();

// 收起面板（把前台与控件级焦点还给调用前的目标窗口）
void HidePanel();

// 面板此刻应不应该继续留着（固定态、菜单/弹层打开、有对话框时都要留着）
bool PanelShouldStay(HWND hwnd);

// 把面板摆到插入符附近，尽量不遮住输入框，且完整落在工作区内
void PlacePanel();

// 按 CaretInfo 调整已有面板的位置
void MovePanelToCaret(const CaretInfo& ci);

// 圆角窗口：DWM 属性 + 区域裁剪
void RoundCorners(HWND hwnd);

// 把前台强抢给 hwnd（跨进程常被前台锁静默拒绝，需要重试）
void ForceForeground(HWND hwnd);

// 还原调用前的控件级焦点（SetForegroundWindow 只还原顶层窗口）
void RestoreTargetFocus(HWND target, HWND focus);

// 面板隐藏期间的闲置回收。分两档（见 app_state.h 的 IDLE_RELEASE_MS 注释）：
//   · 每次闲置到点先 trim 工作集：任务管理器里看到的「内存」就是工作集，
//     被换出的私有页进了 standby，物理内存真的还给了系统，且随时能零成本换回来；
//   · 连续闲置很久（30 分钟）才连 emoji 图集一起放掉——那时用户多半已经离开，
//     再进表情页重新读盘付 ~200ms 可以接受。
// 注意**不释放主线程的 D2D 设备**：重建实测要 60ms，那正好压在「按 Win+V 到面板出现」
// 这条热路径上，宁可让它的私有页留在 standby 里也不要付这个延迟。
void ReleaseIdleMemory();
