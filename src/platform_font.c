#include "./include/platform_font.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__EMSCRIPTEN__)
#include <emscripten.h>
#else
#include <dirent.h>
#include <strings.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "../third_party/stb_truetype.h"

#if defined(__linux__) && !defined(__ANDROID__)
#include <dlfcn.h>
#endif
#endif

#define PLATFORM_FONT_CACHE_SIZE 256
#define PLATFORM_FONT_BITMAP_BYTES 32
#define PLATFORM_FONT_MAX_PIXELS 16

typedef struct PlatformGlyph {
    uint16_t codepoint;
    uint8_t pixel_height;
    uint8_t width;
    uint8_t height;
    uint8_t valid;
    uint8_t bitmap[PLATFORM_FONT_BITMAP_BYTES];
} PlatformGlyph;

static PlatformGlyph platform_glyph_cache[PLATFORM_FONT_CACHE_SIZE];
static int platform_font_ready;

static PlatformGlyph *platform_font_cache_slot(uint16_t codepoint,
                                                int pixel_height) {
    unsigned int key = (unsigned int)codepoint * 33u + (unsigned int)pixel_height;
    return &platform_glyph_cache[key % PLATFORM_FONT_CACHE_SIZE];
}

static int platform_font_normalize_height(int pixel_height) {
    return pixel_height <= 12 ? 12 : 16;
}

/* Mythroad layout is based on fixed handset font cells, even when the host
 * system font itself is proportional. Keep that ABI while changing glyphs. */
static int platform_font_cell_width(uint16_t codepoint, int pixel_height) {
    return codepoint < 128 ? 8 : pixel_height;
}

static void platform_font_set_bit(uint8_t *bitmap, int width, int x, int y) {
    int bit = y * width + x;
    bitmap[bit >> 3] |= (uint8_t)(0x80u >> (bit & 7));
}

#if defined(_WIN32)

static wchar_t platform_font_face[LF_FACESIZE];
static HFONT platform_fonts[17];

static HFONT platform_font_windows_handle(int pixel_height) {
    HFONT font = platform_fonts[pixel_height];
    if (font != NULL) return font;
    font = CreateFontW(-pixel_height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                       platform_font_face);
    platform_fonts[pixel_height] = font;
    return font;
}

static int platform_font_render_uncached(uint16_t codepoint, int pixel_height,
                                         PlatformGlyph *glyph) {
    BITMAPINFO bitmap_info;
    SIZE extent;
    HDC dc;
    HBITMAP dib;
    HGDIOBJ old_bitmap;
    HGDIOBJ old_font;
    HFONT font;
    uint32_t *pixels = NULL;
    wchar_t text = (wchar_t)codepoint;
    int width;
    int x;
    int y;

    memset(&bitmap_info, 0, sizeof(bitmap_info));
    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth = PLATFORM_FONT_MAX_PIXELS;
    bitmap_info.bmiHeader.biHeight = -PLATFORM_FONT_MAX_PIXELS;
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;

    dc = CreateCompatibleDC(NULL);
    if (dc == NULL) return 0;
    dib = CreateDIBSection(dc, &bitmap_info, DIB_RGB_COLORS,
                           (void **)&pixels, NULL, 0);
    if (dib == NULL || pixels == NULL) {
        DeleteDC(dc);
        return 0;
    }
    font = platform_font_windows_handle(pixel_height);
    if (font == NULL) {
        DeleteObject(dib);
        DeleteDC(dc);
        return 0;
    }
    old_bitmap = SelectObject(dc, dib);
    old_font = SelectObject(dc, font);
    memset(pixels, 0,
           PLATFORM_FONT_MAX_PIXELS * PLATFORM_FONT_MAX_PIXELS * sizeof(*pixels));
    SetBkMode(dc, OPAQUE);
    SetBkColor(dc, RGB(0, 0, 0));
    SetTextColor(dc, RGB(255, 255, 255));
    SetTextAlign(dc, TA_LEFT | TA_TOP | TA_NOUPDATECP);
    if (!GetTextExtentPoint32W(dc, &text, 1, &extent)) {
        extent.cx = pixel_height;
    }
    width = platform_font_cell_width(codepoint, pixel_height);
    TextOutW(dc, 0, 0, &text, 1);

    memset(glyph->bitmap, 0, sizeof(glyph->bitmap));
    for (y = 0; y < pixel_height; ++y) {
        for (x = 0; x < width; ++x) {
            int source_width = extent.cx;
            int source_x;
            uint32_t color;
            if (source_width < 1) source_width = 1;
            if (source_width > PLATFORM_FONT_MAX_PIXELS) {
                source_width = PLATFORM_FONT_MAX_PIXELS;
            }
            source_x = source_width > width ? x * source_width / width : x;
            color = pixels[y * PLATFORM_FONT_MAX_PIXELS + source_x];
            if (((color & 0xffu) + ((color >> 8) & 0xffu) +
                 ((color >> 16) & 0xffu)) >= 144u) {
                platform_font_set_bit(glyph->bitmap, width, x, y);
            }
        }
    }
    glyph->width = (uint8_t)width;
    glyph->height = (uint8_t)pixel_height;

    SelectObject(dc, old_font);
    SelectObject(dc, old_bitmap);
    DeleteObject(dib);
    DeleteDC(dc);
    return 1;
}

