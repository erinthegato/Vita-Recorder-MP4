#ifndef _ENCODER_H_
#define _ENCODER_H_

#include <psp2/types.h>
#include <psp2/jpegenc.h>

/*
 * Hardware MJPEG encoder (sceJpegEnc).
 *
 * Memory is only allocated while recording (encoderInit at start,
 * encoderTerm at stop), so games get that memory back when you're not recording.
 */
typedef struct encoder {
	SceUID block;          // Main buffer memblock (CDRAM or physically contiguous RAM)
	SceUID ctx_block;      // Encoder context memblock
	SceJpegEncoderContext context;
	uint8_t *yuv;          // Colour-converted frame
	uint8_t *out;          // JPEG output
	uint32_t out_size;
	uint32_t *scaled;      // Half-size ARGB frame (only when downscaling)
	uint32_t scaled_pitch;
	int src_w, src_h;      // Game framebuffer size
	int enc_w, enc_h;      // Encoded size
	int in_vram;           // 1 = buffers in CDRAM, 0 = main RAM
	int ready;
} encoder;

// quality: 0 (best) .. 255 (smallest). Returns 0 on success, < 0 on error.
int encoderInit(encoder *enc, int src_w, int src_h, int downscale, int quality);
void encoderTerm(encoder *enc);

// Encodes one ARGB framebuffer. Returns JPEG size (> 0) or < 0 on error; *jpeg points at the data.
int encoderEncode(encoder *enc, const void *fb, int pitch, void **jpeg);

#endif
