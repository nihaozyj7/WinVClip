# -*- coding: utf-8 -*-
# 往剪贴板塞测试图片：PNG 格式 / CF_DIB 格式 / 带透明通道的 PNG
# 用法: python test_clip.py png|dib|alpha|all
# --- 路径基准：从 tests/ 回到项目根（P0 结构整理时注入） ---
import os as _os, sys as _sys
_os.chdir(_os.path.dirname(_os.path.dirname(_os.path.abspath(__file__))))
import ctypes, io, sys, time
from ctypes import wintypes
from PIL import Image, ImageDraw

u = ctypes.windll.user32
k = ctypes.windll.kernel32
k.GlobalAlloc.restype = ctypes.c_void_p
k.GlobalAlloc.argtypes = [wintypes.UINT, ctypes.c_size_t]
k.GlobalLock.restype = ctypes.c_void_p
k.GlobalLock.argtypes = [ctypes.c_void_p]
k.GlobalUnlock.argtypes = [ctypes.c_void_p]
u.OpenClipboard.argtypes = [ctypes.c_void_p]
u.OpenClipboard.restype = wintypes.BOOL
u.SetClipboardData.restype = ctypes.c_void_p
u.SetClipboardData.argtypes = [wintypes.UINT, ctypes.c_void_p]
u.RegisterClipboardFormatW.restype = wintypes.UINT
u.RegisterClipboardFormatW.argtypes = [wintypes.LPCWSTR]

CF_DIB = 8
FMT_PNG = u.RegisterClipboardFormatW("PNG")


def put(items):
    for _ in range(20):
        if u.OpenClipboard(None):
            break
        time.sleep(0.05)
    else:
        raise RuntimeError("OpenClipboard failed")
    u.EmptyClipboard()
    for fmt, data in items:
        h = k.GlobalAlloc(2, len(data))  # GMEM_MOVEABLE
        p = k.GlobalLock(h)
        ctypes.memmove(p, data, len(data))
        k.GlobalUnlock(h)
        u.SetClipboardData(fmt, h)
    u.CloseClipboard()


def screenshot_like():
    im = Image.new("RGB", (1600, 900), (18, 24, 38))
    d = ImageDraw.Draw(im)
    for y in range(900):
        d.line([(0, y), (1600, y)], fill=(18 + y // 12, 24 + y // 20, 60 + y // 8))
    d.rounded_rectangle([80, 90, 760, 500], 24, fill=(248, 248, 252), outline=(255, 107, 53), width=4)
    d.rounded_rectangle([840, 160, 1500, 420], 18, fill=(255, 224, 210))
    d.ellipse([200, 200, 420, 420], fill=(255, 107, 53))
    d.rectangle([200, 570, 1400, 640], fill=(60, 70, 90))
    d.rectangle([200, 680, 900, 730], fill=(90, 100, 120))
    buf = io.BytesIO()
    im.save(buf, "PNG")
    return [(FMT_PNG, buf.getvalue())]


def dib_like():
    im = Image.new("RGB", (640, 640), (255, 255, 255))
    d = ImageDraw.Draw(im)
    for i in range(8):
        for j in range(8):
            if (i + j) % 2 == 0:
                d.rectangle([i * 80, j * 80, i * 80 + 80, j * 80 + 80],
                            fill=(30 + i * 28, 60 + j * 22, 200 - i * 18))
    d.ellipse([180, 180, 460, 460], fill=(216, 58, 58))
    buf = io.BytesIO()
    im.save(buf, "BMP")
    return [(CF_DIB, buf.getvalue()[14:])]  # 去掉 BITMAPFILEHEADER


def alpha_like():
    im = Image.new("RGBA", (480, 480), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.ellipse([40, 40, 440, 440], fill=(255, 107, 53, 255))
    d.ellipse([160, 160, 320, 320], fill=(255, 253, 248, 255))
    buf = io.BytesIO()
    im.save(buf, "PNG")
    return [(FMT_PNG, buf.getvalue())]


def bmp24_like():
    im = Image.new("RGB", (900, 300), (240, 240, 240))
    d = ImageDraw.Draw(im)
    for i in range(30):
        d.rectangle([i * 30, 0, i * 30 + 15, 300], fill=(20 + i * 7, 80, 160))
    buf = io.BytesIO()
    im.save(buf, "BMP")
    return [(CF_DIB, buf.getvalue()[14:])]


def rgba_dib_like():
    # 真实截图工具常见的 32bpp CF_DIB，alpha 位全 0
    import struct
    w, h = 1000, 600
    im = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(im)
    for y in range(h):
        d.line([(0, y), (w, y)], fill=(40 + y // 6, 120, 200 - y // 8))
    d.rounded_rectangle([120, 120, 520, 460], 30, fill=(255, 253, 248), outline=(255, 107, 53), width=8)
    d.ellipse([660, 180, 900, 420], fill=(47, 155, 104))
    px = bytearray(w * h * 4)
    rgb = im.tobytes()
    for i in range(w * h):
        px[i * 4]     = rgb[i * 3 + 2]
        px[i * 4 + 1] = rgb[i * 3 + 1]
        px[i * 4 + 2] = rgb[i * 3]
        px[i * 4 + 3] = 0                          # alpha 位全 0，和截图工具一致
    px = bytes(px)
    bi = struct.pack("<IiiHHIIiiII", 40, w, h, 1, 32, 0, len(px), 0, 0, 0, 0)
    return [(CF_DIB, bi + px)]


def big_like():
    im = Image.new("RGB", (3840, 2160), (10, 16, 30))
    d = ImageDraw.Draw(im)
    for i in range(24):
        d.rectangle([i * 160, 0, i * 160 + 80, 2160], fill=(20 + i * 9, 90, 180))
    d.ellipse([1200, 600, 2600, 2000], fill=(255, 107, 53))
    buf = io.BytesIO()
    im.save(buf, "PNG")
    return [(FMT_PNG, buf.getvalue())]


MAP = {"png": screenshot_like, "dib": dib_like, "alpha": alpha_like,
       "bmp24": bmp24_like, "rgbx": rgba_dib_like, "big": big_like}

if __name__ == "__main__":
    which = sys.argv[1] if len(sys.argv) > 1 else "all"
    names = list(MAP) if which == "all" else [which]
    for n in names:
        put(MAP[n]())
        print("pushed", n, flush=True)
        time.sleep(0.8)