#elif defined(__EMSCRIPTEN__)

EM_JS(int, platform_font_web_render,
      (int codepoint, int pixel_height, uint8_t *bitmap, int *width_out), {
    if (!Module.skyengineFontCanvas) {
        Module.skyengineFontCanvas = document.createElement('canvas');
        Module.skyengineFontCanvas.width = 16;
        Module.skyengineFontCanvas.height = 16;
        Module.skyengineFontContext = Module.skyengineFontCanvas.getContext('2d', {
            willReadFrequently: true
        });
    }
    var ctx = Module.skyengineFontContext;
    var canvas = Module.skyengineFontCanvas;
    var text = String.fromCharCode(codepoint);
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    ctx.font = pixel_height + 'px system-ui, sans-serif';
    ctx.textBaseline = 'top';
    ctx.textAlign = 'left';
    ctx.fillStyle = '#fff';
    var width = codepoint < 128 ? 8 : pixel_height;
    var measuredWidth = Math.max(1, ctx.measureText(text).width);
    var horizontalScale = measuredWidth > width ? width / measuredWidth : 1;
    ctx.save();
    ctx.scale(horizontalScale, 1);
    ctx.fillText(text, 0, 0);
    ctx.restore();
    var pixels = ctx.getImageData(0, 0, width, pixel_height).data;
    HEAPU8.fill(0, bitmap, bitmap + 32);
    for (var y = 0; y < pixel_height; ++y) {
        for (var x = 0; x < width; ++x) {
            if (pixels[(y * width + x) * 4 + 3] >= 48) {
                var bit = y * width + x;
                HEAPU8[bitmap + (bit >> 3)] |= 0x80 >> (bit & 7);
            }
        }
    }
    HEAP32[width_out >> 2] = width;
    return 1;
});

static int platform_font_render_uncached(uint16_t codepoint, int pixel_height,
                                         PlatformGlyph *glyph) {
    int width = 0;
    if (!platform_font_web_render((int)codepoint, pixel_height,
                                  glyph->bitmap, &width)) {
        return 0;
    }
    glyph->width = (uint8_t)width;
    glyph->height = (uint8_t)pixel_height;
    return 1;
}

#else

static unsigned char *platform_font_data;
static stbtt_fontinfo platform_font_info;

static unsigned char *platform_font_read_file(const char *path, size_t *size_out) {
    FILE *file;
    long size;
    unsigned char *data;
    if (path == NULL || path[0] == '\0') return NULL;
    file = fopen(path, "rb");
    if (file == NULL) return NULL;
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) <= 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    data = (unsigned char *)malloc((size_t)size);
    if (data == NULL || fread(data, 1, (size_t)size, file) != (size_t)size) {
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *size_out = (size_t)size;
    return data;
}

