/* ============================================================================
 * vgboot.c —— LANGOS「首次开机代码屏」显示后端（自绘版）
 *
 * 背景
 * ====
 * firstboot.sh -> expandtoexfat.sh 把输出硬指向 /dev/tty1（帧缓冲控制台 = 黑底
 * 等宽白字）。plymouth 一让出显示，这屏代码就糊上来顶替 LANGOS 画面，还要持续
 * 好几分钟（扩容 + 解 roms.tar），观感跟"黑屏"没区别。
 *
 * R36H/A10 那两份固件上，这件事是靠 `ffmpeg drawtext` 实时合成的（vgfb.py）。
 * 到 MINILOONG（RK3566 / Debian trixie）走不通：这台 ffmpeg 是 rockchip 自己编的，
 * 配置里 `--disable-autodetect`，**没有 libfreetype，压根没有 drawtext 滤镜**。
 * 所以这里改成自绘：直接用 libfreetype 把字画到裸缓冲上，再贴 /dev/fb0。
 *
 * 好处不只是"能跑"：不再每帧 spawn 一个 ffmpeg 进程（原来一帧 0.3~0.7s），
 * 现在是纯内存位运算 + 一次写帧缓冲，一帧几毫秒，节流可以做到 0.2s。
 *
 * 用法
 * ====
 *     <产生输出的命令> 2>&1 | vgboot [选项]
 *
 *     --fb PATH     帧缓冲设备，默认 /dev/fb0（传普通文件 = 渲染到文件，配合 --size 调试）
 *     --bg PATH     底图（.raw，自带 16 字节头），默认 /boot/bootbg.raw
 *     --tee PATH    同时把这路文本追加进日志文件
 *     --interval S  重画节流，默认 0.5
 *     --hold S      stdin 结束后再多留一会儿（期间每 2s 重贴一次，防 fbcon 盖掉）
 *     --rot R       旋转：auto|0|90|180|270，默认 auto（竖屏 fb + 横屏底图 = 自动转）
 *     --size WxH    调试用：没有真实 fb 时手工指定几何
 *     --bpp N       调试用：位深，默认 32
 *     --stride N    调试用：行跨距
 *     --no-fb       强制退回"原文直写 /dev/tty1"
 *
 * 保底
 * ====
 * 任何一步失败（没 fb / 没字体 / 没底图 / 贴屏失败）都退回把原文写 /dev/tty1，
 * 也就是原来的行为 —— 宁可比原来丑，不能比原来少一行字。
 * ==========================================================================*/

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <linux/fb.h>
#include <poll.h>

#include <ft2build.h>
#include FT_FREETYPE_H

/* ------------------------------------------------------------------ 配置 */
#define FB_DEFAULT "/dev/fb0"
#define BG_DEFAULT "/boot/bootbg.raw"
#define TTY_PATH "/dev/tty1"
#define INI_PATH "/boot/vgboot.ini"

/* 版式比例 —— 跟 build/bootbg_lib.py 烘底图时用的是同一套 */
#define CODE_TOP 0.155
#define CODE_BOT 0.955
#define MARGIN_X 0.030

#define CODE_R 0xc2
#define CODE_G 0xcf
#define CODE_B 0xe8

#define MAX_LINE 4096
#define MAX_LINES 4000

static const char *FONT_MONO[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    NULL,
};
/* 等宽字体只有拉丁字形：tar -xvf 吐出来的中文 ROM 名会全变方块。
   非 ASCII 的字符逐字改用这颗 CJK 字体（逐字回退，不是整帧换字体）。 */
static const char *FONT_CJK[] = {
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
    "/usr/share/fonts/truetype/arphic/uming.ttc",
    NULL,
};

static int g_debug = 0;

