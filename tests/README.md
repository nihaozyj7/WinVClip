# tests/ —— 回归测试

每个脚本都是**独立可运行**的：`python tests/test_xxx.py`。
脚本开头会自动 `chdir` 到项目根，所以内部沿用 `build\winvclip.exe` 这类相对路径。

## 运行前提

- 先构建：`cmake --build build`（产物 `build\winvclip.exe`）
- 大部分用例会**真的弹面板、模拟输入、操作记事本/资源管理器**，是端到端测试，运行时会抢前台，请勿在打字时执行
- 少数用例需要 `pip install pillow`（`test_clip.py`）

## 分类

### 一、断言型回归（失败会 `raise SystemExit` / `assert`）

| 脚本 | 覆盖的行为 |
|---|---|
| `test_addrbar.py` | 资源管理器地址栏唤出面板并粘贴，内容真的进地址栏 |
| `test_edge_outside.py` | Edge 网页里呼出面板后，同窗口内点掉面板即收起 |
| `test_explorer_no_caret.py` | 无可编辑区（文件列表）里点表情，走「借剪贴板 + Ctrl+V」通道 |
| `test_fav.py` | Ctrl+D 收藏 → 收藏页 → 点第一行粘贴到目标窗口 |
| `test_focus_after_close.py` | 关闭面板后地址栏焦点归还，能继续打字 |
| `test_glyph_channel.py` | 表情/符号两条插入通道的分流正确 |
| `test_glyph_insert.py` | 点表情直插目标窗口且剪贴板不动 |
| `test_glyph_ui.py` | 字形页渲染与点击命中 |
| `test_hide_outside.py` | 点面板外部即收起（低级鼠标钩子通道） |
| `test_hook_passthrough.py` | 面板显示时不饿死别家全局键盘钩子（`hook_listen.py` 为配套记录器） |
| `test_ime.py` | 真实输入法组合串与候选窗定位 |
| `test_inputmode.py` | 点搜索框才激活面板的输入态模型 |
| `test_longpress.py` | 长按 Win+V 只切换一次，不闪烁 |
| `test_noactivate.py` | 非激活（WS_EX_NOACTIVATE）焦点模型 |
| `test_noactivate_keys.py` | 非激活面板下的键盘转发与粘贴链路 |
| `test_pinned_insert.py` | 临时固定态下连点多个表情，面板不消失、剪贴板不动 |
| `test_resize.py` | 面板尺寸调整与记忆 |

### 二、驱动器（不是测试，是给上面那些用例造数据的工具）

这几个没有断言，命名却带 `test_` 前缀，属于历史遗留。**改名会牵连其他脚本的调用，暂保留原名**：

| 脚本 | 实际作用 |
|---|---|
| `test_clip.py` | 往剪贴板灌测试图片（`png` / `dib` / `alpha` / `all`） |
| `test_texts.py` | 依次往剪贴板灌若干条文本 |
| `test_pin_panel.py` | 弹面板并立刻点「临时固定」，便于后续自动化 |
| `test_rclick.py` | 直接向面板投递 `WM_RBUTTONUP`，绕开鼠标钩子语义干扰 |

## 维护约定

- 新增用例必须**自己造数据、自己清理**（关掉启动的记事本 / 退出被拉起的程序），不留后台进程
- 涉及面板可见性的用例，先调 `test_pin_panel.py` 固定面板，否则失焦即隐藏会让断言随机失败
- 与具体实现细节耦合的断言（像素坐标、窗口类名）改代码时要同步改，不要为了让它过而放宽断言
