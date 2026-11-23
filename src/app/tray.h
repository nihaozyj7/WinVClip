// tray.h —— 托盘图标（常驻入口）。
//
// 单独拆出来是因为它跟面板是两条互不相干的生命周期：面板反复显示/隐藏，
// 托盘图标从进程启动到退出只创建一次。混在 main 里容易误以为二者相关。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

void AddTray();
void RemoveTray();
