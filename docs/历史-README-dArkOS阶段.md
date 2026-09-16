# dArkOS WebFront —— 把 VagaRetro 网页前端编译进 dArkOS，替换 ES 前端

把 `VagaRetro` 的 CSS/HTML 前端（`index_v2.html`）注入 **dArkOS4Clone** 镜像，
让机器开机不再进原版 EmulationStation（ES），而是走自己的网页前端。

> **当前进度：两个阶段都已在真机闭环 ✅✅**
>
> 阶段 1（启动钩子 + 前端部署 + 兜底回退 + 一键回滚）：
> 真 Linux 上跑通 **47 项端到端断言**，真镜像注入后 **23 项核验全过**，
> 已刷进 TF 卡实测：开机出现 3 秒回执横幅 → 自动回到 ES，钩子生效、兜底正常。
>
> 阶段 2（网页真顶掉 ES + 游戏走镜像里 Linux 原生的 RetroArch）：
> 宿主逻辑 **30 项单测**、游戏循环 **15 项**、真 cfg + 真 cores **干跑 29 个平台 29/29**、
> 注入核验 **23 项**，全过。
> **真机实测（R36H ProMax，2026-09-14）：网页前端已经真的画在屏幕上了 —— ES 被顶掉。**
> 合成器已从 `--use-pixman`（CPU 软渲染）**翻到 GPU(GL)**：
> 真正的拦路石是 `libgbm-shim.so` 的 `DT_NEEDED: libMali.so` 会把老 blob 拉进来，
> 归一到 r13p0 那个 blob 即可，实测 `GL renderer: Mali-G31`、首启一次成功。
> 一路踩过的真机坑见 [`docs/真机渲染栈落地实录.md`](docs/真机渲染栈落地实录.md)。
>
> **游戏启动已端到端真机跑通**（2026-09-14）：
> 前端 `launchGame` → 桥接 → 写 `launch.sh` → 宿主退出让出 DRM →
> RetroArch 起来（日志出现 `librga:RGA_GET_VERSION`）→ 退出后前端自动回来。
> 过程中修掉两个"必然起不来"的 bug：`%CORE%` 跨 emulator 合并（DC/SS/NAOMI）、
> 以及包装脚本用 ROM 路径第 2 段当媒体根导致 BIOS 找不到 —— 详见
> [`docs/真机启动链路与平台映射.md`](docs/真机启动链路与平台映射.md)。
>
> 镜像的机型/DTB 已写死为 **R36H ProMax**（见 3.5）。
> 真机启动链路、平台名映射，见 [`docs/真机启动链路与平台映射.md`](docs/真机启动链路与平台映射.md)。
>
> **设备可以 SSH**（`ark`/`ark`，`sudo -n` 免密）→ 改前端**不用重刷 TF 卡**，
> 用 `tools/deploy-device.sh <ip>` 直接推文件 + 重启服务，迭代快得多。

---

## 1. 为什么是这么做的（关键事实）

这些是读 `lcdyk0517/arkos4clone` 和 `christianhaitian/dArkOS` 源码确认的，不是猜的：

