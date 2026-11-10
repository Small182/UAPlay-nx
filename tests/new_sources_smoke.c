#include "../source/catalog.c"
#include <assert.h>
#include <stdio.h>

/* Offline fixtures reflect the search-card classes of each provider; this
   test needs no network or external scraper and runs on the CI host. */
int main(void){
    Source items[12]={0};
    int count=0;
    const char *eneyida=
        "<nav><a href=\"/films/\">Фільми</a></nav>"
        "<article><a class=\"short_img\" href=\"/10197-na-sviati-revolucii.html\">"
        "<img></a><a class=\"short_title\" href=\"/10197-na-sviati-revolucii.html\">"
        "На святі Революції</a></article>";
    parse_post_cards(eneyida,"https://eneyida.tv","Eneyida",2,0,
                     items,&count,12);
    assert(count==1);
    assert(strstr(items[0].title,"На святі Революції"));
    assert(!strcmp(items[0].url,"https://eneyida.tv/10197-na-sviati-revolucii.html"));

    const char *klon=
        "<div class=\"short-news__slide-item\"><a class=\"card-link__style\" "
        "href=\"/filmy/24817-nadija.html\"><span class=\"card-link__text\">"
        "Надія</span></a></div>"
        "<div class=\"short-news__small\"><a href=\"/serialy/50-test.html\">"
        "<span class=\"card-link__text\">Тестовий серіал</span></a></div>";
    parse_post_cards(klon,"https://klonua.com","UAFilm",3,0,items,&count,12);
    assert(count==2);
    assert(strstr(items[1].title,"Надія"));
    parse_post_cards(klon,"https://klonua.com","UAFilm",3,1,items,&count,12);
    assert(count==3);
    assert(strstr(items[2].url,"/serialy/50-test.html"));

    const char *escaped="{\"content\":\"<a class=\\\"search-result-link\\\" "
       "href=\\\"/ua/filmy/123-test.html\\\"><span "
       "class=\\\"search-result-title\\\">Тест UAKino</span></a>\"}";
    char *decoded=json_string_field_alloc(escaped,"content");
    assert(decoded && strstr(decoded,"search-result-link"));
    parse_post_cards(decoded,"https://uakino.best","UAKino",4,0,items,&count,12);
    assert(count==4);
    assert(strstr(items[3].title,"Тест UAKino"));
    free(decoded);

    char file[1024],voice[96],label[192];
    const char *li="<ul><li data-voice=\"UATeam\" "
                   "data-file=\"//ashdi.vip/vod/123\">1 серія</li></ul>";
    const char *next=next_uakino_playlist_item(li,file,sizeof(file),
                                                 voice,sizeof(voice),
                                                 label,sizeof(label));
    assert(next && !strcmp(file,"https://ashdi.vip/vod/123"));
    assert(!strcmp(voice,"UATeam"));
    assert(!strcmp(label,"1 серія"));
    puts("new Ukrainian sources parser smoke OK");
    return 0;
}
