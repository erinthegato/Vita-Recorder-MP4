#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/clib.h>
#include "encoder.h"
#include "rescaler.h"

#define ALIGN(x, a) (((x) + ((a) - 1)) & ~((a) - 1))
#define PIXFMT (SCE_JPEGENC_PIXELFORMAT_YCBCR420 | SCE_JPEGENC_PIXELFORMAT_CSC_ARGB_YCBCR)

static void freeBlocks(encoder *enc) {
	if (enc->block >= 0) sceKernelFreeMemBlock(enc->block);
	if (enc->ctx_block >= 0) sceKernelFreeMemBlock(enc->ctx_block);
	enc->block = enc->ctx_block = -1;
}

int encoderInit(encoder *enc, int src_w, int src_h, int downscale, int quality) {
	sceClibMemset(enc, 0, sizeof(*enc));
	enc->block = enc->ctx_block = -1;
	enc->src_w = src_w;
	enc->src_h = src_h;

	// Halving only works cleanly on sizes the JPEG encoder likes (e.g. 960x544 -> 480x272)
	if (downscale && (src_w % 32) == 0 && (src_h % 32) == 0) {
		enc->enc_w = src_w / 2;
		enc->enc_h = src_h / 2;
		enc->scaled_pitch = ALIGN(enc->enc_w, 64);
	} else {
		enc->enc_w = src_w;
		enc->enc_h = src_h;
	}

	// Buffer layout: [YUV input][JPEG output][half-size ARGB frame (optional)]
	// The old code put the downscaled frame on top of the JPEG output and overflowed the
	// memblock; the output buffer was also too small for Best/High quality (empty videos).
	uint32_t yuv_size = ALIGN(ALIGN(enc->enc_w, 16) * ALIGN(enc->enc_h, 16) * 3 / 2, 256);
	enc->out_size = yuv_size; // A JPEG never needs more room than the raw YUV it came from
	uint32_t scaled_size = enc->scaled_pitch ? ALIGN(enc->scaled_pitch * enc->enc_h * 4, 256) : 0;
	uint32_t total = yuv_size + enc->out_size + scaled_size;

	enc->in_vram = 1;
	enc->block = sceKernelAllocMemBlock("vrec_enc", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, ALIGN(total, 256 * 1024), NULL);
	if (enc->block < 0) { // No spare VRAM: try regular RAM
		enc->in_vram = 0;
		enc->block = sceKernelAllocMemBlock("vrec_enc", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, ALIGN(total, 1024 * 1024), NULL);
	}
	if (enc->block < 0) {
		int err = enc->block;
		freeBlocks(enc);
		return err;
	}

	void *base;
	sceKernelGetMemBlockBase(enc->block, &base);
	enc->yuv = (uint8_t *)base;
	enc->out = enc->yuv + yuv_size;
	enc->scaled = scaled_size ? (uint32_t *)(enc->out + enc->out_size) : NULL;

	int ctx_size = sceJpegEncoderGetContextSize();
	enc->ctx_block = sceKernelAllocMemBlock("vrec_ctx", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, ALIGN(ctx_size, 4096), NULL);
	if (enc->ctx_block < 0) {
		int err = enc->ctx_block;
		freeBlocks(enc);
		return err;
	}
	sceKernelGetMemBlockBase(enc->ctx_block, &base);
	enc->context = (SceJpegEncoderContext)base;

	SceJpegEncoderInitParam param;
	sceClibMemset(&param, 0, sizeof(param));
	param.size = sizeof(param);
	param.inWidth = enc->enc_w;
	param.inHeight = enc->enc_h;
	param.pixelFormat = PIXFMT;
	param.outBuffer = enc->out;
	param.outSize = enc->out_size;
	param.option = enc->in_vram ? SCE_JPEGENC_INIT_PARAM_OPTION_NONE : SCE_JPEGENC_INIT_PARAM_OPTION_LPDDR2_MEMORY;
	int ret = sceJpegEncoderInitWithParam(enc->context, &param);
	if (ret < 0) {
		freeBlocks(enc);
		return ret;
	}
	sceJpegEncoderSetValidRegion(enc->context, enc->enc_w, enc->enc_h);
	sceJpegEncoderSetCompressionRatio(enc->context, quality);
	sceJpegEncoderSetOutputAddr(enc->context, enc->out, enc->out_size);

	enc->ready = 1;
	return 0;
}

void encoderTerm(encoder *enc) {
	if (enc->ready) sceJpegEncoderEnd(enc->context);
	enc->ready = 0;
	freeBlocks(enc);
}

int encoderEncode(encoder *enc, const void *fb, int pitch, void **jpeg) {
	if (!enc->ready) return -1;
	if (enc->scaled) {
		rescaleBuffer((const uint32_t *)fb, enc->scaled, pitch, enc->scaled_pitch, enc->enc_w, enc->enc_h);
		fb = enc->scaled;
		pitch = enc->scaled_pitch;
	}
	int ret = sceJpegEncoderCsc(enc->context, enc->yuv, fb, pitch, SCE_JPEGENC_PIXELFORMAT_ARGB8888);
	if (ret < 0) return ret;
	ret = sceJpegEncoderEncode(enc->context, enc->yuv);
	if (ret <= 0) return ret < 0 ? ret : -1; // Never write a 0/negative-sized frame
	*jpeg = enc->out;
	return ret;
}
