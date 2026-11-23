# -*- coding: utf-8 -*-
# 把「彩色字形缓存」整段替换成：磁盘持久化 atlas + 后台全量预热 + 视口优先队列
import io

P = "src/main.cpp"
raw = io.open(P, "r", encoding="utf-8", newline="").read()
CRLF = "\r\n" in raw
src = raw.replace("\r\n", "\n")          # 统一成 LF 做替换，最后再转回去

NEW = u"""// ------------------------------------------------ 彩色字形（后台光栅化 + 磁盘持久化缓存）
// 软件光栅化 COLRv1 彩色字形实测 2~5ms/格（见 tools/bench_emoji.cpp），一行 9 格就是
// 20~45ms，放在 UI 线程里必然卡顿，且与格子尺寸/像素格式无关 —— 那是软件光栅化的下限。
// 所以分成三层：
//   1) 磁盘缓存：全部 emoji 的成品位图落盘（%LOCALAPPDATA%\\WinVClip），第二次启动整块
//      memcpy 进 atlas，一个字形都不用再光栅化；
//   2) 后台预热：首次运行（无缓存）时由工作线程把全部 emoji 渲染一遍并写盘；
//   3) 视口优先：当前可见格走高优先队列，预热任务只在 UI 空闲时推进。
// atlas 容量按「全部 emoji 条数」定，正常情况下不做淘汰 —— 淘汰意味着切回旧分类要重新
// 光栅化，那正是「切分类要等一两秒」的来源。LRU 只在图集被 cap 住时兜底。

struct EmoReq { std::string key; int slot = -1; };            // slot<0 = 只渲染不落槽
struct EmoDone { int slot = -1; std::string key; std::vector<uint32_t> px; };

static ID2D1Bitmap* g_emoAtlas = nullptr;                     // 主设备的图集（仅 UI 线程访问）
static int g_emoSz = 0;
static int g_emoAtlasCols = 0, g_emoAtlasSlots = 0, g_emoNextSlot = 0;
static std::unordered_map<std::string, int> g_emoSlot;        // 字形 -> atlas 槽位
static std::vector<std::string>   g_emoSlotKey;               // 槽位 -> 字形（淘汰/写盘用）
static std::vector<int>           g_emoSlotUsed;              // 槽位 -> 最近使用的帧号
static std::vector<unsigned char> g_emoReady;                 // 槽位 -> 像素是否已上传
static int g_emoFrame = 0;

// 预热 / 落盘状态
static std::vector<uint32_t> g_emoDisk;      // 预热期间的整块镜像，写完盘立刻释放
static std::vector<EmoReq>   g_emoWarmJobs;  // 全量预热清单（UI 一次性写入，worker 只读）
static std::atomic<int>      g_emoWarmIdx{0};
static bool g_emoWarmPosted = false;         // 是否已开始预热（或已从缓存恢复）
static bool g_emoWarmSaved  = false;         // 是否已把图集落盘
static int  g_emoWarmSlots  = 0;             // 预热涉及的槽数
static int  g_emoUpN        = 0;             // 已上传进 atlas 的槽数
static bool g_emoCacheTried = false;         // 是否已尝试读盘
static int  g_emoTotal      = 0;             // 全部 emoji 条数（决定图集容量）

static std::vector<std::thread> g_emoThr;    // 2 个 worker
static std::mutex              g_emoMx;
static std::condition_variable g_emoCv;
static std::vector<EmoReq>     g_emoReq;     // 视口请求（后进先出）
static std::unordered_set<std::string> g_emoPending;   // 视口请求：已投递未完成
static std::vector<EmoDone>    g_emoDone;    // 已完成
static bool g_emoQuit = false;
static int  g_emoNewN = 0, g_emoHitN = 0, g_emoFailN = 0;

static void CollectEmojiCells(std::vector<std::pair<RECT, int>>& out);  // 定义在绘制段
static int EmojiCols();   // 定义在布局段
static int EmojiStride() { return g_emoSz + S(4); }
static int EmojiSide()   { return g_emoAtlasCols * EmojiStride(); }

// 工作线程主体：自带一套 D2D/DWrite 设备，循环取请求、渲染、交出像素。
// 取任务顺序 = 视口请求（LIFO）-> 预热（顺序推进），保证切分类/滚动立刻可见。
static void EmojiWorkerMain(int sz) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const int stride = sz + S(4);

    ID2D1Factory* d2d = nullptr;
    IDWriteFactory* dwf = nullptr;
    ID2D1DCRenderTarget* rt = nullptr;
    IDWriteTextFormat* fmt = nullptr;
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    void* bits = nullptr;
    bool ready = false;

    do {
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d)) || !d2d) break;
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       (IUnknown**)&dwf)) || !dwf) break;
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);
        if (FAILED(d2d->CreateDCRenderTarget(&props, &rt)) || !rt) break;

        HDC scr = GetDC(nullptr);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = stride;
        bi.bmiHeader.biHeight = -stride;      // 自上而下
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        bmp = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        dc = CreateCompatibleDC(scr);
        ReleaseDC(nullptr, scr);
        if (!bmp || !dc || !bits) break;
        SelectObject(dc, bmp);
        RECT br{0, 0, stride, stride};
        rt->BindDC(dc, &br);
        rt->SetDpi(96.0f, 96.0f);

        if (FAILED(dwf->CreateTextFormat(L"Segoe UI Emoji", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                         (float)S(21), L"", &fmt)) || !fmt) break;
        fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        ready = true;
    } while (false);

    std::vector<uint32_t> px((size_t)stride * stride);
    for (;;) {
        EmoReq r;
        {
            std::unique_lock<std::mutex> lk(g_emoMx);
            g_emoCv.wait_for(lk, std::chrono::milliseconds(20), [] {
                return g_emoQuit || !g_emoReq.empty() ||
                       g_emoWarmIdx.load() < (int)g_emoWarmJobs.size();
            });
            if (g_emoQuit) break;
            if (!g_emoReq.empty()) {
                r = std::move(g_emoReq.back());     // 后进先出：最近滚到的格子优先
                g_emoReq.pop_back();
            } else {
                int wi = g_emoWarmIdx.fetch_add(1);
                if (wi >= (int)g_emoWarmJobs.size()) continue;   // 预热已跑完
                r = g_emoWarmJobs[(size_t)wi];
            }
        }
        if (!ready) {
            std::lock_guard<std::mutex> lk(g_emoMx);
            g_emoPending.erase(r.key);
            continue;
        }
        std::wstring w = U8N(r.key.data(), (int)r.key.size());
        rt->BeginDraw();
        rt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
        rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        ID2D1SolidColorBrush* br = nullptr;
        if (SUCCEEDED(rt->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &br)) && br) {
            rt->DrawTextW(w.c_str(), (UINT32)w.size(), fmt,
                          D2D1::RectF(0.0f, 0.0f, (float)sz, (float)sz), br,
                          D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
            br->Release();
        }
        rt->EndDraw();
        std::memcpy(px.data(), bits, (size_t)stride * stride * 4);

        std::lock_guard<std::mutex> lk(g_emoMx);
        g_emoPending.erase(r.key);
        // 预热期间顺手把像素留在整块镜像里，全部完成后一次写盘
        if (r.slot >= 0 && !g_emoDisk.empty()) {
            size_t off = (size_t)r.slot * stride * stride;
            if (off + (size_t)stride * stride <= g_emoDisk.size())
                std::memcpy(g_emoDisk.data() + off, px.data(), (size_t)stride * stride * 4);
        }
        g_emoDone.push_back(EmoDone{r.slot, r.key, px});
    }

    if (fmt) fmt->Release();
    if (rt) rt->Release();
    if (dwf) dwf->Release();
    if (d2d) d2d->Release();
    if (bmp) DeleteObject(bmp);
    if (dc) DeleteDC(dc);
    CoUninitialize();
}

static void StopEmojiWorker() {
    {
        std::lock_guard<std::mutex> lk(g_emoMx);
        g_emoQuit = true;
    }
    g_emoCv.notify_all();
    for (auto& t : g_emoThr) if (t.joinable()) t.join();
    g_emoThr.clear();
}

static void ReleaseEmojiCache() {
    StopEmojiWorker();
    {
        std::lock_guard<std::mutex> lk(g_emoMx);
        g_emoReq.clear();
        g_emoPending.clear();
        g_emoDone.clear();
        g_emoWarmJobs.clear();
        g_emoDisk.clear();
        g_emoDisk.shrink_to_fit();
        g_emoQuit = false;
        g_emoSlot.clear();
        g_emoSlotKey.clear();
        g_emoSlotUsed.clear();
        g_emoReady.clear();
        g_emoFrame = 0;
    }
    g_emoWarmIdx.store(0);
    if (g_emoAtlas) { g_emoAtlas->Release(); g_emoAtlas = nullptr; }
    g_emoSz = 0; g_emoAtlasCols = 0; g_emoAtlasSlots = 0; g_emoNextSlot = 0;
    g_emoWarmPosted = false; g_emoWarmSaved = false; g_emoWarmSlots = 0; g_emoUpN = 0;
}

static bool EnsureEmojiCache(int sz) {
    if (g_emoAtlas && g_emoSz == sz) return true;
    ReleaseEmojiCache();
    if (!g_rt) return false;

    // 格子步长留 4px 透明边，线性采样不会串到邻格
    const int stride = sz + S(4);

    if (g_emoTotal <= 0)
        for (size_t i = 0; i < g_emoCats.size(); ++i) g_emoTotal += g_emoCats[i].count;

    // 容量按「全部 emoji 条数 + 8%」定：装得下就永不淘汰
    int need = g_emoTotal + g_emoTotal / 12 + 16;
    if (need < 256) need = 256;
    int aCols = 16;
    while (aCols * aCols < need) ++aCols;
    // 单张图集边长上限 4096（再大会拖慢整块拷贝与显存占用）
    int capCols = 4096 / (stride > 0 ? stride : 1);
    if (capCols < 16) capCols = 16;
    if (aCols > capCols) aCols = capCols;

    const int axis = aCols * stride;
    D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    if (FAILED(g_rt->CreateBitmap(D2D1::SizeU((UINT32)axis, (UINT32)axis), bp, &g_emoAtlas)) ||
        !g_emoAtlas) {
        g_emoAtlas = nullptr;
        return false;
    }
    g_emoSz = sz;
    g_emoAtlasCols = aCols;
    g_emoAtlasSlots = aCols * aCols;
    g_emoNextSlot = 0;
    g_emoFrame = 0;
    g_emoSlotKey.assign((size_t)g_emoAtlasSlots, std::string());
    g_emoSlotUsed.assign((size_t)g_emoAtlasSlots, 0);
    g_emoReady.assign((size_t)g_emoAtlasSlots, 0);

    std::lock_guard<std::mutex> lk(g_emoMx);
    g_emoQuit = false;
    // 单线程光栅化首屏 63 格要 300ms 左右，两个 worker 并行能压到一半
    for (int i = 0; i < 2; ++i) g_emoThr.emplace_back(EmojiWorkerMain, sz);
    return true;
}

// 分配一个图集槽：先用没占过的，用尽后淘汰「最久没出现在视口里」的那个。
// 只有图集被 cap 住、装不下全部字形时才会走到淘汰分支。
static int AllocEmojiSlot() {
    if (g_emoNextSlot < g_emoAtlasSlots) return g_emoNextSlot++;
    int best = -1, bestUsed = INT_MAX;
    for (size_t i = 0; i < g_emoSlotUsed.size(); ++i) {
        int used = g_emoSlotUsed[i];
        if (used < bestUsed) { bestUsed = used; best = (int)i; }
    }
    if (best < 0 || bestUsed >= g_emoFrame) return -1;   // 全部都在当前视口里
    g_emoSlot.erase(g_emoSlotKey[(size_t)best]);
    g_emoSlotKey[(size_t)best].clear();
    g_emoReady[(size_t)best] = 0;
    return best;
}

static D2D1_RECT_F EmojiSlotRect(int slot) {
    const int stride = EmojiStride();
    float x = (float)((slot % g_emoAtlasCols) * stride);
    float y = (float)((slot / g_emoAtlasCols) * stride);
    return D2D1::RectF(x, y, x + (float)g_emoSz, y + (float)g_emoSz);
}

// -------------------------------------------------------------- 磁盘缓存
static std::wstring EmojiCacheDir() {
    wchar_t buf[MAX_PATH] = L"";
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) GetTempPathW(MAX_PATH, buf);
    std::wstring d = std::wstring(buf) + L"\\WinVClip";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

static std::wstring EmojiCacheFile() {
    wchar_t nm[96];
    wsprintfW(nm, L"\\emoji-atlas-v2-%dpx-%d.bin", g_emoSz, g_emoAtlasCols);
    return EmojiCacheDir() + nm;
}

// 从磁盘恢复整块图集：一次 CopyFromMemory 就把所有字形装好，一个都不用光栅化
static bool LoadEmojiCache() {
    if (!g_emoAtlas) return false;
    FILE* fp = _wfopen(EmojiCacheFile().c_str(), L"rb");
    if (!fp) return false;

    bool ok = false;
    unsigned int hdr[8] = {0};
    do {
        if (fread(hdr, sizeof(hdr), 1, fp) != 1) break;
        if (hdr[0] != 0x564F4D45u /* 'EMOV' */ || hdr[1] != 2u) break;
        const int sz = (int)hdr[2], stride = (int)hdr[3], cols = (int)hdr[4];
        const int slots = (int)hdr[5], count = (int)hdr[6], axis = (int)hdr[7];
        if (sz != g_emoSz || stride != EmojiStride() || cols != g_emoAtlasCols ||
            slots != g_emoAtlasSlots || axis != EmojiSide()) break;
        if (count <= 0 || count > slots) break;

        std::vector<std::pair<int, std::string>> idx;
        idx.reserve((size_t)count);
        bool io = true;
        for (int i = 0; i < count; ++i) {
            unsigned int s = 0, l = 0;
            if (fread(&s, 4, 1, fp) != 1 || fread(&l, 4, 1, fp) != 1) { io = false; break; }
            if (l == 0 || l > 96 || (int)s >= slots) { io = false; break; }
            std::string k(l, '\\0');
            if (fread(&k[0], 1, l, fp) != l) { io = false; break; }
            idx.emplace_back((int)s, std::move(k));
        }
        if (!io) break;

        std::vector<uint32_t> px((size_t)slots * stride * stride, 0);
        if (fread(px.data(), 4, px.size(), fp) != px.size()) break;

        D2D1_RECT_U all{0, 0, (UINT32)axis, (UINT32)axis};
        HRESULT hr = g_emoAtlas->CopyFromMemory(&all, px.data(), (UINT32)(axis * 4));
        if (FAILED(hr)) {
            // 整块不走就退回逐格（个别驱动对超大 CopyFromMemory 有长度限制）
            for (auto& e : idx) {
                D2D1_RECT_U r;
                r.left = (UINT32)((e.first % cols) * stride);
                r.top = (UINT32)((e.first / cols) * stride);
                r.right = r.left + (UINT32)stride;
                r.bottom = r.top + (UINT32)stride;
                g_emoAtlas->CopyFromMemory(&r, px.data() + (size_t)e.first * stride * stride,
                                           (UINT32)(stride * 4));
            }
        }

        for (auto& e : idx) {
            g_emoSlot[e.second] = e.first;
            if ((size_t)e.first < g_emoSlotKey.size()) g_emoSlotKey[(size_t)e.first] = e.second;
            if ((size_t)e.first < g_emoReady.size()) g_emoReady[(size_t)e.first] = 1;
        }
        g_emoWarmPosted = true;      // 全部就绪，不再预热
        g_emoWarmSaved = true;       // 已有缓存，不必再写
        g_emoNextSlot = slots;       // 新字形才继续分配（正常不会发生）
        g_emoUpN = count;
        ok = true;
    } while (false);
    fclose(fp);
    return ok;
}

// 把整块图集写盘。数据源是预热期间的镜像 g_emoDisk，写完即释放。
static void SaveEmojiCache() {
    if (g_emoWarmSaved || g_emoDisk.empty() || !g_emoAtlas) return;
    g_emoWarmSaved = true;

    const int stride = EmojiStride();
    const int axis = EmojiSide();
    int count = 0;
    for (auto& k : g_emoSlotKey) if (!k.empty()) ++count;
    if (count <= 0) return;

    FILE* fp = _wfopen(EmojiCacheFile().c_str(), L"wb");
    if (!fp) return;
    unsigned int hdr[8] = {0x564F4D45u, 2u, (unsigned)g_emoSz, (unsigned)stride,
                           (unsigned)g_emoAtlasCols, (unsigned)g_emoAtlasSlots,
                           (unsigned)count, (unsigned)axis};
    fwrite(hdr, sizeof(hdr), 1, fp);
    for (size_t i = 0; i < g_emoSlotKey.size(); ++i) {
        if (g_emoSlotKey[i].empty()) continue;
        unsigned int s = (unsigned)i, l = (unsigned)g_emoSlotKey[i].size();
        fwrite(&s, 4, 1, fp);
        fwrite(&l, 4, 1, fp);
        fwrite(g_emoSlotKey[i].data(), 1, l, fp);
    }
    fwrite(g_emoDisk.data(), 4, g_emoDisk.size(), fp);
    fclose(fp);

    g_emoDisk.clear();
    g_emoDisk.shrink_to_fit();
}

// 启动预热：读缓存命中就整块恢复，否则把全部 emoji 排进低优先队列
static void StartEmojiWarm() {
    if (g_emoWarmPosted) return;
    if (!EnsureEmojiCache(S(44))) return;
    g_emoWarmPosted = true;
    if (LoadEmojiCache()) return;

    std::vector<std::string> keys;
    keys.reserve((size_t)(g_emoTotal > 0 ? g_emoTotal : 2048));
    for (int i = 1; i <= GlyphCatTotal(PG_EMOJI); ++i) {
        GCat& c = GlyphCat(PG_EMOJI, i);
        LoadCat(c);
        for (auto& it : c.items) keys.emplace_back(it.g, (size_t)it.gl);
    }
    if (keys.empty()) { g_emoWarmPosted = false; return; }

    const int stride = EmojiStride();
    // 整块镜像：预热期间每一格都留一份，全部完成后一次写盘
    g_emoDisk.assign((size_t)g_emoAtlasSlots * stride * stride, 0);

    std::vector<EmoReq> jobs;
    jobs.reserve(keys.size());
    for (auto& k : keys) {
        if (g_emoSlot.find(k) != g_emoSlot.end()) continue;   // 视口已经抢先渲染过
        int slot = AllocEmojiSlot();
        if (slot < 0) break;                                  // 图集被 cap 住且已满
        g_emoSlot[k] = slot;
        g_emoSlotKey[(size_t)slot] = k;
        jobs.push_back(EmoReq{k, slot});
    }
    if (jobs.empty()) { g_emoDisk.clear(); g_emoDisk.shrink_to_fit(); return; }

    {
        std::lock_guard<std::mutex> lk(g_emoMx);
        g_emoWarmSlots = (int)jobs.size();
        g_emoWarmJobs = std::move(jobs);
        g_emoWarmIdx.store(0);
    }
    g_emoCv.notify_all();
}

// UI 线程每帧调一次：上传已完成的像素 + 投递缺失格。绝不做光栅化。
static void PrepareEmojiBitmaps() {
    if (!g_rt || !g_emojiFmt) return;
    std::vector<std::pair<RECT, int>> cells;
    CollectEmojiCells(cells);
    if (cells.empty()) return;
    const std::vector<GItem>& list = GlyphItems(PG_EMOJI);
    if (!EnsureEmojiCache(S(44))) return;

    // 0) 首次进来：先看磁盘缓存（命中的话这一帧就是全彩的）
    if (!g_emoCacheTried) {
        g_emoCacheTried = true;
        StartEmojiWarm();
    }

    // 1) 把工作线程做完的像素写进 atlas（一帧最多 160 格，别让拷贝把单帧拖长）
    const int stride = EmojiStride();
    const int cols = g_emoAtlasCols;
    std::vector<EmoDone> done;
    {
        std::lock_guard<std::mutex> lk(g_emoMx);
        done.swap(g_emoDone);
    }
    int up = 0;
    size_t used = 0;
    for (; used < done.size() && up < 160; ++used) {
        EmoDone& d = done[used];
        if (d.slot < 0 || d.slot >= g_emoAtlasSlots ||
            (int)d.px.size() != stride * stride) { ++g_emoFailN; continue; }
        D2D1_RECT_U r;
        r.left = (UINT32)((d.slot % cols) * stride);
        r.top = (UINT32)((d.slot / cols) * stride);
        r.right = r.left + (UINT32)stride;
        r.bottom = r.top + (UINT32)stride;
        if (FAILED(g_emoAtlas->CopyFromMemory(&r, d.px.data(), (UINT32)(stride * 4)))) {
            ++g_emoFailN;
            continue;
        }
        g_emoReady[(size_t)d.slot] = 1;
        g_emoSlotUsed[(size_t)d.slot] = g_emoFrame;
        ++g_emoNewN;
        ++g_emoUpN;
        ++up;
    }
    if (used < done.size()) {          // 没消化完的留到下一帧
        std::lock_guard<std::mutex> lk(g_emoMx);
        done.erase(done.begin(), done.begin() + (ptrdiff_t)used);
        g_emoDone.insert(g_emoDone.end(),
                         std::make_move_iterator(done.begin()),
                         std::make_move_iterator(done.end()));
    }

    // 2) 刷新可见槽位的「最近使用」（淘汰兜底用）
    ++g_emoFrame;
    for (auto& c : cells) {
        const GItem& it = list[(size_t)c.second];
        std::string key(it.g, (size_t)it.gl);
        auto f = g_emoSlot.find(key);
        if (f != g_emoSlot.end() && (size_t)f->second < g_emoSlotUsed.size())
            g_emoSlotUsed[(size_t)f->second] = g_emoFrame;
    }

    // 3) 投递缺失格。队列是后进先出，所以「先投预取、后投视口内」，
    //    这样视口内的格子总排在前面先被渲染。
    bool posted = false;
    {
        std::lock_guard<std::mutex> lk(g_emoMx);
        auto post = [&](int idx) {
            if (idx < 0 || idx >= (int)list.size()) return;
            const GItem& it = list[(size_t)idx];
            std::string key(it.g, (size_t)it.gl);
            auto f = g_emoSlot.find(key);
            if (f != g_emoSlot.end()) {
                int s0 = f->second;
                if ((size_t)s0 < g_emoReady.size() && g_emoReady[(size_t)s0]) return;
            }
            if (g_emoPending.find(key) != g_emoPending.end()) return;
            int slot = AllocEmojiSlot();
            if (slot < 0) return;
            g_emoSlot[key] = slot;
            if ((size_t)slot < g_emoSlotKey.size()) g_emoSlotKey[(size_t)slot] = key;
            g_emoPending.insert(key);
            g_emoReq.push_back(EmoReq{std::move(key), slot});
            posted = true;
        };
        // 预取：视口上下各一行，滚动时下一屏通常已经就绪
        const int ec = EmojiCols();
        if (!cells.empty()) {
            int lo = cells.front().second - ec;
            int hi = cells.back().second + ec;
            for (int i = lo; i <= hi; ++i)
                if (i < cells.front().second || i > cells.back().second) post(i);
        }
        for (auto& c : cells) post(c.second);
    }
    if (posted) g_emoCv.notify_one();

    // 4) 统计可见格命中数
    {
        std::lock_guard<std::mutex> lk(g_emoMx);
        for (auto& c : cells) {
            const GItem& it = list[(size_t)c.second];
            std::string key(it.g, (size_t)it.gl);
            auto f = g_emoSlot.find(key);
            if (f != g_emoSlot.end() && (size_t)f->second < g_emoReady.size() &&
                g_emoReady[(size_t)f->second]) ++g_emoHitN;
        }
    }

    // 5) 全量预热跑完就把图集落盘，下次启动直接整块载入
    if (g_emoWarmSlots > 0 && !g_emoWarmSaved && g_emoUpN >= g_emoWarmSlots) SaveEmojiCache();

    // 6) 还有格子在路上（或预热未完成）：排一个 8ms 的重绘，补齐后自然停下
    bool waiting;
    {
        std::lock_guard<std::mutex> lk(g_emoMx);
        waiting = !g_emoPending.empty() || !g_emoDone.empty() ||
                  g_emoWarmIdx.load() < (int)g_emoWarmJobs.size();
    }
    if (waiting && g_panel) SetTimer(g_panel, TIMER_EMOJI, 8, nullptr);
}

"""

