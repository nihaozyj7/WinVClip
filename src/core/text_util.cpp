#include "core/text_util.h"

#include <cstdio>
#include <cwctype>

namespace wv {

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

bool ContainsCI(const wchar_t* hay, size_t hayLen, const std::wstring& needleLower) {
    if (needleLower.empty()) return true;
    if (!hay || hayLen < needleLower.size()) return false;
    const size_t last = hayLen - needleLower.size();
    const wchar_t first = needleLower[0];
    for (size_t i = 0; i <= last; ++i) {
        // 先比首字符（绝大多数位置在这里就被排除），再逐字符确认整段
        if ((wchar_t)towlower(hay[i]) != first) continue;
        size_t j = 1;
        while (j < needleLower.size() && (wchar_t)towlower(hay[i + j]) == needleLower[j]) ++j;
        if (j == needleLower.size()) return true;
    }
    return false;
}

std::wstring U8N(const char* s, int n) {
    if (!s || n <= 0) return L"";
    int w = MultiByteToWideChar(CP_UTF8, 0, s, n, nullptr, 0);
    std::wstring out(w > 0 ? (size_t)w : 0, L'\0');
    if (w > 0) MultiByteToWideChar(CP_UTF8, 0, s, n, &out[0], w);
    return out;
}

UINT32 NextCp(const wchar_t* s, int& i, int n) {
    UINT32 c = (UINT32)(unsigned short)s[i++];
    if (c >= 0xD800 && c <= 0xDBFF && i < n) {
        UINT32 lo = (UINT32)(unsigned short)s[i];
        if (lo >= 0xDC00 && lo <= 0xDFFF) {
            ++i;
            return 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
        }
    }
    return c;
}

std::string W2U8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n, nullptr, nullptr);
    return out;
}

std::wstring FileNameOf(const std::wstring& p) {
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? p : p.substr(s + 1);
}

std::wstring StampName(const SYSTEMTIME& t) {
    wchar_t b[32];
    swprintf(b, 32, L"%04d%02d%02d_%02d%02d%02d",
             t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return b;
}

}  // namespace wv
