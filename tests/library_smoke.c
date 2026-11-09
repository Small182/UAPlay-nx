#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "library.h"

int main(void) {
    Movie film={.id=19995,.series=0};
    snprintf(film.title,sizeof(film.title),"Аватар");
    snprintf(film.original,sizeof(film.original),"Avatar");
    snprintf(film.year,sizeof(film.year),"2009");
    snprintf(film.poster,sizeof(film.poster),"/poster.jpg");
    library_init();
    assert(!library_favorite(&film));
    library_toggle_favorite(&film);
    library_set_progress(&film,123.5);
    assert(library_favorite(&film));
    assert(library_progress(&film)>123.0);
    library_init();
    assert(library_favorite(&film));
    assert(library_progress(&film)>123.0);
    Movie movies[4]={0};
    assert(library_list(movies,4,1)==1);
    assert(movies[0].id==19995);
    assert(library_list(movies,4,0)==1);
    puts("Local favorites, history and resume persistence OK");
    return 0;
}
