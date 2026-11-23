# -*- coding: utf-8 -*-
"""P1：性能探针与诊断日志迁入 diag.cpp 后，从 main.cpp 切除原定义。

注意 PerfFlush 故意保留在 main.cpp —— 它要把「当前页 + 表情命中率」一起写出，
依赖业务状态；搬进 diag 会造成 diag → emoji 的反向依赖。
"""
import io

MAIN = "src/main.cpp"


def read(p):
    return io.open(p, "r", encoding="utf-8", newline="").read()


def write(p, t):
    io.open(p, "w", encoding="utf-8", newline="").write(t)


def cut_between(src, head, tail, new, tag, eol):
    a = src.find(head)
    if a < 0:
        raise SystemExit("[%s] head 未找到" % tag)
    b = src.find(tail, a)
    if b < 0:
        raise SystemExit("[%s] tail 未找到" % tag)
    b += len(tail)
    chunk = src[a:b]
    if chunk.count(head) != 1:
        raise SystemExit("[%s] head 不唯一" % tag)
    print("[%s] 切掉 %d 行" % (tag, chunk.count(eol) + 1))
    return src[:a] + new + src[b:]


def main():
    src = read(MAIN)
    eol = "\r\n" if "\r\n" in src else "\n"

    old = '#include "text_util.h"'
    if src.count(old) != 1:
        raise SystemExit("[include] 未命中")
    src = src.replace(old, old + eol + '#include "diag.h"', 1)
    print("[include] ok")

    # 1) PerfAcc + 全局 + PerfNow/PerfMs/前向声明 + WarmLog
    src = cut_between(
        src,
        "// ------------------------------------------------ 轻量性能探针（底层，供绘制路径使用）",
        "    PerfAppend(buf);" + eol + "}",
        eol.join([
            "// ------------------------------------------------ 性能探针 / 诊断日志",
            "// 实现在 diag.cpp：PerfAcc / g_perfOn / g_perfFreq / PerfNow / PerfMs /",
            "// PerfAppend / PerfInit / WarmLog / Trace。汇总输出 PerfFlush 仍在下方，",
            "// 因为它要连同当前页与表情命中率一起写，属于业务侧的知识。",
        ]),
        "perf-core", eol)

    # 2) PerfAppend + PerfInit 的实现（PerfFlush 保留）
    src = cut_between(
        src,
        "// ------------------------------------------------ 性能探针：落盘",
        "    g_perfOn = g_perfFreq.QuadPart != 0;" + eol + "}",
        "// PerfAppend / PerfInit 见 diag.cpp；下面是带业务语义的汇总输出。",
        "perf-write", eol)

    # 3) Trace 实现
    src = cut_between(
        src,
        "static void Trace(const char* fmt, ...) {",
        "    fputc('\\n', f);" + eol + "    fclose(f);" + eol + "}",
        "// Trace 见 diag.cpp（WV_TRACE=1 时写 %TEMP%\\winvclip-trace.txt）",
        "trace", eol)

    write(MAIN, src)
    print("done")


if __name__ == "__main__":
    main()
