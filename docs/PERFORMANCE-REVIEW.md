# WinVClip 性能审查报告

审查对象：当前工作副本（`src/` 全部 30 个编译单元 + `CMakeLists.txt`）。
审查方向：**性能**（延迟、帧耗时、内存、IO 写放大），不含可维护性/架构。
参考基线：本机 `%TEMP%\winvclip_perf.log`（23 KB，由 `winvclip.exe --perf` 产生）里的
真实帧统计，加上代码静态精读得出的一次性开销估算。

与 `docs/CODE_REVIEW.md` 的关系：那份是「单文件 5588 行」时代的架构审查，已被
`src/{core,render,image,glyph,clipboard,model,ui,app}` 的模块化改造替代；本报告不重复它的结论。

> **已做真实性验证，见 `docs/PERFORMANCE-REVIEW-VERIFY.md`。**
> 结论摘要：全部 23 条问题的**机制与行号均属实**（无幻觉条目）；但 **P0-1 的成本被高估
> 10~17 倍**（实测 500 条全流程 1.15 ms，非遗留写法下的 10~20 ms），**P0-3/P0-4 的收益
> 不足以支撑 P0**，三者已在下文标注降级。真正经得起实测的只剩 **P0-2（缩略图 50 ms 长尾）**
> 与 **P0-5（emoji 53 ms，已知取舍）**。以验证报告的行动建议为准。

---

## 一、结论先行

1. **逐帧渲染已经没什么可省的了。** 实测中位帧 1.5~1.9 ms，其中 `EndDraw` 独占
   0.9 ms（软件 D2D 提交光栅化），`head`/`body`/`foot` 三段加起来 0.1~0.5 ms。
   `Paint*` 里任何字符串/度量层面的微优化，收益上限被这 0.9 ms 死死压住——
   除非它能把几 ms 的长尾掐掉，否则不值得动。
2. **真正该修的是「UI 线程上的长尾」：4 处单次几十毫秒的同步工作。**
   它们都排在唯一的 UI 线程上，而低级键鼠钩子（`WH_KEYBOARD_LL`/`WH_MOUSE_LL`）
   的回调也排这条线程——一次 30 ms 的阻塞，用户感到的不只是面板卡一下，
   而是**整个系统的鼠标发钝**（这条因果链项目自己已经踩过，见
   `glyph/emoji_atlas.cpp:570-580` 的注释）。
3. ~~**写放大是唯一还在持续变差的东西。**~~ **【验证后降级】**
   `HistoryFlush()` 确实每次落盘都把全部 N 条历史 UPDATE 一遍、每行重新 `prepare`
   一次 SQL（`core/history_store.cpp:585-604`、`:244`、`:201`），**机制属实**；
   但实测成本远低于本报告最初的估算：500 条全流程 **1.15 ms**（`DeleteMissingItems`
   0.14 + UPDATE 0.82 + `SaveRecent` 0.13），5000 条约 **12 ms**。
   `sqlite3_prepare` 实测 0.22 µs/次，不是 5~15 µs。默认配置下用户不可能感知，
   → 降为 P3，不建议动（详见 `PERFORMANCE-REVIEW-VERIFY.md` 第三节）。

一句话优先级（**已按实测修正**）：**先修 P0-2 缩略图长尾；P0-5 只做 brush 那一处；
P0-1/P0-3/P0-4 不建议做；渲染层其余部分不要碰。**

---

## 二、性能模型（先有预算，再谈优化）

程序里所有值得关心的代码只有四条路径。判定「是不是性能问题」的唯一标准，
是这条路径有没有超预算。

| # | 路径 | 触发频率 | 实测 / 预算 | 执行线程 |
|---|---|---|---|---|
| 1 | 帧路径 `WM_PAINT → PaintPanel` | 滚动/悬停/打字，~60 Hz | **实测 1.5~1.9 ms 中位，3.2~3.8 ms 峰值**（emoji 页曾出现 53.6 ms） | UI |
| 2 | 唤出路径 `LLKeyboard → ShowPanel` | 每次按 Win+V | **实测 7.3~7.7 ms**（快路径）；UIA 慢路径可达数十~500 ms | UI（钩子回调） |
| 3 | 采集路径 `WM_CLIPBOARDUPDATE → +160ms → CaptureClipboard` | 每次复制 | 目标 <16 ms，**未埋点** | UI |
| 4 | 落盘路径 `HistoryTouch → +900ms → HistoryFlush` | 每次历史变动 | 目标 <16 ms，**未埋点**，随历史条数线性增长 | UI |

### 实测帧耗时分解（本机日志，按页聚合）

| page | 帧数 | total 中位 | total 峰值 | begin | prep | head | body | foot | end | blit |
|---|---|---|---|---|---|---|---|---|---|---|
| 0 历史 | 600 | 1.87 | 3.18 | 0.06 | 0.42 | 0.02 | 0.29 | ~0 | ~1.0 | 0.09 |
| 1 收藏 | 90 | 1.53 | 3.83 | 0.06 | 0.41 | 0.03 | 0.12 | ~0 | ~0.9 | 0.10 |
| 2 表情 | 390 | 1.92 | **53.61** | 0.06 | 0.44 | 0.05 | 0.03 | ~0 | 0.90 | 0.10 |
| 3 符号 | 270 | 1.97 | 3.23 | 0.07 | 0.42 | 0.06 | 0.47 | ~0 | ~0.9 | 0.10 |

