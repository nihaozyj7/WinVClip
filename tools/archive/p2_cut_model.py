# -*- coding: utf-8 -*-
"""P2：数据结构（Thumb / ClipItem / GItem / GCat / RecentGlyph）迁入 model.h。"""
import io

MAIN = "src/main.cpp"


def main():
    src = io.open(MAIN, "r", encoding="utf-8", newline="").read()
    eol = "\r\n" if "\r\n" in src else "\n"

    head = "// 列表里的图片缩略图（惰性构建，ClipItem 之间靠 shared_ptr 共享："
    tail = "struct RecentGlyph { std::string g, n; };  // 最近使用：字形 + 名称（UTF-8）"
    a = src.find(head)
    b = src.find(tail)
    if a < 0 or b < 0 or b < a:
        raise SystemExit("[model] 定位失败 a=%d b=%d" % (a, b))
    b += len(tail)
    chunk = src[a:b]
    print("[model] 切掉 %d 行" % (chunk.count(eol) + 1))

    src = src[:a] + "// Thumb / ClipItem / GItem / GCat / RecentGlyph 见 model.h" + src[b:]

    old = '#include "io_png.h"'
    if src.count(old) != 1:
        raise SystemExit("[include] 未命中")
    src = src.replace(old, old + eol + '#include "model.h"', 1)

    io.open(MAIN, "w", encoding="utf-8", newline="").write(src)
    print("done")


if __name__ == "__main__":
    main()
