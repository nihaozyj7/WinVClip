# WinVClip —— 接管 Win+V 的自定义剪贴板

纯 Win32 C++ 实现，无第三方依赖（只多用了系统自带的 Direct2D / DirectWrite 画彩色 emoji）。
**完全接管 Win+V**：原生命令板不会出现，弹的是这个程序自己的面板。

面板视觉按设计稿（Clipboard Reliquary · PC Panel · 560 × 620）1:1 还原：
浅色纸感配色、标题栏内嵌页签、搜索框、分类 chip 条、56px 列表行、底部提示条、
右上角 toast、自定义右键菜单、删除确认弹层。配色与尺寸常量集中在 `src/theme.h`。

![表情页](docs/preview-emoji.png)

## 目录结构

```
src/         C++ 源码（按职责分文件，见 src/README.md）
tests/       回归测试（21 个，直接跑单个脚本即可，见 tests/README.md）
tools/       一次性诊断/测量/生成脚本（不是构建的一部分）
docs/        审查报告与截图
build/       构建产物（不入库）
```

改代码前先读 `src/README.md` 的「文件职责」表，确认该改哪个文件。

## 用法

```
build\winvclip.exe           # 常驻，托盘图标，按 Win+V 弹面板
build\winvclip.exe --show    # 启动后立刻弹一次（调试用）
```

托盘图标：左键开/关面板，右键菜单「打开面板 / 退出」。

## 面板

四个页签：

| 页签 | 内容 |
|---|---|
| 剪贴板 | 文本 / 文件 / 图片，置顶项排最前 |
| 收藏 | 按 Ctrl+D 收藏的条目 |
| 表情 | Emoji 全部条目，9 个分类 + 最近，彩色渲染；点一下**直接插入**目标窗口（有插入符时完全不碰剪贴板） |
| 符号 | Unicode 符号库，9 个大类 + 最近；点一下直接插入目标窗口，字体覆盖不到的字形不显示 |

| 操作 | 效果 |
|---|---|
| Win+V | 弹出 / 关闭面板 |
| 直接打字 | 实时过滤（标题 / 来源 / 类型 / 正文，不区分大小写） |
| ↑ ↓ / PgUp PgDn | 移动选择、翻页 |
| Enter / 单击行 | 写回剪贴板并粘贴到原窗口 |
| 单击表情 / 符号 | 直接插入目标窗口（有插入符→`KEYEVENTF_UNICODE` 注入，不碰剪贴板；取不到插入符→借剪贴板 `Ctrl+V` 后还原） |
| Shift + 单击表情 / 符号 | 强制走剪贴板粘贴通道（注入无效时的手动兜底） |
| 右键行 | 菜单：置顶 / 取消置顶、另存为…、删除（贴着光标弹出） |
| 拖动标题栏空白处 | 移动面板（面板默认贴着文本插入符弹出） |
| 拖动窗口边缘 / 角落 | 改变面板尺寸（最小 360×340），松手即记忆，重启沿用 |
| 标题栏图钉钮 | 临时固定：置顶 + 失焦不收起，可连续点多个表情/符号/条目 |
| 标题栏齿轮钮 | 设置入口，弹层暂未实现（当前点击无动作） |
| Tab、Ctrl+1~4 | 切页签（`Ctrl+1~4` 只在点过搜索框的输入态可用，见下文「接管范围」） |
| Ctrl+P / Ctrl+D / Ctrl+S | 置顶 / 收藏 / 另存为当前行（同上，只在输入态可用） |
| Delete | 删除（弹确认层） |
| 滚轮 | 滚动列表与网格 |
| 分类条 chip / 「⋯」 | 切分类 / 打开「全部分类」抽屉（符号、表情各 10 项：最近 + 9 大类） |
| 搜索框 | 输入即搜，符号与表情都按名称搜（中文名/英文名），中文可用输入法直接打 |
| Esc / 点别处 | 关闭（抽屉打开时 Esc 先收抽屉） |

记录类型：文本、文件（多选也算一条）、图片（PNG 或 DIB，记尺寸）。最多 60 条，
最新的在最上面，重复内容自动去重并提到最前。历史只存在内存里，退出即清空。

## 三个关键实现点（也是这套方案全部的坑）

### 1. 拦截 Win+V：`WH_KEYBOARD_LL` 全局钩子

`RegisterHotKey(MOD_WIN, 'V')` 在系统「剪贴板历史」开启时必然返回 `ERROR_HOTKEY_ALREADY_REGISTERED`，所以走低级键盘钩子，在 `LLKeyboard()` 里判断「Win 按住 + V 按下」后 `return 1` 吞掉事件。

**坑一（也是本程序最难的坑）：Win 键的抬起不能吞。**
一开始为了抑制开始菜单，我把 V 的 keyup 和 Win 的 keyup 一起吞了。结果系统键盘状态里 Win 永远处于按下状态，后续所有注入的按键都被当成 Win 组合：
- `SendInput` 的 Ctrl+V 变成 Win+Ctrl+V，粘贴没反应；
- 系统前台锁（ForegroundLockTimeout）生效，`SetForegroundWindow` 开始失败；
- 把 Down 键变成 Win+Down（最小化窗口），输入全乱。

正确做法（PowerToys 同款）：吞掉 V 之后**注入一个无害键 `VK_NONAME`(0xFC)**。Explorer 见到「Win 按下期间有其他键」，松开 Win 时不弹开始菜单；同时 Win 的抬起事件正常送达系统，键盘状态干净。

**坑二：长按会触发键盘自动重复，而这里是 toggle。**
按住 Win+V 不放时，系统在重复延迟（默认 500ms）之后以约 30ms 间隔持续补发 V 的 keydown。每次 keydown 都走「面板可见就隐藏、否则显示」，于是面板进入 Hide/Show 高速交替——视觉上就是疯狂闪烁。低级钩子的 `KBDLLHOOKSTRUCT` 里**没有**「这是重复键」的标志位（`flags` 只有 EXTENDED / INJECTED / ALTDOWN / UP / LOWER_IL_INJECTED），只能靠状态自己认：

```cpp
DWORD now = GetTickCount();
bool repeat = g_vDown && (now - g_vDownAt) <= 700;   // 上一次 keyup 之前的所有 down 都是重复
g_vDown = true; g_vDownAt = now;
if (!repeat) { /* 只有这里才 toggle */ }
```

- 重复键仍然 `return 1` 吞掉（否则 `v` 会漏进目标窗口），也仍然注入 `VK_NONAME`（保持 Explorer 对「Win 期间有别的键」的认知一致）。
- 700ms 窗口有两个作用：大于系统最慢重复速率（约 400ms/次），且能兜住「某次 keyup 没收到、`g_vDown` 卡在 true」——那样下一次按键间隔必然 >700ms，会被当成新的按下，不会变成「Win+V 彻底失灵」。
- `Win` 的 keyup 也清 `g_vDown`（UAC 安全桌面等场景 keyup 可能丢）。

回归脚本：`python test_longpress.py hold 2` 长按 2 秒（期间注入 66 个重复键）应只看到 **1 次** 可见性变化；`double` 验证 Win 全程按住时连续两次长按能正常开关；`seq` 验证快速连按三次依次翻转。

**坑三：接管范围必须窄，否则会把别家软件的全局热键饿死。**

`WH_KEYBOARD_LL` 是一条**串行链**：回调里 `return 1` 不只挡住目标窗口，还会让排在后面的所有低级键盘钩子——AutoHotkey、Quicker、输入法的全局热键、各种快捷键工具——根本收不到这一次按键。用户机器上这些工具通常比本程序先开机启动，也就排在链的后面，于是「面板一弹出来，别家的键盘快捷键就全哑了」，而点一下搜索框（面板拿到真前台、钩子不再介入）又立刻恢复正常——这条线索直接指向钩子的接管范围。

现在非激活态只接面板**确实会消费**的键（`PanelConsumesBareKey()`）：

| 接管 | Esc、Tab，列表页的 Up/Down/Prior/Next/Delete/Enter，弹层里的 Enter/Up/Down，Backspace（仅搜索串非空），以及能打出可见字符的键（非激活态直接打字进搜索框，这是核心体验） |
| 放行 | 所有带 Ctrl / Alt / Win 的组合、功能键、媒体键、PrintScreen、Home/End/Insert、面板当前页面不处理的导航键 |

代价写清楚：`Ctrl+1~4 / P / D / S` 这些面板快捷键在非激活态不再生效，只能点了搜索框进输入态再用（输入态由系统 `WM_KEYDOWN` 直达面板）。换来的是 `Ctrl+S` / `Ctrl+F` / `Ctrl+C` 这些属于前台程序的组合不再被面板抢走，别家的全局热键也能收到键。