static void logmsg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("[vgboot] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
    va_end(ap);
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ------------------------------------------------------------------ 图像 */
typedef struct {
    uint32_t w, h;
    unsigned char *px;          /* BGRA，紧密排列 */
} Image;

/* ------------------------------------------------------------------ 帧缓冲 */
typedef struct {
    int fd;
    int is_dev;
    unsigned char *mem;
    size_t maplen;
    uint32_t w, h;              /* 物理分辨率 */
    uint32_t stride;
    uint32_t bpp;
    uint32_t r_off, g_off, b_off;   /* 位域偏移（按 fb 的 varinfo） */
    /* 可见方向（= 绕物理 fb 转 rot 度后的那个方向） */
    uint32_t vw, vh;
    int rot;                    /* 0/90/180/270：物理 fb 相对可见方向转了多少 */
} Fb;

static int fb_open(Fb *fb, const char *path, int force_w, int force_h,
                   int force_bpp, int force_stride, int rot)
{
    memset(fb, 0, sizeof(*fb));
    fb->fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fb->fd < 0) {
        logmsg("打不开 %s：%s", path, strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(fb->fd, &st) < 0) return -1;
    fb->is_dev = S_ISCHR(st.st_mode);

    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    if (fb->is_dev) {
        if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &var) < 0 ||
            ioctl(fb->fd, FBIOGET_FSCREENINFO, &fix) < 0) {
            logmsg("FBIOGET_* 失败：%s", strerror(errno));
            return -1;
        }
        fb->w = var.xres;
        fb->h = var.yres;
        fb->bpp = var.bits_per_pixel;
        fb->stride = fix.line_length;
        fb->r_off = var.red.offset;
        fb->g_off = var.green.offset;
        fb->b_off = var.blue.offset;
        fb->maplen = fix.smem_len;
    } else {
        /* 调试：渲染到普通文件 */
        if (force_w <= 0 || force_h <= 0) {
            logmsg("%s 不是字符设备，必须给 --size WxH", path);
            return -1;
        }
        fb->w = force_w;
        fb->h = force_h;
        fb->bpp = force_bpp > 0 ? (uint32_t)force_bpp : 32;
        fb->stride = force_stride > 0 ? (uint32_t)force_stride
                                      : fb->w * (fb->bpp / 8);
        fb->r_off = 16;
        fb->g_off = 8;
        fb->b_off = 0;
        fb->maplen = (size_t)fb->stride * fb->h;
    }

    if (fb->stride < fb->w * (fb->bpp / 8)) {
        logmsg("stride %u < 行字节 %u，放弃", fb->stride, fb->w * (fb->bpp / 8));
        return -1;
    }

    if (fb->is_dev) {
        fb->mem = mmap(NULL, fb->maplen, PROT_READ | PROT_WRITE, MAP_SHARED, fb->fd, 0);
        if (fb->mem == MAP_FAILED) {
            logmsg("mmap 失败（改用 pwrite）：%s", strerror(errno));
            fb->mem = NULL;
        }
    } else {
        if (ftruncate(fb->fd, fb->maplen) != 0) {
            logmsg("ftruncate 失败：%s", strerror(errno));
            return -1;
        }
        fb->mem = mmap(NULL, fb->maplen, PROT_READ | PROT_WRITE, MAP_SHARED, fb->fd, 0);
        if (fb->mem == MAP_FAILED) fb->mem = NULL;
    }

    /* 旋转：竖屏 fb + 横屏底图时自动选一个。转 90 还是 270 物理上定不了，
       默认 270（fbcon 的 FB_ROTATE_CCW=3 对应的补偿量），错的用 vgboot.ini 翻。 */
    if (rot < 0) {
        rot = 0;
        if (fb->w < fb->h) rot = 270;
    }
    fb->rot = rot;
    switch (rot) {
    case 0:   fb->vw = fb->w; fb->vh = fb->h; break;
    case 180: fb->vw = fb->w; fb->vh = fb->h; break;
    default:  fb->vw = fb->h; fb->vh = fb->w; break;
    }
    return 0;
}

/* 把可见方向的 BGRA 缓冲写进物理帧缓冲。
   ⚠ /dev/fb0 拿到的是**原始**帧缓冲：fbcon=rotate:N 只管内核控制台怎么画，
   不会替我们转。所以这里得自己把画面转回去，否则屏幕上的字是躺着的。 */
