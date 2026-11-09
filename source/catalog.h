#ifndef LAMPA_CATALOG_H
#define LAMPA_CATALOG_H

#define CATALOG_MAX 40
#define UAKINO_MAX 20

typedef struct {
    int id;
    int series;
    char title[192];
    char original[192];
    char year[12];
    char overview[1024];
    char poster[256];
} Movie;

typedef struct {
    char title[192];
    char url[1024];
} Source;

/* Key is loaded from sdmc:/switch/lampa-nx/tmdb.key; never bundled. */
int catalog_load(const char *category, const char *query, Movie *items, int max,
                 char *error, int error_size);

/* UAFresh integration: uses its worker proxy and UAKino search pages. */
int uakino_search(const Movie *movie, Source *items, int max,
                  char *error, int error_size);
int uakino_resolve(const Source *source, char *stream, int size,
                   char *error, int error_size);

#endif
