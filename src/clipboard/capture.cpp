// capture.cpp —— 实现见 capture.h。
#include "clipboard/capture.h"

#include <cstring>
#include <vector>

#include "core/app_state.h"    // g_main / g_items / g_panel
#include "core/history_store.h" // StoreImageCopy / HistoryTouch（历史跨进程保留）
#include "core/io_png.h"       // Rgba
#include "core/model.h"        // ClipItem
#include "core/settings.h"     // g_cfg / HIST_MIN
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

// 图片条目的内存预算。
// ClipItem::bits 存的是原始数据（浏览器复制图片是 PNG，截图工具/Office 常给 CF_DIB，
// 未压缩的 DIB 一条 4K 就是 33MB）。像素会先落一份到 <临时目录>\history\img
// （见 core/history_store.h），内存里只留最近几张（见 TrimImageBits），所以不再需要
// 「按容量丢条目」这种牺牲历史的做法：内容在磁盘上，需要时用 EnsureItemBits 读回来。
static const size_t IMG_MEM_BUDGET = 32u * 1024 * 1024;
static const int    IMG_KEEP = 4;          // 无论预算如何，最近这 4 张的像素留在内存里

// 内容哈希（FNV-1a）：只为了给去重做粗筛，真判等仍靠 memcmp
static size_t HashBits(const std::vector<BYTE>& b) {
    size_t h = 1469598103934665603ull;
    for (BYTE c : b) { h ^= c; h *= 1099511628211ull; }
    return h;
}

// 像素副本落进 <临时目录>\history\img（见 core/history_store.h）。它既是「内存预算
// 释放后还能读回来」的来源，也是「重启后这条图片还在」的唯一内容来源。
// 失败返回空：那一张退化成只活在内存里，功能不受影响，只是重建缩略图要重新解码。
static std::wstring SaveTempImageCopy(const std::vector<BYTE>& bits) {
    return StoreImageCopy(bits);
}

void DropItemTemp(const ClipItem& it) {
    if (!it.diskPath.empty()) DeleteFileW(it.diskPath.c_str());
}

// 剪贴板里现在装的是不是这一条。只比文本与文件列表：
//   · 图片条目从剪贴板里删掉之后再去唤出面板的情形几乎不存在，而且比像素要把
//     CF_DIB 再解一遍，代价与收益不成比例；
//   · 正文可能只存在库里（见 history_store.h 的内存策略），所以调用方要先
//     EnsureItemText，取不回来时 text 为空——这时一律当作「不是同一条」，
//     宁可多冒一次「删掉的条目被捡回来」的风险，也不要把无关内容错当成它。
bool ClipboardHoldsItem(const ClipItem& it) {
    if (it.kind != 0 && it.kind != 1) return false;
    if (it.kind == 0 && it.text.empty()) return false;
    if (!OpenClipboard(g_main)) return false;
    bool same = false;
    if (it.kind == 0) {
        if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
            if (auto* p = (const wchar_t*)GlobalLock(h)) {
                same = (it.text == p);
                GlobalUnlock(h);
            }
        }
    } else if (HANDLE h = GetClipboardData(CF_HDROP)) {
        auto drop = (HDROP)h;
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        if (n == it.files.size()) {
            same = true;
            for (UINT i = 0; i < n && same; ++i) {
                UINT len = DragQueryFileW(drop, i, nullptr, 0);
                std::wstring s(len + 1, L'\0');
                DragQueryFileW(drop, i, &s[0], len + 1);
                s.resize(len);
                if (s != it.files[i]) same = false;
            }
        }
    }
    CloseClipboard();
    return same;
}

// 释放超出预算的像素（从最新的往后数，最近的保留）。有磁盘副本才敢丢内存。
static void TrimImageBits() {
    size_t total = 0;
    int n = 0;
    for (auto& it : g_items) {
        if (it.kind != 2 || it.bits.empty() || it.diskPath.empty()) continue;
        ++n;
        total += it.bits.size();
        if (n > IMG_KEEP || total > IMG_MEM_BUDGET) {
            total -= it.bits.size();
            std::vector<BYTE>().swap(it.bits);
        }
    }
}

