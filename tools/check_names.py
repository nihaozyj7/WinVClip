# -*- coding: utf-8 -*-
"""静态检查：tests/*.py 里是否还有未定义的名字。

必要性：批量删除样板最危险的失败模式是"删多了"，而这种错误只有在运行到
那行时才报 NameError——端到端用例跑一轮要几十秒，靠运行发现太晚。
这里用 ast 收集模块级定义 + 公共层导出，把未解析的 Load 名字一次列全。
"""
import ast
import builtins
import glob
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))), "tests"))
import wvclip  # noqa: E402

PUBLIC = set(wvclip.__all__) | {"wvclip"}
BUILTINS = set(dir(builtins)) | {"__file__", "__name__", "__doc__"}


def module_names(tree):
    names = set()
    for node in ast.walk(tree):
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
            names.add(node.name)
        elif isinstance(node, ast.Name) and isinstance(node.ctx, ast.Store):
            names.add(node.id)
        elif isinstance(node, ast.arg):
            names.add(node.arg)
        elif isinstance(node, (ast.Import, ast.ImportFrom)):
            for a in node.names:
                names.add(a.asname or a.name.split(".")[0])
        elif isinstance(node, ast.ExceptHandler) and node.name:
            names.add(node.name)
        elif isinstance(node, (ast.comprehension,)):
            pass
    return names


def check(path):
    with open(path, encoding="utf-8") as f:
        tree = ast.parse(f.read())
    defined = module_names(tree)
    bad = {}
    for node in ast.walk(tree):
        if isinstance(node, ast.Name) and isinstance(node.ctx, ast.Load):
            if node.id not in defined and node.id not in PUBLIC and node.id not in BUILTINS:
                bad.setdefault(node.id, node.lineno)
    return bad


def main():
    total = 0
    for path in sorted(glob.glob("tests/*.py")):
        bad = check(path)
        if bad:
            total += len(bad)
            print("%-30s %s" % (os.path.basename(path),
                                ", ".join("%s@%d" % (k, v) for k, v in bad.items())))
    print("\n未解析名字 %d 处" % total)
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main())