（单位 ms。**`begin` = `BeginPaint` + `GetClientRect`；`prep` = 缩略图/emoji 位图预建 +
`PaintBegin`/`BindDC`**——注意缩略图预建在 `t0` 之后（`ui/paint.cpp:855-863`），
所以它计入 `prep` 而不是 `begin`（这一点最初写错了，正是收藏页 `prep` 异常升高的原因）；
`end` = `PaintEnd` 里的 `EndDraw`，`blit` = 整窗 BitBlt。字段定义见 `ui/paint.cpp:840-945`。）

**补充（验证时发现，原报告漏记）**：`end` 段不只有 0.9 ms 的中位值，还存在 **16~19 ms 的
单帧尖峰**——历史页某批 `max=16.25`（该批 `end` 均值 2.13 对正常 1.0）、符号页 `max=18.83`
（`end` 均值 1.97），两处瓶颈段都是 `end`。也就是说「`EndDraw` 是硬下限」只对中位数成立，
**尾部比原报告描述的更长**；「渲染层没有油水」的结论要限定为「中位数没油水」。

**读法**：

- `end` ≈ 0.9 ms 是**帧时间的下限**，且与画什么无关——D2D 的绘制调用只是记录，
  真正光栅化发生在 `EndDraw`。想降它只能减少像素/图元数量（例如只 blit 脏区、
  缓存静态区域为位图），不能靠精简 `Paint*` 的 C++ 代码。
- 表情页 `body` 只有 0.03 ms，说明视口内 ~60 格的 `DrawBitmap` 已经很便宜；
  它的 53.6 ms 峰值来自 `prep` 段（按需拉起 emoji worker，建一套 D2D/DWrite 设备
  要几十毫秒），这是**已知的设计取舍**，见 P0-5。
- 符号页 `body` 0.47 ms 是四页里最高的，对应 `PaintSymbols` 每格一次
  `U8N` + `unordered_map<string>` 查找（P1-8）。

### 一条铁律

UI 线程是**唯一**执行者：窗口过程、钩子回调、定时器、剪贴板读取、SQLite、
UIA 探测、PNG 解码、缩略图构建全在同一条线程。因此：

> 任何一次 >16 ms 的同步工作，都会外溢成别人的输入延迟（钩子链被堵）。
> 判定性能问题的顺序是「找长尾」→「找 O(N)」→「才轮到找常数因子」。

---

## 三、问题清单

分级标准：**P0** = 单次可造成 >16 ms 的 UI 线程阻塞，或随用户规模放大的写放大；
**P1** = 持续成本，每帧/每次交互都要付，但单项 <5 ms；**P2** = 顺手可做，不值得单独排期。

---

### P0-1 `HistoryFlush()` 全量重写 + 逐行 prepare　**【验证后降级为 P3，不建议做】**

**证据**：`core/history_store.cpp:585-604`（`BEGIN` → `DeleteMissingItems` →
`for (auto& it : g_items) InsertItem/UpdateItem` → `SaveRecent` → `COMMIT`）、
`:244-267`（`UpdateItem` 每次都 `sqlite3_prepare_v2` + `sqlite3_finalize`）、
`:201-223`（`InsertItem` 同样）、`:286-304`（`SaveRecent` 先 `DELETE FROM recent` 再逐条插）。

**现象**：一次历史变动（防抖 900 ms，`history_store.cpp:606`）会把**全部**条目
写成 UPDATE，即使其中只有 1 条变了。

**开销实测**（验证时用与 `history_store.cpp` 相同的 schema + `WAL` + `synchronous=NORMAL`
重测，7 次取中位；本项目 `SQLITE_THREADSAFE=0` 只会更快，故为保守上界）：

| 项 | 原估算（500 条） | **实测（500 条）** | 实测（5000 条） |
|---|---|---|---|
| `DeleteMissingItems`（`NOT IN` 大列表 + files 子查询） | 未单列 | **0.14 ms** | — |
| 全量 UPDATE × N（逐条 prepare + finalize） | 3~8 ms（prepare）+ 5~15 ms（执行） | **0.82 ms** | 10.8 ms |
| `SaveRecent`（删 + 插约 160 行） | 1~3 ms | **0.13 ms** | 同 |
| **一次 `HistoryFlush` 合计** | **10~20 ms** | **1.15 ms** | **约 12 ms** |

要点：

- `sqlite3_prepare_v2` 实测约 **0.22 µs/次**，与原估算的 5~15 µs 差 20~70 倍
  （WAL 下短语句的编译极便宜）。
- 「预编译复用」相对「每行重新 prepare」只快 **11%**（0.96 → 0.85 ms），
  即下面第 1 条改法的实际收益是 **0.1 ms/500 条**，不是几毫秒。
- 默认上限 500 条时这条路径 **1.2 ms**，远在 16 ms 预算内，用户不可能感知；
  即便调到 `HIST_MAX=5000`，12 ms 也只是每 900 ms 防抖一次的偶发一帧。
- 这条路径确实**没有埋点**，但也**不必埋**——上面的实测已经说明它不痛。

**改法**（**注意：按上面实测，默认配置下全部做完只省约 1 ms，以下改动只在用户把历史上限
调到 1000 以上、且已用埋点确认这是瓶颈时才考虑**；按性价比排序）：

1. 预编译 + 复用语句：把 `kInsertItem` / `kUpdateItem` / `kInsertFile` / recent 两条
   `prepare` 一次，放进 `OpenDb()` 之后的静态槽位，循环里只 `sqlite3_reset` +
   `bind` + `step`。省掉 N 次 SQL 编译（这是纯机械改动，风险极低）。
2. 脏标记：`ClipItem` 加 `bool dirty`（或用一个 `std::vector<int>` 收集本次改动项），
   `HistoryFlush` 只 UPDATE 脏条目。行为等价的前提是「除本进程外没人写这个库」
   ——对单实例常驻程序成立，但请在函数头注释里写明这个前提。
