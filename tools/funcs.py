import re, sys, io

path = sys.argv[1] if len(sys.argv) > 1 else 'src/app/main.cpp'
with open(path, 'r', encoding='utf-8', newline='') as f:
    lines = f.read().split('\n')

# 顶层函数定义：行首无缩进，含名字(...)，且行尾不是分号（排除声明）
head_re = re.compile(r'^[A-Za-z_][\w:<>,\*&\s]*?\b([A-Za-z_]\w*)\s*\(')

out = []
i = 0
n = len(lines)
while i < n:
    ln = lines[i]
    if not ln or ln[0] in ' \t#/}' or ln.startswith('//'):
        i += 1
        continue
    m = head_re.match(ln)
    if not m:
        i += 1
        continue
    # 拼接签名直到遇到 '{'
    j = i
    sig = ''
    while j < n and '{' not in lines[j]:
        sig += lines[j]
        j += 1
        if j - i > 12:
            break
    if j >= n or '{' not in lines[j]:
        i += 1
        continue
    sig += lines[j].split('{')[0]
    if ';' in sig.replace('(*', ''):
        i += 1
        continue
    if ln.startswith(('struct ', 'enum ', 'union ', 'using ', 'typedef ')):
        i += 1
        continue
    # 括号配平找结束
    depth = 0
    k = j
    started = False
    while k < n:
        # 粗略去掉字符串与注释里的括号
        s = lines[k]
        s = re.sub(r'"(\\.|[^"\\])*"', '', s)
        s = re.sub(r"'(\\.|[^'\\])*'", '', s)
        s = re.sub(r'//.*$', '', s)
        for ch in s:
            if ch == '{':
                depth += 1
                started = True
            elif ch == '}':
                depth -= 1
        if started and depth <= 0:
            break
        k += 1
    name = m.group(1)
    out.append((i + 1, k + 1, name))
    i = k + 1

for a, b, nm in out:
    print('%5d %5d %5d  %s' % (a, b, b - a + 1, nm))
print('--- total functions:', len(out))
