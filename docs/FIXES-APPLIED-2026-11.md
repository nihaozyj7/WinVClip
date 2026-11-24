# 稳定性 / 性能问题修复实施报告

对象：`docs/STABILITY-REVIEW-VERIFIED.md` 与 `docs/PERFORMANCE-REVIEW.md` 经
`docs/REVIEW-AUDIT-2026-11.md` 复核后的待修项。
本报告只记「改了什么、怎么验证的、为什么有的没改」；机理推导见前面两份文档。

---

## 一、一句话结论

**19 项改动全部落地并编译通过；全量回归 `desktop` 组 13/13 通过，`--all` 25 项 24 通过**
（唯一失败 `test_inputmode` 是「前台被资源管理器占着」的环境问题，非回归）。

三条关键链路单独做了针对性验证，都拿到阳性证据：

| 验证 | 方法 | 结果 |
|---|---|---|
| 事务失败不再留中间态 | 外部连接持 `BEGIN EXCLUSIVE` → 复制 20KB 文本 | 日志出现 `begin-failed`，库里该条目 **0 行**；解锁后 2 条正常落盘、正文 20020 字符完整 |
| 图集落盘不带空洞 | 删缓存 → 全量预热 → 重启读回 | `warm: saved up=1911` 对 `load: cells=1911`（此前是 `1923` 对 `1911`） |
| 渲染没退化 | `WINVCLIP_PERF=1` 跑滚动用例 | 表情页 `total` 1.69~2.02ms、`max` ≤ 4.75ms（基线 1.5~1.9 / 曾 53ms） |

---

## 二、稳定性（改动逐条）

### 1. 落盘事务（最高优先，唯一会静默丢数据的地方）

`core/history_store.cpp`

- `Exec()` 改为**返回 `bool`**，所有调用方必须看返回值。
- `HistoryFlush()` 重写：`BEGIN IMMEDIATE` 失败 → 直接 `return`，**一条语句都不执行**。
  这是复核里发现的、比原报告更严重的一条：BEGIN 失败后 SQLite 转入自动提交，
  `DeleteMissingItems` 会单独提交成功、后续 `InsertItem` 失败被忽略，
  结果是「库里删了、新的没写」——中间态比整事务失败脏得多。
- `InsertItem` / `UpdateItem` / `SaveRecent` / `DeleteMissingItems` 全部返回 `bool`，
  任一失败 → `ROLLBACK` + **不调 `ReleaseLongTexts`**（正文还没进库）+ `Trace`。
- 成功路径才 `ReleaseLongTexts()`。

### 2. 用户能看见「落盘失败」

`HistoryFlushFailed()` 暴露给 UI；`app/main.cpp` 的 `TIMER_PERSIST` 失败时 `Toast`
一次（同一次故障只提示一次，恢复后重新武装）。早先数据库打不开是**完全静默**的：
表现就是「这次复制的东西重启后没了」，用户无从排查。

### 3. 注销 / 关机落盘

`app/main.cpp` 新增 `WM_QUERYENDSESSION` / `WM_ENDSESSION`：杀防抖定时器 + 立即
`HistoryFlush()`。此前「复制 → 立刻关机」会丢掉最后一条（900ms 防抖还没到）。

### 4. 表情图集：跨线程竞态与落盘条件

`glyph/emoji_atlas.cpp`

- `g_emoDisk` 的分配/释放纳入 `g_emoMx`（worker 是在锁内 `memcpy` 进这块镜像的，
  早先 UI 线程裸指针 `VirtualFree` 等于可能释放掉对方正在写的那 19.5MB）。
- `SaveEmojiCache()` 只在**预热格全部 ready 且已无在途任务**时执行，
  写成功才置 `g_emoWarmSaved`（失败不再「本会话永久放弃」，而是允许重试一次，
  然后释放镜像以免白占 19.5MB）。
  这条同时修掉 N1：早先的判据 `g_emoUpN >= g_emoWarmSlots` 混进了视口抢跑的格子，
  计数够了也可能有空洞；带洞的缓存读回后整块标 ready，那批表情就**永久空白**。