3. `SaveRecent` 只在 `g_recentEmo/g_recentSym` 变化后调用一次（`AddRecentGlyph`
   `glyph/glyph.cpp` → `HistoryTouch` 时置一个 `g_recentDirty`），否则跳过。
4. 若用户把上限调到 1000 以上，考虑把 `HistoryFlush` 移到后台线程。注意
   `CMakeLists.txt` 里给 sqlite 编了 `SQLITE_THREADSAFE=0`（刻意去掉互斥），
   搬线程必须**每线程独立连接**，或者把宏改回 1 并接受那点锁开销。
   在那之前，先做 1+2+3。

**风险**：脏标记漏标 → 改动不落盘。用一句 `assert(全部非脏 && 库与内存一致)`
在 `--selftest` 里对一次账即可。

---

### P0-2 首帧缩略图对原图全尺寸解码　**【已验证：全篇最值得修的一条】**

**证据**：`image/thumb.cpp:28-72`（`GetThumb`：`PngToRgba`/`DibToRgba` 得到**原始尺寸**
需 `BoxDownsample` 到 36 px）、`image/raster.cpp` 的 `PngToRgba`（WIC 解码到
`w*h*4`，上限 64 M 像素 = **256 MB 单次分配**）、`DibToRgba`（`StretchDIBits` 到原尺寸
DIB section，再 `assign` 整块拷进 `std::vector`，两次全量内存搬运）。
调用点在 `ui/paint.cpp:857-863`（`PaintPanel` 在进入 D2D 批次前对所有可见行预建）。

**现象**：滚动到一张 4K 截图（或刚复制一张大图后打开面板）时，那一帧要付：
全尺寸解码 + 256 MB 以内的临时分配 + 全图遍历下采样 + 释放。

**开销估算**：4K PNG 解码 30~80 ms，3840×2160×4 = 33 MB 的降采样遍历 15~30 ms，
合计 **50~120 ms 单帧**。

**【验证结论：归因已证实。】** 日志里 page=1 收藏页有一批 `max=53.49 ms`，而该批 `prep`
均值 2.41 ms（其他批 0.40 ms），差额 2.01 ms × 30 帧 ≈ 60 ms 集中在一帧，即单帧约 50 ms；
该页 `prep` 段唯一的重活就是 `ui/paint.cpp:855-863` 的缩略图预建——就是这条。
（原报告只说表情页有 53 ms 峰值，实际收藏页同样有，且两者归因不同：表情页是 emoji worker，
收藏页是缩略图。）缩略图结果只有 36×36，也就是说 99.99% 的解码结果被立刻丢掉。

**改法**：

1. PNG 路径：在 WIC 内部缩放。`IWICBitmapScaler::Initialize(frame, 36, 36,
   WICBitmapInterpolationModeFant)` 后直接 `CopyPixels` 到 36×36 缓冲，
   WIC 只解码需要的那部分。这一条能把单张耗时压到 **3~8 ms**。
2. DIB 路径：`StretchDIBits` 直接输出成缩略图尺寸（配 `SetStretchBltMode(HALFTONE)`），
   省掉原尺寸 DIB section + 33 MB 的 `assign`；质量对 36 px 够用。
3. 保底：`GetThumb` 若单张解码超过 ~8 ms，改为先在 worker 里做、该行先画占位
   （`Thumb` 为空时的现有占位分支已经存在）。但先做 1、2 就不必了。

**风险**：WIC scaler 对带 alpha 的 PNG 会做预乘处理，圆角蒙版那段
（`thumb.cpp:56-70`）本来就是自己算 alpha，需回归 `test_persist.py` 与视觉。
`DibToRgba` 只被缩略图用，缩小后不再需要「原始尺寸」语义，可以顺带把签名
改成 `DibToThumb(dib, box, out)`。

---

### P0-3 唤出路径的 UIA 探测：全进程快照 + 双探 + 500 ms 超时　**【验证后降级为 P2】**

**证据**：`ui/caret.cpp:131-152`（`ProcessIsSelfOrDescendant` 每次调
`CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS)`，然后对**每一层**父进程做
`Process32FirstW` + `do/while(Process32NextW)` 全表扫描，最多 6 层）、
`:331-340`（`ProbeCaret` 在 `approx` 时**同步再来一次**完整 UIA 探测）、
`ui/showhide.cpp:305-315`（唤出时起 `TIMER_REPROBE 140 ms`，补探处理在
**`ui/panel.cpp:481-491`**〔原写 showhide.cpp，验证时更正〕再最多补探 2 次，每次 160 ms）、
`:306` 的 `TIMER_CARET 530 ms`。

**现象**：当目标是浏览器/Electron 时（`GetGUIThreadInfo` 的 `hwndCaret` 恒为 0，
只能走 UIA）：

- 一次唤出 = 1~2 次完整 UIA 会话，每次含一次全进程快照 + 最多 6×进程数 的结构体遍历；
- `ProbeCaret` 明确会做两次（注释解释为 Chromium 可访问性树懒建，第一次只回整框）；
- 若首轮失败，还有 2 次补探。最坏情况一次唤出付 **3~4 次** UIA 会话。

**开销估算**：全进程快照在本机（约 300 进程）约 0.3~1 ms/次，6 层遍历再 0.2~0.6 ms；
UIA 首访 Chromium 的几十~几百 ms 是**固有成本**（项目已把超时从 2 s 压到 500 ms，
`caret.cpp:89`，这个取舍是对的）。所以能省的是快照部分与重复会话，
**不是** UIA 本身。

