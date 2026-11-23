# -*- coding: utf-8 -*-
# 重写 README 的「性能（滚动手感）」一节
import io

P = "README.md"
src = io.open(P, "r", encoding="utf-8", newline="").read()
CRLF = "\r\n" in src
src = src.replace("\r\n", "\n")

A = "## 性能（滚动手感）"
B = "## 测试脚本"
i = src.find(A)
j = src.find(B)
assert i > 0 and j > i

NEW = """## 性能（滚动手感）

9000+ 个字形数据本身不是瓶颈，开销全在渲染路径上。同一台机器、同一场景的实测
（560×620 面板，`--perf` 启动，日志在 `%TEMP%\\winvclip_perf.log`）：

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
   `%LOCALAPPDATA%\\WinVClip\\emoji-atlas-v2-<sz>px-<cols>.bin`（约 19MB）。
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
`%TEMP%\\winvclip_perf.log` 追加一行分阶段耗时（begin/prep/head/body/foot/end/blit/total/max），
以及 `warm: posted/cache-hit/saved` 等预热事件。不开时零开销。

"""

src = src[:i] + NEW + src[j:]
out = src.replace("\n", "\r\n") if CRLF else src
io.open(P, "w", encoding="utf-8", newline="").write(out)
print("readme ok")