static int platform_font_use_file(const char *path, int preferred_index) {
    unsigned char *data;
    size_t data_size = 0;
    int face_count;
    int face_index;

    data = platform_font_read_file(path, &data_size);
    if (data == NULL) return 0;
    (void)data_size;
    face_count = stbtt_GetNumberOfFonts(data);
    if (face_count <= 0) face_count = 1;
    for (face_index = -1; face_index < face_count; ++face_index) {
        int selected_index;
        int offset;
        stbtt_fontinfo info;
        if (face_index == -1) {
            if (preferred_index < 0 || preferred_index >= face_count) continue;
            selected_index = preferred_index;
        } else {
            if (face_index == preferred_index) continue;
            selected_index = face_index;
        }
        offset = stbtt_GetFontOffsetForIndex(data, selected_index);
        if (offset < 0 || !stbtt_InitFont(&info, data, offset)) continue;
        /* The emulator needs one face that covers both Latin UI text and CJK. */
        if (!stbtt_FindGlyphIndex(&info, 'A') ||
            !stbtt_FindGlyphIndex(&info, 0x4e2d)) {
            continue;
        }
        free(platform_font_data);
        platform_font_data = data;
        platform_font_info = info;
        return 1;
    }
    free(data);
    return 0;
}

#if defined(__linux__) && !defined(__ANDROID__)

typedef unsigned char FcChar8;
typedef int FcBool;
typedef struct _FcConfig FcConfig;
typedef struct _FcPattern FcPattern;
typedef enum {
    FcResultMatch,
    FcResultNoMatch,
    FcResultTypeMismatch,
    FcResultNoId,
    FcResultOutOfMemory
} FcResult;

typedef struct PlatformFontconfigApi {
    void *library;
    FcConfig *(*init_load_config_and_fonts)(void);
    FcPattern *(*name_parse)(const FcChar8 *name);
    FcBool (*config_substitute)(FcConfig *config, FcPattern *pattern, int kind);
    void (*default_substitute)(FcPattern *pattern);
    FcPattern *(*font_match)(FcConfig *config, FcPattern *pattern,
                             FcResult *result);
    FcResult (*pattern_get_string)(const FcPattern *pattern,
                                   const char *object, int id,
                                   FcChar8 **value);
    FcResult (*pattern_get_integer)(const FcPattern *pattern,
                                    const char *object, int id, int *value);
    void (*pattern_destroy)(FcPattern *pattern);
    void (*config_destroy)(FcConfig *config);
} PlatformFontconfigApi;

static int platform_fontconfig_symbol(void *library, const char *name,
                                      void *target, size_t target_size) {
    void *symbol = dlsym(library, name);
    if (symbol == NULL || target_size != sizeof(symbol)) return 0;
    memcpy(target, &symbol, sizeof(symbol));
    return 1;
}

#define PLATFORM_FC_LOAD(api, member, symbol)                              \
    platform_fontconfig_symbol((api)->library, (symbol),                   \
                               &(api)->member, sizeof((api)->member))