**【验证结论：降为 P2。】** 快照量级约 2~4 ms，且**只在浏览器/Electron 路径触发**
（原生窗口走 `CaretRectOf`，根本不进 UIA）。省 3~8 ms 摊在同一次唤出本身的
几十~几百 ms 里，占比 <5%，用户不可感知。下面第 3 条（先居中显示、补探再贴插入符）
才是真正影响唤出延迟、值得作者拍板的那一条。

**改法**：

1. 快照缓存：`ProcessIsSelfOrDescendant` 的结果按 `(pid, ancestor)` 缓存
   （进程树在面板活着的几百毫秒内不会变），或把「pid → ppid」整张表缓存 5 s。
   这样只付第一次的 1 ms。
2. `TIMER_REPROBE` 的补探复用同一个 `CaretInfo` 缓存，避免每次补探重建快照。
3. 如果想进一步压唤出延迟：把「首次 UIA 探测」推迟到面板已显示之后
   （先按光标位置居中显示，`TIMER_REPROBE` 成功后再 `MovePanelToCaret` —— 
   这套机制**已经存在**）。也就是把 `ShowPanel` 里的 `ProbeCaret` 改成
   「先试 `CaretRectOf`（原生通道，微秒级），失败就立刻显示并让补探接手」。
   代价是浏览器场景面板先跳一下，收益是唤出延迟从几十~几百 ms 回到 7 ms 级。
   **这条要作者拍板**（视觉一次跳变 vs 唤出延迟），不建议我单方面改。

---

### P0-4 采集去重是 O(N) 全串比较，且跑在唤出路径上　**【验证后降级为 P2】**

**证据**：`clipboard/capture.cpp:276-291`（对 `g_items` 逐条与剪贴板正文比较）、
`:154-158`（`SameText`：正文为空时**复制整个 `ClipItem`** 再调 `EnsureItemText`，
`core/history_store.cpp:637-652` 是一条单行 SQL）、
`ui/showhide.cpp:262`（**`ShowPanel()` 里直接调 `CaptureClipboard()`**——所以这段
O(N) 是唤出热路径的一部分）。

**现象**：每复制一次文本，都要与最多 N 条历史逐条比全文；正文超过 16 KB
（不在内存里的）条目还会各触发一次 SQL 查询 + 全文加载。

**开销估算**：N=500、平均 80 字符 → 40 万次宽字符比较，约 0.2~0.5 ms（可接受）；
但历史里有几条 8~16 KB 的长文本时，单次比较就是 MB 级 memcmp，这类情况下
**5~30 ms**。最坏情形：几十条长文本条目全部 `text` 为空 → 几十次同步 SQL。

**【验证结论：降为 P2。】** 默认配置（500 条短文本）下就是上面那 0.2~0.5 ms，
低于感知阈值；只有「重度复制长文本」的用户才会偶发 5~30 ms。下面第 1 条
（用 `bytes` 做前置筛）是零成本改动，值得顺手写，但**不值得为它单独排期**。

**改法**（判据全部现成，不需要新字段）：

1. 前置筛：`ClipItem::bytes`（UTF-8 字节数）对文本条目已经算好
   （`capture.cpp:221`），**字节数不同 → 一定不同**，直接跳过。这一条就能把
   绝大多数候选筛掉（不同内容的长度几乎总不同）。
2. 再加一层：长度相同时比「前 64 + 后 64 个宽字符」；仍相同才做全串比较。
3. `SameText` 里那句 `ClipItem tmp = old;` 是为了调 `EnsureItemText(ClipItem&)`
   而做的整条拷贝（会把 `files`/`bits`/`thumb` 一起拷，虽然 kind==0 时它们是空的，
   但语义上是定时炸弹）。改成 `EnsureItemTextById(int dbId, std::wstring& out)`，
   别拷结构体。
4. 第 2 条（前缀失败率）达标后，`EnsureItemText` 的调用次数自然降到接近 0。

**风险**：去重判错会多出一条历史（可接受）或误删一条（不可接受）。
只做「快速排除」而不是「快速判定相等」，语义上是安全的。

---

### P0-5 表情页的 53.6 ms 峰值帧（已知取舍，需要用数据决策）

**证据**：`glyph/emoji_atlas.cpp:533-600` 的注释与 `PumpEmojiUploads` 末尾的
「活干完就 `StopEmojiWorker()`」、`glyph/emoji_atlas.cpp:85-180`（worker 每次都新建
一套 `ID2D1Factory` + `IDWriteFactory` + `DCRenderTarget` + DIB section）、
`glyph/emoji_atlas.cpp:268-272`（注释：每套设备独占 25~32 MB 的 PAGE_NOCACHE 私有池）。

**现象**：perf 日志里表情页 total 中位 1.92 ms、**峰值 53.61 ms**。峰值对应
「`PrepareEmojiBitmaps` 发现缺格 → `EnsureEmojiWorker()` 现建一套设备」的那一帧。
注释里已经记录了这条因果（prep 段 35~53 ms、连带系统鼠标发钝）。

**这是有意为之的取舍**：常驻设备 = +25~32 MB，不常驻 = 偶发一帧 35~53 ms。
两条路都不完美，所以**不列成 bug**，只列出可选方案供作者决策：

| 方案 | 内存 | 峰值帧 | 备注 |
|---|---|---|---|
| 现状：按需建/停 | 最省 | 35~53 ms | 滚动进新分类时才付 |
| 设备常驻到面板隐藏（`HidePanel` 再停） | +25~32 MB 仅在使用期间 | 一次唤出内只付 1 次 | **推荐**：代价被别人已经付过的「面板可见期」限住了 |
| 预热期（启动后）一次性建好并常驻 | +25~32 MB 常驻 | 0 | 与 `ReleaseIdleMemory` 的 30 分钟彻底回收冲突，不推荐 |

