# -*- coding: utf-8 -*-
"""设置里的「清空剪贴板历史」：只清普通记录，置顶与收藏不受影响。

验的是三条产品级语义（不是实现细节）：
  1. 清空历史**不动**置顶与收藏；
  2. 剪贴板记录上限裁剪同样**不动**它们（不占名额、也不会被裁掉）；
  3. 收藏与历史数据上互不影响（收藏页单独列出），历史页里照旧看得到收藏条目。

为什么必须真重启 + 真查库：清空最容易出的两种错——
「内存里删了、库里没删」（重启就回来）和「库里删多了」（把收藏一起删掉）——
都只有重启之后再查库才看得见。上限裁剪同理：它在 HistoryLoad 的 SQL 与
ApplyHistoryLimit 两处都会发生，漏掉任何一处，用户的收藏就会在下次启动时消失。

跑法：python tests\\test_clear.py
前置：settings.ini 里 admin=0（管理员实例下 SendInput 会被 UIPI 挡，Win+V 到不了面板）。
"""

import os
import shutil
import sqlite3
import struct
import time

import wvclip as w
from wvclip import *  # noqa: F401,F403  统一键鼠/窗口/剪贴板/断言样板

# 复用持久化用例里的历史库辅助：这几个函数（停/启实例、查库、备份还原）那边已经
# 把 WAL 的坑踩平了（删库必须连 -wal/-shm 一起删，否则旧事务会被恢复回来）。
from test_persist import (hist_dir, db_files, app_running, stop_app, start_app,
                          query, rm_db, _ini_value, _ini_path)

CLEAR_ROW = 6          # layout.h 的 SET_ROW_CLEAR


def st_blob():
    """SYSTEMTIME 的 16 字节 BLOB（ReadSt 要求长度严格等于 sizeof(SYSTEMTIME)）。"""
    return struct.pack("<8H", 2025, 1, 1, 12, 0, 0, 0, 0)


def _db():
    return sqlite3.connect(os.path.join(hist_dir(), "history.db"))


def set_flag(text, pinned=None, fav=None):
    con = _db()
    try:
        if pinned is not None:
            con.execute("UPDATE items SET pinned=? WHERE text=?", (pinned, text))
        if fav is not None:
            con.execute("UPDATE items SET fav=? WHERE text=?", (fav, text))
        con.commit()
    finally:
        con.close()


def insert_plain(prefix, n):
    """直接往库里塞 n 条普通（未置顶未收藏）文本记录。

    走库而不是走剪贴板：60 次 set_clip 要等 60 次防抖，而这里要验的是
    「启动时的裁剪会不会误伤置顶/收藏」，与被测路径无关。
    """
    con = _db()
    try:
        for i in range(n):
            t = "%s%03d" % (prefix, i)
            con.execute(
                "INSERT INTO items(kind,st,title,tag,source,text,bytes) "
                "VALUES(0,?,?,'文本','Test',?,?)",
                (st_blob(), t[:60], t, len(t.encode("utf-8"))))
        con.commit()
    finally:
        con.close()


def flags():
    """{text: (pinned, fav)} —— 只看库，不看界面。"""
    return dict((r[0], (r[1], r[2]))
                for r in query("SELECT text,pinned,fav FROM items") if r[0])


def show_panel(tries=6):
    """唤起面板并等它可见。已可见时直接返回：再按一次 Win+V 是「收起」。"""
    try:
        h = panel(timeout=6.0)
    except SystemExit:
        return None
    if vis(h):
        return h
    for _ in range(tries):
        win_v()
        p = wait_panel(visible=True, timeout=1.2)
        if p:
            return p
    return None