| 事实 | 出处 |
|---|---|
| ES 由 systemd 服务拉起：`ExecStart=/usr/bin/emulationstation/emulationstation.sh`，`User=ark`，`Restart=on-failure` | `dArkOS/Emulationstation/emulationstation.service` |
| 该脚本内层是个循环，反复执行同目录的 `emulationstation` 二进制；退出后按 `/tmp/es-restart`、`/tmp/es-sysrestart`、`/tmp/es-shutdown` 决定重启/重启系统/关机 | `emulationstation.sh.g350` 第 130-150 行 |
| 开机按住 B 进 BaRT「Boot and Recovery Tools」恢复菜单（菜单项 1 也是调这个脚本） | 同上，第 60-125 行 |
| **没有 X11 / Wayland**：ES 靠 `SDL_VIDEO_EGL_DRIVER=libEGL.so` 直接跑 DRM/EGL 全屏 | `emulationstation.service` |
| 镜像分区：`p1`=FAT32 boot，`p2`=ext4/btrfs rootfs，`p3`=exFAT roms | `mount_arkos.sh` |
| 构建流程：下载 dArkOS G350 基础镜像 → `build_image.sh`（扩容 / 写 U-Boot / 挂载 / 跑 `clone_support.sh` 注入 `replace_file/`、`consoles/`、`mod_so/` / 卸载 / `xz` 压缩） | `.github/workflows/build-darkos4clone.yml`、`build_image.sh` |
| **U-Boot 的 `bootcmd` 是 `mmc dev 1; cfgload; run setbootargs; load mmc 1:1 0x02000000 Image; load mmc 1:1 0x01f00000 ${dtb_name}; booti ...`** —— `cfgload` 去读 p1 上的 `boot.ini`，从中拿到 `${dtb_name}`。所以**镜像必须有 boot.ini + 对应 .dtb + Image 才能开机** | uboot 区字符串（`build_image.sh` 写入的 u-boot） |
| **构建产出的镜像里故意没有** `boot.ini` / `*.dtb` / `Image`（`clone_support.sh` 会 `rm -rf $MOUNT_DIR/boot/boot.ini $MOUNT_DIR/boot/*.dtb ... `），并要求用户先在 PC 上跑 `dtb_selector_win32.exe` 交互选机型 | `clone_support.sh` 第 353、774 行 |
| 机型包在 p1 的 `consoles/<RealName>/` 里（92 个目录），每个含 `boot.ini` + `rk3326-<RealName>-linux.dtb`；内核在 `consoles/kernel/{original,arkos4clone_fix}/Image`（**两个文件内容不同**）；启动图在 `consoles/logo/<分辨率>/logo.bmp` | p1 `consoles/` 实测 |
| 首启链路：`firstboot.service` → `/boot/firstboot.sh` → `/boot/expandtoexfat.sh`（扩 p2/p3、把 p3 转 exFAT、铺 ROM、写 `/etc/fstab`）→ 调 `/boot/clone.sh`（把 clone.sh 覆盖到 firstboot.sh 后自删）；**`.cn` / `.ko` 标记就是在这里被消费的**（应用中文/韩文 + 时区） | p1 `expandtoexfat.sh`、`clone.sh` 实测 |
| `USE_DTB_SELECT_TO_SELECT_DEVICE` **不被 uboot、也不被 rootfs 里任何东西读取**——它只是个给用户看的提示标记（实测 `grep` 全盘 + `strings` 选择器二进制均为空） | 实测 |

**注入点的选择**：不去替换 ES 那个 5.5MB 的二进制（那样原版就没了，兜底无从谈起），
而是只改 `emulationstation.sh` 内层循环里的**一行**：

```diff
-        "$esdir/emulationstation" "$@"
+        /usr/local/bin/vagaretro-front.sh "$esdir/emulationstation" "$@"
```

好处是：不管 ES 是被 systemd、BaRT 菜单还是别的方式拉起，都会经过我们的启动器；
原版二进制原封不动躺在原处，随时可以兜底。

---

## 2. 目录结构

```
D:\Projects\dArkOS-WebFront\
├── README.md                          本文件
├── frontend\                          前端源码留档
│   ├── index_v2.html                  VagaRetro 前端（源头，未改动）
│   └── logo.png
├── payload\                           要注入进镜像的文件（目录结构 = 镜像内路径）
│   ├── opt\vagaretro\
│   │   ├── index.html                 前端页面（= frontend/index_v2.html）
│   │   ├── config.ini                 开关 + 渲染器 + 宿主参数（enabled=0 一键回退）
│   │   ├── vagaretro-shell.py         **阶段2 核心**：GTK3/WebKit2 全屏宿主 + AndroidBridge 桥接
│   │   └── bridge-linux.js            AndroidBridge 占位实现（默认不引用）
│   └── usr\local\bin\
│       └── vagaretro-front.sh         启动器（渲染器探测 + ES 兜底 + 游戏循环 + 自动熔断）
├── tools\
│   ├── prepare-image.sh               解压 + 只读探明镜像结构（.img / .xz / .7z 自动识别）
│   ├── bake-console.sh                **预置机型/DTB/内核/启动图到 p1**（非交互版选择器）
│   ├── inject-frontend.sh             挂载镜像 → 注入 → 卸载
│   ├── install-browser.sh             **在设备上装渲染栈**（weston + WebKitGTK，约 104 MB）
│   └── restore-es.sh                  一键还原原版 ES
├── tests\
│   ├── test-shell-logic.py            宿主纯逻辑离线单测（30 项，不需要 Linux）
│   ├── real-linux-e2e.sh              真实 Linux 端到端自测（真 losetup / 真 mount，47 项断言）
│   ├── verify-injection.sh            对【注入后】的镜像做只读核验（payload 全量 md5，23 项）
│   ├── verify-console.sh              对【预置机型后】的 p1 做只读核验（21 项断言）
│   ├── launcher-loop-sim.sh           **游戏循环模拟**（假 weston + 假宿主，15 项断言）
│   └── real-launch-dryrun.py          真 cfg + 真 cores 干跑 18 个平台（不执行任何命令）
├── docs\
│   ├── 阶段2-浏览器引擎选型.md         阶段2 的技术方案与取舍
│   └── 真机启动链路与平台映射.md        **真机实测**：占位符语义、平台映射、三个坑
└── ref\                               从上游仓库拉下来的参考文件（只读，供比对）
```

