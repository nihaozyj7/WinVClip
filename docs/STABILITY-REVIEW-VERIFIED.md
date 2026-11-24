# WinVClip 稳定性审查 —— 逐条核实版

核实对象：`docs/STABILITY-REVIEW.md`（下称「原报告」）的 21 条问题。
核实方式：逐条回到源码读实现（不采信原报告的行号与因果推断），对「代码事实是否成立」
与「后果推演是否成立」分别判定；再对判定为真的条目重估严重度。
判定口径：

| 判定 | 含义 |
|---|---|
| ✅ 成立 | 代码事实与后果推演都站得住，按原级别处理 |
| ⚠️ 部分 | 代码事实成立，但机理/后果/严重度有误，需改写后再用 |
| ❌ 撤回 | 代码事实本身不成立，或建议会引入更坏的问题 |

一句话总结核实结果：**21 条里 12 条完全成立、6 条成立但严重度或机理需改写、1 条误报、
1 条建议有害、1 条事实成立但优先级被高估**。原报告指出的「系统性短板是失败之后」
这个总判断是对的；但它给的两条「最该先修」里有一条（`GlobalLock` 判空）几乎不可能触发，
而真正会稳定丢数据的只有一条（`HistoryFlush` 不判事务结果）。

---

## 一、核实结论总览

| 原编号 | 判定 | 核实后级别 | 一句话 |
|---|---|---|---|
| S1-1 emoji 镜像跨线程释放 | ✅ | **S1** | `FreeEmoDisk` 三处都在锁外，worker 持锁 memcpy，竞态真实 |
| S1-2 事务失败仍丢长正文 | ✅ | **S1（最高）** | 唯一「按概率必然发生、且丢用户数据」的路径 |
| S1-3 钩子回调内同步阻塞 | ⚠️ | **S1（理由改写）** | 事实成立，但阻塞源写错了：`OpenClipboard` 不阻塞，真正卡的是 UIA 探测 |
| S1-4 迁移忽略 `MoveFileW` 就删 WAL | ⚠️ | **S2** | 需先 `CheckpointDb` 失败才丢数据，原报告漏了这个前置 |
| S1-5 `GlobalLock` 未判空 | ⚠️ | **S3** | 8 处确实没判空，但 `GlobalAlloc` 成功后它几乎不会失败，属卫生问题 |
| S2-1 worker 失败态槽位泄漏 | ⚠️ | **S2（机理改写）** | 泄漏成立，「8ms 忙循环」不成立；真实代价是反复重建 D2D 设备 |
| S2-2 无 `WM_QUERYENDSESSION` | ✅ | S2 | 属实 |
| S2-3 库打不开完全静默 | ✅ | S2 | 属实 |
| S2-4 `EndDraw` 返回值丢弃 | ✅ | S2 | 属实（软件 DCRenderTarget 下概率低） |
| S2-5 UI 线程 `join` worker | ✅ | S2 | 属实，上界约 20ms + 一格渲染 |
| S2-6 配置非原子写 | ⚠️ | S3 | 属实，但 dir-mark 会兜底，后果被夸大 |
| S2-7 剪贴板无退避重试 | ✅ | S2 | 属实，且比原报告写的更贵（每次都重建视图+重绘） |
| S2-8 菜单期间 `g_menuItem` 错位 | ✅ | S2 | 属实 |
| S3-1 `g_view` 无二重校验 | ✅ | S3 | 属实，纯保险 |
| S3-2 `swprintf` 未判返回值 | ✅ | S3 | 属实，需 >2048 字符命令行 |
| S3-3 另存图片整份拷贝 | ❌ | — | 建议有害：改引用会在模态期间引入 UAF |
| S3-4 `Trace` 每次 fopen | ⚠️ | S3（备注） | 属实，但单次 ~0.1ms，「加剧钩子超时」是夸大 |
| S3-5 无 `WM_DPICHANGED` | ✅ | S3 | 属实，观感问题 |
| S3-6 互斥量用全局命名 | ❌ | — | **误报**：无前缀即会话命名空间，多会话本来不互挡 |
| S3-7 `ReadPngSize` 有符号拼接 | ✅ | S3 | UB 属实，但合法 PNG 触发不了 |
| S3-8 `DibToRgba` 只校验 `off` | ✅ | S3 | 属实，需恶意构造 DIB |
| **新增 N1** | ✅ | **S2** | 落盘缓存可能含未渲染空洞 → 读回即标 ready → 那批表情永久空白 |
| **新增 N2** | ✅ | S3 | `g_emoWarmSaved` 在写盘前置 true，落盘失败后本会话不再重试 |
| **新增 N3** | ✅ | S3 | 预热期间视口格子重复分配槽，加快图集耗尽 |

