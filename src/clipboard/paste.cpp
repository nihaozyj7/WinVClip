// paste.cpp —— 实现见 paste.h。
//
// 注入的每一个事件都必须 MarkMine()：自己的键盘钩子靠 dwExtraInfo 区分
// 「本程序注入」和「其他来源」，否则固定态连插多个表情时，注入的字符会被
// 自己的钩子吞掉（这个坑实测踩过，见 app_state.h 对 g_injectMark 的说明）。
#include "clipboard/paste.h"

#include <shlobj.h>   // DROPFILES

#include <vector>

#include "core/app_state.h"    // g_pasteTarget / g_targetFocus / g_injectMark
#include "core/history_store.h" // EnsureItemText（长正文只在库里）
#include "core/model.h"
#include "core/text_util.h"
#include "core/util.h"         // MarkMine
#include "image/raster.h"      // EnsureItemBits（图片像素的临时副本）

using namespace wv;

void PutToClipboard(const ClipItem& itIn) {
    // 图片像素可能只躺在临时目录里（内存预算已释放），写剪贴板前先读回来；
    // 文本正文同理：超过阈值的正文只在数据库里（见 core/history_store.h）。
    ClipItem copy;
    const ClipItem* use = &itIn;
    if (itIn.kind == 2 && itIn.bits.empty()) {
        copy = itIn;
        if (!EnsureItemBits(copy)) return;
        use = &copy;
    } else if (itIn.kind == 0 && itIn.text.empty()) {
        copy = itIn;
        if (!EnsureItemText(copy)) return;   // 取不回来就别把空文本塞给目标窗口
        use = &copy;
    }
    const ClipItem& it = *use;

    if (!OpenClipboard(g_main)) return;
    EmptyClipboard();

    if (it.kind == 0) {
        size_t bytes = (it.text.size() + 1) * sizeof(wchar_t);
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (h) {
            void* p = GlobalLock(h);
            if (p) {
                memcpy(p, it.text.c_str(), bytes);
                GlobalUnlock(h);
            }
            if (!p || !SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
        }
    } else if (it.kind == 1) {
        size_t chrs = 1;
        for (auto& f : it.files) chrs += f.size() + 1;
        size_t total = sizeof(DROPFILES) + chrs * sizeof(wchar_t);
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, total);
        if (h) {
            auto* df = (DROPFILES*)GlobalLock(h);
            if (!df) { GlobalFree(h); CloseClipboard(); return; }
            df->pFiles = sizeof(DROPFILES);
            df->fWide = TRUE;
            auto* p = (wchar_t*)((BYTE*)df + sizeof(DROPFILES));
            for (auto& f : it.files) {
                memcpy(p, f.c_str(), (f.size() + 1) * sizeof(wchar_t));
                p += f.size() + 1;
            }
            *p = 0;
            GlobalUnlock(h);
            if (!SetClipboardData(CF_HDROP, h)) GlobalFree(h);
        }
    } else if (it.kind == 2 && !it.bits.empty()) {
        UINT fmt = (it.isPng && g_cfPng) ? g_cfPng : CF_DIB;
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, it.bits.size());
        if (h) {
            void* p = GlobalLock(h);
            if (p) {
                memcpy(p, it.bits.data(), it.bits.size());
                GlobalUnlock(h);
            }
            if (!p || !SetClipboardData(fmt, h)) GlobalFree(h);
        }
    }
    CloseClipboard();
}

// 直接把字符注入到焦点窗口（KEYEVENTF_UNICODE），完全不碰剪贴板：
// 点一个表情不该把用户自己的剪贴板内容和历史都顶掉。
// 代理对（emoji）按 UTF-16 码元逐个发，目标侧的 WM_CHAR 会自己合成。
void TypeUnicode(const std::wstring& s) {
    if (s.empty()) return;
    std::vector<INPUT> in;
    in.reserve(s.size() * 2);
    for (wchar_t c : s) {
        INPUT a{};
        a.type = INPUT_KEYBOARD;
        a.ki.wVk = 0;
        a.ki.wScan = (WORD)c;
        a.ki.dwFlags = KEYEVENTF_UNICODE;
        MarkMine(a);
        in.push_back(a);
        a.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        in.push_back(a);
    }
    SendInput((UINT)in.size(), in.data(), sizeof(INPUT));
}