static int platform_font_linux_init(void) {
    PlatformFontconfigApi api;
    FcConfig *config = NULL;
    FcPattern *request = NULL;
    FcPattern *match = NULL;
    FcChar8 *path = NULL;
    FcResult result = FcResultNoMatch;
    int index = 0;
    int loaded = 0;

    memset(&api, 0, sizeof(api));
    api.library = dlopen("libfontconfig.so.1", RTLD_NOW | RTLD_LOCAL);
    if (api.library == NULL ||
        !PLATFORM_FC_LOAD(&api, init_load_config_and_fonts,
                          "FcInitLoadConfigAndFonts") ||
        !PLATFORM_FC_LOAD(&api, name_parse, "FcNameParse") ||
        !PLATFORM_FC_LOAD(&api, config_substitute, "FcConfigSubstitute") ||
        !PLATFORM_FC_LOAD(&api, default_substitute, "FcDefaultSubstitute") ||
        !PLATFORM_FC_LOAD(&api, font_match, "FcFontMatch") ||
        !PLATFORM_FC_LOAD(&api, pattern_get_string, "FcPatternGetString") ||
        !PLATFORM_FC_LOAD(&api, pattern_get_integer, "FcPatternGetInteger") ||
        !PLATFORM_FC_LOAD(&api, pattern_destroy, "FcPatternDestroy") ||
        !PLATFORM_FC_LOAD(&api, config_destroy, "FcConfigDestroy")) {
        if (api.library != NULL) dlclose(api.library);
        return 0;
    }
    config = api.init_load_config_and_fonts();
    request = api.name_parse((const FcChar8 *)"sans-serif:lang=zh-cn");
    if (config != NULL && request != NULL &&
        api.config_substitute(config, request, 0)) {
        api.default_substitute(request);
        match = api.font_match(config, request, &result);
    }
    if (match != NULL && result == FcResultMatch &&
        api.pattern_get_string(match, "file", 0, &path) == FcResultMatch) {
        if (api.pattern_get_integer(match, "index", 0, &index) != FcResultMatch) {
            index = 0;
        }
        loaded = platform_font_use_file((const char *)path, index);
    }
    if (match != NULL) api.pattern_destroy(match);
    if (request != NULL) api.pattern_destroy(request);
    if (config != NULL) api.config_destroy(config);
    dlclose(api.library);
    return loaded;
}

#endif

#if defined(__ANDROID__)

static int platform_font_android_init(void) {
    static const char *const directories[] = {
        "/system/fonts", "/product/fonts", "/system_ext/fonts"
    };
    size_t directory_index;
    for (directory_index = 0;
         directory_index < sizeof(directories) / sizeof(directories[0]);
         ++directory_index) {
        DIR *directory = opendir(directories[directory_index]);
        struct dirent *entry;
        if (directory == NULL) continue;
        while ((entry = readdir(directory)) != NULL) {
            const char *extension = strrchr(entry->d_name, '.');
            char path[1024];
            int length;
            if (extension == NULL ||
                (strcasecmp(extension, ".ttf") != 0 &&
                 strcasecmp(extension, ".ttc") != 0 &&
                 strcasecmp(extension, ".otf") != 0)) {
                continue;
            }
            length = snprintf(path, sizeof(path), "%s/%s",
                              directories[directory_index], entry->d_name);
            if (length <= 0 || (size_t)length >= sizeof(path)) continue;
            if (platform_font_use_file(path, -1)) {
                closedir(directory);
                return 1;
            }
        }
        closedir(directory);
    }
    return 0;
}

#endif