static int fb_present(Fb *fb, const unsigned char *vis)
{
    uint32_t px = fb->bpp / 8;
    unsigned char *row = malloc(fb->stride);
    if (!row) return -1;

    for (uint32_t y = 0; y < fb->h; y++) {
        memset(row, 0, fb->stride);
        for (uint32_t x = 0; x < fb->w; x++) {
            /* 物理 (x,y) <- 可见 (vx,vy) */
            uint32_t vx, vy;
            switch (fb->rot) {
            case 0:   vx = x;               vy = y;                break;
            case 180: vx = fb->w - 1 - x;   vy = fb->h - 1 - y;    break;
            case 90:  vx = y;               vy = fb->w - 1 - x;    break;
            default:  vx = fb->h - 1 - y;   vy = x;                break;  /* 270 */
            }
            const unsigned char *s = vis + ((size_t)vy * fb->vw + vx) * 4;
            unsigned char b = s[0], g = s[1], r = s[2];
            if (px == 4) {
                uint32_t v = ((uint32_t)(r >> (8 - 8)) << fb->r_off) |
                             ((uint32_t)g << fb->g_off) |
                             ((uint32_t)b << fb->b_off);
                memcpy(row + (size_t)x * 4, &v, 4);
            } else if (px == 3) {
                row[x * 3 + 0] = b;
                row[x * 3 + 1] = g;
                row[x * 3 + 2] = r;
            } else {
                uint16_t v = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
                memcpy(row + (size_t)x * 2, &v, 2);
            }
        }
        if (fb->mem) {
            memcpy(fb->mem + (size_t)y * fb->stride, row, fb->stride);
        } else {
            if (pwrite(fb->fd, row, fb->stride, (off_t)y * fb->stride) < 0) {
                free(row);
                return -1;
            }
        }
    }
    free(row);
    return 0;
}

/* ------------------------------------------------------------------ 底图 */
static Image *image_load_raw(const char *path)
{
    FILE *fh = fopen(path, "rb");
    if (!fh) {
        logmsg("打不开底图 %s：%s", path, strerror(errno));
        return NULL;
    }
    unsigned char hdr[16];
    if (fread(hdr, 1, 16, fh) != 16 || memcmp(hdr, "VGBG", 4) != 0) {
        logmsg("底图头不对（要 'VGBG'）");
        fclose(fh);
        return NULL;
    }
    uint32_t w, h, fmt;
    memcpy(&w, hdr + 4, 4);
    memcpy(&h, hdr + 8, 4);
    memcpy(&fmt, hdr + 12, 4);
    if (fmt != 1 || w == 0 || h == 0 || w > 8192 || h > 8192) {
        logmsg("底图参数不对：%ux%u fmt=%u", w, h, fmt);
        fclose(fh);
        return NULL;
    }
    Image *im = calloc(1, sizeof(*im));
    im->w = w;
    im->h = h;
    im->px = malloc((size_t)w * h * 4);
    size_t got = fread(im->px, 1, (size_t)w * h * 4, fh);
    fclose(fh);
    if (got != (size_t)w * h * 4) {
        logmsg("底图数据不足：%zu / %zu", got, (size_t)w * h * 4);
        free(im->px);
        free(im);
        return NULL;
    }
    return im;
}

/* 尺寸对不上就最近邻缩一下，别为了尺寸不匹配整屏退回去 */
static void image_scale_to(Image *im, uint32_t w, uint32_t h)
{
    if (im->w == w && im->h == h) return;
    unsigned char *np = malloc((size_t)w * h * 4);
    if (!np) return;
    for (uint32_t y = 0; y < h; y++) {
        uint32_t sy = (uint32_t)((uint64_t)y * im->h / h);
        for (uint32_t x = 0; x < w; x++) {
            uint32_t sx = (uint32_t)((uint64_t)x * im->w / w);
            memcpy(np + ((size_t)y * w + x) * 4, im->px + ((size_t)sy * im->w + sx) * 4, 4);
        }
    }
    free(im->px);
    im->px = np;
    im->w = w;
    im->h = h;
}

/* ------------------------------------------------------------------ 字体 */
typedef struct {
    FT_Library lib;
    FT_Face mono;
    FT_Face cjk;
    int px;
    int ascent;
} Fonts;

static FT_Face face_open(FT_Library lib, const char *const *cands)
{
    for (int i = 0; cands[i]; i++) {
        FT_Face f = NULL;
        if (access(cands[i], R_OK) != 0) continue;
        if (FT_New_Face(lib, cands[i], 0, &f) == 0) return f;
    }
    return NULL;
}

