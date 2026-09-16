# -*- coding: utf-8 -*-
"""平台元数据表 —— 原生前端索引用它，是这一层数据的唯一真相源。

======================================================================
 和 index.html 的关系
======================================================================
index.html 里有一份等价的 `const platforms = [...]`（网页版保底仍在用）。
**两份必须保持一致**，改一份就要同步另一份，校验：

    python tools/check-platforms-sync.py

它会逐字段比对，不一致直接报错退出。

======================================================================
 为什么颜色写死在 Python 里、而不是继续沿用 CSS class 名
======================================================================
原生前端是 SDL2 自绘，没有 CSS，class 名在那里没有意义。
这里直接存**渐变起止色**，索引里写成 `#RRGGBB,#RRGGBB`，
C 侧拿到就能画 —— 不用查表、不用解析 class、零依赖。

对应关系：`c1/c2` 就是 CSS 里 `linear-gradient(135deg, c1 0%, c2 100%)`。
方向固定 135°（左上 -> 右下），C 侧按这个方向做顶点插值。

======================================================================
 关于"补色"
======================================================================
下面这 9 个 class 在网页版 CSS 里**压根没定义**
（color-nds / color-dos / color-pce / color-ngpc / color-ws / color-wsc /
color-gg / color-naomi，以及被 color-dos 牵连的 EASYRPG）——
platform-art 因此没有背景、只剩一块透明，卡片上部是空的。

这和当初顶栏那个多余的"小 L"是同一个病根：**引用了不存在的 CSS 类**。
（当时写的 build/check-classes.js 只扫静态 HTML，扫不到这里由 JS 生成的 card，
所以漏了。）这里按机种气质统一补齐，网页版 CSS 也一并补上，两边对齐。
"""

