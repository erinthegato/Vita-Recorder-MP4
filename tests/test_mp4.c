// Build & run on a PC:  gcc -DMP4_HOST_TEST -Itests -o test_mp4 tests/test_mp4.c mp4.c
// Usage: ./test_mp4 <folder with f001.jpg..f060.jpg> <frame count> <out.mp4> [crash]
// Host test: muxes JPEG files into an MP4 at uneven frame times, and simulates a crash.
#include <stdlib.h>
#include "host_shim.h"
#include "../mp4.h"
static unsigned char *load(const char *p, unsigned *n) {
	FILE *f = fopen(p, "rb"); fseek(f, 0, SEEK_END); *n = ftell(f); rewind(f);
	unsigned char *b = malloc(*n); fread(b, 1, *n, f); fclose(f); return b;
}
int main(int argc, char **argv) {
	int frames = atoi(argv[2]), crash = argc > 4;
	static mp4_writer w;
	if (mp4Open(&w, argv[3], 480, 272)) return 1;
	uint64_t t = 1000000;
	for (int i = 0; i < frames; i++) {
		char p[256]; unsigned n; sprintf(p, "%s/f%03d.jpg", argv[1], i % 60 + 1);
		unsigned char *b = load(p, &n);
		if (mp4WriteFrame(&w, b, n, t)) return 2;
		free(b);
		t += (i % 3 == 0) ? 33000 : 34000; // ~30fps, uneven
	}
	if (crash) { // Simulate crash: flush index partly, write half a frame, then "die"
		write(w.fd, "garbagegarbage", 14);
		return mp4Recover(argv[3]) == 0 ? 0 : 3; // fds leak, like a crash
	}
	return mp4Close(&w);
}
