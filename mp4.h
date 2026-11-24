#ifndef _MP4_H_
#define _MP4_H_

#include <stdint.h>

/*
 * Tiny streaming MP4 muxer for MJPEG video.
 *
 * Frames go straight into the .mp4 file as they are recorded. A small
 * sidecar index file (<video>.idx) keeps each frame's size and timestamp.
 * When recording stops, the index is turned into the MP4 "moov" header and
 * the sidecar is deleted. If the game crashes mid-recording, the sidecar is
 * still there and mp4Recover() can finish the file on the next launch.
 */

#define MP4_TIMESCALE    90000 // Video track ticks per second
#define MP4_IDX_BATCH    64    // Index entries buffered before being flushed to disk

typedef struct mp4_writer {
	int fd;      // .mp4 file
	int idx_fd;  // .idx sidecar file
	char path[192];
	char idx_path[200];
	uint32_t frames;
	uint64_t data_size;   // bytes of JPEG data written so far
	uint64_t start_us;    // timestamp of the first frame
	uint32_t last_pts;
	uint32_t idx_count;   // entries waiting in idx_buf
	uint32_t idx_buf[MP4_IDX_BATCH * 2];
} mp4_writer;

// All functions return 0 on success, < 0 on error.
int mp4Open(mp4_writer *w, const char *path, uint16_t width, uint16_t height);
int mp4WriteFrame(mp4_writer *w, const void *jpeg, uint32_t size, uint64_t time_us);
int mp4Close(mp4_writer *w); // Writes the header, deletes the sidecar
int mp4Recover(const char *mp4_path); // Finishes a file left behind by a crash

#endif
