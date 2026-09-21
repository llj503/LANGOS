/* ============================================================================
 *  sdl2_compat.h —— 给「运行时 SDL2 较新、但开发头文件较旧」的设备补声明
 *
 *  ★ 为什么需要它（2026-09-21 实测，R36H ProMax / RK3326 / Ubuntu 19.10）：
 *    这台机器上：
 *      · 运行时库 /lib/aarch64-linux-gnu/libSDL2-2.0.so.0.3200.10 = **SDL2 2.0.32**
 *        （nm -D 确认有 SDL_RenderGeometry / SDL_RenderGeometryRaw 符号）
 *      · 但开发头文件是 Ubuntu 19.10 自带的 **2.0.10**（sdl2-config --version = 2.0.10）
 *        —— 2.0.10 里**没有** SDL_Vertex / SDL_RenderGeometry（2.0.18 才加的）
 *    于是编译直接报：
 *        error: unknown type name 'SDL_Vertex'
 *        warning: implicit declaration of function 'SDL_RenderGeometry'
 *    而外网不通，没法 apt 装新版 dev 包。
 *
 *  ★ 解法：只补「头里缺、库里确实有」的那几个声明。
 *    struct 布局照抄 SDL2 官方（本机 2.26.5 的 SDL_render.h 逐字核对），
 *    函数签名照抄官方原型 —— ABI 与运行时库一致，所以链接、运行都对。
 *
 *  ⚠ 只在需要时补：用 SDL_VERSION_ATLEAST 判断，新版头环境下这个文件是**空操作**，
 *    所以同一份源码在迷你龙（SDL2 较新）上照样编，不用改代码。
 *
 *  用法（编译命令里加一行即可，不用改源码）：
 *      gcc ... -include /opt/vagaretro/native/sdl2_compat.h ...
 *  或写成 .c 文件里已有 #include 的话，也可以加到 VG_CFLAGS 里。
 * ==========================================================================*/
#ifndef VG_SDL2_COMPAT_H
#define VG_SDL2_COMPAT_H

#include <SDL2/SDL.h>

/* ---- SDL_Vertex / SDL_RenderGeometry（SDL 2.0.18 引入）--------------------
 * 判据用 SDL_VERSION_ATLEAST(2,0,18)：头文件够新就整段跳过。 */
#if !SDL_VERSION_ATLEAST(2, 0, 18)

#ifndef SDL_RenderGeometry
/* 结构体布局与官方完全一致（三字段，无 padding 变化）：
 *     SDL_FPoint position;  SDL_Color color;  SDL_FPoint tex_coord;   */
typedef struct SDL_Vertex
{
    SDL_FPoint position;        /**< Vertex position, in SDL_Renderer coordinates */
    SDL_Color  color;           /**< Vertex color */
    SDL_FPoint tex_coord;       /**< Normalized texture coordinates, if needed */
} SDL_Vertex;

extern DECLSPEC int SDLCALL SDL_RenderGeometry(SDL_Renderer *renderer,
                                               SDL_Texture *texture,
                                               const SDL_Vertex *vertices, int num_vertices,
                                               const int *indices, int num_indices);

extern DECLSPEC int SDLCALL SDL_RenderGeometryRaw(SDL_Renderer *renderer,
                                                  SDL_Texture *texture,
                                                  const float *xy, int xy_stride,
                                                  const SDL_Color *color, int color_stride,
                                                  const float *uv, int uv_stride,
                                                  int num_vertices,
                                                  const void *indices, int num_indices, int size_indices);
#endif /* SDL_RenderGeometry */

#endif /* !SDL_VERSION_ATLEAST(2,0,18) */

#endif /* VG_SDL2_COMPAT_H */
