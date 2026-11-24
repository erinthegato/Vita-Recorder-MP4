#ifdef MP4_HOST_TEST
#include "tests/host_shim.h"
#else
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#define mp4_memcpy sceClibMemcpy
#define mp4_snprintf sceClibSnprintf
#endif

#include "mp4.h"

#define IDX_MAGIC    0x58495256 // "VRIX"
#define IDX_VERSION  1
#define IDX_HDR_SIZE 16
#define CHUNK_FRAMES 32   // Frames grouped per MP4 chunk
#define FTYP_SIZE    28
#define MDAT_HDR     16   // 64-bit mdat header: lets recordings grow past 4GB

// ----- Small buffered big-endian writer used to build the moov header -----

typedef struct {
	int fd;
	uint64_t base; // File offset of buf[0]
	uint32_t len;
	int err;
	uint8_t buf[4096];
} bw_t;

static void bw_flush(bw_t *b) {
	if (b->len) {
		if (sceIoWrite(b->fd, b->buf, b->len) != (int)b->len) b->err = 1;
		b->base += b->len;
		b->len = 0;
	}
}

static inline uint64_t bw_tell(bw_t *b) { return b->base + b->len; }

static void bw_bytes(bw_t *b, const void *data, uint32_t n) {
	const uint8_t *p = (const uint8_t *)data;
	while (n) {
		uint32_t room = sizeof(b->buf) - b->len;
		uint32_t c = n < room ? n : room;
		mp4_memcpy(b->buf + b->len, p, c);
		b->len += c; p += c; n -= c;
		if (b->len == sizeof(b->buf)) bw_flush(b);
	}
}

static void bw_u8(bw_t *b, uint8_t v) { bw_bytes(b, &v, 1); }
static void bw_u16(bw_t *b, uint16_t v) { uint8_t t[2] = {v >> 8, v}; bw_bytes(b, t, 2); }
static void bw_u32(bw_t *b, uint32_t v) { uint8_t t[4] = {v >> 24, v >> 16, v >> 8, v}; bw_bytes(b, t, 4); }
static void bw_u64(bw_t *b, uint64_t v) { bw_u32(b, v >> 32); bw_u32(b, (uint32_t)v); }
static void bw_zero(bw_t *b, uint32_t n) { while (n--) bw_u8(b, 0); }
static void bw_fourcc(bw_t *b, const char *s) { bw_bytes(b, s, 4); }

static uint64_t box_begin(bw_t *b, const char *type) {
	uint64_t off = bw_tell(b);
	bw_u32(b, 0); // Size, patched by box_end
	bw_fourcc(b, type);
	return off;
}

static uint64_t full_box_begin(bw_t *b, const char *type, uint8_t version, uint32_t flags) {
	uint64_t off = box_begin(b, type);
	bw_u32(b, ((uint32_t)version << 24) | (flags & 0xFFFFFF));
	return off;
}

static void box_end(bw_t *b, uint64_t off) {
	uint32_t size = (uint32_t)(bw_tell(b) - off);
	uint8_t t[4] = {size >> 24, size >> 16, size >> 8, size};
	if (off >= b->base) { // Still in the buffer
		mp4_memcpy(b->buf + (off - b->base), t, 4);
	} else { // Already on disk: patch it in place
		bw_flush(b);
		sceIoLseek(b->fd, off, SCE_SEEK_SET);
		if (sceIoWrite(b->fd, t, 4) != 4) b->err = 1;
		sceIoLseek(b->fd, b->base, SCE_SEEK_SET);
	}
}

static void put_matrix(bw_t *b) {
	static const uint32_t m[9] = {0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000};
	for (int i = 0; i < 9; i++) bw_u32(b, m[i]);
}

// ----- Index (sidecar) reader -----

typedef struct {
	int fd;
	uint32_t pos, len;
	uint32_t buf[1024];
} idx_reader;

