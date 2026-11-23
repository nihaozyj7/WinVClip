# -*- coding: utf-8 -*-
"""解析字形图集缓存，检查内容是否正确（不同 slot 是否真的不同）。"""
import io, os, struct, collections

P = os.path.join(os.environ["LOCALAPPDATA"], "WinVClip", "emoji-atlas-v2-44px-46.bin")
d = io.open(P, "rb").read()
print("size", len(d))
hdr = struct.unpack_from("<8I", d, 0)
print("hdr magic=%08x ver=%d sz=%d stride=%d cols=%d slots=%d count=%d axis=%d" % hdr)
_, _, sz, stride, cols, slots, count, axis = hdr

off = 32
idx = []
for _ in range(count):
    s, l = struct.unpack_from("<II", d, off); off += 8
    k = d[off:off + l].decode("utf-8", "replace"); off += l
    idx.append((s, k))
print("index bytes", off - 32, "first", idx[:3], "last", idx[-3:])
print("dup slots:", [s for s, c in collections.Counter(s for s, _ in idx).items() if c > 1][:10])
print("slot range", min(s for s, _ in idx), max(s for s, _ in idx))

cell = stride * stride * 4
print("cell bytes", cell, "need", off + slots * cell, "actual", len(d))

def cellbytes(slot):
    return d[off + slot * cell: off + (slot + 1) * cell]

# 抽几个 slot 比较像素内容是否相同
sig = {}
for s, k in idx[:20]:
    c = cellbytes(s)
    sig[s] = (len(set(c)), c[:16].hex())
for s, v in list(sig.items())[:8]:
    print("slot", s, "uniq-bytes", v[0], "head", v[1])

# 统计全图里有多少个非空格子（全 0 表示未写入）
nz = 0
for s in range(slots):
    c = cellbytes(s)
    if any(c):
        nz += 1
print("non-empty cells", nz, "/", slots)

# 抽出前 6 个格子写成 PNG 便于肉眼核对
try:
    from PIL import Image
    imgs = []
    for i, (s, k) in enumerate(idx[:6]):
        img = Image.frombytes("RGBA", (stride, stride), cellbytes(s), "raw", "BGRA")
        imgs.append(img)
    sheet = Image.new("RGBA", (stride * 6, stride), (255, 255, 255, 255))
    for i, im in enumerate(imgs):
        sheet.alpha_composite(im, (i * stride, 0))
    out = os.path.join(os.path.dirname(P), "dump-cells.png")
    sheet.save(out)
    print("wrote", out, [k for _, k in idx[:6]])
except Exception as e:
    print("PIL failed:", e)
