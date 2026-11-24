// Lets mp4.c compile on a normal Linux PC for testing (maps Vita I/O calls to POSIX).
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#define SCE_O_RDONLY O_RDONLY
#define SCE_O_WRONLY O_WRONLY
#define SCE_O_RDWR   O_RDWR
#define SCE_O_CREAT  O_CREAT
#define SCE_O_TRUNC  O_TRUNC
#define SCE_SEEK_SET SEEK_SET
#define SCE_SEEK_END SEEK_END
#define sceIoOpen(p, f, m) open(p, f, m)
#define sceIoWrite write
#define sceIoRead read
#define sceIoLseek(fd, o, w) lseek(fd, o, w)
#define sceIoClose close
#define sceIoRemove unlink
#define mp4_memcpy memcpy
#define mp4_snprintf snprintf
