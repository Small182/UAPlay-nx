#include "iptv.h"
#include "platform.h"

#include <curl/curl.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define IPTV_URL "https://iptv-org.github.io/iptv/index.m3u"
#define IPTV_CACHE UAPLAY_DATA_DIR "/iptv.m3u"
#define IPTV_CACHE_TEMP IPTV_CACHE ".part"
#define IPTV_CACHE_BACKUP IPTV_CACHE ".bak"

enum { IPTV_PLAYLIST_LIMIT=6*1024*1024, IPTV_CHANNEL_LIMIT=25000 };

static const IptvCountry countries[]={
#include "iptv_countries.inc"
};

static const UiText genres[]={
    {"Анімація","Animation"},{"Авто","Auto"},{"Бізнес","Business"},
    {"Класика","Classic"},{"Комедія","Comedy"},{"Кулінарія","Cooking"},
    {"Культура","Culture"},{"Документальні","Documentary"},{"Освіта","Education"},
    {"Розваги","Entertainment"},{"Сімейні","Family"},{"Загальні","General"},
    {"Інтерактивні","Interactive"},{"Дитячі","Kids"},{"Парламентські","Legislative"},
    {"Стиль життя","Lifestyle"},{"Фільми","Movies"},{"Музика","Music"},
    {"Новини","News"},{"Активний відпочинок","Outdoor"},{"Суспільні","Public"},
    {"Відпочинок","Relax"},{"Релігійні","Religious"},{"Наука","Science"},
    {"Серіали","Series"},{"Покупки","Shop"},{"Спорт","Sports"},
    {"Подорожі","Travel"},{"Без жанру","Undefined"},{"Погода","Weather"}
};
_Static_assert(sizeof(countries)/sizeof(countries[0])<=IPTV_MAX_COUNTRIES,"IPTV country list too long");
_Static_assert(sizeof(genres)/sizeof(genres[0])<=IPTV_MAX_GENRES,"IPTV genre mask too small");

const IptvCountry *iptv_countries(int *count) {
    *count=sizeof(countries)/sizeof(countries[0]);
    return countries;
}

const UiText *iptv_genres(int *count) {
    *count=sizeof(genres)/sizeof(genres[0]);
    return genres;
}

void iptv_free(IptvCatalog *catalog) {
    free(catalog->channels);
    free(catalog->playlist);
    *catalog=(IptvCatalog){0};
}

typedef struct {
    char *data;
    size_t size, capacity;
} Playlist;

static size_t write_playlist(char *data,size_t size,size_t count,void *opaque) {
    Playlist *p=opaque;
    size_t bytes=size*count;
    if(bytes>IPTV_PLAYLIST_LIMIT-p->size)return 0;
    if(p->size+bytes+1>p->capacity){
        size_t capacity=(p->size+bytes+1)*2;
        if(capacity>IPTV_PLAYLIST_LIMIT+1)capacity=IPTV_PLAYLIST_LIMIT+1;
        char *grown=realloc(p->data,capacity);
        if(!grown)return 0;
        p->data=grown;
        p->capacity=capacity;
    }
    memcpy(p->data+p->size,data,bytes);
    p->size+=bytes;
    p->data[p->size]=0;
    return bytes;
}

/* iptv-org assigns tvg-id as ChannelName.country[@feed]. The suffix is
   the channel's home country, independently of the feed's broadcast area. */
static int channel_country(const char *id) {
    const char *end=id+strcspn(id,"@");
    if(end-id<4 || end[-3]!='.' || !isalpha((unsigned char)end[-2]) ||
       !isalpha((unsigned char)end[-1]))return -1;
    char code[]={toupper((unsigned char)end[-2]),toupper((unsigned char)end[-1]),0};
    int low=0,high=sizeof(countries)/sizeof(countries[0]);
    while(low<high){
        int middle=low+(high-low)/2;
        int cmp=strcmp(code,countries[middle].code);
        if(!cmp)return middle;
        if(cmp<0)high=middle;
        else low=middle+1;
    }
    return -1;
}

