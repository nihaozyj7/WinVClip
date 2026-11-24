# -*- coding: utf-8 -*-
"""列表滚动：滚轮真的改变可视首行、命中判定跟着一起走、滚回顶部不残留偏移。

为什么单独测：滚动是像素级的（g_scrollY + g_rowOff），行矩形由 ComputeLayout
按 g_rowOff 生成。这里踩过两个坑，都是"看起来没反应"却不报错：
  · 只改 g_scrollY 不重算布局 → 绘制仍用旧矩形，滚轮完全没反应；
  · 行矩形会越出 body 上下沿（底部那半行就是要露出来暗示"下面还有"），
    命中判定若不一致，点到的是相邻那条。
所以断言一律落在"点了哪一行、粘出来的是哪条"上，而不是内部状态。

四个坑记在这里，免得下次重犯：
  · WM_MOUSEWHEEL 的 delta **正 = 向上滚**，要往下滚得投 -120；
  · 判断"点到了没有"必须看 GetClipboardSequenceNumber 有没有变：只看文本内容，
    "点了没粘上"和"剪贴板里本来就是那条"完全一样；
  · 粘贴会把那一条移到历史顶部（去重即「最近使用」），所以「点了之后行内容
    变没变」不能当断言——每条断言都必须在同一次「滚动+点击」里自洽；
  · 同理不能在两次滚动之间让面板收起（ShowPanel 会把滚动重置到顶部，测等于白测），
    因此全程临时固定面板，最后再取消固定。

后两段（删除保持视口 / 拖滚动条）同样是黑盒断言，各有前提：
  · 删除走 Del 键，删的是 g_sel 那行；settings.ini 里「删除前确认」开着时，
    Del 之后还得再投一次 Enter 才真的删（见 confirm_on）；
  · 滚动条贴在面板右缘，命中带是 [W-10, W-1)，拖动用消息投递的方式模拟
    （post_click 那一套：SetCapture 对投递消息不影响，LBUTTONUP 会释放捕获）。

会往历史里新增 N 条文本。前置：面板为默认尺寸（本脚本按 ROW0_Y 点首行，
尺寸不符时直接 SKIP 而不报假失败）。
"""
import time

import wvclip as w
from wvclip import *  # noqa: F401,F403  统一键鼠/窗口/剪贴板/断言样板

ROW0_Y = 180          # 列表首行的点击点（客户区坐标，488 高面板下稳定）
N = 12                # 灌入的条数


def show(h, tries=8):
    for _ in range(tries):
        if vis(h):
            return True
        win_v()
        for _ in range(40):
            if vis(h):
                return True
            time.sleep(0.05)
    return False


def wheel(h, notches, down=True):
    """滚轮。正 delta = 向上，所以「向下滚」要投负值。"""
    delta = -120 if down else 120
    for _ in range(notches):
        u.PostMessageW(h, 0x020A, ((delta & 0xFFFF) << 16), 0)   # WM_MOUSEWHEEL
        time.sleep(0.12)
    time.sleep(0.5)      # 等缓动走完（每帧走剩余一半，约 80ms 到位）


def click_row(h, y=ROW0_Y):
    """点该行，返回 (剪贴板文本, 是否真的发生了粘贴)。"""
    seq0 = clip_seq()
    post_click(h, 150, y)
    for _ in range(40):
        if clip_seq() != seq0:
            break
        time.sleep(0.05)
    return get_clip(), clip_seq() != seq0


def set_pin(h, on):
    """右上角一排：关闭贴右缘，向左依次是齿轮、图钉。"""
    r = wintypes.RECT()
    u.GetWindowRect(h, ctypes.byref(r))
    W = r.right - r.left
    post_click(h, W - 75, 17)
    time.sleep(0.4)


