#ifndef VJO_TEST_IO_H
#define VJO_TEST_IO_H
#include <psp2/host_stubs.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
typedef off_t SceOff;
typedef struct { mode_t st_mode; } SceIoStat;
typedef struct { SceIoStat d_stat; char d_name[256]; } SceIoDirent;
#define SCE_O_RDONLY O_RDONLY
#define SCE_O_WRONLY O_WRONLY
#define SCE_O_CREAT O_CREAT
#define SCE_O_TRUNC O_TRUNC
#define SCE_SEEK_END SEEK_END
#define SCE_SEEK_SET SEEK_SET
#define SCE_S_ISDIR S_ISDIR
#define SCE_S_ISREG S_ISREG
int sceIoOpen(const char *path, int flags, unsigned mode);
int sceIoClose(int fd);
int sceIoRead(int fd, void *data, size_t len);
int sceIoWrite(int fd, const void *data, size_t len);
SceOff sceIoLseek(int fd, SceOff off, int whence);
int sceIoMkdir(const char *path, unsigned mode);
int sceIoGetstat(const char *path, SceIoStat *st);
int sceIoRename(const char *from, const char *to);
int sceIoRemove(const char *path);
int sceIoSyncByFd(int fd, int flag);
int sceIoSync(const char *device, unsigned flags);
int sceIoDopen(const char *path);
int sceIoDread(int dir, SceIoDirent *ent);
int sceIoDclose(int dir);
#endif