`KEYEVENTF_UNICODE` 的注入事件要单独认：系统把 `vkCode` 填成 `VK_PACKET`(0xE7)（个别路径是 0），码元在 `scanCode` 里，真人键盘不会产生它——不认的话自动化脚本往面板里打的字会漏进目标窗口。

回归脚本：`python test_hook_passthrough.py`（先起 `hook_listen.py` 装一个外部钩子、再重启本程序，让本程序的钩子排在它前面，等价于用户机器上的顺序）。

### 2. 焦点模型：面板从不抢焦点（整个方案的地基）

早期版本走的是「抢前台」路线：`AttachThreadInput` → `SetForegroundWindow` → `SetActiveWindow` → `SetFocus`，硬把键盘焦点拽到面板上。这条路的根本毛病是：**键盘焦点是单一资源**，面板一拿到，目标窗口立刻收到 `WM_KILLFOCUS` 并在自己的处理里 `DestroyCaret`——记事本里的光标就消失了。

围绕这个洞打过一串补丁：画一根「影子光标」替身、粘贴时「恢复顶层窗口 + 恢复控件级焦点」两层补救，还专门为资源管理器地址栏调过一轮。结果是**换一个软件就歇菜**——因为每换一个目标，控件级焦点的恢复行为都不一样。

现在改成系统 Win+V 面板同款结构：**完全不碰焦点。**

- 面板带 `WS_EX_NOACTIVATE`，显示走 `SetWindowPos(..., SWP_NOACTIVATE | SWP_SHOWWINDOW)`，从不调用 `SetForegroundWindow` / `SetFocus`；`WM_MOUSEACTIVATE` 返回 `MA_NOACTIVATE`，鼠标点面板也不会激活它。
- 目标窗口因此**一直持有键盘焦点**，插入符一直闪——不是替身，是真的。
- 面板要用的按键由已有的低级键盘钩子旁路转发：`SendMessage(g_panel, WM_APP_PANELKEY, vk, MAKELPARAM(ctrl, ch))`。同线程 `SendMessage` 就是一次直接调用，没有跨线程等待，不会拖慢输入。转发范围收得很窄——只接面板会消费的键，其余全部放行（见本节坑三）。
- 粘贴时目标窗口本来就在前台，直接 `SendInput` 即可，不需要任何焦点恢复补丁。这是兼容性变好的根本原因。

三个必须注意的实现细节，都是实测踩出来的：

1. **修饰键必须放行，不能吞**。早期连 Win 的 keyup 一起吞了，系统因此认为 Win 一直按着：之后注入的 `Ctrl+V` 被解释成 Win+Ctrl+V，连按 Enter 都成了 Win 组合——实测直接弹出 Win11「快速设置」，`GetAsyncKeyState(VK_LWIN)` 全程返回「按下」。
2. **修饰键状态要自己跟踪**。钩子吞掉按键后系统不会更新全局键盘状态，`GetAsyncKeyState` / `GetKeyState` 在回调里读到的是「没按下」，`Ctrl+1` 就会被当成字符 `1` 打进搜索框。现在用 `g_ctrlDown` / `g_shiftDown` 自己记。
3. **自己注入的按键要打标记**。`SendInput` 出去的键带 `dwExtraInfo = 'WINV'`，钩子据此放行；其他来源的注入照常转发，与真人按键一致——固定态连着插表情时自己注入的字符才不会被自己的钩子吞掉，自动化测试也才能驱动面板。

### 2.5 唯一会激活面板的入口：点搜索框

中文搜索要靠输入法，而输入法组合**必须有真正的键盘焦点**，这一点绕不过去。所以搜索框留了一个显式切换：

- 平时（非激活态）英文/数字直接进搜索框，目标窗口光标一直闪；
- **点一下搜索框**才把前台交给面板（输入态），输入法可用，目标窗口光标暂时消失——这是用户主动进入搜索态，可以接受；
- `Esc` 或关掉面板时，`HidePanel` 把前台和控件级焦点一起还给目标窗口；
- 面板已隐藏就不再响应这类激活请求（`IsWindowVisible` 守住），否则前台会停在一个不可见的窗口上，而隐藏之后定时器已停，没人再把它收回来。

### 2.6 收起判据：看前台还是不是那个目标窗口

非激活态下面板从来不会被激活，**收不到 `WA_INACTIVE`**，所以「用户有没有切走」只能靠 120ms 的 `TIMER_FOCUS` 轮询 `GetForegroundWindow()`：

- 前台 == 面板自己 → 输入态，留着；
- 前台 == 唤起时的目标窗口 → 正常态，留着；
- 其他 → 用户切走了，`HidePanel()`。

这条判据取代了早期那套「唤起后 500ms 内允许重新抢前台」的抢救逻辑：那套是抢前台方案下的补丁，而抢成功后面板会重新激活，之后再没有失焦事件，反而会永久挂在最上层。

### 2.7 前台判据看不见的那一半：点面板外部即收起

上面那条判据有个盲区，而且是日常最容易撞上的一种操作：**非激活态下前台根本没变**。

面板从不夺取前台，唤起时前台就是目标窗口。用户在**同一个窗口**里点掉面板——在浏览器里点正文/地址栏、在记事本里点正文接着打字——前台仍然是那个窗口，`GetForegroundWindow()` 一动不动，轮询永远不会触发，面板就僵在屏幕上，只能再按一次 Win+V 或切到别的程序才收得掉。

系统 Win+V 没这个毛病，因为它自己是激活窗口，点哪儿都先丢一个 `WA_INACTIVE`。我们坚持不抢焦点，就只能自己盯鼠标：额外装一个 `WH_MOUSE_LL`（`LLMouse`），**按下的点落在面板矩形之外就收起**，并且不吞这次点击——用户点哪儿就是要点哪儿，面板只是让开。三条细节：

- **点自己的窗口不算「外面」**。另存为对话框、消息框都是本进程窗口，不排除的话一开对话框面板就被自己收掉。
- **点别的应用时不要抢先隐藏**。输入态下面板自己是前台，这时若抢在那次点击之前 `ShowWindow(SW_HIDE)`，系统会把前台随手交给 Z 序下一个窗口（实测交给了上一个记事本），用户点的窗口反而因为前台锁抢不到前台。所以这种情况交给前台轮询去收（120ms 内），`HidePanel` 那时看到前台已不是自己，自然不会走「还前台」分支。
- **`WA_INACTIVE` 里也一样**。收到它的时候前台常常还没落定（`GetForegroundWindow()` 仍返回面板），立刻判会走「还前台」把用户正点的窗口挤掉，所以先等一个 120ms 的 `TIMER_DEFERHIDE` 再判。`g_hideNoRestore` 负责在这条路上把 `HidePanel` 的归还动作堵掉。
- **固定态不受影响**：`g_pinned` 时钩子直接跳过，点外面照常留着面板（配合「临时固定」的语义）。

判据依旧只有「前台变化」和「按下点在面板外」两条，但它们是互补的：前者管切走整个应用，后者管同一个窗口内的点击。


### 3. 剪贴板监听：主窗口不能是 message-only

`AddClipboardFormatListener` + `WM_CLIPBOARDUPDATE`。它是发给**顶层窗口**的广播，`HWND_MESSAGE` 的消息窗口收不到——一开始用消息窗口，结果历史永远是空的。改成隐藏的顶层 `WS_POPUP` 窗口即可。

收到通知后不要立刻读，剪贴板常被别的进程占着：设 160ms 定时器再读，`OpenClipboard` 失败就继续重试。另外定时器 ID 不能和窗口消息值撞。

## 界面还原踩到的坑（自绘 UI 相关）

整套界面现在画在 **Direct2D + DirectWrite** 上，GDI 只负责最后 `BitBlt` 到窗口。起因是纯 GDI 处理不好这个画面：

