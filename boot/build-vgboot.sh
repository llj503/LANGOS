#!/bin/bash
# ============================================================================
# build-vgboot-ml.sh —— 给 MINILOONG 交叉编译 vgboot（自绘版引导屏渲染器）
#
# 不装交叉发行版的 freetype：**直接用镜像自己的那份**（头 + .so），
# 版本天然对齐，也省得猜 ABI。
#
# 用法：bash build-vgboot-ml.sh
# 产出：payload-miniloong/boot/vgboot        （aarch64 可执行）
#       build/ml-sysroot/                     （从镜像抠出来的头与库）
# ============================================================================
set -u
IMG="${IMG:-/mnt/d/MINILOONG_trixie_09042026.img.7z/dArkOS_MINILOONG_trixie_09042026.img}"
PROJ=/mnt/d/Projects/dArkOS-WebFront
SRC="$PROJ/payload-miniloong/boot/vgboot.c"
OUT="$PROJ/payload-miniloong/boot/vgboot"
SYS="$PROJ/build/ml-sysroot"
CC=aarch64-linux-gnu-gcc

say() { echo "[+] $*"; }
die() { echo "[x] $*" >&2; exit 1; }

[ -f "$SRC" ] || die "缺源码 $SRC"
[ -f "$IMG" ] || die "缺镜像 $IMG"

# ---------------------------------------------------------------- sysroot
if [ ! -f "$SYS/usr/include/freetype2/ft2build.h" ] || [ ! -f "$SYS/usr/lib/libfreetype.so.6" ]; then
    say "从镜像抠 freetype 头与库 → $SYS"
    L="$(losetup -fP --read-only --show "$IMG")" || die "losetup 失败"
    trap 'mountpoint -q /mnt/ml-sys && umount /mnt/ml-sys; losetup -d "$L" 2>/dev/null' EXIT
    mkdir -p /mnt/ml-sys "$SYS/usr/lib" "$SYS/usr/include"
    mount -o ro "${L}p4" /mnt/ml-sys || die "挂 p4 失败"
    cp -a /mnt/ml-sys/usr/include/freetype2 "$SYS/usr/include/" || die "拷头失败"
    cp -fL /mnt/ml-sys/usr/lib/aarch64-linux-gnu/libfreetype.so.6 "$SYS/usr/lib/libfreetype.so.6" \
        || die "拷 libfreetype 失败"
    umount /mnt/ml-sys
    losetup -d "$L"
    trap - EXIT
    say "头：$(find "$SYS/usr/include/freetype2" -name '*.h' | wc -l) 个"
    say "库：$(ls -l "$SYS/usr/lib/libfreetype.so.6" | awk '{print $5}') 字节"
fi

# ---------------------------------------------------------------- 编译
say "编译 $SRC"
# shellcheck disable=SC2086
$CC -O2 -Wall -Wextra -static-libgcc \
    -I"$SYS/usr/include/freetype2" \
    -o "$OUT" "$SRC" \
    -L"$SYS/usr/lib" -l:libfreetype.so.6 -lm \
    -Wl,--allow-shlib-undefined
# --allow-shlib-undefined：libfreetype.so.6 自己 NEEDED 了 libpng16/libbz2/libz，
# 那些库在目标机上有，咱们只拿了 freetype 一份，不让链接器去追它的整条依赖链。
rc=$?
[ $rc -eq 0 ] || die "编译失败 rc=$rc"

say "产物 $OUT  $(stat -c%s "$OUT") 字节"
file "$OUT"

say "需要的 glibc 符号版本（镜像里是 2.41，超了就跑不起来）"
aarch64-linux-gnu-objdump -T "$OUT" 2>/dev/null | grep -oE 'GLIBC_[0-9.]+' | sort -u -V | tr '\n' ' '
echo
say "依赖的 .so"
aarch64-linux-gnu-objdump -p "$OUT" 2>/dev/null | grep NEEDED | sed 's/^/    /'
echo "done"
