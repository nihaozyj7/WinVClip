# WinVClip 稳定性审查

审查对象：当前工作副本（拆分后的多文件结构，`src/**/*.cpp`）。
审查方向：**稳定性**（崩溃、数据丢失、卡死/挂起、慢性故障、故障可见性），不覆盖可维护性
（那部分见 `docs/CODE_REVIEW.md`）。
方法：精读 12 条关键路径（钩子、剪贴板采集/写入、历史库事务、emoji 光栅化 worker、
面板窗口过程、显示隐藏、UIA 插入符探测、退出清理、设置持久化、自启登记），
对每条跨线程/跨进程/跨进程状态共享点做「谁会失败、失败后状态是否一致」的推演。

---

## 一、结论摘要

| 级别 | 数量 | 一句话 |
|---|---|---|
| **S1（崩溃 / 数据丢失）** | 5 | 1 处跨线程 use-after-free、1 处事务失败仍丢正文、1 处钩子回调内做阻塞 I/O、2 处外部数据/文件操作未校验 |
| **S2（慢性故障 / 静默降级）** | 8 | 失败路径没有出口：要么无限重试，要么永久空白，要么什么都不告诉用户 |
| **S3（健壮性缺口）** | 8 | 单点未校验，靠「不变式由调用方维护」，概率低但一旦命中就是 UB |

**总体判断**：代码对「已知会发生的坏事」防守得相当好（WAL、脏区校验、缓存头校验、
超时压缩、魔数放行、对账式自启、逐格 CopyFromMemory 兜错位），
**系统性短板是「失败之后」**：绝大多数错误返回值被丢弃，于是故障不表现为报错，
而表现为「数据静默消失 / 界面永久空白 / 重试到把 CPU 吃满」——这是本章最值得投入的地方。

真正会**当场崩掉**的只有 S1-1（emoji 镜像与 worker 线程的竞态）和 S1-5（`GlobalLock` 未判空），
两者都在「另一条线程 / 系统内存紧张」的窄窗口里，日常难复现，但一旦复现就是 0xC0000005，
而且用户描述只会是「偶尔闪退」。

---

## 二、做得对的地方（改动时不要破坏）

- **历史库**：WAL + `synchronous=NORMAL`；搬库前先 `wal_checkpoint(TRUNCATE)`
  （`history_store.cpp:69`）；长正文只存库、提交后才释放内存（`history_store.cpp:309`
  `ReleaseLongTexts`）——**顺序是对的**，问题只在没判提交结果（见 S1-2）。
- **图集磁盘缓存**：头里带 magic/版本/尺寸/槽数，读回时逐项比对，任何一项不符就整体放弃
  （`emoji_atlas.cpp:343-405`）；即使整块 `CopyFromMemory` 返回 S_OK 也仍然逐格重写一遍
  （已知的软件 DCRenderTarget 错位）。被强杀留下的半截缓存文件只会被丢弃，不会崩。
- **外部进程调用都有上界**：UIA 连接超时压到 500ms（`caret.cpp:74`）、
  `ProcessIsSelfOrDescendant` 限深 6 层、像素数限 64M、`ReadAllBytes` 带 maxBytes。
- **钩子的克制**：只吞真要消费的键、修饰键整段放行、自己注入的键打 `dwExtraInfo` 魔数、
  鼠标钩子只 `PostMessage` 不同步干活（`hook.cpp:196-215`）——鼠标钩子这个写法是对的，
  键盘钩子没做到（S1-3）。
- **自启对账**：`ReconcileAutostart` 每次启动比对「实际形式 vs 应有形式」，只在真不一致时才写；
  任务 ACL 显式给当前用户完全控制，避免幽灵任务。故障可见性上还留了 `launch.log`。

---

## 三、问题清单

### S1 —— 崩溃与数据丢失

#### S1-1 emoji 预热镜像被 UI 线程释放、worker 线程正在写：use-after-free

**证据**
- worker 在锁内写镜像：`emoji_atlas.cpp:176-178`
  （`if (r.slot >= 0 && g_emoDisk && ...) memcpy(g_emoDisk + off, px.data(), ...)`，
  这段确实在 `g_emoMx` 的保护下）。