static int platform_font_render_uncached(uint16_t codepoint, int pixel_height,
                                         PlatformGlyph *glyph) {
    unsigned char alpha[PLATFORM_FONT_MAX_PIXELS * PLATFORM_FONT_MAX_PIXELS];
    float scale;
    float horizontal_scale;
    int ascent;
    int descent;
    int line_gap;
    int advance;
    int left_bearing;
    int x0;
    int y0;
    int x1;
    int y1;
    int bitmap_width;
    int bitmap_height;
    int baseline;
    int width;
    int x;
    int y;

    scale = stbtt_ScaleForPixelHeight(&platform_font_info, (float)pixel_height);
    stbtt_GetFontVMetrics(&platform_font_info, &ascent, &descent, &line_gap);
    stbtt_GetCodepointHMetrics(&platform_font_info, codepoint,
                               &advance, &left_bearing);
    width = platform_font_cell_width(codepoint, pixel_height);
    horizontal_scale = scale;
    if ((float)advance * horizontal_scale > (float)width) {
        horizontal_scale *= (float)width / ((float)advance * horizontal_scale);
    }
    stbtt_GetCodepointBitmapBox(&platform_font_info, codepoint,
                                horizontal_scale, scale,
                                &x0, &y0, &x1, &y1);
    (void)descent;
    (void)line_gap;
    (void)left_bearing;
    bitmap_width = x1 - x0;
    bitmap_height = y1 - y0;
    memset(alpha, 0, sizeof(alpha));
    if (bitmap_width > 0 && bitmap_height > 0 &&
        bitmap_width <= PLATFORM_FONT_MAX_PIXELS &&
        bitmap_height <= PLATFORM_FONT_MAX_PIXELS) {
        stbtt_MakeCodepointBitmap(&platform_font_info, alpha,
                                  bitmap_width, bitmap_height, bitmap_width,
                                  horizontal_scale, scale, codepoint);
    }
    baseline = (int)((float)ascent * scale + 0.5f);
    memset(glyph->bitmap, 0, sizeof(glyph->bitmap));
    for (y = 0; y < bitmap_height; ++y) {
        int destination_y = baseline + y0 + y;
        if (destination_y < 0 || destination_y >= pixel_height) continue;
        for (x = 0; x < bitmap_width; ++x) {
            int destination_x = x0 + x;
            if (destination_x < 0 || destination_x >= width) continue;
            if (alpha[y * bitmap_width + x] >= 48) {
                platform_font_set_bit(glyph->bitmap, width,
                                      destination_x, destination_y);
            }
        }
    }
    glyph->width = (uint8_t)width;
    glyph->height = (uint8_t)pixel_height;
    return 1;
}

#endif

int platform_font_init(void) {
    if (platform_font_ready) return 1;
#if defined(_WIN32)
    NONCLIENTMETRICSW metrics;
    memset(&metrics, 0, sizeof(metrics));
    metrics.cbSize = sizeof(metrics);
    if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics),
                               &metrics, 0)) {
        return 0;
    }
    memcpy(platform_font_face, metrics.lfMessageFont.lfFaceName,
           sizeof(platform_font_face));
    platform_font_face[LF_FACESIZE - 1] = L'\0';
    platform_font_ready = 1;
#elif defined(__EMSCRIPTEN__)
    platform_font_ready = 1;
#elif defined(__ANDROID__)
    platform_font_ready = platform_font_android_init();
#elif defined(__linux__)
    platform_font_ready = platform_font_linux_init();
#else
#error Unsupported platform font backend
#endif
    return platform_font_ready;
}

const uint8_t *platform_font_glyph(uint16_t codepoint, int pixel_height,
                                   int *width, int *height) {
    PlatformGlyph *glyph;
    pixel_height = platform_font_normalize_height(pixel_height);
    if (!platform_font_init()) return NULL;
    glyph = platform_font_cache_slot(codepoint, pixel_height);
    if (!glyph->valid || glyph->codepoint != codepoint ||
        glyph->pixel_height != pixel_height) {
        memset(glyph, 0, sizeof(*glyph));
        if (!platform_font_render_uncached(codepoint, pixel_height, glyph)) {
            return NULL;
        }
        glyph->codepoint = codepoint;
        glyph->pixel_height = (uint8_t)pixel_height;
        glyph->valid = 1;
    }
    if (width != NULL) *width = glyph->width;
    if (height != NULL) *height = glyph->height;
    return glyph->bitmap;
}

void platform_font_shutdown(void) {
#if defined(_WIN32)
    int height;
    for (height = 0; height < (int)(sizeof(platform_fonts) /
                                    sizeof(platform_fonts[0])); ++height) {
        if (platform_fonts[height] != NULL) {
            DeleteObject(platform_fonts[height]);
            platform_fonts[height] = NULL;
        }
    }
#elif !defined(__EMSCRIPTEN__)
    free(platform_font_data);
    platform_font_data = NULL;
    memset(&platform_font_info, 0, sizeof(platform_font_info));
#endif
    memset(platform_glyph_cache, 0, sizeof(platform_glyph_cache));
    platform_font_ready = 0;
}
