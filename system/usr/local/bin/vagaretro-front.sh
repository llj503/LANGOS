#!/bin/bash
# ============================================================================
# VagaRetro 原生前端启动器 —— MINILOONG (pocket1, RK3566 / Debian 13 trixie)
#
# 调用方：/usr/bin/emulationstation/emulationstation.sh
#   注入脚本（pc/install-miniloong-front.sh）把它内层循环里的
#       "$esdir/emulationstation" "$@"
#   改成了
#       /usr/local/bin/vagaretro-front.sh "$esdir/emulationstation" "$@"
#
# 跟 R36H/A10 那份（payload/usr/local/bin/vagaretro-front.sh）的区别：
#   ① **只有 sdl 一条路**。那台设备上 native(=weston+GTK3/WebKitGTK)、cog、
#      chromium 三条路在这台机器上**根本没有依赖**（镜像里没有
#      webkit2gtk/cog/chromium/electron），留着只会是死代码 + 误判风险。
#      config.ini 里就算写了别的值也会兜回 sdl。
#   ② 卡二设备名 = /dev/mmcblk3p1（厂商 Switch to SD2 for Roms.sh 里
#      `grep MINILOONG → BLOCK=3`），不是 R36H 的 mmcblk1p1。
#   ③ 日志写到 $APPDIR 而不是 /var/log —— 镜像里 /var/log 是 root:root 755，
#      服务以 ark 跑，写不进去（R36H 那边有 /var/log 可写才那么写的）。
#   ④ **没有 plymouth**（镜像里连 /usr/bin/plymouth 都没有，开机画面就是
#      u-boot 读 /boot/logo.bmp 那一屏）→ plymouth quit 这段只是留个兼容壳，
#      在这台机器上必然跳过，不再是"让出 DRM master"的关键步骤。
#   ⑤ **不做任何屏幕旋转**：这块面板硬件是 720x960 竖屏侧装，但厂商把转屏
#      做在了下层 —— ES 走的 SDL2 被改成用 RGA 旋转并且把宽高对调（应用侧
#      看到 960x720），RA 也自带 RGA 旋转。所以 vgfront 照常按 960x720 画
#      就是正的；vgfront.conf 的 rotate= 留 auto 即可（auto 判出横屏就不转）。
#
# 行为：
#   1) enabled=0           -> 屏幕上给提示后退出（exit 0），systemd 不重启
#   2) sdl 起不来          -> 退出非 0，交给 systemd Restart=on-failure 重试
#   3) 游戏退出后自动回前端（靠 $LAUNCHREQ 传启动请求）
#   4) 前端自己退出        -> 留一句回执后结束
#   5) 前端里按"关机"      -> 先播一遍 /roms/shutdownimages/bye.gif，再 poweroff
#                             （见 shutdown_anim()；重启不播）
#
# ==== 应急回退原版 ES（默认关着）============================================
#   本脚本**从不**主动启动 ES。真要回去：
#       touch /opt/vagaretro/allow-es
#       sudo systemctl restart emulationstation
#   （ES 二进制没被改名，allow-es 一在就能直接 exec 回去，不用恢复文件。）
#
# 用法: vagaretro-front.sh [ES真实二进制路径] [ES参数...]
# ============================================================================

APPDIR="/opt/vagaretro"
CONF="$APPDIR/config.ini"
SHELL_PY="$APPDIR/vagaretro-shell.py"
PAGE="$APPDIR/index.html"
# 启动请求必须落在 ark 自己的目录里（宿主在 sdl 路径下是 root 跑的，
# /tmp 带 sticky 位，ark 删不掉 root 建的文件 → 同一个游戏会被反复启动）。
LAUNCHREQ="$APPDIR/launch.sh"
MARKFILE="$APPDIR/.hook-mark"
ALLOW_ES="$APPDIR/allow-es"