def confirm_on():
    """settings.ini 里「删除前确认」是否开启：开着时 Del 之后要再按一次 Enter。

    默认值是 true（见 settings.h），ini 不存在/没这一行都按 true 处理。
    """
    p = os.path.join(os.environ.get("LOCALAPPDATA", ""), "WinVClip", "settings.ini")
    try:
        with open(p, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                s = line.strip()
                if s.startswith("confirmdel"):
                    return s.split("=", 1)[-1].strip() == "1"
    except OSError:
        pass
    return True


def del_selected(h):
    """按 Del 删掉选中行；确认卡片开着时补一次 Enter。"""
    u.PostMessageW(h, 0x0100, 0x2E, 0)          # WM_KEYDOWN VK_DELETE
    time.sleep(0.3)
    if confirm_on():
        u.PostMessageW(h, 0x0100, 0x0D, 0)      # Enter = 确认卡片上的「删除」
        time.sleep(0.35)


def drag_scrollbar(h, y0, y1):
    """在面板右缘的滚动条上按住拖一段（客户区坐标）。"""
    W = client_origin(h)[2]
    x = W - 6                                   # 命中带是 [W-10, W-1)
    u.PostMessageW(h, 0x0200, 0, (y0 << 16) | x)             # WM_MOUSEMOVE 先移过去
    u.PostMessageW(h, 0x0201, 1, (y0 << 16) | x)             # WM_LBUTTONDOWN
    step = 10 if y1 >= y0 else -10
    for y in range(y0 + step, y1 + step, step):
        u.PostMessageW(h, 0x0200, 1, (y << 16) | x)          # 拖动中：MK_LBUTTON
        time.sleep(0.03)
    u.PostMessageW(h, 0x0202, 0, (y1 << 16) | x)             # WM_LBUTTONUP
    time.sleep(0.3)


def main():
    texts = ["滚动回归条目 %02d" % i for i in range(1, N + 1)]
    for t in texts:
        set_clip(t)
        time.sleep(0.5)

    h = panel()
    if not show(h):
        raise SystemExit("面板没弹出，测试不可信")

    # 基线：不滚动时的首行。历史里若有置顶条目，首行就是置顶项而不是本批最新那条
    # ——那是允许的，断言一律落在「前后是否变化」上，不假设首行一定是本批条目。
    top, moved = click_row(h)
    if not moved:
        print("SKIP 首行点击没触发粘贴（面板尺寸/状态与本脚本假设不符）")
        return

    # 固定面板：否则每次点击粘贴都会收起面板，而收起会把滚动重置到顶部
    if not show(h):
        raise SystemExit("面板第二次没弹出")
    set_pin(h, True)
    if not vis(h):
        raise SystemExit("没固定住面板（点在关闭按钮上了？）")

    # 1) 向下滚 3 格：首行必须换一条（内容真的位移了，而不只是滚动条在动）
    wheel(h, 3)
    r3, moved = click_row(h)
    chk(moved, "滚 3 格后点击确实触发粘贴")
    chk(r3 != top, "滚 3 格后首行换了一条", "top=%r now=%r" % (top, r3))
    chk(r3 in texts, "滚 3 格后首行仍在本批条目里", "got=%r" % r3)

    # 2) 向上猛滚（远超顶部）：必须夹回 0——再滚首行也不再变。
    #    判据只能落在「首行是否稳定」上：粘贴会把点过的那条移到历史顶部，
    #    拿 top 或某条具体内容作期望值都会被这个副作用带偏。
    wheel(h, 30, down=False)
    back, moved = click_row(h)
    chk(moved, "滚回顶部后点击确实触发粘贴")
    wheel(h, 5, down=False)
    again, moved = click_row(h)
    chk(moved, "继续向上滚后点击确实触发粘贴")
    chk(again == back, "已到顶：继续向上滚首行不再变化（滚动夹到 0）",
        "want=%r got=%r" % (back, again))

    # 3) 向下猛滚到底：夹住不越界，点首行仍是一条真实历史（此时是较老的条目）
    wheel(h, 60)
    bot, moved = click_row(h)
    chk(moved, "滚到底后点击确实触发粘贴")
    chk(bot != "", "滚到底后点首行粘出了非空内容", "got=%r" % bot)

    # 4) 删除一条之后，视口必须留在原处（曾经是 g_sel = 0 + ResetListScroll()：
    #    每删一条就弹回顶部，清理历史时得一次次重新滚回去）
    section("删除保持视口")
    wheel(h, 40, down=False)          # 先滚回顶部，从确定的起点开始
    wheel(h, 3)                       # 再往下 3 行
    before, moved = click_row(h)      # 取首行（粘贴会把它移到历史顶部）
    chk(moved, "删除前点击首行确实触发粘贴", "got=%r" % before)
    del_selected(h)                   # 删掉 g_sel（= 刚才点的视口首行）
    after, moved = click_row(h)
    chk(moved, "删除后点击首行确实触发粘贴", "got=%r" % after)
    # 旧行为：视口归零 → 首行就是刚粘贴到顶部的 before（断言正好抓它）
    chk(after != before, "删除后视口没有弹回顶部", "before=%r after=%r" % (before, after))

    # 5) 拖滚动条：内容跟着走、不触发行粘贴、不误入「抓右缘改尺寸」
    section("滚动条可拖拽")
    before, moved = click_row(h)
    chk(moved, "拖拽前点击首行确实触发粘贴", "got=%r" % before)
    win0 = rect(h)
    seq0 = clip_seq()
    drag_scrollbar(h, 180, 320)
    chk(clip_seq() == seq0, "拖滚动条没有触发粘贴（剪贴板序列号未变）")
    chk(rect(h) == win0, "拖滚动条没有误入抓右缘改尺寸/拖窗口", "now=%r" % (rect(h),))
    after, moved = click_row(h)
    chk(moved, "拖拽后点击首行确实触发粘贴", "got=%r" % after)
    chk(after != before, "拖滚动条后内容确实位移了", "before=%r after=%r" % (before, after))

    set_pin(h, False)
    u.PostMessageW(h, 0x0100, 0x1B, 0)      # WM_KEYDOWN VK_ESCAPE：收起面板
    time.sleep(0.3)
    report(0)


main()
