# -*- coding: utf-8 -*-
"""重启后历史还在吗 —— 剪贴板历史的跨进程持久化（SQLite）。

验的是「写到哪、读得回来吗、读回来的还能不能用」这三件容易被自认为成立的事：
  1. 复制的内容会落进 <临时目录>\\history\\history.db（items / files / recent 三张表）；
  2. 进程被结束后重启，历史是从**数据库**读回来的（而不是靠重启后又采集了一遍）；
  3. 超长正文只在库里：重启后仍能搜索命中、能完整粘贴出来；
  4. 老版本的 history.dat 首次启动会导入一次，原文件改名 .old 保留；
  5. 改「临时文件目录」后数据库跟过去（只保留一份）。

为什么必须真重启：写入成功不等于读回成功。最容易出问题的是「写了没读」
「读到半截」「顺序反了」「长正文被当成空文本写回去」——只有真重启一遍才暴露。

前置：settings.ini 里 admin=0。管理员实例下普通权限的 SendInput 会被 UIPI 挡，
Win+V 根本到不了面板。

跑法：python tests\\test_persist.py
"""

import os
import shutil
import sqlite3
import struct
import subprocess
import sys
import time

import wvclip as w
from wvclip import *  # noqa: F401,F403  统一键鼠/窗口/剪贴板/断言样板

# 库里的正文超过 16KB 时不会进内存（见 core/history_store.h 的内存策略），
# 这个标记只出现在大文本的末尾，用来证明「搜索/粘贴拿到的是库里的全文」。
BIG_MARK = "kwzebra"
LEGACY_TEXT = "LEGACY-IMPORT-哨兵"


def _ini_path():
    return os.path.join(os.environ.get("LOCALAPPDATA", ""), "WinVClip", "settings.ini")