# (pid, cat, name, sub, core, logo, tag, c1, c2)
PLATFORM_META = [
    # pid          cat         显示名                    sub         core                  logo    tag       渐变起                渐变止
    ("history",    "all",      "游玩历史",               "RECENT",   "最近 30 款",          "历史",  "最近游玩", "#667eea", "#764ba2"),
    ("favorite",   "all",      "我的收藏",               "FAVORITES", "收藏置顶",           "收藏",  "X 键收藏", "#f6d365", "#fda085"),

    ("FC",         "nintendo", "FC / NES 红白机",        "任天堂",   "nestopia",            "FC",    "任天堂",   "#e74c3c", "#c0392b"),
    ("SFC",        "nintendo", "SFC / SNES 超任",        "任天堂",   "snes9x",              "SFC",   "任天堂",   "#9b59b6", "#8e44ad"),
    ("GB",         "handheld", "Game Boy",               "任天堂",   "gambatte",            "GB",    "任天堂",   "#27ae60", "#1e8449"),
    ("GBC",        "handheld", "Game Boy Color",         "任天堂",   "gambatte",            "GBC",   "任天堂",   "#1abc9c", "#16a085"),
    ("GBA",        "nintendo", "Game Boy Advance",       "任天堂",   "mgba",                "GBA",   "任天堂",   "#2980b9", "#1f618d"),
    # ★ NDS（2026-09-16 换高级版）—— core 名 = 包装脚本 /usr/local/bin/drastic_adv-sa.sh
    #   实际跑的是 advanced_drastic（trngaje 版），它靠 resources/bg/1024x768/layout.json
    #   做「左右并排双屏」（NDS 上下屏各占半个屏宽）。原版 DraStic 没有这个能力。
    #   core 写 drastic_adv-sa 而不是 drastic，是为了在「当前核心」里能区分出高级版。
    ("NDS",        "nintendo", "Nintendo DS",            "任天堂",   "drastic_adv-sa",      "NDS",   "任天堂",   "#cfd8dc", "#90a4ae"),
    # ★ J2ME（2026-09-16 新增）—— Java 手机游戏。
    #   目录：/roms2/j2me（卡二），compat 名 J2ME
    #   启动：es_systems.cfg 的 j2me 段 -> /usr/local/bin/freej2me.sh
    #         -> retroarch -L cores/freej2me-plus_libretro.so（用系统 OpenJDK 21）
    #   core 字段写 freej2me-plus 是为了前端显示"当前核心"时有个确切名字。
    ("J2ME",       "other",    "J2ME 手机游戏",          "Java",     "freej2me-plus",       "J2ME",  "Java",     "#f39c12", "#d68910"),
    ("MD",         "sega",     "Mega Drive",             "世嘉",     "genesis_plus_gx",     "MD",    "世嘉",     "#e67e22", "#d35400"),
    ("SS",         "sega",     "Saturn",                 "世嘉",     "yabause",             "SS",    "世嘉",     "#1abc9c", "#16a085"),
    ("DC",         "sega",     "Dreamcast",              "世嘉",     "flycast",             "DC",    "世嘉",     "#ecf0f1", "#bdc3c7"),
    ("PS1",        "sony",     "PlayStation",            "索尼",     "pcsx_rearmed",        "PS",    "索尼",     "#34495e", "#2c3e50"),
    ("PSP",        "sony",     "PlayStation Portable",   "索尼",     "ppsspp",              "PSP",   "索尼",     "#1a1a2e", "#16213e"),
    ("DOS",        "computer", "DOS",                    "电脑",     "dosbox",              "DOS",   "电脑",     "#4a6fa5", "#2b4a72"),
    ("PCE",        "other",    "PC Engine",              "NEC",      "mednafen_pce",        "PCE",   "NEC",      "#ef6c00", "#b34700"),
    ("NGPC",       "handheld", "Neo Geo Pocket Color",   "SNK",      "mednafen_ngp",        "NGPC",  "SNK",      "#455a64", "#263238"),
    ("WS",         "handheld", "WonderSwan",             "万代",     "mednafen_wswan",      "WS",    "万代",     "#546e7a", "#37474f"),
    ("WSC",        "handheld", "WonderSwan Color",       "万代",     "mednafen_wswan",      "WSC",   "万代",     "#00838f", "#006064"),
    ("GG",         "handheld", "Game Gear",              "世嘉",     "genesis_plus_gx",     "GG",    "世嘉",     "#6d4c41", "#3e2723"),
    ("NAOMI",      "arcade",   "NAOMI",                  "街机",     "flycast",             "NAOMI", "街机",     "#ad1457", "#6a1b3d"),

    ("N64",        "nintendo", "Nintendo 64",            "任天堂",   "mupen64plus",         "N64",   "任天堂",   "#2ecc71", "#27ae60"),
    ("GW",         "handheld", "Game & Watch",           "任天堂",   "gw",                  "G&W",   "任天堂",   "#fdcb6e", "#e17055"),
    ("MS",         "sega",     "Master System",          "世嘉",     "genesis_plus_gx",     "MS",    "世嘉",     "#e67e22", "#d35400"),
    ("ARCADE",     "arcade",   "街机 FBNeo",             "街机",     "fbneo",               "ARC",   "街机",     "#e74c3c", "#922b21"),
    ("OPENBOR",    "arcade",   "OpenBOR 同人格斗",       "同人",     "OpenBor",             "OBR",   "同人",     "#8e44ad", "#5b2c6f"),
    ("ATARI2600",  "other",    "Atari 2600",             "雅达利",   "stella",              "2600",  "雅达利",   "#d35400", "#87421f"),
    ("ATARI7800",  "other",    "Atari 7800",             "雅达利",   "prosystem",           "7800",  "雅达利",   "#d35400", "#87421f"),
    ("LYNX",       "handheld", "Atari Lynx",             "雅达利",   "handy",               "LYNX",  "雅达利",   "#d35400", "#87421f"),
    ("PICO8",      "other",    "PICO-8 幻想主机",        "Fantasy",  "pico8",               "P8",    "幻想主机", "#e84393", "#c2185b"),
    ("PORTS",      "computer", "移植游戏 Ports",         "电脑",     "SDL",                 "PORT",  "电脑",     "#00b894", "#00795f"),
    ("EASYRPG",    "computer", "RPG Maker / EasyRPG",    "电脑",     "easyrpg",             "RPG",   "电脑",     "#5c6bc0", "#3949ab"),
]

# PLATFORM_META 元组里前 7 个字段的名字（第 8、9 个是渐变的 c1/c2，单独处理）
P_FIELDS = ("pid", "cat", "name", "sub", "core", "logo", "tag")

# 分类页签：id -> 显示名（顺序即界面顺序）
CATEGORIES = [
    ("all",      "全部"),
    ("nintendo", "任天堂"),
    ("sega",     "世嘉"),
    ("sony",     "索尼"),
    ("arcade",   "街机"),
    ("handheld", "掌机"),
    ("computer", "电脑"),
    ("other",    "其他"),
]

# 虚拟平台：没有真实 ROM 目录，count 由宿主另算
VIRTUAL_PIDS = ("history", "favorite")


def by_pid():
    """pid -> dict，方便查。color 是 (c1, c2) 二元组。"""
    out = {}
    for m in PLATFORM_META:
        d = dict(zip(P_FIELDS, m[:len(P_FIELDS)]))
        d["color"] = (m[len(P_FIELDS)], m[len(P_FIELDS) + 1])
        out[d["pid"]] = d
    return out


def color_pair(meta):
    """把 c1/c2 拼成索引里那个 `#RRGGBB,#RRGGBB` 字段。"""
    return "%s,%s" % (meta["color"][0], meta["color"][1])
