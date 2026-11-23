# -*- coding: utf-8 -*-
"""一次性工具：把 tests/*.py 里与 wvclip.py 重复的样板删掉。

为什么用 AST 而不是正则改整个文件：每个脚本的文件特有部分（用例主体、
本地驱动器函数）必须逐字保留，只有「与公共层同名的顶层定义」才该消失。
按行号区间删除能保证剩下的用例代码一个字符都不动。

同时做几处语义改写（必须显式表明，不能靠同名巧合）：
  click(h, x, y)   -> post_click(h, x, y)   消息投递（面板）
  click(x, y)      -> real_click(x, y)      真实鼠标（钩子用例）
  find(sub)        -> find_title(sub)       返回形状相同
  winv()/win_vi()  -> win_v()               统一命名

保留不动的：main / 文件特有常量 / 本地驱动器（put、char、post、drag 等）。
"""
import ast
import os
import re
import sys

TESTS = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tests")

# 公共层已提供、语义一致的顶层函数
DROP_FUNCS = {
    "key", "tap", "winv", "win_vi", "panel", "vis", "click",
    "set_clip", "get_clip", "cls", "rect", "find_title", "find_class",
    "force_fg", "fg", "inside", "caret_of", "text_of", "edit_text",
    "find_notepad", "uni", "combo", "find", "focus_of", "size", "wait_panel",
}
DROP_CLASSES = {"KEYBDINPUT", "_I", "INPUT", "GUITHREADINFO", "MOUSEINPUT"}

# 这些赋值会遮蔽公共层里已经正确声明过 argtypes 的句柄（ctypes.windll 版本没有声明）
SHADOW_NAMES = {"u", "U", "k", "k32"}
# 公共层已导出的模块，脚本里重复 import 是纯噪音
PUBLIC_MODULES = {"os", "sys", "time", "ctypes", "subprocess", "wintypes"}
PUBLIC_ALIASES = {"_os", "_sys"}

HEADER = (
    "import wvclip as w\n"
    "from wvclip import *  # noqa: F401,F403  统一键鼠/窗口/剪贴板/断言样板\n"
)


def find_docstring_end(tree):
    body = tree.body
    if body and isinstance(body[0], ast.Expr) and isinstance(body[0].value, ast.Constant) \
            and isinstance(body[0].value.value, str):
        return body[0].end_lineno
    return 0


def plan(lines, tree):
    """返回 (删除行号集合, click 参数个数, 是否有 find 定义)。"""
    kill = set()
    click_arity = None
    has_find = False

    def span(node):
        start = node.lineno
        for d in getattr(node, "decorator_list", []) or []:
            start = min(start, d.lineno)
        return range(start, node.end_lineno + 1)

    for node in tree.body:
        if isinstance(node, ast.FunctionDef):
            if node.name in DROP_FUNCS:
                if node.name == "click":
                    # 首参数是句柄 => 消息投递；首参数是屏幕坐标 => 真实鼠标
                    first = node.args.args[0].arg if node.args.args else ""
                    click_arity = "post" if first.lower() in (
                        "h", "hwnd", "panel", "w") else "real"
                if node.name == "find":
                    has_find = True
                kill |= set(span(node))
        elif isinstance(node, ast.ClassDef):
            if node.name in DROP_CLASSES:
                kill |= set(span(node))
        elif isinstance(node, ast.Assign):
            names = {t.id for t in node.targets if isinstance(t, ast.Name)}
            if names & {"VK", "PANEL_CLASS"}:
                kill |= set(span(node))
            elif names & SHADOW_NAMES:
                src = ast.get_source_segment("\n".join(lines), node.value) or ""
                if "windll" in src or "WinDLL" in src:
                    kill |= set(span(node))
            else:
                # u.FindWindowW.restype = ... / k.GlobalAlloc.argtypes = ...
                # 公共层导入时已完整声明，脚本里再声明一遍只会让"谁是准的"变模糊
                for t in node.targets:
                    if isinstance(t, ast.Attribute) and t.attr in ("restype", "argtypes"):
                        base = t.value
                        while isinstance(base, ast.Attribute):
                            base = base.value
                        if isinstance(base, ast.Name) and base.id in SHADOW_NAMES:
                            kill |= set(span(node))
                            break
        elif isinstance(node, ast.Expr) and isinstance(node.value, ast.Call):
            src = ast.get_source_segment("\n".join(lines), node.value) or ""
            if "SetProcessDPIAware" in src or "_os.chdir(" in src:
                kill |= set(span(node))
        elif isinstance(node, ast.Import):
            if all(a.name in PUBLIC_MODULES or a.asname in PUBLIC_ALIASES
                   for a in node.names):
                kill |= set(span(node))
        elif isinstance(node, ast.ImportFrom):
            if node.module in ("ctypes",) and all(
                    a.name in PUBLIC_MODULES for a in node.names):
                kill |= set(span(node))

    # `# --- 路径基准` 注释块 + 紧跟的 chdir 赋值
    for i, ln in enumerate(lines, 1):
        if "路径基准" in ln or "P0 结构整理时注入" in ln:
            kill.add(i)
    return kill, click_arity, has_find


