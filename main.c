#include <vitasdk.h>
#include <taihen.h>
#include <stdarg.h>
#include "renderer.h"
#include "encoder.h"
#include "mp4.h"

#define VERSION_STR     "v0.2"
#define HOOKS_NUM       13
#define MENU_ENTRIES    6
#define QUALITY_ENTRIES 5
#define ERROR_SHOW_US   (5 * 1000 * 1000)

#define OUT_DIR         "ux0:video/VitaRecorder"
#define CFG_DIR         "ux0:data/VitaRecorder"
#define CFG_PATH        CFG_DIR "/config.bin"
#define CFG_MAGIC       0x47464356 // "VCFG"
#define CFG_VERSION     2

#define COLOR_WHITE 0x00FFFFFF
#define COLOR_GREEN 0x0000FF00
#define COLOR_GREY  0x00808080
#define COLOR_RED   0xFF0000FF

enum { NOT_TRIGGERED, CONFIG_MENU };
enum { MENU_QUALITY, MENU_DOWNSCALE, MENU_FRAMESKIP, MENU_TYPE, MENU_INDICATOR, MENU_RECORD };

// ----- Saved settings -----
typedef struct {
	uint32_t magic;
	uint8_t version;
	uint8_t quality;   // index into qualities[]
	uint8_t downscale; // halve resolution (faster, smaller files)
	uint8_t frameskip; // synchronous mode only
	uint8_t async;     // 1 = background thread, 0 = frame-perfect (slower game)
	uint8_t indicator; // draw the red "REC" marker
	uint8_t pad[2];
} config_t;

static config_t cfg = {CFG_MAGIC, CFG_VERSION, 2, 1, 0, 1, 1, {0}};

static const char *qualities[] = {"Best", "High", "Default", "Low", "Worst"};
static const uint8_t qual_val[] = {0, 64, 128, 192, 255};

// ----- Hooks -----
static SceUID g_hooks[HOOKS_NUM];
static tai_hook_ref_t ref[HOOKS_NUM];
static int cur_hook = 0;

// ----- Recording state -----
// is_recording: what the user asked for. writer_ready: the worker has the file + encoder open.
// Only the worker thread opens/closes files and allocates/frees the encoder.
static volatile int is_recording = 0;
static volatile int writer_ready = 0;
static volatile int quit = 0;
static int rec_async = 1; // Mode locked in when a recording starts
static SceUID worker_id = -1, wake_sema = -1;
static SceUID rec_mutex = -1; // Held while a frame is written or the file is opened/closed
static encoder enc;
static mp4_writer mp4;
static volatile uint32_t frames_written = 0, frames_dropped = 0;
static int skip_counter = 0;

// Latest frame the game handed to the display, published with a sequence lock
static volatile uint32_t fb_seq = 0;
static void *fb_base;
static int fb_pitch, fb_width, fb_height, fb_format;
static uint64_t fb_time;

// ----- UI state -----
static uint8_t status = NOT_TRIGGERED;
static int cfg_i = 0;
static uint32_t old_buttons = 0;
static char message[96];
static uint64_t message_time = 0;
static uint32_t message_color = COLOR_WHITE;
static char last_file[64];
static char titleid[16];

static void showMessage(uint32_t color, const char *fmt, ...) {
	va_list va;
	va_start(va, fmt);
	sceClibVsnprintf(message, sizeof(message), fmt, va);
	va_end(va);
	message_color = color;
	message_time = sceKernelGetProcessTimeWide();
}

// ----- Settings file -----
static void loadConfig(void) {
	config_t c;
	SceUID fd = sceIoOpen(CFG_PATH, SCE_O_RDONLY, 0);
	if (fd < 0) return;
	int n = sceIoRead(fd, &c, sizeof(c));
	sceIoClose(fd);
	if (n != sizeof(c) || c.magic != CFG_MAGIC || c.version != CFG_VERSION) return;
	if (c.quality >= QUALITY_ENTRIES || c.frameskip > 4) return;
	c.downscale = !!c.downscale;
	c.async = !!c.async;
	c.indicator = !!c.indicator;
	cfg = c;
}