static void idx_rewind(idx_reader *r) {
	sceIoLseek(r->fd, IDX_HDR_SIZE, SCE_SEEK_SET);
	r->pos = r->len = 0;
}

static int idx_next(idx_reader *r, uint32_t *size, uint32_t *pts) {
	if (r->pos + 2 > r->len) {
		int n = sceIoRead(r->fd, r->buf, sizeof(r->buf));
		if (n < 8) return 0;
		r->len = n / 4;
		r->pos = 0;
	}
	*size = r->buf[r->pos++];
	*pts = r->buf[r->pos++];
	return 1;
}

// ----- Header (moov) builder -----

static int finalize(int fd, int idx_fd) {
	uint32_t hdr[4];
	sceIoLseek(idx_fd, 0, SCE_SEEK_SET);
	if (sceIoRead(idx_fd, hdr, sizeof(hdr)) != sizeof(hdr) || hdr[0] != IDX_MAGIC || hdr[1] != IDX_VERSION)
		return -1;
	uint16_t width = hdr[2] >> 16, height = hdr[2] & 0xFFFF;
	uint32_t data_off = hdr[3];

	static idx_reader r; // Static: too big for the stack. Only one finalize runs at a time.
	r.fd = idx_fd;

	// Pass 1: count frames, total data size, total duration
	uint32_t n = 0, size, pts, first = 0, prev = 0, last_delta = MP4_TIMESCALE / 30;
	uint64_t data_size = 0;
	idx_rewind(&r);
	while (idx_next(&r, &size, &pts)) {
		if (n == 0) first = pts;
		else last_delta = pts - prev;
		prev = pts;
		data_size += size;
		n++;
	}
	if (n == 0) return -2; // Nothing recorded
	uint64_t duration = (uint64_t)(prev - first) + last_delta;
	uint32_t duration_ms = (uint32_t)(duration * 1000 / MP4_TIMESCALE);

	// Patch the mdat size
	uint8_t big[8];
	uint64_t mdat_size = MDAT_HDR + data_size;
	for (int i = 0; i < 8; i++) big[i] = (uint8_t)(mdat_size >> (56 - i * 8));
	sceIoLseek(fd, data_off - MDAT_HDR + 8, SCE_SEEK_SET);
	if (sceIoWrite(fd, big, 8) != 8) return -3;

	// moov goes right after the last indexed frame
	uint64_t moov_off = data_off + data_size;
	uint64_t file_end = (uint64_t)sceIoLseek(fd, 0, SCE_SEEK_END);
	sceIoLseek(fd, moov_off, SCE_SEEK_SET);

	static bw_t b;
	b.fd = fd; b.base = moov_off; b.len = 0; b.err = 0;

	uint64_t moov = box_begin(&b, "moov");

	uint64_t mvhd = full_box_begin(&b, "mvhd", 0, 0);
	bw_u32(&b, 0); bw_u32(&b, 0);          // creation / modification time
	bw_u32(&b, 1000);                      // movie timescale (ms)
	bw_u32(&b, duration_ms);
	bw_u32(&b, 0x00010000);                // rate 1.0
	bw_u16(&b, 0x0100);                    // volume 1.0
	bw_zero(&b, 10);
	put_matrix(&b);
	bw_zero(&b, 24);
	bw_u32(&b, 2);                         // next track id
	box_end(&b, mvhd);

	uint64_t trak = box_begin(&b, "trak");

	uint64_t tkhd = full_box_begin(&b, "tkhd", 0, 3); // enabled | in movie
	bw_u32(&b, 0); bw_u32(&b, 0);
	bw_u32(&b, 1);                         // track id
	bw_u32(&b, 0);
	bw_u32(&b, duration_ms);
	bw_zero(&b, 8);
	bw_u16(&b, 0); bw_u16(&b, 0);          // layer, alternate group
	bw_u16(&b, 0); bw_u16(&b, 0);          // volume, reserved
	put_matrix(&b);
	bw_u32(&b, (uint32_t)width << 16);
	bw_u32(&b, (uint32_t)height << 16);
	box_end(&b, tkhd);

	uint64_t mdia = box_begin(&b, "mdia");

	uint64_t mdhd = full_box_begin(&b, "mdhd", 0, 0);
	bw_u32(&b, 0); bw_u32(&b, 0);
	bw_u32(&b, MP4_TIMESCALE);
	bw_u32(&b, (uint32_t)duration);
	bw_u16(&b, 0x55C4);                    // language "und"
	bw_u16(&b, 0);
	box_end(&b, mdhd);

	uint64_t hdlr = full_box_begin(&b, "hdlr", 0, 0);
	bw_u32(&b, 0);
	bw_fourcc(&b, "vide");
	bw_zero(&b, 12);
	bw_bytes(&b, "VideoHandler", 13);
	box_end(&b, hdlr);

	uint64_t minf = box_begin(&b, "minf");

	uint64_t vmhd = full_box_begin(&b, "vmhd", 0, 1);
	bw_zero(&b, 8);
	box_end(&b, vmhd);

	uint64_t dinf = box_begin(&b, "dinf");
	uint64_t dref = full_box_begin(&b, "dref", 0, 0);
	bw_u32(&b, 1);
	uint64_t url = full_box_begin(&b, "url ", 0, 1); // data is in this file
	box_end(&b, url);
	box_end(&b, dref);
	box_end(&b, dinf);

	uint64_t stbl = box_begin(&b, "stbl");

	// Sample description: MJPEG ("mp4v" + esds with object type 0x6C, same as ffmpeg writes)
	uint64_t stsd = full_box_begin(&b, "stsd", 0, 0);
	bw_u32(&b, 1);
	uint64_t mp4v = box_begin(&b, "mp4v");
	bw_zero(&b, 6);
	bw_u16(&b, 1);                         // data reference index
	bw_zero(&b, 16);
	bw_u16(&b, width);
	bw_u16(&b, height);
	bw_u32(&b, 0x00480000);                // 72 dpi
	bw_u32(&b, 0x00480000);
	bw_u32(&b, 0);
	bw_u16(&b, 1);                         // frames per sample
	bw_u8(&b, 12);                         // compressor name (pascal string, 32 bytes)
	bw_bytes(&b, "VitaRecorder", 12);
	bw_zero(&b, 31 - 12);
	bw_u16(&b, 0x0018);                    // depth
	bw_u16(&b, 0xFFFF);
	uint64_t avg_bitrate = duration ? data_size * 8 * MP4_TIMESCALE / duration : 0;
	uint64_t esds = full_box_begin(&b, "esds", 0, 0);
	bw_u8(&b, 0x03); bw_u8(&b, 21);        // ES descriptor
	bw_u16(&b, 1); bw_u8(&b, 0);
	bw_u8(&b, 0x04); bw_u8(&b, 13);        // Decoder config descriptor
	bw_u8(&b, 0x6C);                       // JPEG
	bw_u8(&b, 0x11);                       // visual stream
	bw_u8(&b, 0); bw_u16(&b, 0);           // buffer size
	bw_u32(&b, (uint32_t)(avg_bitrate > 0xFFFFFFFF ? 0xFFFFFFFF : avg_bitrate)); // max bitrate
	bw_u32(&b, (uint32_t)(avg_bitrate > 0xFFFFFFFF ? 0xFFFFFFFF : avg_bitrate)); // avg bitrate
	bw_u8(&b, 0x06); bw_u8(&b, 1); bw_u8(&b, 0x02); // SL config
	box_end(&b, esds);
	box_end(&b, mp4v);
	box_end(&b, stsd);

	// Frame durations (run-length encoded). Real timestamps = correct playback speed.
	uint64_t stts = full_box_begin(&b, "stts", 0, 0);
	uint64_t stts_count_off = bw_tell(&b);
	bw_u32(&b, 0);
	uint32_t entries = 0, run = 0, run_delta = 0;
	idx_rewind(&r);
	idx_next(&r, &size, &prev);
	for (uint32_t i = 1; i <= n; i++) {
		uint32_t delta;
		if (i < n) {
			idx_next(&r, &size, &pts);
			delta = pts - prev;
			prev = pts;
		} else delta = last_delta;
		if (run && delta == run_delta) run++;
		else {
			if (run) { bw_u32(&b, run); bw_u32(&b, run_delta); entries++; }
			run = 1; run_delta = delta;
		}
	}
	bw_u32(&b, run); bw_u32(&b, run_delta); entries++;
	{ // Patch entry count (box_end trick: write it like a box size at that offset)
		uint64_t save = bw_tell(&b);
		uint8_t t[4] = {entries >> 24, entries >> 16, entries >> 8, entries};
		if (stts_count_off >= b.base) mp4_memcpy(b.buf + (stts_count_off - b.base), t, 4);
		else {
			bw_flush(&b);
			sceIoLseek(fd, stts_count_off, SCE_SEEK_SET);
			if (sceIoWrite(fd, t, 4) != 4) b.err = 1;
			sceIoLseek(fd, save, SCE_SEEK_SET);
		}
	}
	box_end(&b, stts);

	// Chunks: CHUNK_FRAMES frames each, last one may be shorter
	uint32_t chunks = (n + CHUNK_FRAMES - 1) / CHUNK_FRAMES;
	uint32_t rem = n % CHUNK_FRAMES;
	uint64_t stsc = full_box_begin(&b, "stsc", 0, 0);
	if (chunks > 1 && rem) {
		bw_u32(&b, 2);
		bw_u32(&b, 1); bw_u32(&b, CHUNK_FRAMES); bw_u32(&b, 1);
		bw_u32(&b, chunks); bw_u32(&b, rem); bw_u32(&b, 1);
	} else {
		bw_u32(&b, 1);
		bw_u32(&b, 1); bw_u32(&b, chunks > 1 ? CHUNK_FRAMES : n); bw_u32(&b, 1);
	}
	box_end(&b, stsc);

	uint64_t stsz = full_box_begin(&b, "stsz", 0, 0);
	bw_u32(&b, 0);
	bw_u32(&b, n);
	idx_rewind(&r);
	while (idx_next(&r, &size, &pts)) bw_u32(&b, size);
	box_end(&b, stsz);

	uint64_t co64 = full_box_begin(&b, "co64", 0, 0);
	bw_u32(&b, chunks);
	uint64_t off = data_off;
	uint32_t i = 0;
	idx_rewind(&r);
	while (idx_next(&r, &size, &pts)) {
		if (i % CHUNK_FRAMES == 0) bw_u64(&b, off);
		off += size;
		i++;
	}
	box_end(&b, co64);

	box_end(&b, stbl);
	box_end(&b, minf);
	box_end(&b, mdia);
	box_end(&b, trak);
	box_end(&b, moov);

	// Anything left after moov (e.g. a frame cut short by a crash) gets wrapped in a "free" box
	uint64_t end = bw_tell(&b);
	if (file_end > end && file_end - end >= 8 && file_end - end <= 0xFFFFFFFF) {
		bw_u32(&b, (uint32_t)(file_end - end));
		bw_fourcc(&b, "free");
	}
	bw_flush(&b);
	return b.err ? -4 : 0;
}