def rewrite(src, click_arity, has_find):
    if click_arity == "post":
        src = re.sub(r"(?<![\w.])click\(", "post_click(", src)
    elif click_arity == "real":
        src = re.sub(r"(?<![\w.])click\(", "real_click(", src)
    if has_find:
        src = re.sub(r"(?<![\w.])find\(", "find_title(", src)
    src = re.sub(r"(?<![\w.])winv\(", "win_v(", src)
    src = re.sub(r"(?<![\w.])win_vi\(", "win_v(", src)
    return src


def collapse_blank(lines):
    out = []
    blank = 0
    for ln in lines:
        if ln.strip() == "":
            blank += 1
            if blank > 2:
                continue
        else:
            blank = 0
        out.append(ln)
    return out


def process(path):
    with io_open(path) as f:
        text = f.read()
    lines = text.split("\n")
    try:
        tree = ast.parse(text)
    except SyntaxError as e:
        print("  跳过（语法错误）：%s %s" % (os.path.basename(path), e))
        return False
    kill, click_arity, has_find = plan(lines, tree)
    if not kill:
        print("  无需改动：%s" % os.path.basename(path))
        return False

    kept = [ln for i, ln in enumerate(lines, 1) if i not in kill]
    src = "\n".join(kept)
    src = rewrite(src, click_arity, has_find)

    # 插入公共层导入：docstring 之后（已有则跳过，工具可重复运行）
    tree2 = ast.parse(src)
    if "import wvclip as w" in src:
        out = collapse_blank(src.split("\n"))
    else:
        pos = find_docstring_end(tree2)
        lines2 = src.split("\n")
        out = lines2[:pos] + ["", HEADER.rstrip("\n")] + lines2[pos:]
        out = collapse_blank(out)
    src = "\n".join(out)

    # 校验：必须仍能解析，且不再有被删定义
    ast.parse(src)
    with io_open(path, "w") as f:
        f.write(src)
    print("  已改写：%-28s 删 %d 行, click->%s" % (
        os.path.basename(path), len(kill),
        {"post": "post_click", "real": "real_click"}.get(click_arity, "-")))
    return True


def io_open(path, mode="r"):
    return open(path, mode, encoding="utf-8", newline="")


def main():
    changed = 0
    for name in sorted(os.listdir(TESTS)):
        if not name.startswith("test_") or not name.endswith(".py"):
            continue
        if name == "test_pin_panel.py":
            pass
        if process(os.path.join(TESTS, name)):
            changed += 1
    print("\n改写 %d 个文件" % changed)


if __name__ == "__main__":
    sys.exit(main())
