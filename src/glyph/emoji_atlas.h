// emoji_atlas.h —— 彩色 emoji 图集：后台光栅化 + 磁盘缓存 + 视口优先上传。
//
// 为什么必须异步：
//   软件光栅化 COLRv1 彩色字形实测 2~5ms/格（见 tools/bench_emoji.cpp），
//   一行 9 格就是 20~45ms，放在 UI 线程里必然卡顿，而且这个开销与格子尺寸、
//   像素格式无关——它就是软件光栅化的下限。所以分成三层：
//     1) 磁盘缓存：成品位图落盘（%LOCALAPPDATA%\WinVClip），第二次启动整块
//        memcpy 进 atlas，一个字形都不用再光栅化；
//     2) 后台预热：首次运行（无缓存）时由工作线程把全部 emoji 渲染一遍并写盘；
//     3) 视口优先：当前可见格走高优先队列，预热任务只在 UI 空闲时推进。
//
// 容量策略：
//   atlas 容量按「全部 emoji 条数」定，正常情况下不做淘汰——淘汰意味着切回旧分类
//   要重新光栅化，那正是「切分类要等一两秒」的来源。LRU 只在图集被 cap 住时兜底。
//
// 线程约束：
//   工作线程自带一套 D2D/DWrite 设备（COM 的 D2D 设备不能跨线程共享），
//   atlas 位图本身只由 UI 线程上传与使用，worker 只交出像素，不碰 D2D 位图。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d2d1.h>

#include <string>
#include <vector>

// 图集位图：仅 UI 线程访问（绘制布局段读它）
extern ID2D1Bitmap* g_emoAtlas;

// 格子步长（格宽 + 间隙）与图集边长，布局与绘制都按这两个算坐标
int EmojiStride();
int EmojiSide();

// 停止并回收工作线程（退出、DPI 变化时）
void StopEmojiWorker();

// 丢掉内存中的图集与索引（不清磁盘缓存）
void ReleaseEmojiCache();

// 确保图集按格子尺寸 sz 建好；尺寸变了会重建并重新读盘
bool EnsureEmojiCache(int sz);

// 启动/恢复后台预热线程
void StartEmojiWarm();

// UI 定时器驱动：把 worker 交回的像素上传进 atlas，返回是否还有工作在途
bool PumpEmojiUploads();

// 绘制前调用：把当前视口内的格子排进高优先队列
void PrepareEmojiBitmaps();

// 图集自检计数（给 PerfFlush 打日志用）：new=新光栅化，hit=命中缓存，
// fail=光栅化失败，nextSlot/atlasSlots=图集占用
void EmojiPerfSnapshot(int* newN, int* hitN, int* failN, int* nextSlot, int* slots);
void EmojiPerfReset();

// 面板尺寸记忆（%LOCALAPPDATA%\WinVClip\view-size.txt）
void LoadViewSize();
void SaveViewSize();

// 查字形在图集里的槽位（未排上队返回 false）；EmojiSlotReady 判断像素是否已上传
bool EmojiSlotFor(const std::string& key, int* slot);
bool EmojiSlotReady(int slot);

// 槽位在图集里的源矩形（绘制时用）
D2D1_RECT_F EmojiSlotRect(int slot);

// 由 UI 层提供（布局段 / 绘制段）。图集需要知道「当前视口要哪些格子、一行几列」，
// 这是本模块唯一一处向上的调用——用函数注入而不是让图集自己去摸布局状态。
int GlyphCols();
void CollectEmojiCells(std::vector<std::pair<RECT, int>>& out);
