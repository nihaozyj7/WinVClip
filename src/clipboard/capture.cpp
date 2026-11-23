// capture.cpp —— 实现见 capture.h。
#include "clipboard/capture.h"

#include <cstring>
#include <vector>

#include "core/app_state.h"    // g_main / g_items / g_panel
#include "core/io_png.h"       // Rgba
#include "core/model.h"        // ClipItem
#include "core/text_util.h"
#include "core/theme.h"
#include "core/util.h"         // Utf8Bytes / SizeText / FileBase / PathBytes
#include "image/raster.h"      // ReadPngSize / SameBits

using namespace wv;

// ================================================================== 剪贴板读取

void OnClipboardChanged() { SetTimer(g_main, TIMER_CLIP, 160, nullptr); }

std::wstring ForegroundSource() {
    HWND fg = GetForegroundWindow();
    if (!fg || fg == g_panel) return L"桌面";
    wchar_t title[256]{};
    GetWindowTextW(fg, title, 255);
    std::wstring s = title;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (s.empty() || s.size() > 24) {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (h) {
            wchar_t path[MAX_PATH]{};
            DWORD n = MAX_PATH;
            if (QueryFullProcessImageNameW(h, 0, path, &n)) {
                std::wstring base = FileBase(path);
                size_t dot = base.find_last_of(L'.');
                if (dot != std::wstring::npos) base.resize(dot);
                s = base;
            }
            CloseHandle(h);
        }
    }
    if (s.empty()) s = L"未知";
    if (s.size() > 20) s = s.substr(0, 20) + L"…";
    return s;
}

void CaptureClipboard() {
    // 自己写进去的内容（借用剪贴板粘贴表情/符号、或还原备份）不进历史，
    // 否则每点一个表情，历史里就多一条孤零零的 emoji。
    {
        DWORD seq = GetClipboardSequenceNumber();
        if (g_selfClipSeq && seq == g_selfClipSeq) return;
    }
    if (!OpenClipboard(g_main)) { SetTimer(g_main, TIMER_CLIP, 160, nullptr); return; }

    ClipItem it;
    GetLocalTime(&it.st);
    it.source = ForegroundSource();

    HANDLE h = nullptr;
    if ((h = GetClipboardData(CF_UNICODETEXT)) != nullptr) {
        auto* p = (const wchar_t*)GlobalLock(h);
        if (p) { it.kind = 0; it.text = p; GlobalUnlock(h); }
    }
    if (it.text.empty() && (h = GetClipboardData(CF_HDROP)) != nullptr) {
        auto drop = (HDROP)h;
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        if (n > 0 && n < 1000) {
            it.kind = 1;
            for (UINT i = 0; i < n; ++i) {
                UINT len = DragQueryFileW(drop, i, nullptr, 0);
                std::wstring s(len + 1, L'\0');
                DragQueryFileW(drop, i, &s[0], len + 1);
                s.resize(len);
                it.files.push_back(s);
            }
        }
    }
    if (it.text.empty() && it.files.empty()) {
        if (g_cfPng && (h = GetClipboardData(g_cfPng)) != nullptr) {
            SIZE_T sz = GlobalSize(h);
            auto* p = (const BYTE*)GlobalLock(h);
            if (p && sz > 32 && sz < 64u * 1024 * 1024) {
                it.kind = 2; it.isPng = true;
                it.bits.assign(p, p + sz);
                ReadPngSize(it.bits, it.imgW, it.imgH);
            }
            if (p) GlobalUnlock(h);
        } else if ((h = GetClipboardData(CF_DIB)) != nullptr) {
            SIZE_T sz = GlobalSize(h);
            auto* p = (const BYTE*)GlobalLock(h);
            if (p && sz > sizeof(BITMAPINFOHEADER) && sz < 128u * 1024 * 1024) {
                auto* bi = (const BITMAPINFOHEADER*)p;
                if (bi->biWidth > 0 && bi->biWidth < 100000) {
                    it.kind = 2; it.isPng = false;
                    it.bits.assign(p, p + sz);
                    it.imgW = bi->biWidth;
                    it.imgH = bi->biHeight < 0 ? -bi->biHeight : bi->biHeight;
                }
            }
            if (p) GlobalUnlock(h);
        }
    }
    CloseClipboard();

    if (it.kind == 0) {
        it.bytes = Utf8Bytes(it.text);
    } else if (it.kind == 1) {
        for (auto& f : it.files) it.bytes += PathBytes(f);
    } else {
        it.bytes = it.bits.size();
    }

    if (it.text.empty() && it.files.empty() && it.kind != 2) return;

    if (it.kind == 0) {
        std::wstring t = it.text;
        size_t nl = t.find_first_of(L"\r\n");
        if (nl != std::wstring::npos) t.resize(nl);
        for (auto& c : t) if (c == L'\t') c = L' ';
        if (t.size() > 200) t = t.substr(0, 200) + L"…";
        if (t.empty()) t = L"(空白文本)";
        it.title = t;

        std::wstring lo = Lower(it.text);
        if (lo.rfind(L"http://", 0) == 0 || lo.rfind(L"https://", 0) == 0 ||
            lo.rfind(L"www.", 0) == 0) {
            it.isLink = true; it.tag = L"链接";
        } else if (it.text.find(L'{') != std::wstring::npos ||
                   it.text.find(L";\n") != std::wstring::npos ||
                   lo.rfind(L"git ", 0) == 0 || lo.rfind(L"npm ", 0) == 0 ||
                   lo.rfind(L"const ", 0) == 0) {
            it.isCode = true; it.tag = L"代码";
        } else {
            it.tag = L"文本";
        }
    } else if (it.kind == 1) {
        it.title = FileBase(it.files[0]);
        if (it.files.size() > 1)
            it.title += L"  等 " + std::to_wstring(it.files.size()) + L" 项";
        it.tag = L"文件";
    } else {
        it.title = L"图片 " + std::to_wstring(it.imgW) + L" × " + std::to_wstring(it.imgH);
        it.tag = L"图片";
    }

    // 去重
    if (!g_items.empty()) {
        const ClipItem& f = g_items.front();
        bool same = (f.kind == it.kind);
        if (same) {
            if (it.kind == 0) same = (f.text == it.text);
            else if (it.kind == 1) same = (f.files == it.files);
            else same = (f.isPng == it.isPng && SameBits(f.bits, it.bits));
        }
        if (same) return;
        for (size_t i = 1; i < g_items.size(); ++i) {
            ClipItem& o = g_items[i];
            bool dup = (o.kind == it.kind);
            if (dup) {
                if (it.kind == 0) dup = (o.text == it.text);
                else if (it.kind == 1) dup = (o.files == it.files);
                else dup = (o.isPng == it.isPng && SameBits(o.bits, it.bits));
            }
            if (dup) { g_items.erase(g_items.begin() + i); break; }
        }
    }
    g_items.push_front(std::move(it));
    while (g_items.size() > 60) g_items.pop_back();
}