ES_BIN="${1:-/usr/bin/emulationstation/emulationstation}"
[ $# -gt 0 ] && shift

# 日志：$APPDIR 的属主会被注入脚本改成 ark，必定可写。/var/log 在
# MINILOONG 上是 root:root 755，压根不用试。
LOG="$APPDIR/vagaretro-front.log"

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" >> "$LOG" 2>/dev/null
}

# ============================================================================
#  不回退 ES
# ============================================================================
#  直接 exec ES 会很危险：ES 会去抢 /dev/dri/card0，一旦它在我们的前端之外摸过
#  CRTC，后续的原子提交就全废（现象是进程都活着、日志也正常，但屏幕全黑）。
#  R36H 上 2026-09-14 现场踩过一次，所以这里默认锁死，只认 allow-es 标记。
run_es() {
    if [ -f "$ALLOW_ES" ] && [ -x "$ES_BIN" ]; then
        log "allow-es 标记存在且 ES 二进制在场，按旧行为回退 ES: $ES_BIN $*"
        exec "$ES_BIN" "$@"
    fi
    log "已禁用 ES 回退，拒绝执行: $ES_BIN $*"
    return 1
}

fail_notice() {
    log "fail_notice: $*"
    if [ -w /dev/tty1 ]; then
        {
            printf '\033c'
            echo "=================================================="
            echo "  VagaRetro 前端启动失败 (MINILOONG)"
            echo ""
            echo "  原因 : $*"
            echo "  日志 : $LOG"
            echo ""
            echo "  已禁用 ES 回退 —— 不会进入 EmulationStation。"
            echo "  数秒后自动重试；持续失败请 ssh 进来看日志。"
            echo "=================================================="
        } > /dev/tty1 2>/dev/null
    fi
}

conf_get() {
    local key="$1"
    [ -f "$CONF" ] || return 0
    sed '/^[[:space:]]*\[/,$d' "$CONF" \
        | sed -n "s/^[[:space:]]*${key}[[:space:]]*=[[:space:]]*\(.*\)$/\1/p" \
        | tail -n 1 \
        | sed 's/[[:space:]]*$//'
}

# ES 约定的重启/关机标志（前端设置页里"重启/关机"也是靠这两个文件）
#
# ⚠ 这里曾经有两个"静默失效"，合起来就是 2026-09-16 那次"死机"的真凶：
#   ① 前端(vgfront)为了拿 KMSDRM 是**以 root 跑的**，它写出的标志文件属主是
#      root；而本脚本由 emulationstation.service 以 ark 拉起，/tmp 又是
#      sticky bit 目录 —— 裸 `rm -f` 删不掉（Operation not permitted），
#      标志就**永久残留**在 /tmp 里。
#   ② systemd 服务上下文没有 seat 会话，裸调 `systemctl poweroff` 会被 polkit
#      拒掉（Interactive authentication required），关机请求根本没生效。
#   两条一起发作：前端一退出就被残留标志劫持成"要关机"→ 关机又关不掉 →
#   脚本却已经 exit 0 走了 → 游戏不启动、前端不重开 → 全黑屏但 ssh 还活着，
#   用户看到的就是"死机"。而且残留删不掉，**之后每次退出都会重演**。
#   对策：一律走 $SUDO（镜像里 /etc/sudoers.d/ark-no-sudo-password 已给 ark
#   配 NOPASSWD: ALL）；并且关机/重启请求失败时不再硬 exit，宁可退回正常
#   流程，也不能把用户丢在黑屏里。
FLAGFAIL_FILE="/tmp/vgfront-flagfail.count"
# 关机/重启命令失败时的兜底计数。
#   返回 0 → 还允许重开前端；返回 1 → 连续失败太多次，别再重开（防空转）。
#   计数会在一轮前端"稳定跑满 30 秒"后被清掉（见主循环），所以正常使用下它一直是 0。
flag_fail() {
    local what="$1" n
    n=$(cat "$FLAGFAIL_FILE" 2>/dev/null || echo 0)
    n=$(( n + 1 ))
    echo "$n" > "$FLAGFAIL_FILE" 2>/dev/null
    if [ "$n" -le 5 ]; then
        log "警告: $what 请求失败（第 $n 次）→ 重开前端，不把用户丢在黑屏"
        return 0
    fi
    log "警告: $what 请求连续失败 $n 次，不再重开（改走收尾）"
    return 1
}

