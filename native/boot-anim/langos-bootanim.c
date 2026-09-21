/*
 * langos-bootanim.c —— LANGOS 开机动画（userspace splash）· 分辨率自适应版
 *
 * 背景（2026-09-18 实测）：
 *   内核 DRM 初始化时会丢掉 u-boot 留下的开机图（dmesg: "skipping loader logo
 *   handoff"），屏幕从开机 +3s 一直黑到 +21s 前端起来，中间只有 2.9 秒的
 *   "Welcome to dArkOS" 文字。本程序在 systemd 早期接管 /dev/fb0，把 LANGOS
 *   底图铺上去并做轻量动画，把这段黑屏填掉，直到前端顶掉它。
 *
 * ★★ 2026-09-21 改造：分辨率自适应（浪总要求）
 *   原版把 5 个数写死了（FB_W=720 / FB_H=960 / FB_STRIDE=2880 / SRC_W=960 /
 *   DOT_Y=516），只认 MINILOONG(RK3566) 那块 720x960 竖装面板。
 *   移植到 RK3326（R36H ProMax 1024x768 横屏）时整屏全错。
 *
 *   现在改成**启动时问内核**：FBIOGET_VSCREENINFO + FBIOGET_FSCREENINFO 拿到
 *   真实的 xres / yres / line_length / bits_per_pixel / yres_virtual，
 *   连同底图（raw）的尺寸一起，推导出：
 *     · 源图坐标 (sx,sy) → fb 坐标 (u,v) 的映射（横屏直接 1:1，竖装侧屏才转 90°）
 *     · 进度点位置（按源图尺寸的**比例**算，不再写死像素）
 *     · 双缓冲要不要两屏都写（看 yres_virtual 是不是 yres 的两倍）
 *
 *   底图 raw 自带尺寸头（见 RAW_MAGIC），所以换机型只要重新出一份 raw，
 *   程序一行不用改。老格式 raw（无头，纯 720*960*4 像素）也能认 —— 向后兼容。
 *
 * 硬件事实（pc/req578 + req579 标定，MINILOONG）：
 *   · 面板物理 720x960，stride 2880，32bpp，双缓冲（yres_virtual = 1920）
 *   · 像素排列 = BGRA（小端 XRGB8888）
 *   · ★ 映射： fb(u,v) = 源(x = 959 - v, y = u)   —— 源图逆时针 90°
 *     底图 langos-boot.raw 已在 PC 上按此预旋转好，这里只 memcpy。
 *
 * 为什么要解绑 fbcon：开机过程 systemd / 内核会往 tty1 输出，fbcon 一重绘就把
 *   我们画的盖掉。echo 0 > /sys/class/vtconsole/vtcon1/bind 之后 fbcon 不再碰
 *   这块 fb，画面就稳了。退出时不恢复（恢复会立刻重绘成 console 文本，反而闪屏）；
 *   重启后 vtcon 默认 bind=1，自动还原。
 *
 * 编译： gcc -O2 -o /usr/local/bin/langos-bootanim langos-bootanim.c
 * 用法： langos-bootanim [保持秒数，默认 60]
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define RAW_PATH    "/usr/local/share/langos-boot.raw"
#define VTCON_BIND  "/sys/class/vtconsole/vtcon1/bind"

/* 底图文件头（新格式）：魔数 + 宽 + 高，各 4 字节小端，共 12 字节。
   没有这个头的老底图按"720x960 竖屏侧装"处理（向后兼容 MINILOONG）。 */
#define RAW_MAGIC   0x4C42474C      /* "LGBL" */
#define RAW_HDR     12
#define RAW_W_OLD   960             /* 老 raw 正立源图尺寸 */
#define RAW_H_OLD   720

/* ---- 运行时标定出来的屏幕参数（main 里填）------------------------------ */
static int FB_W, FB_H;              /* fb 可见区（列 x 行） */
static int FB_STRIDE;               /* 一行字节数 */
static int FB_YTOTAL;               /* mmap 覆盖的总行数（双缓冲时 = FB_H*2） */
static int FB_TWO_PAGES;            /* 1 = 双缓冲，两屏都要写 */

/* 1 = 本机 fb 不支持 mmap，改用「本地缓冲 + pwrite 整帧」这条路。
   判据见 main() 第 5 步的注释。 */
static int g_use_pwrite;