static uint32_t channel_genres(const char *groups) {
    uint32_t mask=0;
    while(groups && *groups){
        size_t length=strcspn(groups,";");
        for(unsigned i=0;i<sizeof(genres)/sizeof(genres[0]);i++)
            if(strlen(genres[i].en)==length && !strncasecmp(genres[i].en,groups,length))
                mask|=UINT32_C(1)<<i;
        groups+=length;
        if(*groups)groups++;
    }
    if(mask)return mask;
    for(unsigned i=0;i<sizeof(genres)/sizeof(genres[0]);i++)
        if(!strcmp(genres[i].en,"Undefined"))return UINT32_C(1)<<i;
    return 0;
}

static bool http_url(const char *url) {
    return (!strncmp(url,"https://",8) || !strncmp(url,"http://",7)) &&
           strcspn(url," \t\r\n")==strlen(url);
}

static void read_metadata(char *line,IptvChannel *channel) {
    *channel=(IptvChannel){.country=-1};
    /* Walk quoted attributes before the first unquoted comma. */
    char *p=line+8;
    while(*p && *p!=','){
        if(isspace((unsigned char)*p)){p++;continue;}
        char *name=p;
        p+=strcspn(p,"=, \t");
        size_t length=(size_t)(p-name);
        if(p[0]!='=' || p[1]!='"'){
            if(*p=='=')p++;
            continue;
        }
        char *value=p+2;
        char *end=strchr(value,'"');
        if(!end)return;
        *end=0;
        if(length==6 && !memcmp(name,"tvg-id",6)){
            channel->id=value;
            channel->country=channel_country(value);
        }else if(length==11 && !memcmp(name,"group-title",11))channel->groups=value;
        else if(length==15 && !memcmp(name,"http-user-agent",15))channel->agent=value;
        else if(length==13 && !memcmp(name,"http-referrer",13))channel->referrer=value;
        p=end+1;
    }
    if(*p==',' && p[1])channel->title=p+1;
    channel->genres=channel_genres(channel->groups);
}

static bool iptv_header(const char *text) {
    return text && !strncmp(text,"#EXTM3U",7) &&
           (text[7]==0 || isspace((unsigned char)text[7]));
}

static int parse_playlist(char *text,IptvCatalog *catalog) {
    if(!iptv_header(text))return IPTV_INVALID;
    IptvChannel item={.country=-1};
    int capacity=0;
    char *save=NULL;
    for(char *line=strtok_r(text,"\r\n",&save);line;line=strtok_r(NULL,"\r\n",&save)){
        if(!strncmp(line,"#EXTINF:",8)){
            read_metadata(line,&item);
        }else if(item.title && !strncmp(line,"#EXTVLCOPT:",11)){
            const char *value=line+11;
            if(!strncmp(value,"http-user-agent=",16) && !item.agent)item.agent=value+16;
            if(!strncmp(value,"http-referrer=",14) && !item.referrer)item.referrer=value+14;
        }else if(line[0]!='#' && item.title){
            if(item.country>=0 && http_url(line) && strlen(line)<2048){
                if(catalog->count==IPTV_CHANNEL_LIMIT)return IPTV_INVALID;
                if(catalog->count==capacity){
                    capacity=capacity?capacity*2:256;
                    IptvChannel *grown=realloc(catalog->channels,(size_t)capacity*sizeof(*grown));
                    if(!grown)return IPTV_MEMORY;
                    catalog->channels=grown;
                }
                item.url=line;
                catalog->channels[catalog->count++]=item;
            }
            item.title=NULL;
        }
    }
    return catalog->count?IPTV_OK:IPTV_INVALID;
}

