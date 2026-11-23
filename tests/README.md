# tests/ —— 回归测试

```bat
cmake --build build          :: 先构建（winvclip.exe 运行中无法覆盖链接）
python tests\run.py          :: 跑 desktop 组，约 100 秒
python tests\run.py --all    :: 连造数据的 driver 一起跑
python tests\run.py --list   :: 看分组
```

## 三个文件各管什么

| 文件 | 职责 |
|---|---|
| `wvclip.py` | **公共层**。Win32 绑定、键鼠注入、剪贴板、窗口查询、断言辅助。所有用例共享这一份。 |
| `run.py` | runner。发现 / 分组 / 超时 / 汇总。零第三方依赖（本机没装 pytest，也不该为此引入）。 |
| `test_*.py` | 用例。单个文件也能直接 `python tests/test_xxx.py` 跑。 |

`wvclip.py` 里三条硬规则写在文件头，改它之前先读：

1. 所有 Win32 函数在导入时声明 `argtypes/restype`（默认 `c_int` 会把 64 位句柄截断）；
2. `post_click`（PostMessage，驱动 NOACTIVATE 面板）与 `real_click`（真实鼠标，验证低级钩子）**不能混用**；
3. 导入即 chdir 到项目根，脚本里直接用 `build\winvclip.exe` 这类相对路径。

## 分组

`run.py` 顶部一张表维护，不在脚本里散落标记：

- **desktop**（默认跑）：自己带前置条件（自起记事本/资源管理器），可无人值守
- **manual**：需要外部条件或参数（Edge 窗口、输入法状态、指定目标、会改面板尺寸）
- **driver**：不是用例，是造数据的工具（灌剪贴板、临时固定面板、投递右键）

表里没登记的文件默认按 desktop 跑——漏登记的代价是"多跑一个"，不会漏跑。

## 写新用例

```python
# -*- coding: utf-8 -*-
"""一句话说清验证什么，以及为什么需要这个用例（踩过什么坑）。"""

import wvclip as w
from wvclip import *  # noqa: F401,F403

def main():
    proc, np = start_notepad()          # 自带前置，测试结束 kill(proc)
    try:
        set_clip("SENTINEL-123")        # 用哨兵值断言"没碰剪贴板"
        win_v(); time.sleep(0.8)
        p = panel()                     # 等面板出现（超时抛 SystemExit）
        post_click(p, 60, 17)           # 面板交互一律消息投递
        chk(get_clip() == "SENTINEL-123", "剪贴板未被污染")
        report()                        # 有失败项则退出非 0
    finally:
        kill(proc)

if __name__ == "__main__":
    main()
```

**不要**再往用例里抄 ctypes 结构体和 `key/tap/set_clip/force_fg`。需要新能力就加到 `wvclip.py`，并在 `__all__` 里导出。

断言优先用 `need`（失败即停，用于前置条件）或 `chk` + `report`（跑完全部再汇总）。

## 已知的"假失败"

这几条不是回归，先确认再改代码：

| 现象 | 真凶 |
|---|---|
| 前台不是预期窗口 | 截图工具（`SnapCppOverlay`）残留覆盖窗；或 Edge 持有前台锁拒绝 `SetForegroundWindow` |
| 读到的剪贴板/文本是别的东西 | 有多个记事本实例，用例操作的那个不是前台那个 |
| `test_noactivate_keys` 历史页按 Enter 无反应 | 历史页为空（没有可粘的条目），基线上同样如此 |
| `test_glyph_insert` 记事本为空 | 本机 emoji 字体/前台时序问题，基线上同样为空 |
| 面板截图拍到下层窗口 | 面板已失焦收起，或 `WS_EX_LAYERED` 抓不到：先 `test_pin_panel.py` 固定 |

判断方法固定为：**打印 `fg_desc()` 看前台到底是谁**，再决定是环境还是回归；必要时用
`git show HEAD:tests/test_xxx.py > tests/_old.py` 跑基线脚本对比行为。

## 维护约定

- 新增用例自己造数据、自己清理（关掉启动的记事本/资源管理器），不留后台进程
- 涉及面板可见性的用例，先固定面板（`test_pin_panel.py` 或点图钉），否则失焦即隐藏会让断言随机失败
- 与实现细节耦合的断言（像素坐标、窗口类名）改代码时要同步改，**不要为了让它过而放宽断言**
- 不许在脚本里写死窗口句柄（`test_edge_outside.py` 曾写死作者机器的 hwnd，换机器必挂）——按类名 + 进程名查
