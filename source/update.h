#ifndef UAPLAY_UPDATE_H
#define UAPLAY_UPDATE_H

#include <stdatomic.h>
#include <stddef.h>

typedef struct {
    char tag[48];
    char version[24];
    char digest[65];
    size_t size;
} UpdateRelease;

#define UPDATE_NOTES_SIZE 16384
#define UPDATE_MAX_NRO_SIZE (128u * 1024u * 1024u)
#define UPDATE_RELEASES_API "https://api.github.com/repos/Small182/UAPlay-nx/releases"

int update_check(UpdateRelease *release);
int update_details(const UpdateRelease *release,char *notes,size_t size);

enum { UPDATE_OK, UPDATE_NETWORK, UPDATE_SD, UPDATE_INVALID };
int update_download(const UpdateRelease *release,const char *path,
                    atomic_uint *progress,atomic_int *cancel);
int update_install(const char *path);
int update_path(const char *arg,char *path,size_t size);

#endif