- **GDI 的 `RoundRect` / `LineTo` / `Ellipse` / `Polygon` 完全不做抗锯齿。** 1:1 显示下圆角和图标线条就是阶梯锯齿，没有别的办法。改成 `ID2D1PathGeometry` + `DrawGeometry`（`PER_PRIMITIVE` 抗锯齿），笔触用 `D2D1_CAP_STYLE_ROUND` / `D2D1_LINE_JOIN_ROUND`。线宽也不再取整（`stroke * size/24` 直接给浮点），小尺寸图标才不糊。
- **`ANTIALIASED_QUALITY` 的小字号中文发虚，ClearType 又有彩边** —— 这个二选一在 GDI 里绕不出来。交给 DirectWrite 后 `D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE` 直接又锐利又干净。文字测量也换成 `IDWriteTextLayout::GetMetrics`：**量宽和绘制必须同一个引擎**，GDI 量、D2D 画会差 1~2px，页签和胶囊的宽度就会错位。
- **`ID2D1DCRenderTarget` 只认 32bpp 的 B8G8R8A8 目标**，`CreateCompatibleBitmap` 给的 DDB 在部分环境直接绑不上。别猜，手建 `CreateDIBSection`（`biBitCount=32`，`biHeight` 取负表示自上而下）。
- 目标 alpha mode 选 `D2D1_ALPHA_MODE_IGNORE`：ClearType 需要不透明目标，`PREMULTIPLIED` 会让文字出脏边。半透明遮罩不受影响（源 alpha 照常参与 source-over），`rgba(32,37,43,.28)` 一行 `FillRectangle` 就够了，不必再手拼预乘 DIB 做 `AlphaBlend`。
- **`BindDC` 会把 DPI 变成 DC 的**，绑定后必须 `SetDpi(96,96)` 才能保证 1 DIP = 1px；坐标原点是绑定矩形左上角，绑整窗时正好是 (0,0)。
- 一帧只在 `BeginDraw`/`EndDraw` 之间走一遍：滚动区用 `PushAxisAlignedClip` 裁，彩色 emoji 也在同一批里画（`D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT` —— `Segoe UI Emoji` 是 COLR/CPAL 彩色字体，这是它出彩色的唯一途径，GDI 只会画单色轮廓）。
- 1px 外框画在 `RectF(0.5, 0.5, W-0.5, H-0.5)` 上，否则 1px 线跨两个像素会糊成 2px 灰线。
- **齿轮这类「并集再挖孔」的图标别用 `CombineRgn` 了**：路径 `SetFillMode(WINDING)`，外圆和齿圆同向、轴孔反向绕序，一次填充就是正确的镂空。
- **MinGW 的 `swprintf` 是 C99 语义**：宽字符版本里 `%s` 当窄串、`%lld` 也不认，混用会输出乱码。中文字符串一律用 `std::wstring` 拼接，不要用格式化。
- 圆角靠 `SetWindowRgn`；`CS_DROPSHADOW` 提供系统投影。

## 列表行排版

每行两段文字：上行是标题（代码条目换等宽字体），下行左右两端对齐——左侧 `类型 · 来源：窗口标题`，右侧 `大小 - 相对时间`（如 `650KB - 9分钟前`）。右侧文本先按 `g_fMeta` 量宽再从行右缘反推起点，左侧的右边界取这个起点减 8px，超长走 `DT_END_ELLIPSIS`，这样右侧永远是齐的。

大小按内容实际占用算：文本取 UTF-8 编码字节数（不是 `wstring::size()`，中文会少算一半以上）、文件取磁盘上各文件之和（目录记 0）、图片取位图缓冲字节数。显示单位默认 KB，四舍五入后到 1000KB 以上才进位成 MB（`665696 → 650KB`、`1024000 → 1.0MB`）。

## 图片缩略图

图片条目不画类型图标，而是在原来 28px 图标块的位置画 36px 缩略图（按原图宽高比 letterbox 居中，所以 3840×2160 出来是 36×20、正方形图是 36×36）。

数据来源是条目里已有的原始位图（`ClipItem::bits`），解码分两条路：

- **PNG**（注册格式 `"PNG"`）→ WIC：`IWICStream::InitializeFromMemory` + 解码器 + `IWICFormatConverter` 统一成 `32bppBGRA`。
- **`CF_DIB`** → 交给 GDI 的 `StretchDIBits` 以 1:1 画进一张 32bpp DIBSection。这条路能免费拿到 1/4/8/24/32bpp 全部位深、调色板、上下翻转的转换，不用自己解析 `BITMAPINFOHEADER`（只需算准像素起始偏移：`biSize` + BI_BITFIELDS 的 3 个 mask + 调色板）。

之后自己盒式下采样（按面积把每个源像素归到目标桶，一遍扫完）、再做**圆角抗锯齿**：用圆角矩形的有符号距离场算像素覆盖率，乘进 alpha 后**预乘** RGB，`CreateBitmap` 用 `D2D1_ALPHA_MODE_PREMULTIPLIED`，`DrawBitmap` 的 source-over 就把圆角平滑地混到了列表底色上。

三个刻意的选择：

- 不走 `PushLayer` + 几何遮罩做圆角——**DC render target 不支持 layer**。CPU 端预乘是这条渲染链上唯一能拿到平滑圆角的路。
- 缩略图在 `PaintBegin` **之前**批量构建（`CreateBitmap` 是创建设备资源的操作，塞进 `BeginDraw`/`EndDraw` 之间会触发目标内部 flush）。
- 缓存挂在 `ClipItem::thumb` 上是 `shared_ptr`：条目的去重/复制会整条拷贝，裸指针会被重复释放。解码失败也留一个空 `Thumb`，避免每帧重试。

已知限制：4K 图首次显示要解码 + 过一次全图累积（约几十毫秒），只在第一次可见时付一次；`bits` 本身仍然保留原图，内存占用没有因为缩略图而下降。

## 面板定位与拖动

- **弹出位置贴着文本插入符（caret）**：先 `GetGUIThreadInfo` 读目标窗口的插入符矩形；读不到（Chromium / Electron 的插入符在渲染进程里，`hwndCaret` 恒为 0）就改问 **UI Automation 文本模式**（见下节）。拿到矩形后面板左缘对齐插入符、上沿落在插入符下方 `8px`；下方放不下就翻到插入符上方、左右再不行就摆到旁边，**并且尽量不压住输入框**（候选位置按「不遮挡 > 完整在屏幕内 > 遮挡面积小」打分，见下节）。**两条通道都拿不到插入符时（桌面、文件列表、没有任何输入框的窗口）居中于鼠标所在显示器**。每次 Win+V 都重新算，所以拖过的位置只对本次显示有效。
- **拖动用自己写的手动拖拽**（`WM_LBUTTONDOWN` 记屏幕坐标 + `SetCapture`，`WM_MOUSEMOVE` 里按差值 `SetWindowPos`），不是 `WM_NCHITTEST` 返回 `HTCAPTION`。后者走系统的模态移动循环，而在「失焦即隐藏」的弹窗上，那个循环一进去窗口就被判失活、面板自己收起来了，拖动等于没发生。手动拖拽还顺手把标题栏空白处的指针换成 `IDC_SIZEALL`（`WM_SETCURSOR`）。
- 标题栏里**页签、图钉、齿轮、关闭四个热点仍走客户区**，只有真正的空白处才开始拖动，否则点页签会变成拖窗口。
- 右上三个按钮**与标题栏等高**（`y = 0..HEAD_H`）、宽度 30px、彼此紧邻，关闭按钮右缘就是窗口右缘（间距 0），图标按按钮矩形居中（`CenterIcon`），不再靠 `InflateRect` 从矩形内缩推算（按钮变了高度后那样会偏心）。
- 按钮高亮（悬停米白底 / 固定态浅橙 / 关闭深红）画在**1px 外框线之后**（`PaintHeadButtons` 单独一趟，在所有内容与外框画完之后）：否则那 1px 的 `#D5D0C8` 会横在高亮块与窗口边缘之间，在深红旁边看起来就是"按钮右上方还有一圈白"。
- 贴窗口右上角的关闭按钮高亮用 `FillTRRound` 画成**右上角圆角**（其余直角）：窗口靠 `SetWindowRgn` 裁圆角，直角矩形的高亮块会在角上被硬裁出一个缺口，露出后面的桌面。

### 怎么读目标窗口的插入符

`GetGUIThreadInfo(目标线程)` 的 `rcCaret` 就是插入符矩形，但它的**坐标基准在文档与实测之间不一致**（一部分窗口给的是客户区坐标），所以拿插入符所在窗口的屏幕矩形做判别：先 `ClientToScreen` 转一次，转换结果落在窗口矩形内就采用转换值，否则用原值，两者都不在窗口内就当没有插入符。不这么判的话面板会跑到屏幕外面去。

另外 `GetGUIThreadInfo` 对**更高权限进程**的窗口会失败（UIPI），这时按「没有插入符」处理、面板居中，不会出错。

**Chromium / Electron 走 UIA**：Edge、Chrome、VS Code、各种内嵌浏览器的插入符在渲染进程里，`GetGUIThreadInfo` 的 `hwndCaret` 永远是 0（光标在闪，但读不到），第一版因此在这些窗口里一律退回屏幕居中。正解是 UI Automation 的文本模式——`GetFocusedElement` → `TextPattern2::GetCaretRange`（没有就退到 `TextPattern::GetSelection`）→ `IUIAutomationTextRange::GetBoundingRectangles`，每一组 4 个 `double` 就是 `(left, top, width, height)` 的屏幕物理像素矩形。

**焦点元素的归属判定按进程树，不是 pid 相等**：Chromium 把页面内容放在渲染子进程里，Electron 应用同理，所以网页输入框的焦点元素 pid 与顶层窗口 pid 不同。早期版本按 pid 严格判等，结果地址栏（属于浏览器进程）能过、**网页里的输入框一律被静默拒掉**——用户看到的就是「浏览器输入框检测不到」。现在认「同进程 或 目标进程的子孙进程」（`CreateToolhelp32Snapshot` 沿父进程链上溯，最多 6 层），面板自己、输入法候选窗这类旁支仍然被挡在外面。拒绝时也会在 trace 里留一行 `uia: foreign focus elpid=… targetpid=…`，不再是静默返回。

