# -*- coding: utf-8 -*-
"""粗略统计 main.cpp 的顶层函数长度、全局变量数量、段标题分布。"""
import re, sys, collections

src = open('src/main.cpp', encoding='utf-8').read()
lines = src.split('\n')

# 段标题
sections = [(i+1, l.strip()[len('// ==== '):].strip(' =') if False else l.strip())
            for i, l in enumerate(lines) if l.startswith('// ====')]
print('== 段落标题（%d 个）==' % len(sections))
for ln, t in sections:
    print('%5d  %s' % (ln, re.sub(r'^//\s*=+\s*', '', t)))

# 顶层全局变量（行首 static 且不含 '(' 的声明，粗略）
gvars = []
for i, l in enumerate(lines):
    if l.startswith('static ') and '(' not in l and ';' in l:
        gvars.append((i+1, l.strip()[:90]))
print('\n== 顶层 static 全局变量：%d 个 ==' % len(gvars))
by_prefix = collections.Counter()
for ln, t in gvars:
    m = re.match(r'static\s+[\w:<>,\s\*&]+?([\w]+)\s*(=|\[|;)', t)
    name = m.group(1) if m else t
    p = re.match(r'g_([a-z]+)', name)
    by_prefix[p.group(1) if p else '(非g_前缀)'] += 1
print('按前缀分组:', dict(by_prefix.most_common(20)))
print('g_ 前缀示例:', [t for _, t in gvars if 'g_' in t][:5])

# 顶层函数体长度：以 '}' 出现在行首结束（main.cpp 风格）
funcs = []
i = 0
start_re = re.compile(r'^(?:static\s+)?(?:inline\s+)?[A-Za-z_][\w:<>,\s\*&]*?\b(\w+)\s*\([^;]*$|^(?:static\s+)?(?:inline\s+)?[A-Za-z_][\w:<>,\s\*&]*?\b(\w+)\s*\(.*\)\s*\{\s*$')
while i < len(lines):
    l = lines[i]
    m = re.match(r'^(?:static\s+)?(?:inline\s+)?[A-Za-z_][\w:<>,\s\*&]*?\b(\w+)\s*\(', l)
    if m and not l.startswith('//'):
        # 正向找配对的 '{' 与 '}'
        j = i
        depth = 0
        opened = False
        while j < len(lines) and j - i < 4000:
            for ch in lines[j]:
                if ch == '{':
                    depth += 1; opened = True
                elif ch == '}':
                    depth -= 1
            if opened and depth == 0:
                break
            if not opened and lines[j].rstrip().endswith(';'):
                break
            j += 1
        if opened and depth == 0:
            funcs.append((i+1, j+1, j-i+1, m.group(1)))
            i = j + 1
            continue
    i += 1

funcs.sort(key=lambda x: -x[2])
print('\n== 顶层函数体：%d 个，总行 %d ==' % (len(funcs), sum(f[2] for f in funcs)))
print('最长 25 个:')
for s, e, n, name in funcs[:25]:
    print('%5d-%5d  %4d 行  %s' % (s, e, n, name))
gt100 = [f for f in funcs if f[2] > 100]
print('\n>100 行的函数: %d 个，合计 %d 行' % (len(gt100), sum(f[2] for f in gt100)))
print('>300 行:', [(f[3], f[2]) for f in funcs if f[2] > 300])

# 注释密度、TODO
print('\n== 其它 ==')
print('总行数:', len(lines))
cmt = sum(1 for l in lines if l.strip().startswith('//'))
print('纯注释行:', cmt, '(%.1f%%)' % (100.0*cmt/len(lines)))
for kw in ['TODO', 'FIXME', 'HACK', 'XXX']:
    print(kw, src.count(kw))
print('硬编码颜色 0x 出现:', len(re.findall(r'0x[0-9A-Fa-f]{6}', src)))
print('SetTimer/HWND 全局 g_main 使用:', src.count('g_main'))
print('switch 中 case 总数:', len(re.findall(r'\bcase\b', src)))
