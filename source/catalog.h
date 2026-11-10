#ifndef LAMPA_CATALOG_H
#define LAMPA_CATALOG_H

#define CATALOG_MAX 200
#define CATALOG_LOCAL_MAX 256
#define CATALOG_PAGE_SIZE 20
/* TMDB exposes at most 500 result pages; search expands only while browsing. */
#define CATALOG_SEARCH_MAX (500 * CATALOG_PAGE_SIZE)
#define UAKINO_MAX 20
#define EPISODES_MAX 2048

typedef struct {
    int id;
    int series;
    char title[192];
    char original[192];
    char year[12];
    char overview[1024];
    char poster[256];
    float rating;
    float popularity;
    /* Loaded on demand from the TMDB movie/TV details endpoint. */
    int runtime_minutes;
    int season_count;
    int episode_count;
    int details_state; /* 0 pending, 1 loaded, -1 unavailable */
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
    char voice[96];
} Episode;

/* Keep two independent HTTPS connection pools: UI and background TMDB loader.
   Call cleanup only after the loader thread has stopped. */
void catalog_http_init(void);
void catalog_http_cleanup(void);

/* NRO includes a public TMDB key; optional SD override under switch/uaplay. */
int catalog_load(const char *category, const char *query, Movie *items, int max,
                 char *error, int error_size);
/* TMDB paginates 20 titles per page; used for asynchronous catalog expansion. */
int catalog_load_page(const char *category, const char *query, int page,
                      Movie *items, int max, char *error, int error_size,
                      int *has_more);
int catalog_load_page_sorted(const char *category, const char *query, int page,
                            int sort_mode, Movie *items, int max,
                            char *error, int error_size, int *has_more);
/* Exclude titles associated with Russia, using TMDB's country metadata. */
int catalog_filter_countries(Movie *items,int count);
/* Fetch runtime (movie) or total seasons/episodes (TV) for details screen. */
int catalog_fetch_details(Movie *movie);

/* UAFresh integration: current UAFlix/UASerials sources via UAFresh worker. */
int uakino_search(const Movie *movie, Source *items, int max,
                  char *error, int error_size);
/* Rank candidates by title match (Ukrainian and original titles), not by
   their position on provider search pages. preferred_provider: 0=all,
   1=UAFlix, 2=UASerials. Returns the count after removing clear mismatches. */
int catalog_rank_sources(const Movie *movie, Source *items, int count,
                         int preferred_provider);
int uakino_resolve(const Source *source, char *stream, int size,
                   char *error, int error_size);
/* Optional WebVTT URL announced by the last successfully resolved embed. */
const char *uakino_resolved_subtitle(void);
int uakino_episodes(const Source *source, Episode *items, int max,
                    char *error, int error_size);
/* Next chronologically available episode, preferring the current dub when
   the same episode exists in multiple voice variants. -1 if at the end. */
int catalog_next_episode(const Episode *items,int count,int current);
#endif
