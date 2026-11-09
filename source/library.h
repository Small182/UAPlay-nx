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
int library_list(Movie *items, int capacity, int favorites);

#endif
