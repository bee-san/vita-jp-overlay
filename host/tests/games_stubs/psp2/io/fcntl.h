#ifndef VJO_GAMES_TEST_IO_H
#define VJO_GAMES_TEST_IO_H
#include <stddef.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
typedef int SceUID;
typedef size_t SceSize;
typedef off_t SceOff;
typedef struct { mode_t st_mode; } SceIoStat;
#define SCE_O_RDONLY O_RDONLY
#define SCE_O_WRONLY O_WRONLY
#define SCE_O_CREAT O_CREAT
#define SCE_O_TRUNC O_TRUNC
#define SCE_SEEK_END SEEK_END
#define SCE_SEEK_SET SEEK_SET
int sceIoOpen(const char *, int, unsigned);
int sceIoClose(int);
int sceIoRead(int, void *, size_t);
int sceIoWrite(int, const void *, size_t);
SceOff sceIoLseek(int, SceOff, int);
int sceIoMkdir(const char *, unsigned);
int sceIoGetstat(const char *, SceIoStat *);
int sceIoRename(const char *, const char *);
int sceIoRemove(const char *);
int sceIoSync(const char *, unsigned);
#endif