---

## 3. 操作步骤

### 3.1 用 WSL2 就行（不需要另装虚拟机）

> **勘误**：早先以为本机 `wsl.exe` 被安全策略拉黑、只能靠 VMware。
> 实测 **WSL2 完全可用**，而且内核支持 loop 设备、分区扫描和 ext4 挂载 ——
> 也就是说在 Windows 上就能直接处理磁盘镜像。
> **已在真实环境跑通 47 项端到端断言。**

实测环境：

| 项 | 实测值 |
|---|---|
| 发行版 / 内核 | Ubuntu (WSL2) / `6.18.33.2-microsoft-standard-WSL2` |
| loop 设备 | `/dev/loop-control` + `/dev/loop0..7`；`losetup -fP` 后 `loopNp1/p2/p3` 节点**自动出现** |
| ext4 挂载 | 正常 |
| 可用空间 | WSL 根文件系统 948 GB；`D:` 盘 168 GB |

**两个必须注意的坑**

1. **大文件一律放 `/mnt/d/...`（D 盘），别放 WSL 根目录。**
   WSL 的虚拟磁盘在 `C:\Users\...\AppData\Local\wsl\{...}\ext4.vhdx`，
   而本机 **C 盘只剩约 5.7 GB**。往 `/root`、`/tmp`、`/home` 里灌几 GB 会直接撑爆 C 盘。
2. **从 Windows 这边调 WSL 时，不要用 `wsl.exe ... bash -c "带 $ 的脚本"`。**
   实测 `$` 会在参数传递过程中被吃掉、变量变成空值。
   正确做法是把脚本写成文件，再 `wsl.exe -d Ubuntu -- bash /mnt/d/.../xxx.sh`。

装依赖（WSL 里默认就是 root，不必加 sudo）：

```bash
apt-get update
apt-get install -y util-linux fdisk e2fsprogs xz-utils rsync p7zip-full aria2
# 下面这些只有「自己跑完整构建 build_image.sh」时才需要
apt-get install -y parted dosfstools exfatprogs btrfs-progs golang-go unzip
```

### 3.2 项目位置

项目就在 `D:\Projects\dArkOS-WebFront`，WSL 里直接用 `/mnt/d/Projects/dArkOS-WebFront` 访问，不用拷贝。

**唯一要求**：所有脚本必须是 LF 行尾。在 Windows 上编辑过 `.sh` 之后跑一次：

```bash
cd /mnt/d/Projects/dArkOS-WebFront
find . -type f \( -name '*.sh' -o -name '*.ini' -o -name '*.js' \) -exec sed -i 's/\r$//' {} +
```

CRLF 的脚本进到镜像里会直接 `bad interpreter` 起不来 —— 这个坑已经踩过一次。

### 3.3 拿到镜像

两条路任选其一。

**路线 A：用已经构建好的 dArkOS4Clone / ArkOS4Clone 镜像（推荐）**

`build_image.sh` 的产物是 `.img.xz`（超过 1900 MB 时会切成 `.7z.001`、`.7z.002` 分卷）。
拿到它就行，没必要自己再跑一遍耗时几十分钟的完整构建。

```bash
ls -lh /mnt/d/wsl/*.img.xz          # 例：放在 D:\wsl\ 下的镜像
```

> 判断下载是否完整：文件还挂着 `.baiduyun.p.downloading` / `.part` 之类后缀时也别慌，
> 交给 `prepare-image.sh`，它会先用 `xz -t` 校验，**没下完会明确告诉你**，不会拿半个镜像去干活。

**路线 B：自己从零构建（要改 rootfs 里别的东西时才需要）**

```bash
cd /mnt/d
git clone --recursive https://github.com/lcdyk0517/arkos4clone.git
cd arkos4clone
# 基础镜像是 christianhaitian/dArkOS Release 里的 G350 分卷：
#   下载 dArkOS_G350*.7z.001 / .002 后  ->  7z x dArkOS_G350*.7z.001 -o. -y
chmod +x build_image.sh grow_p2_plus.sh mount_arkos.sh clone_support.sh \
         build_dtb_selector.sh uboot/flash_uboot.sh
mkdir -p bin/pm_libs mnt
sudo -E ./build_image.sh /mnt/d/dArkOS_G350*.img /mnt/d/arkos-work
```