static int SRC_W, SRC_H;            /* 底图（正立）尺寸 */
static int SRC_TO_FB_90;            /* 1 = 源图要逆时针 90° 才落到 fb（竖装侧屏） */

/* 进度点：按源图尺寸的**比例**算（原来是 960x720 上写死 452/480/508 @516） */
static int DOT_Y, DOT_R;
static int DOT_X[3];

static void msleep(long ms)
{
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static void fbcon_bind(int on)
{
    int fd = open(VTCON_BIND, O_WRONLY);
    if (fd < 0)
        return;
    if (write(fd, on ? "1" : "0", 1) < 0) {
        /* 失败无所谓：最坏就是画面可能被 console 重绘覆盖 */
    }
    close(fd);
}

/*
 * 前端起来了吗？
 * ★ 为什么要自己查：最初想用 systemd 的 Conflicts=emulationstation.service 让
 *   前端顶掉动画，实测（2026-09-18）会把前端整条启动链"排斥"掉 —— 开机后
 *   前端压根不启动，只能手动 systemctl start。改成程序自己轮询 /proc 最稳。
 */
static int frontend_up(void)
{
    DIR *d;
    struct dirent *e;
    char path[320];
    char buf[64];
    int fd, n, found = 0;

    d = opendir("/proc");
    if (!d)
        return 0;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9')
            continue;
        snprintf(path, sizeof(path), "/proc/%s/comm", e->d_name);
        fd = open(path, O_RDONLY);
        if (fd < 0)
            continue;
        n = (int)read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0)
            continue;
        buf[n] = '\0';
        if (strncmp(buf, "vgfront", 7) == 0 ||
            strncmp(buf, "emulationstation", 16) == 0) {
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
}

/*
 * 在 fb 上画一个实心圆点。
 * 输入是「底图（正立）」坐标 (sx, sy)，按 SRC_TO_FB_90 换算到 fb 的 (u,v)：
 *
 *   竖装侧屏（MINILOONG 720x960，源图 960x720）：
 *       u = sy                （fb 的列 = 源的行）
 *       v = (SRC_W - 1) - sx  （fb 的行 = 源的列反向）
 *       即源图逆时针 90° —— 与 raw 的预旋转一致
 *
 *   横屏（R36H ProMax 1024x768，源图就是 1024x768）：
 *       u = sx,  v = sy       直接 1:1
 *
 * ★ 双缓冲（yres_virtual = yres*2）时两屏都写：可见区落在哪一屏由 panning
 *   决定，只写一屏会有一半概率看不到点（2026-09-18 实测踩到）。
 */
static void put_px(unsigned char *mem, int u, int v)
{
    unsigned char *p;
    /* ★ v 是 fb 的【行】索引，范围到 FB_YTOTAL，不是 FB_W */
    if (u < 0 || u >= FB_W || v < 0 || v >= FB_YTOTAL)
        return;
    p = mem + (size_t)v * (size_t)FB_STRIDE + (size_t)u * 4;
    p[0] = 0xD0;    /* B */
    p[1] = 0xE4;    /* G */
    p[2] = 0xFF;    /* R */
    p[3] = 0x00;    /* X */
}

static void draw_dot(unsigned char *mem, int sx, int sy, int r)
{
    int dy, dx;

    for (dy = -r; dy <= r; dy++) {
        int dd = r * r - dy * dy;           /* 圆形：|dx| <= sqrt(r^2-dy^2) */
        int half;

        if (dd < 0)
            continue;
        half = 0;                           /* 整数开方，免得链 libm */
        while ((half + 1) * (half + 1) <= dd)
            half++;

        for (dx = -half; dx <= half; dx++) {
            int x = sx + dx;
            if (x < 0 || x >= SRC_W)
                continue;
            if (SRC_TO_FB_90) {
                /* 竖装侧屏：fb(u,v) = 源(x = SRC_W-1-v, y = u)
                   → u = sy+dy,  v = (SRC_W-1) - x                     */
                int u = sy + dy;
                int v = (SRC_W - 1) - x;
                if (u < 0 || u >= FB_H)
                    continue;               /* 这一行 dy 整体越界，跳过 */
                put_px(mem, u, v);              /* 第一屏 */
                if (FB_TWO_PAGES)
                    put_px(mem, u, v + FB_H);   /* 第二屏 */
            } else {
                /* 横屏：源图正立，1:1 铺 */
                int u = x;
                int v = sy + dy;
                if (v < 0 || v >= FB_H)
                    continue;
                put_px(mem, u, v);              /* 第一屏 */
                if (FB_TWO_PAGES)
                    put_px(mem, u, v + FB_H);   /* 第二屏 */
            }
        }
    }
}

/* 问内核实屏参数。成功 0，失败 -1（并把原因写 stderr）。 */
static int probe_fb(int fbfd)
{
    struct fb_var_screeninfo vi;
    struct fb_fix_screeninfo fi;

    memset(&vi, 0, sizeof(vi));
    memset(&fi, 0, sizeof(fi));
    if (ioctl(fbfd, FBIOGET_VSCREENINFO, &vi) != 0) {
        fprintf(stderr, "langos-bootanim: FBIOGET_VSCREENINFO 失败: %s\n",
                strerror(errno));
        return -1;
    }
    if (ioctl(fbfd, FBIOGET_FSCREENINFO, &fi) != 0) {
        fprintf(stderr, "langos-bootanim: FBIOGET_FSCREENINFO 失败: %s\n",
                strerror(errno));
        return -1;
    }

    FB_W     = (int)vi.xres;
    FB_H     = (int)vi.yres;
    FB_STRIDE = (int)fi.line_length;
    /* yres_virtual 是"一整块"的总行数：等于 yres*2 就是双缓冲 */
    FB_YTOTAL = (int)(vi.yres_virtual ? vi.yres_virtual : vi.yres);
    FB_TWO_PAGES = (FB_YTOTAL >= FB_H * 2) ? 1 : 0;

    fprintf(stderr, "langos-bootanim: fb %dx%d stride=%d bpp=%d ytotal=%d%s\n",
            FB_W, FB_H, FB_STRIDE, (int)vi.bits_per_pixel, FB_YTOTAL,
            FB_TWO_PAGES ? "（双缓冲）" : "");

    if (FB_W <= 0 || FB_H <= 0 || FB_STRIDE <= 0) {
        fprintf(stderr, "langos-bootanim: 屏参数不合理，放弃\n");
        return -1;
    }
    /* 只支持 32bpp。16/24bpp 的机型没做过标定，宁可不画也别画错色。 */
    if (vi.bits_per_pixel != 32) {
        fprintf(stderr, "langos-bootanim: 只支持 32bpp，本机 %d bpp，跳过\n",
                (int)vi.bits_per_pixel);
        return -1;
    }
    return 0;
}

/* 读底图尺寸头。新格式有 12 字节头；老格式（无头）按 960x720 认。
   返回文件是否已 seek 到像素数据起点。 */
static int read_raw_header(int rf, int *sw, int *sh, int *hdr)
{
    unsigned char h[RAW_HDR];
    ssize_t n = read(rf, h, RAW_HDR);

    if (n == RAW_HDR) {
        unsigned int magic = (unsigned int)h[0]
                           | ((unsigned int)h[1] << 8)
                           | ((unsigned int)h[2] << 16)
                           | ((unsigned int)h[3] << 24);
        if (magic == RAW_MAGIC) {
            *sw = (int)((unsigned int)h[4] | ((unsigned int)h[5] << 8)
                      | ((unsigned int)h[6] << 16) | ((unsigned int)h[7] << 24));
            *sh = (int)((unsigned int)h[8] | ((unsigned int)h[9] << 8)
                      | ((unsigned int)h[10] << 16) | ((unsigned int)h[11] << 24));
            *hdr = RAW_HDR;
            if (*sw > 0 && *sh > 0)
                return 0;
            fprintf(stderr, "langos-bootanim: 底图头里的尺寸不合理 %dx%d\n", *sw, *sh);
            return -1;
        }
    }
    /* 不是新格式 —— 退回老约定：源图 960x720，无头 */
    *sw = RAW_W_OLD;
    *sh = RAW_H_OLD;
    *hdr = 0;
    if (lseek(rf, 0, SEEK_SET) < 0)
        return -1;
    fprintf(stderr, "langos-bootanim: 底图无尺寸头，按老的 %dx%d 处理\n", *sw, *sh);
    return 0;
}

int main(int argc, char **argv)
{
    long hold = 60;
    long steps, i;
    int w, fbfd, rf, hdr = 0;
    int d, lit;
    ssize_t got;
    size_t total, img_size;
    unsigned char *img, *mem;
    int st;

    if (argc > 1)
        hold = atol(argv[1]);
    if (hold <= 0 || hold > 600)
        hold = 60;

    /* 1) 等 /dev/fb0 —— 内核 DRM 大约 +4s 就绪，最多等 20 秒 */
    for (w = 0; w < 400; w++) {
        if (access("/dev/fb0", F_OK) == 0)
            break;
        msleep(50);
    }
    if (access("/dev/fb0", F_OK) != 0) {
        fprintf(stderr, "langos-bootanim: /dev/fb0 始终没出现\n");
        return 1;
    }

    /* 2) 打开 fb 并问内核真实参数（★ 自适应第一步） */
    fbfd = open("/dev/fb0", O_RDWR);
    if (fbfd < 0) {
        fprintf(stderr, "langos-bootanim: 打不开 /dev/fb0: %s\n", strerror(errno));
        return 5;
    }
    if (probe_fb(fbfd) != 0) {
        close(fbfd);
        return 5;
    }

    /* 3) 读底图尺寸头，再按"fb 有多大就铺多大"决定映射方向
     *    ★ 判据：底图和 fb 的**长边对短边**是否一致。
     *      · 一样 → 直接 1:1（横屏机器）
     *      · 底图是横的、fb 是竖的（且底图宽 == fb 高）→ 逆时针 90°
     *    两块 raw 的历史约定本来就是"源图正立"，所以这里用尺寸关系判最稳。 */
    rf = open(RAW_PATH, O_RDONLY);
    if (rf < 0) {
        fprintf(stderr, "langos-bootanim: 打不开 %s: %s\n", RAW_PATH, strerror(errno));
        close(fbfd);
        return 2;
    }
    if (read_raw_header(rf, &SRC_W, &SRC_H, &hdr) != 0) {
        close(rf);
        close(fbfd);
        return 2;
    }

    if (SRC_W == FB_W && SRC_H == FB_H) {
        SRC_TO_FB_90 = 0;
    } else if (SRC_W == FB_H && SRC_H == FB_W) {
        SRC_TO_FB_90 = 1;
    } else {
        /* 尺寸对不上：底图是给别的屏出的。别硬铺（会花屏），直接退出，
           让系统走原来的黑屏 —— 至少不会更糟。 */
        fprintf(stderr, "langos-bootanim: 底图 %dx%d 与屏幕 %dx%d 不匹配，跳过\n",
                SRC_W, SRC_H, FB_W, FB_H);
        close(rf);
        close(fbfd);
        return 4;
    }

    /* 进度点按**比例**放（老值 960x720 上是 X=452/480/508、Y=516、R=7）：
       X 取宽度的 .4708/.5/.5292，Y 取高度的 .7167，R 取宽度的 .0073。
       这样 1024x768 上也落在「请稍等」下方同样的相对位置。 */
    DOT_X[0] = (int)(SRC_W * 0.4708f);
    DOT_X[1] = (int)(SRC_W * 0.5000f);
    DOT_X[2] = (int)(SRC_W * 0.5292f);
    DOT_Y    = (int)(SRC_H * 0.7167f);
    DOT_R    = (int)(SRC_W * 0.0073f + 0.5f);
    if (DOT_R < 3) DOT_R = 3;

    /* 4) 读底图像素。layout 按 fb 走：竖装侧屏时源图要预先转过 90°，
       所以文件大小应当 = FB_H(列) x FB_W(行) x 4；横屏则 = SRC x 4。 */
    {
        struct stat sb;
        long file_px;
        if (fstat(rf, &sb) != 0) {
            close(rf); close(fbfd);
            return 3;
        }
        file_px = (long)(sb.st_size - hdr) / 4;
        if (file_px != (long)FB_W * (long)FB_H) {
            fprintf(stderr,
                    "langos-bootanim: 底图像素数 %ld 与屏幕 %dx%d 不符，跳过\n",
                    file_px, FB_W, FB_H);
            close(rf); close(fbfd);
            return 4;
        }
    }
    img_size = (size_t)FB_STRIDE * (size_t)FB_H;
    img = malloc(img_size);
    if (!img) {
        fprintf(stderr, "langos-bootanim: 内存不足\n");
        close(rf); close(fbfd);
        return 3;
    }
    memset(img, 0, img_size);
    got = 0;
    /* 若 stride > 宽*4（有行填充），按行搬；否则整块搬。 */
    if (FB_STRIDE == FB_W * 4) {
        got = 0;
        while (got < (ssize_t)img_size) {
            st = (int)read(rf, img + got, img_size - (size_t)got);
            if (st <= 0)
                break;
            got += st;
        }
    } else {
        int row;
        size_t rowbytes = (size_t)FB_W * 4;
        for (row = 0; row < FB_H; row++) {
            size_t o = (size_t)row * (size_t)FB_STRIDE;
            got = 0;
            while (got < (ssize_t)rowbytes) {
                st = (int)read(rf, img + o + got, rowbytes - (size_t)got);
                if (st <= 0)
                    break;
                got += st;
            }
            if (got != (ssize_t)rowbytes)
                break;
        }
    }
    close(rf);
    if (got != (ssize_t)(FB_STRIDE == FB_W * 4 ? img_size : (size_t)FB_W * 4)) {
        fprintf(stderr, "langos-bootanim: 底图读得不完整\n");
        free(img);
        close(fbfd);
        return 4;
    }

    /* 5) 拿到能写的帧缓冲。
     *
     *  ★★★ 2026-09-20 在 R36hProMax（RK3326 / Ubuntu 19.10）实测：
     *      mmap(/dev/fb0) 会直接 EINVAL，哪怕长度正好 4096x768、没别的
     *      进程占着（用 Python 的 mmap 单独验过，同样失败）。
     *      —— 是这台内核的 rockchip fb 驱动不支持 mmap，不是我们的用法错。
     *      所以**不能只会 mmap**：失败就退回「本地缓冲 + pwrite 整帧」。
     *      pwrite 实测 3145728 字节一次写完、rc=0，是能用的。
     *
     *  两种模式共存：能用 mmap 的机型（MiniLoong）走老路（动画平滑、
     *  不用每次整帧 IO）；不能 mmap 的走 pwrite（每帧一次 3 MB 写，
     *  400 ms 一次完全无压力）。两条路下面的绘制代码完全一样 ——
     *  draw_dot() 只管往一块内存里画。
     */
    total = (size_t)FB_STRIDE * (size_t)FB_YTOTAL;
    mem = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
    if (mem == MAP_FAILED) {
        /* 退路：自己开一块同样大的内存，每帧整块 pwrite 下去 */
        fprintf(stderr,
                "langos-bootanim: mmap 失败(%s)，改用 pwrite 整帧写（本机 fb 驱动不支持 mmap）\n",
                strerror(errno));
        mem = malloc(total);
        if (!mem) {
            fprintf(stderr, "langos-bootanim: 内存不足（pwrite 缓冲）\n");
            close(fbfd);
            free(img);
            return 6;
        }
        g_use_pwrite = 1;
    } else {
        g_use_pwrite = 0;
    }

    /* 6) 解绑 fbcon，免得被 console 重绘盖掉 */
    fbcon_bind(0);

    /* 7) 动画：每 400ms 重铺底图 + 点亮进度点，3 个点循环。
     *    每 5 步（2 秒）查一次前端是否已起来，起来就收工。 */
    steps = hold * 1000L / 400L;
    for (i = 0; i < steps; i++) {
        if ((i % 5) == 0 && frontend_up()) {
            fprintf(stderr, "langos-bootanim: 前端已起来，第 %ld 步退出\n", i);
            break;
        }
        memcpy(mem, img, img_size);
        if (FB_TWO_PAGES)
            memcpy(mem + img_size, img, img_size);   /* 第二屏也写上，防 panning 错页 */
        lit = (int)(i % 3) + 1;                /* 1,2,3,1,2,3 … */
        for (d = 0; d < 3; d++) {
            if (d < lit)
                draw_dot(mem, DOT_X[d], DOT_Y, DOT_R);
        }
        if (g_use_pwrite) {
            /* 整帧写回。pwrite 不移动文件偏移，也不受 O_APPEND 影响。 */
            size_t off = 0;
            while (off < total) {
                ssize_t w2 = pwrite(fbfd, mem + off, total - off, (off_t)off);
                if (w2 <= 0) {
                    fprintf(stderr, "langos-bootanim: pwrite 失败: %s\n", strerror(errno));
                    break;
                }
                off += (size_t)w2;
            }
        }
        msleep(400);
    }

    if (g_use_pwrite)
        free(mem);
    else
        munmap(mem, total);
    close(fbfd);
    free(img);
    return 0;
}
