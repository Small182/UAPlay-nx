#include "../source/catalog.c"
#include <assert.h>
#include <stdio.h>

int main(void){
    Source source={0};
    snprintf(source.url,sizeof(source.url),"https://animeon.club/api/player/1");
    Episode episodes[128]={0};
    char error[256]={0};
    catalog_http_init();
    int n=uakino_episodes(&source,episodes,128,error,sizeof(error));
    printf("AnimeON: %d episodes, first=%s (%s) %s\n",n,
           n?episodes[0].title:"none",n?episodes[0].url:"",error);
    catalog_http_cleanup();
    assert(n>0);
    assert(strstr(episodes[0].url,"/vod/"));
    return 0;
}