（新增三条都在原报告漏掉的表情页路径上，见第四节。）

---

## 二、原报告需要改写的五处

### 1. S1-5 被高估成「必然的崩溃路径」

原报告写「`GlobalLock` 失败返回 NULL」——这句本身没错，但在这个用法下不会发生：
这 8 处（`clipboard/paste.cpp:44, 55, 71, 107, 173, 183, 197, 204`）全部是
`GlobalAlloc(GMEM_MOVEABLE)` 刚成功、立刻 `GlobalLock`。NT 上 `GMEM_MOVEABLE` 就是堆分配，
`GlobalLock` 直接返回该块指针，**内存不足时 `GlobalAlloc` 已经返回 NULL 了**，轮不到 `GlobalLock` 失败。
它真正会失败的条件是「句柄不是有效的可移动块」，而这里是刚分配的句柄。

所以这是「不符合防御性写法的卫生问题」，不是原报告说的「必然崩溃路径」，也不是
「如果只有两小时就先做它」的两个候选中之一。该补的判空照补（8 行，零风险），
但别拿它占优先级第一。

**顺带**：原报告说这几处在「钩子栈上，按回车闪退看起来像热键坏了」——Enter 粘贴确实经
`hook.cpp:156` 的同步 `SendMessage` 落到这里，链路成立，只是前提几乎不成立。

### 2. S1-3 的阻塞源写错了：`OpenClipboard` 不阻塞

原报告把「别的进程开着剪贴板」当作钩子回调被拖住的主要理由，写「可阻塞数百 ms 到数秒」。
不成立：`OpenClipboard` 在他人持有剪贴板时**立即返回 FALSE**，不等待。
（`capture.cpp:168` 的 `if (!OpenClipboard(g_main)) { SetTimer(...,160,...); return; }` 正是这个语义。）

钩子回调里真正的阻塞源，按严重度排：

| 真实阻塞源 | 位置 | 上界 |
|---|---|---|
| **`ProbeCaret` 的 UIA 探测（原报告完全没提）** | `showhide.cpp:255` → `caret.cpp:331`，连接超时 500ms（`caret.cpp:89`） | **500ms**，且首次访问 Chromium 会唤醒其可访问性树（源码注释自述「几十~几百 ms」） |
| `GetClipboardData` 触发 owner 的延迟渲染（`WM_RENDERFORMAT`） | `capture.cpp:175-215` | 无界：owner 卡住就一直等 |
| `SetForegroundWindow` / `AttachThreadInput`（`HidePanel` 路径） | `showhide.cpp:215-217` | 目标线程无响应时无界 |

前两条在 `ShowPanel` 里就能同时命中：`hook.cpp:93` → `ShowPanel` → `capture.cpp:168`
（`showhide.cpp:242`）→ `ProbeCaret`（`showhide.cpp:255`）。所以「第一次在浏览器窗口上按 Win+V
时钩子线程被卡最多 500ms」是真实的，而这个量级足以吃掉低级钩子的时间预算。