# 由 handle_es_flags 置 1 = "关机/重启没成功，应该把前端重开"。
# 用显式变量而不是返回值 —— bash 的 0=真 很容易在这个位置读反，
# 读反的后果是"正常启动游戏也被 continue 掉"（游戏永远起不来）。
ES_FLAG_REOPEN=0
handle_es_flags() {
    ES_FLAG_REOPEN=0
    pick_sudo || true
    if [ -f /tmp/es-sysrestart ]; then
        log "es-sysrestart -> reboot"
        $SUDO rm -f /tmp/es-sysrestart 2>/dev/null || rm -f /tmp/es-sysrestart 2>/dev/null
        if $SUDO systemctl reboot; then
            exit 0
        fi
        flag_fail "重启" && ES_FLAG_REOPEN=1
        return
    fi
    if [ -f /tmp/es-shutdown ]; then
        log "es-shutdown -> poweroff"
        $SUDO rm -f /tmp/es-shutdown 2>/dev/null || rm -f /tmp/es-shutdown 2>/dev/null
        shutdown_anim
        if $SUDO systemctl poweroff; then
            exit 0
        fi
        flag_fail "关机" && ES_FLAG_REOPEN=1
        return
    fi
}

# ============================================================================
#  关机动画（用户 2026-09-16）
# ============================================================================
#  位置不是随便挑的：**必须**放在 vgfront 退出之后。vgfront 是 root 跑在
#  KMSDRM 上的，它活着的时候 DRM master 在它手里，这时再起一个 SDL 播放器
#  只会跟它抢屏（现象是一块黑 + 闪）。handle_es_flags 是在 run_sdl 返回
#  之后才执行的 —— 那时 vgfront 已经退出、master 已交还，屏幕正好空着。
#
#  参数（放 config.ini，用 conf_get 取；不写就用下面两个默认）：
#     shutdown_anim  1=开（默认） / 0=关
#     shutdown_gif   默认 $SHUTDOWN_GIF_DEF
#     shutdown_sec   默认 $SHUTDOWN_SEC_DEF 秒。这是**上限不是精确时长**：
#                    到点没播完也硬切 —— 关不掉机比没有动画严重得多。
#  ⚠ 参数**故意不放 vgfront.conf**：那份文件是 sconf_save() 整体重写的
#    （fopen(path,"w") 只写它认识的那几个键），前端设置页一改设置就把
#    我们加的键冲掉了。config.ini 没人重写，是放这类键的地方。
#
#  为什么用 timeout 而不是原厂那套"后台起 + sleep + kill -9"（见
#  /usr/local/bin/finish.sh）：① 播放器要用 $SUDO 以 root 起，ark 这个
#  shell 没有权限 kill root 的进程，`kill -9 $(pidof ffplay)` 会静默失败、
#  动画永远赖在屏幕上挡住关机；② timeout 是"一定会收尾"的。
#
#  ⚠ 素材是 200x175 的 GIF，被 ffplay 按比例撑到 960 高（约 4.1 倍），会有点糊。
#    想清晰就换 960x720 的素材，参考 /roms/launchimages/loading.jpg 的尺寸。
#  ⚠ 故意**不加** -autoexit、也不加 -loop：GIF demuxer 默认 ignore_loop=true，
#    所以行为是"播一遍（2.0s）→ 停在末帧等 timeout"。bye 的最后定格正好
#    当收尾画面，比黑屏好。
#
#  ⚠ 这段**绝不能**让关机流程卡住：调用点把它的返回值当 0 用，且外面还有
#    timeout 兜着。它挂了也只是"没有动画"，不影响 poweroff。
SHUTDOWN_GIF_DEF="/roms/shutdownimages/bye.gif"
SHUTDOWN_SEC_DEF=3

