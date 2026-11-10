#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "catalog.h"

/* Optional online smoke test: requires access to api.mikai.me. */
int main(void){
    Source source={0};
    snprintf(source.url,sizeof(source.url),
             "https://api.mikai.me/public/v1/anime/146/player");
    Episode episodes[32]={0};
    char error[256]={0};
    int count=uakino_episodes(&source,episodes,32,error,sizeof(error));
    if(count<1){fprintf(stderr,"Mikai: %s\n",error);return 1;}
    assert(episodes[0].number>0);
    assert(!strncmp(episodes[0].url,"https://",8));
    printf("Mikai episodes: %d; first source: %.80s\n",count,episodes[0].url);
    snprintf(source.url,sizeof(source.url),"https://animeon.club/api/player/913");
    count=uakino_episodes(&source,episodes,32,error,sizeof(error));
    if(count<1){fprintf(stderr,"AnimeON: %s\n",error);return 1;}
    assert(episodes[0].number>0);
    assert(!strncmp(episodes[0].url,"https://",8));
    printf("AnimeON episodes: %d; first source: %.80s\n",count,episodes[0].url);
    return 0;
}
