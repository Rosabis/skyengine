#ifndef __SKYENGINE_PLATFORM_FONT_H__
#define __SKYENGINE_PLATFORM_FONT_H__

#include <stdint.h>

/*
 * Rasterize one UCS-2 code point with the host platform's system sans font.
 * The returned bitmap is tightly packed, row-major, one bit per pixel (MSB
 * first) and remains owned by this module. Glyphs use the handset ABI's fixed
 * cells (ASCII 8px, CJK 12/16px), so width * height never exceeds 256 bits,
 * matching the legacy Mythroad table[30] 32-byte glyph slot.
 */
int platform_font_init(void);
const uint8_t *platform_font_glyph(uint16_t codepoint, int pixel_height,
                                   int *width, int *height);
void platform_font_shutdown(void);

#endif
