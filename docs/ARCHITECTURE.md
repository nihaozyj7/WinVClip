# WinVClip 架构

一个 Win32 自绘的剪贴板面板：全局钩子接管 Win+V，弹出非激活面板，
选中条目后写回剪贴板并粘贴到原来的窗口。

它不是库，没有对外 API，所以**没有 `include/` 与 `src/` 的分离**——
那种分离的意义是让别的项目能复用你的公开接口，这里不存在这个诉求。
取而代之的是**按层分目录**，把「谁能 include 谁」变成一眼可见的事实。

---

## 目录与依赖方向

依赖严格单向，下层不得 include 上层。这条规则写在 `CMakeLists.txt` 顶部，
新增文件时按层放置即可，不要为了图方便反向引用。

```
src/
├── core/        零依赖。只依赖 Win32 与 STL
│   ├── theme.h          配色 / 尺寸 / 窗口类名。改视觉只看这一个文件
│   ├── text_util.*      字符串与编码（Lower / U8N / W2U8 …）
│   ├── text_measure.*   Fmt / TextW：GDI 字体令牌 ↔ DWrite 文本格式的对应与宽度缓存
│   ├── diag.*           Trace / WarmLog / PerfAppend：写 %TEMP% 的调试日志
│   ├── io_png.*         WIC 工厂与 PNG 编解码
│   ├── util.*           文件名 / 相对时间 / 可读大小 / 字节计数 / DPI 缩放 S()
│   ├── model.h          ClipItem / GItem / GCat / Thumb 等纯数据结构
│   └── app_state.*      全部可变全局状态的唯一定义处（见下文）
├── render/      → core
│   ├── dc.*             D2D 绘制原语（batch 边界、圆角、文字出口）
│   ├── path.h           D2D 几何路径包装 PathB
│   ├── icons.*          24×24 线性图标
│   └── fonts.*          字体与 D2D/DWrite 设备初始化
├── image/       → core, render
│   ├── raster.*         剪贴板原始图像 → 统一 BGRA；降采样、圆角蒙版
│   └── thumb.*          缩略图的惰性构建与缓存
├── glyph/       → core, render
│   ├── glyph.*          字形库：解析、索引、搜索、可画性判定
│   ├── glyph_data.cpp   生成文件（835 KB），由 tools/gen_glyph_data.py 产出
│   └── emoji_atlas.*    彩色 emoji 图集：后台光栅化 + 磁盘缓存 + 视口优先
├── clipboard/   → core, render, image
│   ├── capture.*        监听剪贴板并采集历史
│   └── paste.*          两条回写通道（直插 Unicode / 借剪贴板 + Ctrl+V）
├── model/       → core, glyph
│   └── clip_model.*     过滤/搜索/排序与 g_view 重建
├── ui/          → 以上全部
│   ├── layout.*         所有矩形的唯一计算处 + PtIn
│   ├── paint.*          全部绘制（纯读者）
│   ├── caret.*          目标窗口插入符与焦点框探测（UIA）
│   ├── panel.*          面板窗口过程、键盘输入、拖动缩放
│   ├── actions.*        用户动作的执行（粘贴/置顶/收藏/另存为/切页）
│   └── showhide.*       显示、隐藏与落点（「失焦即隐藏」的全部时序）
└── app/         → 全部
    ├── hook.*           全局低级键盘/鼠标钩子 + 按键白名单
    ├── tray.*           托盘图标
    └── main.cpp         只剩 MainProc / InitData / wWinMain
```

---

## 「我要改 X，该动哪个文件」