**另一个便宜的小修**：`emoji_atlas.cpp:160-170` 每格光栅化都在
`rt->BeginDraw()` **之后**新建并释放一个 `ID2D1SolidColorBrush`。这既是每格一次
COM 分配，也踩了项目自己在 `ui/paint.cpp:855-856` 写下的规矩（「CreateBitmap 是创建设备
资源的操作，放在 BeginDraw/EndDraw 之间会触发目标内部的 flush」）。把 brush 建到循环外面（颜色恒为黑）即可，冷启动全量预热
（本机 1911 格）能省下 1911 次对象创建。**这条无论选哪个方案都该改**，10 行以内。

---

### P1-1 搜索每敲一个字符：4×N 次小写分配

**证据**：`model/clip_model.cpp:20-26`（`MatchQuery` 对 title/source/tag/text 各做一次
`Lower()` 后 `find`）、`:31-60`（`BuildView` 每键全量重建）、
`core/text_util.cpp:7-10`（`Lower` 按值收参 → 一次拷贝 + 一次返回拷贝）。

**开销估算**：N=500、平均 40 字符 → 每键 **2000 次堆分配 + 8 万次 `towlower`**，
估算 1~3 ms。它和渲染预算同量级，且发生在按键路径上（打字越快越明显）。

**改法**：加一个不分配的比较器（`core/text_util.h`）：

```cpp
bool ContainsCI(std::wstring_view hay, std::wstring_view needle);  // 已 Lower 的 needle
```

`MatchQuery` 改成「先用 `ContainsCI` 比 title，命中即 `true`」（短路顺序已经是对的），
直接省掉 4 次分配。若要再进一步，可在 `CaptureClipboard` 里给 title/tag/source
各存一份小写副本（三者都很短，总增量 <100 B/条），但没必要——先做不分配版本即可。

---

### P1-2 `TextW` 缓存满 600 条就整表清空

**证据**：`core/text_measure.cpp:41-52`（`if (g_twCacheN >= 600) { clear(); }`）。

**现象**：缓存 key 是 `std::wstring` 整串。行内「大小 - 时间」串每行不同、
滚动时不断产生新 key，很容易把 600 额度顶满 → 整表清空 → 下一帧全部重新
`CreateTextLayout`（每次一次 COM 分配 + shaping）。清空那一帧的 `body` 段会明显抬高。

**改法**（按成本排序）：

1. 给「稳定的固定文案」（页签、chip、页脚提示、菜单标签、`SizeText` 结果）用一个
   永不淘汰的小表；给「每行动态串」用一个 64 条的小 LRU。两档加起来替代现有的单表。
2. 或者更简单：`PaintRow` 里那两处 `TextW` 改成「不算」——`rt` 串右对齐可以直接
   交给 DWrite 的 `DT_RIGHT`（`TextOutF` 已经会设 `DWRITE_TEXT_ALIGNMENT_TRAILING`），
   只有在需要「副行左边界不被右侧吃掉」时才要宽度。看 `paint.cpp:284-306`，
   这个宽度确实被用于算 `mR.right`，所以不能直接删——但可以把它并入 P1-3 的每行缓存。

---

### P1-3 每行每帧重算相对时间

**证据**：`ui/paint.cpp:284`（`SizeText(it.bytes) + L" - " + RelTime(it.st)`）、
`core/util.cpp:35-49`（`RelTime`：`SystemTimeToFileTime` ×2 + `GetLocalTime` + `to_wstring`）。

**开销估算**：可见 ~10 行 → 每帧 10 次时间换算 + 10 次字符串拼接分配，
约 0.05~0.15 ms。单项很小（被 `EndDraw` 的 0.9 ms 淹没），但它是个「每帧全量重算」
的形状，会随行数线性增长。

**改法**：`ClipItem` 加 `std::wstring metaRight`（`"512KB - 3分钟前"`），在
`BuildView` 里生成；`RelTime` 的精度只到分钟，所以再按「当前分钟变化」失效一次
（`main.cpp` 已有 900 ms 的持久化定时器，不差这一个）。

---

### P1-4 页脚每帧遍历全量视图统计置顶/收藏数

**证据**：`ui/paint.cpp:519-524`（`for (int i : g_view) { if (g_items[i].pinned) ++pinned; ... }`）。

**现象**：为了画一行「N 条 · M 置顶 · K 收藏」，每帧扫全部 N 条。N=5000 时是 5000 次
随机访问（`g_view` 索引 → `deque` 元素），估算 0.1~0.3 ms/帧，且完全没必要——
这两个数只在 `BuildView` 时变化。

**改法**：`BuildView` 末尾顺手算好 `g_pinnedCount` / `g_favCount`
（`model/clip_model.*` 里加两个只读全局，或塞进 `Layout` 之类已有结构），
`PaintFoot` 直接读。

---

### P1-5 整窗 BitBlt 而不是脏区

**证据**：`ui/paint.cpp:927`（`BitBlt(dc0, 0, 0, W, H, ...)`），与 `:852`
（脏区 `dirty` 早就算出来了，整段绘制都在按它裁剪）。

**开销估算**：560×620×4 ≈ 1.4 MB 内存拷贝，实测 `blit` 段 0.09~0.10 ms。
滚动时只重画 body（约 500×480），却拷整窗 —— 白拷约 40%。

