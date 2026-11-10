#ifndef LAMPA_POSTERS_H
#define LAMPA_POSTERS_H

#include <GLES2/gl2.h>
#include "catalog.h"

/* Start the background image downloader after SDL and curl initialization. */
void posters_init(void);
/* Returns cached texture or zero while the background download is pending. */
GLuint poster_texture(const Movie *movie, int *network_budget);
/* Begin fetching the currently visible cards and the next few cards. */
void posters_prefetch(const Movie *movies, int count, int start, int ahead);
void posters_destroy(void);

#endif
