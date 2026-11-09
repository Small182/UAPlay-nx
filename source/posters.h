#ifndef LAMPA_POSTERS_H
#define LAMPA_POSTERS_H

#include <GLES2/gl2.h>
#include "catalog.h"

/* Returns a cached GL texture. One network attempt per frame is allowed. */
GLuint poster_texture(const Movie *movie, int *network_budget);
void posters_destroy(void);

#endif