| 诉求 | 文件 |
| --- | --- |
| 改配色、圆角、间距、面板默认尺寸 | `core/theme.h` |
| 改某个区域的几何位置 | `ui/layout.cpp` 的 `ComputeLayout` |
| 改某个区域的画法 | `ui/paint.cpp` 对应的 `PaintXxx` |
| 加一个图标 | `render/icons.h` 的 `IconId` + `icons.cpp` 的 `DrawIcon` case |
| 点某个位置的响应不对 | `ui/panel.cpp` 的 `PanelProc`（只看该 case） |
| 改「粘贴」这个动作本身 | `ui/actions.cpp` 的 `PasteItem` / `PasteGlyph` |
| 面板该不该收起、收起到哪去 | `ui/showhide.cpp` |
| 按键不生效 / 吞到了别家热键 | `app/hook.cpp` 的 `PanelConsumesBareKey` 与 `LLKeyboard` |
| 插入符找不到、面板位置飘 | `ui/caret.cpp` |
| 剪贴板没记录 / 重复条目 | `clipboard/capture.cpp` |
| 搜索、筛选、排序不对 | `model/clip_model.cpp` |
| 表情格子空着、切分类要等 | `glyph/emoji_atlas.cpp` |
| 字形缺字、显示成方框 | `glyph/glyph.cpp` 的可画性判定 + `fonts.cpp` 的字体兜底 |

---

## 三条跨模块约定

### 1. 状态只有一个定义处：`core/app_state.h`

所有可变全局都在这里 `extern`，定义在 `app_state.cpp`。文件里按写入者分组，
每组注明**唯一**允许写它的模块：

- `hook.cpp` 独占键盘状态（`g_ctrlDown` / `g_vDown` / `g_swallowed` …）
- `clip_model.cpp` 独占 `g_items` 与 `g_view`
- `panel.cpp` 独占选中、滚动、悬停
- `paint.cpp` 只读

想加一个全局状态，先在 `app_state.h` 找到它属于哪一组；找不到组就别加，
说明这个状态该是某个模块的内部成员。

### 2. 绘制是纯读者，布局是唯一几何来源

`paint.cpp` 不改任何状态（唯一例外是 `PerfFlush` 清性能计数），
`layout.cpp` 是矩形坐标的唯一计算处。所以：

- 画出来位置不对 → 看 `layout.cpp`
- 位置对但颜色/文字/图标不对 → 看 `paint.cpp`
- 看着对但点不到 → 还是 `layout.cpp`（绘制与命中共用同一份 `L`）

### 3. 不可逆的时序约束写在头文件里

`ui/showhide.h`、`ui/panel.h`、`app/hook.h` 的开头都是「为什么必须这样做」，
不是「这段代码干什么」。它们记录的是踩过的坑：

- 不能在失焦后抢回前台（抢回来之后再没有失活事件，面板会永久挂在最上层）
- 钩子是串行链，`return 1` 会饿死排在后面的别家全局钩子
- 修饰键状态必须自己跟踪（吞掉 keyup 后系统不再更新键盘状态）
- 自己 SendInput 的键要打 `g_injectMark` 放行，否则会被自己的钩子吞掉

改这些文件之前先读那几段话和对应的回归用例，比读实现划算。

---

## 新增代码的检查清单

1. 它属于哪一层？只看该层允许的 include（见上面的目录树）。
2. 需要新状态吗？先查 `app_state.h` 有没有现成的组。
3. 需要新矩形吗？加进 `Layout`，在 `ComputeLayout` 里算。
4. 需要新动作吗？放进 `ui/actions.cpp`，不要在 `PanelProc` 的 case 里写逻辑
   （同一个动作常被菜单、快捷键、鼠标三条路径触发）。
5. 改完跑：`cmake --build build`（要求零警告）+ `python tests\run.py`（desktop 组约 100 秒）。

## 测试层

| 文件 | 职责 |
|---|---|
| `tests/wvclip.py` | 公共层：Win32 绑定（argtypes 齐全）、键鼠注入、剪贴板、窗口查询、断言辅助 |
| `tests/run.py` | runner：发现/分组/超时/汇总，零第三方依赖 |
| `tests/test_*.py` | 用例，也能单独 `python tests\test_xxx.py` 运行 |

分组表只在 `run.py` 顶部维护：desktop（自带前置，默认跑）/ manual（需外部条件）/
driver（造数据工具，非用例）。新增用例只写自己的用例体，键鼠与窗口操作一律走公共层——
`post_click`（消息投递，驱动 NOACTIVATE 面板）与 `real_click`（真实鼠标，验证低级钩子）
语义不同，不能混用。详见 `tests/README.md`。
