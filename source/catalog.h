#ifndef LAMPA_CATALOG_H
#define LAMPA_CATALOG_H

#define CATALOG_MAX 200
#define CATALOG_PAGE_SIZE 20
#define UAKINO_MAX 20
#define EPISODES_MAX 128

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

typedef struct {
    int season;
    int number;
    char title[192];
    char url[1024];
} Episode;

/* Key is loaded from sdmc:/switch/lampa-nx/tmdb.key; never bundled. */
int catalog_load(const char *category, const char *query, Movie *items, int max,
                 char *error, int error_size);
/* TMDB paginates 20 titles per page; used for asynchronous catalog expansion. */
int catalog_load_page(const char *category, const char *query, int page,
                      Movie *items, int max, char *error, int error_size);

/* UAFresh integration: current UAFlix/UASerials sources via UAFresh worker. */
int uakino_search(const Movie *movie, Source *items, int max,
                  char *error, int error_size);
int uakino_resolve(const Source *source, char *stream, int size,
                   char *error, int error_size);
/* Optional WebVTT URL announced by the last successfully resolved embed. */
const char *uakino_resolved_subtitle(void);
int uakino_episodes(const Source *source, Episode *items, int max,
                    char *error, int error_size);
/* Optional UAFlix login, as supported by the user's UAFresh worker.
   Only the session cookie is kept on the SD card; never the password. */
int uaflix_login(const char *login,const char *password,char *error,int error_size);
int uaflix_logged_in(void);
void uaflix_logout(void);

#endif