shutdown_anim() {
    local on gif sec

    on="$(conf_get shutdown_anim)"
    [ -n "$on" ] || on=1
    [ "$on" = "1" ] || { log "关机动画: config.ini shutdown_anim=$on，跳过"; return 0; }

    gif="$(conf_get shutdown_gif)"
    [ -n "$gif" ] || gif="$SHUTDOWN_GIF_DEF"

    sec="$(conf_get shutdown_sec)"
    case "$sec" in ''|*[!0-9]*) sec="$SHUTDOWN_SEC_DEF" ;; esac
    [ "$sec" -ge 1 ] || sec="$SHUTDOWN_SEC_DEF"
    [ "$sec" -le 30 ] || sec=30

    if [ ! -f "$gif" ]; then
        log "关机动画: 没有素材 $gif，跳过"
        return 0
    fi

    pick_sudo || true

    if command -v ffplay >/dev/null 2>&1; then
        log "关机动画: ffplay $gif（上限 ${sec}s）"
        # SDL_VIDEODRIVER 钉死 kmsdrm、EGL 驱动跟着 emulationstation.service
        # 那套走 —— 跟 run_sdl 起 vgfront 用的是同一组变量，屏幕路径一致。
        # shellcheck disable=SC2086
        timeout -k 2 "$sec" $SUDO env SDL_VIDEODRIVER=kmsdrm \
            SDL_VIDEO_EGL_DRIVER=libEGL.so \
            ffplay -hide_banner -loglevel quiet -nostats -an \
                   -x 960 -y 720 "$gif" </dev/null >>"$LOG" 2>&1
        log "关机动画: 结束 rc=$?"
    elif command -v image-viewer >/dev/null 2>&1; then
        # 兜底：image-viewer 只出第一帧，而且是**带边框的窗口**不是全屏，
        # 聊胜于无。正常镜像里 ffplay 必在，走不到这一支。
        log "关机动画: 没有 ffplay，退用 image-viewer（静态首帧/窗口模式）"
        # shellcheck disable=SC2086
        timeout -k 2 "$sec" $SUDO env SDL_VIDEODRIVER=kmsdrm \
            image-viewer "$gif" </dev/null >>"$LOG" 2>&1
        log "关机动画: 结束 rc=$?"
    else
        log "关机动画: 一个播放器都没有，跳过"
    fi
    return 0
}

# 服务由 emulationstation.service 以 ark 拉起，KMSDRM 要 drmSetMaster，
# 非 root 得靠 logind 的 seat 授权 —— 服务不是 tty 会话，没有 seat，
# 所以只能 sudo -n 以 root 起。镜像里 /etc/sudoers.d/ark-no-sudo-password
# 已给 ark 配了 NOPASSWD: ALL，直接能用。
SUDO=""
pick_sudo() {
    [ -n "$SUDO" ] && return 0
    if [ "$(id -u)" = "0" ]; then
        SUDO=""
    elif command -v sudo >/dev/null 2>&1 && sudo -n true 2>/dev/null; then
        SUDO="sudo -n"
    else
        log "拿不到免密 sudo，KMSDRM 起不来（非 root 需要 logind seat）"
        return 1
    fi
    return 0
}

