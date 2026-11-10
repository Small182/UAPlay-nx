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
    snprintf(film.overview,sizeof(film.overview),"Український тестовий опис");
    film.rating=7.8f;
    film.popularity=98.2f;
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
    assert(movies[0].rating>7.79f && movies[0].rating<7.81f);
    assert(movies[0].popularity>98.1f && movies[0].popularity<98.3f);
    assert(!strcmp(movies[0].overview,"Український тестовий опис"));
    assert(library_list(movies,4,0)==1);
    assert(library_list_to_watch(movies,4)==0);
    Movie later={.id=19996,.series=0};
    snprintf(later.title,sizeof(later.title),"Фільм на майбутнє");
    library_toggle_favorite(&later);
    assert(library_list_to_watch(movies,4)==1);
    assert(movies[0].id==19996);
    assert(library_list(movies,4,0)==1);
    library_init();
    assert(library_list_to_watch(movies,4)==1);
    assert(movies[0].id==19996);
    library_mark_watched(&later);
    assert(library_list_to_watch(movies,4)==0);
    assert(library_list(movies,4,0)==2);
    Movie history_only={.id=19997,.series=0};
    snprintf(history_only.title,sizeof(history_only.title),"Перегляд без вибраного");
    library_mark_watched(&history_only);
    assert(library_list(movies,4,0)==3);
    assert(library_clear_history()==3);
    assert(library_list(movies,4,0)==0);
    assert(library_list_to_watch(movies,4)==2);
    assert(library_progress(&film)==0);
    library_init();
    assert(library_list(movies,4,0)==0);
    assert(library_list_to_watch(movies,4)==2);
    puts("Local favorites, to-watch, history and resume persistence OK");
    return 0;
}