void PutText(const std::wstring& s) {
    if (!OpenClipboard(g_main)) return;
    EmptyClipboard();
    size_t bytes = (s.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (h) {
        void* p = GlobalLock(h);
        if (p) {
            memcpy(p, s.c_str(), bytes);
            GlobalUnlock(h);
        }
        if (!p || !SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
    }
    CloseClipboard();
}

// 备份当前剪贴板（只备内容型格式：文本 / 文件列表 / PNG / DIB）。
// 目的：给「取不到插入符」的目标窗口走 Ctrl+V 通道时，粘完还能把用户原来的
// 剪贴板还回去 —— 点一个表情不该留下痕迹。
void BorrowClipboard() {
    if (g_borrowedClip) return;      // 连续点多个字形：只备份第一次，别把上一次的临时内容当成原内容
    g_clipBak = ClipBak{};
    if (!OpenClipboard(g_main)) { g_borrowedClip = true; return; }

    HANDLE h = nullptr;
    if ((h = GetClipboardData(CF_UNICODETEXT)) != nullptr) {
        if (auto* p = (const wchar_t*)GlobalLock(h)) {
            g_clipBak.hadText = true;
            g_clipBak.text = p;
            GlobalUnlock(h);
        }
    }
    if (!g_clipBak.hadText && (h = GetClipboardData(CF_HDROP)) != nullptr) {
        UINT n = DragQueryFileW((HDROP)h, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n && n < 1000; ++i) {
            UINT len = DragQueryFileW((HDROP)h, i, nullptr, 0);
            std::wstring s(len + 1, L'\0');
            DragQueryFileW((HDROP)h, i, &s[0], len + 1);
            s.resize(len);
            g_clipBak.files.push_back(s);
        }
    }
    if (!g_clipBak.hadText && g_clipBak.files.empty()) {
        if (g_cfPng && (h = GetClipboardData(g_cfPng)) != nullptr) {
            SIZE_T sz = GlobalSize(h);
            if (auto* p = (const BYTE*)GlobalLock(h)) {
                if (sz > 32 && sz < 32u * 1024 * 1024) g_clipBak.png.assign(p, p + sz);
                GlobalUnlock(h);
            }
        } else if ((h = GetClipboardData(CF_DIB)) != nullptr) {
            SIZE_T sz = GlobalSize(h);
            if (auto* p = (const BYTE*)GlobalLock(h)) {
                if (sz > sizeof(BITMAPINFOHEADER) && sz < 32u * 1024 * 1024)
                    g_clipBak.dib.assign(p, p + sz);
                GlobalUnlock(h);
            }
        }
    }
    CloseClipboard();
    g_clipBak.valid = true;
    g_borrowedClip = true;
}

// 还原备份。安全前提：剪贴板里还是我们自己写的那份（序列号比对），
// 期间若有别人复制过东西就放弃还原，绝不覆盖新内容。
void RestoreClipboard() {
    if (!g_borrowedClip) return;
    g_borrowedClip = false;
    if (g_clipBak.valid && (!g_selfClipSeq || GetClipboardSequenceNumber() == g_selfClipSeq) &&
        OpenClipboard(g_main)) {
        EmptyClipboard();
        if (g_clipBak.hadText) {
            size_t bytes = (g_clipBak.text.size() + 1) * sizeof(wchar_t);
            if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
                void* p = GlobalLock(h);
                if (p) {
                    memcpy(p, g_clipBak.text.c_str(), bytes);
                    GlobalUnlock(h);
                }
                if (!p || !SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
            }
        } else if (!g_clipBak.files.empty()) {
            size_t chrs = 1;
            for (auto& f : g_clipBak.files) chrs += f.size() + 1;
            size_t total = sizeof(DROPFILES) + chrs * sizeof(wchar_t);
            if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, total)) {
                auto* df = (DROPFILES*)GlobalLock(h);
                if (!df) { GlobalFree(h); }
                else {
                df->pFiles = sizeof(DROPFILES);
                df->fWide = TRUE;
                auto* p = (wchar_t*)((BYTE*)df + sizeof(DROPFILES));
                for (auto& f : g_clipBak.files) {
                    memcpy(p, f.c_str(), (f.size() + 1) * sizeof(wchar_t));
                    p += f.size() + 1;
                }
                *p = 0;
                GlobalUnlock(h);
                if (!SetClipboardData(CF_HDROP, h)) GlobalFree(h);
                }
            }
        } else if (!g_clipBak.png.empty() && g_cfPng) {
            if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, g_clipBak.png.size())) {
                void* p = GlobalLock(h);
                if (p) {
                    memcpy(p, g_clipBak.png.data(), g_clipBak.png.size());
                    GlobalUnlock(h);
                }
                if (!p || !SetClipboardData(g_cfPng, h)) GlobalFree(h);
            }
        } else if (!g_clipBak.dib.empty()) {
            if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, g_clipBak.dib.size())) {
                void* p = GlobalLock(h);
                if (p) {
                    memcpy(p, g_clipBak.dib.data(), g_clipBak.dib.size());
                    GlobalUnlock(h);
                }
                if (!p || !SetClipboardData(CF_DIB, h)) GlobalFree(h);
            }
        }
        g_selfClipSeq = GetClipboardSequenceNumber();   // 还原动作本身也不该进历史
        CloseClipboard();
    }
    g_clipBak = ClipBak{};
}