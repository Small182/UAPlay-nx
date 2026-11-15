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

#define IPTV_URL "https://iptv-org.github.io/iptv/countries/"
#define IPTV_FAVORITES UAPLAY_DATA_DIR "/iptv-favorites.m3u"

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
    return iptv_country_index(code);
}

int iptv_country_index(const char *code) {
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

static bool quality_label(const char *label,size_t length) {
    if(length==2 && (label[0]=='4' || label[0]=='8') &&
       (label[1]=='K' || label[1]=='k'))return true;
    size_t digits=0;
    while(digits<length && isdigit((unsigned char)label[digits]))digits++;
    if(digits<3 || digits>4)return false;
    if(length==digits+1)
        return label[digits]=='p' || label[digits]=='P' ||
               label[digits]=='i' || label[digits]=='I';
    return length==digits+2 &&
           (!memcmp(label+digits,"р",2) || !memcmp(label+digits,"Р",2));
}

static void clean_channel_title(char *title) {
    char *out=title;
    for(const char *in=title;*in;){
        if(*in=='(' || *in=='['){
            const char *end=strchr(in+1,*in=='('?')':']');
            if(end && quality_label(in+1,(size_t)(end-in-1))){
                while(out>title && isspace((unsigned char)out[-1]))out--;
                in=end+1;
                continue;
            }
        }
        *out++=*in++;
    }
    while(out>title && isspace((unsigned char)out[-1]))out--;
    char *suffix=out;
    while(suffix>title && !isspace((unsigned char)suffix[-1]))suffix--;
    if(suffix>title && quality_label(suffix,(size_t)(out-suffix))){
        out=suffix;
        while(out>title && isspace((unsigned char)out[-1]))out--;
    }
    *out=0;
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
        else if(length==8 && !memcmp(name,"tvg-logo",8) && http_url(value))channel->logo=value;
        else if(length==15 && !memcmp(name,"http-user-agent",15))channel->agent=value;
        else if(length==13 && !memcmp(name,"http-referrer",13))channel->referrer=value;
        p=end+1;
    }
    if(*p==','){
        char *title=p+1;
        clean_channel_title(title);
        if(*title)channel->title=title;
    }
    channel->genres=channel_genres(channel->groups);
}

static bool iptv_header(const char *text) {
    return text && !strncmp(text,"#EXTM3U",7) &&
           (text[7]==0 || isspace((unsigned char)text[7]));
}

static int parse_playlist(char *text,IptvCatalog *catalog,int country) {
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
            if(item.country>=0 && (country==IPTV_ALL || item.country==country) &&
               http_url(line) && strlen(line)<2048){
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
    return IPTV_OK;
}

static bool write_channel(FILE *file,const IptvChannel *channel) {
    if(fprintf(file,"#EXTINF:-1 tvg-id=\"%s\" group-title=\"%s\" tvg-logo=\"%s\",%s\n",
                channel->id,channel->groups?channel->groups:"Undefined",
                channel->logo?channel->logo:"",channel->title)<0)return false;
    if(channel->agent && fprintf(file,"#EXTVLCOPT:http-user-agent=%s\n",channel->agent)<0)return false;
    if(channel->referrer && fprintf(file,"#EXTVLCOPT:http-referrer=%s\n",channel->referrer)<0)return false;
    return fprintf(file,"%s\n",channel->url)>=0;
}

static int read_playlist(const char *path,IptvCatalog *catalog,int country) {
    FILE *file=fopen(path,"rb");
    if(!file)return IPTV_NETWORK;
    Playlist buffer={0};
    char data[8192];
    size_t bytes;
    bool complete=true;
    while((bytes=fread(data,1,sizeof(data),file))!=0)
        if(write_playlist(data,1,bytes,&buffer)!=bytes){complete=false;break;}
    complete=complete && !ferror(file);
    fclose(file);
    catalog->playlist=buffer.data;
    int status=complete?parse_playlist(catalog->playlist,catalog,country):IPTV_INVALID;
    if(status!=IPTV_OK)iptv_free(catalog);
    return status;
}

static int read_saved(const char *path,IptvCatalog *catalog,int country) {
    int status=read_playlist(path,catalog,country);
    if(status==IPTV_OK)return status;
    char backup[128];
    snprintf(backup,sizeof(backup),"%s.bak",path);
    return read_playlist(backup,catalog,country);
}

static bool save_playlist(const char *path,const IptvCatalog *catalog,int skip,
                          const IptvChannel *extra,IptvCatalog *replacement) {
    mkdir(UAPLAY_DATA_ROOT,0777);
    mkdir(UAPLAY_DATA_DIR,0777);
    char temp[128],backup[128];
    snprintf(temp,sizeof(temp),"%s.part",path);
    snprintf(backup,sizeof(backup),"%s.bak",path);
    FILE *file=fopen(temp,"wb");
    if(!file)return false;
    bool written=fputs("#EXTM3U\n",file)>=0;
    for(int i=0;i<catalog->count && written;i++)
        if(i!=skip)written=write_channel(file,&catalog->channels[i]);
    if(written && extra)written=write_channel(file,extra);
    bool closed=fclose(file)==0;
    if(closed && written && (!replacement || read_playlist(temp,replacement,IPTV_ALL)==IPTV_OK)){
        struct stat info;
        if(stat(path,&info)==0)remove(backup);
        bool had_file=rename(path,backup)==0;
        if(had_file || errno==ENOENT){
            if(rename(temp,path)==0){
                remove(backup);
                return true;
            }
            if(had_file)rename(backup,path);
        }
    }
    remove(temp);
    return false;
}

void iptv_favorites_load(IptvCatalog *catalog) {
    read_saved(IPTV_FAVORITES,catalog,IPTV_ALL);
}

int iptv_favorite_index(const IptvCatalog *favorites,const char *id) {
    for(int i=0;i<favorites->count;i++)
        if(!strcmp(favorites->channels[i].id,id))return i;
    return -1;
}

bool iptv_favorite_toggle(IptvCatalog *favorites,const IptvChannel *channel) {
    int remove_index=iptv_favorite_index(favorites,channel->id);
    IptvCatalog next={0};
    if(!save_playlist(IPTV_FAVORITES,favorites,remove_index,
                      remove_index<0?channel:NULL,&next)){
        iptv_free(&next);
        return false;
    }
    iptv_free(favorites);
    *favorites=next;
    return true;
}

int iptv_load(int country,IptvCatalog *catalog,int *from_cache) {
    if(country<0 || country>=(int)(sizeof(countries)/sizeof(countries[0])))return IPTV_INVALID;
    const char *code=countries[country].code;
    char path[112],url[128];
    int first=tolower((unsigned char)code[0]),second=tolower((unsigned char)code[1]);
    snprintf(path,sizeof(path),UAPLAY_DATA_DIR "/iptv-%c%c.m3u",first,second);
    snprintf(url,sizeof(url),IPTV_URL "%c%c.m3u",first,second);
    Playlist buffer={0};
    CURL *curl=curl_easy_init();
    CURLcode result=CURLE_FAILED_INIT;
    if(curl){
        curl_easy_setopt(curl,CURLOPT_URL,url);
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
    *from_cache=0;
    if(result==CURLE_OK){
        catalog->playlist=buffer.data;
        int status=parse_playlist(catalog->playlist,catalog,country);
        if(status==IPTV_OK){
            save_playlist(path,catalog,-1,NULL,NULL);
            return status;
        }
        iptv_free(catalog);
    }else free(buffer.data);
    int status=read_saved(path,catalog,country);
    *from_cache=status==IPTV_OK;
    return status;
}
