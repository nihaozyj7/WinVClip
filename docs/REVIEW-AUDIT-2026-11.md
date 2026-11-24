# 两份审查报告的复核与建议

复核对象：`docs/STABILITY-REVIEW-VERIFIED.md`、`docs/PERFORMANCE-REVIEW.md`
复核方式：不采信报告里的因果链，逐条回到 `src/` 读实现；再用本机现有实测数据
（`%TEMP%\winvclip_perf.log`，23 KB）与构建产物交叉印证。
结论口径只有三种：**属实**、**属实但要改写**、**不成立**。

---

## 一、复核结论

**两份报告的代码事实全部属实，没有一条是编造的。** 共复核 17 处关键论点，
14 处完全站得住，3 处需要修正（都不影响「该不该修」的判断，只影响「怎么修」）。
可以直接照 `STABILITY-REVIEW-VERIFIED.md` 的第一、二批执行。

但有 4 个**报告没覆盖**的问题，其中 2 个和已列问题同源、应当合并成一次改动（见第三节）。

### 交叉印证（用现有数据说话）

| 报告论断 | 本机数据 | 结论 |
|---|---|---|
| P0-2 缩略图长尾 ~50 ms | perf 日志 `page=3 ... end=1.97 max=18.83`、`page=2 ... max=12.92` | 属实：单帧尖峰真实存在，且瓶颈段确为 `end` |
| P1-6 整块上传白做一次 | 日志每行 `load: cells=1911 bulk-hr=0x0` | 属实：注释说「内容错位」，但返回值是 S_OK，19.5 MB 那次拷贝照付 |
| N1 落盘索引与预热任务数不等 | 日志同时出现 `load: cells=1923` 与 `warm: posted jobs=1911` | **属实且已发生**：1923 = `g_emoSlotKey` 非空项（含视口抢跑的 12 格），1911 = 预热任务数 |
| P2-6 `-O2` 覆盖 `-O3` | `build\build.ninja` 实为 `-O3 -DNDEBUG ... -O2 -municode` | 属实：生效的是 `-O2` |

---

## 二、要修正的三处

### 2.1 S2-1「8ms 忙循环不成立」—— 对 `!ready` 分支判断正确，但漏了主路径

`emoji_atlas.cpp:154-158` 的分析没问题（`!ready` 时确实只 `erase` 就 `continue`）。
但**预热正常进行的路径**上：

- `PrepareEmojiBitmaps` 末尾 `if (more && g_panel) SetTimer(g_panel, TIMER_EMOJI, 8, nullptr)`
  （`emoji_atlas.cpp:638`）；
- `panel.cpp:475-480` 收到 `TIMER_EMOJI` → `KillTimer` + `PumpEmojiUploads` → 若 `more` 再排 8 ms。

于是**只要是预热期间，UI 线程就以 125 Hz 重绘整帧**（1.9 ms/帧 ⇒ 约 24% 的单核占用），
与「这一帧有没有新格子上传」无关。全量预热 1911 格、每帧最多上传 160 格
（`emoji_atlas.cpp:496`）、worker 约 8 ms 出 3~10 格，所以**首次进表情页会有约 3 秒的
125 Hz 满速重绘**。这恰好发生在用户正在滚动的时候，而滚动本身也在重绘 —— 白白叠加。

修法（5 行，零风险）：`PumpEmojiUploads` 返回 `more` 之外再返回「本帧是否真的上传了新格」，
`TIMER_EMOJI` 只在「有格子落地」或「`more` 刚变 false」时 `Redraw()`，其余仍是 8 ms 轮询但不重绘。
或者更简单：把 8 ms 改成「有 `g_emoDone` 才排 8 ms，否则排 33 ms」。

### 2.2 P1-2 的缓存污染源定位不准

`text_measure.cpp:41-47` 的「满 600 整表清空」属实。但报告说污染源是「行内大小-时间串」
（`paint.cpp:284`）—— 不准确：`RelTime` 精度到分钟，同一分钟内 key 是稳定的，
500 条历史最多产生 500 个稳定 key，不会反复顶穿。

真正的污染源是 `EllipText`（`paint.cpp:648-655`）：

```cpp
while (!out.empty() && TextW(f, out + L"…") > maxW) out.pop_back();
```

**每一次循环都往全局 `TextW` 缓存里插一条中间串。** 一个 60 字符的临时目录路径
（设置窗口「临时文件目录」那一行，调用点 `paint.cpp:691`）截断一次就插几十条、
几乎全是永不命中的垃圾 key。设置窗口活着的期间，几次重绘就能把 600 额度顶穿，
连带把页签/chip 的固定文案全部清掉重新 `CreateTextLayout`。

改法（不改结构）：`EllipText` 里不要用全局 `TextW`，改用二分查找 + 局部缓存的
`TextWUncached`，循环次数从 O(len) 降到 O(log len)，且不污染全局表。

### 2.3 P0-2 的改造范围要划清

