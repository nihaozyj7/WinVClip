# -*- coding: utf-8 -*-
"""把参考页面的 data/symbols.js、data/emoji.js 转成程序内嵌的紧凑 UTF-8 blob。

为什么要转换而不是直接塞 JSON：
  - 程序里不需要 JSON 解析器，也不想为 9000+ 条数据做几千次字符串分配。
  - 输出是「行式纯文本」，运行期只解析几十行分类表，条目按分类惰性建索引。
  - 名称保持 UTF-8 不动，只在真正绘制到屏幕时才转宽字符。

用法：
  python tools/gen_glyph_data.py <参考数据目录> <输出目录>
  python tools/gen_glyph_data.py "C:/.../id-xxx/data" src
"""
import io
import json
import os
import sys

# blob 行格式（字段用 \t 分隔，行尾 \n）：
#   符号： C\tKEY\tNAME              大类（由参考页面的「域」合并而来）
#          I\tGLYPH\tNAME           条目
#   表情： C\tKEY\tNAME              分类
#          I\tGLYPH\tZH\tEN         条目（ZH 中文名 / EN 英文名，用于搜索）
SYM_HEADER = ("SYM\t1\tdomain\n", "SYM\t2\tcat\n")

# 表情分类名统一成两个字：分类条 chip 与抽屉按钮的宽度都是按两字设计的，
# 参考页面里的「表情情绪 / 人物身体 / 旅行地点」这类四字名会把 chip 撑开、
# 并在窄面板下挤掉别的 chip。按 key 映射，未列出的分类保持原名。
EMO_NAME = {
    'smileys':  '情绪',
    'people':   '人物',
    'nature':   '动物',
    'food':     '食物',
    'act':      '活动',
    'travel':   '旅行',
    'objects':  '物品',
    'symbols':  '符号',
    'flags':    '旗帜',
}

HEX = set('0123456789abcdefABCDEF')


def load_js(path, var):
    s = io.open(path, encoding='utf-8').read()
    i = s.index('=')
    j = s.rindex(';')
    return json.loads(s[i + 1:j])


def cxx_literal(data):
    """把 UTF-8 字节串转成 C++ 字符串字面量（每行一个，便于 diff）。

    非 ASCII 一律转成 \\xNN：源码里不出现原始多字节字符，
    任何工具链（不管输入字符集设成什么）编译出来都是同一串字节。
    \\x 是贪婪十六进制转义，紧跟一个十六进制数字会把它吞掉，
    这种位置插入 ""（空字面量）断开——相邻字面量会被拼接，内容不变。
    """
    toks = []          # (文本, 是否以 \xNN 结尾)
    for b in data:
        if b == 0x0A:
            toks.append(('\\n', False))
        elif b == 0x09:
            toks.append(('\\t', False))
        elif b == 0x5C:
            toks.append(('\\\\', False))
        elif b == 0x22:
            toks.append(('\\"', False))
        elif 0x20 <= b < 0x7F:
            toks.append((chr(b), False))
        else:
            toks.append(('\\x%02X' % b, True))

    out, cur = [], ['"']
    curlen = 1
    for i, (t, is_hex) in enumerate(toks):
        if i > 0 and toks[i - 1][1] and t in HEX:
            cur.append('""')      # 断开字面量，避免 \xAA1 被当成 \xAA1
            curlen += 2
        cur.append(t)
        curlen += len(t)
        if curlen > 110:
            cur.append('"')
            out.append(''.join(cur))
            cur = ['"']
            curlen = 1
    if curlen > 1:
        cur.append('"')
        out.append(''.join(cur))
    return '\n'.join(out)


def check_field(s, what):
    if '\t' in s or '\n' in s:
        raise SystemExit('字段里出现制表符/换行，格式会被破坏：%r' % (s,))
    return s


def build_symbols(path):
    """符号按「域」合并成大类，不再输出细分类和域两层。

    参考页面是「域 → 52 个细分类」两级，面板的分类条只放得下几个 chip，
    两层会让抽屉里堆 52 个按钮还要分组标题。这里直接按域合并，
    同一个域内的重复字形只保留第一次出现的（跨域重复保留，语义不同）。
    输出不带 D 行、C 行也没有 DOMAIN 字段，与表情同构。
    """
    d = load_js(path, 'SYM_DATA')
    order, names = [], {}
    for dom in d['domains']:
        order.append(dom['k'])
        names[dom['k']] = dom['n']

    items = {k: [] for k in order}
    seen = {k: set() for k in order}
    for c in d['cats']:
        if c['k'] == 'recent':      # 程序自己维护「最近使用」，不用数据里的空壳
            continue
        dom = c.get('d')
        if dom not in names:
            continue
        for it in c['items']:
            g = check_field(it[0], 'g')
            if g in seen[dom]:
                continue
            seen[dom].add(g)
            items[dom].append((g, check_field(it[1], 'n')))

    lines = []
    for k in order:
        if not items[k]:
            continue
        lines.append('C\t%s\t%s\n' % (check_field(k, 'k'), check_field(names[k], 'n')))
        for g, n in items[k]:
            lines.append('I\t%s\t%s\n' % (g, n))
    return ''.join(lines)


def build_emoji(path):
    d = load_js(path, 'EMOJI_DATA')
    lines = []
    for c in d['cats']:
        if c['k'] == 'recent':
            continue
        name = EMO_NAME.get(c['k'], c['n'])
        lines.append('C\t%s\t%s\n' % (check_field(c['k'], 'k'), check_field(name, 'n')))
        for it in c['items']:
            en = it[2] if len(it) > 2 else ''
            lines.append('I\t%s\t%s\t%s\n' % (check_field(it[0], 'g'),
                                              check_field(it[1], 'n'),
                                              check_field(en, 'n2')))
    return ''.join(lines)


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    datadir, outdir = sys.argv[1], sys.argv[2]
    sym = build_symbols(os.path.join(datadir, 'symbols.js')).encode('utf-8')
    emo = build_emoji(os.path.join(datadir, 'emoji.js')).encode('utf-8')

    os.makedirs(outdir, exist_ok=True)
    with io.open(os.path.join(outdir, 'glyph_data.cpp'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('// 自动生成，请勿手改（tools/gen_glyph_data.py）\n')
        f.write('// 数据来源：Unicode 16.0 官方码表 + CLDR 中文注解 / Emoji 18.0\n')
        f.write('#include "glyph_data.h"\n\nnamespace glyphdata {\n\n')
        f.write('const char* const kSymBlob =\n%s;\n\n' % cxx_literal(sym))
        f.write('const char* const kEmoBlob =\n%s;\n\n' % cxx_literal(emo))
        f.write('} // namespace glyphdata\n')

    with io.open(os.path.join(outdir, 'glyph_data.h'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('// 自动生成，请勿手改（tools/gen_glyph_data.py）\n')
        f.write('#pragma once\n\nnamespace glyphdata {\n')
        f.write('// 行式 UTF-8 blob，格式见 tools/gen_glyph_data.py 顶部注释\n')
        f.write('extern const char* const kSymBlob;\n')
        f.write('extern const char* const kEmoBlob;\n')
        f.write('} // namespace glyphdata\n')

    print('symbols: %d bytes / %d lines' % (len(sym), sym.count(b'\n')))
    print('emoji:   %d bytes / %d lines' % (len(emo), emo.count(b'\n')))
    print('written to %s' % os.path.abspath(outdir))


if __name__ == '__main__':
    main()