def _ini_value(key):
    ini = _ini_path()
    if not os.path.exists(ini):
        return ""
    with open(ini, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith(key + "="):
                return line.split("=", 1)[1].strip()
    return ""


# 临时目录可能被用户在设置里改过：优先读 settings.ini，拿不到再用默认位置
def _temp_dir():
    return _ini_value("tempdir") or os.path.join(os.path.dirname(w.BUILD_EXE), "temp")


def hist_dir():
    return os.path.join(_temp_dir(), "history")


def db_files():
    """history.db 及其 WAL 附属文件。删/搬/备份都必须三个一起处理：
    只删 .db 会留下 -wal，下次打开时 sqlite 会拿旧 WAL 把删掉的记录恢复回来。"""
    db = os.path.join(hist_dir(), "history.db")
    return [db, db + "-wal", db + "-shm"]


def legacy_dat():
    return os.path.join(hist_dir(), "history.dat")


def app_running():
    r = subprocess.run(["tasklist", "/FI", "IMAGENAME eq winvclip.exe"],
                       capture_output=True, text=True, timeout=10)
    return "winvclip.exe" in r.stdout


def stop_app(wait=5.0):
    subprocess.run(["taskkill", "/IM", "winvclip.exe", "/F"], capture_output=True)
    t0 = time.time()
    while time.time() - t0 < wait:
        if not app_running():
            return True
        time.sleep(0.1)
    return False


def start_app():
    p = subprocess.Popen([w.BUILD_EXE], cwd=os.path.dirname(w.BUILD_EXE))
    time.sleep(1.9)          # 等 HistoryLoad + 窗口建好
    panel()
    return p


def query(sql, args=()):
    """只读地查一次（程序同时开着也安全：WAL 允许多连接并发读）。"""
    con = sqlite3.connect(os.path.join(hist_dir(), "history.db"))
    try:
        return con.execute(sql, args).fetchall()
    finally:
        con.close()


def rm_db():
    for p in db_files():
        if os.path.exists(p):
            os.remove(p)


def make_big(ts):
    """造一条 UTF-8 字节数远超 16KB 的文本：开头/结尾各有一个标记，
    结尾那个只可能来自库里的全文（列表标题只有首行 200 字）。"""
    head = "BIGTEXT-%d 开头" % ts
    body = "这是一段足够长的填充文本，用来把单条记录顶到 16KB 以上。" * 420
    tail = "结尾 %s 收束" % BIG_MARK
    return head + "\n" + body + "\n" + tail


def write_legacy_dat(path, texts):
    """按老版本的二进制格式造一个 history.dat（magic/version/count + 每条固定字段）。

    老格式见原来的 history_store.cpp：u8 kind、u8 flags、SYSTEMTIME 16 字节、
    若干个「u32 长度 + UTF-8 字节」字符串、u32 nfiles、u32 imgW/imgH、u64 hash/bytes。
    """
    def s(t):
        b = t.encode("utf-8")
        return struct.pack("<I", len(b)) + b

    out = b"WVCH" + struct.pack("<I", 1) + struct.pack("<I", len(texts))
    for t in texts:
        out += struct.pack("<B", 0)          # kind = 文本
        out += struct.pack("<B", 0)          # flags
        out += struct.pack("<8H", 2025, 1, 1, 12, 0, 0, 0, 0)   # SYSTEMTIME
        out += s(t[:60])                     # title
        out += s("文本")                      # tag
        out += s("LegacyTest")               # source
        out += s(t)                          # text
        out += s("")                         # diskPath
        out += struct.pack("<I", 0)          # nfiles
        out += struct.pack("<I", 0) + struct.pack("<I", 0)        # imgW/imgH
        out += struct.pack("<Q", 0) + struct.pack("<Q", len(t.encode("utf-8")))
    out += struct.pack("<I", 0)              # 最近使用表情：0 条
    out += struct.pack("<I", 0)              # 最近使用符号：0 条
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(out)


def main():
    if not os.path.exists(w.BUILD_EXE):
        raise SystemExit("先构建：cmake --build build")
    if _ini_value("admin") == "1":
        print("SKIP admin=1：管理员实例下测试脚本的 SendInput 会被 UIPI 挡，"
              "Win+V 到不了面板。先把 settings.ini 的 admin 改成 0 并重启程序。")
        return 0

    print("history 目录 =", hist_dir())
    # 备份用户已有的历史：测试不该让真实记录消失。
    # 必须先停实例再备份——运行中的库最新几条还在 WAL 里，直接拷 .db 会漏掉它们，
    # 而拷完若赶上一次 checkpoint 把 -wal 清空，那几条就永久没了。停掉之后库是
    # 关闭状态（WAL 已合并），三个文件一起拷才是完整快照。
    stop_app()
    bak = hist_dir() + "_testbak"
    shutil.rmtree(bak, ignore_errors=True)
    had_db = os.path.exists(db_files()[0])
    had_dat = os.path.exists(legacy_dat())
    if had_db or had_dat:
        os.makedirs(bak, exist_ok=True)
        for p in db_files():
            if os.path.exists(p):
                shutil.copy2(p, os.path.join(bak, os.path.basename(p)))
        if had_dat:
            shutil.copy2(legacy_dat(), os.path.join(bak, "history.dat"))

    try:
        section("准备：清空历史并启动程序")
        need(stop_app(), "旧实例已结束")
        rm_db()
        if os.path.exists(legacy_dat()):
            os.remove(legacy_dat())
        if os.path.exists(legacy_dat() + ".old"):
            os.remove(legacy_dat() + ".old")
        start_app()
        need(app_running(), "程序已启动")

        section("老版本 history.dat 导入（一次性）")
        need(stop_app(), "实例已结束")
        rm_db()
        write_legacy_dat(legacy_dat(), ["LEGACY-ONE-" + LEGACY_TEXT, "LEGACY-TWO-" + LEGACY_TEXT])
        start_app()
        rows = query("SELECT text FROM items ORDER BY id DESC")
        # 启动时会把「当前剪贴板」也采集一条，所以总数 ≥2；关键是旧文件里那两条都在
        chk(sum(1 for r in rows if LEGACY_TEXT in (r[0] or "")) == 2,
            "旧文件里的两条都进了库", "items=%d" % len(rows))
        chk(any(LEGACY_TEXT in (r[0] or "") for r in rows), "导入的正文完整")
        chk(os.path.exists(legacy_dat() + ".old"), "原文件改名 .old 保留")

        section("复制 -> 落库")
        A = "PERSIST-A-%d" % int(time.time())
        set_clip(A)
        time.sleep(1.9)          # 防抖提交是 900ms
        chk(os.path.exists(db_files()[0]), "history.db 已生成")
        chk(query("SELECT count(*) FROM items WHERE text=?", (A,))[0][0] == 1,
            "哨兵文本已写进数据库")

        section("超长文本：正文进库")
        BIG = make_big(int(time.time()))
        need(len(BIG.encode("utf-8")) > 16384, "测试文本确实超过 16KB")
        set_clip(BIG)
        time.sleep(2.0)
        rows = query("SELECT bytes,text FROM items WHERE kind=0 ORDER BY id DESC LIMIT 1")
        chk(rows and rows[0][0] > 16384, "库里的 bytes 记下了超限大小", str(rows[:1]))
        chk(rows and rows[0][1] == BIG, "库里的正文与原文逐字一致")

        # 重启期间的「当前剪贴板」换成新值：重启后它会被重新采集成首条，
        # 于是库里的 BIG 只能落在第二行——粘贴到 BIG 就证明是读库读回来的。
        section("重启（期间剪贴板换成另一个值，避免被『本次采集』冒充）")
        B = "PERSIST-B-%d" % int(time.time())
        set_clip(B)
        time.sleep(0.3)
        need(stop_app(), "实例已结束")
        start_app()
        need(app_running(), "程序已重启")

        section("读回：面板第二行是库里的长文本，能整段粘贴出来")
        proc, np = start_notepad()
        try:
            force_fg(np)
            time.sleep(0.4)
            win_v()
            p = wait_panel(visible=True)
            need(p, "面板已弹出")
            chk(vis(p), "面板可见")
            tap(VK["down"]); time.sleep(0.35)      # 首条 = 重启时采集的 B，下移一条
            tap(VK["enter"]); time.sleep(1.6)
            txt = edit_text(np)
            chk("BIGTEXT-" in txt, "重启后能粘贴出库里的那条（历史真的被读回）")
            chk(BIG_MARK in txt, "粘出来的是完整正文，不只是标题", "记事本长度=%d" % len(txt))

            section("搜索：正文只在库里也能命中")
            combo(VK["ctrl"], VK["a"]); tap(VK["del"]); time.sleep(0.3)
            force_fg(np)
            win_v()
            need(wait_panel(visible=True), "面板已再次弹出")
            uni(BIG_MARK)                          # 逐码元注入到面板搜索框
            time.sleep(0.8)
            chk(vis(panel()), "输入关键字后面板仍在")
            tap(VK["enter"]); time.sleep(1.6)
            txt2 = edit_text(np)
            chk(BIG_MARK in txt2, "按正文里的关键字搜索能命中并粘贴", "结果=%r" % txt2[:60])
        finally:
            kill(proc)

        section("删除一条：库里跟着消失（增量 DELETE，不是全量重写）")
        # 用 Del 键：裸键由钩子转发给面板，本机可靠。Ctrl+P / Ctrl+D 那条路要求面板
        # 真的拿到键盘焦点（ForceForeground 在 NOACTIVATE 面板上时成时不成），
        # 自动化下不稳，所以置顶/收藏的「按键→落库」留作人工确认（见文末说明）。
        before = query("SELECT count(*) FROM items")[0][0]
        h = panel()
        if not vis(h):
            for _ in range(6):
                win_v()
                if wait_panel(visible=True, timeout=1.0):
                    break
        need(vis(h), "面板已弹出")
        # 不再点页签：唤起后本来就在历史页，多一次点击反而可能落到别的控件上
        tap(VK["del"]); time.sleep(2.0)             # 删首行
        after = query("SELECT count(*) FROM items")[0][0]
        if _ini_value("confirmdel") == "1":
            print("  [SKIP] 删除前二次确认开着（confirmdel=1），Del 会先弹卡片，本节跳过")
        else:
            chk(after == before - 1, "删除后库里的行数跟着减 1",
                "before=%d after=%d" % (before, after))

        section("重启：库里的置顶生效、孤立像素被清掉")
        need(stop_app(), "实例已结束")
        imgdir = os.path.join(hist_dir(), "img")
        os.makedirs(imgdir, exist_ok=True)
        orphan = os.path.join(imgdir, "img-orphan-test.bin")
        with open(orphan, "wb") as f:
            f.write(b"orphan")
        con = sqlite3.connect(db_files()[0])
        con.execute("UPDATE items SET pinned=1 WHERE text=?", (A,))
        con.commit(); con.close()
        start_app()
        chk(not os.path.exists(orphan), "启动后没人引用的像素文件被清掉")
        rows = query("SELECT pinned FROM items WHERE text=?", (A,))
        chk(rows and rows[0][0] == 1, "置顶标记还在")
        proc2, np2 = start_notepad()
        try:
            force_fg(np2); time.sleep(0.4)
            combo(VK["ctrl"], VK["a"]); tap(VK["del"]); time.sleep(0.2)
            win_v()
            need(wait_panel(visible=True), "面板已弹出（置顶验证）")
            tap(VK["enter"]); time.sleep(1.2)       # 置顶的排在首行
            txt3 = edit_text(np2)
            chk(A in txt3, "重启后置顶的条目排在首行并粘得出来", "记事本=%r" % txt3[:60])
        finally:
            kill(proc2)

        section("改临时目录后数据库跟过去")
        # 用户改「临时文件目录」= 换了库位置。载入时若新位置没有 history.db
        # 却记着旧位置（%LOCALAPPDATA%\WinVClip\history_dir.txt），要把它搬过来，
        # 否则用户看到的就是「改个设置历史全没了」。
        ini = _ini_path()
        raw = ""
        if os.path.exists(ini):
            with open(ini, encoding="utf-8", newline="") as f:
                raw = f.read()
        newdir = os.path.join(os.environ.get("TEMP", "."), "wvh_test")
        shutil.rmtree(newdir, ignore_errors=True)
        old_db = db_files()[0]
        need(stop_app(), "实例已结束")
        try:
            lines = [("tempdir=" + newdir if l.startswith("tempdir=") else l)
                     for l in raw.split("\r\n")]
            with open(ini, "w", encoding="utf-8", newline="") as f:
                f.write("\r\n".join(lines))
            start_app()
            moved = os.path.join(newdir, "history", "history.db")
            chk(os.path.exists(moved), "新目录里出现 history.db")
            if os.path.exists(moved):
                con = sqlite3.connect(moved)
                try:
                    n = con.execute("SELECT count(*) FROM items WHERE text=?", (A,)).fetchone()[0]
                finally:
                    con.close()
                chk(n == 1, "迁过去的库里含迁移前的条目")
            chk(not os.path.exists(old_db), "旧目录的 history.db 已搬走（只保留一份）")
        finally:
            stop_app()
            if raw:
                with open(ini, "w", encoding="utf-8", newline="") as f:
                    f.write(raw)          # 还原用户的 tempdir
            shutil.rmtree(newdir, ignore_errors=True)

        section("收尾")
        need(stop_app(), "实例已结束")
    finally:
        # 把用户原来的历史还回去：测试不该在真实历史里留哨兵条目
        rm_db()
        if os.path.exists(legacy_dat()):
            os.remove(legacy_dat())
        if os.path.exists(legacy_dat() + ".old"):
            os.remove(legacy_dat() + ".old")
        if had_db or had_dat:
            for name in ["history.db", "history.db-wal", "history.db-shm", "history.dat"]:
                src = os.path.join(bak, name)
                if os.path.exists(src):
                    shutil.copy2(src, os.path.join(hist_dir(), name))
        shutil.rmtree(bak, ignore_errors=True)
        start_app()

    report()


if __name__ == "__main__":
    main()
