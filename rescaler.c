#include "rescaler.h"

// CPU downscaling (0.5x). The framebuffer lives in uncached video memory, where every
// read is slow, so we load two pixels per 64-bit read and keep the first one.
// That halves the number of reads compared to the old pixel-by-pixel loop.
void rescaleBuffer(const uint32_t *src, uint32_t *dst, uint32_t src_pitch, uint32_t dst_pitch, uint32_t dst_w, uint32_t dst_h) {
	for (uint32_t y = 0; y < dst_h; y++) {
		const uint64_t *s = (const uint64_t *)(src + src_pitch * (y << 1));
		uint32_t *d = dst + dst_pitch * y;
		uint32_t x = 0;
		for (; x + 4 <= dst_w; x += 4) {
			uint64_t a = s[x], b = s[x + 1], c = s[x + 2], e = s[x + 3];
			d[x]     = (uint32_t)a;
			d[x + 1] = (uint32_t)b;
			d[x + 2] = (uint32_t)c;
			d[x + 3] = (uint32_t)e;
		}
		for (; x < dst_w; x++) d[x] = (uint32_t)s[x];
	}
}
