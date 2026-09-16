#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# ============================================================================
#  vagaretro-shell.py —— VagaRetro 网页前端的 Linux 宿主
#
#  作用：在 LANGOS（无 X11/Wayland 桌面，只有 DRM/KMS）上把 index.html 全屏画出来。
#        依赖 weston 提供 Wayland 合成器（DRM 后端），本程序作为 Wayland 客户端。
#
#  技术要点（决定了实现方式，改动前请先读）：
#    1. 前端是用 **同步** 方式调用 window.AndroidBridge 的（直接取返回值），
#       例如 `romRoot = AndroidBridge.getRomRoot()`。
#       WebKit 的 postMessage 是异步 IPC，没法同步返回 ——
#       所以桥接方法全部用 **纯 JS 实现**，数据在页面加载前由本程序预扫描好、
#       以 JSON 形式内联进注入的脚本里（见 build_bridge_js）。
#    2. 只有"启动游戏 / 执行 shell"这两个是副作用，不需要返回值：
#       它们通过 postMessage 丢回 Python（异步，fire-and-forget），
#       同步返回 `{"ok":true}`；真正的启动动作写进请求文件后由外层 launcher 执行
#       （这样才能先退出本程序和 weston，把屏幕让给 RetroArch）。
#    3. 封面图前端优先直接用 file:// 加载，失败才走 readMediaDataUrl。
#       所以必须打开 allow_file_access_from_file_urls。
#
#  启动游戏用的是 **镜像里 LANGOS 自带的 RetroArch**
#  （/usr/local/bin/retroarch + /etc/emulationstation/es_systems.cfg 的命令模板），
#  不模拟 Android 那套。
# ============================================================================

import configparser
import glob
import json
import os
import re
import shlex
import signal
import struct
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

# 平台元数据表（原生前端索引和网页版共用同一份，见 vg_platforms.py）。
# 本脚本会被启动器用绝对路径调用，cwd 不固定，所以按 __file__ 找同目录。
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vg_platforms  # noqa: E402

HTML_DEFAULT      = "/opt/vagaretro/index.html"
ESSYS_DEFAULT     = "/etc/emulationstation/es_systems.cfg"
ROMSYS_DEFAULT    = "/roms"
# 第二张卡（TF2）上的 ROM 根。真正装满 ROM 的是这张卡，所以优先于 rom_dir。
# 挂载由启动器负责（vagaretro-front.sh 的 mount_rom_card2）；没插卡时这只是个
# 不存在的路径，兼容目录会退化成悬空链接 —— 插上卡重启即生效。
ROM2_DEFAULT      = "/roms2/roms"
COMPAT_DEFAULT    = "/home/ark/.vagaretro-roms"
REQUEST_DEFAULT   = "/opt/vagaretro/launch.sh"
LOGFILE_DEFAULT   = "/var/log/vagaretro-shell.log"
RA_DEFAULT        = "/usr/local/bin/retroarch"
RACORES_DEFAULT   = "/home/ark/.config/retroarch/cores"

# 前端硬编码的 platform.id -> LANGOS es_systems.cfg 里的系统名
# （系统名以真镜像 /etc/emulationstation/es_systems.cfg 为准，别凭直觉写：
#   PSP 那台机器上叫 ppsspp/pspminis 而不是 psp，DOS 叫 msdos 而不是 dos）
PLATFORM_MAP = {
    "FC":    "nes",
    "SFC":   "snes",
    "GB":    "gb",
    "GBC":   "gbc",
    "GBA":   "gba",
    "NDS":   "nds",
    "MD":    "megadrive",
    "SS":    "saturn",
    "DC":    "dreamcast",
    "PS1":   "psx",
    "PSP":   "ppsspp",
    "DOS":   "msdos",
    "PCE":   "pcengine",
    "NGPC":  "ngpc",
    "WS":    "wonderswan",
    "WSC":   "wonderswancolor",
    "GG":    "gamegear",
    "NAOMI": "naomi",
    # ---- 卡二（TF2）上真实存在、前端后来补上的机种 -----------------------
    # 键 = 前端 platform.id（也就是兼容目录名），值 = es_systems.cfg 的系统名。
    "N64":       "n64",
    "GW":        "gameandwatch",
    "MS":        "mastersystem",
    "ARCADE":    "arcade",      # 卡二目录叫 fbneo，ES 里归在 arcade
    "OPENBOR":   "openbor",
    "ATARI2600": "atari2600",
    "ATARI7800": "atari7800",
    "LYNX":      "atarilynx",
    "PICO8":     "pico-8",
    "PORTS":     "ports",
    "EASYRPG":   "easyrpg",
    # J2ME（2026-09-16 新增）：Java 手机游戏。es_systems.cfg 里本来就有 j2me 段
    # （走 RA 的 freej2me 核心），只是从前端这张表里缺了这一行，点进去必死。
    "J2ME":      "j2me",
}
# 从 es_systems.cfg 里读不到时兜底直接拼 retroarch 命令用的 core 名。
# 这些名字都是在真镜像的 /home/ark/.config/<emu>/cores/ 里核对过的。
# 空字符串 = 该平台不是 retroarch core 方式（走 standalone 包装脚本，没法兜底拼）。
PLATFORM_CORE = {
    "FC": "nestopia", "SFC": "snes9x", "GB": "gambatte", "GBC": "gambatte",
    "GBA": "mgba", "NDS": "", "MD": "genesis_plus_gx", "SS": "",
    "DC": "", "PS1": "pcsx_rearmed_neon", "PSP": "", "DOS": "dosbox_pure",
    "PCE": "mednafen_pce_fast", "NGPC": "mednafen_ngp", "WS": "mednafen_wswan",
    "WSC": "mednafen_wswan", "GG": "genesis_plus_gx", "NAOMI": "",
    # 卡二那批（都在 64 位 cores 目录里核对过存在）
    "GW": "gw", "MS": "genesis_plus_gx", "ARCADE": "fbneo",
    "ATARI2600": "stella", "ATARI7800": "prosystem", "LYNX": "handy",
    # 下面这些走的是自家 .sh 包装脚本，%CORE% 只是代号，不校验 .so
    "N64": "", "OPENBOR": "", "PICO8": "", "PORTS": "", "EASYRPG": "",
    # J2ME（2026-09-16）：走 RetroArch 核心，不是 standalone。
    # 正常流程以 es_systems.cfg 里 j2me 段的 <cores> 顺序（freej2me-plus -> freej2me）
    # 为准，这里写 freej2me-plus 只是"ES 表读不到时"的兜底。
    "J2ME": "freej2me-plus",
}
# 哪些平台的 retroarch core 是 32 位那套（cores 目录不同）
PLATFORM_RA32 = {"PS1"}

# ---- 坏核心黑名单：文件在，但**一跑就挂**，必须跳过 ------------------------
# es_systems.cfg 是按"文件名存在"挑 core 的，可有些核心装了却完全不能用。
# 这里按「core 文件名 -> 原因」记下来，pick_core 会整体跳过它们。
#
# 2026-09-14 实测（真机 R36H ProMax，6 个样本 x 3 个核心跑成/败矩阵）：
#   gpsp_ezode_mod    0/6   —— 每个输入都报
#                              [libretro ERROR] [gpSP]: Failed to materialize
#                              ROM from memory. 后 [ERROR] Failed to load content.
#                              明文 .gba 和 zip 一样死，不是"样本坏"也不是
#                              "只吃明文"，是这个核心本体废了。
#                              而它在 gba 的 <cores> 里排**第一位**，
#                              于是玩家点任何一个 GBA 游戏都必然黑屏退回。
#   gpsp（原版）      1/6   —— 明文 .gba 能跑，碰到 .zip 同样 materialize 失败
#                              （原版 gpSP 不支持直接吃压缩包，要先解出来）
#
# 已排除的可能（都实测过，不是这些原因）：
#   ✗ 样本坏      —— 换 6 个样本（5 zip + 1 明文）全挂，原版 gpsp 在同一批上能过
#   ✗ 只吃明文    —— 明文 .gba 一样报 materialize 失败
#   ✗ 架构不对    —— file(1) 确认是 ELF 64-bit aarch64，跟 mgba/gpsp 一致
#   ✗ 缺 GBA BIOS —— ezode 的 .info 写 firmware0_opt="false"（BIOS 是强制的），
#                    而 retroarch.cfg 的 system_directory=/roms/bios 里确实没有
#                    gba_bios.bin（真身在 /roms2/bios/、/home/bios/）。
#                    但**手工把 BIOS 复制进 /roms/bios 后，ezode 报错一字不变**，
#                    所以死因不是 BIOS，就是核心本体坏了。
#                    （mgba 不依赖 GBA BIOS，这也是它 6/6 全过的原因之一。）
#
# 结论：GBA 一律用 mgba（6/6 全过）。它是 retroarch 系里兼容性最稳的那个，
# 对 zip/gba 通吃，性能在 RK3326 上带 GBA 也够（GBA 本身很轻）。
BAD_CORES = {
    "gpsp_ezode_mod": "所有输入都 Failed to materialize ROM from memory",
}

# ---- 平台级 core 强制指定：**无视 es_systems.cfg 的顺序**直接选它 ----------
# 只在"实测证明 ES 里排第一的那个核心不能用"时才写。值为 core 名，
# 且仍然要求 ${core}_libretro.so 真的存在，否则退回正常流程。
#
# ES 原配置里 GBA 首选 gpsp_ezode_mod（坏的），我们钉死用 mgba。
FORCE_CORE = {
    "GBA": "mgba",
}

# ---- 平台级**模拟器**强制指定：无视 es_systems.cfg 里 emulators 的先后 ------
#
# 与 FORCE_CORE 的区别：FORCE_CORE 选的是"同一个 retroarch 里的哪个核心"，
# 这张表选的是"用哪个模拟器程序"，可以跨到 standalone 那边去。
#
# NDS（2026-09-14 用户指定）：一律走**独立版 drastic**，不用任何 RA 核心。
#   理由：drastic 是实机级兼容的 NDS 模拟器（desmume/melonds 在 RK3326 上
#   要么跑不动、要么没触摸/麦克风），而且它的启动脚本 drastic.sh 会自己把
#   savestates/cheats/slot2 软链到 ROM 所在那张卡上。挑 RA 核心只会更差。
#
# es_systems.cfg 里 nds 的第一个 emulator 本来就是 drastic，但那张表是
# 用户可以改的（BaRT 菜单、换主题都会动它），所以这里再钉一道。
# NDS（**2026-09-16 换成高级版 DraStic**）：走 advanced_drastic（trngaje 版）。
#   包装脚本 /usr/local/bin/drastic_adv-sa.sh -> /opt/advdrastic/advanced_drastic/drastic
#   换版本的原因：用户要求 NDS **默认左右并排双屏**。原版 DraStic 的 cfg 只有
#   screen_orientation/screen_scaling/screen_swap，**根本没有"左右并排"这个能力**；
#   advanced_drastic 靠 resources/bg/<WxH>/layout.json 可以任意摆两个屏，
#   实测已做到「下屏在左 / 上屏在右，各 384x288」。
#   存档与原版完全共享（backup/cheats/savestates/slot2 都软链到 /roms2/nds/）。
FORCE_EMULATOR = {
    "NDS": "drastic_adv-sa",
}

# ---- 连 es_systems.cfg 都指望不上时，standalone 的写死命令 -------------------
# direct_command() 平时对 standalone 直接认输（因为那几家靠自家 .sh 解释参数，
# 拼错了会静默起不来）。但 NDS 这一条我们实测过形态，写死反而更稳：
# 即使 es_systems.cfg 丢了 nds 段，点 NDS 游戏也照样起得来。
# （2026-09-16：原来是 choose_drastic_ver.sh，那个脚本在设备上**根本不存在**，
#   是死路径 —— 已改成真实存在的 drastic_adv-sa.sh。）
STANDALONE_DIRECT = {
    "NDS": "nice -n -19 /usr/local/bin/drastic_adv-sa.sh %ROM%",
}

# ============================================================================
#  「更换模拟器核心」的用户覆盖（2026-09-15）
# ============================================================================
#  前端弹层里那一项只做两件事：把候选核心列出来（候选来自索引的 C 行，由
#  core_candidates() 算好），以及往下面这个文件里写一行。
#
#  **启动链一行都不用改** —— build_launch_script() 会读它，并当作比
#  FORCE_CORE 还高的优先级（仍要过 BAD_CORES 和"文件在不在"两道校验，
#  所以就算用户手改了这个文件也选不出跑不起来的核心）。
#
#  格式（TAB 分隔，与 favorites.txt 同款）：
#      <平台id>\t<核心名>                 平台级：该平台所有游戏都用它
#      <平台id>\t<相对路径>\t<核心名>      游戏级：只有这一个游戏用
#  游戏级优先于平台级。想恢复默认就删掉对应那行 —— 前端选「自动」时就是
#  这么做的，删到一条不剩时干脆把文件也删了。
CORES_OVERRIDE_PATH = "/opt/vagaretro/cores.txt"


def _rel_in_compat(rom, cfg):
    """把 <compat_dir>/<pid>/<rel> 里的 <rel> 抠出来。抠不出返回空串。

    游戏级覆盖的键必须是这个 rel —— 和前端 G 行、--launch 第 2 个参数
    三处完全一致，差一个字就会"选了不生效"。
    """
    if not rom:
        return ""
    try:
        r = os.path.relpath(rom, cfg["compat_dir"])
    except Exception:
        return ""
    if r.startswith(".."):
        return ""
    # 分隔符**写死 "/"**、不用 os.sep：这个键必须跟前端 G 行、跟 cores.txt 里
    # 写下的那个 rel 逐字符相等，而那条链上的分隔符永远是 "/"（索引是宿主在
    # Linux 上生成的）。用 os.sep 只会让"本机自测能过、真机对不上"这种
    # 最难查的错出现 —— 本机是 Windows，os.sep 是反斜杠。
    parts = r.replace(os.sep, "/").split("/")
    return "/".join(parts[1:]) if len(parts) > 1 else ""


def load_core_overrides(path=None):
    """读 cores.txt → (平台级 dict, 游戏级 dict)。读不动就两张空表。

    故意不缓存：文件不到 1KB，而"改完下一次启动立刻生效"比省这一次读盘
    重要得多。
    """
    plat, game = {}, {}
    try:
        with open(path or CORES_OVERRIDE_PATH, "r",
                  encoding="utf-8", errors="replace") as fh:
            for line in fh:
                line = line.rstrip("\r\n")
                if not line or line.startswith("#"):
                    continue
                pid, sep, rest = line.partition("\t")
                if not sep or not pid or not rest:
                    continue
                if "\t" in rest:
                    rel, _, core = rest.rpartition("\t")
                    if rel and core:
                        game[(pid, rel)] = core
                else:
                    plat[pid] = rest
    except OSError:
        pass
    return plat, game


def core_override_for(pid, rel):
    """这个平台 / 这个游戏当前被指定的核心。没有覆盖就返回空串。"""
    plat, game = load_core_overrides()
    if rel and (pid, rel) in game:
        return game[(pid, rel)]
    return plat.get(pid, "")


def parse_core_override(s):
    """把 cores.txt 里的值拆成 (emulator, core)。两个都可能为空串。

    三种形态（由 core_candidates() 生成，前端原样写回）：
        "snes9x"              沿用 ES 默认模拟器，只换核心（老的写法，兼容）
        "retroarch:mupen64plus"  显式换成 retroarch 里的 mupen64plus
        "standalone-Rice:"    显式换成 standalone 模拟器，%CORE% 留空
                              （n64.sh / naomi.sh 那批包装脚本靠**参数个数**
                               区分：%CORE% 空时 $2 才是 ROM）
    """
    if not s:
        return "", ""
    if ":" in s:
        e, _, c = s.partition(":")
        return e.strip(), c.strip()
    return "", s.strip()


def _is_so_emu(ename):
    """这个模拟器是"加载 <core>_libretro.so"的那种吗。

    判据用**名字**而不是命令模板 —— n64 / dreamcast / saturn / naomi 的模板
    都是 `<sys>.sh %EMULATOR% %CORE% %ROM%`，里面压根没有 /cores/ 路径，
    从模板上看不出它会不会加载 .so（要读那个 .sh 才知道）。而这些 .sh 的
    约定是统一的：$1 是模拟器名，带 retroarch / retrorun 的才走
    `-L <cores 目录>/$2_libretro.so`，standalone* 走自家二进制。
    """
    e = (ename or "").lower()
    return "retroarch" in e or "retrorun" in e


def core_candidates(cfg, systems, pid):
    """这个平台在弹层里能选的「模拟器 / 核心」，逗号分隔。算不出返回空串。

    ★ 2026-09-16 改成**跨 emulator 收集**（原来是只看第一个 emulator）。
      根因：前端以前只认 es_systems.cfg 里**第一个** emulator 的 <cores>，
        · N64   首个是 retroarch32（只列 parallel_n64），隔壁 retroarch
                那 3 个核心的 .so 明明全在 64 位目录里，界面上却看不见
                → 用户报"n64 核心太少要加核心"
        · 土星  首个是 standalone-nobios（standalone 没有 <cores>）
                → 候选直接为空，用户报"土星也加个独立模拟器吧"
      同一个代码点，一次修两家。

    条目格式见 parse_core_override。仍然**逐个核对 .so 真的在**、且不在
    BAD_CORES 里，所以候选项不会出现"选了起不来"的。刻意不摊"全部已装
    核心的并集"（几百个跟本平台无关的 .so，用户 2026-09-15 明确要保守口径）。
    """
    if not systems:
        return ""
    sysname = resolve_system(PLATFORM_MAP.get(pid, pid), systems) or pid
    meta = systems.get(sysname, {})
    cmd = (meta.get("command") or "").strip()
    if not cmd:
        return ""
    # 命令里既没有 %CORE% 也没有 %EMULATOR% 的系统根本换不了 —— NDS 的
    # `drastic.sh %ROM%` 就是这种，硬列只会让人选了个不生效的东西。
    if "%CORE%" not in cmd and "%EMULATOR%" not in cmd:
        return ""

    cur_emu = (meta.get("emulator") or "").strip()
    # FORCE_EMULATOR 指到谁，谁才是"当前真在跑的"
    femu = FORCE_EMULATOR.get(pid, "") or FORCE_EMULATOR.get((pid or "").upper(), "")
    if femu:
        cur_emu = femu

    ems = list(meta.get("emulators") or [])
    if not ems:
        ems = [{"name": cur_emu, "cores": list(meta.get("cores") or [])}]

    out = []
    for em in ems:
        ename = (em.get("name") or "").strip()
        if not ename:
            continue
        cores = list(em.get("cores") or [])
        if not _is_so_emu(ename):
            # standalone / 包装脚本：%CORE% 不加载 .so。只有当它不是当前默认
            # 模拟器时才给一项 —— 选了它就把 %EMULATOR% 换过去。当前默认那个
            # 不列（列了行为跟「自动」一模一样，反而让人以为选了会变）。
            if ename != cur_emu:
                item = "%s:" % ename
                if item not in out:
                    out.append(item)
            continue
        # retroarch / retrorun 系：按 es_systems 的顺序逐个核对 .so 在不在
        dirs = _cores_dirs(cmd, ename, cfg)
        if not dirs:
            continue
        for c in cores:
            if not c or c in BAD_CORES:
                continue
            if not any(os.path.isfile(os.path.join(d, "%s_libretro.so" % c))
                       for d in dirs):
                continue
            item = c if ename == cur_emu else "%s:%s" % (ename, c)
            if item not in out:
                out.append(item)

    # FORCE_CORE / 平台兜底表里的那个也放进来 —— 那是我们手工核过能用的。
    # force 的优先级最高，所以提到队首（跟 pick_core() 的规则一致，否则
    # 界面上第一项写着 A、真启动却是 B）。
    for c in [FORCE_CORE.get(pid, ""), PLATFORM_CORE.get(pid, "")]:
        if not c or c in BAD_CORES or c in out:
            continue
        dirs = _cores_dirs(cmd, cur_emu, cfg)
        if dirs and any(os.path.isfile(os.path.join(d, "%s_libretro.so" % c))
                        for d in dirs):
            out.append(c)
    fc = FORCE_CORE.get(pid, "")
    if fc and fc in out:
        out.remove(fc)
        out.insert(0, fc)

    # 用户当前选的那个也放进来：万一它不在上面任何来源里（手工改过
    # cores.txt），界面还得能把它显示成"当前"。排在最后 —— 不能让当前选择
    # 去影响列表的固有顺序，否则"换过一次之后列表顺序就变了"，很怪。
    cur = core_override_for(pid, "")
    if cur and cur not in out:
        out.append(cur)
    return ",".join(out)

