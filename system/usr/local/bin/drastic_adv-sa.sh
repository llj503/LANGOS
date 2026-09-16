#!/bin/bash
# ============================================================================
# drastic_adv-sa.sh  —— NDS「高级版 DraStic」启动包装
#
# 2026-09-16 LANGOS 新增（用户要求：NDS 默认左右并排双屏 + 模拟器换高级版）
#
#   跑的是 advanced_drastic（trngaje 版 = DraStic r2.5.2.0 + hook）：
#       /opt/advdrastic/advanced_drastic/drastic
#       + libs/libSDL2-2.0.so.0  ←★ hook 的代码编在这个 SDL2 里，
#                                   **必须用包自带那份**，换成系统 SDL2 就不加载了
#
#   左右并排双屏靠它自己的布局文件：
#       /opt/advdrastic/advanced_drastic/resources/bg/1024x768/layout.json
#     目录名 1024x768 来自 DraStic 自己报的画布尺寸（日志 GFX_Init:1024x768），
#     **不要改成 720x960 / 960x720**，改了它找不到、直接回退到包内默认。
#     index 0 = 左右并排（下屏在左），由 resources/settings.json 的 "mode":0 选中。
#     布局坐标公式（实测）：玩家视角X = 767 - 画布y，玩家视角Y = 画布x；
#     其中 screen_w 控玩家X跨度、screen_h 控玩家Y跨度。
#
#   路径推导沿用原版 drastic.sh 的修复版（readlink -f + mkdir -p + ln -sfn），
#   因为前端传进来的是软链树 /home/ark/.vagaretro-roms/NDS/x.zip，
#   老写法 dirname|cut -d/ -f2 会得到 "home"，导致软链全指到不存在的目录。
#
#   存档与原版 DraStic 完全共享（backup / cheats / savestates / slot2）。
#
#   回滚（换回原版 DraStic）：
#     sed -i 's#drastic_adv-sa\.sh#drastic.sh#' /etc/emulationstation/es_systems.cfg
#     再删掉 /opt/vagaretro/vg_platforms.py 里 NDS 行的 "_adv-sa" 并重生成索引
#
# ----------------------------------------------------------------------------
# ★★ 2026-09-16 修「NDS 游戏打不开」（真实故障，务必别再踩）
#
#   现象：前端点了游戏 → 闪一下弹回前端，rc=0，日志里只有几行
#           /usr/local/bin/drastic_adv-sa.sh: line NN: /tmp/drastic_adv-sa.log: Permission denied
#
#   根因：本脚本的执行身份有两种 ——
#           * 正常：emulationstation.service 以 **ark(1000)** 跑，前端外壳
#                   vagaretro-front.sh 第 490 行 `bash "$LAUNCHREQ"`，也是 ark；
#           * 调试：我用 ml-run-sh.py 上机看，是 **root(0)**。
#         谁先跑谁就在 /tmp 建出 0644 的日志。我 root 调试过一轮之后，
#         前端再以 ark 跑 → EACCES。
#
#   ★ 致命点：bash 对 `cmd >> file` 重定向失败时**不会执行该命令**，只打一行错。
#     原来第 71 行 `./drastic "$ROM" >> "$LOG" 2>&1` 因此被**整条跳过** ——
#     模拟器根本没起来，而脚本 rc 仍是 0，从外面看就是「点了没反应」。
#
#   对策（两道保险，都留在下面代码里）：
#     1) 先探测日志可写性，不可写就换带 uid 的文件名，再不行丢 /dev/null；
#     2) 改用一次性的 `exec > "$LOG" 2>&1`，之后不再出现任何 per-command 重定向，
#        从根本上杜绝"重定向失败把命令吃掉"。
#
#   顺带核实过（req143 实测）：/opt/advdrastic/advanced_drastic 属主是 ark:ark，
#   ark 可写 → `ln -sfn` 建存档软链没问题；/home/ark/.config/KILLIT 也是 ark:ark。
#   **只有 /tmp 那个日志是坑。**
# ============================================================================

ROM="$1"
if [ -z "$ROM" ]; then
  echo "usage: drastic_adv-sa.sh <rom.nds|rom.zip>"
  exit 1
fi

D=/opt/advdrastic/advanced_drastic

# ---- 日志路径：必须容错（见上面 ★ 说明）--------------------------------
LOG=/tmp/drastic_adv-sa.log
if ! ( : >> "$LOG" ) 2>/dev/null; then
  LOG="/tmp/drastic_adv-sa.u$(id -u).log"
  ( : >> "$LOG" ) 2>/dev/null || LOG=/dev/null
fi
# 一次到位：本脚本之后所有 stdout/stderr（含 drastic 自己的）都进日志，
# 再也不写 per-command 重定向。
exec > "$LOG" 2>&1

echo "[drastic_adv-sa.sh] $(date -Iseconds)  uid=$(id -u) log=$LOG"

# ---- 推导 ROM 所在目录（saves / cheats / slot2 都放它旁边）----
REAL="$(readlink -f "$ROM" 2>/dev/null)"
[ -z "$REAL" ] && REAL="$ROM"
ROMDIR="$(cd "$(dirname "$REAL")" 2>/dev/null && pwd)"

case "$ROMDIR" in
  ""|"/"|"/home"|"/home/ark") ROMDIR="/roms2/nds" ;;
esac

echo "[drastic_adv-sa.sh] rom    = $ROM"
echo "[drastic_adv-sa.sh] romdir = $ROMDIR"
echo "[drastic_adv-sa.sh] dir    = $D"

for d in backup cheats savestates slot2; do
  mkdir -p "$ROMDIR/$d"
  if [ -d "$D/$d" ] && [ ! -L "$D/$d" ]; then
    cp -n "$D/$d"/* "$ROMDIR/$d"/ 2>/dev/null
    rm -rf "$D/$d"/
  fi
  ln -sfn "$ROMDIR/$d" "$D/$d"
  echo "[drastic_adv-sa.sh] link   $D/$d -> $ROMDIR/$d"
done

echo "VAR=drastic" > /home/ark/.config/KILLIT
sudo systemctl restart killer_daemon.service 2>/dev/null

cd "$D" || { echo "[drastic_adv-sa.sh] cd $D 失败"; exit 1; }
export LD_LIBRARY_PATH="$D/libs:$LD_LIBRARY_PATH"

echo "[drastic_adv-sa.sh] 开始 ./drastic（等待它退出）"
./drastic "$ROM"
echo "[drastic_adv-sa.sh] ./drastic 退出 rc=$?"

sudo systemctl stop killer_daemon.service 2>/dev/null
sudo systemctl restart ogage 2>/dev/null &

exit 0
