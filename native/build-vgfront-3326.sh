#!/bin/bash
# ============================================================================
#  build-vgfront-3326.sh —— 在 R36hProMax（RK3326 / dArkOS4Clone）上编 vgfront
#
#  ⚠ 为什么和 build-vgfront.sh（MiniLoong 那份）分开：
#    两台机器都跑 Linux/aarch64，但开发环境的**头文件版本不一样**：
#
#      |            | MiniLoong (RK3566) | R36hProMax (RK3326)      |
#      |------------|--------------------|--------------------------|
#      | 发行版      | Debian 13          | Ubuntu 19.10 (eoan)      |
#      | SDL2 运行时 | 2.26+              | **2.0.32**（自编 vendor）|
#      | SDL2 头文件 | 同版本             | **2.0.10**（老 apt 包）  |
#      | 库位置      | /usr/lib           | 裸 gcc 走系统库          |
#
#    → 3326 这边**头比库老 22 个小版本**，`SDL_Vertex` / `SDL_RenderGeometry`
#      （2.0.18 才加）在头里没有、但库里**确实有**（`nm -D` 能查到）。
#      解法：编的时候 `-include sdl2_compat.h` 把缺的声明补上。
#      那个头用 `#if !SDL_VERSION_ATLEAST(2,0,18)` 包着 ⇒ 在 MiniLoong 这种
#      新头环境下展开成空操作，**同一份源码两端通吃**。
#
#  用法（在设备上、ark 用户即可，gcc 不需要 root）：
#      bash build-vgfront-3326.sh [源码] [输出]
#      默认：源码 /home/ark/vgfront.c，输出 /home/ark/vgfront.new
#
#  编完自己 `ldd -r` 一遍确认没有 undefined；然后：
#      sudo systemctl stop emulationstation
#      sudo cp <输出> /opt/vagaretro/native/vgfront && sudo chmod 755 $_
#      sudo systemctl start emulationstation
# ============================================================================
set -eu

SRC="${1:-/home/ark/vgfront.c}"
OUT="${2:-/home/ark/vgfront.new}"
DEV="${VG_SDL_DEV:-/opt/vagaretro/sdl2dev}"
COMPAT="$(dirname "$SRC")/sdl2_compat.h"

echo "[build-3326] 源码   $SRC"
echo "[build-3326] 输出   $OUT"
echo "[build-3326] SDL 头 $DEV/inc/SDL2"

if [ ! -f "$SRC" ]; then
    echo "[build-3326] 找不到源码 $SRC" >&2
    exit 1
fi

# 兼容头必须和源码在一起；丢了就补一份最小可用的
if [ ! -f "$COMPAT" ]; then
    echo "[build-3326] 警告：缺 $COMPAT，SDL_Vertex 可能编不过" >&2
    echo "[build-3326]       从 PC 侧推 sdl2_compat.h 上来" >&2
fi

# 头文件环境（幂等；缺什么补什么）
if [ ! -f "$DEV/inc/SDL2/SDL.h" ]; then
    echo "[build-3326] 头文件环境不在 $DEV，跑一遍 dev-env.sh"
    bash "$(dirname "$SRC")/dev-env.sh" || true
fi

set -x
gcc -O2 -w \
    -include "$COMPAT" \
    -isystem "$DEV/inc/SDL2" \
    -isystem "$DEV/inc" \
    -isystem "$DEV/inc/aarch64-linux-gnu" \
    -o "$OUT" "$SRC" \
    -L"$DEV/lib" \
    -lSDL2 -lSDL2_ttf -lSDL2_image -lm
set +x

if [ ! -x "$OUT" ]; then
    echo "[build-3326] 失败：没有产出 $OUT" >&2
    exit 2
fi

echo "[build-3326] 产出 $(stat -c '%s' "$OUT") 字节"
echo "[build-3326] md5 $(md5sum "$OUT" | cut -d' ' -f1)"
echo "[build-3326] 未定义符号检查："
# ldd -r 会把 undefined symbol 打到 stderr
if ldd -r "$OUT" 2>&1 | grep -q 'undefined symbol'; then
    echo "[build-3326] ⚠ 有未定义符号，先别装机！" >&2
    ldd -r "$OUT" 2>&1 | grep 'undefined symbol' | head -20 >&2
    exit 3
fi
echo "[build-3326] OK —— 没有未定义符号，可以装机"