# 卡二（TF2 那张 116G 游戏卡）上的目录名 -> 前端 platform.id 的差异。
# 那张卡是按「前端 id 风格」命名的（FC/SFC/MD/DC/PS/SS/NDS/PSP…），
# 跟卡一 LANGOS 目录名（nes/snes/megadrive…）不是一回事，所以单独一张表。
# vfat 大小写不敏感，所以 NAOMI->naomi 这种不用写；只有真正不同名的才要列。
# 实测卡二目录（2026-09-14，ls /roms2/roms）：
#   ATARI2600 ATARI7800 DC EASYRPG fbneo FC GB GBA GBC GG GW LYNX MD MS
#   N64 naomi NDS openbor PCE PICO8 ports PS PSP SFC SS WS
ROM2_DIRNAME = {
    "PS1": "PS",        # 卡二叫 PS，前端叫 PS1
    "ARCADE": "fbneo",  # 卡二那 1998 个街机 ROM 放在 fbneo/ 下
    # 卡二的 WS 目录下面又按机型分了两个子库，正好对上前端 WS / WSC 两个平台
    "WS":  "WS/WS",     # 90 个 WonderSwan
    "WSC": "WS/WSC",    # 82 个 WonderSwan Color
}
# 卡二上找目录时的大小写回退见 _resolve_child()：
# NAOMI/OPENBOR/PORTS 这类前端 id 是全大写，而卡二上的目录名是小写，
# 靠回退匹配，不用在下面这张表里一条条写死。
# 卡二上有、但前端 platforms 数组里还没有 id 的平台。
# 2026-09-14 这 11 个已经全补进 index.html 的 platforms 数组和上面的 PLATFORM_MAP 了，
# 这里留空表示"卡二目录已全部覆盖"。以后卡二再加目录，先把名字记回来再补。
ROM2_EXTRA = ()

# 前端扫描 ROM 时用的扩展名。**必须和 index.html 里 renderGames() 的 exts 一致**：
# 宿主预扫描先按这张表过一遍，前端 listDir 再按自己那张过一遍，
# 少一边文件就进不了列表（表现为"平台在但游戏是空的"）。
# 2026-09-14 按卡二实际内容补：7z(2600/fbneo)、n64/z64/v64、
# p8(pico-8)、pak(openbor)、sh(ports)、cso(psp)、cdi(dc)、a26/a78/lnx/sms。
# 特意不收 png：PICO-8 卡带的封面图也在同一层，收进来会混进游戏列表。
ROM_EXTS = ["zip", "7z", "nes", "smc", "sfc", "gba", "gbc", "gb", "md", "iso",
            "bin", "cue", "pbp", "chd", "nds", "pce", "ngp", "ngc", "ws",
            "wsc", "gg", "exe", "com", "bat",
            "a26", "a78", "lnx", "sms", "n64", "z64", "v64", "mgw",
            "cso", "cdi", "img", "p8", "pak", "sh", "easyrpg", "m3u",
            "gdi"]
# mgw = Game & Watch 的单文件 ROM（libretro 的 gw 核心）。
# 2026-09-15 补：卡上 /roms2/gameandwatch 躺着 52 个 .mgw，GW 平台和 gw_libretro
# 核心都齐，唯独这个扩展名不在表里 —— 结果是"平台在、核心在、ROM 在，列表却是空的"。
# 加扩展名时要顺手去卡上核一遍：`ls 目录 | sed 's/.*\.//' | sort | uniq -c`，
# 别只看 G 行数量。
# 2026-09-16 补 gdi：Dreamcast 的 .gdi 是"描述文件 + 若干数据轨"，**必须**认；
# 而它旁边那些数据轨 .bin 不能认（用户报"DC 的图片 BIN 文件也识别成游戏，
# 变成两个游戏"）。收 .gdi 只是把"本来该认的补回来"，真正治 .bin 混入的是
# 下面的 _plat_exts()（按 es_systems 的 <extension> 收窄），不是这张全局表。
# ⚠ 不能简单把 "bin" 从这张表里删掉：NAOMI 的 <extension> 就写着 .bin/.dat，
#   NAOMI ROM 本体就是 .bin，删了那个平台直接空。
#
# 全局表的冻结版。给 _scan_roms() 当默认值用 —— 那边每个文件都要查一次
# 扩展名，列表上是 O(n) 查找，这里冻成 set 才是 O(1)。
ROM_EXTS_SET = frozenset(ROM_EXTS)
# 读进内存的元数据文件名（前端只会 readText 这两个）
META_FILES = ("metadata.pegasus.txt", "gamelist.xml")
# 前端里"虚拟"平台，只用来放历史/收藏，没有实际目录
VIRTUAL_PLATFORMS = ("history", "favorite")

LOG_FH = None


def log(msg):
    global LOG_FH
    line = "[vagaretro-shell] %s" % msg
    sys.stderr.write(line + "\n")
    if LOG_FH is None:
        for path in (LOGFILE_DEFAULT, "/tmp/vagaretro-shell.log"):
            try:
                LOG_FH = open(path, "a", buffering=1)
                break
            except Exception:
                continue
    if LOG_FH:
        try:
            LOG_FH.write(line + "\n")
        except Exception:
            pass


# --------------------------------------------------------------------- 配置
def _normalize_ini(text):
    """把"顶层键 + [section] 节"混合格式规范成 configparser 认的样子。

    configparser 不允许文件开头出现顶层 key=value（会抛
    MissingSectionHeaderError），而 config.ini 里 enabled/renderer 这些
    开关必须在顶层（bash 启动器只读第一个 [ 之前的内容）。
    所以把顶层键收进 [DEFAULT]：这样 python 读得到，
    节内同名键仍然优先生效。
    """
    lines = text.splitlines(True)
    head, body, seen, has_key = [], [], False, False
    for ln in lines:
        s = ln.strip()
        if seen:
            body.append(ln)
            continue
        # 第一个真节头之前的内容全部留在原位（注释也留着，不动顺序）
        if s.startswith("["):
            seen = True
            body.append(ln)
            continue
        head.append(ln)
        if s and not s.startswith(("#", ";")):
            has_key = True
    # 没有顶层键（比如文件直接以注释/节开头）时原样返回，configparser 本来就吃得下
    if not has_key:
        return text
    return "[DEFAULT]\n" + "".join(head) + "".join(body)


def load_config(path):
    cfg = {
        "html": HTML_DEFAULT,
        "essys": ESSYS_DEFAULT,
        "rom_dir": ROMSYS_DEFAULT,
        "rom_dir2": ROM2_DEFAULT,
        "compat_dir": COMPAT_DEFAULT,
        "request_file": REQUEST_DEFAULT,
        "retroarch": RA_DEFAULT,
        "ra_cores": RACORES_DEFAULT,
        "launch_mode": "system",     # system = 用 es_systems.cfg 的 command；direct = 直接拼 RA 命令
        "fullscreen": "1",
        "debug": "0",
        # ---- 手柄（读 evdev 直接调前端 window.vagaPad，不用 gptokeyb）----
        # 注意：下面这些键必须在这里先有默认值，load_config 只回填"已存在"的键，
        #       不在这个字典里的键**永远不会被读到** —— 加新键别忘了同步加这里。
        "pad_enabled": "1",
        "pad_name": "Gamepad",       # 按 sysfs 里的设备名找 event 节点（event 编号会漂，别写死）
        "pad_device": "",            # 想钉死设备路径就填这里，优先于 pad_name
        "pad_analog": "1",           # 左摇杆是否参与方向导航
        "pad_deadzone_pct": "40",    # 摇杆死区（轴量程的百分比）
        "pad_repeat_delay": "420",   # 长按后开始自动重复的延迟(ms)
        "pad_repeat_rate": "160",    # 自动重复间隔(ms)
        "pad_extra_map": "",         # 覆盖额外键语义，如 704:select,705:start
        # 1 = 页面加载后自动跑一次帧率探针（量静止/切页两段的 rAF 间隔），
        # 结果经 console 转发写进日志。只在调性能时打开。
        "perf_probe": "0",
        # 1 = 把网页 console.log / 报错转发进宿主日志（前缀 [web:...]）。
        # 排查前端问题全靠它，开销可忽略（本项目量级每秒不到 10 条）。
        "web_log": "1",
        # 自定义 JS 探测文件：非空且文件存在时，页面加载后把文件内容当 JS 跑一次，
        # 返回值写进日志（前缀 "JS 回读:"）。
        # 用**文件**而不是配置项，是因为 JS 里带 `:` / `=`，塞进 INI 会被
        # configparser 当成键值分隔符切坏。
        "probe_js_file": "",
    }
    if not os.path.exists(path):
        log("没有配置文件 %s，全部用默认值" % path)
        return cfg
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            raw = fh.read()
        cp = configparser.ConfigParser(strict=False)
        cp.read_string(_normalize_ini(raw))
        for k in cfg:
            # 顶层键被收进了 [DEFAULT]，对任何节都可见；节内同名键优先
            if cp.has_option("shell", k):
                v = cp.get("shell", k).strip()
                if v != "":
                    cfg[k] = v
    except Exception as e:
        log("读取配置失败（用默认值）: %s" % e)
    return cfg


# ------------------------------------------------------- es_systems.cfg 解析
def fix_path(p):
    if not p:
        return p
    p = p.strip()
    if not p.endswith("/") and not os.path.splitext(p)[1]:
        p += "/"
    return p


# LANGOS 自带的 es_systems.cfg 里有一条 <command> 写了未转义的裸 & （"2>&1"），
# 严格 XML 解析器（ElementTree/pugixml）会直接抛 ParseError。
# ES 自己能吃下，我们必须跟着容错，否则整个启动功能全废。
_BARE_AMP = re.compile(r"&(?!(?:[a-zA-Z][a-zA-Z0-9]*|#[0-9]+|#[xX][0-9a-fA-F]+);)")


def sanitize_xml(text):
    return _BARE_AMP.sub("&amp;", text)


def _xml_root(path, raw):
    """先按原文解析；不合法再容错重试。返回 (root, 是否用了容错)"""
    try:
        return ET.fromstring(raw), False
    except ET.ParseError as e:
        log("es_systems.cfg 原文不是合法 XML（%s），改用容错解析" % e)
        return ET.fromstring(sanitize_xml(raw)), True


def parse_es_systems(path):
    """返回 {系统名: {path, command, cores:[...], emulator, extensions}}"""
    out = {}
    if not os.path.exists(path):
        log("找不到 es_systems.cfg: %s" % path)
        return out
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            raw = fh.read()
        root, _ = _xml_root(path, raw)
    except Exception as e:
        log("解析 es_systems.cfg 失败: %s" % e)
        return out

    dups = []
    for sysel in root.findall("system"):
        name = (sysel.findtext("name") or "").strip()
        if not name:
            continue
        path_txt = fix_path(sysel.findtext("path") or "")
        cmd = (sysel.findtext("command") or "").strip()
        ext = (sysel.findtext("extension") or "").strip()

        emulator = ""
        cores = []
        cores_all = []
        # ★ 每个 emulator **各自**的 cores，顺序照 es_systems.cfg。
        #   为什么必须单独留一份：N64 的 retroarch32 只列了 parallel_n64，
        #   而隔壁 retroarch 列了 3 个（.so 全在 64 位目录里）。只留"第一个
        #   emulator 的 cores"就等于把那 3 个白扔了 —— 前端弹层里于是只剩
        #   1 个核心可选（2026-09-16 用户报"n64 核心太少"）。
        #   换模拟器时必须成对换（模拟器+核心），所以这张表是候选的来源。
        ems_meta = []
        ems = sysel.find("emulators")
        if ems is not None:
            for em in ems.findall("emulator"):
                ename = (em.get("name") or "").strip()
                if not emulator:
                    emulator = ename
                # ★ 只有**被选中的那个 emulator**（列表里第一个）的 <cores> 才算数。
                # ES 就是这个语义：%CORE% 取"当前选中 emulator 的当前 core"，
                # 选中 standalone 时它压根没有 cores，%CORE% 就是**空字符串**。
                #
                # 以前这里把**所有** emulator 的 cores 合并成一张表，于是
                # dreamcast / saturn / naomi 这三个系统（首个 emulator 是
                # standalone-r7 / standalone-nobios，没有 cores）被塞进了隔壁
                # retroarch32 的核心名。命令模板是
                #   <系统>.sh %EMULATOR% %CORE% %ROM%
                # 而这三个包装脚本都恰好把 **$2 当 ROM 用**：
                #   dreamcast.sh / naomi.sh -> $FLYCAST_BIN "$2"
                #   saturn.sh               -> $YABA_BIN -r 3 -i "$2"
                # 结果 $2 变成核心名，模拟器去找一个叫 "flycast32_rumble" 的
                # "游戏文件"，必然起不来。并集另存 cores_all，只在"命令确实要
                # -L 加载某个 .so"（即 retroarch 系）时当兜底用。
                selected = (ename == emulator)
                coresel = em.find("cores")
                my_cores = []
                if coresel is not None:
                    for c in coresel.findall("core"):
                        cname = (c.text or "").strip()
                        if not cname:
                            continue
                        my_cores.append(cname)
                        if cname not in cores_all:
                            cores_all.append(cname)
                        if not selected or cname in cores:
                            continue
                        # default="true" 的排前面
                        if (c.get("default") or "").lower() in ("true", "1", "yes"):
                            cores.insert(0, cname)
                        else:
                            cores.append(cname)
                ems_meta.append({"name": ename, "cores": my_cores})

        entry = {
            "path": path_txt,
            "command": cmd,
            "emulator": emulator,
            "cores": cores,
            "cores_all": cores_all,
            "emulators": ems_meta,
            "extensions": ext,
        }

        # LANGOS 里 snes 出现了两次（/roms/sufami/ 和 /roms/snes/），
        # 后一条才是正经目录。谁的目标目录真实存在就用谁，都不可用就以后者为准。
        if name in out:
            dups.append(name)
            old_ok = os.path.isdir(out[name]["path"] or "")
            new_ok = os.path.isdir(path_txt or "")
            if new_ok or not old_ok:
                out[name] = entry
            continue
        out[name] = entry

    if dups:
        log("es_systems.cfg 里有重名系统 %s，已按目录是否存在择优" % ",".join(sorted(set(dups))))
    log("es_systems.cfg: 解析到 %d 个系统" % len(out))
    return out


def resolve_system(target, systems):
    """把 platform.id 或目录名解析成 es_systems.cfg 里的系统名（做几层兜底匹配）"""
    if not target:
        return None
    if target in systems:
        return target
    low = target.lower()
    for k in systems:
        if k.lower() == low:
            return k
    # 目录名恰好匹配系统 path 的尾段
    for k, v in systems.items():
        tail = os.path.basename(v["path"].rstrip("/"))
        if tail and tail.lower() == low:
            return k
    # 模糊：系统名里含关键字
    for k in systems:
        if low in k.lower():
            return k
    return None


# ------------------------------------------------------------ 构建兼容 ROM 树
def canon_path(p, rom_dir):
    """把 es_systems.cfg 里的 <path> 落到实际 ROM 根目录下。

    es_systems.cfg 里写的是镜像内的绝对路径（例如 /roms/nes/）。
    如果实际情况不同（换了卡、ROM 根目录被改、或做测试），
    就退回用它的**目录名**拼到 rom_dir 下 —— 比直接信绝对路径稳得多。
    """
    if not p:
        return p
    p = p.strip()
    if os.path.isdir(p):
        return p.rstrip("/")
    tail = os.path.basename(p.rstrip("/"))
    if not tail:
        return p
    cand = os.path.join(rom_dir, tail)
    if os.path.isdir(cand):
        return cand
    # 原名不行就试小写
    cand2 = os.path.join(rom_dir, tail.lower())
    if os.path.isdir(cand2):
        return cand2
    # 哪个都还不存在（比如机器还没首启铺 ROM）—— 优先按"ROM 根目录 + 目录名"
    # 拼出来的位置，而不是配置里的绝对路径，因为前者才是运行时真正会挂上的地方
    return cand


def _resolve_child(root, name):
    """在 root 下按 name 找一个子项，**大小写不敏感地回退一次**。

    为什么必须回退：卡二那块卡 mount 出来是 `type vfat`，但**实际是区分大小写的**
    —— 2026-09-14 实测 /roms2/roms/OPENBOR 直接 ENOENT，openbor 才在。
    而前端 platform.id 为了卡片好看写的是全大写（NAOMI / OPENBOR / PORTS …），
    照字面拼就会全部落到"目录不存在"的悬空链接上，界面显示 0 个游戏。
    （NAOMI 就这么白丢过 43 个 ROM。）
    """
    if not root:
        return name
    p = os.path.join(root, name)
    if os.path.exists(p):
        return p
    try:
        low = name.lower()
        for e in sorted(os.listdir(root)):
            if e.lower() == low:
                return os.path.join(root, e)
    except OSError:
        pass
    return p


def _resolve_rel(root, rel):
    """把 "WS/WSC" 这种多段相对路径逐段用 _resolve_child 解出来"""
    cur = root
    for part in (rel or "").split("/"):
        if not part:
            continue
        cur = _resolve_child(cur, part)
    return cur


