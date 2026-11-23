# make_icon.py —— 由一张方形 PNG 母版生成多尺寸 app.ico。
# 用法: python tools/make_icon.py <源.png> [输出.ico] [--corner=0.22]
#   --corner 圆角半径占边长的比例（0 = 不加圆角，保留母版原样）；默认 0.22。
# 生成脚本与母版都留在仓库里，图标随时可重建（不依赖外部工具）。
import sys, os

from PIL import Image, ImageChops, ImageDraw

SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]
CANVAS = 256          # 母版统一放大到 256，再交给 PIL 降采样出各帧
SS = 8                # 圆角遮罩的超采样倍数（抗锯齿）


def with_rounded_corner(im, ratio):
    """把 alpha 乘上一个居中圆角矩形遮罩（radius = ratio * 边长）。"""
    if ratio <= 0:
        return im
    w, h = im.size
    r = ratio * min(w, h)
    mask = Image.new("L", (w * SS, h * SS), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        [0, 0, w * SS - 1, h * SS - 1], radius=r * SS, fill=255)
    mask = mask.resize((w, h), Image.LANCZOS)
    out = im.copy()
    out.putalpha(ImageChops.multiply(im.split()[3], mask))
    return out


def main():
    argv = [a for a in sys.argv[1:] if not a.startswith("--")]
    corner = 0.22
    for a in sys.argv[1:]:
        if a.startswith("--corner="):
            corner = float(a.split("=", 1)[1])

    src = argv[0] if argv else "resources/app.png"
    dst = argv[1] if len(argv) > 1 else os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "resources", "app.ico")

    im = Image.open(src).convert("RGBA")
    # PIL 只写入 <= 母版边长的尺寸（超出会静默跳过），先把母版补到 256 再写，
    # 否则资源管理器的大图标视图会拿不到 256 帧、只能靠系统缩放模糊版。
    if im.size != (CANVAS, CANVAS):
        im = im.resize((CANVAS, CANVAS), Image.LANCZOS)
    im = with_rounded_corner(im, corner)

    os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
    im.save(dst, format="ICO", sizes=[(s, s) for s in SIZES])
    print("wrote %s %d bytes (corner=%.2f, r=%.1fpx @256)"
          % (dst, os.path.getsize(dst), corner, corner * CANVAS))


if __name__ == "__main__":
    main()