首次运行会联网下 JDK、PortMaster 和约 1.6 GB 的 runtimes，而且要求工作目录有
**≥ 180 GB** 空闲（扩容 + 压缩都要余量）。`D:` 盘当前 168 GB，偏紧。

### 3.4 先探明镜像结构（只读，不改任何东西）

```bash
cd /mnt/d/Projects/dArkOS-WebFront
bash tools/prepare-image.sh /mnt/d/wsl/ArkOS4Clone-09052026.img.xz
```

它会依次：校验完整性 → 解压出 `.img` → 打印分区表 → 挂载 rootfs →
打印 `/usr/bin/emulationstation/` 目录、`emulationstation.service`、
**ES 启动脚本全文**，以及那行关键的 `"$esdir/emulationstation" "$@"`。

**建议先跑这一步再注入**：能确认这个镜像里 ES 的启动脚本结构与预期一致；
万一格式不一样，也能提前发现，不至于打歪补丁。

### 3.5 预置机型 / 固定 DTB（**必须先做，否则刷完卡开不了机**）

**为什么必须做**：构建产出的镜像里**没有** `boot.ini`、没有 `.dtb`、没有 `Image`。
U-Boot 的 `bootcmd` 靠 `cfgload` 读 `boot.ini` 才拿到 `${dtb_name}`，
官方要求你先在 PC 上跑交互式选择器（`dtb_selector_win32.exe` 之类）选一次机型。

`tools/bake-console.sh` 就是这个选择器的**非交互版**——一次性把某个机型写死，
以后刷完卡开机直接用，不用再插卡跑选择器。

```bash
cd /mnt/d/Projects/dArkOS-WebFront

# 先看有哪些机型可选（只读，不改动）
sudo bash tools/bake-console.sh /mnt/d/darkos/ArkOS4Clone-09052026-vagaretro.img --list

# 写死为 R36H ProMax + original 内核（本项目当前用的配置）
sudo bash tools/bake-console.sh /mnt/d/darkos/ArkOS4Clone-09052026-vagaretro.img \
     --console r36hpromax --kernel original
```

它做的事与选择器**完全等价**：

| 步骤 | 内容 |
|---|---|
| 清理 | 删掉 p1 根目录旧的 `*.dtb` / `*.ini` / `*.orig` / `*.tony` / `.cn` 和 `BMPs/`（对应选择器的 `cleanTargetDirectory`） |
| 写配置 | `consoles/<机型>/boot.ini` → `p1/boot.ini`；`consoles/<机型>/*.dtb` → `p1/<dtb>` |
| 写内核 | `consoles/kernel/<版本>/Image` → `p1/Image` |
| 写启动图 | `consoles/logo/<分辨率>/logo.bmp` → `p1/logo.bmp`（分辨率按机型表自动推导） |
| 去掉提示标记 | 删 `USE_DTB_SELECT_TO_SELECT_DEVICE`（它只是给人看的提示，uboot 和系统都不读它） |

选项：

| 选项 | 说明 |
|---|---|
| `--console <名字>` | 机型目录名，默认 `r36hpromax`（名字 = `consoles/` 下的子目录名，可带空格） |
| `--kernel original\|arkos4clone_fix` | **默认 `original`**。区别见下表 |
| `--logo <目录>` | 启动图目录（如 `768P`），默认按 91 条机型表自动推导 |
| `--lang cn\|ko\|none` | 在 p1 放 `.cn` / `.ko` 标记，**首次开机 `clone.sh` 会自动应用中文/韩文 + 时区**，默认 `none` |
| `--keep-marker` | 保留 `USE_DTB_SELECT_TO_SELECT_DEVICE` |
| `--no-clean` | 跳过清理旧配置 |
| `--list` | 只列出可选的机型 / 内核版本 |
| `--dry-run` | 只检查并打印，不写入 |

**两个内核版本怎么选**（原文来自选择器源码）：

| 版本 | 说明 | 本项目 |
|---|---|---|
| `original` | arkos 原始驱动。**需搭配 `batteryplus`，否则充电时电量显示异常** | ✅ 在用 |
| `arkos4clone_fix` | arkos4clone 电池驱动。充放电都能正确显示电量，且有老化自适应 | 备选 |

**幂等且不占空间**：重复运行会先比对 md5，若已是目标状态就**一个字节都不改**；
真正要改动时才备份，且备份固定放在 `consoles/.bak/`（覆盖式，不会越跑越占空间）。