def build_compat_tree(systems, rom_dir, compat_dir, rom_dir2="", mode="auto"):
    """在 ext4 上建 <compat_dir>/<platform.id> -> <ROM目录> 的软链。

    为什么要这层：exFAT / vfat 的 ROM 卡都不支持软链，而前端是按
    `<romRoot>/<platform.id>/` 去拼路径的，platform.id（FC/SFC/PS1…）
    跟卡一 LANGOS 的目录名（nes/snes/psx…）对不上。

    源目录按这个优先级挑（第一个存在的赢）：
      1. 卡二 <rom_dir2>/<ROM2_DIRNAME[pid] 或 pid>   ← 真正装满 ROM 的那张卡
      2. 卡一 <es_systems.cfg 里该系统的 path>
      3. 卡一 <rom_dir>/<pid 小写>

    mode（设置页「游戏来源」）能锁死只看哪一张：
      "auto" = 上面这个顺序；"1" = 只走卡一那两条；"2" = 只走卡二那条。
      锁"卡二"时卡二没插 → 退回卡一，免得列表整块空掉。
    都不存在时退化成指向候选 1 的**悬空软链**：LANGOS 首启才铺 ROM、
    卡二也可能还没插，早退的话前端那 20 个平台会整块消失；
    悬空链接等 ROM 一出现就自动生效，不需要重跑。

    注意软链**建在 compat_dir（ext4）上**，目标是 exFAT/vfat 没关系 ——
    受限的是"在 exFAT 上建软链"，不是"软链指向 exFAT"。
    """
    try:
        os.makedirs(compat_dir, exist_ok=True)
    except Exception as e:
        log("创建兼容目录失败 %s: %s" % (compat_dir, e))
        return {}

    resolved = {}
    for pid in list(PLATFORM_MAP.keys()) + list(VIRTUAL_PLATFORMS):
        link = os.path.join(compat_dir, pid)
        if pid in VIRTUAL_PLATFORMS:
            os.makedirs(link, exist_ok=True)
            resolved[pid] = pid
            continue

        sysname = resolve_system(PLATFORM_MAP.get(pid, pid), systems)

        # ---- 街机：把卡二上拆开的几个子库合并成一个平台目录 ----------
        # 见 MULTI_SRC 的注释。只在建实体目录这一步做，后面照常。
        if (pid in MULTI_SRC and rom_dir2 and mode in ("auto", "2")):
            subs = []
            for sub in MULTI_SRC[pid]:
                s = _resolve_child(rom_dir2, sub)
                if os.path.isdir(s):
                    subs.append((sub, s))
            if subs:
                try:
                    if os.path.islink(link):
                        os.unlink(link)
                    elif os.path.exists(link) and not os.path.isdir(link):
                        os.unlink(link)
                    os.makedirs(link, exist_ok=True)
                    for sub, s in subs:
                        dst = os.path.join(link, sub)
                        if os.path.islink(dst):
                            if os.readlink(dst) == s.rstrip("/"):
                                continue
                            os.unlink(dst)
                        elif os.path.exists(dst):
                            continue
                        os.symlink(s.rstrip("/"), dst)
                    resolved[pid] = sysname or pid.lower()
                    log("兼容目录 %-6s 合并 %d 个街机子库：%s"
                        % (pid, len(subs), ", ".join(x[0] for x in subs)))
                    continue
                except Exception as e:
                    log("合并 %s 失败（退回单目录）：%s" % (pid, e))

        # 候选源目录，按优先级排。mode 决定看不看卡二：
        #   auto = 卡二优先（老行为）；1 = 只按卡一；2 = 只按卡二
        # 卡二热插拔换卡后不用改这里 —— 每次 --gen-index 都重读 vgfront.conf。
        #
        # ⚠ 卡二那几条**必须带上 es_systems 里的目录名**（sysdir）。原来只用
        #   ROM2_DIRNAME.get(pid, pid)，而那张表是照着一个不存在的路径猜的：
        #   FC 去找 /roms2/FC（卡二叫 nes）→ 925 个 ROM 白丢；MD/SFC/PS1/GG/SS/DC
        #   同理。es_systems 里的 <path> 用的就是 LANGOS 那套目录名，卡一卡二通用。
        sysdir = _sys_dirname(systems, sysname)
        cands = []
        if rom_dir2 and mode in ("auto", "2"):
            if pid in ROM2_DIRNAME:
                cands.append(_resolve_rel(rom_dir2, ROM2_DIRNAME[pid]))
            if sysdir:
                cands.append(_resolve_child(rom_dir2, sysdir))
            cands.append(_resolve_child(rom_dir2, pid))
            cands.append(_resolve_child(rom_dir2, pid.lower()))
        if mode in ("auto", "1"):
            if sysname:
                p = canon_path(systems[sysname]["path"], rom_dir)
                if p:
                    cands.append(p.rstrip("/"))
            cands.append(os.path.join(rom_dir, pid.lower()))
        if not cands:
            # mode="2" 但卡二没插：退回卡一，别让整个平台列表空掉
            cands.append(os.path.join(rom_dir, pid.lower()))

        # 两轮挑：先挑"存在且有 ROM"的，都没有才退回"存在但空"的。
        # 卡二实测 /roms2/sfc 是空目录、/roms2/snes 才有 ROM —— 只按"存在"选
        # 会选中空的那个，界面就显示 0 个游戏。
        src, hit2 = None, False
        for i, c in enumerate(cands):
            if os.path.isdir(c) and _dir_has_rom(c):
                src, hit2 = c, (i == 0 and bool(rom_dir2))
                break
        if not src:
            for i, c in enumerate(cands):
                if os.path.isdir(c):
                    src, hit2 = c, (i == 0 and bool(rom_dir2))
                    break
        if not src:
            # 一个都不存在：悬空指向首选（卡二），ROM 放进去就自动生效
            src = cands[0]
        sysname = sysname or pid.lower()

        exists = os.path.isdir(src)
        if not exists:
            log("注意 %s：ROM 目录 %s 还不存在，先建悬空链接（放上 ROM 即生效）"
                % (pid, src))

        try:
            if os.path.islink(link):
                # 已有链接且已指向正确目标就不用动
                if os.readlink(link) != src.rstrip("/"):
                    os.unlink(link)
                else:
                    resolved[pid] = sysname
                    continue
            elif os.path.isdir(link):
                # 有人手工建了实体目录，尊重它
                resolved[pid] = sysname
                continue
            elif os.path.exists(link):
                os.unlink(link)
            os.symlink(src.rstrip("/"), link)
            resolved[pid] = sysname
            log("兼容链接 %-6s -> %s   (系统 %s, 源=%s%s)" %
                (pid, src, sysname, "卡二" if hit2 else "卡一",
                 "" if exists else "，暂缺"))
        except Exception as e:
            log("建链接失败 %s -> %s: %s" % (pid, src, e))
    return resolved


# ------------------------------------------------- 媒体根桥接（/$directory/xxx）
# 有 5 个包装脚本用 **ROM 路径的第 2 段**当"媒体根"，再从它下面取 BIOS /
# 模拟器数据：
#     directory=$(dirname "$2" | cut -d "/" -f2)
#     [ -d "/$directory/bios/dc" ] || mkdir -p "/$directory/bios/dc"
#     $FLYCAST_BIN ... ;  ln -sf "/$directory/bios/dc" ~/.local/share/flycast
# 我们的兼容目录是 /home/ark/.vagaretro-roms/<id>/，第 2 段永远是 "home"，
# 于是它们会去找 /home/bios、/home/pico-8、/home/psp… 而真身在别处，
# 而且**分属两张卡**：
#     bios   -> /roms2/bios   （385 项，含 dc_boot.bin / saturn_bios.bin）
#     pico-8 -> /roms/pico-8  （pico8_64 可执行文件在这）
#     psp    -> /roms/psp/ppsspp（用户已有的 PPSSPP 配置）
# 所以随便挑一个"根"都满足不了全部 —— 换成真实路径也救不了，
# 因为 dreamcast 要 roms2、pico8 要 roms，只能选一边。
#
# 做法：在 /home 下按脚本里真实出现的名字建**叠加目录**：
#     真目录 + 里面逐项软链到两边卡上的真文件。
#   读：能命中的真文件（BIOS、pico8_64、PPSSPP 配置）；
#   写：落在 /home（可写），正好绕开"卡二只读"。
# 为什么不建在 /roms 下：卡一卡二是 exFAT/vfat，**根本建不了软链**。
# 两卡都没有内容的（dreamcast/saturn/naomi/atomiswave 这些存档目录）
# 故意不建，留给脚本自己在可写的 /home 下创建。
MEDIA_BRIDGE_DST = "/home"
MEDIA_BRIDGE_ROOTS = ("/roms2", "/roms")
MEDIA_BRIDGE_SRC_DIRS = ("/usr/local/bin", "/opt/OpenBor")
# 脚本里没显式写 /$directory/xxx 但确实要用的，补在这里保底
MEDIA_BRIDGE_EXTRA = ("bios", "pico-8", "psp")


def _media_bridge_names(systems=None):
    """抽出要桥接的名字：只扫**我们真正服务的系统**引用到的 .sh 包装脚本。

    两级收窄，都是为了别在 /home 下乱建目录：
      1) 早先版本把 /usr/local/bin 下所有 .sh 都扫了；
      2) 改成扫 es_systems.cfg 的 command 之后仍然过宽 —— 那里面还有几百个
         跟我们无关的系统（atari800/coco3/dragon32/wolf…）。
    所以就认 PLATFORM_MAP 映射出来的那批系统名。
    """
    names = set(MEDIA_BRIDGE_EXTRA)
    pat = re.compile(r"/\$directory/([A-Za-z0-9._-]+)")
    wanted = set()
    for pid in PLATFORM_MAP:
        sn = resolve_system(PLATFORM_MAP.get(pid, pid), systems) or pid
        wanted.add(sn)

    files = set()
    for sname, meta in (systems or {}).items():
        if wanted and sname not in wanted:
            continue
        for m in re.finditer(r"(/[A-Za-z0-9_./-]+\.sh)\b", meta.get("command") or ""):
            files.add(m.group(1))
    if not files:                       # 一个都没解析到就用目录兜底
        for d in MEDIA_BRIDGE_SRC_DIRS:
            try:
                files.update(os.path.join(d, f) for f in os.listdir(d)
                             if f.endswith(".sh"))
            except OSError:
                pass
    for fp in sorted(files):
        try:
            if os.path.getsize(fp) > 262144:
                continue
            with open(fp, "r", errors="replace") as fh:
                names.update(pat.findall(fh.read()))
        except OSError:
            continue
    for junk in ("", ".", ".."):
        names.discard(junk)
    return sorted(names)


def bridge_media_dirs(systems=None):
    """在 /home 下给 /$directory/<名> 建叠加目录（真目录 + 逐项软链）。"""
    made = []
    for name in _media_bridge_names(systems):
        dst = os.path.join(MEDIA_BRIDGE_DST, name)
        if os.path.islink(dst):
            continue                    # 已经是软链，别动
        srcs = []
        for r in MEDIA_BRIDGE_ROOTS:
            s = os.path.join(r, name)
            try:
                if os.path.isdir(s) and os.listdir(s):
                    srcs.append(s)
            except OSError:
                continue
        if not srcs:
            # 两卡都没有真内容：不建，让脚本自己在可写的 /home 下创建
            continue
        try:
            os.makedirs(dst, 0o755, exist_ok=True)
        except OSError as e:
            log("媒体桥 %s 建目录失败: %s" % (dst, e))
            continue
        n = 0
        for s in srcs:
            try:
                entries = os.listdir(s)
            except OSError:
                continue
            for ent in entries:
                t = os.path.join(dst, ent)
                if os.path.lexists(t):      # 已有就保留，先到的源优先
                    continue
                try:
                    os.symlink(os.path.join(s, ent), t)
                    n += 1
                except OSError:
                    pass
        made.append("%s <- %s (%d 项)" % (dst, " + ".join(srcs), n))
    return made


# --------------------------------------------------------------- 预扫描 ROM
# ROM 要递归扫几层。**这不是可选项**：
# 2026-09-14 实测卡二（TF2）的布局，很多平台的 ROM 是按"分类目录"归了档的，
# 平铺在平台根目录下的只是少数：
#     FC(3763) 在 14 个子目录里（01-动作游戏/02-角色扮演/…）
#     GBA(767) 在 7 个分类目录里，有的还再套一层
#     MD(765) / SFC(702) / WS(172) / GB(66) / fbneo(187) / naomi(34) 同理
# 而 GBC/GG/GW/LYNX/MS/N64/PCE/PS/PSP/SS/ATARI* 是平铺的。
# 所以以前"只读一层"的写法会让 FC/GBA/MD/SFC 显示成 0 个游戏。
ROM_SCAN_DEPTH = 3
# 子目录里这些名字不是 ROM 分类目录，进去纯属浪费时间（而且更深的还可能很大）
ROM_SKIP_DIRS = ("images", "image", "media", "videos", "manuals", "boxart",
                 "snap", "screenshots", "themes", "bios")
# 这些平台的子目录放的是"游戏本体数据"而不是 ROM 分类目录，绝对不能递归进去：
#   PORTS  每个端口一个目录，里面是 .sh/.json/.md/.conf 之类的运行数据；
#          真入口是平台根目录下的那个 .sh（实测就 42 个）
#   EASYRPG 同上的 RPG Maker 游戏目录
NO_SUBDIR_SCAN = ("PORTS", "EASYRPG", "DOS")


# 封面目录的候选名，按这个顺序试，第一个存在的赢。
# 卡二实测（2026-09-15）：nes 用 img/（926 张）、gb 用 image/、gba 用 media/、
# arcade 用 media/images/ —— 原来只认 "images" 一个名字，所以 30 个平台里
# 只有 N64 和 PORTS 碰巧有封面目录，界面上封面全是空的。
# previews 是 PS1 专用的（卡二实测 /roms2/psx/previews/ 装了 26 张），
# 漏了它 PS1 就一个封面目录都找不到，索引里 imgdir 直接是空串。
COVER_SUBDIRS = ("images", "img", "image",
                 "media/images", "media/img", "media/image",
                 "media", "boxart", "boxarts", "covers", "cover", "snap",
                 "previews", "media/previews")

# 一个平台要合并多个源目录的情况。前端只有一个「街机 FBNeo」平台，而卡二把
# 街机 ROM 拆在六个子库里（arcade 832 条 + mame 408 + neogeo 350 + cps1/2/3 122）。
# 这些靠 compat 目录下建**实体目录 + 多条软链**合起来 —— compat 在 ext4 上，
# 建软链没问题；前端是递归扫的（ROM_SCAN_DEPTH=3），自然全收。
# 说明：只对 mode=auto/2（看得到卡二）生效。
MULTI_SRC = {
    "ARCADE": ("arcade", "mame", "neogeo", "cps1", "cps2", "cps3"),
}


def _dir_has_rom(path, depth=2):
    """这个目录（含子目录，最深 depth 层）里有没有 ROM 文件。

    只用于挑候选目录，所以找到第一个就返回，不做全量统计。
    """
    try:
        for name in os.listdir(path):
            if name.startswith("."):
                continue
            full = os.path.join(path, name)
            try:
                isdir = os.path.isdir(full)
            except OSError:
                continue
            if isdir:
                if depth <= 0 or name.lower() in ROM_SKIP_DIRS:
                    continue
                if _dir_has_rom(full, depth - 1):
                    return True
                continue
            if os.path.splitext(name)[1].lstrip(".").lower() in ROM_EXTS:
                return True
    except OSError:
        pass
    return False


def _find_cover_dir(real):
    """在平台真实目录下找封面目录，按 COVER_SUBDIRS 的顺序试，第一个存在的赢。

    找不到就再往**下一层**找一次 —— ARCADE 是合并目录（实体目录 + 各子库软链），
    封面在 arcade/media/images 这种子库里，光看根目录永远找不到。
    """
    for sub in COVER_SUBDIRS:
        cand = os.path.join(real, sub)
        if os.path.isdir(cand):
            return cand
    try:
        for name in sorted(os.listdir(real)):
            sub_real = os.path.join(real, name)
            if not os.path.isdir(sub_real):
                continue
            for sub in COVER_SUBDIRS:
                cand = os.path.join(sub_real, sub)
                if os.path.isdir(cand):
                    return cand
    except OSError:
        pass
    return ""


def _sys_dirname(systems, sysname):
    """从 es_systems.cfg 拿到这个系统在 ROM 卡上用的目录名（FC -> nes）。

    卡一卡二是同一套 LANGOS 目录命名，所以这个名字在两张卡上通用。
    """
    if not sysname or not systems:
        return ""
    st = systems.get(sysname) or {}
    p = (st.get("path") or "").strip()
    if not p:
        return ""
    return os.path.basename(p.rstrip("/"))


def _collect_gamelist_names(base):
    """递归收集 base 下所有 gamelist.xml 的 {相对路径: (显示名, 封面绝对路径)}。

    key 是**相对 base 的路径**，跟 _scan_roms 产出的 name 是同一套写法，
    所以能直接对上（含 arcade 那种 <path>./CapcomOthers/lwings.zip</path>）。

    两个字段一起收：
      · <name>  —— 卡二写的是中文名，比文件名好认；
      · <image> —— **封面路径的金矿**。以前只收 name，封面靠"<封面目录>/<文件名>.png"
        硬猜，于是街机那 825 张（名字带 _screenscraper_mix_arrm 后缀）、PS1 那 26 张
        （在 previews/ 下）一张都命中不了。gamelist 里本来就写着真名，白不用。

    <image> 存**绝对路径**，且只在文件真实存在时才存 —— 前端拿到就能直接开，
    不必再 stat。相对路径是按 gamelist.xml **自己所在目录**算的（嵌套的 gamelist
    写的是同级相对路径，比如 fds/重装机兵/ 下写 "previews/xxx.png"），
    所以基准是 root 而不是 base。
    """
    out = {}
    try:
        import xml.etree.ElementTree as ET
    except Exception:
        return out
    base = base.rstrip("/")
    # ⚠ followlinks=True 是必须的：ARCADE 那种"合并目录"是**实体目录 + 各子库软链**，
    #   而 os.walk 默认**不跟随符号链接目录** —— 不打开这个开关就进不去
    #   compat/ARCADE/arcade/，那 mame/neogeo/cps* 的 gamelist.xml 一个都读不到，
    #   街机那 1600 多个游戏就全是文件名当显示名（实测踩过）。
    for root, dirs, files in os.walk(base, followlinks=True):
        depth = root[len(base):].count(os.sep)
        # 剪枝：ROM_SKIP_DIRS 里全是封面/视频这类大目录，进去纯浪费时间
        dirs[:] = [x for x in dirs
                   if x.lower() not in ROM_SKIP_DIRS and not x.startswith(".")]
        if depth >= 2:
            dirs[:] = []
        if "gamelist.xml" not in files:
            continue
        fp = os.path.join(root, "gamelist.xml")
        try:
            tree = ET.parse(fp)
        except Exception as e:
            log("解析 %s 失败: %s" % (fp, e))
            continue
        rel_root = os.path.relpath(root, base)
        rel_root = "" if rel_root == "." else rel_root.replace(os.sep, "/")
        for g in tree.getroot().findall("game"):
            p = (g.findtext("path") or "").strip()
            if not p:
                continue
            nm = (g.findtext("name") or "").strip()
            im = (g.findtext("image") or "").strip()
            if not nm and not im:
                continue
            if p.startswith("./"):
                p = p[2:]
            p = p.lstrip("/")
            key = ("%s/%s" % (rel_root, p)) if rel_root else p

            img_abs = ""
            if im:
                if im.startswith("./"):
                    im = im[2:]
                cand = os.path.normpath(os.path.join(root, im.lstrip("/")))
                if os.path.isfile(cand):
                    img_abs = cand
            out[key] = (nm, img_abs)
    return out


def _plat_exts(systems, pid):
    """这个平台**按 es_systems.cfg 允许**的扩展名集合（小写、不带点）。

    为什么需要它：ROM_EXTS 是一张全局表，导致"DC 的 .bin 数据轨被当成独立
    游戏""土星的模拟器备份文件 backup.bin 混进列表"。而 es_systems.cfg 里每个
    系统自己的 <extension> 才是 ES 认 ROM 的真源 —— dreamcast 那份有 .gdi
    没有 .bin，naomi 那份有 .bin（NAOMI ROM 就是 .bin/.dat）。按它收才准。

    返回 None 表示"查不到"→ 调用方回退全局 ROM_EXTS。查不到的典型是
    虚拟平台（history / favorite）和卡二上有目录但 es_systems 里没这一号的。
    """
    if not systems:
        return None
    sysname = resolve_system(PLATFORM_MAP.get(pid, pid), systems)
    if not sysname:
        sysname = resolve_system(pid, systems)
    if not sysname:
        return None
    raw = (systems.get(sysname, {}).get("extensions") or "").strip()
    if not raw:
        return None
    out = set()
    for tok in raw.replace(",", " ").split():
        t = tok.strip().lower().lstrip(".")
        if t:
            out.add(t)
    return out or None


def _scan_roms(base, rel, depth, pid, out, exts=None):
    """把 base/<rel> 下的 ROM 收进 out。

    out 里的每一项都是
        {"name": "<相对 base 的路径，用 / 分隔>", "dir": False,
         "title": "<显示用文件名>"}
    前端拿 name 拼真实路径（romRoot/platform.id/name），拿 title 显示，
    所以嵌套的 ROM 在界面上不会带一串目录前缀。

    exts —— 本平台允许的扩展名集合（见 _plat_exts）。None = 用全局表兜底。
    """
    allow = exts if exts is not None else ROM_EXTS_SET
    cur = os.path.join(base, rel) if rel else base
    try:
        names = sorted(os.listdir(cur))
    except OSError as e:
        log("跳过 %s: %s" % (cur, e))
        return
    for name in names:
        if name.startswith("."):
            continue
        full = os.path.join(cur, name)
        child_rel = ("%s/%s" % (rel, name)) if rel else name
        try:
            isdir = os.path.isdir(full)
        except OSError:
            continue
        if isdir:
            if depth >= ROM_SCAN_DEPTH:
                continue
            if pid in NO_SUBDIR_SCAN:
                continue
            if name.lower() in ROM_SKIP_DIRS:
                continue
            _scan_roms(base, child_rel, depth + 1, pid, out, exts)
            continue
        ext = os.path.splitext(name)[1].lstrip(".").lower()
        if ext in allow:
            # 默认显示名 = 文件名。但 .sh 是 **ports 的启动脚本**，不是 ROM ——
            # 显示名里带着它，ports 列表上就会出现"吃豆人.sh""蔚蓝复古版.sh"
            # 这种一眼像文件名、不像游戏名的条目（2026-09-15 实测截图里就是）。
            # 只有 sh 去掉；其它扩展名（.nes/.zip…）在缺少 gamelist 名字时
            # 反而是有用的信息，保持原样。
            title = os.path.splitext(name)[0] if ext == "sh" else name
            out.append({"name": child_rel, "dir": False, "title": title,
                        "img": ""})