**可访问性树是懒建的**：Chromium 只在有客户端询问时才建 a11y 树，刚被唤醒的头一两次询问可能还拿不到 `TextPattern`、或者只回整框。所以首轮没探测到插入符、或只拿到「框内近似」时，面板先按近似/居中显示，随后用 `TIMER_REPROBE` 补探两次（140ms / 160ms 后），成功就只挪位置（`SWP_NOACTIVATE`，不碰前台）；用户自己拖过面板（`g_posUserSet`）就不再挪。

**`GetSelection` 兜底只对文本控件类型生效**：Chromium 里 input / textarea 是 `Edit`、contenteditable 是 `Group`，而焦点落在页面本身（body、页面级快捷键劫持）时焦点元素是 `Document`。`Document` 的 `TextPattern` 是整页文本视图，`GetSelection` 返回的是页面里当前那块被选中/被跟踪的内容，与插入符无关——实测 bilibili 视频页给回一个 `46x46` 的方块，采信它面板会贴到屏幕上完全无关的位置。所以：`Document` 只接受 `TextPattern2::GetCaretRange(active=TRUE)`，其余类型才用 `GetSelection` 兜底，列表项/按钮这类即便暴露了 `TextPattern` 也不用。

实测（Edge，`WV_TRACE=1` 的 `uia:` 行）：普通 input `ct=50004 caret2=0`（走选区兜底）、contenteditable `ct=50033` 同样能取到、页面文档 `ct=50030` 正确处理为"没有插入符"→ 居中；WebView2 / Edge 的**空**文本框 `bad=1 approx=1`（回的是整框，走框内近似）。

代价与取舍：第一次访问 Chromium 会唤醒它的可访问性树（实测首次 ~15ms，冷启动可达上百毫秒），所以只在 `GetGUIThreadInfo` 失败时才走这条路，并且用 `IUIAutomation2::put_ConnectionTimeout(500)` 把跨进程调用卡死的最坏情况压到半秒。

**Chromium 不提供 `TextPattern2`，而且空文本框只回「整框」——这是「读到的明显是整个输入框」的根因**。实测 Edge / WebView2 的 `input` 与 `textarea`：`GetCurrentPatternAs(UIA_TextPattern2Id)` 一律失败（`GetCaretRange` 这条最准的通道在 Chromium 上根本走不到），只能用 `TextPattern::GetSelection` 的退化范围当插入符；而这个退化范围在**内容为空**时返回的是**控件整体矩形**（WebView2 聊天输入框 `931x46`、空 textarea `660x300`，真实插入符是 `1x17`）。采信它，落点算法就以为要避让的光标行有整块控件那么大，面板被甩到框外很远。两道防线：

- `CaretRectIsBox`：插入符矩形与焦点元素框的面积占比 ≥8 成、或宽高都与框一致 → 判为假数据丢弃，trace 留 `bad=1`；
- `ApproxCaretInBox`：丢弃后若**框本身可信**（过 `BoxSaneRect`），退到「框内首行起始处」（左上内缩一个 padding、行高 20dp）当插入符，面板贴框外弹出——比居中准得多，空输入框的插入符本来就在首行；框不可信（整窗级编辑器）才放弃定位、居中。

这时 `CaretInfo.approx=true`：面板先照近似位置弹出来，`TIMER_REPROBE` 再补探两次（拿到真插入符就只挪位置）；另外 `ProbeCaret` 里做了一次**同步重试**——Chromium 第一次被问常常只回整框或退化矩形，紧接第二次就精确了，同步重试能让面板不必先跳一下。实测同一台机器上：WebView2 空输入框 `bad=1 approx=1 caret=1432,990`（框外下方，不再甩到远处）、往里打字后 `bad=0 approx=0 caret=1587,985`（真插入符，面板贴那一行）；Edge 地址栏 `caret=146,62`（1x16）、大 textarea `caret=299,188`（1x24）。

### 落点怎么算（不遮挡输入框）

只算一个「下方放不下就翻到上方」是不够的：翻上去之后还要夹进工作区，夹完正好压在输入框上（面板高 620、屏幕 1440、输入框在中部时很常见），用户只能先把面板挪开再输入。现在的做法是：

1. 拿一个**避让矩形** `avoid`：优先用焦点控件的整体矩形（原生 = 焦点控件窗口矩形，UIA = 焦点元素 `CurrentBoundingRectangle`），拿不到或不可信时退化成插入符那一行。
2. 生成 6 个候选落点：插入符下方 / 上方 / 右侧 / 左侧 / 贴屏幕底 / 贴屏幕顶，每个都先夹进工作区。
3. 逐个打分：**不遮挡 `avoid` > 完整落在工作区内 > 遮挡面积更小**，同分取先出现的（所以默认还是下方）。

`avoid` 的可信度由 `BoxSaneRect` 把关：太高（超过工作区 2/3）、又宽又高、或者跟目标窗口面积占比 ≥6 成的一律不认——Win11 记事本的焦点控件就是整个客户区，认了它面板会被摆到窗口下方的空白里，反而离插入符更远。

无解时（面板比可用空间还高）退化成「遮挡面积最小」，至少有插入符的那一行始终可见。

### 影子光标（已删除）

这一节记录的是一个**已经删掉的补丁**，留着是为了说明「为什么当初走了弯路」。

症状：面板抢到焦点后，记事本里的插入符消失。原因是真插入符属于**拥有焦点的线程**，目标窗口一收到 `WM_KILLFOCUS` 就在自己的处理里 `DestroyCaret`（记事本实测 `hwndCaret` 直接变 0），而 `ShowCaret` 是每输入队列的资源、外部没法替别的线程点亮它。

当时的做法是在插入符原位画一根同尺寸替身窗口（`WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED`，非 topmost，压在面板之下，530ms 用 alpha 切闪烁），并且要处理「失焦后读不到新位置，只能沿用旧值」「固定态插入后靠 `TIMER_REPIN` 抢回前台前先量一次插入符」这些细节。

**教训**：影子光标是在为一个自己制造的问题做补救。现在面板根本不抢焦点（见「焦点模型」），真光标一直在闪，这一整套代码连同 `TIMER_GHOST` / `TIMER_REPIN` / `GhostProc` 窗口类全部删除了。

## 面板尺寸调整与记忆

- **拖任意边/角改变尺寸**，范围 `MIN_W=360 × MIN_H=340` 到光标所在显示器的工作区。最小尺寸是「四页签 + 三个按钮排得开」和「至少露出两行」的交集；宽度下限压到 360 后四个页签的自然宽度已经放不下，`ComputeLayout` 会按可用宽度**均分**页签（否则「剪贴板」会顶到图钉按钮底下、或页签互相压住），底部提示条也改成「装不下的项直接不画」，右侧计数优先保留。
- 和标题栏拖动一样，**不用系统的 `HTLEFT`/`HTBOTTOMRIGHT`**（那是模态 sizing 循环，会被「失焦即隐藏」打断），而是 `SetCapture` 后自己算差值：按下记下窗口矩形和鼠标屏幕坐标，`WM_MOUSEMOVE` 里加到对应边，`WM_LBUTTONUP` 收尾。
- 边缘抓取带 `5px`，命中判定排在标题栏拖动**之前**——顶部那条 5px 落在标题栏里，先判 resize 才不会误触发拖动。`WM_SETCURSOR` 按方位给 `IDC_SIZEWE/SIZENS/SIZENWSE/SIZENESW`。
- 夹最小尺寸时「正在拖的那条边不动、对面的边动」（`ClampToMin`），否则拖到下限时窗口会从另一侧往回缩。
- 每次改完尺寸都要 `RoundCorners` 重建圆角 region、`ComputeLayout` 重排、`ClampGridScroll` + `ClampScroll` 校正滚动位置——列表行数现在**跟着高度走**，`Layout::row` 从固定数组改成了 `std::vector`（原来上限 14 行，面板拉高就会溢出）。
- **尺寸记忆**：拖完松手时把尺寸写进 `%LOCALAPPDATA%\WinVClip\view-size.txt`（一行「宽 高」），启动时 `LoadViewSize` 读回，`ShowPanel` 用记忆值并再夹一次最小尺寸与当前工作区。删掉这个文件就回到默认 `560×620`。

## 临时固定与设置面板

