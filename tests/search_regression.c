#include <stdio.h>
#include <string.h>
#include "catalog.h"

/* Live smoke test (optional, needs network):
   cc -O2 -Isource source/catalog.c tests/search_regression.c \
      $(pkg-config --cflags --libs libcurl) -o /tmp/uaplay-search-test */
static int check(const char *query,int series,const char *expected){
    Movie movie={0};
    movie.series=series;
    snprintf(movie.original,sizeof(movie.original),"%s",query);
    Source sources[UAKINO_MAX]={0};
    char error[256]={0};
    int count=uakino_search(&movie,sources,UAKINO_MAX,error,sizeof(error));
    if(count<1){fprintf(stderr,"%s: no sources: %s\n",query,error);return 1;}
    int matched=0;
    for(int i=0;i<count;i++){
        printf("%s -> %s\n",query,sources[i].title);
        if(strstr(sources[i].url,"/films/action/") ||
           strstr(sources[i].url,"/films/melodrama/") ||
           strstr(sources[i].url,"/serials/school/") ||
           strstr(sources[i].url,"/serials/detective/")){
            fprintf(stderr,"Category menu link leaked into result: %s\n",sources[i].url);
            return 2;
        }
        if(strstr(sources[i].url,expected))matched=1;
    }
    if(!matched){fprintf(stderr,"%s: expected title not present\n",query);return 3;}
    return 0;
}
int main(void){
    int films=check("The Odyssey",0,"/films/theodysey/");
    int serials=check("Dexter",1,"/serials/dekster-voskresinnja/");
    return films || serials ? 1 : 0;
}