def prescan(compat_dir, resolved, systems=None):
    """扫描兼容目录，产出注入 JS 用的数据。

    ROM 是**递归**扫的，最深 ROM_SCAN_DEPTH 层，原因见下（卡二的实际布局）。

    同时把 metadata.pegasus.txt / gamelist.xml 的内容读进来 ——
    前端只会同步 readText 这两个文件。

    systems ＝ parse_es_systems() 的结果，用来给每个平台取它自己的
    <extension> 白名单（见 _plat_exts）。传 None 就退回全局 ROM_EXTS。
    """
    dirs = {}
    texts = {}
    total = 0
    n_plat_ext = 0

    for pid in resolved:
        d = os.path.join(compat_dir, pid)
        if not os.path.isdir(d):
            # 悬空链接（ROM 还没放进来）也记一条空列表，
            # 保证前端拿得到这个平台的键，界面上不会整个平台消失
            dirs[d] = []
            continue
        entries = []
        try:
            exts = _plat_exts(systems, pid)
            if exts is not None:
                n_plat_ext += 1
            _scan_roms(d, "", 0, pid, entries, exts)
            # 显示名以 gamelist.xml 的 <name> 为准（卡二那边写的是中文名，
            # 如 "Y 翼人[YR]"，比文件名好认）。gamelist 里没列的 ROM 保持文件名。
            gm = _collect_gamelist_names(d)
            if gm:
                for e in entries:
                    got = gm.get(e["name"])
                    if not got:
                        continue
                    nm, im = got
                    if nm:
                        e["title"] = nm      # 中文显示名
                    if im:
                        e["img"] = im        # 封面绝对路径（已验证存在）
            dirs[d] = entries
            total += len(entries)
        except Exception as e:
            log("扫描 %s 失败: %s" % (d, e))
            dirs[d] = []

        for mf in META_FILES:
            fp = os.path.join(d, mf)
            if os.path.isfile(fp):
                try:
                    with open(fp, "r", encoding="utf-8", errors="replace") as fh:
                        texts[fp] = fh.read()
                except Exception as e:
                    log("读 %s 失败: %s" % (fp, e))

    log("预扫描完成：%d 个目录 / %d 个条目 / %d 个元数据文件 / %d 个平台按 es_systems 白名单收（其余用全局表）"
        % (len(dirs), total, len(texts), n_plat_ext))
    return dirs, texts


# ---------------------------------------------------------------- 索引文件
# 原生前端（C + SDL2）不做目录遍历 —— 它读这份扁平索引。
# 好处是 C 侧只要逐行 strtok 就行，零 JSON、零递归、零分配策略。
#
# 格式（UTF-8 / LF，字段用 TAB 分隔，行首是记录类型）：
#
#     V   2
#     M   <生成时间戳> <ROM根> <平台数> <游戏总数>
#     P   <pid> <cat> <name> <sub> <core> <logo> <tag> <渐变起> <渐变止> <封面目录>
#     G   <显示名> <相对路径> <封面绝对路径>
#
# - **P 行后紧跟该平台的若干 G 行**，C 侧顺序读即可，遇到新 P 就开新组。
#   游戏真实路径 = <ROM根>/<pid>/<相对路径>。
# - 颜色直接写 #RRGGBB（来自 vg_platforms.py），C 侧不用查表、
#   也不用解析 CSS class —— 原生端根本没有 CSS。
# - 平台顺序 = vg_platforms.PLATFORM_META 的顺序，也就是界面上的顺序。
# - 虚拟平台（history / favorite）也在 P 行里，但**没有 G 行**，
#   它们的条目由前端运行时另算。
#
# v2 在**末尾追加了一个字段：封面目录**（绝对路径，指向 <平台真实目录>/images）。
# 是 P 行的**第 11 列**（C 侧 fields[10]）。
# 为什么必须由 Python 给出、C 侧别自己拼：卡二上的平台目录名只是"多数"和
# 前端 platform.id 同名（FC/MD 这种），宿主里有一张 ROM2_DIRNAME 特例表，
# 而且 <compat_dir>/<pid> 本身是条软链 —— 真实目录在哪儿只有 Python 知道。
# 该目录不存在时写空串，C 侧就退回"无封面"的纯文字卡。
INDEX_DEFAULT = "/opt/vagaretro/index.tsv"
# v4 = P 行后面多一条 C 行（每平台能换哪些核心）。纯粹是给"更换模拟器核心"
# 用的；旧的 V 行号没有任何地方校验过，改成 4 只是留个追溯的记号。
INDEX_VERSION = "4"


def _tsv_clean(value):
    """字段里不能出现 TAB / 换行，否则整行的列就错位了。

    ROM 文件名里带 TAB 虽然罕见但不是不可能；换成空格在展示上毫无损失。
    """
    return str(value).replace("\t", " ").replace("\r", " ").replace("\n", " ")


def write_index(cfg, dirs, systems=None, out_path=None):
    """把 prescan 的结果落成索引文件。返回 (平台数, 游戏数)。

    先写 .tmp 再 os.replace 原子替换 —— 原生前端可能正在读这份文件，
    不能让它读到写了一半的内容。

    systems ＝ parse_es_systems() 的结果，只用来算每个平台**能换的模拟器
    核心**（C 行）。传 None 就跳过 C 行，索引照样能用 —— 旧前端本来就
    不认这一行。
    """
    out_path = out_path or cfg.get("index_file", INDEX_DEFAULT)

    # prescan 的键是 "兼容目录/pid"，翻成 pid -> 条目
    games = {}
    for d, entries in dirs.items():
        games[os.path.basename(d.rstrip("/"))] = entries

    meta = vg_platforms.by_pid()
    lines = []
    n_plat = n_game = n_img = n_gal = n_core = 0

    for pid, md in meta.items():
        # 封面目录：<平台真实目录>/images。realpath 是为了穿过 compat_dir 那层软链，
        # 拿到卡二上的真实位置（目录名不一定等于 pid）。目录不在就写空串。
        imgdir = ""
        d = os.path.join(cfg["compat_dir"], pid)
        if os.path.isdir(d):
            try:
                real = os.path.realpath(d)
                cand = _find_cover_dir(real)
                if cand:
                    imgdir = cand
                    n_img += 1
            except Exception as e:
                log("realpath(%s) 失败，该平台无封面目录: %s" % (d, e))

        lines.append("P\t" + "\t".join(_tsv_clean(x) for x in (
            pid, md["cat"], md["name"], md["sub"], md["core"],
            md["logo"], md["tag"], md["color"][0], md["color"][1],
            imgdir)))
        n_plat += 1

        # C 行（索引 v4）：这个平台**能换的核心**，逗号分隔。紧跟在它自己的
        # P 行后面 —— 前端解析时只要记住"当前平台"就够。
        # 索引解析器的 switch 没有 default 分支，所以旧前端读到这一行是
        # **静默忽略**；反过来新前端读到没有 C 行的旧索引，那一项就退化成
        # "只能保持自动"。两头都安全。
        if systems:
            cand = core_candidates(cfg, systems, pid)
            lines.append("C\t%s\t%s" % (_tsv_clean(pid), _tsv_clean(cand)))
            if cand:
                n_core += 1
        for e in games.get(pid, ()):
            # 第 4 列 = gamelist.xml 的 <image> 绝对路径（没有就空串）。
            # 这是封面的**首选来源**，前端拿不到才退回按文件名猜。
            lines.append("G\t%s\t%s\t%s" % (_tsv_clean(e["title"]),
                                              _tsv_clean(e["name"]),
                                              _tsv_clean(e.get("img", ""))))
            if e.get("img"):
                n_gal += 1
            n_game += 1

    # rom_root 只进 M 行（前端拿它显示/排错用），得跟 build_compat_tree
    # 实际扫的那张卡一致 —— 否则设置页切到卡一了，信息里还写着 /roms2/roms。
    if rom_source_mode() == "1":
        rom_root = cfg["rom_dir"]
    else:
        rom_root = cfg.get("rom_dir2") or cfg["rom_dir"]
    tmp = out_path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("V\t%s\n" % INDEX_VERSION)
        fh.write("M\t%d\t%s\t%d\t%d\n"
                 % (int(time.time()), rom_root, n_plat, n_game))
        fh.write("\n".join(lines))
        fh.write("\n")
    os.replace(tmp, out_path)

    log("索引已写出：%s（%d 平台 / %d 游戏 / %d 带 gamelist 封面 / "
        "%d 个平台带封面目录 / %d 个平台可换核心 / %.0f KB）"
        % (out_path, n_plat, n_game, n_gal, n_img, n_core,
           os.path.getsize(out_path) / 1024.0))
    return n_plat, n_game


# ---------------------------------------------------------------- 桥接 JS
BRIDGE_JS_TMPL = r"""
(function () {
  if (window.AndroidBridge) { return; }

  var ROM_ROOT = %(rom_root)s;
  var DIRS     = %(dirs)s;
  var TEXTS    = %(texts)s;
  var EXT_SET  = %(exts)s;
  var SETTINGS = %(settings)s;

  function post(msg) {
    try {
      if (window.webkit && window.webkit.messageHandlers &&
          window.webkit.messageHandlers.vagaretro) {
        window.webkit.messageHandlers.vagaretro.postMessage(msg);
      }
    } catch (e) { console.error('vagaretro postMessage failed', e); }
  }

  window.AndroidBridge = {
    getRomRoot: function () { return ROM_ROOT; },

    readText: function (p) {
      if (!p) { return ''; }
      var v = TEXTS[p];
      return (typeof v === 'string') ? v : '';
    },

    listDir: function (dir, extsCsv) {
      var arr = DIRS[dir];
      if (!arr) { return '[]'; }
      var allow = null;
      if (extsCsv) {
        allow = {};
        String(extsCsv).split(',').forEach(function (e) {
          e = e.trim().toLowerCase();
          if (e) { allow[e] = 1; }
        });
      }
      var out = [];
      for (var i = 0; i < arr.length; i++) {
        var it = arr[i];
        if (it.dir) { continue; }
        var dot = it.name.lastIndexOf('.');
        var ext = dot >= 0 ? it.name.slice(dot + 1).toLowerCase() : '';
        if (allow && !allow[ext]) { continue; }
        /* name = 相对平台目录的路径（嵌套 ROM 会是 "分类目录/文件.zip"），
           前端拿它拼真实路径；
           title = 只给界面显示的文件名，省得嵌套 ROM 带一串目录前缀。 */
        out.push({ name: it.name, title: it.title || it.name,
                   path: it.name, isDir: false, ext: ext });
      }
      return JSON.stringify(out);
    },

    /* 封面图前端优先直接 file:// 加载，这里只是兜底：返回空串，
       前端会保留占位图，不会报错 */
    readMediaDataUrl: function (p) {
      if (!p || !TEXTS['__media__' + p]) { return ''; }
      return TEXTS['__media__' + p];
    },

    /* 有副作用、不需要返回值的两个：丢给 Python 处理，同步返回 ok */
    executeShellCommand: function (cmd) {
      if (!cmd) { return JSON.stringify({ ok: false, message: 'empty command' }); }
      post({ type: 'shell', cmd: String(cmd) });
      return JSON.stringify({ ok: true });
    },

    launchRetroArch: function (romPath, coresJson, platformName) {
      if (!romPath) { return JSON.stringify({ ok: false, message: 'empty rom path' }); }
      post({ type: 'retroarch', rom: String(romPath),
             cores: String(coresJson || ''), platform: String(platformName || '') });
      return JSON.stringify({ ok: true });
    },

    /* ---- 设置页 ----
       读：SETTINGS 是宿主启动时从真实文件/节点扫出来的（前端同步调用，
           必须现成，不能等 onclick 再去问 Python）。
       写：异步丢给 Python 落盘（es_settings.cfg / sysfs / amixer），
           同时同步一份本地缓存，同一次会话里翻回这页看到的是新值。

       flush 参数的用意：音量/亮度这类连续调节，每按一下都写一次磁盘太吵，
       前端会先只改内存（flush=false），离开这一行或退出页面时再
       flushSetting() 真正落盘。 */
    getSettings: function () {
      return JSON.stringify(SETTINGS || {});
    },

    setSetting: function (key, value, flush) {
      key = String(key);
      var v = (value === null || value === undefined) ? '' : String(value);
      if (SETTINGS && SETTINGS[key] !== undefined) { SETTINGS[key] = v; }
      if (flush === false) { return JSON.stringify({ ok: true, deferred: true }); }
      post({ type: 'setting', key: key, value: v });
      return JSON.stringify({ ok: true });
    },

    /* 把 deferred 期间攒下的值真正落盘 */
    flushSetting: function (key) {
      key = String(key);
      if (!SETTINGS || SETTINGS[key] === undefined) {
        return JSON.stringify({ ok: false, message: 'unknown key' });
      }
      post({ type: 'setting', key: key, value: String(SETTINGS[key]) });
      return JSON.stringify({ ok: true });
    }
  };

  console.log('vagaretro: AndroidBridge injected, romRoot=', ROM_ROOT,
              'dirs=', Object.keys(DIRS).length);
})();
"""


# ------------------------------------------------------------------ 诊断钩子
# 网页里的 console.log / 报错全部转发回宿主日志。
# 为什么要这个：封面加载是前端自己的 onerror 分支在管（file:// 先试，
# 失败才走兜底），不转发 console 就只能靠猜；JS 异常同理。
#
# 复用桥接那条 `vagaretro` 通道（GI 里带连字符的信号名映射不出方法名，
# 单独注册 `vagaretro-log` 会 AttributeError）。消息体是 {"type":"log",...}，
# 与已有的 shell/retroarch 消息同构，宿主侧在 on_bridge_msg 里分流。
CONSOLE_HOOK_JS = r"""
(function () {
  if (window.__vgLogHook) { return; }
  window.__vgLogHook = 1;
  function send(level, args) {
    var H = window.webkit && window.webkit.messageHandlers &&
            window.webkit.messageHandlers.vagaretro;
    if (!H) { return; }
    try {
      var parts = [];
      for (var i = 0; i < args.length; i++) {
        var a = args[i];
        try { parts.push(typeof a === 'string' ? a : JSON.stringify(a)); }
        catch (e) { parts.push(String(a)); }
      }
      H.postMessage(JSON.stringify({
        type: 'log', level: level, msg: parts.join(' ')
      }));
    } catch (e) {}
  }
  ['log', 'info', 'warn', 'error'].forEach(function (lv) {
    var orig = console[lv];
    console[lv] = function () {
      send(lv.toUpperCase(), arguments);
      if (orig) { try { orig.apply(console, arguments); } catch (e) {} }
    };
  });
  window.addEventListener('error', function (ev) {
    send('JS-ERROR', [String(ev.message) + ' @ ' +
                      String(ev.filename || '') + ':' + String(ev.lineno || 0)]);
  });
  window.addEventListener('unhandledrejection', function (ev) {
    send('JS-REJECT', [String(ev.reason)]);
  });
})();
"""


# 帧率探针：量两段 rAF 间隔 —— 阶段1 静止（首页），阶段2 切一次页之后。
# 软件合成下"卡不卡"必须变成数字，否则只能凭手感扯。
PERF_JS = r"""
(function () {
  if (window.__vgPerf) { return 'busy'; }
  window.__vgPerf = 1;
  function say(s) {
    var H = window.webkit && window.webkit.messageHandlers &&
            window.webkit.messageHandlers.vagaretro;
    try { H.postMessage(JSON.stringify({type: 'log', level: 'PERF', msg: s})); }
    catch (e) {}
  }
  var N = 150, t = [], phase = 0;

  function tick(ts) {
    t.push(ts);
    if (t.length < N) { requestAnimationFrame(tick); return; }
    var n = t.length, span = t[n - 1] - t[0], worst = 0, over = 0, sum = 0;
    for (var i = 1; i < n; i++) {
      var d = t[i] - t[i - 1];
      sum += d;
      if (d > worst) { worst = d; }
      if (d > 33.4) { over++; }
    }
    say('阶段' + phase + ' 帧数=' + n + ' 时长=' + span.toFixed(0) + 'ms' +
        ' 平均FPS=' + ((n - 1) * 1000 / span).toFixed(1) +
        ' 平均帧=' + (sum / (n - 1)).toFixed(1) + 'ms' +
        ' 最差帧=' + worst.toFixed(1) + 'ms' +
        ' 掉帧(>33ms)=' + over);
    if (phase === 1) {
      try { showPage('settings'); } catch (e) {}
      setTimeout(function () {
        try { showPage('games'); } catch (e) {}
        start(2);
      }, 1500);
    } else {
      window.__vgPerf = 0;
    }
  }

  function start(p) {
    t = [];
    phase = p;
    requestAnimationFrame(tick);
  }

  start(1);
  return 'started';
})()
"""


# ------------------------------------------------------------------- 设置项
#
# 目标：前端那个"设置"页要**复刻 LANGOS 原生 ES 的设置**，而不是自己发明一套。
# 所以这里做两件事：
#   1. read_settings()  —— 启动时把真实的当前值读出来喂给前端（前端是同步
#                          调 bridge 的，必须预扫、不能等 onclick 再取）；
#   2. apply_setting()  —— 前端改某一项时，落到**LANGOS 自己认的那个文件/节点**上。
#
# 关键原则：**不自己发明存储**。每一项都写到这台机器原生 ES 本来就会读/写的地方，
# 这样用户在网页里改了设置，退出前端回到 ES 也照样生效；两款前端不会各说各话。
#
# 各项的真实落脚点（2026-09-14 在真机 R36H ProMax 上逐条查过）：
#   音量      amixer sset Playback / 存 /var/lib/alsa/asound.state
#   亮度      /sys/class/backlight/backlight/brightness（走 brightnessctl）
#   CPU 调速  /sys/devices/system/cpu/cpufreq/policy0/scaling_governor
#   GPU 调速  /sys/devices/platform/ff400000.gpu/devfreq/ff400000.gpu/governor
#   内存调速  /sys/devices/platform/dmc/devfreq/dmc/governor
#   ES 各项   /home/ark/.emulationstation/es_settings.cfg（XML，ES 自己写）

ES_SETTINGS = "/home/ark/.emulationstation/es_settings.cfg"
BACKLIGHT = "/sys/class/backlight/backlight"
CPU_GOV = "/sys/devices/system/cpu/cpufreq/policy0/scaling_governor"
GPU_GOV = "/sys/devices/platform/ff400000.gpu/devfreq/ff400000.gpu/governor"
DMC_GOV = "/sys/devices/platform/dmc/devfreq/dmc/governor"

# CPU 调速器的可选项，按"省电 -> 性能"排，跟 LANGOS 的 PERFORMANCE SETTINGS 一致
GOVERNOR_CHOICES = ["powersave", "conservative", "ondemand", "interactive",
                    "schedutil", "performance"]
# GPU / DMC 是 devfreq，选项跟 CPU 不是一套
DEVFREQ_CHOICES = ["powersave", "dmc_ondemand", "simple_ondemand",
                   "performance", "userspace"]


def _read_first_line(path, default=""):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            return fh.read().strip()
    except Exception:
        return default


def _es_settings_read():
    """把 es_settings.cfg 读成 {键: 值}。

    这个文件是 ES 自己写的简单 XML（`<type name="X" value="Y" />` 一行一条），
    不是规范 XML，所以用正则扫，别上 XML 解析器。
    """
    out = {}
    try:
        with open(ES_SETTINGS, "r", encoding="utf-8", errors="replace") as fh:
            src = fh.read()
    except Exception as e:
        log("读 es_settings.cfg 失败: %s" % e)
        return out
    for m in re.finditer(
            r'<(bool|int|string|float)\s+name="([^"]+)"\s+value="([^"]*)"\s*/?>', src):
        out[m.group(2)] = m.group(3)
    return out


def _es_settings_write(key, value, kind="string"):
    """改 es_settings.cfg 里的一个键（原地改，没有就追加）。

    注意：**只在文件里替换那一行**，不要整个重写 —— 这个文件里还有
    SlideshowScreenSaverBackgroundAudioFile 之类很长的路径值，重写容易把
    转义和顺序搞乱。ES 自己也是原地改。
    """
    try:
        with open(ES_SETTINGS, "r", encoding="utf-8", errors="replace") as fh:
            src = fh.read()
    except Exception as e:
        log("改设置前读 es_settings.cfg 失败: %s" % e)
        return False

    def esc(s):
        return (str(s).replace("&", "&amp;").replace('"', "&quot;")
                .replace("<", "&lt;").replace(">", "&gt;"))

    line = '<%s name="%s" value="%s" />' % (kind, key, esc(value))
    pat = re.compile(r'<[a-z]+\s+name="%s"\s+value="[^"]*"\s*/?>' % re.escape(key))
    if pat.search(src):
        new = pat.sub(lambda m: line, src, count=1)
    else:
        # 没有就插在 <?xml ...?> 后面
        m = re.match(r'^(<\?xml[^>]*\?>\s*\n?)', src)
        if m:
            new = src[:m.end()] + line + "\n" + src[m.end():]
        else:
            new = line + "\n" + src
    if new == src:
        return True
    try:
        with open(ES_SETTINGS, "w", encoding="utf-8") as fh:
            fh.write(new)
        log("设置 %s = %r（%s）" % (key, value, kind))
        return True
    except Exception as e:
        log("写 es_settings.cfg 失败: %s" % e)
        return False


