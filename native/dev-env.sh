#!/bin/bash
# ============================================================================
#  dev-env.sh —— 在设备上准备「编译原生前端」所需的头文件环境
#
#  背景（2026-09-14 实测）：
#    这台 ArkOS 镜像**自带 gcc/g++/make/pkg-config**，但 /usr/include 被精简过：
#    SDL2 和 libc 的开发头文件都没了（dpkg 数据库里却记着 libsdl2-dev 是 installed）。
#    crt1.o / libc.so / Scrt1.o 这些**链接**要用的东西是在的，只缺头文件。
#
#  做法：用 apt-get download 只下载 .deb（不装、不改系统），dpkg -x 解到
#        $DEV 下，编译时用 -isystem 指过去。
#
#  为什么链接符号要单独建 $DEV/lib：
#    解出来的 libc6-dev 里也有 /usr/lib/.../libc.so。如果直接把
#    $DEV/usr/lib/aarch64-linux-gnu 加进 -L，ld 会从那儿找 libc —— 虽然内容
#    一样，但把自己解出来的 libc 混进搜索路径没好处。所以只给 SDL 系列做软链。
#
#  用法：
#      bash dev-env.sh              # 幂等，缺什么补什么
#      source dev-env.sh            # 顺便把 $CFLAGS/$LDFLAGS 导出来
# ============================================================================
set -u

DEV="${VG_SDL_DEV:-/opt/vagaretro/sdl2dev}"
DEBCACHE="${VG_DEB_CACHE:-/opt/vagaretro/sdl2dev-debs}"

# 系统上真实库的位置（aarch64）
SYS_LIB=/lib/aarch64-linux-gnu

log() { printf '[dev-env] %s\n' "$*"; }

have_headers() {
    # 注意 bits/ 不在 inc/ 根下，而在 arch 专属子目录里（踩过一次：
    # 检查写成 $DEV/inc/bits/types.h 就永远"缺头文件"，其实早就解好了）
    [ -f "$DEV/inc/SDL2/SDL.h" ] && [ -f "$DEV/inc/stdio.h" ] \
        && [ -f "$DEV/inc/aarch64-linux-gnu/bits/types.h" ]
}

ensure_debs() {
    mkdir -p "$DEBCACHE"
    local pkgs="libc6-dev linux-libc-dev libsdl2-dev libsdl2-ttf-dev libsdl2-image-dev"
    local missing=""
    for p in $pkgs; do
        # 已经下过就复用（版本号在文件名里，用通配匹配）
        ls "$DEBCACHE"/${p}_*.deb >/dev/null 2>&1 || missing="$missing $p"
    done
    if [ -n "$missing" ]; then
        log "需要下载:$missing"
        ( cd "$DEBCACHE" && for p in $missing; do
            if apt-get download "$p" >/dev/null 2>&1; then
                log "  下载 $p 成功"
            else
                log "  ★下载 $p 失败（没网？镜像源挂了？）"
            fi
        done )
    else
        log "deb 缓存齐全，跳过下载"
    fi
}

extract_all() {
    mkdir -p "$DEV/inc" "$DEV/lib"
    ( cd "$DEBCACHE" || exit 1
      for d in *.deb; do
          [ -f "$d" ] || continue
          # 这里不用 local —— 已经在函数外的子 shell 里（踩过：local 只能用在函数内）
          tmp="$(mktemp -d)"
          dpkg -x "$d" "$tmp" 2>/dev/null || { log "  解包 $d 失败"; rm -rf "$tmp"; continue; }
          # 头文件全部合到 $DEV/inc（libc6-dev 的 arch 专属目录要保留层次）
          [ -d "$tmp/usr/include" ] && cp -a "$tmp/usr/include/." "$DEV/inc/" 2>/dev/null
          rm -rf "$tmp"
      done )
    log "头文件已解到 $DEV/inc（$(find "$DEV/inc" -name '*.h' 2>/dev/null | wc -l) 个 .h）"
}

link_sdl() {
    # 只给 SDL 系列建链接符号。-- 用真实 .so.0 做目标，避免依赖 dev 包里的软链
    ln -sf "$SYS_LIB/libSDL2-2.0.so.0"        "$DEV/lib/libSDL2.so"
    ln -sf "$SYS_LIB/libSDL2_ttf-2.0.so.0"    "$DEV/lib/libSDL2_ttf.so"
    ln -sf "$SYS_LIB/libSDL2_image-2.0.so.0"  "$DEV/lib/libSDL2_image.so"
    for f in libSDL2.so libSDL2_ttf.so libSDL2_image.so; do
        if [ ! -e "$DEV/lib/$f" ]; then
            log "  ★$DEV/lib/$f 指向的库不存在，检查 $SYS_LIB"
        fi
    done
    log "SDL 链接符号已就绪（$DEV/lib）"
}

if ! have_headers; then
    log "头文件不全，开始准备（$DEV）"
    ensure_debs
    extract_all
    link_sdl
    if have_headers; then
        log "环境就绪 ✓"
    else
        log "★仍然缺少头文件，检查上面的下载/解包日志"
    fi
else
    log "头文件环境已就绪（跳过）"
fi

# 供调用方 source 使用
export VG_SDL_DEV="$DEV"
export VG_INC="$DEV/inc"
export VG_LIB="$DEV/lib"

# 顺序有讲究：
#   -isystem 而不是 -I —— 这些是"系统头"，别让 -Wall 把它们的警告也算进来
#   bits/ 在 aarch64-linux-gnu/ 下，必须单独加
VG_CFLAGS="-O2 -isystem $DEV/inc/SDL2 -isystem $DEV/inc -isystem $DEV/inc/aarch64-linux-gnu"
VG_LDFLAGS="-L$DEV/lib"
export VG_CFLAGS VG_LDFLAGS
