// history_store.h —— 剪贴板历史与「最近使用」字形的磁盘持久化（SQLite）。
//
// 为什么改成数据库（原来是自写的二进制 history.dat，每次改动全量重写）：
//   · 全量重写在「复制了一份 20MB 日志」之后就变成真问题——每改一条都要重写 20MB，
//     而落盘还发生在 UI 线程上（TIMER_PERSIST）。页级读写下，一条大文本只写一次。
//   · 正文留在库里、内存里只放摘要和短文本，程序常驻时的内存占用不再随历史内容增长。
//   · 事务 + WAL：被任务管理器强杀时不会有「写了一半」的历史文件。
// 依赖是 vendored 的 sqlite3.c（third_party，公共领域，单文件、无外部依赖）。
//
// 位置：<临时目录>\history
//   · history.db   items / files / recent 三张表（正文就是 TEXT 列，尺寸交给数据库）
//   · img          图片条目的像素副本。条目里存的是**绝对路径**（img_path 列），
//                  所以改过临时目录之后，旧图片仍能按原路径读回来。
//
// 内存策略（这条决定了下面那些接口的语义）：
//   · g_items 仍是 UI 的唯一数据源，但只放元数据 + 不超过 kBodyInMemBytes 的短文本；
//     更长的正文留在库里，需要时用 EnsureItemText / HistorySearchBodies 取。
//   · 会话内新采集的正文要活到第一次提交（HistoryTouch 的 900ms 防抖），
//     提交成功之后才释放内存里的长正文——否则提交时就没东西可写了。
//
// 落盘时机：任何改动（新采集、删除、置顶/收藏、裁剪、最近使用）都调 HistoryTouch()，
//   它只排一个 900ms 的防抖定时器；定时器到点或 WM_DESTROY 时才提交。
//
// 旧数据迁移：目录里还留着老版本的 history.dat 时，首次启动会导入一次并把它改名为
//   history.dat.old（不删，用户的历史值得留一手回退）。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>
#include <vector>

#include "core/model.h"   // ClipItem

// <临时目录>\history（保证存在）
std::wstring HistoryDir();

// <临时目录>\history\img（保证存在）：图片条目像素副本的持久位置
std::wstring HistoryImgDir();

// 把一份像素写进 HistoryImgDir，返回绝对路径；失败返回空串
// （那一条就退化成「只活在内存里」，功能不受影响，只是重启后看不到这张图）。
// 图片像素不进数据库：它是几 MB 级的二进制大块，做成文件仍然是最省事的方式
// （删除条目时 DropItemTemp 顺手删掉，不用再管 BLOB 的回收）。
std::wstring StoreImageCopy(const std::vector<BYTE>& bits);

// 启动时读回历史与「最近使用」。库不存在时按需要：
//   有旧 history.dat → 导入；都没有 → 建空库。库损坏一律当作没有历史，
//   绝不为了「恢复一点」而把半截数据塞进列表。
void HistoryLoad();

// 标记「历史有改动」，排一次防抖提交（需要 g_main 已创建）
void HistoryTouch();

// 立刻提交（防抖定时器到点、退出、RelaunchElevated 之前）
void HistoryFlush();

// 用户改了「临时文件目录」：把旧目录里的数据库取到新目录来，
// 免得改一次设置就跟历史说了再见。oldDir 传改之前 HistoryDir() 的值。
void HistoryMigrateFrom(const std::wstring& oldDir);

// 取回一条目的正文（正文可能只在库里）。返回 text 是否可用。
// 任何「要用真内容」的地方（粘贴、另存为、去重、搜索）先调它。
bool EnsureItemText(ClipItem& it);

// 搜索：正文不在内存的条目交给数据库匹配，命中的 id 追加到 hitDbIds。
// ids 传「内存里便宜字段都没命中」的候选条目主键；query 需已小写化。
// 语义与内存里那条 Lower()+find 一致（ASCII 大小写不敏感；中文本身无大小写）。
void HistorySearchBodies(const std::vector<int>& ids, const std::wstring& query,
                         std::vector<int>& hitDbIds);