def go_settings_clear(h):
    """打开设置窗口（面板上的齿轮）后点「清空」那一行（整行可点）。

    设置是独立窗口（2026 起）：面板会被收起，点击要投给设置窗口而不是面板。
    返回设置窗口句柄——确认卡片也画在那个窗口里。
    """
    g = settings_geom(h)
    post_click(h, g["gear"][0], g["gear"][1])
    sw = settings_win(timeout=4.0)
    need(sw, "设置窗口已弹出")
    g = settings_win_geom(sw)
    y0, y1 = g["rows"][CLEAR_ROW]
    post_click(sw, g["w"] // 4, (y0 + y1) // 2)
    time.sleep(0.55)
    return sw


def run():
    section("准备：空历史 + 四条哨兵记录")
    need(stop_app(), "旧实例已结束")
    rm_db()
    start_app()
    need(app_running(), "程序已启动")

    tag = str(int(time.time()))
    A, B, C, D = ("CLR-A-" + tag, "CLR-B-" + tag, "CLR-C-" + tag, "CLR-D-" + tag)
    for t in (A, B, C, D):
        set_clip(t)
        time.sleep(0.45)           # 采集有 160ms 防抖，贴太密会合并成一次
    time.sleep(1.4)                # 落库防抖 900ms
    got = flags()
    chk(all(t in got for t in (A, B, C, D)), "四条哨兵都进了历史", "库里 %d 条" % len(got))

    section("A 置顶、B 收藏（写库后重启，让它走真实的载入路径）")
    need(stop_app(), "实例已结束")
    set_flag(A, pinned=1)
    set_flag(B, fav=1)
    start_app()
    f = flags()
    chk(f.get(A) == (1, 0), "A 的置顶标记读回来了", str(f.get(A)))
    chk(f.get(B) == (0, 1), "B 的收藏标记读回来了", str(f.get(B)))

    section("点设置里的「清空」：先出确认卡片，此时一条都不该删")
    h = show_panel()
    need(h, "面板已弹出")
    before = len(flags())
    sw = go_settings_clear(h)
    chk(not vis(h), "面板已收起（设置是独立窗口，与面板互斥）")
    chk(vis(sw), "设置窗口可见")
    chk(len(flags()) == before, "只弹了卡片，还没删", "库里 %d 条" % len(flags()))

    section("确认清空：普通记录消失，置顶与收藏原样留下")
    post_click(sw, *modal_ok_point(sw, clear=True))
    time.sleep(1.3)
    chk(vis(sw), "设置窗口仍在（清空后不自动关闭）")
    left = flags()
    chk(A in left and B in left, "置顶/收藏的两条都还在", str(sorted(left)))
    chk(C not in left and D not in left, "普通记录已被清掉")
    chk(left.get(A) == (1, 0) and left.get(B) == (0, 1), "标记位没被顺手清掉")
    chk(not [t for t in left if t and t.startswith("CLR-") and t not in (A, B)],
        "本批只剩置顶与收藏这两条")

    section("重启再查：清空不是只在内存里生效")
    need(stop_app(), "实例已结束")
    # 启动时会把「当前剪贴板」重新采集一条：先换成不属于本批哨兵的内容，
    # 否则 D 会以「新采集」的身份回到库里，看起来像被清掉的记录复活了。
    set_clip("CLR-POST-%d" % int(time.time()))
    time.sleep(0.2)
    start_app()
    texts = set(t for t in flags() if t)
    chk(A in texts and B in texts, "重启后置顶/收藏仍从库里读得回来")
    chk(C not in texts and D not in texts, "重启后普通记录没有『复活』")

    section("记录上限裁剪不碰置顶与收藏（下限 50 + 塞 60 条普通记录）")
    need(stop_app(), "实例已结束")
    ini = _ini_path()
    with open(ini, encoding="utf-8", newline="") as fp:
        raw = fp.read()
    lines = [("history=50" if l.startswith("history=") else l) for l in raw.split("\r\n")]
    with open(ini, "w", encoding="utf-8", newline="") as fp:
        fp.write("\r\n".join(lines))
    insert_plain("CLR-BULK-%d-" % int(time.time()), 60)
    start_app()
    texts = set(t for t in flags() if t)
    chk(A in texts and B in texts, "裁剪后置顶/收藏一条都没少")
    plain = [t for t in texts if t.startswith("CLR-BULK-")]
    chk(len(plain) <= 50, "普通记录被裁到上限以内（含启动时新采集的那条）",
        "剩 %d 条" % len(plain))


def main():
    if not os.path.exists(w.BUILD_EXE):
        raise SystemExit("先构建：cmake --build build")
    if _ini_value("admin") == "1" and not is_elevated():
        print("SKIP admin=1 且测试脚本非提权：SendInput 会被 UIPI 挡，Win+V 到不了提权面板。"
              "用提权的终端跑本脚本，或把 settings.ini 的 admin 改成 0 再重启程序。")
        return 0

    ini = _ini_path()
    raw_ini = ""
    if os.path.exists(ini):
        with open(ini, encoding="utf-8", newline="") as fp:
            raw_ini = fp.read()

    # 备份用户的真实历史：这个用例要真的清空一遍，不能让哨兵与清空操作留下痕迹。
    # 顺序很关键——**必须先停实例再备份**：运行中的库最新几条还在 WAL 里，
    # 直接拷 .db 会漏掉它们，而拷完若赶上一次 checkpoint 把 -wal 清空，那几条就
    # 永久没了（本用例第一版就是这么把真实历史弄丢的）。所以先停，再用 sqlite 的
    # backup 接口做一致性快照（它会把 WAL 里的内容一并写进目标库）。
    bak = hist_dir() + "_testbak"
    shutil.rmtree(bak, ignore_errors=True)
    stop_app()
    had_db = os.path.exists(db_files()[0])
    if had_db:
        os.makedirs(bak, exist_ok=True)
        src = sqlite3.connect(db_files()[0])
        dst = sqlite3.connect(os.path.join(bak, "history.db"))
        try:
            src.backup(dst)
        finally:
            dst.close()
            src.close()

    try:
        run()
    finally:
        stop_app()
        rm_db()
        if had_db and os.path.exists(os.path.join(bak, "history.db")):
            shutil.copy2(os.path.join(bak, "history.db"), os.path.join(hist_dir(), "history.db"))
        shutil.rmtree(bak, ignore_errors=True)
        if raw_ini:
            with open(ini, "w", encoding="utf-8", newline="") as fp:
                fp.write(raw_ini)      # 还原被用例改过的 history=
        start_app()

    report()


if __name__ == "__main__":
    main()