- **但镜像指针本身不受锁保护**：`FreeEmoDisk()`（`emoji_atlas.cpp:73`，`VirtualFree` + 置空）、
  `AllocEmoDisk()`（`:78`）、`SaveEmojiCache()` 的读盘与释放（`:409-431`，末尾 `FreeEmoDisk()`）
  全部在 `g_emoMx` 之外执行。

**触发条件**（窗口很窄但不是理论值）：预热跑到最后一格 → `PumpEmojiUploads` 判定
`g_emoUpN >= g_emoWarmSlots`（`:524`）→ `SaveEmojiCache()` 写 19.5MB 后 `VirtualFree`；
**与此同时**预热期间还有在途的视口请求（滚动过表情页就会有），worker 正持锁往同一块镜像里
`memcpy`。UI 释放 → worker 写已释放页 → 崩溃。

**后果**：进程直接消失（无崩溃日志、无弹窗）。用户侧表现是「翻表情页翻着翻着就没了」。

**修法**（任选，推荐 1）
1. 让镜像的生命周期也归 `g_emoMx` 管：`FreeEmoDisk()` / `AllocEmoDisk()` / `SaveEmojiCache()`
   里的指针读写与 `VirtualFree` 都放进 `lock_guard`；worker 侧已经是持锁 memcpy，改完即闭合。
2. 或者取消「worker 写镜像」：预热格子的像素 worker 本来就 `push_back` 进 `g_emoDone` 了，
   由 UI 线程在 `PumpEmojiUploads` 上传成功后顺手写镜像即可（少一次跨线程大块写，
   也顺手解掉「worker 持有一次性大缓冲」的内存问题）。

**验证**：`WINVCLIP_PERF=1` 启动后反复「进表情页→快速滚动→切分类」，同时用
`Application Verifier` 的 `Basics\Heap` 或 `gflags /p /enable winvclip.exe /full` 跑 10 分钟，
看是否报 use-after-free / 非法释放。

---

#### S1-2 `HistoryFlush` 事务失败仍然丢掉内存里的长正文 → 文本永久丢失

**证据**（`history_store.cpp:585-604`）
```cpp
Exec("BEGIN IMMEDIATE");          // 返回值被丢弃（Exec 本身返回 void，history_store.cpp:63）
DeleteMissingItems();
for (auto& it : g_items) {
    if (it.dbId <= 0) InsertItem(it);   // 返回 bool，调用处不看
    else              UpdateItem(it);   // void，内部 prepare/step 全是「失败就 return」
}
SaveRecent();
Exec("COMMIT");                   // 返回值被丢弃
ReleaseLongTexts();               // line 603：无条件把 >16KB 的正文从内存 swap 掉
```

**触发条件**：磁盘满（`SQLITE_FULL`）、库文件被杀软/同步盘短暂锁住（`SQLITE_BUSY/IOERR`）、
库损坏（`SQLITE_CORRUPT`）、`%TEMP%` 所在盘被拔掉。任何一步失败都会让事务不完整，
但后面三件事照做：不再重试、不报错、**把正文从内存放掉**。

**后果**：新采到的长文本条目（日志、代码块、大段 JSON）在这次 `HistoryFlush` 后变成空正文：
内存里没了（`ReleaseLongTexts`），库里也没写进去（事务没提交）→ `EnsureItemText`
从库里也取不回来 → 点粘贴得到空串，另存为得到 0 字节文件。**用户看不到任何提示**，
只会觉得「复制的东西过一会儿就没了」。

**修法**
1. `Exec` 改成返回 `bool`（`sqlite3_exec` 的 rc 判 `SQLITE_OK`）。
2. `HistoryFlush` 收集每一步的成功与否：`BEGIN` 成功才继续；中途失败 → `ROLLBACK` 并
   **直接 return false（不调用 `ReleaseLongTexts`）**；`COMMIT` 失败同样 return false。
3. `HistoryFlush` 返回 `bool`，`WM_TIMER/TIMER_PERSIST`（`main.cpp:158`）与 `WM_DESTROY`
   里失败时 `LogEvent` 记一次，并用 `Toast` 提示一次（同一故障只提示第一次，避免每 900ms 弹一次）。
