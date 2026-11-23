# -*- coding: utf-8 -*-
"""P1：WIC / Rgba / PNG 落盘迁入 io_png.cpp 后，从 main.cpp 切除原定义。"""
import io

MAIN = "src/main.cpp"


def read(p):
    return io.open(p, "r", encoding="utf-8", newline="").read()


def write(p, t):
    io.open(p, "w", encoding="utf-8", newline="").write(t)


def cut_between(src, head, tail, new, tag, eol):
    a = src.find(head)
    if a < 0:
        raise SystemExit("[%s] head 未找到: %r" % (tag, head[:40]))
    b = src.find(tail, a)
    if b < 0:
        raise SystemExit("[%s] tail 未找到: %r" % (tag, tail[:40]))
    b += len(tail)
    chunk = src[a:b]
    if chunk.count(head) != 1:
        raise SystemExit("[%s] head 不唯一" % tag)
    print("[%s] 切掉 %d 行" % (tag, chunk.count(eol) + 1))
    return src[:a] + new + src[b:]


def main():
    src = read(MAIN)
    eol = "\r\n" if "\r\n" in src else "\n"

    old = '#include "diag.h"'
    if src.count(old) != 1:
        raise SystemExit("[include] 未命中")
    src = src.replace(old, old + eol + '#include "io_png.h"', 1)
    print("[include] ok")

    # 1) 匿名 namespace 里的 Rgba + 4 个 GUID
    src = cut_between(
        src,
        "namespace {" + eol + eol + "struct Rgba { int w = 0, h = 0; std::vector<BYTE> px; };",
        "                             {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x10}};" + eol + eol + "}  // namespace",
        "// Rgba 与 WIC 像素格式 GUID 见 io_png.h",
        "rgba+guids", eol)

    # 2) g_wic + InitWic
    src = cut_between(
        src,
        "static IWICImagingFactory* g_wic = nullptr;",
        "kIidWicFactory, (void**)&g_wic);" + eol + "}",
        "// g_wic / InitWic 见 io_png.cpp",
        "wic-factory", eol)

    # 3) WriteAllBytes
    src = cut_between(
        src,
        "static bool WriteAllBytes(const std::wstring& path, const void* data, size_t n) {",
        "    CloseHandle(h);" + eol + "    return ok;" + eol + "}",
        "// WriteAllBytes 见 io_png.cpp",
        "write-all-bytes", eol)

    # 4) SaveRgbaAsPng
    src = cut_between(
        src,
        "static bool SaveRgbaAsPng(const Rgba& img, const std::wstring& path) {",
        "    if (st) st->Release();" + eol + "    return ok;" + eol + "}",
        "// SaveRgbaAsPng 见 io_png.cpp",
        "save-png", eol)

    # 5) UniquePath
    src = cut_between(
        src,
        "// 目标已存在时自动加 (1)(2)…，避免多文件复制时互相覆盖" + eol + "static std::wstring UniquePath(const std::wstring& p) {",
        "        if (GetFileAttributesW(c.c_str()) == INVALID_FILE_ATTRIBUTES) return c;" + eol + "    }" + eol + "    return p;" + eol + "}",
        "// UniquePath 见 io_png.cpp",
        "unique-path", eol)

    write(MAIN, src)
    print("done")


if __name__ == "__main__":
    main()