# 第二张 TF 卡（游戏卡）。
# LANGOS 自己只把机器内那张卡的 ROMS 分区挂成 /roms，TF2 插上以后没人管，
# 而真正装满 ROM 的通常就是这张卡。这里主动挂到 /roms2；
# 宿主会把 /roms2 当**首选** ROM 根（见 config.ini 的 rom_dir2）。
# 卡不在就直接跳过，不报错、不影响启动。
mount_rom_card2() {
    local dev dir ark_uid ark_gid
    dev="$(conf_get rom2_dev)"; [ -n "$dev" ] || dev=/dev/mmcblk3p1
    dir="$(conf_get rom2_dir)"; [ -n "$dir" ] || dir=/roms2
    [ -b "$dev" ] || { log "卡二: 没有 $dev（没插卡），跳过"; return 0; }

    if mountpoint -q "$dir" 2>/dev/null; then
        log "卡二: $dir 已在挂载状态"
        return 0
    fi

    ark_uid="$(id -u ark 2>/dev/null)"; [ -n "$ark_uid" ] || ark_uid=1000
    ark_gid="$(id -g ark 2>/dev/null)"; [ -n "$ark_gid" ] || ark_gid=1000

    mkdir -p "$dir" 2>/dev/null
    # vfat/exfat 都要靠 uid/gid 才能让 ark 读写；iocharset=utf8 才认得了中文文件名
    if pick_sudo && $SUDO mount -o "rw,noatime,uid=$ark_uid,gid=$ark_gid,umask=0000,iocharset=utf8" \
            "$dev" "$dir" 2>>"$LOG"; then
        log "卡二: $dev -> $dir 已挂载"
    else
        log "卡二: $dev 挂载失败（文件系统不支持或卡有问题）"
    fi
}

# ============================================================================
#  渲染路径：sdl —— 原生 C + SDL2 前端（KMSDRM，不经过 Wayland/weston）
# ============================================================================
#  启动游戏是反方向同一条通道：
#      vgfront 调 `python3 vagaretro-shell.py --launch <pid> <相对路径>`
#        写出 $LAUNCHREQ 后自己退出(0)
#      → 外层 while 看到文件就 bash 它（起 RetroArch / 各种 standalone）
#      → 跑完删掉、continue、再把 vgfront 拉起来
#    所以前端里没有一行启动逻辑：PS1 走 retroarch32、saturn/nds/psp 要先
#    把软链路径还原成 /roms2 真实路径、GBA 的核心要用 FORCE_CORE 压过 ES
#    首选……全部还是宿主那一份在管。
sdl_bin_path() {
    local b; b="$(conf_get sdl_bin)"
    [ -n "$b" ] || b="$APPDIR/native/vgfront"
    printf '%s' "$b"
}

sdl_available() {
    local bin; bin="$(sdl_bin_path)"
    [ -x "$bin" ] || { log "sdl: 没有可执行文件 $bin"; return 1; }
    [ -f "$SHELL_PY" ] || { log "sdl: 缺 $SHELL_PY（生成启动脚本/索引要用它）"; return 1; }
    return 0
}

