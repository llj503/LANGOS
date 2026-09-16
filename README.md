# LANGOS

> 给复古掌机做的**原生 C 前端**——用一份 C + SDL2 + KMSDRM 的程序顶掉 EmulationStation，
> 接管开机后的整个界面，但**把模拟器那一层原封不动地复用**。

LANGOS 不是一套从零写的模拟器固件，而是**换掉渲染层**的做法：
镜像里的 RetroArch、独立模拟器、`es_systems.cfg` 平台表、ROM 目录、存档、热键、手柄映射
全部沿用，只把"界面"换成自己写的。

这样做的好处很直接：不用移植任何模拟器，也不用维护 ROM 数据库；
而界面部分拿到 **59 fps**（原来网页版在 weston/WebKit 里只能 CPU 软渲染，卡到不能用）。

---

## 目录

- [这是什么](#这是什么)
- [跑在哪两台机器上](#跑在哪两台机器上)
- [仓库结构](#仓库结构)
- [核心设计](#核心设计)
- [编译](#编译)
- [部署到设备](#部署到设备)
- [按键](#按键)
- [平台与模拟器](#平台与模拟器)
- [已知限制](#已知限制)
- [这个仓库里没有什么](#这个仓库里没有什么)
- [上游与致谢](#上游与致谢)
- [许可](#许可)

---

## 这是什么

一台 ArkOS / dArkOS 系掌机开机后，正常流程是：

```
内核 → systemd → emulationstation.service → EmulationStation(ES) → 选游戏 → RetroArch
```

LANGOS 把中间那一段换掉：

```
内核 → systemd → emulationstation.service → vagaretro-front.sh
                                                   ↓
                                        /opt/vagaretro/native/vgfront   ← 自己写的原生前端
                                                   ↓  按下"启动游戏"
                                        自己退出、把 DRM 让出来
                                                   ↓
                                        /opt/vagaretro/launch.sh       ← 由前端生成的启动命令
                                                   ↓
                                        RetroArch / 独立模拟器（原封不动）
                                                   ↓  退出游戏
                                        前端重新被拉起
```

关键点：**`emulationstation.service` 这个单元我们不删也不改名**。
镜像里所有依赖它的东西——`After=emulationstation.service` 的 ogage、开机脚本、
厂商的升级包——都不受影响。我们只是让这个单元最终去跑 `vagaretro-front.sh`。

前端本体（`native/vgfront.c`）是一个 **单文件 C 程序**：
自己解析索引、自己画界面、自己读手柄、自己调系统接口（音量、亮度、WiFi、蓝牙、电池、
待机、关机），不依赖任何 GUI 工具包。

---

## 跑在哪两台机器上

| | **MINILOONG Pocket1** | **R36H ProMax（dArkOS 阶段）** |
| --- | --- | --- |
| SoC | RK3566 | RK3326 |
| 系统 | Debian 13 (trixie) | dArkOS4Clone（Armbian 系） |
| 屏幕 | 物理 720×960 竖屏，逻辑 960×720 | 640×480 |
| GPU / 驱动 | Mali-G52 + 现成 blob | Mali-G31 + `libMali.so`（r13p0） |
| 前端 | `vgfront`（SDL2 + KMSDRM + GLES2） | 同左 |
| 状态 | **当前主力开发目标** | 阶段 1/2 已闭环，见 `docs/历史-README-dArkOS阶段.md` |

两台的渲染路径其实是同一套：**SDL2 的 KMSDRM 后端直接占 DRM，走 gbm winsys，绕开 wayland**。
这也是当初决定"不折腾网页前端"的原因——详见 `docs/原生前端方案.md`。

---

## 仓库结构

```
.
├── native/                     原生前端（主战场）
│   ├── vgfront.c               ★ 前端本体，单文件 C，约 1.4 万行
│   ├── dev-env.sh              在设备上补齐 SDL2 / libc 头文件（不装包、不动系统）
│   └── build-vgfront.sh        一键编译
│
├── boot/                       第二屏引导器
│   ├── vgboot.c                开机 logo / 启动画面
│   ├── vgboot.ini              配置（图片路径、停留时间、淡出）
│   └── build-vgboot.sh         编译脚本
│
├── system/                     要落到设备上的系统文件
│   ├── usr/local/bin/
│   │   ├── vagaretro-front.sh  ★ 前端外壳：systemd 入口、启动循环、卡二挂载、关机
│   │   ├── drastic.sh          原版 DraStic 包装（修了 ROM 路径推导的坑）
│   │   └── drastic_adv-sa.sh   高级版 DraStic 包装（左右并排双屏布局）
│   └── opt/vagaretro/
│       ├── vagaretro-shell.py  ★ 平台映射 + 生成 launch.sh 启动命令
│       ├── vg_platforms.py     平台清单（名称 / 分类 / 默认核心 / 配色）
│       ├── index.html          网页版界面（历史资产，也是 vgfront 的样式参考）
│       ├── config.ini          ROM 目录、卡二设备名、渲染器、主题
│       ├── set-hotkeys.sh      写 RA 热键
│       └── bezels/windows.tsv  RA 边框视图坐标表（1024×768 画布）
│
├── frontend/
│   ├── index_v2.html           网页前端（当初的原型，留着做对照）
│   └── logo.png
│
├── prebuilt/
│   └── rk3566/vgfront          现役预编译二进制（不想装工具链可以直接用）
│
└── docs/
    ├── 原生前端方案.md          ★ 为什么原生、架构怎么切、渲染设计
    ├── 真机启动链路与平台映射.md  ★ 从按 A 到游戏起来的完整链路 + 三个必踩的坑
    ├── 真机渲染栈落地实录.md      GPU 不是现成的：libMali / libgbm / weston 踩坑全过程
    ├── 阶段2-浏览器引擎选型.md    为什么最后没用 WebKit / Cog / Chromium
    ├── 快捷键说明.md             ★ 完整按键表（前端的 / RA 的 / 系统的）
    ├── 快捷键说明.html           同一份的网页版（带手柄示意图）
    └── 历史-README-dArkOS阶段.md  RK3326 阶段的完整 README（含镜像注入流程）
```

---

## 核心设计

### 1. 只换渲染层，后端全复用

前端**不碰任何 `input_*` 热键配置**，也不自己实现模拟器。
它做的事只有两件：把界面画出来；把"要启动哪个 ROM、用哪个核心、带什么参数"写成一个
`launch.sh`，然后自己退出，让外层 `vagaretro-front.sh` 去执行。

平台名、核心候选、ROM 目录这些都从镜像自带的 `es_systems.cfg` 里读
（`vagaretro-shell.py` 负责解析，包括处理重名系统）。

### 2. 尺寸策略：把 CSS 设计值照抄进 C

网页版是按 1920×1080 设计、再整体缩放到屏幕的。
`vgfront.c` 里把**设计值原样**写成常量，用 `S()` 宏做同一套缩放——
于是对着 `index.html` 改数值时是 1:1 对应的，不用心算。

> 改样式请直接翻 `frontend/index_v2.html` 的 `<style>`，别凭感觉调数。

### 3. 启动链路是"反方向同一条通道"

```
vgfront  →  python3 vagaretro-shell.py --launch <平台> <ROM>
              ↓  写出 /opt/vagaretro/launch.sh
           自己 exit(0)，把 KMS 让出来
              ↓
vagaretro-front.sh 看到 launch.sh 存在 → bash 它 → 跑完删掉 → 再把 vgfront 拉起来
```

所以前端里**没有一行启动逻辑**，全在 `vagaretro-shell.py` 里。
要多加一个独立模拟器，改那个文件的映射表就行。

### 4. 索引与媒体

`vagaretro-shell.py --gen-index` 扫 ROM 目录生成 `/opt/vagaretro/index.tsv`
（一行一个游戏，`V/M/P/C` 四种行），前端只读这个文件，启动时不需要遍历文件系统。
卡二（TF2）会挂到 `/roms2` 并**优先**当 ROM 根。

---

## 编译

**在设备上编**（RK3566 上没有交叉工具链，但设备自带 `gcc`）：

```bash
# 1) 先补头文件（幂等；用 apt-get download 只下 .deb，不装、不改系统）
bash native/dev-env.sh

# 2) 编
source native/dev-env.sh
gcc $VG_CFLAGS -O2 -Wall -o vgfront native/vgfront.c $VG_LDFLAGS -lSDL2 -lSDL2_ttf -lm
```

实测 RK3566 上编一轮 **30~36 秒**。

> ⚠️ 编译前记得删掉旧产物并 grep 一遍 `error:`——
> gcc 失败时**不会动旧二进制**，光看"文件在不在"会把上一版当成这次成功。

`vgboot` 用 `boot/build-vgboot.sh` 编。

---

## 部署到设备

前提：设备能 SSH，且 `ark` 用户有免密 sudo（出厂镜像里 `/etc/sudoers.d/ark-no-sudo-password` 已经给了）。

```bash
# 停前端（KMSDRM 需要独占 DRM master，会和 weston / 前端抢屏幕）
sudo systemctl stop emulationstation

# 拷二进制 / 脚本
sudo cp vgfront /opt/vagaretro/native/vgfront
sudo chown ark:ark /opt/vagaretro/native/vgfront

# 跑一下看看
SDL_VIDEODRIVER=kmsdrm SDL_VIDEO_EGL_DRIVER=libEGL.so /opt/vagaretro/native/vgfront /opt/vagaretro/index.tsv

# 回到正常开机流程
sudo systemctl start emulationstation
```

改前端**不需要重刷 TF 卡**，这也是整个项目能快速迭代的原因。

> ⚠️ 两件必须知道的事：
> 1. **只用 `systemctl stop/start emulationstation`**，不要自己 `nohup` 起前端——
>    抢 DRM 会闪屏。
> 2. 前端**只能以固定身份跑**。以 root 跑会生成 `root:root` 的文件（日志、解包缓存），
>    之后前端以 `ark` 身份跑就写不进去，表现为"点了游戏没反应"。
>    历史上真踩过这个坑：`/tmp/drastic_adv-sa.log` 与 `unzip_cache/` 被 root 抢先建过。

---

## 按键

完整表见 [`docs/快捷键说明.md`](docs/快捷键说明.md)，机器上也能看：
`设置 → 快捷键说明`（纯 C 画的可滚动页面，随系统语言切中英）。

三层互不干扰：**前端界面** / **游戏里（RetroArch）** / **系统级（ogage 全局热键）**。

前端最常用的几个：

| 位置 | 按键 | 作用 |
| --- | --- | --- |
| 主页 | A | 进入该平台 |
| 主页 | Y | 打开搜索 |
| 主页 | START | 打开设置页 |
| 游戏列表 | A | 打开操作弹层（启动 / 收藏 / 换核心 / 着色器 / 读档 / 自动存档 / 删除） |
| 游戏列表 | B | 回主页 |

游戏里 **所有组合键都要先按住 M 键**（机身中间那个，出厂门禁 `input_enable_hotkey_btn = "10"`）：

| 组合 | 作用 |
| --- | --- |
| M + START | 退出游戏，回到前端 |
| M + A | 暂停 / 继续 |
| M + X | RA 菜单 |
| M + R1 / L1 | 存档 / 读档 |
| M + L2 / R2 | 快进（按住 / 锁住） |
| M + 十字键上 / 下 | 亮度 ± |

---

## 平台与模拟器

前端支持 **32 个平台**，索引里 8000+ 个游戏。
核心与独立模拟器的选择逻辑全在 `system/opt/vagaretro/vg_platforms.py` +
`vagaretro-shell.py` 的 `FORCE_EMULATOR` / `STANDALONE_DIRECT` 两张表里。

独立模拟器（不走 RetroArch）目前接了：

| 平台 | 模拟器 | 说明 |
| --- | --- | --- |
| NDS | advanced_DraStic（trngaje 版） | 自带布局系统，默认**左右并排双屏**，各 384×288 |
| DC / SS | flycast / yabasanshiro（standalone） | 见 `docs/真机启动链路与平台映射.md` |
| J2ME | freej2me-plus（RA 核心）+ 系统 OpenJDK 21 | 2026-09 接入 |
| N64 / PSP | 对应的 standalone | 同上 |

---

## 已知限制

- **前端是启动器，不是模拟器**：游戏里前端是退出的，所以前端的所有按键在游戏中都按不出来，
  这是设计如此。**唯一可靠的退出口是 M + START**。
- **"保存存档"不在前端**：前端不在游戏现场，存不了档。存档请用 M + R1（RA 自己存）。
  前端弹层里的"加载存档"改的是**下次启动时读哪一位**。
- NDS 高级版的自动存档提示弹窗（hook 层新增）目前压不掉，**自动存档必须开**，所以它会弹。
- 息屏后屏幕可能被 `irq77 rk817` 唤醒（未修）。
- 封面覆盖率约 26%（2205 / 8445），封面目录还是空的。
- `perfnorm` 退出时固定有约 1.5 秒等待（硬编码）。
- 换平台的核心后，弹层项数变化要同步改高度算法——改 UI 一定**看截图**验收。

---

## 这个仓库里没有什么

为了不惹麻烦，以下内容**不在仓库里**，请自备：

- **ROM / 游戏**、**BIOS**、**封面与边框美术素材**（`bezels/*.png`）——版权原因。
  边框坐标表 `windows.tsv` 是自测数据，留着了，图自己生成或替换即可。
- **任何第三方模拟器或核心的二进制**（RetroArch、DraStic、flycast…）。
- **`ref/` 目录**：早期从 `lcdyk0517/arkos4clone` 等上游仓库取的参考文件，
  属于别人的代码，公开仓库里不收录。
- 设备 IP、密码、SSH 密钥等一切凭据（原开发脚本里的都已脱敏）。

---

## 上游与致谢

这个项目是站在一群人肩膀上的，特别感谢：

- [**christianhaitian/dArkOS**](https://github.com/christianhaitian/dArkOS) —— ArkOS / dArkOS 固件
- [**lcdyk0517/arkos4clone**](https://github.com/lcdyk0517/arkos4clone) —— 镜像构建脚本与体系
- [**libretro / RetroArch**](https://www.retroarch.com/) —— 实际跑游戏的那一层
- [**Simple DirectMedia Layer (SDL2)**](https://libsdl.org/) —— `vgfront` 的窗口 / 输入 / 渲染底座
- [**EmulationStation**](https://emulationstation.org/) —— 被替换掉的那一层，也是后端逻辑的来源

前端在 RK3566 上的渲染路径参考了 ROCKNIX / Batocera 系对 Mali 老 blob 的处理方式。

---

## 许可

**GPL-3.0**。见 [LICENSE](LICENSE)。

选它是因为整个项目是**建立在 GPL 生态之上**的（上表中的固件、前端、RetroArch 多为 GPL），
沿用同一许可最不容易出问题。如果你希望放宽，把 LICENSE 换成 MIT 并改这一节即可。

---

*README 最后一次核对：2026-09-16。改过 `vagaretro-shell.py` 的映射表、
`vgfront.c` 的按键分派或 RA 热键之后，请回来同步 [按键](#按键) 那一节。*