- **连续插入**（一次会话连点多个表情/符号）靠标题栏的图钉钮：固定态 = 窗口 `HWND_TOPMOST` + 换走前台也不收起。因为面板本来就不抢焦点，插入后焦点仍在目标窗口，连点自然落进同一个插入符。
- 固定的实现是**跳过 `PanelShouldStay` 的判定**：`TIMER_FOCUS` 轮询到「前台已不是目标窗口」时，非固定态收起、固定态留着（见上文「收起判据」一节）。
- **取消固定时就地判断一次**：固定态期间用户可能已经切到别的窗口，此时取消固定不会再有额外事件触发收起，所以当场用同一条判据复查一次，不满足就关掉，否则会回到「挂着不消失」的状态。
- 固定态下顺手把「新的前台窗口」记成粘贴目标：固定的时候用户通常会换到另一个输入框继续插，不更新目标就会一直往最初那个窗口里塞。
- 粘贴分两步的流程不变，只是固定态下**不隐藏面板**。早期还要在粘完之后把前台收回面板，现在完全不需要——面板从不持有前台，目标窗口一直是收键盘的那一方。
- **固定状态只在本次显示期间有效**：`ShowPanel` 和 `HidePanel` 都会把它重置成 `false`，所以关掉面板再打开一定是未固定态。
- 齿轮钮目前**不弹任何东西**：设置弹层暂不实现，点击只保留按钮本身（`WM_LBUTTONDOWN` 里留了 TODO 接入点）。原来的占位覆盖层（压暗 + 居中卡片 + 「暂无可配置项」）已连同 `g_settingsOpen` / `SettingsRect` / `PaintSettings` 一起删除，避免留一个假面板。

## 右键菜单与另存为

菜单三项：**置顶 / 取消置顶**（文案随条目状态变）、**另存为…**、**删除**（底部，红色，上方一条分隔线）。菜单贴着右键点击处弹出，越界翻边，键盘 ↑↓ 循环、Enter 执行。

另存为按条目类型分流：

| 类型 | 落盘方式 |
|---|---|
| 文本 | `.txt`，**UTF-8 带 BOM**（记事本/VS Code 都能正确识别中文）；默认名 `剪贴板文本_YYYYMMDD_HHMMSS.txt` |
| 图片 | 一律存 `.png`：PNG 条目直接写原始字节（无损），`CF_DIB` 条目用 WIC 重新编码 |
| 文件 | 单文件 = 原样另存；多文件 = 用第一个文件确认目标位置，其余复制到同一目录，重名自动加 `(1)(2)` |

三个必须知道的坑：

1. **文件对话框期间必须禁掉「失焦即隐藏」**（`g_dialogOpen`，在 `WM_ACTIVATE` / `TIMER_FOCUS` / `WM_APP_DEFER_HIDE` 三处判断）。否则对话框一弹出面板就收到 `WA_INACTIVE` 把自己收起，`GetSaveFileName` 的 owner 变成隐藏窗口。
2. **`CF_DIB` 没有 alpha 语义**——32bpp DIB 的 alpha 字节通常全是 0，原样编码会得到一张全透明 PNG（各看图软件显示成空白）。所以 DIB 转 PNG 前把 alpha 统一补成 255。带透明信息的 PNG 条目走原字节落盘分支，无损。
3. **多文件不要用 `SHBrowseForFolder` 选目录**。实测在「桌面」这类 shell 节点上 `SHGetPathFromIDListW` 可能返回失败，表现为点完确定什么都没发生（用户以为功能没做）。改为复用已验证可用的 `GetSaveFileName`（拿第一个文件确认路径，其余同目录复制），路径来源单一、行为确定。
4. 临时固定态下面板是 `TOPMOST`，会把对话框压住；打开对话框前临时 `HWND_NOTOPMOST`，结束后恢复。

## 一个 D2D 上的坑：填充路径不渲染

早期齿轮图标用 `FillGeometry` 画（外圆 + 8 齿并集、轴孔反向绕序挖空），在 `DCRenderTarget` 上**完全不可见**——同一函数里走 `DrawGeometry`（描边）的图标都正常。实测把齿轮和 toast 的「⋯」三点都改成描边路径后立刻正常显示，所以本项目的图标一律走描边，不要用填充路径。真需要实心图形时优先用 `FillRoundedRectangle` / `FillRectangle`。

## 字形库（表情 / 符号）

数据来自参考页面的 `data/symbols.js`、`data/emoji.js`（Unicode 16.0 官方码表 + CLDR 中文注解 / Emoji 18.0），
由 `tools/gen_glyph_data.py` 在构建前转成 `src/glyph_data.cpp` 里两个只读 UTF-8 blob：

```
C\t<分类key>\t<分类名>\n          分类（符号的大类＝参考页面的「域」，在生成期合并）
I\t<字形>\t<中文名>[\t<英文名>]\n  条目
```

**三个性能取舍**（9000+ 条数据，直接在启动时全量转 `std::wstring` 会卡一帧）：

1. 启动只解析分类表（60 来行），条目不碰。分类的条目范围用指针记下来即可。
2. 条目索引按分类惰性建立，且**只建当前正在看的那一类**（合并成大类后最大一类 1893 条，指针数组十几 KB）。
3. 名称全程保持 UTF-8，只在真正画到屏幕时转宽字符；绘制按滚动偏移直接算出第一个可见格，
   不会从第 0 条开始遍历。
4. 搜索直接在 blob 上顺序扫描（不求建索引），7500 条走一遍是微秒级，命中上限 400（与参考页面一致）。

生成脚本用 `\xNN` 转义输出所有非 ASCII 字节：源码里不出现原始多字节字符，任何工具链编出来的
字节都一致（`\x` 是贪婪转义，后面紧跟十六进制数字的位置会插入 `""` 断开字面量）。
代价是 `glyph_data.cpp` 有 ~840KB 纯字面量，编译多花一两秒。

### 分类条与「全部分类」抽屉

参考页面是「域 → 52 个细分类」两级，面板只用一层大类（两级会让抽屉里堆 52 个按钮还要分组标题），
所以**在生成期按域合并**：符号就是 9 个大类（常用 319 / 标点 / 数学 / 箭头 / 图形 / 单位 / 标记 / 字符 / 其他），
表情 9 类，两页同构。

- chip 固定最多 4 个 = 「最近」+ 该页 3 个常用分类；当前分类不在其中时，挤掉最后一个补上；
  宽度不够时从最左边丢，当前分类永远保留；剩下全部进「⋯」抽屉（角标显示分类总数）。
- **「最近」没内容时整条分类都不出现**：分类条 chip、抽屉按钮、抽屉标题与「⋯」角标的计数一起隐藏
  （`RecentEmpty()` 读 `RebuildRecentItems()` 维护的条数），否则打开面板先看到的是一个空的「还没有最近」页；
  当前分类若正好停在「最近」上，`ComputeLayout()` 会把它挪到该页第一个分类。
- 抽屉从分类条下方展开、盖住列表区：**一片扁平按钮，没有分组标题**（两页都一样），
  4 列 30px 按钮，可滚动，点选即切分类并关抽屉。
- 合并带来的副作用：大类的条目数变多（最大的「图形」1893 条、去重后本机能画 957 个）。
  首次打开这类要建索引 + 逐个测字体覆盖度，实测 8ms，可接受。

### 搜索（按名称，符号和表情都支持）

搜索框的查询串对两个网格页都生效，**跨全部分类**扫描 blob：
字形本身精确匹配，或中文名/英文名包含关键词（大小写不敏感，`LowerU8` 后逐字节比较）。
搜「箭头」→ 396 条、「破折号」→ 6 条、`cat` → 14 条。命中上限 400（与参考页面一致），不建索引，7500 条扫描在微秒级。

搜索时分类条左侧标签显示「搜索 · N」，分类 chips 仍在（可以直接切分类收窄，但搜索本身跨分类）。

### 中文输入：输入法消息要自己接

搜索框是自绘的，系统不知道 caret 在哪。**不处理 IME 消息的话，中文一个字都打不进来**——
按键会以普通 `WM_CHAR` 直接进查询串。所以：

- `WM_IME_SETCONTEXT`：清掉 `ISC_SHOWUICOMPOSITIONWINDOW`，组合串由面板自己画（否则系统会在别处开一张组合窗口）。
- `WM_IME_STARTCOMPOSITION`：只清组合串、定位候选窗，**必须交给 `DefWindowProc`**。
  早期这里 `return 0` 拦掉了，表现是组合串能显示、但空格上屏不认（输入法状态机不完整）。
- `WM_IME_COMPOSITION`：`GCS_RESULTSTR` 并入 `g_query`，`GCS_COMPSTR` 存进 `g_comp` 只作预览。
  预览串画在查询串后面并带一条橙色下划线，caret 位置按「查询串 + 组合串」的总宽算。