写完后核验（只读）：

```bash
sudo bash tests/verify-console.sh /mnt/d/darkos/ArkOS4Clone-09052026-vagaretro.img
```

它会**不需要你告诉它烧了哪个机型**——靠 md5 反查 `consoles/` 下的来源目录，
自动判定机型 / 内核版本 / 启动图分辨率，并检查 boot.ini 与 dtb 引用是否对得上。

> **想换机型**：重新跑一次 `bake-console.sh --console <新机型>` 即可（会自动清理旧配置）。
> 也可以照旧用官方选择器——`consoles/` 和 `dtb_selector_*` 都保留在 p1 上。

### 3.6 注入前端

```bash
cd /mnt/d/Projects/dArkOS-WebFront/tools
# 直接喂 .img.xz 也行，脚本会自动解压、打完补丁再压回去
# （需要约等于镜像大小的额外空间；加 --no-recompress 可跳过重压）
sudo ./inject-frontend.sh /mnt/d/wsl/ArkOS4Clone-09052026.img.xz
```

脚本会**自己定位 rootfs 分区**（不写死 `p2`），把 `payload/` 按镜像内路径铺开，
然后只改 ES 启动脚本里的那一行。原脚本会先备份成 `.vagaretro.bak`；
**如果找不到预期的那一行，脚本会直接中止而不是硬改**，不会把系统搞坏。

想省掉反复压/解压的时间：加 `--no-recompress`，留下未压缩的 `.img` 直接刷卡。
可选加 `--with-bridge-shim` 让页面挂上 `bridge-linux.js` 占位桥接（默认不挂，保持与源头一致）。

### 3.7 在设备上装渲染栈（阶段 2，必做）

前端页面要真画到屏幕上，需要 Wayland 合成器 + WebKitGTK。
镜像里原本**一个浏览器引擎都没有**（只有 mpv），所以要在设备上补这一层：

```bash
# 拷到设备上（或 351Files 的终端里）然后：
sudo bash install-browser.sh --check   # 先体检，看缺什么、空间够不够
sudo bash install-browser.sh           # 真装，约 104 MB（35 个包）
```

装的是 `weston + libwebkit2gtk-4.0-37 + gir1.2-gtk-3.0 + gir1.2-webkit2-4.0`，
另外补一套最小 GStreamer（页面里的视频预览用；装不上不影响图片界面）。

装完重启机器：

```bash
sudo systemctl restart emulationstation
```

> **为什么 weston 要以 root 跑**：`emulationstation.service` 是 `User=ark`、
> 没有分配 tty，weston 的 DRM 后端在非 root 下要靠 logind 的 seat 授权，
> 服务没有 seat 所以起不来。镜像的 `/etc/sudoers` 里有 `ark ALL=NOPASSWD: ALL`，
> 所以启动器用 `sudo -n` 拉 weston，宿主也跟着以 root 跑。这个细节改动前请先读
> [`docs/真机启动链路与平台映射.md`](docs/真机启动链路与平台映射.md) 第 5 节 ——
> 连带影响启动请求文件放哪（放 `/tmp` 会导致同一个游戏被无限重复启动）。

装完还要两样东西才能起来（**都已随镜像注入，不用手工做**，但知道原理省得踩）：

1. **libgbm shim**：`/opt/vagaretro/lib/libgbm-shim.so`。
   这台机器的 `libgbm.so.1 -> libMali.so`，缺 6 个 mesa 新符号，
   weston 的 `drm-backend.so` 一加载就 fatal。启动器会自动 `LD_PRELOAD` 它；
   源码 `build/libgbm-shim.c`，重编脚本 `build/build-shim.sh`。
2. **VT 复位**：启动器每次拉起 weston 前会把 tty1 从 `KD_GRAPHICS` 复位成 `KD_TEXT`，
   否则 weston 报 `/dev/tty1 is already in graphics mode`。

**实测可用组合**（R36H ProMax，2026-09-14）：

```
weston --backend=drm-backend.so --use-pixman --idle-time=0 --tty=1 --socket=wayland-vagaretro
```

`--use-pixman` 是必须的：不加的话 `gl-renderer.so` 会拿到
`/usr/local/lib/aarch64-linux-gnu/libEGL.so.1`（另一个 DDK 版本不符的 mali blob），
`eglInitialize` 直接 `EGL_NOT_INITIALIZED`。
想拿 GPU 硬渲染，方向是给 weston 设 `LD_LIBRARY_PATH=/usr/lib/aarch64-linux-gnu`
（已验证该 blob 的 GBM+EGL+GLES 全通，但还没接到 weston 上试）。

