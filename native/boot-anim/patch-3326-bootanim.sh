#!/bin/bash
# ============================================================================
#  patch-3326-bootanim.sh —— 给 R36hProMax（RK3326）的前端启动器挂上开机动画
#
#  为什么要有这个脚本：
#    /usr/local/bin/vagaretro-front.sh 里 run_sdl() 在拉起 vgfront 之前，
#    plymouth 已经 quit（--retain-splash 保留最后一帧），从那一帧到 vgfront
#    画出第一帧之间有一段黑屏。MiniLoong 上是用 langos-bootanim 盖住的，
#    3326 这边照搬 —— 但**必须走 pwrite 那条路**（本机 fb 驱动不支持 mmap，
#    见 langos-bootanim.c 第 5 步注释）。
#
#  做法：在 `log "sdl: 启动 $bin $idx $extra"` 这一行**之前**插一段：
#      · 后台起 langos-bootanim（它自己会在前端起来后退出）
#      · 用 `[ -x ... ]` 守卫，没装就不打扰
#
#  幂等：已经插过（认 marker 注释）就跳过。
#  备份：改前存 /usr/local/bin/vagaretro-front.sh.bak-bootanim
#
#  ⚠ 本机 sed -i 会把文件属主从 ark 变成 root ⇒ 用「sed > tmp && cat tmp > 原文件」
# ============================================================================
set -u

TARGET=/usr/local/bin/vagaretro-front.sh
MARK='# >>> LANGOS-BOOTANIM (3326) >>>'

if [ ! -f "$TARGET" ]; then
    echo "patch-3326-bootanim: 找不到 $TARGET" >&2
    exit 1
fi

if grep -qF "$MARK" "$TARGET"; then
    echo "patch-3326-bootanim: 已经打过补丁，跳过"
    exit 0
fi

# 锚点：run_sdl 里启动 vgfront 前那一行 log
ANCHOR='    log "sdl: 启动 $bin $idx $extra"'
if ! grep -qF "$ANCHOR" "$TARGET"; then
    echo "patch-3326-bootanim: 找不到锚点行，脚本可能改过，人工看一下" >&2
    grep -n 'sdl: 启动' "$TARGET" >&2 || true
    exit 2
fi

cp -p "$TARGET" "$TARGET.bak-bootanim"

# 用 python 做插入（sed 处理多行插入 + 中文容易翻车）
python3 - "$TARGET" <<'PYEOF'
import sys, io

path = sys.argv[1]
anchor = '    log "sdl: 启动 $bin $idx $extra"'

block = '''    # >>> LANGOS-BOOTANIM (3326) >>>
    # 开机动画：plymouth quit 之后、vgfront 第一帧之前有一段黑屏，用
    # langos-bootanim 盖上。它自己会每 2 秒查一次前端起没起，起来就退。
    # 后台起，不阻塞 —— 前端抢到 DRM 时它会写失败自行结束。
    # 本机 fb 不支持 mmap，程序内部自动退回 pwrite（见其源码注释）。
    if [ -x /usr/local/bin/langos-bootanim ] && [ -f /usr/local/share/langos-boot.raw ]; then
        /usr/local/bin/langos-bootanim 30 >> "$LOG" 2>&1 &
    fi
    # <<< LANGOS-BOOTANIM (3326) <<<
'''

src = io.open(path, 'r', encoding='utf-8').read()
if block in src:
    print('already-patched')
    sys.exit(0)

idx = src.find(anchor)
if idx < 0:
    print('anchor-not-found', file=sys.stderr)
    sys.exit(2)

src = src[:idx] + block + src[idx:]
io.open(path, 'w', encoding='utf-8', newline='\n').write(src)
print('inserted at offset', idx)
PYEOF
rc=$?
if [ $rc -ne 0 ]; then
    echo "patch-3326-bootanim: 插入失败 rc=$rc，还原" >&2
    cp -p "$TARGET.bak-bootanim" "$TARGET"
    exit 3
fi

# 语法自检
if ! bash -n "$TARGET"; then
    echo "patch-3326-bootanim: 语法检查不过，还原" >&2
    cp -p "$TARGET.bak-bootanim" "$TARGET"
    exit 4
fi

echo "patch-3326-bootanim: 完成（备份 $TARGET.bak-bootanim）"
grep -n -A 8 'LANGOS-BOOTANIM (3326) >>>' "$TARGET" | head -12
