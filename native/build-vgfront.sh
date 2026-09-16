#!/bin/bash
# ============================================================================
#  build-vgfront.sh —— 在设备上编译原生前端
#
#  头文件环境（/usr/include 被镜像精简掉了）由 native/dev-env.sh 幂等准备，
#  它设好 $VG_CFLAGS / $VG_LDFLAGS。
#
#  用法：
#      rsh.py -f native/build-vgfront.sh                  # 编 /tmp/vgfront.c -> /tmp/vgfront
#      rsh.py -f native/build-vgfront.sh -- /tmp/a.c /tmp/b
#      rsh.py -f native/build-vgfront.sh -- /tmp/vgfront.c /tmp/vgfront \
#                 /opt/vagaretro/native/vgfront          # 编完直接装到启动器认的位置
#
#  注意：/tmp/vgfront 被跑着的时候覆盖会得到 "Text file busy"，
#  先停掉再编（native/run-vgfront.sh 那套流程就是这么做的）。
# ============================================================================
set -u

SRC="${1:-/tmp/vgfront.c}"
OUT="${2:-/tmp/vgfront}"
DST="${3:-}"

if [ ! -f "$SRC" ]; then
    echo "缺源文件 $SRC"
    exit 1
fi

if [ -f /opt/vagaretro/native/dev-env.sh ]; then
    # shellcheck disable=SC1091
    source /opt/vagaretro/native/dev-env.sh
else
    echo "警告: 找不到 dev-env.sh，按裸环境编译"
    VG_CFLAGS=""
    VG_LDFLAGS=""
fi

echo "== 编译 $SRC -> $OUT =="
echo "VG_CFLAGS  = ${VG_CFLAGS:-(空)}"
echo "VG_LDFLAGS = ${VG_LDFLAGS:-(空)}"
echo

# shellcheck disable=SC2086
gcc $VG_CFLAGS -O2 -Wall -o "$OUT" "$SRC" $VG_LDFLAGS \
    -lSDL2 -lSDL2_ttf -lSDL2_image -lm
rc=$?

if [ $rc -eq 0 ]; then
    echo
    echo "== 编译成功 =="
    ls -l "$OUT"
    if [ -n "$DST" ]; then
        install -D -m 755 "$OUT" "$DST" \
            && echo "== 已安装到 $DST ==" \
            || echo "!! 安装到 $DST 失败"
    fi
else
    echo
    echo "== 编译失败 rc=$rc =="
fi
exit $rc