- `WM_IME_ENDCOMPOSITION`：清 `g_comp`。
- 候选窗用 `ImmSetCandidateWindow(CFS_CANDIDATEPOS)` 钉到搜索框光标正下方，否则会飘到屏幕角落。
- `WM_KEYDOWN` 开头：`g_comp` 非空时直接 `return 0`——组合中的空格/数字/Esc 都属于输入法，
  不能顺带触发面板快捷键（尤其 Esc，否则想取消组合反而把面板关了）。
- 输入法的「中/英」模式是系统状态，程序不干预；英文模式下敲字母就是普通字符。

### 网格与排布

表情页和符号页共用同一套网格：9 列、每格 44px 高、间距 5px，格内**只画字形本身**（居中、无边框、无名称）。

符号页早期是 4 列 72px 的卡片，字形左上、下方带一行中文名（`句号`、`右箭头` 之类）。
名称对「我要找一个长得像 ▷ 的符号」这件事没有帮助，反而把每格撑大、一屏只能看 4×8 个，
所以统一成和表情页一样的纯字形网格，一屏可看 9×10。字形字号 24px。

分类抽屉里的首字形（`CatFirstGlyph`）仍用 11px 的 `g_fSymName`，只有那里还需要小字。

### 字体与缺字

符号的字体不是随便挑的：`Segoe UI Symbol` 覆盖不到数学字母（𝒜）、古文字等区块，
缺字会画成方框。所以按字形逐个测覆盖度，依次退回 `Cambria Math` → `Segoe UI Historic` → `SimSun`。

判定必须用 DirectWrite 的 `IDWriteFont::HasCharacter`：GDI 的 `GetGlyphIndicesW` 按 **UTF-16 码元**算，
遇到代理对（𝒜、🫠）一律报缺字，会把本来能显示的字形误判成不支持（踩过，统计出「1739/1923 个 emoji 缺字」的假结果）。

仍然画不出来的是本机字体真的没有的字形（主要是 Emoji 18.0 新增的少数表情，如 U+1FAEB「打破」）。
这类字形**不显示**，见下一节。

### 画不出来的字形直接剔除

有任何候选字体都覆盖不到的字形（也就是会渲染成方框的那种），在建索引时就跳过，不会占一个空格子：

- 过滤点在 `LoadCat`（惰性建分类索引时）、`GlyphSearchScan`（跨分类搜索时）、`RebuildRecentItems`（「最近」）。
- 判定结果按字形缓存（`g_drawCache`，key 前缀区分表情/符号），一个字形只测一次覆盖度。
- 分类计数改用「真正能画出来的条目数」，所以分类条上的数字会比原始数据少几个：实测表情「情绪」172 → 171（少的正是 U+1FAEB），符号「常用」321 → 319。
- 未加载的分类仍显示 blob 里的原始数量，首次打开该类时才校正——这样启动不必把 9000 多条全测一遍覆盖度。
- DWrite 探测对象建不起来时**一律放行**，否则整页会变空白。

> 老脚本 `tools/count_missing_glyphs.py` 已删除：它用 GDI 的 `GetGlyphIndicesW` 判定，会得出
> 「1739/1923 个 emoji 缺字」这种假结果，不能用来核对这套逻辑。

## 粘贴回原窗口的流程

1. 弹面板前记下 `GetForegroundWindow()` 作为粘贴目标；
2. 选中某条 → 隐藏面板 → 把该条写回系统剪贴板（`CF_UNICODETEXT` / `CF_HDROP` / `PNG`|`CF_DIB`）；
3. 非激活态下目标窗口本来就在前台，直接起 90ms 定时器即可（定时器里仍会断言 `GetForegroundWindow() == 目标`，只有输入态才需要先 `ForceForeground` 交还前台）；
4. 定时器里 `SendInput` 发 Ctrl+V（抢不到前台就不发，见下文「注入前必须确认两件事」）。

直接同步发 Ctrl+V 会粘到旧窗口，这是必须分两步的原因。

### 点表情 / 符号：两条通道（兼容性的关键在这）

表情和符号有两种落地方式，选哪条取决于**能不能拿到目标的文本插入符**：

| 目标 | 通道 | 剪贴板 |
|---|---|---|
| 有文本插入符（记事本、Edit/RichEdit、Office、绝大多数原生输入框） | `SendInput` 的 `KEYEVENTF_UNICODE` 逐码元直插 | **完全不碰** |
| 取不到插入符（Chromium/Electron 页面、Edge 地址栏、UWP、终端、shell 列表） | 借剪贴板 + `SendInput` 发 `Ctrl+V`，粘完还原 | 短暂借用后还原 |

注意这条判据仍然只看 `GetGUIThreadInfo`（`CaretRectOf`），**故意不用 UIA**：UIA 能读到浏览器里的插入符是为了定位面板，而 Chromium 的渲染输入框接受 `KEYEVENTF_UNICODE` 直插并不可靠（组合输入、受控输入框会吞字符），粘贴通道才是稳的那条。定位通道和注入通道分开判定，别再合并。

#### 为什么不再用「有没有插入符」判断「能不能插入」

这是踩过的坑，记下来别再犯：

- 早期版本一律 `KEYEVENTF_UNICODE` 直插。资源管理器文件列表没有文本输入区，注入的字符被 Shell 当成「键入查找」去匹配文件名，匹配不到就**叮一声系统提示音**，什么都没插进去。
- 于是加了「没有插入符就拒绝插入」。但这个判据把太多东西一起拒了：**Chromium 系列的插入符在渲染进程里**，`GetGUIThreadInfo` 根本取不到，Edge 地址栏、网页输入框全被判成「不接受文本输入」。用户看到的就是「系统自带面板能粘，这个面板点表情没反应」。
- 现在的结论：**取不到插入符 ≠ 不能粘**。分通道而不是分能否：
  - 插入符可读 → 直插（不碰剪贴板，最干净）；
  - 插入符不可读 → 走 `Ctrl+V`。对所有这类目标都成立，系统 Win+V 面板走的也是这条路。
  - 文件列表收到 `Ctrl+V` 对文本剪贴板是**无操作**，不响铃——比直插安全。

#### 借用剪贴板的细节

- 备份当前剪贴板（`CF_UNICODETEXT` / `CF_HDROP` / PNG / `CF_DIB`，图片有 32MB 上限）。
- 写完立即记下 `GetClipboardSequenceNumber()`。还原前再比一次：**序列号变了就放弃还原**，绝不覆盖用户或别的程序刚复制的新内容。
- 粘贴后 600ms 才还原：部分程序是异步读剪贴板的，还原太早会粘到旧内容。面板隐藏（`Esc`、取消固定、切走）时也会立刻归还。
- 借用期间写入的内容**不进历史**：`CaptureClipboard` 开头比序列号，是自家写的就跳过。否则每点一个表情，历史里就多一条孤零零的 emoji。
- 连续点多个字形只备份一次，不会把上一次的临时内容当成「原剪贴板」。

#### 手动兜底：Shift + 点击

注入通道偶尔也会失效（目标控件只认粘贴消息、IME 正在组字等）。**按住 Shift 点格子 = 强制走剪贴板粘贴通道**，这条对所有目标都最兼容。

其他实现要点：

- 代理对（emoji）按 UTF-16 **码元逐个发**，目标侧的 `WM_CHAR` 会自己合成；ZWJ 组合序列（如 👨‍👩‍👧）也是逐码元连着发。
- 注入前同样走「先隐藏面板 → 90ms 定时器」，定时器里再判断：`g_typePending` 非空就走注入，否则走 `Ctrl+V`（历史页/收藏页的条目含图片和文件，只能走剪贴板）。
- 固定态下不隐藏面板；焦点本来就在目标窗口，直接可连点。
- **没有可插入的目标窗口时**（比如在桌面上唤起面板）退回复制到剪贴板并提示，否则「点了没反应」更让人困惑。
- 收藏页仍走剪贴板粘贴：收藏的可能是图片/文件，注入对它们没有意义。
- 注入前还要确认**目标窗口真的在前台**。非激活态下它本来就在前台；只有用户点过搜索框（输入态）时才需要 `ForceForeground` 把前台交还回去，并在注入前**断言 `GetForegroundWindow() == 目标`**，不成立就放弃并提示「目标窗口没有获得焦点，已取消」，绝不盲发。

限制：目标进程以管理员权限运行时注入同样被 UIPI 挡住（走粘贴通道也一样）；IME 正在组字时注入的字符可能被输入法吞掉，此时用 Shift+点击。

## 已知限制