void ApplyHistoryLimit() {
    int lim = g_cfg.historyLimit;
    if (lim < HIST_MIN) lim = HIST_MIN;
    if (lim > HIST_MAX) lim = HIST_MAX;

    // 上限只约束「普通历史」：置顶与收藏不占名额，也不会被裁掉——它们是用户显式
    // 标记过的数据，和历史不是同一条线（清空历史同理，见 actions.cpp 的
    // ClearHistoryKeepPinnedFav；载入时也必须把它们全取回来，见 HistoryLoad）。
    // g_items 是「最新在前」，所以正向数够 lim 条普通条目之后，剩下的都是更旧的。
    int normal = 0;
    for (const auto& it : g_items) if (!it.pinned && !it.fav) ++normal;
    if (normal > lim) {
        std::deque<ClipItem> keep;
        int kept = 0;
        for (auto& it : g_items) {
            if (it.pinned || it.fav) { keep.push_back(std::move(it)); continue; }
            if (kept < lim) { ++kept; keep.push_back(std::move(it)); continue; }
            DropItemTemp(it);
        }
        g_items.swap(keep);
    }
    TrimImageBits();
}

// 与已有文本条目比较正文。已有条目的超长正文可能只在数据库里（见 core/history_store.h
// 的内存策略），先取回来再比；取不回来就当作不同——宁可多出一条历史，
// 也不要把用户的记录误去重掉。
static bool SameText(const ClipItem& old, const std::wstring& text) {
    if (!old.text.empty()) return old.text == text;
    ClipItem tmp = old;
    return EnsureItemText(tmp) && tmp.text == text;
}

// 返回值 = 「历史是否真的变了」。调用方（TIMER_CLIP）据此决定要不要重建视图：
// 剪贴板序号变了但内容与已有条目重复、或者只是被清空，都不该触发一次全量重排 + 重绘。
// 剪贴板被占用时的退避步长（毫秒）。成功打开后清零，见函数内说明。
static int g_clipRetry = 0;

bool CaptureClipboard() {
    DWORD seq = GetClipboardSequenceNumber();
    // 自己写进去的内容（借用剪贴板粘贴表情/符号、或还原备份）不进历史，
    // 否则每点一个表情，历史里就多一条孤零零的 emoji。
    if (g_selfClipSeq && seq == g_selfClipSeq) return false;
    // 用户刚删掉的那条正躺在剪贴板里（见 app_state.h 的 g_delClipSeq）：
    // 唤出面板时这次补捕必须跳过它，否则删掉的内容立刻被捡回来。
    if (g_delClipSeq && seq == g_delClipSeq) return false;
    if (!OpenClipboard(g_main)) {
        // 剪贴板被别人占着：OpenClipboard 立即失败（不等待），退避重试。
        // 固定 160ms 无限重试在「有个程序长期持着剪贴板」时会一直滴答，这里指数退避到 2s 封顶。
        static const int kStep[5] = {160, 320, 640, 1200, 2000};
        SetTimer(g_main, TIMER_CLIP, kStep[g_clipRetry < 5 ? g_clipRetry : 4], nullptr);
        if (g_clipRetry < 5) ++g_clipRetry;
        return false;
    }
    g_clipRetry = 0;

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
        it.hash = HashBits(it.bits);
    }

    if (it.text.empty() && it.files.empty() && it.kind != 2) return false;

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

    // 去重（图片先比 size+hash，只有两者都相同才做全量 memcmp）
    auto sameBits = [](const ClipItem& a, const ClipItem& b) {
        return a.isPng == b.isPng && a.bits.size() == b.bits.size() && a.hash == b.hash &&
               SameBits(a.bits, b.bits);
    };
    if (!g_items.empty()) {
        ClipItem& f = g_items.front();
        bool same = (f.kind == it.kind);
        if (same) {
            if (it.kind == 0) same = SameText(f, it.text);
            else if (it.kind == 1) same = (f.files == it.files);
            else same = EnsureItemBits(f) && sameBits(f, it);
        }
        if (same) return false;
        for (size_t i = 1; i < g_items.size(); ++i) {
            ClipItem& o = g_items[i];
            bool dup = (o.kind == it.kind);
            if (dup) {
                if (it.kind == 0) dup = SameText(o, it.text);
                else if (it.kind == 1) dup = (o.files == it.files);
                else dup = EnsureItemBits(o) && sameBits(o, it);
            }
            if (dup) {
                DropItemTemp(o);      // 旧的副本文件先删，别留成垃圾
                g_items.erase(g_items.begin() + (ptrdiff_t)i);
                break;
            }
        }
    }
    // 像素副本落盘放在去重之后：去重提前返回时不会留下没人引用的文件
    if (it.kind == 2 && !it.bits.empty()) it.diskPath = SaveTempImageCopy(it.bits);

    g_items.push_front(std::move(it));
    ApplyHistoryLimit();      // 历史上限 + 图片像素的内存预算（见 settings.h）
    HistoryTouch();           // 新条目排一次防抖落盘
    return true;
}