run_sdl() {
    pick_sudo || return 1

    local bin idx py extra
    bin="$(sdl_bin_path)"
    idx="$(conf_get sdl_index)"; [ -n "$idx" ] || idx="$APPDIR/index.tsv"
    py="$(conf_get shell_python)"; [ -n "$py" ] || py=/usr/bin/python3
    extra="$(conf_get sdl_args)"

    # ---- 索引 + 兼容软链树：每个启动器进程只刷一次 ------------------------
    # 刷一次要扫上万个条目（这台机器 31 平台 / 8400 游戏，约 3 秒），
    # 而 run_sdl 在游戏循环里会被反复调用（每从游戏里退出来一次就调一次），
    # 每次都重扫就等于每次返回菜单都多等几秒。
    # 不用文件时间戳判断"陈旧" —— ROM 卡热插拔后时间戳判不出来。
    if [ -z "${SDL_INDEX_DONE:-}" ]; then
        SDL_INDEX_DONE=1
        if [ -s "$idx" ]; then
            # 已有索引 → **后台**刷新，前端立刻起。
            # 全量扫描要十几秒（8443 个游戏 + 卡二 48G，冷缓存时实测 16 秒），
            # 同步等它等于每次开机白等十几秒 —— 这就是"开机慢"的大头。
            # write_index() 是"先写 .tmp 再 os.replace"的原子替换，前端正在
            # 读这份文件也只会读到旧的完整版、不会读到半份，所以后台刷是安全的。
            # nice -n 19 是让出 CPU（/usr/bin/nice 已带 cap_sys_nice）。
            ( $SUDO nice -n 19 "$py" "$SHELL_PY" "$CONF" --gen-index >> "$LOG" 2>&1 \
              && log "sdl: 后台索引已刷新" \
              || log "sdl: 后台 --gen-index 失败（继续用在用的那份索引）" ) &
            log "sdl: 已有索引，后台刷新（不挡启动）"
        else
            # 真·首次开机：没有索引就没东西可画，只能同步生成。
            log "sdl: 无索引，同步生成（首次开机）"
            if $SUDO "$py" "$SHELL_PY" "$CONF" --gen-index >> "$LOG" 2>&1; then
                log "sdl: 索引已生成"
            else
                log "sdl: --gen-index 失败，沿用现有索引启动界面"
            fi
        fi
    fi

    [ -f "$idx" ] || { log "sdl: 缺索引 $idx，无法启动界面"; return 1; }

    # ---- 让出 DRM master（best-effort）----------------------------------
    # MINILOONG 镜像里没有 plymouth，这段必然跳过 —— 留着是为了跟 R36H 那份
    # 保持同一套骨架，将来厂商加了开机动画也不用再改。
    if [ -z "${SDL_PLY_DONE:-}" ]; then
        SDL_PLY_DONE=1
        if command -v plymouth >/dev/null 2>&1; then
            if $SUDO plymouth quit --retain-splash 2>/dev/null; then
                log "sdl: plymouth 已让出（保留最后一帧）"
            else
                log "sdl: plymouth quit 没成功（可能已退出），继续"
            fi
        fi
    fi

    # 上一轮的启动请求绝不能留：留着会让 vgfront 一起来就被当成"用户按了 A"
    rm -f "$LAUNCHREQ" 2>/dev/null

    log "sdl: 启动 $bin $idx $extra"
    # 退出码代表"界面这条路通不通"：
    #   0   = 正常（用户退出 / 去启动游戏了 —— 两者靠 $LAUNCHREQ 区分）
    #   非0 = 起不来（没 DRM、没字体、索引坏了）→ 返回 1，交给 systemd 重试
    #        或者退出去看日志
    # 显式带上 SDL_VIDEO_EGL_DRIVER：镜像的 emulationstation.service 里设了它，
    # 而 /etc/sudoers.d/ark_preserve_sdl_video_egl_driver 也 env_keep 了它 ——
    # 但这里再显式传一道，免得以后有人动 sudoers 就把 GPU 路径弄丢。
    # 显式钉死 SDL_VIDEODRIVER=kmsdrm：环境里万一有残留会退化成开个窗口。
    # shellcheck disable=SC2086
    $SUDO env SDL_VIDEODRIVER=kmsdrm SDL_VIDEO_EGL_DRIVER=libEGL.so \
        "$bin" "$idx" $extra >> "$LOG" 2>&1
    local rc=$?
    log "sdl: 前端退出 rc=$rc"
    [ "$rc" -eq 0 ] || return 1
    return 0
}

# ============================================================================
#  入口
# ============================================================================
ENABLED="$(conf_get enabled)"
[ -n "$ENABLED" ] || ENABLED=1

if [ "$ENABLED" != "1" ]; then
    log "config.ini enabled=$ENABLED -> 停用前端（不回退 ES）"
    run_es "$@" || true
    if [ -w /dev/tty1 ]; then
        {
            printf '\033c'
            echo "=================================================="
            echo "  VagaRetro 前端已被 config.ini 停用 (MINILOONG)"
            echo ""
            echo "  配置文件 : $CONF"
            echo "  恢复方法 : enabled 改回 1，然后"
            echo "             sudo systemctl restart emulationstation"
            echo "=================================================="
        } > /dev/tty1 2>/dev/null
    fi
    exit 0
fi

PAGE_OVERRIDE="$(conf_get page)"
[ -n "$PAGE_OVERRIDE" ] && PAGE="$PAGE_OVERRIDE"