static int fonts_load(Fonts *fo, int px)
{
    memset(fo, 0, sizeof(*fo));
    fo->px = px;
    if (FT_Init_FreeType(&fo->lib) != 0) {
        logmsg("FT_Init_FreeType 失败");
        return -1;
    }
    fo->mono = face_open(fo->lib, FONT_MONO);
    if (!fo->mono) {
        logmsg("找不到等宽字体");
        return -1;
    }
    fo->cjk = face_open(fo->lib, FONT_CJK);      /* 没有也能跑，只是中文变方块 */
    FT_Set_Pixel_Sizes(fo->mono, 0, px);
    if (fo->cjk) FT_Set_Pixel_Sizes(fo->cjk, 0, px);
    fo->ascent = fo->mono->size->metrics.ascender >> 6;
    return 0;
}

/* 往 BGRA 缓冲里混合一个字形位图 */
static inline void blend_px(unsigned char *d, int a, unsigned char r,
                            unsigned char g, unsigned char b)
{
    if (a <= 0) return;
    if (a >= 255) {
        d[0] = b; d[1] = g; d[2] = r;
        return;
    }
    d[0] = (unsigned char)((d[0] * (255 - a) + b * a) / 255);
    d[1] = (unsigned char)((d[1] * (255 - a) + g * a) / 255);
    d[2] = (unsigned char)((d[2] * (255 - a) + r * a) / 255);
}

static void glyph_blit(unsigned char *buf, uint32_t W, uint32_t H,
                       FT_Bitmap *bm, int bx, int by,
                       unsigned char r, unsigned char g, unsigned char b, int alpha)
{
    for (unsigned row = 0; row < bm->rows; row++) {
        int y = by + (int)row;
        if (y < 0 || y >= (int)H) continue;
        for (unsigned col = 0; col < bm->width; col++) {
            int x = bx + (int)col;
            if (x < 0 || x >= (int)W) continue;
            unsigned cov = bm->buffer[row * bm->pitch + col];
            if (!cov) continue;
            blend_px(buf + ((size_t)y * W + x) * 4, cov * alpha / 255, r, g, b);
        }
    }
}

static void draw_utf8(const Fonts *fo, unsigned char *buf, uint32_t W, uint32_t H,
                      const char *s, int x, int baseline, int alpha)
{
    const unsigned char *p = (const unsigned char *)s;
    int pen = x;
    while (*p) {
        uint32_t cp = *p++;
        if (cp < 0x80) {
            /* ascii，原样 */
        } else if ((cp & 0xE0) == 0xC0 && p[0]) {
            cp = ((cp & 0x1F) << 6) | (p[0] & 0x3F);
            p += 1;
        } else if ((cp & 0xF0) == 0xE0 && p[0] && p[1]) {
            cp = ((cp & 0x0F) << 12) | ((p[0] & 0x3F) << 6) | (p[1] & 0x3F);
            p += 2;
        } else if ((cp & 0xF8) == 0xF0 && p[0] && p[1] && p[2]) {
            cp = ((cp & 0x07) << 18) | ((p[0] & 0x3F) << 12) |
                 ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            p += 3;
        } else {
            cp = '?';
        }
        if (cp == '\t') {
            pen += fo->px * 4;
            continue;
        }
        if (cp < 0x20) continue;

        FT_Face face = (cp > 126 && fo->cjk) ? fo->cjk : fo->mono;
        if (FT_Load_Char(face, cp, FT_LOAD_RENDER) != 0) {
            if (face != fo->mono && FT_Load_Char(fo->mono, '?', FT_LOAD_RENDER) == 0)
                face = fo->mono;
            else
                continue;
        }
        FT_GlyphSlot slot = face->glyph;
        /* 阴影：黑底上基本看不见，但压在亮一点的柔光上能把字托出来 */
        if (alpha == 255)
            glyph_blit(buf, W, H, &slot->bitmap,
                       pen + slot->bitmap_left, baseline - slot->bitmap_top + 1,
                       0, 0, 0, 140);
        glyph_blit(buf, W, H, &slot->bitmap,
                   pen + slot->bitmap_left, baseline - slot->bitmap_top,
                   CODE_R, CODE_G, CODE_B, alpha);
        pen += slot->advance.x >> 6;
    }
}