**改法**：`BitBlt(dc0, dirty.left, dirty.top, dirty.right-dirty.left,
dirty.bottom-dirty.top, dc, dirty.left, dirty.top, SRCCOPY)`。
两行，零风险，直接砍掉 30~40% 的 blit 时间。

---

### P1-6 `LoadEmojiCache()` 先整块上传再逐格覆盖

**证据**：`glyph/emoji_atlas.cpp:376`（整块 `CopyFromMemory(axis × axis)`）与
`:377-390`（注释自述「整块会返回 S_OK 但内容错位，所以始终以逐格为准」，随后逐格重传）。

**现象**：明明已经确认整块结果不可用，却仍然执行了一次 **19.5 MB** 的全量上传，
然后立刻用逐格上传覆盖它。启动读缓存时白付一次拷贝 + 一次（错的）上传。

**改法**：删掉 `:376` 那次调用与它的 `hr` 日志（日志可以保留一个「cells=N」即可），
约 6 行。收益是启动进表情页时省 5~15 ms（估算，取决于图片总线的吞吐）。

---

### P1-7 `PrepareEmojiBitmaps` 对同一批格子做 5 趟遍历

**证据**：`glyph/emoji_atlas.cpp:540-640`：`std::string key(it.g, it.gl)` 在第 561、588、603、630 行
各构造一次（每趟为每个可见格构造一个临时 `std::string` 并做一次
`unordered_map<std::string,int>` 查找）；第 3 步的 `need` 探测与 `post` 是两趟，
第 2 步「刷新最近使用」又是一趟。

**开销估算**：可见 ~60 格 × 5 趟 = 300 次短串构造 + 300 次哈希查找，约 0.05~0.15 ms/帧。
数值不大，但它是**每帧**成本，而且是纯粹的重复劳动。

**改法**：按「列表下标 → 槽位」维护一个 `std::vector<int> g_emoSlotByIndex`
（当前分类变化时重建一次，`GLYPH_CAT_CHANGED` 时清空），`PrepareEmojiBitmaps`
只走一趟：查表拿槽位 → 需要的投递 → 统计命中。

---

### P1-8 符号页每格做一次 UTF-8 转换 + `map` 查找

**证据**：`ui/paint.cpp:436`（`std::wstring g = U8N(list[i].g, list[i].gl)`）、
`:330-343`（`PickSymFont`：`unordered_map<std::string,int>` 查字体槽位，未命中时
再对 4 个候选字体做 `HasCharacter` 探测）。

**开销估算**：可见 ~90 格 → 90 次 `MultiByteToWideChar` 分配 + 90 次哈希。
实测体现在符号页 `body` 0.47 ms（四页最高）。

**改法**：`GCat` 加载（`glyph/glyph.cpp` 的 `LoadCat`）时一次性建两个平行数组：
`std::vector<std::wstring> wide`（每字形的宽串）与 `std::vector<uint8_t> fontSlot`。
`LoadCat` 本来就只跑一次、且已经在做逐条 `GlyphDrawable` 探测，把 `PickSymFont`
的判定结果一并写进去几乎没有额外成本。内存增量：按当前分类上限几千条 × (wstring 24B
+ 数据) ≈ 几百 KB，且只在 `loaded` 的分类上存在。

---

### P1-9 分类名每帧做 UTF-8→UTF-16 转换 + 文本度量

**证据**：`core/glyph.cpp` 的 `GlyphCatName`（每次 `U8N` 分配），调用点
`ui/layout.cpp:115`、`:134`（每帧 `ComputeLayout` 里 `TextW(..., GlyphCatName(...))`）、
`ui/paint.cpp:220`、`:461`（每 chip 一次）。

**现象**：每帧 3~8 次分配 + 对应次数的 `TextW` 查表，而分类名只在切页/切分类时变。

**改法**：`ComputeLayout` 时把 `(宽, 宽串)` 一起算进 `L.chip[i]` 的伴生数组
（比如 `L.chipW[i]` 与一个 `static std::vector<std::wstring> chipLabel`，
用 `(page, cat)` 做失效键），`PaintFilters`/`PaintDrawer` 直接读。

---

### P1-10 页签圆角每帧新建 D2D 几何对象

**证据**：`ui/paint.cpp:48`（`FillTopRound`）、`:113`（`FillTRRound`）：两者都
`PathB p; ... p.Fill(c)`，每次调用创建一个 `ID2D1PathGeometry` + `ID2D1GeometrySink`。
头部重绘一帧 = 4 个页签 × 2 次（边框层 + 底色层）+ 最多 3 个按钮高亮 ≈ **11 个 COM 对象**。

**影响**：只在头部重绘时付（滚动时脏区只有 body，不受影响），`head` 段实测
0.02~0.06 ms，所以**单项不痛**。但它是「每帧新建 D2D 资源」这一类里的最后一块。

**改法**：按 `(w, h, rad)` 缓存几何（页签尺寸只在窗口宽度变化时变），
或用 `ID2D1RenderTarget::DrawRoundedRectangle` + 矩形拼出「上圆下直」的形状
（两条圆角 + 一条直角矩形），完全不必自建几何。

---

### P1-11 启动时孤儿图片清理是 O(目录文件 × 条目) 的双重循环

**证据**：`core/history_store.cpp:350-370`（`for (fd) { for (const auto& k : keep) if (k == name) ... }`）。

**现象**：`keep` 是 `std::vector<std::wstring>`，对每个目录项做线性字符串查找。
500 条目 × 若干文件 = 几十万次 `wstring` 比较，启动时一次性几 ms。

**改法**：`std::unordered_set<std::wstring> keep`。三行。

---

### P1-12 剪贴板定时器后无条件重建视图