- 目标窗口若以管理员权限运行，普通权限的本程序既收不到它的按键（钩子 UIPI 隔离）也注入不进去。要覆盖这类窗口只能以管理员身份运行本程序。
- UAC 安全桌面、锁屏下钩子天然不生效（这是好事）。
- 历史、置顶、收藏都只存在内存里，退出即清空；没有落盘持久化。
- 图片行显示的是**缩略图**（36px，保持原图宽高比），见「图片缩略图」一节。
- 表情/符号里的「最近」只存内存（每页 80 条），退出即清空。
- 本机字体画不出来的字形（如 Emoji 18.0 新增的少数表情）**不显示**，不是画成方框，见「画不出来的字形直接剔除」。
- 表情/符号按「能不能拿到插入符」分流：能拿到就直插（不碰剪贴板），拿不到就借剪贴板 + `Ctrl+V` 再还原。注入偶尔仍会失效（控件只认粘贴、IME 组字中），此时 **Shift + 点击** 强制走粘贴通道。
- 面板尺寸可拖边缘调整（最小 `360×340`），松手即记住；关掉面板再打开、以及重启软件都会沿用。
- 没有文本插入符的目标窗口（桌面、资源管理器文件列表等）点表情/符号会走粘贴通道：对文件列表等于「无操作」（不会响提示音），此时面板照常收起。
- 面板不抢焦点，目标窗口的插入符全程是真光标、一直在闪。唯一例外是用户主动点了搜索框（输入态，为了输入法），此时光标暂时消失，`Esc` 或关面板即恢复。
- 一个实例（互斥体 `WinVClip_SingleInstance`）。

## 构建