def _sh(cmd):
    """跑一条 shell，返回 (rc, 输出)。设置类命令都是短平快的，不需要异步。"""
    try:
        p = subprocess.Popen(["/bin/bash", "-lc", cmd],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        out, _ = p.communicate(timeout=15)
        return p.returncode, (out or b"").decode("utf-8", "replace").strip()
    except Exception as e:
        return 1, str(e)


def _alsa_volume(retries=6, delay=0.5):
    """当前音量百分比。amixer 的输出是 'Front Left: 13 [5%] [-23.25dB]'。

    为什么带重试：这个函数在**服务启动早期**就被 read_settings() 调到，而
    RK817 codec 是异步探测的（dmesg 里 #0: rockchip,rk817-codec 在 4.2s 才注册），
    我们比它先跑到这一步时 amixer 会 "Mixer attach error"、输出为空。
    实测：同一个 root、同一套 env，开机后手工跑永远成功 → 纯粹是竞态。

    重试上限 ~3s：够覆盖 codec 还没出来的窗口，又不至于把启动拖久。
    还读不到就返回 -1，前端显示"不可用"（不假装有个 0）——
    而且用户在设置页里左右调一次就会立刻拿到真实值，不会一直卡在不可用。"""
    for attempt in range(retries):
        rc, out = _sh("amixer sget Playback 2>/dev/null")
        m = re.search(r'\[(\d+)%\]', out)
        if m:
            return int(m.group(1))
        if attempt < retries - 1:
            time.sleep(delay)
    log("音量读取失败（amixer 连续 %d 次没给出百分比），前端将显示不可用" % retries)
    return -1


def _backlight():
    cur = _read_first_line(os.path.join(BACKLIGHT, "brightness"))
    mx = _read_first_line(os.path.join(BACKLIGHT, "max_brightness"))
    try:
        return int(cur), int(mx)
    except ValueError:
        return -1, -1


def read_settings():
    """把当前设置读成一个 dict，注入给前端。

    读不到的都返回 -1 / ""，前端据此显示"不可用"，不要假装有个默认值。
    """
    es = _es_settings_read()
    cur_bl, max_bl = _backlight()

    def es_bool(k, default=False):
        v = es.get(k)
        if v is None:
            return default
        return v.strip().lower() in ("true", "1", "yes")

    def es_int(k, default=0):
        try:
            return int(es.get(k, default))
        except (TypeError, ValueError):
            return default

    # LANGOS 的亮度不是 0~100，而是 backlight 的原始值。前端拿 max 自己换算。
    bl_pct = round(cur_bl * 100.0 / max_bl) if (cur_bl >= 0 and max_bl > 0) else -1

    return {
        # ---- 硬件 ----
        "volume": _alsa_volume(),
        "brightness": bl_pct,
        "brightness_max": max_bl,
        # ---- 调速器 ----
        "cpu_governor": _read_first_line(CPU_GOV, "?"),
        "gpu_governor": _read_first_line(GPU_GOV, "?"),
        "dmc_governor": _read_first_line(DMC_GOV, "?"),
        # ---- ES 里那些选项（网页端也照抄过来，改了写回同一个文件）----
        "language": es.get("Language", "en-US"),
        "theme": es.get("ThemeSet", ""),
        "gamelist_view": es.get("GamelistViewStyle", "detailed"),
        "transition": es.get("TransitionStyle", "instant"),
        "screensaver_behavior": es.get("ScreenSaverBehavior", "dim"),
        "screensaver_after": es.get("ScreenSaverAfter", "300"),
        "show_clock": es_bool("DrawClock"),
        "clock_12h": es_bool("ClockMode12"),
        "favorites_first": es_bool("FavoritesFirst"),
        "quick_system_select": es_bool("QuickSystemSelect"),
        "folder_view": es.get("FolderViewMode", "never"),
        "sort_systems": es.get("SortSystems", "default"),
        "hidden_systems": es.get("HiddenSystems", ""),
        "max_vram": es_int("MaxVRAM", 150),
        # 可选项一并给前端，省得两边各写一份还写歪
        "choices": {
            "cpu_governor": GOVERNOR_CHOICES,
            "gpu_governor": DEVFREQ_CHOICES,
            "language": ["en-US", "zh-CN", "ja-JP", "ko-KR", "fr-FR", "de-DE", "es-ES"],
            "gamelist_view": ["automatic", "basic", "detailed", "video", "grid"],
            "transition": ["instant", "fade", "slide", "slidefade", "flip", "cube"],
            "screensaver_behavior": ["dim", "black", "random video", "slideshow", "clock"],
            "screensaver_after": ["60", "120", "300", "600", "1800", "0"],
            "folder_view": ["never", "always", "when_single"],
        },
    }


# 前端 setSetting(key, value) 的路由表。
# 每个 key 对应一个能真正落地的操作 —— 落不下的宁可不做，也不要假装成功。
def apply_setting(key, value):
    """把一项设置真正落到设备上。返回 (ok, 人类可读说明)。"""
    v = "" if value is None else str(value)

    if key == "volume":
        try:
            pct = max(0, min(100, int(float(v))))
        except ValueError:
            return False, "音量要 0-100"
        rc, out = _sh("amixer sset Playback %d%% && alsactl store 2>/dev/null" % pct)
        if rc != 0:
            return False, out[:120]
        # 写完回读一次：一是确认真的落了，二是顺带把启动期读到的 -1 修掉
        # （开机那一刻 codec 可能还没就绪，用户在设置页调一次就该拿到真值）
        real = _alsa_volume(retries=2, delay=0.2)
        return True, ("音量设为 %d%%（回读 %s）"
                      % (pct, ("%d%%" % real) if real >= 0 else "读不到"))

    if key == "brightness":
        try:
            pct = max(1, min(100, int(float(v))))
        except ValueError:
            return False, "亮度要 1-100"
        rc, out = _sh("brightnessctl set %d%%" % pct)
        return rc == 0, ("亮度设为 %d%%" % pct) if rc == 0 else out[:120]

    if key in ("cpu_governor", "gpu_governor", "dmc_governor"):
        path = {"cpu_governor": CPU_GOV, "gpu_governor": GPU_GOV,
                "dmc_governor": DMC_GOV}[key]
        allowed = GOVERNOR_CHOICES if key == "cpu_governor" else DEVFREQ_CHOICES
        if v not in allowed:
            return False, "不支持的调速器 %s" % v
        rc, out = _sh("echo %s | tee %s >/dev/null" % (v, path))
        if rc != 0:
            return False, out[:120]
        return True, "%s 已切到 %s" % (
            {"cpu_governor": "CPU", "gpu_governor": "GPU", "dmc_governor": "内存"}[key], v)

    # ---- ES 选项：按各自的类型写回 es_settings.cfg ----
    ES_MAP = {
        "language":             ("Language", "string"),
        "theme":                ("ThemeSet", "string"),
        "gamelist_view":        ("GamelistViewStyle", "string"),
        "transition":           ("TransitionStyle", "string"),
        "screensaver_behavior": ("ScreenSaverBehavior", "string"),
        "screensaver_after":    ("ScreenSaverAfter", "int"),
        "folder_view":          ("FolderViewMode", "string"),
        "sort_systems":         ("SortSystems", "string"),
        "max_vram":             ("MaxVRAM", "int"),
        "show_clock":           ("DrawClock", "bool"),
        "clock_12h":            ("ClockMode12", "bool"),
        "favorites_first":      ("FavoritesFirst", "bool"),
        "quick_system_select":  ("QuickSystemSelect", "bool"),
    }
    if key in ES_MAP:
        name, kind = ES_MAP[key]
        if kind == "bool":
            val = "true" if v.strip().lower() in ("true", "1", "yes", "on") else "false"
        elif kind == "int":
            try:
                val = str(int(float(v)))
            except ValueError:
                return False, "%s 要整数" % key
        else:
            val = v
        ok = _es_settings_write(name, val, kind)
        return ok, ("%s = %s（重启前端后生效）" % (name, val)) if ok else "写入失败"

    return False, "未知设置项 %s" % key


def build_bridge_js(cfg, dirs, texts):
    exts = {e: 1 for e in ROM_EXTS}
    try:
        settings = read_settings()
    except Exception as e:
        # 设置读不出来不能连累整个前端 —— 桥接照发，设置页显示"不可用"
        log("read_settings() 失败，设置页将不可用: %s" % e)
        settings = {}
    return BRIDGE_JS_TMPL % {
        "rom_root": json.dumps(cfg["compat_dir"]),
        "dirs":     json.dumps(dirs, ensure_ascii=False),
        "texts":    json.dumps(texts, ensure_ascii=False),
        "exts":     json.dumps(exts),
        "settings": json.dumps(settings, ensure_ascii=False),
    }


# ------------------------------------------------------------- 启动命令构造
#
# 原则：**照抄 es_systems.cfg 的 <command> 模板**，只做占位符替换。
# 这样跑出来的东西跟 ES 点游戏时一模一样（包括 sudo perfmax/perfnorm 调频、
# 走 /usr/local/bin/retroarch 那个包装脚本把 tty0 让给 RA 等等），
# 也就等于"调用 Linux 原生的 RA"，不用自己另起一套。
#
# 占位符语义（在真镜像是核对过的）：
#   %ROM%        ROM 的 shell 转义路径
#   %EMULATOR%   模拟器名，可能是 retroarch / retroarch32 / drastic /
#                standalone-r7 / standalone-nobios / standalone
#   %CORE%       选中的 core 名。**只有**模板里出现 %CORE%_libretro.so 时
#                它才真的是 cores 目录里的 .so；standalone 那几家
#                （dreamcast.sh / naomi.sh / saturn.sh）是自己解释这个名字的，
#                所以不能拿 .so 是否存在去校验它
#   %GOVERNOR%   调速器名，perfmax 拿它当 $1：performance / powersave / ...
_CORE_SO_RE = re.compile(r"(\S*)/%CORE%_libretro\.so")

# 这些系统的 %CORE% 不是 libretro 的 .so，而是给自家 .sh 包装脚本看的代号
# （n64.sh / OpenBor.sh / easyrpg.sh / pico8.sh / AltSDL.sh 都是这样）
STANDALONE_SYSTEMS = ("nds", "saturn", "dreamcast", "naomi", "ppsspp", "pspminis",
                      "n64", "openbor", "easyrpg", "pico-8", "ports")


def _cores_dir_for(cmd, emulator):
    """从命令模板里推出 core 的所在目录（模板不带 libretro 时返回 None）"""
    m = _CORE_SO_RE.search(cmd or "")
    if not m:
        return None
    return m.group(1).replace("%EMULATOR%", emulator)


def _cores_dirs(cmd, emulator, cfg):
    """core 可能呆的目录，按可信度排序，只留真实存在的。

    模板里写死了 /home/ark/.config/retroarch/cores，但那个目录在
    非目标环境下不一定在（自测、换卡）。这时以配置里的 ra_cores 为准。
    PS1 走的是 32 位那套（retroarch32），目录名要跟着换。
    """
    cands = []
    d = _cores_dir_for(cmd, emulator)
    if d:
        cands.append(d)
    base = (cfg.get("ra_cores") or "").rstrip("/")
    if base:
        cands.append(base.replace("/retroarch/", "/retroarch32/") if "32" in (emulator or "")
                     else base)
    out = []
    for c in cands:
        if c and c not in out and os.path.isdir(c):
            out.append(c)
    return out


def pick_core(cmd, emulator, cores, fallback, cfg, cores_all=None, force=None):
    """挑一个真正存在的 core。

    es_systems.cfg 里每个系统列了一串 core，第一个不一定是装了的
    （真镜像里 nds 的 desmume 就没装）。所以按顺序挑第一个
    `${core}_libretro.so` 真的躺在 cores 目录里的。

    cores     —— **被选中的那个 emulator** 自己的 core 列表，只有它才算数
    cores_all —— 全部 emulator 的 core 并集，仅当命令确实要 `-L` 加载某个 .so
                 （retroarch 系）时用来兜底；standalone 命令绝不能碰它，
                 否则 %CORE% 会多占一个参数位，把 $2 处的 ROM 顶掉。
    force     —— FORCE_CORE 里的平台级指定。文件在就无条件用它，
                 盖过 es_systems.cfg 的顺序（见 BAD_CORES 的注释）。
    """
    dirs = None
    if _cores_dir_for(cmd, emulator) is not None:
        dirs = _cores_dirs(cmd, emulator, cfg)

    cands = []
    for c in list(cores or []) + ([fallback] if fallback else []):
        if c and c not in cands:
            cands.append(c)

    # force 的优先级最高：直接提到队首走同一条"文件在不在"的校验
    if force and force in cands:
        cands.remove(force)
    if force:
        cands.insert(0, force)
    elif fallback and fallback in cands:
        # 没有 force 时，兜底表里的 core 也提前 —— 那是我们手工核过能用的。
        # 但不能盖过 ES 自己的首选，只提到"ES 首选之后"。
        pass

    # 坏核心一律剔除：文件在也算不装。必须放在 force 之后，
    # 否则 force 指向的 core 若恰好也在黑名单里就没法报错提示了。
    def _bad(c):
        return c in BAD_CORES

    if dirs is None:
        # standalone / 包装脚本：%CORE% 不是要加载的 .so。多数脚本根本不用它，
        # 少数（n64.sh 的 standalone-Rice 分支）把它当开关标志。
        # 只认被选中 emulator 自己的 core，没有就返回**空串** —— 与 ES 一致。
        return cands[0] if cands else ""

    for c in cands:
        if _bad(c):
            continue
        for d in dirs:
            if os.path.isfile(os.path.join(d, "%s_libretro.so" % c)):
                if force and c == force:
                    log("平台强制使用 core %s（ES 首选是 %s）" % (c, cores[0] if cores else "?"))
                return c

    # 选中的 core 一个都没装，才允许动用并集兜底（仍是真实的 .so，安全）
    for c in (cores_all or []):
        if c in cands or _bad(c):
            continue
        for d in dirs:
            if os.path.isfile(os.path.join(d, "%s_libretro.so" % c)):
                log("选中的 core 都没装，改用并集里的 %s 兜底" % c)
                return c

    log("cores 目录 %s 里一个候选 core 都没装（%s），仍按第一个走"
        % (dirs or "无", cands))
    return cands[0] if cands else ""


def substitute_command(cmd, rom, systems, sysname, cfg, platform_core, force_core=None,
                       force_emulator="", override_core=None):
    """把 es_systems.cfg 的 command 模板填好

    override_core —— 用户在前端显式选的核心。None = 没选（走原来的挑选逻辑）；
                     字符串（**可以是空串**）= 就用这个值。空串是合法值：
                     standalone 的 %CORE% 本来就该是空的，那批包装脚本
                     （n64.sh / naomi.sh / saturn.sh）靠**参数个数**区分 ——
                     `if [ "$#" -gt 2 ]; then game="${3}"; else game="${2}"; fi`。
                     所以显式指定时必须跳过 pick_core 的"文件在不在"挑选，
                     否则空串会被它换成别的核心名，把 $2 处的 ROM 顶掉。
    """
    meta = systems.get(sysname, {}) if sysname else {}
    emulator = (meta.get("emulator") or "").strip() or "retroarch"
    cores = meta.get("cores")

    # 平台级强制换模拟器（见 FORCE_EMULATOR）。换过去之后：
    #   - 不能再用原 emulator 的 core 表（那是别人家的核心名）
    #   - %CORE% 按 ES 的语义应该为空 —— standalone 的 .sh 大多不用它，
    #     少数（n64.sh 的 standalone-Rice 分支）把它当开关，给错值会错分支
    if force_emulator and force_emulator != emulator:
        log("平台强制使用模拟器 %s（es_systems.cfg 里排第一的是 %s）"
            % (force_emulator, emulator or "?"))
        emulator = force_emulator
        cores = []

    if override_core is not None:
        core = override_core
    else:
        core = pick_core(cmd, emulator, cores, platform_core, cfg,
                         meta.get("cores_all") if cores else None, force_core)

    out = cmd
    out = out.replace("%ROM_RAW%", rom)
    out = out.replace("%ROM%", shlex.quote(rom))
    out = out.replace("%EMULATOR%", emulator)
    out = out.replace("%CORE%", core)
    out = out.replace("%GOVERNOR%", "performance")
    out = out.replace("%BASENAME%", shlex.quote(os.path.basename(rom)))
    # 去掉可能残留的 %CONTROLLER% 之类
    out = re.sub(r"%[A-Z_]+%", "", out)
    return out


def direct_command(cfg, rom, pid, sysname, platform_core):
    """兜底：不走 es_systems.cfg，直接拼一条 RetroArch 命令。

    只有 retroarch / retroarch32 这两类能这么拼；
    standalone（NDS/Saturn/Dreamcast/NAOMI/PSP）必须由各自的 .sh 包装脚本处理，
    这里返回 None 让上层报错，而不是拼一条注定跑不起来的命令。
    """
    if sysname in STANDALONE_SYSTEMS:
        # 少数 standalone 的命令形态我们实测过、可以写死（见 STANDALONE_DIRECT）
        tmpl = STANDALONE_DIRECT.get(pid) or STANDALONE_DIRECT.get(pid.upper()) \
            or STANDALONE_DIRECT.get(sysname)
        if tmpl:
            femu = FORCE_EMULATOR.get(pid) or FORCE_EMULATOR.get(pid.upper()) or ""
            out = tmpl.replace("%EMULATOR%", femu)
            out = out.replace("%ROM%", shlex.quote(rom))
            log("es_systems.cfg 没有可用的 %s 模板，用写死的 standalone 命令兜底" % sysname)
            return re.sub(r"%[A-Z_]+%", "", out)

        log("平台 %s（系统 %s）是 standalone 模拟器，没有 ES 模板启动不了"
            % (pid, sysname))
        return None

    emulator = "retroarch32" if pid in PLATFORM_RA32 else "retroarch"
    bin_path = cfg["retroarch"].rstrip("/")
    if pid in PLATFORM_RA32:
        bin_path += "32"

    dirs = _cores_dirs("", emulator, cfg)
    if not dirs:
        log("找不到 cores 目录，没法直接拼 RA 命令（ra_cores=%s）" % cfg.get("ra_cores"))
        return None

    core = platform_core or sysname or ""
    corefile = None
    for d in dirs:
        cand = os.path.join(d, "%s_libretro.so" % core)
        if os.path.isfile(cand):
            corefile = cand
            break
    if not corefile:
        log("兜底 core 不存在（%s in %s）" % (core, dirs))
        return None
    return "nice -n -19 %s -L %s %s" % (shlex.quote(bin_path),
                                        shlex.quote(corefile),
                                        shlex.quote(rom))



def _fix_easyrpg_content(rom):
    """EasyRPG 的"游戏条目"可能是**同名目录**，而不是那个 .zip。

    实测卡二内容（/roms2/roms/EASYRPG）：
        FF72DR/                    <- 真正的游戏目录（Map0001.lmu / ChipSet/ ...）
        FF72DR.zip                 <- 旁边一个 31MB 的压缩包
        FF72DR.zip.save/           <- ES 生成的存档目录
    `*.zip.save/` 这个目录名是决定性证据：ES 把游戏记成 "FF72DR.zip" 时，
    存档目录才叫这个名字。但 EasyRPG 核心要吃的是**解开的游戏目录**，
    把 .zip 直接喂过去起不来。

    规则：给出 <dir>/<name>.zip 时，若旁边存在同名目录 <dir>/<name>/，就用目录。
    找不到同名目录就原样返回（不猜，免得把真·zip 内容改错）。
    """
    if not rom.lower().endswith(".zip"):
        return rom
    twin = rom[:-4]                      # 去掉 ".zip"
    if os.path.isdir(twin):
        return twin
    return rom


# ------------------------------------------------------------------- 游戏遮罩
# 用户 2026-09-14："一键设置遮罩 把所以平台画面比例换成核心原生比例 然后
# 对应遮罩自动加载且不透明度 100%"，并要求三种分辨率（1024x768 / 640x480 /
# 720x720）能在设置里一键切换。
#
# 分工：设置项在 vgfront 的设置页（写 /opt/vagaretro/vgfront.conf 的 bezel=），
# 这里只负责"启动某个平台时，把它对应的那套遮罩挂上去"。
#
# 为什么用 --appendconfig 而不是直接改 retroarch.cfg：
#   ① retroarch.cfg 是玩家自己的设置，不该被我们写脏（改坏了整个 RA 都乱）；
#   ② appendconfig 优先级高于 `-c`，天然压过全局；
#   ③ 换平台就换一份 appendconfig —— 一个临时文件覆盖所有情况，
#      不需要给几十个 core 各写一份 override。
#
# ⚠ 为什么画面位置用 custom_viewport_* 而不是 aspect_ratio_index=21：
#   这三套包的 cfg 根本不是一种格式 —— 1024x768 是**标准 RA overlay**
#   （只有 overlay0_* 四行），而 640x480 / 720x720 带 `grh_custom_viewport_*`，
#   那是**定制版 RA** 的扩展参数。设备上这个标准 RA 1.22.2 一个都不认
#   （strings /opt/retroarch/bin/retroarch | grep -c grh_ 结果是 0）。
#   照抄的话遮罩会贴上去、画面却还占满全屏，四周被遮罩糊掉一圈。
#   好在窗口位置本来就藏在 PNG 的 alpha 里（作者把窗口画成全透明），
#   PC 端预先算好存成 bezels/windows.tsv，运行期查表即可（120 行，零开销）。
#   用 custom viewport 还多一个好处：RA 会**保持核心原生比例**把画面缩放
#   居中放进这个矩形 —— 既不变形，又正好落在窗口里，等于两头都满足。
BEZELS_DIR     = "/opt/vagaretro/bezels"
# ⚠ 这份文件现在装的是"启动期要覆盖的所有东西"：遮罩 + 退出自动存档。
#   RA 只认**最后一个** --appendconfig，所以两者必须合并写进同一份，
#   分两次注入后面那个会把前面整个顶掉（只能生效一个）。
PLAY_APPCONF   = "/opt/vagaretro/bezel-active.cfg"
VGCONF_DEFAULT = "/opt/vagaretro/vgfront.conf"

# 退出自动存档的平台名单（一行一个 platform.id，支持 # 注释）。
# 前端弹层的「退出自动存档」开关直接读写它 —— 粒度是**平台**（用户
# 2026-09-16 明确："针对平台生效 不用加到设置里面"）。
AUTOSAVE_PATH  = "/opt/vagaretro/autosave.txt"

# 存档位（每个游戏一套）：一行 `<pid>\t<rel>\t<加载位>`。
# （老文件是四列、多一个「保存位」，读的时候按 ≥3 列处理、忽略第 4 段。）
# 前端弹层的「加载存档」写它，这里读出来注入 state_slot。
# 为什么只能"启动时生效"：前端是启动器，游戏跑起来之后它已经退出了，
# 没法在游戏运行中替玩家读/存档 —— 只能把位号交给 RetroArch。
# ⚠ 「保存存档」2026-09-16 已按用户要求去掉（原话："保存只能在 RA 完成"）。
SLOTS_PATH     = "/opt/vagaretro/slots.txt"

# 平台 id -> 遮罩包里的目录名（给候选列表，取第一个存在的）。
# 同一台机在三套包里叫法不统一（红白机叫 famicom 也叫 nes，超任叫 sfc
# 也叫 snes），所以不能一对一。
BEZEL_PLAT = {
    "FC":     ["famicom", "nes"],
    "SFC":    ["sfc", "snes"],
    "GB":     ["gb"],
    "GBC":    ["gbc"],
    "GBA":    ["gba"],
    "NDS":    ["nds"],
    "MD":     ["megadrive", "genesis"],
    "SS":     ["saturn"],
    "DC":     ["dreamcast"],
    "PS1":    ["psx"],
    "DOS":    ["pc"],
    "PCE":    ["pcengine", "tg16"],
    "NGPC":   ["ngpc", "ngp"],
    "WS":     ["wonderswan"],
    "WSC":    ["wonderswancolor", "wonderswan"],
    "GG":     ["gamegear"],
    "NAOMI":  ["naomi", "atomiswave"],
    "N64":    ["n64"],
    "MS":     ["mastersystem"],
    "ARCADE": ["arcade", "varcade"],
    "LYNX":   ["lynx"],
}

_BEZEL_WIN = None      # {(分辨率, 平台): (x, y, w, h)}，懒加载一次


def _bezel_windows():
    global _BEZEL_WIN
    if _BEZEL_WIN is not None:
        return _BEZEL_WIN
    out = {}
    try:
        with open(os.path.join(BEZELS_DIR, "windows.tsv"), "r", encoding="utf-8") as fh:
            for ln in fh:
                if not ln.strip() or ln.lstrip().startswith("#"):
                    continue
                f = ln.rstrip("\n").split("\t")
                if len(f) < 6:
                    continue
                out[(f[0], f[1])] = (int(f[2]), int(f[3]), int(f[4]), int(f[5]))
    except Exception as e:
        log("读遮罩窗口表失败（画面会退回全屏，至少不变形）: %s" % e)
    _BEZEL_WIN = out
    return out


def read_vgfront_conf(key, default=""):
    """从 vgfront 的设置文件里取一个键。

    单独读它而不是并进 load_config：那是前端的设置、这是启动器的设置，
    两边本来就不该互相依赖，文件小直接扫一遍最省事。
    """
    try:
        with open(VGCONF_DEFAULT, "r", encoding="utf-8", errors="replace") as fh:
            for ln in fh:
                ln = ln.strip()
                if ln.startswith(key + "="):
                    return ln[len(key) + 1:].strip()
    except Exception:
        pass
    return default


def rom_source_mode():
    """设置页「游戏来源」选的档：auto / 1 / 2。

    auto = 两卡都扫、卡二优先（老行为）；1 = 只读卡一（/roms）；
    2 = 只读卡二（/roms2）。

    每次建树都现读一遍，不缓存 —— 用户可能在设置页刚改过，而那个改动
    只写了 vgfront.conf、进程还活着。认不出的值一律当 auto。
    """
    v = read_vgfront_conf("romsrc", "auto").strip().lower()
    return v if v in ("auto", "1", "2") else "auto"


def autosave_on(pid):
    """这个平台开没开「退出自动存档」。"""
    if not pid:
        return False
    try:
        with open(AUTOSAVE_PATH, "r", encoding="utf-8", errors="replace") as fh:
            for line in fh:
                line = line.strip()
                if line and not line.startswith("#") and line == pid:
                    return True
    except OSError:
        pass
    return False


def autosave_lines(pid):
    """退出自动存档要写进 appendconfig 的行。没开就返回 []。

    为什么要 savestate_auto_load：只开 auto_save 的话，退出时存了档但下次
    启动不会用 —— 用户要的是"接着玩"，所以两个一起开。
    （savefiles/savestates_in_content_dir 本来就是 true，存档落在 ROM 旁边，
      不会跟别的平台串。）
    """
    if not autosave_on(pid):
        return []
    return [
        "# 退出自动存档（前端「启动游戏 → 退出自动存档」开的）",
        'savestate_auto_save = "true"',
        'savestate_auto_load = "true"',
    ]


def slot_override_for(pid, rel):
    """读 slots.txt 里这个游戏的**加载位**，没配过就是 0。

    格式（前端弹层「加载存档」写的；2026-09-16 起是三列）：
        <平台id>\t<游戏相对路径>\t<加载位>
    位号 0..5，0 = 关闭。
    ⚠ 老文件是四列（多一个「保存位」）。这里**只要求 ≥3 列、只认第 3 列**，
     所以盘上已有的老数据不用迁移也读得出来。
    ⚠ 「保存存档」2026-09-16 按用户要求从弹层去掉（原话："保存只能在 RA 完成"），
     保存位不再存在；要"退出时自动存"请用「退出自动存档」那个平台级开关。

    键的两个字段必须和前端 G 行、和 --launch 的 rel **逐字符相等** ——
    差一个字就是"选了不生效"，所以这里不做任何规范化，直接字符串比。
    """
    if not pid or not rel:
        return 0
    try:
        with open(SLOTS_PATH, "r", encoding="utf-8", errors="replace") as fh:
            for ln in fh:
                ln = ln.rstrip("\r\n")
                if not ln or ln.startswith("#"):
                    continue
                f = ln.split("\t")
                if len(f) < 3:
                    continue
                if f[0] != pid or f[1] != rel:
                    continue
                try:
                    a = int(f[2])
                except ValueError:
                    return 0
                return a if 0 <= a <= 5 else 0
    except OSError:
        pass
    return 0


def slot_lines(pid, rel):
    """存档位要写进 appendconfig 的行。没配过就返回 []。

    「加载存档」= 启动时把 state_slot 指到第 N 位 + savestate_auto_load，
    进游戏由 RA 自己去读 —— 前端不是游戏现场，只能做到这一步。
    （「保存存档」2026-09-16 已去掉，所以这里不再写 savestate_auto_save；
     要"退出自动存"走「退出自动存档」的 autosave_lines()。）
    """
    load = slot_override_for(pid, rel)
    if not load:
        return []
    return ["# 存档位（前端「启动游戏 → 加载存档」设的）",
            'state_slot = "%d"' % load,
            'savestate_auto_load = "true"']


def bezel_lines(pid, mode):
    """按平台凑遮罩 appendconfig 的行；该平台没遮罩就返回 []。

    mode = 分辨率目录名（1024x768 / 640x480 / 720x720），空串 = 关闭遮罩。
    """
    if not mode:
        return []
    cands = BEZEL_PLAT.get(pid) or BEZEL_PLAT.get((pid or "").upper())
    if not cands:
        return []
    base = os.path.join(BEZELS_DIR, mode)
    plat = None
    for c in cands:
        if os.path.isfile(os.path.join(base, c, "default.cfg")):
            plat = c
            break
    if not plat:
        return []                        # 这套包里没这个平台，安静跳过

    lines = [
        'input_overlay = "%s/%s/default.cfg"' % (base, plat),
        'input_overlay_enable = "true"',
        'input_overlay_opacity = "1.000000"',      # 用户要求：不透明度 100%
        'input_overlay_hide_in_menu = "true"',
    ]
    win = _bezel_windows().get((mode, plat))
    if win:
        lines += [
            'aspect_ratio_index = "22"',           # 22 = custom（用下面这个视口）
            'custom_viewport_x = "%d"' % win[0],
            'custom_viewport_y = "%d"' % win[1],
            'custom_viewport_width = "%d"' % win[2],
            'custom_viewport_height = "%d"' % win[3],
        ]
    else:
        # 没算出窗口就只保证"核心原生比例"，画面仍占满屏 —— 至少不变形
        lines.append('aspect_ratio_index = "21"')  # 21 = core provided
    return lines


def play_apconfig_write(lines):
    """把启动期要覆盖的行落盘，返回路径；没有内容就删掉文件并返回 None。

    返回 None 是正常情况（这个平台既没遮罩也没开自动存档）：调用方就不再
    往命令里插 --appendconfig 了，跟这个功能不存在时一模一样。
    """
    if not lines:
        try:
            os.remove(PLAY_APPCONF)
        except OSError:
            pass
        return None
    head = "# 由 vagaretro-shell.py 生成，勿手工编辑（每次启动游戏都会重写）"
    try:
        with open(PLAY_APPCONF, "w", encoding="utf-8") as fh:
            fh.write(head + "\n" + "\n".join(lines) + "\n")
    except Exception as e:
        log("写启动配置失败: %s" % e)
        return None
    return PLAY_APPCONF


def _inject_appendconfig(cmd, rom_arg, appconf):
    """把 --appendconfig 塞进命令里 retroarch 那一段。

    ⚠ 不能直接往命令末尾拼。ES 的模板是
        `sudo perfmax ... %ROM%; nice -n -19 retroarch -L xxx.so %ROM%; sudo perfnorm`
    —— 尾巴上挂着 `sudo perfnorm`，拼到最后会变成给 perfnorm 传参数，
    retroarch 反而收不到。所以要找 **retroarch 之后**那次 ROM 参数，插在它后面。
    """
    q = shlex.quote(rom_arg)
    ri = cmd.find("retroarch")
    if ri < 0:
        return None
    i = cmd.rfind(q)
    if i < ri:
        return None                      # ROM 没出现在 retroarch 之后，不敢乱插
    j = i + len(q)
    return cmd[:j] + " --appendconfig=" + shlex.quote(appconf) + cmd[j:]


# ------------------------------------------------------------------- 着色器
# 用户 2026-09-16："启动游戏弹层加个着色器功能"、"着色器就是核心生效
# 只要是同个核心都生效"。
#
# 粒度 = **模拟器核心**（不是游戏、不是平台、更不是全局）。前端把选择写成
# 一行一条 <核心名>\t<预设相对路径>；同一 core 下的所有游戏共用一套 ——
# 这正是 RA 自己的粒度，也是用户要的。
#
# 为什么用 --set-shader 而不是往 retroarch.cfg 里写 video_shader：
#   ① 那份 cfg 是**全局**的，一写所有核心一起变，做不到"按核心"；
#   ② 实测 --appendconfig 传 video_shader（路径类键）**会被丢成空串**
#      （2026-09-16 shader-aptest3.log；同一份里的 menu_driver / video_scale /
#        video_smooth 这些普通键都正常生效）；
#   ③ RA 1.22.2 自带 `--set-shader=PATH`，帮助原文：
#        "Path to a shader (preset) that will be loaded each time content is
#         loaded. Effectively overrides automatic shader presets. An empty
#         argument \"\" will disable automatic shader presets."
#      一次进程一个值，不碰任何配置文件、退出即失效、零残留。
SHADERS_PATH   = "/opt/vagaretro/shaders.txt"
SHADERS_DIR    = "/home/ark/.config/retroarch/shaders/shaders_glsl"
SHADERS_DIR_32 = "/home/ark/.config/retroarch32/shaders/shaders_glsl"


def shader_override_for(core):
    """这个核心被指定的着色器预设（相对 shaders_glsl 的路径）。没有就空串。

    和 load_core_overrides 一样刻意不缓存：文件不到 1KB，而"改完下一次启动
    立刻生效"比省这一次读盘重要得多。
    """
    if not core:
        return ""
    try:
        with open(SHADERS_PATH, "r", encoding="utf-8", errors="replace") as fh:
            for line in fh:
                line = line.rstrip("\r\n")
                if not line or line.startswith("#"):
                    continue
                k, sep, v = line.partition("\t")
                if sep and k.strip() == core:
                    return v.strip()
    except OSError:
        pass
    return ""


def _core_in_cmd(cmd):
    """从拼好的启动命令里抠出**真正会加载**的那个核心名。

    取的是 `-L /path/xxx_libretro.so` 里的 xxx。为什么不复用
    substitute_command() 里算出来的那个 core：那是"我们以为会跑的"，这里要的
    是"命令里实际写着的"。前端按核心配着色器，两者一旦错位，表现就是
    "配了没反应"，而且永远查不出来 —— 所以以命令为准。
    RA 的 -L 是最后出现的那个生效，所以取最后一个匹配。
    standalone 包装脚本（yabasanshiro 那些）命令里没有 -L，返回空串。
    """
    m = None
    for m2 in re.finditer(r"""-L\s+('[^']*'|"[^"]*"|\S+)""", cmd):
        m = m2
    if not m:
        return ""
    p = m.group(1).strip("'\"")
    b = os.path.basename(p)
    if b.endswith("_libretro.so"):
        return b[: -len("_libretro.so")]
    return ""


def _inject_set_shader(cmd, rom_arg, preset_abs):
    """把 --set-shader 插进 retroarch 那一段，紧跟 ROM 参数之后。

    定位法和 _inject_appendconfig 完全一样（见那个函数的注释）：ES 模板的
    尾巴上挂着 `sudo perfnorm`，直接往末尾拼会被它吃掉。
    和 --appendconfig 同时存在时两个选项互不干扰（不同的设置键）。
    """
    q = shlex.quote(rom_arg)
    ri = cmd.find("retroarch")
    if ri < 0:
        return None
    i = cmd.rfind(q)
    if i < ri:
        return None
    j = i + len(q)
    return cmd[:j] + " --set-shader=" + shlex.quote(preset_abs) + cmd[j:]


def build_launch_script(cfg, systems, payload, platform_id):
    """根据桥接传来的信息，生成真正要执行的启动脚本"""
    rom = payload.get("rom", "")
    if not rom:
        log("启动请求里没有 rom 路径")
        return None

    # 从 rom 路径反推 platform.id：<compat_dir>/<ID>/<file>
    pid = platform_id
    if not pid:
        try:
            rel = os.path.relpath(rom, cfg["compat_dir"])
            pid = rel.split(os.sep)[0]
        except Exception:
            pid = ""

    sysname = resolve_system(PLATFORM_MAP.get(pid, pid), systems) or pid
    pcore = PLATFORM_CORE.get(pid, "")
    fcore = FORCE_CORE.get(pid, "")
    femu = FORCE_EMULATOR.get(pid, "") or FORCE_EMULATOR.get((pid or "").upper(), "")
    if fcore and fcore in BAD_CORES:
        log("配置冲突：%s 的 FORCE_CORE 指向黑名单核心 %s，忽略 force" % (pid, fcore))
        fcore = ""

    # ---- 用户在前端「更换模拟器核心」里选的 core / 模拟器 ----------------
    # 前端只写 cores.txt，这里读。没有该文件（或者没为这个平台配过）时拿到
    # 的就是空串，行为跟这个功能不存在时一模一样。
    #
    # ★ 2026-09-16 一天内修了两处，都只在“用户显式选过核心”时才暴露：
    #   ① 传给 substitute_command 的参数原来写的是 ocore_name（未定义）。
    #      平时怎么点都不炸，一选 standalone-Glide64mk2 就 NameError，
    #      前端看着像卡住，日志里其实是“生成启动脚本失败，留在原界面”。
    #   ② 原来把 cores.txt 的整串直接当核心名用，没走 parse_core_override。
    #      于是 “standalone-Glide64mk2:”（注意尾部那个冒号）被塞进 %CORE%，
    #      命令变成 `n64.sh retroarch32 standalone-Glide64mk2: <rom>`；
    #      而 n64.sh 是拿 **$1** 判断走不走 standalone 的，$1 仍是
    #      retroarch32 → 落进 else 分支去找
    #      cores/standalone-Glide64mk2:_libretro.so（根本不存在）→ 起不来。
    #      正确语义：冒号左边是**模拟器**、右边是核心；左边一旦写了就换掉
    #      %EMULATOR%、并把 %CORE% 留空 —— n64.sh / saturn.sh 那批包装脚本
    #      靠**参数个数**区分（%CORE% 空时 $2 才是 ROM）。
    ocore = core_override_for(pid, _rel_in_compat(rom, cfg))
    oemu, oname = parse_core_override(ocore)
    if oemu and oemu in BAD_CORES:
        log("用户选的模拟器 %s（平台 %s）在黑名单里，忽略" % (oemu, pid))
        oemu = ""
    if oname and oname in BAD_CORES:
        log("用户选的 core %s（平台 %s）在黑名单里，忽略" % (oname, pid))
        oname = ""
    if oemu:
        # 换模拟器走 substitute_command 的 force_emulator 通道：它会顺带清空
        # cores 列表，%CORE% 于是走 standalone 分支返回空串（与 ES 语义一致）。
        femu = oemu
        fcore = ""
        # 注意 %CORE% 要写成 %%CORE%% —— 这一行本身走 % 格式化，
        # 直接写 %C 会被当成格式符，运行时抛 unsupported format character。
        log("平台 %s：用户指定模拟器 %s（%%CORE%% 留空；FORCE_CORE=%s / 平台默认 %s）"
            % (pid, oemu, FORCE_CORE.get(pid, "") or "无", pcore or "无"))
    elif oname:
        fcore = oname
        log("平台 %s：用户指定 core %s（FORCE_CORE=%s / 平台默认 %s）"
            % (pid, oname, FORCE_CORE.get(pid, "") or "无", pcore or "无"))

    # ---- 把兼容软链路径换回**真实 ROM 路径**再交给模拟器 ----------------
    # 前端只认 <compat_dir>/<ID>/<file>（那是 ext4 上的软链树），
    # 但有一批 LANGOS 包装脚本会**从 ROM 路径里抠目录名**来决定存档位置，
    # 典型是 saturn.sh：
    #     directory=$(dirname "$2" | cut -d "/" -f2)
    # 原厂路径 /roms2/roms/SS/x.chd  -> 第 2 段 roms2  -> /roms2/saturn/yabasanshiro
    # 兼容路径 /home/ark/.vagaretro-roms/SS/x.chd
    #                                -> 第 2 段 home   -> /home/saturn/yabasanshiro
    # 于是 yabasanshiro 去一个不存在的备份目录，直接启动失败（实测必挂）。
    # 换成 realpath 之后第 2 段回到 roms2/roms，脚本行为跟原厂 ES 完全一致。
    #
    # 只对 STANDALONE_SYSTEMS 做这件事：retroarch 那套根本不看路径结构，
    # 而 realpath 会把中文/特殊字符路径原样带过去，没必要多绕一圈。
    rom_arg = rom
    if sysname in STANDALONE_SYSTEMS:
        # EasyRPG 先做"条目修正"：拿到的是 .zip 而旁边有同名目录时改成目录。
        # 必须排在 realpath 之前 —— 兼容树里的链接指向的是卡二那个 .zip，
        # 先改条目名才能让后面的 realpath 落到真正的游戏目录上。
        if sysname == "easyrpg":
            fixed = _fix_easyrpg_content(rom)
            if fixed != rom:
                log("EasyRPG 条目修正：%s -> %s（.zip 是旁挂包，游戏本体是同名目录）"
                    % (os.path.basename(rom), os.path.basename(fixed)))
                rom = fixed

        try:
            real = os.path.realpath(rom)
            if os.path.exists(real) and real != rom:
                rom_arg = real
                log("ROM 路径已还原为真实路径（%s 需要从路径里抠目录名）: %s"
                    % (sysname, real))
        except Exception as e:
            log("realpath(%s) 失败，沿用兼容路径: %s" % (rom, e))

    meta = systems.get(sysname, {}) if sysname else {}
    cmd = (meta.get("command") or "").strip()

    mode = (cfg.get("launch_mode") or "system").lower()
    final = None
    if mode == "system" and cmd:
        final = substitute_command(cmd, rom_arg, systems, sysname, cfg, pcore, fcore, femu,
                                   override_core=(oname if oname else None))
    if not final:
        final = direct_command(cfg, rom_arg, pid, sysname, pcore)
    if not final:
        log("平台 %s（系统 %s）没有可用的启动命令" % (pid, sysname))
        return None

    # ---- 启动期 appendconfig：游戏遮罩 + 退出自动存档 ---------------------
    # 遮罩由前端写进 vgfront.conf 的 bezel=；空串（或没有这一行）= 关闭。
    # 退出自动存档由前端弹层的开关写 autosave.txt（按平台）。
    #
    # ⚠ 两者**必须合并成同一份**再注入 —— RA 只认**最后一个** --appendconfig，
    #   分两次注入的话后面那份会把前面整个顶掉，遮罩和自动存档只能活一个。
    # ⚠ 只对走 retroarch 的命令处理 —— standalone（yabasanshiro 那些）不认
    #   --appendconfig，硬塞会直接启动失败。
    ap_lines = []
    bezel = read_vgfront_conf("bezel")
    if bezel:
        bl = bezel_lines(pid, bezel)
        if bl:
            ap_lines += bl
            log("遮罩 %s 已挂上：平台 %s" % (bezel, pid))
        else:
            log("遮罩 %s：平台 %s 在这套包里没有对应目录，跳过" % (bezel, pid))
    al = autosave_lines(pid)
    if al:
        ap_lines += al
        log("退出自动存档已挂上：平台 %s" % pid)

    # ---- 存档位（每个游戏一套）--------------------------------------------
    # 前端弹层的「加载存档」。它跟「退出自动存档」是**两件事**：
    #   退出自动存档 = 平台级开关，用 RA 当前的 state_slot（默认 0）
    #   加载存档     = 游戏级，把 state_slot 指到第 N 位、只开 auto_load
    # 两个都开时，RA 后读的行覆盖先读的（这段排在 autosave 之后），
    # 所以 state_slot 最终落在游戏级那一位，而 auto_save 仍由平台开关决定 ——
    # 存进去的正好是玩家选的那一位，语义正确，顺序放后面没问题。
    sl = slot_lines(pid, _rel_in_compat(rom, cfg))
    if sl:
        ap_lines += sl
        log("存档位已挂上：平台 %s" % pid)

    if ap_lines:
        if "retroarch" in final:
            ap = play_apconfig_write(ap_lines)
            injected = _inject_appendconfig(final, rom_arg, ap) if ap else None
            if injected:
                final = injected
                log("启动配置已注入（%d 行）：%s" % (len(ap_lines), ap))
            else:
                log("启动配置：没找到可插入的位置，本次跳过（命令: %s）" % final)
        else:
            log("平台 %s 不走 retroarch，遮罩 / 自动存档 / 存档位都跳过" % pid)
    else:
        play_apconfig_write([])          # 清掉上一份，别让它粘到别的平台上

    # ---- 着色器：按**核心**生效 -------------------------------------------
    # 前端写 shaders.txt（<核心名>\t<预设相对路径>）。核心名从**同一条命令**里
    # 抠（见 _core_in_cmd），不另算一遍 —— 否则"前端按 A 核心配的、实际跑的是
    # B 核心"这种错位永远不会被发现。
    core_name = _core_in_cmd(final)
    sh_rel = shader_override_for(core_name)
    if sh_rel and "retroarch" in final:
        # 32 位那套（PS1 走的就是它）有自己独立的 shaders 目录，路径不能混。
        base = SHADERS_DIR_32 if "retroarch32" in final else SHADERS_DIR
        preset = os.path.join(base, sh_rel)
        if os.path.isfile(preset):
            injected = _inject_set_shader(final, rom_arg, preset)
            if injected:
                final = injected
                log("着色器 %s 已挂上：核心 %s -> %s" % (sh_rel, core_name, preset))
            else:
                log("着色器 %s：没找到可插入的位置，本次跳过（命令: %s）"
                    % (sh_rel, final))
        else:
            log("着色器 %s 不存在（核心 %s，找的是 %s），跳过"
                % (sh_rel, core_name, preset))
    elif sh_rel:
        log("核心 %s：这条命令不走 retroarch，着色器跳过" % core_name)

    log("平台 %s -> 系统 %s%s，启动命令: %s"
        % (pid, sysname, ("，模拟器钉死 %s" % femu) if femu else "", final))
    return "#!/bin/bash\n# 由 vagaretro-shell.py 生成，勿手工编辑\n%s\n" % final



# ------------------------------------------------------------------- GTK 宿主
def write_request(cfg, script, what="启动请求"):
    """把要执行的脚本写到 request_file，交给外层启动器 bash 掉。

    宿主在 native 路径下是**以 root 跑的**（weston 的 DRM 后端需要 root，
    非 root 得靠 logind 的 seat，而 systemd 服务没有 tty 会话），
    启动器却仍然以 ark 跑。所以这里必须：
      - chmod 755：ark 得读得到才能 bash 它
      - 属主尽力交回 ark：万一 request_file 被放回 /tmp 这种带 sticky 位的目录，
        ark 才有权删掉它 —— 删不掉就会同一条请求反复执行，游戏被无限重启
    """
    path = cfg["request_file"]
    d = os.path.dirname(path)
    if d and not os.path.isdir(d):
        try:
            os.makedirs(d, exist_ok=True)
        except Exception:
            pass
    try:
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(script)
        os.chmod(path, 0o755)
    except Exception as e:
        log("写%s失败: %s" % (what, e))
        return False
    try:
        import pwd
        pw = pwd.getpwnam("ark")
        os.chown(path, pw.pw_uid, pw.pw_gid)
    except Exception:
        pass
    log("已写出%s: %s" % (what, path))
    return True


def prepare_rom_tree(cfg, systems):
    """建兼容软链树 + 桥接媒体目录。返回 build_compat_tree 的 resolved。

    抽出来是因为有**两个**调用点：开机跑 --gen-index 时建一次，
    以及 --launch 发现 ROM 不在树里时（ROM 卡热插拔过）兜底重建一次。
    两边必须做同样的事，否则兜底出来的树会缺 /home/bios 这些媒体桥。
    """
    resolved = build_compat_tree(systems, cfg["rom_dir"], cfg["compat_dir"],
                                 cfg.get("rom_dir2", ""), rom_source_mode())
    if not resolved:
        log("警告：没有建立任何 ROM 兼容链接，前端列表会是空的")
    # 包装脚本用 ROM 路径第 2 段当"媒体根"，我们的兼容目录让它落到 /home，
    # 所以得先把 /home/bios、/home/pico-8 这些真实资源桥过去（详见函数注释）
    for line in bridge_media_dirs(systems):
        log("媒体桥 %s" % line)
    return resolved


# ============================================================================
#  ports 的卡根问题：把 /roms2/ports 绑到 /roms/ports
# ============================================================================
#  PortMaster 的 port 脚本都靠 control.txt 里的 $directory 拼自己的游戏目录：
#      GAMEDIR="/$directory/ports/<名字>"          （吃豆人 / 三维弹球 / 崩溃大陆…）
#      GAMEDIR="/$directory/ports/<名字>"          （Celeste / Stardew / TMNT…）
#  个别更偷懒的直接写死：GAMEDIR="/roms/ports/wiliwili"（wiliwili153版.sh）
#
#  而 control.txt 是这么定 $directory 的：
#      if [[ -e /usr/share/plymouth/themes/text.plymouth ]]; then
#        if [ ! -z "$(cat /etc/fstab | grep roms2 | tr -d '\0')" ]; then
#          export directory="roms2"       # 双卡机型
#        else
#          export directory="roms"
#        fi
#      else
#        export directory="roms"
#      fi
#
#  本机（R36H ProMax + LANGOS 镜像）两层都落空：
#    · text.plymouth **存在**（LANGOS 保留了这个文件）→ 进第一层判断；
#    · 卡二是 /dev/mmcblk1p1，由 udev 自动挂载，/etc/fstab 里**没有 roms2 这一行**
#      → grep 落空 → directory="roms"。
#
#  于是每个 port 都去找 /roms/ports/<名字>，也就是**卡一**上的同名目录；
#  而卡一那个 /roms/ports 是启动失败时脚本自己 mkdir 出来的空壳，
#  真数据（celeste/gamedata、wiliwili/wiliwili 那个 33MB 可执行文件 …）
#  全在卡二的 /roms2/ports 下。每个 port 都死在第一屏：
#      /roms2/ports/Celeste.sh: line 22: cd: /roms/ports/celeste/gamedata: No such file or directory
#  实测 30 个 port 里 29 个走 $directory、1 个硬编码 /roms/ports ——
#  所以**只能从路径下手**，改脚本治不了（改了下次装 PortMaster 又被覆盖）。
#
#  修法：一条 mount 把卡二的 ports 绑到卡一的路径上，两族脚本一起治好。
#  不碰任何一张卡上的文件，umount 就还原。
#  （2026-09-15 用户报「移植游戏ports不能运行」时查明。）
#
#  为什么不往 /etc/fstab 里加一行 roms2：那是在骗 control.txt 的 grep，
#  而且 fstab 会去抢 udev 现在这套（选项是对的）挂载，风险远大于收益。
# ============================================================================
PORTS_BIND_SRC = "/roms2/ports"
PORTS_BIND_DST = "/roms/ports"


def ensure_ports_bind():
    """确保 /roms/ports 就是 /roms2/ports（同一个目录）。已经绑过就什么都不做。"""
    if not os.path.isdir(PORTS_BIND_SRC):
        return False                     # 卡二没插或没有 ports，无从谈起
    try:
        if os.path.samefile(PORTS_BIND_SRC, PORTS_BIND_DST):
            return True                  # 已经是同一个目录 = 绑过了
    except OSError:
        pass                             # 目标不存在 —— 下面建一个

    try:
        os.makedirs(PORTS_BIND_DST, exist_ok=True)
    except OSError as e:
        log("ports 绑定：建不出 %s（%s）" % (PORTS_BIND_DST, e))
        return False

    cmd = ["mount", "--bind", PORTS_BIND_SRC, PORTS_BIND_DST]
    if os.geteuid() != 0:
        cmd = ["sudo", "-n"] + cmd
    try:
        r = subprocess.run(cmd, stdout=subprocess.DEVNULL,
                           stderr=subprocess.PIPE)
    except OSError as e:
        log("ports 绑定：mount 起不来（%s）" % e)
        return False
    if r.returncode != 0:
        log("ports 绑定：mount --bind 失败 rc=%d %s"
            % (r.returncode, r.stderr.decode("utf-8", "replace").strip()))
        return False

    log("ports 绑定：%s -> %s（卡二上的 port 数据现在挂在卡一路径上）"
        % (PORTS_BIND_SRC, PORTS_BIND_DST))
    return True


def do_launch(cfg, systems, pid, rel):
    """命令行入口：给原生前端（C + SDL2）用的"启动这个游戏"。

    这是网页版 `handle_launch()` 的等价物，**只换了请求的来源**：
    网页版是桥接 JS 发 `retroarch` 消息进来，原生版是 `--launch <pid> <相对路径>`。
    中间那一整套（PLATFORM_MAP 选系统 / STANDALONE_SYSTEMS 的 realpath 还原 /
    EasyRPG 条目修正 / 核心选择 / es_systems.cfg 的 command 模板替换）
    **一行都不重写**，直接调 build_launch_script。

    为什么不把 launch.sh 的拼装挪到 C 里：那不是"再写一遍"，是"再维护一遍"。
    这台机器上有 20 多个平台，其中 saturn/yabasanshiro 这种必须先把软链路径
    还原成真实路径才起得来（见 build_launch_script 的长注释），
    这种东西一旦两份实现就必然会漂。C 侧只负责把"用户点了哪个游戏"告诉 Python。

    返回值和退出码同义：0 = launch.sh 已写出，外层启动器可以执行了。
    """
    rel = (rel or "").lstrip("/")
    if not pid or not rel:
        log("--launch 参数不完整：pid=%r rel=%r" % (pid, rel))
        return 2

    # ports 是特例：脚本**自己**去 /roms/ports 找数据，不看我们给它的路径，
    # 所以启动前必须先把那条路径弄对（见下面 ensure_ports_bind 的长注释）。
    if pid.upper() == "PORTS":
        ensure_ports_bind()

    rom = os.path.join(cfg["compat_dir"], pid, rel)
    if not os.path.exists(rom):
        # 兼容树是开机时由 --gen-index 建的；走到这里只可能是
        # ROM 卡热插拔过、或者索引是上一张卡的。重建一次再试。
        log("启动：ROM 不在兼容树里（%s），重建兼容树后重试" % rom)
        try:
            prepare_rom_tree(cfg, systems)
        except Exception as e:
            log("重建兼容树失败: %s" % e)
        if not os.path.exists(rom):
            log("启动失败：ROM 仍然不存在 %s" % rom)
            return 4

    script = build_launch_script(cfg, systems, {"rom": rom}, pid)
    if not script:
        return 5
    if not write_request(cfg, script, "启动请求"):
        return 6
    return 0


# ============================================================================
#  手柄输入
# ============================================================================
#  前端 index.html 脚本末尾**本来就内建好了手柄入口**：
#      handlePad(btn, action)    # btn: u/d/l/r/a/b/x/y/start/lb|r1 …
#      window.vagaPad = function(btn){ handlePad(btn, 'down'); }
#  注释写的就是"供 Native 层调用"。所以这里最正确的做法是**直接调它**，
#  而不是去伪造键盘事件让前端自己解 —— 后者是绕远路。
#
#  为什么不用 LANGOS 自带的 gptokeyb（手柄→键盘 映射）：
#    1) 它的键名是**固定小集合**（实测 strings：esc / enter / backspace /
#       tab / space / 方向键 / pageup / pagedown / home / end / insert /
#       delete / shift / ctrl / alt / f1..），**没有字母键**。
#       而前端需要 y（搜索页）、s（设置页），映射不出来 → 功能残缺；
#    2) 它要造一个 uinput 虚拟键盘，还得靠 udev 热插拔才能被 weston 认到，
#       多两层时序不确定；而且 /dev/uinput 是 0600 root:root，必须 root 跑；
#    3) weston 的 libinput 明确把 "GO-Super Gamepad" 标成 joystick 并
#       **ignoring**（见 weston 日志），本来就不会把这台手柄递给客户端。
#  直接读 evdev → 调 JS 函数：零依赖、零时序问题、能表达任意按键。
#
#  这台机器（R36H ProMax，实测 /proc/bus/input/devices + EVIOCGBIT）：
#    event2 "GO-Super Gamepad"
#      EV_KEY : 304/305/307/308 = A/B/X/Y
#               310/311/312/313 = L1/R1/L2/R2
#               544/545/546/547 = BTN_DPAD_UP/DOWN/LEFT/RIGHT（数字十字键）
#               704..708        = BTN_TRIGGER_HAPPY1..5（Select/Start/Mode…）
#      EV_ABS : ABS_X/ABS_Y（左摇杆）、ABS_RX/ABS_RY（右摇杆）
#               —— 注意**没有 ABS_HAT0X/Y**，十字键是按键不是帽子开关，
#                  所以按 544..547 来判方向，别去找 HAT。
#    event3 "odroidgo3-keys"
#      EV_KEY : 只有 114/115 = VOLUMEDOWN/VOLUMEUP，与导航无关
#      （所以"方向键在 event3 上"是错的，别往那查）
# ============================================================================

EV_SYN, EV_KEY, EV_ABS = 0, 1, 3
EV_FMT = "llHHi"
EV_SZ = struct.calcsize(EV_FMT)          # 64 位上是 24

ABS_X, ABS_Y, ABS_RX, ABS_RY = 0, 1, 3, 4

# BTN_SOUTH/EAST/NORTH/WEST 就是厂商按 Xbox 布局标的 A/B/X/Y
PAD_BTN_NAME = {
    304: "a", 305: "b", 307: "x", 308: "y",
    310: "l1", 311: "r1", 312: "l2", 313: "r2",
}
# 数字十字键
PAD_DPAD = {544: "u", 545: "d", 546: "l", 547: "r"}
# BTN_TRIGGER_HAPPY1..5 的语义各厂不一。默认按最通行的排法，
# 具体哪个是 Select/Start 用 build/probe-input.py 按一遍就能确认，
# 也可以在 config.ini 的 pad_extra_map 里改。
PAD_HAPPY_DEFAULT = {704: "select", 705: "start", 706: "mode", 707: "l3", 708: "r3"}


def _eviocgabs(code):
    """EVIOCGABS(code) —— 取轴的取值范围，用来算死区。"""
    return (2 << 30) | (24 << 16) | (ord("E") << 8) | (0x40 + code)


def find_pad_device(hint="Gamepad"):
    """按名字找手柄的 event 节点。

    不能写死 /dev/input/event2 —— event 编号会随枚举顺序变（插拔过 USB、
    内核探测顺序不同都会挪）。sysfs 里的名字才是稳定的。
    """
    for path in sorted(glob.glob("/dev/input/event*")):
        base = os.path.basename(path)
        try:
            with open("/sys/class/input/%s/device/name" % base) as fh:
                name = fh.read().strip()
        except OSError:
            continue
        if hint.lower() in name.lower():
            return path, name
    return None, None


class PadReader:
    """读手柄 evdev → 调前端 window.vagaPad(btn)。

    长按自动重复是自己做的：这台手柄的 EV 能力里没有 EV_REP
    （实测 EV types = SYN/KEY/ABS/FF），内核不会替我们发重复事件，
    不自己补的话按住方向键只会走一格。
    """

    def __init__(self, glib, view, cfg):
        self.GLib = glib
        self.view = view
        self.path = ""
        self.fd = None
        self.held = {}          # src -> GLib source id
        self.axes = {ABS_X: 0, ABS_Y: 0, ABS_RX: 0, ABS_RY: 0}
        self.axis_btn = {}      # 轴名 -> 当前产生的 btn
        self.bounds = {}        # code -> (min, max)

        self.enabled = cfg.get("pad_enabled", "1") == "1"
        self.hint = cfg.get("pad_name", "Gamepad")
        self.dev_cfg = cfg.get("pad_device", "")
        self.analog = cfg.get("pad_analog", "1") == "1"
        self.dz_pct = int(cfg.get("pad_deadzone_pct", "40") or 40)
        self.repeat_delay = int(cfg.get("pad_repeat_delay", "420") or 420)
        self.repeat_rate = int(cfg.get("pad_repeat_rate", "160") or 160)

        self.buttons = dict(PAD_BTN_NAME)
        self.buttons.update(PAD_DPAD)
        self.buttons.update(PAD_HAPPY_DEFAULT)
        # config.ini: pad_extra_map=704:select,705:start
        for item in (cfg.get("pad_extra_map", "") or "").split(","):
            item = item.strip()
            if ":" not in item:
                continue
            code, btn = item.split(":", 1)
            try:
                self.buttons[int(code.strip())] = btn.strip()
            except ValueError:
                log("手柄: pad_extra_map 里 %r 不是数字，跳过" % code)

    # ------------------------------------------------------------ 生命周期
    def start(self):
        if not self.enabled:
            log("手柄: config 里 pad_enabled=0，跳过")
            return False

        path, name = (None, None)
        if self.dev_cfg and os.path.exists(self.dev_cfg):
            path, name = self.dev_cfg, "(config 指定)"
        else:
            path, name = find_pad_device(self.hint)

        if not path:
            log("手柄: 没找到名字含 %r 的输入设备（pad_device 也没指定），跳过" % self.hint)
            return False

        try:
            self.fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
        except OSError as e:
            log("手柄: 打不开 %s (%r)。宿主需 root 或在 input 组里，跳过" % (path, e))
            return False

        self.path = path
        self._read_bounds()
        log("手柄: 已接上 %s [%s]，模拟量=%s，按键表=%s"
            % (path, name, "开" if self.analog else "关",
               ",".join("%d->%s" % kv for kv in sorted(self.buttons.items()))))
        self.GLib.io_add_watch(
            self.fd,
            self.GLib.IO_IN | self.GLib.IO_ERR | self.GLib.IO_HUP,
            self._on_readable)
        return True

    def _read_bounds(self):
        """取各轴 min/max，死区按百分比算，避免写死 32767 在别的机器上不合适。"""
        import fcntl
        for code in (ABS_X, ABS_Y, ABS_RX, ABS_RY):
            try:
                buf = fcntl.ioctl(self.fd, _eviocgabs(code), b"\0" * 24)
                _v, mn, mx, _fz, _fl, _rs = struct.unpack("6i", buf)
                if mx > mn:
                    self.bounds[code] = (mn, mx)
            except OSError:
                pass
        log("手柄: 轴范围 %s" % (self.bounds or "未知（用 ±32767 兜底）"))

    def stop(self):
        for src in list(self.held):
            self._cancel(src)
        if self.fd is not None:
            try:
                os.close(self.fd)
            except OSError:
                pass
            self.fd = None

    # ---------------------------------------------------------------- 读
    def _on_readable(self, fd, _cond):
        try:
            data = os.read(fd, EV_SZ * 64)
        except (BlockingIOError, OSError):
            return True
        for off in range(0, len(data) - EV_SZ + 1, EV_SZ):
            try:
                _sec, _usec, typ, code, val = struct.unpack(
                    EV_FMT, data[off:off + EV_SZ])
            except struct.error:
                break
            if typ == EV_KEY:
                self._on_key(code, val)
            elif typ == EV_ABS and self.analog:
                self._on_abs(code, val)
        return True

    def _on_key(self, code, val):
        btn = self.buttons.get(code)
        if not btn:
            if val == 1:
                log("手柄: 未映射的按键 code=%d" % code)
            return
        src = "k%d" % code
        if val == 1:
            log("手柄: 按下 %s (code=%d)" % (btn, code))
            self._press(src, btn)
        elif val == 0:
            self._cancel(src)
        # val == 2 是内核自动重复；这台机器不会发，发了也忽略，自己重复更可控

    def _on_abs(self, code, val):
        # 守卫放这里，不放 _on_readable —— 否则换个调用入口就绕过去了。
        if not self.analog:
            return
        if code not in (ABS_X, ABS_Y, ABS_RX, ABS_RY):
            return
        self.axes[code] = val
        # 右摇杆不参与导航（前端也没有对应语义），只跟左摇杆
        self._axis_to_btn("L", ABS_X, ABS_Y)

    def _axis_to_btn(self, tag, cx, cy):
        # 死区按**半量程**算，不是全量程。
        # 这台机器的轴范围只有 ±1800（量程 3600），按全量程 40% 算出来的门限
        # 是 1440，等于要推到底 80% 才触发 —— 实测手感就是"摇杆没反应"。
        in_min, in_max = self.bounds.get(cx, (-32767, 32767))
        dz = (in_max - in_min) / 2.0 * self.dz_pct / 100.0
        center = (in_min + in_max) / 2.0
        ax = self.axes.get(cx, 0)
        ay = self.axes.get(cy, 0)
        dx, dy = ax - center, ay - center

        btn = None
        if abs(dx) > dz or abs(dy) > dz:
            if abs(dx) >= abs(dy):
                btn = "r" if dx > 0 else "l"
            else:
                btn = "d" if dy > 0 else "u"

        if btn == self.axis_btn.get(tag):
            return
        prev = self.axis_btn.pop(tag, None)
        if prev:
            self._cancel("a" + tag)
        if btn:
            self.axis_btn[tag] = btn
            self._press("a" + tag, btn)

    # ------------------------------------------------------------- 事件
    def _fire(self, btn):
        js = ("try{window.vagaPad&&window.vagaPad(%s);"
              "}catch(e){}" % json.dumps(btn))
        try:
            self.view.run_javascript(js, None, None, None)
        except Exception as e:
            log("手柄: 注入按键 %s 失败: %s" % (btn, e))

    def _press(self, src, btn):
        self._fire(btn)
        self._cancel(src)
        self.held[src] = self.GLib.timeout_add(
            self.repeat_delay, self._repeat_start, src, btn)

    def _repeat_start(self, src, btn):
        if src not in self.held:
            return False           # 期间已经松开了
        self._fire(btn)
        self.held[src] = self.GLib.timeout_add(
            self.repeat_rate, self._repeat_tick, src, btn)
        return False               # 让这个"首次延迟"定时器停掉，换成持续重复的

    def _repeat_tick(self, src, btn):
        if src not in self.held:
            return False
        self._fire(btn)
        return True

    def _cancel(self, src):
        tid = self.held.pop(src, None)
        if tid:
            try:
                self.GLib.source_remove(tid)
            except Exception:
                pass


def run_gtk(cfg, bridge_js):
    import gi
    gi.require_version("Gtk", "3.0")
    gi.require_version("WebKit2", "4.0")
    from gi.repository import Gtk, Gdk, WebKit2, GLib

    state = {"launch_script": None}

    class Shell:
        def __init__(self):
            self.win = Gtk.Window(type=Gtk.WindowType.TOPLEVEL)
            self.win.set_title("VagaRetro")
            self.win.set_default_size(1024, 768)
            self.win.set_decorated(False)

            # 黑底，避免页面加载期间闪白
            self.win.override_background_color(
                Gtk.StateFlags.NORMAL, Gdk.RGBA(0, 0, 0, 1))

            self.view = WebKit2.WebView()
            try:
                s = self.view.get_settings()
                # 封面图前端会用 file:// 直接加载，必须放开
                if hasattr(s, "set_allow_file_access_from_file_urls"):
                    s.set_allow_file_access_from_file_urls(True)
                if hasattr(s, "set_allow_universal_access_from_file_urls"):
                    s.set_allow_universal_access_from_file_urls(True)
                if hasattr(s, "set_enable_developer_extras"):
                    s.set_enable_developer_extras(cfg.get("debug") == "1")
                for setter in ("set_enable_webaudio", "set_enable_mediasource",
                               "set_enable_html5_database"):
                    if hasattr(s, setter):
                        getattr(s, setter)(True)
            except Exception as e:
                log("WebKit 设置调整失败（可忽略）: %s" % e)

            # 注入桥接（必须在页面脚本执行之前）
            try:
                ucm = self.view.get_user_content_manager()
                # ⚠ 顺序要紧：必须先 register_script_message_handler，再 add_script。
                # 反过来做的话网页里 `window.webkit.messageHandlers` 会是个**空对象**
                # （实测 Object.keys() 返回空数组），postMessage 全部静默丢失 ——
                # 表现就是"点游戏没反应"，而且一点报错都没有，极难查。
                ucm.register_script_message_handler("vagaretro")
                ucm.connect("script-message-received::vagaretro", self.on_bridge_msg)

                script = WebKit2.UserScript.new(
                    bridge_js,
                    WebKit2.UserContentInjectedFrames.TOP_FRAME,
                    WebKit2.UserScriptInjectionTime.START,
                    None, None)
                ucm.add_script(script)

                # console/报错转发（诊断用；也走 START，保证页面脚本之前就挂上）
                if cfg.get("web_log") == "1":
                    hook = WebKit2.UserScript.new(
                        CONSOLE_HOOK_JS,
                        WebKit2.UserContentInjectedFrames.TOP_FRAME,
                        WebKit2.UserScriptInjectionTime.START,
                        None, None)
                    ucm.add_script(hook)
                    log("桥接脚本已注册到 UserContentManager（含 console 转发）")
                else:
                    log("桥接脚本已注册到 UserContentManager")
            except Exception as e:
                log("!! 桥接脚本注册失败: %s" % e)

            self.view.connect("load-failed", self.on_load_failed)
            self.view.connect("load-changed", self.on_load_changed)

            self.win.add(self.view)
            self.win.connect("destroy", Gtk.main_quit)
            self.win.connect("key-press-event", self.on_key)

        # ---------------------------------------------------------- 回调
        def on_bridge_msg(self, ucm, res):
            """取 postMessage 的内容。

            踩过的坑：PyGObject **没有** `script_message_received__<name>` 这个
            便捷方法（那是 C 侧的宏名），照抄 WebKit C 文档会 AttributeError，
            而且这个异常被 try 吞掉后表现为"postMessage 完全没反应"—— 之前
            启动游戏/执行命令那条路就是这么静默失效的。
            正确做法：信号回调第二个参数本身就是结果对象，直接取它的 JSCValue。
            """
            try:
                # 老 API 给 WebKitJavascriptResult，新 API 直接给 JSCValue
                jsval = res.get_js_value() if hasattr(res, "get_js_value") else res
                if hasattr(jsval, "to_json"):
                    msg = jsval.to_json(0)
                elif hasattr(jsval, "to_string"):
                    msg = jsval.to_string()
                else:
                    msg = str(jsval)
                if isinstance(msg, str):
                    msg = json.loads(msg)
            except Exception as e:
                log("解析桥接消息失败: %s" % e)
                return

            typ = (msg or {}).get("type")

            # 网页 console / 报错（诊断用，不参与业务）
            if typ == "log":
                lv = str((msg or {}).get("level") or "LOG")
                txt = str((msg or {}).get("msg") or "")
                log("[web:%s] %s" % (lv, txt[:400]))
                return

            log("收到桥接消息: %s" % typ)
            if typ == "retroarch":
                self.handle_launch(msg)
            elif typ == "shell":
                self.handle_shell(msg)
            elif typ == "setting":
                self.handle_setting(msg)

        def handle_launch(self, msg):
            script = build_launch_script(cfg, SYS_CACHE["systems"],
                                         msg, self.platform_of_rom(msg.get("rom", "")))
            if not script:
                return
            if not write_request(cfg, script, "启动请求"):
                return
            # 退出界面，把屏幕让给 RetroArch
            GLib.timeout_add(200, Gtk.main_quit)

        def handle_shell(self, msg):
            cmd = (msg or {}).get("cmd", "")
            if not cmd:
                return
            script = "#!/bin/bash\n# 由前端 executeShellCommand 请求\n%s\n" % cmd
            if not write_request(cfg, script, "shell 请求"):
                return
            GLib.timeout_add(200, Gtk.main_quit)

        def handle_setting(self, msg):
            """前端设置页改了一项 —— 直接落盘，**不退出界面**。

            跟 shell/retroarch 那两条不一样：这里不写"请求脚本 + Gtk.main_quit"，
            因为改设置时用户还站在设置页上，退出界面等于把前端关掉。
            设置命令都是毫秒级的（写个 sysfs、跑一句 amixer），在 GTK 主线程里
            直接做完即可；真卡也就十几毫秒，比异步往返简单可靠得多。
            """
            key = str((msg or {}).get("key") or "")
            val = (msg or {}).get("value")
            if not key:
                return
            try:
                ok, note = apply_setting(key, val)
            except Exception as e:
                log("应用设置 %s 异常: %s" % (key, e))
                return
            log("设置 %s=%r -> %s（%s）" % (key, val, "OK" if ok else "失败", note))

        def platform_of_rom(self, rom):
            if not rom:
                return ""
            try:
                rel = os.path.relpath(rom, cfg["compat_dir"])
                return rel.split(os.sep)[0]
            except Exception:
                return ""

        def on_load_failed(self, view, event, uri, gerror):
            log("!! 页面加载失败: %s (%s)" % (uri, gerror))
            Gtk.main_quit()
            return True

        def on_load_changed(self, view, event):
            try:
                if event == WebKit2.LoadEvent.FINISHED:
                    log("页面加载完成: %s" % (view.get_uri() or ""))
                    # 真机上"全屏"只能靠数字确认：webview 里跑一段 JS 量视口，
                    # 再读 GTK 窗口/视件的分配尺寸。两者都 == 1024x768 才算铺满。
                    GLib.timeout_add(700, self.probe_geometry, 0)
                    # 封面/资源加载是异步的，晚一点再量一次帧率
                    if cfg.get("perf_probe") == "1" or cfg.get("probe_js_file"):
                        GLib.timeout_add(8000, self.probe_perf, 0)
            except Exception:
                pass

        def probe_perf(self, _x=0):
            # 自定义探测（文件里的一段 JS），返回值写进日志
            pf = cfg.get("probe_js_file")
            if pf and os.path.isfile(pf):
                try:
                    with open(pf, "r", encoding="utf-8", errors="replace") as fh:
                        js = fh.read()
                    log("跑自定义探测: %s" % pf)
                    self.view.run_javascript(js, None, self._on_probe_js, "自定义探测")
                    # 兜底取回通道：不依赖网页的 postMessage（那条通道本身可能
                    # 就是坏的，而这正是要诊断的东西）。探测脚本把结果写进
                    # document.title，Python 这边直接读标题 —— 单向、无依赖。
                    GLib.timeout_add(1500, self.read_title_probe, 0)
                except Exception as e:
                    log("自定义探测失败: %s" % e)
            if cfg.get("perf_probe") == "1":
                try:
                    self.view.run_javascript(PERF_JS, None, None, None)
                except Exception as e:
                    log("帧率探针启动失败: %s" % e)
            return False

        def read_title_probe(self, n=0):
            # 轮询几次：探测脚本可能要等页面渲染完（异步加载封面）才写结论
            try:
                log("页面标题回读[%d]: %s" % (n, self.view.get_title() or ""))
            except Exception as e:
                log("读页面标题失败: %s" % e)
            if n < 5:
                GLib.timeout_add(2500, self.read_title_probe, n + 1)
            return False

        def probe_geometry(self, tries=0):
            try:
                w, h = self.win.get_size()
                aw = self.view.get_allocated_width()
                ah = self.view.get_allocated_height()
                mon = []
                scr = self.win.get_screen()
                for i in range(scr.get_n_monitors()):
                    g = scr.get_monitor_geometry(i)
                    mon.append("%dx%d+%d+%d" % (g.width, g.height, g.x, g.y))
                log("几何: 窗口=%dx%d 视件=%dx%d 显示器=%s"
                    % (w, h, aw, ah, ",".join(mon) or "无"))
            except Exception as e:
                log("几何探测失败: %s" % e)

            js = ("(function(){var b=document.body,d=document.documentElement;"
                  "var c=document.getElementById('designContainer');"
                  "var p=document.querySelector('.padbar');"
                  "var rc=c?c.getBoundingClientRect():null;"
                  "var rp=p?p.getBoundingClientRect():null;"
                  "function f(r){return r?[Math.round(r.left),Math.round(r.top),"
                  "Math.round(r.width),Math.round(r.height)].join(','):'-';}"
                  "return [window.innerWidth,window.innerHeight,"
                  "(window.devicePixelRatio||1),"
                  "d.clientWidth,d.clientHeight,"
                  "f(rc),f(rp)"
                  "].join(' | ');})()")
            try:
                self.view.run_javascript(js, None, self._on_probe_js,
                                         "视口(W/H/dpr/docW/docH/画布/底栏)")
            except Exception as e:
                log("几何 JS 失败: %s" % e)

            if tries < 2:
                GLib.timeout_add(1500, self.probe_geometry, tries + 1)
            return False

        def _on_probe_js(self, view, res, _data):
            try:
                r = view.run_javascript_finish(res)
                v = r.get_js_value() if hasattr(r, "get_js_value") else r
                s = v.to_string() if hasattr(v, "to_string") else str(v)
                log("视口: W/H/dpr/docW/docH | 画布(l,t,w,h) | 底栏(l,t,w,h) = %s"
                    % s)
            except Exception as e:
                log("视口回读失败: %s" % e)

        def on_key(self, widget, event):
            # Escape 退出（调试用；正式环境由 config 里的 exit_key 控制）
            try:
                if event.keyval == Gdk.KEY_Escape and cfg.get("debug") == "1":
                    Gtk.main_quit()
            except Exception:
                pass
            return False

        def show(self, uri):
            self.win.show_all()
            full = cfg.get("fullscreen", "1") == "1"
            if full:
                self.win.fullscreen()
            log("加载页面: %s" % uri)
            self.view.load_uri(uri)

    SYS_CACHE["shell"] = Shell()
    SYS_CACHE["shell"].show("file://" + cfg["html"])

    # 手柄：直接读 evdev 调前端的 window.vagaPad()（前端自带这个入口，见 index.html）
    pad = PadReader(GLib, SYS_CACHE["shell"].view, cfg)
    pad.start()
    SYS_CACHE["pad"] = pad

    def on_sigterm(_s, _f):
        Gtk.main_quit()
    signal.signal(signal.SIGTERM, on_sigterm)

    Gtk.main()
    try:
        pad.stop()
    except Exception:
        pass
    log("GTK 主循环退出")
    return 0


SYS_CACHE = {"systems": {}, "shell": None}


# --------------------------------------------------------------------- main
def main():
    # 参数： [配置文件路径] [--gen-index] [--launch <平台id> <相对路径>]
    #   --gen-index = 只跑扫描 + 写索引就退出，不启 weston / GTK。
    #                 离线调试、以及"ROM 卡热插拔后手动刷索引"都用它。
    #   --launch    = 给原生前端（C + SDL2）用：把"用户点了这个游戏"翻译成
    #                 launch.sh 就退出。**不扫 ROM、不起 weston**，毫秒级。
    args = sys.argv[1:]
    gen_only = "--gen-index" in args

    launch_pid = launch_rel = None
    if "--launch" in args:
        i = args.index("--launch")
        rest = [a for a in args[i + 1:] if not a.startswith("--")]
        if len(rest) < 2:
            log("--launch 需要 <平台id> <相对路径> 两个参数")
            return 2
        launch_pid, launch_rel = rest[0], rest[1]
        # 把 --launch 及其两个参数从位置参数里摘掉，否则下面的 argv[0]
        # 会把平台 id（比如 "FC"）当成配置文件路径
        args = args[:i] + [a for a in args[i + 1:] if a.startswith("--")]

    argv = [a for a in args if not a.startswith("--")]
    cfg_path = argv[0] if argv else "/opt/vagaretro/config.ini"
    cfg = load_config(cfg_path)

    # --launch 不看页面：原生前端根本没有 index.html，而且"删掉网页版"
    # 正是这条路线要走到的地方 —— 不能让它反过来成为启动游戏的前提。
    if not gen_only and not launch_pid and not os.path.isfile(cfg["html"]):
        log("找不到页面: %s" % cfg["html"])
        return 3

    systems = parse_es_systems(cfg["essys"])
    SYS_CACHE["systems"] = systems

    # 启动请求这条路到此为止 —— 剩下的（建树 / 扫 ROM / 写索引 / 起 GTK）
    # 一样都不需要，纯属浪费。
    if launch_pid:
        return do_launch(cfg, systems, launch_pid, launch_rel)

    resolved = prepare_rom_tree(cfg, systems)

    dirs, texts = prescan(cfg["compat_dir"], resolved, systems)

    # 原生前端要吃的扁平索引。写失败不影响网页版运行，所以只记日志不中断。
    try:
        write_index(cfg, dirs, systems=systems)
    except Exception as e:
        log("写索引失败（不影响网页前端）: %s" % e)

    if gen_only:
        return 0

    bridge_js = build_bridge_js(cfg, dirs, texts)

    # 便于排查：把注入脚本落盘一份
    try:
        with open("/tmp/vagaretro-bridge.js", "w", encoding="utf-8") as fh:
            fh.write(bridge_js)
    except Exception:
        pass

    return run_gtk(cfg, bridge_js)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(0)
    except Exception:
        import traceback
        log("未捕获异常:\n%s" % traceback.format_exc())
        sys.exit(4)
