# tools/archive —— 已执行完毕的一次性脚本

这里的脚本**不需要再运行**，保留只为追溯"某次结构调整是怎么做的"。
定期清理时可以直接删（git 历史里随时可取），不要在这里加新东西。

| 脚本 | 当时做的事 |
|---|---|
| `refactor_tests.py` | 把 `tests/*.py` 里重复的 ctypes 样板收敛到 `tests/wvclip.py` |
| `p0_fix_paths.py` | P0 目录归位后，把测试脚本的路径基准重新锚到项目根 |
| `p0_list_tests.py` | 列出每个测试的用途（取 docstring 首行），用于整理时分类 |
| `p1_cut.py` | 从 `main.cpp` 原位置删除已独立成文件的代码块 |
| `p1_cut2.py` | 把 `Lower/U8N/NextCp/W2U8/FileNameOf/StampName` 迁入 `core/text_util.cpp` |
| `p1_cut3.py` | 性能探针与诊断日志迁入 `core/diag.cpp` |
| `p1_cut4.py` | `WIC/Rgba/PNG` 落盘迁入 `core/io_png.cpp` |
| `p2_cut_model.py` | 数据结构（`Thumb/ClipItem/GItem/GCat/RecentGlyph`）迁入 `core/model.h` |
| `patch_emojicache.py` | 把彩色字形缓存整段替换为「磁盘 atlas + 后台预热 + 视口优先队列」 |
| `patch_pump.py` | 把上传/落盘从 `PrepareEmojiBitmaps` 抽成 `PumpEmojiUploads` |
| `patch_readme.py` | 重写 README 的「性能（滚动手感）」一节 |

这些脚本的共同套路：按实际行尾（CRLF）构造 old/new，`assert` 命中数 == 1，
写回时 `newline=''` 保持原样。要再做同类改动时，抄这个套路比手写 `edit` 安全。

## 仍然有用、留在 tools/ 的

| 脚本 | 用途 |
|---|---|
| `split.py` | 按行号区间把源文件里的代码块剪切到新模块（通用） |
| `analyze_struct.py` | 统计 `main.cpp` 的顶层函数长度、全局变量数、段标题分布 |
| `gen_glyph_data.py` | 生成 `src/glyph/glyph_data.cpp`（**输入不在仓库**，见项目记忆） |
| `check_names.py` | 静态检查 `tests/*.py` 里是否有未定义名字 |
| `bench_emoji.cpp` / `bench2.cpp` / `run_perf.bat` | 性能基准 |
| `caret_multi.py` / `seq_diag.py` / `quick_probe.py` / `measure*.py` / `perf_drive.py` / `send_winv.py` / `grab.py` / `inspect_cache.py` / `click_diag.py` | 插入符、时序、落点、耗时的现场诊断 |
| `uia_diag.cpp` / `uia_diag.exe` / `probe_uia_caret.ps1` / `uia_probe.ps1` | UIA 插入符探测实验 |
| `caret_*.html` | 浏览器插入符探测用的测试页面 |
