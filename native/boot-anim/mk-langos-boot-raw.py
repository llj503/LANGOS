#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""mk-langos-boot-raw.py —— 把 LANGOS 开机图转成 /dev/fb0 可直接 memcpy 的原始数据

背景（2026-09-18 实测标定，见 pc/req578/579-fb0-*.sh）：
  · 面板物理 720x960、stride 2880、32bpp、双缓冲（yres_virtual=1920）
  · 像素排列 = BGRA（小端 XRGB8888）
  · ★ 映射公式：  fb(u,v) = 源(x = 959 - v, y = u)
       u = fb 的列（0..719），v = fb 的行（0..959）
       等价于「源图逆时针转 90°」—— PIL 里正好是 im.rotate(90, expand=True)
  · 实测验证：画「┌」形标记写入后截图，白条精确落在顶边与左边、红块在左上角

★★ 2026-09-21 改造：分辨率自适应（浪总要求）
  原来只出 720x960（MINILOONG）。现在按 `--size WxH` 出任意屏幕的 raw，
  并加一个 12 字节文件头（魔数 "LGBL" + 屏宽 + 屏高），让设备上的
  langos-bootanim 自己读出底图尺寸，从而推出映射方向（横屏 1:1 / 竖屏转 90°）。

  · 竖屏侧装（MINILOONG，屏 720x960，源图 960x720）→ 旋转 90°，输出 720x960
  · 横屏（R36H ProMax，屏 1024x768，源图 1024x768）→ 不旋转，输出 1024x768

  注意：**老格式 raw（无头）仍然被设备端识别**（按 960x720 认），所以这个脚本
  升级后，旧固件里的 raw 不用重出；反过来，新出的带头的 raw 也能装到新固件上。

产物：
    build/boot-anim/langos-boot.raw            屏 720x960（老命名，给 MINILOONG）
    build/boot-anim/langos-boot-<W>x<H>.raw    其它屏幕尺寸
"""
import os
import struct
import sys

from PIL import Image

sys.stdout.reconfigure(encoding="utf-8")

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)
ANIM = os.path.join(PROJ, "build", "boot-anim")

RAW_MAGIC = 0x4C42474C      # "LGBL"，与 langos-bootanim.c 的 RAW_MAGIC 一致

# 屏幕尺寸（fb 可见区，列 x 行）。默认 = MINILOONG（竖屏侧装）。
SCR_W, SCR_H = 720, 960
SRC_ARG = ""
OUT_ARG = ""

_argv = sys.argv[1:]
for _i, _a in enumerate(_argv):
    if _a == "--size" and _i + 1 < len(_argv):
        _w, _h = _argv[_i + 1].lower().split("x")
        SCR_W, SCR_H = int(_w), int(_h)
    elif _a == "--src" and _i + 1 < len(_argv):
        SRC_ARG = _argv[_i + 1]
    elif _a == "--out" and _i + 1 < len(_argv):
        OUT_ARG = _argv[_i + 1]


def default_src():
    """按屏幕尺寸猜源图名（项目里已备好 bootlogo-langos-<W>x<H>.png 这些）。"""
    if SCR_W > SCR_H:
        return "bootlogo-langos-%dx%d.png" % (SCR_W, SCR_H)
    # 竖屏：源图是"正立"的，尺寸对调
    return "bootlogo-langos-%dx%d.png" % (SCR_H, SCR_W)


def pick_src():
    if SRC_ARG:
        return SRC_ARG if os.path.isabs(SRC_ARG) else os.path.join(PROJ, "build", SRC_ARG)
    cand = os.path.join(PROJ, "build", default_src())
    if os.path.exists(cand):
        return cand
    # 兜底：老命名（不带尺寸后缀的那份是 1024x768 的默认产物）
    return os.path.join(PROJ, "build", "bootlogo-langos.png")


SRC = pick_src()
if OUT_ARG:
    OUT = OUT_ARG if os.path.isabs(OUT_ARG) else os.path.join(PROJ, OUT_ARG)
elif (SCR_W, SCR_H) == (720, 960):
    OUT = os.path.join(ANIM, "langos-boot.raw")     # 老命名，兼容既有引用
else:
    OUT = os.path.join(ANIM, "langos-boot-%dx%d.raw" % (SCR_W, SCR_H))

if not os.path.exists(SRC):
    raise SystemExit("★ 缺源图：%s\n  （可用 build/make-bootlogo.py --size WxH 先生成）" % SRC)

im = Image.open(SRC).convert("RGB")
print("源图 %s  %s" % (SRC, im.size))

# 竖屏侧装 → 逆时针 90°；横屏 → 原样
rot90 = (SCR_W < SCR_H)
if rot90:
    if im.size != (SCR_H, SCR_W):
        raise SystemExit("★ 竖屏模式要求源图是 %dx%d，实际 %s" % (SCR_H, SCR_W, im.size))
    work = im.rotate(90, expand=True)
    print("旋转 90°（逆时针）→ %s" % (work.size,))
else:
    if im.size != (SCR_W, SCR_H):
        raise SystemExit("★ 横屏模式要求源图是 %dx%d，实际 %s" % (SCR_W, SCR_H, im.size))
    work = im
    print("横屏，不旋转")

assert work.size == (SCR_W, SCR_H), (work.size, SCR_W, SCR_H)

r, g, b = work.split()
bgra = Image.merge("RGBA", (b, g, r, Image.new("L", work.size, 0)))
data = bgra.tobytes()
expect = SCR_W * SCR_H * 4
print("打包 %d 字节（期望 %d）%s" % (len(data), expect, "✔" if len(data) == expect else "✘"))
assert len(data) == expect

os.makedirs(os.path.dirname(OUT), exist_ok=True)
with open(OUT, "wb") as f:
    # 12 字节头：魔数 + 屏宽 + 屏高（小端）。设备端 read_raw_header() 读它。
    f.write(struct.pack("<III", RAW_MAGIC, SCR_W, SCR_H))
    f.write(data)

# 顺手存一张预览（按屏幕朝向还原，用来肉眼确认）
prev = Image.frombytes("RGBA", (SCR_W, SCR_H), data).convert("RGB")
if rot90:
    prev = prev.rotate(-90, expand=True)
if (SCR_W, SCR_H) == (720, 960):
    prev_name = "langos-boot-preview.png"
else:
    prev_name = "langos-boot-preview-%dx%d.png" % (SCR_W, SCR_H)
prev.save(os.path.join(ANIM, prev_name))

print("已写 %s  (%d 字节，含 12 字节头)" % (OUT, os.path.getsize(OUT)))
print("预览 build/boot-anim/%s  ← 那张就是屏幕上的样子" % prev_name)
