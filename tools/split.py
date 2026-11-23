# -*- coding: utf-8 -*-
"""按行号区间把源文件里的代码块剪切到新模块。

用法: python tools/split.py <jobs.py> [源文件]

jobs.py 里定义 JOBS = [dict(dest=..., ranges=[(a,b),...], head='...'), ...]
  · ranges 是 1-based 闭区间，含注释块；全部从同一个源文件里切
  · head 会作为新文件的头部（include 与模块说明）
  · 剪切只切行、不改内容，搬出去的函数与原文件逐字节相同
  · 顺手去掉函数定义上的 static（跨 TU 必须外部链接），变量定义保持 static
"""
import re, os, sys, runpy

jobs_path = sys.argv[1]
SRC = sys.argv[2] if len(sys.argv) > 2 else 'src/app/main.cpp'
NS = runpy.run_path(jobs_path)
JOBS = NS['JOBS']

with open(SRC, 'r', encoding='utf-8', newline='') as f:
    lines = f.read().split('\n')
nl = '\r\n' if (lines and lines[0].endswith('\r')) else '\n'

def seg(a, b):
    return lines[a - 1:b]

def destatic(s):
    # 行首 static 且该行带 '(' → 是函数定义，去掉 static
    return re.sub(r'^static (?=[^\n]*\()', '', s, flags=re.M)

pieces = []
for job in JOBS:
    body = []
    for (a, b) in job['ranges']:
        body.extend(seg(a, b))
    pieces.append((job, body))

for (a, b) in sorted([r for j in JOBS for r in j['ranges']], reverse=True):
    del lines[a - 1:b]

with open(SRC, 'w', encoding='utf-8', newline='') as f:
    f.write('\n'.join(lines))

for job, body in pieces:
    dest = job['dest']
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    text = '\n'.join(body)
    text = destatic(text)
    head = job.get('head', '')
    if head:
        text = head.replace('\n', nl) + text
    with open(dest, 'w', encoding='utf-8', newline='') as f:
        f.write(text)
    print('wrote %-32s %4d lines' % (dest, len(body)))