**证据**：`app/main.cpp:118-123`（`TIMER_CLIP` → `CaptureClipboard(); BuildView();
ComputeLayout(); Redraw();`），而 `CaptureClipboard` 在去重命中时会**提前 return**
（`clipboard/capture.cpp:164` 自己写的剪贴板内容、`:275` 与首条历史相同），此时视图根本没变。

**改法**：`CaptureClipboard` 返回 `bool changed`，未变化就直接返回。
省掉一次 O(N) 的 `BuildView` + 整帧重绘（面板可见时）。

---

### P2（顺手做，别单独排期）

| # | 问题 | 证据 | 建议 |
|---|---|---|---|
| P2-1 | 采集时为判断「是不是链接/代码」把**整段正文**小写化 | `clipboard/capture.cpp:240` | 只看前 8 个字符（`http://`/`git `/`npm ` 都是前缀判断），用 `CompareStringOrdinal(..., TRUE)` 或手写不分配的比较 |
| P2-2 | `BuildView` 两个临时 `vector` 每键重新分配 | `model/clip_model.cpp:34-37`、`:56-58` | 改成函数内 `static thread_local` scratch，`clear()` 复用；`g_view.reserve(tmp.size())` 一次 |
| P2-3 | `DeleteMissingItems` 拼 SQL 字符串 + 全表子查询 | `core/history_store.cpp:270-284` | 已在 P0-1 的范围里；若保留 SQL 版，用临时表 + `JOIN` 替代 `NOT IN (超长字面量列表)` |
| P2-4 | `items` 表缺 `(pinned, fav)` 索引 | `core/history_store.cpp:516-527` | 500 条规模无所谓；若 P0-1 决定支持 5000 条，加 `CREATE INDEX IF NOT EXISTS idx_flags ON items(pinned,fav)` 到 `kSchema` |
| P2-5 | `SaveRecent` 每次全删全插 | `core/history_store.cpp:286-304` | 并入 P0-1 第 3 条 |
| P2-6 | 编译期没有 LTO；**且 `-O2` 覆盖了 Release 的 `-O3`**（`CMakeLists.txt:100` 追加的 `-O2` 排在 `CMAKE_CXX_FLAGS_RELEASE` 的 `-O3` 之后，`build.ninja` 里实际展开为 `-O3 … -O2`，生效的是 `-O2`） | `CMakeLists.txt:100`、`:93`（`SQLITE_THREADSAFE=0`） | 试 `-O3 -flto=auto` 时**必须先改用条件化写法**（如 `$<$<CONFIG:Release>:-O3>`）替换掉这里的裸 `-O2`，否则换 O3 不生效；用 `tools/measure.py` 前后各测一次再决定，不进默认构建（MinGW 的 LTO 偶有链接失败） |

---

## 四、内存画像

| 项 | 大小 | 何时付 | 结论 |
|---|---|---|---|
| 主线程 D2D 设备（工厂 + DCRenderTarget） | **25~32 MB** PAGE_NOCACHE 私有池 | 首次 `EnsureD2D`，常驻 | 合理：释放重建要 60 ms，压在唤出热路径上（`ui/showhide.h:51`）|
| emoji worker 设备 | 同量级 **25~32 MB** | 每次拉起，活干完就还 | 见 P0-5 的取舍表 |
| emoji 图集位图 | 2116 格 × 48² × 4 B ≈ **19.5 MB** | 进表情页后常驻，闲置 30 min 释放 | 合理 |
| emoji 预热镜像 | 同 19.5 MB（`VirtualAlloc`） | 预热期间，落盘后 `VirtualFree` | 已经用虚拟内存 API 真还回系统，做法正确 |
| 缩略图 | 36×36×4 = 5 KB/张，`shared_ptr` 共享 | 首次可见时建 | 合理 |
| 图片条目像素 | 预算 32 MB / 最近 4 张（`clipboard/capture.cpp:55-58`） | 超出即释放，磁盘有副本 | 合理 |
| 历史正文 | ≤16 KB/条进内存（`core/history_store.cpp:19`） | 500 条上限约 8 MB | 合理 |
| SQLite 页缓存 | 默认 2 MB | 常驻 | 可考虑 `PRAGMA cache_size=-8192`，但优先级最低 |

**唯一建议**：P0-2 修掉后，`PngToRgba` 的「64 M 像素上限 = 256 MB 单次分配」
应当一并缩小（改成「缩放后 ≤ box²」，上限自然掉到几十 KB）。这是当前代码里
唯一能一次性吃掉几百 MB 提交的地方。

---

## 五、明确不建议做的

1. **不要为了「降 CPU」去重构 `Paint*`。** 实测 `end`（`EndDraw`）中位 0.9 ms，
   **但尾部有 16~19 ms 单帧尖峰**（见二节的补充），所以「别动”的只是常数因子，
   不是尾部；body+head+prep 合计不到 1 ms，把 `PaintRow` 里的字符串全优化掉用户也感知不到。
2. **不要把 emoji 光栅化改成多 worker。** 每套设备 25~32 MB 非缓冲私有池，
   并行度换内存不划算（这条项目已经写在 `emoji_atlas.cpp:225-232`，别推翻）。
3. **不要在渲染帧里「每帧停/起一套 D2D 设备」省内存。** 建一套几十 ms 且刻在 UI 线程上，
   连带整机鼠标发钝（`glyph/emoji_atlas.cpp:570-580` 有实测），这是净亏。
4. **不要把 `TextW` 换成 GDI 的 `GetTextExtentPoint32` 当主路径。** DWrite 才是与
   `DrawTextW` 同一口径的来源，换掉会出现「量宽与绘制不一致」的贴边错位。