报告建议「`DibToRgba` 顺带改名 `DibToThumb`」——**这会弄坏「另存为图片」**：
`DibToRgba` 有两个调用方，`thumb.cpp:41`（要缩略图）和 `actions.cpp:269`（要原尺寸）。

`PngToRgba` 只被 `thumb.cpp:41` 调用（全仓确认），所以 **PNG 那条可以随便改成
WIC 内部缩放，是封闭改动**；`DibToRgba` 必须**新增** `DibToThumb(dib, box, out)`，
原函数保留给另存为。

---

## 三、报告漏掉的、我核实过的问题

### N-A 预热期间 125 Hz 全帧重绘 —— P1
见 2.1。这是性能报告里唯一一处「机制说反了」的地方，实际开销比报告的
「没有忙循环」要大。

### N-B `BEGIN IMMEDIATE` 失败会让删除单独提交 —— 属 S1，和 A1 一起修

A1 说「COMMIT 结果被丢弃」，但 `Exec`（`history_store.cpp:63-65`）丢弃的是**每一条**
语句的返回值，包括 `Exec("BEGIN IMMEDIATE")`（`:594`）。若 BEGIN 失败（库被占用/只读），
后续语句在 SQLite 的自动提交模式下逐条执行：

1. `DeleteMissingItems()`（`:595`）**成功并提交**；
2. `InsertItem`（`:597`）失败被忽略；
3. 结果：库里删掉了内存中不存在的行，新增的条目一条没进去 —— 中间态比「整事务失败」更脏。

所以修 A1 时不能只判 `COMMIT`，要**先判 `BEGIN`，失败就 `return`，不做任何删除**。
这是本次复核里唯一一条「比报告描述的更严重」的。

### N-C `Lower` 的分配体积被低估 —— 属 P1-1 的补充

`MatchQuery`（`clip_model.cpp:20-26`）四次 `Lower(...)`，而 `Lower` 是
`std::wstring Lower(std::wstring s)`（`text_util.cpp:7-10`）—— **按值收参**：
拷贝一次、原地改写、返回再一次拷贝/移动。

报告只算了「2000 次堆分配/键」，但没算体积。历史里若有 20 条 16 KB 正文，
每个键每次 `BuildView` 就是 **20 × 32 KB ≈ 640 KB** 的分配/释放，而且其中
三次（title/source/tag）是纯浪费的短串。改成
`bool ContainsCI(std::wstring_view hay, std::wstring_view needle)`
后次数和体积一起归零，这是本次最高性价比的性能改动（打字路径，随输入实时发生）。

### N-D 落盘索引与预热任务数的偏差已经在日志里了
`load: cells=1923` 对 `warm: posted jobs=1911`。差 12 是视口请求抢跑的格子 ——
正是 N1 所述「混合计数导致提前落盘」的直接证据。修 N1 后应当看到两者相等。

---

## 四、建议（按性价比排序，含具体改法）

### 第 1 批：一次改完，都是「失败之后没有出口」这一类（半天）

| # | 位置 | 改法 | 判据 |
|---|---|---|---|
| 1 | `history_store.cpp:63-65` | `Exec` 改返回 `bool`；判 `BEGIN`（`:594`）失败 → `return`；`InsertItem`/`UpdateItem` 返回值向上传递；`COMMIT` 失败 → `ROLLBACK` + 保留正文 + `Trace` 一行 | `attrib +R history.db` 或外部 `BEGIN EXCLUSIVE` 锁库 → 复制一条 >16 KB 文本 → 粘贴仍能取到全文，日志有一行失败记录 |
| 2 | `emoji_atlas.cpp:73-84 / 173-179 / 435` | `g_emoDisk` 的指针读写、`AllocEmoDisk`、`FreeEmoDisk` 全部纳入 `g_emoMx`；`SaveEmojiCache` 的落盘条件加「`g_emoWarmIdx >= jobs.size()` 且预热格全 `ready`」 | 删掉 `emoji-atlas-v2-*.bin` 重启，进表情页滚动 1 分钟，无空白格；缓存重载后 `cells` 数 == `posted jobs` 数 |
| 3 | `main.cpp`（无 `WM_QUERYENDSESSION`） | 加 `WM_QUERYENDSESSION`/`WM_ENDSESSION` → `KillTimer(TIMER_PERSIST)` + 立即 `HistoryFlush()` | 复制一条后 1 秒内注销，重启后该条仍在 |
| 4 | `history_store.cpp:146-164 → :586` | 打开失败 `Trace` + 一次性 Toast（复用现成的 `g_toast`），别静默 `return` | 故意把 `history` 目录设成只读，能看到提示 |
| 5 | `main.cpp:118-123` | `CaptureClipboard` 改返回 `bool changed`，未变化不 `BuildView`/`Redraw` | 同一个内容复制两次，perf 日志帧数不增加 |
| 6 | N-A：`emoji_atlas.cpp:638` / `panel.cpp:475-480` | 只在「有新格子上传」或 `more` 转 false 时 `Redraw()`；否则 8 ms 只 `Pump` 不重绘 | 首次预热期间的帧数下降一个量级 |