- 冷启动把 `ID2D1SolidColorBrush` 移出 `for(;;)`（1911 格各建一个纯属浪费）。
- 删掉缓存载入时那次整块 `CopyFromMemory`：注释自述它「返回 S_OK 但内容错位」，
  后面本来就要逐格覆盖，那 19.5MB 拷贝纯属白做（`bulk-hr=0x0` 是它的墓志铭）。

### 5. 表情预热不再 125Hz 全帧重绘

`PumpEmojiUploads(int* uploaded)` 回传「本帧真上传了几格」，`panel.cpp` 的
`TIMER_EMOJI` 只在 `uploaded > 0` 时 `Redraw`，其余仍按 8ms 轮询但不重绘。
早先预热期间（约 3 秒、1911 格）是以 125Hz 重绘整帧的，正好撞在用户滚动时。

### 6. 剪贴板采集

`clipboard/capture.cpp`

- `CaptureClipboard()` 返回「历史是否真的变了」；`TIMER_CLIP` 只在变化时才
  `BuildView + ComputeLayout + Redraw`（同一内容复制两次不再白跑一次全量重排）。
- `OpenClipboard` 失败改为**指数退避** 160/320/640/1200/2000ms 封顶（原先固定 160ms
  无限重试，遇到长期占用剪贴板的程序会一直滴答）。

### 7. 菜单期间条目错位

右键菜单锁的是 `g_view` 里的**下标**，菜单开着时若剪贴板变化导致列表重排，
按下「删除」删掉的会是另一条。现在呼出菜单时同时记下该条目的
`dbId + st + title` 指纹，`RunMenuItem` 执行前核对，不一致就提示「列表已变化」
并放弃动作（删除不可撤销，宁可让用户再点一次）。

### 8. 其它卫生项

- `GlobalLock` 返回值判空（`clipboard/paste.cpp` 8 处）。
- `MoveDataFromDir`：`MoveFileW` 失败时**不再删 WAL**（早先会先删掉 -wal，
  那批已提交事务就真没了）。
- `PruneOrphanImages` 的保留集改 `unordered_set`（启动时少几十万次 wstring 比较）。
- `Render::PaintEnd` 检查 `EndDraw` 返回值并 `Trace`（`D2DERR_RECREATE_TARGET`
  时界面会「突然不刷新」，此前日志里什么都没有）。
- `ReadPngSize` 改用无符号拼接（原写法 `b[16] << 24` 对有符号 int 是 UB），并判掉 0。
- 数据库打开/建表失败留 `Trace`；`HistoryLoad` 打一行 `db: load mark=[...] dir=[...]`
  （路径按 UTF-8 打，`%ls` 在当前 locale 下会把中文截断，反而看不清）。

### 9. 顺带修的一个既有 bug

启动时 `ApplyHistoryLimit()` 只裁内存、不落盘：用户把上限从 500 改到 50 再重启，
库里那 450 条会一直躺到下一次复制。现在裁剪真的发生了就补一次 `HistoryTouch()`。
（`test_clear` 的「上限裁剪」一节此前在 `admin=0` 下是**失败**的，与本轮改动无关，
是它第一次真正被执行到。）

---

## 三、性能（只做经得起实测的）

### 10. 缩略图长尾（唯一被实测证实的 P0）

`image/raster.cpp` / `image/thumb.cpp` 新增 `PngToThumb` / `DibToThumb`：

- PNG：WIC 解码 → `IWICBitmapScaler`（Fant）→ 32bpp，**只拷 box 大小的像素**；
- CF_DIB：`StretchDIBits` + `HALFTONE` 一次完成「位深统一 + 缩放」。