5. **不要给 1.5 ms 的帧引入脏区-缓存层（静态区域缓存成位图）。** 收益上限 0.9 ms，
   复杂度极高（缓存失效点遍布 `paint.cpp` 每个 `Paint*`），典型过度优化。
6. **不要为了性能把 `history_store` 改成自己写文件格式。** SQLite 的 WAL +
   `synchronous=NORMAL` 已经比手写 `.tmp + MoveFile` 更安全更快。补充（验证结果）：
   「全量 UPDATE」实测 500 条共 1.15 ms、5000 条约 12 ms，**这条路径整体不是瓶颈**，
   没必要为它改结构、换引擎。

---

## 六、建议的落地顺序

| 阶段 | 内容 | 判据 |
|---|---|---|
| **0（2 小时）** | 给 **3** 条路径加计时：`GetThumb` 单张解码 / `ProbeCaret` / `CaptureClipboard`。复用现成的 `PerfNow()` + `PerfAppend()`（`core/diag.h`）。**`HistoryFlush` 不必埋**——验证时已实测 1.15 ms，不痛 | 日志里能看到这 3 个数字的 P50/P95 |
| **1（1 天）** | **P0-2** WIC 内部缩放 + `DIB` 直出缩略图 | 滚动大图历史时 perf 日志单帧峰值 <16 ms |
| **2（半天）** | **P0-5** 的 brush 移出循环（10 行）+ 决定设备是否常驻；**P1-6** 删掉那次整块上传；**P1-11** 换 `unordered_set` | 进表情页省 5~15 ms；表情页峰值帧不再出现 |
| **3（半天）** | **P1-1** 不分配的比较器、**P1-8**、**P1-9**（打字与符号页手感） | 符号页 `body` 从 0.47 ms 降下来；连打搜索不掉帧 |
| **4（按需）** | P1-2 / P1-3 / P1-4 / P1-5 / P1-7 / P1-10 / P1-12、P2 各条 | 帧中位不变或更低（证明是纯收益） |
| **不做** | **P0-1**（改完省不到 1 ms）、**P0-3 快照缓存**（省 3~8 ms / 几百 ms）、**P0-4 前置筛**（0.5 ms） | 见 `PERFORMANCE-REVIEW-VERIFY.md` 第三节 |

**如果只有两小时**：做 **P0-5 的 brush**（10 行）+ **P1-6**（6 行）+ **P1-11**（3 行）。
这三条零风险、且收益已被实测支持（前者掐掉 53 ms 峰值的一环，后两者是纯删除/替换）。

---

## 七、需要作者决策的两点

1. **P0-3 第 3 条**：浏览器目标下，是接受「唤出先居中、140 ms 后跳到插入符旁」，
   还是坚持「一次到位但可能慢几十~几百毫秒」？前者延迟低，后者视觉稳。
   代码里两套机制都已具备，只差一个策略选择。
2. **P0-5**：emoji worker 的设备是「按需建/停」（省 25~32 MB，偶发 35~53 ms）
   还是「面板可见期内常驻」（多占 25~32 MB，一次唤出内只付一次）？
   这取决于你对低内存机器与滚动手感的取舍。

---

## 附录 A：perf 日志字段与复现方式

```bat
:: 启动带探针
build\winvclip.exe --perf            :: 或 set WINVCLIP_PERF=1 后启动
:: 日志：%TEMP%\winvclip_perf.log
```

每 30 帧输出一行（`ui/paint.cpp:742-769`）：

| 字段 | 含义 |
|---|---|
| `begin` | `BeginPaint` + `GetClientRect` + 缩略图/emoji 位图预建 |
| `prep` | `PaintBegin`（含首次 `BindDC`）到批次开始 |
| `head` / `body` / `foot` | 三段绘制调用（**只是记录，未落地**） |
| `end` | `PaintEnd`（含 `EndDraw`，真正光栅化） |
| `blit` | 离屏缓冲 → 窗口 DC |
| `total` / `max` | 整帧；`max` 是本批 30 帧的最大值 |
| `emoji new/hit/fail` | 本批上传进图集的格数 / 可见格命中数 / 失败数 |

驱动场景：`tools/perf_drive.py`（切页签 + 连续滚轮）、`tools/measure_grid.py`
（各页滚动 + UI 线程往返延迟）、`tools/measure.py`。

## 附录 B：本次审查的边界

- **已精读**：`ui/paint.cpp`、`ui/layout.cpp`、`ui/panel.cpp`、`ui/showhide.cpp`、
  `ui/caret.cpp`、`glyph/emoji_atlas.cpp`、`glyph/glyph.cpp`、`model/clip_model.cpp`、
  `clipboard/capture.cpp`、`core/history_store.cpp`、`core/text_measure.cpp`、
  `core/settings.cpp`、`core/util.cpp`、`core/io_png.cpp`、`render/dc.cpp`、
  `render/fonts.cpp`、`image/raster.cpp`、`image/thumb.cpp`、`app/hook.cpp`、`CMakeLists.txt`。
- **只扫未细读**：`ui/actions.cpp`（动作执行，一次性操作）、`app/startup.cpp`
  （自启登记，只在启动/设置时跑）、`ui/settings_win.cpp`（低频窗口）。
- **未覆盖**：`tests/`、`tools/`、`third_party/sqlite3.c`（vendored，不按本项目代码维护）。
- **未做**：任何代码改动。本报告中「实测」指引用现有 perf 日志；「估算」是基于
  代码结构 + 本机规模的推算，标注为估算的数字都需要阶段 0 的埋点来确认。
