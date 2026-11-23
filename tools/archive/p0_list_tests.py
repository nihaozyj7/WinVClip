# -*- coding: utf-8 -*-
"""列出 tests/ 下每个测试的一行用途（取模块 docstring 首行）。"""
import glob
import io
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

for path in sorted(glob.glob(os.path.join(ROOT, "tests", "test_*.py"))):
    src = io.open(path, encoding="utf-8").read()
    m = re.search(r'"""(.*?)"""', src, re.S)
    desc = ""
    if m:
        for line in m.group(1).strip().splitlines():
            line = line.strip()
            if line:
                desc = line
                break
    print("%-28s %s" % (os.path.basename(path), desc))