static void cache_playlist(const IptvCatalog *catalog) {
    mkdir(UAPLAY_DATA_ROOT,0777);
    mkdir(UAPLAY_DATA_DIR,0777);
    FILE *file=fopen(IPTV_CACHE_TEMP,"wb");
    if(!file)return;
    bool written=fputs("#EXTM3U\n",file)>=0;
    for(int i=0;i<catalog->count && written;i++){
        const IptvChannel *channel=&catalog->channels[i];
        written=fprintf(file,"#EXTINF:-1 tvg-id=\"%s\" group-title=\"%s\",%s\n",
                        channel->id,channel->groups?channel->groups:"Undefined",channel->title)>=0;
        if(written && channel->agent)
            written=fprintf(file,"#EXTVLCOPT:http-user-agent=%s\n",channel->agent)>=0;
        if(written && channel->referrer)
            written=fprintf(file,"#EXTVLCOPT:http-referrer=%s\n",channel->referrer)>=0;
        if(written)written=fprintf(file,"%s\n",channel->url)>=0;
    }
    if(fclose(file)==0 && written){
        /* libnx cannot rename over an existing file. Retain a usable
           cache if replacing it fails, even across an app restart. */
        struct stat info;
        if(stat(IPTV_CACHE,&info)==0)remove(IPTV_CACHE_BACKUP);
        int had_cache=rename(IPTV_CACHE,IPTV_CACHE_BACKUP)==0;
        if(had_cache || errno==ENOENT){
            if(rename(IPTV_CACHE_TEMP,IPTV_CACHE)==0){
                if(had_cache)remove(IPTV_CACHE_BACKUP);
            }else if(had_cache)rename(IPTV_CACHE_BACKUP,IPTV_CACHE);
        }
    }
    remove(IPTV_CACHE_TEMP);
}

static void read_cache(Playlist *p) {
    FILE *file=fopen(IPTV_CACHE,"rb");
    if(!file)file=fopen(IPTV_CACHE_BACKUP,"rb");
    if(!file)return;
    char buffer[8192];
    size_t bytes;
    while((bytes=fread(buffer,1,sizeof(buffer),file))!=0)
        if(write_playlist(buffer,1,bytes,p)!=bytes)break;
    fclose(file);
}

int iptv_load(IptvCatalog *catalog,int *from_cache) {
    Playlist buffer={0};
    CURL *curl=curl_easy_init();
    CURLcode result=CURLE_FAILED_INIT;
    if(curl){
        curl_easy_setopt(curl,CURLOPT_URL,IPTV_URL);
        curl_easy_setopt(curl,CURLOPT_USERAGENT,"UAPlay (Nintendo Switch)");
        curl_easy_setopt(curl,CURLOPT_FAILONERROR,1L);
        curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);
        curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT,12L);
        curl_easy_setopt(curl,CURLOPT_TIMEOUT,25L);
#ifdef __SWITCH__
        curl_easy_setopt(curl,CURLOPT_IPRESOLVE,(long)CURL_IPRESOLVE_V4);
#endif
        curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,write_playlist);
        curl_easy_setopt(curl,CURLOPT_WRITEDATA,&buffer);
        result=curl_easy_perform(curl);
        curl_easy_cleanup(curl);
    }
    *from_cache=result!=CURLE_OK || !iptv_header(buffer.data);
    if(*from_cache){
        free(buffer.data);
        buffer=(Playlist){0};
        read_cache(&buffer);
        if(!buffer.data)return IPTV_NETWORK;
    }
    catalog->playlist=buffer.data;
    int status=parse_playlist(catalog->playlist,catalog);
    if(status==IPTV_OK && !*from_cache)cache_playlist(catalog);
    else if(status!=IPTV_OK && !*from_cache){
        iptv_free(catalog);
        buffer=(Playlist){0};
        read_cache(&buffer);
        catalog->playlist=buffer.data;
        if(buffer.data){
            status=parse_playlist(catalog->playlist,catalog);
            *from_cache=1;
        }
    }
    if(status!=IPTV_OK)iptv_free(catalog);
    return status;
}
