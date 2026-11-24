#ifndef _RESCALER_H_
#define _RESCALER_H_

#include <stdint.h>

// Halves a 32-bit framebuffer (nearest neighbour). Pitches are in pixels.
void rescaleBuffer(const uint32_t *src, uint32_t *dst, uint32_t src_pitch, uint32_t dst_pitch, uint32_t dst_w, uint32_t dst_h);

#endif