这一路的 **7 个真机坑**（sudo 清环境变量、VT 模式、shim 怎么链接、
D 状态进程的连锁灾难、`/dev/fb0` 不可信、`pgrep -x` 匹配不到……）都写在这里：
[`docs/真机渲染栈落地实录.md`](docs/真机渲染栈落地实录.md)。

### 3.8 改前端不用重刷卡（开发期推荐）

设备能 SSH（`ark`/`ark`，`sudo -n` 免密），所以迭代走这条路：

```bash
bash tools/deploy-device.sh <设备IP>        # 推 payload + 重启服务 + 打日志
bash tools/diag-device.sh                        # 只诊断，看三条渲染路径各自卡在哪
```

> 诊断脚本里**绝对不要写 `find /`** —— 会扫进 4.4 G 的 `/roms`（exFAT/FUSE），
> 把 TF 卡 IO 打满，连 sshd 都发不出 banner，只能强制重启。

### 3.8 刷卡验证

用 Win32DiskImager / rufus 把 `.img`（若无 `.img.xz` 先解压）写进 TF 卡，插机器开机。

**判断钩子生效**：

- 开机后 tty1 上会出现一个 3 秒的回执横幅：
  ```
  VagaRetro Web Frontend  --  启动钩子已生效
  页面入口 : /opt/vagaretro/index.html
  ...
  正在探测渲染引擎，失败会自动回退 EmulationStation
  ```
- 装好渲染栈后，横幅消失应当**直接进网页前端**（不再是 ES）
- 没装渲染栈 / 探测失败 → 照常进 EmulationStation（永不黑屏）
- 设备上 `/var/log/vagaretro-front.log` 和 `/var/log/vagaretro-shell.log` 是主要排查入口

看到这个横幅 = 注入成功。看不到但机器正常进 ES = 注入没生效，回去查步骤 3.6 的输出。

> **实测结果（2026-09-14）：已刷进 TF 卡，开机横幅正常出现，随后正常进 ES。**
> 阶段 1 在真机上闭环 ✅

### 3.9 回滚

```bash
sudo ./restore-es.sh /mnt/d/wsl/ArkOS4Clone-09052026.img.xz
```

或者不改镜像，直接在设备上把 `/opt/vagaretro/config.ini` 里的 `enabled` 改成 `0`
（`/opt/vagaretro/config.ini` 所在是 ext4 rootfs，从 Windows 读不了，需要 Linux 环境或设备上的文件管理器）。

### 3.10 本次实测记录（真镜像，2026-09-14）

| | |
|---|---|
| 输入 | `D:\darkos\ArkOS4Clone-09052026.img.xz`（4.0 GB） |
| 成品 | `D:\darkos\ArkOS4Clone-09052026-vagaretro.img`（9.9 GB） |
| md5 | `21504b96868545cdf6d3214b819f8fcf` |

真镜像结构（`tools/prepare-image.sh` 探明）：

| 项 | 实测值 |
|---|---|
| 解压后 | 9.88 GiB / 10607394816 字节，`Disklabel type: dos` |
| p1 | vfat，label `BOOT`，112 MB |
| p2 | ext4，label `root`，9.0 GB ← **注入目标** |
| p3 | **NTFS**，label `EASYROMS`，752 MB |
| ES 启动脚本 | `/usr/bin/emulationstation/emulationstation.sh`，5171 字节，128 行，**CR=0** |
| 注入点 | 第 **112** 行，6 空格缩进 |
| systemd unit | `emulationstation.service`（`After=firstboot.service play-video.service`、`User=ark`） |
| 运行身份 | **`ark` = uid 1002 / gid 1002**（启动器是以它跑的，不是 root） |
| rootfs 占用 | 8.8G / 8.9G（`Avail 0`）—— 68 KB payload 仍写入成功（ext4 root 保留块） |

结果：

- `tools/inject-frontend.sh` —— 全绿：挂载 → 铺 payload → 备份 → 打补丁 → 权限修正 → 卸载
- `tests/verify-injection.sh` —— **19 项断言全过，0 失败**

补丁落点（脚本第 110-114 行）：

```bash
110-  while true; do
111-      rm -f /tmp/es-restart /tmp/es-sysrestart /tmp/es-shutdown
112:      /usr/local/bin/vagaretro-front.sh "$esdir/emulationstation" "$@"
113-      ret=$?
114-      [ -f /tmp/es-restart ] && continue
```

三个实测发现：

