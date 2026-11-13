#ifndef UAPLAY_SUBDL_H
#define UAPLAY_SUBDL_H

#include "catalog.h"

#define SUBDL_MAX 24
#define SUBDL_FILE_SIZE 256

typedef struct {
    char release[176];
    char resource[144];
    char language[3];
    char format[4];
    int hearing_impaired;
} SubdlSubtitle;

int subdl_search(const Movie *movie, int season, int episode, SubdlSubtitle *out,
                 char *error, int error_size);
int subdl_download(const SubdlSubtitle *subtitle, char *path, int path_size,
                   char *error, int error_size);

#endif