4. 另外：`BEGIN IMMEDIATE` 失败时不要把它留下的隐式事务状态忘掉——加了 ROLLBACK 就一并解决。

**验证**：把 `history.db` 用 `attrib +R` 设为只读（或以另一个进程 `sqlite3 history.db "BEGIN EXCLUSIVE"` 占住），
复制一段 >16KB 的文本，等 1 秒后点该条目粘贴：现在会得到空串；修好后应得到原文（或至少
正文仍在内存里、粘贴正常），并且 `launch.log` 里有一行落盘失败记录。

---

#### S1-3 键盘钩子回调里做阻塞/跨进程操作，可能被系统静默卸载钩子

**证据**：低级键盘钩子跑在 UI 线程上，而它的回调里是**同步**执行动作的：
- `hook.cpp:93`：`ShowPanel()` / `HidePanel()` 直接在钩子回调里调用。
- `hook.cpp:156`：`SendMessageW(g_panel, WM_APP_PANELKEY, ...)` —— 同线程 `SendMessage` 就是
  直接函数调用，于是 `PanelKeyDown` → `PasteItem` / `HidePanel` / `RequestDeleteRow` 全部
  在钩子回调栈里跑完。

这条栈上会做的事：
| 动作 | 位置 | 潜在阻塞 |
|---|---|---|
| `OpenClipboard` + 逐条读 CF_UNICODETEXT/HDROP/DIB | `showhide.cpp:242` → `capture.cpp:165` | **别的进程开着剪贴板时可阻塞数百 ms 到数秒** |
| `SetForegroundWindow` + `AttachThreadInput` + `SetFocus` | `showhide.cpp:116-117`、`208-218` | `AttachThreadInput` 要等目标线程消息循环，目标无响应时无界 |
| `PutToClipboard`（Enter 粘贴） | `actions.cpp:87` → `paste.cpp:21` | 同上 OpenClipboard |
| `RequestDeleteRow` → `ClipboardHoldsItem` | `actions.cpp:337` → `capture.cpp:82` | 同上 |

**后果**：Windows 对低级钩子回调有时间预算（`LowLevelHooksTimeout`，超时后系统可以
**不再调用甚至卸载**这个钩子，且不通知程序）。表现是「用一段时间后 Win+V 时灵时不灵」、
「别的软件的全局热键反而还正常」，而程序自己完全不知道，日志里也没有任何东西。
这是本项目最难现场排查的一类故障，而且**打开 `WV_TRACE=1` 会让它更严重**——`Trace`
每次都 `fopen/fclose`（`diag.cpp:56-70`），而 `hook.cpp:88` 在主路径上有调用。

**修法**（最小改动即可闭合）
1. 钩子回调里**只做判断和吞键**，执行一律投递：
   `hook.cpp:93` → `PostMessageW(g_main, WM_APP_HOTKEY, 0, 0)`（`MainProc` 已有这个 case，
   `main.cpp:103`）；`hook.cpp:156` → `PostMessageW(g_panel, WM_APP_PANELKEY, ...)`。
   注意吞键语义不变：`return 1` 仍要在回调返回前同步做掉，只是「干活」延后到消息循环。
2. 若担心 `WM_APP_PANELKEY` 的时序（Esc 收起晚一帧），可以在 `WM_APP_PANELKEY` 处理前
   加一句 `ComputeLayout()`——现在 `PanelKeyDown` 里已经有隐式的布局依赖，
   Post 化之后这一点反而更明确。
3. `Trace` 改成「进程内缓冲 + 退出/满 4KB 时一次性落盘」，或至少在主路径上删掉
   `hook.cpp:88` 那条（每次 Win+V 都会命中）。

**验证**：写一个小脚本持有剪贴板（`OpenClipboard` 后不 `CloseClipboard`，sleep 3 秒），
在这 3 秒里按 Win+V：当前会看到热键「吃了不弹」且随后一段时间热键失灵；改后会立即弹面板
（只是这一次跳过采集）。再用 `hook_listen.py` 观察按键是否仍被别家钩子收到。

---

#### S1-4 迁移临时目录时忽略 `MoveFileW` 结果就删 WAL，可能丢掉最后一次已提交事务

