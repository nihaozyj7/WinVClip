# -*- coding: utf-8 -*-
"""P0 辅助：把 tests/ 下的测试脚本的路径基准重新锚到项目根。

这些脚本原本都假定「当前工作目录 = 项目根」，用 build\\winvclip.exe 这类
相对路径定位被测程序。移进 tests/ 后不再成立，因此统一在 import 区
之前注入「切回项目根」的两行。

对已经有了 HERE 的脚本，额外把 HERE 指向项目根，避免 EXE 拼错。
"""
import io
import os
import glob

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TESTS = os.path.join(ROOT, "tests")

MARK = "# --- 路径基准：从 tests/ 回到项目根（P0 结构整理时注入） ---"
INJECT = (
    MARK + "\n"
    "import os as _os, sys as _sys\n"
    "_os.chdir(_os.path.dirname(_os.path.dirname(_os.path.abspath(__file__))))\n"
)


def split_eol(src):
    return "\r\n" if "\r\n" in src else "\n"


def inject(path):
    src = io.open(path, "r", encoding="utf-8", newline="").read()
    if MARK in src:
        return "skip"
    eol = split_eol(src)
    lines = src.split(eol)
    at = None
    for i, line in enumerate(lines):
        s = line.lstrip()
        if s.startswith("import ") or s.startswith("from "):
            at = i
            break
    if at is None:
        at = 0
    block = INJECT.replace("\n", eol).rstrip(eol).split(eol)
    lines[at:at] = block
    io.open(path, "w", encoding="utf-8", newline="").write(eol.join(lines))
    return "ok@" + str(at + 1)


def fix_here(path):
    """test_hook_passthrough.py: HERE 原本是脚本目录，移动后需指向项目根。"""
    src = io.open(path, "r", encoding="utf-8", newline="").read()
    old = 'HERE = os.path.dirname(os.path.abspath(__file__))'
    new = ('HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))'
           '  # 项目根（P0 后脚本位于 tests/）')
    if old not in src:
        return "no-match"
    io.open(path, "w", encoding="utf-8", newline="").write(src.replace(old, new, 1))
    return "ok"


def main():
    report = []
    for p in sorted(glob.glob(os.path.join(TESTS, "test_*.py"))):
        report.append((os.path.basename(p), inject(p)))
    special = os.path.join(TESTS, "test_hook_passthrough.py")
    if os.path.exists(special):
        report.append(("[HERE] " + os.path.basename(special), fix_here(special)))
    for name, st in report:
        print("%-34s %s" % (name, st))


if __name__ == "__main__":
    main()
