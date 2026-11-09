#include <stdio.h>
#include <string.h>
#include "catalog.h"

int main(void) {
    Source series = {0};
    snprintf(series.url,sizeof(series.url),
             "https://uafix.net/serials/avatar-ostannij-zahisnik-avatar-the-last-airbender/");
    Episode episodes[EPISODES_MAX]={0};
    char error[256]={0};
    int count=uakino_episodes(&series,episodes,EPISODES_MAX,error,sizeof(error));
    if (!count) {fprintf(stderr,"Episode discovery failed: %s\n",error);return 1;}
    int first=0,second=0;
    for (int i=0;i<count;i++) {
        if (episodes[i].season==1 && episodes[i].number==1) first=1;
        if (episodes[i].season==2 && episodes[i].number==7) second=1;
    }
    printf("Found %d episodes: season 1 episode 1=%d, season 2 episode 7=%d\n",
           count,first,second);
    return first && second ? 0 : 2;
}