```bat
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

`src/glyph_data.cpp`（字形库，~840KB 字面量）已经生成好并入库，日常编译不用管它；
要换数据（新版 Unicode / 改分类）时用 `python tools/gen_glyph_data.py <参考数据目录> src` 重新生成。

MinGW-w64 g++ 16.2 + CMake + Ninja，链接 `d2d1 dwrite ole32 windowscodecs comdlg32`（界面是 Direct2D 绘制的，早期版本用 GDI 手绘 + `msimg32` 做 AlphaBlend，现已不需要；`windowscodecs` 是解码 PNG 缩略图 / 编码另存为 PNG 用的 WIC；`comdlg32` 提供另存为对话框）。
`-municode` 必须同时出现在编译和链接选项里，否则链接期找不到 `WinMain`。

想开机自启：把 exe 的快捷方式丢进 `shell:startup`。

## 性能（滚动手感）

9000+ 个字形数据本身不是瓶颈，开销全在渲染路径上。同一台机器、同一场景的实测
（560×620 面板，`--perf` 启动，日志在 `%TEMP%\winvclip_perf.log`）：

| 场景 | 最初 | 现在（热缓存） |
|---|---|---|
| 表情页滚动 | **30.1ms/帧**（峰值 429ms） | **2.7ms/帧**（峰值 4ms） |
| 切分类（反复点 chip） | **首帧阻塞 1~2 秒** | **2.7~2.8ms/帧**（峰值 4.2ms） |
| 冷启动首次进表情页 | 同上 | 2.1~3.5ms/帧 |
| 符号页滚动 | 6.1ms/帧 | 2.0~2.6ms/帧 |
| 历史页滚动 | 5.6ms/帧 | 2.2ms/帧 |
| 抽屉滚动 | 4.5ms/帧 | 4.5ms/帧（未改） |

热缓存下 `emoji new=0`（一帧里光栅化的字形数），即滚动、切分类都不再产生任何
字形光栅化。

### 结论一：慢的不是光栅化，是「每格一个新 RenderTarget」

一开始把 emoji 缓存成「每个字形一张独立位图」，仍然卡。用 `tools/bench_emoji.cpp`
逐项对比才定位到真相：

| 方式 | 每格 |
|---|---|
| A 每格 `CreateCompatibleRenderTarget` + Release | 5.3~5.8ms |
| C 复用同一个 RT | 0.76ms |
| E 一次 Begin/End 批量画 9 格 | 0.70ms |
| G 复用 RT（PREMULTIPLIED / 432×432 都一样） | 0.7ms |

创建/销毁离屏目标的代价是光栅化本身的 **7 倍**，且与格子尺寸、像素格式无关。
但即便修掉这一点，真实程序里每格仍是 5ms —— 软件光栅化 COLRv1 在「时序里夹着
主帧」的条件下就是这个量级，一行 9 格 = 45ms，UI 线程怎么排都躲不开。

### 结论二：真正的解法是「别在运行时光栅化」

浏览器渲染几万个 DOM 不卡，靠的不是把光栅化写得更快，而是**缓存 + 只处理可见部分**。
这套字形是编译期就确定的数据，天生适合预计算。现在的结构是三层：

1. **磁盘持久化图集（关键）**：全部 emoji 渲染成一张 atlas 落在
   `%LOCALAPPDATA%\WinVClip\emoji-atlas-v2-<sz>px-<cols>.bin`（约 19MB）。
   下次启动只读文件 + 逐格 `CopyFromMemory`，一个字形都不用再光栅化。
2. **后台全量预热**：首次运行（无缓存）时由 2 个 worker 把全部 1923 个 emoji
   渲染一遍，全部进 atlas 后一次写盘。启动后 120ms 自动开始（`TIMER_WARM`），
   不用等第一次打开面板。实测 1~3 秒跑完。
3. **视口优先队列**：UI 侧的请求（当前可见格 / 预取行）永远排在预热任务之前
   —— worker 先取 `g_emoReq`（LIFO），空了才顺序推进 `g_emoWarmJobs`。
   所以切分类、滚动在任何时刻都是即时的，预热只填空白。

atlas 容量按「全部 emoji 条数 + 8%」定（`cols = ceil(sqrt(1936)) = 46`，
46×46 = 2116 格，2208×2208 px），**装得下就不淘汰**。之前 400 格 + LRU 淘汰
的问题在于「切回旧分类要重新光栅化」，那正是「切分类等一两秒」的另一个来源。

### 关于「黑色变彩色」

未就绪的格子以前画**实心黑单色字形**，跳变刺眼。现在改成 `C_MUTED` + 22% alpha
的极淡剪影，即使真的看到过渡，也是「淡影 → 显色」，基本无感。
配上磁盘缓存，正常情况下这个过渡根本不会出现（首帧就是全彩）。

### 为什么没上显卡加速

D2D 的 `ID2D1DCRenderTarget` 是**纯软件光栅化**（这也是当初选它的原因：直接绑
内存 DIB，配合 `BitBlt` 呈现）。要吃到 GPU 得换 `D3D11 + DXGI flip model` 或
`CreateHwndRenderTarget`，代价是：

- 现有的「内存 DIB + `BitBlt` + `SetWindowRgn` 圆角」呈现链要整体重做；
  flip model 下 `SetWindowRgn` 效果会变，圆角得改用 `DWMWA_WINDOW_CORNER_PREFERENCE`
  或自己做 alpha 合成；
- 面板是静态 UI，只有滚动/输入才重绘，本来就 2~3ms（60fps 预算 16.7ms 的 1/6），
  GPU 化的收益是 0；
- 真正的痛点是「首次生成 1923 个字形」，那是**一次性**成本，用磁盘缓存消掉比
  用 GPU 提速更彻底（GPU 也不会让它变成 0）。

所以这里选了「缓存」而不是「换渲染后端」。如果哪天要做逐帧动画或半透明模糊，
才值得动 GPU 那条路。

### 其他仍然有效的优化

`BindDC` 只做一次、离屏 DIB 复用（每帧省 2.5ms）、按脏区绘制并跳过脏区外的整段
调用、`TextW` 按「字体+字符串」缓存量宽、上传每帧上限 160 格。

线程收尾要注意两点，否则会 `std::terminate`：`StopEmojiWorker()` 必须在
`wWinMain` 返回前调用（join 所有 worker），且要早于释放 atlas / D2D 对象。

### 两个踩过的坑

- **整块 `CopyFromMemory` 会静默出错**。把 19MB 图集一次性 `CopyFromMemory`
  到整个 atlas 区域，返回 `S_OK`，但画出来每格都是「被压扁的邻居」——采样矩形
  明明是对的（`sr=(0,0,44,44)`、`(48,0,92,44)`…逐格验证过）。改成逐格拷贝后
  立即正常。软件 DCRenderTarget 上这个 API 对超大区域不可信，别用。
- **写盘内容必须自己验**。用 `tools/inspect_cache.py` 直接解析缓存文件、把前几格
  导成 PNG，才能区分「写盘错」还是「加载/绘制错」——这次正是靠它确认文件没问题，
  从而把怀疑范围收窄到 `CopyFromMemory`。

探针：`winvclip.exe --perf`（或设 `WINVCLIP_PERF=1`）后每 30 帧往
`%TEMP%\winvclip_perf.log` 追加一行分阶段耗时（begin/prep/head/body/foot/end/blit/total/max），
以及 `warm: posted/cache-hit/saved` 等预热事件。不开时零开销。

## 测试脚本

| 脚本 | 作用 |
|---|---|
| `test_clip.py png\|dib\|alpha\|all` | 往剪贴板塞测试图片（PNG / CF_DIB / 带透明通道） |
| `test_texts.py` | 塞 4 条文本（含链接、命令、长文本） |
| `send_winv.py` | `SendInput` 注入一次真实 Win+V，弹出面板 |
| `test_pin_panel.py` | Win+V 后立刻点标题栏图钉，把面板固定住，便于后续自动化点击/截图 |
| `test_rclick.py <hwnd> <x> <y>` | 向面板投递 `WM_RBUTTONUP`，弹出某行的右键菜单 |
| `test_glyph_ui.py ping` | 弹面板并立刻点图钉固定，后续可稳定点击/截图 |
| `test_glyph_ui.py click X Y` | 向面板投递一次左键单击（客户区坐标） |
| `test_glyph_ui.py wheel -120 X Y` | 投递滚轮事件（抽屉/网格滚动） |
| `test_glyph_ui.py char 7bad` | 直接投递 `WM_CHAR`（绕开输入法），用于测搜索 |
| `test_glyph_ui.py state` | 打印面板句柄、可见性、窗口矩形 |
| `test_ime.py compose weixiao` / `space` / `esc` / `shift` | 用**真实按键**（SendInput VK，非 unicode 注入）喂微软拼音，验证组合串、上屏、取消；`shift` 切输入法中/英模式——这一步不能漏，英文模式下敲字母只是普通字符 |
| `measure_click.py X Y` | 同步投递一次点击并用 `SendMessageTimeout` 计时，等于测「点击 → 界面处理完成」耗时（用来验大类首次加载的成本） |
| `perf_drive.py sym\|emo\|hist\|cats\|drawer [次数]` | 驱动切页 / 连续滚轮 / 切分类 / 抽屉滚动，配合 `WINVCLIP_PERF=1` 采帧耗时 |
| `measure.py emo\|sym\|hist [次数]` | 自己搞定「必要时注入 Win+V 弹面板」，再驱动并直接打印 perf 日志（避开 cmd 时序抖动） |
| `measure_cats.py` | 反复点分类 chip 的压力测试，验证切分类没有长帧 |
| `grab.py <输出.png> [x,y ...] [wN] [sN]` | 弹面板 → 点若干坐标 → 可选滚轮 N 次 → 可选等待 N 毫秒 → 截取窗口存 PNG（失焦前抢拍，用于验收渲染效果） |
| `tools/bench_emoji.cpp` | 彩色字形光栅化基准：逐格建 RT / 复用 RT / 批量 / 尺寸与像素格式对比 |
| `tools/bench2.cpp` | 复现真实渲染时序（主帧 + atlas copy），用于定位「缓存做对了仍慢」的原因 |
| `run_perf.bat` | 以 `WINVCLIP_PERF=1` 启动程序并直接落到后台（日志在 `%TEMP%\winvclip_perf.log`） |
| `test_glyph_insert.py [emo\|sym]` | 起记事本 → Win+V → 切表情/符号页（可选搜索 `cat`）→ 点第一格；断言「剪贴板仍是哨兵」且记事本里出现了该字形 |
| `test_pinned_insert.py` | 固定态连点 3 个表情：面板不消失、剪贴板不动、记事本得到 3 个 emoji |
| `test_fav.py` | `Ctrl+D` 收藏一条（先进输入态，非激活态的 Ctrl 组合已放行）→ 切收藏页点第一行，验证收藏页仍走剪贴板粘贴 |
| `test_resize.py grow\|minh\|state\|reset` | 拖边缘/角落改尺寸、验证最小尺寸、读/删尺寸记忆文件 |
| `test_longpress.py hold\|double\|seq\|once` | 长按 Win+V 回归：`hold 2` 期间注入 66 个重复键，可见性只应变化 1 次（不应反复闪烁） |
| `test_noactivate.py [窗口标题子串]` | 核心回归：面板弹出前后前台是否仍是目标窗口、真实插入符是否还在原位、面板开着打字是否**不进**目标窗口、`Esc` 后能否继续打字 |
| `test_noactivate_keys.py [窗口标题子串]` | 非激活态下按 `Tab` 切页、`Enter` 粘贴、鼠标点条目粘贴，每步断言面板与前台状态、插入符是否前进 |
| `test_hook_passthrough.py` | 钩子接管范围回归：先起 `hook_listen.py` 装一个外部 `WH_KEYBOARD_LL`、再重启本程序（让本程序的钩子排在它前面，等价于用户机器上「别家工具先开机」的顺序）。断言非激活态下 `Ctrl+Shift+Q`/`F5` 外部钩子能收到、裸字母收不到但面板收到了，输入态下按键照样透传，非激活态 `Esc` 被吞且面板收起 |
| `test_inputmode.py` | 点搜索框 → 面板真正拿到前台（输入态）→ `Esc` 收起并把前台还给目标窗口 |
| `test_explorer_no_caret.py` | 资源管理器（没有文本输入区）里点表情：断言走粘贴通道、面板收起、剪贴板还原、前台不变 |
| `test_glyph_channel.py notepad\|web\|explorer [shift]` | 两条插入通道：直插目标剪贴板不动；无插入符目标借剪贴板粘完还原；`shift` 强制走粘贴 |
| `test_addrbar.py` | 在资源管理器**地址栏**里 Win+V → Enter 粘第一条，再 `Ctrl+A/Ctrl+C` 读回地址栏内容（不再需要任何焦点恢复补丁） |
| `test_focus_after_close.py` | 地址栏输入 → Win+V → Esc → 继续输入，读回验证焦点回到地址栏 |
| `test_hide_outside.py` | 收起判据的另一半：点面板外的**同一个窗口**（前台不变）应收起且目标窗口仍是前台、插入符还在；点面板内部不收起；输入态点别的窗口收起且**那个窗口**拿到前台；固定态点外面不收起；拖标题栏移出面板区不收起 |
| `test_edge_outside.py` | 上面那条的真实场景版：在 Edge 网页里 Win+V → 点正文 → 面板收起、Edge 仍是前台、能再次唤起 |
| `tools/gen_glyph_data.py <数据目录> src` | 从参考页面的 symbols.js / emoji.js 重新生成 `glyph_data.cpp` |
| `tools/uia_probe.ps1` | 只读诊断：打印前台窗口 / UIA 焦点元素的 pid、类型、矩形、TextPattern 选区矩形。排查「某个输入框为什么读不到插入符」就靠它 |
| `tools/uia_diag.cpp` → `uia_diag.exe [hwnd]` | 更细的只读诊断：焦点元素的全部文本通道（`GetCaretRange` / `GetSelection` / `RangeFromPoint` / 字符级扩展）与各自的矩形，加 pattern 可用性。排查「读到整框 / 退化矩形」用这个。编译：`g++ -std=c++17 -O2 tools/uia_diag.cpp -o tools/uia_diag.exe -luiautomationcore -lole32 -loleaut32` |
| `tools/caret_big*.html` | 大输入框探针页：大 textarea（多行）、空 contenteditable（placeholder）、滚动长文 contenteditable、iframe 内 textarea |
| `tools/quick_probe.py <X> <Y> [收起]` | 点一下屏幕坐标 → 发 Win+V → 打印本次 trace。用于验证落点 |
| `tools/caret_multi.py [次数]` | 对网页逐个 Tab 聚焦并逐次 Win+V，打印每次的插入符探测结果 |
| `tools/caret_probe*.html` | 插入符探针页：普通 input / contenteditable / textarea / iframe 内 input / 屏幕中部与底部的输入框 |

自动化点击面板请用 `PostMessage` 投递 `WM_LBUTTONDOWN/UP`（客户区坐标）：`SendInput` 的鼠标移动不会给窗口送 `WM_MOUSEMOVE`，而真实拖拽又会打断面板。

自动化脚本里跨进程切前台一律先校验 `GetForegroundWindow()`：`SetForegroundWindow` 常被前台锁静默拒绝，不校验就会把操作打到别的窗口上（`test_glyph_insert.py` / `test_pinned_insert.py` 里的 `force_fg()` 就是为此加的）。

### 诊断日志

前台 / 焦点类问题不好靠猜，可以设环境变量 `WV_TRACE=1` 启动，日志写进 `%TEMP%\winvclip-trace.txt`：

```
hook: downV -> toggle (fg=... vis=0)
=== ShowPanel enter fg=... vis=0
  after SWP_SHOWWINDOW fg=... vis=1
  ForceForeground(...) enter fg=...
  after ForceForeground fg=... vis=1
WM_ACTIVATE wp=0 fg=...
WM_APP_DEFER_HIDE fg=...
FocusStillOurs: rescue #1 (fg=...)
HidePanel() from <返回地址> fg=...
```

默认关闭时只多一次 `getenv`，正常使用的开销可忽略。

## 卸载/恢复系统行为

本程序**不修改任何注册表**，退出（托盘右键退出）后系统剪贴板历史与 Win+V 立刻恢复原样。

## 许可

MIT License，见 [LICENSE](LICENSE)。