// ----- Public API -----

static int idx_flush(mp4_writer *w) {
	if (!w->idx_count) return 0;
	int bytes = w->idx_count * 8;
	w->idx_count = 0;
	return sceIoWrite(w->idx_fd, w->idx_buf, bytes) == bytes ? 0 : -1;
}

int mp4Open(mp4_writer *w, const char *path, uint16_t width, uint16_t height) {
	w->frames = 0;
	w->data_size = 0;
	w->idx_count = 0;
	mp4_snprintf(w->path, sizeof(w->path), "%s", path);
	mp4_snprintf(w->idx_path, sizeof(w->idx_path), "%s.idx", path);

	w->fd = sceIoOpen(w->path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
	if (w->fd < 0) return w->fd;
	w->idx_fd = sceIoOpen(w->idx_path, SCE_O_RDWR | SCE_O_CREAT | SCE_O_TRUNC, 0777);
	if (w->idx_fd < 0) {
		int err = w->idx_fd;
		sceIoClose(w->fd);
		sceIoRemove(w->path);
		return err;
	}

	static const uint8_t head[FTYP_SIZE + MDAT_HDR] = {
		0, 0, 0, FTYP_SIZE, 'f', 't', 'y', 'p', 'i', 's', 'o', 'm', 0, 0, 2, 0,
		'i', 's', 'o', 'm', 'i', 's', 'o', '2', 'm', 'p', '4', '1',
		0, 0, 0, 1, 'm', 'd', 'a', 't', 0, 0, 0, 0, 0, 0, 0, MDAT_HDR // size=1 -> 64-bit size follows
	};
	uint32_t idx_hdr[4] = {IDX_MAGIC, IDX_VERSION, ((uint32_t)width << 16) | height, sizeof(head)};
	if (sceIoWrite(w->fd, head, sizeof(head)) != sizeof(head) ||
		sceIoWrite(w->idx_fd, idx_hdr, sizeof(idx_hdr)) != sizeof(idx_hdr)) {
		sceIoClose(w->fd);
		sceIoClose(w->idx_fd);
		sceIoRemove(w->path);
		sceIoRemove(w->idx_path);
		return -1;
	}
	return 0;
}

int mp4WriteFrame(mp4_writer *w, const void *jpeg, uint32_t size, uint64_t time_us) {
	if (w->frames == 0) w->start_us = time_us;
	uint32_t pts = (uint32_t)((time_us - w->start_us) * 9 / 100); // us -> 90kHz
	if (w->frames && pts <= w->last_pts) pts = w->last_pts + 1; // Keep timestamps strictly increasing
	w->last_pts = pts;

	if (sceIoWrite(w->fd, jpeg, size) != (int)size) return -1;
	w->idx_buf[w->idx_count * 2] = size;
	w->idx_buf[w->idx_count * 2 + 1] = pts;
	w->idx_count++;
	w->frames++;
	w->data_size += size;
	if (w->idx_count == MP4_IDX_BATCH) return idx_flush(w);
	return 0;
}

int mp4Close(mp4_writer *w) {
	int ret = idx_flush(w);
	if (ret == 0) ret = finalize(w->fd, w->idx_fd);
	sceIoClose(w->fd);
	sceIoClose(w->idx_fd);
	if (ret == -2) sceIoRemove(w->path); // Empty recording: don't leave a broken file around
	if (ret == 0 || ret == -2) sceIoRemove(w->idx_path);
	return ret;
}

int mp4Recover(const char *mp4_path) {
	char idx_path[200];
	mp4_snprintf(idx_path, sizeof(idx_path), "%s.idx", mp4_path);
	int idx_fd = sceIoOpen(idx_path, SCE_O_RDONLY, 0);
	if (idx_fd < 0) return idx_fd;
	int fd = sceIoOpen(mp4_path, SCE_O_WRONLY, 0);
	if (fd < 0) {
		sceIoClose(idx_fd);
		sceIoRemove(idx_path); // Video is gone, sidecar is useless
		return fd;
	}
	int ret = finalize(fd, idx_fd);
	sceIoClose(fd);
	sceIoClose(idx_fd);
	if (ret == -2) sceIoRemove(mp4_path);
	if (ret == 0 || ret == -2 || ret == -1) sceIoRemove(idx_path);
	return ret;
}