# 首次运行：在 tty1 给个可见回执，证明钩子生效
if [ ! -f "$MARKFILE" ]; then
    : > "$MARKFILE" 2>/dev/null
    {
        printf '\033c'
        echo "=================================================="
        echo "  VagaRetro 前端  --  启动钩子已生效 (MINILOONG)"
        echo "  日志     : $LOG"
        echo "  配置文件 : $CONF"
        echo ""
        echo "  正在启动原生前端 (sdl / KMSDRM)"
        echo "=================================================="
    } > /dev/tty1 2>/dev/null
    sleep 3
fi

# 渲染器：这台机器只有 sdl 一条路，其它值一律兜回 sdl（并在日志里说明）。
# VG_RENDERER 可临时覆盖，方便调试。
RENDERER="${VG_RENDERER:-$(conf_get renderer)}"
[ -n "$RENDERER" ] || RENDERER=auto
case "$RENDERER" in
    sdl|auto) : ;;
    *)  log "renderer=$RENDERER 在 MINILOONG 上不可用（没有 weston/webkit/cog/chromium），改走 sdl"
        RENDERER=sdl ;;
esac

# index.html 只有"网页系"渲染路径才需要，MINILOONG 上不会用到 ——
# 文件缺失不算错误，这里只是记一笔，别让它拦住启动。
if [ "$RENDERER" != "sdl" ] && [ ! -f "$PAGE" ]; then
    log "renderer=$RENDERER 需要 $PAGE，但文件不在 —— 改走 sdl"
    RENDERER=sdl
fi

# ---- 卡二（TF2 游戏卡）：插上就挂 ----
mount_rom_card2

# ---- 主循环 ----------------------------------------------------------------
START_TS="$(date +%s)"

FRONTEND_RC=1
GAME_LOOPS=0
while :; do
    LOOP_TS=$(date +%s)
    if sdl_available && run_sdl; then
        FRONTEND_RC=0
        # 前端稳定跑满 30 秒 = 运行环境是好的，把"关机失败"计数清零
        if [ $(( $(date +%s) - LOOP_TS )) -ge 30 ]; then
            rm -f "$FLAGFAIL_FILE" 2>/dev/null
        fi
    else
        FRONTEND_RC=1
        log "sdl 路径失败"
        break
    fi

    handle_es_flags
    # 关机/重启没成功 → 重开前端，别把用户丢在黑屏
    if [ "$ES_FLAG_REOPEN" = "1" ]; then
        continue
    fi

    # 前端请求启动游戏 / 执行命令 -> 跑完回到前端
    if [ -x "$LAUNCHREQ" ]; then
        GAME_LOOPS=$(( GAME_LOOPS + 1 ))
        if [ "$GAME_LOOPS" -gt 200 ]; then
            log "游戏循环次数过多($GAME_LOOPS)，停止"
            rm -f "$LAUNCHREQ"
            break
        fi
        log "===== 执行启动请求 [$GAME_LOOPS]: $(tr '\n' ';' < "$LAUNCHREQ" | tail -c 300) ====="
        bash "$LAUNCHREQ" >> "$LOG" 2>&1
        log "===== 启动请求结束 rc=$? ====="
        rm -f "$LAUNCHREQ"
        sleep 1
        continue
    fi
    break
done

ELAPSED=$(( $(date +%s) - START_TS ))
log "frontend loop ended rc=$FRONTEND_RC after ${ELAPSED}s"

# ---- 收尾 ------------------------------------------------------------------
if [ "$FRONTEND_RC" -ne 0 ]; then
    run_es "$@" || true
    fail_notice "sdl 渲染路径失败"
    sleep 15
    exit 1
fi

log "前端正常退出（不回退 ES）"
run_es "$@" || true
if [ -w /dev/tty1 ]; then
    {
        printf '\033c'
        echo "=================================================="
        echo "  VagaRetro 前端已退出 (MINILOONG)"
        echo ""
        echo "  重新启动 : sudo systemctl restart emulationstation"
        echo "  日志     : $LOG"
        echo "=================================================="
    } > /dev/tty1 2>/dev/null
fi
exit 0