### 第 2 批：静音的性能收益（一天）

| # | 位置 | 改法 | 预期 |
|---|---|---|---|
| 7 | `thumb.cpp:41` + `raster.cpp:33` | PNG 走 `IWICBitmapScaler`（`Fant` 插值）直出 `box` 尺寸；`DibToRgba` **新增** `DibToThumb`，保留原函数给 `actions.cpp:269` | 收藏页 `max` 从 50 ms 级回到 16 ms 以内；`PngToRgba` 的 64 M 像素上限一并缩小 |
| 8 | `emoji_atlas.cpp:163-169` | `CreateSolidColorBrush` 移到 `for(;;)` 之前建一次（颜色恒黑） | 冷启动省 1911 次 COM 分配 |
| 9 | `emoji_atlas.cpp:375-390` | 删掉整块 `CopyFromMemory` 那次调用（保留 `cells=` 日志） | 启动进表情页省一次 19.5 MB 上传 |
| 10 | `paint.cpp:927` | `BitBlt` 只拷 `dirty` 区域（`paint.cpp:852` 已经算好了） | `blit` 段砍 30~40% |
| 11 | `history_store.cpp:350-370` | `keep` 改 `std::unordered_set<std::wstring>` | 启动少几十万次 `wstring` 比较 |
| 12 | `paint.cpp:520-521` | `BuildView` 末尾算好 `g_pinnedCount`/`g_favCount` | 页脚不再每帧 O(N) |
| 13 | `clip_model.cpp:20-26` + `text_util` | 新增 `ContainsCI(std::wstring_view, std::wstring_view)`，`MatchQuery` 全换掉（含 N-C 的 640 KB 分配） | 连打搜索不掉帧 |
| 14 | `paint.cpp:648-655` | `EllipText` 改二分 + 局部缓存，不写全局 `TextW` | 设置窗口不再清空全局缓存（见 2.2） |

### 第 3 批：顺手做，别单独排期

- `paint.cpp:857-863` 缩略图预建已在 `PaintPanel` 里对**所有可见行**做，可以只对
  「脏区内实际要画的行」做（滚动时下游行不进脏区）。
- `paint.cpp:436` 符号页每格 `U8N` + `PickSymFont` 哈希 —— 在 `LoadCat` 时预算成
  平行数组（`P1-8`），`body` 段 0.47 ms 是四页最高，值得做。
- `dc.cpp:53` `EndDraw` 返回值至少 `Trace` 一次（`D2DERR_RECREATE_TARGET` 时重建设备）。
- `main.cpp:273` `Local\` 前缀（等价改写，非 bug）；`WM_DPICHANGED`；`GlobalLock` 判空
  （纯卫生，`GlobalAlloc` 成功后不会失败）。

### 明确不建议做

与两份报告一致，并且我复核后更确认：

- **`P0-1` 全量重写优化**：实测 500 条 1.15 ms，用户不可感知。
- **`P0-3` 进程快照缓存**、**`P0-4` 去重前置筛**：省 1~8 ms，占比 <5%。只有把它们当成
  「唤出路径不该在钩子栈上跑」的那条改（A3）时才顺带受益。
- **`S3-3` 另存图片改引用**：会在模态期间引入 UAF，**必须保持现状**。
- **`S3-6` 互斥量加 `Global\`**：误报，无前缀即会话命名空间。
- **`HistoryFlush` 加重试线程 / emoji 光栅化改多 worker / 每帧停起 D2D 设备**：
  三条都与 `MEMORY.md` 记过的实测冲突，不要推翻。

### 关于 `-O3`

`build\build.ninja` 实测为 `-O3 -DNDEBUG ... -O2 -municode`，生效的是 `-O2`。
想试 `-O3`/`-flto` 必须先删掉 `CMakeLists.txt` 末尾裸的 `-O2`，或改成
`$<$<CONFIG:Release>:-O3>`；否则改了也不生效，白测一轮。按本机帧耗时
（`EndDraw` 占 0.9/1.9）推断上限约 10%，不要期待更多。

---

## 五、如果只有两小时

按这个顺序，三项都不需要读别的代码：

1. **`Exec` 判返回值 + 判 `BEGIN`**（第 1 批第 1 项）—— 唯一会静默丢用户数据的地方，
   而且 N-B 表明它还会留下「删了旧的、没写新的」的中间态。
2. **`WM_QUERYENDSESSION` 落盘**（第 1 批第 3 项）—— 两行，消掉「复制完立刻关机丢最后一条」。
3. **`emoji_atlas.cpp:163-169` 的 brush 移出循环**（第 2 批第 8 项）—— 10 行以内，
   掐掉 53 ms 峰值帧里的一个固定成本。

第 1 项验证时不要用 `attrib +R`（只读文件会让 `sqlite3_open_v2` 直接失败，
走 `OpenDb` 的早退路径，反而测不到 A1）——用外部连接持 `BEGIN EXCLUSIVE` 锁库，
程序侧才会走到 `BEGIN IMMEDIATE` 失败那条链。
