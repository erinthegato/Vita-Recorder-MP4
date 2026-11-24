#include <psp2/types.h>
#include <psp2/display.h>
#include <psp2/kernel/clib.h>
#include <stdarg.h>
#include "renderer.h"
#include "font.h"

#define CHAR_W 12 // 6px font drawn at 2x
#define CHAR_H 20

static uint32_t *vram32;
static int pwidth, pheight, bufferwidth;
static uint32_t color;

void updateFramebuf(const SceDisplayFrameBuf *param) {
	pwidth = param->width;
	pheight = param->height;
	vram32 = (uint32_t *)param->base;
	bufferwidth = param->pitch;
}

void setTextColor(uint32_t clr) {
	color = clr;
}

void drawCharacter(int character, int x, int y) {
	// Skip anything that would land outside the framebuffer (the old code could write past it)
	if (!vram32 || x < 0 || y < 0 || x + CHAR_W > pwidth || y + CHAR_H > pheight) return;
	const unsigned char *glyph = &font[(character & 0xFF) * FONT_HEIGHT];
	for (int yy = 0; yy < FONT_HEIGHT; yy++) {
		uint32_t *row = vram32 + (y + (yy << 1)) * bufferwidth + x;
		uint8_t bits = glyph[yy];
		for (int xx = 7; xx >= 2; xx--) {
			uint32_t clr = ((bits >> xx) & 1) ? color : 0xFF000000;
			row[0] = clr;
			row[1] = clr;
			row[bufferwidth] = clr;
			row[bufferwidth + 1] = clr;
			row += 2;
		}
	}
}

void drawString(int x, int y, const char *str) {
	for (; *str; str++, x += CHAR_W) drawCharacter((unsigned char)*str, x, y);
}

void drawStringF(int x, int y, const char *format, ...) {
	char str[128]; // This runs on the game's thread, so keep stack use small
	va_list va;
	va_start(va, format);
	sceClibVsnprintf(str, sizeof(str), format, va);
	va_end(va);
	drawString(x, y, str);
}
