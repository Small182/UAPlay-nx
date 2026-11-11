#include "../source/catalog.c"
#include <assert.h>
#include <stdio.h>

int main(void){
    Movie movie={0};
    snprintf(movie.title,sizeof(movie.title),"Аватар");
    movie.series=1;
    Source results[40]={0};
    char error[256]={0};
    catalog_http_init();
    int count=uakino_search_provider(&movie,results,40,5,error,sizeof(error));
    printf("UAKino search: %d results; %s; %s\n",count,
           count?results[0].title:"none",error);
    catalog_http_cleanup();
    assert(count>0);
    assert(strstr(results[0].url,"uakino.eu/"));
    return 0;
}