A = "// ---------------------------------------------------------------- 彩色字形（后台光栅化）"
B = "// ================================================================== 图片缩略图"
i = src.find(A)
j = src.find(B)
assert i > 0 and j > i, (i, j)
src = src[:i] + NEW + src[j:]

# 新增 <atomic> / <chrono>
old_inc = "#include <thread>\n#include <mutex>"
assert src.count(old_inc) == 1
src = src.replace(old_inc, "#include <thread>\n#include <atomic>\n#include <chrono>\n#include <mutex>", 1)

# ---- PaintEmoji：未就绪改成极淡剪影（原来是实心黑字形，跳变刺眼）
OLD_PAINT = """    const int sz = S(44);
    if (g_rt && g_painting && g_emojiFmt) {
        Brush(C_INK);
        for (auto& c : cells) {
            const GItem& it = list[(size_t)c.second];
            auto f = g_emoSlot.find(std::string(it.g, (size_t)it.gl));
            if (f != g_emoSlot.end() && g_emoAtlas) {"""
NEW_PAINT = """    const int sz = S(44);
    if (g_rt && g_painting && g_emojiFmt) {
        for (auto& c : cells) {
            const GItem& it = list[(size_t)c.second];
            auto f = g_emoSlot.find(std::string(it.g, (size_t)it.gl));
            if (f != g_emoSlot.end() && g_emoAtlas &&
                (size_t)f->second < g_emoReady.size() && g_emoReady[(size_t)f->second]) {"""