static void saveConfig(void) {
	sceIoMkdir(CFG_DIR, 0777);
	SceUID fd = sceIoOpen(CFG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
	if (fd < 0) return;
	sceIoWrite(fd, &cfg, sizeof(cfg));
	sceIoClose(fd);
}

// ----- Recording (worker side) -----

// Finishes any .mp4 left unfinished by a crash during a previous recording
static void recoverLeftovers(void) {
	static SceIoDirent ent;
	static char path[192];
	SceUID d = sceIoDopen(OUT_DIR);
	if (d < 0) return;
	int recovered = 0;
	while (sceIoDread(d, &ent) > 0) {
		int len = sceClibStrnlen(ent.d_name, sizeof(ent.d_name));
		if (len > 8 && sceClibStrncmp(ent.d_name + len - 8, ".mp4.idx", 8) == 0) {
			sceClibSnprintf(path, sizeof(path), "%s/%s", OUT_DIR, ent.d_name);
			path[sceClibStrnlen(path, sizeof(path)) - 4] = 0; // Strip ".idx"
			if (mp4Recover(path) == 0) recovered++;
		}
	}
	sceIoDclose(d);
	if (recovered) showMessage(COLOR_GREEN, "Recovered %d unfinished video(s)", recovered);
}

// Grabs the most recent frame without tearing it mid-update
static int readLatestFrame(void **base, int *pitch, int *w, int *h, int *fmt, uint64_t *t, uint32_t *seq) {
	for (int tries = 0; tries < 8; tries++) {
		uint32_t s1 = fb_seq;
		if (s1 == 0 || (s1 & 1)) continue;
		__sync_synchronize();
		*base = fb_base; *pitch = fb_pitch; *w = fb_width; *h = fb_height; *fmt = fb_format; *t = fb_time;
		__sync_synchronize();
		if (fb_seq == s1) { *seq = s1; return 1; }
	}
	return 0;
}

// Encodes and saves one frame. Called from the worker (async) or the game thread (sync).
static void writeFrame(void *base, int pitch, int w, int h, int fmt, uint64_t t) {
	// The encoder is set up for the resolution at record start; skip frames that don't match
	// (some games switch resolution) instead of reading the framebuffer with the wrong size.
	if (w != enc.src_w || h != enc.src_h || fmt != SCE_DISPLAY_PIXELFORMAT_A8B8G8R8) {
		frames_dropped++;
		return;
	}
	void *jpeg;
	int size = encoderEncode(&enc, base, pitch, &jpeg);
	if (size <= 0) {
		frames_dropped++;
		return;
	}
	if (mp4WriteFrame(&mp4, jpeg, size, t) < 0) {
		showMessage(COLOR_RED, "ERROR: write failed (storage full?)");
		is_recording = 0;
		return;
	}
	frames_written++;
}

static int startSession(void) {
	void *base; int pitch, w, h, fmt; uint64_t t; uint32_t seq;
	if (!readLatestFrame(&base, &pitch, &w, &h, &fmt, &t, &seq)) {
		showMessage(COLOR_RED, "ERROR: no frame to record yet");
		return -1;
	}
	if (fmt != SCE_DISPLAY_PIXELFORMAT_A8B8G8R8) {
		showMessage(COLOR_RED, "ERROR: unsupported pixel format 0x%X", fmt);
		return -1;
	}

	int ret = encoderInit(&enc, w, h, cfg.downscale, qual_val[cfg.quality]);
	if (ret < 0) {
		showMessage(COLOR_RED, "ERROR: not enough memory (0x%08X)", ret);
		return -1;
	}

	static char path[160];
	SceDateTime d;
	sceRtcGetCurrentClockLocalTime(&d);
	sceIoMkdir("ux0:video", 0777);
	sceIoMkdir(OUT_DIR, 0777);
	sceClibSnprintf(last_file, sizeof(last_file), "%s_%04d-%02d-%02d_%02d-%02d-%02d.mp4",
		titleid, d.year, d.month, d.day, d.hour, d.minute, d.second);
	sceClibSnprintf(path, sizeof(path), "%s/%s", OUT_DIR, last_file);

	ret = mp4Open(&mp4, path, enc.enc_w, enc.enc_h);
	if (ret < 0) {
		encoderTerm(&enc);
		showMessage(COLOR_RED, "ERROR: can't create video (0x%08X)", ret);
		return -1;
	}
	frames_written = frames_dropped = 0;
	skip_counter = 0;
	return 0;
}

static void stopSession(void) {
	int ret = mp4Close(&mp4);
	encoderTerm(&enc);
	if (ret == 0) showMessage(COLOR_GREEN, "Saved %s", last_file);
	else if (ret == -2) showMessage(COLOR_RED, "Nothing was recorded");
	else showMessage(COLOR_RED, "ERROR: finishing video failed (%d)", ret);
}

static int worker(SceSize args, void *argp) {
	uint32_t last_seq = 0;
	recoverLeftovers();
	while (!quit) {
		sceKernelWaitSema(wake_sema, 1, NULL); // Woken by the display hook, once per game frame
		if (quit) break;

		sceKernelLockMutex(rec_mutex, 1, NULL);
		if (is_recording && !writer_ready) {
			rec_async = cfg.async;
			if (startSession() < 0) {
				is_recording = 0;
				sceKernelUnlockMutex(rec_mutex, 1);
				continue;
			}
			__sync_synchronize(); // Encoder must be fully set up before the game thread sees writer_ready
			writer_ready = 1;
			last_seq = 0;
		}

		if (!is_recording && writer_ready) {
			writer_ready = 0;
			stopSession();
			sceKernelUnlockMutex(rec_mutex, 1);
			continue;
		}

		if (is_recording && writer_ready && rec_async) {
			void *base; int pitch, w, h, fmt; uint64_t t; uint32_t seq;
			// Only encode frames we haven't seen (the old loop re-encoded the same frame over and over)
			if (readLatestFrame(&base, &pitch, &w, &h, &fmt, &t, &seq) && seq != last_seq) {
				last_seq = seq;
				writeFrame(base, pitch, w, h, fmt, t);
			}
		}
		sceKernelUnlockMutex(rec_mutex, 1);
	}
	if (writer_ready) {
		writer_ready = 0;
		stopSession();
	}
	return 0;
}

// ----- UI (runs on the game's thread) -----

static int settingsLocked(void) {
	return is_recording || writer_ready;
}

static void toggleRecording(void) {
	is_recording = !is_recording;
	if (is_recording) status = NOT_TRIGGERED;
	sceKernelSignalSema(wake_sema, 1);
}

static void drawConfigMenu(int width, int height) {
	int locked = settingsLocked();
	setTextColor(COLOR_WHITE);
	drawString(5, 5, "Vita Recorder " VERSION_STR " - CONFIG MENU");
	drawStringF(5, 25, "Title ID: %s  Res: %dx%d", titleid, width, height);
	drawString(5, 45, "Output: " OUT_DIR);

	for (int i = 0; i < MENU_ENTRIES; i++) {
		int y = 75 + i * 20;
		uint32_t c = (i == cfg_i) ? COLOR_GREEN : COLOR_WHITE;
		if (locked && i != MENU_RECORD) c = (i == cfg_i) ? 0x00008000 : COLOR_GREY;
		setTextColor(c);
		switch (i) {
		case MENU_QUALITY:   drawStringF(5, y, "Video Quality: %s", qualities[cfg.quality]); break;
		case MENU_DOWNSCALE: drawStringF(5, y, "Downscaler: %s", cfg.downscale ? "Enabled" : "Disabled"); break;
		case MENU_FRAMESKIP:
			if (cfg.async) drawString(5, y, "Frame Skip: Auto");
			else drawStringF(5, y, "Frame Skip: %d", cfg.frameskip);
			break;
		case MENU_TYPE:      drawStringF(5, y, "Recorder Type: %s", cfg.async ? "Asynchronous" : "Synchronous"); break;
		case MENU_INDICATOR: drawStringF(5, y, "REC Indicator: %s", cfg.indicator ? "Shown" : "Hidden"); break;
		case MENU_RECORD:    drawStringF(5, y, "%s Screen Recording", is_recording ? "Stop" : "Start"); break;
		}
	}

	setTextColor(COLOR_WHITE);
	if (writer_ready) {
		drawStringF(5, 205, "Recording: %u frames, %u skipped, %u MB",
			(unsigned int)frames_written, (unsigned int)frames_dropped, (unsigned int)(mp4.data_size >> 20));
	} else if (last_file[0]) {
		drawStringF(5, 205, "Last: %s", last_file);
	}
	drawString(5, 225, locked ? "Stop recording to change settings" : "X: change  Triangle: close");
}

static void checkInput(const SceCtrlData *ctrl) {
	uint32_t pressed = ctrl->buttons & ~old_buttons;
	if (status == CONFIG_MENU) {
		if (pressed & SCE_CTRL_DOWN) {
			cfg_i = (cfg_i + 1) % MENU_ENTRIES;
		} else if (pressed & SCE_CTRL_UP) {
			cfg_i = (cfg_i + MENU_ENTRIES - 1) % MENU_ENTRIES;
		} else if (pressed & SCE_CTRL_CROSS) {
			// Changing the encoder mid-recording used to free memory the encoder was still using
			if (cfg_i == MENU_RECORD) toggleRecording();
			else if (!settingsLocked()) {
				switch (cfg_i) {
				case MENU_QUALITY:   cfg.quality = (cfg.quality + 1) % QUALITY_ENTRIES; break;
				case MENU_DOWNSCALE: cfg.downscale = !cfg.downscale; break;
				case MENU_FRAMESKIP: if (!cfg.async) cfg.frameskip = (cfg.frameskip + 1) % 5; break;
				case MENU_TYPE:      cfg.async = !cfg.async; break;
				case MENU_INDICATOR: cfg.indicator = !cfg.indicator; break;
				}
			}
		} else if (pressed & SCE_CTRL_TRIANGLE) {
			status = NOT_TRIGGERED;
			saveConfig();
		}
	} else if ((ctrl->buttons & SCE_CTRL_LTRIGGER) && (ctrl->buttons & SCE_CTRL_SELECT)) {
		status = CONFIG_MENU;
	} else if ((ctrl->buttons & SCE_CTRL_LTRIGGER) && (ctrl->buttons & SCE_CTRL_START)
		&& !((old_buttons & SCE_CTRL_LTRIGGER) && (old_buttons & SCE_CTRL_START))) {
		toggleRecording();
	}
	old_buttons = ctrl->buttons;
}

// ----- Hooks -----

// Called every time the game shows a new frame: this is our main loop
static int sceDisplaySetFrameBuf_patched(const SceDisplayFrameBuf *pParam, SceDisplaySetBufSync sync) {
	if (!pParam || !pParam->base) return TAI_CONTINUE(int, ref[0], pParam, sync);
	uint64_t now = sceKernelGetProcessTimeWide();

	// Publish this frame to the worker thread
	fb_seq++;
	__sync_synchronize();
	fb_base = pParam->base;
	fb_pitch = pParam->pitch;
	fb_width = pParam->width;
	fb_height = pParam->height;
	fb_format = pParam->pixelformat;
	fb_time = now;
	__sync_synchronize();
	fb_seq++;

	// Synchronous recording: encode before drawing any overlay, so the menu never ends up in the video
	if (is_recording && writer_ready && !rec_async) {
		if (skip_counter >= cfg.frameskip) {
			skip_counter = 0;
			// Never block the game: if the worker is busy opening/closing the file, skip this frame
			if (sceKernelTryLockMutex(rec_mutex, 1) >= 0) {
				if (is_recording && writer_ready)
					writeFrame(pParam->base, pParam->pitch, pParam->width, pParam->height, pParam->pixelformat, now);
				sceKernelUnlockMutex(rec_mutex, 1);
			} else frames_dropped++;
		} else skip_counter++;
	}

	updateFramebuf(pParam);
	SceCtrlData pad;
	sceClibMemset(&pad, 0, sizeof(pad));
	sceCtrlPeekBufferPositive(0, &pad, 1);
	checkInput(&pad);

	if (status == CONFIG_MENU) {
		drawConfigMenu(pParam->width, pParam->height);
	} else {
		if (is_recording && cfg.indicator) {
			setTextColor(COLOR_RED);
			drawString(5, 5, "R");
		}
		if (message_time && now - message_time < ERROR_SHOW_US) {
			setTextColor(message_color);
			drawString(5, (is_recording && cfg.indicator) ? 25 : 5, message);
		}
	}

	if (is_recording || writer_ready) sceKernelSignalSema(wake_sema, 1);
	return TAI_CONTINUE(int, ref[0], pParam, sync);
}

static int genericInputDisable(int idx, int port, SceCtrlData *ctrl, int count, int is_negative) {
	int ret = TAI_CONTINUE(int, ref[idx], port, ctrl, count);
	if (status == CONFIG_MENU && ctrl && ret > 0) {
		// Block every returned sample (the old code only cleared the first one) and the sticks too
		for (int i = 0; i < ret; i++) {
			ctrl[i].buttons = is_negative ? 0xFFFFFFFF : 0;
			ctrl[i].lx = ctrl[i].ly = ctrl[i].rx = ctrl[i].ry = 128;
		}
	}
	return ret;
}

#define INPUT_HOOK(name, idx, neg) \
	static int name##_patched(int port, SceCtrlData *ctrl, int count) { \
		return genericInputDisable(idx, port, ctrl, count, neg); \
	}