**证据**（`history_store.cpp:613-630`）
```cpp
if (FileExists(oldDb)) {
    if (!FileExists(HistoryDbPath())) {
        CheckpointDb(oldDb);
        MoveFileW(oldDb.c_str(), HistoryDbPath().c_str());   // 返回值不看
    }
    DeleteFileW((oldDb + L"-wal").c_str());    // 无条件删 WAL
    DeleteFileW((oldDb + L"-shm").c_str());
    return;
}
```
而调用它的 `HistoryLoad`（`:500-505`）随后会 `WriteDirMark(HistoryDir())`，
`SettingsPickTempDir`（`actions.cpp:520`）随后还会 `HistoryFlush()`。

**触发条件**：跨卷迁移（`MoveFileW` 对跨卷是复制+删除，可能失败/中断）、旧库被另一个进程
（备份工具、杀软扫描、上一个没退干净的实例）短暂占用、目标路径已存在、磁盘满。

**后果**：旧库还在原处、WAL 被删（里面是最后若干个**已提交**事务）、dir-mark 已指向新目录 →
用户看到「改了临时目录，历史少了最近几条」，且新目录的库在下一次 flush 时以「内存里没有」
为由把库里多出来的行删掉（`DeleteMissingItems`），**旧数据被彻底遗忘**。

**修法**
1. `MoveFileW` 失败（或 `MoveFileExW` 带 `MOVEFILE_COPY_ALLOWED` 仍失败）→ **不要**删
   `-wal/-shm`，返回 false；调用方（`HistoryLoad` / `HistoryMigrateFrom`）看到失败就**不更新
   dir-mark**，并让 `SettingsPickTempDir` `Toast`「历史迁移失败，数据仍在原目录」。
2. `DeleteFileW(-wal)` 只在「移动确认成功」或「目标库已存在（老目录只是残留）」两个分支做。
3. 顺带把 `HistoryMigrateFrom` 的 `CloseDb()` 挪到失败路径上也要成对（现在失败也不重开，
   而是在 `HistoryLoad` 后续 `OpenDb` 时恢复，逻辑上能自愈，但状态是糊的）。

**验证**：把设置里的临时目录指向一个**只读**的磁盘/目录（或用 `icacls` 拒写），点「浏览」换过去：
当前会「看起来成功、历史空了」；修后应提示失败且历史仍在。

---

#### S1-5 `GlobalLock` 结果不判空就 `memcpy`（8 处，空指针写必崩）

**证据**：`paste.cpp:44-47`、`:58-61`、`:70-73`、`:106-109`、`:172-176`、`:186-190`、
`:196-199`、`:203-206` —— 全部是
```cpp
HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
if (h) { void* p = GlobalLock(h); memcpy(p, ..., bytes); ... }
```
`GlobalLock` 失败返回 `NULL`（句柄被别的进程锁住、内存紧张、或大块分配成功但锁定失败时）。

**触发条件**：写一张 32MB 的 CF_DIB 到剪贴板（4K 截图/Office 粘贴）在内存紧张时；
或剪贴板句柄被别的进程 `GlobalLock` 住。

**后果**：`memcpy` 到 NULL → 立即崩溃。注意这条路径**在 S1-3 的钩子栈上**（Enter 粘贴），
所以崩溃现场会出现在钩子回调里，看起来像「按回车偶尔闪退」。

**修法**：统一改成
```cpp
void* p = GlobalLock(h);
if (!p) { GlobalFree(h); CloseClipboard(); return; }   // 或 return false
```
（`PutToClipboard` 有 `EmptyClipboard` 在前，失败时剪贴板会被清空——这一点可以接受，
但要保证不崩；如果想更稳，把 `GlobalAlloc`+`memcpy`+`GlobalLock` 全部提前到
`OpenClipboard` 之前，即「先在私有堆里准备好 HGLOBAL，再开剪贴板」，这样打不开剪贴板
时也不会把用户原有内容清掉。这是更好的形态，改动也不大：把 `OpenClipboard/EmptyClipboard`
挪到所有 `GlobalAlloc` 之后即可。）