/* ------------------------------------------------------------------ 文本清洗 */
/* 去掉 ANSI / 控制序列：dialog、setfont、进度条都会往里塞。
   返回 0 = 这行没内容（调用方可以直接丢） */
static size_t ansi_strip(const char *in, size_t n, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < cap; ) {
        unsigned char c = (unsigned char)in[i];
        if (c == 0x1b) {
            if (i + 1 < n && in[i + 1] == ']') {          /* OSC：到 BEL 或 ESC \ */
                i += 2;
                while (i < n && in[i] != '\a' &&
                       !(in[i] == 0x1b && i + 1 < n && in[i + 1] == '\\')) i++;
                if (i < n && in[i] == 0x1b) i++;
                if (i < n) i++;
                continue;
            }
            if (i + 1 < n && in[i + 1] == '[') {          /* CSI：到 @..~ */
                i += 2;
                while (i < n && !(in[i] >= 0x40 && in[i] <= 0x7e)) i++;
                if (i < n) i++;
                continue;
            }
            if (i + 1 < n) {                              /* 双字符 escape（含 ESC c）*/
                i += 2;
                continue;
            }
            i++;
            continue;
        }
        if (c < 0x20 || c == 0x7f) {                       /* 其余控制字符丢掉 */
            i++;
            continue;
        }
        out[o++] = (char)c;
        i++;
    }
    while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == '\t')) o--;
    out[o] = 0;
    return o;
}

/* ------------------------------------------------------------------ 行缓冲 */
typedef struct {
    char **line;
    int cap;
    int count;          /* 一共收到多少行（含已滚掉的） */
    int head;           /* 环形里最老那行所在的下标 */
} Ring;

static Ring *ring_new(int cap)
{
    if (cap < 1) cap = 1;
    Ring *r = calloc(1, sizeof(*r));
    r->cap = cap;
    r->line = calloc(cap, sizeof(char *));
    return r;
}

static void ring_push(Ring *r, const char *s)
{
    if (r->count < r->cap) {
        r->line[r->count] = strdup(s);
    } else {
        free(r->line[r->head]);
        r->line[r->head] = strdup(s);
        r->head = (r->head + 1) % r->cap;
    }
    r->count++;
}

/* i = 0 是最老的那行可见行 */
static const char *ring_get(const Ring *r, int i)
{
    return r->line[(r->head + i) % r->cap];
}

static int ring_n(const Ring *r) { return r->count < r->cap ? r->count : r->cap; }

/* ------------------------------------------------------------------ 渲染 */
typedef struct {
    Fb *fb;
    Image *bg;
    Fonts *fo;
    unsigned char *vis;     /* 可见方向合成缓冲（BGRA） */
    char *txt;              /* 纯文本快照，调试用 */
    int margin, top, lh, cap;
} Painter;

static int painter_init(Painter *pt, Fb *fb, Image *bg, Fonts *fo)
{
    memset(pt, 0, sizeof(*pt));
    pt->fb = fb;
    pt->bg = bg;
    pt->fo = fo;
    int fs = fo->px;
    pt->lh = (int)(fs * 1.30f + 0.5f);
    pt->margin = (int)(fb->vw * MARGIN_X);
    pt->top = (int)(fb->vh * CODE_TOP);
    pt->cap = ((int)(fb->vh * CODE_BOT) - pt->top) / pt->lh;
    if (pt->cap < 1) return -1;
    pt->vis = malloc((size_t)fb->vw * fb->vh * 4);
    if (!pt->vis) return -1;
    return 0;
}

static void painter_render(Painter *pt, const Ring *r)
{
    int n = ring_n(r);
    if (n > pt->cap) n = pt->cap;

    memcpy(pt->vis, pt->bg->px, (size_t)pt->fb->vw * pt->fb->vh * 4);
    for (int i = 0; i < n; i++) {
        int baseline = pt->top + i * pt->lh + pt->fo->ascent;
        draw_utf8(pt->fo, pt->vis, pt->fb->vw, pt->fb->vh,
                  ring_get(r, i), pt->margin, baseline, 255);
    }
}

/* ------------------------------------------------------------------ ini */
/* /boot/vgboot.ini —— 放在 vfat 上，插到 Windows 就能改。
   刷完机发现画面躺了/正反了，不用重新烘焙镜像。 */
