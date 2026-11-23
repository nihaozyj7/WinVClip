# -*- coding: utf-8 -*-
"""P1：把 Lower/U8N/NextCp/W2U8/FileNameOf/StampName 从 main.cpp 切除（已迁入 text_util.cpp）。"""
import io

MAIN = "src/main.cpp"


def read(p):
    return io.open(p, "r", encoding="utf-8", newline="").read()


def write(p, t):
    io.open(p, "w", encoding="utf-8", newline="").write(t)


def cut_between(src, head, tail, new, tag, eol):
    a = src.find(head)
    b = src.find(tail)
    if a < 0 or b < 0 or b < a:
        raise SystemExit("[%s] 定位失败" % tag)
    b += len(tail)
    chunk = src[a:b]
    if chunk.count(head) != 1:
        raise SystemExit("[%s] head 不唯一" % tag)
    print("[%s] 切掉 %d 行" % (tag, chunk.count(eol) + 1))
    return src[:a] + new + src[b:]


def main():
    src = read(MAIN)
    eol = "\r\n" if "\r\n" in src else "\n"

    # 1) include
    old = '#include "theme.h"' + eol + "using namespace wv;"
    if src.count(old) != 1:
        raise SystemExit("[include] 未命中")
    src = src.replace(old, old + eol + '#include "text_util.h"', 1)
    print("[include] ok")

    # 2) Lower + U8N
    src = cut_between(
        src,
        "static std::wstring Lower(std::wstring s) {",
        '    if (w > 0) MultiByteToWideChar(CP_UTF8, 0, s, n, &out[0], w);' + eol + "    return out;" + eol + "}",
        "// 字符串 / 编码转换见 text_util.h（Lower / U8N / NextCp / W2U8 / FileNameOf / StampName）",
        "Lower+U8N", eol)

    # 3) NextCp
    src = cut_between(
        src,
        "static UINT32 NextCp(const wchar_t* s, int& i, int n) {",
        "    return c;" + eol + "}",
        "// NextCp（代理对安全取码位）见 text_util.h",
        "NextCp", eol)

    # 4) W2U8 / FileNameOf / StampName
    src = cut_between(
        src,
        "static std::string W2U8(const std::wstring& s) {",
        "    return b;" + eol + "}",
        "// W2U8 / FileNameOf / StampName 见 text_util.h",
        "W2U8+FileNameOf+StampName", eol)

    write(MAIN, src)
    print("done")


if __name__ == "__main__":
    main()