1. 镜像里**有两份 ES** —— `/usr/bin/emulationstation/` 和 `/opt/mupen64plus/emulationstation/`
   （两边脚本 md5 不同）。只改 `/usr/bin/` 那份（systemd 指向它）；
   mupen64plus 那份是模拟器自带的包装器，**保持未改动**。
2. `p3` 实测是 **NTFS**，不是常被假设的 exFAT。注入脚本只挂 p2，不受影响；
   但往里塞 ROM 时要注意这点。
3. **权限必须提前铺好**：启动器由 systemd 以 `ark`(1002) 拉起，而镜像里
   `/var/log` 是 `root:110`、注入后 `/opt/vagaretro` 是 `root:root`，ark 都写不进去
   → 日志会静默丢失、熔断计数也写不了。

### 3.11 阶段 2 实测记录（真镜像 + 真 cfg，2026-09-14）

阶段 2（让前端真顶掉 ES、游戏走 Linux 原生 RA）在**真镜像的真数据**上验证过：

| 验证项 | 脚本 | 结果 |
|---|---|---|
| 宿主纯逻辑 | `tests/test-shell-logic.py` | **30 项 OK**（1 项 Windows 跳过） |
| 注入→校验→幂等→回滚→逐字节还原 | `tests/real-linux-e2e.sh` | **47 项全过** |
| 注入后核验（payload 全量 md5 + 权限 + 补丁） | `tests/verify-injection.sh` | **23 项全过** |
| **游戏循环**（点游戏→RA→回前端→再点） | `tests/launcher-loop-sim.sh` | **15 项全过** |
| **真 cfg + 真 cores 干跑 29 个平台** | `tests/real-launch-dryrun.py` | **29/29 全过** |

"干跑"的意思是：读真镜像的 `/etc/emulationstation/es_systems.cfg` 和真的
`/home/ark/.config/retroarch*/cores/`，给 `vagaretro-shell.py` 里 `PLATFORM_MAP`
的**每个**平台各生成一条启动命令（表驱动，卡二加机种自动覆盖），
然后把命令里出现的每个 `retroarch` 二进制 / `*_libretro.so` / `*.sh` 包装脚本
拿到真 rootfs 上核对是否存在。29 个平台全部命中。

顺带挖出并修掉了 **3 个只在真机才会暴露的 bug**（详见
[`docs/真机启动链路与平台映射.md`](docs/真机启动链路与平台映射.md)）：

1. 真 `es_systems.cfg` **不是合法 XML**（`2>&1` 里的 `&` 没转义），
   严格解析器直接抛异常 → 整个启动功能会静默失效。已加容错解析。
2. 平台映射表写错了：**PSP 在真机上叫 `ppsspp` 不是 `psp`，DOS 叫 `msdos` 不是 `dos`**。
3. weston 必须以 root 跑（服务没有 tty/seat），连带必须把启动请求文件从
   `/tmp` 挪到 `/opt/vagaretro/` —— 否则 ark 删不掉 root 建的文件，
   **同一个游戏会被无限重复启动**。
   注入脚本现在会把**目录**（不是文件）交给 ark：
   `chown 1002:1002 /opt/vagaretro`（保持 755）+ 预建
   `/var/log/vagaretro-front.log` 并 `chown 1002`。
   `index.html` 等文件仍是 `root 644`，不受影响、也防篡改。

---

## 4. 启动器行为说明

`/usr/local/bin/vagaretro-front.sh` 被 ES 启动脚本调用，逻辑：

1. 读 `/opt/vagaretro/config.ini`；`enabled=0` → 直接 `exec` 原版 ES
2. 按 `renderer` 选渲染路径（`auto` = 依次尝试）：
   - **`native`** —— `sudo -n weston --backend=drm-backend.so` 起 Wayland 合成器，
     再以 root 跑 `vagaretro-shell.py`（GTK3 + WebKit2 全屏宿主，自带桥接）。
     第一次失败会自动加 `--use-pixman` 软件渲染重试一次。
   - `cog` —— `cog -P drm`（该镜像的 eoan 源里其实装不到）
   - `chromium` —— `chromium --ozone-platform=drm`
3. **三种路径都不可用 → 直接 `exec` 原版 ES**（永不黑屏）
4. 网页前端退出后：若 `/opt/vagaretro/launch.sh` 存在（前端请求启动游戏），
   就 `bash` 它跑 RA，跑完**回到网页前端**；否则回原版 ES
5. **熔断**：前端起来后 10 秒内非正常退出，计一次失败；连续 3 次自动把
   `enabled=0` 写回 `config.ini`，下次开机彻底直通 ES

