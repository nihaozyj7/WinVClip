# WinVClip 实现笔记

这里放的是「为什么代码必须这样写」——每一条都是当场踩出来的，同一处再改一遍就会复现同一个 bug。
入口文档见 [README](../README.md)，模块划分见 [ARCHITECTURE](ARCHITECTURE.md)。

改下面这些文件前，先读对应小节：

| 要动的东西 | 先读 |
|---|---|
| `app/hook.cpp` | [1](#1-接管-winv全局键盘钩子)、[2](#2-焦点模型面板从不抢焦点) |
| `ui/showhide.cpp`、`ui/panel.cpp` | [2](#2-焦点模型面板从不抢焦点) |
| `clipboard/paste.cpp`、`ui/actions.cpp` | [4](#4-粘贴与插入两条通道) |
| `ui/caret.cpp` | [5](#5-面板落点插入符探测) |
| `render/*`、`ui/paint.cpp` | [6](#6-渲染为什么走-d2d) |
| `glyph/emoji_atlas.cpp` | [9](#9-字形库与渲染性能) |
| `ui/layout.cpp`、`ui/panel.cpp` 的拖动/缩放 | [8](#8-面板交互拖动缩放固定右键菜单) |

---

## 1. 接管 Win+V：全局键盘钩子

`RegisterHotKey(MOD_WIN, 'V')` 在系统「剪贴板历史」开启时必然返回
`ERROR_HOTKEY_ALREADY_REGISTERED`，所以走 `WH_KEYBOARD_LL`，在 `LLKeyboard()` 里判
「Win 按住 + V 按下」后 `return 1` 吞掉。

### 1.1 Win 键的抬起不能吞

早期为了抑制开始菜单，把 V 和 Win 的 keyup 一起吞了。结果系统键盘状态里 Win 永远按着：
`SendInput` 的 Ctrl+V 变成 Win+Ctrl+V（粘不动）、`SetForegroundWindow` 开始被前台锁拒、
按 Enter 弹出 Win11 快速设置、`GetAsyncKeyState(VK_LWIN)` 全程返回按下。

正解（PowerToys 同款）：吞掉 V 之后**注入一个无害键 `VK_NONAME`(0xFC)**。
Explorer 见到「Win 按下期间有其他键」，松开 Win 时不弹开始菜单；同时 Win 的抬起正常送达系统。

### 1.2 长按会触发自动重复，而这里是 toggle

按住 Win+V 不放，500ms 后系统以约 30ms 间隔补发 V 的 keydown，每次都会 toggle → 面板高速闪烁。
`KBDLLHOOKSTRUCT::flags` **没有**「重复键」标志位（只有 EXTENDED / INJECTED / ALTDOWN / UP /
LOWER_IL_INJECTED），只能自己维护状态：

```cpp
DWORD now = GetTickCount();
bool repeat = g_vDown && (now - g_vDownAt) <= 700;   // 上一次 keyup 之前的所有 down 都是重复
g_vDown = true; g_vDownAt = now;
if (!repeat) { /* 只有这里才 toggle */ }
```

- 重复键仍然吞掉（否则 `v` 漏进目标窗口），也仍然注入 `VK_NONAME`。
- 700ms 的两个作用：大于系统最慢重复速率（约 400ms/次）；并且当某次 keyup 丢了、`g_vDown`
  卡在 true 时，下一次按键间隔必然 >700ms，会被当成新的按下，不会变成「Win+V 彻底失灵」。
- `Win` 的 keyup 也清 `g_vDown`（UAC 安全桌面等场景 keyup 会丢）。

回归：`python tests\test_longpress.py hold 2`（期间 66 个重复键，可见性只应变 1 次）、
`double`（Win 全程按住时两次长按能正常开关）、`seq`（快速三连按依次翻转）。

### 1.3 接管范围必须窄，否则饿死别家的全局热键

`WH_KEYBOARD_LL` 是一条**串行链**：回调里 `return 1` 不只挡住目标窗口，还会让排在后面的
所有低级键盘钩子（AutoHotkey、Quicker、输入法热键、各种快捷键工具）收不到这次按键。
用户机器上这些工具通常先开机启动、排在链的后面，于是「面板一弹出，别家快捷键全哑」，
而点一下搜索框（面板拿到真前台，钩子不再介入）立刻恢复——这条线索直接指向接管范围。

非激活态只接 `PanelConsumesBareKey()` 判定为「面板确实会消费」的键：

| 接管 | Esc、Tab；列表页的 Up/Down/Prior/Next/Delete/Enter；弹层里的 Enter/Up/Down；Backspace（仅搜索串非空）；能打出可见字符的键（非激活态直接打字进搜索框，这是核心体验） |
|---|---|
| 放行 | 所有带 Ctrl/Alt/Win 的组合、功能键、媒体键、PrintScreen、Home/End/Insert、当前页面不处理的导航键 |

代价写清楚：`Ctrl+1~4 / P / D / S` 在非激活态不生效，只能点了搜索框进输入态再用（输入态由系统
`WM_KEYDOWN` 直达面板）。换来的是 `Ctrl+S` / `Ctrl+F` / `Ctrl+C` 不再被面板抢走。

`KEYEVENTF_UNICODE` 的注入事件要单独认：系统把 `vkCode` 填成 `VK_PACKET`(0xE7)（个别路径是 0），
码元在 `scanCode` 里，真人键盘不会产生它——不认的话自动化脚本往面板里打的字会漏进目标窗口。

回归：`python tests\test_hook_passthrough.py`（先起 `hook_listen.py` 装一个外部钩子、再重启本程序，
让本程序的钩子排在它前面，等价于用户机器上的顺序）。

---

## 2. 焦点模型：面板从不抢焦点

这是整个方案的地基。早期走「抢前台」路线（`AttachThreadInput` → `SetForegroundWindow` →
`SetActiveWindow` → `SetFocus`），根本毛病是**键盘焦点是单一资源**：面板一拿到，目标窗口立刻
收到 `WM_KILLFOCUS` 并 `DestroyCaret`，记事本的光标就没了。围绕这个洞打的补丁（影子光标、
粘贴后恢复两层焦点、为地址栏单独调一轮）**换一个软件就歇菜**——每个目标的控件级焦点恢复行为都不同。

现在的结构同系统 Win+V：

- 面板带 `WS_EX_NOACTIVATE`，显示走 `SetWindowPos(..., SWP_NOACTIVATE | SWP_SHOWWINDOW)`，
  从不 `SetForegroundWindow` / `SetFocus`；`WM_MOUSEACTIVATE` 返回 `MA_NOACTIVATE`，
  鼠标点面板也不会激活它。
- 目标窗口一直持有键盘焦点，插入符一直闪——不是替身，是真的。
- 面板要用的按键由已有的低级钩子旁路转发：
  `SendMessage(g_panel, WM_APP_PANELKEY, vk, MAKELPARAM(ctrl, ch))`。同线程 `SendMessage`
  就是一次直接调用，没有跨线程等待，不会拖慢输入。

三个实测细节：

1. **修饰键必须放行，不能吞**（症状见 [1.1](#11-win-键的抬起不能吞)）。
2. **修饰键状态要自己跟踪**：钩子吞键后系统不更新全局键盘状态，`GetAsyncKeyState` / `GetKeyState`
   在回调里读到的是「没按下」，`Ctrl+1` 会被当成字符 `1` 打进搜索框。用 `g_ctrlDown` /
   `g_shiftDown` 自己记。
3. **自己注入的键要打标记**：`SendInput` 出去的键带 `dwExtraInfo = 'WINV'`，钩子据此放行；
   其他来源的注入照常转发，与真人按键一致。固定态连插表情时自己注入的字符才不会被自己的钩子吞掉，
   自动化测试也才驱动得了面板。

### 2.1 唯一会激活面板的入口：点搜索框

中文搜索靠输入法，而输入法组合**必须有真正的键盘焦点**，绕不过去。所以搜索框留一个显式切换：

- 平时（非激活态）英文/数字直接进搜索框，目标窗口光标一直闪；
- 点一下搜索框才把前台交给面板（输入态），输入法可用，目标窗口光标暂时消失；
- `Esc` 或关面板时 `HidePanel` 把前台和控件级焦点一起还给目标窗口；
- 面板已隐藏就不再响应这类激活请求（`IsWindowVisible` 守住），否则前台会停在一个不可见窗口上，
  而隐藏后定时器已停，没人再把它收回来。

### 2.2 收起判据：看前台还是不是那个目标窗口

非激活态下面板从来不会被激活，**收不到 `WA_INACTIVE`**，所以「用户有没有切走」只能靠 120ms 的
`TIMER_FOCUS` 轮询 `GetForegroundWindow()`：前台 == 面板（输入态）或 == 唤起时的目标窗口 → 留着；
其他 → `HidePanel()`。

这条判据取代了早期「唤起后 500ms 内允许重新抢前台」的抢救逻辑：抢成功后面板会重新激活，
之后再没有失活事件，反而会永久挂在最上层。

### 2.3 前台判据的盲区：点面板外部即收起

面板从不夺取前台，唤起时前台就是目标窗口。用户在**同一个窗口**里点掉面板（在浏览器里点正文、
在记事本里点正文接着打字）——前台一动不动，轮询永不触发，面板僵在屏幕上。

系统 Win+V 没这毛病，因为它自己是激活窗口，点哪都先丢一个 `WA_INACTIVE`。我们坚持不抢焦点，
就只能自己盯鼠标：额外装一个 `WH_MOUSE_LL`（`LLMouse`），**按下的点落在面板矩形外就收起**，
并且不吞这次点击。三条细节：

- **点自己的窗口不算「外面」**：另存为对话框、消息框都是本进程窗口，不排除的话一开对话框面板就被自己收掉。
- **点别的应用时不要抢先隐藏**：输入态下面板自己是前台，若抢在那次点击之前 `SW_HIDE`，系统会把前台
  交给 Z 序下一个窗口（实测交给了上一个记事本），用户点的窗口反而因前台锁抢不到前台。这种情况交给
  轮询去收（120ms 内），那时前台已不是自己，不会走「还前台」分支。
- **`WA_INACTIVE` 里也一样**：收到它时前台常常还没落定（`GetForegroundWindow()` 仍返回面板），
  立刻判会走「还前台」把用户正点的窗口挤掉，所以先等一个 120ms 的 `TIMER_DEFERHIDE` 再判；
  `g_hideNoRestore` 在这条路上把 `HidePanel` 的归还动作堵掉。
- **固定态不受影响**：`g_pinned` 时钩子直接跳过。

两条判据互补：前者管切走整个应用，后者管同一窗口内的点击。

回归：`tests\test_hide_outside.py`（五种场景全覆盖）、`tests\test_edge_outside.py`（Edge 真实场景）。

### 2.4 影子光标（已删除）

记下来是为了说明「为什么当初走了弯路」：真插入符属于**拥有焦点的线程**，目标一收到 `WM_KILLFOCUS`
就 `DestroyCaret`（记事本 `hwndCaret` 直接变 0），而 `ShowCaret` 是每输入队列的资源，外部替别的线程
点亮不了。当时只能画一根替身窗口（`WS_EX_TOOLWINDOW|NOACTIVATE|TRANSPARENT|LAYERED`，530ms 切 alpha），
还要处理「失焦后读不到新位置」「固定态插入后靠 `TIMER_REPIN` 抢回前台前先量一次」等细节。

教训：影子光标是在为一个自己制造的问题做补救。现在配套代码（`TIMER_GHOST` / `TIMER_REPIN` /
`GhostProc` 窗口类）全部删除，**不要以任何形式恢复抢焦点**。

---

## 3. 剪贴板采集

`AddClipboardFormatListener` + `WM_CLIPBOARDUPDATE`。它是发给**顶层窗口**的广播，
`HWND_MESSAGE` 的消息窗口收不到——早期用消息窗口，历史永远是空的。用隐藏的顶层 `WS_POPUP` 窗口。

收到通知后不要立刻读，剪贴板常被别的进程占着：设 160ms 定时器再读，`OpenClipboard` 失败就继续重试。
定时器 ID 不能和窗口消息值撞。

---

## 4. 粘贴与插入：两条通道

### 4.1 粘贴回原窗口

1. 弹面板前记下 `GetForegroundWindow()` 作为粘贴目标；
2. 选中条目 → 隐藏面板 → 写回系统剪贴板（`CF_UNICODETEXT` / `CF_HDROP` / `PNG`|`CF_DIB`）；
3. 非激活态下目标本来就在前台，直接起 90ms 定时器（定时器里仍断言 `GetForegroundWindow() == 目标`）；
   只有输入态需要先 `ForceForeground` 交还前台；
4. 定时器里 `SendInput` 发 Ctrl+V（抢不到前台就不发）。

同步直接发 Ctrl+V 会粘到旧窗口，这是必须分两步的原因。

### 4.2 点表情/符号：按「能不能拿到插入符」分流

| 目标 | 通道 | 剪贴板 |
|---|---|---|
| 有文本插入符（记事本、Edit/RichEdit、Office、绝大多数原生输入框） | `SendInput` 的 `KEYEVENTF_UNICODE` 逐码元直插 | **完全不碰** |
| 取不到插入符（Chromium/Electron 页面、Edge 地址栏、UWP、终端、shell 列表） | 借剪贴板 + `SendInput` 发 `Ctrl+V`，粘完还原 | 短暂借用后还原 |

这条判据仍然只看 `GetGUIThreadInfo`（`CaretRectOf`），**故意不用 UIA**：UIA 能读到浏览器里的插入符
是为了定位面板，而 Chromium 的渲染输入框接受 `KEYEVENTF_UNICODE` 直插并不可靠（组合输入、受控输入框
会吞字符），粘贴通道才是稳的。**定位通道与注入通道分开判定，别再合并。**

#### 为什么不再用「有没有插入符」判断「能不能插入」

- 早期一律直插。资源管理器文件列表没有文本输入区，注入的字符被 Shell 当成「键入查找」匹配文件名，
  匹配不到就**叮一声系统提示音**，什么都没插进去。
- 于是加了「没有插入符就拒绝插入」，又把太多东西一起拒了：**Chromium 的插入符在渲染进程里**，
  `GetGUIThreadInfo` 取不到，Edge 地址栏、网页输入框全被判成「不接受文本输入」，用户看到的是
  「系统自带面板能粘，这个面板点表情没反应」。
- 结论：**取不到插入符 ≠ 不能粘**，分通道而不是分能否。文件列表收到 `Ctrl+V` 对文本剪贴板是
  无操作、不响铃，比直插安全。

#### 借用剪贴板的细节

- 备份当前剪贴板（`CF_UNICODETEXT` / `CF_HDROP` / PNG / `CF_DIB`，图片 32MB 上限）。
- 写完立即记 `GetClipboardSequenceNumber()`。还原前再比一次：**序列号变了就放弃还原**，
  绝不覆盖用户或别的程序刚复制的新内容。
- 粘贴后 600ms 才还原：部分程序异步读剪贴板，还原太早会粘到旧内容。面板隐藏（`Esc`、取消固定、切走）
  时也会立刻归还。
- 借用期间写入的内容**不进历史**：`CaptureClipboard` 开头比序列号，是自家写的就跳过。
- 连续点多个字形只备份一次，不会把上一次的临时内容当成「原剪贴板」。

#### 手动兜底与实现要点

- **按住 Shift 点格子 = 强制走剪贴板粘贴通道**（注入无效时的手动兜底，对所有目标最兼容）。
- 代理对（emoji）按 UTF-16 码元逐个发，目标侧 `WM_CHAR` 自己合成；ZWJ 组合序列（👨‍👩‍👧）同样逐码元。
- 注入前同样「先隐藏面板 → 90ms 定时器」，定时器里再判：`g_typePending` 非空走注入，否则走 `Ctrl+V`
  （历史页/收藏页含图片和文件，只能走剪贴板）。
- 固定态下不隐藏面板，焦点本来就在目标窗口，直接连点。
- **没有可插入的目标窗口时**（在桌面上唤起面板）退回复制到剪贴板并提示，否则「点了没反应」更让人困惑。
- 收藏页仍走剪贴板粘贴：收藏的可能是图片/文件，注入对它们没有意义。
- 注入前要确认目标窗口真的在前台。输入态需 `ForceForeground` 并在注入前断言 `GetForegroundWindow`。
- 限制：目标进程以管理员权限运行时注入被 UIPI 挡住（粘贴通道也一样）；IME 正在组字时注入的字符
  可能被输入法吞掉，此时用 Shift+点击。

---

## 5. 面板落点：插入符探测

### 5.1 原生窗口：`GetGUIThreadInfo`

`rcCaret` 就是插入符矩形，但它的**坐标基准在文档与实测之间不一致**（一部分窗口给客户区坐标），
所以拿插入符所在窗口的屏幕矩形做判别：先 `ClientToScreen` 转一次，结果落在窗口矩形内就采用转换值，
否则用原值；两者都不在窗口内当作没有插入符。不这么判面板会跑到屏幕外。

`GetGUIThreadInfo` 对**更高权限进程**的窗口会失败（UIPI），这时按「没有插入符」处理、面板居中。

### 5.2 Chromium / Electron：走 UIA

它们的插入符在渲染进程里，`hwndCaret` 恒为 0（光标在闪但读不到），第一版因此在这些窗口里一律
退回屏幕居中。正解是 UI Automation 文本模式：`GetFocusedElement` → `TextPattern2::GetCaretRange`
（没有就退到 `TextPattern::GetSelection`）→ `IUIAutomationTextRange::GetBoundingRectangles`，
每 4 个 `double` 是 `(left, top, width, height)` 的屏幕物理像素。

三个坑：

- **归属判定按进程树，不是 pid 相等**：Chromium 把页面放渲染子进程，Electron 同理。早期按 pid 严格
  判等，结果地址栏（浏览器进程）能过、**网页输入框一律被静默拒掉**。现在认「同进程 或 目标进程的子孙
  进程」（`CreateToolhelp32Snapshot` 沿父链上溯，最多 6 层），面板自己、输入法候选窗仍被挡在外面；
  拒绝时 trace 留一行 `uia: foreign focus elpid=… targetpid=…`，不再静默。
- **可访问性树是懒建的**：Chromium 只在有客户端询问时才建 a11y 树，刚唤醒的头一两次可能拿不到
  `TextPattern` 或只回整框。所以首轮失败或只拿到近似时，先按近似/居中显示，随后 `TIMER_REPROBE`
  补探两次（140ms / 160ms），成功就只挪位置（`SWP_NOACTIVATE`）；用户自己拖过面板（`g_posUserSet`）
  就不再挪。
- **`GetSelection` 兜底只对文本控件类型生效**：Chromium 里 input/textarea 是 `Edit`、contenteditable
  是 `Group`；焦点落在页面本身时元素是 `Document`，它的 `TextPattern` 是整页文本视图，
  `GetSelection` 返回的是页面里当前被跟踪/选中的内容，与插入符无关（实测 bilibili 视频页给回
  `46x46` 方块）。所以 `Document` 只接受 `TextPattern2::GetCaretRange(active=TRUE)`；
  列表项/按钮这类即便暴露了 `TextPattern` 也不用。

代价与取舍：首次访问 Chromium 会唤醒它的 a11y 树（首次 ~15ms，冷启动可上百毫秒），所以只在
`GetGUIThreadInfo` 失败时才走这条路，并用 `IUIAutomation2::put_ConnectionTimeout(500)` 把跨进程卡死
压到半秒。

### 5.3 Chromium 不提供 `TextPattern2`，空文本框只回「整框」

`GetCurrentPatternAs(UIA_TextPattern2Id)` 在 Chromium 上一律失败（`GetCaretRange` 这条最准的通道走不到），
只能用 `GetSelection` 的退化范围当插入符；而这个范围在**内容为空**时返回的是**控件整体矩形**
（WebView2 聊天框 `931x46`、空 textarea `660x300`，真实插入符是 `1x17`）。采信它，落点算法就以为要避让的
光标行有整块控件那么大，面板被甩到框外很远。两道防线：

- `CaretRectIsBox`：插入符矩形与焦点元素框面积占比 ≥8 成、或宽高都与框一致 → 判假并丢弃，trace 留 `bad=1`；
- `ApproxCaretInBox`：丢弃后若框本身可信（过 `BoxSaneRect`），退到「框内首行起始处」（左内缩一个 padding、
  行高 20dp）当插入符，面板贴框外弹出；框不可信（整窗级编辑器）才放弃定位、居中。

`CaretInfo.approx=true` 时先照近似位置弹出，`TIMER_REPROBE` 再补探；另外 `ProbeCaret` 里做了一次
**同步重试**——Chromium 第一次被问常常只回整框或退化矩形，紧接第二次就精确了，这样面板不必先跳一下。
实测：WebView2 空输入框 `bad=1 approx=1`（框外下方）、打字后 `bad=0 approx=0`（贴真插入符行）；
Edge 地址栏 `caret=(1x16)`、大 textarea `1x24`。

### 5.4 落点怎么算（不遮挡输入框）

只做「下方放不下就翻上方」不够：翻上去夹进工作区后常正好压在输入框上（面板高 620、屏幕 1440、
输入框在中部时很常见）。现在的做法：

1. 取一个**避让矩形** `avoid`：优先焦点控件整体矩形（原生 = 焦点控件窗口矩形，UIA = 焦点元素
   `CurrentBoundingRectangle`），不可信时退化成插入符那一行。
2. 生成 6 个候选落点：插入符下方 / 上方 / 右侧 / 左侧 / 贴屏幕底 / 贴屏幕顶，每个先夹进工作区。
3. 逐个打分：**不遮挡 `avoid` > 完整落在工作区内 > 遮挡面积更小**，同分取先出现的（默认仍是下方）。

`avoid` 的可信度由 `BoxSaneRect` 把关：太高（超过工作区 2/3）、又宽又高、或与目标窗口面积占比 ≥6 成的
一律不认——Win11 记事本的焦点控件就是整个客户区，认了面板会被摆到窗口下方空白里，离插入符更远。
无解时退化成「遮挡面积最小」，保证插入符那一行始终可见。

两条通道都拿不到插入符时（桌面、文件列表、无输入框的窗口）居中于鼠标所在显示器。每次 Win+V 都重算，
拖过的位置只对本次显示有效。

---

## 6. 渲染：为什么走 D2D

GDI 只负责最后 `BitBlt` 到窗口，画面全部画在 Direct2D + DirectWrite 上，起因是纯 GDI 处理不好这个画面：

- **GDI 的 `RoundRect` / `LineTo` / `Ellipse` / `Polygon` 完全不做抗锯齿**：1:1 下圆角和图标线条就是阶梯。
  改 `ID2D1PathGeometry` + `DrawGeometry`（`PER_PRIMITIVE`），笔触 `D2D1_CAP_STYLE_ROUND` /
  `D2D1_LINE_JOIN_ROUND`；线宽不取整（`stroke * size/24` 给浮点），小图标才不糊。
- **`ANTIALIASED_QUALITY` 的小字号中文发虚，ClearType 又有彩边**，这个二选一在 GDI 里绕不出来。
  DWrite 的 `D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE` 又锐利又干净。文字测量也换成
  `IDWriteTextLayout::GetMetrics`：**量宽和绘制必须同一个引擎**，GDI 量、D2D 画会差 1~2px，
  页签和胶囊宽度就错位。
- **`ID2D1DCRenderTarget` 只认 32bpp B8G8R8A8 目标**，`CreateCompatibleBitmap` 的 DDB 在部分环境绑不上。
  手建 `CreateDIBSection`（`biBitCount=32`，`biHeight` 取负表示自上而下）。
- alpha mode 选 `D2D1_ALPHA_MODE_IGNORE`：ClearType 需要不透明目标，`PREMULTIPLIED` 会让文字出脏边。
  半透明遮罩不受影响（源 alpha 照常参与 source-over），一行 `FillRectangle` 就够。
- **`BindDC` 会把 DPI 变成 DC 的**，绑定后必须 `SetDpi(96,96)` 才保证 1 DIP = 1px；坐标原点是绑定矩形
  左上角（绑整窗时正好 (0,0)）。
- 一帧只在 `BeginDraw`/`EndDraw` 之间走一遍：滚动区用 `PushAxisAlignedClip` 裁，彩色 emoji 同批绘制
  （`D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT`——`Segoe UI Emoji` 是 COLR/CPAL 彩色字体，这是它出彩色的
  唯一途径，GDI 只画单色轮廓）。
- 1px 外框画在 `RectF(0.5, 0.5, W-0.5, H-0.5)` 上，否则 1px 线跨两个像素糊成 2px 灰线。
- **齿轮这类「并集再挖孔」别用 `CombineRgn`**：路径 `SetFillMode(WINDING)`，外圆与齿圆同向、轴孔反向绕序，
  一次填充就是正确镂空。但见下条，本项目图标一律描边。
- **`FillGeometry` 在 `DCRenderTarget` 上不渲染**：齿轮（并集挖孔）和 toast 的「⋯」用填充路径完全不可见，
  同一函数里走 `DrawGeometry` 的图标都正常。所以图标一律描边；真要实心图形用 `FillRoundedRectangle` /
  `FillRectangle`。
- **MinGW 的 `swprintf` 是 C99 语义**：宽字符版本里 `%s` 当窄串、`%lld` 也不认，混用输出乱码。
  中文字符串一律 `std::wstring` 拼接，不要格式化。
- 圆角靠 `SetWindowRgn`；`CS_DROPSHADOW` 提供系统投影。

---

## 7. 列表行排版与缩略图

排版：每行两段文字，上行标题（代码条目换等宽字体），下行左右对齐——左 `类型 · 来源：窗口标题`，
右 `大小 - 相对时间`（如 `650KB - 9分钟前`）。右侧先按 `g_fMeta` 量宽再从行右缘反推起点，左侧右边界取
该起点减 8px，超长 `DT_END_ELLIPSIS`，这样右侧永远齐。大小按内容实际占用算：文本取 **UTF-8 字节数**
（不是 `wstring::size()`，中文会少算一半以上）、文件取磁盘文件之和（目录记 0）、图片取位图缓冲字节数；
默认单位 KB，四舍五入到 1000KB 以上才进位 MB（`665696 → 650KB`、`1024000 → 1.0MB`）。

缩略图：图片条目不画类型图标，而是在 28px 图标块的位置画 36px 缩略图（按宽高比 letterbox 居中，
3840×2160 出来是 36×20）。解码两条路：

- **PNG**（注册格式 `"PNG"`）→ WIC：`IWICStream::InitializeFromMemory` + 解码器 +
  `IWICFormatConverter` 统一成 `32bppBGRA`。
- **`CF_DIB`** → GDI `StretchDIBits` 以 1:1 画进 32bpp DIBSection，免费拿到 1/4/8/24/32bpp 全部位深、
  调色板、上下翻转的转换，不用自己解析 `BITMAPINFOHEADER`（只需算准像素起始偏移：`biSize` +
  BI_BITFIELDS 的 3 个 mask + 调色板）。

然后自己盒式下采样（按面积把源像素归到目标桶，一遍扫完）+ 圆角抗锯齿：用圆角矩形有符号距离场算像素
覆盖率，乘进 alpha 后**预乘** RGB，`CreateBitmap` 用 `D2D1_ALPHA_MODE_PREMULTIPLIED`，source-over 就把
圆角平滑混到列表底色上。三个刻意选择：

- 不走 `PushLayer` + 几何遮罩做圆角——**DC render target 不支持 layer**，CPU 端预乘是这条链上唯一
  能拿到平滑圆角的路。
- 缩略图在 `PaintBegin` **之前**批量构建（`CreateBitmap` 是建设备资源的操作，塞进 `BeginDraw`/`EndDraw`
  会触发目标内部 flush）。
- 缓存挂在 `ClipItem::thumb` 上是 `shared_ptr`：去重/复制会整条拷贝，裸指针会被重复释放。解码失败也留
  一个空 `Thumb`，避免每帧重试。

限制：4K 图首次显示要解码 + 全图累积（几十毫秒），只付一次；`bits` 仍保留原图，内存没因缩略图下降。

---

## 8. 面板交互：拖动、缩放、固定、右键菜单

### 8.1 拖动与缩放

- **拖动用自己写的手动拖拽**（`WM_LBUTTONDOWN` 记屏幕坐标 + `SetCapture`，`WM_MOUSEMOVE` 按差值
  `SetWindowPos`），**不用** `WM_NCHITTEST` 返回 `HTCAPTION`：后者走系统模态移动循环，而「失焦即隐藏」
  的窗口一进那个循环就被判失活、面板自己收起，拖动等于没发生。顺手把标题栏空白处的指针换成
  `IDC_SIZEALL`（`WM_SETCURSOR`）。
- 标题栏里**页签、图钉、齿轮、关闭四个热点仍走客户区**，只有真正的空白处才开始拖动。
- 右上三个按钮与标题栏等高（`y = 0..HEAD_H`）、宽 30px、紧邻，关闭按钮右缘即窗口右缘（间距 0），
  图标 `CenterIcon` 居中，不靠 `InflateRect` 推算（按钮改高后会偏心）。
- 按钮高亮（悬停米白 / 固定态浅橙 / 关闭深红）画在**1px 外框线之后**（`PaintHeadButtons` 单独一趟）：
  否则那 1px `#D5D0C8` 会横在高亮块与窗口边缘之间，在深红旁看起来像「按钮右上方还有一圈白」。
- 贴窗口右上角的关闭按钮高亮用 `FillTRRound` 画成右上圆角：窗口靠 `SetWindowRgn` 裁圆角，直角高亮块会
  在角上被硬裁出缺口、露出桌面。
- **缩放同样不用系统的 `HTLEFT`/`HTBOTTOMRIGHT`**（也是模态循环），改为 `SetCapture` + 自己算差值。
  边缘抓取 5px，命中判定排在标题栏拖动**之前**（顶部那 5px 落在标题栏里）。
  `WM_SETCURSOR` 按方位给 `IDC_SIZEWE/SIZENS/SIZENWSE/SIZENESW`。
- 夹最小尺寸时「正在拖的那条边不动、对面的边动」（`ClampToMin`），否则拖到下限窗口会从另一侧往回缩。
- 每次改完尺寸都要 `RoundCorners` 重建圆角 region、`ComputeLayout` 重排、`ClampGridScroll` + `ClampScroll`
  校正滚动位置——列表行数跟着高度走，`Layout::row` 是 `std::vector`（早期固定 14 行，拉高就溢出）。
- 最小尺寸 `360×340` = 「四页签 + 三按钮排得开」与「至少露出两行」的交集。宽度下限压到 360 后四页签的
  自然宽度放不下，`ComputeLayout` 按可用宽度**均分**页签，底部提示条「装不下的项直接不画」，右侧计数优先保留。

### 8.1.1 列表滚动：行高固定 + 像素偏移 + 缓动

- **行高恒为 `ROW_H`（56），不拉伸**。曾经试过「列表填满可视区时把不足一行的余数摊给各行」来消灭底部
  空档，结果更糟：那几十像素的留白本来就在暗示「下面还有内容」，摊平之后行高变得不整齐，观感反而像到了底。
  正确做法是**照常画出底部那半行**，让下边沿把它切掉——`L.nRow` 因此可能多算一行，`PaintRow` 拿到的是完整
  行矩形（会越出 `body` 上下沿），裁切交给 `PaintPanel` 已有的 `PushAxisAlignedClip(body)`。
  命中判定同样依赖这点：先 `PtIn(L.body, p)` 再遍历 `L.row`，点到越界的那半行就等于点到它所属的那一行。
- **滚动状态只有一个**：`g_scrollY`（相对内容顶部的像素）。`g_top = g_scrollY / 行高`、
  `g_rowOff = g_scrollY % 行高` 是派生量，只由 `SyncListScroll()` 写——早先 `g_top` 是主状态，
  滚轮一格是 `g_top++`，内容一帧整行跳一次，观感是「一批格子换成另一批」，用户直接点出「完全没有滚动」。
- **坑：改了 `g_scrollY` 必须重算布局**。行矩形是 `ComputeLayout` 按 `g_rowOff` 生成的，只改偏移不重算，
  绘制仍用旧矩形 → 滚轮"完全没反应"且不报错。`TickListScroll()` 末尾因此带一次 `ComputeLayout()`。
- **缓动**：滚轮只改 `g_scrollTarget` 并起 `TIMER_SCROLL`（16ms），每帧把剩余行程走一半（约 5 帧 / 80ms 到位）。
  一格滚轮 = 一行高，`-delta * 行高 / WHEEL_DELTA` 保证触控板的零碎 delta 按比例滚，是真正的逐像素滚动。
  连滚时从**目标**累加而不是当前位置，否则上一格没走完的行程会被吃掉，手感比手慢半拍。
- 键盘导航（↑↓/PgUp/PgDn）、重置、`WM_SIZE` 一律**即时到位**（`ClampScroll` 里 `g_scrollY = g_scrollTarget = y`
  并 `KillTimer`）：这些场景要的是「准确」而不是「顺滑」，带惯性会出现连按 ↓ 时视口一直在追。
- 滚动条用 `g_scrollY` 而不是 `g_top * 行高`：后者只能整行对齐，滚到一半时滑块不动。
- 回归用例 `tests/test_scroll.py`（util 归一，见该文件头部的四条坑：滚轮正 delta 是向上、要按
  `GetClipboardSequenceNumber` 判「粘没粘上」、粘贴会把条目移到顶部所以不能跨步比较内容、面板收起会重置滚动）。
- **尺寸记忆**：松手时写 `%LOCALAPPDATA%\WinVClip\view-size.txt`（一行「宽 高」），启动时 `LoadViewSize`
  读回，`ShowPanel` 再用并夹一次最小尺寸与当前工作区。删掉该文件回到默认 `560×620`。

### 8.2 临时固定

- 连续插入（一次会话连点多个表情/符号）靠标题栏图钉：固定态 = `HWND_TOPMOST` + 换走前台也不收起。
  面板本来就不抢焦点，插入后焦点仍在目标窗口，连点自然落进同一个插入符。
- 实现是**跳过 `PanelShouldStay` 的判定**：`TIMER_FOCUS` 轮询到「前台已不是目标窗口」时，非固定态收起、
  固定态留着。
- **取消固定时就地复查一次**：固定期间可能已切到别的窗口，此时不会再有额外事件触发收起，不满足就关掉。
- 固定态下把「新的前台窗口」记成粘贴目标：固定时用户通常会换到另一个输入框继续插。
- 固定态下**不隐藏面板**；粘贴仍分两步。不需要把前台收回面板——面板从不持有前台。
- **固定状态只在本次显示期间有效**：`ShowPanel` / `HidePanel` 都重置为 false。
- 齿轮钮**不弹任何东西**：设置弹层暂未实现，`WM_LBUTTONDOWN` 里留了 TODO。原来的占位覆盖层
  （压暗 + 居中卡片 + 「暂无可配置项」）连同 `g_settingsOpen` / `SettingsRect` / `PaintSettings` 一起删除，
  避免留一个假面板。

### 8.3 右键菜单与另存为

菜单三项：**置顶 / 取消置顶**、**另存为…**、**删除**（底部、红色、上方分隔线）。贴着右键处弹出，
越界翻边，键盘 ↑↓ 循环、Enter 执行。另存为按类型分流：

| 类型 | 落盘方式 |
|---|---|
| 文本 | `.txt`，**UTF-8 带 BOM**；默认名 `剪贴板文本_YYYYMMDD_HHMMSS.txt` |
| 图片 | 一律 `.png`：PNG 条目直写原始字节（无损），`CF_DIB` 用 WIC 重新编码 |
| 文件 | 单文件 = 原样另存；多文件 = 用第一个确认目标位置，其余复制到同目录，重名自动加 `(1)(2)` |

四个必须知道的坑：

1. **对话框期间必须禁掉「失焦即隐藏」**（`g_dialogOpen`，在 `WM_ACTIVATE` / `TIMER_FOCUS` /
   `WM_APP_DEFER_HIDE` 三处判断）。否则对话框一弹出面板收到 `WA_INACTIVE` 自己收起，
   `GetSaveFileName` 的 owner 变成隐藏窗口。
2. **`CF_DIB` 没有 alpha 语义**：32bpp DIB 的 alpha 字节通常全 0，原样编码得到全透明 PNG（各看图软件显示
   空白）。DIB 转 PNG 前把 alpha 补成 255；带透明信息的 PNG 条目走原字节分支，无损。
3. **多文件不要用 `SHBrowseForFolder` 选目录**：在「桌面」这类 shell 节点上 `SHGetPathFromIDListW` 可能
   失败，表现为点完确定什么都没发生。改为复用 `GetSaveFileName`（拿第一个文件确认路径，其余同目录复制）。
4. 固定态面板是 `TOPMOST`，会压住对话框：打开前临时 `HWND_NOTOPMOST`，结束后恢复。

---

## 9. 字形库与渲染性能

### 9.1 数据与索引

数据来自参考页面的 `data/symbols.js`、`data/emoji.js`（Unicode 16.0 官方码表 + CLDR 中文注解 /
Emoji 18.0），由 `tools/gen_glyph_data.py` 转成 `src/glyph/glyph_data.cpp` 里两个只读 UTF-8 blob：

```
C\t<分类key>\t<分类名>\n          分类（符号的大类＝参考页面的「域」，生成期合并）
I\t<字形>\t<中文名>[\t<英文名>]\n  条目
```

**四个性能取舍**（9000+ 条，启动时全量转 `std::wstring` 会卡一帧）：

1. 启动只解析分类表（60 来行），条目不动，范围用指针记下。
2. 条目索引按分类惰性建立，且只建当前看的那一类（最大类 1893 条，指针数组十几 KB）。
3. 名称全程 UTF-8，只在画到屏幕时转宽字符；绘制按滚动偏移直接算第一个可见格，不从第 0 条遍历。
4. 搜索直接在 blob 上顺序扫描（不建索引），7500 条走一遍微秒级，命中上限 400。

生成脚本用 `\xNN` 转义所有非 ASCII 字节：源码不出现原始多字节字符，任何工具链编出的字节都一致
（`\x` 是贪婪转义，紧跟十六进制数字处插 `""` 断开字面量）。代价是 `glyph_data.cpp` ~840KB 纯字面量，
编译多花一两秒。**这个文件已入库，日常编译不用管它**；换数据时才重跑生成脚本。

### 9.2 分类与网格

参考页面是「域 → 52 个细分类」两级，面板只用一层大类（两级会让抽屉堆 52 个按钮还要分组标题），
所以**生成期按域合并**成 9 个大类。表情页与符号页共用同一套网格：9 列、每格 44px、间距 5px，
格内只画字形（居中、无边框、无名称）。

符号页早期是 4 列 72px 卡片（字形左上 + 中文名）：名称对「找一个长得像 ▷ 的符号」没帮助，反而把每格撑大、
一屏只能 4×8。统一成纯字形网格后一屏 9×10，字号 24px。抽屉里的首字形（`CatFirstGlyph`）仍用 11px
`g_fSymName`。

### 9.3 字体与缺字

`Segoe UI Symbol` 覆盖不到数学字母（𝒜）、古文字等区块，缺字会画成方框，所以按字形逐个测覆盖度，
依次退回 `Cambria Math` → `Segoe UI Historic` → `SimSun`。

判定必须用 DWrite 的 `IDWriteFont::HasCharacter`：GDI 的 `GetGlyphIndicesW` 按 **UTF-16 码元**算，
遇到代理对（𝒜、🫠）一律报缺字，会把能显示的字形误判成不支持（踩过，统计出「1739/1923 个 emoji 缺字」
的假结果）。**别再用 GDI 那条路做覆盖度判定。**

### 9.4 画不出来的字形直接剔除

任何候选字体都覆盖不到的字形（会渲染成方框）在建索引时就跳过，不占空格子：

- 过滤点：`LoadCat`（惰性建分类索引）、`GlyphSearchScan`（跨分类搜索）、`RebuildRecentItems`（「最近」）。
- 判定结果按字形缓存（`g_drawCache`，key 前缀区分表情/符号），一个字形只测一次。
- 分类计数用「真正能画出来的条目数」，所以数字比原始数据少几个：实测表情「情绪」172→171
  （少的正是 U+1FAEB），符号「常用」321→319。
- 未加载的分类仍显示 blob 原始数量，首次打开该类才校正——启动不必把 9000+ 条全测一遍。
- DWrite 探测对象建不起来时**一律放行**，否则整页空白。

### 9.5 性能：慢的不是光栅化，是「每格一个新 RenderTarget」

实测（560×620，`--perf`，日志 `%TEMP%\winvclip_perf.log`）：

| 场景 | 最初 | 现在（热缓存） |
|---|---|---|
| 表情页滚动 | 30.1ms/帧（峰值 429ms） | 2.7ms/帧（峰值 4ms） |
| 切分类（反复点 chip） | 首帧阻塞 1~2 秒 | 2.7~2.8ms/帧（峰值 4.2ms） |
| 冷启动首次进表情页 | 同上 | 2.1~3.5ms/帧 |
| 符号页滚动 | 6.1ms/帧 | 2.0~2.6ms/帧 |
| 历史页滚动 | 5.6ms/帧 | 2.2ms/帧 |

`tools/bench_emoji.cpp` 逐项对比定位到真相：每格 `CreateCompatibleRenderTarget` + Release 是 5.3~5.8ms，
复用同一个 RT 是 0.76ms，一次 Begin/End 批量画 9 格 0.70ms——**创建/销毁离屏目标的代价是光栅化本身的
7 倍**，且与格子尺寸、像素格式无关。但修掉这点后每格仍 5ms：软件光栅化 COLRv1 夹在主帧时序里就是这个
量级，一行 9 格 = 45ms，UI 线程怎么排都躲不开。

真正的解法是**别在运行时光栅化**（浏览器渲染几万 DOM 靠的是缓存 + 只处理可见部分，不是把光栅化写得更快）。
现在是三层：

1. **磁盘持久化图集**：全部 emoji 渲染成一张 atlas 落在
   `%LOCALAPPDATA%\WinVClip\emoji-atlas-v2-<sz>px-<cols>.bin`（约 19MB），下次启动只读文件 + 逐格
   `CopyFromMemory`，一个字形都不用光栅化。
2. **后台全量预热**：首次运行（无缓存）时把 1923 个 emoji 渲一遍，全进 atlas 后一次写盘；
   启动 120ms 后自动开始（`TIMER_WARM`），实测 1~3 秒。只开 1 个 worker——每多一个就要多一套
   D2D 设备（25~32MB，见 9.8），而磁盘缓存命中后这一步根本不会跑。
3. **视口优先队列**：UI 请求（可见格/预取行）永远排在预热任务之前——worker 先取 `g_emoReq`（LIFO），
   空了才推进 `g_emoWarmJobs`，所以切分类、滚动任何时刻都是即时的。
4. **worker 按需拉起**（见 9.9）：干完活的 worker 会自行退出以归还那套 D2D/DWrite 设备，
   所以 `PrepareEmojiBitmaps` 里只能「有缺失格时才 `EnsureEmojiWorker`」——无条件拉 =
   每帧「刚停又建」，一台设备几十毫秒。

atlas 容量按「全部 emoji 条数 + 8%」定（`cols = ceil(sqrt(1936)) = 46`，46×46 = 2116 格，2208×2208 px），
**装得下就不淘汰**。早期 400 格 + LRU 淘汰的问题正是「切回旧分类要重新光栅化」。

未就绪的格子早期画实心黑单色字形、跳变刺眼；现在画 `C_MUTED` + 22% alpha 的极淡剪影，即使真看到过渡也是
「淡影 → 显色」。配上磁盘缓存，正常情况下过渡根本不会出现。

### 9.6 为什么没上显卡加速

`ID2D1DCRenderTarget` 是纯软件光栅化（这也是当初选它的原因：直接绑内存 DIB + `BitBlt` 呈现）。吃 GPU 得换
`D3D11 + DXGI flip model` 或 `CreateHwndRenderTarget`，代价是「内存 DIB + `BitBlt` + `SetWindowRgn` 圆角」
这条呈现链要整体重做（flip model 下 `SetWindowRgn` 效果会变，圆角得改 `DWMWA_WINDOW_CORNER_PREFERENCE`
或自己做 alpha 合成）。而面板是静态 UI，只在滚动/输入时重绘，本来就 2~3ms（60fps 预算的 1/6），GPU 收益
是 0；真正的痛点是首次生成 1923 个字形，那是一次性成本，用磁盘缓存消掉比用 GPU 提速更彻底。
要做逐帧动画或半透明模糊时，才值得动 GPU 那条路。

### 9.7 其他仍然有效的优化与两个坑

`BindDC` 只做一次、离屏 DIB 复用（每帧省 2.5ms）、按脏区绘制并跳过脏区外的整段调用、`TextW` 按
「字体+字符串」缓存量宽、上传每帧上限 160 格。线程收尾：`StopEmojiWorker()` 必须在 `wWinMain` 返回前调用
（join 所有 worker），且早于释放 atlas / D2D 对象，否则 `std::terminate`。

- **整块 `CopyFromMemory` 会静默出错**：把 19MB 图集一次性拷到整个 atlas 区域，返回 `S_OK`，但画出来每格
  都是「被压扁的邻居」——采样矩形明明对（`(0,0,44,44)`、`(48,0,92,44)`…逐格验证过）。改成逐格拷贝立刻正常。
  软件 DCRenderTarget 上这个 API 对超大区域不可信，别用。
- **写盘内容必须自己验**：`tools/inspect_cache.py` 直接解析缓存文件、把前几格导成 PNG，才能区分「写盘错」
  还是「加载/绘制错」。

### 9.8 常驻内存：为什么是 100+MB，以及怎么压下去

`vmmap <pid>`（`tools/vmmap.c`）把提交内存按类型列出来才看清真相：**每一套 D2D 软件设备独占一块
25~32MB 的 `PAGE_NOCACHE` 私有池**。原来 UI 线程 + 2 个 emoji worker 各自 `D2D1CreateFactory`，
三套 = 96MB，这才是「一个剪贴板工具占一百多 MB」的全部原因。隔离实验（最小复现，逐项加）：

| 步骤 | 工作集 | 私有提交 |
|---|---|---|
| 空进程 | 8.5MB | 1.2MB |
| + D2D 工厂 & DCRenderTarget & 首次 BeginDraw | 44.5MB | 26.4MB |
| 同一工厂里再建 2 个 DCRenderTarget | +0.1MB | +0.1MB |
| **各自独立的工厂**再建 2 个 | +27MB | +32MB |

结论：池是「每个工厂（设备）一份」，不是每个 target 一份；只放 target 不还，工厂一起放才还。
字体/DWrite 不是大头（<1MB）。

三条对应的改法（都在 9.5 那张图集架构内）：

1. **worker 只留 1 个**，并且**活干完就停**（`PumpEmojiUploads` 末尾 `StopEmojiWorker`）——
   设备连同它缓存的字形一起还给系统，下次要新字形时由 `EnsureEmojiWorker` 现拉起来（~60ms，一次性）。
   日常切分类全是磁盘缓存命中，根本不进光栅化，所以不会有感。
2. **预热镜像用 `VirtualAlloc/VirtualFree`**：19.5MB 的 `vector` 即使 `clear + shrink_to_fit`，
   CRT 也常把页留在提交里；这里是一次性大块，直接走虚拟内存 API 才能真回收。
3. **闲置回收只 trim 工作集，不释放 D2D 设备**：`SetProcessWorkingSetSize(-1,-1)` 让私有页进 standby，
   任务管理器里那一列（= 工作集）直接掉到 1MB 以下，物理内存真的还给了系统；而对象还在，
   唤出不用重建。实测唤出延迟：热态中位 7.7ms、trim 后 7.3ms——**trim 不影响响应**。
   反过来若为了省提交去释放 D2D 设备，唤出要付 60ms 重建，那是砍在热路径上，不能做。
4. 图集在连续闲置 30 分钟后才放（`ReleaseEmojiCache`，磁盘缓存保留），因为重新读盘 ~200ms，
   只在「离开电脑很久之后」才会发生。

改完的实测：**隐藏闲置 0.2~0.5MB 工作集**（原 111.8MB），唤出后历史页 ~45MB、表情页 ~67MB（含 19.5MB 图集），
冷启动峰值从 192MB 降到 ~90MB。顺手修的两处：图片历史加 32MB 总量预算（`TrimImageBudget`，
单条 CF_DIB 上限 128MB × 60 条是潜在的 GB 级膨胀）；图片去重先比 size+hash 再 memcmp
（同分辨率截图的 DIB 大小完全一样，原来每次复制要逐字节比几十 MB × 最多 60 条）。

---

### 9.9 「每帧停一次再建一次 worker」：表情页滚动卡顿的真正原因

`PumpEmojiUploads` 末尾「活干完就 `StopEmojiWorker`」本身是对的（归还 25~32MB 设备，见 9.8），
但它和 `PrepareEmojiBitmaps` 里**无条件**的 `EnsureEmojiWorker()` 撞在一起就成了灾难：

- 每画一帧 → 上传队列空 → 停 worker（join + 释放设备/工厂/位图）→ 紧接着又拉起来
  （新建 D2D 工厂 + DCRenderTarget + DIB + DWrite 工厂 + 44px TextFormat）。
- 实测（`--perf`，560×620）：表情页滚动 `prep` 段 **35~53ms/帧**、`total` 36~53ms；
  另用「PostMessage(滚轮) 之后 SendMessage(WM_NULL) 的往返」量 UI 线程延迟，中位 **24ms**。
- 这个延迟之所以会让**整个系统的鼠标**都发钝：低级鼠标钩子（`WH_MOUSE_LL`）的回调排在装钩子的
  线程（我们的 UI 线程）上，线程一帧被占几十毫秒，全系统的鼠标事件都跟着排队。

改法：先算「可见格 + 上下各一行预取里有没有缺格」，**有才** `EnsureEmojiWorker()`；
热缓存、没有缺失格时一次设备都不建。同场景复测：`prep` 0.45ms、`total` 1.5~2.0ms、
线程往返中位 0.2ms（历史页 1.7~2.3ms、符号页 1.9~2.0ms，三页齐平）。

同批修掉的两条「只有一半动了」类问题（`tools/measure_grid.py` 可复测，回归见
`tests/test_glyph_scroll.py`）：

- **网格页的滚动条几何**：滑块位置在 `ComputeScrollbar`（挂在 `ComputeLayout` 里）算。表情/符号页
  滚轮只改 `g_gridScroll` 却不重算布局时，视图会滚、滑块钉在原地——用户侧原话是
  「滚动条用滚轮时无响应，但视图确实在滚动」。
- **网格页也走缓动**：一格滚轮 52px 一帧跳完 = 一整排格子瞬间换掉。现在与列表共用同一份
  smoothstep 曲线和 16ms 定时器（`TickListScroll` 按 `g_animGrid` 分流到 `g_gridScroll`），
  动画自己记住「上次写到哪」（`g_gridAnchor`），别处直接改偏移（切分类 / 改尺寸 / 拖滑块）
  时它自动让位，不会把新位置按旧行程拽回去。

---

## 10. 诊断

- **`WV_TRACE=1`** 启动 → 焦点/前台排查日志 `%TEMP%\winvclip-trace.txt`（`hook: downV -> toggle`、
  `ShowPanel enter`、`WM_ACTIVATE`、`HidePanel() from …`，以及 `uia:` 行）。默认关闭时只多一次 `getenv`。
- **`--perf` / `WINVCLIP_PERF=1`** → 每 30 帧往 `%TEMP%\winvclip_perf.log` 追加分阶段耗时
  （begin/prep/head/body/foot/end/blit/total/max）与 `warm:` 事件，不开时零开销。
- `tools/uia_probe.ps1`：只读诊断，打印前台窗口 / UIA 焦点元素的 pid、类型、矩形、选区矩形。
  「某个输入框为什么读不到插入符」先用它。
- `tools/uia_diag.exe [hwnd]`：更细的只读诊断，列出焦点元素全部文本通道（`GetCaretRange` /
  `GetSelection` / `RangeFromPoint` / 字符级扩展）与各自矩形、pattern 可用性。「读到整框 / 退化矩形」用它。
- `tools/caret_probe*.html`、`caret_big*.html`：插入符探针页（普通 input / contenteditable / textarea /
  iframe 内 / 空框 / 滚动长文）。
- `tools/bench_emoji.cpp`、`bench2.cpp`：字形光栅化基准与真实时序复现；`tools/analyze_struct.py`、
  `inspect_cache.py` 解析产物。

回归测试的用法、分组与已知假失败见 [tests/README](../tests/README.md)。

自动化交互的两条通则：点面板用 `PostMessage` 投递 `WM_LBUTTONDOWN/UP`（客户区坐标，`SendInput` 的鼠标移动
不给窗口送 `WM_MOUSEMOVE`，真实拖拽又打断面板）；跨进程切前台一律先校验 `GetForegroundWindow()`
（`SetForegroundWindow` 常被前台锁静默拒绝）。