**验证**：`test_clip.py` 覆盖文本，另加一条：复制 4K 截图后按 Enter 粘贴，循环 50 次
（`Application Verifier` 打开 `Handles` 检查，能捕获 NULL 写）。

---

### S2 —— 慢性故障与静默降级

| # | 问题 | 证据 | 后果 | 修法 |
|---|---|---|---|---|
| S2-1 | worker 建不起来时槽位持续泄漏 + 8ms 忙循环 | `emoji_atlas.cpp:154-164`（`!ready` 只 `erase(pending)` 就 `continue`）；`PrepareEmojiBitmaps` 的 `post` 对「已有槽但未 ready」会再 `AllocEmojiSlot` 覆盖 `g_emoSlot[key]`（`:565-585`） | D2D 创建失败（远程桌面/软件渲染受限/内存紧张）时，每帧泄漏一个槽、槽满后每帧继续尝试并排 8ms 重绘 → CPU 占用长期偏高、表情永久空白 | worker 侧：`!ready` 时置 `g_emoWarmFailed`，之后 `PrepareEmojiBitmaps` 直接 return（不投递、不排定时器）；`EnsureEmojiWorker` 在 failed 状态不再拉起 |
| S2-2 | 关机/注销不落盘 | 无 `WM_QUERYENDSESSION` / `WM_ENDSESSION` 处理（`main.cpp:100-200` 只有 `WM_DESTROY`） | 复制后 900ms 内关机（防抖窗口 `TIMER_PERSIST`）→ 最后一条丢；`WM_DESTROY` 在关机流程里不一定及时 | `MainProc` 加 `case WM_QUERYENDSESSION: HistoryFlush(); return TRUE;` |
| S2-3 | 历史库打不开完全静默 | `OpenDb`（`history_store.cpp:146-176`）失败 → `HistoryFlush` 直接 `return`（`:586`）、`EnsureItemText` 返回 false | 库损坏/目录只读时，历史在内存里正常用、重启后全没，用户与日志都收不到任何信号 | 首次失败时 `LogEvent("db: open failed ...")` + 一次性 `Toast`（用静态 bool 保证只提示一次） |
| S2-4 | `EndDraw` 的 `D2DERR_RECREATE_TARGET` 被丢弃 | `dc.cpp:47-53` `PaintEnd()` 里 `g_rt->EndDraw();` 忽略返回值 | 设备失效后每一帧都画不出来，且永远不会自愈 → 面板变成一块死白区域 | `EndDraw` 返回值判 `D2DERR_RECREATE_TARGET` → `ReleaseD2D(); EnsureD2D(); InvalidateRect(...)`；至少也要 `Trace` 一行 |
| S2-5 | `StopEmojiWorker` 在 UI 线程 `join` | `emoji_atlas.cpp:524` → `:192-204`（`PumpEmojiUploads` 末尾） | worker 正在光栅化一帧时 `join` 会把 UI 线程（**也就是 `WH_MOUSE_LL` 回调所在的线程**）卡住数十 ms → 鼠标发钝——正是 `MEMORY.md` 里记录过的那类问题 | 改成「置 `g_emoQuit` 后不 join」，把 `join` 推迟到下一次 `EnsureEmojiWorker`（先收尸再起新线程），或干脆让 worker 永不退出、只用 `SetThreadPriority` 降优先级 |
| S2-6 | 配置文件非原子写 | `settings.cpp:144-160` `SettingsSave` 直接 `_wfopen(...,"wb")` 覆盖 | 写入中途断电/被杀 → `settings.ini` 空文件 → 设置全回默认；`tempdir` 丢失会让用户以为**历史没了**（数据还在旧目录） | 写 `settings.ini.tmp` → `FlushFileBuffers` → `MoveFileExW(..., MOVEFILE_REPLACE_EXISTING)`；`view-size.txt` 同理（影响小，可一并做） |
| S2-7 | 剪贴板打不开时无退避重试 | `capture.cpp:165` 失败 → `SetTimer(TIMER_CLIP, 160)`（`:166`） | 某个进程长期占着剪贴板 → 每 160ms 尝试一次、无上限、无日志，持续抢占（也持续占用钩子线程，见 S1-3） | 加尝试计数：>5 次就退到 1s 间隔，>10 次放弃本次采集并 `Trace` 一行 |
| S2-8 | 菜单打开期间 `g_items` 变动导致 `g_menuItem` 错位 | `panel.cpp:811` 记绝对下标；`TIMER_CLIP`→`CaptureClipboard` 会 `push_front`/`erase`（`capture.cpp:270-300`），且不检查 `g_menuOpen` | `PaintMenu` 有边界检查（`paint.cpp:572`）所以不崩，但菜单里「置顶/收藏」状态会对应到下一条 → 点「置顶」置错了条目 | `CaptureClipboard` 开头 `if (g_menuOpen) { g_menuOpen = false; }`（关菜单最简单），或存入 `dbId` 而不是下标，在 Paint/Run 时反查 |