关于日志路径：启动器优先写 `/var/log/vagaretro-front.log`（注入时已预建并交给 `ark`）；
万一写不进去会自动退到 `/tmp/vagaretro-front.log`，不会静默丢日志。
宿主（root 跑）写 `/var/log/vagaretro-shell.log`。

调参都在 `/opt/vagaretro/config.ini`，改完不用重新注入，重启即生效。
**注意**：`request_file` 必须留在 `/opt/vagaretro/` 下 —— 那是 `ark` 的目录。
放 `/tmp` 会因为 sticky 位导致 ark 删不掉 root 建的文件，**同一个游戏被无限重复启动**。

---

## 5. 已知限制 / 下一步

- **两个阶段都已在真机闭环 ✅✅**
  - 阶段 1：`tests/real-linux-e2e.sh` 用**真** `losetup`、**真** `mount`、**真** ext4
    跑完「注入 → 校验 → 幂等 → 启动器控制流 → 回滚」全流程，**47 项断言全过**，
    回滚后 rootfs 与注入前逐文件逐字节一致。真镜像注入后 **23 项核验全过**（见 3.9）。
    机型/DTB 用 `bake-console.sh` 写死为 R36H ProMax，`verify-console.sh` **21 项全过**。
    成品：`D:\darkos\ArkOS4Clone-09052026-vagaretro.img`。
  - 阶段 2：宿主逻辑 **30 项单测**、游戏循环模拟 **15 项**、
    真 cfg + 真 cores 干跑 **29/29 个平台**，全过（见 3.11）。
    **真机实测（2026-09-14）：网页前端已经真的画在屏幕上了，ES 被顶掉；
    GPU(GL) 合成；卡二 29 个平台 11001 个条目；游戏启动端到端跑通。**
    踩坑全记录见 [`docs/真机渲染栈落地实录.md`](docs/真机渲染栈落地实录.md)。
- **换成别的机型**：跑 `bake-console.sh --console <名字>` 即可（会自动清理并备份旧配置）。
  注意 `--kernel` 选 `original` 还是 `arkos4clone_fix` 会影响充电时的电量显示。
- **前端不是空壳了**：桥接 (`vagaretro-shell.py` 的 `build_bridge_js`) 已落地 6 个方法
  `getRomRoot` / `listDir` / `readText` / `readMediaDataUrl` / `launchRetroArch` /
  `executeShellCommand`。前 4 个是**纯 JS 同步返回**（数据和目录列表在页面加载前
  由 Python 预扫描好、以 JSON 内联进注入脚本），后 2 个走 postMessage 异步丢回 Python。
  这么做是因为前端是同步调用 `AndroidBridge.*` 取返回值的，而 WebKit 的 postMessage
  是异步 IPC，没法同步返回。
- **渲染性能（下一步的主要课题）**：目前用 `--use-pixman`（CPU 软渲染），
  因为 GL 那条路上 weston 会拿到 `/usr/local` 那份 DDK 版本不符的 libEGL。
  已实测 `/usr/lib` 那个 blob 的 GBM+EGL+GLES 全通（Mali-G31 / OpenGL ES 3.2），
  所以加 `LD_LIBRARY_PATH=/usr/lib/aarch64-linux-gnu` 换掉 blob、再拿掉
  `--use-pixman` 大概率能拿到 GPU 加速 —— **尚未在真机验证**。
- **`/dev/fb0` 不能用来判断画面有没有出来**：跑 `weston-simple-shm` 时 fb0 也毫无变化，
  这块 rockchip DRM 上它不镜像 weston 的 scanout 缓冲。要取证得读 CRTC 的 fb，或者直接看屏幕。
- **GPIO/按键导航尚未实测**：weston 已认出手柄（`event3 odroidgo3-keys` 被识别为键盘，
  `event2 GO-Super Gamepad` 被 weston 当 joystick 忽略）。前端是靠**键盘事件**导航的
  （ArkOS 用 `/opt/inttools/gptokeyb` 把手柄映射成键盘），这条链还没实测过。
- **行尾**：注入进镜像的所有 `.sh` 必须是 LF。在 Windows 上编辑过这些文件之后，
  记得跑一次 `tests/real-linux-e2e.sh` 或者手动确认没有 CR。
- **payload 里不能有 `__pycache__`**：单测/干跑脚本 `import` 宿主模块时 Python 会顺手
  在 payload 里写 `.pyc`，然后被当成 payload 铺进镜像。三处一起防住了
  （`sys.dont_write_bytecode`、注入器 `rsync --exclude`、核验脚本用 `PYTHONPYCACHEPREFIX`）。
