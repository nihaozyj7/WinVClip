# verify_icon.py —— 构建后自检：确认 exe 的资源目录里有一个含多尺寸帧的 GROUP_ICON。
# 由 CMake 的 POST_BUILD 之外单独调用（或手工跑），失败即说明图标没贴上。
import struct, sys

RT_GROUP_ICON = 14


def main(path):
    data = open(path, "rb").read()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    optsz = struct.unpack_from("<H", data, pe + 20)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    ddir = opt + (112 if magic == 0x20B else 96)
    rs_va, rs_size = struct.unpack_from("<II", data, ddir + 2 * 8)
    if not rs_size:
        raise SystemExit("FAIL: exe 没有资源目录")
    secs = []
    for i in range(nsec):
        p = opt + optsz + i * 40
        vsz, va = struct.unpack_from("<II", data, p + 8)
        rawsz, rawptr = struct.unpack_from("<II", data, p + 16)
        secs.append((va, max(vsz, rawsz), rawptr))

    def off(rva):
        for va, sz, rawptr in secs:
            if va <= rva < va + sz:
                return rawptr + (rva - va)
        raise SystemExit("FAIL: 资源 RVA 0x%X 不在任何节内" % rva)

    base = off(rs_va)

    def entries(rel):
        o = base + rel
        nn, ni = struct.unpack_from("<HH", data, o + 12)
        return [struct.unpack_from("<II", data, o + 16 + i * 8) for i in range(nn + ni)]

    for tid, sub in entries(0):
        if tid != RT_GROUP_ICON:
            continue
        for gid, sub2 in entries(sub & 0x7FFFFFFF):
            for _lang, sub3 in entries(sub2 & 0x7FFFFFFF):
                rva, size = struct.unpack_from("<II", data, base + (sub3 & 0x7FFFFFFF))
                cnt = struct.unpack_from("<H", data, off(rva) + 4)[0]
                if cnt < 2:
                    raise SystemExit("FAIL: GROUP_ICON 只有 %d 帧" % cnt)
                print("OK: GROUP_ICON id=%d，%d 帧" % (gid, cnt))
                return
    raise SystemExit("FAIL: 找不到 GROUP_ICON")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "build/winvclip.exe")
