# -*- coding: utf-8 -*-
"""回归测试 runner（零第三方依赖）。

为什么自己写而不是用 pytest：本机没装 pytest，引入它意味着给一个纯 Win32
小工具加上"先 pip install 才能跑测试"的前置条件。用标准库做完发现/超时/
汇总三件事就够了。

分组只在这一张表里维护，不在脚本内部散落注释：
  - 表里没登记的文件默认按 desktop 跑（漏登记的代价是"多跑一个"，不是漏跑）
  - driver 是造数据的工具（灌剪贴板/固定面板），默认跳过，--all 才跑
  - manual 需要外部条件（Edge 窗口、输入法状态），默认跳过

用法：
  python tests/run.py                 # 跑 default 组
  python tests/run.py --all           # 连 driver/manual 一起跑
  python tests/run.py --only test_fav.py test_glyph_channel.py
  python tests/run.py --list
  python tests/run.py --no-app        # 不自动拉起 winvclip.exe

注意：desktop 组会真的弹面板、抢前台、模拟输入，请在空闲时运行。
"""
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
EXE = os.path.join(ROOT, "build", "winvclip.exe")

DRIVERS = {
    "test_clip.py",          # 往剪贴板灌测试图片
    "test_texts.py",         # 往剪贴板灌若干文本
    "test_pin_panel.py",     # 弹面板并临时固定
    "test_rclick.py",        # 直接投递 WM_RBUTTONUP
}
MANUAL = {
    "test_edge_outside.py",  # 需要已开着的 Edge 窗口
    "test_ime.py",           # 需要真实输入法状态，且必须带参数
    "test_glyph_channel.py",  # 需要指定目标（notepad/web/explorer）
    "test_glyph_insert.py",  # 需要指定目标
    "test_inputmode.py",     # 需要已开着的记事本（不自己拉起）
    "test_noactivate.py",    # 需要已开着的记事本
    "test_noactivate_keys.py",
    "test_resize.py",        # 会改面板尺寸/写记忆文件
}
# 这些脚本不接受命令行参数
NO_ARGS = {
    "test_ime.py": ["compose", "weixiao"],
    "test_glyph_channel.py": ["notepad"],
    "test_glyph_insert.py": ["notepad"],
}

TIMEOUT = 240


def discover():
    out = []
    for name in sorted(os.listdir(HERE)):
        if name.startswith("test_") and name.endswith(".py"):
            out.append(name)
    return out


def group_of(name):
    if name in DRIVERS:
        return "driver"
    if name in MANUAL:
        return "manual"
    return "desktop"


def app_running():
    try:
        r = subprocess.run(["tasklist", "/FI", "IMAGENAME eq winvclip.exe"],
                           capture_output=True, text=True, timeout=10)
        return "winvclip.exe" in r.stdout
    except Exception:
        return False


def ensure_app():
    if app_running():
        return None
    proc = subprocess.Popen([EXE], cwd=os.path.join(ROOT, "build"))
    time.sleep(1.5)
    return proc


def stop_app():
    subprocess.run(["taskkill", "/IM", "winvclip.exe", "/F"], capture_output=True)


def run_one(name, use_app=True):
    if not os.path.exists(EXE):
        return "SKIP", 0.0, "缺少 build\\winvclip.exe，先 cmake --build build"
    if use_app:
        ensure_app()
    args = [sys.executable, os.path.join(HERE, name)] + NO_ARGS.get(name, [])
    t0 = time.time()
    try:
        r = subprocess.run(args, cwd=ROOT, capture_output=True, text=True,
                           timeout=TIMEOUT, encoding="utf-8", errors="replace")
    except subprocess.TimeoutExpired:
        return "TIMEOUT", time.time() - t0, "超过 %ds" % TIMEOUT
    dt = time.time() - t0
    out = (r.stdout or "") + (r.stderr or "")
    if r.returncode == 0:
        return "PASS", dt, out
    return "FAIL", dt, out


def main(argv):
    names = discover()
    if "--list" in argv:
        for n in names:
            print("%-28s %s" % (n, group_of(n)))
        return 0

    only = None
    if "--only" in argv:
        i = argv.index("--only")
        only = set(argv[i + 1:])
    run_all = "--all" in argv
    use_app = "--no-app" not in argv
    verbose = "--verbose" in argv or "-v" in argv

    todo = [n for n in names if only is None or n in only]
    if only is None and not run_all:
        todo = [n for n in todo if group_of(n) == "desktop"]

    print("将运行 %d 个用例（超时 %ds）\n" % (len(todo), TIMEOUT))
    results = []
    try:
        for n in todo:
            sys.stdout.write("%-28s ... " % n)
            sys.stdout.flush()
            status, dt, out = run_one(n, use_app)
            print("%-7s %5.1fs" % (status, dt))
            results.append((n, status, out))
            if status != "PASS" and not verbose:
                tail = [l for l in out.strip().splitlines() if l.strip()][-6:]
                for l in tail:
                    print("    | " + l)
    finally:
        if use_app and "--keep-app" not in argv:
            stop_app()

    passed = sum(1 for _n, s, _o in results if s == "PASS")
    failed = [(n, s) for n, s, _o in results if s != "PASS"]
    print("\n%d/%d 通过" % (passed, len(results)))
    if failed:
        print("未通过：")
        for n, s in failed:
            print("  %-28s %s" % (n, s))
        return 1

    if verbose:
        for n, _s, out in results:
            print("\n===== %s =====" % n)
            print(out.rstrip())
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