结论：**S1 级别保留、修法保留（回调内只判吞键 + PostMessage，`main.cpp:102` 的
`WM_APP_HOTKEY` 通道已经现成）**，但理由要从「剪贴板」改成「UIA + 延迟渲染」。

### 3. S1-4 丢数据需要三重叠加，不是「忽略返回值就丢」

`MoveDataFromDir`（`core/history_store.cpp:613-627`）前面还有一句 `CheckpointDb(oldDb)`，
它内部同样是 `sqlite3_exec` 不判结果（`:76`）。若 checkpoint 成功，WAL 已被 `TRUNCATE`
截空，后面删 `-wal` **没有损失**。真正丢数据的组合是：

1. `CheckpointDb` 失败（旧库被别的实例/杀软占住，open 或 checkpoint 失败）→ WAL 里还压着已提交事务；
2. `MoveFileW` 也失败（同样因为被占用）→ 旧库留在原处；
3. 无条件删掉 `-wal` → 那批已提交事务永久消失。

三者同时发生的场景就是「上一个实例没退干净时改临时目录」，概率不高但真实。降到 S2，
修法照原报告（失败就不删 WAL、不改 dir-mark、Toast 提示），成本很低值得做。

### 4. S2-1 的「8ms 忙循环」不成立

`!ready` 分支（`glyph/emoji_atlas.cpp:154-158`）只 `erase(g_emoPending)` 就 `continue`，
而 `PrepareEmojiBitmaps` 末尾的续排条件是 `more`（`:529-530`）＝ pending 非空 / done 非空 /
预热未跑完。pending 被清空、预热清单被快速消费完之后 `more` 即为 false，走
`StopEmojiWorker()`（`:535`），**不会留下 8ms 定时器**。

真实代价换了个形式，而且更值得修：worker 被停掉后，下一帧 `need` 判定（`:585-594`）仍为 true
（槽未 ready）→ `EnsureEmojiWorker`（`:596`）重新建线程 + 一整套 D2D/DWrite（`:99-127`），
渲染不了又立刻被停。也就是「**每次重绘都白建一套 25~32MB 设备**」，
比原报告描述的忙循环更贵——正是 `MEMORY.md` 里记过的「表情页每帧 35~53ms」那类问题。
槽位泄漏（`:601-616` 对「有槽但未 ready」的 key 重新 `AllocEmojiSlot`）也确实存在。

### 5. S3-6 是误报；S3-3 的建议有害

