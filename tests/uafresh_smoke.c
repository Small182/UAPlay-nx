#include <stdio.h>
#include <string.h>
#include "catalog.h"

/* Optional Linux smoke test: cc -Isource source/catalog.c tests/uafresh_smoke.c
   $(pkg-config --cflags --libs libcurl) -o /tmp/uafresh-smoke */
int main(void) {
    Movie film = { .id = 19995, .series = 0 };
    snprintf(film.title, sizeof(film.title), "Аватар");
    snprintf(film.original, sizeof(film.original), "Avatar");
    snprintf(film.year, sizeof(film.year), "2009");
    Source pages[UAKINO_MAX] = {0};
    char error[256] = {0};
    int count = uakino_search(&film,pages,UAKINO_MAX,error,sizeof(error));
    fprintf(stderr,"UAFresh returned %d pages: %s\n",count,error);
    if (count < 1) return 1;
    int verified = 0;
    for (int i=0; i<count; i++) {
        if (!strstr(pages[i].url,"uafix.net/films/avatar-1/")) continue;
        char stream[2048] = {0};
        if (uakino_resolve(&pages[i],stream,sizeof(stream),error,sizeof(error)) &&
            strstr(stream,".m3u8")) {
            printf("UAFresh HLS resolution OK: %s\n",stream);
            verified = 1;
        } else fprintf(stderr,"Resolve: %s\n",error);
    }
    return verified ? 0 : 2;
}
