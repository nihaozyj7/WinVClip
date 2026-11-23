# -*- coding: utf-8 -*-
# 把「上传+落盘」从 PrepareEmojiBitmaps 里抽成 PumpEmojiUploads，
# 这样预热可以在面板没显示、甚至不在表情页时继续推进。
import io

P = "src/main.cpp"
raw = io.open(P, "r", encoding="utf-8", newline="").read()
CRLF = "\r\n" in raw
src = raw.replace("\r\n", "\n")

OLD = """    // 1) 把工作线程做完的像素写进 atlas（一帧最多 160 格，别让拷贝把单帧拖长）
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

    // 2) 刷新可见槽位的「最近使用」（淘汰兜底用）"""

NEW = """    // 1) 先把工作线程做好的像素搬进 atlas（与当前页面无关，预热期间也要推进）
    bool more = PumpEmojiUploads();

    // 2) 刷新可见槽位的「最近使用」（淘汰兜底用）"""

assert src.count(OLD) == 1
src = src.replace(OLD, NEW, 1)

OLD2 = """    // 5) 全量预热跑完就把图集落盘，下次启动直接整块载入
    if (g_emoWarmSlots > 0 && !g_emoWarmSaved && g_emoUpN >= g_emoWarmSlots) SaveEmojiCache();

    // 6) 还有格子在路上（或预热未完成）：排一个 8ms 的重绘，补齐后自然停下
    bool waiting;
    {
        std::lock_guard<std::mutex> lk(g_emoMx);
        waiting = !g_emoPending.empty() || !g_emoDone.empty() ||
                  g_emoWarmIdx.load() < (int)g_emoWarmJobs.size();
    }
    if (waiting && g_panel) SetTimer(g_panel, TIMER_EMOJI, 8, nullptr);
}"""

NEW2 = """    // 5) 还有格子在路上（或预热未完成）：排一个 8ms 的重绘，补齐后自然停下
    if (more && g_panel) SetTimer(g_panel, TIMER_EMOJI, 8, nullptr);
}"""

assert src.count(OLD2) == 1
src = src.replace(OLD2, NEW2, 1)

# 在 PrepareEmojiBitmaps 之前插入 PumpEmojiUploads
ANCHOR = """// UI 线程每帧调一次：上传已完成的像素 + 投递缺失格。绝不做光栅化。
static void PrepareEmojiBitmaps() {"""

PUMP = """// 把工作线程做好的像素搬进 atlas，并在全量预热跑完后落盘。
// 与当前页面无关：预热期间面板可能没显示、也可能停在历史页，都得继续推进。
// 返回是否还有未完成的活（调用方据此续排定时器）。
static bool PumpEmojiUploads() {
    if (!g_emoAtlas) return false;
    const int stride = EmojiStride();
    const int cols = g_emoAtlasCols;

    std::vector<EmoDone> done;
    {
        std::lock_guard<std::mutex> lk(g_emoMx);
        done.swap(g_emoDone);
    }
    // 一帧最多 160 格，别让拷贝把单帧拖长（160 格约 1.5ms）
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
        if ((size_t)d.slot < g_emoSlotUsed.size()) g_emoSlotUsed[(size_t)d.slot] = g_emoFrame;
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

    // 全部预热格都进 atlas 了：整块落盘，下次启动直接载入
    if (g_emoWarmSlots > 0 && !g_emoWarmSaved && g_emoUpN >= g_emoWarmSlots) SaveEmojiCache();

    std::lock_guard<std::mutex> lk(g_emoMx);
    return !g_emoPending.empty() || !g_emoDone.empty() ||
           g_emoWarmIdx.load() < (int)g_emoWarmJobs.size();
}

// UI 线程每帧调一次：上传已完成的像素 + 投递缺失格。绝不做光栅化。
static void PrepareEmojiBitmaps() {"""

assert src.count(ANCHOR) == 1
src = src.replace(ANCHOR, PUMP, 1)

# TIMER_EMOJI：先推进上传，再决定是否重画/续期
OLD_TM = """        } else if (wp == TIMER_EMOJI) {
            KillTimer(hwnd, TIMER_EMOJI);
            Redraw(&L.body);"""
NEW_TM = """        } else if (wp == TIMER_EMOJI) {
            KillTimer(hwnd, TIMER_EMOJI);
            // 预热/补齐都要推进，哪怕面板此刻不在表情页、甚至不可见
            bool more = PumpEmojiUploads();
            if (g_page == PG_EMOJI) Redraw(&L.body);
            if (more) SetTimer(hwnd, TIMER_EMOJI, 8, nullptr);"""
assert src.count(OLD_TM) == 1
src = src.replace(OLD_TM, NEW_TM, 1)

# TIMER_WARM 里启动预热后要驱动上传
OLD_WARM = """        if (wp == TIMER_WARM) {
            KillTimer(hwnd, TIMER_WARM);
            g_emoCacheTried = true;
            StartEmojiWarm();
        } else if (wp == TIMER_CLIP) {"""
NEW_WARM = """        if (wp == TIMER_WARM) {
            KillTimer(hwnd, TIMER_WARM);
            g_emoCacheTried = true;
            StartEmojiWarm();
            if (g_panel) SetTimer(g_panel, TIMER_EMOJI, 8, nullptr);
        } else if (wp == TIMER_CLIP) {"""
assert src.count(OLD_WARM) == 1
src = src.replace(OLD_WARM, NEW_WARM, 1)

out = src.replace("\n", "\r\n") if CRLF else src
io.open(P, "w", encoding="utf-8", newline="").write(out)
print("patched ok")