INPUT_HOOK(sceCtrlPeekBufferPositive, 1, 0)
INPUT_HOOK(sceCtrlPeekBufferPositive2, 2, 0)
INPUT_HOOK(sceCtrlReadBufferPositive, 3, 0)
INPUT_HOOK(sceCtrlReadBufferPositive2, 4, 0)
INPUT_HOOK(sceCtrlPeekBufferPositiveExt, 5, 0)
INPUT_HOOK(sceCtrlPeekBufferPositiveExt2, 6, 0)
INPUT_HOOK(sceCtrlReadBufferPositiveExt, 7, 0)
INPUT_HOOK(sceCtrlReadBufferPositiveExt2, 8, 0)
INPUT_HOOK(sceCtrlPeekBufferNegative, 9, 1)
INPUT_HOOK(sceCtrlPeekBufferNegative2, 10, 1)
INPUT_HOOK(sceCtrlReadBufferNegative, 11, 1)
INPUT_HOOK(sceCtrlReadBufferNegative2, 12, 1)

static void hookFunction(uint32_t nid, const void *func) {
	g_hooks[cur_hook] = taiHookFunctionImport(&ref[cur_hook], TAI_MAIN_MODULE, TAI_ANY_LIBRARY, nid, func);
	cur_hook++;
}

