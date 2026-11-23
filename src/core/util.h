// util.h —— 与业务无关的小工具：文件名、相对时间、可读大小、字节计数。
//
// 共同点是没有状态、没有依赖（除 DPI），所以沉在最底层，谁都可以用，
// 它们也不该反过来知道任何关于面板/剪贴板的事。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

// 给注入的 INPUT 打上本程序标记（理由见 app_state.h 里的 g_injectMark）
void MarkMine(INPUT& in);

// DPI 缩放。设计稿按 96 DPI 出图，所有像素尺寸都必须过这一层，
// 不要直接写字面量——否则 150% 缩放下整块布局会错位。
int S(int v);

std::wstring FileBase(const std::wstring& p);    // 取路径最后一段
std::wstring RelTime(const SYSTEMTIME& st);      // 「3分钟前」「昨天」「11-20」
std::wstring SizeText(size_t bytes);             // 「12KB」「1.4MB」
size_t Utf8Bytes(const std::wstring& s);         // 与剪贴板上实际占用一致
size_t PathBytes(const std::wstring& p);         // 单个文件的大小；目录算 0
