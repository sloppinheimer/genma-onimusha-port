// Linux: reading and writing pictures, which WIC does on Windows (texpack.c): PNG and JPEG in, PNG out, with stb_image
// (public domain / MIT, src/linux/stb). Pixels are BGRA, as the host keeps them.
#include <windows.h>
#include <stdlib.h>
// stb_image uses its SSE2 IDCT and colour conversion (linux.cmake builds with -msse2): a 4096x2048 HD background loads
// in about 80 ms on a Steam Deck instead of 160, the same pixels.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

uint32_t *compat_image_load(const wchar_t *path, uint32_t *w, uint32_t *h)
{
    char p[4096];
    int W, H, n;
    if (!compat_path(path, p, sizeof p)) return 0;
    uint32_t *px = (uint32_t *)stbi_load(p, &W, &H, &n, 4);   // malloc'd (stb's default), as the caller frees it
    if (!px) return 0;
    if (W <= 0 || H <= 0 || W > 16384 || H > 16384) { stbi_image_free(px); return 0; }
    // RGBA to BGRA where it is
    for (size_t i = 0; i < (size_t)W * H; i++) px[i] = (px[i] & 0xFF00FF00) | (px[i] >> 16 & 0xFF) | (px[i] & 0xFF) << 16;
    *w = (uint32_t)W; *h = (uint32_t)H;
    return px;
}
int compat_png_write(const wchar_t *path, const void *bgra, uint32_t w, uint32_t h, uint32_t pitch)
{
    char p[4096];
    if (!compat_path(path, p, sizeof p)) return 0;
    uint8_t *rgba = malloc((size_t)w * h * 4);
    if (!rgba) return 0;
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *s = (const uint8_t *)bgra + (size_t)y * pitch;
        uint8_t *d = rgba + (size_t)y * w * 4;
        for (uint32_t x = 0; x < w; x++) { d[4 * x] = s[4 * x + 2]; d[4 * x + 1] = s[4 * x + 1]; d[4 * x + 2] = s[4 * x]; d[4 * x + 3] = s[4 * x + 3]; }
    }
    int ok = stbi_write_png(p, (int)w, (int)h, 4, rgba, (int)w * 4);
    free(rgba);
    return ok;
}