void _start() __attribute__((weak, alias("module_start")));
int module_start(SceSize argc, const void *args) {
	// Fixed: the old code asked for 256 bytes into this 16-byte buffer
	sceClibMemset(titleid, 0, sizeof(titleid));
	sceAppMgrAppParamGetString(0, 12, titleid, sizeof(titleid) - 1);
	if (sceClibStrncmp(titleid, "NPXS", 4) == 0) // System apps aren't supported
		return SCE_KERNEL_START_NO_RESIDENT;

	// Max clocks to give the encoder some headroom
	scePowerSetArmClockFrequency(444);
	scePowerSetBusClockFrequency(222);
	scePowerSetGpuClockFrequency(222);
	scePowerSetGpuXbarClockFrequency(166);

	loadConfig();

	wake_sema = sceKernelCreateSema("vrec_wake", 0, 0, 1, NULL);
	rec_mutex = sceKernelCreateMutex("vrec_mutex", 0, 0, NULL);
	worker_id = sceKernelCreateThread("vrec_worker", worker, 0xA0, 0x10000, 0, 0, NULL);
	if (worker_id >= 0) sceKernelStartThread(worker_id, 0, NULL);

	hookFunction(0x7A410B64, sceDisplaySetFrameBuf_patched);
	hookFunction(0xA9C3CED6, sceCtrlPeekBufferPositive_patched);
	hookFunction(0x15F81E8C, sceCtrlPeekBufferPositive2_patched);
	hookFunction(0x67E7AB83, sceCtrlReadBufferPositive_patched);
	hookFunction(0xC4226A3E, sceCtrlReadBufferPositive2_patched);
	hookFunction(0xA59454D3, sceCtrlPeekBufferPositiveExt_patched);
	hookFunction(0x860BF292, sceCtrlPeekBufferPositiveExt2_patched);
	hookFunction(0xE2D99296, sceCtrlReadBufferPositiveExt_patched);
	hookFunction(0xA7178860, sceCtrlReadBufferPositiveExt2_patched);
	hookFunction(0x104ED1A7, sceCtrlPeekBufferNegative_patched);
	hookFunction(0x81A89660, sceCtrlPeekBufferNegative2_patched);
	hookFunction(0x15F96FB0, sceCtrlReadBufferNegative_patched);
	hookFunction(0x27A0C5FB, sceCtrlReadBufferNegative2_patched);

	return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args) {
	// Release hooks first so the game stops calling into us
	for (int i = 0; i < cur_hook; i++) {
		if (g_hooks[i] >= 0) taiHookRelease(g_hooks[i], ref[i]);
	}

	// Let the worker finish (and save) any recording in progress
	is_recording = 0;
	quit = 1;
	if (worker_id >= 0) {
		SceUInt timeout = 5 * 1000 * 1000;
		sceKernelSignalSema(wake_sema, 1);
		sceKernelWaitThreadEnd(worker_id, NULL, &timeout);
		sceKernelDeleteThread(worker_id);
	}
	if (wake_sema >= 0) sceKernelDeleteSema(wake_sema);
	if (rec_mutex >= 0) sceKernelDeleteMutex(rec_mutex);
	return SCE_KERNEL_STOP_SUCCESS;
}
