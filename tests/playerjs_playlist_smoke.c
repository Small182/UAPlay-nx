#include <stdio.h>
#include <string.h>

/* Host-only unit test for the embedded playlist parser, without network. */
#include "../source/catalog.c"

static Episode examples[EPISODES_MAX];

int main(void){
    const char *zetvideo=
        "<script>new Playerjs({file:'["
        "{\"title\":\"Голос A\",\"folder\":["
          "{\"title\":\"Сезон 1\",\"folder\":["
            "{\"title\":\"Серія 1\",\"file\":\"https://zetvideo.net/media/s1e1.m3u8\"},"
            "{\"title\":\"Серія 2\",\"file\":\"https://zetvideo.net/media/s1e2.m3u8\"}]},"
          "{\"title\":\"Сезон 2\",\"folder\":["
            "{\"title\":\"Серія 1\",\"file\":\"https://zetvideo.net/media/s2e1.m3u8\"}]}]},"
        "{\"title\":\"Голос B\",\"folder\":["
          "{\"title\":\"Сезон 1\",\"folder\":["
            "{\"title\":\"Серія 1\",\"file\":\"https://ashdi.vip/content/s1e1.mp4\"}]}]}]'});</script>";
    int count=playlist_from_html(zetvideo,examples,EPISODES_MAX);
    if(count!=4){fprintf(stderr,"Expected four stream variants, got %d\n",count);return 1;}
    if(examples[0].season!=1 || examples[0].number!=1 ||
       strcmp(examples[0].voice,"Голос A") ||
       examples[2].season!=2 || examples[2].number!=1 ||
       strcmp(examples[3].voice,"Голос B")){
        fprintf(stderr,"Wrong parsed season/episode/voice\n");return 2;
    }
    memset(examples,0,sizeof(examples));
    const char *escaped="<script>file:\"[{\\\"title\\\":\\\"Voice\\\","
        "\\\"folder\\\":[{\\\"title\\\":\\\"Season 5\\\","
        "\\\"folder\\\":[{\\\"title\\\":\\\"Episode 7\\\","
        "\\\"file\\\":\\\"https://ashdi.vip/e7.m3u8\\\"}]}]}]\"</script>";
    count=playlist_from_html(escaped,examples,EPISODES_MAX);
    if(count!=1 || examples[0].season!=5 || examples[0].number!=7){
        fprintf(stderr,"Escaped playlist failed: count=%d S%dE%d\n",
                count,examples[0].season,examples[0].number);return 3;
    }
    memset(examples,0,sizeof(examples));
    char seasons[8][1024]={0};int season_count=0;
    const char *uaflix_root="https://uafix.net/serials/example/";
    const char *home=
        "<a href=\"/serials/example/sezon-1/\">Season 1</a>"
        "<a href=\"https://uafix.net/serials/example/sezon-2/\">Season 2</a>"
        "<a href=\"/serials/example/season-02-episode-03/\">Episode</a>"
        "<a href=\"https://uafix.net/serials/other/season-01-episode-01/\">Other</a>";
    count=0;
    parse_uaflix_episodes(home,uaflix_root,examples,&count,EPISODES_MAX,
                          seasons,&season_count,8);
    if(count!=1 || season_count!=2 || examples[0].season!=2 ||
       strcmp(examples[0].url,"https://uafix.net/serials/example/season-02-episode-03/")){
        fprintf(stderr,"UAFlix season link discovery failed: episodes=%d seasons=%d\n",
                count,season_count);return 4;
    }
    const char *archive=
        "<a href=\"/serials/example/season-01-episode-01/\">E1</a>"
        "<a href=\"https://uafix.net/serials/example/season-01-episode-02/\">E2</a>"
        "<a href=\"/serials/example/season-02-episode-03/\">Duplicate</a>";
    parse_uaflix_episodes(archive,uaflix_root,examples,&count,EPISODES_MAX,
                          NULL,NULL,0);
    if(count!=3 || examples[1].season!=1 || examples[2].number!=2){
        fprintf(stderr,"UAFlix archive episode collection failed: %d\n",count);return 5;
    }
    puts("Playerjs serial playlist parser: OK");
    return 0;
}