`PngToRgba` / `DibToRgba` **原样保留**——`DibToRgba` 还被「另存为图片」
（`ui/actions.cpp`）用着，改它的语义会把保存出来的图变小（复核时发现原报告的建议会弄坏这条）。
效果：6000×4000 的截图不再产生一份 ~96MB 的全尺寸缓冲，收藏页 50ms 级单帧尖峰的来源被掐掉。

### 11. 搜索路径的分配归零

`core/text_util.cpp` 新增 `ContainsCI(hay, needleLower)`：不分配内存，
逐字符 `towlower` 比较。`MatchQuery` 原来对每条条目做**四次** `Lower(...)`，
而 `Lower` 是按值收参（拷一次、原地改、返回再拷/移动）——历史里有 20 条 16KB 正文时，
每敲一个键就是几百 KB 的分配/释放。现在为 0。

### 12. 截断不再污染全局度量缓存

`ui/paint.cpp` 的 `EllipText` 原来是「从尾部一个字符一个字符砍、每次查全局 `TextW`
缓存」，一个 60 字符的临时目录路径就插几十条永不命中的中间串，几次重绘就把
600 条额度顶穿，连带把页签/chip 的固定文案全清掉重新 `CreateTextLayout`。
现在改二分查找 + `TextWNoCache`（新增，不进缓存），循环次数 O(len) → O(log len)。

### 13. 只贴脏区

`PaintPanel` 的 `BitBlt` 从整窗改为只贴 `dirty`（缓冲本来就是复用的，
脏区外仍是上一帧内容；`paint.cpp` 早就把 `dirty` 算好了，只是没用上）。

---

## 四、刻意没做的（附理由）

| 项 | 理由 |
|---|---|
| `Exec` 之外的重试线程 / `HistoryFlush` 异步化 | 与 `MEMORY.md` 记过的实测冲突：落盘在 500 条上只要 1.15ms，加线程只是增加竞态面 |
| `P0-1` 全量重写优化、`P0-3` 进程快照、`P0-4` 去重前置筛 | 实测收益 <5%，用户不可感知 |
| `S3-3` 另存图片改引用 | 会在模态对话框期间引入 UAF |
| `S3-6` 互斥量加 `Global\` | 误报：无前缀即会话命名空间 |
| `S3-5` `WM_DPICHANGED` | 纯观感，要动的地方多（面板几何全部按 `g_dpi` 预算），单独排期 |
| `S1-3` 钩子栈上的 UIA 探测（500ms） | 真正的架构问题（唤出路径不该在钩子回调里跑），改动面横跨 showhide/caret/hook，不适合与本次批量修混在一起 |
| 页脚 O(N) 计数缓存 | 500 条遍历是微秒级，为它引入一处「可能失配的派生状态」不划算 |
| `-O3` / `-flto` | `CMakeLists.txt` 末尾那条裸 `-O2` 会覆盖 `-O3`，要试必须先删它；上限按帧耗时推断只有 ~10% |

---

## 五、过程中踩到的坑（记一笔）

跑测试前我把 `settings.ini` 的 `admin` 从 1 改成 0，用的是
「读进来 → `str.replace` → 写回去」的写法，**把 CRLF 全写成了 LF**。
`test_clear` / `test_persist` 用 `raw.split("\r\n")` 解析 ini，于是整份文件被当成一行、
`history=50` / `tempdir=...` 都没写进去，两个用例假失败。改成按原行尾写回后立刻 13/13。
—— 改这两个测试的 ini 之前，先确认行尾还是 CRLF。

---

## 六、当前状态与后续建议

- 程序当前**未运行**（回归测试会停实例，且我把 `settings.ini` 还原成了原值
  `admin=1`，启动会弹一次 UAC）。双击 `build\winvclip.exe` 即可。
- 下一件值得做的是 **S1-3**：把 `ProbeCaret` 的 UIA 探测从钩子回调里挪出去
  （先 `ShowPanel` 用上次位置，探测结果回来再 `MovePanelToCaret`），
  这样唤出路径的 500ms 上限就从热键响应里消失。
