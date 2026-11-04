#!/usr/bin/env python3
"""半透半反面板"洗白"预览：把模拟器截图按面板特性压淡，用于颜色验收。

面板实测现象（见 docs/ui/README 与 helm_palette.h 的取色规则）：
  - 黑画成环境光的浅灰，暗饱和色整体抬亮；
  - 浅色填充（fill token）会淡到接近纸白。
这是个**近似模型**，不是校色：先按反射白混一层，再轻微去饱和。参数按现场
观感调（默认 k=0.35 / s=0.15），只用来判断"上板还分不分得清"，不用于比对色值。

用法：
    python3 panel_wash.py <src_dir_or_png> <dst_dir> [k] [s]
"""
import os
import sys

from PIL import Image

# 反射白：比纸色更亮一点，模拟环境光叠在屏上的底。
WHITE = (238, 233, 223)


def wash(im, k, s):
    im = im.convert("RGB")
    px = im.load()
    w, h = im.size
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            r = (1.0 - k) * r + k * WHITE[0]
            g = (1.0 - k) * g + k * WHITE[1]
            b = (1.0 - k) * b + k * WHITE[2]
            # 去饱和：往自身亮度靠 s。
            lum = 0.299 * r + 0.587 * g + 0.114 * b
            r = r + (lum - r) * s
            g = g + (lum - g) * s
            b = b + (lum - b) * s
            px[x, y] = (int(round(r)), int(round(g)), int(round(b)))
    return im


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    src, dst = sys.argv[1], sys.argv[2]
    k = float(sys.argv[3]) if len(sys.argv) > 3 else 0.35
    s = float(sys.argv[4]) if len(sys.argv) > 4 else 0.15
    os.makedirs(dst, exist_ok=True)

    if os.path.isdir(src):
        names = sorted(n for n in os.listdir(src) if n.lower().endswith(".png"))
        for n in names:
            im = Image.open(os.path.join(src, n))
            wash(im, k, s).save(os.path.join(dst, n))
        print("washed %d files -> %s" % (len(names), dst))
    else:
        im = Image.open(src)
        out = os.path.join(dst, os.path.basename(src))
        wash(im, k, s).save(out)
        print(out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
