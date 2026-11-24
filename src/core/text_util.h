// 字符串与编码转换工具。无状态、不依赖任何运行时全局。
//
// 这些函数原本散在 main.cpp 两个相距三千多行的位置（工具区与「另存为」区），
// 实际都是通用的编码转换，合到一处便于复用与单测。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

namespace wv {

// 逐字符转小写（宽字符，用于不区分大小写的过滤）
std::wstring Lower(std::wstring s);

// 不区分大小写的子串查找（needle 需已小写化）。**不分配内存**：搜索路径上每敲一个键
// 都会对全部条目做匹配，而 Lower(...) 是按值收参、拷两次，历史里有几条 16KB 正文时
// 每次 BuildView 就是几百 KB 的分配/释放。逐字符 towlower 比较足够（ASCII 大小写 +
// 中文本身无大小写）。
bool ContainsCI(const wchar_t* hay, size_t hayLen, const std::wstring& needleLower);

inline bool ContainsCI(const std::wstring& hay, const std::wstring& needleLower) {
    return ContainsCI(hay.c_str(), hay.size(), needleLower);
}

// UTF-8 → UTF-16，固定长度 n（不依赖 '\0' 结尾，用于解析内嵌数据表）
std::wstring U8N(const char* s, int n);

// 从 UTF-16 串的 i 位置取一个码位（正确处理代理对），i 前进 1 或 2。
// GDI 的 GetGlyphIndicesW 按码元算，遇到代理对必错，所以字形判定一律走这里。
UINT32 NextCp(const wchar_t* s, int& i, int n);

// UTF-16 → UTF-8
std::string W2U8(const std::wstring& s);

// 取路径最后一段（不查文件系统，纯字符串）
std::wstring FileNameOf(const std::wstring& p);

// 生成 20240131_235959 形式的时间戳文件名
std::wstring StampName(const SYSTEMTIME& t);

}  // namespace wv