assert src.count(OLD_PAINT) == 1
src = src.replace(OLD_PAINT, NEW_PAINT, 1)

OLD_FB = """            } else {
                // 后台还没光栅化好：先用单色字形顶一两帧（彩色字形在此处画会卡）
                std::wstring w = U8N(it.g, it.gl);"""
NEW_FB = """            } else {
                // 还没就绪：只给一层极淡的剪影。以前这里画的是实心黑字形，
                // 于是「黑色->彩色」的跳变非常刺眼；淡影过渡基本看不出来。
                Brush(C_MUTED, 0.22f);
                std::wstring w = U8N(it.g, it.gl);"""
assert src.count(OLD_FB) == 1
src = src.replace(OLD_FB, NEW_FB, 1)

# ---- 新增 TIMER_WARM
OLD_T = "TIMER_REPIN = 0x105, TIMER_FOCUS = 0x106, TIMER_EMOJI = 0x107 };"
NEW_T = "TIMER_REPIN = 0x105, TIMER_FOCUS = 0x106, TIMER_EMOJI = 0x107,\n       TIMER_WARM = 0x108 };"
assert src.count(OLD_T) == 1
src = src.replace(OLD_T, NEW_T, 1)

# ---- 启动后自动预热（不用等第一次打开面板）
OLD_TRAY = "    AddTray();\n    PostMessageW(g_main, WM_CLIPBOARDUPDATE, 0, 0);"
NEW_TRAY = ("    AddTray();\n"
            "    SetTimer(g_main, TIMER_WARM, 120, nullptr);   // 启动后就把字形图集准备好\n"
            "    PostMessageW(g_main, WM_CLIPBOARDUPDATE, 0, 0);")
assert src.count(OLD_TRAY) == 1
src = src.replace(OLD_TRAY, NEW_TRAY, 1)

# ---- 主窗口定时器里处理 TIMER_WARM（MainProc 的 WM_TIMER，8 空格缩进）
OLD_WM = """    case WM_TIMER:
        if (wp == TIMER_CLIP) {"""
NEW_WM = """    case WM_TIMER:
        if (wp == TIMER_WARM) {
            KillTimer(hwnd, TIMER_WARM);
            g_emoCacheTried = true;
            StartEmojiWarm();
        } else if (wp == TIMER_CLIP) {"""
assert src.count(OLD_WM) == 1, src.count(OLD_WM)
src = src.replace(OLD_WM, NEW_WM, 1)

out = src.replace("\n", "\r\n") if CRLF else src
io.open(P, "w", encoding="utf-8", newline="").write(out)
print("patched ok, %d chars" % len(out))