---

### S3 —— 健壮性缺口（概率低，但命中即 UB）

| # | 问题 | 证据 | 建议 |
|---|---|---|---|
| S3-1 | `g_view` 元素当作 `g_items` 下标直接用，无第二重校验 | `paint.cpp:255`、`actions.cpp:87/157`、`paint.cpp:521` | 每个入口加一句 `if (idx < 0 \|\| idx >= (int)g_items.size()) return;`。这是 1 行的保险，抵掉整类「忘了 BuildView」的越界 |
| S3-2 | `LogLaunch` 的 `swprintf` 返回值未判 | `startup.cpp:561-580`（`wchar_t head[2048]`，塞 `GetCommandLineW()`） | 命令行超长时 `swprintf` 返回 -1、内容未定义 → `AppendLaunchLog` 里 `W2U8` 可能读过界。判返回值，失败就截断重写 |
| S3-3 | 另存图片时整份拷贝 `bits` | `actions.cpp:223` `ClipItem it = g_items[idx];` | 4K DIB 33MB 起，保存瞬间内存双份；改成只拷元数据，像素走 `EnsureItemBits` 返回的引用 |
| S3-4 | `Trace` 每次调用 `fopen/fclose` | `diag.cpp:56-70` | 见 S1-3：改成缓冲落盘；至少也别在钩子路径上调 |
| S3-5 | 无 `WM_DPICHANGED` 处理 | `main.cpp:295` 只 `SetProcessDPIAware()`，`g_dpi` 仅启动取一次 | 拖到另一块不同缩放的屏时文字/尺寸按旧 DPI 算（观感糊、布局跳）。要么声明 per-monitor v2 并处理 `WM_DPICHANGED`（重算 `g_dpi` + `MakeFonts` + `ComputeLayout`），要么在 README 里写明「不建议跨缩放屏拖动」 |
| S3-6 | 单实例互斥量用全局命名 | `main.cpp:265` `CreateMutexW(nullptr, TRUE, L"WinVClip_SingleInstance")` | 多会话（两个 RDP 会话/快速用户切换）下会互相挡住。改 `Local\\WinVClip_SingleInstance` |
| S3-7 | `ReadPngSize` 把 4 字节当有符号拼接 | `raster.cpp:186-192` | `b[16]<<24` 溢出成负数 → 图片标题显示负尺寸、排序异常。用 `unsigned` 移位再判上限 |
| S3-8 | `DibToRgba` 信任 DIB 头里的调色板尺寸 | `raster.cpp:88-98`（`StretchDIBits` 的源数据由 GDI 按头解析，只校验了 `off < dib.size()`） | 剪贴板内容来自**任意进程**，构造一个 `biClrUsed` 巨大的 DIB 可让 GDI 越界读源缓冲。加一条：`off + 必需的像素/调色板字节 > dib.size()` 就拒绝（按 `biBitCount` 与 `biSizeImage`/行宽推算下界） |

---

## 四、故障注入矩阵（当前行为 vs 期望行为）

