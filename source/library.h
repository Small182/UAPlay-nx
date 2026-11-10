#ifndef UAFLIX_LIBRARY_H
#define UAFLIX_LIBRARY_H

#include "catalog.h"

/* Local-only storage. No CUB, accounts or remote synchronization. */
void library_init(void);
int library_favorite(const Movie *movie);
void library_toggle_favorite(const Movie *movie);
void library_mark_watched(const Movie *movie);
void library_set_progress(const Movie *movie, double seconds);
double library_progress(const Movie *movie);
/* Persist the exact series episode (and position) so the picker can reopen
   the last watched season without confusing episode timestamps. */
void library_set_episode_progress(const Movie *movie,int season,int episode,double seconds);
int library_last_episode(const Movie *movie,int *season,int *episode,double *seconds);
int library_list(Movie *items, int capacity, int favorites);
/* Saved titles that have not been watched yet ("На майбутнє"). */
int library_list_to_watch(Movie *items, int capacity);
/* Clear watched flags and playback progress without removing saved favorites. */
int library_clear_history(void);

#endif
