#!/bin/bash
# ============================================================================
# drastic.sh  —— NDS 独立模拟器 DraStic 的启动包装
#
# 2026-09-16 LANGOS 修复
#   原版 md5 283eee538731b9ccea06646d449d57ab (589 B, 2026-09-05)
#
#   原版第 3 行：
#       directory="$(dirname "$1" | cut -d "/" -f2)"
#   这个写法只对「卡名正好是路径第一段」的形态成立，比如
#   /roms2/nds/xx.zip -> roms2。但 LANGOS 前端传进来的是
#   /home/ark/.vagaretro-roms/NDS/xx.zip（compat 软链树），
#   cut -f2 得到的是 "home"，于是：
#       mkdir /home/nds/backup
#           -> 父目录 /home/nds 不存在、又没 -p，直接失败
#       ln -sf /home/nds/backup /opt/drastic/backup
#           -> 把 backup / cheats / savestates / slot2
#              四个软链全改指到不存在的 /home/nds/
#   后果：DraStic 起来跑几秒、一访问存档目录就 Segfault
#   （实测：裸跑 ./drastic 稳定，走本脚本 5 秒崩）。
#
#   修法：
#     1) 用 readlink -f 把 ROM 解析成真实路径（吃掉软链树），
#        再从它的 dirname 反推存档目录；
#     2) mkdir 加 -p；
#     3) 软链改用 ln -sfn（防止钻到已有软链里建子链）；
#     4) 解析不出来时回退 /roms2/nds。
#
#   回滚：
#     cp -a /usr/local/bin/drastic.sh.bak-pre-pathfix-20260916 \
#           /usr/local/bin/drastic.sh
# ============================================================================

ROM="$1"
if [ -z "$ROM" ]; then
  echo "usage: drastic.sh <rom.nds|rom.zip>"
  exit 1
fi

# ---- 推导 ROM 所在目录（saves / cheats / slot2 都放它旁边）----
REAL="$(readlink -f "$ROM" 2>/dev/null)"
[ -z "$REAL" ] && REAL="$ROM"
ROMDIR="$(cd "$(dirname "$REAL")" 2>/dev/null && pwd)"

case "$ROMDIR" in
  ""|"/"|"/home"|"/home/ark") ROMDIR="/roms2/nds" ;;
esac

echo "[drastic.sh] rom    = $ROM"
echo "[drastic.sh] romdir = $ROMDIR"

for d in backup cheats savestates slot2; do
  mkdir -p "$ROMDIR/$d"
  if [ -d "/opt/drastic/$d" ] && [ ! -L "/opt/drastic/$d" ]; then
    cp -n /opt/drastic/${d}/* "$ROMDIR/$d"/ 2>/dev/null
    rm -rf /opt/drastic/${d}/
  fi
  ln -sfn "$ROMDIR/$d" "/opt/drastic/$d"
  echo "[drastic.sh] link   /opt/drastic/$d -> $ROMDIR/$d"
done

echo "VAR=drastic" > /home/ark/.config/KILLIT
sudo systemctl restart killer_daemon.service 2>/dev/null

cd /opt/drastic || exit 1
./drastic "$1"

sudo systemctl stop killer_daemon.service 2>/dev/null

sudo systemctl restart ogage 2>/dev/null &

exit 0