| 外部故障 | 现在会怎样 | 期望 |
|---|---|---|
| 磁盘满（历史库写入） | 事务失败被忽略，长正文被释放 → **文本丢失且无提示** | 回滚、保留正文、提示一次、日志留痕（S1-2） |
| `history.db` 损坏 / 只读 / 被独占 | 静默降级为「只在内存里」，重启即空 | 提示 + 日志；最好把坏库改名备份（`history.db.corrupt-<时间>`）后重建 |
| `%TEMP%` 目录被删 / 盘符消失 | `EnsureDirW` 重建；重建失败 → `StoreImageCopy` 返回空 → 图片退化为只活在内存 | 可接受，但应 `Trace` 一次 |
| 改临时目录时迁移失败 | **旧 WAL 被删、mark 已改 → 最近事务丢失** | 不删 WAL、不改 mark、提示失败（S1-4） |
| 剪贴板被别的进程长期占用 | 每 160ms 重试、无日志；钩子路径上还会阻塞（S1-3） | 退避 + 放弃 + 日志（S2-7 + S1-3） |
| D2D/DWrite 创建失败 | 表情页面永久空白 + 槽位泄漏 + 8ms 忙循环 | 一次性失败标志、停止重试、退化成占位（S2-1） |
| D2D 设备中途失效 | 面板永久白屏，无自愈 | `EndDraw` 检 `D2DERR_RECREATE_TARGET` 并重建（S2-4） |
| 键盘钩子被系统卸载 | 程序无感知，热键永久失灵 | 定时 `GetLastInputInfo`/心跳自检，或至少在 `Trace` 里周期记录「钩子仍在」（可选） |
| 关机 / 注销 | 最后 900ms 内的条目可能丢 | `WM_QUERYENDSESSION` 里 flush（S2-2） |
| 任务管理器强杀 | 已提交事务安全（WAL）；未落盘的 900ms 窗口丢失 | 可接受 |
| 以管理员运行时普通权限脚本注入 | UIPI 挡住（已知） | 测试前把 `settings.ini` 的 `admin` 改 0 |
| 跨缩放屏拖动面板 | 尺寸/字体按旧 DPI | 见 S3-5 |

---

## 五、修复顺序（每步都能独立验证）

| 批次 | 内容 | 判据 |
|---|---|---|
| **第 1 批（当天可完成）** | S1-5 `GlobalLock` 判空（8 处，机械改动）、S1-2 `HistoryFlush` 判事务结果、S2-2 `WM_QUERYENDSESSION`、S3-1 索引二重校验 | `cmake --build build` 通过；`tests/run.py` 的 `test_clip` / `test_persist` / `test_clear` 全绿；手动做一次「只读 db + 16KB 文本」验证 |
| **第 2 批** | S1-1 镜像指针纳入锁、S1-3 钩子回调只投递、S1-4 迁移失败不删 WAL | 表情页反复滚动 + Application Verifier 跑 10 分钟无报告；持剪贴板 3 秒期间 Win+V 仍能弹出 |
| **第 3 批** | S2-1 失败标志、S2-4 `EndDraw` 自愈、S2-5 worker 不 join、S2-6 配置原子写 | 断网/降配置（远程桌面里跑）时表情页不占 CPU；`settings.ini` 改为 tmp+rename 后可复现性验证 |
| **第 4 批** | 其余 S2/S3 | 顺手做，不单独排期 |

**如果只有两小时**：只做 S1-2 和 S1-5 —— 一个是静默数据丢失，一个是必然的崩溃路径，
两者都是纯局部改动、不触碰并发与窗口模型。

---

## 六、明确不建议做的（避免为「稳定」引入新风险）

- **不要**把历史库换成「每次改动全量重写」或自写二进制格式：WAL 已经是这套需求下最省心的方案，
  现在的所有问题都在「不判返回值」，不在 SQLite 本身。
- **不要**为了 `HistoryFlush` 的失败重试引入后台线程：单实例桌面程序，重试放在
  `TIMER_PERSIST` 的下一次触发即可，加线程只会把一个简单问题变成两个。
- **不要**给 emoji worker 加线程池「预防卡顿」：每套 D2D 设备 25~32MB，
  当前 1 个 worker 的取舍是对的（见 `emoji_atlas.cpp:245-255` 的注释），本次只修生命周期不修并行度。
- **不要**在钩子回调里加任何「补偿式」逻辑（重试、日志落盘、弹窗）——那条路径的唯一正确形态是
  「只做内存判断 + 吞键 + PostMessage」。
