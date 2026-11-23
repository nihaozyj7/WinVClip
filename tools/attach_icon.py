# attach_icon.py —— 把 windres 编好的资源节（.rsrc）附加到已链接的 exe 上，并填好 PE 的资源目录。
#
# 为什么不直接让链接器带上 app.rc.obj：
# 本机 binutils 2.47（MinGW-w64 r1）的 ld 在合并 .rsrc 时会把每个 icon 叶判定成
# 「duplicate leaf」，链接直接失败（--unique 还会触发 bfd 断言崩溃）。只用 STRINGTABLE
# 则正常，说明是它对 ICON/GROUP_ICON 的合并逻辑坏了。所以绕开 ld：先纯 C++ 链接出 exe，
# 再把 windres 生成的资源节原样贴上去。
#
# 除了「新增一个节、把 DataDirectory[2] 指向它」，还必须处理一件事：windres 生成的
# .rsrc 里，各层目录项与 IMAGE_RESOURCE_DATA_ENTRY.OffsetToData 存的都是 RVA，而目标
# 文件里这些字段还是「相对节首的偏移」——真正的 base 由链接期的重定位项补上
# （objdump -r 能看到一串指向 .rsrc 自身的 ADDR32NB）。跳过链接器，就得自己按
# 「value = 节RVA + 原值」把重定位应用一遍，否则 Windows 解析不到任何资源数据。
#
# 用法: python tools/attach_icon.py <exe> <windres 输出的 .obj>

import os
import struct
import sys

RT_RSRC = 2


def align_up(v, a):
    return (v + a - 1) // a * a


def read_obj_rsrc(obj_path):
    """取出 .rsrc 节字节，以及节内需要加 base 的重定位字段偏移。"""
    data = open(obj_path, "rb").read()
    assert data[:2] == b"\x64\x86", "不是 x86-64 COFF 目标文件"
    nsec = struct.unpack_from("<H", data, 2)[0]
    optsz = struct.unpack_from("<H", data, 16)[0]
    sec_off = 20 + optsz
    rsrc = None
    for i in range(nsec):
        p = sec_off + i * 40
        name = data[p:p + 8].rstrip(b"\0")
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, p + 8)
        nrel = struct.unpack_from("<H", data, p + 32)[0]
        relptr = struct.unpack_from("<I", data, p + 24)[0]
        if name == b".rsrc":
            rsrc = data[rawptr:rawptr + rawsize]
            relocs = []
            for k in range(nrel):
                off, sym, typ = struct.unpack_from("<IIH", data, relptr + k * 10)
                # IMAGE_REL_AMD64_ADDR32NB(3) / IMAGE_REL_I386_DIR32NB(7)：值 = 目标RVA + 原值
                if typ in (3, 7):
                    relocs.append(off)
                else:
                    raise SystemExit("意外的重定位类型 %d（偏移 0x%X）" % (typ, off))
            return rsrc, relocs
    raise SystemExit("目标文件里没有 .rsrc 节: %s" % obj_path)


def apply_relocs(buf, relocs, base_rva):
    """把「节内偏移」改写成真正的 RVA：value = base_rva + 原值。"""
    out = bytearray(buf)
    for off in relocs:
        v = struct.unpack_from("<I", out, off)[0]
        struct.pack_into("<I", out, off, v + base_rva)
    return bytes(out)


def main(exe_path, obj_path):
    rsrc, relocs = read_obj_rsrc(obj_path)
    img = bytearray(open(exe_path, "rb").read())

    pe = struct.unpack_from("<I", img, 0x3C)[0]
    assert img[pe:pe + 4] == b"PE\0\0"
    nsec = struct.unpack_from("<H", img, pe + 6)[0]
    optsz = struct.unpack_from("<H", img, pe + 20)[0]
    magic = struct.unpack_from("<H", img, pe + 24)[0]
    opt = pe + 24
    if magic == 0x20B:      # PE32+
        ddir = opt + 112
        size_of_image_off = opt + 56
    else:                   # PE32
        ddir = opt + 96
        size_of_image_off = opt + 56
    sect_align, file_align = struct.unpack_from("<II", img, opt + 32)
    size_of_headers = struct.unpack_from("<I", img, opt + 60)[0]

    sec_tab = opt + optsz
    last_vend = 0
    last_fileend = 0
    existing = None     # (节头偏移, vsize, vaddr, rawsize, rawptr)
    for i in range(nsec):
        p = sec_tab + i * 40
        name = bytes(img[p:p + 8]).rstrip(b"\0")
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", img, p + 8)
        if name == b".rsrc":
            existing = (p, vsize, vaddr, rawsize, rawptr)
        last_vend = max(last_vend, vaddr + max(vsize, rawsize))
        last_fileend = max(last_fileend, rawptr + rawsize)

    # 链接器默认会给每个 exe 塞一个 .rsrc 节（mingw-w64 的默认 manifest 等）。
    # 它通常比我们的图标大得多，直接原地覆盖最省事：资源树从节首解析，覆盖后目录
    # 就只指向我们的图标。留在后面的旧字节无害（节 VirtualSize 会盖住它们）。
    if existing and existing[3] >= align_up(len(rsrc), file_align):
        p, vsize, vaddr, rawsize, rawptr = existing
        raw = apply_relocs(rsrc, relocs, vaddr)
        raw += b"\0" * (align_up(len(raw), file_align) - len(raw))
        img[rawptr:rawptr + len(raw)] = raw
        struct.pack_into("<I", img, p + 8, max(vsize, len(rsrc)))
        struct.pack_into("<II", img, ddir + RT_RSRC * 8, vaddr, len(rsrc))
        open(exe_path, "wb").write(img)
        print("replaced .rsrc: %d bytes -> %s (rva 0x%X)" % (len(rsrc), exe_path, vaddr))
        return

    # 节表末尾之后到文件头区结束还有多少空位：不够就没法原地加节。
    max_sec = (size_of_headers - sec_tab) // 40
    if nsec + 1 > max_sec:
        raise SystemExit("PE 头里没有空闲节表项（已有 %d，上限 %d），无法追加 .rsrc"
                         % (nsec, max_sec))

    new_rva = align_up(last_vend, sect_align)
    new_ptr = align_up(max(last_fileend, len(img)), file_align)
    raw = apply_relocs(rsrc, relocs, new_rva)
    raw += b"\0" * (align_up(len(raw), file_align) - len(raw))

    # 追加节数据
    if len(img) < new_ptr:
        img += b"\0" * (new_ptr - len(img))
    img += raw

    # 写节头
    hdr = bytearray(40)
    hdr[0:8] = b".rsrc\0\0\0"
    struct.pack_into("<IIII", hdr, 8, len(rsrc), new_rva, len(raw), new_ptr)
    struct.pack_into("<I", hdr, 36, 0x40000040)   # 已初始化数据 | 可读
    p = sec_tab + nsec * 40
    img[p:p + 40] = hdr
    struct.pack_into("<H", img, pe + 6, nsec + 1)

    # 资源数据目录 + SizeOfImage
    struct.pack_into("<II", img, ddir + RT_RSRC * 8, new_rva, len(rsrc))
    size_of_image = align_up(new_rva + len(raw), sect_align)
    struct.pack_into("<I", img, size_of_image_off, size_of_image)

    open(exe_path, "wb").write(img)
    print("attached .rsrc: %d bytes -> %s (rva 0x%X)" % (len(rsrc), exe_path, new_rva))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    main(sys.argv[1], sys.argv[2])
