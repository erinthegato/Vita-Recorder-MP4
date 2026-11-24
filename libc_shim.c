#include <stddef.h>
#include <psp2/kernel/clib.h>

/*
 * The plugin is built without a C library (-nostdlib) to stay small and avoid
 * fighting the game for memory. GCC can still emit calls to these for struct
 * copies or zero-fills, so route them to the system's built-in versions.
 */
void *memcpy(void *dst, const void *src, size_t n) { return sceClibMemcpy(dst, src, n); }
void *memmove(void *dst, const void *src, size_t n) { return sceClibMemmove(dst, src, n); }
void *memset(void *dst, int c, size_t n) { return sceClibMemset(dst, c, n); }
int memcmp(const void *a, const void *b, size_t n) { return sceClibMemcmp(a, b, n); }
