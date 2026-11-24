# -*- coding: utf-8 -*-
"""量一下各页滚动的绘制耗时 + UI 线程响应延迟（表情 / 符号页专项）。

为什么要这个脚本：`--perf` 的日志只统计「已经在画的那一帧」，而用户感觉到的
「卡」有两层——① 单帧绘制本身很贵；② UI 线程被占住导致低级鼠标钩子回调排队
（系统级鼠标都跟着变钝）。所以这里除了读 perf 日志，还同时测
「PostMessage(滚轮) 之后 SendMessage(WM_NULL) 的往返耗时」：它近似等于
「UI 线程还要多久才回到消息泵」，也就是那次绘制把消息循环堵了多久。

用法：python tools/measure_grid.py [页序号 0=历史 2=表情 3=符号]
前置：没有别的实例在跑（本脚本自己拉起一个带 --perf 的）。
"""
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests"))
import wvclip as w            # noqa: E402
from wvclip import *          # noqa: E402,F401,F403

EXE = os.path.join(ROOT, "build", "winvclip.exe")
LOG = os.path.join(os.environ.get("TEMP", "."), "winvclip_perf.log")

PAGE_NAME = {0: "历史", 1: "收藏", 2: "表情", 3: "符号"}


def log_size():
    try:
        return os.path.getsize(LOG)
    except OSError:
        return 0


def log_tail(off):
    try:
        with open(LOG, "rb") as f:
            f.seek(off)
            return f.read().decode("utf-8", "replace")
    except OSError:
        return ""


def start(more_args=()):
    subprocess.Popen([EXE, "--perf"] + list(more_args), cwd=os.path.dirname(EXE))


def roundtrip(h):
    """UI 线程多久回到消息泵（ms）。放在滚轮消息后面测，就是那一帧的阻塞时长。"""
    t0 = time.perf_counter()
    u.SendMessageW(h, 0, 0, 0)      # WM_NULL：排队到 UI 线程再回来
    return (time.perf_counter() - t0) * 1000.0


def wheel_burst(h, notches, down=True, gap=0.03):
    """连滚 notches 格，每格之后量一次线程往返，返回 (样本列表)。

    delta 正 = 向上滚，往下滚要投负值（见 test_scroll.py 的坑）。
    """
    d = -120 if down else 120
    lat = []
    for _ in range(notches):
        u.PostMessageW(h, 0x020A, ((d & 0xFFFF) << 16), 0)
        lat.append(roundtrip(h))
        time.sleep(gap)
    return lat


def stat(name, lat):
    if not lat:
        print("  %-10s 无样本" % name)
        return
    s = sorted(lat)
    print("  %-10s n=%d  中位=%.1fms  p90=%.1fms  最大=%.1fms"
          % (name, len(lat), s[len(s) // 2], s[int(len(s) * 0.9)], s[-1]))


def goto(h, cur, idx):
    """切到指定页：先按满一圈（4 页一圈，回到原页）再按到目标，免得页面漂移。"""
    for _ in range(4):
        u.PostMessageW(h, 0x0100, 0x09, 0)     # WM_KEYDOWN VK_TAB
        time.sleep(0.1)
    for _ in range((idx - cur) % 4):
        u.PostMessageW(h, 0x0100, 0x09, 0)
        time.sleep(0.12)
    time.sleep(0.6)                           # 等首帧（可能触发读盘/预热）画完


def main():
    pages = [int(a) for a in sys.argv[1:]] or [0, 2, 3]
    cur = 0
    start()
    h = w.panel(6.0)
    if not vis(h):
        w.win_v(1.2)
    if not vis(h):
        raise SystemExit("面板没弹出，测不了")

    for idx in pages:
        off0 = log_size()
        goto(h, cur, idx)
        cur = idx
        print("================ 场景：%s 页" % PAGE_NAME.get(idx, idx))
        print("[空转] 不滚动时的线程往返：")
        stat("idle", [roundtrip(h) for _ in range(20)])

        print("[滚动] 向下连滚 40 格：")
        stat("scroll", wheel_burst(h, 40, down=True))
        time.sleep(0.5)
        print("[滚动] 向上连滚 40 格：")
        stat("scroll-up", wheel_burst(h, 40, down=False))
        time.sleep(1.0)

        lines = [l for l in log_tail(off0).splitlines() if "page=" in l]
        print("[perf 日志] %s 页本次新增 %d 条：" % (PAGE_NAME.get(idx, idx), len(lines)))
        for l in lines:
            print("   " + l.strip())

    u.PostMessageW(h, 0x0100, 0x1B, 0)      # Esc 收起（顺便让实例回到空闲）


main()