- **S3-6 撤回**：`CreateMutexW(nullptr, TRUE, L"WinVClip_SingleInstance")`（`main.cpp:273`）
  名字没有 `Global\` 前缀，按 Win32 对象命名规则默认落在**会话**命名空间（等价于 `Local\`），
  两个 RDP 会话/快速用户切换本来就不会互相挡住。改成 `Local\` 是等价改写，不是修 bug。
- **S3-3 撤回**：`ui/actions.cpp:223` 的 `ClipItem it = g_items[idx];` 有注释说明理由
  （「对话框是模态的，历史可能在此期间被改动」）。改成持有引用/下标，正好在对话框打开期间
  被 `CaptureClipboard` 的 `push_front` 打成悬垂/错位——把一个瞬时内存峰值换成 UAF。
  **保持现状。**

---

## 三、核实后确认成立的清单（按修订后的优先级）

### A. 该修（S1）

**A1 `HistoryFlush` 不判事务结果仍释放长正文**（`core/history_store.cpp:585-604`）
完全属实：`Exec` 返回 void（`:63`）、`InsertItem` 的 bool 被忽略（`:597`）、
`COMMIT` 结果丢弃（`:601`）、`ReleaseLongTexts()` 无条件执行（`:603`）。
事务没提交时 >16KB 的正文既不在库也不在内存，`EnsureItemText` 也取不回来。
**这是全项目唯一确定会丢用户数据、且用户无从察觉的路径。**
修法同原报告（`Exec` 判 rc、失败 `ROLLBACK` 并保留正文、失败时记一次日志+一次性提示）。

**A2 emoji 预热镜像的生命周期不受锁保护**（`glyph/emoji_atlas.cpp:73-84, 176-178, 409-435`）
完全属实：镜像指针的分配/释放（`AllocEmoDisk`/`FreeEmoDisk`/`SaveEmojiCache` 末尾）都在
`g_emoMx` 之外，而 worker 在 `:176-178` **持锁** 往同一块内存 `memcpy`。
UI 线程在 `PumpEmojiUploads`（`:524`）里调 `SaveEmojiCache` → `VirtualFree`，与 worker 的写并发。
视口请求的 `EmoReq` 也带 slot（`:615`），所以「预热已全部上传」并不排除 worker 仍在写镜像。
修法：把镜像指针的读写与 `VirtualFree` 一并纳入 `g_emoMx`。

**A3 键盘钩子回调里同步执行会阻塞的动作**（`app/hook.cpp:93, 156`）
事实成立、理由按第二节第 2 条改写。目标形态：回调里只做内存判断 + 吞键 + `PostMessage`
（`WM_APP_HOTKEY` / `WM_APP_PANELKEY` 都已存在）。

### B. 该修（S2）

| # | 问题 | 位置（已核实） | 备注 |
|---|---|---|---|
| B1 | 关机/注销不落盘 | `main.cpp:100-228` 只有 `WM_DESTROY` | 加 `WM_QUERYENDSESSION` 里 flush |
| B2 | 库打不开完全静默 | `history_store.cpp:146-164`（失败 return false）→ `:586` 直接 return | 首次失败记日志 + 一次性 Toast |
| B3 | 迁移失败仍删 WAL | `history_store.cpp:613-627` | 见第二节第 3 条：前置是 `CheckpointDb` 也失败 |
| B4 | worker 失败态反复重建设备 | `emoji_atlas.cpp:154-158, 596` | 加 `g_emoWarmFailed`，失败态不再拉起 worker、不再投递 |
| B5 | `EndDraw` 返回值丢弃 | `render/dc.cpp:53` | 检 `D2DERR_RECREATE_TARGET` 重建；至少 `Trace` |
| B6 | UI 线程 `join` worker | `emoji_atlas.cpp:199`（由 `:535` 调） | 上限约 20ms（`wait_for` 周期）+ 一格渲染 |
| B7 | 剪贴板打不开无退避 | `capture.cpp:168` → `main.cpp:118-123` | 每次重试都 `BuildView+ComputeLayout+Redraw`，比单纯重试贵得多 |
| B8 | 菜单期间条目错位 | `panel.cpp:811` 存绝对下标；`actions.cpp:286` 直接用；`capture.cpp` 不检查 `g_menuOpen` | `paint.cpp:572` 有边界检查所以不崩，只是操作到隔壁条目 |
| **N1** | 落盘缓存含未渲染空洞 → 永久空白 | `emoji_atlas.cpp:524`（判定用 `g_emoUpN`）vs `emoji_atlas.cpp:395`（读回无条件 `ready=1`） | 见第四节 |

### C. 其余（S3，顺手做，别单独排期）

`g_view` 二重校验（`paint.cpp:255`、`actions.cpp:87/157`）、`swprintf` 判返回值
（`startup.cpp:570-577`）、`GlobalLock` 判空（`paste.cpp` 8 处，见第二节第 1 条）、
`WM_DPICHANGED`（`main.cpp:299`）、`ReadPngSize` 无符号化（`raster.cpp:172-173`）、
`DibToRgba` 校验像素区下界（`raster.cpp:78`）、`settings.ini` 原子写（`settings.cpp:159-162`）。

---

## 四、原报告漏掉的三条（都在表情页路径上）

### N1 落盘缓存可能含未渲染空洞，读回后那批表情永久空白 —— S2

`SaveEmojiCache` 的触发条件是 `g_emoUpN >= g_emoWarmSlots`（`emoji_atlas.cpp:524`），
但 `g_emoUpN` 统计的是**所有**成功上传的格（`:512`，含视口请求），而 `g_emoWarmSlots`
只统计预热任务（`:472`）。预热期间只要用户待在表情页滚动，视口请求的上传就会把
`g_emoUpN` 抬到阈值之上 → 提前落盘，此时镜像里还有若干预热格没渲染（全 0 像素）。

而落盘的索引是 `g_emoSlotKey` 里所有非空项（`:424-430`），包含那些没渲染的格；
读回时 `LoadEmojiCache` 对索引里每一项无条件写 `g_emoReady[slot]=1`（`:395`）——
于是下次启动这批格子被当作「已就绪」，永远显示空白，且不会重试。

**首次安装后第一次打开表情页**正是「预热在跑 + 用户在滚动」的组合，所以这不是窄窗口。
修法：落盘前加 `g_emoWarmIdx >= (int)g_emoWarmJobs.size()` 且所有预热格 ready 的判断；
或落盘时只写 ready 的格（索引与像素一起筛）。

### N2 `SaveEmojiCache` 先置成功标志再写盘 —— S3

`g_emoWarmSaved = true;`（`:410`）在 `_wfopen`（`:418`）**之前**执行，所以磁盘满或目录只读
导致落盘失败时，本会话内再也不会尝试写缓存。后果只是「下次启动重新光栅化」，不丢数据，但
属于同一类「失败之后没有出口」。

### N3 预热期间视口格子重复占槽 —— S3

投递逻辑（`emoji_atlas.cpp:601-616`）看到某 key 「已有槽但未 ready」时不复用该槽，
而是再 `AllocEmojiSlot()` 并覆盖 `g_emoSlot[key]`，旧槽的 key 仍留在 `g_emoSlotKey` 里。
同一格因此占两个槽，图集容量按 `emoTotal + 8%` 定（`:239`），在首轮预热窗口内可能被吃满，
槽用尽后 `post` 直接 return（`:611`）→ 新格子拿不到槽、不渲染。

---

## 五、修订后的修复顺序

| 批次 | 内容 | 判据 |
|---|---|---|
| **第 1 批** | A1（事务判结果）、B1（`WM_QUERYENDSESSION`）、B2（库失败可见） | 编译通过；用 `attrib +R history.db` 复现「>16KB 文本落盘失败后正文仍在、粘贴正常」，日志有一行 |
| **第 2 批** | A2（镜像纳入锁）、A3（钩子只投递）、N1（落盘判预热完成） | 表情页反复滚动 + Application Verifier 跑 10 分钟无报告；删掉 `emoji.cache` 重启一次，确认无空白格 |
| **第 3 批** | B3-B8、N2、N3 | 逐条按原报告验证法 |
| 第 4 批 | C 组（S3） | 顺手 |

**如果只有两小时**：只做 A1。原报告建议的「A1 + `GlobalLock` 判空」里的后者几乎不可能触发，
把这个时间花在 B1 上更划算（两行代码，直接消掉「复制完立刻关机丢最后一条」）。

---

## 六、原报告「不建议做」的清单，核实后同意

- 不换掉 SQLite/WAL：确认，问题全在「不判返回值」，不在 SQLite。
- 不给 `HistoryFlush` 加重试线程：确认。
- 不给 emoji worker 加线程池：确认，`emoji_atlas.cpp:267-271` 的注释与 `MEMORY.md` 的
  内存实测一致（每套设备 25~32MB）。
- 钩子回调里不做任何补偿式逻辑：确认，且核实后这条更该遵守——回调里连 `Trace` 都不该有
  （`hook.cpp:91` 在 Win+V 主路径上，`Trace` 每次 `fopen/fclose`，见 `core/diag.cpp:57-64`）。
  （单次开销约 0.1ms，不致命，但没有任何理由留在热路径上。）
