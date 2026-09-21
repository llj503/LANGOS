/* ============================================================================
 *  vgfront.c —— VagaRetro 原生前端（C + SDL2 + KMSDRM/opengles2）
 *
 *  ---------------------------------------------------------------------------
 *  为什么要有它
 *  ---------------------------------------------------------------------------
 *  网页版在 weston 里跑，但 WebKit 拿不到 GPU：设备上缺一份
 *  「winsys=wayland 且 DDK>=11.7」的 mali blob（取证见
 *  docs/原生前端方案.md 第九节），只能 CPU 渲染。SDL2 的 KMSDRM 后端
 *  直接占 DRM、走 gbm winsys，绕开 wayland，实测 59 fps。
 *
 *  ---------------------------------------------------------------------------
 *  尺寸策略：把 CSS 原值照抄进来
 *  ---------------------------------------------------------------------------
 *  index.html 是按 1920x1080 设计、再整体缩放到屏幕的（.design-container）。
 *  这里把**设计值原样**写进常量，用 S() 宏做同一套缩放，
 *  于是对着 index.html 改数值时是 1:1 对应的，不用心算。
 *
 *  改样式时请直接翻 index.html 的 <style>，别凭感觉调数。
 *
 *  ---------------------------------------------------------------------------
 *  编译（在设备上，头文件环境见 native/dev-env.sh）
 *  ---------------------------------------------------------------------------
 *    source /opt/vagaretro/native/dev-env.sh
 *    gcc $VG_CFLAGS -O2 -Wall -o vgfront vgfront.c $VG_LDFLAGS \
 *        -lSDL2 -lSDL2_ttf -lm
 *
 *  运行（**必须先停 weston** —— KMSDRM 需要独占 DRM master）：
 *    systemctl stop emulationstation
 *    ./vgfront /opt/vagaretro/index.tsv
 * ==========================================================================*/
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>       /* strcasecmp（电池 status 的大小写不保证） */
#include <time.h>
#include <unistd.h>        /* access / fork / execv / sysconf */
#include <fcntl.h>         /* open（写 sysfs 用 O_WRONLY，不能用 fopen("w")） */
#include <dirent.h>        /* 扫 /sys/class/power_supply 找电池节点 */
#include <sys/statvfs.h>   /* statvfs：建 swapfile 前先看根分区还剩多少 */
#include <sys/statvfs.h>   /* 看 TF 卡剩余空间 */
#include <sys/wait.h>      /* waitpid */
#include <sys/utsname.h>   /* uname（设置页的系统信息） */
#include <sys/stat.h>      /* chmod（写 launch.sh 要可执行） */
#include <signal.h>        /* kill */

#include <SDL.h>
#include <SDL_image.h>
#include <SDL_ttf.h>

/* ==========================================================================
 *  0. 设计基准与调色板
 * ==========================================================================*/
#define DESIGN_W 1920
#define DESIGN_H 1080

static float g_scale = 1.0f;

/* 把 CSS 里的设计像素换算成屏幕像素 */
#define S(v) ((int)((v) * g_scale + 0.5f))

/* 用户可调的字号倍率（设置 → 显示 → 前端字体 / 游戏列表字体）。
   ⚠ 字体纹理是启动时一次性烘出来的，这两项改完**必须重启前端**才生效 ——
   设置页里改完会自己 execv 重启，用户感觉像"瞬间应用"。
   ⚠ 布局算式（行高、卡片高、底栏高…）也必须跟着用这两个宏，
   否则字变大了容器没变，字就会互相压在一起。 */
/* 存的是**百分比整数**（80..160，100 = 设计值），不是浮点倍率 ——
   因为滑块天生是整数，省掉一层 float<->int 的来回换算，存档里也直白。 */
static int g_fs_ui    = 100;      /* 前端整体字号 % */
static int g_fs_games = 100;      /* 游戏列表再乘一档 % */

#define FS_UI(v) ((int)((v) * g_scale * (g_fs_ui    * 0.01f) + 0.5f))
#define FS_G(v)  ((int)((v) * g_scale * (g_fs_ui    * 0.01f)                 \
                                     * (g_fs_games * 0.01f) + 0.5f))

/* 字号可调的上下限。80% 已经偏小但还能看；160% 是"整体 UI"的上限 ——
   再大底栏按键提示会互相压住、设置页的行名会换行。
   但**游戏列表**单独放宽到 300%（用户 2026-09-15 要求"游戏列表字号可以调到 300%"）：
   那部分只有封面卡片和标题，1.5 倍以后撑得住，而老人/远距离看正需要这么大。 */
#define FS_PCT_MIN  80
#define FS_PCT_MAX  160      /* 前端整体字号（fs_ui）上限 */
#define FS_GAME_MAX 300      /* 游戏列表字号（fs_games，在 fs_ui 基础上再乘）上限 */
#define FS_PCT_STEP 10
/* 步进放大阈值：过了这一档，字号的步子自动翻倍。
   100→300 若一律 10 一格要按二十下，翻倍后是十来下，手感才正常。 */
#define FS_PCT_FAST_FROM 160


/* 「退出自动存档」写进 appendconfig 的那个 state_slot。
   出厂 9 —— 刻意跟玩家手动存档常用的 0~5 位错开：自动存档每次退出都覆盖，
   撞上玩家手存的位就等于把人家的进度盖掉了。
   0..10 由设置页「自动存档位」调。
   ⚠ 真正读这个值的是**启动器**（vagaretro-shell.py 的 autosave_lines()），
     前端只负责把 autosave_slot 写进 vgfront.conf。 */
#define AUTOSLOT_MIN 0
#define AUTOSLOT_MAX 10
#define AUTOSLOT_DEF 9
static int g_autoslot = AUTOSLOT_DEF;

/* ---- 调色板：从编译期宏改成**运行期数组** ------------------------------
   加了主题系统（设置页可切、内存卡可自定义）之后，颜色必须是运行期变量。
   但下面几百处使用点全部写成 C_BG / C_TEXT 这样的宏名 —— 所以做法是
   "保留宏名、把值换成数组下标"，使用点一行都不用改。 */
enum {
    TH_BG = 0, TH_BG2, TH_PANEL, TH_PANEL2, TH_ACCENT, TH_ACCENT2, TH_GOLD,
    TH_DANGER, TH_OK, TH_TEXT, TH_SUBTEXT, TH_DIM, TH_LINE, TH_LINE2, TH_N
};

static Uint32 g_th[TH_N] = {
    0x0a0e1a, 0x0d1322, 0x121a30, 0x18213c, 0x4f8cff, 0x35e0b0,
    0xffc24d, 0xff5b6a, 0x43d17c, 0xeaf0fb, 0x8e99b4, 0x5f6b87,
    0x22304f, 0x2c3d63,
};

#define C_BG      (g_th[TH_BG])
#define C_BG2     (g_th[TH_BG2])
#define C_PANEL   (g_th[TH_PANEL])
#define C_PANEL2  (g_th[TH_PANEL2])
#define C_ACCENT  (g_th[TH_ACCENT])
#define C_ACCENT2 (g_th[TH_ACCENT2])
#define C_GOLD    (g_th[TH_GOLD])
#define C_DANGER  (g_th[TH_DANGER])
#define C_OK      (g_th[TH_OK])
#define C_TEXT    (g_th[TH_TEXT])
#define C_SUBTEXT (g_th[TH_SUBTEXT])
#define C_DIM     (g_th[TH_DIM])
#define C_LINE    (g_th[TH_LINE])
#define C_LINE2   (g_th[TH_LINE2])

/* 把颜色按比例压暗。主题里没有"强调色的暗版"这种字段 —— 需要的时候现算
   （tab 选中胶囊是 accent 叠一层压暗色，近似 CSS 的 90deg 渐变）。
   ⚠ 加主题之前这些地方是**硬编码**的 0x4f8cff / 0x6a9fff，所以换了主题
   只有背景变、胶囊还是蓝的 —— 一眼就看得出"主题没做全"。 */
static Uint32 th_darken(Uint32 c, float f)
{
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    Uint32 r = (Uint32)(((c >> 16) & 0xff) * f);
    Uint32 g = (Uint32)(((c >>  8) & 0xff) * f);
    Uint32 b = (Uint32)(( c        & 0xff) * f);
    return (r << 16) | (g << 8) | b;
}

/* 字段名（主题文件里用的 key，顺序与 TH_* 严格一致）。
   主题制作器和教程都按这张表来，改这里就是改协议。 */
static const char *const TH_KEY[TH_N] = {
    "bg", "bg2", "panel", "panel2", "accent", "accent2", "gold",
    "danger", "ok", "text", "subtext", "dim", "line", "line2",
};

/* ---- 内置主题 ----------------------------------------------------------
   ⚠ 第 0 套必须与上面 g_th[] 的出厂默认值**逐字一致** —— 它是兜底
   （没指定主题、或自定义主题文件缺字段时都拿它补）。

   自定义主题放内存卡的 themes 文件夹（*.theme，也认 /opt/vagaretro/themes/），
   格式 = 每行 `key=值`，key 就是上面 TH_KEY 那 14 个，值支持
   0xRRGGBB / #RRGGBB / RRGGBB。缺哪个字段就继承第 0 套。 */
typedef struct { const char *id; const char *name; Uint32 c[TH_N]; } ThemeDef;

#define THEME_BUILTIN_N 7
static const ThemeDef THEME_BUILTIN[THEME_BUILTIN_N] = {
    /* 0. LANGOS 经典 —— 出厂默认（深蓝）。
      ⚠ 这一套的颜色必须与 g_th[] 的初值**逐字一致**：
        theme_load() 找不到配置文件时就是拿它当兜底。 */
    { "langos", "LANGOS 经典",
      { 0x0a0e1a, 0x0d1322, 0x121a30, 0x18213c, 0x4f8cff, 0x35e0b0,
        0xffc24d, 0xff5b6a, 0x43d17c, 0xeaf0fb, 0x8e99b4, 0x5f6b87,
        0x22304f, 0x2c3d63 } },
    /* 1. 赛博霓虹（紫黑底 + 洋红主色 / 青色点缀） */
    { "neon", "赛博霓虹",
      { 0x0b0713, 0x130a20, 0x1b1030, 0x261745, 0xff3ea5, 0x22e0e0,
        0xffd166, 0xff5b6a, 0x3ee07a, 0xf6ecff, 0xa48fc4, 0x705c92,
        0x342153, 0x472c6e } },
    /* 2. 复古琥珀（CRT 暖色，像素掌机的味道） */
    { "amber", "复古琥珀",
      { 0x140c05, 0x1d1208, 0x271a08, 0x33230b, 0xffa726, 0xffd54f,
        0xffe082, 0xff6b3d, 0x9ccc65, 0xfff0d8, 0xc3a279, 0x8b7452,
        0x40300f, 0x57411a } },
    /* 3. 明亮纸白（浅色主题 —— 白天在户外看得清） */
    { "paper", "明亮纸白",
      { 0xeef1f7, 0xe3e8f2, 0xf7f9fd, 0xe9edf6, 0x2f6fe4, 0x0e9b78,
        0xc98a06, 0xd9384d, 0x1f9d55, 0x1a2233, 0x55637e, 0x8794aa,
        0xcfd7e5, 0xb9c4d6 } },
    /* 4. 极光（墨绿底 + 青绿主色 / 紫点缀）—— 2026-09-14 新增。
       深色但不发蓝，晚上玩不刺眼；青绿 + 紫这组是极光的经典配色。 */
    { "aurora", "极光",
      { 0x061410, 0x08201a, 0x0c2a22, 0x11362c, 0x2fd6a6, 0x8b6cff,
        0xffd166, 0xff5b6a, 0x3ee07a, 0xe8fff6, 0x7fa79a, 0x557a6e,
        0x1b4438, 0x26584a } },
    /* 5. 暮色（深紫褐底 + 暖橙主色 / 青色点缀）—— 2026-09-14 新增。
       黄昏天空那一挂：紫褐底配落日橙，比 amber 更暗更沉。 */
    { "dusk", "暮色",
      { 0x120a14, 0x1a0f1e, 0x24152a, 0x2f1c36, 0xff8a3d, 0x5ad2c8,
        0xffd98a, 0xff6b6b, 0x62d17c, 0xf6e9f0, 0xa88ba0, 0x7a6070,
        0x3a2340, 0x4d2f55 } },
    /* 6. 复古游戏（红白机那味儿）—— 2026-09-14 用户："再做个复古游戏主题"。
       老电视外壳的深棕黑打底，主色取卡带/机身的红，辅色取贴纸和按钮的金；
       跟 amber（CRT 暖黄）和 dusk（落日橙）都不同：这一套是**红配金**。
       这套主题配首页的漫画机种图最搭（图标是粗黑描边的美漫风）。 */
    { "retro", "复古游戏",
      { 0x14100c, 0x1e1812, 0x2a2119, 0x382b1f, 0xe8402f, 0xf2b544,
        0xffd166, 0xff5b6a, 0x7ac74f, 0xfff2e2, 0xd6b79c, 0xa08064,
        0x4a382a, 0x664a34 } },
};

/* 当前主题 id。改主题 = 改这个 + 重烘所有颜色。 */
static char g_theme_id[48] = "langos";

/* 顶栏 / 分类标签 / 标题 / 内容区的尺寸（全部来自 CSS）
   ⚠ 2026-09-14 用户："主界面顶部整体文字调大，现在太小了看不清"。
   根因：设计基准是 1920x1080，屏幕只有 1024x768 → g_scale≈0.533，
   **所有字号都被等比缩到 53%**。LOGO 48→26px、tab 26→14px 是真的看不清。
   所以这一组顶部字号按 ≈1.4x 上调，让上屏尺寸回到接近原始的观感。 */
#define TB_PAD_T   34
#define TB_PAD_L   44
#define TB_PAD_B   30
#define LOGO_FS    66      /* 上屏 ≈35px（原 26） */
#define SB_H       78      /* 顶栏内容高度还是按它算（提示本身已移除，见 draw_topbar） */
/* SB_MAXW / SB_RAD / SB_HINT_A 都随顶栏那段"搜索游戏…"提示一起删掉了
   （2026-09-16 用户："算了直接去掉 电量旁边的搜索游戏吧 反正是装饰去掉吧"）。
   现在顶栏不再画任何搜索相关的东西，搜索入口只剩底栏那项「Y 搜索」，功能没少。 */
#define SB_FS      32      /* ≈17px（原 13）—— ⚠ 电量/音量数字也用它，别动 */
#define STATUS_FS  32
#define STATUS_GAP 24
#define TIME_FS    58      /* ≈31px（原 23） */
#define TIMESUB_FS 24      /* ≈13px（原 10） */

#define TAB_PADX   44
#define TAB_PADY   22
#define TAB_RAD    34
#define TAB_FS     34      /* ≈18px（原 14） */
#define TAB_GAP    18      /* 字大了，间距收一点，7 个 tab 才排得下 */
#define TAB_B_FS   26
#define TABS_PADB  14

#define HDR_PAD_T  14
#define HDR_PAD_B  22
#define HDR_H1_FS  64      /* ≈34px（原 30） */
#define HDR_P_FS   30

#define CT_PAD_X   40

/* 平台网格。
   网页版 CSS 是 repeat(4,1fr)，这里**刻意**收成 3 列（用户 2026-09-14：
   "主页改成一排三个，现在太小"）—— 1024 宽的屏上 3 列的卡片宽 300px
   比 4 列的 219px 舒服得多，代价只是多滚一点。

   2026-09-14 加设置页后改成**运行时可变**：下面的宏只是出厂默认值，
   实际用的是 g_pg_cols / g_game_cols（可在设置页里改，存 vgfront.conf）。
   所有布局/导航计算点必须用变量，不要再用宏 —— 否则改完列数，
   网格按新列画、光标还按旧列跳，错位成那种"看着对、按着歪"的怪 bug。 */
#define PG_COLS_DEF   3
#define GAME_COLS_DEF 4

static int g_pg_cols   = PG_COLS_DEF;    /* 主页平台网格列数 3..5 */
static int g_game_cols = GAME_COLS_DEF;  /* 游戏列表列数    3..6 */

/* 游戏封面样式。用户 2026-09-14："游戏封面显示样式 正方形和竖版两种
   切换 游戏封面自动拉伸适应"。
   0 = 竖版（高 = 宽 × 4/3，原生 ES/ArkOS 的比例，也是出厂默认）
   1 = 正方形（高 = 宽 × 1，一屏能多放一行）
   封面图不管哪种都是**等比裁切铺满**（draw_cover_fit 的 cover 语义），
   不拉伸变形 —— 样式一变卡片高变、封面自己跟着适配。 */
static int g_cover_sty = 0;
/* 游戏遮罩：0 = 关闭，1..4 = BEZEL_VAL 的分辨率。见 BEZEL_VAL 那段注释 ——
   前端只记选择，真正挂遮罩的是启动器（vagaretro-shell.py）。
   ★ 2026-09-18：初始值仍是 0，但 sconf_load() 在 conf **没有** `bezel=`
     这一行时会调 bezel_auto_detect() 按本机屏幕自动定档（浪总要求：
     "什么固件什么dtb你就默认对应的遮罩"）—— 所以"0"只是"还没算"，
     不等于"用户选了关闭"。 */
static int g_bezel     = 0;
/* 游戏来源：0=自动（卡二优先，没插就读卡一）1=只读卡一 2=只读卡二。
   真干活的是宿主 vagaretro-shell.py —— 它读 vgfront.conf 的 romsrc=
   决定扫哪张卡建兼容树（--gen-index），这里只负责存值和显示。 */
static int g_romsrc    = 0;

/* 要不要播 LANGOS 品牌画面（默认**播**）。
   用户 2026-09-14："有涉及黑屏加载的地方 都用LANGOS的动画代替" ——
   前端每次起来（开机、退游戏回前端、设置改完的自重启）中间都有黑屏，
   统一用品牌画面兜住；--no-splash 关掉（离屏截图核对版式时用，
   不然每次截图都要多等半秒动画）。 */
static int g_want_splash = 1;

/* 调试/截图模式（argv[2] = 跑 N 帧就退）。
   ⚠ 它不只表示"跑完就退"，还要**挡住会改动设备状态的动作**。
   离屏实例跑的是真机同款二进制，又跟真机上那个前端共用同一个手柄设备：
   2026-09-14 出过一次事故 —— 截图跑到一半，手柄的按键被离屏实例收走，
   它当成玩家按了 A，把 launch.sh 写到了 /opt/vagaretro/ 上，
   真机前端一退出就会执行那个脚本、莫名其妙起一个游戏。
   见 launch_game() / main() 里的分支。 */
static int g_shot_mode = 0;

/* ---- 模拟器卡片外观：可被主题文件覆盖（cd_ 前缀的 key）-----------------
   用户 2026-09-15："主题制作工具加上模拟器卡片自定义"。

   这里暴露的是**卡片自己的外观** —— 封面区高度、圆角、间距、字号、底部
   不透明度，外加三个可选部件（封面大 logo / 左下角平台标签 / 右上角数量
   徽标）的开关。刻意**不动主界面布局**（顶栏/标签/内容区留白都是常量），
   也不放列数（列数是布局，照样留在设置页里改）。

   ⚠ 这些值要参与字体表的构建（PN_FS 等直接拿去 TTF_OpenFont），所以
     theme_load() 必须排在 main() 的 fonts[] 表**之前** —— 见那里的注释。
   ⚠ CD_DEF 与 tools/theme-maker.html 的 CARD_DEF 必须逐字一致，
     否则"导入主题 → 掌机看到的"和"预览里看到的"对不上。 */
enum {
    CD_ART_H = 0,   /* 封面区高度       px      */
    CD_RAD,         /* 卡片圆角         px      */
    CD_GAP,         /* 卡片间距         px      */
    CD_PAD_X,       /* 文字区左右内边距 px      */
    CD_PAD_T,       /* meta 区上内边距  px      */
    CD_PAD_B,       /* meta 区下内边距  px      */
    CD_NAME_FS,     /* 机种名字号               */
    CD_SUB_FS,      /* 副标题字号               */
    CD_LOGO_FS,     /* 封面里的大 logo 字号     */
    CD_SEL_RING,    /* 选中环粗细       px      */
    CD_ART_ALPHA,   /* 卡片底不透明度   0..255  */
    CD_SHOW_LOGO,   /* 显示封面大 logo   0/1    */
    CD_SHOW_SUB,    /* 显示副标题行      0/1    */
    CD_SHOW_TAG,    /* 显示左下角平台标签 0/1   */
    CD_SHOW_CNT,    /* 显示右上角数量徽标 0/1   */
    CD_N
};

static const char *const CD_KEY[CD_N] = {
    "cd_art_h", "cd_rad", "cd_gap", "cd_pad_x", "cd_pad_t", "cd_pad_b",
    "cd_name_fs", "cd_sub_fs", "cd_logo_fs", "cd_sel_ring", "cd_art_alpha",
    "cd_show_logo", "cd_show_sub", "cd_show_tag", "cd_show_cnt",
};

/* 出厂值。与 g_cd[] 的初值、与 theme-maker.html 的 CARD_DEF 三处必须一致。 */
static const int CD_DEF[CD_N] = {
    170, 20, 22, 24, 12, 14, 34, 26, 64, 6, 175, 1, 1, 1, 1,
};

/* 每项的合法范围 [min,max]（主题制作器的滑杆范围与这张表一致）。
   手写或第三方主题文件里的值都要过一遍 —— 写个 9999 不至于把卡片撑出屏，
   写个 0 也不至于把卡画没了。 */
static const int CD_LIM[CD_N][2] = {
    {  40, 400 },   /* art_h     */
    {   0,  48 },   /* rad       */
    {   2,  64 },   /* gap       */
    {   0,  80 },   /* pad_x     */
    {   0,  60 },   /* pad_t     */
    {   0,  60 },   /* pad_b     */
    {  14,  72 },   /* name_fs   */
    {  10,  60 },   /* sub_fs    */
    {  20, 160 },   /* logo_fs   */
    {   0,  20 },   /* sel_ring  */
    {   0, 255 },   /* art_alpha */
    {   0,   1 },   /* show_logo */
    {   0,   1 },   /* show_sub  */
    {   0,   1 },   /* show_tag  */
    {   0,   1 },   /* show_cnt  */
};

/* 当前生效的卡片外观（theme_load() 按主题填，缺字段留 CD_DEF） */
static int g_cd[CD_N] = { 170, 20, 22, 24, 12, 14, 34, 26, 64, 6, 175, 1, 1, 1, 1 };

/* 卡片封面图目录 —— 主题文件里的 cd_cover_dir。
   目录里按**平台 id** 放图（fc.png / sfc.jpg / history.png …），
   还可以放一张 _default.png 给所有没单独配图的卡片兜底。
   找到图的卡片，封面区直接铺那张图，不再画渐变和机种大字。

   空串 = 这套主题不用自定义图（卡片回到原有的渐变 + 大字）。
   ⚠ 必须排在 bake_all() 之前读好 —— 纹理是那时候烘的。 */
static char g_card_cover_dir[288] = "";

#define PG_GAP     (g_cd[CD_GAP])
#define PC_RAD     (g_cd[CD_RAD])
#define PC_BW      1
#define PC_BW_ON   3
#define PA_H       (g_cd[CD_ART_H])
#define PA_PADL    28
/* ⚠ 用户 2026-09-14："除了机种的标题，整个前端的字体都要放大" ——
   这张卡片上的**机种大字**（PL_FS）就是那个唯一的例外：它本来就顶满卡片，
   再放大就撑爆了。其余每一档都按 ≈1.45x 上调。
   背景：设备是 4 寸屏，而设计基准 1920 宽会被缩到 53%，
   原本 16px 设计字上屏只有 9px —— 不到 1 毫米高，真看不清。 */
/* 下面这几个原来是编译期常量，2026-09-15 起改成主题文件可覆盖（见上面的
   CD_* 表）：PL_FS=cd_logo_fs、PM_PAD_*=cd_pad_*、PN_FS=cd_name_fs、
   PSUB_FS=cd_sub_fs。取值方式从"取值"变成"读 g_cd[]"，用起来一模一样。 */
#define PL_FS      (g_cd[CD_LOGO_FS])
#define PCNT_FS    24      /* ≈13px（原 9）  右上角计数徽章 */
#define PCNT_RAD   16
#define PTAG_FS    22      /* ≈12px（原 7）  左下角标签 */
#define PTAG_RAD   12
#define PM_PAD_T   (g_cd[CD_PAD_T])
#define PM_PAD_X   (g_cd[CD_PAD_X])
#define PM_PAD_B   (g_cd[CD_PAD_B])
#define PN_FS      (g_cd[CD_NAME_FS])
#define PSUB_FS    (g_cd[CD_SUB_FS])
#define PCORE_FS   24      /* ≈13px（原 8）  核心名标签 */
#define PCORE_RAD  8
#define PSUB_GAP   8

/* 卡片下半区两行的行高（像素）。
   ★ 不能按"字号 × 1.35"估 —— 中文字形比拉丁字母高（有下沉部），
     带中文的机种名（"SFC / SNES 超任"、"我的收藏"）实测纹理高度会**超过**
     字号 × 1.35，于是第二行（"任天堂 [nes9x]"）压上来，两行糊在一起。
     用户 2026-09-15 反馈："主界面出现了模拟器卡片文字重叠现象"。
     改成烘完所有 t_name/t_sub 之后取**实测最大值** —— 这样所有卡片行高一致
     （视觉整齐，副标题仍对齐），且最高的那种也放得下。见 bake_all() 末尾。 */
static int g_name_h = 0;
static int g_sub_h  = 0;

/* 底栏 */
#define PB_GAP     14
#define PB_PADX    36
#define PB_PADY    14
#define PB_RAD     24
#define PBI_FS     32      /* ≈17px（原 13） */
#define PBK_FS     28      /* ≈15px（原 12） */
#define PBK_PADX   20
#define PBK_PADY   9
#define PBK_RAD    10
#define PB_MB      24

/* 游戏列表页（#pageGames）。数值一律照抄 index.html 的 .game-* 规则。 */
#define GPAD_T      20      /* .content{padding-top:20px} 只在这个页面有 */
#define GB_FS       26      /* ≈14px（原 9）  .back-btn */
#define GB_MB       20      /* .back-btn margin-bottom */
#define GH_H1_FS    54      /* ≈29px（原 21） .game-header h1 */
#define GH_H1_MB    8
#define GH_MB       24      /* .game-header margin-bottom */
#define GH_INFO_FS  24      /* ≈13px（原 8）  .game-header .info */
#define GH_INFO_GAP 20
/* 网页版 CSS 是 repeat(5,1fr)，这里改成 4 列（用户 2026-09-14：
   "游戏列表一排四个"）。列宽 176→224，封面高按 宽*4/3 跟着 235→299。
   GAME_COLS_DEF 在文件头跟 PG_COLS_DEF 一起定义（两者都是运行时变量）。 */
#define GAME_GAP    16      /* 列 gap 与片间距同为 16px */
#define GC_RAD      12      /* .game-card border-radius */
#define GC_BW       1
#define GC_BW_ON    3

/* 选中环的粗细（设计值）。用户 2026-09-14："选择框加粗一点呀 不太明显"
   —— 原来选中就是玻璃卡片自带的 1px 亮边 + 一层淡 accent 罩，
   在 4 寸屏上几乎看不出"当前选中是哪张"。这里改成在卡片底下垫一圈
   实色 accent，卡片盖上去后四周露出一道粗边（S(6) 在 1024 宽下 = 3px）。
   ⚠ 必须小于卡片间距的一半，否则相邻两卡的环会连成一片：
     游戏卡 gap S(16)=8px、首页卡 gap S(22)=12px，都够。 */
#define SEL_RING   (g_cd[CD_SEL_RING])
#define GCOVER_FS   22      /* ≈12px（原 7）  .game-cover 无封面时的居中文字 */
#define GNOV_FS     22      /* ≈12px（原 7）  .game-name-overlay */
#define GNOV_PADT   16
#define GNOV_PADX   8
#define GNOV_PADB   8

/* 搜索页（#pageSearch）。版式数值照抄 index.html 的 .search-* / .keyboard-*。
 *
 *  注意：网页版这一页**只有壳子** —— .search-input 是个 div 不是 input，
 *  .search-result-count 永远写着 "0 款"，按「搜索」键只 console.log，
 *  既没有过滤也没有结果列表。所以这里不是"照搬"，是按它的版式补成
 *  真能搜、真能启动：输入串实时过滤全平台游戏，结果列在下面可直接启动。 */
#define SP_PAD      32      /* .search-panel padding（字大了，内边距收一点） */
#define SP_RAD      20
#define SP_MAXW     1400    /* .search-panel max-width */
#define SP_TITLE_FS 34      /* ≈18px（原 13） .search-title */
#define SP_TITLE_MB 20
#define SP_IN_H     68      /* .search-input height（跟着字高走） */
#define SP_IN_FS    28      /* ≈15px（原 10） */
#define SP_IN_RAD   12
#define SP_IN_PADX  20
#define SP_ROW_GAP  16      /* .search-input-row gap */
#define SP_ROW_MB   30
#define SP_CNT_FS   28      /* ≈15px（原 10） .search-result-count */
#define KB_COLS     10      /* .keyboard 是 10 列网格 */
#define KB_KEY_H    78      /* .keyboard-key height（30px 字 + 上下留白） */
#define KB_GAP      10
#define KB_KEY_FS   30      /* ≈16px（原 11） */
#define KB_KEY_RAD  10
#define KB_HINT_FS  22      /* ≈12px（原 7）  .keyboard-hint */
#define KB_HINT_MT  30

/* 结果列表：网页版没有，这里补。观感沿用 .game-card ——
   --panel 底 + --line 描边，选中换 --accent。 */
#define SR_H        64      /* 搜索结果行高（跟着放大后的字走） */
#define SR_GAP      8
#define SR_RAD      10
#define SR_NMAX     500     /* 结果上限：空关键词会命中几千条，没必要全留 */

/* 软键盘的键表。放在这里而不是搜索页那一段 —— draw_search_page() 比
   handle_key() 靠前得多，宏得先看见。索引 36/37/38 是三个功能键。 */
#define KB_CHARS     "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
#define KB_N         ((int)sizeof(KB_CHARS) - 1)   /* 36 */
#define KB_ACT_BACK  (KB_N + 0)                    /* ⌫ */
#define KB_ACT_CLEAR (KB_N + 1)                    /* 清空 */
#define KB_ACT_GO    (KB_N + 2)                    /* 搜索 */
#define KB_TOTAL     (KB_N + 3)                    /* 39 = 3 行满 + 末行 9 格 */

/* ==========================================================================
 *  1. 索引数据结构
 *
 *  索引是宿主（vagaretro-shell.py）写出来的扁平 TSV，格式见那边的
 *  write_index()。这里**原地解析** —— 把 TAB 改成 '\0'，字符串指针直接
 *  指向读进来的缓冲，一个字节省一次 strdup。
 * ==========================================================================*/
typedef struct {
    const char *title;   /* 显示名（文件名） */

    /* 标题的**拼音首字母**（索引 v5 起有：「魂斗罗1」-> "hdl1"；旧索引为 NULL）。
       只服务「搜索游戏」—— 玩家输 HDL 得能搜到魂斗罗，而占位文案一直写着
       「输入首字母或关键词」（2026-09-17 浪总实测：字面匹配 0 条）。
       为什么表在宿主侧：那张 6763 字的声母表固化成 Python 字符串最省事，
       C 侧只管拿来比对。也正因为是"宿主给什么就比什么"，这里不假设它是
       小写 —— ci_contains() 自己会折叠大小写。 */
    const char *py;

    const char *rel;     /* 相对路径，真实路径 = rom_root/pid/rel */

    /* gamelist.xml 的 <image> 绝对路径（索引 v3 起有；没有就是 NULL）。
       为什么必须由 Python 给、不能靠文件名猜：街机的封面叫
       "1943 - The Battle Of Midway_screenscraper_mix_arrm.png"、PS1 的封面在
       previews/ 下 —— 这类名字跟 ROM 文件名毫无关系，规则再怎么写都猜不到，
       只有 gamelist 里明明白白写着。宿主已经确认过文件存在，这边不必再 stat。 */
    const char *img;

    /* 按文件名规则**探测出来**的封面路径，只算一次就记住。
       NULL = 还没算过；指向空串 = 算过了，确认这个游戏没有封面。
       为什么非要记忆化：cover_path() 是在绘制循环里调的，每个可见卡片每帧都调
       一次，不缓存的话每秒几千次 access() 全砸在 SD 卡上。 */
    char *cov;

    /* 这一条是从哪个**真实机种**来的。只有虚拟平台（history / favorite）
       上的条目才非 NULL —— 从「游玩历史」里再启动一个游戏时，当前平台的
       pid 是 "history"，拿它拼出来的 <compat>/history/<rel> 根本不存在，
       启动会直接失败（2026-09-15 发现：从收藏启动其实一直是坏的）。
       非虚拟平台上是 NULL，含义就是「用当前平台的 pid」。 */
    const char *src_pid;
} Game;

typedef struct {
    const char *pid, *cat, *name, *sub, *core, *logo, *tag;

    /* 这个平台**能换的核心**，逗号分隔（索引 v4 的 C 行；旧索引为 NULL）。
       "能换的" = es_systems.cfg 里这个平台原本就列着、装了 .so、且不在
       BAD_CORES 里的那些 —— 全是宿主在 --gen-index 时算好的。
       前端**不自己扫 cores 目录**：那一摊里有几百个跟本平台无关的 .so，
       放出来只会让人选到跑不起来的（用户 2026-09-15 就此专门拍过口径）。 */
    const char *cores;
    const char *imgdir;  /* 封面目录的**真实绝对路径**（索引 v2 起有；可能为 NULL）。
                            由宿主 realpath 出来 —— 卡二上的目录名不一定等于 pid，
                            而 compat_dir/pid 本身是软链，C 侧自己拼一定会猜错。 */
    Uint32 c1, c2;       /* 135° 渐变的起止色 */
    Game *games;
    int ngames, gcap;

    /* 预渲染的文字纹理 —— 每帧重新 TTF 渲染上千个纹理会卡，
       所以卡片上的文字在启动时一次性烘好，主循环里只做 blit。 */
    SDL_Texture *t_logo, *t_name, *t_sub, *t_core, *t_tag;
    int lw, lh, nw, nh, sw, sh, cw, ch, tw, th;

    /* 封面区的渐变纹理（圆角 + 对角渐变 + 高光，跟 CSS 的 platform-art 一致） */
    SDL_Texture *art;
    int aw, ah;

    /* 主题自带的卡片封面图（见 g_card_cover_dir）。
       非 NULL 时封面区直接铺这张图 —— 不画渐变、也不画机种大字。
       纹理由本结构持有，退出时统一释放。缺图就是 NULL，一切照旧。 */
    SDL_Texture *cimg;
    int ciw, cih;
} Platform;

typedef struct {
    long gen;
    const char *rom_root;
    Platform *p;
    int np, pcap;
    int total_games;
} Index;

/* --- 一次性 arena：索引里的所有字符串都在这里，程序结束才释放 -------- */
static char *ARENA;
static size_t ARENA_U, ARENA_C;

static void arena_init(size_t cap)
{
    ARENA = malloc(cap);
    if (!ARENA) { fprintf(stderr, "OOM arena %zu\n", cap); exit(1); }
    ARENA_C = cap;
    ARENA_U = 0;
}

static void arena_reset(void) { ARENA_U = 0; }

/* ==========================================================================
 *  2. 读文件 + 解析索引
 * ==========================================================================*/
static char *slurp(const char *path, size_t *out_len)
{
    FILE *fh = fopen(path, "rb");
    if (!fh) return NULL;
    fseek(fh, 0, SEEK_END);
    long n = ftell(fh);
    fseek(fh, 0, SEEK_SET);
    if (n <= 0) { fclose(fh); return NULL; }

    char *buf = ARENA + ARENA_U;
    if (ARENA_U + (size_t)n + 1 > ARENA_C) { fclose(fh); return NULL; }

    size_t got = fread(buf, 1, (size_t)n, fh);
    fclose(fh);
    buf[got] = '\0';
    ARENA_U += got + 1;
    if (out_len) *out_len = got;
    return buf;
}

/* 按 TAB 原地切分，返回字段数 */
static int split_tab(char *line, char **out, int max)
{
    int n = 0;
    out[n++] = line;
    for (char *c = line; *c; c++) {
        if (*c == '\t') {
            *c = '\0';
            if (n >= max) return n;
            out[n++] = c + 1;
        }
    }
    return n;
}

/* 十六进制 #rrggbb -> Uint32 */
static Uint32 parse_hex(const char *s)
{
    if (!s) return 0;
    if (*s == '#') s++;
    Uint32 v = 0;
    for (int i = 0; i < 6 && s[i]; i++) {
        char c = s[i];
        int d = (c >= '0' && c <= '9') ? c - '0'
              : (c >= 'a' && c <= 'f') ? c - 'a' + 10
              : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : 0;
        v = (v << 4) | (Uint32)d;
    }
    return v;
}

static Platform *plat_push(Index *ix)
{
    if (ix->np >= ix->pcap) {
        ix->pcap = ix->pcap ? ix->pcap * 2 : 48;
        ix->p = realloc(ix->p, (size_t)ix->pcap * sizeof(Platform));
        if (!ix->p) { fprintf(stderr, "OOM platforms\n"); exit(1); }
    }
    Platform *p = &ix->p[ix->np++];
    memset(p, 0, sizeof(*p));
    return p;
}

static void game_push(Platform *p, const char *title, const char *rel,
                     const char *img, const char *py)
{
    if (p->ngames >= p->gcap) {
        p->gcap = p->gcap ? p->gcap * 2 : 64;
        p->games = realloc(p->games, (size_t)p->gcap * sizeof(Game));
        if (!p->games) { fprintf(stderr, "OOM games\n"); exit(1); }
    }
    Game *g = &p->games[p->ngames];
    g->title = title;
    g->py    = py;
    g->rel   = rel;
    g->img   = img;
    g->cov   = NULL;          /* realloc 出来的新槽是脏内存，必须显式清零 */
    g->src_pid = NULL;
    p->ngames++;
}

static int idx_load(const char *path, Index *ix)
{
    memset(ix, 0, sizeof(*ix));

    /* 索引文件 800KB 上下，直接给 4 倍余量（切分不改结构，都是原地操作） */
    arena_init(8u * 1024 * 1024);

    size_t len = 0;
    char *buf = slurp(path, &len);
    if (!buf) {
        fprintf(stderr, "读不到索引: %s\n", path);
        return -1;
    }

    Platform *cur = NULL;
    char *fields[12];
    char *p = buf;

    while (*p) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = '\0';
        if (*p) {
            int n = split_tab(p, fields, 12);
            switch (fields[0][0]) {
            case 'V':
                break;
            case 'M':                       /* M 时间戳 根目录 平台数 游戏数 */
                if (n >= 5) {
                    ix->gen = atol(fields[1]);
                    ix->rom_root = fields[2];
                    ix->total_games = atoi(fields[4]);
                }
                break;
            case 'P':                       /* P pid cat name sub core logo tag c1 c2 imgdir */
                if (n >= 10) {
                    cur = plat_push(ix);
                    cur->pid  = fields[1];
                    cur->cat  = fields[2];
                    cur->name = fields[3];
                    cur->sub  = fields[4];
                    cur->core = fields[5];
                    cur->logo = fields[6];
                    cur->tag  = fields[7];
                    cur->c1   = parse_hex(fields[8]);
                    cur->c2   = parse_hex(fields[9]);
                    /* fields[10] 是 v2 才追加的封面目录（P 行的第 11 列）。
                       旧索引读进来是空 → NULL，前端自动退回"纯文字卡"，不会崩。 */
                    cur->imgdir = (n >= 11 && fields[10][0]) ? fields[10] : NULL;
                }
                break;
            case 'C':                       /* C pid core1,core2,...（索引 v4） */
                /* 紧跟在它自己的 P 行后面，所以认 cur 就够（和 G 行一个道理）。
                   旧索引没有这一行 → cur->cores 保持 NULL，弹层里那一项退化成
                   "只能保持自动"，不会崩。 */
                if (n >= 3 && cur && cur->pid && strcmp(cur->pid, fields[1]) == 0)
                    cur->cores = fields[2][0] ? fields[2] : NULL;
                break;
            case 'G':                       /* G 显示名 相对路径 封面绝对路径 [拼音首字母] */
                /* 第 4 列是 v3 才追加的封面绝对路径（gamelist 的 <image>）。
                   旧索引只有 3 列 → 空指针，前端自动退回按文件名猜。
                   第 5 列是 v5 才追加的拼音首字母，同理：旧索引没有就 NULL，
                   搜索退化成纯字面匹配（就是 v5 之前的行为），不会崩。 */
                if (n >= 3 && cur)
                    game_push(cur, fields[1], fields[2],
                              (n >= 4 && fields[3][0]) ? fields[3] : NULL,
                              (n >= 5 && fields[4][0]) ? fields[4] : NULL);
                break;
            }
        }
        p = nl ? nl + 1 : p + strlen(p);
    }

    return ix->np;
}

/* ==========================================================================
 *  3. 绘制原语
 *
 *  SDL2 2.0.10 没有 SDL_RenderGeometry（2.0.18 才有），也没有圆角和渐变，
 *  全部得自己来。好在这几招够用且不慢。
 * ==========================================================================*/
static void setc(SDL_Renderer *r, Uint32 rgb, int a)
{
    SDL_SetRenderDrawColor(r, (Uint8)(rgb >> 16), (Uint8)(rgb >> 8),
                           (Uint8)rgb, (Uint8)a);
}

/* --- 圆角矩形：烘成纹理再贴，不要逐行 DrawLine ----------------------------
 *
 *  第一版是逐行 SDL_RenderDrawLine 描一个圆角矩形（想做"零资源"的圆角）。
 *  真机一跑只有 4.5 fps —— 因为 **GL 后端里每一次 DrawLine 就是一个 draw
 *  call**：一张卡片要 142 行 x 2 层（描边+填充），31 张就是近万次，
 *  管线全耗在提交上。
 *
 *  改成：把"白色圆角矩形"按 (w,h,r) 缓存成纹理，绘制时 ColorMod 上色 +
 *  RenderCopy —— 一个形状 1 次 draw call。尺寸种类很少（卡片就 1 种），
 *  所以缓存命中率接近 100%。
 */
typedef struct RCache {
    struct RCache *next;
    int w, h, r;
    SDL_Texture *tex;
} RCache;

static RCache *g_rcache;

static SDL_Texture *round_tex(SDL_Renderer *ren, int w, int h, int r)
{
    if (w <= 0 || h <= 0) return NULL;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;

    for (RCache *c = g_rcache; c; c = c->next)
        if (c->w == w && c->h == h && c->r == r) return c->tex;

    SDL_Surface *sf = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32,
                                                     SDL_PIXELFORMAT_ARGB8888);
    if (!sf) return NULL;

    SDL_LockSurface(sf);
    Uint32 *px = (Uint32 *)sf->pixels;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            double cx, cy;
            if (x < r && y < r) {
                cx = r - x - 0.5;        cy = r - y - 0.5;
            } else if (x >= w - r && y < r) {
                cx = x - (w - r) + 0.5;  cy = r - y - 0.5;
            } else if (x < r && y >= h - r) {
                cx = r - x - 0.5;        cy = y - (h - r) + 0.5;
            } else if (x >= w - r && y >= h - r) {
                cx = x - (w - r) + 0.5;  cy = y - (h - r) + 0.5;
            } else {
                px[y * w + x] = 0xffffffffu;   /* 直边区域，全不透明 */
                continue;
            }
            /* d = 到角心的距离；a>0 在圆内。留 1px 做抗锯齿过渡，
               否则圆角会有明显台阶（1024 宽下看得出来）。 */
            double d = sqrt(cx * cx + cy * cy);
            double a = (double)r - d;
            if (a <= -1.0)      px[y * w + x] = 0;
            else if (a >= 0.0)  px[y * w + x] = 0xffffffffu;
            else {
                int al = (int)((a + 1.0) * 255.0);
                if (al < 0) al = 0;
                if (al > 255) al = 255;
                px[y * w + x] = ((Uint32)al << 24) | 0x00ffffffu;
            }
        }
    }
    SDL_UnlockSurface(sf);

    SDL_Texture *t = SDL_CreateTextureFromSurface(ren, sf);
    SDL_FreeSurface(sf);
    if (!t) return NULL;
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);

    RCache *c = malloc(sizeof(RCache));
    if (c) {
        c->w = w; c->h = h; c->r = r; c->tex = t;
        c->next = g_rcache;
        g_rcache = c;
    }
    return t;
}

static void draw_round(SDL_Renderer *ren, SDL_Rect rc, int r,
                       Uint32 rgb, int alpha)
{
    if (rc.w <= 0 || rc.h <= 0) return;
    SDL_Texture *t = round_tex(ren, rc.w, rc.h, r);
    if (!t) {
        setc(ren, rgb, alpha);
        SDL_RenderFillRect(ren, &rc);
        return;
    }
    SDL_SetTextureColorMod(t, (Uint8)(rgb >> 16), (Uint8)(rgb >> 8),
                              (Uint8)rgb);
    SDL_SetTextureAlphaMod(t, (Uint8)alpha);
    SDL_RenderCopy(ren, t, NULL, &rc);
    SDL_SetTextureAlphaMod(t, 255);
}

/* 带描边的圆角矩形：先铺一层边框色，再铺内缩的填充色 */
static void round_box(SDL_Renderer *ren, SDL_Rect rc, int r,
                      Uint32 fill, Uint32 border, int bw)
{
    if (bw > 0) {
        draw_round(ren, rc, r, border, 255);
        SDL_Rect in = { rc.x + bw, rc.y + bw, rc.w - 2 * bw, rc.h - 2 * bw };
        draw_round(ren, in, r > bw ? r - bw : 0, fill, 255);
    } else {
        draw_round(ren, rc, r, fill, 255);
    }
}

/* 135° 对角渐变烘焙成纹理 —— 卡片封面区用。
   CSS 是 linear-gradient(135deg, c1, c2)：135° 在 CSS 里指向右下，
   所以投影方向取 (1,1)/√2，t = ((x+w*y)/(w+h)) 归一化。 */
static SDL_Texture *make_grad_art(SDL_Renderer *r, int w, int h, int rad,
                                  Uint32 c1, Uint32 c2, int top_round_only)
{
    SDL_Surface *sf = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32,
                                                     SDL_PIXELFORMAT_ARGB8888);
    if (!sf) return NULL;

    SDL_LockSurface(sf);
    Uint32 *px = (Uint32 *)sf->pixels;
    int r1 = (c1 >> 16) & 0xff, g1 = (c1 >> 8) & 0xff, b1 = c1 & 0xff;
    int r2 = (c2 >> 16) & 0xff, g2 = (c2 >> 8) & 0xff, b2 = c2 & 0xff;
    const double denom = (double)(w + h);

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            /* 圆角遮罩：上两角正常，下两角（top_round_only 时）不做 */
            int inx = 1;
            if (y < rad) {
                double dx = rad - x, dy = rad - y;
                if (x < rad && dx * dx + dy * dy > (double)rad * rad) inx = 0;
                double ex = x - (w - rad);
                if (x >= w - rad && ex * ex + dy * dy > (double)rad * rad) inx = 0;
            } else if (!top_round_only && y >= h - rad) {
                double dy = y - (h - rad);
                double dx = rad - x;
                if (x < rad && dx * dx + dy * dy > (double)rad * rad) inx = 0;
                double ex = x - (w - rad);
                if (x >= w - rad && ex * ex + dy * dy > (double)rad * rad) inx = 0;
            }
            if (!inx) { px[y * w + x] = 0; continue; }

            double t = ((double)x + (double)y) / denom;
            if (t > 1.0) t = 1.0;
            int rr = (int)(r1 + (r2 - r1) * t + 0.5);
            int gg = (int)(g1 + (g2 - g1) * t + 0.5);
            int bb = (int)(b1 + (b2 - b1) * t + 0.5);

            /* CSS 那层 radial-gradient 高光：圆心在 30% 30%，白色 15% 淡出到 60% */
            double hx = x - w * 0.30, hy = y - h * 0.30;
            double hd = sqrt(hx * hx + hy * hy);
            double hrad = (double)w * 0.60;
            if (hd < hrad) {
                double a = (1.0 - hd / hrad) * 0.15;
                rr = (int)(rr + (255 - rr) * a + 0.5);
                gg = (int)(gg + (255 - gg) * a + 0.5);
                bb = (int)(bb + (255 - bb) * a + 0.5);
            }
            px[y * w + x] = 0xff000000u | ((Uint32)rr << 16)
                          | ((Uint32)gg << 8) | (Uint32)bb;
        }
    }
    SDL_UnlockSurface(sf);

    SDL_Texture *t = SDL_CreateTextureFromSurface(r, sf);
    SDL_FreeSurface(sf);
    if (t) SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    return t;
}

/* --- 字体 ----------------------------------------------------------------
 *  **别再用 DroidSansFallbackFull.ttf** —— 它是个纯 CJK 的 fallback 字体，
 *  cmap 里连 U+0041(A) 和 U+0030(0) 都没有（native/font-test.c 实测），
 *  拿它渲染 "LANGOS" / "926" 会全变成 .notdef 方块。
 *  Android 上它本来只负责补 CJK，拉丁交给主字体，所以这个坑很容易踩。
 *
 *  NotoSansCJK-Regular.ttc 里拉丁+CJK+数字全覆盖，face 顺序：
 *      0=JP  1=KR  2=SC  3=TC  4=HK   （5..9 是等宽版）
 *  取 face 2 拿简体字形。
 *
 *  粗体用 TTF_STYLE_BOLD 合成，不加载 Bold.ttc —— 那要多 20MB 和 7 个 face，
 *  在这台 1GB 内存的机器上不值。
 */
#define FONT_PATH "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"
#define FONT_FACE 2

#ifdef OLD_FONT_KEEP_FOR_REFERENCE
#define FONT_PATH "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf"
#endif

static TTF_Font *F_LOGO, *F_BODY, *F_SMALL, *F_H1, *F_TIME, *F_TIMESUB,
                *F_PNAME, *F_PSUB, *F_PCORE, *F_PTAG, *F_PLOGO, *F_PCNT,
                *F_TAB, *F_TABB, *F_PBI, *F_PBK, *F_SB,
                /* 游戏列表页 */
                *F_BACK, *F_GH1, *F_GINFO, *F_GCOVER, *F_GNOV,
                /* 搜索页（字体数与字号都对齐 .search-* / .keyboard-key） */
                *F_STITLE, *F_SINPUT, *F_SKEY,
                /* 设置页（.settings-row 的 18px 名 / 17px 值） */
                *F_SETN, *F_SETV,
                /* 设置页的顶部提示 / 底部说明 / 溢出行号。原先复用 F_GINFO
                   （game-info 的 15px 设计字号）—— 在 1024x768 上只有 ≈8px，
                   全屏后完全看不清，所以单独给一档大字号。 */
                *F_SETF;

/* ==========================================================================
 *  界面语言（中文 / English）
 *
 *  收口点**只有一处**：bake() 里的 TTF_RenderUTF8_Blended。整个前端所有上屏
 *  文字都得走它（bake_cached 是转发给它的；ellipsize 只用 TTF_SizeUTF8 量宽、
 *  根本不渲染），所以在那一行前面查一次表就覆盖了全界面 —— 不用去改上百个
 *  绘制点，也不会漏掉哪个角落。
 *
 *  查不到的原样返回。游戏名、封面名这类是**数据**，不翻正好；索引里带的
 *  中文（"FC / NES 红白机"、"移植游戏 Ports"）也塞进表里了，所以宿主不用加
 *  英文列，索引格式不用动。
 *
 *  ⚠ 表会显式跳过纯 ASCII 串，省掉一次全表扫描 —— 而 bake() 在绘制循环的
 *    热路径上（bake_cached 未命中的时候），能省则省。
 * ==========================================================================*/
#define UI_ZH 0
#define UI_EN 1
static int g_uilang = UI_ZH;

#define UILANG_N 2
/* 语言名一律显示各自的本名（中文 / English），不互相翻译 —— 界面正显示英文时
   "中文" 这项还得认得出来，翻成 "Chinese" 反而更难找。所以 "中文" 故意不进表。 */
static const char *const UILANG_LABEL[UILANG_N] = { "中文", "English" };
static const int         UILANG_VAL[UILANG_N]   = { UI_ZH, UI_EN };

static const struct { const char *zh, *en; } TR[] = {
    { "日", "Sun" },
    { "一", "Mon" },
    { "二", "Tue" },
    { "三", "Wed" },
    { "四", "Thu" },
    { "五", "Fri" },
    { "六", "Sat" },
    { "全部", "All" },
    { "任天堂", "Nintendo" },
    { "世嘉", "Sega" },
    { "索尼", "Sony" },
    { "掌机", "Handheld" },
    { "街机", "Arcade" },
    { "电脑", "Computer" },
    { "其他", "Other" },
    { "雅达利", "Atari" },
    { "万代", "Bandai" },
    { "同人", "Homebrew" },
    { "收藏", "Favorites" },
    { "历史", "History" },
    { "暂无介绍", "No description" },
    { "全部机种", "All systems" },
    { "任天堂机种", "Nintendo systems" },
    { "世嘉机种", "Sega systems" },
    { "索尼机种", "Sony systems" },
    { "FC / NES 红白机", "FC / NES" },
    { "SFC / SNES 超任", "SFC / SNES" },
    { "移植游戏 Ports", "Ports" },
    { "街机 FBNeo", "Arcade FBNeo" },
    { "OpenBOR 同人格斗", "OpenBOR" },
    { "PICO-8 幻想主机", "PICO-8" },
    { "游玩历史", "Play History" },
    { "我的收藏", "Favorites" },
    { "主题", "Theme" },
    { "关闭", "Off" },
    { "关机", "Power Off" },
    { "内存", "Memory" },
    { "内核", "Kernel" },
    { "分类", "Category" },
    { "前端", "Frontend" },
    { "卡一", "Card 1" },
    { "卡二", "Card 2" },
    { "声音", "Sound" },
    { "存储", "Storage" },
    { "安装", "Install" },
    { "屏幕", "Screen" },
    { "平台", "Platform" },
    { "性能", "Performance" },
    { "执行", "Run" },
    { "时区", "Time Zone" },
    { "显示", "Display" },
    { "未知", "Unknown" },
    { "核心", "Core" },
    { "清除", "Clear" },
    { "游戏", "Games" },
    { "电池", "Battery" },
    { "确认", "Confirm" },
    { "索引", "Index" },
    { "网络", "Network" },
    { "自动", "Auto" },
    { "设置", "Settings" },
    { "调值", "Adjust" },
    { "返回", "Back" },
    { "进入", "Open" },
    { "退出", "Quit" },
    { "选择", "Select" },
    { "金手指", "Cheats" },
    { "← 返回", "← Back" },
    { "充电中", "Charging" },
    { "已充满", "Full" },
    { "放电中", "Discharging" },
    { "未充电", "Not charging" },
    { "处理器", "CPU" },
    { "正在启动", "Starting up" },
    { "小", "Small" },
    { "标准", "Standard" },
    { "大", "Large" },
    { "特大", "Extra Large" },
    { "超大", "Huge" },
    { "竖版", "Portrait" },
    { "正方形", "Square" },
    { "LANGOS 经典", "LANGOS Classic" },
    { "赛博霓虹", "Cyber Neon" },
    { "复古琥珀", "Retro Amber" },
    { "明亮纸白", "Paper White" },
    { "极光", "Aurora" },
    { "暮色", "Dusk" },
    { "复古游戏", "Retro" },
    { "内置主题", "Built-in themes" },
    { "声音设置", "Sound Settings" },
    { "显示设置", "Display Settings" },
    { "主题外观", "Theme" },
    { "网络与传输", "Network & SSH" },
    { "性能设置", "Performance" },
    { "时间与地区", "Time & Region" },
    { "系统信息", "System Info" },
    { "快捷键说明", "Hotkey guide" },
    { "电源 / 退出", "Power & Quit" },
    { "无线网络", "Wi-Fi Network" },
    { "游戏来源", "Game Source" },
    { "游戏遮罩", "Bezel" },
    { "屏幕亮度", "Brightness" },
    { "息屏时间", "Screen timeout" },
    { "系统音量", "System Volume" },
    { "无线开关", "Wi-Fi" },
    { "SSH 服务", "SSH service" },
    { "虚拟内存", "Swap File" },
    { "前端字体大小", "UI Font Size" },
    { "游戏列表字号", "Game list font scale" },
    { "游戏封面样式", "Cover style" },
    { "主页列数", "Home columns" },
    { "游戏列数", "Game list columns" },
    { "RA 界面语言", "RA menu language" },
    /* 「RA 加速按键」的中英文标签已随设置项一起删掉（2026-09-16）。 */
    { "一键安装金手指", "Install cheats" },
    { "清除金手指", "Remove cheats" },
    { "已装金手指", "Installed cheats" },
    { "金手指包", "Cheat pack" },
    { "中文金手指", "Chinese cheats" },
    { "CPU 调速器", "CPU governor" },
    { "GPU 调速器", "GPU governor" },
    { "内存调速器", "Memory governor" },
    { "DDR 频率策略", "DDR frequency" },
    { "当前调频", "Current governor" },
    { "当前连接", "Current connection" },
    { "当前时间", "Current time" },
    { "设备型号", "Device Model" },
    { "ROM 目录", "ROM folder" },
    { "SSH 地址", "SSH address" },
    { "搜索游戏", "Search games" },
    { "搜索游戏…", "Search games…" },
    { "系统语言", "System Language" },
    { "正在关机…", "Shutting down…" },
    { "正在重启前端…", "Restarting UI…" },
    { "正在重启系统…", "Rebooting…" },
    { "正在重扫游戏…", "Rescanning games…" },
    { "正在载入游戏库…", "Loading game library…" },
    { "正在解压金手指包…", "Extracting cheat pack…" },
    { "正在连接，请稍候…", "Connecting, please wait…" },
    { "正在重启界面…", "Restarting UI…" },
    { "没有找到游戏", "No games found" },
    { "没有找到匹配的游戏", "No matching games" },
    { "已经在用这套主题了", "This theme is already active" },
    { "再按一次 A 确认", "Press A again to confirm" },
    { "已保存，请手动重启前端", "Saved — restart the UI manually" },
    { "已保存，重启前端后生效", "Saved — applies after a UI restart" },
    { "重启失败：写不了 launch.sh", "Restart failed: cannot write launch.sh" },
    { "关机失败：写不了 /tmp/es-shutdown", "Shutdown failed: cannot write /tmp/es-shutdown" },
    { "重启失败：写不了 /tmp/es-sysrestart", "Reboot failed: cannot write /tmp/es-sysrestart" },
    { "连接失败：密码不对 / 信号太弱 / 超出范围", "Connection failed: wrong password / weak signal / out of range" },
    { "只读，看看当前设备状态", "Read-only — current device state" },
    { "前端、弹层、游戏里、系统四级快捷键全表。可滚动，B 返回", "Full hotkey reference: frontend, popup, in-game and system. Scrollable; B goes back" },
    { "A 确认 / B 返回", "A Confirm / B Back" },
    { "A 确认 / ←→ 调值 / B 返回", "A Confirm / ←→ Adjust / B Back" },
    { "A 应用（界面会重启）/ B 返回", "A Apply (UI restarts) / B Back" },
    { "输入首字母或关键词...", "Type initials or keywords..." },
    { "输入首字母或关键词开始搜索", "Type initials or keywords to search" },
    { "方向键选字母/结果 · A 输入/启动 · B 删除/返回 · ↓ 进入结果 ↑ 回键盘 · START 清空", "D-pad letter/result · A type or launch · B delete or back · ↓ results ↑ keyboard · START clear" },
    { "屏幕亮度，以及主页/游戏列表的列数", "Screen brightness, plus home and game grid columns" },
    { "系统音量（amixer + alsactl），改完立即生效", "System volume (amixer + alsactl), applied immediately" },
    { "换一套界面配色。把 .theme 文件放进内存卡的 themes 文件夹就能加自己的", "Switch the UI palette. Drop a .theme file into the card's themes folder to add your own" },
    { "连 WiFi、开关 SSH —— 传 ROM 和改文件都靠它", "Join Wi-Fi and toggle SSH — needed to copy ROMs and edit files" },
    { "CPU / GPU / 内存的调频策略", "CPU / GPU / memory frequency governors" },
    { "时区（出厂值是纽约，中文用户基本都要改）", "Time zone (the factory default is New York — most users change it)" },
    { "现在连着的无线名和它的 IP 地址", "Current Wi-Fi name and its IP address" },
    { "主页平台网格一排几个。列少了卡片大、列多了信息密", "Platforms per row. Fewer means bigger cards, more means denser" },
    { "游戏列表一排几个。存 vgfront.conf，下次启动仍是这个值", "Games per row. Saved to vgfront.conf and reused next boot" },
    { "竖版 = 高是宽的 1.33 倍（传统卡盒比例）；正方形 = 1:1，一屏多一行。封面自动裁切适配", "Portrait is 1.33x taller (classic box art); Square is 1:1 and fits one more row. Covers are cropped to fit" },
    { "给游戏画面套一层主机边框，画面按核心原生比例显示不错位。选和屏幕分辨率一致的那档，下次进游戏生效", "Draws a console-style frame around the game at native aspect ratio. Pick the entry matching your screen; applies next launch" },
    { "整个前端的字号（顶栏、标签、设置页）。左右调，改完自动重启前端", "Font scale for the whole UI (top bar, labels, settings). Use ←→; the UI restarts automatically" },
    { "游戏列表单独再调一档，在「前端字体大小」的基础上乘，最大 300%。调完自动重启", "Extra font scale for the game list, multiplied on top of UI Font Size (max 300%). The UI restarts after a change" },
    { "一键安装中文金手指合集（RA + PPSSPP 两边的目录都放到位）", "Install the Chinese cheat pack into both RetroArch and PPSSPP" },
    { "只删中文包，RA 自带的官方金手指库（那 76 个目录）原样保留", "Removes only the Chinese pack; RetroArch's own cheat library (76 folders) is left intact" },
    { "RA 的 cheats/中文金手指 下现有多少个 .cht（整包是 2152 个）", "How many .cht files are in RetroArch's cheats folder (the full pack has 2152)" },
    { "内存卡 /roms/vagaretro/cheats.zip 优先；没有就用内置的那份", "Prefers /roms/vagaretro/cheats.zip on the card, falls back to the built-in copy" },
    { "把金手指包装进 RA（.cht，2152 个）和 PPSSPP（.ini，4308 个）。约半分钟，装完按 RA 菜单→金手指→加载金手指文件", "Unpacks the cheat pack into RetroArch (.cht, 2152 files) and PPSSPP (.ini, 4308 files). Takes about 30 seconds; then use RetroArch menu, Cheats, Load Cheat File" },
    { "改 RetroArch 自己的菜单语言（写 retroarch.cfg，下次进游戏生效）。前端界面语言不受影响；PS1 那份配置也一起改", "Changes RetroArch's own menu language (writes retroarch.cfg; applies next launch). The front-end UI language is separate, and the PS1 config is updated too" },
    { "游戏里按住这个键就快进。写的是 RetroArch 的快进键（retroarch.cfg），下次进游戏生效", "Hold this key in-game to fast-forward. Writes RetroArch's fast-forward binding (retroarch.cfg); applies next launch" },
    { "自动 = 插了卡二就读卡二，没插就读卡一；锁定卡一/卡二则只读那一张。改完自动重扫游戏列表并重启前端", "Auto uses card 2 when present and card 1 otherwise; locking picks one. Changing this rescans the library and restarts the UI" },
    { "用根分区的交换文件顶内存（放 /roms 会吃掉本就不多的卡空间）。改完立即生效", "Swap file on the root partition (putting it on /roms would eat scarce card space). Applied immediately" },
    { "平时用省电的，玩 3D 游戏前可以切 performance（启动游戏时 LANGOS 会临时切）", "Keeps a power-saving governor; switch to performance for 3D games (LANGOS does this temporarily at launch)" },
    { "Mali-G31 的 devfreq 策略，卡顿或发热可以在这里换", "Mali-G31 devfreq policy — switch if you see stutter or excess heat" },
    { "系统时间。时间不对先看上面那项时区对不对", "System time. If it is wrong, check the time zone above first" },
    { "改完立即生效（timedatectl set-timezone）；顶栏的时钟跟着走", "Applied immediately (timedatectl set-timezone); the top-bar clock follows along" },
    { "关掉无线可以省电；关着的时候一个网络也扫不到", "Turning Wi-Fi off saves power; while it is off no networks can be scanned" },
    { "扫描附近的无线网，选一个连（没连过的会让你输密码）", "Scan nearby networks and pick one (new ones ask for a password)" },
    { "开着才能用电脑传 ROM / 传封面 / 改文件（账号 root，端口 22）", "Must be on to copy ROMs, covers or files from a PC (user root, port 22)" },
    { "在电脑上就 ssh 这个串。电脑要和掌机连同一个 WiFi", "ssh this from a PC; the PC must be on the same Wi-Fi" },
    { "IPv4 地址（已滤掉 IPv6 长串）—— 传 ROM、开 SSH 都要先知道这个", "IPv4 address (IPv6 noise filtered out) — needed to copy ROMs or SSH in" },
    { "只重开这个界面，不动系统；退出前的页面会保留", "Restarts only this UI, not the system; you come back to the same page" },
    { "关机、重启设备、重启界面 —— 都在这一页（要按两次 A 确认）", "Power off, reboot and restart the UI are all on this page (each needs A twice)" },
    { "整机重启（systemctl reboot）。按 A 两次确认", "Reboot the device (systemctl reboot). Press A twice to confirm" },
    { "整机断电（systemctl poweroff）。按 A 两次确认", "Power off the device (systemctl poweroff). Press A twice to confirm" },
    { "读 /sys/class/power_supply 的 battery 节点（容量 + 充放电状态）", "Reads the battery node under /sys/class/power_supply (capacity and charge state)" },
    { "TF1 / TF2 的剩余空间，卡掉了或没挂上一眼能看出来", "Free space on TF1 and TF2 — spot an unmounted card at a glance" },
    { "直接写 backlight 的 brightness（按 max_brightness 换算成百分比）", "Writes the backlight brightness node, scaled to a percentage using max_brightness" },
    { "闲置多久自动息屏待机（背光灭 + 进待机，省电）。调试时嫌它老黑屏就选「关闭」。改完立即生效",
      "Idle minutes before the screen blanks and the device suspends. Pick 关闭 to keep debugging; applies immediately" },
    { "系统语言：中文 / English。改完自动重启前端", "UI language: 中文 / English. The UI restarts automatically" },

    /* 覆盖率自检补上的 3 条（build/check-i18n.py 报的缺口）——
       前两条在「电源 / 退出」那页当按钮用，第三条是系统音量的说明。 */
    { "重启界面", "Restart UI" },
    { "重启设备", "Reboot" },
    { "写 RK817 codec 的主音量，并 alsactl store 落盘",
      "Sets the RK817 codec master volume and persists it with alsactl store" },

    /* 索引（宿主 P 行）带过来的中文，以及底栏按键标签。
       前四条来自 vg_platforms.py 写进索引的 core/tag 字段，
       不是本文件里的字面量 —— 光扫源码抓不到，得手工登记。 */
    { "最近 30 款", "Last 30 games" },
    { "收藏置顶", "Pinned to top" },
    { "最近游玩", "Recently played" },
    { "X 键收藏", "X to favorite" },
    { "搜索", "Search" },
    { "清空", "Clear" },

    /* 游戏操作弹层的三项 + 状态字 */
    { "启动游戏", "Launch Game" },
    { "收藏游戏", "Favorite" },
    { "删除游戏", "Delete Game" },
    { "已收藏", "Favorited" },
    { "未收藏", "Not favorited" },
    { "从内存卡删除", "Erase from card" },
    { "再按一次确认", "Press again" },
    { "再按一次 A 确认删除，B 取消", "Press A again to delete, B to cancel" },
    { "↑↓ 选择 · A 确认 · B 返回", "↑↓ Select · A Confirm · B Back" },

    /* 摇杆灯（2026-09-15 用户要求）：菜单名 + 三项的标题和说明。 */
    { "摇杆灯", "Joystick LED" },
    { "灯光模式", "LED Mode" },
    { "灯光亮度", "LED Brightness" },
    { "灯光效果", "LED Effect" },
    { "两侧摇杆的 RGB 灯：颜色、亮度、灯效。出厂是随电量变色",
      "RGB lighting on both sticks: color, brightness and effect. The factory default follows battery level" },
    { "「电量」= 按电池状态变色（出厂）；选固定颜色后灯就一直是那个色，彩虹则是自动流动",
      "Battery follows the charge state (factory default); a fixed color stays put, and Rainbow animates on its own" },
    { "摇杆灯的亮度。夜里嫌刺眼往下调，最低一档几乎看不见但还留着指示",
      "Joystick LED brightness. Turn it down if it is harsh at night — the lowest step is faint but still visible" },
    { "灯的变化方式：常亮最省电；追光是颜色沿色环一格一格往前跑，炫彩是平滑的彩色流动；「电量」模式下这一项不生效",
      "How the LED animates. Solid uses the least power; Chase steps the color around the wheel, and Colorflow blends it smoothly. Ignored in Battery mode" },

    /* RA 主题（2026-09-16 用户要求）：菜单名 + 两项的标题和说明。 */
    { "RA 菜单风格", "RA Menu Style" },
    { "RA 主题配色", "RA Theme" },
    { "游戏里 RetroArch 菜单的整体外观。XMB 是横版大图标（配色主题最多，下面那项就挑它），出厂是 RGUI",
      "The look of RetroArch's in-game menu. XMB is the horizontal icon layout (most color themes — the item below picks them); the factory default is RGUI" },
    { "RetroArch 的配色主题（共 9 套）。只在「RA 菜单风格」选 XMB 时生效，其它风格用 RA 内置的那几档",
      "RetroArch color theme (9 included). Applies only when RA Menu Style is XMB; other styles use RetroArch's own palettes" },

    /* RA 帧数显示（2026-09-17 用户要求）：标题 + 说明。
       选项标签「关闭」TR[] 里已有（跟遮罩/虚拟内存/息屏共用），这里补「开启」。
       ⚠ TR[] 是**精确匹配** —— 这一行的中文必须跟 IT_ROOT 里 .hint 一个字都不差，
         否则英文界面下这句会露中文（我第一版源码写"所有 RA 核心"、这里写"所有走 RA 核心"，
         就不匹配了）。 */
    { "RA 帧数显示", "RA FPS Display" },
    { "游戏里画面左上角显示实时帧率（RA 的 OSD 文字），所有 RA 核心的平台一起生效。下次进游戏生效",
      "Shows real-time FPS in the top-left corner in-game (RetroArch OSD). Applies to all RA-core platforms. Takes effect next launch" },
    { "开启", "On" },

    /* 自动存档位（2026-09-17 用户要求）：菜单名 + 说明。
       值显示走 LF("状态槽 %d", ...) —— TR[] 是精确匹配，查不到格式串。 */
    { "自动存档位", "Auto-Save Slot" },
    { "「退出自动存档」把进度存到第几个状态槽，出厂 9（跟玩家手动的 0~5 位错开，免得盖掉手动存档）。下次进游戏生效",
      "Which state slot Exit Auto-Save writes to. Factory default is 9 — kept away from slots 0-5 that players use manually, so automatic saving cannot overwrite a manual save. Applies from the next launch" },
};
#define TR_N ((int)(sizeof(TR) / sizeof(TR[0])))

static const char *tr(const char *s)
{
    if (g_uilang == UI_ZH || !s || !*s) return s;

    int ascii = 1;
    for (const char *p = s; *p; p++)
        if ((unsigned char)*p >= 0x80) { ascii = 0; break; }
    if (ascii) return s;                 /* 没有中文可翻，别白扫一遍表 */

    for (int i = 0; i < TR_N; i++)
        if (strcmp(TR[i].zh, s) == 0) return TR[i].en;
    return s;                            /* 数据（游戏名等）原样放行 */
}

/* 拼出来的句子没法查表 —— "31 个平台 · 共 8394 款游戏" 这种组合无穷无尽，
   表里永远列不全。所以**模板**单独配一句英文格式串，用这个宏二选一。
   ⚠ 只给真正带数字的模板用；固定文案一律走 tr()，别两边都写。 */
#define LF(zh, en) (g_uilang == UI_EN ? (en) : (zh))

static SDL_Texture *bake(SDL_Renderer *r, TTF_Font *f, const char *s,
                         Uint32 rgb, int *w, int *h)
{
    if (!f || !s || !*s) {
        if (w) *w = 0;
        if (h) *h = 0;
        return NULL;
    }
    /* 界面语言：整个前端**唯一**的文案收口点，见上面那段注释。
       必须在取颜色之前 —— tr() 换的是字符串，跟颜色无关，但放这儿
       最早、最不容易被后面的早退分支跳过。 */
    s = tr(s);
    SDL_Color col = { (Uint8)(rgb >> 16), (Uint8)(rgb >> 8), (Uint8)rgb, 255 };
    SDL_Surface *sf = TTF_RenderUTF8_Blended(f, s, col);
    if (!sf) { if (w) *w = 0; if (h) *h = 0; return NULL; }
    SDL_Texture *t = SDL_CreateTextureFromSurface(r, sf);
    if (w) *w = sf->w;
    if (h) *h = sf->h;
    SDL_FreeSurface(sf);
    if (t) SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    return t;
}

static void blit(SDL_Renderer *r, SDL_Texture *t, int x, int y, int w, int h)
{
    if (!t || w <= 0 || h <= 0) return;
    SDL_Rect d = { x, y, w, h };
    SDL_RenderCopy(r, t, NULL, &d);
}

static void blit_c(SDL_Renderer *r, SDL_Texture *t, int cx, int y, int w, int h)
{
    blit(r, t, cx - w / 2, y, w, h);
}

static void blit_r(SDL_Renderer *r, SDL_Texture *t, int rx, int y, int w, int h)
{
    blit(r, t, rx - w, y, w, h);
}

/* ==========================================================================
 *  4. 界面状态
 * ==========================================================================*/
static SDL_Renderer *REN;
static SDL_Window   *WIN;
static Index         IX;

/* 分类页签（顺序与 vg_platforms.CATEGORIES 一致） */
static const char *CAT_IDS[] = { "all", "nintendo", "sega", "sony",
                                 "arcade", "handheld", "computer", "other" };
static const char *CAT_NAMES[] = { "全部", "任天堂", "世嘉", "索尼",
                                   "街机", "掌机", "电脑", "其他" };
#define NCAT ((int)(sizeof(CAT_IDS) / sizeof(CAT_IDS[0])))

static int g_cat = 0;          /* 当前分类下标 */
static int g_sel = 0;          /* 选中项在"过滤后列表"里的下标 */
static int g_scroll = 0;       /* 内容区垂直滚动量（像素） */

static int g_win_w, g_win_h;

/* --- 页面栈：网页版是 .page 的 display 切换，这里用同样的三页 -------------
 *   PAGE_HOME     = #pageHome     （平台网格）
 *   PAGE_GAMES    = #pageGames    （某个平台下的游戏列表）
 *   PAGE_SEARCH   = #pageSearch   （搜索：软键盘 + 结果列表）
 *   PAGE_SETTINGS = #pageSettings （设置，2026-09-14 补上；网页版早就有了，
 *                                  原生版一直缺，按 START 之前是空操作）
 *  和网页版一致：topbar 与底部按键条是四个页面共用的，tabs 只属于主页。 */
#define PAGE_HOME     0
#define PAGE_GAMES    1
#define PAGE_SEARCH   2
#define PAGE_SETTINGS 3
/* ★ 2026-09-16 加的第 5 页：「快捷键说明」（用户原话："在 C 里做一个可滚动的
   说明页"）。它由设置页根菜单里的一个按钮打开，是**全屏只读**页 ——
   不属于"网页版 .page 体系"（网页版根本没有这一页），所以状态只多一个
   "滚到第几行"。返回时回**设置页**（不是主页），光标还停在开它的那一项上。
   为什么不干脆做成一堆静态图片：图片要走 SDL_image + 单独的文件路径，
   换语言还得两套图；纯文本页跟着 g_uilang 自动切中英，固件里也不多一个文件。 */
#define PAGE_HELP     4

static int g_page = PAGE_HOME;
static int g_plat = -1;        /* 进入的游戏页对应 IX.p 的下标 */
static int g_gsel = 0;         /* 游戏页选中项 */
static int g_gscroll = 0;      /* 游戏页内容区滚动量（像素） */

/* --- 搜索页状态 ---------------------------------------------------------
 *  g_q 只存大写字母和数字 —— 软键盘上就这 36 个键，不存在别的东西，
 *  于是匹配时不用做任何 locale 相关的大小写处理。 */
static char  g_q[64];
static int   g_qlen = 0;
static int   g_kbd  = 0;       /* 软键盘光标 0..KB_TOTAL-1 */
static int   g_sfocus = 0;     /* 0 = 光标在软键盘上，1 = 在结果列表上 */
static int   g_rsel = 0;       /* 结果列表选中项 */
static int   g_rscroll = 0;    /* 结果列表滚动量（像素） */
static int   g_res_y = 0;      /* 结果列表起始 y，draw 时写回 */
static int   g_res_view = 0;   /* 结果列表可视高度，draw 时写回 */
static int   g_search_from = PAGE_HOME;  /* 从哪一页进来的，返回时回哪去
                                            （主页按 Y 和游戏页按 Y 都进搜索，
                                             退回游戏页要能接着看原来的列表） */

/* 结果项 = (平台下标, 该平台内第几个游戏)。SR_NMAX 条 = 4KB，一次 malloc。 */
typedef struct { int plat, gi; } SRes;
static SRes *g_res;
static int   g_res_n = 0;

/* 搜索页的两个函数在 draw_search_page() / state_load() 里就要用到，
   定义在 handle_key 那一段 —— 先声明。 */
static void search_run(void);
static void ensure_visible_search(void);

/* --- 设置页状态（实现见「5.7 设置页」那一段） ---------------------------
 * 菜单是**静态表**（跟网页版 SETTINGS_MENU 一一对应，见那段的大注释），
 * 这里只存"光标在哪"。
 *   g_setmenu   当前菜单下标（0=根）
 *   g_setrow    光标行
 *   g_settop    滚动偏移（行数，不是像素 —— 行高固定，按行滚更简单）
 *   g_setedit   0=普通 1=值编辑态 2=危险动作待确认（网页版 settingsCur.editing）
 *   g_setflash  底部状态行文案（应用设置的回执："音量设为 45%" 之类），
 *               非空时盖住 hint 显示，几秒后由 tick 清掉
 * 绘制函数在 render() 里被调用，而 render() 比实现那一段靠前 —— 先声明。 */
static void draw_settings_page(int clip_y1);
static void draw_help_page(int clip_y1);
static void settings_tick(void);
/* 说明页的四条。open/close 在 setting_confirm()（比正文早一千多行）里就要用，
   不先声明就是隐式声明 —— gcc 12 给警告，一开 -Werror 直接编不过。 */
static int  help_view_rows(void);
static void help_scroll(int lines);
static void help_open(void);
static void help_close(void);

/* --- 快捷键说明页状态（实现见「5.8 快捷键说明页」那一段） ---------------
 *  g_help_top  滚到第几**行**（不是像素）。行高固定，按行滚最省事 ——
 *              跟设置页的 g_settop 一个思路，绘制时按可视行数夹一次。
 *  这一页没有"光标"：它是只读的，↑↓ 直接滚内容。 */
static int g_help_top = 0;
/* 列数存档：sv_apply() 里要写它，但函数本体在下面 —— 先声明 */
static void sconf_save(void);
static void sconf_load(void);
static void set_flash(const char *s);
static void theme_load(void);
static void theme_scan(void);
/* 毛玻璃面板 + 背景层：实现见「6c. 背景层 + 毛玻璃面板」那一段，
   但 draw_tabs / draw_grid 这些绘制函数在它前面 —— 先声明。 */
static void glass_card(SDL_Rect rc, int rad, Uint32 tint, int a, Uint32 edge);
static void theme_set(const char *id);
/* 定义在"封面图"那一段（比这里晚），但平台卡片要用来铺自定义封面 */
static void draw_cover_fit(SDL_Texture *cov, int iw, int ih,
                           SDL_Rect cv, int r, Uint32 bg, SDL_Rect clip);
/* 定义在 cover_get 之后 —— 但 bake_all() 要调它 */
static void card_img_load(void);
/* 毛玻璃缓存的有效位。settings_open() 要用它来作废缓存，
   而缓存本体（含那几张缓冲纹理）定义在绘制那一段 —— 先声明。
   静态变量的 tentative definition 后面再跟真正的定义是合法的。 */
static int g_glass_valid;

/* 调试钩子 --keys 的按键序列（解析在 main）。
   真机上没有 xdotool 之类可以注入按键，这是唯一能自动跑通
   "软键盘输入 → 结果导航 → 启动"这条交互链的办法。
   字符表：L R U D 方向 / A 确认 / B 退格 / T TAB / Y 搜索。 */
static const char *g_keyseq = NULL;
static size_t      g_keyseq_i = 0;

/* 退出请求。三种退出对启动器都是"rc=0"，但日志里要能分清：
 *   0 = 还在跑
 *   1 = 用户按了退出（手柄 BACK）
 *   2 = 去启动游戏了（launch.sh 已写出，屏幕交给模拟器）
 * 定义在这里而不是手柄那一段，是因为 handle_key（游戏页按 A 启动）
 * 比手柄代码更靠前就需要写它。 */
static int g_quit = 0;

/* ==========================================================================
 *  游戏操作弹层（用户 2026-09-15）
 *
 *  按 A 打开，六项：启动游戏 / 收藏游戏 / 更换模拟器核心 / 着色器效果 /
 *                退出自动存档 / 删除游戏。
 *  删除是**真删卡上的源文件**（unlink），所以有两道确认 + 一条模式保险，
 *  细节见 act_key() 里那段注释。
 *  「更换模拟器核心」「着色器效果」会在这个面板里**原地切成二级列表**，
 *  见 draw_core_list() / draw_shader_list()。
 *
 *  ⚠ 2026-09-17 整个「加载存档」功能删除（用户：「弹层删除加载存档功能」）。
 *    它只能做"启动时把 RA 的 state_slot 指到第 N 位"，却要求玩家自己先理解
 *    RA 的位号（而且 RA 默认那一位 0 在旧列表里被叫成「关闭」，玩家手存的
 *    档根本选不到）。续档改由「退出自动存档」承担：平台级开关 + 一个全局的
 *    「自动存档位」（设置页可调 0..10，出厂 9），存和读用同一个位号，
 *    进游戏直接续上，玩家不用管位号。
 * ==========================================================================*/
#define ACT_START  0
#define ACT_FAV    1
#define ACT_CORE   2
#define ACT_SHADER 3    /* 着色器效果（用户 2026-09-16） */
#define ACT_AUTOSV 4    /* 退出自动存档（按平台生效，用户 2026-09-16） */
#define ACT_CHEAT  5    /* 金手指（用户 2026-09-18） */
#define ACT_DEL    6    /* 危险项永远排最后一个：加新项只能往它前面加 */
#define ACT_N      7

/* 弹层文案放**文件作用域** —— 二级列表 draw_core_list() 也要用，
   原来塞在 draw_act_popup() 里面，那个函数够不着。 */
static const char *const ACT_LABEL[ACT_N] = {
    "启动游戏", "收藏游戏", "更换模拟器核心", "着色器效果",
    "退出自动存档", "金手指", "删除游戏"
};
static const char *const ACT_FOOT[3] = {
    "↑↓ 选择 · A 确认 · B 返回",
    "再按一次 A 确认删除，B 取消",
    "↑↓ 选择 · ←→ 适用范围 · A 确认 · B 返回",
};
/* 二级列表第 0 行：这个选择要套在多大范围上 */
static const char *const SCOPE_LABEL     = "适用范围";
static const char *const SCOPE_VALUE[2]  = { "仅本游戏", "整个平台" };
/* 二级列表第 1 行：删掉覆盖、回到宿主那套（FORCE_CORE / es_systems.cfg） */
static const char *const AUTO_LABEL      = "自动（跟随平台默认）";

/* 金手指二级页（用户 2026-09-18）的按键提示。**不能照抄 ACT_FOOT[2]** ——
   那条写的是"←→ 适用范围"，而这一页第 1 行是开关，←→ 是"换值"不是换范围，
   照抄会出现一条对着空气说的提示（2026-09-16 着色器那页就踩过同一个坑）。 */
static const char *const CHEAT_FOOT = "↑↓ 选择 · ←→ 切换 · A 确认 · B 返回";
/* 二级列表第 1 行（总开关）的标签。**不能直接用 ACT_LABEL[ACT_CHEAT]** ——
   弹层里那项叫"金手指"没问题，但下面一溜具体功能条目的语境里，单独一行
   "金手指"会被当成分类标题（2026-09-18 截图核对发现的）。 */
static const char *const CHEAT_SW_LABEL  = "金手指总开关";

static int g_act_on  = 0;      /* 弹层开着 */
static int g_act_pi  = -1;     /* 针对 IX.p 里的第几个平台 */
static int g_act_gi  = -1;     /* 针对该平台的第几个游戏 */
static int g_act_sel = ACT_START;
static int g_act_ask = 0;      /* 1 = 删除待确认（再按一次 A 才真删） */

/* ---- 「更换模拟器核心」的二级列表（用户 2026-09-15）--------------------
   弹层选中 ACT_CORE 按 A 时展开。行序固定：
       第 0 行  适用范围（仅本游戏 / 整个平台）—— ←→ 或 A 就地切换
       第 1 行  自动（= 删掉覆盖，回到平台默认）
       第 2 行起 候选核心（来自索引的 C 行，宿主算好的"装了且不在黑名单"）
   选完立刻回主弹层，下一次启动游戏就按新核心走（宿主每次启动都重读文件）。 */
#define CORES_PATH "/opt/vagaretro/cores.txt"
/* 一个平台的候选上限。原为 12，2026-09-16 提到 16 —— C 行的候选改成
   **跨模拟器**收集之后数量涨了：Dreamcast 一家就有 11 项
   （standalone + retroarch32 的 3 个 + retroarch 的 2 个 + retrorun32 的 3 个
    + retrorun 的 2 个），12 已经贴脸了。 */
#define CORE_MAX   16
#define CORE_VIS   10          /* 一屏最多显示几行，超了滚动 */
static int  g_act_core  = 0;   /* 二级列表是否展开 */
static int  g_act_scope = 0;   /* 0 = 仅本游戏，1 = 整个平台 */
static int  g_act_csel  = 0;   /* 二级列表光标（行号，不是候选下标） */
static int  g_act_ctop  = 0;   /* 二级列表的滚动首行 */
static char g_act_ocur[80];    /* 本游戏当前生效的覆盖核心（空 = 没覆盖，走平台默认） */

/* ---- 「着色器效果」的二级列表（用户 2026-09-16）-------------------------
   用户原话："启动游戏弹层加个着色器功能"、"着色器就是核心生效 只要是同个
   核心都生效"。所以粒度是 **模拟器核心**，不是游戏、不是平台、更不是全局：
   同一 core 下的所有游戏共用一套，这正是 RA 自己的粒度。

   存盘格式（/opt/vagaretro/shaders.txt，一行一条，TAB 分隔）：
       <核心名>\t<预设相对路径>
   例：nestopia	crt/crt-cgwg-fast.glslp
   没有这个 core 的行 = 不启用着色器。

   ⚠ 为什么不写 RA 的 retroarch.cfg：
     ① 那份 cfg 是**全局**的，一写就是所有核心一起变，做不到"按核心"；
     ② 实测 --appendconfig 传 video_shader（路径类键）**会被丢成空串**
        （2026-09-16 shader-aptest3.log，普通键如 menu_driver 都正常）；
     ③ RA 1.22.2 自带 `--set-shader=PATH`（帮助原文："Path to a shader
        (preset) that will be loaded each time content is loaded...
        An empty argument will disable automatic shader presets"），
        启动器每次启动注入一个值，不用碰任何配置文件、退出即失效、零残留。
     所以前端只管写 shaders.txt，注入由 vagaretro-shell.py 做。

   ⚠⚠ 预设清单**只收实测能编译过的**（2026-09-16 真机 GLES 逐个验）：
     运行环境 = `Found GL context: "kms"` / Mali-G52 / OpenGL ES 3.2。
       编译失败、会被 RA 静默回退到 stock 的：
         crt/crt-caligari.glslp   3:140: L0001: Typename expected, found 'if'
         xbr/xbr-lv2.glslp        3:180: S0012: 全局变量初值必须是常量表达式
     其余 15 个全部 `Linking GLSL program` + `Loaded 1 program(s)` 通过。
     加新预设前**必须先在设备上跑一遍**（--set-shader + 看日志有没有
     "Failed to initialize shader, falling back to stock"），
     否则用户选了没反应，还会以为是他自己操作错了。 */
#define SHADERS_PATH "/opt/vagaretro/shaders.txt"
#define SHADER_MAXLINES 64     /* 最多记多少个核心的着色器 */
#define SHADER_N  16
#define SHADER_VIS 12          /* 一屏最多显示几行，超了滚动 */
static const char *const SHADER_LABEL[SHADER_N] = {
    "关闭", "双线性平滑", "抗锯齿", "抗锯齿 高级",
    "CRT 扫描线（快）", "CRT 扫描线", "CRT 遮罩", "CRT 纯扫描线",
    "CRT 电视机", "LCD 网格", "LCD 网格（快）", "GameBoy 绿",
    "NTSC 电视信号", "高斯模糊", "像素化", "缩放增强"
};
static const char *const SHADER_REL[SHADER_N] = {
    "",
    "bilinear.glslp",
    "anti-aliasing/fxaa.glslp",
    "anti-aliasing/advanced-aa.glslp",
    "crt/crt-cgwg-fast.glslp",
    "crt/crt-Cyclon.glslp",
    "crt/crt-aperture.glslp",
    "crt/GritsScanlines.glslp",
    "crt/crt-consumer-classic.glslp",
    "handheld/lcd-grid-v2.glslp",
    "handheld/zfast-lcd.glslp",
    "handheld/gameboy.glslp",
    "ntsc/ntsc.glslp",
    "blurs/gauss_4tap.glslp",
    "interpolation/pixellate.glslp",
    "scalefx/scalefx.glslp"
};
/* 二级列表的底部按键提示。**不能照抄 ACT_FOOT[2]** —— 那条里写着
   "←→ 适用范围"，而着色器是按核心生效的，压根没有"适用范围"这一行，
   照抄会出现一条对着空气说的提示（2026-09-16 截图核对时抓到的）。 */
static const char *const SHADER_FOOT = "↑↓ 选择 · A 确认 · B 返回";
static int  g_act_shader = 0;  /* 着色器二级列表是否展开 */
static int  g_act_ssel   = 0;  /* 光标（SHADER_LABEL 下标） */
static int  g_act_stop   = 0;  /* 滚动首行 */
static char g_act_scur[80];    /* 当前核心已绑的预设（空 = 关闭） */

/* ★ 2026-09-17：原来这里是「加载存档」二级列表的整套定义
   （SLOTS_PATH / SLOT_N / SLOT_MAXLINES / SLOT_LINE / SLOT_LABEL / SLOT_FOOT
     + g_act_slot / g_act_lsel / g_act_lcur 三个全局状态），
   已按用户要求整体删除（原话：「弹层删除加载存档功能」）。
   要接回来请参考 git 历史，或者照 draw_shader_list() 抄一份 ——
   那是同一个模式的现役实例。 */

/* 收藏列表：一行一条 `<平台id>\t<相对路径>`。
   为什么不塞进 vgfront.conf：那是"设置项"的键值表，收藏是**列表**，
   混在一起两边都难读；而且独立文件将来宿主想直接读也方便。 */
#define FAV_PATH "/opt/vagaretro/favorites.txt"

/* 游玩历史：一行一条 `<平台id>\t<相对路径>`，**最近的排最前**。
   上限 30 要和索引 P 行里「宿主写死的」最近 30 款对上，
   否则界面上写着 30 款、实际能列出 100 条，很怪。 */
#define HIST_PATH "/opt/vagaretro/history.txt"
#define HIST_MAX  30

static void act_open(int pi, int gi);
static void draw_act_popup(void);
/* 二级列表是 draw_act_popup 的"另一个版式"，本体写在它后面 ——
   所以这里必须前置声明（C 不认后面才定义的函数）。 */
static void draw_core_list(void);
static void draw_shader_list(void);
static void draw_cheat_list(void);
static int  act_cheat_key(Platform *p, SDL_Keycode k);
static void act_cheat_scroll(void);   /* act_cheat_refresh / --actsub 都要用它 */
static int  act_shader_key(Platform *p, SDL_Keycode k);
static const char *act_cur_core(Platform *p);
static int  act_key(SDL_Keycode k);
static void favorites_load(void);
static void history_load(void);
static void hist_record(const char *pid, const char *rel);

/* 非空时，render() 会盖一层"正在启动 XXX"的全屏遮罩。
   指向索引 arena 里的标题（地址稳定），所以不用拷贝。 */
static const char *g_launch_msg = NULL;

/* 游戏网格的实际起始 y —— 由 draw_games_head() 每帧写回。
   交给 ensure_visible 用，比按字号估算准（文字纹理的实高只有烘完才知道）。 */
static int g_games_grid_y = 0;

/* 过滤后的平台下标表 */
static int  *g_view;
static int   g_view_n;

/* 某个分类下有**几个有游戏的平台**。
   build_view() 摆卡片、draw_tabs() 报数字、cat_step() 跳空分类，三处共用
   这一份口径 —— 各写一遍迟早会漂（数字和卡片对不上就是这么来的）。 */
static int cat_count(int i)
{
    if (i < 0 || i >= NCAT) return 0;
    int n = 0;
    for (int k = 0; k < IX.np; k++) {
        if (i != 0 && strcmp(IX.p[k].cat, CAT_IDS[i]) != 0) continue;
        if (IX.p[k].ngames > 0) n++;
    }
    return n;
}

static void build_view(void)
{
    const char *want = CAT_IDS[g_cat];
    int all = (strcmp(want, "all") == 0);
    g_view_n = 0;
    for (int i = 0; i < IX.np; i++) {
        if (!all && strcmp(IX.p[i].cat, want) != 0) continue;
        /* 空平台不上主页（浪总 2026-09-16）：卡二里没放 ROM 的机种
           （PCE/NGPC/WS/WSC/MS/NAOMI/ATARI2600/ATARI7800/LYNX/PICO8/DOS/EASYRPG
           这 12 个）以前照样占一张卡片，点进去是一片空白网格 —— 看着像坏了。
           ⚠ history / favorite 是**虚拟平台**，索引里 G 行恒为 0，
           但 ngames 由 favorites_load()/history_load() 填，而这两者都在
           build_view() **之前**跑（见 main）→ 这里读到的是填好的值，
           不会把收藏/历史误判成空平台。 */
        if (IX.p[i].ngames <= 0) continue;
        g_view[g_view_n++] = i;
    }
    if (g_sel >= g_view_n) g_sel = g_view_n > 0 ? g_view_n - 1 : 0;
    if (g_sel < 0) g_sel = 0;
}

/* ==========================================================================
 *  5. 游戏页用的缓存与文本工具
 * ==========================================================================*/

/* --- 游戏名纹理：按需烘焙 + 直接映射缓存 ---------------------------------
 *  平台页的文字能在启动时一次烘完（31 个平台 x 5 处）。游戏页不行 ——
 *  光一个 FC 就 2600 款，全烘出来是上万个纹理，1GB 内存扛不住。
 *
 *  于是改成按需烘 + 512 槽直接映射缓存，key 取字符串内容的 64 位 hash。
 *
 *  **不能拿指针当 key**：调用方会把超长的名字截断进栈上的临时缓冲，
 *  那个地址每张卡都一样、内容却每次不同，按指针比对会把上一张卡的
 *  文字贴到这一张上。内容 hash 撞车概率 2^-64（真撞了也只是显示错一个
 *  名字，不会崩），换来的是省掉一次 strcmp。
 *
 *  槽位撞了直接顶掉旧的 —— 重建一个文字纹理只要几十微秒，
 *  远低于每帧重新 TTF 渲染一整屏。
 *  （同一个名字要用多种字号/颜色，所以 key 里连 font 和 rgb 一起比。）
 */
#define TC_N 512

static struct {
    unsigned long long key;      /* 字符串内容的 FNV-1a 64 */
    TTF_Font    *font;
    Uint32       rgb;
    SDL_Texture *t;
    int w, h;
} g_tc[TC_N];

static unsigned long long str_hash64(const char *s)
{
    unsigned long long h = 1469598103934665603ull;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        h ^= (unsigned long long)*p;
        h *= 1099511628211ull;
    }
    return h ? h : 1ull;         /* 0 留给"空槽" */
}

static unsigned tc_slot(unsigned long long k, TTF_Font *f, Uint32 rgb)
{
    unsigned long long v = k ^ ((unsigned long long)(size_t)f * 0x9e3779b97f4a7c15ull);
    v ^= (unsigned long long)rgb * 0xbf58476d1ce4e5b9ull;
    return (unsigned)((v ^ (v >> 29)) & (TC_N - 1));
}

static SDL_Texture *bake_cached(SDL_Renderer *r, TTF_Font *f, const char *s,
                                Uint32 rgb, int *w, int *h)
{
    if (!f || !s || !*s) { if (w) *w = 0; if (h) *h = 0; return NULL; }

    unsigned long long k = str_hash64(s);
    unsigned i = tc_slot(k, f, rgb);
    if (g_tc[i].t && g_tc[i].key == k && g_tc[i].font == f &&
        g_tc[i].rgb == rgb) {
        if (w) *w = g_tc[i].w;
        if (h) *h = g_tc[i].h;
        return g_tc[i].t;
    }

    int tw = 0, th = 0;
    SDL_Texture *t = bake(r, f, s, rgb, &tw, &th);
    if (!t) { if (w) *w = 0; if (h) *h = 0; return NULL; }

    if (g_tc[i].t) SDL_DestroyTexture(g_tc[i].t);
    g_tc[i].key = k;  g_tc[i].font = f; g_tc[i].rgb = rgb;
    g_tc[i].t = t;    g_tc[i].w = tw;   g_tc[i].h = th;
    if (w) *w = tw;
    if (h) *h = th;
    return t;
}

/* --- UTF-8 截断 ----------------------------------------------------------
 *  CSS 那边靠 text-overflow:ellipsis 收尾。SDL 没这功能，得自己来：
 *  按字符边界（UTF-8 里非首字节都是 10xxxxxx）累加量宽，放不下就在上一个
 *  边界收尾并补省略号。不截的话长名字会直接糊出卡片外面盖到邻居上。
 */
static void ellipsize(TTF_Font *f, const char *s, int maxw,
                      char *out, size_t cap)
{
    if (!f || !s || cap == 0) { if (cap) out[0] = '\0'; return; }
    if (*s == '\0') { out[0] = '\0'; return; }

    int w = 0, h = 0;
    if (TTF_SizeUTF8(f, s, &w, &h) == 0 && w <= maxw) {
        snprintf(out, cap, "%s", s);
        return;
    }

    int ellw = 0, ellh = 0;
    if (TTF_SizeUTF8(f, "…", &ellw, &ellh) != 0) ellw = 0;

    char   tmp[256];
    size_t n = strlen(s);
    size_t best = 0;

    for (size_t i = 1; i < n && i < sizeof(tmp) - 1; ) {
        size_t j = i + 1;                       /* 跳到下一个字符起点 */
        while (j < n && ((unsigned char)s[j] & 0xC0) == 0x80) j++;
        if (j >= sizeof(tmp)) break;

        memcpy(tmp, s, j);
        tmp[j] = '\0';
        int tw = 0, th = 0;
        if (TTF_SizeUTF8(f, tmp, &tw, &th) != 0) break;
        if (tw + ellw > maxw) break;

        best = j;
        i = j;
    }

    if (best == 0) { snprintf(out, cap, "…"); return; }
    memcpy(out, s, best);
    out[best] = '\0';
    if (best + 4 <= cap) strcat(out, "…");      /* "…" 是 3 字节 + NUL */
}

/* --- .game-name-overlay 的 linear-gradient(transparent, rgba(0,0,0,.8)) ---
 *  每张卡都要盖一层，尺寸还完全一样 —— 烘一次就够，之后一次 RenderCopy。
 */
static SDL_Texture *g_nov;
static int g_nov_w, g_nov_h;

static SDL_Texture *nov_tex(SDL_Renderer *r, int w, int h)
{
    if (g_nov && g_nov_w == w && g_nov_h == h) return g_nov;
    if (g_nov) { SDL_DestroyTexture(g_nov); g_nov = NULL; g_nov_w = g_nov_h = 0; }
    if (w <= 0 || h <= 0) return NULL;

    SDL_Surface *sf = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32,
                                                     SDL_PIXELFORMAT_ARGB8888);
    if (!sf) return NULL;
    SDL_LockSurface(sf);
    Uint32 *px = (Uint32 *)sf->pixels;
    int den = (h > 1) ? h - 1 : 1;
    for (int y = 0; y < h; y++) {
        int a = (int)(204.0 * (double)y / (double)den + 0.5);  /* .8*255 */
        Uint32 v = (Uint32)a << 24;
        for (int x = 0; x < w; x++) px[y * w + x] = v;
    }
    SDL_UnlockSurface(sf);

    g_nov = SDL_CreateTextureFromSurface(r, sf);
    SDL_FreeSurface(sf);
    if (g_nov) {
        SDL_SetTextureBlendMode(g_nov, SDL_BLENDMODE_BLEND);
        g_nov_w = w; g_nov_h = h;
    }
    return g_nov;
}

/* ==========================================================================
 *  5b. 烘焙所有静态文字（启动时一次）
 * ==========================================================================*/
static void bake_all(void)
{
    for (int i = 0; i < IX.np; i++) {
        Platform *p = &IX.p[i];
        p->t_logo = bake(REN, F_PLOGO, p->logo, 0xffffff, &p->lw, &p->lh);
        p->t_name = bake(REN, F_PNAME, p->name, C_TEXT, &p->nw, &p->nh);
        p->t_sub  = bake(REN, F_PSUB, p->sub, C_DIM, &p->sw, &p->sh);
        p->t_core = bake(REN, F_PCORE, p->core, C_ACCENT2, &p->cw, &p->ch);
        p->t_tag  = bake(REN, F_PTAG, p->tag, 0xffffff, &p->tw, &p->th);
    }

    /* 卡片文字行高：取实测最大纹理高。兜底是"字号 × 1.35"，
       但只要有任何一张卡的文字比它高就顶上去 —— 中文字形（"超任"）
       就是会比同字号的拉丁字母高，这正是文字重叠的根源。 */
    g_name_h = (int)(FS_UI(PN_FS)   * 1.35);
    g_sub_h  = (int)(FS_UI(PSUB_FS) * 1.35);
    for (int i = 0; i < IX.np; i++) {
        if (IX.p[i].nh > g_name_h) g_name_h = IX.p[i].nh;
        if (IX.p[i].sh > g_sub_h)  g_sub_h  = IX.p[i].sh;
    }
    g_name_h += S(2);
    g_sub_h  += S(2);

    /* 主题带的卡片封面图（如果这套主题配了 cd_cover_dir）。
       放在最后：它跟上面那两行行高互不影响，只是顺手铺一次图。 */
    card_img_load();
}

/* ==========================================================================
 *  6. 各区块绘制
 * ==========================================================================*/
static int topbar_h(void)
{
    int content = FS_UI(TIME_FS) + S(2) + FS_UI(TIMESUB_FS);
    int sb = S(SB_H);
    if (sb > content) content = sb;
    return S(TB_PAD_T) + content + S(TB_PAD_B);
}

/* ==========================================================================
 *  状态栏的实时数据（电量 / WiFi / 音量）
 *
 *  draw_topbar() **每帧**都跑，而 battery_pct() 要 opendir("/sys/class/power_supply")、
 *  alsa_volume() 要 fork 一个 amixer。谁都不能按帧来。
 *  统一在这里 1.5 秒刷一次 —— 电池和音量本来也不会秒变，显示延迟看不出来。
 * ========================================================================== */
/* 这两个的定义在文件后半段（那儿才是 sysfs / alsa 那一堆工具函数），先声明。 */
static int battery_pct(char *st, size_t stcap);
static int alsa_volume(void);
static int alsa_volume_refresh(void);   /* 状态栏用：强制重读，见 alsa_volume() 附近注释 */
static int sread_line(const char *path, char *out, size_t cap);   /* 充电判据要用 */

#define STATUS_POLL_MS 1500

static int    g_st_bat = -1;    /* 电量 0..100，-1 = 读不到 */
static int    g_st_chg = -1;    /* 充电：1 = 在充/已满，0 = 放电，-1 = 读不到 */
static int    g_st_vol = -1;    /* 音量 0..100，-1 = 没有可调控件 */
static int    g_st_wif = -1;    /* WiFi 链路质量 0..100，-1 = 未连接 */
static Uint32 g_st_t0  = 0;

/* wlan0 的连接状态 + 信号质量。**只用 fopen 读文件**，绝不 fork nmcli ——
   这个会被状态栏按帧问到（nmcli 起一次要 200ms+，按帧调界面直接死）。
   返回 -1 表示没连（或没有无线网卡）。 */
static int wifi_link_quality(void)
{
    FILE *f = fopen("/sys/class/net/wlan0/operstate", "r");
    if (!f) return -1;                          /* 没有 wlan0 */
    char b[32] = "";
    if (fgets(b, sizeof(b), f)) b[sizeof(b) - 1] = '\0';
    fclose(f);
    if (strncmp(b, "up", 2) != 0) return -1;    /* down / dormant / unknown */

    /* 连上了，再去 /proc/net/wireless 拿链路质量。
       行格式：  wlan0: 0000   52.  -58.  -256  0  0  0  0  0  0
       冒号后第一个数是 quality（内核这里通常报 0..70）。 */
    int q = -1;
    f = fopen("/proc/net/wireless", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "wlan", 4) != 0) continue;
            char *p = strchr(line, ':');
            if (!p) continue;
            int v = 0;
            if (sscanf(p + 1, " %d", &v) == 1) q = v;
            break;
        }
        fclose(f);
    }
    if (q < 0) return 60;                       /* 连上了但质量读不到：给个中间值 */
    if (q > 70) q = 70;
    return q * 100 / 70;
}

static void status_poll(void)
{
    Uint32 now = SDL_GetTicks();
    if (g_st_t0 && now - g_st_t0 < STATUS_POLL_MS) return;
    g_st_t0  = now;

    /* 电量：顺便把 battery 的 status 串带回来判充电 —— battery_pct() 本来就在读
       那个文件（扫到主电池节点时顺手抄 status），所以这里不额外多开文件。 */
    char st[32] = "";
    g_st_bat = battery_pct(st, sizeof(st));

    /* 充电判据两道（用户 2026-09-16："加个充电的时候电池变色，不然充电看不出来"）：
       ① battery/status = Charging / Full —— RK817 插上适配器就先变 Charging；
       ② ac / usb 的 online = 1 —— 兜住"插着但已充满"时 status 报 Not charging 的情况。 */
    int chg = st[0] ? 0 : -1;
    if (!strcasecmp(st, "Charging") || !strcasecmp(st, "Full")) chg = 1;
    if (chg != 1) {
        static const char *const ON[] = {
            "/sys/class/power_supply/ac/online",
            "/sys/class/power_supply/usb/online",
        };
        char b[8];
        for (int i = 0; i < 2; i++) {
            if (sread_line(ON[i], b, sizeof(b)) && atoi(b) > 0) { chg = 1; break; }
        }
    }
    g_st_chg = chg;

    /* ★ 必须走 refresh 版：实体音量键改的是 ALSA mixer（ogage → volume.sh），
       完全不经过前端；用缓存版的话状态栏永远停在开机那次的数字。 */
    g_st_vol = alsa_volume_refresh();
    g_st_wif = wifi_link_quality();
}

static void draw_topbar(int y0)
{
    int padL = S(TB_PAD_L);
    int cy = y0 + S(TB_PAD_T);

    /* 内容竖直中点 */
    int content = FS_UI(TIME_FS) + S(2) + FS_UI(TIMESUB_FS);
    int sb = S(SB_H);
    int h = (sb > content) ? sb : content;
    int mid = cy + h / 2;

    /* --- LOGO：CSS 用 background-clip:text 做渐变字，
           这里退一步用 accent->accent2 的双色近似（纹理一次成形，成本低） --- */
    static SDL_Texture *t_logo;
    static int lw = 0, lh = 0;
    if (!t_logo) t_logo = bake(REN, F_LOGO, "LANGOS", C_ACCENT, &lw, &lh);
    blit(REN, t_logo, padL, mid - lh / 2, lw, lh);

    /* --- 搜索提示：整块已移除（2026-09-16） ---
       顶栏这里先后画过三版"搜索游戏…"提示：
         ① 白底 + 可见描边的圆角卡片（alpha 180 / rad 18 / 边 C_LINE）；
         ② 用户："主页的搜索框可不可以沉浸一点" → 收成轻量胶囊
            （全圆角 / alpha 96 / 同色描边）；
         ③ 用户："电池旁边的搜索也融进背景比较好" + "装饰做小点"
            + "主要是框太长了" → 去掉面，只留一颗居中放的小放大镜 + 四个字
            （F_TIMESUB ≈13px、alpha 165）。
       最后用户拍板："算了直接去掉 电量旁边的搜索游戏吧 反正是装饰去掉吧"。
       —— 于是这一段整块删除：不画底、不画放大镜、不画占位文字。
       这个决定本来就站得住：它**从来不是输入框**，没有任何点击/焦点行为，
       按 Y 才是真搜索页（底栏那项「Y 搜索」）。既然是纯装饰，
       就别占顶栏那点注意力 —— 功能上的可发现性一点没丢。
       ⚠ 连带删掉的东西（都只为算这颗提示的居中位置而存在）：
         · 局部：logo_w、TB_RESERVE、zone_l/zone_r、t_ph/pw/ph、ic/gpad/gw/ix
         · 宏：  SB_MAXW / SB_RAD / SB_HINT_A（见文件顶部 TB_PAD_T 那一段）
         保留 SB_H（顶栏内容高度还是按它算）和 SB_FS（电量/音量数字在用）。
       顶栏现在就是：LOGO 靠左，电量 / 音量 / 时间靠右，中间留白。 */

    /* --- 右侧状态：电量 / 音量 / 时间 --- */
    static SDL_Texture *t_time, *t_sub;
    static int tw = 0, thh = 0, sw = 0, shh = 0;
    static char last_min[16] = "";

    time_t now = time(NULL);
    struct tm tmv = *localtime(&now);
    char buf[32];
    strftime(buf, sizeof(buf), "%H:%M", &tmv);

    if (!t_time || strcmp(buf, last_min) != 0) {
        /* 时间每分钟才变一次，变了才重烘，别每帧都建纹理 */
        if (t_time) SDL_DestroyTexture(t_time);
        strncpy(last_min, buf, sizeof(last_min) - 1);
        t_time = bake(REN, F_TIME, buf, C_TEXT, &tw, &thh);

        const char *wd[] = { "日","一","二","三","四","五","六" };
        char sub[48];
        snprintf(sub, sizeof(sub), "%04d-%02d-%02d %s",
                 tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                 wd[tmv.tm_wday]);
        if (t_sub) SDL_DestroyTexture(t_sub);
        t_sub = bake(REN, F_TIMESUB, sub, C_SUBTEXT, &sw, &shh);
    }

    int rx = g_win_w - padL;
    blit_r(REN, t_time, rx, mid - (thh + S(2) + shh) / 2, tw, thh);
    blit_r(REN, t_sub, rx, mid - (thh + S(2) + shh) / 2 + thh + S(2), sw, shh);

    /* --- 电量 / WiFi / 音量：都接真实数据，都带数字 ---------------------
       2026-09-16 用户反馈："电量图标太小不显示数字，WiFi 图标也太小"。
       旧版这块只复刻 CSS 形态：电池壳 S(26)xS(18)，而设计基准是 1920x1080、
       屏幕只有 960x720 → g_scale≈0.5，**上屏才 ~13x9 像素**，内部填充还是
       写死的 S(15)、既不接数据也没有数字；而且**根本没有 WiFi 图标** ——
       旁边那个三段递增的竖条其实是音量喇叭，只是形状像信号格才被当成了 WiFi。
       现在统一放大，电量按真实百分比填充 + 百分比数字，音量也带数字，
       另加一个真正的 WiFi 信号图标（连通=亮，断开=暗 + 一道斜杠）。
       数字用 F_SB（字号 SB_FS=32，与 STATUS_FS 同值）—— 顶部这组小字是同一套观感。 */
    status_poll();

    char s_bat[16], s_vol[16];
    if (g_st_bat < 0) snprintf(s_bat, sizeof(s_bat), "--");
    else              snprintf(s_bat, sizeof(s_bat), "%d%%", g_st_bat);
    if (g_st_vol < 0) snprintf(s_vol, sizeof(s_vol), "--");
    else              snprintf(s_vol, sizeof(s_vol), "%d%%", g_st_vol);

    /* 充电中把整组（外框 / 正极 / 填充 / 数字）换成 C_OK(亮绿 #43d17c)，
       放电时是原来的 C_SUBTEXT。用户 2026-09-16："加个充电的时候电池变色，
       不然充电看不出来"。
       ⚠ 数字纹理是有缓存的，只比较文本 —— 所以判据里**必须带上 g_st_chg**：
         插上充电器时电量数字往往没变，不带上就不会重烘，颜色也就不跟着变。 */
    const Uint32 bat_col = (g_st_chg == 1) ? C_OK : C_SUBTEXT;

    static SDL_Texture *t_bat, *t_vol;
    static int bwx = 0, bhx = 0, vwx = 0, vhx = 0;
    static char last_bat[16] = "", last_vol[16] = "";
    static int  last_bat_chg = -2;              /* -2 = 还没烘过 */
    if (!t_bat || strcmp(s_bat, last_bat) != 0 || g_st_chg != last_bat_chg) {
        if (t_bat) SDL_DestroyTexture(t_bat);
        snprintf(last_bat, sizeof(last_bat), "%s", s_bat);
        last_bat_chg = g_st_chg;
        t_bat = bake(REN, F_SB, s_bat, bat_col, &bwx, &bhx);
    }
    if (!t_vol || strcmp(s_vol, last_vol) != 0) {
        if (t_vol) SDL_DestroyTexture(t_vol);
        snprintf(last_vol, sizeof(last_vol), "%s", s_vol);
        t_vol = bake(REN, F_SB, s_vol, C_SUBTEXT, &vwx, &vhx);
    }

    const int BAT_W = S(58), BAT_H = S(34);      /* 上屏 ~29x17（旧版才 ~13x9） */
    const int SPK_W = S(16), SPK_H = S(30);
    const int WIF_W = S(40), WIF_H = S(32);
    const int GAP   = S(STATUS_GAP);             /* STATUS_GAP=24 → 上屏 12px */
    const int GAPIN = S(12);                     /* 图标与它旁边数字的间距 */
    (void)WIF_H;

    int bw2 = BAT_W + GAPIN + bwx;                     /* 电池 + 数字 */
    int vw2 = SPK_W + S(4) + 3 * S(7) + S(10) + vwx;   /* 喇叭 + 声波 + 数字 */
    int x = rx - tw - S(30) - (bw2 + GAP + WIF_W + GAP + vw2);

    /* ① 电量：外框 + 正极凸起 + 按真实百分比的填充 + 数字
          （颜色 = bat_col：充电中亮绿，放电是普通副标题色，见上面那段） */
    setc(REN, bat_col, 235);
    SDL_Rect bat = { x, mid - BAT_H / 2, BAT_W, BAT_H };
    SDL_RenderDrawRect(REN, &bat);
    SDL_Rect cap = { bat.x + bat.w + S(5), mid - S(9), S(5), S(18) };
    SDL_RenderFillRect(REN, &cap);
    if (g_st_bat > 0) {
        int iw = BAT_W - S(10);
        int fw = iw * (g_st_bat > 100 ? 100 : g_st_bat) / 100;
        if (fw < S(3)) fw = S(3);
        SDL_Rect lvl = { bat.x + S(5), bat.y + S(5), fw, BAT_H - S(10) };
        SDL_RenderFillRect(REN, &lvl);
    }
    blit(REN, t_bat, bat.x + BAT_W + S(12) + GAPIN, mid - bhx / 2, bwx, bhx);
    x += bw2 + GAP;

    /* ② WiFi：三段递增竖条 + 底部小点（连通）；断开则整体压暗并拉一道斜杠 */
    int wif_on = (g_st_wif >= 0);
    setc(REN, C_SUBTEXT, wif_on ? 235 : 80);
    for (int i = 0; i < 3; i++) {
        int hh = S(12) + i * S(10);
        SDL_Rect bar = { x + i * S(13), mid + S(16) - hh, S(9), hh };
        SDL_RenderFillRect(REN, &bar);
    }
    if (wif_on) {
        SDL_Rect dot = { x + S(3), mid + S(19), S(6), S(6) };
        SDL_RenderFillRect(REN, &dot);
    } else {
        SDL_RenderDrawLine(REN, x - S(3), mid + S(17), x + WIF_W, mid - S(17));
    }
    x += WIF_W + GAP;

    /* ③ 音量：喇叭主体 + 三段递增声波 + 数字 */
    setc(REN, C_SUBTEXT, 235);
    SDL_Rect spk = { x, mid - SPK_H / 2, SPK_W, SPK_H };
    SDL_RenderFillRect(REN, &spk);
    for (int i = 0; i < 3; i++) {
        int hh = S(11) + i * S(8);
        SDL_Rect wv = { x + SPK_W + S(5) + i * S(7), mid - hh / 2, S(4), hh };
        SDL_RenderFillRect(REN, &wv);
    }
    blit(REN, t_vol, x + SPK_W + S(5) + 3 * S(7) + S(10), mid - vhx / 2, vwx, vhx);
}

static int tabs_h(void)
{
    /* 标签高 = 上下 padding + 单行字高（按 font-size * 1.2 估） */
    int th = S(TAB_PADY) * 2 + (int)(FS_UI(TAB_FS) * 1.2);
    return th + S(TABS_PADB);
}

static void draw_tabs(int y0, int *out_ty, int *out_th)
{
    int padL = S(TB_PAD_L);
    int gap = S(TAB_GAP);
    int th = S(TAB_PADY) * 2 + (int)(FS_UI(TAB_FS) * 1.2);
    int x = padL;

    *out_ty = y0;
    *out_th = th;

    for (int i = 0; i < NCAT; i++) {
        /* 计数只数**有游戏的**平台（cat_count 与 build_view 同口径）。
           否则"全部 31"下面只摆 19 张卡片，数字和眼睛看到的对不上。 */
        int n = cat_count(i);

        /* 空分类整个不画（浪总 2026-09-16：没有游戏的卡片不要显示）——
           留着"其他 0"这种标签，点进去是一片空白网格，比不显示更像坏了。
           卡二上以后放了对应 ROM，ngames 一涨标签就自动回来。 */
        if (n <= 0) continue;

        /* ⚠ 这两个纹理**必须按值重建**，不能只烘一次：
           - 标签字色取决于"是不是当前分类"（i == g_cat），切分类时会变；
           - 计数取决于当前索引，索引刷新（宿主重写 index.tsv）后会变。
           原来只写了 `if (!cache[i])` —— 结果切分类后旧的高亮色留在屏幕上，
           计数也永远停在第一次烘的那个值上（`--cat 0` 直跳时第一帧 n 还是 0，
           于是整排 tab 都看不到数字）。 */
        static SDL_Texture *cache[16];
        static int cw[16], ch[16];
        static Uint32 ccol[16];
        Uint32 want_col = (i == g_cat) ? 0xffffff : C_SUBTEXT;
        if (!cache[i] || ccol[i] != want_col) {
            if (cache[i]) SDL_DestroyTexture(cache[i]);
            cache[i] = bake(REN, F_TAB, CAT_NAMES[i], want_col, &cw[i], &ch[i]);
            ccol[i] = want_col;
        }

        char nb[16];
        snprintf(nb, sizeof(nb), "%d", n);
        static SDL_Texture *nc[16];
        static int nw[16], nh[16], nval[16];
        if (!nc[i] || nval[i] != n) {
            if (nc[i]) SDL_DestroyTexture(nc[i]);
            nc[i] = bake(REN, F_TABB, nb, 0xffffff, &nw[i], &nh[i]);
            nval[i] = n;
        }

        int tw = cw[i] + S(8) + nw[i];
        int bw = tw + S(TAB_PADX) * 2;

        SDL_Rect r = { x, y0, bw, th };
        if (i == g_cat) {
            /* .active 是蓝色渐变。SDL 没有渐变填充，
               用两层实心叠一下近似（底亮 + 上暗）。
               选中的保持**实色** —— 玻璃底上再叠半透明，高亮就不突出了。 */
            draw_round(REN, r, S(TAB_RAD), C_ACCENT, 255);
            draw_round(REN, r, S(TAB_RAD), th_darken(C_ACCENT, 0.72f), 90);
        } else {
            glass_card(r, S(TAB_RAD), C_PANEL, 170, C_LINE);
        }

        /* 文字：图形居中，数量跟在后面（CSS 里 b 有 opacity .6） */
        int tx = x + (bw - tw) / 2;
        int ty = y0 + (th - ch[i]) / 2;
        blit(REN, cache[i], tx, ty, cw[i], ch[i]);
        if (i == g_cat) {
            SDL_SetTextureAlphaMod(nc[i], 153);   /* 0.6 */
            blit(REN, nc[i], tx + cw[i] + S(8),
                 y0 + (th - nh[i]) / 2, nw[i], nh[i]);
            SDL_SetTextureAlphaMod(nc[i], 255);
        } else {
            SDL_SetTextureAlphaMod(nc[i], 153);
            blit(REN, nc[i], tx + cw[i] + S(8),
                 y0 + (th - nh[i]) / 2, nw[i], nh[i]);
            SDL_SetTextureAlphaMod(nc[i], 255);
        }

        x += bw + gap;
    }
}

/* 内容区几何 */
static int card_w(void)
{
    int avail = g_win_w - 2 * S(CT_PAD_X);
    return (avail - (g_pg_cols - 1) * S(PG_GAP)) / g_pg_cols;
}

static int card_h(void)
{
    /* 两行文字的高度都用实测值（见 bake_all 末尾），不再用"字号×1.35"估 ——
       估小了第二行会压上来。
       副标题行可以被主题关掉（cd_show_sub=0）—— 关掉时高度也要跟着减，
       否则卡片底下空出一大块。 */
    int h = S(PA_H) + S(PM_PAD_T) + g_name_h + S(PM_PAD_B);
    if (g_cd[CD_SHOW_SUB]) h += S(8) + g_sub_h;
    return h;
}

static int grid_x(void) { return S(CT_PAD_X); }

static void draw_grid(int y0, int clip_y0, int clip_y1)
{
    int cw = card_w(), ch = card_h();
    int gx = grid_x(), gy = S(PG_GAP);
    int rows = (g_view_n + g_pg_cols - 1) / g_pg_cols;

    SDL_Rect clip = { 0, clip_y0, g_win_w, clip_y1 - clip_y0 };
    SDL_RenderSetClipRect(REN, &clip);

    for (int i = 0; i < g_view_n; i++) {
        Platform *p = &IX.p[g_view[i]];
        int col = i % g_pg_cols, row = i / g_pg_cols;
        int x = gx + col * (cw + S(PG_GAP));
        int y = y0 + row * (ch + gy) - g_scroll;

        if (y + ch < clip_y0 || y > clip_y1) continue;   /* 视口外跳过 */

        int on = (i == g_sel);
        SDL_Rect rc = { x, y, cw, ch };
        /* 卡片底也是玻璃。封面区本身是不透明的彩色渐变，玻璃只体现在
           下面那块 meta 区 —— 半透明之后能透出背景的光晕。 */
        /* 选中环（粗）：跟游戏列表页同一套做法，垫在卡片底下露出一圈 accent。
           和游戏卡保持一致 —— 两页的"当前选中"要是一粗一细，看着像两个前端。 */
        if (on) {
            int rb = S(SEL_RING);
            SDL_Rect ring = { rc.x - rb, rc.y - rb,
                              rc.w + 2 * rb, rc.h + 2 * rb };
            draw_round(REN, ring, S(PC_RAD) + rb, C_ACCENT, 255);
        }
        /* 卡片底的不透明度也能由主题调（cd_art_alpha）：选中时统一 +30、封顶 255 */
        int ca = g_cd[CD_ART_ALPHA] + (on ? 30 : 0);
        if (ca > 255) ca = 255;
        glass_card(rc, S(PC_RAD), C_PANEL, ca, on ? C_ACCENT : C_LINE);
        if (on) {
            /* 再罩一层淡 accent，让整张卡"亮"起来。
               glass_card 自带的 1px 边框在 4 寸屏上太细，镇不住选中这件事。 */
            draw_round(REN, rc, S(PC_RAD), C_ACCENT, 62);
        }

        /* 封面区：渐变 + 高光（启动时烘好的纹理）。
           内缩量固定按**最大描边**算 —— 否则选中/取消选中时尺寸一变，
           烘好的纹理就全作废了。

           ⚠ 封面颜色用索引里的**原始机种色**，不掺主题色。
             中间试过"混 45% 主题色让卡片跟着主题变"，用户看过之后
             要求恢复 —— 卡片那身颜色是机种的识别色（街机橙、PSP 蓝），
             那是它的身份，不该被主题改掉。主题换的是背景、面板、文字这些。 */
        int ib = S(PC_BW_ON);
        if (p->cimg) {
            /* 主题给这张卡配了封面图 → 直接铺图（等比裁切、圆角），
               渐变和机种大字都不画 —— 图本身就是封面。
               图里的内容是用户自己设计的，再糊一层大字只会打架。 */
            SDL_Rect cv = { x + ib, y + ib, cw - 2 * ib, S(PA_H) };
            draw_cover_fit(p->cimg, p->ciw, p->cih, cv, S(PC_RAD),
                           C_PANEL, clip);
        } else {
            if (!p->art || p->aw != cw - 2 * ib || p->ah != S(PA_H)) {
                if (p->art) SDL_DestroyTexture(p->art);
                p->aw = cw - 2 * ib;
                p->ah = S(PA_H);
                p->art = make_grad_art(REN, p->aw, p->ah, S(PC_RAD),
                                       p->c1, p->c2, 1);
            }
            blit(REN, p->art, x + ib, y + ib, p->aw, p->ah);

            /* 大 logo 文字（左对齐，padding-left 28）。
               主题可关掉（cd_show_logo）—— 只想要纯色封面、不要字的用。 */
            if (g_cd[CD_SHOW_LOGO]) {
                int ly = y + S(PC_BW) + (S(PA_H) - p->lh) / 2;
                blit(REN, p->t_logo, x + S(PC_BW) + S(PA_PADL), ly, p->lw, p->lh);
            }
        }

        /* 右上角数量徽标（cd_show_cnt）。缓存的 key 必须是**数字本身** ——
           按卡片下标缓存的话，一滚动数字就和缓存错位了。 */
        if (g_cd[CD_SHOW_CNT]) {
            char cnt[16];
            snprintf(cnt, sizeof(cnt), "%d", p->ngames);
            static char ckey[128][16];
            static SDL_Texture *tc[128];
            static int tcw[128], tch[128];
            int ci = i & 127;
            if (!tc[ci] || strcmp(ckey[ci], cnt) != 0) {
                if (tc[ci]) SDL_DestroyTexture(tc[ci]);
                snprintf(ckey[ci], sizeof(ckey[ci]), "%s", cnt);
                tc[ci] = bake(REN, F_PCNT, cnt, 0xffffff, &tcw[ci], &tch[ci]);
            }
            int bw2 = tcw[ci] + S(14) * 2;
            int bh2 = tch[ci] + S(6) * 2;
            SDL_Rect bg = { x + cw - S(14) - bw2, y + S(14), bw2, bh2 };
            draw_round(REN, bg, S(PCNT_RAD), 0x000000, 140);
            blit(REN, tc[ci], bg.x + S(14), bg.y + S(6), tcw[ci], tch[ci]);
        }

        /* 左下角平台标签（cd_show_tag） */
        if (g_cd[CD_SHOW_TAG]) {
            int tagw = p->tw + S(12) * 2;
            int tagh = p->th + S(5) * 2;
            SDL_Rect tg = { x + S(14), y + S(PA_H) - S(14) - tagh + ib,
                            tagw, tagh };
            draw_round(REN, tg, S(PTAG_RAD), 0x000000, 128);
            blit(REN, p->t_tag, tg.x + S(12), tg.y + S(5), p->tw, p->th);
        }

        /* 下半 meta 区 */
        int my = y + S(PC_BW) + S(PA_H) + S(PM_PAD_T);
        blit(REN, p->t_name, x + S(PM_PAD_X), my, p->nw, p->nh);

        /* 副标题行 + 核心名胶囊。整行可关（cd_show_sub）—— 关掉时
           card_h() 也会少算这一行的高度，卡片跟着变矮。 */
        if (g_cd[CD_SHOW_SUB]) {
            int sy = my + g_name_h + S(8);
            blit(REN, p->t_sub, x + S(PM_PAD_X), sy, p->sw, p->sh);

            int ccx = x + S(PM_PAD_X) + p->sw + S(PSUB_GAP);
            int ccw = p->cw + S(11) * 2;
            int cch = p->ch + S(4) * 2;
            SDL_Rect cb = { ccx, sy - S(4), ccw, cch };
            draw_round(REN, cb, S(PCORE_RAD), C_ACCENT2, 31);  /* rgba(...,.12) */
            blit(REN, p->t_core, cb.x + S(11), cb.y + S(4), p->cw, p->ch);
        }
    }

    SDL_RenderSetClipRect(REN, NULL);

    /* 滚动条（CSS 是 8px 宽的 thumb，颜色 --line） */
    if (rows > 0) {
        int view_h = clip_y1 - clip_y0;
        int total = rows * (ch + gy) - gy + S(PM_PAD_B);
        if (total > view_h) {
            int track_x = g_win_w - S(CT_PAD_X) + S(8);
            int bar_h = view_h * view_h / total;
            if (bar_h < S(24)) bar_h = S(24);
            int bar_y = clip_y0 + (view_h - bar_h) * g_scroll / (total - view_h);
            SDL_Rect tb = { track_x, bar_y, S(8), bar_h };
            draw_round(REN, tb, S(4), C_LINE, 255);
        }
    }
}

/* ==========================================================================
 *  6b. 游戏列表页（#pageGames）
 *
 *  对照 index.html：
 *    .content{padding-top:20px}              内容区左右仍是 40px
 *    .back-btn                                accent 16px，下间距 20px
 *    .game-header h1                          40px bold，下间距 8px
 *    .game-header .info                       15px，flex gap 20px
 *    .game-header{margin-bottom:24px}
 *    .game-chunk{grid-template-columns:repeat(5,1fr); gap:16px}
 *    .game-card{border-radius:12px; height = 列宽*4/3}
 *    .game-cover                              无封面时 bg2 底 + dim 居中字
 *    .game-name-overlay                       底部渐变 + 白字 13px
 * ==========================================================================*/

/* 分类 id -> 中文名（info 行「分类」那格用） */
static const char *cat_cn(const char *id)
{
    for (int i = 0; i < NCAT; i++)
        if (strcmp(CAT_IDS[i], id) == 0) return CAT_NAMES[i];
    return id;
}

static Platform *cur_plat(void)
{
    if (g_plat < 0 || g_plat >= IX.np) return NULL;
    return &IX.p[g_plat];
}

static int game_card_w(void)
{
    int avail = g_win_w - 2 * S(CT_PAD_X);
    return (avail - (g_game_cols - 1) * S(GAME_GAP)) / g_game_cols;
}

/* 高度按 syncCardMetrics() 的算法：列宽 * 4 / 3（CSS 里 aspect-ratio 不生效，
   真高度就是被这段 JS 写进 --card-h 的）。
   封面样式（设置页「游戏封面样式」）可切成正方形 1:1 —— 见 g_cover_sty。
   ⚠ 这里被 game_card_h() 的每个调用点共用（网格、滚动范围、翻页步长），
   所以切样式之后**所有**靠它算的东西都会跟着变，不用逐处改。 */
static int game_card_h(void)
{
    int w = game_card_w();
    return g_cover_sty == 1 ? w : w * 4 / 3;
}

/* 游戏网格的滚动范围（像素），和平台页那套一模一样 */
static int games_total_h(void)
{
    Platform *p = cur_plat();
    if (!p || p->ngames <= 0) return 0;
    int rows = (p->ngames + g_game_cols - 1) / g_game_cols;
    return rows * (game_card_h() + S(GAME_GAP)) - S(GAME_GAP);
}

/* 从内容区顶部画到网格起始位置，返回网格的 y */
static int draw_games_head(int y)
{
    Platform *p = cur_plat();
    if (!p) return y;

    int padL = S(CT_PAD_X);
    y += S(GPAD_T);                      /* .content 的 padding-top 只有这页有 */

    /* --- .back-btn「← 返回」 --- */
    static SDL_Texture *t_back;
    static int bw = 0, bh = 0;
    if (!t_back) t_back = bake(REN, F_BACK, "← 返回", C_ACCENT, &bw, &bh);
    blit(REN, t_back, padL, y, bw, bh);
    y += bh + S(GB_MB);

    /* --- .game-header h1 ---
       加一层 ellipsize：原来直接把 p->name 烘成纹理、宽度不限，
       字号一到 160% 就冲出右边界（2026-09-15 实测 160% 下
       "FC / NES 红白机" 的最后一个字被切掉）。页头标题是**数据**、
       长度不可控，必须收边。 */
    int h1w = 0, h1h = 0;
    char h1s[256];
    ellipsize(F_GH1, p->name, g_win_w - padL * 2, h1s, sizeof(h1s));
    SDL_Texture *t_h1 = bake_cached(REN, F_GH1, h1s, C_TEXT, &h1w, &h1h);
    blit(REN, t_h1, padL, y, h1w, h1h);
    y += h1h + S(GH_H1_MB);

    /* --- .info：<span>标签</span> 值，label 是 --dim，值跟随 --subtext ---
       网页版这里是「厂商/分组/扩展名/RA核心」，那几项来自 platform 对象；
       原生索引里对应得上的只有分类/核心/平台 id，另外补一个真实游戏数
       （总比照抄一份假数据强）。 */
    {
        static const char *LAB[4] = { "分类", "核心", "平台", "游戏" };
        char nb[48];
        snprintf(nb, sizeof(nb), LF("%d 款", "%d games"), p->ngames);
        const char *VAL[4] = { cat_cn(p->cat), p->core, p->pid, nb };

        int x = padL, ih = 0;
        for (int i = 0; i < 4; i++) {
            if (i) x += S(GH_INFO_GAP);
            int lw = 0, lh = 0, vw = 0, vh = 0;
            SDL_Texture *tl = bake_cached(REN, F_GINFO, LAB[i], C_DIM, &lw, &lh);
            SDL_Texture *tv = bake_cached(REN, F_GINFO, VAL[i], C_SUBTEXT, &vw, &vh);
            if (lh > ih) ih = lh;
            blit(REN, tl, x, y, lw, lh);
            x += lw + S(6);              /* span 后面本来就是空格 */
            blit(REN, tv, x, y, vw, vh);
            x += vw;
        }
        y += ih + S(GH_MB);
    }

    g_games_grid_y = y;
    return y;
}

/* ==========================================================================
 *  6c. 封面图：纹理缓存 + cover 缩放 + 圆角裁切
 *
 *  实测数据（native/imgbench.c，真机）：FC 的 3684 张封面平均 256×224 / 14.9 KB，
 *  PNG 解码 3.16 ms/张、最大 11.9 ms。一屏 30 张全同步加载约 95 ms ——
 *  而滚动时**真正新增的只有新进视口的那一行 5 张（约 16 ms）**，其余命中缓存。
 *  所以这里**不上后台线程**：同步加载 + 定容缓存就够，代码少一半、也没有竞态。
 *  （如果哪天换成 640×480 的 JPEG，单张 60 ms，那就必须异步了。）
 *
 *  槽位用"直接映射 + 路径 hash"，和文字缓存同一套路数，撞了顶掉旧的即可。
 *  256 槽按实测单张 239 KB 上限约 60 MB，但**只有真被看到的图才占槽**，
 *  实际稳态远低于此。
 * ==========================================================================*/
#define IC_N 256

static struct {
    char        *path;   /* 自己 strdup 的完整路径；NULL = 空槽 */
    Uint64       hash;
    SDL_Texture *tex;
    int w, h;
    int dead;            /* 1 = 确认加载失败过，别每帧重试同一个不存在的文件 */
} g_ic[IC_N];

static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static Uint64 path_hash(const char *s)
{
    Uint64 v = 1469598103934665603ull;              /* FNV-1a 64 */
    for (; *s; s++) { v ^= (unsigned char)*s; v *= 1099511628211ull; }
    return v;
}

static SDL_Texture *cover_get(const char *path, int *w, int *h)
{
    if (!path || !*path) return NULL;

    Uint64 hv = path_hash(path);
    unsigned i = (unsigned)(hv ^ (hv >> 32)) & (IC_N - 1);

    if (g_ic[i].path && g_ic[i].hash == hv && strcmp(g_ic[i].path, path) == 0) {
        if (g_ic[i].dead) return NULL;
        if (w) *w = g_ic[i].w;
        if (h) *h = g_ic[i].h;
        return g_ic[i].tex;
    }

    /* 未命中：腾槽再加载 */
    if (g_ic[i].tex)  SDL_DestroyTexture(g_ic[i].tex);
    if (g_ic[i].path) free(g_ic[i].path);
    g_ic[i].tex = NULL; g_ic[i].path = NULL; g_ic[i].w = g_ic[i].h = 0;
    g_ic[i].dead = 0;

    SDL_Surface *s = IMG_Load(path);
    if (!s) {
        /* 记一笔"这个没有"，否则每帧都会对同一个不存在的文件做一次 IO */
        g_ic[i].path = dup_str(path);
        g_ic[i].hash = hv;
        g_ic[i].dead = 1;
        return NULL;
    }

    SDL_Texture *t = SDL_CreateTextureFromSurface(REN, s);
    if (t) {
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        g_ic[i].path = dup_str(path);
        g_ic[i].hash = hv;
        g_ic[i].tex  = t;
        g_ic[i].w    = s->w;
        g_ic[i].h    = s->h;
        if (w) *w = s->w;
        if (h) *h = s->h;
    }
    SDL_FreeSurface(s);
    return t;
}

/* ---- 卡片封面图（主题自带的）--------------------------------------------
   在当前主题的封面图目录（g_card_cover_dir）里按**平台 id** 找图：
   fc.png / fc.jpg / sfc.webp …，先命中的扩展名赢；一个都没有时再看
   _default.<ext> —— 那一张会铺给所有没单独配图的卡片。

   ⚠ 只在启动时跑一次（bake_all 末尾）：每个平台最多 5 次 access()，
     几十个平台也就百来次，比每帧去 stat 便宜得多。
   ⚠ 图**不预缩放**，直接交给 draw_cover_fit 按 cover 语义贴
     （等比放大铺满、多的裁掉，不变形）。用户丢一张 4K 进去也不会有问题，
     缩放那一下是 GPU 干的。 */
static const char *const CIMG_EXT[] = { "png", "jpg", "jpeg", "webp", "bmp" };
#define CIMG_EXT_N ((int)(sizeof(CIMG_EXT) / sizeof(CIMG_EXT[0])))

/* 找一个平台的封面图。pid 原样试一遍，再拿**全小写**试一遍 ——
   索引里的 pid 是大写的（FC / SFC / PS1），而人给文件起名时几乎都写小写，
   不该为了一个大小写就让人对着"图没生效"发半天呆。 */
static int cimg_find(const char *pid, char *out, size_t cap)
{
    if (!g_card_cover_dir[0] || !pid || !*pid) return 0;

    char lower[64];
    int n = (int)strlen(pid);
    if (n > (int)sizeof(lower) - 1) n = (int)sizeof(lower) - 1;
    for (int i = 0; i < n; i++) {
        char ch = pid[i];
        lower[i] = (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
    }
    lower[n] = '\0';
    int differ = (strcmp(lower, pid) != 0);

    for (int i = 0; i < CIMG_EXT_N; i++) {
        snprintf(out, cap, "%s/%s.%s", g_card_cover_dir, pid, CIMG_EXT[i]);
        if (access(out, R_OK) == 0) return 1;
        if (differ) {
            snprintf(out, cap, "%s/%s.%s", g_card_cover_dir, lower, CIMG_EXT[i]);
            if (access(out, R_OK) == 0) return 1;
        }
    }
    return 0;
}

static void card_img_load(void)
{
    if (!g_card_cover_dir[0]) return;

    char def[320];
    int have_def = cimg_find("_default", def, sizeof(def));

    int hit = 0;
    for (int i = 0; i < IX.np; i++) {
        Platform *p = &IX.p[i];
        char path[320];
        const char *use = NULL;
        if (cimg_find(p->pid, path, sizeof(path))) use = path;
        else if (have_def)                        use = def;
        if (!use) continue;

        int w = 0, h = 0;
        SDL_Texture *t = cover_get(use, &w, &h);
        if (!t || w <= 0 || h <= 0) continue;
        p->cimg = t;
        p->ciw  = w;
        p->cih  = h;
        hit++;
    }
    printf("[卡片图] %s → %d/%d 个平台用上了自定义封面（%s默认图）\n",
           g_card_cover_dir, hit, IX.np, have_def ? "含" : "无");
    fflush(stdout);
}

/* 封面路径，两条路按优先级来：
 *
 *   ① gamelist.xml 的 <image>（索引 v3 起由宿主写进 G 行第 4 列）。
 *      **这条最权威** —— 街机封面名带 _screenscraper_mix_arrm 后缀、PS1 封面在
 *      previews/ 下，靠文件名规则永远猜不出来，只有 gamelist 写着真名。
 *      宿主写进来之前已经确认过文件存在，所以这里不再 stat。
 *
 *   ② 退回老规则 <封面目录>/<游戏文件名去扩展名>.<ext>。扩展名要挨个试 ——
 *      gamegear 那 105 张封面全是 .jpg，只认 .png 的话整个平台都是空白。
 *
 * 两个仍然成立的注意点：
 *   ① 图片**平铺在平台根下的封面目录**里 —— 哪怕游戏本身在 "01-动作游戏/"
 *      这种子目录里（真机核对过），所以只取 basename，不要拼 rel 的目录部分；
 *   ② 封面目录必须用索引给的真实路径，不能拿 <rom_root>/<pid> 自己拼 ——
 *      卡二上的目录名不一定等于 pid。
 */
static const char *const COVER_EXT[] = {
    ".png", ".jpg", ".jpeg", ".webp", ".bmp"
};

static void cover_path(const Platform *p, Game *g, char *out, size_t n)
{
    out[0] = '\0';

    if (g->img && *g->img) {              /* ① gamelist 的 <image> */
        snprintf(out, n, "%s", g->img);
        return;
    }

    if (!p->imgdir) return;

    if (!g->cov) {                        /* ② 按文件名猜，只算一次 */
        const char *base = strrchr(g->rel, '/');
        base = base ? base + 1 : g->rel;

        char stem[300];
        snprintf(stem, sizeof(stem), "%s", base);
        char *dot = strrchr(stem, '.');   /* 只砍最后一个扩展名 */
        if (dot && dot > stem) *dot = '\0';

        const char *hit = "";
        for (size_t i = 0; i < sizeof(COVER_EXT) / sizeof(COVER_EXT[0]); i++) {
            char probe[600];
            snprintf(probe, sizeof(probe), "%s/%s%s",
                     p->imgdir, stem, COVER_EXT[i]);
            if (access(probe, R_OK) == 0) { hit = probe; break; }
        }
        g->cov = dup_str(hit);
    }
    snprintf(out, n, "%s", g->cov);
}

/* --- 四角遮罩：把封面溢出圆角的那四个角涂回卡片底色 --------------------
 *
 *  .game-cover 是矩形，图片按 cover 铺满之后四个角是**直角**，
 *  而 CSS 靠 .game-card{overflow:hidden; border-radius:12px} 把它们裁掉。
 *  SDL 2.0.10 没有 RenderGeometry、也没法给纹理套圆角，所以反过来做：
 *  烘一张 r×r 的"角遮罩"（圆角**外侧**涂卡片底色、内侧透明），
 *  贴到图片四个角；另外三个角用 RenderCopyEx 翻转复用同一张纹理。
 *  尺寸只有一种，烘一次就够。
 */
static SDL_Texture *g_cn_tex = NULL;
static int          g_cn_r   = 0;
static Uint32       g_cn_bg  = 0xffffff;

static SDL_Texture *corner_mask(SDL_Renderer *ren, int r, Uint32 bg)
{
    if (r <= 0) return NULL;
    if (g_cn_tex && g_cn_r == r && g_cn_bg == bg) return g_cn_tex;
    if (g_cn_tex) { SDL_DestroyTexture(g_cn_tex); g_cn_tex = NULL; }

    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(
        0, r, r, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!s) return NULL;

    Uint32 opaque = 0xff000000u | (bg & 0xffffffu);
    for (int y = 0; y < r; y++) {
        Uint32 *row = (Uint32 *)((Uint8 *)s->pixels + y * s->pitch);
        for (int x = 0; x < r; x++) {
            /* 圆心落在本块的右下角 —— 也就是这个角被圆角切掉的那块区域 */
            float dx = (float)(x - (r - 1)), dy = (float)(y - (r - 1));
            float d  = sqrtf(dx * dx + dy * dy);
            row[x] = (d <= (float)r - 0.5f) ? 0u : opaque;
        }
    }

    g_cn_tex = SDL_CreateTextureFromSurface(ren, s);
    SDL_FreeSurface(s);
    if (g_cn_tex) {
        SDL_SetTextureBlendMode(g_cn_tex, SDL_BLENDMODE_BLEND);
        g_cn_r = r;
        g_cn_bg = bg;
    }
    return g_cn_tex;
}

/* 把封面**拉满**整个 cv（非等比拉伸），然后把四角裁回圆角。
   clip 是当前的视口裁剪框 —— 卡片可能上下各露出一半，不能画到视口外。

   ★ 2026-09-16 用户选的是"拉满变形"：原来是 object-fit:cover（等比放大到
     填满、多出来的边缘切掉），横版街机封面塞进 3:4 的框里左右会被切掉一大
     截、竖版封面则被切上下。现在直接拉伸到刚好盖住 cv —— **整张图一定看全**，
     代价是比例失真（横图被压扁、竖图被拉长），这是刻意换来的。 */
static void draw_cover_fit(SDL_Texture *cov, int iw, int ih,
                           SDL_Rect cv, int r, Uint32 bg, SDL_Rect clip)
{
    SDL_Rect inter;
    if (!SDL_IntersectRect(&clip, &cv, &inter)) return;

    SDL_RenderSetClipRect(REN, &inter);

    (void)iw; (void)ih;                    /* 拉满就不需要原图尺寸了 */
    SDL_RenderCopy(REN, cov, NULL, &cv);

    SDL_Texture *m = corner_mask(REN, r, bg);
    if (m) {
        SDL_Rect tl = { cv.x,               cv.y,               r, r };
        SDL_Rect tr = { cv.x + cv.w - r,    cv.y,               r, r };
        SDL_Rect bl = { cv.x,               cv.y + cv.h - r,    r, r };
        SDL_Rect br = { cv.x + cv.w - r,    cv.y + cv.h - r,    r, r };
        SDL_RenderCopy(REN, m, NULL, &tl);
        SDL_RenderCopyEx(REN, m, NULL, &tr, 0, NULL, SDL_FLIP_HORIZONTAL);
        SDL_RenderCopyEx(REN, m, NULL, &bl, 0, NULL, SDL_FLIP_VERTICAL);
        SDL_RenderCopyEx(REN, m, NULL, &br, 0, NULL,
                         (SDL_RendererFlip)(SDL_FLIP_HORIZONTAL |
                                            SDL_FLIP_VERTICAL));
    }

    SDL_RenderSetClipRect(REN, &clip);   /* 还原成整个内容区 */
}

static void draw_game_cards(int y0, int clip_y0, int clip_y1)
{
    Platform *p = cur_plat();
    if (!p) return;

    int gx = S(CT_PAD_X);
    int cw = game_card_w(), ch = game_card_h();
    int gy = S(GAME_GAP);
    int ib = S(GC_BW_ON);                 /* 内缩按最大描边算，纹理才不用重做 */

    SDL_Rect clip = { 0, clip_y0, g_win_w, clip_y1 - clip_y0 };
    SDL_RenderSetClipRect(REN, &clip);

    if (p->ngames <= 0) {
        static SDL_Texture *t_empty;
        static int ew = 0, eh = 0;
        if (!t_empty) t_empty = bake(REN, F_GCOVER, "没有找到游戏", C_DIM, &ew, &eh);
        blit(REN, t_empty, gx, y0 + S(24), ew, eh);
        SDL_RenderSetClipRect(REN, NULL);
        return;
    }

    for (int i = 0; i < p->ngames; i++) {
        int col = i % g_game_cols, row = i / g_game_cols;
        int x = gx + col * (cw + S(GAME_GAP));
        int y = y0 + row * (ch + gy) - g_gscroll;
        if (y + ch < clip_y0 || y > clip_y1) continue;   /* 视口外不碰 */

        /* 弹层开着时不画选中环 —— 模态弹层背后再留一个焦点环，
           人会以为"焦点没进弹层"（2026-09-15 用户原话）。 */
        int on = (i == g_gsel) && !g_act_on;
        SDL_Rect rc = { x, y, cw, ch };
        /* 选中环（粗）：垫在卡片底下，卡片盖上去后四周露出一圈实色 accent。
           必须在 glass_card **之前**画 —— 反过来会把封面图盖掉。 */
        if (on) {
            int rb = S(SEL_RING);
            SDL_Rect ring = { rc.x - rb, rc.y - rb,
                              rc.w + 2 * rb, rc.h + 2 * rb };
            draw_round(REN, ring, S(GC_RAD) + rb, C_ACCENT, 255);
        }
        glass_card(rc, S(GC_RAD), C_PANEL, on ? 205 : 175,
                   on ? C_ACCENT : C_LINE);
        if (on) draw_round(REN, rc, S(GC_RAD), C_ACCENT, 62);

        /* .game-cover 底色（--bg2）、圆角内缩一层 */
        SDL_Rect cv = { x + ib, y + ib, cw - 2 * ib, ch - 2 * ib };
        int cr = S(GC_RAD) - ib > 0 ? S(GC_RAD) - ib : 0;
        draw_round(REN, cv, cr, C_BG2, 255);

        const char *title = p->games[i].title;

        /* 封面图：有就按 object-fit:cover 铺满、四角裁圆角；
           没有才退回 CSS 的"名字居中"（.game-cover 本身是个 flex 居中容器，
           这段兜底文案正是它存在的意义）。 */
        char cpath[600];
        cover_path(p, &p->games[i], cpath, sizeof(cpath));

        int iw = 0, ih = 0;
        SDL_Texture *cov = cover_get(cpath, &iw, &ih);
        char cut[256];
        int tw = 0, th = 0;
        SDL_Texture *t = NULL;

        if (cov && iw > 0 && ih > 0) {
            draw_cover_fit(cov, iw, ih, cv, cr, C_BG2, clip);
        } else {
            ellipsize(F_GCOVER, title, cv.w - S(16), cut, sizeof(cut));
            t = bake_cached(REN, F_GCOVER, cut, C_DIM, &tw, &th);
            blit(REN, t, cv.x + (cv.w - tw) / 2, cv.y + (cv.h - th) / 2, tw, th);
        }

        /* 底部 .game-name-overlay：渐变遮罩 + 白色单行名字 */
        int oh = S(GNOV_PADT) + (int)(FS_G(GNOV_FS) * 1.35) + S(GNOV_PADB);
        SDL_Texture *g = nov_tex(REN, cv.w, oh);
        if (g) {
            SDL_Rect dr = { cv.x, cv.y + cv.h - oh, cv.w, oh };
            SDL_RenderCopy(REN, g, NULL, &dr);
        }

        char cut2[256];                   /* 独立缓冲：上面那个还要当缓存 key 用 */
        ellipsize(F_GNOV, title, cv.w - S(GNOV_PADX) * 2, cut2, sizeof(cut2));
        tw = th = 0;
        t = bake_cached(REN, F_GNOV, cut2, 0xffffff, &tw, &th);
        blit(REN, t, cv.x + (cv.w - tw) / 2,
             cv.y + cv.h - S(GNOV_PADB) - th, tw, th);
    }

    SDL_RenderSetClipRect(REN, NULL);

    /* 滚动条（和平台页同一套算法） */
    int total = games_total_h();
    int view_h = clip_y1 - clip_y0;
    if (total > view_h && view_h > 0) {
        int track_x = g_win_w - S(CT_PAD_X) + S(8);
        int bar_h = view_h * view_h / total;
        if (bar_h < S(24)) bar_h = S(24);
        int bar_y = clip_y0 + (view_h - bar_h) * g_gscroll / (total - view_h);
        SDL_Rect tb = { track_x, bar_y, S(8), bar_h };
        draw_round(REN, tb, S(4), C_LINE, 255);
    }
}

/* 底栏（.padbar-container 固定贴底）会占掉的垂直空间。
   内容区到 g_win_h - padbar_zone() 为止，免得卡片钻到按键条底下。 */
static int padbar_zone(void)
{
    return S(PB_MB) + S(PB_PADY) * 2 + (int)(FS_UI(PBK_FS) * 1.35) + S(20);
}

/* 底栏按键条。数量得是宏 —— static 数组的长度要求编译期常量，
   用 `const int N` 会报 "storage size isn't constant"。

   ⚠ 这里写什么就得真是什么。原来那版写着「SEL 分类 / STA 设置」，
   实际 SEL 是**退出前端**、STA 才是切分类 —— 用户照着底栏按 SEL 想切
   分类，结果前端直接退了，正是"总容易误触"的一半来源（2026-09-14 修正）。
   没实现的东西宁可不标，也不要标了按下去没反应。

   2026-09-14 补了设置页之后：STA 真的进设置了（所以它现在能标出来），
   但设置页里键的语义和主页**完全不同**（那儿没有搜索、没有分类，
   A/B 也从"进入/返回"变成"确认/返回"），所以底栏按页换成两套 ——
   一张永远正确的键位表，比一张"大体上对"的有用得多。 */
typedef struct { const char *k; Uint32 col; const char *label; } PadHint;

#define PADBAR_N  5      /* 主页/游戏页/搜索页 */
#define PADBAR_NS 4      /* 设置页 */

/* ⚠ 2026-09-15 去掉原来的第 6 项「SEL 退出」（用户：「主界面 select 退出
   这个可以删掉了」）。理由不是"看着多余"，是它**真的是个坑**：
   前端已经完全接管启动链路（ES 不再参与），在主页按 SEL 退出去，
   emulationstation.sh 那个 while 循环会原地把前端再拉起来 ——
   唯一的效果是把「上次停在哪一页」的续页状态删掉（main 里 g_quit==1
   会 unlink(STATE_PATH)），也就是误触一下就丢进度。
   真要走人有「设置 → 电源 / 退出」，那儿是写了字的按钮。
   底栏的规矩不变：写什么就得真是什么，所以键绑定也一并撤掉
   （见 handle_pad 的 SDL_CONTROLLER_BUTTON_BACK 分支）。 */
static const PadHint MAIN_HINTS[PADBAR_N] = {
    { "A",   0xf5a623, "进入" },
    { "B",   0xe74c3c, "返回" },
    { "Y",   0x2ecc71, "搜索" },
    { "L/R", 0x5a6478, "分类" },
    { "STA", 0x8e44ad, "设置" },
};

/* 当前菜单里有没有可调项。由 draw_settings_page() 每帧算一次 ——
   它比 draw_padbar() 先跑，所以底栏拿到的一定是本帧的值。
   ⚠ 不在这里直接问 cur_menu()：那个函数和 SMenu 类型都定义在后面，
   底栏这段在前（编译期就得知道类型），会编不过。 */
static int g_set_has_adj = 0;

static const PadHint SET_HINTS[PADBAR_NS] = {
    { "↑↓", 0x5a6478, "选择" },
    { "←→", 0x2ecc71, "调值" },
    { "A",  0xf5a623, "确认" },
    { "B",  0xe74c3c, "返回" },
};

/* 菜单里一个可调项都没有时用的那套 —— 退出菜单整页是按钮，
   标着"←→ 调值"会让人去按左右键试（跟之前底栏标 SEL 分类其实是退出
   是同一类毛病：写了什么就得真是什么）。 */
static const PadHint SET_HINTS_NOADJ[3] = {
    { "↑↓", 0x5a6478, "选择" },
    { "A",  0xf5a623, "确认" },
    { "B",  0xe74c3c, "返回" },
};

/* 快捷键说明页（2026-09-16）。只读页：没有"选择"也没有"确认"，
   能做的就这三件 —— 所以键位表也照实只写这三件。 */
#define PADBAR_NH 3
static const PadHint HELP_HINTS[PADBAR_NH] = {
    { "↑↓",  0x5a6478, "滚动" },
    { "L/R", 0x2ecc71, "翻页" },
    { "B",   0xe74c3c, "返回" },
};

/* 纹理缓存。两套表各一份 —— 键帽的烘制跟字体/颜色绑定，
   混用一份缓存会在切页时闪出上一页的键帽。 */
typedef struct {
    SDL_Texture *tk[PADBAR_N], *tl[PADBAR_N];
    int tkw[PADBAR_N], tkh[PADBAR_N], tlw[PADBAR_N], tlh[PADBAR_N];
} PadCache;

static void padbar_draw(const PadHint *items, int n, PadCache *c)
{
    const int gap = S(PB_GAP), padx = S(PB_PADX);

    /* 先量一遍总宽和最大高度，才能居中 */
    int total = 0, bh = 0;
    for (int i = 0; i < n; i++) {
        if (!c->tk[i]) c->tk[i] = bake(REN, F_PBK, items[i].k, 0xffffff, &c->tkw[i], &c->tkh[i]);
        if (!c->tl[i]) c->tl[i] = bake(REN, F_PBI, items[i].label, C_SUBTEXT, &c->tlw[i], &c->tlh[i]);
        int kh = c->tkh[i] + S(PBK_PADY) * 2;
        if (kh > bh) bh = kh;
        if (c->tlh[i] > bh) bh = c->tlh[i];
        total += c->tkw[i] + S(PBK_PADX) * 2 + S(10) + c->tlw[i];
        if (i) total += gap;
    }
    int bw = total + padx * 2;
    bh += S(PB_PADY) * 2;

    int bx = (g_win_w - bw) / 2;
    int by = g_win_h - S(PB_MB) - bh;

    SDL_Rect bg = { bx, by, bw, bh };
    /* 底栏也是玻璃。alpha 比卡片高（210）：这底下压着主页的卡片和文字，
       太透会让按键提示和卡片字糊成一片。键帽本身保持实色 ——
       彩色键帽是这一条上唯一需要"跳出来"的东西。 */
    glass_card(bg, S(PB_RAD), C_BG2, 210, C_LINE);

    int x = bx + padx;
    for (int i = 0; i < n; i++) {
        int kw = c->tkw[i] + S(PBK_PADX) * 2;
        int kh = c->tkh[i] + S(PBK_PADY) * 2;
        SDL_Rect kr = { x, by + (bh - kh) / 2, kw, kh };
        draw_round(REN, kr, S(PBK_RAD), items[i].col, 255);
        blit(REN, c->tk[i], kr.x + S(PBK_PADX), kr.y + S(PBK_PADY), c->tkw[i], c->tkh[i]);
        blit(REN, c->tl[i], x + kw + S(10), by + (bh - c->tlh[i]) / 2, c->tlw[i], c->tlh[i]);
        x += kw + S(10) + c->tlw[i] + gap;
    }
}

static void draw_padbar(void)
{
    /* CSS: .padbar 固定在视口底部、水平居中，margin-bottom 24px */
    static PadCache cmain, cset, cset2, chelp;
    if (g_page == PAGE_HELP) { padbar_draw(HELP_HINTS, PADBAR_NH, &chelp); return; }
    if (g_page != PAGE_SETTINGS) { padbar_draw(MAIN_HINTS, PADBAR_N, &cmain); return; }

    /* 设置页：←→ 调值 只在该菜单真有可调项时才出现（见 SET_HINTS_NOADJ 的注释）。
       两套表必须各配一份缓存 —— 键帽纹理是按"字体+颜色+下标"烘的，
       共用一份会在切菜单时把上一套的键帽闪现出来。 */
    if (g_set_has_adj) padbar_draw(SET_HINTS, PADBAR_NS, &cset);
    else               padbar_draw(SET_HINTS_NOADJ, 3, &cset2);
}

/* ==========================================================================
 *  7. 主循环
 * ==========================================================================*/
/* 大标题文案。design 里 .header h1 是页面主标题，分类切换时跟着变。 */
static const char *CAT_TITLES[] = { "全部机种", "任天堂机种", "世嘉机种",
                                    "索尼机种", "街机", "掌机", "电脑", "其他" };

/* 脏帧缓存：界面只在选中/滚动/切分类/分钟跳变时才变，其余帧没必要
   把上千个元素重画一遍再提交（实测那样在 Mali-G31 上只有 30fps）。
   静态帧就一次 RenderCopy，稳在 vsync 上。 */
static SDL_Texture *g_frame_tex;
static int g_dirty = 1;

static void mark_dirty(void) { g_dirty = 1; }

/* ---- 屏幕旋转（2026-09-15 为 MINILOONG pocket1 加）-------------------------
 *  有些机器用的是**竖屏面板侧装**：MINILOONG 那块 DSI 屏硬件就是 720x960
 *  （DTS 里 hactive=720 / vactive=960），可人眼看到的是 960x720 横屏。
 *
 *  ★ 但 MINILOONG 上**我们不能转**，因为厂商把转屏做在了下层：
 *      · ES 走的 SDL2 是被改过的（二进制里有 KMSDRM_InitRotateBuffer + librga，
 *        并且把 desktop_mode / current_mode / RESIZED 事件的宽高全对调了）
 *      · RA 也自带 RGA 旋转（c_RkRgaBlit）
 *    所以 SDL_GetDesktopDisplayMode() 报出来的**已经是 960x720 横屏**。
 *    上面 auto 的第一条就是"横的就不转"，MINILOONG 走的就是那条。
 *
 *  那这套代码还有什么用：
 *    ① 万一碰上"竖屏面板 + 下层没人管"的机器（或厂商换了没打补丁的 SDL2），
 *       auto 会判出竖屏 → 自己转 270，至少画面是正的不是躺着的；
 *    ② 真机上"图歪了"时，不用重编 —— 改 vgfront.conf 的 rotate= 或设
 *       VG_ROT 环境变量，重启前端就翻过来。
 *
 *  怎么转：**所有绘制照旧画在 g_frame_tex 这块离屏画布上**，但画布尺寸改用
 *  "逻辑尺寸"（横向那一对）；上屏的最后一步再把整块画布转 0/90/180/270 贴到
 *  窗口。这样布局代码一个字都不用动 —— g_win_w/g_win_h 从此就等于逻辑尺寸，
 *  S() / FS_UI() / clip_y1 这些全按它算，跟 R36H/A10 上完全一致。
 *
 *  取哪个角度：环境变量 VG_ROT 优先，其次 vgfront.conf 的 rotate=，
 *  最后 auto（auto = 横屏不转 / 竖屏转 270，见 rot_resolve()）。
 * -------------------------------------------------------------------------*/
static int g_rot = 0;                       /* 0/90/180/270，顺时针 */
static int g_phys_w = 0, g_phys_h = 0;      /* 我们真正能画的那块 surface 的尺寸 */

/* 把后续绘制导向离屏画布。
   **不管转不转都画在这里** —— 不旋转时它就是原来那块 g_frame_tex，
   整条"先画离屏、再整块拷到 backbuffer"的路径一个字没改。 */
static void frame_target(void)
{
    SDL_SetRenderTarget(REN, g_frame_tex);
}

static int g_soft = 0;                  /* 渲染器落到 software 上（main 里判） */
static SDL_Texture *g_rot_tex = NULL;   /* 软渲兜底用的物理尺寸贴图 */
static Uint32 *g_rot_a = NULL, *g_rot_b = NULL;
static size_t  g_rot_cap = 0;           /* 上面两块缓冲的容量（像素数） */

/* 逻辑画布四角在物理屏上的落点。四角顺序固定 TL,TR,BR,BL，UV 跟着走。
   角度 = 画面**顺时针**转多少度，与内核 fbcon=rotate:N 的 N*90 对齐。
   GLES2 实测（native/rot-math-test.c）：
     90°  逻辑左上 → 物理右上      270° 逻辑左上 → 物理左下
     180° 逻辑左上 → 物理右下 */
static void rot_verts(SDL_Vertex *v)
{
    const float W = (float)g_phys_w, H = (float)g_phys_h;
    static const float UV[4][2] = { {0,0}, {1,0}, {1,1}, {0,1} };
    float px[4], py[4];

    switch (g_rot) {
    case 90:  px[0]=W; py[0]=0; px[1]=W; py[1]=H; px[2]=0; py[2]=H; px[3]=0; py[3]=0; break;
    case 180: px[0]=W; py[0]=H; px[1]=0; py[1]=H; px[2]=0; py[2]=0; px[3]=W; py[3]=0; break;
    case 270: px[0]=0; py[0]=H; px[1]=0; py[1]=0; px[2]=W; py[2]=0; px[3]=W; py[3]=H; break;
    default:  px[0]=0; py[0]=0; px[1]=W; py[1]=0; px[2]=W; py[2]=H; px[3]=0; py[3]=H; break;
    }
    for (int i = 0; i < 4; i++) {
        v[i].position.x  = px[i];
        v[i].position.y  = py[i];
        v[i].color.r = v[i].color.g = v[i].color.b = v[i].color.a = 255;
        v[i].tex_coord.x = UV[i][0];
        v[i].tex_coord.y = UV[i][1];
    }
}

/* 软件渲染器兜底：手动搬像素。
   软渲本来就跑在 CPU 上，ReadPixels 只是一次 memcpy，不存在读回卡顿；
   GL/GLES2 那条路绝不走这里（glReadPixels 会同步等 GPU，一帧能拖掉十几毫秒）。 */
static void frame_flush_soft(void)
{
    const int lw = g_win_w, lh = g_win_h;
    const int pw = g_phys_w, ph = g_phys_h;
    const size_t n = (size_t)pw * (size_t)ph;

    if (!g_rot_tex || g_rot_cap < n) {
        if (g_rot_tex) { SDL_DestroyTexture(g_rot_tex); g_rot_tex = NULL; }
        free(g_rot_a); free(g_rot_b); g_rot_a = g_rot_b = NULL;
        g_rot_cap = 0;
        g_rot_a = (Uint32 *)malloc(n * 4);
        g_rot_b = (Uint32 *)malloc(n * 4);
        g_rot_tex = SDL_CreateTexture(REN, SDL_PIXELFORMAT_ARGB8888,
                                      SDL_TEXTUREACCESS_STREAMING, pw, ph);
        if (!g_rot_a || !g_rot_b || !g_rot_tex) {   /* 彻底没辙：宁可黑屏也别画错 */
            free(g_rot_a); free(g_rot_b); g_rot_a = g_rot_b = NULL;
            if (g_rot_tex) { SDL_DestroyTexture(g_rot_tex); g_rot_tex = NULL; }
            SDL_SetRenderTarget(REN, NULL);
            return;
        }
        SDL_SetTextureBlendMode(g_rot_tex, SDL_BLENDMODE_NONE);
        g_rot_cap = n;
    }

    /* 此刻目标还是那块逻辑画布，先把它读回来 */
    if (SDL_RenderReadPixels(REN, NULL, SDL_PIXELFORMAT_ARGB8888,
                             g_rot_a, lw * 4) != 0) {
        SDL_SetRenderTarget(REN, NULL);
        return;
    }
    SDL_SetRenderTarget(REN, NULL);

    /* dst(x,y) ← src：按"顺时针 g_rot"反算源坐标 */
    for (int y = 0; y < ph; y++) {
        Uint32 *drow = g_rot_b + (size_t)y * pw;
        for (int x = 0; x < pw; x++) {
            int sx, sy;
            switch (g_rot) {
            case 90:  sx = y;           sy = lh - 1 - x; break;
            case 180: sx = lw - 1 - x;  sy = lh - 1 - y; break;
            case 270: sx = lw - 1 - y;  sy = x;          break;
            default:  sx = x;           sy = y;          break;
            }
            drow[x] = g_rot_a[(size_t)sy * lw + sx];
        }
    }
    SDL_UpdateTexture(g_rot_tex, NULL, g_rot_b, pw * 4);
    SDL_RenderCopy(REN, g_rot_tex, NULL, NULL);
}

/* 离屏画布 → 物理屏幕。
   不旋转：原样整块拷（就是改动前那一句 RenderCopy）。
   旋转：给四个顶点做变换，尺寸正好铺满物理屏。

   为什么不用 SDL_RenderCopyEx 的 "dstrect + center"：
     那套语义是"把 dstrect 连同里面的纹理绕 center 转"。GL/GLES2 上这是标准的
     顶点变换，没问题；但**软件渲染器**会把它退化成"先转源图、再缩进 dstrect"
     而且忽略 center —— 720x960 的屏上实测跑偏 240px、还丢了半张画面。
     所以改成自己给四个顶点（软渲和 GL 都会照画，语义一致），
     软件渲染器上再退一层手动搬像素。 */
static void frame_flush(void)
{
    if (!g_rot) {
        SDL_SetRenderTarget(REN, NULL);
        SDL_RenderCopy(REN, g_frame_tex, NULL, NULL);
        return;
    }
    if (g_soft) { frame_flush_soft(); return; }

    SDL_SetRenderTarget(REN, NULL);

    SDL_Vertex v[4];
    rot_verts(v);
    static const int idx[6] = { 0, 1, 2, 0, 2, 3 };

    /* 帧缓存是不透明底，用 NONE 免得跟背景混一次；用完立刻还原，
       别把外围 UI 依赖的 BLEND 状态改掉。 */
    SDL_BlendMode old;
    SDL_GetRenderDrawBlendMode(REN, &old);
    SDL_SetRenderDrawBlendMode(REN, SDL_BLENDMODE_NONE);
    SDL_RenderGeometry(REN, g_frame_tex, v, 4, idx, 6);
    SDL_SetRenderDrawBlendMode(REN, old);
}

/* 主页（#pageHome）：顶栏 + 分类标签 + 大标题 + 平台网格 */
static void draw_home_page(int clip_y1)
{
    int y = 0;
    draw_topbar(y);          y += topbar_h();

    int ty, th;
    draw_tabs(y, &ty, &th);
    y += tabs_h();

    /* 标题区：大标题 + 当前分类的实计数。
       （网页版这句 "20 个平台 · 共 6109 款游戏" 是写死的假数据，
         这里直接用索引里的真实值。） */
    static SDL_Texture *t_h1, *t_hp;
    static int h1w, h1h, hpw, hph;
    static int last_cat = -1, last_np = -1, last_ng = -1;

    int np_cat = 0, ng_cat = 0;
    for (int i = 0; i < g_view_n; i++) {
        np_cat++;
        ng_cat += IX.p[g_view[i]].ngames;
    }
    if (last_cat != g_cat || last_np != np_cat || last_ng != ng_cat) {
        if (t_h1) SDL_DestroyTexture(t_h1);
        if (t_hp) SDL_DestroyTexture(t_hp);
        t_h1 = bake(REN, F_H1, CAT_TITLES[g_cat], C_TEXT, &h1w, &h1h);
        char hp[128];
        snprintf(hp, sizeof(hp),
                 LF("%d 个平台 · 共 %d 款游戏", "%d systems · %d games"),
                 np_cat, ng_cat);
        t_hp = bake(REN, F_PSUB, hp, C_SUBTEXT, &hpw, &hph);
        last_cat = g_cat;
        last_np = np_cat;
        last_ng = ng_cat;
    }

    y += S(HDR_PAD_T);
    blit(REN, t_h1, S(CT_PAD_X), y, h1w, h1h);
    blit(REN, t_hp, S(CT_PAD_X), y + h1h + S(10), hpw, hph);
    y += h1h + S(10) + hph + S(HDR_PAD_B);

    draw_grid(y, y, clip_y1);
}

/* 游戏列表页（#pageGames）：顶栏 + 返回 + 平台标题 + 游戏网格。
   注意这页**没有分类标签** —— 网页版里 tabs 长在 #pageHome 的 .header 里。 */
static void draw_games_page(int clip_y1)
{
    int y = 0;
    draw_topbar(y); y += topbar_h();
    int gy = draw_games_head(y);
    draw_game_cards(gy, gy, clip_y1);
}

/* 「正在启动」遮罩。画在 g_frame_tex 上（跟底图一起被拷到屏幕），
   不能直接往 backbuffer 画 —— 下一帧那次 RenderCopy 会把它盖掉。 */
/* 带 alpha 的缩放 blit（开场动画全靠它做淡入与"浮起"）。 */
static void blit_a(SDL_Texture *t, int x, int y, int w, int h, int a)
{
    if (!t || w <= 0 || h <= 0 || a <= 0) return;
    SDL_SetTextureAlphaMod(t, (Uint8)(a > 255 ? 255 : a));
    SDL_Rect d = { x, y, w, h };
    SDL_RenderCopy(REN, t, NULL, &d);
    SDL_SetTextureAlphaMod(t, 255);
}

/* ---- 启动动画 -------------------------------------------------------------
   从按下 A 到模拟器真正出画面之间有 2~4 秒纯黑（要先起子进程，RetroArch
   还要初始化音视频）。原先只在按下的瞬间推一帧"正在启动 + 游戏名"就撒手，
   剩下几秒看着就像卡死了。这里补一段 LangOS 自己的开场：品牌字浮起、
   一道光带扫过、进度条走到头，把那段黑屏填满，也明确交代"确实按上了"。

   ⚠ 故意做成**阻塞播放**（自己 Present），不走主循环。走主循环就得改退出
   条件，而"用户主动退出"和"因启动而退出"在外层 shell 里的收尾完全不同
   （前者结束服务，后者要继续拉起模拟器），那条线不值得为一段动画去动。 */
static void launch_anim_play(const char *title, const char *plat)
{
    const int FRAMES = 46;
    const int FMS    = 18;              /* 每帧 18ms → 总长约 0.83 秒 */
    const int want_shot = getenv("VG_ANIM_SHOT") != NULL;

    int lw = 0, lh = 0, tw2 = 0, th2 = 0, pw = 0, ph = 0;
    SDL_Texture *tl = bake(REN, F_LOGO, "LANGOS", C_ACCENT, &lw, &lh);
    SDL_Texture *tt = (title && *title) ? bake(REN, F_H1, title, C_TEXT, &tw2, &th2) : NULL;
    SDL_Texture *tp = (plat  && *plat ) ? bake(REN, F_GINFO, plat, C_DIM, &pw, &ph)  : NULL;

    /* 画进"当前该画的那块画布"：旋转的机器上是离屏画布，否则就是 backbuffer。
       不能写死 NULL —— 那样在旋转机器上等于把没转的画面又转了一次。 */
    frame_target();

    for (int f = 0; f < FRAMES; f++) {
        double t = (double)f / (FRAMES - 1);        /* 0 → 1 */

        setc(REN, C_BG, 255);
        SDL_RenderClear(REN);

        /* 1) 中心柔光：三层同心圆，随进度极缓地收缩 —— 给深色底一点呼吸 */
        for (int i = 3; i >= 1; i--) {
            int rr = (int)(g_win_h * 0.34 * (1.0 - t * 0.10) * i / 3.0);
            SDL_Rect c = { g_win_w / 2 - rr, (int)(g_win_h * 0.45) - rr, rr * 2, rr * 2 };
            draw_round(REN, c, rr, C_ACCENT, 7);
        }

        /* 2) 品牌字：0→55% 放大 + 淡入（从 0.80 倍涨到原大小） */
        if (tl) {
            double ap = t < 0.55 ? t / 0.55 : 1.0;
            double sc = 0.80 + 0.20 * ap;
            int w2 = (int)(lw * sc), h2 = (int)(lh * sc);
            blit_a(tl, (g_win_w - w2) / 2,
                   (int)(g_win_h * 0.36) - h2 / 2, w2, h2, (int)(ap * 255));
        }

        /* 3) 光带：在品牌字下方横扫一道，两端淡、中间亮 */
        if (t > 0.15 && t < 0.68) {
            double u = (t - 0.15) / 0.53;
            int bw = S(150);
            SDL_Rect band = { (int)((g_win_w + bw) * u) - bw, (int)(g_win_h * 0.485),
                              bw, S(3) };
            double k = 1.0 - fabs(u - 0.5) * 1.7;   /* 中间最亮 */
            if (k < 0) k = 0;
            draw_round(REN, band, S(2), C_ACCENT, (int)(150 * k));
        }

        /* 4) 进度条：后 45% 从左走到右 */
        int pw2 = (int)(g_win_w * 0.30);
        SDL_Rect track = { (g_win_w - pw2) / 2, (int)(g_win_h * 0.53), pw2, S(5) };
        draw_round(REN, track, S(3), C_LINE, 255);
        if (t > 0.55) {
            double u = (t - 0.55) / 0.45;
            SDL_Rect fill = { track.x, track.y, (int)(pw2 * u), track.h };
            if (fill.w > 0) draw_round(REN, fill, S(3), C_ACCENT, 255);
        }

        /* 5) 游戏名：0.45 之后淡入，同时从下方浮上来一点 */
        if (tt && t > 0.45) {
            double u = (t - 0.45) / 0.55;
            if (u > 1) u = 1;
            int ty2 = (int)(g_win_h * 0.63 + (1 - u) * S(14)) - th2 / 2;
            blit_a(tt, (g_win_w - tw2) / 2, ty2, tw2, th2, (int)(u * 255));
        }
        if (tp && t > 0.58) {
            double u = (t - 0.58) / 0.42;
            if (u > 1) u = 1;
            blit_a(tp, (g_win_w - pw) / 2, (int)(g_win_h * 0.69), pw, ph,
                   (int)(u * 190));
        }

        /* 6) 底部"正在启动"，用 alpha 脉动暗示还在做事（静态字会显得死） */
        {
            static SDL_Texture *th3 = NULL;
            static int w3 = 0, h3 = 0;
            if (!th3) th3 = bake(REN, F_SB, "正在启动", C_SUBTEXT, &w3, &h3);
            int pa = (int)(140 + 115 * (0.5 + 0.5 * sin(t * 12.0)));
            blit_a(th3, (g_win_w - w3) / 2, (int)(g_win_h * 0.79), w3, h3, pa);
        }

        /* 调试：离屏核对动画用。动画是阻塞播放的，走不到 render() 里那个
           --shot 分支，所以设了 VG_ANIM_SHOT 就在这里自己留几帧。 */
        if (want_shot && (f == 12 || f == 26 || f == 40 || f == FRAMES - 1)) {
            SDL_Surface *ss = SDL_CreateRGBSurfaceWithFormat(
                0, g_win_w, g_win_h, 32, SDL_PIXELFORMAT_ARGB8888);
            if (ss) {
                SDL_RenderReadPixels(REN, NULL, SDL_PIXELFORMAT_ARGB8888,
                                     ss->pixels, ss->pitch);
                char pth[80];
                snprintf(pth, sizeof(pth), "/tmp/anim-%02d.bmp", f);
                SDL_SaveBMP(ss, pth);
                SDL_FreeSurface(ss);
            }
        }

        frame_flush();          /* 逻辑画布 → 物理屏（旋转机器上才真做事） */
        SDL_RenderPresent(REN);
        SDL_Delay(FMS);
    }
}

/* ---- 自重启的品牌画面 -----------------------------------------------------
   用户 2026-09-14："设置里面调整字体大小后 重启后有个黑屏的界面也LANGOS代替"。

   设置页改字号/换主题走的是 execv 自重启（同进程换镜像），中间必然有几段黑：
   旧进程收尾 → 新进程 SDL_Init / KMS 重建 → 字体加载 → 首帧。
   这里把那些黑全换成 LANGOS 品牌画面，分两处调用：
     ① settings_restart() 在 execv **之前**调一次（hold=0，只顶一帧），
        盖住"旧进程退出 → 新进程 KMS 重建"那段；
     ② 新进程带 --splash 起来，渲染器一建好就先铺一层底色，
        字体一到位再播 0.7 秒完整版（见 main）。
   只在自重启时播 —— 开机、退游戏回前端这两条路不带 --splash：
   那两处本来就有宿主启动流程 / 游戏画面接着，凭空多等 0.7 秒是倒退。

   hold_ms <= 0 表示只画一帧、不占时间；msg 为 NULL 或字体还没加载
   就跳过文字（只铺底和光晕），绝不能在这里崩 —— 它是启动路径上的代码。 */
static void boot_splash(const char *msg, int hold_ms)
{
    /* 调进来的时候渲染目标可能还冲着离屏帧缓存（设置页就是这么画的），
       这里统一走 frame_target()：旋转机器上目标就是那块画布，不旋转时
       切回 backbuffer —— 总之画到"待会儿真会上屏"的那块上。 */
    frame_target();

    int lw = 0, lh = 0, mw = 0, mh = 0;
    SDL_Texture *tl = (msg && F_LOGO)
                    ? bake(REN, F_LOGO, "LANGOS", C_ACCENT, &lw, &lh) : NULL;
    SDL_Texture *tm = (msg && F_SB)
                    ? bake(REN, F_SB, msg, C_SUBTEXT, &mw, &mh) : NULL;

    const int FMS = 16;                          /* 每帧 16ms → 62.5fps */
    int frames = hold_ms > 0 ? hold_ms / FMS : 1;
    if (frames < 1) frames = 1;

    for (int f = 0; f < frames; f++) {
        double t = frames > 1 ? (double)f / (double)(frames - 1) : 1.0;

        setc(REN, C_BG, 255);
        SDL_RenderClear(REN);

        /* 中心柔光：跟 launch_anim_play 同一套画法，两处画面看着像一家人 */
        for (int i = 3; i >= 1; i--) {
            int rr = (int)(g_win_h * 0.34 * i / 3.0);
            SDL_Rect c = { g_win_w / 2 - rr,
                           (int)(g_win_h * 0.45) - rr, rr * 2, rr * 2 };
            draw_round(REN, c, rr, C_ACCENT, 7);
        }

        if (tl) {
            /* 品牌字：0→50% 从小涨到原大小并淡入 */
            double ap = t < 0.5 ? t / 0.5 : 1.0;
            double sc = 0.86 + 0.14 * ap;
            int w2 = (int)(lw * sc), h2 = (int)(lh * sc);
            blit_a(tl, (g_win_w - w2) / 2,
                   (int)(g_win_h * 0.40) - h2 / 2, w2, h2, (int)(ap * 255));

            /* 进度条。这里没有真实进度可报（索引早就读完了），
               但它让"确实在做事"一目了然 —— 静止画面会被当成死机。 */
            int pw2 = (int)(g_win_w * 0.30);
            SDL_Rect track = { (g_win_w - pw2) / 2,
                               (int)(g_win_h * 0.545), pw2, S(5) };
            draw_round(REN, track, S(3), C_LINE, 255);
            int fw = (int)(pw2 * (0.15 + 0.85 * t));
            if (fw > 0) {
                SDL_Rect fill = { track.x, track.y, fw, track.h };
                draw_round(REN, fill, S(3), C_ACCENT, 255);
            }

            if (tm)
                blit(REN, tm, (g_win_w - mw) / 2,
                     (int)(g_win_h * 0.62), mw, mh);
        }

        frame_flush();          /* 逻辑画布 → 物理屏（旋转机器上才真做事） */
        SDL_RenderPresent(REN);
        if (frames > 1) SDL_Delay(FMS);
    }
}

/* 按了 A 之后那一帧盖的"正在启动"遮罩（动画跑完到进程退出之间还会渲染
   一两帧，得让它接得上，不能突然变回一屏纯色）。 */
static void draw_launch_overlay(void)
{
    static SDL_Texture *t1, *t2;
    static int w1 = 0, h1 = 0, w2 = 0, h2 = 0;

    setc(REN, C_BG, 236);
    SDL_Rect full = { 0, 0, g_win_w, g_win_h };
    SDL_RenderFillRect(REN, &full);

    if (!t1) t1 = bake(REN, F_H1, "正在启动", C_TEXT, &w1, &h1);
    if (!t2 && g_launch_msg) {
        /* 只烘一次 —— 这条路走完进程就退出去了，不会换第二款游戏 */
        char cut[256];
        ellipsize(F_GCOVER, g_launch_msg, S(DESIGN_W - 240),
                  cut, sizeof(cut));
        if (cut[0]) t2 = bake(REN, F_GCOVER, cut, C_SUBTEXT, &w2, &h2);
    }

    int cy = g_win_h / 2;
    if (t1) blit(REN, t1, (g_win_w - w1) / 2, cy - h1 - S(10), w1, h1);
    if (t2) blit(REN, t2, (g_win_w - w2) / 2, cy + S(10), w2, h2);
}

/* ==========================================================================
 *  6d. 搜索页（#pageSearch）
 * ==========================================================================
 *  版式照抄 index.html：.content{padding-top:20px} + .back-btn + .search-panel
 *  （max-width:1400 居中、padding:40、圆角 20）里装 .search-title / .search-input-row
 *  / 10 列 .keyboard / .keyboard-hint。面板下方那块结果列表是这一版新加的 ——
 *  网页版这一页只有壳，没有结果区（见上面 SP_PAD 的注释）。
 */
static void draw_search_page(int clip_y1)
{
    int y = 0;
    draw_topbar(y); y += topbar_h();
    y += S(GPAD_T);                      /* .content 的 padding-top */

    int padL = S(CT_PAD_X);

    /* --- .back-btn「← 返回」 --- */
    static SDL_Texture *t_back;
    static int bw = 0, bh = 0;
    if (!t_back) t_back = bake(REN, F_BACK, "← 返回", C_ACCENT, &bw, &bh);
    blit(REN, t_back, padL, y, bw, bh);
    y += bh + S(GB_MB);

    /* --- .search-panel 的几何。先算出来，面板底边得先知道，
           底下那块结果列表才知道从哪开始。 --- */
    int pw = S(SP_MAXW);
    if (pw > g_win_w - 2 * padL) pw = g_win_w - 2 * padL;
    int px = (g_win_w - pw) / 2;
    int inner = pw - 2 * S(SP_PAD);
    int keyw = (inner - (KB_COLS - 1) * S(KB_GAP)) / KB_COLS;
    int kb_rows = (KB_TOTAL + KB_COLS - 1) / KB_COLS;
    int kb_h = kb_rows * FS_UI(KB_KEY_H) + (kb_rows - 1) * S(KB_GAP);

    int panel_h = S(SP_PAD) * 2
                + (int)(FS_UI(SP_TITLE_FS) * 1.35) + S(SP_TITLE_MB)
                + S(SP_IN_H) + S(SP_ROW_MB)
                + kb_h + S(KB_HINT_MT) + (int)(FS_UI(KB_HINT_FS) * 1.35);

    SDL_Rect box = { px, y, pw, panel_h };
    glass_card(box, S(SP_RAD), C_PANEL, 190, C_LINE);

    int cx = px + S(SP_PAD);
    int cy = y + S(SP_PAD);

    /* .search-title：24px bold --accent2 */
    {
        static SDL_Texture *t;
        static int tw = 0, th = 0;
        if (!t) t = bake(REN, F_STITLE, "搜索游戏", C_ACCENT2, &tw, &th);
        blit(REN, t, cx, cy, tw, th);
        cy += th + S(SP_TITLE_MB);
    }

    /* .search-input-row：输入框 flex:1 + 右侧 .search-result-count */
    {
        static SDL_Texture *tc;
        static int cw = 0, chh = 0, last_n = -1;
        if (!tc || last_n != g_res_n) {
            char cnt[32];
            snprintf(cnt, sizeof(cnt), LF("%d 款", "%d games"), g_res_n);
            if (tc) SDL_DestroyTexture(tc);
            tc = bake(REN, F_SINPUT, cnt, C_SUBTEXT, &cw, &chh);
            last_n = g_res_n;
        }

        int inw = inner - S(SP_ROW_GAP) - cw;
        SDL_Rect ir = { cx, cy, inw, S(SP_IN_H) };
        round_box(REN, ir, S(SP_IN_RAD), C_BG2, C_LINE, S(1));

        /* 输入内容：空的时候显示占位（--dim），有内容白字 + 一根光标竖线。
           竖线只在焦点落在软键盘上时画，对齐网页版 :focus 的观感。 */
        static SDL_Texture *tin;
        static int iw = 0, ih = 0;
        static char last_q[64] = "\x01";       /* 与空串必不相等，首帧必烘 */
        if (!tin || strcmp(last_q, g_q) != 0) {
            const char *s = g_qlen ? g_q : "输入首字母或关键词...";
            if (tin) SDL_DestroyTexture(tin);
            tin = bake(REN, F_SINPUT, s, g_qlen ? C_TEXT : C_DIM, &iw, &ih);
            snprintf(last_q, sizeof(last_q), "%s", g_q);
        }
        int tx = ir.x + S(SP_IN_PADX);
        blit(REN, tin, tx, ir.y + (ir.h - ih) / 2, iw, ih);

        if (g_sfocus == 0) {
            setc(REN, C_ACCENT, 255);
            SDL_Rect car = { tx + iw + S(4), ir.y + S(14), S(3), ir.h - S(28) };
            SDL_RenderFillRect(REN, &car);
        }

        blit(REN, tc, cx + inw + S(SP_ROW_GAP),
             ir.y + (ir.h - chh) / 2, cw, chh);
        cy += S(SP_IN_H) + S(SP_ROW_MB);
    }

    /* 软键盘：36 个字母数字 + ⌫ / 清空 / 搜索，10 列自动换行
       （第 4 行 9 格、末格留空，跟 CSS grid 的自动流动一致）。 */
    for (int i = 0; i < KB_TOTAL; i++) {
        int col = i % KB_COLS, row = i / KB_COLS;
        int kx = cx + col * (keyw + S(KB_GAP));
        int ky = cy + row * (FS_UI(KB_KEY_H) + S(KB_GAP));
        SDL_Rect kr = { kx, ky, keyw, FS_UI(KB_KEY_H) };

        int on = (g_sfocus == 0 && i == g_kbd);
        if (on) round_box(REN, kr, S(KB_KEY_RAD), C_ACCENT, C_ACCENT, S(1));
        else    round_box(REN, kr, S(KB_KEY_RAD), C_PANEL2, C_LINE, S(1));

        char lab[12];
        Uint32 fg;
        if (i < KB_N) {
            lab[0] = KB_CHARS[i];
            lab[1] = '\0';
            fg = 0xffffff;
        } else {
            /* 网页版这里写的是 ⌫（U+232B），但 NotoSansCJK 里没有这个字形，
               实测渲染成 .notdef 方块（截图核对过）。换成 U+2190「←」——
               同一份字体里一定有，语义也是"往回删"，"← 返回"那个按钮用的就是它。 */
            snprintf(lab, sizeof(lab), "%s",
                     i == KB_ACT_BACK  ? "\u2190" :
                     i == KB_ACT_CLEAR ? "清空" : "搜索");
            fg = C_DANGER;                       /* .keyboard-key.action */
        }
        if (on) fg = 0xffffff;                   /* .active 时按键变蓝、字压白 */

        int lw = 0, lh = 0;
        SDL_Texture *lt = bake_cached(REN, F_SKEY, lab, fg, &lw, &lh);
        blit(REN, lt, kx + (keyw - lw) / 2, ky + (kr.h - lh) / 2, lw, lh);
    }
    cy += kb_h + S(KB_HINT_MT);

    /* .keyboard-hint：14px --dim 居中 */
    {
        static SDL_Texture *t;
        static int tw = 0, th = 0;
        if (!t)
            t = bake(REN, F_PTAG,
                     "方向键选字母/结果 · A 输入/启动 · B 删除/返回 · "
                     "↓ 进入结果 ↑ 回键盘 · START 清空",
                     C_DIM, &tw, &th);
        blit(REN, t, (g_win_w - tw) / 2, cy, tw, th);
    }

    /* --- 结果列表：面板下方剩下多少画多少，可滚动 --- */
    int ry = y + panel_h + S(20);
    g_res_y = ry;

    int view_h = clip_y1 - ry - S(12);
    if (view_h < FS_UI(SR_H)) view_h = 0;
    g_res_view = view_h;
    if (!view_h) return;

    if (!g_qlen || !g_res_n) {
        static SDL_Texture *t0;
        static int w0 = 0, h0 = 0;
        if (!t0)
            t0 = bake(REN, F_GCOVER,
                      g_qlen ? "没有找到匹配的游戏"
                             : "输入首字母或关键词开始搜索",
                      C_DIM, &w0, &h0);
        blit(REN, t0, (g_win_w - w0) / 2, ry + S(14), w0, h0);
        return;
    }

    ensure_visible_search();

    int step = FS_UI(SR_H) + S(SR_GAP);
    int rx = px + S(SP_PAD);
    int rw = pw - 2 * S(SP_PAD);

    SDL_Rect clip = { 0, ry, g_win_w, view_h };
    SDL_RenderSetClipRect(REN, &clip);

    for (int i = 0; i < g_res_n; i++) {
        int yy = ry + i * step - g_rscroll;
        if (yy + FS_UI(SR_H) < ry || yy > ry + view_h) continue;   /* 视口外不烘 */

        Platform *p = &IX.p[g_res[i].plat];
        const char *title = p->games[g_res[i].gi].title;
        int on = (g_sfocus == 1 && i == g_rsel);

        SDL_Rect rr = { rx, yy, rw, FS_UI(SR_H) };
        glass_card(rr, S(SR_RAD), on ? C_PANEL2 : C_PANEL,
                   on ? 210 : 180, on ? C_ACCENT : C_LINE);

        /* 先把右边的平台名量出来，左边才知道游戏名能占多宽 */
        int pw2 = 0, ph2 = 0;
        SDL_Texture *pt = bake_cached(REN, F_GINFO, p->name, C_SUBTEXT,
                                      &pw2, &ph2);
        blit_r(REN, pt, rx + rw - S(20), yy + (FS_UI(SR_H) - ph2) / 2, pw2, ph2);

        char cut[256];
        ellipsize(F_SINPUT, title, rw - S(40) - pw2 - S(24), cut, sizeof(cut));
        int tw = 0, th = 0;
        SDL_Texture *t = bake_cached(REN, F_SINPUT, cut,
                                     on ? 0xffffff : C_TEXT, &tw, &th);
        blit(REN, t, rx + S(20), yy + (FS_UI(SR_H) - th) / 2, tw, th);
    }

    SDL_RenderSetClipRect(REN, NULL);

    /* 滚动条：和游戏页同一套算法 */
    int total = g_res_n * step - S(SR_GAP);
    if (total > view_h) {
        int bar_h = view_h * view_h / total;
        if (bar_h < S(24)) bar_h = S(24);
        int bar_y = ry + (view_h - bar_h) * g_rscroll / (total - view_h);
        SDL_Rect tb = { g_win_w - S(CT_PAD_X) + S(8), bar_y, S(8), bar_h };
        draw_round(REN, tb, S(4), C_LINE, 255);
    }
}

/* ==========================================================================
 *  6c. 背景层 + 毛玻璃面板
 *
 *  2026-09-14 用户："背景都毛玻璃"。
 *
 *  做毛玻璃有个前提：**背后得有东西可透**。原来背景是一块纯色 C_BG，
 *  半透明面板压上去还是那块纯色，糊出来一模一样 —— 玻璃感为零。
 *  所以先给背景加层次（两团极淡的大光晕），面板才有东西可"透"。
 *
 *  ⚠ 为什么不做真卷积模糊：
 *    真模糊要先 SDL_RenderReadPixels 把显存读回来（KMSDRM 下这条路径
 *    在 Mali-G31 上是几十毫秒级的），主页一屏有十几个卡片，
 *    每个都读一次 = 界面直接卡死。
 *    真模糊只保留给设置页那块**全屏大面板**（一次调用、还带缓存，
 *    见 glass_panel()）。卡片/标签这类小控件用"半透明 + 顶部高光 + 亮边"
 *    近似 —— 在有光晕的背景上，观感和真毛玻璃几乎分不出来。
 * ==========================================================================*/

/* 径向渐变光斑：烘一次 128×128，之后按需缩放贴。
   不用 draw_round 画大圆 —— 那会给每个尺寸各建一张上千像素的纹理，
   两团光晕就是十几 MB 显存，掌机扛不住。 */
static SDL_Texture *g_blob;

static SDL_Texture *blob_tex(void)
{
    if (g_blob) return g_blob;
    const int N = 128;
    SDL_Surface *sf = SDL_CreateRGBSurfaceWithFormat(0, N, N, 32,
                                                     SDL_PIXELFORMAT_ARGB8888);
    if (!sf) return NULL;
    SDL_LockSurface(sf);
    Uint32 *px = (Uint32 *)sf->pixels;
    const double c = (N - 1) / 2.0;
    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            double dx = (x - c) / c, dy = (y - c) / c;
            double d2 = dx * dx + dy * dy;          /* 0 中心 → 1 边缘 */
            double a  = d2 >= 1.0 ? 0.0 : (1.0 - d2);
            a = a * a;                              /* 平方衰减，边缘更柔 */
            px[y * N + x] = (Uint32)(int)(a * 255.0 + 0.5) << 24;
        }
    }
    SDL_UnlockSurface(sf);
    g_blob = SDL_CreateTextureFromSurface(REN, sf);
    SDL_FreeSurface(sf);
    if (g_blob) SDL_SetTextureBlendMode(g_blob, SDL_BLENDMODE_BLEND);
    return g_blob;
}

/* fx/fy 是光斑圆心（屏幕宽高的比例），fr 是半径（占屏宽的比例） */
static void draw_blob(double fx, double fy, double fr, Uint32 col, int alpha)
{
    SDL_Texture *t = blob_tex();
    if (!t) return;
    int rr = (int)(g_win_w * fr);
    if (rr <= 0) return;
    SDL_Rect rc = { (int)(g_win_w * fx) - rr, (int)(g_win_h * fy) - rr,
                    rr * 2, rr * 2 };
    SDL_SetTextureColorMod(t, (Uint8)(col >> 16), (Uint8)(col >> 8),
                              (Uint8)col);
    SDL_SetTextureAlphaMod(t, (Uint8)alpha);
    SDL_RenderCopy(REN, t, NULL, &rc);
    SDL_SetTextureAlphaMod(t, 255);
}

/* 主题底色 + 两团光晕。取代原来的裸 SDL_RenderClear(C_BG)。 */
static void draw_backdrop(void)
{
    setc(REN, C_BG, 255);
    SDL_RenderClear(REN);

    /* 左上主色、右下辅色 —— 斜着对角，视觉上比上下分布更"有纵深"。
       alpha 压得很低（深色主题 ~26，浅色主题同样会显得脏一点，
       但 paper 那套 accent 饱和度高，反而正好有一点淡彩）。 */
    draw_blob(-0.04, -0.12, 0.66, C_ACCENT,  26);
    draw_blob( 1.02,  1.10, 0.74, C_ACCENT2, 22);
}

/* 玻璃面板：整块先铺亮边色，再内缩 1px 铺半透明主題色，顶部 1/4 提亮。
   alpha 给 150~190 之间 —— 太低字会看不清（这是 4 寸屏，不是显示器）。 */
static void glass_card(SDL_Rect rc, int rad, Uint32 tint, int a, Uint32 edge)
{
    if (rc.w <= 2 || rc.h <= 2) return;
    draw_round(REN, rc, rad, edge, 46);
    SDL_Rect in = { rc.x + 1, rc.y + 1, rc.w - 2, rc.h - 2 };
    int r2 = rad > 1 ? rad - 1 : 0;
    draw_round(REN, in, r2, tint, a);
    if (rc.h >= 28) {                       /* 顶部高光：模拟天光反射 */
        SDL_Rect hi = { in.x, in.y, in.w, rc.h / 4 };
        draw_round(REN, hi, r2, 0xffffff, 18);
    }
}

static void render(int want_shot)
{
    if (g_dirty) {
        frame_target();

        draw_backdrop();

        int clip_y1 = g_win_h - padbar_zone();
        if (g_page == PAGE_SEARCH)        draw_search_page(clip_y1);
        else if (g_page == PAGE_GAMES)    draw_games_page(clip_y1);
        else if (g_page == PAGE_SETTINGS) {
            /* 设置页是**浮在主页上的一层**：先把主页画出来当背景，压暗，
               再叠毛玻璃面板（面板在 draw_settings_page 里画）。
               少了这一步面板背后就是空的 —— 没东西可糊。 */
            draw_home_page(clip_y1);
            /* 压暗别太重 —— 压太狠玻璃后面就全是黑的，糊出来还是一片黑，
               毛玻璃等于白做。76/255 刚好：背景退后一层，但颜色还在。 */
            setc(REN, 0x000000, 76);
            SDL_Rect full = { 0, 0, g_win_w, g_win_h };
            SDL_RenderFillRect(REN, &full);
            draw_settings_page(clip_y1);
        }
        else if (g_page == PAGE_HELP) {
            /* 说明页和设置页同款"浮层"：先铺主页当背景、压暗，再叠玻璃面板。
               少了背景这一步，毛玻璃后面就是空的，糊出来一片死黑。 */
            draw_home_page(clip_y1);
            setc(REN, 0x000000, 76);
            SDL_Rect full = { 0, 0, g_win_w, g_win_h };
            SDL_RenderFillRect(REN, &full);
            draw_help_page(clip_y1);
        }
        else                              draw_home_page(clip_y1);

        draw_padbar();          /* 底栏两页共用（.padbar-container 在页面外） */

        /* 游戏操作弹层：**最后**画，连底栏一起压暗 —— 它是模态的。 */
        if (g_act_on) draw_act_popup();

        /* 按了 A 之后那一帧盖的"正在启动"遮罩 */
        if (g_launch_msg) draw_launch_overlay();

        g_dirty = 0;        /* 目标由下面的 frame_flush() 统一切回 backbuffer */
    }

    /* 截图放在上屏**之前**：这会儿离屏画布上还是刚渲好的逻辑画面，
       读出来存的 BMP 是正常横屏的（不会跟着屏幕转 90°，看截图不用歪头）。
       KMSDRM 下 RenderReadPixels 是唯一可靠的截图手段，而且必须赶在
       Present 之前 —— Present 会 swap backbuffer。 */
    if (want_shot) {
        frame_target();     /* 确保读的是刚画完的那块，而不是别处的残留 */
        SDL_Surface *ss = SDL_CreateRGBSurfaceWithFormat(0, g_win_w, g_win_h,
                                                        32, SDL_PIXELFORMAT_ARGB8888);
        if (ss) {
            SDL_RenderReadPixels(REN, NULL, SDL_PIXELFORMAT_ARGB8888,
                                 ss->pixels, ss->pitch);
            SDL_SaveBMP(ss, "/tmp/vgfront-shot.bmp");
            SDL_FreeSurface(ss);
            printf("截图已存 /tmp/vgfront-shot.bmp\n");
            fflush(stdout);
        }
    }

    /* ---- 上屏 ----
       VG_NOCOPY 是调试开关：跳过"把画布拷到 backbuffer"这一步，用来分辨帧率
       是卡在这步拷贝还是卡在 SDL_RenderPresent 的 vsync 逻辑上。正常跑不要设
       （会看到上一帧的残留）。旋转机器上这一步换成了带角度的拷贝，同样受它管。 */
    if (getenv("VG_NOCOPY")) SDL_SetRenderTarget(REN, NULL);
    else                     frame_flush();

    SDL_RenderPresent(REN);
}

/* 保证选中项可见 */
static void ensure_visible(int y0, int clip_y1)
{
    int ch = card_h(), gy = S(PG_GAP);
    int row = g_sel / g_pg_cols;
    int top = row * (ch + gy);
    int view_h = clip_y1 - y0;

    if (top < g_scroll) g_scroll = top;
    else if (top + ch > g_scroll + view_h)
        g_scroll = top + ch - view_h;
    if (g_scroll < 0) g_scroll = 0;
}

/* 游戏页的同一件事。网格起点用 draw_games_head() 写回的实测值，
   比按字号估算准（行高只有纹理烘完才确定）。 */
static void ensure_visible_games(int clip_y1)
{
    Platform *p = cur_plat();
    if (!p || p->ngames <= 0 || g_games_grid_y <= 0) return;

    int ch = game_card_h(), gy = S(GAME_GAP);
    int top = (g_gsel / g_game_cols) * (ch + gy);
    int view_h = clip_y1 - g_games_grid_y;
    if (view_h < 1) return;

    if (top < g_gscroll) g_gscroll = top;
    else if (top + ch > g_gscroll + view_h) g_gscroll = top + ch - view_h;

    int maxs = games_total_h() - view_h;
    if (maxs < 0) maxs = 0;
    if (g_gscroll > maxs) g_gscroll = maxs;
    if (g_gscroll < 0) g_gscroll = 0;
}

/* 进/出游戏页。对应网页版的 showPage('games') + renderGames(platform)。
   返回主页时把游戏页状态清干净 —— 网页版每次进来都是重建列表，从第一张
   开始选中，这里跟着它走。 */
static void enter_games(int pix)
{
    if (pix < 0 || pix >= IX.np) return;
    g_plat = pix;
    g_page = PAGE_GAMES;
    g_gsel = 0;
    g_gscroll = 0;
    printf("[进入] %s（%d 款）\n", IX.p[pix].name, IX.p[pix].ngames);
    fflush(stdout);
}

static void leave_games(void)
{
    g_page = PAGE_HOME;
    g_gsel = 0;
    g_gscroll = 0;
    g_games_grid_y = 0;          /* 下次进来重新量 */
}

/* ==========================================================================
 *  5.5 启动游戏 —— 复用宿主现成的启动通道
 * ==========================================================================
 *  整条链路上**一个字节的启动逻辑都不重写**：
 *
 *      vgfront ──fork/exec──► python3 vagaretro-shell.py --launch <pid> <相对路径>
 *          │                            │
 *          │                            └─► 写出 /opt/vagaretro/launch.sh
 *          └─ 正常退出(0) ──► vagaretro-front.sh 的游戏循环看到 launch.sh
 *                                └─► bash launch.sh（起 RetroArch）
 *                                     └─► rm launch.sh → continue → 重新拉起 vgfront
 *
 *  为什么绕这一圈，而不是 C 里直接拼 RetroArch 命令行：
 *  命令怎么拼是**平台相关**的，而且坑很深 ——
 *      · PS1 走的是 /usr/local/bin/retroarch32 + 32 位那套 cores；
 *      · saturn / nds / psp 必须先 realpath 还原成 /roms2/... 真实路径，
 *        否则包装脚本 `dirname "$2" | cut -d/ -f2` 会抠到 "home" 而不是 "roms2"，
 *        yabasanshiro 直接启动失败（宿主里那条长注释记着这次实测）；
 *      · EasyRPG 要把 .zip 条目改成旁边的同名目录；
 *      · GBA 的 ES 首选核心是坏的，宿主里有一张 FORCE_CORE 表要压过去。
 *  这些规则宿主里已经有一份，再在 C 里实现一遍就是第二份，必然漂。
 *  C 只负责回答"用户点了哪个游戏"，剩下的交给同一份 Python。
 */
#define SHELL_PY     "/opt/vagaretro/vagaretro-shell.py"
#define CONF_INI     "/opt/vagaretro/config.ini"
#define LAUNCH_REQ   "/opt/vagaretro/launch.sh"
#define LAUNCH_WAIT  15000      /* 等 --launch 返回的上限（毫秒） */

/* 从 config.ini 的**第一个 [section] 之前**取一个键。
   规则和启动器的 conf_get 一致：只认第一个节之前的部分 ——
   [shell] 里有一堆同名的键，混进来会取错值。 */
static int conf_get_str(const char *path, const char *key,
                        char *out, size_t cap)
{
    FILE *fh = fopen(path, "r");
    if (!fh) return 0;
    char line[512];
    size_t klen = strlen(key);
    int found = 0;
    while (fgets(line, sizeof(line), fh)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '[') break;                       /* 进第一个节，停 */
        if (*p == '#' || *p == ';' || *p == '\n' || *p == '\r') continue;
        if (strncmp(p, key, klen) != 0) continue;
        char *q = p + klen;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != '=') continue;
        q++;
        while (*q == ' ' || *q == '\t') q++;
        char *e = q;
        while (*e && *e != '\n' && *e != '\r') e++;
        while (e > q && (e[-1] == ' ' || e[-1] == '\t')) e--;
        size_t n = (size_t)(e - q);
        if (n >= cap) n = cap - 1;
        memcpy(out, q, n);
        out[n] = '\0';
        found = 1;
        break;
    }
    fclose(fh);
    return found;
}

/* ---- 跨进程状态：从游戏退回来时回到原来那一页 --------------------------
 * 启动游戏要**退出进程**（把屏幕让给模拟器），回来时启动器是把 vgfront
 * 重新拉一遍 —— 新进程从零开始，默认落在主页第一项。
 * 于是"玩了某个平台第 300 个游戏，退出来却回到主页"，得再翻回去，很别扭。
 * 所以启动前把页面状态落一个小文件，下次起来读它，**读完就删**（一次性）。
 *
 * 只在文件存在时恢复；用户主动退出（BACK）会把它删掉 ——
 * 于是**下次开机仍然从主页开始**，不会"上次退到哪这次就停在哪"。
 */
#define STATE_PATH "/opt/vagaretro/.vgfront-state"

/* 落盘格式（TAB 分隔）：page plat cat sel scroll gsel gscroll [query]
   第 8 个字段是搜索串 —— 搜到一半启动游戏、退回来时能接着看原来的结果。
   gsel 那一格对搜索页复用成"结果的第几项"。 */
static void state_save(void)
{
    FILE *fh = fopen(STATE_PATH, "w");
    if (!fh) return;                    /* 写不了就算了，最多是回到主页 */
    fprintf(fh, "%d\t%d\t%d\t%d\t%d\t%d\t%d\t%s\n",
            g_page, g_plat, g_cat, g_sel, g_scroll,
            g_page == PAGE_SEARCH ? g_rsel : g_gsel,
            g_gscroll, g_q);
    fclose(fh);
}

static void state_load(void)
{
    FILE *fh = fopen(STATE_PATH, "r");
    if (!fh) return;

    int page = PAGE_HOME, plat = -1, cat = 0, sel = 0,
        scroll = 0, gsel = 0, gscroll = 0;
    char q[64] = "";
    /* 第 8 个字段是可选的：早先写的状态文件只有 7 个字段，读到 7 个也算数 */
    int ok = fscanf(fh, "%d\t%d\t%d\t%d\t%d\t%d\t%d\t%63s",
                    &page, &plat, &cat, &sel, &scroll, &gsel, &gscroll, q);
    fclose(fh);
    if (ok < 7) return;

    if (cat >= 0 && cat < NCAT) { g_cat = cat; build_view(); }
    if (sel >= 0 && sel < g_view_n) g_sel = sel;
    if (scroll > 0) g_scroll = scroll;

    if (page == PAGE_SEARCH) {
        /* 搜索页去启动游戏是常事，退回来输入串和结果都还在 */
        g_search_from = PAGE_HOME;
        g_page = PAGE_SEARCH;
        snprintf(g_q, sizeof(g_q), "%s", q);
        g_qlen = (int)strlen(g_q);
        search_run();                    /* search_run 会把 g_rsel 清 0 */
        if (g_res_n > 0) {
            g_sfocus = 1;
            if (gsel >= 0 && gsel < g_res_n) g_rsel = gsel;
        }
        printf("[续页] 回到搜索页「%s」(%d 条)\n", g_q, g_res_n);
        return;
    }

    if (page == PAGE_GAMES && plat >= 0 && plat < IX.np) {
        g_plat = plat;
        g_page = PAGE_GAMES;
        int n = IX.p[plat].ngames;
        if (gsel >= 0 && gsel < n) g_gsel = gsel;
        if (gscroll > 0) g_gscroll = gscroll;
        printf("[续页] 回到 %s 的第 %d 项\n", IX.p[plat].name, g_gsel);
    }
}

/* 跑一个子进程并等它结束，最多 timeout_ms。   返回 0 = 正常退出且 rc==0；-1 = 起不来 / 超时 / 非 0 退出。

   为什么自己写 waitpid 轮询而不用 system()：
   system() 会 fork 一个 /bin/sh，还得再拼一次引号 ——
   我们的条目名里有中文、空格、方括号（"CT特种部队2重返战壕汉化版.zip" 这种），
   过 shell 就得多一层转义。execv 直接给 argv，一个引号都不用碰。 */
static int run_child(char *const argv[], int timeout_ms)
{
    pid_t pid = fork();
    if (pid < 0) { perror("[启动] fork 失败"); return -1; }

    if (pid == 0) {                 /* 子进程：execv 成功就不返回了 */
        execv(argv[0], argv);
        _exit(127);                 /* execv 失败（127 = command not found 惯例） */
    }

    int waited = 0;
    for (;;) {
        int st = 0;
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid)
            return (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : -1;
        if (r < 0) return -1;
        if (waited >= timeout_ms) {
            fprintf(stderr, "[启动] 子进程超时 %dms，杀掉\n", timeout_ms);
            kill(pid, SIGKILL);
            waitpid(pid, &st, 0);
            return -1;
        }
        SDL_Delay(20);
        waited += 20;
    }
}

/* 请求启动 p 的第 gi 个游戏。成功（launch.sh 已写出）返回 1。
   成功之后调用方应当立刻退出进程 —— 屏幕要交给 RetroArch。 */
static int launch_game(Platform *p, int gi)
{
    if (!p || gi < 0 || gi >= p->ngames) return 0;

    const char *rel = p->games[gi].rel;
    if (!rel || !*rel) {
        fprintf(stderr, "[启动] %s 没有相对路径，跳过\n", p->games[gi].title);
        return 0;
    }

    /* **真实机种**。从「游玩历史」「我的收藏」里再启动时，当前平台的 pid 是
       history / favorite，那不是机种 —— 拿它去拼 <compat>/history/<rel>，
       那个目录不存在，宿主 do_launch() 会直接失败。条目上的 src_pid 记着
       它真正来自哪个平台，启动和记历史都得用它。 */
    const char *real_pid = p->games[gi].src_pid ? p->games[gi].src_pid
                                                 : p->pid;

    char py[256] = "/usr/bin/python3";
    char msg[256];
    conf_get_str(CONF_INI, "shell_python", py, sizeof(py));

    /* 命令行先打出来，**再**判断要不要真启动。
       为什么要这个次序：上面这些都是纯计算，而截图/调试模式下会被拦下。
       打印如果排在拦截之后，`--games 0 --launch 0` 这个钩子就永远看不到
       real_pid —— 而「从游玩历史/收藏启动时，传给宿主的到底是不是真实
       机种」恰恰只能这么验（2026-09-15 为了验 src_pid 特意挪上来的）。 */
    printf("[启动] %s / %s\n       %s --launch %s '%s'\n",
           p->name, p->games[gi].title, py, real_pid, rel);
    fflush(stdout);

    /* ⚠ 截图/调试模式下**绝不**真去启动游戏。
       离屏实例跑的是真机同款二进制，一旦走进这条路就会把 launch.sh
       写到 /opt/vagaretro/，真机上正在跑的前端下次退出时就会去执行它 ——
       表现是「没人按，机器自己起了个游戏」（2026-09-14 真踩过）。
       unlink(LAUNCH_REQ) 必须留在这道拦截**之后** —— 离屏实例绝不能把
       真机上待执行的启动请求删掉。 */
    if (g_shot_mode) {
        printf("[启动] 截图/调试模式，忽略启动请求: %s\n", p->games[gi].title);
        return 0;
    }

    /* 先清掉可能残留的旧请求。留着它的话哪怕这次启动失败，
       外层循环也会把上一次的游戏再拉一遍。 */
    unlink(LAUNCH_REQ);

    /* 先把"正在启动"推到屏幕上，再去干活。
       从这一刻到模拟器真正出画面之间有 2~4 秒纯黑（子进程要起，
       RetroArch 还要初始化音频/视频），不给反馈的话玩家会以为没按上，
       然后连按好几下 —— 那几下会排队生成好几个启动请求。
       这里放的是 **LangOS 自己的开场动画**（阻塞约 0.8 秒），不是原来
       那种一帧纯色遮罩：品牌字浮起 + 一道光带扫过 + 进度条走到底。 */
    g_launch_msg = p->games[gi].title;
    mark_dirty();
    launch_anim_play(p->games[gi].title, p->name);

    char *const argv[] = {
        py, (char *)SHELL_PY, (char *)CONF_INI,
        "--launch", (char *)real_pid, (char *)rel, NULL
    };

    if (run_child(argv, LAUNCH_WAIT) != 0) {
        fprintf(stderr, "[启动] 生成启动脚本失败，留在原界面\n");
        unlink(LAUNCH_REQ);
        return 0;
    }
    if (access(LAUNCH_REQ, X_OK) != 0) {
        fprintf(stderr, "[启动] %s 没有生成，留在原界面\n", LAUNCH_REQ);
        return 0;
    }

    snprintf(msg, sizeof(msg), "[启动] 已就绪，退出前端交给模拟器: %s",
             LAUNCH_REQ);
    printf("%s\n", msg);
    fflush(stdout);

    /* 记进游玩历史。放在这里而不是函数开头 —— launch.sh 已经确认生成了，
       也就是「这一把确实要玩了」；前面任何一条失败路径都不该留下记录。 */
    hist_record(real_pid, rel);

    /* 记住"我在哪一页"，下次被启动器重新拉起来时回到这里 */
    state_save();
    return 1;
}

/* ==========================================================================
 *  5.6 搜索页 —— 软键盘 + 结果列表
 * ==========================================================================
 *  软键盘上就 36 个键（A-Z0-9），所以 g_q 天然只含大写字母和数字 ——
 *  匹配时不用做任何 locale 相关的大小写折叠，逐字节把 a-z 抬成 A-Z 即可。
 *
 *  匹配源**只有游戏名**，不含平台名：匹配平台名的话按一个 "F" 会一次
 *  命中几千条（Famicom 全平台），结果列表反而失去意义。
 *
 *  网页版这一页只有壳子（见 SP_PAD 那段注释），所以"照抄"到此为止 ——
 *  过滤、结果列表、启动都是这一版补的。
 */

/* haystack 里不区分大小写地找 needle（needle 保证是大写字母或数字）。
   只折叠 ASCII 的 a-z，中文字节（>=0x80）原样比 —— 于是 "1941" 能命中
   「1941 Counter Attack」，而中文名也不会被误折叠坏掉。 */
static int ci_contains(const char *hay, const char *needle)
{
    if (!hay || !needle || !*needle) return 0;
    size_t n = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t k = 0;
        while (k < n) {
            char c = p[k];
            if (c >= 'a' && c <= 'z') c = (char)(c - 32);
            if (c != needle[k]) break;
            k++;
        }
        if (k == n) return 1;
    }
    return 0;
}

/* 重算结果集。每按一键全量扫一遍索引 —— 六千条游戏名做子串比较是
   亚毫秒级，比维护"增量收窄上一次结果"少一半代码，也少一半出错的地方。
   中文标题再比一遍拼音首字母（g->py）：输 "hdl" 命中「魂斗罗」——
   占位文案一直承诺「输入首字母或关键词」，v5 之前只是句空话。 */
static void search_run(void)
{
    g_res_n = 0;
    g_rsel = 0;
    g_rscroll = 0;
    if (!g_qlen) {
        printf("[搜索] 已清空\n");
        fflush(stdout);
        return;
    }

    int n_py = 0;                        /* 有多少条是靠拼音命中的（排错用） */
    for (int i = 0; i < IX.np && g_res_n < SR_NMAX; i++) {
        Platform *p = &IX.p[i];
        for (int g = 0; g < p->ngames && g_res_n < SR_NMAX; g++) {
            const Game *gm = &p->games[g];
            int by_py = 0;
            if (ci_contains(gm->title, g_q)) {
                ;                        /* 字面命中，不需要拼音 */
            } else if (gm->py && ci_contains(gm->py, g_q)) {
                by_py = 1;
            } else {
                continue;
            }
            g_res[g_res_n].plat = i;
            g_res[g_res_n].gi = g;
            g_res_n++;
            if (by_py) n_py++;
        }
    }

    printf("[搜索] 「%s」→ %d 条%s（其中拼音命中 %d）\n", g_q, g_res_n,
           g_res_n >= SR_NMAX ? "（到上限）" : "", n_py);
    fflush(stdout);
}

/* 结果列表的滚动。可视高度只有 draw 时才知道，所以用上一帧写回的
   g_res_view —— 和游戏页 ensure_visible_games() 是同一个套路。 */
static void ensure_visible_search(void)
{
    int view_h = g_res_view;
    if (view_h <= 0 || g_res_n <= 0) return;

    int step = FS_UI(SR_H) + S(SR_GAP);
    int top = g_rsel * step;

    if (top < g_rscroll) g_rscroll = top;
    else if (top + FS_UI(SR_H) > g_rscroll + view_h)
        g_rscroll = top + FS_UI(SR_H) - view_h;

    int maxs = g_res_n * step - S(SR_GAP) - view_h;
    if (maxs < 0) maxs = 0;
    if (g_rscroll > maxs) g_rscroll = maxs;
    if (g_rscroll < 0) g_rscroll = 0;
}

/* 软键盘上按下某一格。idx 用 KB_CHARS / KB_ACT_* 那一套编号。 */
static void search_press(int idx)
{
    if (idx < KB_N) {
        if (g_qlen < (int)sizeof(g_q) - 1) {
            g_q[g_qlen++] = KB_CHARS[idx];
            g_q[g_qlen] = '\0';
            search_run();
        }
    } else if (idx == KB_ACT_BACK) {
        if (g_qlen > 0) { g_q[--g_qlen] = '\0'; search_run(); }
    } else if (idx == KB_ACT_CLEAR) {
        g_qlen = 0;
        g_q[0] = '\0';
        search_run();
    } else if (idx == KB_ACT_GO) {
        /* 网页版这里只 console.log。既然结果已经有了，就让它真的
           把光标交给结果列表 —— 省得还要再按一次 ↓。 */
        search_run();
        if (g_res_n) { g_sfocus = 1; g_rsel = 0; g_rscroll = 0; }
    }
    mark_dirty();
}

/* 搜索页的方向键。move: -1 左 / 1 右 / -2 上 / 2 下 */
static void search_move(int move)
{
    if (g_sfocus == 0) {
        if (move == -1) {
            if (g_kbd % KB_COLS > 0) g_kbd--;
        } else if (move == 1) {
            if (g_kbd % KB_COLS < KB_COLS - 1 && g_kbd + 1 < KB_TOTAL) g_kbd++;
        } else if (move == -2) {
            if (g_kbd - KB_COLS >= 0) g_kbd -= KB_COLS;
        } else if (move == 2) {
            /* 已经在最后一行还往下 = 进结果列表（有结果才算数） */
            if (g_kbd + KB_COLS < KB_TOTAL) g_kbd += KB_COLS;
            else if (g_res_n) { g_sfocus = 1; g_rsel = 0; g_rscroll = 0; }
        }
    } else {
        if (!g_res_n) { g_sfocus = 0; return; }
        if (move == -1 || move == -2) {
            /* 结果列表是单列，← 和 ↑ 都是"往上"，到顶就回键盘 */
            if (g_rsel == 0) g_sfocus = 0;
            else g_rsel--;
        } else {
            if (g_rsel + 1 < g_res_n) g_rsel++;
        }
    }
    ensure_visible_search();
    mark_dirty();
}

static void enter_search(void)
{
    g_search_from = g_page;              /* 先记下来，再改 g_page */
    g_page = PAGE_SEARCH;
    g_qlen = 0;
    g_q[0] = '\0';
    g_kbd = 0;
    g_sfocus = 0;
    g_rsel = 0;
    g_rscroll = 0;
    g_res_n = 0;
    printf("[搜索] 进入搜索页\n");
    fflush(stdout);
}

static void leave_search(void)
{
    g_page = (g_search_from == PAGE_GAMES && g_plat >= 0) ? PAGE_GAMES
                                                          : PAGE_HOME;
    g_qlen = 0;
    g_q[0] = '\0';
    g_sfocus = 0;
    g_rsel = 0;
    g_rscroll = 0;
    g_res_n = 0;
}

/* 切分类（顶栏那排标签）。d = -1 上一个 / +1 下一个。
   三个入口共用一个函数：键盘 TAB、手柄 START、手柄 L/R 肩键 ——
   免得三处各写一遍"改下标 + 复位选中 + 复位滚动 + 重建视图"，
   漏掉哪一步就是那种"看着换了其实没换"的怪 bug。

   只在主页生效：分类标签只属于主页。别的页调用是无操作，
   这样在游戏页/搜索页误按 L/R 不会把分类偷偷换掉，
   回来时一脸陌生（用户抱怨的"误触"有一半是这类）。 */
static void cat_step(int d)
{
    if (g_page != PAGE_HOME || NCAT <= 0) return;
    /* 跳过空分类（一个平台都没有的）。不跳的话会切到一片空白网格上，
       用户只能再按一次才知道"哦这分类是空的"。NCAT 次都空就原地不动。 */
    int c = g_cat;
    for (int i = 0; i < NCAT; i++) {
        c = (c + d + NCAT) % NCAT;
        if (cat_count(c) > 0) break;
    }
    g_cat = c;
    g_sel = 0;
    g_scroll = 0;
    build_view();
    mark_dirty();
    printf("[分类] 切到 %d/%d（%s，本类 %d 个平台）\n",
           g_cat, NCAT, CAT_NAMES[g_cat], g_view_n);
    fflush(stdout);
}

/* 当前分类一个有游戏的平台都没有时（索引里这个机种没 ROM），
   切到第一个有内容的分类。否则标签栏没有高亮、网格全空 —— 看着像坏了。
   调用点：续页状态恢复之后（state_load 可能把 g_cat 恢复成空分类）。 */
static void cat_fallback(void)
{
    if (g_view_n > 0) return;
    for (int c = 0; c < NCAT; c++)
        if (cat_count(c) > 0) { g_cat = c; build_view(); return; }
}

/* ==========================================================================
 *  5.7 设置页
 * ==========================================================================
 *  网页版早就有一页设置（index.html 的 SETTINGS_MENU + 宿主
 *  vagaretro-shell.py 的 read_settings/apply_setting），原生前端一直缺，
 *  按 START 是空操作。这一段把它补齐。
 *
 *  ---------------------------------------------------------------------
 *  为什么**不照抄**网页版的菜单树
 *  ---------------------------------------------------------------------
 *  网页版那 7 个二级菜单里，四分之三是 EmulationStation 自己的设置 ——
 *  语言、主题、游戏列表视图、切换动画、屏保行为、收藏置顶……改了是写进
 *  /home/ark/.emulationstation/es_settings.cfg 给 **ES** 读的。
 *  而 ES 已经在这台机器上被禁用了（前端接管了启动链路，见启动器头部注释），
 *  这些值对本前端**零影响**：字体、布局、列表样式全在这份 C 里，
 *  ROM 索引由宿主自己扫，进度/收藏也不读 ES 的库。
 *  摆上去就是一排"按了没反应"的开关 —— 正是 2026-09-14 修底栏假文案
 *  （SEL 标"分类"实为"退出"）时定下的规矩：**没实现就别标**。
 *
 *  所以这里只留三类：
 *    ① 改完**立刻**看得见效果的硬件项：音量、亮度、三个调速器。
 *       落盘位置与宿主完全一致（amixer+alsactl / sysfs），不另建存储。
 *    ② 本前端**自己的**两项：主页列数、游戏列表列数。这两个是用户
 *       2026-09-14 亲手要求调过的东西（3 列 / 4 列），做成设置项之后
 *       不用改代码重编就能试。存 vgfront.conf，与系统设置互不干扰。
 *    ③ 只读的系统信息 + 重启/关机（危险动作要按两次 A 确认）。
 *
 *  ---------------------------------------------------------------------
 *  为什么不复用宿主的 Python 数据层
 *  ---------------------------------------------------------------------
 *  原生前端存在的全部意义就是"不依赖 WebKit/Python 也能出界面"。
 *  为了改个音量去 fork 一次 python3，光解释器启动就是 ~300ms，
 *  而这几个动作 C 直接做只要几毫秒（写 sysfs 是一次 write）。
 *  所以读值/落盘都在 C 里实现，**行为对齐**宿主那份，代码不共用。
 *
 *  ---------------------------------------------------------------------
 *  重启 / 关机为什么只 touch 文件
 *  ---------------------------------------------------------------------
 *  启动器 vagaretro-front.sh 里已经有 handle_es_flags()，它认
 *  /tmp/es-sysrestart 和 /tmp/es-shutdown 两个 ES 约定的标志
 *  （ArkOS 自己也用它们），前端退出后由它执行 systemctl reboot/poweroff。
 *  复用这条通路比自己调 shutdown 稳：不用管权限、不用管我们自己
 *  是不是 root、也跟 ES 的既有行为一致。
 *  "重启前端"同理 —— 写一个空操作脚本到 launch.sh，外层 while 会
 *  bash 它然后 continue，于是 run_sdl 被再跑一次、vgfront 被重新拉起。
 *  这是唯一能真正"重启前端"的通路：直接 exit(0) 的话外层会走收尾逻辑
 *  把服务结束掉（前端就不会再起来了）。
 * ==========================================================================*/

/* ---- 路径：一律与宿主 vagaretro-shell.py 的同名常量保持一致 ---- */
#define BL_DIR        "/sys/class/backlight/backlight"
#define CPU_GOV_PATH  "/sys/devices/system/cpu/cpufreq/policy0/scaling_governor"
#define GPU_GOV_PATH  "/sys/devices/platform/ff400000.gpu/devfreq/ff400000.gpu/governor"
#define DMC_GOV_PATH  "/sys/devices/platform/dmc/devfreq/dmc/governor"
#define ES_RESTART_FLAG "/tmp/es-sysrestart"
#define ES_SHUTDOWN_FLAG "/tmp/es-shutdown"
/* 本前端自己的设置（列数）。放 $APPDIR 下 —— 启动器以 ark 跑、前端以
   root 跑，两边都写得进去；放 /tmp 重启就丢，放 /home 又跟系统设置混在一起。 */
#define SCONF_PATH    "/opt/vagaretro/vgfront.conf"
#define VG_VERSION    "1.0"

/* ---- 条目类型（对应网页版 CSS 的 .settings-row 几个修饰类）---- */
#define ST_MENU   0     /* 二级菜单，A 进入，右侧 › */
#define ST_VAL    1     /* 左右改值，右侧显示当前值 */
#define ST_TOGGLE 2     /* 开关，A 或左右翻转 */
#define ST_BUTTON 3     /* A 执行一次（危险动作要两次） */
#define ST_RO     4     /* 只读展示 */

/* ---- 取值来源 ---- */
enum {
    SV_NONE = 0,
    SV_VOLUME, SV_BRIGHT,                    /* 硬件，改完立即生效 */
    SV_CPUGOV, SV_GPUGOV, SV_DMCGOV,         /* sysfs 调速器（选项型） */
    SV_HCOLS, SV_GCOLS,                      /* 本前端自己的列数（选项型） */
    SV_COVERSTY,                             /* 游戏封面样式（选项型：竖版/正方形） */
    SV_BEZEL,                                /* 游戏遮罩（选项型：关/三种分辨率） */
    SV_ROMSRC,                               /* 游戏来源（选项型：自动/卡一/卡二） */
    SV_RALANG,                               /* RA 界面语言（选项型：English/简体中文） */
    SV_RAFWD,                                /* RA 加速按键（选项型：L2/R2/L3/R3） */
    SV_UILANG,                               /* 前端界面语言（选项型：中文/English） */
    SV_SWAP,                                 /* 虚拟内存（选项型：关闭/256/512/1024 MB） */
    SV_FSUI, SV_FSGAME,                      /* 字号倍率（数值型，百分比 80..160） */
    SV_TZ,                                   /* 时区（选项型；走 timedatectl） */
    SV_WIFI_RADIO, SV_SSH,                   /* 开关型（nmcli radio / systemctl） */
    SV_LED_MODE, SV_LED_FX,                  /* 摇杆灯（选项型） */
    SV_LED_BRIGHT,                           /* 摇杆灯亮度（数值型，百分比） */
    SV_RAMENU,                               /* RA 菜单风格（选项型） */
    SV_RATHEME,                              /* RA 配色主题（选项型，XMB 9 套） */
    SV_RAFPS,                                /* RA 帧数显示（选项型：关闭/开启） */

    SV_SLEEP,                                /* 息屏时间（选项型：关 / 5 / 10 / 15 / 30 / 60 分钟） */
    SV_AUTOSLOT,                             /* 自动存档位（数值型 0..10，给「退出自动存档」用） */
    /* 只读信息。掌机上这几个是**高频要看的**（还剩多少电、卡是不是满了、
       IP 是多少好传 ROM），所以宁可多占几行也要给出来。 */
    SV_I_BATT, SV_I_NET, SV_I_DISK, SV_I_TIME,
    SV_I_RES, SV_I_DEV, SV_I_KERN, SV_I_CPU, SV_I_MEM,
    SV_I_GOV, SV_I_ROM, SV_I_INDEX, SV_I_FRONT,
    SV_I_WIFI_CUR, SV_I_SSH_ADDR,
    SV_I_CHEATS, SV_I_CHEATPACK          /* 金手指：已装数量 / 包来源 */
};

/* ---- 动作 ---- */
enum { SA_NONE = 0, SA_RESTART_FRONT, SA_REBOOT, SA_SHUTDOWN, SA_SET_THEME,
       SA_INSTALL_CHEATS, SA_REMOVE_CHEATS,
       /* ★ 2026-09-16：打开「快捷键说明」页。跟上面几个不一样 ——
          它不是"危险动作"，按一次 A 就进去（setting_confirm 里有特判，
          不走"再按一次确认"那条路）。 */
       SA_HELP };

typedef struct {
    const char *name;              /* 英文名（原生 ES 菜单就是全大写英文） */
    const char *cn;                /* 中文名，跟在英文右边 */
    int         type;
    int         menu;              /* ST_MENU：目标菜单下标 */
    int         src;               /* 取值来源 */
    int         action;            /* ST_BUTTON：动作 */
    const char *const *choices;    /* 选项型：候选表（值为**下标**）；数值型传 NULL */
    int         nch;
    int         vmin, vmax, step;  /* 数值型：范围与步长 */
    const char *hint;              /* 底部那行的说明 */
    const char *val;               /* 直接写死的值文本（WiFi 动态行用）。
                                      非空时优先于 sv_text() —— 动态行的值
                                      （信号格数、"已保存/新网络"）是扫描时
                                      算好的，再让 sv_text 按 src 去猜没意义。 */
    const char *arg;               /* 通用附加参数。目前只有主题行用：
                                      存该行对应的主题 id（自定义主题名是
                                      运行期才知道的，塞不进常量表）。 */
} SItem;

/* ---- 菜单下标（SItem.menu 要引用，先定义）----
   M_WIFI 是**动态**菜单：条目在扫描完成后才填，见 IT_WIFI_DYN。
   所以下标必须与 MENUS[] 的物理顺序严格一致，加菜单时两边一起改。 */
#define M_ROOT   0
#define M_SOUND  1
#define M_DISP   2
#define M_THEME  3
#define M_NET    4
#define M_WIFI   5
#define M_PERF   6
#define M_REGION 7
#define M_INFO   8
#define M_QUIT   9
#define M_CHEATS 10
#define M_LED    11

/* 选项表跟宿主 read_settings() 里的 choices 一字不差。
   ⚠ 两边都改了要一起改 —— 但**不要**改 CPU 那张表的顺序：
   sysfs 认的是字符串，顺序只影响左右翻页的手感（按省电→性能排）。 */
static const char *const GOV_CPU[] = {
    "powersave", "conservative", "ondemand", "interactive",
    "schedutil", "performance"
};
static const char *const GOV_DEVFREQ[] = {
    "powersave", "dmc_ondemand", "simple_ondemand", "performance", "userspace"
};
/* 列数表：必须从 "3" 开始且连续 —— sv_cur_index() 拿 `值 - 3` 当下标。 */
static const char *const COLS_HOME[] = { "3", "4", "5" };
static const char *const COLS_GAME[] = { "3", "4", "5", "6" };

/* 封面样式表：下标就是 g_cover_sty 的值（0 竖版 / 1 正方形），顺序不能动。
   这两串直接显示在设置页右侧（sv_text 的 SV_COVERSTY 分支）。 */
static const char *const COVER_STY[] = { "竖版", "正方形" };

/* 游戏遮罩。用户 2026-09-14："一键设置遮罩 把所有平台画面比例换成核心原生
   比例 然后对应遮罩自动加载且不透明度 100%" + "内置三种遮罩可以一键切换"。

   三套遮罩随系统装好（/opt/vagaretro/bezels/<分辨率>/<平台>/），这里只负责
   "选哪一套 / 关掉"，下标就是 g_bezel。存盘写 BEZEL_VAL[]（"1024x768" 这种），
   因为真正要用它的是**启动器** vagaretro-shell.py（它读 vgfront.conf 的
   bezel=，在启动游戏时把对应遮罩用 --appendconfig 挂给 RetroArch）。

   ⚠ 所以改这一项**不用重启前端** —— 前端只是把选择记下来，
   下次进游戏才生效。这跟字体/主题那两项不一样，别照着抄。

   ⚠ **选的分辨率必须和屏幕一致 —— 这台 MINILOONG 是 960x720。**
   2026-09-18 修正：以前这里写的是"这台是 1024x768"，是错的，
   害得默认选了原厂那套 1024x768 包 —— 那套的窗口表是按 1024x768 画布
   硬搬的（famicom 给的是 941x700 @ 41,34，**941 已经超过本机 720 的屏宽**），
   挂上去 RA 的画面矩形整个落到屏外 ⇒ **进游戏全黑，只剩一行 FPS 水印**。
   现在 960x720 这套是拿 1024x768 那套等比缩放（两者同为 4:3，×0.9375）
   再用**新图自己的 alpha 通道**重新量的窗口，全部落在屏内。 */
#define BEZEL_N 5
static const char *const BEZEL_VAL[]   = { "", "960x720", "1024x768", "640x480", "720x720" };

/* 遮罩包根目录（bezel_auto_detect 的"只有一个档就用它"兜底要扫它）。 */
#define BEZELS_ROOT "/opt/vagaretro/bezels"

/* 读文件第一行（去掉行尾 CR/LF）。成功且非空返回 1。 */
static int read_first_line(const char *path, char *buf, size_t cap)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    buf[0] = '\0';
    if (fgets(buf, (int)cap, fp)) {
        size_t n = strlen(buf);
        while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';
    }
    fclose(fp);
    return buf[0] ? 1 : 0;
}

/* 从字符串里抠出第一处 "宽x高"，容忍前后缀 —— "720x960" 和
   "U:720x960p-0"（fb0 的 modes 就是这格式）都要能吃。 */
static int parse_wxh(const char *s, int *w, int *h)
{
    if (!s) return 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') continue;
        int a = atoi(p);
        const char *q = p;
        while (*q >= '0' && *q <= '9') q++;
        if ((*q == 'x' || *q == 'X') && q[1] >= '0' && q[1] <= '9') {
            int b = atoi(q + 1);
            if (a > 0 && b > 0) { *w = a; *h = b; return 1; }
        }
        p = q - 1;                        /* for 还会 ++，等价于从 q 继续 */
    }
    return 0;
}

/* ★★★ 2026-09-18 浪总：「什么固件什么dtb你就默认对应的遮罩呗 比如迷你龙
 *      你设置里面就默认 960*720 的遮罩」
 *
 *   为什么必须自动：g_bezel 的初始值是 0（= 关闭），而 sconf_load() 只在
 *   vgfront.conf **有** `bezel=` 这一行时才覆盖它 ⇒ 新装机 / 恢复出厂 /
 *   conf 被删过的机器，遮罩是**关着**的，玩家压根不知道有这功能。
 *   比"关着"更糟的是"选错档"：原厂那套 1024x768 的窗口表是按 1024 画布画的
 *   （famicom 给的是 941x700 @ 41,34，941 超过本机 720 的屏宽），挂上去画
 *   面矩形整个落到屏外 —— 实测**进游戏全黑、只剩一行 FPS 水印**。
 *   所以默认得"跟着设备走"，不能指望玩家在一堆档里猜对。
 *
 *   探测顺序：
 *     ① DSI 面板的首选模式（掌机主屏；本机 = 720x960）
 *     ② framebuffer 的 modes（"U:720x960p-0"）
 *        ⚠ 别用 /sys/class/graphics/fb0/virtual_size —— 那是**双缓冲**，
 *          本机报的是 720,1920，拿它去匹配一个档都对不上
 *     ③ 屏幕没探到、或尺寸不在档位表里 ⇒ bezels/ 下**只有一个档**就用它
 *        （换了机型但只装了一套包时不至于空手）
 *     ④ 都不行 ⇒ 返回 0（关闭），跟改动前一样
 *
 *   ⚠ **方向不敏感**：面板是物理竖装（modes 报 720x960）、逻辑横屏
 *     （画布 960x720），同一块屏 —— 下面按「集合相等」比，不认谁宽谁高。
 *   ⚠ 只在 conf **没有** `bezel=` 这一行时调用（见 sconf_load）。
 *     玩家手工选过就以文件为准，哪怕他显式选的是「关闭」（值 = 空串）。
 */
static int bezel_auto_detect(void)
{
    int sw = 0, sh = 0;
    char buf[160];

    static const char *const MODES[] = {
        "/sys/class/drm/card0-DSI-1/modes",     /* 本机主屏 */
        "/sys/class/drm/card0-DSI-2/modes",
        "/sys/class/graphics/fb0/modes",        /* 兜底 */
        NULL
    };
    for (int i = 0; MODES[i] && !sw; i++)
        if (read_first_line(MODES[i], buf, sizeof(buf)))
            parse_wxh(buf, &sw, &sh);

    if (sw > 0 && sh > 0) {
        for (int i = 1; i < BEZEL_N; i++) {
            int bw = 0, bh = 0;
            if (parse_wxh(BEZEL_VAL[i], &bw, &bh) &&
                ((bw == sw && bh == sh) || (bw == sh && bh == sw)))
                return i;
        }
    }

    DIR *dp = opendir(BEZELS_ROOT);
    if (dp) {
        char only[64] = "";
        int cnt = 0;
        struct dirent *de;
        while ((de = readdir(dp)) != NULL) {
            if (de->d_name[0] == '.') continue;
            for (int i = 1; i < BEZEL_N; i++)
                if (strcmp(de->d_name, BEZEL_VAL[i]) == 0) {
                    if (cnt == 0) snprintf(only, sizeof(only), "%s", de->d_name);
                    cnt++;
                    break;
                }
        }
        closedir(dp);
        if (cnt == 1)
            for (int i = 1; i < BEZEL_N; i++)
                if (strcmp(only, BEZEL_VAL[i]) == 0) return i;
    }
    return 0;
}


/* 游戏来源（用户 2026-09-15 要求）。存盘写 ROMSRC_VAL[]（auto/1/2），
   宿主读同一个键决定扫哪张卡。 */
#define ROMSRC_N 3
static const char *const ROMSRC_VAL[]   = { "auto", "1", "2" };
static const char *const ROMSRC_LABEL[] = { "自动", "卡一", "卡二" };

/* RetroArch 界面语言（用户 2026-09-15："设备里面 ra 语言切换"）。
   值就是 libretro 的 enum retro_language 序号，写进 RA 的 user_language。

   ⚠ 真源是 retroarch.cfg，本前端**不复存**到 vgfront.conf：
   RA 退出时会把内存里的设置回写那份 cfg，两边都记必然对不上，
   所以一律以 cfg 为准，要显示就现读。

   ⚠ 这台机器的 RA **自带 langextra** —— 二进制里 88203 个 CJK 字符，
   简体/繁体两套资源都在（strings 能看到 "Chinese (Simplified) - "）。
   只改配置就能出中文，**不用换二进制、更不用重编**。
   2026-09-15 以 ark 身份实机跑过：user_language=12 → 全中文菜单
   （build/ra-lang-cn.png），=0 → 英文（build/ra-lang-en.png）。
   （之前判成"纯英文编译"是 grep -P 匹配 UTF-8 多字节不生效造成的假阴性。）

   ⚠ 真实调用链（实测）：
     systemd emulationstation.service（User=ark）
       → /usr/bin/emulationstation/emulationstation.sh
         → /usr/local/bin/retroarch  （包装脚本）
             /opt/retroarch/bin/retroarch -c /home/ark/.config/retroarch/retroarch.cfg
   `-c` 的路径是**写死的** /home/ark，所以不管 RA 以 root 还是 ark 起，
   改这一份都算数。PS1 走 retroarch32，配置在另一个目录，一并改，
   否则 PS1 里还是英文。 */
#define RALANG_N 2
static const int RALANG_VAL[] = { 0, 12 };      /* 0=English  12=简体中文 */
static const char *const RALANG_LABEL[] = { "English", "简体中文" };
#define RA_CFG_MAIN "/home/ark/.config/retroarch/retroarch.cfg"
#define RA_CFG_32   "/home/ark/.config/retroarch32/retroarch.cfg"
static int g_ralang = 0;                        /* RALANG_VAL 的下标 */

/* RA 加速键（快进）。用户 2026-09-16："设置里加个 RA 游戏加速按键自定义，
   可以自定义 L2 R2 L3 R3"；当天晚些："默认L2加速没反应"。

   ⚠ 值写的是**按键编号**，不是 "l2" 这种名字 —— RA 1.22 的配置里就是这个
   形式。本机实测（直接取两份 retroarch.cfg 里 input_player1_*_btn 的值）：
        L2=6   R2=7   L3=11  R3=12     ← 64 位与 retroarch32 那份完全一样
   另外按键位图（/proc/bus/input/devices 的 B: KEY=）解出来 17 个键码
   （303/304/306/307/309/310/311/312/313/314/315/316/317 + 544~547 方向键），
   与上面这套编号逐个吻合，可以互相印证。

   ⚠ 2026-09-16 之前这里写的是 L3=14 / R3=15（32 位那份写 16/16），那是
   照抄了**别的机器**的 autoconfig（GO-Super Gamepad）里的数字。本机 14 是
   **方向键「下」**、15 是「左」—— 选 L3 会变成"按住方向下快进"，同时方向键
   失灵。已按实测值改正，两份 cfg 共用一张表（本机两份编号一致，不必分开）。

   ⚠⚠ 为什么"单按 L2 没反应"：RA 的快进属于**热键**，热键都要先按住
   input_enable_hotkey_btn 那个门禁键才生效 —— Batocera wiki 的原话是
   "Making this 'nul' will disable the requirement for holding the hotkey
   first to activate commands with button presses"，并且把加速明确列为
   [HOTKEY]+[D-PAD RIGHT]。本机出厂门禁绑在 10 上、加速键是 "nul"，
   所以**按 L2 也好、L2+别的也好，都不会快进**（前端日志里连一条
   [RA加速键] 都没有，证明这个设置从来没写进盘 —— 见 sconf_load 里的自检）。

   ✘✘ 下面这个"方案 B"**当天下午就作废了** —— 用户改主意：整个加速功能都不要，
   连设置项一起删。原话："快捷键换回原来的方式  设置里面去掉加速快捷键选项"。
   现在的铁律是：**前端一个 RA 热键的字节都不写**，出厂是 "nul" 就保持 "nul"。
   保留这段是为了记住「门禁键为什么不能动」—— 那是一次真实事故，不是理论。

   【已作废的旧方案 B】门禁键锁死出厂值 10（guide 键），本设置只改"快进键"：
        按住 guide + L2    = 快进            ← [HOTKEY] + [快进键]
        按住 guide + START = 退出游戏
        按住 guide + X     = 打开 RA 菜单
        按住 guide + L1/R1 = 读档 / 存档
        按住 guide + B     = 重开游戏
        按住 guide + Y     = 截图
   玩家原来那套 guide+START / guide+X 的肌肉记忆**一字不改**。
   代价：快进不再能"单按某个键"触发（那是 RA 的模型限制，见下）。

   ⚠ 为什么不再"把门禁一起设成加速键"（曾经的实现，已废）：那样确实能做到
   "单按 L2 就快进"，但代价是**玩家原组合全部失效** —— 2026-09-16 真把人
   卡在游戏里十分钟出不来。以后再想动门禁键，先问一句"玩家原组合怎么办"。
   另一条退路保留着：input_menu_toggle_gamepad_combo = "4"（长按 START 开
   RA 菜单），不依赖门禁键。

   ⚠ 顺带把 input_toggle_fast_forward_btn 清成 "nul"：那是"按一下持续加速"的
   另一种绑法，留着会和"按住"打架（按一下进加速、再按一下才停）。
   ⚠ 快进键一旦选 L2/R2，还要清掉撞号的亮度热键（brightness_up=6 /
   brightness_down=7）—— 不然按住 guide+L2 会一边快进一边把亮度顶到最亮。
   编号 11/12 不撞任何非 player1 热键，见 ra_bright_fix()。

   ★ 现状（2026-09-16 之后）：上面这几个函数（ra_fastfwd_read_raw /
     ra_fastfwd_read / ra_fastfwd_write / ra_bright_fix）和下面那几个常量
     **已不再被任何代码调用**，留着只为"万一哪天要接回来"。
     构建带 -w，未使用的 static 函数不会报警告。 */
#define RAFWD_N 4
static const char *const RAFWD_LABEL[] = { "L2", "R2", "L3", "R3" };
static const char *const RAFWD_ID[]    = { "6",  "7",  "11", "12" };  /* 两份 cfg 通用 */
/* ★ 门禁键（input_enable_hotkey_btn）**恒定**写这个值 = 出厂值 = SDL 10 = guide 键。
   改这个常量等于换掉全机热键的门禁，会废掉玩家所有组合键，动手前先看上面那段。 */
#define RAFWD_GATE "10"
static int g_rafwd = 0;                         /* RAFWD_LABEL 的下标 */

/* 重扫索引。跟 vagaretro-front.sh 里那条（$py $SHELL_PY $CONF --gen-index）
   一字不差，只是把变量展开写死 —— 换卡后必须重扫，否则列表还是旧的。 */
#define ROM_INDEX_CMD \
    "/usr/bin/python3 /opt/vagaretro/vagaretro-shell.py /opt/vagaretro/config.ini --gen-index"
static const char *const BEZEL_LABEL[] = { "关闭", "960×720", "1024×768", "640×480", "720×720" };

/* ---- 摇杆灯（AW20036）--------------------------------------------------
   用户 2026-09-15："迷你龙这个LANGOS设置里面要加个摇杆灯调节"。

   硬件在 /sys/class/leds/aw20036_led/，MiniLoong 打过的补丁驱动多挂了几个
   自定义属性（实测全部可写）：
     miniloong_color       off red yellow green blue cyan magenta white
     miniloong_effect      solid pulse blink breathe rainbow off
     miniloong_brightness  0..255（max_brightness 也是 255）
     miniloong_pulse_rate / blink_rate / breathe_rate / rainbow_rate
   后四个的频率参数没法从界面调（一个灯而已），保持系统默认即可。

   ⚠ 电量模式是**另一个进程在管**：miniloong_led.service 跑
   /usr/local/bin/miniloong-charge-led.sh，它每 5~10 秒按电池状态刷一次
   颜色。想固定成某个颜色就必须先把它停掉（disable 掉，否则它 Restart=always
   会自己回来），不然玩家的颜色几秒后就被它覆盖，看起来像"设置没生效"。

   ⚠ 存档格式沿用官方脚本的那份（/home/ark/.config/miniloong_led_mode，
   三行：模式 / 亮度 / 灯效）。第 2 行官方 charge-led.sh 是**直接写进驱动**的
   （0..255），而 led-mode.sh 又当百分比换算 —— 两个脚本自己就不一致。
   我们统一按**驱动值 0..255** 存（跟正在跑的 charge-led.service 对齐），
   界面上显示百分比。 */
#define LED_DIR     "/sys/class/leds/aw20036_led"
#define LED_CONF    "/home/ark/.config/miniloong_led_mode"
#define LED_SERVICE "miniloong_led.service"
#define LED_MAXBRI  255

#define LED_MODE_N 10
static const char *const LED_MODE_LABEL[] = {
    "关闭", "电量", "红色", "黄色", "绿色", "蓝色", "青色", "品红", "白色", "彩虹"
};
static const char *const LED_MODE_VAL[] = {
    "off", "battery", "red", "yellow", "green", "blue", "cyan", "magenta", "white", "rainbow"
};

#define LED_FX_N 6
static const char *const LED_FX_LABEL[] = { "常亮", "呼吸", "闪烁", "脉冲", "追光", "炫彩" };
/* 前四档直接对应驱动的 miniloong_effect；后两档是"合成"出来的：
     · 炫彩 = 原生 rainbow（驱动自己做的平滑 RGB 混合）
     · 追光 = 驱动没有这一档，用软件跑马实现（见 langos-led-chase）
   存档里照写 chase/rainbow，落到硬件时再分流。 */
static const char *const LED_FX_VAL[]   = { "solid", "breathe", "blink", "pulse", "chase", "rainbow" };
#define LED_FX_CHASE 4
#define LED_FX_RAINBOW 5
/* 软件追光的小脚本 + 服务（跟着前端一起装到设备上） */
#define LED_CHASE_SH   "/usr/local/bin/langos-led-chase.sh"
#define LED_CHASE_SVC  "langos-led-chase.service"

/* ---- 硬件能力探测（2026-09-21，为 RK3326 平台移植加）--------------------
 *
 *  ★ 背景：前端从 MINILOONG(RK3566) 移植到 RK3326（R36H Pro Max / A10Mini
 *    / xu10 这些 dArkOS 克隆机）时，有几样**只有迷你龙有**的外设：
 *      · aw20036 摇杆灯（LED_DIR）
 *      · batteryplus 算出来的 /tmp/battery.percent
 *      · /usr/local/bin/auto_suspend_update.sh
 *    这些在 3326 上都不存在。原代码里 LED_DIR 是**直接拼路径写**的，
 *    在 3326 上会静默失败（写不进去、界面不报错），但更糟的是**设置页会
 *    多出一整页"摇杆灯"，玩家点进去三项全都改不动** —— 这才是要治的。
 *
 *  ★ 策略（浪总 2026-09-21 定）：**保留 + 加降级** ——
 *    不删任何功能，统一用 hw_probe() 在启动时探一次硬件，探不到就
 *    · 把设置页里对应的**入口整项藏掉**（跟金手指"库里没有就藏项"一个思路）
 *    · 相关写入函数直接返回失败并给中文原因，不静默
 *    这样同一份源码两个平台通吃，以后加新机型只要往 hw_probe() 里加一条。
 *
 *  ⚠ 探测只在启动时跑一次，结果缓存在全局变量里 —— 这些硬件不会热插拔。
 * ========================================================================*/
static int g_hw_led   = -1;   /* -1=还没探  0=没有摇杆灯  1=有 */

static void hw_probe(void)
{
    /* 摇杆灯：认 LED_DIR 这个目录本身。aw20036 驱动只装在迷你龙上，
       3326 的 /sys/class/leds/ 下是别的名字（或者根本没有）。
       ★ 这里只探这一项 —— 电量（battery_pct）和息屏（SV_SLEEP）那两处
         本来就是容错的，各自在使用点降级，不需要在这里预判。 */
    g_hw_led = (access(LED_DIR, F_OK) == 0) ? 1 : 0;

    printf("硬件探测: 摇杆灯=%s\n", g_hw_led ? "有" : "无（设置页会藏掉那一项）");
    fflush(stdout);
}

/* 设置页「显示」分组里，摇杆灯入口是不是要藏掉。
   （IT_DISP 的最后一项就是它，藏了就少一行 —— MENUS[M_DISP].n 跟着减。） */
static int hw_has_led(void) { return g_hw_led > 0; }

/* ---- RetroArch 主题（用户 2026-09-16："设置里面加个 RA 主题调节功能"）----

   RA 的外观分两层，配置键名是从设备上的 RA 二进制里 strings 出来的（实测）：
     ① menu_driver          菜单整体风格  xmb / ozone / rgui / glui / switch
     ② 具体配色主题，随风格而变：
        · xmb    → xmb_theme                    （9 套，见 assets/xmb/ 目录名）
        · ozone  → ozone_menu_color_theme        （内置编号）
        · rgui   → rgui_menu_color_theme（内置）+ rgui_menu_theme_preset（30 个 .cfg）
   本机出厂 menu_driver=rgui，玩家多半没见过 XMB —— 所以这一项做成
   「先选风格，XMB 再挑主题」，只暴露最好看也最好选的那条链。
   其它风格只切 menu_driver，配色留给 RA 自己内置的那几档。

   ⚠ 跟 RA 语言/加速键一样：**只写盘、不重启前端**（RA 的外观跟前端界面无关），
   两份 cfg 都写（PS1 那份走 retroarch32）。 */
#define RAMENU_N 5
static const char *const RAMENU_LABEL[] = { "XMB", "OZONE", "RGUI", "GLUI", "SWITCH" };
static const char *const RAMENU_VAL[]   = { "xmb", "ozone", "rgui", "glui", "switch" };

#define RAXMB_N 9
static const char *const RAXMB_LABEL[] = {
    "自动", "Daite", "Dot-Art", "FlatUI", "FlatUX", "Monochrome", "Pixel", "RetroSystem", "Systematic"
};
static const char *const RAXMB_VAL[] = {
    "automatic", "daite", "dot-art", "flatui", "flatux", "monochrome", "pixel", "retrosystem", "systematic"
};

/* RetroArch 帧数显示（fps_show）。2026-09-17 用户："设置里面加一个 ra 全局帧数显示开关"。
   RA 把实时帧率画在画面**左上角**，开关就是这个布尔键 —— 它是**全局**的：
   所有走 RA 核心的平台（FC/SFC/GBA/PS1/街机…）一起生效，不用逐核心配。
   独立模拟器（NDS/SS/PSP/PICO8）不归它管，那些各有各的显示方式。

   ⚠ **前置条件：`video_font_enable = "true"`**（RA 的 OSD 文字总开关）——
     它关着的话 fps_show 开了也一个字都不画，属于"设了没反应"的静默失效。
     实测两份 cfg 里都是 true，所以这一项只动 fps_show 就够。
   ⚠ 跟 RA 语言/主题一样：**只写盘、不重启前端**（OSD 跟前端界面无关），
     下次进游戏生效。`config_save_on_exit = "false"` ⇒ RA 退出不回写 cfg，
     所以这里写下去的值就是最终态，不会被 RA 自己覆盖。
   ⚠ `fps_update_interval`（两份都是 60）= 每 60 帧刷新一次读数，
     60fps 下正好一秒一跳，是 RA 出厂值，不用动。 */
#define RAFPS_N 2
static const char *const RAFPS_LABEL[] = { "关闭", "开启" };
static const char *const RAFPS_VAL[]   = { "false", "true" };

/* 虚拟内存的可选项。放在这儿（而不是 swap_apply 旁边）是因为
   性能菜单的条目表在后面一点的 IT_PERF 里就要引用 SWAP_LABEL / SWAP_N。
   上限 1024MB 的理由见 swap_apply 上面的注释。 */
#define SWAPFILE   "/opt/vagaretro/swapfile"
#define SWAP_N     4
static const char *const SWAP_MB[]    = { "0", "256", "512", "1024" };
static const char *const SWAP_LABEL[] = { "关闭", "256 MB", "512 MB", "1024 MB" };

/* 时区表。用 IANA 名 —— timedatectl 认的就是它们，不另造中文缩写。
   出厂默认是 America/New_York（ArkOS 的老底子），中文用户拿到手时钟就是
   错的（屏幕上显示 EDT），所以这一项是**真需要**的，不是凑数。
   表里只放常用的几个：全部 zoneinfo 有 600 多个，翻一遍要按几百下。 */
static const char *const TZ_CHOICES[] = {
    "Asia/Shanghai",      /* 中国标准时间 UTC+8 */
    "Asia/Tokyo",         /* UTC+9  */
    "Asia/Seoul",         /* UTC+9  */
    "Asia/Kolkata",       /* UTC+5:30 */
    "UTC",                /* 不偏移 */
    "Europe/London",
    "Europe/Berlin",
    "America/New_York",   /* 本机出厂值 */
    "America/Los_Angeles",
};

static const SItem IT_ROOT[] = {
    { .name = "SOUND SETTINGS",       .cn = "声音设置", .type = ST_MENU, .menu = M_SOUND,
      .hint = "系统音量（amixer + alsactl），改完立即生效" },
    { .name = "DISPLAY SETTINGS",     .cn = "显示设置", .type = ST_MENU, .menu = M_DISP,
      .hint = "屏幕亮度，以及主页/游戏列表的列数" },
    { .name = "THEME",                .cn = "主题外观", .type = ST_MENU, .menu = M_THEME,
      .hint = "换一套界面配色。把 .theme 文件放进内存卡的 themes 文件夹就能加自己的" },
    { .name = "NETWORK & SSH",        .cn = "网络与传输", .type = ST_MENU, .menu = M_NET,
      .hint = "连 WiFi、开关 SSH —— 传 ROM 和改文件都靠它" },
    { .name = "PERFORMANCE SETTINGS", .cn = "性能设置", .type = ST_MENU, .menu = M_PERF,
      .hint = "CPU / GPU / 内存的调频策略" },
    { .name = "TIME & REGION",        .cn = "时间与地区", .type = ST_MENU, .menu = M_REGION,
      .hint = "时区（出厂就是中国标准时间）+ 当前系统时间" },
    /* 用户 2026-09-15："设置里面加个读取卡一或者卡二游戏 切换"。
       宿主本来就是"两卡都扫、卡二优先"，这一行让玩家能锁定读哪一张。
       改完要重扫索引（几秒）+ 重启前端。 */
    { .name = "GAME SOURCE",          .cn = "游戏来源", .type = ST_VAL, .src = SV_ROMSRC,
      .choices = ROMSRC_LABEL, .nch = ROMSRC_N,
      .hint = "自动 = 插了卡二就读卡二，没插就读卡一；锁定卡一/卡二则只读那一张。改完自动重扫游戏列表并重启前端" },

    /* 用户 2026-09-17：「退出自动保存存档功能  自动保存默认在状态槽9
       设置里面可以修改状态槽0-10状态槽」。
       紧挨着「游戏来源」—— 两项都是"启动游戏时怎么干活"的设置。
       ⚠ 这一项只写 vgfront.conf；真正读它的是**启动器**，所以改完
         **不用重启前端**，下次进游戏生效。 */
    { .name = "AUTO-SAVE SLOT",       .cn = "自动存档位", .type = ST_VAL, .src = SV_AUTOSLOT,
      .vmin = AUTOSLOT_MIN, .vmax = AUTOSLOT_MAX, .step = 1,
      .hint = "「退出自动存档」把进度存到第几个状态槽，出厂 9（跟玩家手动的 0~5 位错开，免得盖掉手动存档）。下次进游戏生效" },
    /* 用户 2026-09-15："设备里面 ra 语言切换"。
       跟遮罩那项一样属于"只写盘"：RA 的菜单语言影响不到前端界面本身，
       所以**不重启前端**，改完下次进 RA 就生效。 */
    /* 用户 2026-09-15："设置里面再加个系统语言选项 可以切换我们系统的界面语言
       中英双语就够了"。跟前一个 RA 界面语言**不是一回事** —— 那个改的是
       RetroArch 自己的菜单，这个改的是本前端。所以两项挨着放、名字都带
       "界面语言"，到底哪个是哪个一眼能分清。 */
    { .name = "SYSTEM LANGUAGE",      .cn = "系统语言", .type = ST_VAL, .src = SV_UILANG,
      .choices = UILANG_LABEL, .nch = UILANG_N,
      .hint = "系统语言：中文 / English。改完自动重启前端" },
    { .name = "RA LANGUAGE",          .cn = "RA 界面语言", .type = ST_VAL, .src = SV_RALANG,
      .choices = RALANG_LABEL, .nch = RALANG_N,
      .hint = "改 RetroArch 自己的菜单语言（写 retroarch.cfg，下次进游戏生效）。前端界面语言不受影响；PS1 那份配置也一起改" },
    /* ★ 2026-09-16：「RA 加速按键」这一项**已按用户要求删除**（原话：
       "快捷键换回原来的方式  设置里面去掉加速快捷键选项"）。
       原因见 ra_fastfwd_* 那段的墓碑注释 —— 总之 RA 的快进是"热键"，
       想让它变成能自定义的单键，就必须动 input_enable_hotkey_btn 那个门禁，
       而门禁一动，玩家整套 guide + XXX 就全废了。得不偿失，不做了。
       把下面这几行原样放回去就能恢复这个功能（函数和常量都还在，
       只是不再被引用）。 */
    /* 用户 2026-09-16："设置里面加个 RA 主题调节功能"。
       和上面两项一样是"只写盘、不重启前端"——RA 的菜单外观跟前端界面无关。 */
    { .name = "RA MENU STYLE",        .cn = "RA 菜单风格", .type = ST_VAL, .src = SV_RAMENU,
      .choices = RAMENU_LABEL, .nch = RAMENU_N,
      .hint = "游戏里 RetroArch 菜单的整体外观。XMB 是横版大图标（配色主题最多，下面那项就挑它），出厂是 RGUI" },
    { .name = "RA THEME",             .cn = "RA 主题配色", .type = ST_VAL, .src = SV_RATHEME,
      .choices = RAXMB_LABEL, .nch = RAXMB_N,
      .hint = "RetroArch 的配色主题（共 9 套）。只在「RA 菜单风格」选 XMB 时生效，其它风格用 RA 内置的那几档" },
    /* 2026-09-17 用户："设置里面加一个 ra 全局帧数显示开关"。
       放在 RA 那三项后面 —— 都是"游戏里 RA 长什么样"的设置。
       同样是只写盘、不重启前端，下次进游戏生效。 */
    { .name = "RA FPS DISPLAY",       .cn = "RA 帧数显示", .type = ST_VAL, .src = SV_RAFPS,
      .choices = RAFPS_LABEL, .nch = RAFPS_N,
      .hint = "游戏里画面左上角显示实时帧率（RA 的 OSD 文字），所有 RA 核心的平台一起生效。下次进游戏生效" },
    { .name = "GAME CHEATS",          .cn = "金手指", .type = ST_MENU, .menu = M_CHEATS,
      .hint = "一键安装中文金手指合集（RA + PPSSPP 两边的目录都放到位）" },
    { .name = "SYSTEM INFO",          .cn = "系统信息", .type = ST_MENU, .menu = M_INFO,
      .hint = "只读，看看当前设备状态" },
    /* ★ 2026-09-16：用户"快捷键说明书做到设置里面  打开就可以看"。
       放在系统信息旁边 —— 都是"查东西"的项，跟上面那些"改东西"的分开。
       不破坏任何状态，所以按一次 A 直接进（见 setting_confirm 的特判）。 */
    { .name = "HOTKEY GUIDE",         .cn = "快捷键说明", .type = ST_BUTTON,
      .action = SA_HELP,
      .hint = "前端、弹层、游戏里、系统四级快捷键全表。可滚动，B 返回" },
    /* 用户 2026-09-14："设置里加上关机和重启的功能" —— 功能本来就在这一页，
       但原来叫"退出"，玩家不会想到关机藏在里面。改成"电源 / 退出"直说。 */
    { .name = "POWER & QUIT",         .cn = "电源 / 退出", .type = ST_MENU, .menu = M_QUIT,
      .hint = "关机、重启设备、重启界面 —— 都在这一页（要按两次 A 确认）" },
};

static const SItem IT_SOUND[] = {
    { .name = "SYSTEM VOLUME", .cn = "系统音量", .type = ST_VAL, .src = SV_VOLUME,
      .vmin = 0, .vmax = 100, .step = 5,
      .hint = "写 RK817 codec 的主音量，并 alsactl store 落盘" },
};

/* ---- 息屏时间（= 官方 autosuspend 的空闲阈值）-----------------------------
   用户 2026-09-17："设置加个息屏时间调整吧 不然调试容易黑屏"。
   真源只有一个：/home/ark/.config/.TIMEOUT —— 官方 /usr/local/bin/auto_suspend.py
   读它当空闲分钟数（闲置到点 **且** 没充电 **且** 没在跑游戏 → systemctl suspend，
   走 systemd-sleep 钩子把背光写 0，就是"黑屏"那一步）；内容是 "Off" 时，
   /usr/local/bin/auto_suspend_update.sh 会把 autosuspend 停掉并 disable。
   所以这一项**不用自带守护进程**，只写一个文件 + 跑官方开关脚本：
     · SLEEP_LABEL[] 只是界面文案，SLEEP_VAL[] 才是写进 .TIMEOUT 的值
     · 两张表下标一一对应，**别往中间插档**（插了老配置会显示错档）
   出厂是 10 分钟（官方 Restore 脚本写的就是它）。改完立即生效，不重启前端。 */
#define SLEEP_TIMEOUT_PATH "/home/ark/.config/.TIMEOUT"
#define SLEEP_UPDATE_SH    "/usr/local/bin/auto_suspend_update.sh"
static const char *const SLEEP_LABEL[] = { "关闭", "5 分钟", "10 分钟", "15 分钟", "30 分钟", "60 分钟" };
static const char *const SLEEP_VAL[]   = { "Off",  "5",      "10",      "15",      "30",      "60" };
#define SLEEP_N ((int)(sizeof(SLEEP_LABEL) / sizeof(SLEEP_LABEL[0])))

static const SItem IT_DISP[] = {
    { .name = "BACKLIGHT",        .cn = "屏幕亮度", .type = ST_VAL, .src = SV_BRIGHT,
      .vmin = 5, .vmax = 100, .step = 5,
      .hint = "直接写 backlight 的 brightness（按 max_brightness 换算成百分比）" },

    /* 用户 2026-09-17："设置加个息屏时间调整吧 不然调试容易黑屏"。
       走系统自带的自动待机（autosuspend.service + .TIMEOUT），不新增守护进程；
       选「关闭」= stop + disable 那个服务。放在「屏幕亮度」旁边 —— 都是屏幕行为。 */
    { .name = "SCREEN TIMEOUT",   .cn = "息屏时间", .type = ST_VAL, .src = SV_SLEEP,
      .choices = SLEEP_LABEL, .nch = SLEEP_N,
      .hint = "闲置多久自动息屏待机（背光灭 + 进待机，省电）。调试时嫌它老黑屏就选「关闭」。改完立即生效" },
    { .name = "HOME COLUMNS",     .cn = "主页列数", .type = ST_VAL, .src = SV_HCOLS,
      .choices = COLS_HOME, .nch = 3,
      .hint = "主页平台网格一排几个。列少了卡片大、列多了信息密" },
    { .name = "GAMELIST COLUMNS", .cn = "游戏列数", .type = ST_VAL, .src = SV_GCOLS,
      .choices = COLS_GAME, .nch = 4,
      .hint = "游戏列表一排几个。存 vgfront.conf，下次启动仍是这个值" },
    /* 封面样式 —— 2026-09-14 用户要求。竖版是原生比例（也是出厂值），
       正方形一屏能多放一行；两种都是等比裁切铺满，不会把封面拉变形。 */
    { .name = "COVER STYLE",      .cn = "游戏封面样式", .type = ST_VAL, .src = SV_COVERSTY,
      .choices = COVER_STY, .nch = 2,
      .hint = "竖版 = 高是宽的 1.33 倍（传统卡盒比例）；正方形 = 1:1，一屏多一行。封面自动裁切适配" },
    /* 游戏遮罩 —— 2026-09-14 用户要求。遮罩包内置在系统里，这里选一套。
       ★★ 2026-09-18（浪总："什么固件什么dtb你就默认对应的遮罩呗"）：
         默认值不再是"关闭"，改成**按本机屏幕自动定档** —— 本机 MINILOONG
         会自动落到 960×720（见 bezel_auto_detect）。玩家手工选过就以他的为准。
       ⚠ 这里列的是**所有可能的档**，不代表都装了包。选到没装的那档不会崩：
         启动器发现 `bezels/<档>/` 里没这个平台就跳过遮罩（日志写"没有对应
         目录"），画面仍按核心比例显示。
       ⚠ 改完**不用重启前端** —— 前端只记选择，下次进游戏才生效。 */
    { .name = "GAME BEZEL",       .cn = "游戏遮罩", .type = ST_VAL, .src = SV_BEZEL,
      .choices = BEZEL_LABEL, .nch = BEZEL_N,
      .hint = "给游戏画面套一层主机边框，画面按核心原生比例显示不错位。默认按本机屏幕自动选，下次进游戏生效" },
    /* 字号两项 —— 2026-09-14 用户要求。设备是 4 寸屏，设计基准却是
       1920x1080，所有字号上屏都被缩到 53%，不少人反馈看不清。
       默认 100% 就是设计值；改完**立即重启前端**（字是烘进纹理的，
       不重启不生效，见 sv_apply 里的说明）。 */
    { .name = "UI FONT SIZE",     .cn = "前端字体大小", .type = ST_VAL, .src = SV_FSUI,
      .vmin = FS_PCT_MIN, .vmax = FS_PCT_MAX, .step = FS_PCT_STEP,
      .hint = "整个前端的字号（顶栏、标签、设置页）。左右调，改完自动重启前端" },
    { .name = "GAMELIST FONT",    .cn = "游戏列表字号", .type = ST_VAL, .src = SV_FSGAME,
      .vmin = FS_PCT_MIN, .vmax = FS_GAME_MAX, .step = FS_PCT_STEP,
      .hint = "游戏列表单独再调一档，在「前端字体大小」的基础上乘，最大 300%。调完自动重启" },
    /* 摇杆灯（用户 2026-09-15 要求）。归在"显示设置"里 —— 掌机上灯光就是
       外观的一部分，玩家找它第一反应是显示设置，不是声音。 */
    { .name = "JOYSTICK LED",     .cn = "摇杆灯", .type = ST_MENU, .menu = M_LED,
      .hint = "两侧摇杆的 RGB 灯：颜色、亮度、灯效。出厂是随电量变色" },
};

/* 摇杆灯子页。三项都是"改完立即生效"，没有需要重启前端的（灯是内核侧的）。 */
static const SItem IT_LED[] = {
    { .name = "LED MODE",     .cn = "灯光模式", .type = ST_VAL, .src = SV_LED_MODE,
      .choices = LED_MODE_LABEL, .nch = LED_MODE_N,
      .hint = "「电量」= 按电池状态变色（出厂）；选固定颜色后灯就一直亮那个色；彩虹则是自动流动。选固定颜色时若灯效还停在追光/炫彩，会自动切成常亮（否则颜色会被那两个效果盖掉）" },
    { .name = "LED BRIGHTNESS", .cn = "灯光亮度", .type = ST_VAL, .src = SV_LED_BRIGHT,
      .vmin = 4, .vmax = 100, .step = 4,
      .hint = "摇杆灯的亮度。夜里嫌刺眼往下调，最低一档几乎看不见但还留着指示" },
    { .name = "LED EFFECT",   .cn = "灯光效果", .type = ST_VAL, .src = SV_LED_FX,
      .choices = LED_FX_LABEL, .nch = LED_FX_N,
      .hint = "灯的变化方式：常亮最省电；追光是颜色沿色环一格一格往前跑，炫彩是平滑的彩色流动；「电量」模式下这一项不生效" },
};

/* ---- 网络与传输（M_NET）------------------------------------------------
   掌机上这两件事是连在一起用的：连上 WiFi → 用 SSH 从电脑塞 ROM 进来。
   所以放在同一页：上面是无线，下面是 SSH。

   ⚠ WiFi 列表是**动态**的（每次扫描结果不同），排在 IT_WIFI_DYN 里，
   由 wifi_scan() 填内容、MENUS[M_WIFI].n 记条数。见下面 wifi_scan()。 */
static const SItem IT_NET[] = {
    { .name = "WIFI NETWORK", .cn = "无线网络", .type = ST_MENU, .menu = M_WIFI,
      .hint = "扫描附近的无线网，选一个连（没连过的会让你输密码）" },
    { .name = "WIFI RADIO",   .cn = "无线开关", .type = ST_TOGGLE, .src = SV_WIFI_RADIO,
      .hint = "关掉无线可以省电；关着的时候一个网络也扫不到" },
    { .name = "CURRENT WIFI", .cn = "当前连接", .type = ST_RO, .src = SV_I_WIFI_CUR,
      .hint = "现在连着的无线名和它的 IP 地址" },
    { .name = "SSH",          .cn = "SSH 服务", .type = ST_TOGGLE, .src = SV_SSH,
      .hint = "开着才能用电脑传 ROM / 传封面 / 改文件（账号 root，端口 22）" },
    { .name = "SSH ADDRESS",  .cn = "SSH 地址", .type = ST_RO, .src = SV_I_SSH_ADDR,
      .hint = "在电脑上就 ssh 这个串。电脑要和掌机连同一个 WiFi" },
};

#define WIFI_MAX 24

/* WiFi 扫描结果的动态行。名字用一个通用槽位，具体 SSID 放在这里；
   右侧的值显示信号格数与是否已存过密码。 */
static SItem IT_WIFI_DYN[WIFI_MAX + 1];
static int   g_wifi_n = 0;

static const SItem IT_PERF[] = {
    { .name = "CPU GOVERNOR", .cn = "CPU 调速器",  .type = ST_VAL, .src = SV_CPUGOV,
      .choices = GOV_CPU, .nch = 6,
      .hint = "平时用省电的，玩 3D 游戏前可以切 performance（启动游戏时 LANGOS 会临时切）" },
    { .name = "GPU GOVERNOR", .cn = "GPU 调速器",  .type = ST_VAL, .src = SV_GPUGOV,
      .choices = GOV_DEVFREQ, .nch = 5,
      .hint = "Mali-G31 的 devfreq 策略，卡顿或发热可以在这里换" },
    { .name = "DMC GOVERNOR", .cn = "内存调速器", .type = ST_VAL, .src = SV_DMCGOV,
      .choices = GOV_DEVFREQ, .nch = 5,
      .hint = "DDR 频率策略" },
    /* 虚拟内存 —— 2026-09-14 用户要求，且点名要 1024MB 这一档。
       这台机器 917MB 物理内存，挂点 swap 能少被杀几次进程（代价是卡读写慢），
       所以做成可关；上限卡在 1024MB，理由见 swap_apply 上面那段注释。 */
    { .name = "VIRTUAL MEMORY", .cn = "虚拟内存", .type = ST_VAL, .src = SV_SWAP,
      .choices = SWAP_LABEL, .nch = SWAP_N,
      .hint = "用根分区的交换文件顶内存（放 /roms 会吃掉本就不多的卡空间）。改完立即生效" },
};

/* 金手指。用户 2026-09-14："设置加个一键安装金手指功能 自动把金手指文件
   解压到 RA 对应目录"。装的是那个中文合集（6586 个文件）：
   RA 的 .cht 2152 个 + PPSSPP 的 .ini 4308 个，实现见 cheat_install()。 */
static const SItem IT_CHEATS[] = {
    { .name = "INSTALL CHEATS", .cn = "一键安装金手指", .type = ST_BUTTON,
      .action = SA_INSTALL_CHEATS,
      .hint = "把金手指包装进 RA（.cht，2152 个）和 PPSSPP（.ini，4308 个）。"
              "约半分钟，装完按 RA 菜单→金手指→加载金手指文件" },
    { .name = "INSTALLED",      .cn = "已装金手指", .type = ST_RO, .src = SV_I_CHEATS,
      .hint = "RA 的 cheats/中文金手指 下现有多少个 .cht（整包是 2152 个）" },
    { .name = "PACK SOURCE",    .cn = "金手指包", .type = ST_RO, .src = SV_I_CHEATPACK,
      .hint = "内存卡 /roms/vagaretro/cheats.zip 优先；没有就用内置的那份" },
    { .name = "REMOVE CHEATS",  .cn = "清除金手指", .type = ST_BUTTON,
      .action = SA_REMOVE_CHEATS,
      .hint = "只删中文包，RA 自带的官方金手指库（那 76 个目录）原样保留" },
};

/* 时间与地区。CLOCK 从"系统信息"挪到这里 —— 时区是这一页调的东西，
   改完当场就能在这一页最下面看到时间跳过来，比跑回信息页验证顺手。 */
static const SItem IT_REGION[] = {
    { .name = "TIMEZONE", .cn = "时区", .type = ST_VAL, .src = SV_TZ,
      .choices = TZ_CHOICES, .nch = (int)(sizeof(TZ_CHOICES) / sizeof(TZ_CHOICES[0])),
      .hint = "改完立即生效（timedatectl set-timezone）；顶栏的时钟跟着走" },
    { .name = "CLOCK",    .cn = "当前时间", .type = ST_RO, .src = SV_I_TIME,
      .hint = "系统时间。时间不对先看上面那项时区对不对" },
};

/* 只读状态。顺序 = 掌机上的实际关注频率：还剩多少电、能不能连上、
   卡满了没有 —— 这三件排在最前面，剩下的是"要报 bug 时才来抄"。 */
static const SItem IT_INFO[] = {
    { .name = "BATTERY",   .cn = "电池",     .type = ST_RO, .src = SV_I_BATT,
      .hint = "读 /sys/class/power_supply 的 battery 节点（容量 + 充放电状态）" },
    { .name = "NETWORK",   .cn = "网络",     .type = ST_RO, .src = SV_I_NET,
      .hint = "IPv4 地址（已滤掉 IPv6 长串）—— 传 ROM、开 SSH 都要先知道这个" },
    { .name = "STORAGE",   .cn = "存储",     .type = ST_RO, .src = SV_I_DISK,
      .hint = "TF1 / TF2 的剩余空间，卡掉了或没挂上一眼能看出来" },
    { .name = "SCREEN",    .cn = "屏幕",     .type = ST_RO, .src = SV_I_RES },
    { .name = "DEVICE",    .cn = "设备型号", .type = ST_RO, .src = SV_I_DEV },
    { .name = "KERNEL",    .cn = "内核",     .type = ST_RO, .src = SV_I_KERN },
    { .name = "CPU",       .cn = "处理器",   .type = ST_RO, .src = SV_I_CPU },
    { .name = "MEMORY",    .cn = "内存",     .type = ST_RO, .src = SV_I_MEM },
    { .name = "GOVERNORS", .cn = "当前调频", .type = ST_RO, .src = SV_I_GOV },
    { .name = "ROM ROOT",  .cn = "ROM 目录", .type = ST_RO, .src = SV_I_ROM },
    { .name = "INDEX",     .cn = "索引",     .type = ST_RO, .src = SV_I_INDEX },
    { .name = "FRONTEND",  .cn = "前端",     .type = ST_RO, .src = SV_I_FRONT },
};

static const SItem IT_QUIT[] = {
    /* 顺序按"越常用越靠前"排：玩家要收工了，多半是关机或重启设备；
       "重启界面"是修界面卡住用的，排在最后。三项都要按两次 A 确认。 */
    { .name = "SHUTDOWN SYSTEM",  .cn = "关机",     .type = ST_BUTTON, .action = SA_SHUTDOWN,
      .hint = "整机断电（systemctl poweroff）。按 A 两次确认" },
    { .name = "REBOOT SYSTEM",    .cn = "重启设备", .type = ST_BUTTON, .action = SA_REBOOT,
      .hint = "整机重启（systemctl reboot）。按 A 两次确认" },
    { .name = "RESTART FRONTEND", .cn = "重启界面", .type = ST_BUTTON, .action = SA_RESTART_FRONT,
      .hint = "只重开这个界面，不动系统；退出前的页面会保留" },
};

/* ---- 主题菜单（M_THEME）的动态行 ----------------------------------------
   和 WiFi 那页一样是运行期填的：内置主题是常量，但内存卡 themes 文件夹里
   有几套主题要扫了才知道。行内容由 theme_scan() 填。

   ⚠ 这里的 SItem.name 指向的是**长期存活**的字符串（内置表里的字面量、
   或 g_utheme[] 里的缓冲），不能指向局部数组。 */
#define THEME_MAX 24
static SItem IT_THEME_DYN[THEME_MAX + 1];
static int   g_theme_n = 0;

typedef struct { const char *title; const SItem *items; int n; } SMenu;

/* ⚠ 顺序必须与上面 M_* 下标严格一致。
   这张表**不能加 const** —— M_WIFI 那一项的 n 是扫描完才填的。 */
#define IT_DISP_N ((int)(sizeof(IT_DISP) / sizeof(IT_DISP[0])))

static SMenu MENUS[] = {
    { "设置",       IT_ROOT,     (int)(sizeof(IT_ROOT)  / sizeof(IT_ROOT[0]))  },
    { "声音",       IT_SOUND,    (int)(sizeof(IT_SOUND) / sizeof(IT_SOUND[0])) },
    /* ★ 显示页的最后一项是「摇杆灯」—— 迷你龙专属。没有 aw20036 的机器
       （RK3326 全体）上必须把它藏掉，否则玩家点进去三项全改不动。
       条数在 main() 里 hw_probe() 之后按 g_hw_led 覆写（见 main）。 */
    { "显示",       IT_DISP,     IT_DISP_N  },
    { "主题",       IT_THEME_DYN, 0 },          /* 条数由 theme_scan() 填 */
    { "网络与传输", IT_NET,      (int)(sizeof(IT_NET)   / sizeof(IT_NET[0]))   },
    { "无线网络",   IT_WIFI_DYN, 0 },          /* 条数由 wifi_scan() 填 */
    { "性能",       IT_PERF,     (int)(sizeof(IT_PERF)  / sizeof(IT_PERF[0]))  },
    { "时间与地区", IT_REGION,   (int)(sizeof(IT_REGION)/ sizeof(IT_REGION[0]))},
    { "系统信息",   IT_INFO,     (int)(sizeof(IT_INFO)  / sizeof(IT_INFO[0]))  },
    { "退出",       IT_QUIT,     (int)(sizeof(IT_QUIT)  / sizeof(IT_QUIT[0]))  },
    /* M_CHEATS=10：根菜单里的位置由 IT_ROOT 的物理顺序定（排在"系统信息"前），
       这里只决定下标，两者互不影响。 */
    { "金手指",     IT_CHEATS,   (int)(sizeof(IT_CHEATS)/ sizeof(IT_CHEATS[0]))},
    /* M_LED=11：摇杆灯子页。入口挂在"显示设置"里，不占根菜单的位置。 */
    { "摇杆灯",     IT_LED,      (int)(sizeof(IT_LED)   / sizeof(IT_LED[0]))   },
};
#define NMENU ((int)(sizeof(MENUS) / sizeof(MENUS[0])))

/* 设置页状态（声明在「4. 界面状态」那一段，绘制函数被 render() 提前调用） */
static int  g_setmenu   = M_ROOT;
static int  g_setrow    = 0;
static int  g_settop    = 0;      /* 滚动偏移，单位是"行"（行高固定，按行滚更简单） */
static int  g_setedit   = 0;      /* 1 = 危险动作待确认（网页版 settingsCur.editing） */
static char g_flash[160];         /* 底部回执（"音量设为 45%"），几秒后自动消失 */
static Uint32 g_flash_t0 = 0;

/* 取当前菜单。带越界兜底 —— g_setmenu 是从"当前菜单 + 动作"推出来的，
   万一哪天表改了顺序而这里漏改，兜回根菜单总好过越界读结构体。
   ⚠ 必须定义在 g_setmenu 之后、MENUS 之后。 */
static const SMenu *cur_menu(void)
{
    if (g_setmenu < 0 || g_setmenu >= NMENU) g_setmenu = M_ROOT;
    return &MENUS[g_setmenu];
}

/* ==========================================================================
 *  主题
 *
 *  自定义主题放内存卡：/roms/themes/<任意名>.theme
 *  （也认 /opt/vagaretro/themes/，给"系统自带但不进固件"的用）
 *
 *  ⚠ 为什么切换主题要**重启前端进程**：
 *  颜色是烘进纹理的 —— 顶栏 LOGO、8 个 tab 的字、平台卡片底、底栏键帽……
 *  运行期换调色板等于要把几十张 static 纹理全部重烘一遍。execv 自己只要
 *  一秒，还能保证没有漏网的旧颜色，比维护一套"纹理失效代"可靠得多。
 *  重启时带上 --settings M_THEME --setrow N，用户会回到主题列表原地，
 *  可以连着一个一个试。
 * ==========================================================================*/
#define THEME_DIR_ROMS "/roms/themes"
#define THEME_DIR_OPT  "/opt/vagaretro/themes"

typedef struct {
    char   id[48];
    char   name[64];
    char   path[288];
    Uint32 c[TH_N];
    int    cd[CD_N];    /* 卡片外观（cd_* 字段），缺省 = CD_DEF */
    char   cover[288];  /* 卡片封面图目录（cd_cover_dir），空 = 不用自定义图 */
} UserTheme;

static UserTheme g_utheme[THEME_MAX];
static int       g_utheme_n = 0;

/* 认 0xRRGGBB / #RRGGBB / RRGGBB 三种写法 */
static int th_parse_color(const char *s, Uint32 *out)
{
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '#') s++;
    else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    if (!*s) return 0;
    char *end = NULL;
    long v = strtol(s, &end, 16);
    if (end == s || v < 0 || v > 0xffffffL) return 0;
    *out = (Uint32)v;
    return 1;
}

/* 读一个 .theme。没写的字段保持"内置第 0 套"的值 —— 这样用户可以只写
   两三个想改的颜色，其余自动跟着走。 */
static int th_read_file(const char *path, UserTheme *ut)
{
    FILE *fh = fopen(path, "r");
    if (!fh) return 0;
    memcpy(ut->c, THEME_BUILTIN[0].c, sizeof(ut->c));
    memcpy(ut->cd, CD_DEF, sizeof(ut->cd));
    ut->cover[0] = '\0';

    char line[512];
    while (fgets(line, sizeof(line), fh)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '\n' || *p == '\r' || *p == '#' || *p == ';') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = p, *v = eq + 1;

        char *ke = k + strlen(k);                 /* key 去尾随空白 */
        while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t')) *--ke = '\0';
        while (*v == ' ' || *v == '\t') v++;      /* value 去首尾空白 */
        char *ve = v + strlen(v);
        while (ve > v && (ve[-1] == '\n' || ve[-1] == '\r' ||
                          ve[-1] == ' '  || ve[-1] == '\t')) *--ve = '\0';

        if (strcmp(k, "id") == 0) {
            if (*v) snprintf(ut->id, sizeof(ut->id), "%s", v);
        } else if (strcmp(k, "name") == 0) {
            if (*v) snprintf(ut->name, sizeof(ut->name), "%s", v);
        } else if (strcmp(k, "cd_cover_dir") == 0) {
            /* 卡片封面图目录。绝对路径直接用；写相对路径时按**主题文件所在
               目录**算 —— 这样"一个 .theme + 一个图目录"能整包发给别人，
               对方不用改路径（卡一卡二的挂载点不一样，写死 /roms 会翻车）。 */
            if (*v) {
                if (v[0] == '/') {
                    snprintf(ut->cover, sizeof(ut->cover), "%s", v);
                } else {
                    char dir[288];
                    snprintf(dir, sizeof(dir), "%s", path);
                    char *sl = strrchr(dir, '/');
                    if (sl) *sl = '\0'; else dir[0] = '\0';
                    snprintf(ut->cover, sizeof(ut->cover), "%s/%s", dir, v);
                }
            }
        } else {
            int done = 0;
            for (int i = 0; i < TH_N; i++)
                if (strcmp(k, TH_KEY[i]) == 0) {
                    Uint32 c;
                    if (th_parse_color(v, &c)) ut->c[i] = c;
                    done = 1;
                    break;
                }
            /* 卡片外观（cd_*）：整数，范围在 th_clamp_cd() 里统一钳制 */
            if (!done)
                for (int i = 0; i < CD_N; i++)
                    if (strcmp(k, CD_KEY[i]) == 0) {
                        char *end = NULL;
                        long n = strtol(v, &end, 10);
                        if (end != v) ut->cd[i] = (int)n;
                        break;
                    }
        }
    }
    fclose(fh);
    return 1;
}

static void th_scan_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && g_utheme_n < THEME_MAX) {
        const char *n = e->d_name;
        size_t L = strlen(n);
        if (L <= 6 || strcmp(n + L - 6, ".theme") != 0) continue;

        char stem[48];
        snprintf(stem, sizeof(stem), "%.*s", (int)(L - 6), n);

        /* 同名只认先扫到的：/roms（内存卡）先于 /opt；内置同名也跳过，
           免得菜单里出现两行一模一样的。 */
        int dup = 0;
        for (int i = 0; i < g_utheme_n; i++)
            if (strcmp(g_utheme[i].id, stem) == 0) { dup = 1; break; }
        for (int i = 0; !dup && i < THEME_BUILTIN_N; i++)
            if (strcmp(THEME_BUILTIN[i].id, stem) == 0) dup = 1;
        if (dup) continue;

        UserTheme *ut = &g_utheme[g_utheme_n];
        memset(ut, 0, sizeof(*ut));
        snprintf(ut->id,   sizeof(ut->id),   "%s", stem);
        snprintf(ut->name, sizeof(ut->name), "%s", stem);
        snprintf(ut->path, sizeof(ut->path), "%s/%s", dir, n);
        if (th_read_file(ut->path, ut)) {
            if (!ut->name[0]) snprintf(ut->name, sizeof(ut->name), "%s", ut->id);
            printf("[主题] 载入自定义主题 %s（%s）\n", ut->name, ut->path);
            g_utheme_n++;
        }
    }
    closedir(d);
}

/* 卡片字段统一钳到 CD_LIM 的范围里再落地 */
static void th_clamp_cd(int *cd)
{
    for (int i = 0; i < CD_N; i++) {
        if (cd[i] < CD_LIM[i][0]) cd[i] = CD_LIM[i][0];
        if (cd[i] > CD_LIM[i][1]) cd[i] = CD_LIM[i][1];
    }
}

static int th_find(const char *id, Uint32 *out, int *cd,
                   char *cover, size_t ccap)
{
    for (int i = 0; i < THEME_BUILTIN_N; i++)
        if (strcmp(id, THEME_BUILTIN[i].id) == 0) {
            memcpy(out, THEME_BUILTIN[i].c, sizeof(Uint32) * TH_N);
            memcpy(cd,  CD_DEF,             sizeof(int)    * CD_N);
            cover[0] = '\0';                       /* 内置主题没有配图 */
            return 1;
        }
    for (int i = 0; i < g_utheme_n; i++)
        if (strcmp(id, g_utheme[i].id) == 0) {
            memcpy(out, g_utheme[i].c,  sizeof(Uint32) * TH_N);
            memcpy(cd,  g_utheme[i].cd, sizeof(int)    * CD_N);
            snprintf(cover, ccap, "%s", g_utheme[i].cover);
            return 1;
        }
    return 0;
}

/* 刷进 g_th[] / g_cd[] / g_card_cover_dir。找不到就退内置第 0 套
   （绝不留下半套颜色、上一套主题的卡片尺寸、或者它的封面图目录）。 */
static void theme_apply(const char *id)
{
    Uint32 c[TH_N];
    int    cd[CD_N];
    char   cov[288] = "";
    if (!id || !*id || !th_find(id, c, cd, cov, sizeof(cov))) {
        memcpy(g_th, THEME_BUILTIN[0].c, sizeof(Uint32) * TH_N);
        memcpy(g_cd, CD_DEF,             sizeof(int)    * CD_N);
        g_card_cover_dir[0] = '\0';
        snprintf(g_theme_id, sizeof(g_theme_id), "%s", THEME_BUILTIN[0].id);
        return;
    }
    memcpy(g_th, c, sizeof(Uint32) * TH_N);
    th_clamp_cd(cd);
    memcpy(g_cd, cd, sizeof(int) * CD_N);
    snprintf(g_card_cover_dir, sizeof(g_card_cover_dir), "%s", cov);
    snprintf(g_theme_id, sizeof(g_theme_id), "%s", id);
}

/* 开机调一次。**必须早于 bake_all()** —— 颜色要在烘纹理之前定下来。 */
static void theme_load(void)
{
    g_utheme_n = 0;
    th_scan_dir(THEME_DIR_ROMS);
    th_scan_dir(THEME_DIR_OPT);

    char b[64] = "";
    if (conf_get_str(SCONF_PATH, "theme", b, sizeof(b)) && b[0])
        theme_apply(b);
    else
        theme_apply(THEME_BUILTIN[0].id);

    printf("[主题] 当前 %s（内置 %d 套 + 自定义 %d 套）\n",
           g_theme_id, THEME_BUILTIN_N, g_utheme_n);
    fflush(stdout);
}

/* 进主题菜单时摊成菜单行 */
static void theme_scan(void)
{
    int n = 0;
    for (int i = 0; i < THEME_BUILTIN_N && n < THEME_MAX; i++, n++) {
        SItem *it = &IT_THEME_DYN[n];
        memset(it, 0, sizeof(*it));
        it->name   = THEME_BUILTIN[i].name;
        it->type   = ST_BUTTON;
        it->action = SA_SET_THEME;
        it->arg    = THEME_BUILTIN[i].id;
        it->hint   = "内置主题";
    }
    for (int i = 0; i < g_utheme_n && n < THEME_MAX; i++, n++) {
        SItem *it = &IT_THEME_DYN[n];
        memset(it, 0, sizeof(*it));
        it->name   = g_utheme[i].name;
        it->type   = ST_BUTTON;
        it->action = SA_SET_THEME;
        it->arg    = g_utheme[i].id;    /* 行 → 主题 id 的桥 */
        it->hint   = g_utheme[i].path;
    }
    g_theme_n = n;
    MENUS[M_THEME].n = n;
}

/* 就地重启，回到设置页的指定菜单、当前行。
 *
 * 换主题、改字号这类"改了必须重烘纹理"的项都走这里 —— 颜色和字号都是
 * **烘进纹理**的，改完不重启就只是配置变了、屏幕上还是旧样子。
 * execv 自己最省事：不需要重建窗口/渲染器/字体，进程重启一遍全好，
 * 而且 argv 直接把 "--settings N --setrow R" 传回去，用户回来时还站在原地。
 *
 * ⚠ 绝不能改成"退出去让启动器重拉"：vgfront 退出会被 vagaretro-front.sh
 *   当成用户主动退出，走收尾逻辑把服务结束掉（见 do_action 里的注释）。 */
static void settings_restart(int menu)
{
    char exe[512];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) { set_flash("已保存，重启前端后生效"); return; }
    exe[n] = '\0';

    char mm[8], row[8];
    snprintf(mm,  sizeof(mm),  "%d", menu);
    snprintf(row, sizeof(row), "%d", g_setrow);
    /* argv[2]/argv[3] 是调试用的"跑几帧/第几帧截图"，重启必须给 0。
       末尾的 --splash 让新进程播 LANGOS 品牌画面顶着黑屏（见 boot_splash）：
       参数解析都是扫描式的，多一个无值开关放在最后不会影响 --settings/--setrow。 */
    char *av[] = { exe, (char *)"/opt/vagaretro/index.tsv", (char *)"0", (char *)"0",
                   (char *)"--settings", mm, (char *)"--setrow", row,
                   (char *)"--splash", NULL };

    /* execv 之前先在屏幕上留一帧品牌画面 —— 盖住"旧进程收尾 → 新进程
       KMS 重建"这段黑；新进程自己带 --splash，会接着把剩下的黑补上。 */
    printf("[重启] 带 --splash 重启前端…\n");
    fflush(stdout);
    boot_splash("正在重启前端…", 0);

    execv(exe, av);
    /* execv 成功不会回到这里。走到这一行说明重启没成 ——
       刚才那帧 splash 还糊在屏幕上，得把界面重画回来。 */
    mark_dirty();
    set_flash("已保存，请手动重启前端");
}

/* 用户按 A 选定一套：存盘 + 就地重启（回到主题菜单原行） */
static void theme_set(const char *id)
{
    if (!id || !*id) return;
    theme_apply(id);
    sconf_save();
    printf("[主题] 切到 %s，重启前端…\n", id);
    fflush(stdout);
    settings_restart(M_THEME);
}

/* ---- 基础工具 --------------------------------------------------------- */

/* 读文件第一行。sysfs / procfs 都这么读。
   注意结尾：governor 节点后面跟着换行，不裁掉的话 strcmp 永远不相等，
   表现是"设置页显示的值和实际值对不上"，很难查。 */
static int sread_line(const char *path, char *out, size_t cap)
{
    if (!cap) return 0;
    out[0] = '\0';
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    ssize_t n = read(fd, out, cap - 1);
    close(fd);
    if (n <= 0) { out[0] = '\0'; return 0; }
    out[n] = '\0';
    for (ssize_t i = 0; i < n; i++)
        if (out[i] == '\n' || out[i] == '\r' || out[i] == '\0') { out[i] = '\0'; break; }
    size_t l = strlen(out);
    while (l && (out[l - 1] == ' ' || out[l - 1] == '\t')) out[--l] = '\0';
    return 1;
}

/* 写 sysfs。
   ⚠ 不能用 fopen(path, "w") —— 它带 O_CREAT|O_TRUNC，而 sysfs 的属性节点
   对 O_TRUNC 的处理各家不一致（部分节点直接 EINVAL）。必须裸 open(O_WRONLY)。 */
static int swrite_line(const char *path, const char *s)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) return 0;
    size_t n = strlen(s);
    ssize_t w = write(fd, s, n);
    close(fd);
    return w == (ssize_t)n;
}

/* 跑命令。抓输出 / 不抓输出两版。命令串都是我们自己拼的常量+数字，
   没有外部输入，所以不过 shell 转义也没问题。 */
static int srun_capture(const char *cmd, char *out, size_t cap)
{
    if (cap) out[0] = '\0';
    FILE *p = popen(cmd, "r");
    if (!p) return -1;
    size_t used = 0;
    if (cap > 1) {
        size_t n;
        while (used + 1 < cap && (n = fread(out + used, 1, cap - 1 - used, p)) > 0)
            used += n;
        out[used] = '\0';
    }
    return pclose(p);
}

static int srun(const char *cmd)
{
    return system(cmd) == 0;
}

static int touch_file(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT, 0644);
    if (fd < 0) return 0;
    close(fd);
    return 1;
}

static int write_script(const char *path, const char *body)
{
    FILE *fh = fopen(path, "w");
    if (!fh) return 0;
    fputs(body, fh);
    fclose(fh);
    chmod(path, 0755);
    return 1;
}

/* ---- 音量 / 亮度 ------------------------------------------------------- */

/* 音量读一次之后就缓存。为什么必须缓存：sv_text() 在每次重画时都要取值，
   而 amixer 是 fork+exec（~10ms 起），每帧跑一次等于给界面塞了个卡顿源。
   写音量时顺手把缓存改掉，于是"调完立刻看到新值"也不需要重读。
   ★ 但这个缓存**没有外部失效路径** ⇒ 实体音量键（ogage → volume.sh → amixer）
     改了 mixer 之后前端看不见。所以状态栏那一路必须走 alsa_volume_refresh()。 */
static int g_vol_cache = -2;      /* -2 = 还没读过 */

/* 从 amixer sget 的输出里抠出百分比。
   输出形如：Front Left: 13 [ 5%] [-23.25dB] [on]
   ⚠ 百分号前面那个空格是 amixer 自己加的（个位数时），
   所以不能直接 strstr("[5%]")，得按 '[' 逐段试。
   抠不到返回 -1（== 这个控件不存在，或输出格式不认识）。 */
static int mix_pct(const char *buf)
{
    const char *p = buf;
    while ((p = strchr(p, '[')) != NULL) {
        int v = 0;
        if (sscanf(p, "[ %d%%]", &v) == 1 || sscanf(p, "[%d%%]", &v) == 1)
            return v;
        p++;
    }
    return -1;
}

/* 系统主音量的控件名。**不能写死。**
   MINILOONG 的 /home/ark/.asoundrc 把 softvol 的 control.name 设成了
   "Master"，根本没有 "Playback" 这个控件 —— 写死 "Playback" 时 amixer
   报 "Unable to find simple control" 并返回非 0，读回 -1，
   界面显示"不可用"，用户就以为音量坏了
   （2026-09-15 反馈："目前系统音量无法在设置里调节"）。

   顺序照官方 /usr/local/bin/current_volume 的判断来：Master 优先。
   首次探测命中哪个名字就**记住**它 —— 否则每帧都要 fork 一次 amixer
   （读值走的是 alsa_volume()，界面上音量那行每次都调）。
   NULL = 还没探测过。 */
static const char *g_volctl = NULL;

/* ★★ 真去 fork 一次 amixer 读音量。**别直接调它** —— sv_text() 每次重画都要取值，
   按帧 fork 会卡；常规路径走下面带缓存的 alsa_volume()。 */
static int alsa_volume_raw(void)
{
    char buf[1024];
    char cmd[128];

    /* ① 已经知道控件名 → 只问这一个 */
    if (g_volctl) {
        snprintf(cmd, sizeof(cmd), "amixer sget %s 2>/dev/null", g_volctl);
        srun_capture(cmd, buf, sizeof(buf));
        int v = mix_pct(buf);
        if (v >= 0) { g_vol_cache = v; return v; }
        g_volctl = NULL;            /* 控件没了（换了设备/改了 asoundrc）→ 重探 */
    }

    /* ② 逐个候选探测。顺序有讲究：Master 是本机实际生效的那个
          （.asoundrc 的 softvol 挂在 Master 上），放最前面省一次 fork。 */
    static const char *const CAND[] = { "Master", "Playback", "PCM", "DAC", "Speaker" };
    for (unsigned i = 0; i < sizeof(CAND) / sizeof(CAND[0]); i++) {
        snprintf(cmd, sizeof(cmd), "amixer sget %s 2>/dev/null", CAND[i]);
        srun_capture(cmd, buf, sizeof(buf));
        int v = mix_pct(buf);
        if (v >= 0) { g_volctl = CAND[i]; g_vol_cache = v; return v; }
    }

    g_vol_cache = -1;               /* 一个都没有 —— 界面显示"不可用"，不假装是 0 */
    return -1;
}

/* 设置页 / 每帧调用的缓存版：命中就直接返回，不 fork。 */
static int alsa_volume(void)
{
    if (g_vol_cache != -2) return g_vol_cache;
    return alsa_volume_raw();
}

/* ★★★ 状态栏专用：强制重读。
   为什么必须存在这一个：实体音量键走的是
       ogage → /usr/local/bin/volume.sh → `amixer sset Master`
   **完全不经过前端**；而 g_vol_cache 一旦读到值就永远返回它，没有任何外部失效路径
   ⇒ 按实体键调音量时，状态栏会一直停在开机那一次读到的数字
   （2026-09-18 浪总反馈："按实体按键调节音量，状态栏的音量不会动"）。
   status_poll() 本来就 1.5 秒跑一次，这里顺手把缓存打掉重读，成本可接受。 */
static int alsa_volume_refresh(void)
{
    g_vol_cache = -2;
    return alsa_volume();
}

static int bl_cur_pct(void)
{
    char cur[32], mx[32];
    if (!sread_line(BL_DIR "/brightness", cur, sizeof(cur))) return -1;
    if (!sread_line(BL_DIR "/max_brightness", mx, sizeof(mx))) return -1;
    int c = atoi(cur), m = atoi(mx);
    if (m <= 0) return -1;
    return (c * 100 + m / 2) / m;   /* 四舍五入，别让 99% 显示成 98% */
}

/* ---- 掌机状态：电量 / 存储 / 网络 --------------------------------------
 *  这三个是掌机上真正会去翻的东西，做法刻意都很"笨"（扫目录、statvfs、
 *  问内核要），不依赖任何用户态服务 —— 掌机上 NetworkManager 之类
 *  不一定在跑，但 /sys 和 /proc 永远在。
 */

/* batteryplus 算出来的电量（百分比，纯文本一个数）。
 *
 * ★ 这台设备上它是电量的**真源**：RetroArch 本体（/opt/retroarch/bin/retroarch
 *   二进制里就写着这个路径）、充电灯 miniloong-charge-led.sh、语音报电量
 *   speak_bat_life.sh 全都读它。前端以前只读内核 sysfs，于是出现
 *   "全能模拟器里 38%、前端 35%"这种两边对不上（2026-09-16 用户报的）。
 *
 *   它比内核讲究：读 voltage_now 查自己标定的曲线
 *   （/home/ark/.config/batteryplus/batteryplus-voltage.map），做平滑，
 *   而且充/放电用两套端点电压（V_FULL_CHG/V_EMPTY_CHG vs *_DIS）——
 *   充电时端电压被电流抬高，这一步修正正是内核 RK817 的 capacity 缺的。
 *   所以**以它为准**，读不到才回退 sysfs。 */
#define BATT_PCT_PATH "/tmp/battery.percent"

/* 电池电量（0..100），顺带把 status 抄进 st。找不到电池节点返回 -1。
   为什么要扫目录而不是写死路径：RK817 的电池节点名在不同内核/设备树上
   见过 rk817-battery / battery / rk817_battery 好几种，写死一个必踩。 */
static int battery_pct(char *st, size_t stcap)
{
    if (stcap) st[0] = '\0';
    DIR *d = opendir("/sys/class/power_supply");
    if (!d) return -1;

    struct dirent *e;
    int best = -1;
    char best_name[64] = "";
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char p[256], b[32];
        snprintf(p, sizeof(p), "/sys/class/power_supply/%s/capacity", e->d_name);
        if (!sread_line(p, b, sizeof(b))) continue;
        int v = atoi(b);
        if (v <= 0 || v > 100) continue;
        /* 一个设备上可能同时有多个 power_supply（电池 + 假电池 + USB），
           优先挑名字里带 battery / rk817 的那个主电池。 */
        int mine = (strstr(e->d_name, "battery") || strstr(e->d_name, "rk817"));
        int cur_best = (strstr(best_name, "battery") || strstr(best_name, "rk817"));
        if (best < 0 || (mine && !cur_best)) {
            best = v;
            snprintf(best_name, sizeof(best_name), "%s", e->d_name);
            snprintf(p, sizeof(p), "/sys/class/power_supply/%s/status", e->d_name);
            if (stcap) sread_line(p, st, stcap);
        }
    }
    closedir(d);

    /* 电池节点在，就看 batteryplus 有没有更准的值。0 是合法电量（不能像
       上面那样把 0 当"读失败"），所以判据是 0..100。 */
    if (best >= 0) {
        char pb[32];
        if (sread_line(BATT_PCT_PATH, pb, sizeof(pb))) {
            int v = atoi(pb);
            if (v >= 0 && v <= 100) best = v;
        }
    }
    return best;
}

/* TF1(/roms) / TF2(/roms2) 的剩余空间。没挂载的点直接说明"未挂载"，
   不显示 0 —— 掌机上"卡没插"和"卡满了"是两回事。 */
static void disk_line(char *out, size_t cap)
{
    if (!cap) return;
    out[0] = '\0';
    const char *mnts[] = { "/roms", "/roms2" };
    for (int i = 0; i < 2; i++) {
        char seg[128];
        struct statvfs v;
        if (statvfs(mnts[i], &v) != 0) {
            snprintf(seg, sizeof(seg), "%s 未挂载", mnts[i]);
        } else {
            double tot = (double)v.f_blocks  * (double)v.f_frsize / 1073741824.0;
            double fre = (double)v.f_bavail  * (double)v.f_frsize / 1073741824.0;
            snprintf(seg, sizeof(seg), "%s 剩 %.1f/%.1fG", mnts[i], fre, tot);
        }
        if (out[0]) strncat(out, " · ", cap - strlen(out) - 1);
        strncat(out, seg, cap - strlen(out) - 1);
    }
}

/* IPv4 地址。用 hostname -I（busybox 和 coreutils 都有），
   不走 ioctl/SIOCGIFADDR —— 那套要枚举接口 + 处理 ioctl 返回，代码多得多，
   而这里只是"给人看一眼好去 ssh"。

   ⚠ 必须**滤掉 IPv6**。hostname -I 会把全球单播 IPv6 一起吐出来，这台机器上
   就是 1 个 IPv4 + 3 个 2408:... 开头的长串。全排出来的话这行值有 200 多字符，
   右对齐后一路压到左边界，看着像一坨乱码（2026-09-14 截图核对时发现的）。
   接口上真要用 IPv6 也不该在设置页里抄 —— 抄不动，而且 ssh 用 IPv4 就够。
   地址多于 2 个也只显示前两个，后面加 "…"，避免撑爆行宽。 */
static void net_line(char *out, size_t cap)
{
    if (!cap) return;
    out[0] = '\0';
    char b[512];
    srun_capture("hostname -I 2>/dev/null", b, sizeof(b));

    int shown = 0, extra = 0;
    for (char *p = b; *p; ) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (!*p) break;
        char *e = p;
        while (*e && *e != ' ' && *e != '\t' && *e != '\n' && *e != '\r') e++;
        char save = *e;
        *e = '\0';
        int is_v4 = strchr(p, '.') && !strchr(p, ':');   /* 有'.'无':'才是 IPv4 */
        if (is_v4) {
            if (shown < 2) {
                if (shown) strncat(out, " · ", cap - strlen(out) - 1);
                strncat(out, p, cap - strlen(out) - 1);
                shown++;
            } else {
                extra++;
            }
        }
        *e = save;
        p = e;
    }
    if (extra) strncat(out, " …", cap - strlen(out) - 1);
    if (!out[0]) snprintf(out, cap, "没有 IPv4 地址（WiFi 断了吗？）");
}

/* ---- 取当前值 / 落盘 --------------------------------------------------- */

/* 当前时区名（IANA 形式，如 Asia/Shanghai）。
   权威来源是 /etc/localtime 这个软链 —— glibc 就看它，`date` 也看它。
   ⚠ **不要**读 /etc/timezone：这台机器上它是 "Etc/UTC"，而 /etc/localtime
   指向 America/New_York，`date` 显示的是 EDT —— 两个源打架，信 localtime。 */
static int tz_current(char *out, size_t cap)
{
    if (!cap) return 0;
    out[0] = '\0';
    char buf[256];
    ssize_t n = readlink("/etc/localtime", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        const char *p = strstr(buf, "/zoneinfo/");
        snprintf(out, cap, "%s", p ? p + 10 : buf);
        return 1;
    }
    const char *e = getenv("TZ");         /* 不是软链时退回环境变量 */
    if (e && *e) { snprintf(out, cap, "%s", e); return 1; }
    return 0;
}

static int gov_index(const char *const *ch, int nch, const char *cur)
{
    for (int i = 0; i < nch; i++)
        if (strcmp(ch[i], cur) == 0) return i;
    return 0;    /* 当前值不在表里（自定义值）→ 从表头开始，不乱跳 */
}

/* ---- 虚拟内存（swapfile）--------------------------------------------------
   用户 2026-09-14："设置里加上虚拟内存的选项 可以设置1024MB的虚拟内存"。
   这台机器只有 917MB 内存（free 显示 total 917），跑重一点的模拟器
   或者边玩游戏边后台传送文件时容易 OOM 把进程杀掉。挂个 swapfile
   能把"内存不足"的坑填上（代价是 SD 卡读写，慢但比被杀强）。

   放哪儿、放多大，都是按这台机器的实况定的：
   ⚠ 位置 = 根分区（/opt/vagaretro/swapfile）。df 显示 /roms 只剩 557MB、
     /roms2 只剩 1.3GB，只有根分区有 2.6GB 可用。
   ⚠ 上限 1024MB。再往上（比如 2048）会把根分区吃到只剩几百兆 ——
     系统盘写满比缺 swap 严重得多，所以选项到 1024 为止。

   持久化写 /etc/fstab（带 nofail：万一文件被删了，开机只是少一个 swap，
   不会把引导卡住）。 */
/* 当前 swap 大小对应的选项下标。读 /proc/swaps —— 它是内核给的实况，
   自己存一份"我设成了多少"很容易和实际对不上（别人手工 swapoff 过之类）。 */
static int swap_cur_index(void)
{
    FILE *fh = fopen("/proc/swaps", "r");
    if (!fh) return 0;
    char line[256];
    int mb = 0;
    if (fgets(line, sizeof(line), fh))            /* 第一行是表头 */
        if (fgets(line, sizeof(line), fh)) {      /* 第二行才是第一条 swap */
            char dev[128], type[32];
            long kb = 0;
            if (sscanf(line, "%127s %31s %ld", dev, type, &kb) == 3)
                mb = (int)(kb / 1024);
        }
    fclose(fh);

    /* 容差 32MB：内核报的可用大小比文件本身略小，别因为几 MB 就对不上选项 */
    for (int i = SWAP_N - 1; i > 0; i--)
        if (mb >= atoi(SWAP_MB[i]) - 32) return i;
    return 0;
}

/* 真正去建/换/关 swapfile。mb<=0 表示关闭。
   全程 sudo -n：mkswap/swapon/写 fstab 都要 root，而前端本来就是
   被 sudo 拉起来的（rsh 连上来的身份是 ark，sudo 组免密）。 */
static int swap_apply(int mb, char *fb, size_t cap)
{
    char cmd[768];

    if (mb <= 0) {
        snprintf(cmd, sizeof(cmd),
                 "sudo -n swapoff " SWAPFILE " 2>/dev/null; "
                 "sudo -n rm -f " SWAPFILE "; "
                 "sudo -n sed -i '\\|" SWAPFILE "|d' /etc/fstab");
        srun(cmd);
        if (swap_cur_index() != 0) {
            snprintf(fb, cap, "关闭失败（swapoff 没吃住？）");
            return 0;
        }
        return 1;
    }

    /* 空间检查：swapfile 之外还得给系统留 300MB，否则根分区写满更麻烦 */
    struct statvfs st;
    if (statvfs("/", &st) == 0) {
        unsigned long long avail = (unsigned long long)st.f_bavail
                                 * (unsigned long long)st.f_frsize / (1024ULL * 1024ULL);
        if (avail < (unsigned long long)mb + 300ULL) {
            snprintf(fb, cap, "根分区只剩 %llu MB，放不下 %d MB（要留 300MB 余量）",
                     avail, mb);
            return 0;
        }
    }

    /* fallocate 最快；有的文件系统不支持就退回 dd（慢但一定行）。
       先 swapoff + 删旧文件：旧 swap 还挂着时直接覆盖文件会留下脏映射。 */
    snprintf(cmd, sizeof(cmd),
             "sudo -n swapoff " SWAPFILE " 2>/dev/null; "
             "sudo -n rm -f " SWAPFILE "; "
             "( sudo -n fallocate -l %dM " SWAPFILE
             "  || sudo -n dd if=/dev/zero of=" SWAPFILE " bs=1M count=%d status=none ) && "
             "sudo -n chmod 600 " SWAPFILE " && "
             "sudo -n mkswap " SWAPFILE " >/dev/null 2>&1 && "
             "sudo -n swapon " SWAPFILE " && "
             "sudo -n sed -i '\\|" SWAPFILE "|d' /etc/fstab; "
             "echo '" SWAPFILE " none swap sw,nofail 0 0' | sudo -n tee -a /etc/fstab >/dev/null",
             mb, mb);

    if (!srun(cmd)) {
        snprintf(fb, cap, "建立交换文件失败（磁盘满 / 写不了 fstab？）");
        return 0;
    }
    if (swap_cur_index() == 0) {
        snprintf(fb, cap, "swapon 没生效（内核不支持？dmesg 里看看）");
        return 0;
    }
    return 1;
}

/* ---- 一键安装金手指 --------------------------------------------------------
   用户 2026-09-14："设置加个一键安装金手指功能 自动把金手指文件解压到
   RA 对应目录"。

   包从哪儿来（按顺序取第一个存在的）：
     ① /roms/vagaretro/cheats.zip  —— 内存卡上的，用户想换包就换这个
     ② /opt/vagaretro/cheats.zip   —— 随系统一起装的那份（出厂内置）
   解到哪儿去：
     · RA 格式（.cht）→ RetroArch 的 cheat 库目录，即 retroarch.cfg 里的
       cheat_database_path（这台机器是 ~/.config/retroarch/cheats），
       统一收在子目录 中文金手指/ 下 —— 卸载时一把删干净，
       也不跟 RA 自带的 76 个官方目录混在一起。
     · PSP 的 .ini     → PPSSPP 的金手指目录 ~/.config/ppsspp/PSP/Cheats/。
       包里 PSP 那 4400 多个是 **PPSSPP 格式**（按游戏 ID 命名，如 UCES00842.ini），
       RA 根本不读，丢进 cheats 只是白占 16MB。

   ⚠⚠ 两个坑，都是在这台机器上实测撞出来的，换包/换设备前务必先看：

   (1) **设备上的 p7zip 是 16.02，解不了 Rar5**。原包正是 Rar5，`7z x` 会逐条报
       "Unsupported Method"，但它**照样把目录和文件建出来** —— 看着解了 6572 个，
       其实全是 0 字节的空壳。不看日志根本发现不了（rc 也只是 2，不中断）。
       所以内置包在 PC 上转成 zip 再装过来，见 build/make-cheatpack.py。

   (2) **这台设备的 `unzip` 解不出中文名，别用它**（2026-09-15 全量实测，
       结论和上一版注释**正好相反** —— 上一版写的"必须带 -O UTF-8"是错的）：

       · `unzip -O UTF-8 …` → **rc=10，直接把 usage 打出来**。Debian 的
         UnZip 6.00 编译时压根没带 `-O`（`unzip -hh` 的选项表里既没有 -O
         也没有 -I），整条命令被当语法错误拒掉，一个文件都不解。而原命令后面
         还接着别的语句、又 `>/dev/null 2>&1`，rc 被彻底吞掉 —— 真机上只表现
         为"解压完但一个 .cht 都没有"，光看界面根本查不到原因。
         **这就是"一键安装金手指点了没反应"的真凶。**

       · `unzip -o -q …`（不带 -O）→ rc=0，2152 个 .cht 确实都解出来了，
         但**顶层目录名是坏字节**："中" 落盘成 `f5 a9 a1`（期望 `e4 b8 ad`）。
         Info-ZIP 把 zip 里的非 ASCII 名字按本机 OEM 代码页转了一道，
         `-UU`（忽略 Unicode 字段）也拦不住 —— 加了它照样是 f5 a9 a1。
         名字一坏，下面 `$SRC/*中文金手指` 就通配不上，等于白解。

       → 改用 **`7z x` 为主 + `python3 -m zipfile -e` 兜底**。两者的名字都取自
         zip 自己的 UTF-8 标志位（bit 11），落盘逐字节正确
         （`e4 b8 ad e6 96 87 …` = 中文金手指），实测各 2152 个 .cht、0 个空文件。
         python3 也不算新增依赖 —— 前端本来就走 python3 的 vagaretro-shell.py。 */

#define RA_CHEAT_DIR   "/home/ark/.config/retroarch/cheats"
#define PPSSPP_CHEAT   "/home/ark/.config/ppsspp/PSP/Cheats"
#define CHEAT_SUBDIR   "中文金手指"          /* RA cheats 下的中文包目录名 */
#define CHEAT_TMP      "/opt/vagaretro/_cheat_tmp"

static const char *const CHEAT_PACKS[] = {
    "/roms/vagaretro/cheats.zip",            /* 内存卡优先：用户自己换包走这儿 */
    "/opt/vagaretro/cheats.zip",             /* 内置兜底 */
    NULL
};

/* 找包。返回静态表里的路径（不用释放），size 回填字节数；没有就返回 NULL。
   为什么不一进来就报"未安装"：cheat_install() 先调它，找不到才给提示。 */
static const char *cheat_pack_find(long *size)
{
    if (size) *size = 0;
    for (int i = 0; CHEAT_PACKS[i]; i++) {
        struct stat st;
        if (stat(CHEAT_PACKS[i], &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
            if (size) *size = (long)st.st_size;
            return CHEAT_PACKS[i];
        }
    }
    return NULL;
}

/* RA cheats/中文金手指 下现在有多少个 .cht（整包是 2152 个）。
   直接问文件系统，不自己记"装过没" —— 用户手动删过也能一眼看出来。 */
static int cheat_count(void)
{
    char cmd[512], out[64];
    snprintf(cmd, sizeof(cmd),
             "find '" RA_CHEAT_DIR "/" CHEAT_SUBDIR "' -name '*.cht' 2>/dev/null | wc -l");
    if (srun_capture(cmd, out, sizeof(out)) != 0) return 0;
    return atoi(out);
}

/* 解包 + 分派。成功 1，失败 0 并把原因写进 fb。

   全程同步阻塞（六千多个文件，半分钟上下），调用方先顶一帧品牌画面。
   没做"后台 + 进度条"：拆成异步要再开一套 IPC，而这一步一台机器一辈子跑一次。 */
static int cheat_install(char *fb, size_t cap)
{
    long psz = 0;
    const char *pack = cheat_pack_find(&psz);
    if (!pack) {
        snprintf(fb, cap, "没找到金手指包（请放 /roms/vagaretro/cheats.zip）");
        return 0;
    }

    /* 空间：解压过程里临时目录 70MB 和目标 35MB 会同时在。
       根分区只剩 1.4G，正常够用，但用户往别处塞过东西就得先拦住。 */
    struct statvfs st;
    if (statvfs("/", &st) == 0) {
        unsigned long long avail = (unsigned long long)st.f_bavail
                                 * (unsigned long long)st.f_frsize / (1024ULL * 1024ULL);
        if (avail < 200ULL) {
            snprintf(fb, cap, "根分区只剩 %llu MB，装不下（要留 200MB）", avail);
            return 0;
        }
    }

    boot_splash("正在解压金手指包…", 700);

    /* 写成一条 sh 序列：每一步单独 fork 一次太慢，而且中间状态没法共享变量。
       ⚠ 不要加 `set -e`：下面 `[ -d "$d" ] && mv` 在条件为假时整条返回非 0，
       set -e 会把它当错误直接退出（经典坑，这里用 if 显式判断更稳）。 */
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
        "TMP=" CHEAT_TMP "; RA='" RA_CHEAT_DIR "'; PP='" PPSSPP_CHEAT "'; "
        "SUB='" CHEAT_SUBDIR "'; "
        "rm -rf \"$TMP\"; mkdir -p \"$TMP\"; "
        /* 解压：unzip 这条路在这台机器上是死的，见上面 (2)。
           7z 主、python3 兜底，两者都按 zip 的 UTF-8 标志位解名字。 */
        "if command -v 7z >/dev/null 2>&1; then "
        "  7z x -y -o\"$TMP\" '%s' >/dev/null 2>&1; "
        "fi; "
        "if [ ! -d \"$TMP/$SUB\" ]; then "
        "  python3 -m zipfile -e '%s' \"$TMP\" >/dev/null 2>&1; "
        "fi; "
        "SRC=\"$TMP/$SUB\"; "
        /* 顶层名理论上就是 $SUB；万一包结构变了也别整个失败 */
        "[ -d \"$SRC\" ] || SRC=$(find \"$TMP\" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | head -1); "
        /* RA 侧：把 10 个 "XX中文金手指" 平台目录搬进 cheats/中文金手指/。
           先整目录删掉重来 —— 重装/换包时才不会留上一版的残渣。 */
        "rm -rf \"$RA/$SUB\"; mkdir -p \"$RA/$SUB\"; "
        "for d in \"$SRC\"/*中文金手指; do "
        "  if [ -d \"$d\" ]; then mv \"$d\" \"$RA/$SUB/\"; fi; "
        "done; "
        /* PSP 侧：只在机器上确实装了 PPSSPP 时才投喂 */
        "if [ -d \"$PP\" ] && [ -d \"$SRC/PSP/金手指\" ]; then "
        "  cp -f \"$SRC/PSP/金手指/\"*.ini \"$PP/\" 2>/dev/null; "
        "  cp -f \"$SRC/PSP/金手指/cheat.db\" \"$PP/\" 2>/dev/null; "
        "fi; "
        "rm -rf \"$TMP\"; "
        /* RA 以 ark 身份跑，文件归 root 读是读得到，但存档/改名会别扭 */
        "chown -R ark:ark \"$RA/$SUB\" 2>/dev/null; "
        "true",
        pack, pack);

    if (system(cmd) != 0) {
        snprintf(fb, cap, "安装失败（包坏了 / 空间不够？）");
        return 0;
    }

    int n = cheat_count();
    if (n <= 0) {
        snprintf(fb, cap, "解压失败或包里没有 .cht（7z/python3 都解不出？）");
        return 0;
    }
    snprintf(fb, cap, "金手指已装好 %d 个（RA 菜单→金手指→加载金手指文件）", n);
    return 1;
}

/* 卸载：只删中文包，RA 自带的官方库（那 76 个目录 + cheats.zip）一点不碰。 */
static int cheat_remove(char *fb, size_t cap)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf '" RA_CHEAT_DIR "/" CHEAT_SUBDIR "'; true");
    int rc = system(cmd);
    (void)rc;                      /* 返回值不看：下面 cheat_count() 才是真判据 */
    if (cheat_count() != 0) {
        snprintf(fb, cap, "删除失败（目录只读？）");
        return 0;
    }
    snprintf(fb, cap, "已清除中文金手指（RA 自带金手指库没动）");
    return 1;
}

/* 选项型条目的当前下标 */
/* 开关型条目的当前状态：1=开 / 0=关 / -1=读不到。
   每次绘制都会问一次 —— nmcli / systemctl 各几十毫秒，设置页刷新率本来就低，
   换来的是"别人在别的终端改了开关，这里也立刻是对的"。权威来源只有一个。 */
static int sv_toggle_state(int src)
{
    char b[64];
    switch (src) {
    case SV_WIFI_RADIO:
        if (srun_capture("nmcli radio wifi 2>/dev/null", b, sizeof(b)) != 0) return -1;
        return strncmp(b, "enabled", 7) == 0;
    case SV_SSH:
        /* ★★ 2026-09-17 用户报「ssh 关闭后又打不开了」—— **根因就在这行**。
           `systemctl is-active` 用**退出码**表达状态（active=0 / inactive=3 /
           failed=3），而 srun_capture 返回的正是 pclose() 的 status。
           把它当"命令失败"的判据，服务一关就返回 -1 ⇒ 界面显示「不可用」、
           按 A 也切不动 ⇒ SSH 成了「**能关不能开**」的死锁；而 SSH 一关，
           远程通道也断了，玩家只能拆机插卡去捞。
           **判状态只看输出文本，退出码一律不管**（文本读不到才算失败：
           那会得到空串，strncmp 自然不为 0，照样判成"关"—— 安全的默认）。
           ⚠ 无线开关没这毛病：`nmcli radio wifi` 即使 disabled 也返回 0。 */
        srun_capture("systemctl is-active ssh 2>/dev/null", b, sizeof(b));
        return strncmp(b, "active", 6) == 0;
    default: return 0;
    }
}

/* 读 retroarch.cfg 的 user_language，换算成 RALANG_VAL 的下标。
   读不到、或者玩家设了表里没有的语言（日语/法语…），一律按 English 显示：
   前端只暴露两档，不替玩家猜他想看哪个。 */
static int ra_lang_read(void)
{
    FILE *fh = fopen(RA_CFG_MAIN, "r");
    if (!fh) return 0;
    char line[512];
    int val = 0;
    while (fgets(line, sizeof(line), fh)) {
        if (strncmp(line, "user_language", 13) != 0) continue;
        const char *q = strchr(line, '"');
        if (q) val = atoi(q + 1);
        break;
    }
    fclose(fh);
    for (int i = 0; i < RALANG_N; i++)
        if (RALANG_VAL[i] == val) return i;
    return 0;
}

/* 把 user_language 写进两份 cfg（64 位 RA + PS1 用的 retroarch32）。
   sed 就地替换 `^user_language`；万一那行被玩家删干净了就补一行 ——
   只 sed 不补的话，文件里没这行时命令会"成功但没改"，属于静默失败。
   两份都改是为了 PS1：它走 retroarch32，配置目录不一样。 */
static int ra_lang_write(int idx)
{
    if (idx < 0 || idx >= RALANG_N) return 0;
    char cmd[640];
    snprintf(cmd, sizeof(cmd),
             "for f in " RA_CFG_MAIN " " RA_CFG_32 "; do "
             "[ -f \"$f\" ] || continue; "
             "if grep -q '^user_language' \"$f\" 2>/dev/null; then "
             "sed -i 's/^user_language *=.*/user_language = \"%d\"/' \"$f\"; "
             "else printf 'user_language = \"%d\"\\n' >> \"$f\"; fi; "
             "done >/dev/null 2>&1",
             RALANG_VAL[idx], RALANG_VAL[idx]);
    int rc = system(cmd);
    printf("[RA语言] 写 user_language=%d rc=%d\n", RALANG_VAL[idx], rc);
    fflush(stdout);
    return rc == 0;
}

/* 读 retroarch.cfg 的 input_hold_fast_forward_btn。命中已知编号返回下标；
   没这一行 / 是 "nul" / 编号不认识（换过手柄）返回 -1。

   ⚠ 必须是 -1 而不是 0：以前这里读不到就 return 0，于是**界面上永远显示
   「L2」**——用户看到已经是 L2 就不去动它，那条绑定也就永远写不进盘，
   实际按 L2 一点反应都没有（2026-09-16 用户就这么报的）。 */
static int ra_fastfwd_read_raw(void)
{
    FILE *fh = fopen(RA_CFG_MAIN, "r");
    if (!fh) return -1;
    char line[512];
    char val[32] = "";
    /* ⚠ 键名长度必须用 sizeof-1 算，别手数。这里原来写的是 28，而这个键名
       只有 27 个字符 —— strncmp 会拿行里第 28 个字符（空格）去跟字面串结尾的
       '\0' 比，**永远不相等**，于是读回永远失败：每次前端启动都误判"出厂未绑"
       重写一遍，而且设置页永远显示 L2（用户改成 L3 界面也不会动）。
       2026-09-16 从设备日志里连续 6 组 "[RA加速键] 出厂未绑" 抓到的。 */
    static const char RFWD_KEY[] = "input_hold_fast_forward_btn";
    while (fgets(line, sizeof(line), fh)) {
        if (strncmp(line, RFWD_KEY, sizeof(RFWD_KEY) - 1) != 0) continue;
        const char *q = strchr(line, '"');
        if (q) {
            const char *e = strchr(q + 1, '"');
            size_t n = e ? (size_t)(e - q - 1) : 0;
            if (n >= sizeof(val)) n = sizeof(val) - 1;
            memcpy(val, q + 1, n);
            val[n] = '\0';
        }
        break;
    }
    fclose(fh);
    for (int i = 0; i < RAFWD_N; i++)
        if (strcmp(RAFWD_ID[i], val) == 0) return i;
    return -1;
}

/* 界面用：读不到就当第 0 档（L2）显示，别让设置页出现负数下标。 */
static int ra_fastfwd_read(void)
{
    int i = ra_fastfwd_read_raw();
    return (i < 0) ? 0 : i;
}

/* 亮度热键的善后。出厂是 brightness_up=L2(6) / brightness_down=R2(7)；
   快进键撞上谁，谁就得清成 nul（不清的话按住 guide+L2 会一边快进一边把
   亮度顶到最亮）。不撞号时**写回出厂值** —— 免得用户从 L2 换成 L3 之后，
   亮度键就永久消失了。out 是要拼进 shell 命令里的一段。 */
static void ra_bright_fix(int idx, char *out, size_t cap)
{
    const char *up   = (strcmp(RAFWD_ID[idx], "6") == 0) ? "nul" : "6";
    const char *down = (strcmp(RAFWD_ID[idx], "7") == 0) ? "nul" : "7";
    snprintf(out, cap,
             "sed -i 's|^input_brightness_up_btn *=.*|input_brightness_up_btn = \"%s\"|' \"$f\"; "
             "sed -i 's|^input_brightness_down_btn *=.*|input_brightness_down_btn = \"%s\"|' \"$f\"; ",
             up, down);
}

/* 把快进键写进两份 cfg（64 位 RA + PS1 用的 retroarch32）。
   ⚠ 两份的编号本机实测是**一样**的（L2=6 R2=7 L3=11 R3=12），所以共用
   RAFWD_ID 一张表；哪天某份不一样了，在这里分裂成两张即可。

   一共写四件事（每份 cfg 都做一遍）：
     ① input_hold_fast_forward_btn = <选的键>     按住 [门禁] + 它就快进
     ② input_toggle_fast_forward_btn = "nul"      清掉"按一下持续加速"
     ③ input_enable_hotkey_btn = "10"             ⚠ 门禁**恒为出厂 guide 键**，
                                                  玩家原组合一个不动（方案 B）
     ④ 亮度热键撞号善后（见 ra_bright_fix）
   其余热键（退出/菜单/存读档/重置/截图）**一律不动** —— 它们自动就是
   "按住 guide + 原按键"的组合，不需要改值。 */
static int ra_fastfwd_write(int idx)
{
    if (idx < 0 || idx >= RAFWD_N) return 0;
    const char *id = RAFWD_ID[idx];

    char bright[512];
    ra_bright_fix(idx, bright, sizeof(bright));

    char body[1400];
    snprintf(body, sizeof(body),
             "if grep -q '^input_hold_fast_forward_btn' \"$f\" 2>/dev/null; then "
               "sed -i 's|^input_hold_fast_forward_btn *=.*|input_hold_fast_forward_btn = \"%s\"|' \"$f\"; "
             "else printf 'input_hold_fast_forward_btn = \"%s\"\\n' >> \"$f\"; fi; "
             "sed -i 's|^input_toggle_fast_forward_btn *=.*|input_toggle_fast_forward_btn = \"nul\"|' \"$f\"; "
             /* 门禁键：**无条件写回出厂值**。以前这里写的是玩家选的快进键，
                结果把全机热键组合一起废掉了（2026-09-16 事故）。 */
             "if grep -q '^input_enable_hotkey_btn' \"$f\" 2>/dev/null; then "
               "sed -i 's|^input_enable_hotkey_btn *=.*|input_enable_hotkey_btn = \"" RAFWD_GATE "\"|' \"$f\"; "
             "else printf 'input_enable_hotkey_btn = \"" RAFWD_GATE "\"\\n' >> \"$f\"; fi; "
             "%s",
             id, id, bright);

    char cmd[3400];
    snprintf(cmd, sizeof(cmd),
             "{ f='" RA_CFG_MAIN "'; if [ -f \"$f\" ]; then %s fi; "
             "f='" RA_CFG_32 "'; if [ -f \"$f\" ]; then %s fi; } >/dev/null 2>&1",
             body, body);
    int rc = system(cmd);
    printf("[RA加速键] hold_btn=%s gate=%s rc=%d\n", id, RAFWD_GATE, rc);
    fflush(stdout);
    return rc == 0;
}

/* ==========================================================================
 *  摇杆灯（AW20036）读写
 * ==========================================================================*/

/* 去首尾空白 + 转小写。存档是给 shell 脚本也读的，容错一点。 */
static void led_trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n-1] == '\n' || s[n-1] == '\r' || s[n-1] == ' ' || s[n-1] == '\t'))
        s[--n] = '\0';
    size_t i = 0;
    while (s[i] == ' ' || s[i] == '\t') i++;
    if (i) memmove(s, s + i, strlen(s + i) + 1);
    for (char *p = s; *p; p++)
        if (*p >= 'A' && *p <= 'Z') *p += 32;
}

/* 读存档。文件不在（出厂状态）就按 charge-led.sh 的默认：电量 / 16 / 常亮。
   ⚠ 第 2 行存的是**驱动值 0..255**，不是百分比 —— 见 LED_DIR 那段注释。 */
static void led_conf_read(int *mode, int *bri, int *fx)
{
    *mode = 1;          /* battery */
    *bri  = 16;
    *fx   = 0;          /* solid */

    FILE *fh = fopen(LED_CONF, "r");
    if (!fh) return;
    char l[3][80] = { "", "", "" };
    for (int i = 0; i < 3 && fgets(l[i], sizeof(l[i]), fh); i++) { }
    fclose(fh);

    led_trim(l[0]); led_trim(l[1]); led_trim(l[2]);

    if (l[0][0])
        for (int i = 0; i < LED_MODE_N; i++)
            if (strcmp(LED_MODE_VAL[i], l[0]) == 0) { *mode = i; break; }
    if (l[1][0]) {
        int v = atoi(l[1]);
        if (v > 0) *bri = v > LED_MAXBRI ? LED_MAXBRI : v;
    }
    if (l[2][0])
        for (int i = 0; i < LED_FX_N; i++)
            if (strcmp(LED_FX_VAL[i], l[2]) == 0) { *fx = i; break; }
}

static int led_cur_pct(void)
{
    int m, b, f;
    led_conf_read(&m, &b, &f);
    return (b * 100 + LED_MAXBRI / 2) / LED_MAXBRI;
}

/* 写一个 led 属性。sysfs 有的节点要带换行的 echo 才认，所以直写失败
   再用 shell 兜一次 —— 跟调速器那几项同一套写法。
   ★ 2026-09-21：没有摇杆灯的机器直接短路返回 0，不去 write 一个不存在的
     路径（免得 srun 起一个必然失败的 shell，在 3326 上白耗一次 fork）。 */
static int led_write(const char *attr, const char *val)
{
    char path[128], cmd[192];
    if (!hw_has_led()) return 0;
    snprintf(path, sizeof(path), LED_DIR "/%s", attr);
    if (swrite_line(path, val)) return 1;
    snprintf(cmd, sizeof(cmd), "echo %s > %s 2>/dev/null", val, path);
    return srun(cmd);
}

/* 「追光」是软件效果：aw20036 原生的 miniloong_effect 只有
   solid/pulse/blink/breathe/rainbow/off 六种，没有跑马/流水这一档，
   所以另起一个小脚本（langos-led-chase.sh，跟前端一起装到设备上）
   沿色环快速步进颜色，做出来就是"光在一格一格往前追"。
   enable 是为了重启后还能自动恢复到这个效果。 */
/* ⚠⚠ systemctl enable/disable 必须带 --no-reload（2026-09-16 实测）。
   不带时它会顺带做一次 daemon-reload（重解析全部 unit），在这台 RK3566 上
   **单次 1.3~1.5 秒**；而 led_apply 一轮要跑 2~4 个这类调用 →
   界面直接冻住约 3 秒（用户报"设置摇杆灯非常卡"就是这个）。
   这里根本不需要 reload：enable/disable 只改"下次开机自启"的符号链接，
   当前跑不跑由后面的 start/stop 决定，两件事互不相干；
   systemd 下次真要 reload 时会自己从磁盘同步。
   带上之后单次 35~44 ms，**快 35 倍**。 */
static void led_chase_start(void)
{
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "[ -x " LED_CHASE_SH " ] && { "
             "systemctl enable --no-reload " LED_CHASE_SVC " >/dev/null 2>&1; "
             "systemctl restart " LED_CHASE_SVC " >/dev/null 2>&1; }");
    srun(cmd);
}

static void led_chase_stop(void)
{
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "systemctl disable --no-reload " LED_CHASE_SVC " >/dev/null 2>&1; "
             "systemctl stop " LED_CHASE_SVC " >/dev/null 2>&1");
    srun(cmd);
}

/* 把 (模式, 驱动亮度, 灯效) 落到硬件 + 存档。
   ⚠ 除"电量"以外的模式都要 disable 掉 miniloong_led.service：
   那个服务 Restart=always，只 stop 会在下次它按电池状态刷色时把玩家的
   固定色盖掉（表现为"设了颜色几秒后又变回电量色"）。 */
static int led_apply(int mode, int bri, int fx, int user_mode, char *fb, size_t cap)
{
    if (mode < 0 || mode >= LED_MODE_N) { snprintf(fb, cap, "选项越界"); return 0; }
    if (fx   < 0 || fx   >= LED_FX_N)   { snprintf(fb, cap, "选项越界"); return 0; }
    /* ★ 2026-09-21：本机没有 aw20036 摇杆灯（RK3326 全体）——
       设置页那个入口本来就被藏了，这里是第二道闸：万一有别的路径调进来，
       给一句人话而不是静默失败。 */
    if (!hw_has_led()) {
        snprintf(fb, cap, "本机没有摇杆灯硬件");
        return 0;
    }
    if (bri < 1) bri = 1;
    if (bri > LED_MAXBRI) bri = LED_MAXBRI;

    /* ---- 归一：把"在当前模式下必然不生效"的效果拉回「常亮」 ----------------
       驱动里颜色和效果是两个独立属性，但 effect=rainbow 会**忽略** color，
       而"追光"是我们自己的软件跑马（0.22 秒换一色），同样会把颜色盖掉。
       于是这几种组合下玩家看到的现象是"**颜色选了没用**"：

         · 「电量」：颜色由 miniloong_led.service 按电量刷，
           存档里留着"追光/炫彩"只会让设置页显示一个并没在跑的效果；
         · 「彩虹」：effect=rainbow 自己走色，颜色本来就无意义；
         · 固定颜色（红/黄/绿/蓝/青/品红/白）：追光脚本 0.22 秒后就把
           刚写的颜色换掉 —— 2026-09-15 用户实测反馈"摇杆灯不能自己调颜色"
           就是这个（当时存档是 mode=battery + fx=chase 的矛盾态）。

       solid / pulse / blink / breathe 四种**不动** —— 它们是"按颜色做变化"，
       和固定颜色是相容的（比如红色呼吸）。 */
    int fx_fixed = 0;
    /* ⓪ 关闭：灯都关了，存档里再留着"追光/炫彩"毫无意义 —— 而且它会一直
       赖着不走：下次只调亮度时 f 是从存档读出来的，于是 mode=0 + fx=chase
       会让设置页显示"关闭"却挂着一个追光效果（2026-09-16 实测存档就是
       off|36|chase 这个矛盾态）。这里不给玩家提示 —— 关灯不需要解释。 */
    if (mode == 0 && (fx == LED_FX_CHASE || fx == LED_FX_RAINBOW)) {
        fx = 0;
    }
    /* ① 电量 / 彩虹 模式：效果"追光/炫彩"在这里必然不生效（颜色分别由
       miniloong_led.service 和驱动的 rainbow 自己走），存档里留着它只会让
       设置页显示一个并没在跑的效果 → 一律归到「常亮」。 */
    else if ((mode == 1 || mode == 9) && (fx == LED_FX_CHASE || fx == LED_FX_RAINBOW)) {
        fx = 0;
        fx_fixed = 1;
    }
    /* ② 固定颜色 + 玩家这次动的就是「灯光模式」：追光脚本 0.22 秒后就把刚写的
       颜色换掉、原生 rainbow 更是直接无视 color，"追光/炫彩"留着 =
       颜色选了没用（2026-09-15 用户实测反馈的原话）。归到「常亮」。
       ⚠ 只在 user_mode（玩家主动改的是模式那一项）时做 —— 否则"只调一下亮度"
       就把玩家设好的追光关掉了。 */
    else if (user_mode && mode >= 2 && mode <= 8 &&
             (fx == LED_FX_CHASE || fx == LED_FX_RAINBOW)) {
        fx = 0;
        fx_fixed = 1;
    }

    /* ① 存档。先写临时文件再 mv，免得写到一半被打断留下半份配置。 */
    char tmp[256], cmd[384];
    snprintf(tmp, sizeof(tmp), "%s.new", LED_CONF);
    FILE *fh = fopen(tmp, "w");
    if (fh) {
        fprintf(fh, "%s\n%d\n%s\n", LED_MODE_VAL[mode], bri, LED_FX_VAL[fx]);
        fclose(fh);
        snprintf(cmd, sizeof(cmd),
                 "mkdir -p /home/ark/.config && mv %s %s && "
                 "chown ark:ark %s 2>/dev/null", tmp, LED_CONF, LED_CONF);
        srun(cmd);
    }
    /* 写不动也不拦着 —— 硬件照设，只是重启后回到上次存的值。 */

    char b[16];
    snprintf(b, sizeof(b), "%d", bri);
    int pct = (bri * 100 + LED_MAXBRI / 2) / LED_MAXBRI;
    if (pct < 1) pct = 1;

    /* ② 电量模式：交回官方服务管（追光脚本也要停 —— 电量色是这个服务按
       电池状态刷的，软件跑马跟它抢同一个属性，会闪成一团）。 */
    if (mode == 1) {
        led_chase_stop();
        snprintf(cmd, sizeof(cmd),
                 "systemctl enable --no-reload " LED_SERVICE " >/dev/null 2>&1; "
                 "systemctl restart " LED_SERVICE " >/dev/null 2>&1");
        srun(cmd);
        led_write("miniloong_brightness", b);
        if (fx_fixed)
            snprintf(fb, cap, "摇杆灯：随电量变色（亮度 %d%%）；灯效已改为常亮 —— "
                     "电量模式的颜色由电池状态决定，呼吸/追光在这里不生效", pct);
        else
            snprintf(fb, cap, "摇杆灯：随电量变色（亮度 %d%%）", pct);
        return 1;
    }

    /* ③ 其余模式：先把官方服务摘干净，否则它会把颜色刷回去。 */
    snprintf(cmd, sizeof(cmd),
             "systemctl disable --no-reload " LED_SERVICE " >/dev/null 2>&1; "
             "systemctl stop " LED_SERVICE " >/dev/null 2>&1");
    srun(cmd);

    led_write("miniloong_brightness", b);

    if (mode == 0) {                    /* 关闭 */
        led_chase_stop();
        led_write("miniloong_effect", "solid");
        led_write("miniloong_color",  "off");
        snprintf(fb, cap, "摇杆灯已关闭");
        return 1;
    }

    if (mode == 9) {                    /* 彩虹（颜色模式）：颜色由效果自己走 */
        led_chase_stop();
        led_write("miniloong_rainbow_rate", "120");
        led_write("miniloong_effect", "rainbow");
        snprintf(fb, cap, "摇杆灯：彩虹流动（亮度 %d%%）", pct);
        return 1;
    }

    /* 追光：变色交给后台脚本跑。硬件这边先铺一个常亮底色，
       免得脚本起来那几十毫秒是黑的（看起来像"选完灯灭了"）。 */
    if (fx == LED_FX_CHASE) {
        led_write("miniloong_effect", "solid");
        led_write("miniloong_color",  LED_MODE_VAL[mode]);
        led_chase_start();
        snprintf(fb, cap, "摇杆灯：追光（亮度 %d%%）", pct);
        return 1;
    }

    /* 其余档（含"炫彩"= 原生 rainbow）都是直接写驱动属性。 */
    led_chase_stop();
    /* 炫彩用驱动的 rainbow：顺手给它一个舒服的流动速度，
       驱动默认的 120 已经不错，这里显式写一次免得被别的入口改乱。 */
    if (fx == LED_FX_RAINBOW) led_write("miniloong_rainbow_rate", "120");
    led_write("miniloong_effect", LED_FX_VAL[fx]);
    led_write("miniloong_color",  LED_MODE_VAL[mode]);
    if (fx_fixed)
        snprintf(fb, cap, "摇杆灯：%s 常亮（亮度 %d%%）—— 原来的「追光/炫彩」会自己换色，"
                 "把刚选的%s盖掉，已改成常亮", LED_MODE_LABEL[mode], pct, LED_MODE_LABEL[mode]);
    else
        snprintf(fb, cap, "摇杆灯：%s %s（亮度 %d%%）",
                 LED_MODE_LABEL[mode], LED_FX_LABEL[fx], pct);
    return 1;
}

/* ---- RetroArch 主题（menu_driver / xmb_theme）-------------------------
   跟 RA 语言、加速键同一套路：只写盘，不重启前端。 */

/* 从一份 cfg 里取 `key = "value"` 的 value。找不到返回 0。
   ⚠ 比 ra_lang_read 那种 strncmp 严一点：键名后面必须跟 = 或空白，
   免得 "menu_driver" 把 "menu_driver_xxx" 也匹配上。 */
static int ra_cfg_get(const char *path, const char *key, char *out, size_t cap)
{
    if (cap) out[0] = '\0';
    FILE *fh = fopen(path, "r");
    if (!fh) return 0;
    size_t kl = strlen(key);
    char line[512];
    int found = 0;
    while (fgets(line, sizeof(line), fh)) {
        if (strncmp(line, key, kl) != 0) continue;
        char c = line[kl];
        if (c != ' ' && c != '\t' && c != '=') continue;
        const char *q = strchr(line, '"');
        if (!q) break;
        const char *e = strchr(q + 1, '"');
        size_t n = e ? (size_t)(e - q - 1) : 0;
        if (n >= cap) n = cap - 1;
        memcpy(out, q + 1, n);
        out[n] = '\0';
        found = 1;
        break;
    }
    fclose(fh);
    return found;
}

/* 往两份 cfg（64 位 RA + PS1 的 retroarch32）写一个字符串键。
   缺行就补一行 —— 只 sed 不补的话，文件里没这个键时命令"成功但没改"，
   属于静默失败（ra_lang_write 那段踩过）。 */
static int ra_cfg_set(const char *key, const char *val)
{
    char cmd[768];
    snprintf(cmd, sizeof(cmd),
             "for f in " RA_CFG_MAIN " " RA_CFG_32 "; do "
             "[ -f \"$f\" ] || continue; "
             "if grep -q '^%s' \"$f\" 2>/dev/null; then "
             "sed -i 's|^%s *=.*|%s = \"%s\"|' \"$f\"; "
             "else printf '%s = \"%s\"\\n' >> \"$f\"; fi; "
             "done >/dev/null 2>&1",
             key, key, key, val, key, val);
    return system(cmd) == 0;
}

static int ra_menu_read(void)
{
    char v[32];
    /* 读不到就报 OZONE（用户 2026-09-16："ra 默认主题改成 ozone"）。
       这也是 RA 自己现在的出厂值 —— 老版 RA 是 rgui，所以这一行以前写的是 2。 */
    if (!ra_cfg_get(RA_CFG_MAIN, "menu_driver", v, sizeof(v))) return 1;
    for (int i = 0; i < RAMENU_N; i++)
        if (strcmp(RAMENU_VAL[i], v) == 0) return i;
    /* 盘上是没见过的新值（RA 以后加了别的 driver）：同样按出厂 OZONE 报，
       总好过显示成"RGUI"而实际不是。 */
    return 1;
}

static int ra_theme_read(void)
{
    char v[48];
    /* 没写过这一项时 RA 用 assets/xmb/retrosystem（RA 自己的默认），
       所以读不到就报第 7 档，别撒谎说成第 0 档。 */
    if (!ra_cfg_get(RA_CFG_MAIN, "xmb_theme", v, sizeof(v))) return 7;
    for (int i = 0; i < RAXMB_N; i++)
        if (strcmp(RAXMB_VAL[i], v) == 0) return i;
    return 7;
}

static int ra_menu_write(int idx)
{
    if (idx < 0 || idx >= RAMENU_N) return 0;
    int ok = ra_cfg_set("menu_driver", RAMENU_VAL[idx]);
    printf("[RA主题] menu_driver=%s rc=%d\n", RAMENU_VAL[idx], ok);
    fflush(stdout);
    return ok;
}

static int ra_theme_write(int idx)
{
    if (idx < 0 || idx >= RAXMB_N) return 0;
    int ok = ra_cfg_set("xmb_theme", RAXMB_VAL[idx]);
    printf("[RA主题] xmb_theme=%s rc=%d\n", RAXMB_VAL[idx], ok);
    fflush(stdout);
    return ok;
}

/* RA 帧数显示（fps_show）。跟主题两项同一套路：实读 cfg、写两份、不重启前端。
   ⚠ 读的是**主 cfg**：两份虽然分开写，但前端只认 64 位那份当"当前值"，
     跟 ra_menu_read / ra_theme_read 保持一致（retroarch32 那份只是捎带写）。 */
static int ra_fps_read(void)
{
    char v[16];
    /* 读不到就报"关" —— RA 自己的出厂值就是 false，别撒谎说成开着。 */
    if (!ra_cfg_get(RA_CFG_MAIN, "fps_show", v, sizeof(v))) return 0;
    return (strcasecmp(v, "true") == 0) ? 1 : 0;
}

static int ra_fps_write(int idx)
{
    if (idx < 0 || idx >= RAFPS_N) return 0;
    int ok = ra_cfg_set("fps_show", RAFPS_VAL[idx]);
    printf("[RA帧数] fps_show=%s rc=%d\n", RAFPS_VAL[idx], ok);
    fflush(stdout);
    return ok;
}

/* .TIMEOUT 的当前值 → SLEEP_LABEL 的下标。
   ⚠ 文件不存在时返回「30 分钟」那一档 —— auto_suspend.py 读不到就用它的
     DEFAULT_TIMEOUT_MINUTES=30，界面必须跟它说同一件事，不能显示 10 糊弄人。
   ⚠ 认不出的值（玩家自己写了 45 这种）取"不超过它的最大档"，比落到「关闭」
     更接近实况；真值是 0 或者负数才退到默认档。 */
static int sleep_cur_index(void)
{
    char b[32];
    if (!sread_line(SLEEP_TIMEOUT_PATH, b, sizeof(b))) {
        for (int i = 0; i < SLEEP_N; i++)
            if (strcmp(SLEEP_VAL[i], "30") == 0) return i;
        return 0;
    }
    for (int i = 0; i < SLEEP_N; i++)
        if (strcasecmp(b, SLEEP_VAL[i]) == 0) return i;
    int n = atoi(b);
    if (n > 0) {
        int best = 1;                        /* 从"5 分钟"起步，别落到「关闭」 */
        for (int i = 1; i < SLEEP_N; i++)
            if (atoi(SLEEP_VAL[i]) <= n) best = i;
        return best;
    }
    for (int i = 0; i < SLEEP_N; i++)
        if (strcmp(SLEEP_VAL[i], "30") == 0) return i;
    return 0;
}

static int sv_cur_index(const SItem *it)
{
    char b[64];
    switch (it->src) {
    case SV_CPUGOV: return sread_line(CPU_GOV_PATH, b, sizeof(b)) ? gov_index(it->choices, it->nch, b) : 0;
    case SV_GPUGOV: return sread_line(GPU_GOV_PATH, b, sizeof(b)) ? gov_index(it->choices, it->nch, b) : 0;
    case SV_DMCGOV: return sread_line(DMC_GOV_PATH, b, sizeof(b)) ? gov_index(it->choices, it->nch, b) : 0;
    case SV_HCOLS:  return g_pg_cols   - 3;     /* COLS_HOME 从 "3" 开始 */
    case SV_GCOLS:  return g_game_cols - 3;     /* COLS_GAME 从 "3" 开始 */
    case SV_COVERSTY: return g_cover_sty;       /* COVER_STY 的下标就是它 */
    case SV_BEZEL:    return g_bezel;           /* BEZEL_LABEL 的下标就是它 */
    case SV_ROMSRC:
        return (g_romsrc >= 0 && g_romsrc < ROMSRC_N) ? g_romsrc : 0;
    case SV_RALANG:
        return (g_ralang >= 0 && g_ralang < RALANG_N) ? g_ralang : 0;
    /* ⚠ SV_RAFWD 已在 2026-09-16 停用（见 ra_fastfwd_* 的墓碑注释），
       这里不再返回任何值。函数保留着，随时能接回来。 */
    /* 摇杆灯三项都**实读存档**（灯可能被别的脚本改过，比如官方 Tools 里的
       那个入口），不缓存在内存里。 */
    case SV_LED_MODE: { int m, b, f; led_conf_read(&m, &b, &f); return m; }
    case SV_LED_FX:   { int m, b, f; led_conf_read(&m, &b, &f); return f; }
    case SV_LED_BRIGHT: return led_cur_pct();
    /* RA 主题两项也是实读 cfg —— 玩家可能自己进 RA 的界面里改过。 */
    case SV_RAMENU:  return ra_menu_read();
    case SV_RATHEME: return ra_theme_read();
    case SV_RAFPS:   return ra_fps_read();
    case SV_UILANG:
        return (g_uilang >= 0 && g_uilang < UILANG_N) ? g_uilang : UI_ZH;
    case SV_SWAP:     return swap_cur_index();  /* 直接问 /proc/swaps 的实况 */
    /* 字号是数值型（没 choices），setting_step 走的是自己那段读值逻辑；
       这里返回"值"本身只是为了让 sf_text/调试看在同一个地方能查到。 */
    case SV_FSUI:   return g_fs_ui;
    case SV_FSGAME: return g_fs_games;

    case SV_AUTOSLOT: return g_autoslot;        /* 数值型，值就在内存里 */
    case SV_SLEEP:  return sleep_cur_index();   /* 实读 /home/ark/.config/.TIMEOUT */
    case SV_TZ: {
        /* 当前时区不在表里（出厂 Asia/Shanghai 在表内；用户手工设过别的
           就会落空）→ 返回 -1，左右翻时从表头接上。 */
        char z[96];
        if (!tz_current(z, sizeof(z))) return 0;
        for (int i = 0; i < it->nch; i++)
            if (strcmp(it->choices[i], z) == 0) return i;
        return -1;
    }
    default:        return 0;
    }
}

/* 从 /proc/cpuinfo 里抠一个键的值 */
static int cpuinfo_get(const char *key, char *out, size_t cap)
{
    FILE *fh = fopen("/proc/cpuinfo", "r");
    if (!fh) return 0;
    char line[256];
    size_t kl = strlen(key);
    int found = 0;
    while (fgets(line, sizeof(line), fh)) {
        if (strncmp(line, key, kl) != 0) continue;
        char *c = strchr(line, ':');
        if (!c) continue;
        c++;
        while (*c == ' ' || *c == '\t') c++;
        size_t n = strlen(c);
        while (n && (c[n - 1] == '\n' || c[n - 1] == '\r')) c[--n] = '\0';
        snprintf(out, cap, "%s", c);
        found = 1;
        break;
    }
    fclose(fh);
    return found;
}

/* ---- WiFi（走 NetworkManager）--------------------------------------------
   这台机器上是 NetworkManager + wpa_supplicant（`nmcli` 在 /usr/bin）。
   不去手写 wpa_supplicant.conf —— 那样既要自己算 psk 又要 reconfigure，
   nmcli 一句话就完了，而且改完立刻生效。

   扫描解析故意用 `-m multiline` 而不是 `-t`：`-t` 是 ':' 分隔字段，
   而 SSID 里本来就可以有 ':'（虽然它会转义成 '\:'，那我这边还得写反转义）。
   multiline 是每条记录若干行 `键:值`、记录之间空行，解析没有歧义。 */

static char g_wifi_name[WIFI_MAX][64];
static char g_wifi_vals[WIFI_MAX][48];
static char g_wifi_hint[WIFI_MAX][112];
static int  g_wifi_sig[WIFI_MAX];
static int  g_wifi_saved[WIFI_MAX];

/* 去掉尾部换行/空白。srun_capture() 是不加处理的 fread，尾巴上大概率挂 \n。 */
static void chomp(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
                 s[n - 1] == ' '  || s[n - 1] == '\t'))
        s[--n] = '\0';
}

/* 把 s 转成可以安全塞进 shell 命令的单引号形式（内部的 ' 变 '\''）。
   命令串平时都是常量拼数字，不需要转义；但 SSID 和密码是用户给的，
   里面完全可能有空格、$、引号 —— 不转义就是命令注入。 */
static void shell_sq(char *out, size_t cap, const char *s)
{
    size_t o = 0;
    if (o + 1 < cap) out[o++] = '\'';
    for (const char *p = s; *p && o + 5 < cap; p++) {
        if (*p == '\'') { memcpy(out + o, "'\\''", 4); o += 4; }
        else            out[o++] = *p;
    }
    if (o + 1 < cap) out[o++] = '\'';
    out[o] = '\0';
}

/* 当前连着的无线名；没连就是空串。只读 wlan0 —— 掌机只有一个无线网卡。 */
static void wifi_current(char *out, size_t cap)
{
    if (!cap) return;
    out[0] = '\0';
    char b[256];
    if (srun_capture("nmcli -t -f GENERAL.CONNECTION device show wlan0 2>/dev/null",
                     b, sizeof(b)) != 0) return;
    char *c = strchr(b, ':');
    if (!c) return;
    c++;
    chomp(c);
    if (!*c || strcmp(c, "--") == 0) { out[0] = '\0'; return; }
    snprintf(out, cap, "%s", c);
}

/* 这个 SSID 是不是已经存过密码（存过就能直接 up，不用再问一遍） */
static int wifi_saved(const char *ssid)
{
    char b[4096], line[96];
    if (srun_capture("nmcli -t -f NAME connection show 2>/dev/null",
                     b, sizeof(b)) != 0) return 0;
    for (char *p = b; *p; ) {
        char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = '\0';
        chomp(line);
        /* ⚠ `-t` 下**单字段是不带分隔符的**（只有多字段才拿 ':' 拼起来），
           所以这里不能像之前那样找 "NAME:" 前缀 —— 会一条都匹配不上，
           屏幕上表现成"明明连着却写着新网络"。整行比，顺手兼容多字段写法。 */
        char *c = strchr(line, ':');
        if (c) *c = '\0';
        if (strcmp(line, ssid) == 0) return 1;
        if (!e) break;
        p = e + 1;
    }
    return 0;
}

/* 是不是一段完整的 UTF-8。扫描结果是从一个定长缓冲里切出来的，
   一旦被截断就会留下半个汉字 —— 屏幕上显示成一个乱码字（第一次
   核对时列表最后那条就是这个），所以收条目前先验一遍。 */
static int utf8_valid(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        int n;
        if      (*p < 0x80)            n = 0;
        else if ((*p & 0xE0) == 0xC0)  n = 1;
        else if ((*p & 0xF0) == 0xE0)  n = 2;
        else if ((*p & 0xF8) == 0xF0)  n = 3;
        else return 0;
        for (int i = 1; i <= n; i++)
            if ((p[i] & 0xC0) != 0x80) return 0;
        p += n + 1;
    }
    return 1;
}

/* 收一条扫描结果：去重、填 IT_WIFI_DYN。
   nmcli 已按信号降序排，所以同名 AP 里第一条就是最强的，后面的直接丢。 */
static void wifi_push(const char *ssid, int sig)
{
    if (!ssid || !ssid[0] || g_wifi_n >= WIFI_MAX) return;
    if (strcmp(ssid, "--") == 0) return;              /* 隐藏 SSID，nmcli 用 -- 占位 */
    if (!utf8_valid(ssid)) return;                    /* 被截断的半个字，丢掉 */
    for (int i = 0; i < g_wifi_n; i++)
        if (strcmp(g_wifi_name[i], ssid) == 0) return;

    int k = g_wifi_n;
    snprintf(g_wifi_name[k], sizeof(g_wifi_name[0]), "%s", ssid);
    g_wifi_sig[k]   = sig;
    g_wifi_saved[k] = wifi_saved(ssid);

    /* ⚠ 信号 0 是 nmcli 的"这一次没测到"，不等于"强度为零"。
       写成 0% 看着像坏了，用 — 更诚实（实测 rescan 之后只有当前连接的
       那个 AP 有真实数值，其余多数是 0）。 */
    char sb[16];
    if (sig > 0) snprintf(sb, sizeof(sb), "%d%%", sig);
    else         snprintf(sb, sizeof(sb), "—");

    snprintf(g_wifi_vals[k], sizeof(g_wifi_vals[0]), "%s · %s",
             sb, g_wifi_saved[k] ? "已保存" : "新网络");
    snprintf(g_wifi_hint[k], sizeof(g_wifi_hint[k]), "%s",
             g_wifi_saved[k] ? "按 A 直接连（密码已经存过）"
                             : "按 A 连接，会让你输一次密码");
    IT_WIFI_DYN[k] = (SItem){ .cn = g_wifi_name[k], .type = ST_BUTTON,
                              .val = g_wifi_vals[k], .hint = g_wifi_hint[k] };
    g_wifi_n++;
}

/* 扫描并重建 IT_WIFI_DYN + MENUS[M_WIFI].n。返回列出来的条数。
   ⚠ 阻塞 1~3 秒（nmcli 要等一轮全信道扫描），所以只在**进这一页**时调，
   不要放进每帧或每次按键的路径里。 */
static int wifi_scan(void)
{
    char b[16384];
    g_wifi_n = 0;
    MENUS[M_WIFI].n = 0;

    /* 无线关着的时候扫也白扫（nmcli 直接报错），给一行说明而不是空菜单 */
    char rb[32] = "";
    srun_capture("nmcli radio wifi 2>/dev/null", rb, sizeof(rb));
    if (strncmp(rb, "enabled", 7) != 0) {
        snprintf(g_wifi_name[0], sizeof(g_wifi_name[0]), "无线已关闭");
        snprintf(g_wifi_vals[0], sizeof(g_wifi_vals[0]), "先打开开关");
        snprintf(g_wifi_hint[0], sizeof(g_wifi_hint[0]),
                 "回上一级把「无线开关」打开，再进来就会扫描");
        IT_WIFI_DYN[0] = (SItem){ .cn = g_wifi_name[0], .type = ST_RO,
                                  .val = g_wifi_vals[0], .hint = g_wifi_hint[0] };
        g_wifi_n = 1;
        MENUS[M_WIFI].n = 1;
        return 1;
    }

    if (srun_capture("nmcli -m multiline -f SSID,SIGNAL,SECURITY "
                     "device wifi list --rescan yes 2>/dev/null",
                     b, sizeof(b)) != 0) {
        snprintf(g_wifi_name[0], sizeof(g_wifi_name[0]), "扫描失败");
        snprintf(g_wifi_vals[0], sizeof(g_wifi_vals[0]), "nmcli 没响应");
        snprintf(g_wifi_hint[0], sizeof(g_wifi_hint[0]),
                 "NetworkManager 可能没在跑：systemctl status NetworkManager");
        IT_WIFI_DYN[0] = (SItem){ .cn = g_wifi_name[0], .type = ST_RO,
                                  .val = g_wifi_vals[0], .hint = g_wifi_hint[0] };
        g_wifi_n = 1;
        MENUS[M_WIFI].n = 1;
        return 1;
    }

    /* ⚠ multiline 的真实格式是 `键:` + **空格补齐到固定宽度** + 值，
       而且记录之间**没有空行**（跟文档给人的印象不一样，实测 nmcli 1.52
       就是连续输出）。所以判"新记录"只能靠又出现一行 `SSID:`，
       取值也得先跳过那一串对齐空格 —— 直接 line+5 会连空格一起吃进来，
       屏幕上显示成一坨右对齐的乱码（第一次就是这么错的）。 */
    char ssid[64] = "", sig[16] = "0";
    int have = 0;
    char *p = b;
    while (*p) {
        char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        char line[256];
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = '\0';
        p = e ? e + 1 : p + len;
        if (!line[0]) continue;

        char *c = strchr(line, ':');
        if (!c) continue;
        *c = '\0';
        char *v = c + 1;
        while (*v == ' ' || *v == '\t') v++;
        chomp(v);

        if (strcmp(line, "SSID") == 0) {
            if (have) wifi_push(ssid, atoi(sig));      /* 上一条收尾 */
            snprintf(ssid, sizeof(ssid), "%s", v);
            snprintf(sig, sizeof(sig), "0");
            have = 1;
        } else if (strcmp(line, "SIGNAL") == 0) {
            snprintf(sig, sizeof(sig), "%s", v);
        }
    }
    if (have) wifi_push(ssid, atoi(sig));

    if (g_wifi_n == 0) {
        snprintf(g_wifi_name[0], sizeof(g_wifi_name[0]), "没扫到网络");
        snprintf(g_wifi_vals[0], sizeof(g_wifi_vals[0]), "—");
        snprintf(g_wifi_hint[0], sizeof(g_wifi_hint[0]),
                 "确认无线开关开着、离路由器近一点，再重进这一页");
        IT_WIFI_DYN[0] = (SItem){ .cn = g_wifi_name[0], .type = ST_RO,
                                  .val = g_wifi_vals[0], .hint = g_wifi_hint[0] };
        g_wifi_n = 1;
    }

    MENUS[M_WIFI].n = g_wifi_n;
    IT_WIFI_DYN[g_wifi_n].cn = NULL;      /* 收尾标记（调试时看着安心） */
    return g_wifi_n;
}

/* 把 src 的当前值写成一行给人看的字 */
static void sv_text(int src, char *out, size_t cap)
{
    char b[160];
    switch (src) {
    case SV_VOLUME: {
        int v = alsa_volume();
        if (v < 0) snprintf(out, cap, "不可用");
        else       snprintf(out, cap, "%d%%", v);
        return; }
    case SV_BRIGHT: {
        int v = bl_cur_pct();
        if (v < 0) snprintf(out, cap, "不可用");
        else       snprintf(out, cap, "%d%%", v);
        return; }
    case SV_SLEEP:
        snprintf(out, cap, "%s", SLEEP_LABEL[sleep_cur_index()]);
        return;
    case SV_CPUGOV:
        snprintf(out, cap, "%s", sread_line(CPU_GOV_PATH, b, sizeof(b)) ? b : "不可用");
        return;
    case SV_GPUGOV:
        snprintf(out, cap, "%s", sread_line(GPU_GOV_PATH, b, sizeof(b)) ? b : "不可用");
        return;
    case SV_DMCGOV:
        snprintf(out, cap, "%s", sread_line(DMC_GOV_PATH, b, sizeof(b)) ? b : "不可用");
        return;
    case SV_HCOLS: snprintf(out, cap, "%d 列", g_pg_cols);   return;
    case SV_GCOLS: snprintf(out, cap, "%d 列", g_game_cols); return;
    case SV_COVERSTY:
        snprintf(out, cap, "%s", COVER_STY[g_cover_sty == 1 ? 1 : 0]);
        return;
    case SV_BEZEL:
        snprintf(out, cap, "%s",
                 BEZEL_LABEL[(g_bezel >= 0 && g_bezel < BEZEL_N) ? g_bezel : 0]);
        return;
    /* 游戏来源：光写"自动"看不出实际在读哪张卡，把探测结果一起显示。
       ⚠ 卡二的路径必须**读 config.ini 的 rom_dir2**，不能写死 "/roms2/roms" ——
       那个多套了一层 roms 的旧路径早就改掉了，写死的话这一行永远显示
       "（还没插卡）"，明明插着卡也报没插（真机踩过）。跟信息页同源。 */
    case SV_ROMSRC: {
        char r2[192] = "";
        if (!conf_get_str(CONF_INI, "rom_dir2", r2, sizeof(r2)) || !r2[0])
            snprintf(r2, sizeof(r2), "/roms2");
        int has2 = (access(r2, F_OK) == 0);
        if (g_romsrc == 1)
            snprintf(out, cap, "%s", LF("卡一 /roms", "Card 1 /roms"));
        else if (g_romsrc == 2)
            snprintf(out, cap, LF("卡二 %s", "Card 2 %s"),
                     has2 ? r2 : LF("（还没插卡）", "(not inserted)"));
        else
            snprintf(out, cap, LF("自动（卡二%s）", "Auto (card 2 %s)"),
                     has2 ? LF("优先", "preferred")
                          : LF("未插，读卡一", "absent, using card 1"));
        return; }
    /* RA 语言：**实读** retroarch.cfg，不用内存里的值 —— 玩家可能自己进 RA
       改过语言（RA 退出会回写 cfg），或者上次我们写失败，显示真的那个才对。 */
    case SV_RALANG: {
        snprintf(out, cap, "%s", RALANG_LABEL[ra_lang_read()]);
        return; }
    /* ⚠ SV_RAFWD 已停用（2026-09-16），设置页不再有这一项，这里也不返回。
       见 ra_fastfwd_* 的墓碑注释。 */
    /* 摇杆灯三项：也**实读存档**，理由同上（官方 Tools 菜单里也有个灯模式入口，
       玩家可能从那儿改过）。 */
    case SV_LED_MODE: {
        int m, b, f; led_conf_read(&m, &b, &f);
        snprintf(out, cap, "%s", LED_MODE_LABEL[m]);
        return; }
    case SV_LED_BRIGHT: {
        snprintf(out, cap, "%d%%", led_cur_pct());
        return; }
    case SV_LED_FX: {
        int m, b, f; led_conf_read(&m, &b, &f);
        /* 电量模式下灯效是那个服务自己定的，界面这一项写了也不生效 ——
           不说清楚玩家会以为设置坏了。⚠ 但**要把玩家选的那档名字显示出来**，
           写死"常亮"会让刚选了"追光"的人以为没选上。 */
        if (m == 1) snprintf(out, cap, LF("%s（电量模式不生效）", "%s (ignored in Battery mode)"),
                             LED_FX_LABEL[f]);
        else        snprintf(out, cap, "%s", LED_FX_LABEL[f]);
        return; }
    case SV_RAMENU: {
        snprintf(out, cap, "%s", RAMENU_LABEL[ra_menu_read()]);
        return; }
    case SV_RATHEME: {
        /* 只有 XMB 风格吃 xmb_theme；别的风格下写了也是白写，标一下。
           ⚠ 这里原来写的是 `snprintf(out, cap, "%s", LF("%s（仅 XMB 生效）", ...), 标签)`
             —— LF() 返回的串**里面就带 %s**，却被当成参数塞进 "%s" 格式里，
             于是界面上原样显示「%s（仅 XMB 生效）」（2026-09-16 截图抓到）。
             正确写法是：把 LF() 的结果当**格式**，标签当参数 —— 跟上面
             SV_LED_FX 那条一模一样。 */
        if (ra_menu_read() != 0)
            snprintf(out, cap, LF("%s（仅 XMB 生效）", "%s (XMB only)"),
                     RAXMB_LABEL[ra_theme_read()]);
        else
            snprintf(out, cap, "%s", RAXMB_LABEL[ra_theme_read()]);
        return; }
    /* RA 帧数显示：也**实读 cfg** —— 玩家可能自己进 RA 的「设置 → 屏幕显示」改过。 */
    case SV_RAFPS:
        snprintf(out, cap, "%s", RAFPS_LABEL[ra_fps_read()]);
        return;
    /* 界面语言：值就是内存里的 g_uilang，不用去盘上问 ——
       sconf_load() 启动时读过一次，之后只有本进程会改它。
       注意标签走 UILANG_LABEL 而不是 tr()："中文"故意不在表里。 */
    case SV_UILANG:
        snprintf(out, cap, "%s", UILANG_LABEL[(g_uilang >= 0 && g_uilang < UILANG_N) ? g_uilang : UI_ZH]);
        return;
    /* 虚拟内存：显示的是**实际挂着的**大小（/proc/swaps），
       不是"上次我设成了多少" —— 手工 swapoff 过也能一眼看出来。 */
    case SV_SWAP: {
        int i = swap_cur_index();
        snprintf(out, cap, "%s", SWAP_LABEL[i]);
        return; }
    /* 字号：光给百分比不直观（"110%" 到底多大？），后面缀一档文字描述。
       100% = 设计原大小，也就是这套 UI 按 1920x1080 画出来的样子。 */
    case SV_FSUI:
    case SV_FSGAME: {
        int v = (src == SV_FSUI) ? g_fs_ui : g_fs_games;
        /* 分档跟着上限走：游戏列表能到 300%，所以要留"超大"这一档，
           否则 300% 和 170% 都写着"特大"，看不出区别。 */
        const char *lv = v <= 90 ? "小" : v <= 110 ? "标准"
                       : v <= 160 ? "大" : v <= 220 ? "特大" : "超大";
        snprintf(out, cap, "%d%%（%s）", v, lv);
        return; }

    /* 自动存档位：光给个数字太干，"这是给谁用的"写进值里。 */
    case SV_AUTOSLOT:
        snprintf(out, cap, LF("状态槽 %d", "State slot %d"), g_autoslot);
        return;
    case SV_TZ: {
        char z[96];
        if (!tz_current(z, sizeof(z))) snprintf(out, cap, "不可用");
        else                           snprintf(out, cap, "%s", z);
        return; }

    case SV_I_WIFI_CUR: {
        char nm[96];
        wifi_current(nm, sizeof(nm));
        if (!nm[0]) { snprintf(out, cap, "未连接无线"); return; }
        char ip[112];
        net_line(ip, sizeof(ip));
        snprintf(out, cap, "%s · %s", nm, ip);
        return; }
    case SV_I_SSH_ADDR: {
        char ip[112];
        net_line(ip, sizeof(ip));
        char *sp = strstr(ip, " · ");          /* 有多个地址就只留第一个 */
        if (sp) *sp = '\0';
        /* net_line 取不到地址时给的是一句中文说明，用首字符是不是数字区分 */
        if (!ip[0] || ip[0] < '0' || ip[0] > '9') {
            snprintf(out, cap, "还没联网，先连 WiFi");
            return;
        }
        snprintf(out, cap, "root@%s", ip);
        return; }

    /* 金手指：两个只读状态行。都走实况查询 —— 用户手动删过 / 换过包，
       这里立刻是对的，不靠"我上次装过"这种记忆。 */
    case SV_I_CHEATS: {
        int n = cheat_count();
        if (n <= 0) snprintf(out, cap, "未安装");
        else        snprintf(out, cap, "%d 个 .cht", n);
        return; }
    case SV_I_CHEATPACK: {
        long sz = 0;
        const char *p = cheat_pack_find(&sz);
        if (!p) { snprintf(out, cap, "未找到"); return; }
        /* 只留"在哪个盘 + 多大"，完整路径太长，设置页那点宽度放不下 */
        snprintf(out, cap, "%s · %.1fMB",
                 strstr(p, "/roms/") ? "内存卡" : "内置", sz / 1048576.0);
        return; }

    /* --- 只读信息：现算，不缓存（分辨率/可用内存/剩余电量本来就会变） --- */
    case SV_I_BATT: {
        char st[32];
        int p = battery_pct(st, sizeof(st));
        if (p < 0) { snprintf(out, cap, "没有电池节点（台式/模拟器？）"); return; }
        const char *zh = "未知";
        if (strcasecmp(st, "Charging") == 0)        zh = "充电中";
        else if (strcasecmp(st, "Discharging") == 0) zh = "放电中";
        else if (strcasecmp(st, "Full") == 0)        zh = "已充满";
        else if (strcasecmp(st, "Not charging") == 0) zh = "未充电";
        snprintf(out, cap, "%d%%（%s）", p, zh);
        return; }
    case SV_I_NET:  net_line(out, cap);  return;
    case SV_I_DISK: disk_line(out, cap); return;
    case SV_I_TIME: {
        time_t now = time(NULL);
        struct tm tv = *localtime(&now);
        char tb[64];
        strftime(tb, sizeof(tb), "%Y-%m-%d %H:%M:%S", &tv);
        snprintf(out, cap, "%s %s", tb, tv.tm_zone ? tv.tm_zone : "");
        return; }
    case SV_I_RES: snprintf(out, cap, "%d × %d", g_win_w, g_win_h); return;
    case SV_I_DEV:
        snprintf(out, cap, "%s",
                 sread_line("/sys/firmware/devicetree/base/model", b, sizeof(b)) ? b : "—");
        return;
    case SV_I_KERN: {
        struct utsname u;
        if (uname(&u) == 0) snprintf(out, cap, "%s · %s", u.release, u.machine);
        else                snprintf(out, cap, "—");
        return; }
    case SV_I_CPU: {
        long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
        if (!cpuinfo_get("model name", b, sizeof(b)) &&
            !cpuinfo_get("Hardware", b, sizeof(b)))
            snprintf(b, sizeof(b), "ARM");
        snprintf(out, cap, "%s × %ld", b, ncpu > 0 ? ncpu : 1);
        return; }
    case SV_I_MEM: {
        char tot[32] = "0", avl[32] = "0";
        FILE *fh = fopen("/proc/meminfo", "r");
        if (fh) {
            char line[160];
            while (fgets(line, sizeof(line), fh)) {
                if (strncmp(line, "MemTotal:", 9) == 0)     sscanf(line + 9, "%31s", tot);
                if (strncmp(line, "MemAvailable:", 13) == 0) sscanf(line + 13, "%31s", avl);
            }
            fclose(fh);
        }
        /* meminfo 的单位是 kB */
        snprintf(out, cap, "%.2f GB 可用 / %.2f GB",
                 atof(avl) / 1024.0 / 1024.0, atof(tot) / 1024.0 / 1024.0);
        return; }
    case SV_I_GOV: {
        char c[32] = "?", g2[32] = "?", d[32] = "?";
        sread_line(CPU_GOV_PATH, c, sizeof(c));
        sread_line(GPU_GOV_PATH, g2, sizeof(g2));
        sread_line(DMC_GOV_PATH, d, sizeof(d));
        snprintf(out, cap, "CPU %s · GPU %s · DMC %s", c, g2, d);
        return; }
    case SV_I_ROM: {
        char r[192] = "";
        if (!conf_get_str(CONF_INI, "rom_dir2", r, sizeof(r)) || !r[0])
            if (!conf_get_str(CONF_INI, "rom_dir", r, sizeof(r)))
                snprintf(r, sizeof(r), "/roms");
        /* 带上真实条目数 —— 卡没插/没挂上去时一眼能看出来 */
        int total = 0;
        for (int i = 0; i < IX.np; i++) total += IX.p[i].ngames;
        snprintf(out, cap, LF("%s（%d 款）", "%s (%d games)"), r, total);
        return; }
    case SV_I_INDEX: {
        int total = 0;
        for (int i = 0; i < IX.np; i++) total += IX.p[i].ngames;
        snprintf(out, cap, LF("%d 个平台 · %d 款游戏", "%d systems · %d games"),
                 IX.np, total);
        return; }
    case SV_I_FRONT:
        snprintf(out, cap, "vgfront %s（原生 SDL2/KMSDRM）", VG_VERSION);
        return;
    default:
        snprintf(out, cap, "—");
        return;
    }
}

/* 把一项真正落到设备上。v：数值型是数值，选项型是**下标**。
   回执写进 fb（截图/日志都看得到），失败也要说清楚失败在哪。 */
static void sv_apply(int src, int v, char *fb, size_t cap)
{
    char cmd[320];
    fb[0] = '\0';

    switch (src) {
    case SV_VOLUME: {
        int pct = v < 0 ? 0 : (v > 100 ? 100 : v);
        /* ★ 控件名不能写死成 "Playback"。这台 MINILOONG 的
           /home/ark/.asoundrc 把 softvol 的 control.name 设成了 "Master"，
           根本没有 "Playback" 这个控件 —— 写死的话 amixer 报
           "Unable to find simple control" 并返回非 0，音量就永远调不动
           （用户 2026-09-15 反馈："目前系统音量无法在设置里调节"）。
           顺序照官方 /usr/local/bin/current_volume 的判断来：Master 优先。
           alsactl store 落盘 —— 不存的话重启又回到默认音量。 */
        int ok = 0;
        /* 顺序：① 已经探测到的那个（读值走的就是它，写读要对齐）
                 ② Master（本机 .asoundrc 的 softvol）
                 ③ Playback（多数通用发行版的默认名） */
        const char *cand[3];
        int ncand = 0;
        if (g_volctl) cand[ncand++] = g_volctl;
        cand[ncand++] = "Master";
        cand[ncand++] = "Playback";
        for (int i = 0; i < ncand && !ok; i++) {
            if (i > 0 && strcmp(cand[i], cand[0]) == 0) continue;   /* 别试两遍同一个 */
            snprintf(cmd, sizeof(cmd), "amixer sset %s %d%% >/dev/null 2>&1",
                     cand[i], pct);
            ok = srun(cmd);
        }
        if (ok) srun("alsactl store >/dev/null 2>&1");
        g_vol_cache = ok ? pct : -2;      /* 成功就更新缓存，失败下次重读 */
        if (ok) snprintf(fb, cap, "音量设为 %d%%", pct);
        else    snprintf(fb, cap, "音量设置失败（amixer）");
        return; }

    case SV_BRIGHT: {
        int pct = v < 5 ? 5 : (v > 100 ? 100 : v);
        char mx[32], val[32];
        int ok = 0;
        if (sread_line(BL_DIR "/max_brightness", mx, sizeof(mx))) {
            int m = atoi(mx);
            if (m > 0) {
                snprintf(val, sizeof(val), "%d", pct * m / 100);
                ok = swrite_line(BL_DIR "/brightness", val);
            }
        }
        if (!ok) {                        /* sysfs 直接写不动就退到 brightnessctl */
            snprintf(cmd, sizeof(cmd), "brightnessctl set %d%% >/dev/null 2>&1", pct);
            ok = srun(cmd);
        }
        if (ok) snprintf(fb, cap, "亮度设为 %d%%", pct);
        else    snprintf(fb, cap, "亮度设置失败（backlight 节点写不动？）");
        return; }
    case SV_SLEEP: {
        if (v < 0 || v >= SLEEP_N) { snprintf(fb, cap, "选项越界"); return; }
        const char *val = SLEEP_VAL[v];
        /* 官方那一路：写 .TIMEOUT（带换行 —— 官方 Restore 脚本也是 echo 写的）
           → 跑 auto_suspend_update.sh（内容是 Off 就 stop+disable，否则 restart+enable）。 */
        snprintf(cmd, sizeof(cmd),
                 "printf '%%s\\n' %s > %s && %s >/dev/null 2>&1",
                 val, SLEEP_TIMEOUT_PATH, SLEEP_UPDATE_SH);
        int ok = srun(cmd);
        /* 回读复核：真源就是那个文件，别信"命令没报错" */
        char b[32] = "";
        int back = sread_line(SLEEP_TIMEOUT_PATH, b, sizeof(b));
        if (ok && back && strcasecmp(b, val) == 0)
            snprintf(fb, cap, "息屏时间：%s%s", SLEEP_LABEL[v],
                     v == 0 ? "（自动待机已停用）" : "");
        else
            snprintf(fb, cap, "息屏时间设置失败（%s 写不动？）", SLEEP_TIMEOUT_PATH);
        return; }

    case SV_CPUGOV:
    case SV_GPUGOV:
    case SV_DMCGOV: {
        const char *path = (src == SV_CPUGOV) ? CPU_GOV_PATH
                         : (src == SV_GPUGOV) ? GPU_GOV_PATH : DMC_GOV_PATH;
        const char *const *ch = (src == SV_CPUGOV) ? GOV_CPU : GOV_DEVFREQ;
        int nch = (src == SV_CPUGOV) ? 6 : 5;
        if (v < 0 || v >= nch) { snprintf(fb, cap, "选项越界"); return; }
        int ok = swrite_line(path, ch[v]);
        if (!ok) {                        /* 有些内核节点要求带换行的 echo */
            snprintf(cmd, sizeof(cmd), "echo %s > %s 2>/dev/null", ch[v], path);
            ok = srun(cmd);
        }
        if (ok) snprintf(fb, cap, "%s → %s",
                         src == SV_CPUGOV ? "CPU" : src == SV_GPUGOV ? "GPU" : "DMC",
                         ch[v]);
        else    snprintf(fb, cap, "写入失败（%s 节点写不动？）",
                         src == SV_CPUGOV ? "CPU" : src == SV_GPUGOV ? "GPU" : "DMC");
        return; }

    case SV_HCOLS:
    case SV_GCOLS: {
        int cols = v + 3;                 /* 表从 "3" 开始 */
        if (src == SV_HCOLS) {
            if (cols < 3 || cols > 5) { snprintf(fb, cap, "越界"); return; }
            g_pg_cols = cols;
            /* 列数一变，原来的行列位置就没意义了 —— 选中项夹回范围、
               滚动归零，免得出现"光标在屏幕外"的空选中 */
            if (g_sel >= g_view_n) g_sel = g_view_n > 0 ? g_view_n - 1 : 0;
            if (g_sel < 0) g_sel = 0;
            g_scroll = 0;
        } else {
            if (cols < 3 || cols > 6) { snprintf(fb, cap, "越界"); return; }
            g_game_cols = cols;
            Platform *p = cur_plat();
            int n = p ? p->ngames : 0;
            if (g_gsel >= n) g_gsel = n > 0 ? n - 1 : 0;
            if (g_gsel < 0) g_gsel = 0;
            g_gscroll = 0;
        }
        sconf_save();
        snprintf(fb, cap, "%s改成 %d 列（已存 vgfront.conf）",
                 src == SV_HCOLS ? "主页" : "游戏列表", cols);
        return; }

    case SV_COVERSTY: {
        if (v < 0 || v >= 2) { snprintf(fb, cap, "越界"); return; }
        g_cover_sty = v;
        /* 卡片高变了，旧的滚动偏移落不到行界上（会停在半屏空白处）——
           归零最省心，跟上面改列数同一套处理。 */
        g_gscroll = 0;
        sconf_save();
        snprintf(fb, cap, "封面样式改成%s（已存 vgfront.conf）", COVER_STY[v]);
        return; }

    /* 遮罩：只写盘，**不重启前端**。挂载是启动器在启动游戏时按 pid 做的，
       没有"烘进纹理"这回事（跟字体/主题那两项不一样，别照着抄）。 */
    case SV_BEZEL: {
        if (v < 0 || v >= BEZEL_N) { snprintf(fb, cap, "越界"); return; }
        g_bezel = v;
        sconf_save();
        if (v == 0) snprintf(fb, cap, "遮罩已关闭（下次进游戏生效）");
        else        snprintf(fb, cap, "遮罩已设为 %s（下次进游戏生效）", BEZEL_LABEL[v]);
        return; }

    /* 游戏来源：写盘 → 重扫索引 → execv 自重启。
       ⚠ 跟字号那两项的关键区别：execv 自重启**不会**重跑
       vagaretro-front.sh，而索引是启动器那一步扫的（--gen-index），
       所以这里必须自己先扫一遍，否则重启回来还是旧列表。
       扫描只写 index.tsv 和兼容树，不碰显示，持有 DRM 时跑也安全
       （几秒，期间用品牌画面顶着）。 */
    case SV_ROMSRC: {
        if (v < 0 || v >= ROMSRC_N) { snprintf(fb, cap, "选项越界"); return; }
        if (v == g_romsrc) {
            snprintf(fb, cap, "已经是「%s」了", ROMSRC_LABEL[g_romsrc]);
            return;
        }
        g_romsrc = v;
        sconf_save();
        boot_splash("正在重扫游戏…", 0);
        printf("[游戏来源] 切到 %s，重扫索引…\n", ROMSRC_VAL[g_romsrc]);
        fflush(stdout);
        int rc = system(ROM_INDEX_CMD);
        if (rc != 0) {
            printf("[游戏来源] --gen-index 失败 rc=%d，沿用现有索引\n", rc);
            fflush(stdout);
        }
        snprintf(fb, cap, "游戏来源：%s，正在重启前端…", ROMSRC_LABEL[g_romsrc]);
        settings_restart(M_ROOT);
        return; }

    /* RA 语言：写 retroarch.cfg 就完事。
       ⚠ 与字号/游戏来源的关键区别：**不重启前端**，也没索引要重扫 ——
       前端界面是自己的，RA 的语言影响不到它。别照着那两项抄 restart。 */
    case SV_RALANG: {
        if (v < 0 || v >= RALANG_N) { snprintf(fb, cap, "选项越界"); return; }
        if (!ra_lang_write(v)) {
            snprintf(fb, cap, "改不动 retroarch.cfg（写入失败）");
            return;
        }
        /* 回读确认。回读顺手把 g_ralang 校准 —— 写失败/被别的进程覆盖
           都不会让内存值和盘上值脱钩。 */
        g_ralang = ra_lang_read();
        snprintf(fb, cap, "RetroArch 界面语言设为「%s」，下次进游戏生效",
                 RALANG_LABEL[g_ralang]);
        return; }

    /* 系统语言：写盘 + **必须重启前端**。
       ⚠ 跟上面 RA 语言/RA 加速键的关键区别：那两项只动 retroarch.cfg，
       跟前端界面无关；这一项动的是界面本身 —— 所有文字都已经**烘成纹理**了
       （bake_cached 还按原文哈希存了一层缓存），不重启就只是配置变了、
       屏幕上还是旧语言。走 settings_restart()，它 execv 自己、argv 重造，
       顺带把截图钩子的 --keys 丢掉，正好避免「重启完又按一次右键」来回翻转。 */
    case SV_UILANG: {
        if (v < 0 || v >= UILANG_N) { snprintf(fb, cap, "选项越界"); return; }
        if (v == g_uilang) {
            snprintf(fb, cap, "已经是「%s」了", UILANG_LABEL[g_uilang]);
            return;
        }
        g_uilang = v;
        sconf_save();
        printf("[界面语言] 切到 %s，重启前端…\n", UILANG_LABEL[g_uilang]);
        fflush(stdout);
        /* 不返回：settings_restart 里的 execv 成功就不会回来了。 */
        settings_restart(M_ROOT);
        return; }

    /* ⚠ SV_RAFWD 已停用（2026-09-16）：设置页已经没有这一项，没人会走到这里。
       **这一条曾经把玩家的热键组合整套废掉**（它连门禁键一起改），
       详见 ra_fastfwd_* 上的墓碑注释。不删代码是为了万一要接回来。 */

    /* 摇杆灯：三项共用一段。亮度是数值型（v = 百分比），模式和灯效是选项型
       （v = 下标）。改任何一项都整组重设一遍 —— 三者在硬件上是分开的属性，
       但**官方服务的开关状态取决于模式**，只改亮度也得重新判定一次
       （比如玩家从"电量"调亮一点，服务得继续开着；从"红"调亮，服务得继续关着）。 */
    case SV_LED_MODE:
    case SV_LED_FX:
    case SV_LED_BRIGHT: {
        int m, b, f;
        led_conf_read(&m, &b, &f);
        if (src == SV_LED_MODE) {
            if (v < 0 || v >= LED_MODE_N) { snprintf(fb, cap, "选项越界"); return; }
            m = v;
        } else if (src == SV_LED_FX) {
            if (v < 0 || v >= LED_FX_N) { snprintf(fb, cap, "选项越界"); return; }
            f = v;
        } else {
            int pct = v < 4 ? 4 : (v > 100 ? 100 : v);
            b = (pct * LED_MAXBRI + 50) / 100;
            if (b < 1) b = 1;
        }
        led_apply(m, b, f, src == SV_LED_MODE, fb, cap);   /* 失败也会把原因写进 fb */
        return; }

    /* RA 菜单风格 / 配色主题：跟上面 RA 语言一样**只写盘、不重启前端**
       （菜单外观是 RA 自己的事，跟前端界面无关），下次进游戏生效。 */
    case SV_RAMENU: {
        if (v < 0 || v >= RAMENU_N) { snprintf(fb, cap, "选项越界"); return; }
        if (!ra_menu_write(v)) { snprintf(fb, cap, "改不动 retroarch.cfg（写入失败）"); return; }
        int now = ra_menu_read();
        if (now == 0)
            snprintf(fb, cap, "菜单风格设为 XMB，配色用下面那项调（下次进游戏生效）");
        else
            snprintf(fb, cap, "菜单风格设为 %s（下次进游戏生效）", RAMENU_LABEL[now]);
        return; }

    case SV_RATHEME: {
        if (v < 0 || v >= RAXMB_N) { snprintf(fb, cap, "选项越界"); return; }
        if (!ra_theme_write(v)) { snprintf(fb, cap, "改不动 retroarch.cfg（写入失败）"); return; }
        if (ra_menu_read() != 0)
            snprintf(fb, cap, "主题设为 %s，但菜单风格不是 XMB —— 先切到 XMB 才看得到",
                     RAXMB_LABEL[ra_theme_read()]);
        else
            snprintf(fb, cap, "RA 主题设为 %s（下次进游戏生效）", RAXMB_LABEL[ra_theme_read()]);
        return; }

    /* RA 帧数显示：只写 fps_show 一个键（ra_cfg_set 内部自己循环把两份 cfg 都改）。
       ⚠ 这里**不**去校验 video_font_enable —— 那是 RA 的 OSD 文字总开关，
         出厂两份都是 true；真被谁关掉了，回执里替玩家下结论反而误导。 */
    case SV_RAFPS: {
        if (v < 0 || v >= RAFPS_N) { snprintf(fb, cap, "选项越界"); return; }
        if (!ra_fps_write(v)) { snprintf(fb, cap, "改不动 retroarch.cfg（写入失败）"); return; }
        if (ra_fps_read() == 1)
            snprintf(fb, cap, "RA 帧数显示已开启，画面左上角（下次进游戏生效）");
        else
            snprintf(fb, cap, "RA 帧数显示已关闭（下次进游戏生效）");
        return; }

    case SV_SWAP: {
        if (v < 0 || v >= SWAP_N) { snprintf(fb, cap, "选项越界"); return; }
        if (!swap_apply(atoi(SWAP_MB[v]), fb, cap)) return;
        snprintf(fb, cap, "虚拟内存已设为 %s（已写 fstab，重启后仍然生效）",
                 SWAP_LABEL[v]);
        return; }

    case SV_FSUI:
    case SV_FSGAME: {
        /* 两项上限不同：整体字号封 160%，游戏列表放到 300%（用户要求） */
        int hi  = (src == SV_FSUI) ? FS_PCT_MAX : FS_GAME_MAX;
        int pct = v < FS_PCT_MIN ? FS_PCT_MIN
                : v > hi ? hi : v;
        if (src == SV_FSUI) g_fs_ui    = pct;
        else                g_fs_games = pct;
        sconf_save();
        snprintf(fb, cap, "%s调成 %d%%，正在重启前端…",
                 src == SV_FSUI ? "前端字体" : "游戏列表字体", pct);
        printf("[字号] %s -> %d%%，重启前端…\n",
               src == SV_FSUI ? "ui" : "games", pct);
        fflush(stdout);
        settings_restart(M_DISP);
        return; }

    /* 自动存档位：只写 vgfront.conf，**不重启前端**（读它的是启动器，
       下次进游戏才用到）。越界值夹到 0..10，别让手工写坏的 conf 传下去。 */
    case SV_AUTOSLOT: {
        int s = v < AUTOSLOT_MIN ? AUTOSLOT_MIN
              : v > AUTOSLOT_MAX ? AUTOSLOT_MAX : v;
        g_autoslot = s;
        sconf_save();
        snprintf(fb, cap, "自动存档位设为状态槽 %d（下次进游戏生效）", s);
        printf("[存档位] autosave_slot -> %d\n", s);
        fflush(stdout);
        return; }

    case SV_TZ: {
        int nz = (int)(sizeof(TZ_CHOICES) / sizeof(TZ_CHOICES[0]));
        if (v < 0 || v >= nz) { snprintf(fb, cap, "选项越界"); return; }
        const char *z = TZ_CHOICES[v];
        /* timedatectl 会同时处理 /etc/localtime 软链和 /etc/timezone（systemd 环境下
           这是正路）。它要是不可用（dbus 没起之类）就手工落两份 —— zoneinfo
           里的文件直接软过去，glibc 一样认。 */
        snprintf(cmd, sizeof(cmd),
                 "timedatectl set-timezone %s >/dev/null 2>&1 || "
                 "{ ln -sf /usr/share/zoneinfo/%s /etc/localtime "
                 "&& echo %s > /etc/timezone; }", z, z, z);
        int ok = srun(cmd);
        /* ⚠ 必须 tzset()。localtime() 在 glibc 里把时区缓存住了，不刷的话
           本进程显示的还是旧时区 —— 表现是"设置页写着改了，时间没变"，
           得重启前端才对，会被当成设置没生效。 */
        tzset();
        if (ok) snprintf(fb, cap, "时区设为 %s", z);
        else    snprintf(fb, cap, "时区设置失败（timedatectl / /etc 写不动？）");
        return; }

    case SV_WIFI_RADIO: {
        int ok = srun(v ? "nmcli radio wifi on  >/dev/null 2>&1"
                        : "nmcli radio wifi off >/dev/null 2>&1");
        if (!ok) { snprintf(fb, cap, "切换失败（nmcli radio）"); return; }
        /* 刚打开时马上扫描是扫不到的 —— wpa_supplicant 起扫描要几秒。
           这里只报开关状态，扫描交给用户回列表页时做。 */
        snprintf(fb, cap, v ? "无线已打开（等两三秒再回列表扫描）" : "无线已关闭");
        return; }

    case SV_SSH: {
        /* enable/disable 一并做：只 start 的话重启就没了，用户会当成
           "我明明开过"。--now 一次把当前状态和开机自启都切过去。 */
        /* --no-reload 理由同 led_chase_*：不带的话 enable/disable 会顺带
           做一次 daemon-reload，在这台机器上单次 1.3 秒，开一下 SSH 也卡。 */
        int ok = v ? srun("systemctl enable --no-reload --now ssh >/dev/null 2>&1")
                   : srun("systemctl disable --no-reload --now ssh >/dev/null 2>&1");
        if (ok) snprintf(fb, cap, v ? "SSH 已开启（重启后仍自动启动）"
                                    : "SSH 已关闭");
        else    snprintf(fb, cap, "切换失败（systemctl ssh）");
        return; }

    default:
        snprintf(fb, cap, "该项只读，改不了");
        return;
    }
}

/* ---- 列数存档 ---------------------------------------------------------- */

static void sconf_save(void)
{
    FILE *fh = fopen(SCONF_PATH, "w");
    if (!fh) {
        printf("[设置] 写不了 %s（列数这次改的只在本次运行有效）\n", SCONF_PATH);
        fflush(stdout);
        return;
    }
    fprintf(fh, "# vgfront 自己的设置。由设置页写入；手工改也行，重启前端生效。\n");
    fprintf(fh, "#   home_cols  主页平台网格列数   3..5\n");
    fprintf(fh, "#   game_cols  游戏列表列数       3..6\n");
    fprintf(fh, "#   cover_style 游戏封面样式       0=竖版(4:3) 1=正方形(1:1)\n");
    fprintf(fh, "#   fs_ui      前端整体字号 %%      80..160（100 = 设计原大小）\n");
    fprintf(fh, "#   fs_games   游戏列表字号再乘 %%  80..300（在 fs_ui 基础上乘）\n");
    fprintf(fh, "#   theme      主题 id。内置：langos / neon / amber / paper /\n");
    fprintf(fh, "#              aurora / dusk / retro；\n");
    fprintf(fh, "#              自定义：内存卡 /roms/themes/*.theme 里的 id\n");
    fprintf(fh, "#   bezel      游戏遮罩分辨率，留空=关闭。可选 960x720（本机原生，\n");
    fprintf(fh, "#              推荐）/ 1024x768 / 640x480 / 720x720。\n");
    fprintf(fh, "#              ⚠ 必须选和屏幕一致的那档：本机是 960x720，选 1024x768\n");
    fprintf(fh, "#              会让画面矩形落到屏外（进游戏全黑）。\n");
    fprintf(fh, "#              **启动器**（vagaretro-shell.py）\n");
    fprintf(fh, "#              读这一行，在启动游戏时用 --appendconfig 挂给 RetroArch\n");
    fprintf(fh, "#   romsrc     游戏来源 auto / 1 / 2。**启动器**（vagaretro-shell.py）\n");
    fprintf(fh, "#              读这一行决定扫哪张卡：auto = 卡二优先（没插读卡一）、\n");
    fprintf(fh, "#              1 = 只读卡一（/roms）、2 = 只读卡二（/roms2）\n");
    fprintf(fh, "#   ui_lang    前端界面语言             0=中文 1=English\n");
    fprintf(fh, "#   （RA 界面语言**故意不存这里** —— 真源是 retroarch.cfg 的\n");
    fprintf(fh, "#     user_language，RA 自己退出时也会回写那一份，两边都记必对不上）\n");

    fprintf(fh, "#   autosave_slot 「退出自动存档」用的状态槽   0..10（出厂 9）\n");
    fprintf(fh, "home_cols=%d\ngame_cols=%d\ncover_style=%d\nfs_ui=%d\nfs_games=%d\n"
                "bezel=%s\nromsrc=%s\ntheme=%s\nui_lang=%d\nautosave_slot=%d\n",
            g_pg_cols, g_game_cols, g_cover_sty, g_fs_ui, g_fs_games,
            BEZEL_VAL[(g_bezel >= 0 && g_bezel < BEZEL_N) ? g_bezel : 0],
            ROMSRC_VAL[(g_romsrc >= 0 && g_romsrc < ROMSRC_N) ? g_romsrc : 0],
            g_theme_id,
            (g_uilang >= 0 && g_uilang < UILANG_N) ? g_uilang : UI_ZH,
            g_autoslot);
    fclose(fh);
}

static void sconf_load(void)
{
    char b[32];
    if (conf_get_str(SCONF_PATH, "home_cols", b, sizeof(b))) {
        int v = atoi(b);
        if (v >= 3 && v <= 5) g_pg_cols = v;
    }
    if (conf_get_str(SCONF_PATH, "game_cols", b, sizeof(b))) {
        int v = atoi(b);
        if (v >= 3 && v <= 6) g_game_cols = v;
    }
    if (conf_get_str(SCONF_PATH, "cover_style", b, sizeof(b))) {
        int v = atoi(b);
        if (v == 0 || v == 1) g_cover_sty = v;
    }
    /* 字号：越界就忽略（手工把 conf 写成 500 的话，宁可回到默认 100，
       也不能让整屏字大到把布局撑爆）。 */
    if (conf_get_str(SCONF_PATH, "fs_ui", b, sizeof(b))) {
        int v = atoi(b);
        if (v >= FS_PCT_MIN && v <= FS_PCT_MAX) g_fs_ui = v;
    }
    if (conf_get_str(SCONF_PATH, "fs_games", b, sizeof(b))) {
        int v = atoi(b);
        if (v >= FS_PCT_MIN && v <= FS_GAME_MAX) g_fs_games = v;
    }
    /* 遮罩存的是字符串（"960x720"），要拿 BEZEL_VAL 反查下标。
       认不出来的一律当关闭 —— 手工写错别名不该让前端行为不可预期。
       ★★ 2026-09-18（浪总："什么固件什么dtb你就默认对应的遮罩"）：
         **没有这一行**时不再默认"关闭"，而是按本机屏幕自动定档 ——
         见 bezel_auto_detect() 的长注释。两种情况必须分清楚：
           · conf 里没 `bezel=` 这一行  ⇒ conf_get_str 返回 0 ⇒ 自动探测
           · conf 里是 `bezel=`（空串）  ⇒ 返回 1 且 b = ""  ⇒ 玩家显式
             选了「关闭」，尊重他的选择，不要又给自动开回来 */
    if (conf_get_str(SCONF_PATH, "bezel", b, sizeof(b))) {
        g_bezel = 0;
        for (int i = 1; i < BEZEL_N; i++)
            if (strcmp(b, BEZEL_VAL[i]) == 0) { g_bezel = i; break; }
    } else {
        g_bezel = bezel_auto_detect();
    }
    /* 游戏来源同理：认不出的值回到"自动"。 */
    if (conf_get_str(SCONF_PATH, "romsrc", b, sizeof(b))) {
        g_romsrc = 0;
        for (int i = 1; i < ROMSRC_N; i++)
            if (strcmp(b, ROMSRC_VAL[i]) == 0) { g_romsrc = i; break; }
    }
    /* 界面语言：认不出的值回到中文（手工把 conf 写错不该让界面变成
       一堆看不懂的英文）。这一项只影响烘纹理时的查表，跟字体无关，
       所以放在 sconf_load 里（在字体表之前）没问题。 */
    if (conf_get_str(SCONF_PATH, "ui_lang", b, sizeof(b))) {
        int v = atoi(b);
        if (v == UI_ZH || v == UI_EN) g_uilang = v;
    }


    /* 自动存档位：认不出的值（手工把 conf 写坏）回到出厂 9 ——
       宁可回到一个"不会盖掉玩家手动存档"的位号，也不跟着乱写。 */
    if (conf_get_str(SCONF_PATH, "autosave_slot", b, sizeof(b))) {
        int v = atoi(b);
        if (v >= AUTOSLOT_MIN && v <= AUTOSLOT_MAX) g_autoslot = v;
    }
    /* RA 语言：**不读 vgfront.conf**，直接从 retroarch.cfg 认一下，
       让设置页一进来就显示真实档位。 */
    g_ralang = ra_lang_read();

    /* ⚠ RA 快进键的"自检写入"**已停用**（2026-09-16）。
       以前这里发现 input_hold_fast_forward_btn = "nul" 就自动写一遍默认档
       （L2），顺带把门禁键拉回 "10"。用户要求"快捷键换回原来的方式"之后，
       前端改成**一个 RA 热键的字节都不写** —— 出厂是 nul 就让它保持 nul。
       下面这段留作墓碑，函数本体见 ra_fastfwd_* 的注释。 */
}

/* ---- 交互 ------------------------------------------------------------- */

static void set_flash(const char *s)
{
    snprintf(g_flash, sizeof(g_flash), "%s", s ? s : "");
    g_flash_t0 = SDL_GetTicks();
    mark_dirty();
}

/* ---- 连接 --------------------------------------------------------------- */

/* 真去连。pass 为空/NULL 时走"已存过的连接"那条路（不重复问密码）。

   ⚠ nmcli 的连接是**同步阻塞**的，成不成要 3~15 秒。所以先画一帧
   "正在连接…" 再进阻塞 —— 否则用户按下 A 之后整屏一动不动，
   会当成死机然后去长按电源键。 */
static void wifi_do_connect(const char *ssid, const char *pass)
{
    char q[160], cmd[640];
    shell_sq(q, sizeof(q), ssid);

    if (pass && *pass) {
        char qp[256];
        shell_sq(qp, sizeof(qp), pass);
        snprintf(cmd, sizeof(cmd),
                 "nmcli device wifi connect %s password %s >/dev/null 2>&1",
                 q, qp);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "nmcli connection up id %s >/dev/null 2>&1 || "
                 "nmcli device wifi connect %s >/dev/null 2>&1", q, q);
    }

    set_flash("正在连接，请稍候…");
    render(0);                    /* 先把这句话画出去，再进阻塞 */

    int ok = srun(cmd);
    if (ok) {
        char nm[96], fb[160];
        wifi_current(nm, sizeof(nm));
        snprintf(fb, sizeof(fb), "已连接 %s", nm[0] ? nm : ssid);
        set_flash(fb);
    } else {
        set_flash("连接失败：密码不对 / 信号太弱 / 超出范围");
    }
    mark_dirty();
}

/* ---- 密码输入弹层 ---------------------------------------------------------
   WiFi 密码是这个前端里唯一需要"打一串任意字符"的地方。掌机没有物理键盘，
   于是照搜索页那套软键盘再做一层：10 列网格 + 十字键选 + A 输入。

   键位表把大写字母放前面、SHIFT 切小写 —— 家庭 WiFi 密码多是
   "首字母大写 + 其余小写 + 几个符号"，默认大写能少按不少次 SHIFT。
   键表里全是单字节 ASCII，所以 DEL 直接砍最后一个字节就是砍一个字符，
   不用像中文那样回退 UTF-8 边界。 */

#define KB2_COLS   10
#define KB2_KEY_H  54
#define KB2_GAP    10
#define KB2_RAD    10

static const char *const KB2_KEYS[] = {
    "1","2","3","4","5","6","7","8","9","0",
    "Q","W","E","R","T","Y","U","I","O","P",
    "A","S","D","F","G","H","J","K","L","-",
    "Z","X","C","V","B","N","M",".","_","@",
    "SHIFT","#","!","?","*","+","/","=","DEL","OK",
};
#define KB2_N       ((int)(sizeof(KB2_KEYS) / sizeof(KB2_KEYS[0])))   /* 50 */
#define KB2_ROWS    5                    /* 50 / 10 */
#define KB2_I_SHIFT 40
#define KB2_I_DEL   48
#define KB2_I_OK    49

static int  g_kb_on;                     /* 弹层开着吗 */
static int  g_kb_sel;                    /* 当前选中的键 */
static int  g_kb_shift = 1;              /* 1 = 大写（默认），0 = 小写 */
static char g_kb_buf[68];                /* 已输入的密码 */
static char g_kb_ssid[64];               /* 要连的 SSID */

static void kb2_press(void)
{
    int i = g_kb_sel;
    if (i < 0 || i >= KB2_N) return;

    if (i == KB2_I_SHIFT) { g_kb_shift = !g_kb_shift; return; }
    if (i == KB2_I_DEL) {
        size_t n = strlen(g_kb_buf);
        if (n) g_kb_buf[n - 1] = '\0';
        return;
    }
    if (i == KB2_I_OK) {
        char ssid[64], pass[72];
        snprintf(ssid, sizeof(ssid), "%s", g_kb_ssid);
        snprintf(pass, sizeof(pass), "%s", g_kb_buf);
        g_kb_on = 0;                 /* 先收弹层：接着要阻塞十几秒，留着它会
                                        让人以为还能改，其实按什么都没用 */
        mark_dirty();
        wifi_do_connect(ssid, pass);
        /* 连完不管成没成都重扫一遍：连上了 CURRENT WIFI 会变，
           列表里那张卡也该从"新网络"变成"已保存"。 */
        if (g_page == PAGE_SETTINGS && g_setmenu == M_WIFI) wifi_scan();
        return;
    }

    char c = KB2_KEYS[i][0];
    if (!g_kb_shift && c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    size_t n = strlen(g_kb_buf);
    if (n + 1 < sizeof(g_kb_buf)) { g_kb_buf[n] = c; g_kb_buf[n + 1] = '\0'; }
}

static void kb2_open(const char *ssid)
{
    g_kb_on = 1;
    g_kb_sel = 0;
    g_kb_shift = 1;
    g_kb_buf[0] = '\0';
    snprintf(g_kb_ssid, sizeof(g_kb_ssid), "%s", ssid ? ssid : "");
    mark_dirty();
}

/* nav 编码与 settings_key 一致：0上 1下 2左 3右 4A 5B */
static void kb2_nav(int nav)
{
    int r = g_kb_sel / KB2_COLS, c = g_kb_sel % KB2_COLS;
    switch (nav) {
    case 0: if (r > 0)                g_kb_sel -= KB2_COLS; break;
    case 1: if (r < KB2_ROWS - 1)     g_kb_sel += KB2_COLS; break;
    case 2: if (c > 0)                g_kb_sel--; break;
    case 3: if (c < KB2_COLS - 1)     g_kb_sel++; break;
    case 4: kb2_press(); break;
    case 5: g_kb_on = 0; break;                  /* B = 取消 */
    default: break;
    }
    mark_dirty();
}

/* ---- 交互 ------------------------------------------------------------- */

static void settings_open(void)
{
    g_page = PAGE_SETTINGS;
    g_setmenu = M_ROOT;
    g_setrow = 0;
    g_settop = 0;
    g_setedit = 0;
    g_flash[0] = '\0';
    g_kb_on = 0;
    g_glass_valid = 0;        /* 背景这会儿才画出来，模糊得重算 */
    mark_dirty();
    printf("[设置] 进入（音量 %d，亮度 %d%%，主页 %d 列 / 游戏 %d 列）\n",
           alsa_volume(), bl_cur_pct(), g_pg_cols, g_game_cols);
    fflush(stdout);
}

static void settings_leave(void)
{
    g_page = PAGE_HOME;
    g_setedit = 0;
    /* 回主页时顺手把分类视图重建一次 —— 主页列数可能在设置里改过，
       build_view 本身跟列数无关，但选中项要重新夹一遍范围。 */
    build_view();
    mark_dirty();
}

static void settings_back(void)
{
    if (g_setedit) { g_setedit = 0; mark_dirty(); return; }
    if (g_setmenu != M_ROOT) {
        /* 回父菜单，光标停回"刚才从哪一项进来的"那一行。
           ⚠ M_WIFI / M_LED 都不在根菜单里（分别是 M_NET、M_DISP 的子页），
           直接搜会搜不到、光标掉回第一行 —— 所以先折回父菜单再搜。 */
        int from = (g_setmenu == M_WIFI) ? M_NET
                 : (g_setmenu == M_LED)  ? M_DISP : g_setmenu;
        int back = 0;
        for (int i = 0; i < MENUS[M_ROOT].n; i++)
            if (IT_ROOT[i].menu == from) { back = i; break; }
        g_setmenu = M_ROOT;
        g_setrow = back;
        g_settop = 0;
        mark_dirty();
        return;
    }
    settings_leave();
}

static void settings_move(int d)
{
    int n = MENUS[g_setmenu].n;
    if (n <= 0) return;
    int r = g_setrow + d;
    if (r < 0) r = 0;                 /* 跟网页版一致：夹紧，不环绕 */
    if (r > n - 1) r = n - 1;
    if (r == g_setrow) return;
    g_setrow = r;
    g_setedit = 0;                    /* 移动即取消"待确认" */
    g_flash[0] = '\0';
    mark_dirty();
}

/* 左右改值。dir = -1 / +1 */
static void setting_step(const SItem *it, int dir)
{
    if (!it || it->type == ST_RO || it->type == ST_MENU) return;

    char fb[160];
    if (it->type == ST_BUTTON) return;

    /* ★ 开关型（无线开关 / SSH 服务）：**翻一下就切**。
       2026-09-17 用户报「WiFi 无法关闭」，根因就在这儿 ——
       这两项既没有 choices，又不在下面那串 `cur` 判断里（SV_VOLUME /
       SV_BRIGHT / SV_LED_BRIGHT / SV_FSUI / SV_FSGAME / SV_AUTOSLOT），
       于是 cur 恒为 -1，直接吐「读不到当前值，无法调节」就 return 了，
       **连 nmcli 都没调** —— NetworkManager 的 journal 里收不到任何 off
       请求（只有别人发的 wireless-enabled:on），所以怎么按都关不掉。
       这里按 ST_TOGGLE 定义处那句注释实现：「A 或左右翻转」。 */
    if (it->type == ST_TOGGLE) {
        int st = sv_toggle_state(it->src);
        if (st < 0) {
            set_flash("读不到当前值，无法调节");
            mark_dirty();
            return;
        }
        sv_apply(it->src, !st, fb, sizeof(fb));
        set_flash(fb);
        mark_dirty();
        return;
    }

    if (it->choices) {
        int cur = sv_cur_index(it);
        int i;
        /* 摇杆灯「灯光模式」**不许回绕**（2026-09-16 用户报"不能关闭"的成因）：
           它第 0 档是「关闭」、第 9 档是「彩虹」，一旦回绕，在「关闭」上再按
           一下左键就跳到「彩虹」—— 灯不但没关，还变成彩色流动。而每次操作
           本身有延迟（老写法要 3 秒），玩家很容易连按两下，把自己刚关掉的灯
           又点亮，看起来就是"关不掉"。
           其余选项型（音量 / 主题 / 字号 …）保持原来的回绕行为不变。 */
        if (it->src == SV_LED_MODE) {
            i = cur + dir;
            if (i < 0)              i = 0;
            if (i >= it->nch)       i = it->nch - 1;
        } else {
            i = (cur + dir + it->nch) % it->nch;
        }
        sv_apply(it->src, i, fb, sizeof(fb));
    } else {
        char b[64];
        int cur = -1;
        if (it->src == SV_VOLUME) cur = alsa_volume();
        else if (it->src == SV_BRIGHT) {
            snprintf(b, sizeof(b), "%d", bl_cur_pct());
            cur = atoi(b);
        }
        /* 摇杆灯亮度：跟屏幕亮度一样是"去问系统"型（真源在存档里，
           不过存档也是我们写的；实读保证跟官方脚本改动不脱钩）。 */
        else if (it->src == SV_LED_BRIGHT) cur = led_cur_pct();
        /* 字号：当前值就在内存里，直接读，不用去问系统 */
        else if (it->src == SV_FSUI)   cur = g_fs_ui;
        else if (it->src == SV_FSGAME) cur = g_fs_games;

        else if (it->src == SV_AUTOSLOT) cur = g_autoslot;
        if (cur < 0) { snprintf(fb, sizeof(fb), "读不到当前值，无法调节"); }
        else {
            int step = it->step ? it->step : 1;
            /* 字号过了 FS_PCT_FAST_FROM 之后步子翻倍 —— 游戏列表能调到 300%，
               一律 10 一格的话从 100 拉上去要按二十下。 */
            if ((it->src == SV_FSUI || it->src == SV_FSGAME)
                && cur >= FS_PCT_FAST_FROM)
                step *= 2;
            int v = cur + dir * step;
            if (v < it->vmin) v = it->vmin;
            if (v > it->vmax) v = it->vmax;
            sv_apply(it->src, v, fb, sizeof(fb));
        }
    }
    set_flash(fb);
    mark_dirty();
}

static void do_action(int action)
{
    switch (action) {
    case SA_RESTART_FRONT:
        /* 写一个"什么都不做"的启动请求。外层 while 会 bash 它、然后 continue，
           于是 run_sdl 再跑一遍、vgfront 被重新拉起 —— 这才是"重启前端"。
           直接 g_quit=1 是不行的：那会被当成"用户退出"，外层走收尾逻辑
           把服务结束掉，前端就再也不起来了（见 vagaretro-front.sh 的收尾段）。 */
        if (write_script(LAUNCH_REQ,
                "#!/bin/sh\n"
                "# vgfront 设置页：重启前端。\n"
                "# 本脚本**故意什么都不做** —— 存在的意义只是让启动器的 while 循环\n"
                "# 走一次 `bash launch.sh; continue`，从而重新拉起 vgfront。\n"
                "sleep 1\n"
                "exit 0\n")) {
            g_flash[0] = '\0';
            /* 退出到被外层重新拉起之间有 1 秒多的黑（脚本里那个 sleep），
               先顶一帧品牌画面；完整的动画由新进程自己接着播。 */
            boot_splash("正在重启前端…", 0);
            g_quit = 2;               /* 2 = "因启动请求而退出"，与用户退出区分 */
        } else {
            set_flash("重启失败：写不了 launch.sh");
        }
        return;

    case SA_REBOOT:
        /* 只用 ES 约定的标志文件，真正的动作交给启动器的 handle_es_flags()。
           这样跨机器/跨权限都成立 —— 前端是不是 root、有没有 sudo 都不影响。 */
        if (touch_file(ES_RESTART_FLAG)) {
            g_flash[0] = '\0';
            /* 命令发出去到真正重启之间，屏幕会先黑下来 —— 用品牌画面顶上
               （用户："有涉及黑屏加载的地方都用LANGOS的动画代替"）。 */
            boot_splash("正在重启系统…", 400);
            g_quit = 1;
        }
        else set_flash("重启失败：写不了 /tmp/es-sysrestart");
        return;

    case SA_SHUTDOWN:
        if (touch_file(ES_SHUTDOWN_FLAG)) {
            g_flash[0] = '\0';
            boot_splash("正在关机…", 400);
            g_quit = 1;
        }
        else set_flash("关机失败：写不了 /tmp/es-shutdown");
        return;

    /* 金手指：装/卸都要跑几十秒的 shell（cheat_install 内部先顶一帧品牌画面）。
       ⚠ 收尾得自己做 —— do_action 的调用方 setting_confirm() 既不管回执也不管
       重绘，别指望它。 */
    case SA_INSTALL_CHEATS: {
        char fb[192];
        cheat_install(fb, sizeof(fb));
        set_flash(fb);
        mark_dirty();
        return; }

    case SA_REMOVE_CHEATS: {
        char fb[192];
        cheat_remove(fb, sizeof(fb));
        set_flash(fb);
        mark_dirty();
        return; }

    /* 快捷键说明页：只读、无副作用、随时能退，所以**不**走 danger 那套
       两次确认（setting_confirm 里也特判了，正常按 A 一次就到这）。 */
    case SA_HELP:
        help_open();
        return;

    default:
        return;
    }
}

static void setting_confirm(const SItem *it)
{
    if (!it) return;

    /* WiFi 列表的行是动态的：按 A = 连它。
       存过密码的直接连；没存过的先弹软键盘要一次密码。 */
    if (g_setmenu == M_WIFI && it->type == ST_BUTTON) {
        int i = g_setrow;
        if (i < 0 || i >= g_wifi_n) return;
        if (g_wifi_saved[i]) wifi_do_connect(g_wifi_name[i], NULL);
        else                 kb2_open(g_wifi_name[i]);
        return;
    }

    /* 主题列表的行也是动态的：按 A = 换成这套。
       主题是**单选**、可逆、没有破坏性，所以不做"再按一次确认"——
       直接换（内部会重启前端回到本页，用户能连着试）。 */
    if (g_setmenu == M_THEME && it->type == ST_BUTTON) {
        if (it->arg && strcmp(it->arg, g_theme_id) == 0) {
            set_flash("已经在用这套主题了");
            return;
        }
        theme_set(it->arg);
        return;
    }

    /* 快捷键说明页：只读、可逆、没有任何副作用 —— 跟主题那行一个道理，
       不做"再按一次 A 确认"（那个机制是给关机/清除金手指这类动作的）。
       放在 switch 之前 return，跟上面两个动态列表同一个位置。 */
    if (it->action == SA_HELP) {
        help_open();
        return;
    }

    switch (it->type) {
    case ST_MENU:
        if (it->menu >= 0 && it->menu < NMENU) {
            /* 进 WiFi 列表就先扫一遍（阻塞 1~3 秒）。放这里而不是定时刷新：
               扫描比较贵，而用户要的正是"我进来时看到的是最新的"。 */
            if (it->menu == M_WIFI)  wifi_scan();
            if (it->menu == M_THEME) theme_scan();
            g_setmenu = it->menu;
            g_setrow = 0;
            g_settop = 0;
            g_setedit = 0;
            mark_dirty();
        }
        return;

    case ST_BUTTON:
        /* 危险动作两次确认（网页版也是这个手感）：第一次只是亮出提示 */
        if (!g_setedit) {
            g_setedit = 1;
            set_flash("再按一次 A 确认");
            return;
        }
        do_action(it->action);
        return;

    case ST_VAL:
    case ST_TOGGLE:
        /* 值项按 A 也当"往右调一格" —— 左右能调、A 也能调，
           手柄上少一个需要记的组合（不用先"进编辑态"再改） */
        setting_step(it, +1);
        return;

    default:
        return;
    }
}

/* 设置页按键。键盘与手柄都走这里（手柄在 handle_pad 里转成同一个调用）。 */
static void settings_key(int nav)
{
    /* 密码弹层开着时，方向键/A/B 全归它 —— 底下的列表不该跟着动 */
    if (g_kb_on) { kb2_nav(nav); return; }

    const SMenu *m = cur_menu();
    const SItem *it = (g_setrow >= 0 && g_setrow < m->n) ? &m->items[g_setrow] : NULL;

    switch (nav) {
    case 0: settings_move(-1); return;                 /* 上 */
    case 1: settings_move(+1); return;                 /* 下 */
    case 2: if (it) setting_step(it, -1); return;      /* 左 */
    case 3: if (it) setting_step(it, +1); return;      /* 右 */
    case 4: setting_confirm(it); return;               /* A */
    case 5: settings_back(); return;                   /* B */
    default: return;
    }
}

/* 回执显示 4 秒后自动消失。主循环每帧调一次。 */
static void settings_tick(void)
{
    if (!g_flash[0]) return;
    if (SDL_GetTicks() - g_flash_t0 > 4000) {
        g_flash[0] = '\0';
        mark_dirty();
    }
}

/* ---- 绘制 -------------------------------------------------------------- */

/* 版式数值起于 index.html 的 .settings-*（shell 60/40、行高 46、圆角 20），
   2026-09-14 用户要求「字体加大」后整体上调一档：
   行名 18→24、值 17→21、标题 24→28、行高 46→58。
   行名改用中文，字少一半、字大一圈，两种改法叠加后一行读起来正合适。 */
/* ⚠ 2026-09-14 用户："设置界面改成全屏，文字调大"。
   原来是**居中浮层**面板（左右各留 48 设计px、高度按内容自适应再垂直居中）。
   现在铺满：边距归零、直角（贴边还留圆角会很怪）、高度占满内容区，
   底栏那一行按键提示仍然露在下面。字号同步 ≈1.5x 上调。 */
#define SET_SHELL_X   0
#define SET_SHELL_Y   0
#define SET_RAD       0
#define SET_CRUMB_FS  42      /* ≈22px（原 15） */
#define SET_HEAD_PX   30
#define SET_HEAD_PY   24
#define SET_ROW_H     86      /* ≈46px（原 31） */
#define SET_ROW_RAD   12
#define SET_ROW_PX    28
#define SET_AUX_FS    21
#define SET_NAME_FS   36      /* ≈19px（原 13） */
#define SET_VAL_FS    32      /* ≈17px（原 11） */
#define SET_FOOT_FS   26      /* ≈14px（原 9） */
#define SET_FOOT_PY   16

/* ---- 毛玻璃面板 -----------------------------------------------------------
   SDL2 的 renderer 没有 shader / 滤镜接口，"模糊"只有两条路：
   (a) 把画面读回内存，CPU 上做 box blur 再传回纹理；
   (b) 把帧缓存放进一张 1/N 的小纹理再放大回去，借双线性插值近似模糊。

   这里走 (a)。决定因素是**圆角**：面板是 24px 圆角，(b) 贴回来的是矩形，
   四角会硬邦邦地溢出去；要吃圆角就得再套一层遮罩，而 (a) 在合成时按
   圆角公式写 alpha 就完了，一次成型。

   代价是每帧要 glReadPixels 一次。所以**结果缓存住**：面板背后是主页，
   它在设置页里根本不会变，进页面算一次就够，之后按键不重算。 */

static SDL_Texture *g_glass_tex = NULL;
static int g_glass_w = 0, g_glass_h = 0;
static int g_glass_valid = 0;

/* 圆角遮罩的覆盖率（0~255），边缘留 1px 过渡做抗锯齿。
   直边直接返回 255，只有四个角要算。 */
static int round_alpha(int x, int y, int w, int h, int r)
{
    double cx, cy;
    if      (x < r && y < r)            { cx = r - x - 0.5;       cy = r - y - 0.5; }
    else if (x >= w - r && y < r)       { cx = x - (w - r) + 0.5; cy = r - y - 0.5; }
    else if (x < r && y >= h - r)       { cx = r - x - 0.5;       cy = y - (h - r) + 0.5; }
    else if (x >= w - r && y >= h - r)  { cx = x - (w - r) + 0.5; cy = y - (h - r) + 0.5; }
    else return 255;
    double d = sqrt(cx * cx + cy * cy);
    double a = (double)r - d;
    if (a <= -1.0) return 0;
    if (a >=  0.0) return 255;
    int al = (int)((a + 1.0) * 255.0);
    return al < 0 ? 0 : (al > 255 ? 255 : al);
}

/* 把 rc 这块做成毛玻璃画上去。tint / tint_a 是"玻璃"本身的颜色，
   叠在模糊后的背景之上 —— 面板才不会是背景的纯滤镜版。 */
static void glass_panel(SDL_Rect rc, int rad, Uint32 tint, int tint_a)
{
    const int w = rc.w, h = rc.h;
    if (w < 8 || h < 8) return;

    /* 尺寸没变就直接复用上一块 —— 面板背后是主页，设置页里它不会动 */
    if (g_glass_valid && g_glass_tex && g_glass_w == w && g_glass_h == h) {
        SDL_RenderCopy(REN, g_glass_tex, NULL, &rc);
        return;
    }

    Uint32 *src = (Uint32 *)malloc((size_t)w * h * 4);
    Uint32 *out = (Uint32 *)malloc((size_t)w * h * 4);
    if (!src || !out) {
        free(src); free(out);
        draw_round(REN, rc, rad, tint, 255);      /* 兜底：退回纯色面板 */
        return;
    }
    if (SDL_RenderReadPixels(REN, &rc, SDL_PIXELFORMAT_ARGB8888,
                             src, w * 4) != 0) {
        free(src); free(out);
        draw_round(REN, rc, rad, tint, 255);
        return;
    }

    /* --- 1) 降采样 1/DS，块内取均值。这一步就是模糊的主体 --- */
    const int DS = 4;
    int sw = (w + DS - 1) / DS, sh = (h + DS - 1) / DS;
    Uint32 *A = (Uint32 *)calloc((size_t)sw * sh, 4);
    Uint32 *B = (Uint32 *)calloc((size_t)sw * sh, 4);
    if (!A || !B) {
        free(src); free(out); free(A); free(B);
        draw_round(REN, rc, rad, tint, 255);
        return;
    }

    for (int y = 0; y < sh; y++) {
        for (int x = 0; x < sw; x++) {
            int r = 0, g2 = 0, b = 0, n = 0;
            for (int dy = 0; dy < DS; dy++) {
                int yy = y * DS + dy;
                if (yy >= h) break;
                const Uint32 *row = src + (size_t)yy * w;
                for (int dx = 0; dx < DS; dx++) {
                    int xx = x * DS + dx;
                    if (xx >= w) break;
                    Uint32 p = row[xx];
                    r += (p >> 16) & 0xff;
                    g2 += (p >> 8) & 0xff;
                    b += p & 0xff;
                    n++;
                }
            }
            if (!n) n = 1;
            A[y * sw + x] = 0xff000000u | ((Uint32)(r / n) << 16)
                          | ((Uint32)(g2 / n) << 8) | (Uint32)(b / n);
        }
    }

    /* --- 2) 在小图上再走两遍 3x3 均值。低分辨率上做，成本可忽略，
           却把有效模糊半径从 4px 抬到 12px 上下，糊得更"毛" --- */
    for (int pass = 0; pass < 2; pass++) {
        for (int y = 0; y < sh; y++) {
            for (int x = 0; x < sw; x++) {
                int r = 0, g2 = 0, b = 0, n = 0;
                for (int dy = -1; dy <= 1; dy++) {
                    int yy = y + dy;
                    if (yy < 0 || yy >= sh) continue;
                    const Uint32 *row = A + (size_t)yy * sw;
                    for (int dx = -1; dx <= 1; dx++) {
                        int xx = x + dx;
                        if (xx < 0 || xx >= sw) continue;
                        Uint32 p = row[xx];
                        r += (p >> 16) & 0xff;
                        g2 += (p >> 8) & 0xff;
                        b += p & 0xff;
                        n++;
                    }
                }
                B[y * sw + x] = 0xff000000u | ((Uint32)(r / n) << 16)
                              | ((Uint32)(g2 / n) << 8) | (Uint32)(b / n);
            }
        }
        Uint32 *t = A; A = B; B = t;      /* 结果换到 A（就地会读到已改的像素） */
    }

    /* --- 3) 双线性放大回原尺寸，叠 tint，按圆角写 alpha --- */
    const int tr = (tint >> 16) & 0xff, tg = (tint >> 8) & 0xff, tb = tint & 0xff;
    const int base_a = 238;               /* 面板整体留一点通透感 */
    for (int y = 0; y < h; y++) {
        double fy = ((double)y + 0.5) / DS - 0.5;
        int y0 = (int)floor(fy);
        double wy = fy - y0;
        if (y0 < 0) { y0 = 0; wy = 0; }
        int y1 = y0 + 1;
        if (y1 >= sh) y1 = sh - 1;
        const Uint32 *r0 = A + (size_t)y0 * sw;
        const Uint32 *r1 = A + (size_t)y1 * sw;

        for (int x = 0; x < w; x++) {
            int a = round_alpha(x, y, w, h, rad);
            if (!a) { out[(size_t)y * w + x] = 0; continue; }

            double fx = ((double)x + 0.5) / DS - 0.5;
            int x0 = (int)floor(fx);
            double wx = fx - x0;
            if (x0 < 0) { x0 = 0; wx = 0; }
            int x1 = x0 + 1;
            if (x1 >= sw) x1 = sw - 1;

            Uint32 p00 = r0[x0], p10 = r0[x1], p01 = r1[x0], p11 = r1[x1];
            double w00 = (1 - wx) * (1 - wy), w10 = wx * (1 - wy);
            double w01 = (1 - wx) * wy,       w11 = wx * wy;
            int br = (int)(((p00 >> 16) & 0xff) * w00 + ((p10 >> 16) & 0xff) * w10
                         + ((p01 >> 16) & 0xff) * w01 + ((p11 >> 16) & 0xff) * w11);
            int bg = (int)(((p00 >>  8) & 0xff) * w00 + ((p10 >>  8) & 0xff) * w10
                         + ((p01 >>  8) & 0xff) * w01 + ((p11 >>  8) & 0xff) * w11);
            int bb = (int)(( p00        & 0xff) * w00 + ( p10        & 0xff) * w10
                         + ( p01        & 0xff) * w01 + ( p11        & 0xff) * w11);

            int orr = (br * (255 - tint_a) + tr * tint_a) / 255;
            int ogg = (bg * (255 - tint_a) + tg * tint_a) / 255;
            int obb = (bb * (255 - tint_a) + tb * tint_a) / 255;
            int al  = a * base_a / 255;
            out[(size_t)y * w + x] = ((Uint32)al << 24)
                                   | ((Uint32)orr << 16)
                                   | ((Uint32)ogg << 8) | (Uint32)obb;
        }
    }

    free(src); free(B);

    if (!g_glass_tex || g_glass_w != w || g_glass_h != h) {
        if (g_glass_tex) SDL_DestroyTexture(g_glass_tex);
        g_glass_tex = SDL_CreateTexture(REN, SDL_PIXELFORMAT_ARGB8888,
                                        SDL_TEXTUREACCESS_STREAMING, w, h);
        g_glass_w = w;
        g_glass_h = h;
    }
    if (!g_glass_tex) {
        free(A); free(out);
        draw_round(REN, rc, rad, tint, 255);
        return;
    }
    SDL_UpdateTexture(g_glass_tex, NULL, out, w * 4);
    SDL_SetTextureBlendMode(g_glass_tex, SDL_BLENDMODE_BLEND);
    SDL_RenderCopy(REN, g_glass_tex, NULL, &rc);

    free(A); free(out);
    g_glass_valid = 1;
}

/* 开关胶囊：底槽 + 滑块。开=绿槽白点，关=灰槽暗点。
   两种状态的尺寸一样，用圆角矩形拼就行（r = h/2 就是圆端），
   不必为它走 round_tex —— 那套是按 (w,h,r) 缓存贴图的，为两个状态
   各烘一张不值得。 */
static void draw_toggle(int x, int y, int w, int h, int on)
{
    int pd = h / 9;
    if (pd < 2) pd = 2;
    SDL_Rect track = { x, y, w, h };
    draw_round(REN, track, h / 2, on ? C_OK : C_LINE2, 255);
    int d = h - pd * 2;
    SDL_Rect knob = { on ? (x + w - pd - d) : (x + pd), y + pd, d, d };
    draw_round(REN, knob, d / 2, on ? 0xffffff : C_SUBTEXT, 255);
}

/* 密码弹层：压暗全屏 + 居中面板（标题/输入框/键盘/提示）。
   盖在设置页之上，所以由 draw_settings_page() 最后调。 */
static void draw_kb_layer(void)
{
    if (!g_kb_on) return;

    setc(REN, 0x000000, 168);
    SDL_Rect full = { 0, 0, g_win_w, g_win_h };
    SDL_RenderFillRect(REN, &full);

    int kcell = S(66), kh = S(KB2_KEY_H), gap = S(KB2_GAP), pad = S(26);
    int gw = KB2_COLS * kcell + (KB2_COLS - 1) * gap;
    int gh = KB2_ROWS * kh + (KB2_ROWS - 1) * gap;
    int title_h = (int)(FS_UI(SET_NAME_FS) * 1.45);
    int field_h = S(52);
    int foot_h2 = (int)(FS_UI(SET_FOOT_FS) * 1.4);
    int pw = gw + pad * 2;
    int ph = pad + title_h + S(10) + field_h + S(16) + gh + S(12) + foot_h2 + pad;
    int px = (g_win_w - pw) / 2;
    int py = (g_win_h - ph) / 2;
    if (px < S(10) || py < S(10)) return;          /* 屏太小就别画了 */

    SDL_Rect pan = { px, py, pw, ph };
    round_box(REN, pan, S(SET_RAD), C_PANEL, C_LINE2, S(2));

    int tw = 0, th = 0;
    char t[128];
    snprintf(t, sizeof(t), "连接到 %s", g_kb_ssid);
    SDL_Texture *tt = bake_cached(REN, F_SETN, t, C_ACCENT2, &tw, &th);
    blit(REN, tt, px + pad, py + pad, tw, th);

    /* 输入框：明文（掌机上只有自己看，能核对比打成圆点重要） */
    SDL_Rect fld = { px + pad, py + pad + title_h + S(10), gw, field_h };
    draw_round(REN, fld, S(12), C_BG2, 255);
    int fx = fld.x + S(18);
    if (g_kb_buf[0]) {
        SDL_Texture *tb = bake_cached(REN, F_SETV, g_kb_buf, C_TEXT, &tw, &th);
        blit(REN, tb, fx, fld.y + (field_h - th) / 2, tw, th);
        fx += tw;
    }
    /* 光标 */
    setc(REN, C_ACCENT, 255);
    SDL_Rect cur = { fx + S(3), fld.y + (field_h - S(20)) / 2, S(2), S(20) };
    SDL_RenderFillRect(REN, &cur);

    /* 键位 */
    int gy = fld.y + field_h + S(16);
    for (int i = 0; i < KB2_N; i++) {
        int r = i / KB2_COLS, c = i % KB2_COLS;
        SDL_Rect k = { px + pad + c * (kcell + gap), gy + r * (kh + gap), kcell, kh };
        int sel = (i == g_kb_sel);

        const char *lab = KB2_KEYS[i];
        char low[4];
        if (!g_kb_shift && lab[1] == '\0' && lab[0] >= 'A' && lab[0] <= 'Z') {
            low[0] = (char)(lab[0] - 'A' + 'a');
            low[1] = '\0';
            lab = low;
        }

        int special = (i == KB2_I_SHIFT || i == KB2_I_DEL || i == KB2_I_OK);
        Uint32 bg = sel ? C_ACCENT
                   : (i == KB2_I_OK) ? 0x1d4b3a
                   : special ? 0x1b2440 : C_PANEL2;
        draw_round(REN, k, S(KB2_RAD), bg, 255);

        /* SHIFT 键的"当前是不是大写"用底色/字色表达，键面字一直是 SHIFT */
        Uint32 fc = (i == KB2_I_OK) ? 0x9df5c8
                  : sel ? 0xffffff
                  : (i == KB2_I_SHIFT && g_kb_shift) ? C_GOLD : C_TEXT;
        SDL_Texture *tk = bake_cached(REN, F_SKEY, lab, fc, &tw, &th);
        blit(REN, tk, k.x + (kcell - tw) / 2, k.y + (kh - th) / 2, tw, th);
    }

    char hint[128];
    snprintf(hint, sizeof(hint), "A 输入 · B 取消 · 现在是%s",
             g_kb_shift ? "大写" : "小写");
    SDL_Texture *th2 = bake_cached(REN, F_GINFO, hint, C_DIM, &tw, &th);
    blit(REN, th2, px + pad, py + ph - pad - th, tw, th);
}

static void draw_settings_page(int clip_y1)
{
    const SMenu *m = cur_menu();
    if (m->n <= 0) return;

    /* 底栏拿这个决定要不要显示「←→ 调值」（本函数比 draw_padbar 先跑） */
    g_set_has_adj = 0;
    for (int i = 0; i < m->n; i++) {
        int t = m->items[i].type;
        if (t == ST_VAL || t == ST_TOGGLE) { g_set_has_adj = 1; break; }
    }

    const int x0 = S(SET_SHELL_X), x1 = g_win_w - S(SET_SHELL_X);
    const int w  = x1 - x0;
    const int head_h = S(SET_HEAD_PY) * 2 + (int)(FS_UI(SET_CRUMB_FS) * 1.3);
    const int foot_h = S(SET_FOOT_PY) * 2 + (int)(FS_UI(SET_FOOT_FS) * 1.4);
    const int row_h  = FS_UI(SET_ROW_H);

    /* 全屏：铺满整个内容区（主页顶栏被设置页盖住；底部那一行按键提示
       仍然露出来 —— 那是底栏，不在本函数的范围内）。
       原先的"按内容自适应高度 + 垂直居中"已废弃：用户要的是全屏。 */
    int y0  = S(SET_SHELL_Y);
    /* hgt     = 内容区高度。下界 clip_y1 是底栏按键条的上沿，
                 行数/滚动都按它算，别动。
       shell_h = 玻璃面板的高度，要**铺到屏幕最底**。
                 面板只铺到 clip_y1 的话，底下那一条露出的是"主页背景 +
                 压暗层"，在浅色主题下就是一条很显眼的灰带
                 （2026-09-14 截图核对时发现）。底栏按键条本来就画在
                 最上层，压上去没有任何问题。 */
    int hgt = clip_y1 - y0;
    if (w < S(240) || hgt < S(160)) return;
    int y1 = y0 + hgt;
    int shell_h = g_win_h - y0;

    /* shell：毛玻璃面板。全屏后整屏都是面板，背景的"主页"整块透上来
       （主页那个 09:32 时间会糊成一块白斑停在右上角，很脏），所以 tint
       权重提到 205 —— 面板接近实心，只留一点玻璃的透色感。
       阴影/描边也去掉了：贴边留着只会是一圈脏边。 */
    SDL_Rect shell = { x0, y0, w, shell_h };
    glass_panel(shell, S(SET_RAD), C_PANEL, 205);

    /* --- 头部：面包屑 + 右侧按键提示。
       ⚠ 这里和底部一样改成**半透明** —— 它们是玻璃面板上的一层，
       填 255 会把刚做好的毛玻璃整块盖死。 */
    SDL_Rect hd = { x0 + S(1), y0 + S(1), w - S(2), head_h };
    draw_round(REN, hd, S(SET_RAD), C_PANEL2, 168);
    /* 顶面高光：一道很淡的白，模拟玻璃边缘的反光 */
    SDL_Rect hi = { x0 + S(SET_RAD), y0 + S(2), w - 2 * S(SET_RAD), S(2) };
    setc(REN, 0xffffff, 30);
    SDL_RenderFillRect(REN, &hi);
    setc(REN, C_LINE, 255);
    SDL_RenderDrawLine(REN, x0 + S(1), y0 + S(1) + head_h, x1 - S(1), y0 + S(1) + head_h);

    int tw = 0, th = 0;
    SDL_Texture *t = bake_cached(REN, F_STITLE, m->title, C_ACCENT2, &tw, &th);
    blit(REN, t, x0 + S(SET_HEAD_PX), y0 + S(1) + (head_h - th) / 2, tw, th);

    /* 顶部提示也跟着底栏一起动 —— 信息页/根菜单里没有可调项，
       再写「←→ 调值」就是假的（跟当初那条假底栏一个毛病）。 */
    const char *tip = (g_setmenu == M_THEME)
                    ? "A 应用（界面会重启）/ B 返回"
                    : (g_set_has_adj ? "A 确认 / ←→ 调值 / B 返回"
                                     : "A 确认 / B 返回");
    t = bake_cached(REN, F_SETF, tip, C_DIM, &tw, &th);
    blit_r(REN, t, x1 - S(SET_HEAD_PX), y0 + S(1) + (head_h - th) / 2, tw, th);

    /* --- 内容区 --- */
    int body_y = y0 + S(1) + head_h;
    int body_h = hgt - head_h - foot_h - S(2);
    int view_rows = body_h / row_h;
    if (view_rows < 1) view_rows = 1;

    /* 把选中行滚进可视区。行高固定，所以这步在绘制时算最省事
       （不需要在每次移动时都去问"一屏能放几行"）。 */
    if (g_setrow < g_settop) g_settop = g_setrow;
    if (g_setrow >= g_settop + view_rows) g_settop = g_setrow - view_rows + 1;
    int max_top = m->n - view_rows;
    if (max_top < 0) max_top = 0;
    if (g_settop > max_top) g_settop = max_top;
    if (g_settop < 0) g_settop = 0;

    for (int i = g_settop; i < m->n && i < g_settop + view_rows; i++) {
        const SItem *it = &m->items[i];
        int ry = body_y + (i - g_settop) * row_h;
        SDL_Rect row = { x0 + S(18), ry, w - S(36), row_h - S(8) };
        int sel = (i == g_setrow);

        /* 选中行：网页版是 90deg 的 accent→#3a6fd6 横向渐变。
           这里用**单色** accent —— 这种宽度的横向渐变得单独烘一张纹理，
           为一行高亮不值得；而用"右半压一个矩形"去近似会把右端圆角
           盖成方角（试过，很难看）。 */
        if (sel)
            draw_round(REN, row, S(SET_ROW_RAD), C_ACCENT, 255);

        int tx = row.x + S(SET_ROW_PX);
        int cy = row.y + row.h / 2;

        /* 行名只上中文（用户 2026-09-14：「选项改为中文」+「字体加大」）。
           SItem.name 还留着 —— 调试 printf 与索引构建仍在用它，只是不上屏。 */
        const char *label = (it->cn && *it->cn) ? it->cn : it->name;
        Uint32 name_col = sel ? 0xffffff : (it->type == ST_RO ? C_SUBTEXT : C_TEXT);
        SDL_Texture *t1 = bake_cached(REN, F_SETN, label, name_col, &tw, &th);
        blit(REN, t1, tx, cy - th / 2, tw, th);
        int used = tx + tw;                     /* 左半已经占到的右边界 */

        /* --- 右侧的值 ---
           ⚠ 必须**限宽**。信息页里有 IP 列表、设备型号、ROM 路径这种长值，
           不限的话它会一路向左压到名字上，两行字叠成一团（真机上第一眼
           就是这个）。算完可用宽度交给 ellipsize() 按 UTF-8 边界收尾，
           不会把一个汉字切成半个。 */
        char val[192];
        const char *valp = val;      /* 可能被下面的 ‹ › 装饰版替换 */
        Uint32 val_col = sel ? 0xffffff : C_ACCENT2;
        const int is_toggle = (it->type == ST_TOGGLE);
        /* 开关状态在这里问一次就够（每次都是一次 nmcli / systemctl 调用），
           下面画胶囊还要用，别问两遍。 */
        const int tog_st = is_toggle ? sv_toggle_state(it->src) : 0;

        if (it->val) {
            /* 动态行自带值文本（WiFi 的"72% · 已保存"） */
            snprintf(val, sizeof(val), "%s", it->val);
            if (it->type == ST_RO) val_col = sel ? 0xffffff : C_SUBTEXT;
        } else if (is_toggle) {
            snprintf(val, sizeof(val), "%s",
                     tog_st < 0 ? "不可用" : (tog_st ? "已开启" : "已关闭"));
            val_col = sel ? 0xffffff : (tog_st ? C_OK : C_SUBTEXT);
        } else if (it->type == ST_MENU) {
            snprintf(val, sizeof(val), "›");
            val_col = sel ? 0xffffff : C_DIM;
        } else if (it->type == ST_BUTTON) {
            if (it->action == SA_SET_THEME) {
                /* 主题行：右侧直接标出"是不是正在用的这套" */
                int cur = (it->arg && strcmp(it->arg, g_theme_id) == 0);
                snprintf(val, sizeof(val), "%s", cur ? "● 使用中" : "A 应用");
                val_col = sel ? 0xffffff : (cur ? C_OK : C_ACCENT2);
            } else {
                /* 按钮右侧的动词按动作给 —— 全是"执行"的话，
                   "安装金手指"看着像要执行什么危险东西。 */
                const char *verb = "执行";
                if (it->action == SA_INSTALL_CHEATS) verb = "安装";
                else if (it->action == SA_REMOVE_CHEATS) verb = "清除";
                snprintf(val, sizeof(val), "%s",
                         (sel && g_setedit) ? "再按 A 确认" : verb);
                if (sel && g_setedit) val_col = 0xffd166;   /* .s-val.editing 的黄 */
            }
        } else {
            sv_text(it->src, val, sizeof(val));
            if (it->type == ST_RO) val_col = sel ? 0xffffff : C_SUBTEXT;
            /* 可调项：选中时两侧加 ‹ ›，一眼看出"这个能左右调" */
            if (sel && it->type != ST_RO) {
                static char dec[224];    /* ≥ 192 + 4，装得下装饰后的整串 */
                snprintf(dec, sizeof(dec), "‹ %s ›", val);
                valp = dec;
            }
        }

        /* 开关行：胶囊占值区最右端，状态词（已开启/已关闭）排在它左边 */
        int right = row.x + row.w - S(SET_ROW_PX);
        if (is_toggle) {
            int sw2 = S(66), sh2 = S(34);
            draw_toggle(right - sw2, cy - sh2 / 2, sw2, sh2, tog_st == 1);
            right -= sw2 + S(14);
        }

        int val_max = right - used - S(24);
        if (val_max < S(90)) val_max = S(90);
        char valfit[224];
        ellipsize(F_SETV, valp, val_max, valfit, sizeof(valfit));

        SDL_Texture *tv = bake_cached(REN, F_SETV, valfit, val_col, &tw, &th);
        blit_r(REN, tv, right, cy - th / 2, tw, th);
    }

    /* 条目比屏幕还多时的溢出提示（这台机器上装不下，留个保险） */
    if (m->n > view_rows) {
        char cnt[48];
        snprintf(cnt, sizeof(cnt), "%d/%d", g_setrow + 1, m->n);
        SDL_Texture *tc = bake_cached(REN, F_SETF, cnt, C_DIM, &tw, &th);
        blit_r(REN, tc, x1 - S(SET_HEAD_PX), body_y + S(2), tw, th);
    }

    /* --- 底部：优先显示回执，没有就显示当前项的说明 --- */
    SDL_Rect ft = { x0 + S(1), y1 - foot_h - S(1), w - S(2), foot_h };
    draw_round(REN, ft, S(SET_RAD), C_PANEL2, 168);      /* 同头部：半透明 */
    setc(REN, C_LINE, 255);
    SDL_RenderDrawLine(REN, x0 + S(1), ft.y, x1 - S(1), ft.y);

    const SItem *cur = (g_setrow >= 0 && g_setrow < m->n) ? &m->items[g_setrow] : NULL;
    const char *line = g_flash[0] ? g_flash
                     : (cur && cur->hint ? cur->hint : (cur ? cur->cn : ""));
    if (line && *line) {
        Uint32 fc = g_flash[0] ? C_ACCENT2 : C_DIM;
        /* 也限宽 —— WiFi 那几行的 hint 偏长，不限会顶出面板右边界 */
        int hint_max = (x1 - S(SET_HEAD_PX)) - (x0 + S(SET_HEAD_PX));
        char hfit[256];
        ellipsize(F_SETF, line, hint_max, hfit, sizeof(hfit));
        SDL_Texture *tf = bake_cached(REN, F_SETF, hfit, fc, &tw, &th);
        blit(REN, tf, x0 + S(SET_HEAD_PX), ft.y + (foot_h - th) / 2, tw, th);
    }

    /* 密码弹层盖在最上面（它自己会把整屏压暗） */
    draw_kb_layer();
}


/* ==========================================================================
 *  5.8 快捷键说明页
 * ==========================================================================
 *  用户 2026-09-16："快捷键说明书做到设置里面  打开就可以看" →
 *                   "在 C 里做一个可滚动的说明页"。
 *
 *  为什么是**纯文本页**而不是塞一张 HTML/PNG：vgfront 是 SDL2 手写渲染，
 *  没有网页引擎；走图片的话要引 SDL_image、多一条资源路径、中英还得两套图。
 *  这一页跟着 g_uilang（系统语言）自动切中英，固件里一个文件都不多。
 *
 *  ★ 这一页的内容必须跟**真源**对得上，不能照记忆写：
 *    前端那几张表来自 vgfront.c 自己的 handle_key / handle_pad；
 *    游戏里那几张来自设备上 /home/ark/.config/retroarch{32}/retroarch.cfg
 *    的 input_* 绑定（2026-09-16 从真机导出核对过）。
 *    改了任何一边的键位，这一页要跟着改 —— 否则就变成"底栏写着 SEL 分类
 *    其实 SEL 是退出"那种假说明书，比没有还坏。
 *
 *  机身那颗键：**丝印是 M**，SDL 里叫 guide、编号 10，RA 配置里它叫
 *    input_enable_hotkey_btn。三个说法指同一个键（用户 2026-09-16：
 *    "实体按键写的是M"）。给人看的文案一律写 M。
 */

#define HELP_ROW_H    62      /* 行高（设计值）≈31px */
#define HELP_KEYCOLW  250     /* 键位栏宽（设计值）≈125px */

/* 四列：键位中文 / 键位英文 / 说明中文 / 说明英文。
   k_zh 为空 = 这一行是**小节标题**（整行铺开，前面一条 accent 竖条）。 */
typedef struct {
    const char *kzh, *ken;
    const char *dzh, *den;
} HelpRow;

static const HelpRow HELP_ROWS[] = {
    { NULL, NULL, "前端界面", "Frontend" },
    { "方向键", "D-Pad",  "移动光标、选择项目",           "Move cursor, select item" },
    { "A", "A",           "确认：进平台、启动游戏、执行",  "Confirm: open system, launch game, run" },
    { "B", "B",           "返回上一级",                   "Back one level" },
    { "Y", "Y",           "打开搜索（只有主页响应）",      "Open search (home screen only)" },
    { "L1 / R1", "L1 / R1", "顶栏分类左右切换（主页）",    "Switch top-bar category (home)" },
    { "START", "START",   "主页开设置；搜索页清空；设置页返回",
                          "Home: open settings. Search: clear. Settings: back" },
    { "SELECT", "SELECT", "返回（设置页 / 搜索页）",       "Back (settings / search)" },

    { NULL, NULL, "游戏操作弹层", "Game action popup" },
    { "方向键", "D-Pad",  "上下选择项目",                 "Select item" },
    { "A", "A",           "确认（删除游戏要按两次）",      "Confirm (delete needs two presses)" },
    { "B", "B",           "关闭弹层",                     "Close popup" },
    { "← →", "← →",       "只有「更换核心」的列表里用：切换适用范围",
                          "Only inside the core list: switch scope" },

    { NULL, NULL, "游戏里（RetroArch）", "In-game (RetroArch)" },
    { "M", "M",           "热键门禁 —— 下面每一条都要先按住它",
                          "Hotkey gate — hold this first for every combo below" },
    { "M + A", "M + A",   "暂停 / 继续",                  "Pause / resume" },
    { "M + B", "M + B",   "重置游戏（不退出）",            "Reset game (no exit)" },
    { "M + START", "M + START", "退出游戏，回到前端",       "Exit game, back to frontend" },
    { "M + X", "M + X",   "打开 RetroArch 菜单",          "Open RetroArch menu" },
    { "M + R1", "M + R1", "保存即时存档",                 "Save state" },
    { "M + L1", "M + L1", "读取即时存档",                 "Load state" },
    /* ⚠ 2026-09-16 用户要求把 L2 / R2 让给快进，亮度挪到十字键上/下。
       改的是设备上两份 retroarch.cfg（前端一个 input_* 字节都不写）：
         input_hold_fast_forward_btn   = "6"   → M + L2 按住快进
         input_toggle_fast_forward_btn = "7"   → M + R2 按一下锁住快进
         input_brightness_up_btn       = "13"  → M + 十字键上
         input_brightness_down_btn     = "14"  → M + 十字键下
       门禁 input_enable_hotkey_btn 保持出厂值 "10"（M）—— 所以上面那几条
       M + A/B/X/START/L1/R1 一个都没变。
       ⚠ 这条表是给人看的"说明书"，改 cfg 的键位时**必须同步改这里**，
         否则就变成"底栏写着 SEL 分类、其实 SEL 是退出"那种假说明书。 */
    { "M + L2 / R2", "M + L2 / R2", "快进：L2 按住不放 / R2 按一下锁住",
                          "Fast-forward: L2 hold / R2 tap to lock" },
    { "M + ↑ / ↓", "M + ↑ / ↓", "屏幕亮度 + / −",          "Brightness + / −" },
    { "L3 + R3", "L3 + R3", "备用开菜单组合",              "Alternative: open menu" },
    { "音量", "Volume",   "用机身音量键，RA 里没绑手柄音量",
                          "Use the side volume keys — RA has none bound" },

    /* ---- 独立模拟器（2026-09-17 用户："把我们独立模拟器的快捷键也写进设置说明书"）
       全段真源（都是从设备上扒的，不是照记忆写）：
         · "M + START 退出" = /usr/local/bin/killer_daemon.py 的 JOY_MAPPINGS
           `"retrogame_joypad": { "f1": 316, "f3": 315 }` —— f1(316=M) 必须**按住**，
           f3(315=START) 一按就 `pkill <VAR>*`；VAR 由各包装脚本写进
           /home/ark/.config/KILLIT。挂了这个的共 15+ 个：drastic / yaba(SS) /
           PPSSPPSDL(PSP) / flycast(DC·NAOMI·AW) / gzdoom·lzdoom / scummvm /
           solarus / ecwolf / hypseus(Daphne·Singe) / bigpemu / piemu / ti99sim /
           GameTankEmulator。所以它是**全设备统一**的退出键。
         · NDS 那四条 = /opt/advdrastic/advanced_drastic/config/drastic.cfg：
             HOT(GATE)         = 1032 → SDL 8  = SELECT
             MENU              = 1034 → SDL 10 = M
             HOT_SAVE_STATE    = 1029 → SDL 5  = R1
             HOT_LOAD_STATE    = 1028 → SDL 4  = L1
             HOT_TOGGLE_FAST_FORWARD = 1030 → SDL 6 = L2
           ⚠ SS / PSP 的"呼出模拟器菜单"键**没找到可靠真源**（PPSSPP 的
             ppsspp.ini 里压根没有 [ControlMapping] 段；yabasanshiro 只有
             ESC/Pause 字串），所以不写 —— 假说明书比没有更坏。
       ⚠ 左列宽上限那条"≈11 半角宽"是**按等宽数出来的，会骗人**：这套字体是
         比例字体，"SELECT+R1"（9 个字符）比 "M + L2 / R2"（11 个，但一半是空格）
         还宽，实测直接顶到右列、中间一个像素的空隙都没有（2026-09-17 截图可见）。
         ⇒ **大写字母堆在一起的键名，按 9 个字符就要掂量**；长了就拆成
         "门禁行 + '+ R1' 续行"（跟上面 RetroArch 那段一个写法），别硬塞。 */
    { NULL, NULL, "独立模拟器（NDS / SS / PSP）", "Standalone emulators" },
    { "M + START", "M + START", "退出模拟器，回到前端（各家通用）",
                          "Exit emulator, back to frontend" },
    { "M", "M",           "NDS：呼出 DraStic 菜单（金手指 / 存档 / 画面）",
                          "NDS: open DraStic menu (cheats / states / display)" },
    { "SELECT", "SELECT", "NDS：热键门禁 —— 下面三条都要先按住它",
                          "NDS: hotkey gate — hold it for the rows below" },
    { "+ R1", "+ R1",     "NDS：保存即时存档",     "NDS: save state" },
    { "+ L1", "+ L1",     "NDS：读取即时存档",     "NDS: load state" },
    { "+ L2", "+ L2",     "NDS：快进（按住不放）", "NDS: fast-forward (hold)" },

    /* ---- J2ME（Java 游戏，2026-09-18 用户："Java的按键说明也加到设置快捷键说明书里面吧"）
       真源有**两处，必须一起看**：
         · /opt/j2me-3566/keymap.cfg —— ★★ 它【覆盖】源码里的 KEY_* 常量，
           下面"哪颗键"一律以它为准：左键=X 右键=B OK=A  *=SELECT  #=START
                                     0=Y  1=L1   3=R1   7=L2     9=R2
         · native/j2me-sdl/miniloong.cpp 的 startCapturing()（门禁 M_MOD=10）
       实测（2026-09-18 21:1x，uinput 假手柄 + GUID 锁设备，脚本 pc/req858，34 条记录）：
         304(B)→'w' 右软键 · 305(A)→RETURN 确定 · 307(X)→'q' 左软键 · 308(Y)→'0' 数字 0
         310(L1)→'1' · 311(R1)→'3' · 312(L2)→'7' · 313(R2)→'9' · 544(↑)→UP
         M(316) 单击 → 【无任何输出】；M+START → 退出；M+SELECT → 'c' 切按键模式
         M+Y(308) → 旋转 3 档 ★；M+X(307) → 触屏开关 ★
       ⚠⚠ 曾经把旋转/触屏**记反**成 "M+B 旋转 / M+Y 触屏" —— 真值是
          **M+Y = 旋转、M+X = 触屏**。根因还是老坑：KEY_0 被 cfg 指到 Y、
          KEY_LEFT 指到 X，**别拿 #define 猜**（见文件头那条注意事项）。
       ⚠ M+B / M+A / M+L1… **不是**组合键 —— M 只是门禁，按住它时这些键照常
          发自己的键。全表只有 START / Y / X / SELECT 四个跟 M 有组合。
       ⚠ 改 miniloong.cpp 或 keymap.cfg 的映射，这一段必须同步改。 */
    { NULL, NULL, "J2ME（Java 游戏）", "J2ME (Java games)" },
    { "方向键", "D-Pad",          "移动 / 菜单选择",       "Move / menu select" },
    { "A", "A",                   "确定（OK 键）",         "Confirm (OK)" },
    { "B", "B",                   "右软键",                "Right soft key" },
    { "X", "X",                   "左软键",                "Left soft key" },
    { "Y", "Y",                   "数字键 0",              "Number 0" },
    { "L1 / R1", "L1 / R1",       "数字键 1 / 3",          "Numbers 1 / 3" },
    { "L2 / R2", "L2 / R2",       "数字键 7 / 9",          "Numbers 7 / 9" },
    /* ⚠ 说明列**不要用 markdown 反引号** —— 这个渲染器原样打出来，
       2026-09-18 实测截图里显示成 "`*` 星号键"，很难看。 */
    { "SELECT", "SELECT",         "* 星号键",              "* key" },
    { "START", "START",           "# 井号键",              "# key" },
    { "M", "M",                   "组合门禁（单击不做任何事）",
                                  "Combo gate (single press: nothing)" },
    { "M + START", "M + START",   "退出游戏，回到前端",     "Exit game, back to frontend" },
    { "M + Y", "M + Y",           "旋转画面（3 档循环）",   "Rotate screen (3 steps)" },
    { "M + X", "M + X",           "触屏模式开关",           "Toggle touch-screen mode" },
    /* ⚠ "M + SELECT" 10 个字符放不下 —— 2026-09-18 实测被 ellipsize 成
       "M + SELE…"。去掉两个空格（8 个字符）正好跟 "M + START" 等宽，
       别再加空格、也别改成全角。 */
    { "M+SELECT", "M+SELECT",     "循环切换按键模式",       "Cycle key layout" },

    { NULL, NULL, "系统热键（任何界面都生效）", "System hotkeys (any screen)" },
    { "电源键 短按", "Power (short)", "进入待机；按任意键唤醒", "Sleep; press any key to wake" },
    { "电源键 长按", "Power (long)",  "关机（先播关机动画）",   "Power off (plays shutdown animation)" },
    { "音量 + / −", "Volume + / −",  "系统主音量",           "System volume" },
    { "自动待机", "Auto sleep", "闲置到「显示设置 → 息屏时间」设定的时长后自动进入（可调、可关）",
                          "Auto sleep after the idle time set in 息屏时间 (Display settings)" },
};
#define HELP_N ((int)(sizeof(HELP_ROWS) / sizeof(HELP_ROWS[0])))

/* 一屏能放几行。⚠ 必须跟 draw_help_page() 里那道算式**逐项一致** ——
   两处对不上时的症状是"翻页翻过头，最后一行下面留一块空白"或者
   "最后几行永远滚不到"。所以抽出来共用，别各算各的。 */
static int help_view_rows(void)
{
    int head_h = S(SET_HEAD_PY) * 2 + (int)(FS_UI(SET_CRUMB_FS) * 1.3);
    int foot_h = S(SET_FOOT_PY) * 2 + (int)(FS_UI(SET_FOOT_FS) * 1.4);
    int hgt = (g_win_h - padbar_zone()) - S(SET_SHELL_Y);
    int body_h = hgt - head_h - foot_h - S(2);
    int v = body_h / (FS_UI(HELP_ROW_H) ? FS_UI(HELP_ROW_H) : 1);
    return v < 1 ? 1 : v;
}

/* 滚 lines 行（±1 = 一行，±一屏 = 翻页）。正数往下。
   滚不动就不重绘 —— 免得按住 ↓ 到底之后每帧都在刷整屏。 */
static void help_scroll(int lines)
{
    int max_top = HELP_N - help_view_rows();
    if (max_top < 0) max_top = 0;
    int v = g_help_top + lines;
    if (v < 0) v = 0;
    if (v > max_top) v = max_top;
    if (v == g_help_top) return;
    g_help_top = v;
    mark_dirty();
}

static void help_open(void)
{
    g_page = PAGE_HELP;
    g_help_top = 0;
    g_flash[0] = '\0';          /* 别把设置页那条回执带过来 */
    mark_dirty();
    printf("[说明] 打开快捷键说明页（共 %d 行，一屏 %d 行）\n",
           HELP_N, help_view_rows());
    fflush(stdout);
}

/* 从说明页回设置页。**光标不动** —— 它是从设置页某一项按 A 进来的，
   回去还应该停在那项上（跟 settings_back() 从子菜单回根菜单一个手感）。 */
static void help_close(void)
{
    g_page = PAGE_SETTINGS;
    mark_dirty();
}

static void draw_help_page(int clip_y1)
{
    const int x0 = S(SET_SHELL_X), x1 = g_win_w - S(SET_SHELL_X);
    const int w  = x1 - x0;
    const int head_h = S(SET_HEAD_PY) * 2 + (int)(FS_UI(SET_CRUMB_FS) * 1.3);
    const int foot_h = S(SET_FOOT_PY) * 2 + (int)(FS_UI(SET_FOOT_FS) * 1.4);
    const int row_h  = FS_UI(HELP_ROW_H);

    int y0  = S(SET_SHELL_Y);
    int hgt = clip_y1 - y0;
    if (w < S(240) || hgt < S(160)) return;
    int y1 = y0 + hgt;
    int shell_h = g_win_h - y0;         /* 面板铺到屏幕最底，理由同设置页 */

    SDL_Rect shell = { x0, y0, w, shell_h };
    glass_panel(shell, S(SET_RAD), C_PANEL, 205);

    /* --- 头部：标题 + 右侧滚动位置 --- */
    SDL_Rect hd = { x0 + S(1), y0 + S(1), w - S(2), head_h };
    draw_round(REN, hd, S(SET_RAD), C_PANEL2, 168);
    SDL_Rect hi = { x0 + S(SET_RAD), y0 + S(2), w - 2 * S(SET_RAD), S(2) };
    setc(REN, 0xffffff, 30);
    SDL_RenderFillRect(REN, &hi);
    setc(REN, C_LINE, 255);
    SDL_RenderDrawLine(REN, x0 + S(1), y0 + S(1) + head_h, x1 - S(1), y0 + S(1) + head_h);

    int tw = 0, th = 0;
    SDL_Texture *t = bake_cached(REN, F_STITLE, LF("快捷键说明", "Hotkey guide"),
                                 C_ACCENT2, &tw, &th);
    blit(REN, t, x0 + S(SET_HEAD_PX), y0 + S(1) + (head_h - th) / 2, tw, th);

    int body_y = y0 + S(1) + head_h;
    int body_h = hgt - head_h - foot_h - S(2);
    int view_rows = body_h / row_h;
    if (view_rows < 1) view_rows = 1;

    /* 夹一次滚动量。绘制时兜底 —— 按 F1 之类的旁路进来也能保证不露白。 */
    int max_top = HELP_N - view_rows;
    if (max_top < 0) max_top = 0;
    if (g_help_top > max_top) g_help_top = max_top;
    if (g_help_top < 0) g_help_top = 0;

    /* 溢出提示：给的是**行区间**不是页码 —— "第 8-26 行 / 共 29 行"比
       "2/2" 更有用（内容长度一眼看得见）。放得下就不显示。 */
    if (HELP_N > view_rows) {
        int last = g_help_top + view_rows;
        if (last > HELP_N) last = HELP_N;
        char cnt[48];
        snprintf(cnt, sizeof(cnt), "%d-%d/%d", g_help_top + 1, last, HELP_N);
        t = bake_cached(REN, F_SETF, cnt, C_DIM, &tw, &th);
        blit_r(REN, t, x1 - S(SET_HEAD_PX), y0 + S(1) + (head_h - th) / 2, tw, th);
    }

    /* --- 内容 --- */
    const int kx = x0 + S(SET_ROW_PX);
    const int dx = kx + S(HELP_KEYCOLW);
    const int dmax = (x1 - S(SET_ROW_PX)) - dx;

    for (int i = g_help_top; i < HELP_N && i < g_help_top + view_rows; i++) {
        const HelpRow *r = &HELP_ROWS[i];
        int ry = body_y + (i - g_help_top) * row_h;

        if (!r->kzh || !*r->kzh) {
            /* 小节标题：左边一条 accent 竖条 + 标题字 */
            SDL_Rect bar = { kx, ry + S(9), S(6), row_h - S(18) };
            setc(REN, C_ACCENT, 255);
            SDL_RenderFillRect(REN, &bar);
            t = bake_cached(REN, F_SETN, LF(r->dzh, r->den), C_ACCENT, &tw, &th);
            blit(REN, t, kx + S(20), ry + (row_h - th) / 2, tw, th);
            continue;
        }

        /* 键位（左列）。限宽 —— "M + L2 / R2" "电源键 短按" 这种已经是极限，
           不收边会顶到右边说明上。
           ⚠ 列宽 = S(HELP_KEYCOLW) - S(20) ≈ 115px，**最多约 11 个半角宽**
             （中文/全角按 2 个半角算）。加新行请按这个上限写 —— 曾经写过
             "无操作 10 分钟"（14 宽），界面上被截成 "无操作 1…"。
             宁愿缩短键位名（"M + ↑ / ↓"），也别让它被吃掉。 */
        char kfit[80];
        ellipsize(F_SETN, LF(r->kzh, r->ken), S(HELP_KEYCOLW) - S(20),
                  kfit, sizeof(kfit));
        t = bake_cached(REN, F_SETN, kfit, C_ACCENT2, &tw, &th);
        blit(REN, t, kx + S(20), ry + (row_h - th) / 2, tw, th);

        /* 说明（右列） */
        if (dmax > S(80)) {
            char dfit[256];
            ellipsize(F_SETV, LF(r->dzh, r->den), dmax, dfit, sizeof(dfit));
            t = bake_cached(REN, F_SETV, dfit, C_TEXT, &tw, &th);
            blit(REN, t, dx, ry + (row_h - th) / 2, tw, th);
        }
    }

    /* --- 底部：a 最关键的一句话，常驻。这一页别的都可以滚过去，
     *          "M 是哪颗键"不能滚过去 —— 用户第一次翻这一页时，
     *          卡住的地方十有八九就是"哪个是 M"。 --- */
    SDL_Rect ft = { x0 + S(1), y1 - foot_h - S(1), w - S(2), foot_h };
    draw_round(REN, ft, S(SET_RAD), C_PANEL2, 168);
    setc(REN, C_LINE, 255);
    SDL_RenderDrawLine(REN, x0 + S(1), ft.y, x1 - S(1), ft.y);

    const char *footp = LF("M = 机身中间那颗键（热键门禁）；游戏里的组合都要先按住它",
                           "M = the middle key on the shell (hotkey gate); hold it first for any combo");
    char ffit[256];
    ellipsize(F_SETF, footp, (x1 - S(SET_HEAD_PX)) - (x0 + S(SET_HEAD_PX)),
              ffit, sizeof(ffit));
    t = bake_cached(REN, F_SETF, ffit, C_DIM, &tw, &th);
    blit(REN, t, x0 + S(SET_HEAD_PX), ft.y + (foot_h - th) / 2, tw, th);
}


/* ==========================================================================
 *  游戏操作弹层 —— 实现
 * ==========================================================================*/

/* 拼出 ROM 在**兼容树**里的路径：<compat_dir>/<pid>/<rel>。
 *
 * ⚠ 不能拿 <rom_root>/<pid>/<rel> 拼。ARCADE 是"实体目录 + 各子库软链"的
 *   合并目录，卡上那些 ROM 散在 arcade/mame/neogeo/cps* 几个子目录里，
 *   只有兼容树把它们摊平在 <pid>/ 这一层；而且 compat_dir 下面全是软链，
 *   unlink 会顺着链删掉卡上的真文件 —— 正是我们要的。
 *   这个拼法和宿主 do_launch() 里的 os.path.join(cfg["compat_dir"], pid, rel)
 *   **必须一致**，否则会出现"能启动但删不到"。 */
static int act_path_of(Platform *p, int gi, char *out, size_t cap)
{
    if (!p || gi < 0 || gi >= p->ngames) return 0;
    char compat[256] = "/home/ark/.vagaretro-roms";
    conf_get_str(CONF_INI, "compat_dir", compat, sizeof(compat));
    snprintf(out, cap, "%s/%s/%s", compat, p->pid, p->games[gi].rel);
    return 1;
}

static int fav_has(const char *pid, const char *rel)
{
    if (!pid || !rel) return 0;
    FILE *fh = fopen(FAV_PATH, "r");
    if (!fh) return 0;
    char line[800];
    int hit = 0;
    while (fgets(line, sizeof(line), fh)) {
        size_t L = strlen(line);
        while (L && (line[L - 1] == '\n' || line[L - 1] == '\r')) line[--L] = '\0';
        char *t = strchr(line, '\t');
        if (!t) continue;
        *t = '\0';
        if (strcmp(line, pid) == 0 && strcmp(t + 1, rel) == 0) { hit = 1; break; }
    }
    fclose(fh);
    return hit;
}

/* 加 / 去一条收藏，重写整个文件。收藏撑死几十条，不值得搞增量更新。 */
static int fav_set(const char *pid, const char *rel, int on)
{
    if (!pid || !rel) return 0;

    char want[800];
    snprintf(want, sizeof(want), "%s\t%s", pid, rel);

    /* 静态缓冲：64 条 × 800 字节 = 50KB，别放栈上 */
    static char keep[64][800];
    int n = 0;

    FILE *fh = fopen(FAV_PATH, "r");
    if (fh) {
        char line[800];
        while (fgets(line, sizeof(line), fh)) {
            size_t L = strlen(line);
            while (L && (line[L - 1] == '\n' || line[L - 1] == '\r')) line[--L] = '\0';
            if (!line[0]) continue;
            if (strcmp(line, want) == 0) continue;      /* 旧的那条丢掉，避免写出两份 */
            if (n < 64) snprintf(keep[n++], sizeof(keep[0]), "%s", line);
        }
        fclose(fh);
    }
    if (on && n < 64) snprintf(keep[n++], sizeof(keep[0]), "%s", want);

    FILE *w = fopen(FAV_PATH, "w");
    if (!w) return 0;
    for (int i = 0; i < n; i++) fprintf(w, "%s\n", keep[i]);
    fclose(w);
    return 1;
}

/* ---- /opt/vagaretro/autosave.txt：开了「退出自动存档」的平台 ------------
   一行一个 platform.id，支持 # 注释和前后空白。

   粒度是**平台**而不是游戏，也不是全局（用户 2026-09-16 原话：
   "自动存档开关也加到弹层里面吧 针对平台生效 不用加到设置里面"）——
   存档习惯本来就跟着机种走：PS1 那种一次玩半天的和 FC 随手玩两把的，
   需求完全不一样。

   前端只写这个名单，**启动链一行都不用改**：宿主 vagaretro-shell.py 读它，
   翻译成启动期的 appendconfig
       savestate_auto_save = "true"     退出内容时自动存档
       savestate_auto_load = "true"     下次进同一个内容自动读回来
   （两份 retroarch.cfg 都不动 —— 那是玩家自己的设置，而且 32/64 位要同时改，
     改坏了整个模拟器都乱。） */
#define AUTOSAVE_PATH "/opt/vagaretro/autosave.txt"

static int autosave_has(const char *pid)
{
    if (!pid || !pid[0]) return 0;
    FILE *fh = fopen(AUTOSAVE_PATH, "r");
    if (!fh) return 0;
    char line[128];
    int hit = 0;
    while (fgets(line, sizeof(line), fh)) {
        size_t L = strlen(line);
        while (L && (line[L - 1] == '\n' || line[L - 1] == '\r')) line[--L] = '\0';
        const char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == '#') continue;
        if (strcmp(s, pid) == 0) { hit = 1; break; }
    }
    fclose(fh);
    return hit;
}

/* 开 / 关一个平台的自动存档，整体重写。撑死几十行，不值得搞增量更新。 */
static int autosave_set(const char *pid, int on)
{
    if (!pid || !pid[0]) return 0;

    static char keep[96][128];
    int n = 0;
    FILE *fh = fopen(AUTOSAVE_PATH, "r");
    if (fh) {
        char line[128];
        while (fgets(line, sizeof(line), fh)) {
            size_t L = strlen(line);
            while (L && (line[L - 1] == '\n' || line[L - 1] == '\r')) line[--L] = '\0';
            if (!line[0]) continue;
            if (strcmp(line, pid) == 0) continue;      /* 旧那条丢掉，避免写出两份 */
            if (n < 96) snprintf(keep[n++], sizeof(keep[0]), "%s", line);
        }
        fclose(fh);
    }
    if (on && n < 96) snprintf(keep[n++], sizeof(keep[0]), "%s", pid);

    /* 一条不剩（全关了）就把文件删掉 —— 留个空文件会让"开没开过"没法一眼看出 */
    if (n == 0) { remove(AUTOSAVE_PATH); return 1; }

    FILE *w = fopen(AUTOSAVE_PATH, "w");
    if (!w) return 0;
    for (int i = 0; i < n; i++) fprintf(w, "%s\n", keep[i]);
    fclose(w);
    return 1;
}

/* ---- /opt/vagaretro/cheats.txt：金手指开关（用户 2026-09-18）-------------
 * 用户原话："启动弹层能加个金手指选项吗  自动找到咱们系统里面自带的对应的
 *           ra 金手指文件  金手指功能的开关可以做到二级页面"
 *
 * 一行一条，TAB 分隔（和 cores.txt / favorites.txt 同款）：
 *     <平台id>\t<rom 文件名>\t<0|1>     游戏级
 *     <平台id>\t*\t<0|1>                平台级（rom 名写 *）
 * 游戏级优先，没有再看平台级，都没有 = 关。
 *
 * ★ 前端**只管写这一个文件**。真正的活儿 —— 把匹配到的 cht 复制进隔离目录、
 *   把 cheatN_enable 翻成 true、给 RA 挂 --appendconfig（apply_cheats_after_load
 *   + cheat_database_path）—— 全在宿主 vagaretro-shell.py 里。那边有 os/re，
 *   做中文名归一化比在 C 里手搓稳妥，而且**两边各写一套匹配规则迟早会漂**，
 *   所以规则以宿主那份为准。这里的 cheat_scan() 只负责"让玩家看见找到了哪一份、
 *   有几条金手指"，不参与实际加载。
 * ★ 金手指库（/home/ark/.config/retroarch/cheats/中文金手指/）是**只读**的。
 *   RA 退出时会打 `[Cheats] Save game-specific cheatfile:`，也就是**把内存里的
 *   金手指写回文件**；直接给它软链等于改写源库（2026-09-18 实测踩到：一个
 *   642 字节的简写 cht 被重写成 4127 字节的全字段格式）。宿主那边是"复制到
 *   /opt/vagaretro/cheats-auto 再让 RA 读那一份"，源库一字不动。
 * ------------------------------------------------------------------------- */
#define CHEATS_PATH   "/opt/vagaretro/cheats.txt"
#define CHEAT_LIB_DIR "/home/ark/.config/retroarch/cheats/中文金手指"

/* 条目级开关（用户 2026-09-18 晚追加："金手指具体什么功能的开关没显示"）。
 *   二级列表 = 行 0 适用范围 / 行 1 总开关 / 行 2.. 每条具体功能。
 * ★ 库里的 cht 规模非常野（实测 2152 个）：62% ≤ 9 条，但 101 个 > 255 条，
 *   最大的 大航海时代2PS.cht 有 **23107 条**（3.2 MB / 9.2 万行）。
 *   界面只展示前 CHEAT_MAX 条，超出的由宿主按"默认开"处理（等于老行为）。 */
#define CHEAT_MAX      192
#define CHEAT_DESC_CAP 120

static int  g_act_cheat   = 0;    /* 二级列表开着 */
static int  g_act_chsel   = 0;    /* 光标行：0 适用范围 / 1 总开关 / 2.. 条目 */
static int  g_act_chtop   = 0;    /* 滚动首行 */
static int  g_act_chscope = 0;    /* 0 = 仅本游戏，1 = 整个平台 */
static int  g_act_chon    = 0;    /* 当前这一级的开关值 */
static int  g_act_chlib   = 0;    /* 库里找到匹配的 cht 了吗 */
static int  g_act_chcount = 0;    /* 那一份里**真实**有几条（可能 > CHEAT_MAX） */
static int  g_act_chvis   = 0;    /* 展示条数 = min(chcount, CHEAT_MAX) */
static char g_act_chfile[128];    /* 匹配到的 cht 文件名（空 = 没找到） */
static char g_act_chdesc[CHEAT_MAX][CHEAT_DESC_CAP];  /* 条目名（解析时已截断） */
static char g_act_chbits[CHEAT_MAX + 2];              /* 每位 '0'/'1'，长度 = chvis */

/* 二级列表的总行数：2 个固定行 + 条目 */
static int cheat_nrow(void) { return 2 + g_act_chvis; }

/* 名字归一化。规则**必须和宿主 vagaretro-shell.py 的 _cht_norm() 一致**：
 *     "1943.zip" / "1943 (J)_ra.cht" / "1943（日版）_ra.cht"  ->  "1943"
 * 顺序：去扩展名 -> 去 "_ra" 尾巴 -> 去括号内容 -> 去空白连接符 -> 转小写。
 * ⚠ 中文按 UTF-8 字节处理，只对 ASCII 转小写 —— 不能用 tolower()，那玩意按
 *   当前 locale 解释单字节，中文会被啃掉。 */
static void cheat_norm(const char *in, char *out, size_t cap)
{
    if (!out || !cap) return;
    out[0] = '\0';
    if (!in || !in[0]) return;

    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s", in);

    char *dot = strrchr(tmp, '.');                    /* ① 去扩展名 */
    if (dot && dot != tmp) *dot = '\0';

    size_t L = strlen(tmp);                           /* ② 去 "_ra" */
    if (L >= 3 && tmp[L - 3] == '_' &&
        (tmp[L - 2] == 'r' || tmp[L - 2] == 'R') &&
        (tmp[L - 1] == 'a' || tmp[L - 1] == 'A'))
        tmp[L - 3] = '\0';

    size_t tlen = strlen(tmp);
    size_t j = 0;
    int depth = 0;                                    /* ③ 去括号（含括号里的内容） */
    for (size_t i = 0; i < tlen && j + 1 < cap; ) {
        unsigned char c  = (unsigned char)tmp[i];
        unsigned char c1 = (i + 1 < tlen) ? (unsigned char)tmp[i + 1] : 0;
        unsigned char c2 = (i + 2 < tlen) ? (unsigned char)tmp[i + 2] : 0;
        int adv = 1;

        if (c == '(' || c == '[' || c == '{') {
            depth++;
        } else if (c == ')' || c == ']' || c == '}') {
            if (depth) depth--;
        } else if (c == 0xEF && c1 == 0xBC && (c2 == 0x88 || c2 == 0x89)) {
            if (c2 == 0x88) depth++;                  /* 全角 （ U+FF08 */
            else if (depth) depth--;                  /* 全角 ） U+FF09 */
            adv = 3;
        } else if (c == 0xE3 && c1 == 0x80 && (c2 == 0x90 || c2 == 0x91)) {
            if (c2 == 0x90) depth++;                  /* 【 U+3010 */
            else if (depth) depth--;                  /* 】 U+3011 */
            adv = 3;
        } else if (c == ' ' || c == '\t' || c == '_' || c == '-' || c == '\'') {
            /* ④ 直接吞掉，不落 out */
        } else if (c == 0xC2 && c1 == 0xB7) {
            adv = 2;                                  /* · U+00B7 */
        } else if (!depth) {
            out[j++] = (char)((c >= 'A' && c <= 'Z') ? (c + 32) : c);
        }
        i += adv;
    }
    out[j] = '\0';
}

/* 「版本修饰词」白名单 —— 反向命中的安全阀。
 * ROM 名 = cht 名 + 这些词 时，认作同一款游戏、共用同一份金手指。
 *
 * ★★ 为什么**必须**白名单，不能裸用"谁长听谁的"（2026-09-18 实测）：
 *   裸反向匹配会出安全事故 —— cht `忍_ra.cht`(norm="忍") 会把「忍者神龟3汉化版」
 *   抢走；cht `炸弹人2` 会把「炸弹人2002」抢走。挂错金手指 = 改错内存、直接崩游戏，
 *   这账我们在 NDS 上吃过。白名单里的词只覆盖"同一款游戏的不同版本/汉化/加强"，
 *   语义上共用金手指是安全的。
 *   ⚠ 用前缀比较（不是相等）：实测 extra 常是复合词，
 *     如「无限命汉化版」「汉化版震动版」。 */
static int cheat_suffix_ok(const char *extra)
{
    static const char *OK[] = {
        "汉化版", "汉化", "可视化", "无敌版", "无敌", "改版", "强化版", "强化",
        "超级版", "美版", "日版", "欧版", "限定版", "震动版", "不死版", "不死",
        "完美版", "完美汉化", "增强版", "重制版", "复刻版", "最终版",
        "简体版", "繁体版", "中文版", "无限",
        NULL
    };
    if (!extra || !extra[0]) return 0;
    for (int i = 0; OK[i]; i++)
        if (strncmp(extra, OK[i], strlen(OK[i])) == 0) return 1;
    return 0;
}

/* 归一后的目录名是不是「平台目录」（FC / FC hack / MD hack(picodrive) …）。
 *
 * ★★ 子目录回退必须先过这一关，否则会大面积错配（2026-09-18 实测）：
 *   索引里 FC/SFC/MD 的 rel 是**带平台前缀**的（`FC/1943.zip`），
 *   dirname 就是 `FC` —— 一旦拿它去反向匹配，cht `FC原人.cht`(norm="fc原人")
 *   会把整个 FC 平台的 37 款 ROM 全挂到它头上。
 *   注释里的值都是 chat_norm() 处理后的形态（空格/连字符被吞）。 */
static int cheat_is_plat_dir(const char *s)
{
    static const char *P[] = {
        "fc", "sfc", "md", "gb", "gbc", "gba", "pce", "ps1", "dc", "n64",
        "psp", "ss", "nds", "arcade", "neogeo", "naomi", "ws", "wsc", "gg",
        "ngpc", "dos", "pico8", "j2me", "atari", "hack",
        "fchack", "sfchack", "mdhack", "fchd", "sfchd", "mdhd", "mdsfc",
        NULL
    };
    if (!s || !s[0]) return 1;               /* 没有目录名 —— 当平台处理 */
    for (int i = 0; P[i]; i++)
        if (strcmp(s, P[i]) == 0) return 1;
    return 0;
}

/* 在库里找这个游戏的 cht，**只找文件、不解析内容**。
 * 返回 1 = 找到（文件名写进 out，不含目录）。
 *
 * ★ 五级匹配（后四级是 2026-09-18 第二轮加的，专治 PS1/DC「一个都认不出来」）：
 *   ① exact   归一后完全相等                                    （最可信）
 *   ② fwd     cht 名比 ROM 名长 —— cht 带副标题/区域后缀，取**最短**那个
 *   ③ rev     ROM 名比 cht 名长，且多出来的是版本修饰词（汉化版/无敌版/…）
 *             例：ROM「放浪冒险谭 汉化版.chd」-> cht「放浪冒险谭.cht」
 *   ④ DIRrev  拿 ROM 的**真子目录名**再走一遍 ③（多碟盘常这么放）
 *             例：ROM「异度装甲 汉化版/Xenogears (Disc 1).chd」-> cht「异度装甲.cht」
 *   ⑤ NAMEex  拿游戏库的**显示名**精确匹配。索引里的 title 是整理过的中文译名，
 *             能把「FC-HD/Kung Fu (U) [!].zip」接到「成龙踢馆_ra.cht」、
 *             「[SuperGrafx]1941 反击战」接到「1941反击战_ra.cht」（norm 会剥掉
 *             方括号内容，正好把 [SuperGrafx] 前缀吃掉）           （最靠猜）
 *   ★ 顺序不能换：越靠后的"猜"成分越大，前面的能命中就不许往后走。
 *
 * ★ 为什么和 cheat_scan() 拆开（2026-09-18）：弹层要判断"这个游戏有没有
 *   金手指"来决定「金手指」这一项显不显示，那个判断每帧都可能跑到；
 *   而下面那半段解析是**有副作用**的（会重置 g_act_chbits，还会往
 *   g_act_chdesc 里灌条目名）。判据和"真的要用"必须分开 ——
 *   否则弹层一画就把玩家存的开关位串冲掉。 */
static int cheat_find_file(const char *pid, const char *rom_rel,
                           const char *disp_name, char *out, size_t cap)
{
    if (out && cap) out[0] = '\0';
    if (!pid || !pid[0] || !out || !cap) return 0;

    char dir[420];
    snprintf(dir, sizeof(dir), "%s/%s中文金手指", CHEAT_LIB_DIR, pid);

    DIR *dp = opendir(dir);
    if (!dp) return 0;                    /* 这个平台没有中文金手指，安静返回 */

    /* 待查 ROM 名 —— rel 是相对路径（可能带目录），只取文件名那一段 */
    char want[280];
    {
        const char *sl = rom_rel ? strrchr(rom_rel, '/') : NULL;
        cheat_norm(sl ? sl + 1 : (rom_rel ? rom_rel : ""), want, sizeof(want));
    }
    if (!want[0]) { closedir(dp); return 0; }

    /* ⑤ 的键 = 显示名。和 ROM 名归一后同源就没信息量，直接不启用。 */
    char wname[280];
    cheat_norm(disp_name ? disp_name : "", wname, sizeof(wname));
    if (!wname[0] || strcmp(wname, want) == 0) wname[0] = '\0';

    /* ④ 的键 = 真子目录名（dirname 的**最后一段**）。⚠ 平台目录必须排除，见 cheat_is_plat_dir */
    char wdir[280];
    wdir[0] = '\0';
    if (rom_rel) {
        const char *sl = strrchr(rom_rel, '/');
        if (sl && sl != rom_rel) {
            char d[300];
            size_t dl = (size_t)(sl - rom_rel);
            if (dl >= sizeof(d)) dl = sizeof(d) - 1;
            memcpy(d, rom_rel, dl);
            d[dl] = '\0';
            const char *dlast = strrchr(d, '/');      /* 多级目录只认最靠里那一级 */
            char t[280];
            cheat_norm(dlast ? dlast + 1 : d, t, sizeof(t));
            if (!cheat_is_plat_dir(t)) snprintf(wdir, sizeof(wdir), "%s", t);
        }
    }

    char   best[256];  best[0] = '\0';  size_t bestlen = 0;   /* ② 取最短 */
    char   rev[256];   rev[0]  = '\0';  size_t revlen  = 0;   /* ③ 取最长前缀 */
    char   dre[256];   dre[0]  = '\0';  size_t drelen  = 0;   /* ④ 取最长前缀 */
    char   nmx[256];   nmx[0]  = '\0';  size_t nmxlen  = 0;   /* ⑤ 取最短 */
    int    exact = 0;                   size_t exactlen = 0;
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        size_t nl = strlen(de->d_name);
        if (nl < 5) continue;                             /* 至少 "x.cht" */
        if (strcasecmp(de->d_name + nl - 4, ".cht") != 0) continue;

        char got[280];
        cheat_norm(de->d_name, got, sizeof(got));
        if (!got[0]) continue;
        size_t gl = strlen(got), wl = strlen(want);

        /* ★★ 每一级的候选都要**确定性**地选出来，不能"遇上谁算谁"。
           归一化会吞掉空格/连字符/括号内容，于是库里出现大量同归一的重复：
             「放浪冒险谭.cht」 vs 「放浪冒险谭 .cht」
             「战场之狼_ra.cht」 vs 「战场之狼(美版)_ra.cht」
             「超级机器人大战 α.cht」 vs 「超级机器人大战α.cht」
           readdir 的顺序就是文件系统顺序（不保证稳定）⇒ 不 tie-break 的话，
           同一款游戏每次进可能挂上**不同**的金手指。统一按
           「文件名字节数」「字典序」定序，两端（C / Python 模拟）算法同源。 */

        if (strcmp(got, want) == 0) {                     /* ① exact：取最短 */
            if (!exact || nl < exactlen ||
                (nl == exactlen && strcmp(de->d_name, out) < 0)) {
                snprintf(out, cap, "%s", de->d_name);
                exactlen = nl;
                exact = 1;
            }
            continue;
        }
        if (gl > wl && strncmp(got, want, wl) == 0 &&     /* ② fwd：取最短 */
            (!best[0] || gl < bestlen ||
             (gl == bestlen && strcmp(de->d_name, best) < 0))) {
            bestlen = gl;
            snprintf(best, sizeof(best), "%s", de->d_name);
        }
        if (gl < wl && strncmp(want, got, gl) == 0 &&     /* ③ rev：取最长前缀 */
            cheat_suffix_ok(want + gl) &&
            (!rev[0] || gl > revlen ||
             (gl == revlen && strcmp(de->d_name, rev) < 0))) {
            revlen = gl;
            snprintf(rev, sizeof(rev), "%s", de->d_name);
        }
        if (wdir[0]) {                                    /* ④ DIRrev */
            size_t dl = strlen(wdir);
            if (gl < dl && strncmp(wdir, got, gl) == 0 &&
                cheat_suffix_ok(wdir + gl) &&
                (!dre[0] || gl > drelen ||
                 (gl == drelen && strcmp(de->d_name, dre) < 0))) {
                drelen = gl;
                snprintf(dre, sizeof(dre), "%s", de->d_name);
            }
        }
        if (wname[0] && strcmp(got, wname) == 0 &&        /* ⑤ NAMEex：取最短 */
            (!nmx[0] || nl < nmxlen ||
             (nl == nmxlen && strcmp(de->d_name, nmx) < 0))) {
            nmxlen = nl;
            snprintf(nmx, sizeof(nmx), "%s", de->d_name);
        }
    }
    closedir(dp);

    if (exact)   return out[0] ? 1 : 0;
    if (best[0]) { snprintf(out, cap, "%s", best); return 1; }
    if (rev[0])  { snprintf(out, cap, "%s", rev);  return 1; }
    if (dre[0])  { snprintf(out, cap, "%s", dre);  return 1; }
    if (nmx[0])  { snprintf(out, cap, "%s", nmx);  return 1; }
    return 0;
}

/* 在库里找这个游戏的 cht 并**读进来**：文件名写进 out、条数写进 cnt，
 * 条目名灌进 g_act_chdesc、g_act_chvis/CHEAT_MAX 与 g_act_chbits 一并复位。 */
static int cheat_scan(const char *pid, const char *rom_rel,
                      const char *disp_name, char *out, size_t cap, int *cnt)
{
    if (out && cap) out[0] = '\0';
    if (cnt) *cnt = 0;
    if (!cheat_find_file(pid, rom_rel, disp_name, out, cap)) return 0;

    char dir[420];
    snprintf(dir, sizeof(dir), "%s/%s中文金手指", CHEAT_LIB_DIR, pid);

    /* 数条数 + 抓条目名。两件事都得防坑：
     *  ① **不要信文件头的 `cheats = N`** —— 实测有 decl=8/real=14、decl=39/real=125
     *     对不上的，一律以实际 `cheatN_desc` 行为准（RA 自己也是按行读的）。
     *  ② desc 文本里**本身可能带引号**（`"时间0'0"00"`、`""吸收能力"`），
     *     所以取**第一个引号到最后一个引号**之间，不能按引号 split。
     * 顺带把条目名逐条拷进 g_act_chdesc（超 CHEAT_MAX 的只计数不存）。
     * bits 默认**全 '0'（全关）** —— 浪总 2026-09-18 晚特意要求的：
     *   "金手指打开之后点进去的功能不要默认都是开启的呀 容易出bug"。
     *   这个坑很实在：库里有 101 个文件 >255 条（最大 23107 条），全 enable
     *   既可能把 RA 拖死，多条金手指互相打架也可能把游戏搞崩。
     *   之后 act_cheat_refresh() 读 cheats.txt 时再按记录覆盖。 */
    char full[600];
    snprintf(full, sizeof(full), "%s/%s", dir, out);
    g_act_chvis = 0;
    for (size_t i = 0; i < sizeof(g_act_chbits) - 1; i++) g_act_chbits[i] = '0';

    FILE *fh = fopen(full, "r");
    if (fh) {
        char line[1024];
        int n = 0;
        while (fgets(line, sizeof(line), fh)) {
            const char *s = line;
            while (*s == ' ' || *s == '\t') s++;
            if (strncmp(s, "cheat", 5) != 0) continue;
            const char *u = strchr(s, '_');
            if (!u || strncmp(u, "_desc", 5) != 0) continue;

            const char *eq = strchr(u + 5, '=');
            if (eq) {
                const char *q1 = strchr(eq, '"');
                const char *q2 = q1 ? strrchr(eq, '"') : NULL;
                if (q1 && q2 && q2 > q1 + 1 && n < CHEAT_MAX) {
                    size_t len = (size_t)(q2 - q1 - 1);
                    if (len > CHEAT_DESC_CAP - 1) len = CHEAT_DESC_CAP - 1;
                    /* 别把 UTF-8 汉字截成半个（续字节 10xxxxxx 就再退一格） */
                    while (len && ((unsigned char)q1[1 + len] & 0xC0) == 0x80) len--;
                    memcpy(g_act_chdesc[n], q1 + 1, len);
                    g_act_chdesc[n][len] = '\0';
                }
            }
            n++;
        }
        fclose(fh);
        g_act_chcount = n;
        g_act_chvis   = (n > CHEAT_MAX) ? CHEAT_MAX : n;
        g_act_chbits[g_act_chvis] = '\0';
        if (cnt) *cnt = n;
    }
    return 1;
}

/* 这一行是不是"这次要替换掉"的那条。 */
static int cheat_line_matches(const char *line, const char *pid,
                              const char *rom, int scope)
{
    const char *t1 = strchr(line, '\t');
    if (!t1 || !pid) return 0;

    size_t pl = strlen(pid);
    if ((size_t)(t1 - line) != pl || strncmp(line, pid, pl) != 0) return 0;

    const char *t2 = strchr(t1 + 1, '\t');
    if (!t2) return 0;                                /* 只认三字段行 */

    if (scope == 1) return (t2 - (t1 + 1)) == 1 && t1[1] == '*';
    if (!rom || !rom[0]) return 0;
    size_t rl = strlen(rom);
    return (size_t)(t2 - (t1 + 1)) == rl && strncmp(t1 + 1, rom, rl) == 0;
}

/* 读某一级的开关。返回 1 = 有记录。
 *   on   <- 总开关（第 3 列）
 *   bits <- 条目级位串（第 4 列，可选；没有就让调用方保留"全开"默认值）
 * ★ 第 4 列是 2026-09-18 晚加的 —— 老记录只有三列，读到就保持全开，
 *   老 cheats.txt 因此完全向后兼容。 */
static int cheat_get(const char *pid, const char *rom, int scope,
                     int *on, char *bits, size_t bcap)
{
    if (on) *on = 0;
    if (bits && bcap) bits[0] = '\0';
    if (!pid || !pid[0]) return 0;

    FILE *fh = fopen(CHEATS_PATH, "r");
    if (!fh) return 0;

    char line[768];
    int hit = 0;
    while (fgets(line, sizeof(line), fh)) {
        size_t Lq = strlen(line);
        while (Lq && (line[Lq - 1] == '\n' || line[Lq - 1] == '\r')) line[--Lq] = '\0';
        if (!line[0] || line[0] == '#') continue;

        const char *t1 = strchr(line, '\t');
        if (!t1) continue;
        const char *t2 = strchr(t1 + 1, '\t');
        if (!t2) continue;

        size_t pl = strlen(pid);
        if ((size_t)(t1 - line) != pl || strncmp(line, pid, pl) != 0) continue;

        int ok;
        if (scope == 1) {
            ok = ((t2 - (t1 + 1)) == 1 && t1[1] == '*');
        } else {
            size_t rl = rom ? strlen(rom) : 0;
            ok = rl > 0 && (size_t)(t2 - (t1 + 1)) == rl &&
                 strncmp(t1 + 1, rom, rl) == 0;
        }
        if (!ok) continue;

        if (on) *on = (t2[1] == '1');
        if (bits && bcap) {
            const char *t3 = strchr(t2 + 1, '\t');
            if (t3 && t3[1]) {
                size_t j = 0;
                for (const char *q = t3 + 1; *q && j + 1 < bcap; q++) {
                    if (*q != '0' && *q != '1') break;   /* 只认位串，别的当场收手 */
                    bits[j++] = *q;
                }
                bits[j] = '\0';
            }
        }
        hit = 1;
        break;
    }
    fclose(fh);
    return hit;
}

/* 把记录里的位串套到当前这份 cht 上。
 * ★ 只有记录里**明确的 '1' 才算开**，其余（长度不够 / 位串为空）一律保持
 *   scan 给的默认值 '0'（全关）—— 跟"默认不勾选"这条产品约定一致。 */
static void cheat_apply_bits(const char *bits)
{
    if (!bits || !bits[0]) return;
    size_t L = strlen(bits);
    for (int i = 0; i < g_act_chvis; i++)
        if ((size_t)i < L) g_act_chbits[i] = (bits[i] == '1') ? '1' : '0';
    g_act_chbits[g_act_chvis] = '\0';
}

/* 写某一级的开关，整体重写。撑死几百行，不值得搞增量更新。
 *   bits 非空且非空串 -> 追加第 4 列（条目级位串）。
 * ★ 行缓冲从 256 放大到 768：加了位串列之后一行能到约 490 字节，256 会
 *   **静默截断** —— fgets/snprintf 截断都不报错，位串会悄悄丢尾巴。 */
static int cheat_set(const char *pid, const char *rom, int scope, int on,
                     const char *bits)
{
    if (!pid || !pid[0]) return 0;

    static char keep[256][768];
    int n = 0;
    FILE *fh = fopen(CHEATS_PATH, "r");
    if (fh) {
        char line[768];
        while (fgets(line, sizeof(line), fh)) {
            size_t Lq = strlen(line);
            while (Lq && (line[Lq - 1] == '\n' || line[Lq - 1] == '\r')) line[--Lq] = '\0';
            if (!line[0]) continue;
            if (cheat_line_matches(line, pid, rom, scope)) continue;   /* 旧的丢掉 */
            if (n < 256) snprintf(keep[n++], sizeof(keep[0]), "%s", line);
        }
        fclose(fh);
    }

    char nl[768];
    if (scope == 1) snprintf(nl, sizeof(nl), "%s\t*\t%d", pid, on ? 1 : 0);
    else            snprintf(nl, sizeof(nl), "%s\t%s\t%d",
                            pid, (rom && rom[0]) ? rom : "", on ? 1 : 0);
    if (bits && bits[0]) {
        size_t bl = strlen(nl);
        if (bl + 1 + strlen(bits) < sizeof(nl)) {
            nl[bl] = '\t';
            snprintf(nl + bl + 1, sizeof(nl) - bl - 1, "%s", bits);
        }
    }
    if (n < 256) snprintf(keep[n++], sizeof(keep[0]), "%s", nl);

    FILE *w = fopen(CHEATS_PATH, "w");
    if (!w) return 0;
    for (int i = 0; i < n; i++) fprintf(w, "%s\n", keep[i]);
    fclose(w);
    return 1;
}

/* 这个游戏在 cheats.txt 里的键 = ROM 的**文件名**（含扩展名，不含目录）。
 * ★ 必须和宿主对齐 —— vagaretro-shell.py 那边用的是 os.path.basename(rom)。 */
static const char *cheat_rom_name(Platform *p)
{
    static char buf[256];
    buf[0] = '\0';
    if (!p || g_act_gi < 0 || g_act_gi >= p->ngames) return buf;
    const char *rel = p->games[g_act_gi].rel;
    if (!rel || !rel[0]) return buf;
    const char *sl = strrchr(rel, '/');
    snprintf(buf, sizeof(buf), "%s", sl ? sl + 1 : rel);
    return buf;
}

/* ---- /opt/vagaretro/cores.txt：用户挑的模拟器核心 ----------------------
   与 favorites.txt 同款格式（一行一条，TAB 分隔）：
       <平台id>\t<核心名>                 平台级
       <平台id>\t<相对路径>\t<核心名>      游戏级（优先）
   游戏级没命中再退回平台级；两级都没有就是"自动"，宿主按 FORCE_CORE /
   es_systems.cfg 原样走。宿主 vagaretro-shell.py 读的就是这个文件，
   所以**前端只管写，启动链不用改**。 */

/* 拿当前生效的覆盖核心，只看 scope 指定的那一级。没有就返回 0（out 置空）。 */
static int core_get(const char *pid, const char *rel, int scope,
                    char *out, size_t cap)
{
    if (out && cap) out[0] = '\0';
    if (!pid || !pid[0] || !out || !cap) return 0;

    FILE *fh = fopen(CORES_PATH, "r");
    if (!fh) return 0;

    char line[900];
    int hit = 0;
    while (fgets(line, sizeof(line), fh)) {
        size_t L = strlen(line);
        while (L && (line[L - 1] == '\n' || line[L - 1] == '\r')) line[--L] = '\0';
        if (!line[0]) continue;

        char *t1 = strchr(line, '\t');
        if (!t1) continue;
        *t1 = '\0';
        if (strcmp(line, pid) != 0) continue;

        char *t2 = strchr(t1 + 1, '\t');
        if (scope == 1) {                       /* 平台级：只认两个字段的 */
            if (t2) continue;
            snprintf(out, cap, "%s", t1 + 1);
            hit = 1;
            break;
        }
        /* ⚠ 游戏级要比的是**第 2 个字段**（rel，从 t1+1 到 t2），不是 t2+1。
           t2+1 是核心名 —— 拿它跟 rel 比永远不相等，表现成"游戏级覆盖写进去了、
           但界面读不回来"：开列表时"当前"错显示成「自动」、光标也不对准。
           （2026-09-15 离屏测试第 8 段抓到的：选回自动没删掉、反而选成了别的核心。
             写入侧一直是好的，因为 core_line_matches() 里那段是对的。） */
        if (!t2 || !rel) continue;
        size_t fl = (size_t)(t2 - (t1 + 1));
        if (strlen(rel) != fl || strncmp(t1 + 1, rel, fl) != 0) continue;
        snprintf(out, cap, "%s", t2 + 1);
        hit = 1;
        break;
    }
    fclose(fh);
    return hit;
}

/* 这一行是不是"这次要替换/删掉"的那条。 */
static int core_line_matches(const char *line, const char *pid,
                             const char *rel, int scope)
{
    const char *t1 = strchr(line, '\t');
    if (!t1 || !pid) return 0;

    size_t pl = strlen(pid);
    if ((size_t)(t1 - line) != pl || strncmp(line, pid, pl) != 0) return 0;

    const char *t2 = strchr(t1 + 1, '\t');
    if (scope == 1) return t2 == NULL;                  /* 平台级 = 两个字段 */

    if (!t2 || !rel) return 0;
    size_t rl = strlen(rel);
    return (size_t)(t2 - t1 - 1) == rl && strncmp(t1 + 1, rel, rl) == 0;
}

/* 写 / 删一条覆盖，整体重写文件（和 fav_set 一样的偷懒法：这文件撑死几十行）。
   core 为空 = 删掉这一条（前端选「自动」时）。成功返回 1。
   只动 pid + scope 命中的那一级那一行，另一级的原样保留 ——
   所以"平台级配了 X、某个游戏又单独配了 Y"这种组合不会被抹平。 */
static int core_set(const char *pid, const char *rel, int scope, const char *core)
{
    if (!pid || !pid[0]) return 0;

    /* 静态缓冲：候选撑死十几个平台 × 几条，这里给足余量，别放栈上 */
    static char keep[CORE_MAX * 8][900];
    const int KMAX = (int)(sizeof(keep) / sizeof(keep[0]));
    int n = 0;

    FILE *fh = fopen(CORES_PATH, "r");
    if (fh) {
        char line[900];
        while (fgets(line, sizeof(line), fh)) {
            size_t L = strlen(line);
            while (L && (line[L - 1] == '\n' || line[L - 1] == '\r')) line[--L] = '\0';
            if (!line[0]) continue;
            if (core_line_matches(line, pid, rel, scope)) continue;   /* 旧的丢掉 */
            if (n < KMAX) snprintf(keep[n++], sizeof(keep[0]), "%s", line);
        }
        fclose(fh);
    }

    if (core && core[0] && n < KMAX) {
        if (scope == 1)
            snprintf(keep[n++], sizeof(keep[0]), "%s\t%s", pid, core);
        else
            snprintf(keep[n++], sizeof(keep[0]), "%s\t%s\t%s",
                     pid, rel ? rel : "", core);
    }

    /* 一条都不剩就把文件删掉 —— 免得留个空文件让人以为配过什么，
       宿主的 load_core_overrides() 读它也是白读 */
    if (n == 0) { remove(CORES_PATH); return 1; }

    FILE *w = fopen(CORES_PATH, "w");
    if (!w) return 0;
    for (int i = 0; i < n; i++) fprintf(w, "%s\n", keep[i]);
    fclose(w);
    return 1;
}

/* ---- /opt/vagaretro/shaders.txt：按核心记的着色器预设 -------------------
   一行一条 `<核心名>\t<预设相对路径>`（相对 shaders_glsl/）。
   和 cores.txt 的区别：那张表按 <pid> 或 <pid+rel> 索引，这张按 **core 名**。
   同一个 core 被多个平台用（比如 fbneo 既跑 arcade 又跑 fbneo、snes9x 既跑
   snes 又跑 sfc），就是**一处设置、多处生效** —— 用户要的正是这个。

   读：命中返回 1 并把预设填进 out；没有该 core 的行返回 0（out 置空）。
   ⚠ 不缓存：文件不到 1KB，而"改完下一次启动立刻生效"比省这一次读盘重要得多
     （和 load_core_overrides 同一个取舍）。 */
static int shader_get(const char *core, char *out, size_t cap)
{
    if (out && cap) out[0] = '\0';
    if (!core || !core[0] || !out || !cap) return 0;

    FILE *fh = fopen(SHADERS_PATH, "r");
    if (!fh) return 0;

    char line[900];
    int hit = 0;
    while (fgets(line, sizeof(line), fh)) {
        size_t L = strlen(line);
        while (L && (line[L - 1] == '\n' || line[L - 1] == '\r')) line[--L] = '\0';
        if (!line[0] || line[0] == '#') continue;

        char *t = strchr(line, '\t');
        if (!t) continue;
        *t = '\0';
        if (strcmp(line, core) != 0) continue;
        snprintf(out, cap, "%s", t + 1);
        hit = 1;
        break;
    }
    fclose(fh);
    return hit;
}

/* 写 / 删一个核心的预设，整体重写文件（和 core_set 一样的偷懒法）。
   rel 为空 = 删掉这个核心那一条（用户在列表里选「关闭」）。
   成功返回 1。只有这一个核心的行会被动，别的核心原样保留。 */
static int shader_set(const char *core, const char *rel)
{
    if (!core || !core[0]) return 0;

    static char keep[SHADER_MAXLINES][900];
    const int KMAX = (int)(sizeof(keep) / sizeof(keep[0]));
    int n = 0;

    FILE *fh = fopen(SHADERS_PATH, "r");
    if (fh) {
        char line[900];
        while (fgets(line, sizeof(line), fh)) {
            size_t L = strlen(line);
            while (L && (line[L - 1] == '\n' || line[L - 1] == '\r')) line[--L] = '\0';
            if (!line[0]) continue;

            /* 只丢掉"这个核心"那一条 —— 比的时候要按 TAB 切，别拿整行 strncmp：
               核心名是前缀关系（fbneo / fbneo_plus）时整行比会误伤。 */
            char tmp[900];
            snprintf(tmp, sizeof(tmp), "%s", line);
            char *t = strchr(tmp, '\t');
            if (t) {
                *t = '\0';
                if (strcmp(tmp, core) == 0) continue;
            }
            if (n < KMAX) snprintf(keep[n++], sizeof(keep[0]), "%s", line);
        }
        fclose(fh);
    }

    if (rel && rel[0] && n < KMAX)
        snprintf(keep[n++], sizeof(keep[0]), "%s\t%s", core, rel);

    /* 一条都不剩就把文件删掉，别留个空文件让人以为配过什么 */
    if (n == 0) { remove(SHADERS_PATH); return 1; }

    FILE *w = fopen(SHADERS_PATH, "w");
    if (!w) return 0;
    for (int i = 0; i < n; i++) fprintf(w, "%s\n", keep[i]);
    fclose(w);
    return 1;
}

/* ★ 2026-09-17：slot_read() / slot_write() 随「加载存档」一并删除
   （它们读写 /opt/vagaretro/slots.txt，那个文件也不再有人生成）。
   续档改由「退出自动存档」+ 设置页「自动存档位」承担。 */

/* 把 p->cores（逗号分隔）拆成字符串数组，返回数组、个数写进 *n_out。
   就地拆：第一次把逗号换成 '\0'，之后按指针命中缓存直接返回。
   这些指针指向**索引缓冲区**，活到进程结束，所以不用拷贝也不会悬空。 */
static const char *const *act_core_list(Platform *p, int *n_out)
{
    static const char *list[CORE_MAX];
    static const char *cached = NULL;
    static int n = 0;

    if (!p || !p->cores || !p->cores[0]) {
        cached = NULL;
        n = 0;
        if (n_out) *n_out = 0;
        return list;
    }
    if (cached == p->cores) {                  /* 拆过了 */
        if (n_out) *n_out = n;
        return list;
    }

    char *s = (char *)p->cores;
    n = 0;
    while (*s && n < CORE_MAX) {
        char *c = strchr(s, ',');
        if (c) *c = '\0';
        list[n++] = s;
        if (!c) break;
        s = c + 1;
    }
    cached = p->cores;
    if (n_out) *n_out = n;
    return list;
}

/* 二级列表一共几行：0 适用范围 + 1 自动 + 候选 */
static int act_core_sel_count(Platform *p)
{
    int nc = 0;
    act_core_list(p, &nc);
    return 2 + nc;
}

/* 把收藏文件里的条目挂到索引的 favorite 平台上。
 *
 * ⚠ 指针直接指回原平台那条 Game 的 title/rel/img —— 它们都在索引缓冲区里，
 *   活到进程结束，所以不用 strdup，也不怕 game_push 把 games 数组 realloc 掉
 *   （搬的是 Game 结构体本身，不是它指向的字符串）。
 * ⚠ favorite 是**虚拟平台**，宿主故意不给它 G 行，条目本来就该前端自己算。
 *   这就是"我的收藏永远是空的"的正解。 */
static void favorites_load(void)
{
    int fi = -1;
    for (int i = 0; i < IX.np; i++)
        if (IX.p[i].pid && strcmp(IX.p[i].pid, "favorite") == 0) { fi = i; break; }
    if (fi < 0) return;

    Platform *fp = &IX.p[fi];
    fp->ngames = 0;                       /* 重填，别在旧内容上叠加 */

    FILE *fh = fopen(FAV_PATH, "r");
    if (!fh) {
        printf("[收藏] 还没有 %s，收藏列表为空\n", FAV_PATH);
        fflush(stdout);
        return;
    }

    char line[800];
    int n = 0;
    while (fgets(line, sizeof(line), fh)) {
        size_t L = strlen(line);
        while (L && (line[L - 1] == '\n' || line[L - 1] == '\r')) line[--L] = '\0';
        char *t = strchr(line, '\t');
        if (!t) continue;
        *t = '\0';
        const char *pid = line, *rel = t + 1;

        int pi = -1;
        for (int i = 0; i < IX.np; i++)
            if (IX.p[i].pid && strcmp(IX.p[i].pid, pid) == 0) { pi = i; break; }
        if (pi < 0) continue;

        int gi = -1;
        for (int i = 0; i < IX.p[pi].ngames; i++)
            if (IX.p[pi].games[i].rel && strcmp(IX.p[pi].games[i].rel, rel) == 0) {
                gi = i; break;
            }
        if (gi < 0) continue;             /* ROM 已经删了 → 自动掉出收藏，正合适 */

        Game *g = &IX.p[pi].games[gi];
        game_push(fp, g->title, g->rel, g->img, g->py);
        fp->games[fp->ngames - 1].src_pid = IX.p[pi].pid;  /* 启动时要靠它 */
        n++;
    }
    fclose(fh);
    printf("[收藏] 载入 %d 条（%s）\n", n, FAV_PATH);
    fflush(stdout);
}

/* ==========================================================================
 *  5.5b 游玩历史（用户 2026-09-15 报"不会记录玩过的游戏"）
 *
 *  索引里 history 平台的 P 行是宿主写死的（"最近 30 款"），
 *  但宿主**不发 G 行** —— compat 目录下压根没有 history/ 这个目录。
 *  所以内容和收藏一样，由前端自己维护一份文本文件：
 *
 *      /opt/vagaretro/history.txt     一行一条  <平台id>\t<相对路径>
 *
 *  **最近的排最前**，文件顺序即显示顺序（不需要额外的时间戳列）。
 *  同一个游戏只占一行：再玩一次就把它从原位置摘掉、插到最前，
 *  而不是在列表里刷出十行同名。
 * ==========================================================================*/
static void hist_record(const char *pid, const char *rel)
{
    if (!pid || !*pid || !rel || !*rel) return;

    static char keep[HIST_MAX][800];
    int n = 0;

    /* 先把旧内容读进来，顺手把这一条摘掉（去重） */
    FILE *fh = fopen(HIST_PATH, "r");
    if (fh) {
        char line[800];
        while (n < HIST_MAX && fgets(line, sizeof(line), fh)) {
            size_t L = strlen(line);
            while (L && (line[L - 1] == '\n' || line[L - 1] == '\r'))
                line[--L] = '\0';
            if (!*line) continue;
            char *t = strchr(line, '\t');
            if (!t) continue;
            *t = '\0';
            if (strcmp(line, pid) == 0 && strcmp(t + 1, rel) == 0) continue;
            snprintf(keep[n], sizeof(keep[n]), "%s\t%s", line, t + 1);
            n++;
        }
        fclose(fh);
    }

    fh = fopen(HIST_PATH, "w");
    if (!fh) {
        fprintf(stderr, "[历史] 写不了 %s，本次不记\n", HIST_PATH);
        fflush(stderr);
        return;
    }
    fprintf(fh, "%s\t%s\n", pid, rel);
    int wrote = 1;
    for (int i = 0; i < n && wrote < HIST_MAX; i++, wrote++)
        fprintf(fh, "%s\n", keep[i]);
    fclose(fh);

    printf("[历史] 记下 %s / %s（现有 %d 条）\n", pid, rel, wrote);
    fflush(stdout);
}

/* 把 history.txt 挂到索引的 history 平台上。
   和 favorites_load() 完全一个套路：宿主不发 G 行，内容全靠这里填。
   找不到对应 Game 的条目直接跳过 —— ROM 删了就该从历史里消失。 */
static void history_load(void)
{
    int hi = -1;
    for (int i = 0; i < IX.np; i++)
        if (IX.p[i].pid && strcmp(IX.p[i].pid, "history") == 0) { hi = i; break; }
    if (hi < 0) return;

    Platform *hp = &IX.p[hi];
    hp->ngames = 0;                      /* 重填，别在旧内容上叠加 */

    FILE *fh = fopen(HIST_PATH, "r");
    if (!fh) {
        printf("[历史] 还没有 %s，游玩历史为空\n", HIST_PATH);
        fflush(stdout);
        return;
    }

    char line[800];
    int n = 0;
    while (n < HIST_MAX && fgets(line, sizeof(line), fh)) {
        size_t L = strlen(line);
        while (L && (line[L - 1] == '\n' || line[L - 1] == '\r'))
            line[--L] = '\0';
        char *t = strchr(line, '\t');
        if (!t) continue;
        *t = '\0';
        const char *pid = line, *rel = t + 1;

        int pi = -1;
        for (int i = 0; i < IX.np; i++)
            if (IX.p[i].pid && strcmp(IX.p[i].pid, pid) == 0) { pi = i; break; }
        if (pi < 0) continue;

        int gi = -1;
        for (int i = 0; i < IX.p[pi].ngames; i++)
            if (IX.p[pi].games[i].rel && strcmp(IX.p[pi].games[i].rel, rel) == 0) {
                gi = i; break;
            }
        if (gi < 0) continue;

        Game *g = &IX.p[pi].games[gi];
        game_push(hp, g->title, g->rel, g->img, g->py);
        hp->games[hp->ngames - 1].src_pid = IX.p[pi].pid;   /* 记住真实机种 */
        n++;
    }
    fclose(fh);
    printf("[历史] 载入 %d 条（%s）\n", n, HIST_PATH);
    fflush(stdout);
}

/* 真删卡上的源文件。成功返回 1。 */
static int act_do_delete(Platform *p, int gi)
{
    char path[1024];
    if (!act_path_of(p, gi, path, sizeof(path))) return 0;

    /* ② 模式保险：截图/调试模式一律不删。
       离屏实例跑的是真机同款二进制，跑一次截图脚本就可能把用户卡上的
       ROM 抹掉 —— 这坑在 launch_game 里踩过一次，这里不能再踩。
       唯一的例外是显式设了 VG_ALLOW_DELETE=1：只给自动化测试留的口子，
       绝不写进任何启动脚本。 */
    if (g_shot_mode && !getenv("VG_ALLOW_DELETE")) {
        printf("[操作] 截图模式，跳过删除：%s\n", path);
        fflush(stdout);
        return 0;
    }

    if (unlink(path) != 0) {
        fprintf(stderr, "[操作] 删除失败：%s\n", path);
        fflush(stderr);
        return 0;
    }
    printf("[操作] 已删除 %s\n", path);
    fflush(stdout);

    /* ④ 只摘掉这一条，不重扫。先把它那份记忆化的封面路径还掉，
       再整体前移 —— 不还的话那个 strdup 出来的串就漏了。 */
    free(p->games[gi].cov);
    for (int i = gi; i + 1 < p->ngames; i++) p->games[i] = p->games[i + 1];
    p->ngames--;
    return 1;
}

/* ---- 「更换模拟器核心」的状态机 ----------------------------------------
   ⚠ 一律用 src_pid：从「游玩历史」「我的收藏」进来时当前平台的 pid 是虚拟的
     （history / favorite），拿它去查覆盖表永远查不到，写进去也是个没人读的
     键 —— 和 launch_game() 里踩过的那个坑是同一个。 */

/* 弹层/二级列表当前针对的那个平台 */
static Platform *act_plat(void)
{
    return (g_act_pi >= 0 && g_act_pi < IX.np) ? &IX.p[g_act_pi] : NULL;
}

/* 这个游戏的真实机种 id */
static const char *act_real_pid(Platform *p)
{
    if (!p || g_act_gi < 0 || g_act_gi >= p->ngames) return NULL;
    const char *rp = p->games[g_act_gi].src_pid;
    return (rp && rp[0]) ? rp : p->pid;
}

/* 这个游戏真正归属的平台结构体。
   ⚠ 收藏 / 历史是**虚拟平台**：p->cores 是空的（宿主不给虚拟平台发 C 行），
     直接 act_core_list(p) 永远拿不到候选。所以先按 src_pid 找回真实平台；
     游戏本来就在自己平台上时原样返回，不多绕。 */
static Platform *act_real_plat(Platform *p)
{
    const char *rp = act_real_pid(p);
    if (!rp || !rp[0]) return p;
    if (p->pid && strcmp(rp, p->pid) == 0) return p;
    for (int i = 0; i < IX.np; i++)
        if (IX.p[i].pid && strcmp(IX.p[i].pid, rp) == 0) return &IX.p[i];
    return p;              /* 找回失败就退回原来那个，别指到空 */
}

/* cores.txt / 索引 C 行里的候选可能是 "<模拟器>:<核心>" 形态 —— 跨模拟器换
   核心（见 vagaretro-shell.py 的 parse_core_override / core_candidates）：
       "parallel_n64"               默认模拟器，原样显示
       "retroarch:mupen64plus_next" 换到 retroarch 里的 mupen64plus_next
       "standalone-Rice:"           换到 standalone 模拟器（%CORE% 留空）
   直接把原始串画出来会是一行 retroarch:mupen64plus_next，所以转成人看的写法。 */
static void core_label_into(const char *item, char *out, size_t cap)
{
    if (!cap) return;
    out[0] = '\0';
    if (!item || !item[0]) return;
    const char *c = strchr(item, ':');
    if (!c) {                                    /* 默认模拟器的核心 */
        snprintf(out, cap, "%s", item);
        return;
    }
    if (c[1])                                    /* "<模拟器>:<核心>" */
        snprintf(out, cap, "%s (%.*s)", c + 1, (int)(c - item), item);
    else                                         /* "<模拟器>:" —— 独立模拟器 */
        snprintf(out, cap, "独立模拟器 %.*s", (int)(c - item), item);
}

/* 同上，但给要"纯核心名"的地方用：着色器是按**核心名**配的（shaders.txt 的键
   就是核心名），带模拟器前缀去查表永远查不到。冒号后为空 = 用户选了 standalone，
   那种情况真没有 RA 核心，返回空串正好（着色器那一项会提示挂不上）。 */
static char *core_strip_emu(char *s)
{
    if (!s) return s;
    char *c = strchr(s, ':');
    return c ? c + 1 : s;
}

/* 这个游戏当前**实际会跑**的那个核心名。空串 = 用不了着色器（standalone 平台）。
   优先级必须和宿主 pick_core() 一致，否则会出现"前端按 A 核心配的、实际跑的
   是 B 核心"这种永远查不出来的错位：
       游戏级覆盖 > 平台级覆盖 > 索引 C 行的第一个候选
   最后一条的依据见 vagaretro-shell.py 里 core_candidates() 的注释 ——
   宿主保证 C 行顺序就是它实际会选的顺序（FORCE_CORE 排头）。 */
static const char *act_cur_core(Platform *p)
{
    static char buf[80];
    buf[0] = '\0';
    if (!p) return buf;

    const char *rp  = act_real_pid(p);
    const char *rel = (g_act_gi >= 0 && g_act_gi < p->ngames)
                      ? p->games[g_act_gi].rel : NULL;
    if (rp && rel) {
        if (core_get(rp, rel, 0, buf, sizeof(buf))) return core_strip_emu(buf);   /* 游戏级 */
        if (core_get(rp, rel, 1, buf, sizeof(buf))) return core_strip_emu(buf);   /* 平台级 */
    }

    int nc = 0;
    const char *const *cs = act_core_list(act_real_plat(p), &nc);
    if (nc > 0 && cs[0] && cs[0][0]) return core_strip_emu((char *)cs[0]);
    return buf;            /* 空：standalone，或虚拟平台且找回失败 */
}

/* 让光标所在行落在可见窗口里（候选多过一屏就要滚动） */
static void act_core_scroll(void)
{
    Platform *p = act_plat();
    int n = act_core_sel_count(p);

    if (n <= CORE_VIS) { g_act_ctop = 0; return; }
    if (g_act_csel < g_act_ctop) g_act_ctop = g_act_csel;
    if (g_act_csel > g_act_ctop + CORE_VIS - 1) g_act_ctop = g_act_csel - CORE_VIS + 1;
    if (g_act_ctop > n - CORE_VIS) g_act_ctop = n - CORE_VIS;
    if (g_act_ctop < 0) g_act_ctop = 0;
}

/* 按**当前 g_act_scope** 重读一遍覆盖值，并把光标对准生效的那一项。
   刻意不动 g_act_scope —— 适用范围是用户自己选的，不能被回读覆盖掉。 */
static void act_core_refresh(Platform *p)
{
    g_act_ocur[0] = '\0';
    g_act_csel = 1;                            /* 1 = "自动"那行 */

    const char *rp = act_real_pid(p);
    const char *rel = (p && g_act_gi >= 0 && g_act_gi < p->ngames)
                      ? p->games[g_act_gi].rel : NULL;
    if (!rp || !rel) return;

    core_get(rp, rel, g_act_scope, g_act_ocur, sizeof(g_act_ocur));

    int nc = 0;
    const char *const *cs = act_core_list(p, &nc);
    for (int i = 0; i < nc; i++)
        if (g_act_ocur[0] && strcmp(cs[i], g_act_ocur) == 0) {
            g_act_csel = 2 + i;
            break;
        }
}

/* 弹层打开时用：先定"默认看哪一级"，再交给 act_core_refresh 填值。
   默认规则：这个游戏已经单独配过 → 停在本游戏级；只配过平台级 → 停在平台级；
   都没配过 → 停在本游戏级（弹层是从某个具体游戏上叫出来的，先动这一层
   最不容易误伤整个平台）。 */
static void act_core_sync(Platform *p)
{
    g_act_scope = 0;

    char tmp[80];
    const char *rp = act_real_pid(p);
    const char *rel = (p && g_act_gi >= 0 && g_act_gi < p->ngames)
                      ? p->games[g_act_gi].rel : NULL;
    if (rp && rel) {
        if (core_get(rp, rel, 0, tmp, sizeof(tmp)))      g_act_scope = 0;
        else if (core_get(rp, rel, 1, tmp, sizeof(tmp))) g_act_scope = 1;
    }
    act_core_refresh(p);
    act_core_scroll();
}

/* 切换"适用范围"。切完立刻按新范围回读 —— 右边那个"当前"要跟着换，
   否则会出现"标着整个平台用 mgba，切到本游戏却还显示 mgba"的假象。 */
static void act_core_flip_scope(Platform *p)
{
    g_act_scope = !g_act_scope;
    act_core_refresh(p);
    act_core_scroll();
    mark_dirty();
}

/* 二级列表的按键。返回 1 = 已消费（和 act_key 一个约定）。 */
static int act_core_key(Platform *p, SDL_Keycode k)
{
    int n = act_core_sel_count(p);
    if (n < 1) n = 1;

    switch (k) {
    case SDLK_UP:
        g_act_csel = (g_act_csel + n - 1) % n;
        act_core_scroll();
        mark_dirty();
        return 1;

    case SDLK_DOWN:
        g_act_csel = (g_act_csel + 1) % n;
        act_core_scroll();
        mark_dirty();
        return 1;

    case SDLK_LEFT:
    case SDLK_RIGHT:
        act_core_flip_scope(p);              /* 左右换适用范围 */
        return 1;

    case SDLK_ESCAPE:
    case SDLK_BACKSPACE:
        g_act_core = 0;                      /* 只退二级列表，主弹层还开着 */
        mark_dirty();
        return 1;

    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        break;

    default:
        return 1;                            /* 模态：别的键吞掉 */
    }

    if (!p || g_act_gi < 0 || g_act_gi >= p->ngames) {
        g_act_core = 0;
        mark_dirty();
        return 1;
    }

    /* 第 0 行 = 适用范围：就地切换，**不关列表** —— 切完接着选核心 */
    if (g_act_csel == 0) { act_core_flip_scope(p); return 1; }

    const char *rp  = act_real_pid(p);
    const char *rel = p->games[g_act_gi].rel;
    int nc = 0;
    const char *const *cs = act_core_list(p, &nc);
    const char *pick = "";
    if (g_act_csel >= 2 && g_act_csel - 2 < nc) pick = cs[g_act_csel - 2];

    if (!core_set(rp, rel, g_act_scope, pick)) {
        fprintf(stderr, "[核心] 写不了 %s\n", CORES_PATH);
        fflush(stderr);
        set_flash("更换失败：写不了 cores.txt");
    } else {
        printf("[核心] %s %s：%s -> %s\n",
               g_act_scope ? "整个平台" : "仅本游戏", rp ? rp : "?",
               g_act_ocur[0] ? g_act_ocur : "(自动)",
               pick[0] ? pick : "(自动)");
        fflush(stdout);
        set_flash(pick[0] ? "已记录，下次启动这个游戏就用它"
                          : "已恢复自动选择核心");
        act_core_sync(p);                    /* 回读：状态行立刻对上 */
    }

    g_act_core = 0;                          /* 选完回主弹层 */
    mark_dirty();
    return 1;
}

/* ---- 着色器二级列表 ---------------------------------------------------- */

/* 按当前核心回读已绑的预设，并把光标对准那一项。
   读不到 → 停在"关闭"，这是对的：盘上没有这个核心的记录就表示没启用。 */
static void act_shader_refresh(Platform *p)
{
    g_act_scur[0] = '\0';
    g_act_ssel = 0;

    const char *core = act_cur_core(p);
    if (!core || !core[0]) return;
    shader_get(core, g_act_scur, sizeof(g_act_scur));

    for (int i = 0; i < SHADER_N; i++)
        if (strcmp(SHADER_REL[i], g_act_scur) == 0) { g_act_ssel = i; break; }
}

static void act_shader_scroll(void)
{
    const int n = SHADER_N;
    if (n <= SHADER_VIS) { g_act_stop = 0; return; }
    if (g_act_ssel < g_act_stop) g_act_stop = g_act_ssel;
    if (g_act_ssel > g_act_stop + SHADER_VIS - 1) g_act_stop = g_act_ssel - SHADER_VIS + 1;
    if (g_act_stop > n - SHADER_VIS) g_act_stop = n - SHADER_VIS;
    if (g_act_stop < 0) g_act_stop = 0;
}

static void act_shader_sync(Platform *p)
{
    act_shader_refresh(p);
    act_shader_scroll();
}

/* 二级列表的按键。返回 1 = 已消费（和 act_key 一个约定）。 */
static int act_shader_key(Platform *p, SDL_Keycode k)
{
    const int n = SHADER_N;

    switch (k) {
    case SDLK_UP:
        g_act_ssel = (g_act_ssel + n - 1) % n;
        act_shader_scroll();
        mark_dirty();
        return 1;

    case SDLK_DOWN:
        g_act_ssel = (g_act_ssel + 1) % n;
        act_shader_scroll();
        mark_dirty();
        return 1;

    case SDLK_ESCAPE:
    case SDLK_BACKSPACE:
        g_act_shader = 0;                    /* 只退二级列表，主弹层还开着 */
        mark_dirty();
        return 1;

    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        break;

    default:
        return 1;                            /* 模态：别的键吞掉 */
    }

    /* standalone 平台（NAOMI / DC / Saturn 那些）压根不走 retroarch，
       着色器挂不上去。直说比默默写盘好 —— 写了也不会生效。 */
    const char *core = act_cur_core(p);
    if (!core || !core[0]) {
        g_act_shader = 0;
        set_flash("这个平台不用 RetroArch 核心，没有着色器可挂");
        mark_dirty();
        return 1;
    }

    const char *rel = SHADER_REL[g_act_ssel];
    if (!shader_set(core, rel)) {
        fprintf(stderr, "[着色器] 写不了 %s\n", SHADERS_PATH);
        fflush(stderr);
        set_flash("着色器设置失败：写不了 shaders.txt");
    } else {
        printf("[着色器] 核心 %s -> %s\n", core, rel[0] ? rel : "(关闭)");
        fflush(stdout);
        set_flash(rel[0] ? "已记录，这个核心的游戏下次启动就带上它"
                         : "已关闭这个核心的着色器");
        act_shader_refresh(p);               /* 回读：状态行立刻对上 */
    }

    g_act_shader = 0;                        /* 选完回主弹层 */
    mark_dirty();
    return 1;
}

/* ★ 2026-09-17：act_slot_sync() / act_slot_key() 随「加载存档」一并删除。
   见 draw_slot_list() 上面的墓碑注释。 */

static void act_open(int pi, int gi)
{
    if (pi < 0 || pi >= IX.np) return;
    Platform *p = &IX.p[pi];
    if (gi < 0 || gi >= p->ngames) return;

    g_act_on = 1;
    g_act_pi = pi;
    g_act_gi = gi;
    g_act_sel = ACT_START;                /* 默认停在"启动游戏"，想直接开的人按两下 A */
    g_act_ask = 0;
    printf("[操作] 打开弹层：%s / %s\n", p->name, p->games[gi].title);
    fflush(stdout);
    mark_dirty();
}

/* ---- 金手指二级列表（用户 2026-09-18）----------------------------------
   行 0 = 适用范围（仅本游戏 / 整个平台），行 1 = 总开关，
   行 2.. = 这一份 cht 里的**每条具体功能**（"具体什么功能的开关没显示"
   是浪总当晚追加的需求）。
   没有"候选文件"这一说 —— 文件是**自动匹配**的，玩家不需要选。
   库里匹配到哪一份、有几条，写在副标题里（draw_cheat_list 那段）。 */
static void act_cheat_refresh(Platform *p)
{
    g_act_chscope = 0;
    g_act_chon    = 0;
    g_act_chlib   = 0;
    g_act_chcount = 0;
    g_act_chvis   = 0;
    g_act_chfile[0] = '\0';
    g_act_chtop   = 0;

    const char *rp  = act_real_pid(p);
    const char *rom = cheat_rom_name(p);
    if (!rp || !rp[0] || !rom[0]) return;

    /* 匹配要用的另两个键：完整 rel（取子目录名）和显示名（⑤ NAMEex）。
       ⚠ rel 取不到就退回 rom —— 否则会从"按文件名找"退化成"找不到"。 */
    const char *rel = (g_act_gi >= 0 && g_act_gi < p->ngames)
                      ? p->games[g_act_gi].rel : NULL;
    const char *ttl = (g_act_gi >= 0 && g_act_gi < p->ngames)
                      ? p->games[g_act_gi].title : NULL;
    if (!rel || !rel[0]) rel = rom;

    /* ① 先扫库：填 g_act_chdesc[] / g_act_chcount / g_act_chvis，
          并把 bits 复位成"全开"。**必须排在 ② 前面** —— scan 会重置 bits，
          反过来会把玩家存的记录冲掉。 */
    g_act_chlib = cheat_scan(rp, rel, ttl, g_act_chfile, sizeof(g_act_chfile),
                             &g_act_chcount);

    /* ② 再读开关记录（游戏级优先，没有再看平台级）。 */
    int v = 0;
    char bits[CHEAT_MAX + 2];
    if (cheat_get(rp, rom, 0, &v, bits, sizeof(bits))) {
        g_act_chscope = 0; g_act_chon = v; cheat_apply_bits(bits);
    } else if (cheat_get(rp, rom, 1, &v, bits, sizeof(bits))) {
        g_act_chscope = 1; g_act_chon = v; cheat_apply_bits(bits);
    }

    /* 光标夹回合法范围（换了 cht / 条数变了的时候） */
    int nr = cheat_nrow();
    if (g_act_chsel >= nr) g_act_chsel = nr - 1;
    if (g_act_chsel < 0)     g_act_chsel = 0;
    act_cheat_scroll();
}

static void act_cheat_sync(Platform *p)
{
    act_cheat_refresh(p);
}

/* ---- 弹层里「金手指」这一项要不要显示（用户 2026-09-18）----------------
   原话："如果这个游戏没有金手指 弹层就不用显示金手指了"。
   判据 = 库里能不能找到这个游戏的 cht（cheat_find_file）。
   找不到就**整项隐藏** —— 以前会留一个点进去只有一句
   "库里没有这个平台的金手指"的废项，看着像坏了。

   ★ 带缓存：draw_act_popup() 每帧都问，而 cheat_find_file() 要
     opendir+readdir（一个平台目录里上千个 cht），绝不能每帧扫一遍。
     缓存键 = 弹层当前指向的 (平台, 游戏)，换游戏自动失效。
   ★ 用 cheat_find_file() 而不是 cheat_scan()：后者有副作用（重置 bits）。 */
static int g_act_chavail    = 0;          /* 1 = 有这个游戏的金手指 */
static int g_act_chavail_pi = -2, g_act_chavail_gi = -2;   /* -2 = 还没算过 */

static int act_cheat_avail(Platform *p)
{
    if (!p) return 0;
    if (g_act_chavail_pi == g_act_pi && g_act_chavail_gi == g_act_gi)
        return g_act_chavail;

    g_act_chavail_pi = g_act_pi;
    g_act_chavail_gi = g_act_gi;
    g_act_chavail    = 0;

    /* 弹层还没指向有效游戏（刚开、或 --act 之前的空档）—— 直接算"没有"。
       ⚠ 这句必须在下面那个 printf 之前：它要按 g_act_gi 取游戏名。 */
    if (g_act_gi < 0 || g_act_gi >= p->ngames) return 0;

    const char *rp  = act_real_pid(p);
    const char *rom = cheat_rom_name(p);
    const char *rel = p->games[g_act_gi].rel;      /* 完整相对路径（取子目录名用） */
    const char *ttl = p->games[g_act_gi].title;    /* 显示名（⑤ NAMEex 用） */
    if (!rel || !rel[0]) rel = rom;
    if (rp && rp[0] && rom[0]) {
        char fn[256];
        g_act_chavail = cheat_find_file(rp, rel, ttl, fn, sizeof(fn));
        printf("[金手指] 弹层显不显示这一项：%s / %s -> %s\n",
               p->name, p->games[g_act_gi].title,
               g_act_chavail ? "有，显示" : "库里没有，整项隐藏");
        fflush(stdout);
    }
    return g_act_chavail;
}

/* 弹层实际显示几行；以及"第 r 行 ↔ 第几个 ACT_* 项"的互相换算。
   藏掉的那一项在它后面的项整体上移一格（金手指是第 5 项，删除第 6 项）。 */
static int act_rows(Platform *p)
{
    return ACT_N - (act_cheat_avail(p) ? 0 : 1);
}
static int act_row2idx(Platform *p, int r)
{
    if (!act_cheat_avail(p) && r >= ACT_CHEAT) return r + 1;
    return r;
}
static int act_idx2row(Platform *p, int i)
{
    if (!act_cheat_avail(p) && i == ACT_CHEAT) return -1;   /* 这一项被藏了 */
    if (!act_cheat_avail(p) && i >  ACT_CHEAT) return i - 1;
    return i;
}
/* 光标所在的项被藏了（从有金手指的游戏移到没有的）就挪回"启动游戏"。
   act_open() / --act 之后、以及 draw 每帧都会走一遍，保证不变鬼。 */
static void act_sel_fix(Platform *p)
{
    if (!p || g_act_sel < 0 || g_act_sel >= ACT_N) { g_act_sel = ACT_START; return; }
    if (act_idx2row(p, g_act_sel) < 0) g_act_sel = ACT_START;
}

/* 切"适用范围"那一行：换级之后必须**回读那一级的值**，否则显示的还是上一级的。
   读不到就回落到"关 + 全关"（那一级没配过 = 出厂状态，条目一条都不勾）。 */
static void act_cheat_flip_scope(Platform *p)
{
    g_act_chscope = g_act_chscope ? 0 : 1;
    int v = 0;
    const char *rp  = act_real_pid(p);
    const char *rom = cheat_rom_name(p);
    char bits[CHEAT_MAX + 2];
    if (rp && rp[0] && rom[0] &&
        cheat_get(rp, rom, g_act_chscope, &v, bits, sizeof(bits))) {
        g_act_chon = v;
        cheat_apply_bits(bits);
    } else {
        g_act_chon = 0;
        for (int i = 0; i < g_act_chvis; i++) g_act_chbits[i] = '0';
        g_act_chbits[g_act_chvis] = '\0';
    }
}

/* 滚动钳制：光标永远留在可视区里。算法与 act_shader_scroll() 同一套
   （那个是 2026-09-16 截图特意调过的）。
   ★ CHEAT_VIS = 9 是**实测**出来的：2026-09-18 截图量到面板可用高度
     （hmax − head_h − foot_h ≈ 488px）除以行高（≈50px）能摆 9 行多点，
     所以取 9，比 CORE_VIS=10 保守一档。draw 那边还会按剩余高度再缩一次，
     两边不会打架。 */
#define CHEAT_VIS 9
static void act_cheat_scroll(void)
{
    int n = cheat_nrow();
    if (n <= CHEAT_VIS) { g_act_chtop = 0; return; }
    if (g_act_chsel < g_act_chtop) g_act_chtop = g_act_chsel;
    if (g_act_chsel > g_act_chtop + CHEAT_VIS - 1)
        g_act_chtop = g_act_chsel - CHEAT_VIS + 1;
    if (g_act_chtop > n - CHEAT_VIS) g_act_chtop = n - CHEAT_VIS;
    if (g_act_chtop < 0) g_act_chtop = 0;
}

/* 二级列表的按键。返回 1 = 已消费（和 act_key 一个约定）。 */
static int act_cheat_key(Platform *p, SDL_Keycode k)
{
    int nr = cheat_nrow();

    switch (k) {
    case SDLK_UP:
    case SDLK_DOWN:
        g_act_chsel += (k == SDLK_DOWN) ? 1 : -1;
        if (g_act_chsel < 0)   g_act_chsel = nr - 1;     /* 环绕，和核心/着色器一致 */
        if (g_act_chsel >= nr) g_act_chsel = 0;
        act_cheat_scroll();
        mark_dirty();
        return 1;

    case SDLK_LEFT:
    case SDLK_RIGHT:
        /* 跟核心列表第 0 行一个手感：**就地切换，不关列表** —— 切完接着看开关。
           行 0 换适用范围、行 1 翻总开关、行 2+ 翻那一条具体功能。
           ★ 这里只改内存不写盘，A 才落盘（提示写的就是"←→ 切换 · A 确认"）。 */
        if (g_act_chsel == 0) {
            act_cheat_flip_scope(p);
        } else if (g_act_chsel == 1) {
            g_act_chon = g_act_chon ? 0 : 1;
        } else {
            int i = g_act_chsel - 2;
            if (i >= 0 && i < g_act_chvis)
                g_act_chbits[i] = (g_act_chbits[i] == '0') ? '1' : '0';
        }
        mark_dirty();
        return 1;

    case SDLK_ESCAPE:
    case SDLK_BACKSPACE:
        g_act_cheat = 0;                     /* 只退二级列表，主弹层还开着 */
        mark_dirty();
        return 1;

    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        break;

    default:
        return 1;                            /* 模态：别的键吞掉 */
    }

    if (!p || g_act_gi < 0 || g_act_gi >= p->ngames) {
        g_act_cheat = 0;
        mark_dirty();
        return 1;
    }

    /* A 落在"适用范围"上 = 跟左右键一样，就地切换 */
    if (g_act_chsel == 0) { act_cheat_flip_scope(p); return 1; }

    const char *rp  = act_real_pid(p);
    const char *rom = cheat_rom_name(p);
    if (!rp || !rp[0] || !rom[0]) { g_act_cheat = 0; mark_dirty(); return 1; }

    /* standalone 平台（NDS/PSP/SS 那些）不走 retroarch，金手指挂不上去。
       照写不误是没意义的 —— 宿主那侧也会因为找不到 *_libretro.so 而跳过，
       这里直说一句，免得玩家以为开了却没生效。 */
    const char *core = act_cur_core(p);
    if (!core || !core[0]) {
        set_flash("这个平台不走 RetroArch，金手指用不了");
        g_act_cheat = 0;
        mark_dirty();
        return 1;
    }

    /* ---- A 落在具体功能那一行：翻这一位 + 落盘，**但不关列表** ----
       玩家通常是连着调好几条（"无限命"要、"双激光"不要），每条都关一次
       列表太难受。 */
    if (g_act_chsel >= 2) {
        int i = g_act_chsel - 2;
        if (i >= g_act_chvis) { mark_dirty(); return 1; }
        g_act_chbits[i] = (g_act_chbits[i] == '0') ? '1' : '0';
        if (cheat_set(rp, rom, g_act_chscope, g_act_chon, g_act_chbits)) {
            printf("[金手指] 条目%d「%s」-> %s（%s / %s）\n",
                   i, g_act_chdesc[i], g_act_chbits[i] == '0' ? "关" : "开", rp, rom);
            fflush(stdout);
            set_flash(g_act_chon ? "已记录，下次启动这个游戏生效"
                                 : "已记录（总开关还关着，先把它打开）");
        } else {
            fprintf(stderr, "[金手指] 写不了 %s\n", CHEATS_PATH);
            fflush(stderr);
            set_flash("保存失败：写不了 cheats.txt");
        }
        mark_dirty();
        return 1;
    }

    /* ---- A 落在总开关上：翻 + 落盘，写完回主弹层 ----
       （跟核心/着色器"选完就回"一致） */
    int nv = g_act_chon ? 0 : 1;
    if (cheat_set(rp, rom, g_act_chscope, nv, g_act_chbits)) {
        g_act_chon = nv;
        printf("[金手指] %s %s：%s（%s，匹配 %s / %d 条）\n",
               nv ? "开启" : "关闭", rom,
               g_act_chscope ? "整个平台" : "仅本游戏", rp,
               g_act_chlib ? g_act_chfile : "未找到", g_act_chcount);
        fflush(stdout);
        if (!nv) {
            set_flash("已关闭，下次启动这个游戏不加载金手指");
        } else if (!g_act_chlib) {
            set_flash("已记录（库里没找到匹配的金手指，等于没开）");
        } else {
            /* ★ 条目默认**一条都不勾**（浪总 2026-09-18 晚："点进去的功能
               不要默认都是开启的，容易出bug"）。所以光开总开关是没用的 ——
               这里必须说清楚，否则玩家进游戏发现"金手指没反应"。 */
            int any = 0;
            for (int i = 0; i < g_act_chvis; i++)
                if (g_act_chbits[i] == '1') { any = 1; break; }
            if (any) set_flash("已记录，下次启动这个游戏生效");
            else     set_flash("已开启，但一条功能都没勾 —— 把想要的按 A 打开");
        }
    } else {
        fprintf(stderr, "[金手指] 写不了 %s\n", CHEATS_PATH);
        fflush(stderr);
        set_flash("保存失败：写不了 cheats.txt");
    }

    g_act_cheat = 0;
    mark_dirty();
    return 1;
}

/* 弹层开着时的按键。返回 1 = 已消费（调用方直接 return）。
   弹层是**模态**的：没认出来的键也一律吃掉，免得底下页面跟着动。 */
static int act_key(SDL_Keycode k)
{
    Platform *p = act_plat();

    /* 二级列表开着就全归它：它的行数跟主弹层不一样（还多一组左右键），
       必须在下面那个按 ACT_N 取模的光标循环**之前**分流出去。 */
    if (g_act_shader) return act_shader_key(p, k);
    if (g_act_cheat)  return act_cheat_key(p, k);
    if (g_act_core)   return act_core_key(p, k);

    switch (k) {
    case SDLK_UP:
    case SDLK_DOWN:
        /* 一动光标就撤销"待确认" —— 否则选到删除上按了 A、又移到别处，
           回来再按 A 就会直接删掉，白瞎一道确认。 */
        g_act_ask = 0;
        /* ★ 按**显示行**走位，不能按项号取模（2026-09-18）：金手指项藏掉时
           项号是有洞的（第 5 项没了，删除顶上来），按 ACT_N=7 取模会走到
           那个画不出来的项上 —— 表现成"按一下 ↓，选中框凭空消失"。 */
        {
            int nr  = act_rows(p);
            int row = act_idx2row(p, g_act_sel);
            if (row < 0) row = 0;
            if (nr > 0)  row = (row + (k == SDLK_DOWN ? 1 : nr - 1)) % nr;
            g_act_sel = act_row2idx(p, row);
        }
        mark_dirty();
        return 1;

    case SDLK_ESCAPE:
    case SDLK_BACKSPACE:
        if (g_act_ask) { g_act_ask = 0; mark_dirty(); return 1; }   /* 先撤确认 */
        g_act_on = 0;
        mark_dirty();
        return 1;

    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        break;

    default:
        return 1;                          /* 模态：别的键吞掉 */
    }

    if (!p || g_act_gi < 0 || g_act_gi >= p->ngames) {
        g_act_on = 0; mark_dirty(); return 1;
    }

    /* ---- 启动游戏 ---- */
    if (g_act_sel == ACT_START) {
        g_act_on = 0;
        printf("[操作] 启动 %s\n", p->games[g_act_gi].title);
        fflush(stdout);
        if (launch_game(p, g_act_gi)) g_quit = 2;
        return 1;
    }

    /* ---- 收藏游戏：切换 ---- */
    if (g_act_sel == ACT_FAV) {
        const char *rel = p->games[g_act_gi].rel;
        int on = !fav_has(p->pid, rel);
        if (fav_set(p->pid, rel, on)) {
            printf("[收藏] %s %s\n", on ? "加入" : "取消", p->games[g_act_gi].title);
            favorites_load();              /* 立刻反映到"我的收藏"平台 */
            build_view();                  /* 0→1 / 1→0 会让收藏卡片出现或下架 */
        } else {
            fprintf(stderr, "[收藏] 写不了 %s\n", FAV_PATH);
        }
        fflush(stdout);
        mark_dirty();
        return 1;
    }

    /* ---- 更换模拟器核心：展开二级列表（不是就地切换）---- */
    if (g_act_sel == ACT_CORE) {
        int nc = 0;
        act_core_list(p, &nc);
        g_act_core = 1;
        act_core_sync(p);
        printf("[核心] 打开核心列表：%s / %s（候选 %d 个，当前 %s）\n",
               p->name, p->games[g_act_gi].title, nc,
               g_act_ocur[0] ? g_act_ocur : "(自动)");
        fflush(stdout);
        mark_dirty();
        return 1;
    }

    /* ---- 着色器效果：展开二级列表（不是就地切换）---- */
    if (g_act_sel == ACT_SHADER) {
        g_act_shader = 1;
        act_shader_sync(p);
        const char *core = act_cur_core(p);
        printf("[着色器] 打开列表：%s / %s（核心 %s，当前 %s）\n",
               p->name, p->games[g_act_gi].title,
               (core && core[0]) ? core : "无",
               g_act_scur[0] ? g_act_scur : "(关闭)");
        fflush(stdout);
        mark_dirty();
        return 1;
    }

    /* ---- 退出自动存档：就地切换（开关型，不像核心那样展开二级列表）---- */
    if (g_act_sel == ACT_AUTOSV) {
        /* ⚠ 必须落到**真实平台**上：在「游玩历史 / 我的收藏」里打开的弹层，
           p->pid 是 history/favorite —— 那个名字写进 autosave.txt 一点用没有
           （宿主拿它去配 es_systems.cfg 会查不到，自动存档静默失效）。 */
        const char *rp = act_real_pid(p);
        if (!rp || !rp[0]) rp = p->pid;
        int on = !autosave_has(rp);
        if (autosave_set(rp, on))
            printf("[自动存档] %s %s（平台 %s）\n",
                   on ? "开启" : "关闭", p->name, rp ? rp : "?");
        else
            fprintf(stderr, "[自动存档] 写不了 %s\n", AUTOSAVE_PATH);
        fflush(stdout);
        mark_dirty();
        return 1;
    }

    /* ---- 金手指：展开二级列表（不是就地切换）---- */
    if (g_act_sel == ACT_CHEAT) {
        g_act_cheat = 1;
        /* 默认停在"开关"那一行 —— 点进这一页十有八九就是想开关它 */
        g_act_chsel = 1;
        act_cheat_sync(p);
        printf("[金手指] 打开：%s / %s（%s，库中 %s / %d 条）\n",
               p->name, p->games[g_act_gi].title,
               g_act_chon ? "开" : "关",
               g_act_chlib ? g_act_chfile : "未找到", g_act_chcount);
        fflush(stdout);
        mark_dirty();
        return 1;
    }

    /* ---- 删除游戏：二次确认 ---- */
    if (!g_act_ask) {                      /* 第一次 A：只进入待确认，不删 */
        g_act_ask = 1;
        mark_dirty();
        return 1;
    }
    if (act_do_delete(p, g_act_gi)) {
        g_act_on = 0;
        g_act_ask = 0;
        favorites_load();                  /* 删掉的可能正躺在收藏里 */
        history_load();                    /* 历史里也可能有它 */
        build_view();                      /* 计数变了，被删空的平台要下架 */
        if (g_gsel >= p->ngames) g_gsel = p->ngames > 0 ? p->ngames - 1 : 0;
        if (g_gsel < 0) g_gsel = 0;
    } else {
        g_act_ask = 0;
    }
    mark_dirty();
    return 1;
}

/* ---- 游戏介绍（弹层右侧那块，用户 2026-09-16）--------------------------
 * 数据是宿主写的 <索引目录>/desc/<pid>.txt：**第 N 行 = 该平台第 N 个游戏**，
 * 没介绍的游戏是空行（见 vagaretro-shell.py 的 write_index）。天马卡上就是
 * metadata.pegasus.txt 的 `description:` 字段（中文长文，常用字面 `\n` 断行）。
 *
 * 为什么不塞进 index.tsv：一条描述动辄上千字，全库好几 MB，而只有**打开
 * 弹层**时才需要看它 —— 按需读一行的代价可以忽略，索引也能保持小巧。
 * 目录名由 main() 从 argv[1] 的索引路径推导（换成 --index 也跟得上）。
 * ---------------------------------------------------------------------- */
static char g_desc_dir[288] = "/opt/vagaretro/desc";
/* 一条描述最多 Python 侧截的 1000 个汉字（UTF-8 最长 3 字节/字 → 3000 字节），
   留够余量。原来只有 1600，长描述会被 snprintf 截掉半截。 */
static char g_desc_buf[4096];
static int  g_desc_pi = -1;
static int  g_desc_gi = -1;

/* 取第 gi 个游戏的介绍（带一级缓存：同一个游戏反复画不重复读盘）。 */
static const char *game_desc(const Platform *p, int gi)
{
    int pi = (int)(p - IX.p);
    if (pi == g_desc_pi && gi == g_desc_gi) return g_desc_buf;

    g_desc_pi = pi;
    g_desc_gi = gi;
    g_desc_buf[0] = '\0';
    if (!g_desc_dir[0]) return g_desc_buf;

    char path[380];
    snprintf(path, sizeof(path), "%s/%s.txt", g_desc_dir, p->pid);

    FILE *f = fopen(path, "r");
    if (!f) return g_desc_buf;                 /* 原卡没有 desc 目录 —— 正常 */

    /* ⚠⚠ 千万别拿 fgets 的"调用次数"当行号。
       fgets 的语义是「读满缓冲 **或** 遇到换行就返回」—— 长描述会被**切成
       好几段**，每段都算一次调用。实测（2026-09-16）：
         · 宿主写 desc 时每条截 1000 个汉字 = UTF-8 最多 3000 字节；
         · 缓冲只有 1800 → SFC 第 5 条（1991 字节）被切成 2 段；
         · 于是**从第 6 个游戏开始，介绍的整个行号永久错位 1**，
           越往后错得越多 —— ARCADE 有 164 条超长，末尾能错到 160 多位，
           等于整个平台的介绍全是别人的（用户报"介绍和游戏对不上"）。
       所以：n 只在「上一段是以换行结束的」时才递增；缓冲也放大到能一次
       装下一条。                           —— 2026-09-16 修 */
    char line[4096];
    int  n = 0;
    int  cont = 0;                 /* 1 = 这一段是上一行的续段，不算新行 */
    while (fgets(line, sizeof(line), f)) {
        size_t L = strlen(line);
        if (!cont && n++ == gi) {
            while (L && (line[L - 1] == '\n' || line[L - 1] == '\r'))
                line[--L] = '\0';
            snprintf(g_desc_buf, sizeof(g_desc_buf), "%s", line);
            break;
        }
        cont = !(L > 0 && line[L - 1] == '\n');
    }
    fclose(f);
    return g_desc_buf;
}

/* 这个平台**有没有**介绍文件。布局拿它当开关，为的是让面板宽度**稳定**：
   同一平台里某条游戏恰好没描述时，不该让整个弹层忽宽忽窄（右栏显示
   "暂无介绍"就行）。原卡上压根没有 desc/ 目录 → 永远走单栏老版式。 */
static int desc_avail(const Platform *p)
{
    static char cpid[80] = "";
    static int  cval = 0;
    if (cpid[0] && strcmp(cpid, p->pid) == 0) return cval;

    snprintf(cpid, sizeof(cpid), "%s", p->pid);
    cval = 0;
    char path[380];
    snprintf(path, sizeof(path), "%s/%s.txt", g_desc_dir, p->pid);
    FILE *f = fopen(path, "r");
    if (f) { cval = 1; fclose(f); }
    return cval;
}

/* 自动折行画一段文本，最多画 maxh 高；放不下的末行截断补 "…"。
   中文逐字断（TTF_SizeUTF8 量出来的就是 UTF-8 字符宽度），够用。
   ⚠ 天马把换行写成**字面** `\n`（反斜杠 + n，不是真换行），一并当换行处理。 */
/* 返回值 = **实际画了几行**；`dry` 非 0 时只数不画 ——
   draw_act_popup() 的右栏靠它把介绍整段做垂直居中。 */
static int draw_para(TTF_Font *f, const char *s, int x, int y,
                     int w, int maxh, Uint32 col, int dry)
{
    int lines = 0;
    if (!f || !s || !*s || w <= 0 || maxh <= 0) return 0;
    int lh = TTF_FontLineSkip(f);
    if (lh <= 0) lh = 24;
    int maxl = maxh / lh;
    if (maxl < 1) maxl = 1;

    static char line[1200];
    int ln = 0, li = 0;
    int tw = 0, th = 0;
    const char *q = s;
    line[0] = '\0';

    for (;;) {
        int brk = 0, adv = 1;

        if (!*q) {
            brk = 1;
        } else if (q[0] == '\\' && q[1] == 'n') {
            brk = 1; adv = 2;
        } else if (q[0] == '\n' || q[0] == '\r') {
            brk = 1; adv = 1;
        } else {
            int cl = 1;
            while (q[cl] && ((unsigned char)q[cl] & 0xC0) == 0x80) cl++;
            if (ln + cl >= (int)sizeof(line) - 1) {
                brk = 1; adv = 0;              /* 缓冲满了，换行重来 */
            } else {
                memcpy(line + ln, q, cl);
                ln += cl;
                line[ln] = '\0';
                tw = 0;
                TTF_SizeUTF8(f, line, &tw, &th);
                if (tw > w && ln > cl) {       /* 这一行装不下了 */
                    ln -= cl;
                    line[ln] = '\0';
                    brk = 1; adv = 0;          /* 这个字留到下一行 */
                } else {
                    adv = cl;
                }
            }
        }

        if (brk) {
            int lw = 0, lhh = 0;
            if (li >= maxl) {                  /* 画满 —— 末行必须带 "…" */
                char fit[1200];
                int ew = 0, eh = 0;
                if (TTF_SizeUTF8(f, "…", &ew, &eh) != 0) ew = 0;
                /* 先按"留出省略号的位置"截、再把省略号拼上去。
                   ⚠ 不能直接拿 line 去调 ellipsize —— line 本来就是"还没超宽"
                     的那一截，ellipsize 见它装得下会原样返回、一个省略号都不
                     加，用户就以为介绍到这儿就完了（第一版就是这样）。 */
                ellipsize(f, line, w - ew, fit, sizeof(fit));
                size_t fl = strlen(fit);
                /* ellipsize 自己就可能补过一个 "…"，别补成 "……" */
                int has_ell = (fl >= 3 &&
                               (unsigned char)fit[fl - 3] == 0xE2 &&
                               (unsigned char)fit[fl - 2] == 0x80 &&
                               (unsigned char)fit[fl - 1] == 0xA6);
                if (!has_ell && fl + 4 < sizeof(fit)) strcat(fit, "…");
                if (!dry && fit[0] && TTF_SizeUTF8(f, fit, &lw, &lhh) == 0)
                    blit(REN, bake_cached(REN, f, fit, col, &lw, &lhh),
                         x, y + li * lh, lw, lhh);
                lines = li + 1;
                break;
            }
            if (!dry && line[0] && TTF_SizeUTF8(f, line, &lw, &lhh) == 0)
                blit(REN, bake_cached(REN, f, line, col, &lw, &lhh),
                     x, y + li * lh, lw, lhh);
            li++;
            ln = 0;
            line[0] = '\0';
            if (!*q) { lines = li; break; }
        }
        q += adv;
    }
    return lines;
}

/* 玻璃拟态弹层。背景用 glass_panel() —— 那是设置页同一套**真·背景模糊**
   （读像素 → 1/4 降采样 → 两次盒式模糊），不是半透明色块。 */
static void draw_act_popup(void)
{
    if (g_act_pi < 0 || g_act_pi >= IX.np) { g_act_on = 0; return; }
    Platform *p = &IX.p[g_act_pi];
    if (g_act_gi < 0 || g_act_gi >= p->ngames) { g_act_on = 0; return; }

    /* 压暗整屏，**连底栏一起** —— 模态弹层要盖住一切才像样 */
    setc(REN, 0x000000, 132);
    SDL_Rect full = { 0, 0, g_win_w, g_win_h };
    SDL_RenderFillRect(REN, &full);

    /* 二级列表整个换一套版式（行数是变的），所以压在压暗层之后直接交给它。
       放在这里而不是函数开头：那层压暗是两边共用的，提早 return 会让
       二级列表下面亮着一片，模态感就没了。 */
    if (g_act_shader) { draw_shader_list(); return; }
    if (g_act_cheat)  { draw_cheat_list();  return; }
    if (g_act_core)   { draw_core_list();   return; }

    /* ACT_LABEL / ACT_FOOT 已提到文件作用域 —— 二级列表 draw_core_list()
       也要用同一套文案，塞在这里它够不着。 */

    /* ==================================================================
       先量后排 —— 弹层的宽高**全部由实际文字算出来**。

       原来是写死的设计像素 S(620)/S(106)/S(90)。设计稿 1920 宽、屏幕
       1024 → g_scale≈0.533，于是实测只有 331px 宽、标题头 56px 高；
       而 H1 名字（FS_UI(64)→34px，一行≈40px）+ 副标题（FS_PSUB）一共
       要 72px，根本塞不进 56px —— 副标题被第一行盖住、分隔线横穿它。
       字号再往上调只会更糟（2026-09-15 用户报"弹层太小了 字体都溢出了"）。

       写死的布局在**可变字号**的前端里天生站不住：任何新字号档位都要
       回来改常数。这里改成量出来再加，一劳永逸。
       ================================================================== */
    const int PADX = S(34);          /* 弹层左右内边距 */
    const int ROWIN = S(16);         /* 行框距弹层左右 */
    const int LBX  = S(44);          /* 标签距行框左 */
    const int VBR  = S(24);          /* 右侧状态字距行框右 */
    const int GAP  = S(26);          /* 标签与状态字之间的最小空隙 */

    int tw = 0, th = 0;
    if (TTF_SizeUTF8(F_H1, p->games[g_act_gi].title, &tw, &th) != 0) { tw = 0; th = 0; }
    int sw = 0, sh = 0;
    if (TTF_SizeUTF8(F_PSUB, p->name, &sw, &sh) != 0) { sw = 0; sh = 0; }

    /* 三项的标签宽度（取最宽那项） */
    int lw_max = 0, lh_max = 0;
    for (int i = 0; i < ACT_N; i++) {
        int a = 0, b = 0;
        if (TTF_SizeUTF8(F_SETN, ACT_LABEL[i], &a, &b) == 0) {
            if (a > lw_max) lw_max = a;
            if (b > lh_max) lh_max = b;
        }
    }
    /* 右侧状态字：收藏/删除各自两个文案都量一遍，取最宽 ——
       否则"从内存卡删除"六个字会把标签挤掉。 */
    int vw_max = 0, vh_max = 0;
    {
        static const char *const VS[6] = {
            "未收藏", "已收藏", "从内存卡删除", "再按一次确认",
            "已开启", "已关闭"
        };
        for (int i = 0; i < 6; i++) {
            int a = 0, b = 0;
            if (TTF_SizeUTF8(F_SETV, VS[i], &a, &b) == 0) {
                if (a > vw_max) vw_max = a;
                if (b > vh_max) vh_max = b;
            }
        }
    }
    /* 「更换模拟器核心」那行的右侧显示**当前核心名**（有覆盖就是覆盖的那个，
       否则是平台默认）。核心名长短不一，不量的话长名字会把左边标签挤掉 ——
       这就是这个弹层当初从"写死宽度"改成"先量后排"的原因。 */
    {
        const char *cv[2] = { g_act_ocur, p->core };
        for (int i = 0; i < 2; i++) {
            int a = 0, b = 0;
            if (!cv[i] || !cv[i][0]) continue;
            if (TTF_SizeUTF8(F_SETV, cv[i], &a, &b) == 0) {
                if (a > vw_max) vw_max = a;
                if (b > vh_max) vh_max = b;
            }
        }
    }
    /* 底部提示：三条文案取最宽（"再按一次 A 确认删除，B 取消"更长） */
    int fw_max = 0, fh_max = 0;
    for (int i = 0; i < 3; i++) {
        int a = 0, b = 0;
        if (TTF_SizeUTF8(F_SETF, ACT_FOOT[i], &a, &b) == 0) {
            if (a > fw_max) fw_max = a;
            if (b > fh_max) fh_max = b;
        }
    }

    /* ---- 定宽：下限是屏幕的 46%，内容更宽就跟着涨，再长也得留边 ---- */
    int w = g_win_w * 46 / 100;
    int need = (tw > sw ? tw : sw) + PADX * 2;
    if (need > w) w = need;
    need = fw_max + PADX * 2;
    if (need > w) w = need;
    need = ROWIN + LBX + lw_max + GAP + vw_max + VBR + ROWIN;
    if (need > w) w = need;
    int wmax = g_win_w - S(72);
    if (w > wmax) w = wmax;

    /* ---- 右侧游戏介绍（用户 2026-09-16）--------------------------------
       有介绍才变两栏：左栏就是原来那块菜单（宽度 w 一点不改），右栏放介绍，
       总宽 = 左栏 + 间隙 + 右栏 + 两侧内边距。
       ★ 没介绍的卡（原卡上根本没有 pegasus 元数据）**保持单栏**，宽度和
         布局跟以前逐像素一致 —— 这个功能是新卡专用，不能动老卡的观感。
       挤不出 260px 就不显示（比硬塞一条每行三个字的窄条好看）。 */
    const char *desc = game_desc(p, g_act_gi);
    int wl = w;                       /* 左栏（菜单）宽度 */
    int wr = 0;                       /* 右栏（介绍）宽度 */
    if (desc_avail(p)) {
        const int dgap = S(28);
        const int dpad = PADX * 2;
        /* ⚠ S() 是**设计像素 → 实际像素**的缩放（本机 ≈0.49），不是像素值：
           这里写 S(720) 落地才 ≈350px。第一版写了 S(360)、只有 176px 宽，
           一行才 12 个汉字，读起来像条竖着的电报（截图核对时发现的）。 */
        wr = S(720);
        if (wl + dgap + wr + dpad > wmax) wr = wmax - wl - dgap - dpad;
        if (wr >= S(260)) w = wl + dgap + wr + dpad;
        else              wr = 0;
    }

    /* ---- 定高：每一段都按量出来的字高累加，不再有"魔法高度" ----
       ★ 2026-09-16 加「加载存档 / 保存存档」时项目一度涨到 8 条，原来那句
         "h 超了就一刀切到屏幕内" 会**把最后两行连底部提示一起裁掉**
         （和 draw_shader_list 当初踩的是同一个坑：设 12 行时最后一行被底边
         截断、整条按键提示不见了）。这里改成**压行高**：项目数是死的
         （靠 ↑↓ 循环，没有滚动），所以只能从行高里省。
         下限 = 字高 + 14px，再挤就真糊了；真到那一步宁可略微超屏收尾，
         也不画成一坨分不清的行。
       ★ 2026-09-17 删掉「加载存档」后回到 6 条，这段逻辑照样保留 ——
         它跟项数无关，保证的是"任何项数都不裁切"。 */
    const int rad    = S(30);   /* 圆角比原来大一圈，玻璃面板更柔 */
    const int head_h = S(26) + th + S(10) + sh + S(18);
    int row_h  = (lh_max > vh_max ? lh_max : vh_max) + S(28) + S(10);
    int row_in = row_h - S(14);
    const int foot_h = S(18) + fh_max + S(16);
    const int hmax   = g_win_h - S(72);
    /* ★ 行数是**变的**：库里没这个游戏的金手指就不显示那一项（2026-09-18）。
       所以不能再用常数 ACT_N —— 高度、循环、光标走位三处全得跟着 nrow 走，
       否则要么底部被裁、要么空出一行。 */
    const int nrow = act_rows(p);
    if (head_h + row_h * nrow + foot_h > hmax) {
        int rh2  = (hmax - head_h - foot_h) / nrow;
        int rmin = (lh_max > vh_max ? lh_max : vh_max) + S(14);
        if (rh2 < rmin) rh2 = rmin;
        row_h  = rh2;
        row_in = row_h - S(14);              /* 行框比行距小一圈 = 行间留白 */
    }
    int h = head_h + row_h * nrow + foot_h;
    /* 极端字号（fs_ui 到 160%）下的最后一道保险：宁可裁一点，也不能让
       面板上半截跑到屏幕外 —— 顶部出去之后标题就看不见了。 */
    if (h > g_win_h - S(36)) h = g_win_h - S(36);
    SDL_Rect rc = { (g_win_w - w) / 2, (g_win_h - h) / 2, w, h };

    glass_panel(rc, rad, C_PANEL, 232);
    draw_round(REN, rc, rad, C_LINE, 96);              /* 一圈细边（72→96，暗底上太虚了）*/

    /* --- 标题：游戏名（只有真放不下才收省略号）--- */
    char nm[256];
    ellipsize(F_H1, p->games[g_act_gi].title, w - PADX * 2, nm, sizeof(nm));
    int tw2 = 0, th2 = 0;
    blit(REN, bake_cached(REN, F_H1, nm, C_TEXT, &tw2, &th2),
         rc.x + PADX, rc.y + S(26), tw2, th2);

    /* 副标题：平台名 —— 光看游戏名有时分不清是哪个机种的同名作品 */
    char sub[256];
    ellipsize(F_PSUB, p->name, w - PADX * 2, sub, sizeof(sub));
    int sw2 = 0, sh2 = 0;
    blit(REN, bake_cached(REN, F_PSUB, sub, C_SUBTEXT, &sw2, &sh2),
         rc.x + PADX, rc.y + S(26) + th2 + S(10), sw2, sh2);

    /* 标题下一条分隔线。head_h 是算出来的，所以这条线永远落在副标题下面 */
    SDL_Rect sep = { rc.x + S(22), rc.y + head_h - S(10), w - S(44), 1 };
    setc(REN, C_LINE, 150);
    SDL_RenderFillRect(REN, &sep);

    /* --- 那一列菜单项（宽度用 wl，两栏时不会压到右边的介绍）--- */
    /* 光标所在的项要是被藏了（nrow 刚变小），先挪回"启动游戏"再画 ——
       否则这一帧会没有任何一行是选中态，看着像光标丢了。 */
    act_sel_fix(p);
    int y = rc.y + head_h;
    for (int r = 0; r < nrow; r++) {
        int i      = act_row2idx(p, r);          /* 行号 -> ACT_* 项号 */
        SDL_Rect row = { rc.x + ROWIN, y, wl - ROWIN * 2, row_in };
        int on     = (i == g_act_sel);
        int danger = (i == ACT_DEL);

        /* 选中底：删除那项用危险色，其它用强调色。
           待确认时加重 —— 红得很明显，不会误以为只是普通选中。 */
        if (on) {
            int a = (danger && g_act_ask) ? 215 : 150;
            draw_round(REN, row, S(12), danger ? C_DANGER : C_ACCENT, a);
        }

        Uint32 fg = on ? 0xffffff : (danger ? C_DANGER : C_TEXT);

        /* 左边一条小色块当图标位 —— **只在选中行显示**。
           原来每行都挂一条，未选中时那一列蓝竖线看着像排打字光标，很吵；
           未选中的行靠文字层次和留白区分就够了。
           选中底色本身就是 C_ACCENT / C_DANGER，所以 chip 一律反白。 */
        if (on) {
            SDL_Rect chip = { row.x + S(18), row.y + (row.h - S(36)) / 2, S(6), S(36) };
            draw_round(REN, chip, S(3), 0xffffff, 240);
        }

        /* 右侧状态字先量 —— 标签的可用宽度就是"行宽 − 两侧留白 − 状态字" */
        char vbuf[128];
        vbuf[0] = '\0';
        const char *val = NULL;
        if (i == ACT_FAV)
            val = fav_has(p->pid, p->games[g_act_gi].rel) ? "已收藏" : "未收藏";
        else if (i == ACT_CORE) {
            /* 核心名可能是 "<模拟器>:<核心>"（跨模拟器换核心），格式化一下再显示 */
            const char *cv = g_act_ocur[0] ? g_act_ocur
                            : ((p->core && p->core[0]) ? p->core : "自动");
            core_label_into(cv, vbuf, sizeof(vbuf));
            val = vbuf;
        }
        else if (i == ACT_AUTOSV) {
            const char *rp = act_real_pid(p);
            val = autosave_has(rp ? rp : p->pid) ? "已开启" : "已关闭";
        }
        else if (i == ACT_DEL)
            val = g_act_ask ? "再按一次确认" : "从内存卡删除";

        int vw = 0, vh = 0;
        char vfit[64];
        vfit[0] = '\0';
        if (val) {
            ellipsize(F_SETV, val, row.w - LBX - S(60), vfit, sizeof(vfit));
            if (TTF_SizeUTF8(F_SETV, vfit, &vw, &vh) != 0) { vw = 0; vh = 0; }
        }

        char lab[64];
        ellipsize(F_SETN, ACT_LABEL[i],
                  row.w - LBX - VBR - (vw ? vw + GAP : 0), lab, sizeof(lab));
        int lw = 0, lh = 0;
        blit(REN, bake_cached(REN, F_SETN, lab, fg, &lw, &lh),
             row.x + LBX, row.y + (row.h - lh) / 2, lw, lh);

        if (val) {
            /* 选中行上的状态字必须反白。原来一律给 C_DANGER/C_SUBTEXT ——
               而"删除游戏"选中时底色本身就是 C_DANGER，红字压红底，
               "从内存卡删除"几乎看不清（2026-09-15 截图核对时发现的）。
               危险感由**整行底色**承担，文字只管读得清。 */
            Uint32 vc = on ? 0xffffff : ((i == ACT_DEL) ? C_DANGER : C_SUBTEXT);
            blit(REN, bake_cached(REN, F_SETV, vfit, vc, &vw, &vh),
                 row.x + row.w - VBR - vw, row.y + (row.h - vh) / 2, vw, vh);
        }
        y += row_h;
    }

    /* --- 右栏：游戏介绍 ---
       竖分隔线跟标题下那条横线同一套颜色/透明度，两栏才像"一体的面板"。
       文字用 draw_para 自动折行、画满就截断，所以永远不会溢出面板。 */
    if (wr > 0) {
        int bx = rc.x + PADX + wl + S(28);
        int by = rc.y + head_h;
        int bh = h - head_h - foot_h;

        SDL_Rect vl = { bx - S(14), by + S(6), 1, bh - S(12) };
        setc(REN, C_LINE, 110);
        SDL_RenderFillRect(REN, &vl);

        /* 介绍整段**垂直居中**：左栏行数固定、面板高度被它锁死，
           介绍却长短不一；顶对齐时短介绍下面空掉半栏，看着"上挤下空"。
           先干跑一遍数行数（一行都不画），再按行高算偏移重画一次。 */
        const char *dtxt = (desc && desc[0]) ? desc : "暂无介绍";
        int availh = bh - S(16);
        int dlh = TTF_FontLineSkip(F_SETV);
        if (dlh <= 0) dlh = S(24);
        int dl  = draw_para(F_SETV, dtxt, bx, 0, wr, availh, C_SUBTEXT, 1);
        int maxl = availh / dlh;
        if (maxl >= 1 && dl > maxl) dl = maxl;
        int dy  = by + S(8);
        int dph = dl * dlh;
        if (dph < availh) dy += (availh - dph) / 2;
        draw_para(F_SETV, dtxt, bx, dy, wr, availh, C_SUBTEXT, 0);
    }

    /* --- 底部按键提示 ---
       上面补一条分隔线，和标题下那条同色同透明度 —— 两栏/菜单才像一个整体，
       否则底部提示看着像浮在菜单行上面。
       ⚠ 用 nrow 不是 ACT_N：金手指项藏掉时少一行，这两个 y 跟着上移，
         不然提示会悬在面板下面 S(一个行高) 的位置。 */
    {
        SDL_Rect fsep = { rc.x + S(22), rc.y + head_h + row_h * nrow + S(3),
                          w - S(44), 1 };
        setc(REN, C_LINE, 90);
        SDL_RenderFillRect(REN, &fsep);
    }
    char ffit[96];
    ellipsize(F_SETF, ACT_FOOT[g_act_ask ? 1 : 0], w - S(40), ffit, sizeof(ffit));
    int fw = 0, fh = 0;
    blit(REN, bake_cached(REN, F_SETF, ffit,
                          g_act_ask ? C_DANGER : C_SUBTEXT, &fw, &fh),
         rc.x + (w - fw) / 2,
         rc.y + head_h + row_h * nrow + (foot_h - fh) / 2, fw, fh);
}

/* 「更换模拟器核心」的二级列表。
 *
 * 为什么不复用 draw_act_popup 的版式：那边是"**固定 4 项**"的静态布局
 * （高度 = row_h * ACT_N），这边行数跟着平台的候选核心数走、还要滚动，
 * 硬塞进去就得把已经验收过的 4 项版式一起改坏。
 * 两边的玻璃面板、量字方式、配色、行内间距常数**完全一致**，只是行数
 * 算法不同 —— 看起来是一个东西，实现上是两段。
 *
 * 行序：0 = 适用范围，1 = 自动，2.. = 候选核心。
 */
static void draw_core_list(void)
{
    Platform *p = act_plat();
    if (!p || g_act_gi < 0 || g_act_gi >= p->ngames) {
        g_act_on = 0;
        g_act_core = 0;
        return;
    }

    /* 和 draw_act_popup 完全相同的间距常数 */
    const int PADX = S(34);
    const int ROWIN = S(16);
    const int LBX  = S(44);
    const int VBR  = S(24);
    const int GAP  = S(26);

    int nc = 0;
    const char *const *cs = act_core_list(p, &nc);
    const int nrow = 2 + nc;
    int nvis = (nrow > CORE_VIS) ? CORE_VIS : nrow;

    /* 候选标签先格式化到本地数组。跨模拟器的项在 C 行里是
       "<模拟器>:<核心>"（见 core_label_into），直接画太丑。
       放在这里而不是逐行现算：量宽度和绘制都要用，算两遍浪费；也**不能**
       改成返回静态缓冲 —— bake_cached() 按字符串内容缓存，共用一块缓冲会串。 */
    char lab_buf[CORE_MAX][128];
    for (int i = 0; i < nc && i < CORE_MAX; i++)
        core_label_into(cs[i], lab_buf[i], sizeof(lab_buf[i]));

    /* ---- 量标题 ---- */
    int tw = 0, th = 0;
    if (TTF_SizeUTF8(F_H1, p->games[g_act_gi].title, &tw, &th) != 0) { tw = 0; th = 0; }

    char sub[192];
    if (nrow > nvis)
        snprintf(sub, sizeof(sub), "更换模拟器核心 · %s · %d/%d",
                 p->name, g_act_csel + 1, nrow);
    else
        snprintf(sub, sizeof(sub), "更换模拟器核心 · %s", p->name);
    int sw = 0, sh = 0;
    if (TTF_SizeUTF8(F_PSUB, sub, &sw, &sh) != 0) { sw = 0; sh = 0; }

    /* ---- 量标签：最长的一行 = 最长的核心名 ---- */
    int lw_max = 0, lh_max = 0;
    {
        int a = 0, b = 0;
        if (TTF_SizeUTF8(F_SETN, AUTO_LABEL, &a, &b) == 0) { lw_max = a; lh_max = b; }
        if (TTF_SizeUTF8(F_SETN, SCOPE_LABEL, &a, &b) == 0 && a > lw_max)
            lw_max = a;
        for (int i = 0; i < nc; i++) {
            if (TTF_SizeUTF8(F_SETN, lab_buf[i], &a, &b) == 0) {
                if (a > lw_max) lw_max = a;
                if (b > lh_max) lh_max = b;
            }
        }
    }

    /* ---- 量右侧状态字：两种适用范围 + "当前" ---- */
    int vw_max = 0, vh_max = 0;
    {
        static const char *const VS[3] = { "仅本游戏", "整个平台", "当前" };
        for (int i = 0; i < 3; i++) {
            int a = 0, b = 0;
            if (TTF_SizeUTF8(F_SETV, VS[i], &a, &b) == 0) {
                if (a > vw_max) vw_max = a;
                if (b > vh_max) vh_max = b;
            }
        }
    }

    /* ---- 量底部提示 ---- */
    int fw_max = 0, fh_max = 0;
    if (TTF_SizeUTF8(F_SETF, ACT_FOOT[2], &fw_max, &fh_max) != 0) {
        fw_max = 0;
        fh_max = 0;
    }

    /* ---- 定宽（与 draw_act_popup 同一套算法）---- */
    int w = g_win_w * 46 / 100;
    int need = (tw > sw ? tw : sw) + PADX * 2;
    if (need > w) w = need;
    need = fw_max + PADX * 2;
    if (need > w) w = need;
    need = ROWIN + LBX + lw_max + GAP + vw_max + VBR + ROWIN;
    if (need > w) w = need;
    int wmax = g_win_w - S(72);
    if (w > wmax) w = wmax;

    /* ---- 定高 ---- */
    const int rad    = S(30);   /* 圆角比原来大一圈，玻璃面板更柔 */
    const int head_h = S(26) + th + S(10) + sh + S(18);
    const int row_in = (lh_max > vh_max ? lh_max : vh_max) + S(28);
    const int row_h  = row_in + S(10);
    const int foot_h = S(18) + fh_max + S(16);
    int h = head_h + row_h * nvis + foot_h;
    int hmax = g_win_h - S(72);
    if (h > hmax) {
        /* 同 draw_shader_list：太高就减行数，别压扁（压扁会裁掉末行和提示） */
        int room = hmax - head_h - foot_h;
        nvis = room / row_h;
        if (nvis < 1) nvis = 1;
        if (nvis > nrow) nvis = nrow;
        h = head_h + row_h * nvis + foot_h;
    }
    SDL_Rect rc = { (g_win_w - w) / 2, (g_win_h - h) / 2, w, h };

    glass_panel(rc, rad, C_PANEL, 232);
    draw_round(REN, rc, rad, C_LINE, 72);

    /* ---- 标题：游戏名 + "更换模拟器核心 · 平台名" ---- */
    char nm[256];
    ellipsize(F_H1, p->games[g_act_gi].title, w - PADX * 2, nm, sizeof(nm));
    int tw2 = 0, th2 = 0;
    blit(REN, bake_cached(REN, F_H1, nm, C_TEXT, &tw2, &th2),
         rc.x + PADX, rc.y + S(26), tw2, th2);

    char sub2[256];
    ellipsize(F_PSUB, sub, w - PADX * 2, sub2, sizeof(sub2));
    int sw2 = 0, sh2 = 0;
    blit(REN, bake_cached(REN, F_PSUB, sub2, C_SUBTEXT, &sw2, &sh2),
         rc.x + PADX, rc.y + S(26) + th2 + S(10), sw2, sh2);

    SDL_Rect sep = { rc.x + S(22), rc.y + head_h - S(10), w - S(44), 1 };
    setc(REN, C_LINE, 150);
    SDL_RenderFillRect(REN, &sep);

    /* ---- 行 ---- */
    int y = rc.y + head_h;
    for (int k = 0; k < nvis; k++) {
        int i = g_act_ctop + k;                 /* 真实行号（滚动后的） */
        SDL_Rect row = { rc.x + ROWIN, y, w - ROWIN * 2, row_in };
        int on = (i == g_act_csel);

        if (on) draw_round(REN, row, S(12), C_ACCENT, 150);
        Uint32 fg = on ? 0xffffff : C_TEXT;

        SDL_Rect chip = { row.x + S(20), row.y + (row.h - S(32)) / 2, S(6), S(32) };
        draw_round(REN, chip, S(3), on ? 0xffffff : C_ACCENT, 235);

        /* 三类行：0 适用范围 / 1 自动 / 2.. 候选核心。
           ⚠ lab 是**显示用**（候选已由 core_label_into 格式化过），而判"当前"
             必须用**原始串** —— cores.txt 里存的就是 C 行那个原样值（含
             "retroarch:" 前缀），拿格式化后的 "x (retroarch)" 去比永远不相等。 */
        const char *raw = (i >= 2 && i - 2 < nc) ? cs[i - 2] : "";
        const char *lab = (i == 0) ? SCOPE_LABEL
                        : (i == 1) ? AUTO_LABEL
                                   : ((i - 2 >= 0 && i - 2 < nc) ? lab_buf[i - 2] : "");
        const char *val = NULL;
        if (i == 0) {
            val = SCOPE_VALUE[g_act_scope ? 1 : 0];
        } else {
            int cur = (i == 1) ? (g_act_ocur[0] == '\0')
                               : (g_act_ocur[0] && raw[0] && strcmp(raw, g_act_ocur) == 0);
            if (cur) val = "当前";
        }

        int vw = 0, vh = 0;
        char vfit[64];
        vfit[0] = '\0';
        if (val) {
            ellipsize(F_SETV, val, row.w - LBX - S(60), vfit, sizeof(vfit));
            if (TTF_SizeUTF8(F_SETV, vfit, &vw, &vh) != 0) { vw = 0; vh = 0; }
        }

        char lfit[192];
        ellipsize(F_SETN, lab, row.w - LBX - VBR - (vw ? vw + GAP : 0),
                  lfit, sizeof(lfit));
        int lw = 0, lh = 0;
        blit(REN, bake_cached(REN, F_SETN, lfit, fg, &lw, &lh),
             row.x + LBX, row.y + (row.h - lh) / 2, lw, lh);

        if (val) {
            /* 选中行的状态字必须反白 —— 底色是强调色，"当前"再用灰色就糊了 */
            Uint32 vc = on ? 0xffffff : C_SUBTEXT;
            blit(REN, bake_cached(REN, F_SETV, vfit, vc, &vw, &vh),
                 row.x + row.w - VBR - vw, row.y + (row.h - vh) / 2, vw, vh);
        }
        y += row_h;
    }

    /* ---- 底部按键提示 ---- */
    char ffit[96];
    ellipsize(F_SETF, ACT_FOOT[2], w - S(40), ffit, sizeof(ffit));
    int fw = 0, fh = 0;
    blit(REN, bake_cached(REN, F_SETF, ffit, C_SUBTEXT, &fw, &fh),
         rc.x + (w - fw) / 2,
         rc.y + head_h + row_h * nvis + (foot_h - fh) / 2, fw, fh);
}

/* 着色器列表版式。和 draw_core_list() 同一套"先量后排"算法，但**少两行**：
   没有「适用范围」（粒度就是核心，不给用户"仅本游戏"这个做不到的幻觉选项）、
   没有「自动」行（不用就直接不写盘，不需要一条显式的"自动"）。
   副标题把**核心名**摆在最显眼处 —— 用户要的"同个核心都生效"，
   得让他一眼看见自己正在改的是哪个核心。 */
static void draw_shader_list(void)
{
    Platform *p = act_plat();
    if (!p || g_act_gi < 0 || g_act_gi >= p->ngames) {
        g_act_on = 0;
        g_act_shader = 0;
        return;
    }

    /* 和 draw_act_popup / draw_core_list 完全相同的间距常数 */
    const int PADX = S(34);
    const int ROWIN = S(16);
    const int LBX  = S(44);
    const int VBR  = S(24);
    const int GAP  = S(26);

    const int nrow = SHADER_N;
    int nvis = (nrow > SHADER_VIS) ? SHADER_VIS : nrow;

    /* ---- 量标题 ---- */
    int tw = 0, th = 0;
    if (TTF_SizeUTF8(F_H1, p->games[g_act_gi].title, &tw, &th) != 0) { tw = 0; th = 0; }

    const char *core = act_cur_core(p);
    char sub[224];
    if (!core || !core[0])
        snprintf(sub, sizeof(sub), "着色器效果 · %s · 这个平台不用 RetroArch 核心", p->name);
    else if (nrow > nvis)
        snprintf(sub, sizeof(sub), "着色器效果 · 核心 %s · %d/%d",
                 core, g_act_ssel + 1, nrow);
    else
        snprintf(sub, sizeof(sub), "着色器效果 · 核心 %s", core);
    int sw = 0, sh = 0;
    if (TTF_SizeUTF8(F_PSUB, sub, &sw, &sh) != 0) { sw = 0; sh = 0; }

    /* ---- 量标签：最长的一行 ---- */
    int lw_max = 0, lh_max = 0;
    for (int i = 0; i < nrow; i++) {
        int a = 0, b = 0;
        if (TTF_SizeUTF8(F_SETN, SHADER_LABEL[i], &a, &b) == 0) {
            if (a > lw_max) lw_max = a;
            if (b > lh_max) lh_max = b;
        }
    }

    /* ---- 量右侧状态字 ---- */
    int vw_max = 0, vh_max = 0;
    if (TTF_SizeUTF8(F_SETV, "当前", &vw_max, &vh_max) != 0) { vw_max = 0; vh_max = 0; }

    /* ---- 量底部提示 ---- */
    int fw_max = 0, fh_max = 0;
    if (TTF_SizeUTF8(F_SETF, SHADER_FOOT, &fw_max, &fh_max) != 0) {
        fw_max = 0;
        fh_max = 0;
    }

    /* ---- 定宽 ---- */
    int w = g_win_w * 46 / 100;
    int need = (tw > sw ? tw : sw) + PADX * 2;
    if (need > w) w = need;
    need = fw_max + PADX * 2;
    if (need > w) w = need;
    need = ROWIN + LBX + lw_max + GAP + vw_max + VBR + ROWIN;
    if (need > w) w = need;
    int wmax = g_win_w - S(72);
    if (w > wmax) w = wmax;

    /* ---- 定高 ---- */
    const int rad    = S(30);   /* 圆角比原来大一圈，玻璃面板更柔 */
    const int head_h = S(26) + th + S(10) + sh + S(18);
    const int row_in = (lh_max > vh_max ? lh_max : vh_max) + S(28);
    const int row_h  = row_in + S(10);
    const int foot_h = S(18) + fh_max + S(16);
    int h = head_h + row_h * nvis + foot_h;
    int hmax = g_win_h - S(72);
    if (h > hmax) {
        /* 高度不够就**减少可见行数**，别把面板压扁 —— 压扁会把最后一行和
           底部按键提示一起裁掉（2026-09-16 截图核对抓到的：设 12 行时
           "GameBoy 绿"被底边截断、整条提示不见了）。 */
        int room = hmax - head_h - foot_h;
        nvis = room / row_h;
        if (nvis < 1) nvis = 1;
        if (nvis > nrow) nvis = nrow;
        h = head_h + row_h * nvis + foot_h;
    }
    SDL_Rect rc = { (g_win_w - w) / 2, (g_win_h - h) / 2, w, h };

    glass_panel(rc, rad, C_PANEL, 232);
    draw_round(REN, rc, rad, C_LINE, 72);

    /* ---- 标题：游戏名 + "着色器效果 · 核心 xxx" ---- */
    char nm[256];
    ellipsize(F_H1, p->games[g_act_gi].title, w - PADX * 2, nm, sizeof(nm));
    int tw2 = 0, th2 = 0;
    blit(REN, bake_cached(REN, F_H1, nm, C_TEXT, &tw2, &th2),
         rc.x + PADX, rc.y + S(26), tw2, th2);

    char sub2[256];
    ellipsize(F_PSUB, sub, w - PADX * 2, sub2, sizeof(sub2));
    int sw2 = 0, sh2 = 0;
    blit(REN, bake_cached(REN, F_PSUB, sub2, C_SUBTEXT, &sw2, &sh2),
         rc.x + PADX, rc.y + S(26) + th2 + S(10), sw2, sh2);

    SDL_Rect sep = { rc.x + S(22), rc.y + head_h - S(10), w - S(44), 1 };
    setc(REN, C_LINE, 150);
    SDL_RenderFillRect(REN, &sep);

    /* ---- 行 ---- */
    int y = rc.y + head_h;
    for (int k = 0; k < nvis; k++) {
        int i = g_act_stop + k;                 /* 真实行号（滚动后的） */
        if (i < 0 || i >= nrow) break;
        SDL_Rect row = { rc.x + ROWIN, y, w - ROWIN * 2, row_in };
        int on = (i == g_act_ssel);

        if (on) draw_round(REN, row, S(12), C_ACCENT, 150);
        Uint32 fg = on ? 0xffffff : C_TEXT;

        SDL_Rect chip = { row.x + S(20), row.y + (row.h - S(32)) / 2, S(6), S(32) };
        draw_round(REN, chip, S(3), on ? 0xffffff : C_ACCENT, 235);

        const char *lab = SHADER_LABEL[i];
        /* 第 0 行是"关闭"——它也是"当前"的一种状态（盘上没这个核心的行），
           所以判据用 g_act_scur 空不空，而不是拿 rel 去跟它比。 */
        const char *val = NULL;
        if (i == 0) {
            if (g_act_scur[0] == '\0') val = "当前";
        } else if (g_act_scur[0] && strcmp(SHADER_REL[i], g_act_scur) == 0) {
            val = "当前";
        }

        int vw = 0, vh = 0;
        char vfit[64];
        vfit[0] = '\0';
        if (val) {
            ellipsize(F_SETV, val, row.w - LBX - S(60), vfit, sizeof(vfit));
            if (TTF_SizeUTF8(F_SETV, vfit, &vw, &vh) != 0) { vw = 0; vh = 0; }
        }

        char lfit[192];
        ellipsize(F_SETN, lab, row.w - LBX - VBR - (vw ? vw + GAP : 0),
                  lfit, sizeof(lfit));
        int lw = 0, lh = 0;
        blit(REN, bake_cached(REN, F_SETN, lfit, fg, &lw, &lh),
             row.x + LBX, row.y + (row.h - lh) / 2, lw, lh);

        if (val) {
            /* 选中行的状态字必须反白 —— 底色是强调色，再用灰色就糊了 */
            Uint32 vc = on ? 0xffffff : C_SUBTEXT;
            blit(REN, bake_cached(REN, F_SETV, vfit, vc, &vw, &vh),
                 row.x + row.w - VBR - vw, row.y + (row.h - vh) / 2, vw, vh);
        }
        y += row_h;
    }

    /* ---- 底部按键提示 ---- */
    char ffit[96];
    ellipsize(F_SETF, SHADER_FOOT, w - S(40), ffit, sizeof(ffit));
    int fw = 0, fh = 0;
    blit(REN, bake_cached(REN, F_SETF, ffit, C_SUBTEXT, &fw, &fh),
         rc.x + (w - fw) / 2,
         rc.y + head_h + row_h * nvis + (foot_h - fh) / 2, fw, fh);
}

/* 金手指二级页（用户 2026-09-18）。就两行：适用范围 + 开关。
   版式和 draw_core_list() 同一套"先量后排"，只是行数固定 2。

   副标题是这一页唯一的"检测结果"：库里匹配到哪一份、几条金手指。
   玩家得看见这个才知道开了到底有没有用 —— 尤其"库里压根没有"这种情况，
   不写出来他会以为是自己没操作对。 */
static void draw_cheat_list(void)
{
    Platform *p = act_plat();
    if (!p || g_act_gi < 0 || g_act_gi >= p->ngames) {
        g_act_on = 0;
        g_act_cheat = 0;
        return;
    }

    /* 和 draw_act_popup / draw_shader_list 完全相同的间距常数 */
    const int PADX = S(34);
    const int ROWIN = S(16);
    const int LBX  = S(44);
    const int VBR  = S(24);
    const int GAP  = S(26);

    const int nrow = cheat_nrow();           /* 0 适用范围 / 1 总开关 / 2.. 每条功能 */
    int nvis = (nrow > CHEAT_VIS) ? CHEAT_VIS : nrow;

    /* ---- 量标题 ---- */
    int tw = 0, th = 0;
    if (TTF_SizeUTF8(F_H1, p->games[g_act_gi].title, &tw, &th) != 0) { tw = 0; th = 0; }

    const char *core = act_cur_core(p);
    char sub[320];
    if (!core || !core[0])
        snprintf(sub, sizeof(sub), "金手指 · %s · 这个平台不走 RetroArch", p->name);
    else if (!g_act_chlib)
        snprintf(sub, sizeof(sub), "金手指 · %s · 库里没有这个平台的金手指", p->name);
    else if (g_act_chcount > CHEAT_MAX)
        snprintf(sub, sizeof(sub), "金手指 · %s · %s（共 %d 条，只列前 %d）",
                 p->name, g_act_chfile, g_act_chcount, CHEAT_MAX);
    else
        snprintf(sub, sizeof(sub), "金手指 · %s · 已找到 %s（%d 条）",
                 p->name, g_act_chfile, g_act_chcount);
    int sw = 0, sh = 0;
    if (TTF_SizeUTF8(F_PSUB, sub, &sw, &sh) != 0) { sw = 0; sh = 0; }

    /* ---- 量标签：最长的一行 ---- */
    int lw_max = 0, lh_max = 0;
    {
        int a = 0, b = 0;
        if (TTF_SizeUTF8(F_SETN, SCOPE_LABEL, &a, &b) == 0) { lw_max = a; lh_max = b; }
        if (TTF_SizeUTF8(F_SETN, CHEAT_SW_LABEL, &a, &b) == 0) {
            if (a > lw_max) lw_max = a;
            if (b > lh_max) lh_max = b;
        }
    }

    /* ---- 量右侧状态字：两种适用范围 + 开/关 ---- */
    int vw_max = 0, vh_max = 0;
    {
        static const char *const VS[4] = { "仅本游戏", "整个平台", "开", "关" };
        for (int i = 0; i < 4; i++) {
            int a = 0, b = 0;
            if (TTF_SizeUTF8(F_SETV, VS[i], &a, &b) == 0) {
                if (a > vw_max) vw_max = a;
                if (b > vh_max) vh_max = b;
            }
        }
    }

    /* ---- 量底部提示 ---- */
    int fw_max = 0, fh_max = 0;
    if (TTF_SizeUTF8(F_SETF, CHEAT_FOOT, &fw_max, &fh_max) != 0) {
        fw_max = 0;
        fh_max = 0;
    }

    /* ---- 定宽 ---- */
    int w = g_win_w * 46 / 100;
    int need = (tw > sw ? tw : sw) + PADX * 2;
    if (need > w) w = need;
    need = fw_max + PADX * 2;
    if (need > w) w = need;
    need = ROWIN + LBX + lw_max + GAP + vw_max + VBR + ROWIN;
    if (need > w) w = need;
    int wmax = g_win_w - S(72);
    if (w > wmax) w = wmax;

    /* ---- 定高 ---- */
    const int rad    = S(30);
    const int head_h = S(26) + th + S(10) + sh + S(18);
    const int row_in = (lh_max > vh_max ? lh_max : vh_max) + S(28);
    const int row_h  = row_in + S(10);
    const int foot_h = S(18) + fh_max + S(16);
    int h = head_h + row_h * nvis + foot_h;
    int hmax = g_win_h - S(72);
    if (h > hmax) {
        /* 行高是按字号算的，字号调大之后 6 行也可能超 —— 那就少摆几行，
           别把面板压扁（压扁会把最后一行和底部提示一起裁掉，
           draw_shader_list 2026-09-16 踩过这个）。 */
        int room = hmax - head_h - foot_h;
        nvis = room / row_h;
        if (nvis < 1) nvis = 1;
        if (nvis > nrow) nvis = nrow;
        h = head_h + row_h * nvis + foot_h;
    }
    SDL_Rect rc = { (g_win_w - w) / 2, (g_win_h - h) / 2, w, h };

    glass_panel(rc, rad, C_PANEL, 232);
    draw_round(REN, rc, rad, C_LINE, 72);

    /* ---- 标题：游戏名 + "金手指 · 平台名" ---- */
    char nm[256];
    ellipsize(F_H1, p->games[g_act_gi].title, w - PADX * 2, nm, sizeof(nm));
    int tw2 = 0, th2 = 0;
    blit(REN, bake_cached(REN, F_H1, nm, C_TEXT, &tw2, &th2),
         rc.x + PADX, rc.y + S(26), tw2, th2);

    char sub2[352];
    ellipsize(F_PSUB, sub, w - PADX * 2, sub2, sizeof(sub2));
    int sw2 = 0, sh2 = 0;
    blit(REN, bake_cached(REN, F_PSUB, sub2, C_SUBTEXT, &sw2, &sh2),
         rc.x + PADX, rc.y + S(26) + th2 + S(10), sw2, sh2);

    SDL_Rect sep = { rc.x + S(22), rc.y + head_h - S(10), w - S(44), 1 };
    setc(REN, C_LINE, 150);
    SDL_RenderFillRect(REN, &sep);

    /* ---- 滚动窗口里的 nvis 行 ----
       行 0/1 是固定项，行 2+ 是这一份 cht 的具体功能（浪总要的就是它）。
       条目行给个短一号的 chip，视觉上从属于上面两项。 */
    int y = rc.y + head_h;
    for (int k = 0; k < nvis; k++) {
        int i = g_act_chtop + k;                 /* 真实行号（滚动后的） */
        if (i < 0 || i >= nrow) break;

        SDL_Rect row = { rc.x + ROWIN, y, w - ROWIN * 2, row_in };
        int on = (i == g_act_chsel);
        int is_item = (i >= 2);

        if (on) draw_round(REN, row, S(12), C_ACCENT, 150);
        Uint32 fg = on ? 0xffffff : C_TEXT;

        SDL_Rect chip;
        if (is_item) {
            chip.x = row.x + S(22); chip.y = row.y + (row.h - S(18)) / 2;
            chip.w = S(4);          chip.h = S(18);
        } else {
            chip.x = row.x + S(20); chip.y = row.y + (row.h - S(32)) / 2;
            chip.w = S(6);          chip.h = S(32);
        }
        draw_round(REN, chip, S(3), on ? 0xffffff : C_ACCENT, is_item ? 170 : 235);

        const char *lab;
        const char *val;
        if (i == 0) {
            lab = SCOPE_LABEL;
            val = SCOPE_VALUE[g_act_chscope ? 1 : 0];
        } else if (i == 1) {
            lab = CHEAT_SW_LABEL;
            val = g_act_chon ? "开" : "关";
        } else {
            int ci = i - 2;
            lab = (ci < g_act_chvis) ? g_act_chdesc[ci] : "";
            val = (g_act_chbits[ci] == '0') ? "关" : "开";
        }

        int vw = 0, vh = 0;
        char vfit[64];
        vfit[0] = '\0';
        ellipsize(F_SETV, val, row.w - LBX - S(60), vfit, sizeof(vfit));
        if (TTF_SizeUTF8(F_SETV, vfit, &vw, &vh) != 0) { vw = 0; vh = 0; }

        char lfit[256];
        ellipsize(F_SETN, lab, row.w - LBX - VBR - (vw ? vw + GAP : 0),
                  lfit, sizeof(lfit));
        int lw = 0, lh = 0;
        blit(REN, bake_cached(REN, F_SETN, lfit, fg, &lw, &lh),
             row.x + LBX, row.y + (row.h - lh) / 2, lw, lh);

        /* ⚠ 右侧字是"当前值"（不是核心列表那种"当前/非当前"标记），
           所以无脑跟着反白走 —— 选中行白字，其余灰字。 */
        blit(REN, bake_cached(REN, F_SETV, vfit, on ? 0xffffff : C_SUBTEXT, &vw, &vh),
             row.x + row.w - VBR - vw, row.y + (row.h - vh) / 2, vw, vh);

        y += row_h;
    }

    /* ---- 底部按键提示 ---- */
    char ffit[128];
    ellipsize(F_SETF, CHEAT_FOOT, w - S(40), ffit, sizeof(ffit));
    int fw = 0, fh = 0;
    blit(REN, bake_cached(REN, F_SETF, ffit, C_SUBTEXT, &fw, &fh),
         rc.x + (w - fw) / 2,
         rc.y + head_h + row_h * nvis + (foot_h - fh) / 2, fw, fh);
}

/* ★ 2026-09-17：draw_slot_list()（弹层的第 3 个二级列表）已整体删除。
   见文件上方 SLOT_LABEL 那块墓碑注释。 */


static void handle_key(SDL_Keycode k)
{
    /* 弹层是**模态**的：它开着的时候，所有按键先交给它，认不认都由它决定。
       放在最前面 —— 放在任何一个页面分支后面都会漏掉别的页面。 */
    if (g_act_on) { act_key(k); return; }

    /* --- 快捷键说明页（只读）--- */
    if (g_page == PAGE_HELP) {
        switch (k) {
        case SDLK_UP:   help_scroll(-1); return;
        case SDLK_DOWN: help_scroll(+1); return;
        /* 上/下翻页也给键盘留着（手柄那边是 L1/R1） */
        case SDLK_PAGEUP:   help_scroll(-help_view_rows()); return;
        case SDLK_PAGEDOWN: help_scroll(+help_view_rows()); return;
        /* A / B / ESC / TAB **都是返回**。这一页没有别的事能做，
           留一堆"按了没反应"的键只会让人以为卡住了 ——
           底栏写的是「B 返回」，A 也就近一起收下，手感跟弹层一致。 */
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_ESCAPE:
        case SDLK_BACKSPACE:
        case SDLK_TAB:  help_close(); return;
        default: return;
        }
    }

    /* --- 设置页 --- */
    if (g_page == PAGE_SETTINGS) {
        /* 键盘上就是方向键 + 回车/退格，与手柄的十字键 + A/B 一一对应。
           TAB 在设置页当"返回"（手柄 START 在设置页也是返回）；主页那边
           TAB 才是切分类 —— 不冲突，因为这里处理完就 return 了。 */
        switch (k) {
        case SDLK_UP:        settings_key(0); return;
        case SDLK_DOWN:      settings_key(1); return;
        case SDLK_LEFT:      settings_key(2); return;
        case SDLK_RIGHT:     settings_key(3); return;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:  settings_key(4); return;
        case SDLK_ESCAPE:
        case SDLK_BACKSPACE:
        case SDLK_TAB:       settings_key(5); return;
        default: return;
        }
    }

    /* --- 游戏列表页 --- */
    if (g_page == PAGE_GAMES) {
        Platform *p = cur_plat();
        int n = p ? p->ngames : 0;
        switch (k) {
        case SDLK_ESCAPE:
        case SDLK_BACKSPACE:  leave_games(); return;
        case SDLK_LEFT:
            if (n && g_gsel % g_game_cols > 0) g_gsel--;
            return;
        case SDLK_RIGHT:
            if (n && g_gsel % g_game_cols < g_game_cols - 1 && g_gsel + 1 < n) g_gsel++;
            return;
        case SDLK_UP:
            if (n && g_gsel - g_game_cols >= 0) g_gsel -= g_game_cols;
            return;
        case SDLK_DOWN:
            if (n && g_gsel + g_game_cols < n) g_gsel += g_game_cols;
            return;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            /* 启动通道见上面「5.5 启动游戏」：写 launch.sh 后退出进程，
               由启动器的游戏循环把屏幕交给 RetroArch，游戏退出后再拉起本程序。

               g_quit=2 是"因启动而退出"，和 g_quit=1（用户按了退出）区分开 ——
               两者对启动器是同一件事（rc=0 + launch.sh 在不在），
               但日志里一眼能看出这次是为什么退的。 */
            /* 以前这里直接启动；现在先开弹层（启动游戏/收藏/删除），
               第一项就是启动，所以「想直接开」只是多按一下 A。 */
            if (p && g_gsel < p->ngames) act_open(g_plat, g_gsel);
            return;
        default:
            return;
        }
    }

    /* --- 搜索页 --- */
    if (g_page == PAGE_SEARCH) {
        switch (k) {
        case SDLK_LEFT:  search_move(-1); return;
        case SDLK_RIGHT: search_move(1);  return;
        case SDLK_UP:    search_move(-2); return;
        case SDLK_DOWN:  search_move(2);  return;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            if (g_sfocus == 0) {
                search_press(g_kbd);
            } else if (g_res_n) {
                /* 从结果里直接启动 —— 走的还是同一条启动通道 */
                /* 搜索结果走同一条路：也开弹层，别两套行为 */
                act_open(g_res[g_rsel].plat, g_res[g_rsel].gi);
            }
            return;
        case SDLK_BACKSPACE:
            /* B：焦点在结果上先退回软键盘；在软键盘上就退格；
               输入已经空了才当"返回主页" —— 与底栏「B 返回」一致。 */
            if (g_sfocus == 1) { g_sfocus = 0; mark_dirty(); return; }
            if (g_qlen > 0) { search_press(KB_ACT_BACK); return; }
            leave_search();
            return;
        case SDLK_ESCAPE:
            if (g_sfocus == 1) { g_sfocus = 0; mark_dirty(); return; }
            leave_search();
            return;
        case SDLK_TAB:                       /* 手柄 START = 清空 */
            search_press(KB_ACT_CLEAR);
            return;
        default:
            return;
        }
    }

    int n = g_view_n;
    if (!n) return;
    switch (k) {
    case SDLK_LEFT:
        if (g_sel % g_pg_cols > 0) g_sel--;
        else if (g_sel >= 0) { /* 行首再左：不动 */ }
        break;
    case SDLK_RIGHT:
        if (g_sel % g_pg_cols < g_pg_cols - 1 && g_sel + 1 < n) g_sel++;
        break;
    case SDLK_UP:
        if (g_sel - g_pg_cols >= 0) g_sel -= g_pg_cols;
        break;
    case SDLK_DOWN:
        if (g_sel + g_pg_cols < n) g_sel += g_pg_cols;
        break;
    case SDLK_LEFTBRACKET:
    case SDLK_q:
        if (g_sel - 1 >= 0) g_sel--;
        break;
    case SDLK_RIGHTBRACKET:
    case SDLK_e:
        if (g_sel + 1 < n) g_sel++;
        break;
    case SDLK_TAB:
        cat_step(1);
        break;
    case SDLK_y:                     /* 网页版：y 键 → showPage('search') */
        enter_search();
        break;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        if (g_view_n) enter_games(g_view[g_sel]);
        break;
    default:
        break;
    }
}

/* --------------------------------------------------------------------------
 *  手柄：SDL 映射补丁
 * --------------------------------------------------------------------------
 *  这台机器的手柄 evdev 上报的按钮就这么几个（真机 evtest 核对）：
 *      304..308  BTN_SOUTH / EAST / C / NORTH / WEST
 *      310, 311  BTN_TL / TR
 *      312, 313  BTN_TL2 / TR2
 *      544..547  BTN_DPAD_UP / DOWN / LEFT / RIGHT
 *      704..708  BTN_TRIGGER_HAPPY1..5
 *  全设备**没有** BTN_START(315) / BTN_SELECT(314) —— 厂家把 select/start
 *  塞进了 TRIGGER_HAPPY1/2，也就是 SDL 的按钮索引 b12/b13。
 *  config.ini 里那段 "704=select 705=start" 的注释就是这么实测出来的。
 *
 *  SDL 2.0.10 自带的库（GUID 里 crc:3ebb 那条）只映射到 b11，
 *  start/back 是空的 —— 不补这一条，SDL 永远不会派发 START/BACK，
 *  "START 切分类"就是死代码，而且不会有任何报错，最难查的那类。
 */
/* --------------------------------------------------------------------------
 *  A/B 是反的：必须对调，不能照抄 SDL 默认（2026-09-14 用户反馈修）
 * --------------------------------------------------------------------------
 *  SDL 的 evdev 默认约定是 BTN_SOUTH(0x130)=b0 → A、BTN_EAST(0x131)=b1 → B。
 *  这台 GO-Super Gamepad 的**物理 A 键在 evdev 上是 b1、物理 B 是 b0**，
 *  正好和 SDL 默认相反 —— 照默认映射的话，界面上"确认/返回"就整个反过来：
 *  想按确认按到返回、想返回却把游戏启动了，右侧那排键的物理排布让人
 *  本能按错（用户原话："ab 是反的总容易误触"）。
 *
 *  硬证据是原厂 ES 的按键表 `/etc/emulationstation/es_input.cfg`：
 *      <input name="a" ... id="1" />     ← 物理 A = raw button 1
 *      <input name="b" ... id="0" />     ← 物理 B = raw button 0
 *      <input name="x" ... id="2" />     ← X/Y 与 SDL 默认一致，不用动
 *      <input name="y" ... id="3" />
 *  也就是说原厂已经手工纠正过 A/B，我们照它来。
 *
 *  （x/y 对调过没有？没有 —— ES 的 x=2/y=3 与 SDL 的 x:b2/y:b3 相同，
 *    真调过的话 ES 里也会是反的，所以只改 a/b 两个。）
 *
 *  ⚠ 2026-09-15 补充：上面这段的结论**方向**是对的，但当时的**实现方式**
 *    错了（GUID 写死成 R36H 的、整张表照搬），细节见 pad_add_mapping()
 *    上面那段注释。现在改成运行期从 SDL 取映射再对调 a/b —— 结论没变，
 *    只是不再写死任何设备相关的值。
 */

/* 映射串里某个键在不在（键名恰好是 `key:` 那一段） */
static int pad_has_key(const char *map, const char *key)
{
    size_t kl = strlen(key);
    for (const char *p = map; *p; ) {
        const char *e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len > kl + 1 && p[kl] == ':' && strncmp(p, key, kl) == 0) return 1;
        if (!e) break;
        p = e + 1;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 *  补齐 SDL 映射里缺失的关键键（2026-09-20 在 R36hProMax 上踩出来的）
 *  --------------------------------------------------------------------------
 *  症状：新版 vgfront 在这台机器上日志打
 *        [_pad] START -> 未绑定(type=0) / BACK -> 未绑定(type=0)
 *        而同一台机器跑旧版（09-14 编的）却是 start=b13 back=b12。
 *
 *  根因**不在我们的改动**，而是 SDL 报回来的那张基础映射本身就缺键：
 *      SDL_GameControllerMappingForGUID() 在 2.0.26+ 之后，若 GUID 不在
 *      自带库里就返回 NULL；若在库里但那条记录**没写 start/back**（这台
 *      「GO-Super Gamepad」就是这种），返回的串里就真没有这两段。
 *        base = 1900bb3e..., GO-Super Gamepad, a:b0,b:b1, x:b2,y:b3,
 *               leftshoulder:b4, rightshoulder:b5, dpup:b8..dpright:b11,
 *               leftx:a0..righty:a3, lefttrigger:b6, righttrigger:b7
 *        ← 到此为止，start/back/guide 一个都没有。
 *    vendor 库（我们自编的 libSDL2-2.0.so.0.3200.10）自带表里就是这条。
 *
 *  为什么不能干脆写死：本工程 09-15 已经因为「写死 GUID + 整张表」翻过车
 *  （见 pad_add_mapping 上面的长注释）。所以这里**只补缺**：
 *    · 先问 SDL 自带的 `es_input.cfg`（原厂按键表，权威且随设备走）要值；
 *    · 取不到才回落到内置默认（start=b13 / back=b12，本机实测）。
 *  已有的键一个都不动 —— SDL 说什么就是什么，我们只填它没说的。
 */
#define PAD_ES_INPUT_CFG "/etc/emulationstation/es_input.cfg"

/* 从 es_input.cfg 里抓 <input name="Xxx" type="button" id="N" ...> 的 N。
   找不到返回 -1。不用 XML 库 —— 这文件格式固定，扫字符串就够，
   也不引入额外依赖。 */
static int pad_es_button_id(const char *name, char *buf, size_t cap)
{
    FILE *fp = fopen(PAD_ES_INPUT_CFG, "r");
    if (!fp) return -1;
    char line[512];
    int found = -1;
    while (fgets(line, sizeof(line), fp)) {
        if (!strstr(line, "type=\"button\"")) continue;
        /* 键名必须精确匹配 name="Xxx" */
        char pat[96];
        snprintf(pat, sizeof(pat), "name=\"%s\"", name);
        const char *q = strstr(line, pat);
        if (!q) continue;
        /* 同一行里找 id="N" */
        const char *id = strstr(q, "id=\"");
        if (!id) continue;
        id += 4;
        int v = atoi(id);
        if (v >= 0) { found = v; break; }
    }
    fclose(fp);
    if (found < 0) return -1;
    snprintf(buf, cap, "b%d", found);
    return found;
}

/* 把 map 里缺的关键键补上。返回补了几个。 */
static int pad_fill_missing(char *map, size_t cap)
{
    /* 键名 → SDL 认的字符串值。值优先从 es_input.cfg 读，读不到用默认。 */
    static const struct { const char *key; const char *es; const char *fallback; }
    NEED[] = {
        { "start", "start",  "b13" },
        { "back",  "select", "b12" },
    };
    int n = 0;
    for (unsigned i = 0; i < sizeof(NEED) / sizeof(NEED[0]); i++) {
        if (pad_has_key(map, NEED[i].key)) continue;      /* 已有，不动 */

        char v[16] = { 0 };
        if (pad_es_button_id(NEED[i].es, v, sizeof(v)) < 0)
            snprintf(v, sizeof(v), "%s", NEED[i].fallback);

        size_t ml = strlen(map);
        int w = snprintf(map + ml, cap > ml ? cap - ml : 0,
                         ",%s:%s", NEED[i].key, v);
        if (w <= 0 || (size_t)w >= (cap > ml ? cap - ml : 0)) {
            map[ml] = '\0';                              /* 放不下就还原 */
            break;
        }
        printf("[pad] 基础映射缺 %s，按原厂 es_input.cfg 补成 %s:%s\n",
               NEED[i].key, NEED[i].key, v);
        n++;
    }
    return n;
}

/* 把映射串里 `a:<x>` 与 `b:<y>` 的值对调，结果写进 dst。
   映射串形如 guid,name,key:val,key:val,...（第一段 GUID、第二段名字）。
   返回 1 = 两个字段都找到且换过了；0 = 没找全（此时 dst 仍是原串副本）。 */
static int pad_swap_ab(const char *src, char *dst, size_t cap)
{
    const char *va = NULL, *vb = NULL;
    size_t la = 0, lb = 0;

    /* 第一遍：定位 a: 和 b: 的值 */
    for (const char *p = src; *p; ) {
        const char *e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len > 2 && p[1] == ':') {
            if (p[0] == 'a')      { va = p + 2; la = len - 2; }
            else if (p[0] == 'b') { vb = p + 2; lb = len - 2; }
        }
        if (!e) break;
        p = e + 1;
    }
    if (!va || !vb) { snprintf(dst, cap, "%s", src); return 0; }

    /* 第二遍：逐段拼回去，碰到 a:/b: 就写对方的值 */
    size_t o = 0;
    for (const char *p = src; *p; ) {
        const char *e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        const char *val = NULL;
        size_t vl = 0;
        if (len > 2 && p[1] == ':') {
            if (p[0] == 'a')      { val = vb; vl = lb; }
            else if (p[0] == 'b') { val = va; vl = la; }
        }
        if (val) {                       /* 这一段整个换掉 */
            if (o + 2 + vl + 2 > cap) return 0;
            dst[o++] = p[0];
            dst[o++] = ':';
            memcpy(dst + o, val, vl); o += vl;
        } else {
            if (o + len + 2 > cap) return 0;
            memcpy(dst + o, p, len); o += len;
        }
        if (e) { dst[o++] = ','; p = e + 1; } else break;
    }
    dst[o] = '\0';
    return 1;
}

/* 手柄映射修正：把 A/B 对调。
 *
 *  ---------------------------------------------------------------------
 *  ⚠ 为什么**不能写死 GUID 和整张按键表**（2026-09-15 踩的坑）
 *  ---------------------------------------------------------------------
 *  这里原来是一张静态常量表，GUID 写的是 1900bb3e4b4800000011000000010000、
 *  名字 "GO-Super Gamepad"、start:b13 back:b12 dpad:b8~b11 —— 那是
 *  **R36H / A10 那台 RK3326** 的手柄。整套搬到 MINILOONG 上两个问题：
 *    ① GUID 对不上（这台是 19009b4d4b4800000111000000010000、
 *       名字 retrogame_joypad）。而 SDL_GameControllerAddMapping() 对
 *       **匹配不到任何设备**的 GUID 照样返回成功 —— 补丁静默失效，
 *       日志里还老老实实印着"已追加映射"，按键却全走 SDL 默认表。
 *       这就是"主界面 A/B 是反的"整整查了半天的原因：日志在骗人。
 *    ② 就算把 GUID 改对这台的，**那张表的键位仍然是 R36H 的**。
 *       实测本机 dpad 在 b13~b16、start/back 在 b9/b8、板机是轴不是键，
 *       照搬会把这些本来正常的键一起弄坏。
 *
 *  所以改成**运行期按 SDL 自己的映射现算**：问 SDL 要这台手柄当前的映射串，
 *  只把 a: 和 b: 两个值对调，其余一个字节不动。哪台机器、哪个手柄都对得上，
 *  而且天然不会碰坏别的键。
 *
 *  为什么要对调：绝大多数国产掌机的 A/B 是**任天堂布局**（A 在右、B 在下），
 *  而 SDL 内置表按 Xbox 布局认（A=南=b0、B=东=b1）。本机实测
 *  A->b0 / B->b1，物理 A 却发的是 BTN_EAST，正好反着 —— 原厂 ES 的
 *  es_input.cfg 里 "GPIO Controller 1" 也是 a=id1(305) b=id0(304)，
 *  跟这个结论一致。
 *
 *  ⚠ 必须在 SDL_GameControllerOpen() **之前**调用，否则打开的已经是旧表。
 */
static void pad_add_mapping(void)
{
    int n = SDL_NumJoysticks();
    int ok_n = 0;

    for (int i = 0; i < n; i++) {
        if (!SDL_IsGameController(i)) continue;

        SDL_JoystickGUID g = SDL_JoystickGetDeviceGUID(i);
        char gs[64];
        SDL_JoystickGetGUIDString(g, gs, sizeof(gs));

        /* SDL_GameControllerMappingForGUID 返回的是 malloc 出来的串，
           用完要 SDL_free（不是 free —— 可能是 SDL 自己那套分配器）。 */
        char *base = SDL_GameControllerMappingForGUID(g);
        char  map[1024];
        int   swapped = 0;

        if (base) {
            swapped = pad_swap_ab(base, map, sizeof(map));
            SDL_free(base);
        }
        if (!base || !swapped) {
            /* 兜底：SDL 没给基础映射（正常不会走到）。用最小可用集合，
               值取自本机实测 —— 但这是**下策**，记进日志好回头查。 */
            snprintf(map, sizeof(map),
                     "%s,retrogame_joypad,a:b1,b:b0,x:b2,y:b3,"
                     "leftshoulder:b4,rightshoulder:b5,"
                     "back:b8,start:b9,guide:b10,"
                     "dpup:b13,dpdown:b14,dpleft:b15,dpright:b16,"
                     "leftx:a0,lefty:a1,rightx:a2,righty:a3,platform:Linux",
                     gs);
            printf("[pad] 警告：SDL 未提供基础映射，用内置兜底表（%s）\n", gs);
        }

        /* SDL 自带表可能只给了一部分键（这台 R36hProMax 就是 start/back
           全缺），缺一个就补一个 —— 补值优先问原厂 es_input.cfg。 */
        pad_fill_missing(map, sizeof(map));

        if (SDL_GameControllerAddMapping(map) < 0) {
            printf("[pad] 追加映射失败: %s\n", SDL_GetError());
        } else {
            ok_n++;
            /* 把换完的结果整个印出来 —— 下次再有"按键反了"，直接看这行就能定，
               不用再靠人按了猜。 */
            printf("[pad] 已追加映射(%s)：%s\n", gs, map);
        }
    }

    if (ok_n == 0)
        printf("[pad] 没有可用的手柄映射目标（joystick 数=%d）\n", n);
}

/* 打开手柄后把关键键的**实际绑定**打出来 —— A/B 到底反没反，看这一行字
   就行，不用靠人按了猜。SDL 有 GetBindForButton，能直接问出 raw button 号。 */
static void pad_dump_bindings(SDL_GameController *gc)
{
    static const struct { int b; const char *n; } T[] = {
        { SDL_CONTROLLER_BUTTON_A,      "A"     },
        { SDL_CONTROLLER_BUTTON_B,      "B"     },
        { SDL_CONTROLLER_BUTTON_X,      "X"     },
        { SDL_CONTROLLER_BUTTON_Y,      "Y"     },
        { SDL_CONTROLLER_BUTTON_START,  "START" },
        { SDL_CONTROLLER_BUTTON_BACK,   "BACK"  },
    };
    for (unsigned i = 0; i < sizeof(T) / sizeof(T[0]); i++) {
        SDL_GameControllerButtonBind bd =
            SDL_GameControllerGetBindForButton(gc, (SDL_GameControllerButton)T[i].b);
        if (bd.bindType == SDL_CONTROLLER_BINDTYPE_BUTTON)
            printf("  [pad] %-5s -> raw b%d\n", T[i].n, bd.value.button);
        else
            printf("  [pad] %-5s -> 未绑定(type=%d)\n", T[i].n, bd.bindType);
    }
}

/* --------------------------------------------------------------------------
 *  手柄：长按连发
 * --------------------------------------------------------------------------
 *  不能只靠"收到事件就动一格"：
 *    ① 摇杆只在**值变化**时产生 AXISMOTION，推住不动不会再有事件；
 *    ② 十字键按下/抬起虽然成对，但这台设备的 evdev 能力里没有 EV_REP 位
 *       —— 内核不发按住重复，`cat /sys/class/input/event2/device/
 *          capabilities/ev` 是 0x20000b，bit4(EV_REP) 没置。
 *  所以"按住连续走"必须自己做。时间参数对齐网页版 config.ini 的
 *  pad_repeat_delay=420 / pad_repeat_rate=160，手感才一致。
 *
 *  两个来源分开记（十字键 / 左摇杆），摇杆优先；谁变谁调 pad_hold_sync()。
 */
#define PAD_HOLD_DELAY 420
#define PAD_HOLD_RATE  160

static int    g_btn_dir  = 0;   /* 十字键按住的方向 */
static int    g_axis_dir = 0;   /* 左摇杆推出的方向 */
static int    g_act_dir  = 0;   /* 正在连发的方向 */
static Uint32 g_act_t0   = 0;   /* 该方向按下的时刻（首格延迟用） */
static Uint32 g_act_last = 0;   /* 上一次实际移动的时刻 */
/* （g_quit 在页面状态那一段声明，handle_key 更靠前就要用） */

static void pad_move(int move);          /* 前向声明 */

/* 方向来源有变动时调用：换方向就立刻走一格，并重置连发计时 */
static void pad_hold_sync(void)
{
    int want = g_axis_dir ? g_axis_dir : g_btn_dir;
    if (want == g_act_dir) return;

    g_act_dir = want;
    if (want) {
        g_act_t0 = g_act_last = SDL_GetTicks();
        pad_move(want);                  /* 按下立刻响应，别让人干等 420ms */
    }
}

/* 主循环每帧调一次：按住超过首延迟后，按固定间隔连发 */
static void pad_tick(void)
{
    if (!g_act_dir) return;
    Uint32 now = SDL_GetTicks();
    if (now - g_act_t0 < PAD_HOLD_DELAY) return;
    if (now - g_act_last < PAD_HOLD_RATE) return;
    g_act_last = now;
    pad_move(g_act_dir);
}

/* 只负责"走一格"，页面分派在这里 */
static void pad_move(int move)
{
    /* 弹层是**模态**的，手柄方向键也得归它管。
       这里是所有方向输入的**唯一汇合点** —— 十字键、左摇杆、长按连发
       最终都走到 pad_move()，所以在这一处拦就全覆盖了。
       （原来只拦了 handle_key()，而手柄方向压根不走那条路：
         用户 2026-09-15 报"按了启动游戏之后那个弹窗都不会聚焦到"，
         真因就是弹层画出来了、十字键却还在动背后的游戏网格。）
       ⚠ 不能改去拦 handle_pad() 的十字键分支：那样 g_btn_dir 收不到
         按下事件，长按连发和抬起都乱了；拦在这个汇合点上最干净。 */
    if (g_act_on) {
        if      (move == -2) act_key(SDLK_UP);
        else if (move ==  2) act_key(SDLK_DOWN);
        else if (move == -1) act_key(SDLK_LEFT);
        else if (move ==  1) act_key(SDLK_RIGHT);
        return;                        /* 左右在弹层里没含义，act_key 会吞掉 */
    }

    if (g_page == PAGE_SEARCH) { search_move(move); return; }

    /* 说明页：上下滚，左右吞掉。
       ⚠ 左右必须在这里吃掉 —— 说明页下面是**主页**（浮层渲染），
         不 return 的话会一路落到最下面的主页网格分支，把 g_sel 改掉；
         用户退回去会发现主页选中卡片自己跳了。 */
    if (g_page == PAGE_HELP) {
        if      (move == -2) help_scroll(-1);
        else if (move ==  2) help_scroll(+1);
        return;
    }

    /* 设置页：上下移动光标，左右改值 —— 与键盘方向键完全一致 */
    if (g_page == PAGE_SETTINGS) {
        if      (move == -2) settings_key(0);
        else if (move ==  2) settings_key(1);
        else if (move == -1) settings_key(2);
        else if (move ==  1) settings_key(3);
        return;
    }

    if (g_page == PAGE_GAMES) {
        Platform *p = cur_plat();
        int n = p ? p->ngames : 0;
        if (!n) return;
        const int C = g_game_cols;
        if (move == -1)      { if (g_gsel % C > 0) g_gsel--; }
        else if (move == 1)  { if (g_gsel % C < C - 1 && g_gsel + 1 < n) g_gsel++; }
        else if (move == -2) { if (g_gsel - C >= 0) g_gsel -= C; }
        else if (move == 2)  { if (g_gsel + C < n) g_gsel += C; }
        return;
    }

    int n = g_view_n;
    if (!n) return;
    if (move == -1) { if (g_sel % g_pg_cols > 0) g_sel--; }
    else if (move == 1) { if (g_sel % g_pg_cols < g_pg_cols - 1 && g_sel + 1 < n) g_sel++; }
    else if (move == -2) { if (g_sel - g_pg_cols >= 0) g_sel -= g_pg_cols; }
    else if (move == 2) { if (g_sel + g_pg_cols < n) g_sel += g_pg_cols; }
}

static void handle_pad(SDL_Event *e)
{
    if (e->type == SDL_CONTROLLERBUTTONDOWN) {
        /* 弹层开着时，手柄只留三条路：
             A → 确认、B → 返回/取消确认、方向 → 交给弹层移动光标。
           其余功能键一律吞掉：不拦的话 B 会直接退出游戏页、
           START 会弹出设置页、Y 会跳去搜索页，模态弹层就被架空了。
         ⚠⚠ 十字键**不能吞**！它在这里只负责把方向记到 g_btn_dir，
            真正的「走一格」在 pad_move() 里 —— 那里才是弹层的接管点。
            这里要是把它 return 掉，g_btn_dir 永远是 0，
            pad_move() 压根不会被调用，弹层就「按不动了」：
            2026-09-15 第一版正是这么写的，用户随即复报
            「十字键还是无法控制弹层」。
            所以下面用 break（落回外层 switch）而不是 return。 */
        if (g_act_on) {
            switch (e->cbutton.button) {
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
            case SDL_CONTROLLER_BUTTON_DPAD_UP:
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
                break;              /* 放过去：下面会设置 g_btn_dir */
            case SDL_CONTROLLER_BUTTON_A:
                act_key(SDLK_RETURN);    return;
            case SDL_CONTROLLER_BUTTON_B:
                act_key(SDLK_BACKSPACE); return;
            default:
                return;             /* 其余功能键吞掉 */
            }
        }
        switch (e->cbutton.button) {
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  g_btn_dir = -1; pad_hold_sync(); return;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: g_btn_dir =  1; pad_hold_sync(); return;
        case SDL_CONTROLLER_BUTTON_DPAD_UP:    g_btn_dir = -2; pad_hold_sync(); return;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  g_btn_dir =  2; pad_hold_sync(); return;
        case SDL_CONTROLLER_BUTTON_A:  handle_key(SDLK_RETURN); return;
        /* B = 返回。主页上本来就无处可回，只有游戏页/搜索页/设置页/说明页有事可做。 */
        case SDL_CONTROLLER_BUTTON_B:
            if (g_page == PAGE_GAMES) leave_games();
            else if (g_page == PAGE_SEARCH) handle_key(SDLK_BACKSPACE);
            else if (g_page == PAGE_SETTINGS) settings_key(5);
            else if (g_page == PAGE_HELP) help_close();
            return;
        /* Y = 搜索。底栏第 4 项写的就是「Y 搜索」，网页版键盘对应
           y → showPage('search')。搜索页上再按一次 Y 不做事；
           设置页里也不响应（那页有自己的导航，按 Y 跳去搜索会回不来）；
           ⚠ 说明页也必须排除 —— 它是浮在主页上的只读页，按 Y 跳到搜索页
             再退回来会落在主页（搜索页的"从哪来"记的是主页），
             等于把人从设置里踢出去了。 */
        case SDL_CONTROLLER_BUTTON_Y:
            if (g_page != PAGE_SEARCH && g_page != PAGE_SETTINGS
                && g_page != PAGE_HELP) enter_search();
            return;
        /* L1 / R1 = 顶栏分类左右切换。
           用户 2026-09-14 指定要"顶栏 L/R 切" —— 方向键左右在主页是在
           卡片之间挪，不碰分类，所以分类得单独给一对键，两件事不打架。
           只在主页生效（cat_step 内部自己判页，游戏页/搜索页/设置页
           按了不动）。 */
        /* 说明页上 L1/R1 = 翻页（一次一屏）。别的页仍是切分类 ——
           分类只在主页有意义，cat_step 内部自己会判页。 */
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
            if (g_page == PAGE_HELP) help_scroll(-help_view_rows());
            else                     cat_step(-1);
            return;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
            if (g_page == PAGE_HELP) help_scroll(+help_view_rows());
            else                     cat_step(+1);
            return;
        /* START：主页进**设置页**（网页版就是这个行为：
           keydown 'Start' -> showPage('settings')）。
           搜索页仍是"清空输入"；设置页里当"返回上一级"。
           ⚠ 分类从 START 挪到 L/R 之后，START 就空出来了 —— 这一版把它
           接到设置上，正好补上原生前端一直缺的那一页。
           靠 pad_add_mapping() 补的那条映射才有这个事件。 */
        case SDL_CONTROLLER_BUTTON_START:
            if (g_page == PAGE_HOME)          settings_open();
            else if (g_page == PAGE_SEARCH)   handle_key(SDLK_TAB);
            else if (g_page == PAGE_SETTINGS) settings_key(5);
            else if (g_page == PAGE_HELP)     help_close();
            return;
        /* BACK(select) 在搜索页 = 返回；在设置页 = 返回上一级。
           主页上以前是「退出前端」，2026-09-15 撤掉了 —— 前端已经完全接管
           启动链路，退出去会被 emulationstation.sh 的循环原地拉起来，
           只白白丢掉续页状态（用户：「主界面 select 退出这个可以删掉了」）。
           手柄上没有 ESC，但真要走人有「设置 → 电源 / 退出」那页按钮，
           不是没路可走。 */
        case SDL_CONTROLLER_BUTTON_BACK:
            if (g_page == PAGE_SEARCH) leave_search();
            else if (g_page == PAGE_SETTINGS) settings_key(5);
            else if (g_page == PAGE_HELP) help_close();
            return;
        default: return;
        }
    }

    if (e->type == SDL_CONTROLLERBUTTONUP) {
        int d;
        switch (e->cbutton.button) {
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  d = -1; break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: d =  1; break;
        case SDL_CONTROLLER_BUTTON_DPAD_UP:    d = -2; break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  d =  2; break;
        default: return;
        }
        /* 抬起的正是当前按住的那个才清 —— 斜着同时按两个键时别互踩 */
        if (g_btn_dir == d) { g_btn_dir = 0; pad_hold_sync(); }
        return;
    }

    if (e->type == SDL_CONTROLLERAXISMOTION) {
        if (e->caxis.axis != 0 && e->caxis.axis != 1) return;
        int v = e->caxis.value;
        /* 死区 40%，对齐网页版 pad_deadzone_pct=40。
           硬件轴其实只有 -1800..1800，SDL 会归一化到 ±32767，
           这里按归一化后的量程算。 */
        const int DZ = 32767 * 40 / 100;
        int dir = 0;
        if (v > DZ || v < -DZ)
            dir = (e->caxis.axis == 0) ? (v > 0 ? 1 : -1)
                                       : (v > 0 ? 2 : -2);
        if (dir != g_axis_dir) { g_axis_dir = dir; pad_hold_sync(); }
        return;
    }
}

/* ==========================================================================
 *  屏幕旋转角度从哪来
 *   1) 环境变量 VG_ROT（优先）—— 启动器/命令行现场试角度用，改完重启前端即可，
 *      不用动任何文件：VG_ROT=90 / 0 / 180 / 270 / auto
 *   2) vgfront.conf 的 rotate=（设置项，持久）
 *   3) 都没有 → auto
 *
 *  auto 的判据（按可靠度从高到低）：
 *   ① 内核 cmdline 里的 `fbcon=rotate:N` —— 厂商就是靠它把 console 掰正的，
 *      说明面板肯定要转。内核文档写明 N 的含义：1=顺时针90°、2=180°、
 *      3=逆时针270°，也就是**画面要顺时针转 N*90°**（dArkOS 自己的
 *      SCREEN_ROTATION 也是这么用的：RGB10/RK2020 竖屏设备取 1）。
 *      所以直接跟它走最稳，厂商换值我们自动跟上。
 *   ② 读不到 cmdline 就退回"物理模式是竖的（w<h）→ 270"。
 *      MINILOONG 实测 cmdline 是 `fbcon=rotate:3` → 270°，两条路结论一致。
 * ========================================================================*/
static int rot_resolve(int pw, int ph)
{
    char b[32] = { 0 };
    const char *e = getenv("VG_ROT");

    if (!e || !*e) {
        if (!conf_get_str(SCONF_PATH, "rotate", b, sizeof(b))) b[0] = 0;
        e = b;
    }

    if (e && *e) {
        if      (!strcasecmp(e, "auto"))                 goto auto_;
        else if (!strcmp(e, "0"))                        return 0;
        else if (!strcmp(e, "90"))                       return 90;
        else if (!strcmp(e, "180"))                      return 180;
        else if (!strcmp(e, "270"))                      return 270;
        /* 认不出来的一律回 auto：一个手滑的配置不该把屏幕转歪到没法操作 */
        printf("rotate: 认不出的值 \"%s\"，按 auto 处理\n", e);
    }

auto_:
    {
        /* cmdline 打一行留档。**只是留档，不作依据** ——
           fbcon=rotate:N 管的是内核 console 和第一屏，管不到用户态。 */
        FILE *f = fopen("/proc/cmdline", "r");
        if (f) {
            char buf[2048];
            size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            buf[n] = '\0';
            fclose(f);
            const char *p = strstr(buf, "fbcon=rotate:");
            if (p)
                printf("rotate: 内核 cmdline 里有 fbcon=rotate:%c（只管 console）\n",
                       p[13]);                 /* "fbcon=rotate:" 正好 13 字节 */
        }

        /* ① 横的就不转。**这条必须先判**。
           MINILOONG 靠的就是这一条：那块面板是 720x960 **竖屏侧装**，但厂商
           把转屏做在了**下层** —— ES 走的 SDL2 是被改过的（二进制里有
           KMSDRM_InitRotateBuffer + librga 的 RGA blit，而且把 desktop_mode /
           current_mode / RESIZED 事件的宽高全对调了），RA 也自带 RGA 旋转
           （c_RkRgaBlit）。所以这类机器上 SDL 报给我们的**已经是 960x720 横屏**，
           我们照着画的也是横屏 —— 再转一次反而会歪。
           （厂商那份 SDL2 的补丁源码在 AmberELEC 的
             packages/games/tools/SDL2/patches/rotation/，对调宽高那几行就是铁证。） */
        if (pw >= ph) {
            printf("rotate: auto → 0°（%dx%d 已经是横的）\n", pw, ph);
            return 0;
        }

        /* ② 真是竖屏、又没人替我们转 → 自己转 270。
           （270 这个方向有两处佐证：内核 cmdline 的 fbcon=rotate:3，以及厂商
             SDL2 补丁里写死的 HAL_TRANSFORM_ROT_270。） */
        printf("rotate: auto → 270°（%dx%d 是竖屏，SDL 没替我们转）\n", pw, ph);
        return 270;
    }
}

int main(int argc, char **argv)
{
    /* 输出会被启动器重定向到日志**文件**，那是块缓冲 —— 不设这一行，
       界面在屏幕上跑着的几十分钟里，"屏幕/渲染器/手柄/索引"这几行全都压在
       stdio 缓冲里，日志上一片空白，排查只能靠猜。行缓冲之后每行立刻落盘。 */
    setvbuf(stdout, NULL, _IOLBF, 0);

    const char *idx_path = (argc > 1) ? argv[1] : "/opt/vagaretro/index.tsv";

    /* 游戏介绍目录 = 索引文件**同级**的 desc/（宿主 write_index 写在那儿，
       见 vagaretro-shell.py）。从 argv[1] 推、不写死，换索引路径也跟得上。 */
    {
        const char *sl = strrchr(idx_path, '/');
        if (sl && (int)(sl - idx_path) < (int)sizeof(g_desc_dir) - 8) {
            int n = (int)(sl - idx_path);
            memcpy(g_desc_dir, idx_path, n);
            g_desc_dir[n] = '\0';
            strncat(g_desc_dir, "/desc",
                    sizeof(g_desc_dir) - strlen(g_desc_dir) - 1);
        }
    }

    int max_frames = (argc > 2) ? atoi(argv[2]) : 0;   /* 调试用：跑 N 帧就退 */
    int shot_frame = (argc > 3) ? atoi(argv[3]) : 0;   /* 调试用：第 N 帧存 BMP */

    /* 品牌画面默认开（见 g_want_splash）。两个开关都是无值、位置无关：
         --no-splash  关掉（截图脚本用，省掉那半秒动画）
         --splash     显式打开（settings_restart 往下传，语义更明确） */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-splash") == 0) g_want_splash = 0;
        if (strcmp(argv[i], "--splash")    == 0) g_want_splash = 1;
    }
    /* 调试/截图模式（argv[2] = 跑 N 帧就退）一律不播 —— 品牌画面是给真机
       用户看的，截图脚本每页都要多等半秒纯属浪费（--no-splash 也能关）。 */
    if (max_frames > 0) g_want_splash = 0;
    g_shot_mode = (max_frames > 0);

    if (idx_load(idx_path, &IX) < 0) return 1;
    /* 索引读完才能挂收藏 —— 收藏条目是靠 pid+rel 回到索引里找 Game 的。
       必须在 idx_load 之后、任何绘制之前。 */
    favorites_load();
    history_load();                    /* 和收藏一样，必须在 idx_load 之后 */
    printf("索引: %d 平台 / %d 游戏 / 根=%s\n", IX.np, IX.total_games,
           IX.rom_root ? IX.rom_root : "(无)");

    g_view = malloc(sizeof(int) * (size_t)(IX.np + 1));
    build_view();

    /*   --matchdump  把"每个游戏 → 匹配到的 cht"全量打出来（只读、立刻退出、
     *               **不碰 SDL**，所以不受"要等 12 秒出画面"的拖累）。
     * 为什么要有它：cheat_find_file() 的匹配规则改一次，就得知道全量影响面 ——
     *   而 5573 款只有**真机上真跑一遍**才作数，Python 侧的复刻再怎么对齐也会漂移
     *   （2026-09-18 实锤：模拟器里的白名单多两个词，数字就对不上了，现在改成
     *   从本文件解析词表）。这条就是给"改完匹配规则立刻对账"用的回归工具。
     * 输出 TSV（stdout）：平台 \t rel \t 显示名 \t 命中的 cht（空 = 没命中）；
     *   末行 `#` 开头是汇总。 */
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--matchdump") == 0) {
            int all = 0, hits = 0;
            for (int pi = 0; pi < IX.np; pi++) {
                Platform *p = &IX.p[pi];
                if (!p->pid || !p->pid[0]) continue;
                /* ⚠ 收藏/历史是**虚拟平台**，里面的条目是真实平台游戏的副本
                   （src_pid 指回真实平台）—— 这里必须跳过，否则同一款游戏被数两遍。
                   ⚠ 也**不能**用 act_real_pid()：它看的是全局游标 g_act_gi
                   （弹层当前指向哪个游戏），遍历时必须按 gi 自己取，
                   而且真实平台的游戏 src_pid 恒为 NULL，直接用 p->pid 就对。 */
                if (strcmp(p->pid, "history") == 0 ||
                    strcmp(p->pid, "favorite") == 0) continue;
                for (int gi = 0; gi < p->ngames; gi++) {
                    const char *rel = p->games[gi].rel;
                    const char *ttl = p->games[gi].title;
                    char fn[256];
                    int h = cheat_find_file(p->pid, rel, ttl, fn, sizeof(fn));
                    printf("%s\t%s\t%s\t%s\n", p->pid, rel ? rel : "",
                           ttl ? ttl : "", h ? fn : "");
                    all++;
                    if (h) hits++;
                }
            }
            printf("# 合计: %d 款中有 %d 款命中（%.1f%%）\n",
                   all, hits, all ? hits * 100.0 / all : 0.0);
            fflush(stdout);
            return 0;
        }

    /* 搜索结果表：定长，一次拿好。放这里而不是等进搜索页再拿，
       免得"按 Y 那一下"要处理 malloc 失败。 */
    g_res = malloc(sizeof(SRes) * SR_NMAX);

    /* 截图/调试模式**不打开手柄**：离屏实例跟真机上跑着的那个共用同一个
       手柄设备，两边都收事件的话，截图进程会跟着真机上的按键乱跳
       （甚至误触发启动游戏 —— 见 g_shot_mode 的注释）。 */
    /* ⚠ 这里**故意只初始化 VIDEO**，手柄/控制器子系统延后到第一帧画完再起
       （见下面 boot_splash() 之后那段 SDL_InitSubSystem 的注释）。
       原因：JOYSTICK|GAMECONTROLLER 要枚举 /dev/input 下所有设备、还要载入
       SDL 的控制器映射库，实测吃掉近 1 秒；而这一秒恰好落在"KMS 已经重建完、
       第一帧还没画"的空窗里，屏幕上就是纯黑 —— 用户 2026-09-15 报的
       "开机出现黑屏"就是它（实拍：blackdetect 只测出这一段 1.0 秒）。
       SDL_InitSubSystem 幂等，后面补初始化完全安全。 */
    Uint32 sdl_flags = SDL_INIT_VIDEO;
    if (SDL_Init(sdl_flags) != 0) {
        printf("FATAL SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    /* 封面是 PNG。IMG_Init 失败不致命 —— 退化成"所有卡都没封面"，
       界面上仍然看得到标题，比整个前端起不来强。 */
    if (!(IMG_Init(IMG_INIT_PNG) & IMG_INIT_PNG)) {
        printf("警告: IMG_Init(PNG) 失败（%s），本次不显示封面\n",
               IMG_GetError());
    }
    if (TTF_Init() != 0) {
        printf("FATAL TTF_Init: %s\n", TTF_GetError());
        return 1;
    }

    /* 分辨率先不写死：拿显示模式的真实尺寸再算缩放 */
    SDL_DisplayMode dm;
    if (SDL_GetDesktopDisplayMode(0, &dm) != 0) {
        printf("FATAL 取显示模式: %s\n", SDL_GetError());
        return 1;
    }

    /* 窗口按 DRM 报的模式尺寸开 —— KMSDRM 要的就是 CRTC 那个模式。
       （厂商改过 SDL2 的机器上，全屏窗口的实际 surface 由 SDL 自己定，
         下面那句查出来的才是真的。） */
    WIN = SDL_CreateWindow("VagaRetro Native",
                           SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           dm.w, dm.h,
                           SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_SHOWN);
    if (!WIN) { printf("FATAL CreateWindow: %s\n", SDL_GetError()); return 1; }

    /* ★ 用"可绘制尺寸"，不是显示模式尺寸 —— 它才是我们真正能画的那块 surface。
       MINILOONG 上厂商改过的 SDL2 会把宽高对调（模式 720x960 → 可绘制 960x720），
       这里查出来就是横的 → auto 判 rot=0，不会叠成转两次。
       逻辑尺寸 = 人眼看到的；侧装竖屏而没人替我们转时两者互换 —— 而布局一律按
       **逻辑尺寸**算，所以下面所有 g_win_w/g_win_h 的用法一个字都不用改。 */
    {
        int aw = 0, ah = 0;
        SDL_GL_GetDrawableSize(WIN, &aw, &ah);   /* 2.0.1 起就有，比 2.26 的
                                                    SDL_GetWindowSizeInPixels 保险 */
        if (aw <= 0 || ah <= 0) { aw = dm.w; ah = dm.h; }
        g_phys_w = aw;
        g_phys_h = ah;
    }
    g_rot = rot_resolve(g_phys_w, g_phys_h);
    if (g_rot == 90 || g_rot == 270) {
        g_win_w = g_phys_h;
        g_win_h = g_phys_w;
    } else {
        g_win_w = g_phys_w;
        g_win_h = g_phys_h;
    }
    printf("屏幕: DRM %dx%d  可绘制 %dx%d  逻辑 %dx%d  旋转 %d°\n",
           dm.w, dm.h, g_phys_w, g_phys_h, g_win_w, g_win_h, g_rot);

    g_scale = (float)g_win_w / (float)DESIGN_W;

    /* 硬件能力探测 + 设置页按机型裁剪（2026-09-21，为 RK3326 移植加）。
       放在这里：屏幕尺寸已定、bake_all() 还没跑，设置页任何入口都还没画。
       ★ 摇杆灯入口是 IT_DISP 的最后一项，没有 aw20036 就把它藏掉。 */
    hw_probe();
    {
        int want = IT_DISP_N;
        if (!hw_has_led()) want -= 1;          /* 藏掉「摇杆灯」 */
        if (want < 0) want = 0;
        MENUS[M_DISP].n = want;
        if (want != IT_DISP_N)
            printf("设置页: 「显示」藏掉 %d 项（无摇杆灯硬件），实显 %d 项\n",
                   IT_DISP_N - want, want);
    }

    /* 封面/图标缩放用线性过滤：默认的最近邻在"正方形"这类卡片尺寸下
       会把封面缩出一身锯齿，远处看着像花了。Mali-G31 上线性采样的开销
       可以忽略，画质差别却是一眼可见。 */
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");

    const char *want[] = { "opengles2", "opengl", "software", NULL };
    for (int i = 0; want[i] && !REN; i++) {
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, want[i]);
        Uint32 fl = strcmp(want[i], "software") ? SDL_RENDERER_ACCELERATED
                                                : SDL_RENDERER_SOFTWARE;
        REN = SDL_CreateRenderer(WIN, -1, fl);
    }
    if (!REN) { printf("FATAL CreateRenderer: %s\n", SDL_GetError()); return 1; }
    SDL_RendererInfo ri;
    SDL_GetRendererInfo(REN, &ri);
    printf("渲染器: %s (加速=%s)\n", ri.name,
           (ri.flags & SDL_RENDERER_ACCELERATED) ? "是" : "否");
    /* 软件渲染器对 RenderGeometry 有毛病（四个顶点只画出其中一个三角形），
       旋转上屏得换成手动搬像素那条路 —— 见 frame_flush_soft()。 */
    g_soft = !(ri.flags & SDL_RENDERER_ACCELERATED);
    SDL_SetRenderDrawBlendMode(REN, SDL_BLENDMODE_BLEND);

    /* 脏帧缓存用的离屏目标 */
    g_frame_tex = SDL_CreateTexture(REN, SDL_PIXELFORMAT_ARGB8888,
                                    SDL_TEXTUREACCESS_TARGET, g_win_w, g_win_h);
    if (!g_frame_tex) {
        printf("FATAL 建帧缓存失败: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetTextureBlendMode(g_frame_tex, SDL_BLENDMODE_NONE);

    /* 本前端自己的设置（主页/游戏列数、两档字号、遮罩）必须在**字库加载之前**
       读进来。字体是按 FS_UI()/FS_G() 算出来的像素尺寸直接 TTF_OpenFont 的，
       下面的 fonts[] 表一建，字号就定死了 —— 原来 sconf_load() 排在
       bake_all() 之后，字体永远按默认 100% 出炉，设置页把「游戏列表字号」
       拉到 300% 也只是改了一个再没人读的整数，屏幕上一丝变化都没有。
       （2026-09-15 用户报"调不动"，根因就是这一行位置。挪过两次才对：
         第一次挪到 theme_load() 前面，结果字体表在 main 更靠前的地方建的。） */
    sconf_load();

    /* ⚠ 主题也必须在字库加载之前定下来 —— 2026-09-15 从 fonts[] 表之后挪上来。
       主题文件现在不只有配色，还带**卡片外观**（cd_* 字段），其中
       cd_name_fs / cd_sub_fs / cd_logo_fs 是直接拿去 TTF_OpenFont 的像素尺寸：
       晚一步读，字号就永远按默认出炉，主题里那几根字号滑杆等于没接线。
       顺带修掉一处旧毛病 —— 原来 theme_load() 排在下面那次
       boot_splash(NULL, 0) 之后，自重启的第一帧徽标画的是**上一套**主题的底色。 */
    theme_load();

    /* 自重启：KMS 一重建屏幕就全黑，而字体还在下面的 fonts[] 表里没加载，
       先铺一层主题底 + 光晕顶着（无字版），字体到位后再画完整版。 */
    if (g_want_splash) boot_splash(NULL, 0);

    /* ---- 手柄/控制器子系统：挪到第一帧画完之后才起 ------------------------
       上面那次 SDL_Init 只起了 VIDEO。到这里 boot_splash() 刚铺完第一帧，
       屏幕上有内容了；再花那近 1 秒枚举手柄，用户就看不到黑屏。
       实测收益：开机黑屏窗口从 1.0 秒压到约 0.3 秒（blackdetect 复测）。
       ⚠ 顺序不能反 —— 后面的 pad_add_mapping() 和 SDL_NumJoysticks() 都要求
       这两个子系统已经就绪。
       ⚠ 截图/调试模式仍然不碰手柄：理由同原来 SDL_Init 那处 —— 离屏实例会和
       真机上跑着的那个共用同一批输入设备，两边都收事件的话截图进程会跟着
       真机上的按键乱跳，甚至误触发启动游戏。 */
    if (!g_shot_mode &&
        SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) != 0)
        printf("警告: 手柄子系统初始化失败（%s），本次可能操作不了界面\n",
               SDL_GetError());

    /* --- 字体（选型理由见文件头 FONT_PATH 的注释）---
       字号一律走 FS_UI()/FS_G()：前者跟"前端字体"那档，后者再乘"游戏列表字体"。
       FS_G 只给游戏列表页那几个字体用。 */
    struct { TTF_Font **f; int px; const char *tag; } fonts[] = {
        { &F_LOGO,    FS_UI(LOGO_FS),    "logo"    },
        { &F_H1,      FS_UI(HDR_H1_FS),  "h1"      },
        { &F_TIME,    FS_UI(TIME_FS),    "time"    },
        { &F_TIMESUB, FS_UI(TIMESUB_FS), "time-sub"},
        { &F_SB,      FS_UI(SB_FS),      "search"  },
        { &F_TAB,     FS_UI(TAB_FS),     "tab"     },
        { &F_TABB,    FS_UI(TAB_B_FS),   "tab-cnt" },
        { &F_PLOGO,   FS_UI(PL_FS),      "plat-logo"},
        { &F_PCNT,    FS_UI(PCNT_FS),    "plat-cnt"},
        { &F_PTAG,    FS_UI(PTAG_FS),    "plat-tag"},
        { &F_PNAME,   FS_UI(PN_FS),      "plat-name"},
        { &F_PSUB,    FS_UI(PSUB_FS),    "plat-sub"},
        { &F_PCORE,   FS_UI(PCORE_FS),   "plat-core"},
        { &F_PBI,     FS_UI(PBI_FS),     "pad-label"},
        { &F_PBK,     FS_UI(PBK_FS),     "pad-key" },
        /* 游戏列表页。注意**只有卡片上的字**吃"游戏列表字体"那档：
           标题(F_GH1)、返回(F_BACK)、信息行(F_GINFO) 都走 FS_UI ——
           它们贴在屏幕顶部、可用宽度是死的，跟着 3 倍放大直接冲出屏幕
           （2026-09-15 实测 300% 时标题只剩 "Game Boy Adva" 在屏上）。
           卡片名字本来就靠 ellipsize 收边，放大才是用户真正要的效果。 */
        { &F_BACK,    FS_UI(GB_FS),      "back"      },
        { &F_GH1,     FS_UI(GH_H1_FS),   "game-h1"   },
        { &F_GINFO,   FS_UI(GH_INFO_FS), "game-info" },
        { &F_GCOVER,  FS_G(GCOVER_FS),  "game-cover"},
        { &F_GNOV,    FS_G(GNOV_FS),    "game-name" },
        /* 搜索页 */
        { &F_STITLE,  FS_UI(SP_TITLE_FS), "search-title"},
        { &F_SINPUT,  FS_UI(SP_IN_FS),    "search-input"},
        { &F_SKEY,    FS_UI(KB_KEY_FS),   "search-key"  },
        /* 设置页 */
        { &F_SETN,    FS_UI(SET_NAME_FS), "set-name"    },
        { &F_SETV,    FS_UI(SET_VAL_FS),  "set-value"   },
        { &F_SETF,    FS_UI(SET_FOOT_FS), "set-foot"    },
    };
    int font_fail = 0;
    for (unsigned i = 0; i < sizeof(fonts) / sizeof(fonts[0]); i++) {
        *fonts[i].f = TTF_OpenFontIndex(FONT_PATH, fonts[i].px, FONT_FACE);
        if (!*fonts[i].f) {
            printf("警告: 字体 %s 打不开(%dpx): %s\n",
                   fonts[i].tag, fonts[i].px, TTF_GetError());
            font_fail++;
        }
    }
    if (font_fail)
        printf("★ %d 个字体丢失 —— 界面上的字会缺，先查 %s\n",
               font_fail, FONT_PATH);

    /* CSS 里这些位置是 font-weight:bold。用合成粗体顶上，
       不另加载一份 20MB 的 Bold.ttc。 */
    {
        TTF_Font *bolds[] = { F_LOGO, F_TIME, F_H1, F_PLOGO,
                              F_PNAME, F_PCNT, F_PBK, F_GH1, F_STITLE };
        for (unsigned i = 0; i < sizeof(bolds) / sizeof(bolds[0]); i++)
            if (bolds[i]) TTF_SetFontStyle(bolds[i], TTF_STYLE_BOLD);
    }
    F_BODY = F_PSUB;
    F_SMALL = F_PTAG;

    /* ⚠ 主题在更上面（sconf_load() 之后）就读好了，这里不能再读一遍 ——
       会覆盖已经建好的字体尺寸。颜色必须在 bake_all() **之前**定下来
       （颜色是烘进纹理的），那一条由上面那次 theme_load() 满足。 */

    /* 自重启的品牌画面（见 boot_splash）。排在 theme_load() 之后 ——
       这样它用的是**新**主题的配色；排在 bake_all() 之前 ——
       这几百毫秒正好盖住"载入索引 → 烘纹理"这段最长的黑。 */
    if (g_want_splash) boot_splash("正在载入游戏库…", 700);

    bake_all();

    /* 必须在开 controller 之前补映射，否则打开的已经是没 start/back 的旧表 */
    pad_add_mapping();

    int njoy = SDL_NumJoysticks();
    printf("手柄: %d 个\n", njoy);
    for (int i = 0; i < njoy; i++) {
        int isgc = SDL_IsGameController(i);
        printf("  [%d] %s  isGC=%d\n", i, SDL_JoystickNameForIndex(i), isgc);
        /* isGC=0 的话下面这个 open 会被跳过，手柄一点反应都没有却也不报错 */
        if (isgc) {
            SDL_GameController *gc = SDL_GameControllerOpen(i);
            if (gc) pad_dump_bindings(gc);
        }
    }

    /* 从游戏退回来时续到原来那一页。放在调试钩子**之前** ——
       钩子是给截图脚本用的，优先级更高，要能压过续页状态。
       （本前端自己的设置已经在前面读过，就在 theme_load() 之前。） */
    state_load();

    /* 续页状态可能把一个"现在没游戏"的分类恢复回来（那个机种的 ROM
       本来就没放，或被挪走了）→ 标签栏没高亮、网格全空。兜底换个分类。 */
    cat_fallback();

    /* 调试钩子（截图核对版式用）：
     *   --cat N    直接切到第 N 个分类
     *   --games N  直接落到第 N 个平台的游戏页
     *   --gsel N   预置游戏页的选中项 —— 用来验证滚动和 ensure_visible
     * 真实交互还是走键盘/手柄，这几条只是让截图脚本能一步到位。 */
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "--cat") == 0) {
            int c = atoi(argv[i + 1]);
            if (c >= 0 && c < NCAT) { g_cat = c; build_view(); }
        } else if (strcmp(argv[i], "--games") == 0) {
            enter_games(atoi(argv[i + 1]));
        }
    }
    for (int i = 1; i < argc - 1; i++)       /* 得在 --games 之后 */
        if (strcmp(argv[i], "--gsel") == 0) g_gsel = atoi(argv[i + 1]);

    /*   --search Q  直接落到搜索页并预置输入串，用来截图核对这一页的版式。
     *             Q 里的字母会折成大写（软键盘上只有大写）。
     *   --sfocus N  0 = 光标在软键盘上，1 = 在结果列表上。 */
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], "--search") == 0) {
            enter_search();
            snprintf(g_q, sizeof(g_q), "%s", argv[i + 1]);
            for (char *q = g_q; *q; q++)
                if (*q >= 'a' && *q <= 'z') *q = (char)(*q - 32);
            g_qlen = (int)strlen(g_q);
            search_run();
            if (g_res_n) g_sfocus = 1;
        }
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], "--sfocus") == 0) g_sfocus = atoi(argv[i + 1]);

    /*   --settings M  直接落到设置页（M 省略/0 = 根菜单，1..5 = 各子菜单），
     *              配合 --setrow R 定光标行。走的是真实入口 settings_open()，
     *              不是旁路，用来截图核对这一页的版式与取值。 */
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], "--settings") == 0) {
            int mm = atoi(argv[i + 1]);
            settings_open();
            if (mm > 0 && mm < NMENU) {
                g_setmenu = mm;
                g_setrow = 0;
                g_settop = 0;
                /* WiFi / 主题两个列表的条目都是运行期填的 —— 直跳进来
                   不会经过 setting_confirm()，得在这里补一次，否则那页
                   是空的（真机走按键路径，不存在这个问题）。 */
                if (mm == M_WIFI)  wifi_scan();
                if (mm == M_THEME) theme_scan();
            }
            mark_dirty();
        }
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], "--setrow") == 0) {
            int r = atoi(argv[i + 1]);
            int n = MENUS[g_setmenu].n;
            if (r >= 0 && r < n) { g_setrow = r; mark_dirty(); }
        }

    /*   --kb  直接拉出 WiFi 密码软键盘并预填几个字符。
     *         真机上要走到这一步得先扫到一个没存过密码的网络、再按 A，
     *         截图脚本没法稳定复现，所以给个直通钩子核对版式。
     *         注意这里是 i < argc（无值开关），不是别的钩子那种 argc - 1。
     *         必须放在 --settings 之后 —— settings_open() 会把弹层关掉。 */
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--kb") == 0) {
            kb2_open("MyHome_5G");
            snprintf(g_kb_buf, sizeof(g_kb_buf), "Ab1-_@");
            mark_dirty();
        }

    /*   --act N  直接打开游戏操作弹层、光标停在第 N 项（走真实入口 act_open()，
     *           不是旁路）。配合 --actsub W S 还能把二级列表也展开：
     *           W = 1 核心列表 / 2 着色器列表 / 3 金手指列表，
     *           S = 那个列表里的光标行。
     *           弹层和两个二级列表的版式都靠它截图核对。
     *           ⚠ 必须放在 --settings 之后 —— settings_open() 会把弹层关掉；
     *             也得放在 --games/--gsel 之后，它要用那两个算出来的选中项。
     *           ★ 2026-09-18 修：--games N 之后，弹层要落在**N 那个平台**上。
     *             以前只会取 g_view[g_sel]（主页网格的选中项），而 --games
     *             走的是 enter_games()→g_plat，两者根本不搭界 —— 于是
     *             "--games 2（FC）--gsel 0" 拍出来的却是"游玩历史 / 三目童子"，
     *             截图脚本怎么指都指不到想拍的游戏（req626/628/631 连踩三次）。 */
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], "--act") == 0) {
            int row = atoi(argv[i + 1]);
            int on_games = (g_page == PAGE_GAMES && g_plat >= 0 && g_plat < IX.np
                            && IX.p[g_plat].ngames > 0);
            int pi = on_games ? g_plat
                              : ((g_sel >= 0 && g_sel < g_view_n) ? g_view[g_sel] : -1);
            int gi = g_gsel;
            /* 落到的平台是空的（分类过滤后头一个是虚拟平台、又恰好没内容）
               就往后找一个真有游戏的 —— 截图脚本不该因为"第一个平台是空的"
               就拍出一张空图。 */
            if (pi < 0 || pi >= IX.np || IX.p[pi].ngames <= 0) {
                for (int k = 0; k < g_view_n; k++) {
                    int q = g_view[k];
                    if (q >= 0 && q < IX.np && IX.p[q].ngames > 0) { pi = q; gi = 0; break; }
                }
            }
            if (pi >= 0 && pi < IX.np && IX.p[pi].ngames > 0) {
                if (gi < 0 || gi >= IX.p[pi].ngames) gi = 0;
                act_open(pi, gi);
                if (row >= 0 && row < ACT_N) g_act_sel = row;
                /* row 是**项号**（0=启动 … 5=金手指 / 6=删除）。没有金手指的
                   游戏第 5 项是藏着的，指到它会被 act_sel_fix() 挪回"启动游戏"。 */
                act_sel_fix(&IX.p[pi]);
                mark_dirty();
            }
        }
    for (int i = 1; i < argc - 2; i++)
        if (strcmp(argv[i], "--actsub") == 0) {
            int which = atoi(argv[i + 1]);
            int srow  = atoi(argv[i + 2]);
            Platform *p = act_plat();
            if (g_act_on && p) {
                /* which: 0/1 = 核心列表，2 = 着色器列表，3 = 金手指列表。
                   （4 = 加载存档列表，2026-09-17 随该功能一起删除） */
                if (which == 3) {
                    g_act_cheat = 1;
                    act_cheat_sync(p);
                    /* srow 现在能指到条目行了（2..2+条数-1），默认还是总开关那行 */
                    g_act_chsel = (srow >= 0 && srow < cheat_nrow()) ? srow : 1;
                    act_cheat_scroll();
                } else if (which == 2) {
                    g_act_shader = 1;
                    act_shader_sync(p);
                    if (srow >= 0 && srow < SHADER_N) g_act_ssel = srow;
                    act_shader_scroll();
                } else {
                    g_act_core = 1;
                    act_core_sync(p);
                    int n = act_core_sel_count(p);
                    if (srow >= 0 && srow < n) g_act_csel = srow;
                    act_core_scroll();
                }
                mark_dirty();
            }
        }

    /*   --keys SEQ  启动后每帧喂一个按键（走的还是 handle_key()，不是旁路），
     *             用来在真机上自动验证搜索页的导航与输入。
     *             字符表：L R U D / A 确认 / B 退格 / T TAB / Y 搜索 */
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], "--keys") == 0) g_keyseq = argv[i + 1];

    /*   --launch N  直接"当作按了 A"启动当前平台的第 N 个游戏。
     * 真机上没法用 xdotool 之类注入按键，这是唯一能自动验证阶段2
     * 全链路的钩子（生成 launch.sh → 退出 → 外层循环执行）。
     * 它和真实按键走的是**同一个** launch_game()，不存在"只在调试路径下对"。
     * 需要配合 --games N 用；不执行 launch.sh 本身（那会真抢屏）。 */
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], "--launch") == 0) {
            Platform *p = cur_plat();
            int gi = atoi(argv[i + 1]);
            /* 同步 g_gsel —— 真实按键路径下光标本来就在那一项上，
               钩子得跟它一致，否则续页状态文件里存的是 0，测不准 */
            if (p && gi >= 0 && gi < p->ngames) g_gsel = gi;
            if (p && launch_game(p, gi)) g_quit = 2;
        }

    int running = 1, frames = 0;
    int prev_sel = -1, prev_scroll = -1, prev_cat = -1;
    int prev_page = -1, prev_gsel = -1, prev_gscroll = -1, prev_plat = -1;
    long prev_min = -1;
    /* 状态栏四个读数上一轮的值。初值故意取得跟 status_poll() 的初始值不同，
       这样第一轮必置脏（反正启动第一帧本来就该画）。 */
    int prev_sv = -2, prev_sb = -2, prev_sc = -9, prev_sw = -2;
    Uint32 t0 = SDL_GetTicks();
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = 0;
            else if (e.type == SDL_KEYDOWN) {
                /* ESC 在游戏页/搜索页/设置页都是"返回"，只有主页才是退出程序。
                   设置页尤其不能退：按 ESC 直接结束前端，用户会以为"设置崩了"
                   （而且外层会走收尾逻辑把服务停掉，界面就再也不在了）。 */
                if (e.key.keysym.sym == SDLK_ESCAPE) {
                    if (g_page == PAGE_GAMES) leave_games();
                    else if (g_page == PAGE_SEARCH || g_page == PAGE_SETTINGS)
                        handle_key(SDLK_ESCAPE);
                    else running = 0;
                } else handle_key(e.key.keysym.sym);
            } else if (e.type == SDL_CONTROLLERBUTTONDOWN ||
                       e.type == SDL_CONTROLLERBUTTONUP ||
                       e.type == SDL_CONTROLLERAXISMOTION) {
                handle_pad(&e);
            }
        }

        /* --keys 的按键序列：每帧喂一个。**不旁路** —— 直接调 handle_key()，
           所以它验证的就是真实按键走的那条路。 */
        if (g_keyseq && g_keyseq[g_keyseq_i]) {
            SDL_Keycode ks = SDLK_UNKNOWN;
            int pad_hit = 0;   /* 这一个字符喂的是 handle_pad()，不是 handle_key() */
            switch (g_keyseq[g_keyseq_i++]) {
            /* 小写 = 走**手柄那条路**：真的造一个 SDL_CONTROLLERBUTTONDOWN
               丢给 handle_pad()。
               ⚠ 千万别图省事直接调 pad_move() —— 那样会绕过 handle_pad
                 里的模态守卫，查不出「守卫把十字键一起吞了」这类漏洞。
                 2026-09-15 就是这么被骗过去的：直接调 pad_move 的测试
                 全绿，真机上十字键却依旧穿透。
               小写字符表：u/d/l/r = 十字键，a/b = 手柄 A/B，
               { } = 手柄 L1 / R1（2026-09-16 补 —— 原先这两个字符是**直接
               调 cat_step()**，理由是"肩键注入不了"。注入得了：跟上面几个
               一样造个事件喂 handle_pad() 就行。直接调 cat_step() 会绕过
               handle_pad 的整段分派（说明页翻页、弹层模态守卫都在那儿），
               测出来的"肩键正常"是假绿。 */
            case 'u': case 'd': case 'l': case 'r':
            case 'a': case 'b': case '{': case '}': {
                SDL_Event pev;
                SDL_zero(pev);
                pev.type = SDL_CONTROLLERBUTTONDOWN;
                char pc = g_keyseq[g_keyseq_i - 1];
                pev.cbutton.button =
                    pc == 'u' ? SDL_CONTROLLER_BUTTON_DPAD_UP       :
                    pc == 'd' ? SDL_CONTROLLER_BUTTON_DPAD_DOWN     :
                    pc == 'l' ? SDL_CONTROLLER_BUTTON_DPAD_LEFT     :
                    pc == 'r' ? SDL_CONTROLLER_BUTTON_DPAD_RIGHT    :
                    pc == 'a' ? SDL_CONTROLLER_BUTTON_A             :
                    pc == '{' ? SDL_CONTROLLER_BUTTON_LEFTSHOULDER  :
                    pc == '}' ? SDL_CONTROLLER_BUTTON_RIGHTSHOULDER :
                                SDL_CONTROLLER_BUTTON_B;
                handle_pad(&pev);
                pad_hit = 1;
                break;
            }
            case 'L': ks = SDLK_LEFT;      break;
            case 'R': ks = SDLK_RIGHT;     break;
            case 'U': ks = SDLK_UP;        break;
            case 'D': ks = SDLK_DOWN;      break;
            case 'A': ks = SDLK_RETURN;    break;
            case 'B': ks = SDLK_BACKSPACE; break;
            case 'T': ks = SDLK_TAB;       break;
            case 'Y': ks = SDLK_y;         break;
            /* { } （L1 / R1）已经并到上面的"手柄路"那个 case 里了 ——
               它们现在也是造事件喂 handle_pad()。
               ⚠ 别在这儿再加一份 case '{' / case '}'：同一条 switch 里
                 重复的 case 值是**编译错误**（duplicate case value）。 */
            default: break;
            }
            if (pad_hit) { /* 已经喂给 handle_pad()，别再喂一遍 */ }
            else if (ks != SDLK_UNKNOWN) handle_key(ks);
        }

        /* 长按连发：按住方向不放时，这一步负责每隔 PAD_HOLD_RATE 走一格 */
        pad_tick();
        settings_tick();          /* 设置页底部回执的 4 秒超时 */
        /* ⚠ 退出前**别**再渲染：重启/关机/重启前端这几条路径刚播完 LANGOS
           品牌画面（见 do_action），走到末尾那句 render() 会把旧界面重新
           Copy 出去，那画面就白播了。break 直接跳到收尾，画面保持品牌图。 */
        if (g_quit) { running = 0; break; }

        int clip_y1 = g_win_h - padbar_zone();
        if (g_page == PAGE_GAMES) {
            ensure_visible_games(clip_y1);
        } else if (g_page == PAGE_SEARCH) {
            ensure_visible_search();
        } else {
            int y = 0;
            y += topbar_h() + tabs_h();
            y += S(HDR_PAD_T) + (int)(FS_UI(HDR_H1_FS) * 1.3) + S(10)
               + (int)(FS_UI(PSUB_FS) * 1.3) + S(HDR_PAD_B);
            ensure_visible(y, clip_y1);
        }

        /* 只有状态真的变了才重画整屏 —— 静态界面下每帧只做一次
           RenderCopy，这才是帧率能稳在 vsync 上的关键。
           分钟跳变也要重画，否则顶栏的时间会停住。 */
        long cur_min = (long)(time(NULL) / 60);
        if (g_sel != prev_sel || g_scroll != prev_scroll ||
            g_cat != prev_cat || g_page != prev_page ||
            g_gsel != prev_gsel || g_gscroll != prev_gscroll ||
            g_plat != prev_plat || cur_min != prev_min) {
            mark_dirty();
            prev_sel = g_sel;
            prev_scroll = g_scroll;
            prev_cat = g_cat;
            prev_page = g_page;
            prev_gsel = g_gsel;
            prev_gscroll = g_gscroll;
            prev_plat = g_plat;
            prev_min = cur_min;
        }

        /* ★★ 状态栏必须在这里**每帧**问一次（status_poll() 内部 1.5 秒节流），
           不能只挂在 draw_topbar() 里 —— 那条路只有 g_dirty 时才走得到，
           而重绘条件（光标/滚动/分类/页码/平台/分钟跳变）里**没有**"状态栏读数变了"。

           这就是"按实体音量键，状态栏的音量不会动"的最终根因：
           实体音量键走 ogage → volume.sh → amixer，**前端一个字的事件都收不到**，
           于是永远不会置脏、永远不会重绘、draw_topbar() 也就永远不被调用，
           数字冻在开机那一次；顶多等"分钟跳变"才顺带更新一回。
           （而 alsa_volume 的缓存问题是**上一层**的 bug，2026-09-18 已先修掉，
             两层都修完状态栏才会跟着实体键走。）

           检测到变化就置脏，下一句 render() 自然重画。 */
        status_poll();
        if (prev_sv != g_st_vol || prev_sb != g_st_bat ||
            prev_sc != g_st_chg || prev_sw != g_st_wif) {
            if (prev_sv != g_st_vol)
                printf("[状态栏] 音量 %d -> %d\n", prev_sv, g_st_vol);
            prev_sv = g_st_vol; prev_sb = g_st_bat;
            prev_sc = g_st_chg; prev_sw = g_st_wif;
            mark_dirty();
        }

        int want_shot = (shot_frame > 0 && frames + 1 == shot_frame);
        render(want_shot);
        frames++;

        if (want_shot) running = 0;
        if (max_frames && frames >= max_frames) running = 0;
    }

    double el = (SDL_GetTicks() - t0) / 1000.0;
    printf("渲染 %d 帧 / %.2f s -> %.1f fps\n", frames, el,
           frames / (el > 0 ? el : 1));

    /* 两种退出对启动器是同一件事（rc=0），但对日志不是：
       看日志时要能一眼分清"玩家按了退出"和"去启动游戏了"。
       启动器只看 launch.sh 在不在，所以 rc 一律 0 —— 千万别在这里
       因为"是启动游戏"就换个非 0 的码，那会被 run_sdl 当成渲染器失败。 */
    if (g_quit == 2) {
        printf("退出: 启动请求已写出（%s），屏幕交给模拟器\n", LAUNCH_REQ);
    } else {
        /* 用户自己退出的 —— 把续页状态清掉，否则下次开机还会停在这一页。
           只有"去启动游戏"那条路才该留下这个文件。 */
        unlink(STATE_PATH);
        if (g_quit == 1) printf("退出: 用户按键退出\n");
    }

    SDL_Quit();
    return 0;
}
