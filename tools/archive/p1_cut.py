# -*- coding: utf-8 -*-
"""P1 源码拆分：把 main.cpp 里已独立成文件的代码块原位删除。

每次只做一处替换，assert 命中数 == 1，避免无声误删。
按实际行尾（CRLF）构造 old/new，写回时用 newline='' 保持原样。
"""
import io
import sys

MAIN = "src/main.cpp"


def read(path):
    return io.open(path, "r", encoding="utf-8", newline="").read()


def write(path, text):
    io.open(path, "w", encoding="utf-8", newline="").write(text)


def cut(src, old, new, tag):
    n = src.count(old)
    if n != 1:
        raise SystemExit("[%s] 期望命中 1 处，实际 %d 处" % (tag, n))
    print("[%s] ok" % tag)
    return src.replace(old, new, 1)


def main():
    src = read(MAIN)
    eol = "\r\n" if "\r\n" in src else "\n"

    # ---- 1) 常量块 -> theme.h
    old = eol.join([
        "// ================================================================== 常量",
        'static const wchar_t* MAIN_CLASS  = L"WinVClipMain";',
        'static const wchar_t* PANEL_CLASS = L"WinVClipPanelUI";',
    ])
    # 从常量标题一直到 RESIZE_EDGE 那行整段切掉（中间夹杂注释与空行，
    # 直接按首尾定位，避免逐行抄写）。
    start = src.find(old)
    key_end = "static const int RESIZE_EDGE = 5;     // 边缘可抓取的带宽（px）"
    end = src.find(key_end)
    if start < 0 or end < 0 or end < start:
        raise SystemExit("[theme] 定位失败 start=%d end=%d" % (start, end))
    end += len(key_end)
    block = src[start:end]
    print("[theme] 切掉 %d 字符 / %d 行" % (len(block), block.count(eol) + 1))
    new_block = eol.join([
        "// ================================================================== 常量",
        "// 视觉常量（配色 / 尺寸 / 窗口类名）集中在 theme.h，改配色只看那一个文件。",
        '#include "theme.h"',
        "using namespace wv;",
    ])
    src = src[:start] + new_block + src[end:]

    write(MAIN, src)
    print("done")


if __name__ == "__main__":
    main()
