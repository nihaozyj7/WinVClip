// util.cpp —— 实现见 util.h。
//
// 这里只放「纯搬运」的自洽小函数：输入输出都是值，没有全局副作用
// （唯一例外是 MarkMine 往 INPUT 上盖一个标记）。正因如此，
// 它们可以随便被别处调用，不会引入任何循环依赖。
#include "core/util.h"

#include <cstdio>

#include "core/app_state.h"   // g_dpi / g_injectMark

void MarkMine(INPUT& in) { in.ki.dwExtraInfo = g_injectMark; }

int S(int v) { return MulDiv(v, g_dpi, 96); }

// ================================================================== 工具
// 字符串 / 编码转换见 text_util.h（Lower / U8N / NextCp / W2U8 / FileNameOf / StampName）

// 字形库（解析 / 索引 / 搜索 / 可画性）见 glyph.h / glyph.cpp

std::wstring FileBase(const std::wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? p : p.substr(i + 1);
}

// 文字度量（Fmt / TextW）见 text_measure.h / text_measure.cpp

std::wstring RelTime(const SYSTEMTIME& st) {
    FILETIME ft{}, ftNow{};
    SystemTimeToFileTime(&st, &ft);
    SYSTEMTIME now{};
    GetLocalTime(&now);
    SystemTimeToFileTime(&now, &ftNow);
    ULARGE_INTEGER a{}, b{};
    a.LowPart = ft.dwLowDateTime;  a.HighPart = ft.dwHighDateTime;
    b.LowPart = ftNow.dwLowDateTime; b.HighPart = ftNow.dwHighDateTime;
    long long sec = (long long)((b.QuadPart - a.QuadPart) / 10000000ULL);
    if (sec < 0) sec = 0;
    if (sec < 3600)       return std::to_wstring(sec < 60 ? 1 : sec / 60) + L"分钟前";
    if (sec < 86400)      return std::to_wstring(sec / 3600) + L"小时前";
    if (sec < 172800)     return L"昨天";
    wchar_t buf[16];
    swprintf(buf, 16, L"%02d-%02d", (int)st.wMonth, (int)st.wDay);
    return buf;
}

// 字节数 -> 可读大小（默认 KB，进到 1000KB 以上换 MB）
std::wstring SizeText(size_t bytes) {
    double kb = (double)bytes / 1024.0;
    if (kb < 1.0) kb = 1.0;
    wchar_t buf[32];
    int ikb = (int)(kb + 0.5);
    if (ikb < 1000) swprintf(buf, 32, L"%dKB", ikb);
    else            swprintf(buf, 32, L"%.1fMB", kb / 1024.0);
    return buf;
}

// 文本按 UTF-8 编码后的字节数（与剪贴板上实际占用的量一致）
size_t Utf8Bytes(const std::wstring& s) {
    size_t n = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned c = (unsigned)s[i];
        if (c < 0x80) n += 1;
        else if (c < 0x800) n += 2;
        else if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.size()) { n += 4; ++i; }
        else n += 3;
    }
    return n;
}

// 单个文件大小（目录和取不到的算 0）
size_t PathBytes(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fad)) return 0;
    if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return 0;
    return ((size_t)fad.nFileSizeHigh << 32) | (size_t)fad.nFileSizeLow;
}