static void ini_read(const char *path, int *rot, double *interval, int *fs)
{
    FILE *fh = fopen(path, "r");
    if (!fh) return;
    char line[256];
    while (fgets(line, sizeof(line), fh)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char *v = eq + 1;
        while (*v == ' ' || *v == '\t') v++;
        char *e = v + strlen(v);
        while (e > v && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
        if (!strcmp(p, "rot")) {
            if (!strcmp(v, "auto")) *rot = -1;
            else *rot = atoi(v);
        } else if (!strcmp(p, "interval")) {
            double d = atof(v);
            if (d > 0) *interval = d;
        } else if (!strcmp(p, "fontsize")) {
            int s = atoi(v);
            if (s >= 6 && s <= 64) *fs = s;
        }
    }
    fclose(fh);
}

/* ------------------------------------------------------------------ main */
int main(int argc, char **argv)
{
    const char *fb_path = FB_DEFAULT, *bg_path = BG_DEFAULT, *tee_path = NULL;
    const char *tty_path = TTY_PATH;
    double interval = 0.5, hold = 0.0;
    int rot = -1, sw = 0, sh = 0, sbpp = 0, sstride = 0, nofb = 0, force_fs = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (!strcmp(a, "--fb") && v) { fb_path = v; i++; }
        else if (!strcmp(a, "--bg") && v) { bg_path = v; i++; }
        else if (!strcmp(a, "--tee") && v) { tee_path = v; i++; }
        else if (!strcmp(a, "--tty") && v) { tty_path = v; i++; }
        else if (!strcmp(a, "--interval") && v) { interval = atof(v); i++; }
        else if (!strcmp(a, "--hold") && v) { hold = atof(v); i++; }
        else if (!strcmp(a, "--rot") && v) { rot = strcmp(v, "auto") ? atoi(v) : -1; i++; }
        else if (!strcmp(a, "--size") && v) { sscanf(v, "%dx%d", &sw, &sh); i++; }
        else if (!strcmp(a, "--bpp") && v) { sbpp = atoi(v); i++; }
        else if (!strcmp(a, "--stride") && v) { sstride = atoi(v); i++; }
        else if (!strcmp(a, "--fontsize") && v) { force_fs = atoi(v); i++; }
        else if (!strcmp(a, "--no-fb")) { nofb = 1; }
        else if (!strcmp(a, "--debug")) { g_debug = 1; }
    }
    if (getenv("VGBOOT_DEBUG")) g_debug = 1;

    ini_read(INI_PATH, &rot, &interval, &force_fs);

    /* 兜底通道：老行为，原文写 /dev/tty1 */
    FILE *tty = fopen(tty_path, "w");
    int fallback = nofb;

    Fb fb;
    Image *bg = NULL;
    Fonts fo;
    Painter pt;
    int painter_ok = 0;

    if (!fallback) {
        if (fb_open(&fb, fb_path, sw, sh, sbpp, sstride, rot) != 0) fallback = 1;
    }
    if (!fallback) {
        bg = image_load_raw(bg_path);
        if (!bg) fallback = 1;
    }
    if (!fallback) {
        int fs = force_fs > 0 ? force_fs
                              : (int)(fb.vh / 56.0 + 0.5);
        if (fs < 8) fs = 8;
        if (fonts_load(&fo, fs) != 0) fallback = 1;
    }
    if (!fallback) {
        image_scale_to(bg, fb.vw, fb.vh);
        if (painter_init(&pt, &fb, bg, &fo) == 0) {
            painter_ok = 1;
            logmsg("贴屏 %ux%u rot=%d → 可见 %ux%u %ubpp stride=%u 字号=%d 行容量=%d%s",
                   fb.w, fb.h, fb.rot, fb.vw, fb.vh, fb.bpp, fb.stride, fo.px, pt.cap,
                   fo.cjk ? "" : "（无 CJK 字体，中文会变方块）");
        } else {
            fallback = 1;
        }
    }
    if (fallback) {
        logmsg("退回 tty1 模式（%s）", nofb ? "--no-fb" : "贴屏条件不满足");
        if (!tty) {
            /* 连 tty1 也没有（PC 上跑）→ 至少别把字丢了，转给 stdout */
            tty = stdout;
        }
    } else {
        /* 让 plymouth 让出显示 + 藏掉控制台光标 */
        if (system("pidof plymouthd >/dev/null 2>&1") == 0) {
            if (system("plymouth --quit >/dev/null 2>&1") == 0) {
                logmsg("已让 plymouth 退场");
                usleep(600000);
            }
        }
        FILE *t1 = fopen(tty_path, "w");
        if (t1) {
            fputs("\033[?25l", t1);
            fclose(t1);
        }
    }

    FILE *tee = NULL;
    if (tee_path) {
        tee = fopen(tee_path, "a");
        if (!tee) logmsg("打不开 --tee %s：%s", tee_path, strerror(errno));
    }

    Ring *ring = ring_new(painter_ok ? pt.cap : 64);
    int dead = !painter_ok;
    double last = 0.0;
    int dirty = 0;
    int have_frame = 0;
    int frames = 0;

    char chunk[8192];
    char pending[MAX_LINE];
    size_t pend = 0;
    ssize_t got;

    for (;;) {
        /* 空转补贴：这屏大部分时间没有新输出（比如 tar 解包那几分钟），
           而 plymouth 让位 / 内核打印都会让 fbcon 整屏重绘把我们盖掉 ——
           盖掉之后如果一直没新行，屏幕就黑在那儿了。所以每条 1.5s 主动重贴一次。 */
        if (painter_ok && have_frame) {
            struct pollfd pfd;
            pfd.fd = STDIN_FILENO;
            pfd.events = POLLIN;
            pfd.revents = 0;
            int pr = poll(&pfd, 1, 1500);
            if (pr == 0) {
                fb_present(&fb, pt.vis);
                continue;
            }
            if (pr < 0) {
                if (errno == EINTR) continue;
                break;
            }
        }
        got = read(STDIN_FILENO, chunk, sizeof(chunk));
        if (got <= 0) break;

        for (ssize_t i = 0; i < got; i++) {
            char c = chunk[i];
            if (c == '\n' || c == '\r') {
                char clean[MAX_LINE];
                pending[pend] = 0;
                ansi_strip(pending, pend, clean, sizeof(clean));
                pend = 0;
                if (tee) { fprintf(tee, "%s\n", clean); fflush(tee); }
                ring_push(ring, clean);
                if (dead || !painter_ok) {
                    if (tty) { fprintf(tty, "%s\n", clean); fflush(tty); }
                    continue;
                }
                dirty = 1;
                double t = now_s();
                if (t - last >= interval) {
                    last = t;
                    dirty = 0;
                    painter_render(&pt, ring);
                    if (fb_present(&fb, pt.vis) == 0) have_frame = 1;
                    frames++;
                    if (g_debug) logmsg("第 %d 行 → 第 %d 帧", ring->count, frames);
                }
            } else if (pend + 1 < sizeof(pending)) {
                pending[pend++] = c;
            }
        }
    }
    if (pend) {
        char clean[MAX_LINE];
        pending[pend] = 0;
        ansi_strip(pending, pend, clean, sizeof(clean));
        if (tee) { fprintf(tee, "%s\n", clean); fflush(tee); }
        ring_push(ring, clean);
        if (dead || !painter_ok) {
            if (tty) { fprintf(tty, "%s\n", clean); fflush(tty); }
        } else {
            dirty = 1;
        }
    }

    if (painter_ok && (dirty || !have_frame)) {
        painter_render(&pt, ring);
        if (fb_present(&fb, pt.vis) == 0) have_frame = 1;
        frames++;
    }
    /* 收尾：fbcon 在 VT 重新激活/被写入时会整屏重绘，把我们的图盖掉。
       结束后仍按 --hold 重贴一段时间，是最便宜的保险。 */
    if (painter_ok && have_frame) {
        usleep(300000);
        fb_present(&fb, pt.vis);
        double end = now_s() + (hold > 0 ? hold : 0);
        while (now_s() < end) {
            double left = end - now_s();
            usleep((useconds_t)(left > 2.0 ? 2.0 : left) * 1000000);
            if (now_s() >= end) break;
            fb_present(&fb, pt.vis);
        }
    }
    if (tee) fclose(tee);
    logmsg("结束，共 %d 行%s", ring->count,
           painter_ok ? "" : "（走的是 tty1 老路）");
    return 0;
}
