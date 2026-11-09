#include "catalog.h"

#include <curl/curl.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define TMDB_KEY_PATH "sdmc:/switch/lampa-nx/tmdb.key"
/* Public client-side key from yumata/lampa-source src/core/tmdb/tmdb.js.
   Users may override it with their own tmdb.key. */
#define LAMPA_PUBLIC_TMDB_KEY "4ef0d7355d9ffb5151e987764708ce96"
#define UAFRESH_WORKER "https://uakino.ofreshofresh.workers.dev"
#define UAFLIX_COOKIE_PATH "sdmc:/switch/uaflix/uaflix.cookie"
static char uaflix_cookie[2048];
static int cookie_loaded=0;

static void load_cookie(void) {
    if(cookie_loaded)return;
    cookie_loaded=1;
    FILE *file=fopen(UAFLIX_COOKIE_PATH,"rb");
    if(file){
        if(fgets(uaflix_cookie,sizeof(uaflix_cookie),file))
            uaflix_cookie[strcspn(uaflix_cookie,"\r\n")]=0;
        fclose(file);
    }
}
int uaflix_logged_in(void){load_cookie();return uaflix_cookie[0]!=0;}
void uaflix_logout(void){
    cookie_loaded=1;uaflix_cookie[0]=0;
    remove(UAFLIX_COOKIE_PATH);
}

typedef struct { char *data; size_t length, capacity; } Response;
static void json_field(const char *object,const char *end,const char *key,
                       char *value,size_t size);

static size_t on_data(char *ptr, size_t size, size_t count, void *opaque) {
    Response *r = opaque;
    size_t bytes = size * count;
    if (r->length + bytes > 3 * 1024 * 1024) return 0;
    if (r->length + bytes + 1 > r->capacity) {
        size_t next = (r->length + bytes + 1) * 2;
        char *new_data = realloc(r->data, next);
        if (!new_data) return 0;
        r->data = new_data;
        r->capacity = next;
    }
    memcpy(r->data + r->length, ptr, bytes);
    r->length += bytes;
    r->data[r->length] = 0;
    return bytes;
}

static char *request(const char *url, char *error, int error_size) {
    CURL *curl = curl_easy_init();
    Response r = {0};
    if (!curl) { snprintf(error, error_size, "curl init failed"); return NULL; }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 18L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Lampa-nx/0.2 (Nintendo Switch)");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &r);
    CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (result != CURLE_OK || status < 200 || status >= 300) {
        if (result == CURLE_OK) snprintf(error, error_size, "Server returned HTTP %ld", status);
        else snprintf(error, error_size, "HTTP %ld: %s", status, curl_easy_strerror(result));
        free(r.data);
        return NULL;
    }
    return r.data;
}

static char *encode(const char *value) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    char *encoded = curl_easy_escape(curl, value, 0);
    char *result = encoded ? strdup(encoded) : NULL;
    curl_free(encoded);
    curl_easy_cleanup(curl);
    return result;
}

int uaflix_login(const char *login,const char *password,char *error,int error_size){
    if(!login || !password || !*login || !*password){
        snprintf(error,error_size,"Введіть логін і пароль UAFlix");return 0;
    }
    CURL *curl=curl_easy_init();
    if(!curl){snprintf(error,error_size,"Помилка мережі");return 0;}
    char *user=curl_easy_escape(curl,login,0),*pass=curl_easy_escape(curl,password,0);
    if(!user || !pass){
        if(user)curl_free(user);
        if(pass)curl_free(pass);
        curl_easy_cleanup(curl);return 0;
    }
    size_t size=strlen(user)+strlen(pass)+50;
    char *body=malloc(size);
    if(!body){curl_free(user);curl_free(pass);curl_easy_cleanup(curl);return 0;}
    snprintf(body,size,"site=uaflix&login=%s&password=%s",user,pass);
    curl_free(user);curl_free(pass);
    Response response={0};
    curl_easy_setopt(curl,CURLOPT_URL,UAFRESH_WORKER "/login");
    curl_easy_setopt(curl,CURLOPT_POSTFIELDS,body);
    curl_easy_setopt(curl,CURLOPT_TIMEOUT,30L);
    curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,on_data);
    curl_easy_setopt(curl,CURLOPT_WRITEDATA,&response);
    CURLcode result=curl_easy_perform(curl);
    long status=0;curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&status);
    curl_easy_cleanup(curl);
    memset(body,0,size);free(body);
    if(result!=CURLE_OK || status!=200 || !response.data){
        snprintf(error,error_size,"UAFlix: помилка входу (HTTP %ld)",status);
        free(response.data);return 0;
    }
    char ok[12]={0},cookie[sizeof(uaflix_cookie)]={0};
    const char *end=response.data+strlen(response.data);
    json_field(response.data,end,"ok",ok,sizeof(ok));
    json_field(response.data,end,"cookie",cookie,sizeof(cookie));
    free(response.data);
    if(strcmp(ok,"true") || !cookie[0] || strpbrk(cookie,"\r\n")){
        snprintf(error,error_size,"UAFlix: вхід не виконано. Перевірте дані.");return 0;
    }
    snprintf(uaflix_cookie,sizeof(uaflix_cookie),"%s",cookie);
    cookie_loaded=1;
    mkdir("sdmc:/switch",0777);mkdir("sdmc:/switch/uaflix",0700);
    FILE *file=fopen(UAFLIX_COOKIE_PATH,"wb");
    if(file){fputs(uaflix_cookie,file);fclose(file);}
    snprintf(error,error_size,"UAFlix: вхід виконано. Спробуйте серію ще раз.");
    return 1;
}

/* A compact JSON string reader, safe for escaped quotation marks. */
static void json_field(const char *object, const char *end, const char *key,
                       char *value, size_t size) {
    value[0] = 0;
    char needle[80];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = object;
    while ((p = strstr(p, needle)) && p < end) {
        p += strlen(needle);
        while (p < end && isspace((unsigned char)*p)) p++;
        if (p >= end || *p != ':') continue;
        p++;
        while (p < end && isspace((unsigned char)*p)) p++;
        if (p >= end) return;
        int quoted = (*p == '"');
        if (quoted) p++;
        size_t n = 0;
        while (p < end && n + 1 < size) {
            if (quoted && *p == '"') break;
            if (!quoted && (*p == ',' || *p == '}' || isspace((unsigned char)*p))) break;
            if (*p == '\\' && p + 1 < end) {
                p++;
                if (*p == 'n' || *p == 'r') { value[n++] = ' '; p++; continue; }
                if (*p == 'u' && p + 4 < end) { p += 5; continue; }
            }
            value[n++] = *p++;
        }
        value[n] = 0;
        return;
    }
}

static const char *object_end(const char *p) {
    int depth = 0, quoted = 0, escaped = 0;
    for (; *p; p++) {
        if (escaped) { escaped = 0; continue; }
        if (quoted && *p == '\\') { escaped = 1; continue; }
        if (*p == '"') { quoted = !quoted; continue; }
        if (!quoted && *p == '{') depth++;
        if (!quoted && *p == '}' && --depth == 0) return p + 1;
    }
    return NULL;
}

static char *read_offline_catalog(const char *category) {
    const char *name = !strcmp(category, "series") ? "series" :
                       !strcmp(category, "top") ? "top" : "movies";
    char path[80];
    snprintf(path, sizeof(path), "romfs:/%s.json", name);
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END)) { fclose(file); return NULL; }
    long length = ftell(file);
    if (length < 1 || length > 3 * 1024 * 1024) { fclose(file); return NULL; }
    rewind(file);
    char *result = malloc((size_t)length + 1);
    if (!result) { fclose(file); return NULL; }
    size_t got = fread(result, 1, (size_t)length, file);
    fclose(file);
    result[got] = 0;
    if (got != (size_t)length) { free(result); return NULL; }
    return result;
}

int catalog_load(const char *category, const char *query, Movie *items, int max,
                 char *error, int error_size) {
    char key[256] = LAMPA_PUBLIC_TMDB_KEY;
    FILE *f = fopen(TMDB_KEY_PATH, "r");
    if (f) {
        char override[256] = {0};
        if (fgets(override, sizeof(override), f) && override[0])
            snprintf(key, sizeof(key), "%s", override);
        fclose(f);
    }
    key[strcspn(key, "\r\n \t")] = 0;
    if (!key[0]) { snprintf(error, error_size, "TMDB API key is empty"); return 0; }

    char url[1800];
    int searching=query && *query;
    char *q = searching ? encode(query) : NULL;
    if (q) {
        snprintf(url, sizeof(url), "https://api.themoviedb.org/3/search/multi?api_key=%s&language=uk-UA&include_adult=false&query=%s", key, q);
    } else {
        if (!strncmp(category,"genre_",6))
            snprintf(url, sizeof(url), "https://api.themoviedb.org/3/discover/movie?api_key=%s&language=uk-UA&sort_by=popularity.desc&with_genres=%s",key,category+6);
        else {
            const char *path = !strcmp(category, "series") ? "trending/tv/week" :
                               !strcmp(category, "top") ? "movie/top_rated" : "trending/movie/week";
            snprintf(url, sizeof(url), "https://api.themoviedb.org/3/%s?api_key=%s&language=uk-UA", path, key);
        }
    }
    free(q);
    char *json = request(url, error, error_size);
    if (!json) {
        /* Never show the unrelated offline home catalogue as the result of
           a failed search query. That was the "wrong movie" regression. */
        if(searching){
            char reason[160];snprintf(reason,sizeof(reason),"%s",error);
            snprintf(error,error_size,"Пошук TMDB недоступний: %.155s",reason);
            return 0;
        }
        json = read_offline_catalog(category);
        if (json) {
            snprintf(error, error_size, "Offline catalog (network unavailable): %s", category);
        } else return 0;
    } else error[0] = 0;
    const char *results = strstr(json, "\"results\"");
    if (!results || !(results = strchr(results, '['))) { free(json); snprintf(error, error_size, "Bad TMDB response"); return 0; }
    const char *p = results + 1;
    int count = 0;
    while (*p && count < max && count < CATALOG_MAX) {
        if (*p == ']') break;
        if (*p != '{') { p++; continue; }
        const char *end = object_end(p);
        if (!end) break;
        Movie m = {0};
        char number[40], type[40], date[40];
        json_field(p, end, "id", number, sizeof(number));
        json_field(p, end, "media_type", type, sizeof(type));
        json_field(p, end, "title", m.title, sizeof(m.title));
        if (!m.title[0]) json_field(p, end, "name", m.title, sizeof(m.title));
        json_field(p, end, "original_title", m.original, sizeof(m.original));
        if (!m.original[0]) json_field(p, end, "original_name", m.original, sizeof(m.original));
        json_field(p, end, "overview", m.overview, sizeof(m.overview));
        json_field(p, end, "poster_path", m.poster, sizeof(m.poster));
        json_field(p, end, "release_date", date, sizeof(date));
        if (!date[0]) json_field(p, end, "first_air_date", date, sizeof(date));
        memcpy(m.year, date, strlen(date) >= 4 ? 4 : strlen(date));
        m.id = atoi(number);
        m.series = !strcmp(type, "tv") || (!q && !strcmp(category, "series"));
        if (m.id && m.title[0] && strcmp(type, "person")) items[count++] = m;
        p = end;
    }
    free(json);
    if (!count) snprintf(error, error_size, "No titles found");
    return count;
}

/* Use UAFlix and UASerials sources. The experimental UAKino.best provider
   is disabled: it produced unreliable search failures on the console. */
static char *fetch_uafresh(const char *upstream, char *error, int error_size) {
    /* Native homebrew does not have browser CORS restrictions. A direct
       request originates from the Switch user's Ukrainian network, unlike
       the Cloudflare worker; UAFlix episode availability is geo-dependent. */
    if(!strncmp(upstream,"https://uafix.net/",18) ||
       !strncmp(upstream,"https://zetvideo.net/",21) ||
       !strncmp(upstream,"https://hdvbua.pro/",19) ||
       !strncmp(upstream,"https://ashdi.vip/",18)){
        char *direct=request(upstream,error,error_size);
        if(direct)return direct;
        /* Existing public UAFresh worker remains a normal network fallback. */
    }
    char *encoded = encode(upstream);
    if (!encoded) { snprintf(error, error_size, "URL encoding failed"); return NULL; }
    load_cookie();
    int use_cookie=strstr(upstream,"https://uafix.net/")==upstream && uaflix_cookie[0];
    char *cookie=use_cookie?encode(uaflix_cookie):NULL;
    size_t length = strlen(encoded) + strlen(UAFRESH_WORKER) + 24 +
                    (cookie?strlen(cookie)+9:0);
    char *url = malloc(length);
    if (!url) { free(cookie);free(encoded); return NULL; }
    snprintf(url, length, UAFRESH_WORKER "/proxy?url=%s%s%s", encoded,
             cookie?"&cookie=":"",cookie?cookie:"");
    free(encoded);
    free(cookie);
    char *html = request(url, error, error_size);
    free(url);
    return html;
}

static int looks_like_result(const char *url, const char *host, int provider,
                             int want_series) {
    size_t hostlen = strlen(host);
    if (strncmp(url, host, hostlen)) return 0;
    const char *path = url + hostlen;
    if (*path != '/') return 0;
    if (provider == 0) { /* UAFlix: film/series slug, not category or menu */
        const char *end = strchr(path+1, '/');
        if (!end) return 0;
        int group_ok = ((size_t)(end-(path+1)) == 5 && !strncmp(path+1, "films", 5)) ||
                       ((size_t)(end-(path+1)) == 7 && !strncmp(path+1, "serials", 7)) ||
                       ((size_t)(end-(path+1)) == 5 && !strncmp(path+1, "anime", 5));
        if (!group_ok || end[1] == 0) return 0;
        /* A search for a TV series must not list unrelated film cards (and
           vice versa), even when the provider returns both in one search. */
        int films=(size_t)(end-(path+1))==5 && !strncmp(path+1,"films",5);
        if ((want_series && films) || (!want_series && !films))return 0;
        const char *slug_end = strchr(end+1, '/');
        if (!slug_end || slug_end[1] != 0 || slug_end == end+1) return 0;
        for (const char *p=end+1; p<slug_end; p++)
            if (!(isalnum((unsigned char)*p) || *p=='-')) return 0;
        return 1;
    }
    if(provider==2){ /* UAKino DLE: category/.../123-title.html */
        const char *last=strrchr(path,'/');
        if(!last || last==path)return 0;
        last++;
        if(!isdigit((unsigned char)*last))return 0;
        while(isdigit((unsigned char)*last))last++;
        return *last=='-' && strstr(last,".html")!=NULL;
    }
    /* UASerials: /2389-avatar-2009.html */
    const char *p = path+1;
    if (!isdigit((unsigned char)*p)) return 0;
    while (isdigit((unsigned char)*p)) p++;
    return *p == '-' && strstr(p, ".html") != NULL;
}

static void html_title(const char *source,char *target,size_t capacity){
    size_t written=0;
    while(*source && written+1<capacity){
        const char *replacement=NULL;
        size_t consumed=0;
        if(!strncmp(source,"&#039;",6)){replacement="'";consumed=6;}
        else if(!strncmp(source,"&apos;",6)){replacement="'";consumed=6;}
        else if(!strncmp(source,"&quot;",6)){replacement="\"";consumed=6;}
        else if(!strncmp(source,"&amp;",5)){replacement="&";consumed=5;}
        else if(!strncmp(source,"&nbsp;",6)){replacement=" ";consumed=6;}
        if(replacement){target[written++]=*replacement;source+=consumed;}
        else target[written++]=*source++;
    }
    target[written]=0;
}

static void search_provider(const char *host, const char *name, int provider,
                            const char *query, int want_series,
                            Source *items, int *count, int limit,
                            char *error, int error_size) {
    char *q = encode(query);
    if (!q) return;
    size_t length = strlen(q)+strlen(host)+92;
    char *search_url = malloc(length);
    if (!search_url) { free(q); return; }
    snprintf(search_url, length, "%s/index.php?do=search&subaction=search&story=%s", host, q);
    free(q);
    char *html = fetch_uafresh(search_url, error, error_size);
    free(search_url);
    if (!html) return;
    const char *p = html;
    /* UAFlix includes its entire category-navigation menu BEFORE the search
       results. Scanning every href mistakenly returned films/action/ and
       serials/school/ as playable titles, filling the list with folders.
       Real search result cards are <a class="sres-wrap ..." href="...">. */
    while (*count < limit &&
           (p = strstr(p,provider==0?"class=\"sres-wrap":"href=")) != NULL) {
        const char *card=p;
        if(provider==0){
            const char *tag_end=strchr(card,'>');
            if(!tag_end)break;
            const char *href=strstr(card,"href=");
            if(!href || href>=tag_end){p=tag_end+1;continue;}
            p=href;
        }
        p += 5;
        if (*p != '"' && *p != '\'') continue;
        char quote = *p++;
        const char *end = strchr(p, quote);
        if (!end) break;
        size_t n = (size_t)(end-p);
        char candidate[1024];
        if (n > 12 && n < sizeof(candidate)) {
            if(*p=='/' && n+strlen(host)+1<sizeof(candidate))
                snprintf(candidate,sizeof(candidate),"%s%.*s",host,(int)n,p);
            else{
                memcpy(candidate,p,n);
                candidate[n]=0;
            }
            if (looks_like_result(candidate, host, provider,want_series)) {
                int exists=0;
                for (int i=0; i<*count; i++)
                    if (!strcmp(items[i].url,candidate)) exists=1;
                if (!exists) {
                    Source *dst = &items[(*count)++];
                    snprintf(dst->url,sizeof(dst->url),"%s",candidate);
                    /* Use the card's real human-readable title, not URL slug. */
                    const char *title_start=NULL,*title_end=NULL;
                    if(provider==0){
                        const char *heading=strstr(end,"<h2>");
                        if(heading && heading-end<800){
                            title_start=heading+4;
                            title_end=strstr(title_start,"</h2>");
                            if(!title_end || title_end-title_start>300)title_start=NULL;
                        }
                    }
                    if(title_start){
                        char label[176];size_t len=(size_t)(title_end-title_start);
                        if(len>=sizeof(label))len=sizeof(label)-1;
                        /* Never cut inside a UTF-8 continuation byte. */
                        while(len && ((unsigned char)title_start[len]&0xC0)==0x80)len--;
                        memcpy(label,title_start,len);label[len]=0;
                        char human[176];html_title(label,human,sizeof(human));
                        snprintf(dst->title,sizeof(dst->title),"%s: %s",name,human);
                    }else snprintf(dst->title,sizeof(dst->title),"%s: %s",name,
                                  candidate+strlen(host)+1);
                }
            }
        }
        p=end+1;
    }
    free(html);
}

int uakino_search(const Movie *movie, Source *items, int max,
                  char *error, int error_size) {
    int count=0;
    const char *search = movie->original[0] ? movie->original : movie->title;
    error[0]=0;
    search_provider("https://uafix.net", "UAFlix", 0, search,movie->series,
                    items, &count, max, error, error_size);
    search_provider("https://uaserials.my", "UASerials", 1, search,movie->series,
                    items, &count, max, error, error_size);
    if (count) error[0]=0;
    else if (!error[0]) snprintf(error,error_size,"UAFresh: no matching pages");
    return count;
}

static int extract_media(const char *html, char *stream, int size) {
    const char *p=html;
    while ((p=strstr(p,"https://")) != NULL) {
        const char *end=p;
        while (*end && *end!='"' && *end!='\'' && *end!='<' &&
               *end!=' ' && *end!='\\' && end-p<2047) end++;
        size_t length=(size_t)(end-p);
        if (length < (size_t)size && length > 12) {
            char url[2048];
            memcpy(url,p,length);url[length]=0;
            if ((strstr(url,".m3u8") || strstr(url,".mp4") || strstr(url,".mpd")) &&
                !strstr(url,"trailer")) {
                snprintf(stream,(size_t)size,"%s",url);
                return 1;
            }
        }
        p+=8;
    }
    return 0;
}

static void extract_subtitle(const char *html);
/* Parse actual iframe tags, not just the single hard-coded video host.
   Return a cursor for the next candidate so one failed player does not
   prevent trying the next tab/embedded player on the episode page. */
static const char *find_embed_after(const char *cursor,char *url,int size) {
    const char *tag=cursor;
    while((tag=strstr(tag,"<iframe"))!=NULL){
        const char *tag_end=strchr(tag,'>');
        if(!tag_end)break;
        const char *attr=tag;
        while((attr=strstr(attr,"src=")) && attr<tag_end){
            const char *value=attr+4;
            if(*value=='"' || *value=='\''){
                char quote=*value++;
                const char *end=strchr(value,quote);
                if(end && end<=tag_end){
                    size_t len=(size_t)(end-value);
                    if(len>8 && len<(size_t)size-8){
                        if(!strncmp(value,"https://",8)){
                            memcpy(url,value,len);url[len]=0;
                        }else if(!strncmp(value,"//",2)){
                            snprintf(url,(size_t)size,"https:%.*s",(int)len,value);
                        }else url[0]=0;
                        if(url[0] && !strstr(url,"youtube") &&
                           !strstr(url,"trailer") && !strstr(url,"doubleclick") &&
                           !strstr(url,"google"))return tag_end+1;
                    }
                }
            }
            attr+=4;
        }
        tag=tag_end+1;
    }
    return NULL;
}

static int resolve_embedded(const char *html,char *stream,int size,
                            int depth,int *requests_left,char *error,int error_size){
    if(extract_media(html,stream,size))return 1;
    if(depth<=0)return 0;
    const char *next=html;
    for(int attempt=0;attempt<3 && *requests_left>0;attempt++){
        char embedded[1024]={0};
        next=find_embed_after(next,embedded,sizeof(embedded));
        if(!next)break;
        --*requests_left;
        char *content=fetch_uafresh(embedded,error,error_size);
        if(!content)continue;
        extract_subtitle(content);
        int found=resolve_embedded(content,stream,size,depth-1,requests_left,error,error_size);
        free(content);
        if(found)return 1;
    }
    /* Keep the original non-iframe provider fallback: several film pages
       store a player URL in a script attribute instead of an iframe tag. */
    if(depth>0){
        const char *hosts[]={"https://zetvideo.net/vod/","https://hdvbua.pro/",
                              "https://ashdi.vip/","https://tortuga."};
        for(size_t i=0;i<sizeof(hosts)/sizeof(hosts[0]) && *requests_left>0;i++){
            const char *p=strstr(html,hosts[i]);
            if(!p)continue;
            const char *end=p;
            while(*end && *end!='"' && *end!='\'' && *end!='<' &&
                  *end!=' ' && *end!='\\' && end-p<1000)end++;
            size_t len=(size_t)(end-p);
            if(len<15 || len>999)continue;
            char link[1024];memcpy(link,p,len);link[len]=0;
            if(strstr(link,"trailer") || strstr(link,"youtube"))continue;
            --*requests_left;
            char *content=fetch_uafresh(link,error,error_size);
            if(!content)continue;
            extract_subtitle(content);
            int found=extract_media(content,stream,size);
            free(content);
            if(found)return 1;
        }
    }
    return 0;
}

static char resolved_subtitle[1024];
const char *uakino_resolved_subtitle(void) { return resolved_subtitle; }

static void extract_subtitle(const char *html) {
    const char *field=strstr(html,"subtitle:");
    if (!field) field=strstr(html,"subtitle\":");
    if (!field) return;
    const char *url=strstr(field,"https://");
    if (!url || url-field>500) return;
    const char *end=url;
    while (*end && *end!=',' && *end!='"' && *end!='\'' && *end!='<' &&
           *end!=' ' && end-url < (int)sizeof(resolved_subtitle)-1) end++;
    size_t n=(size_t)(end-url);
    if (n>=7 && (strstr(url,".vtt") || strstr(url,".srt"))) {
        memcpy(resolved_subtitle,url,n);
        resolved_subtitle[n]=0;
    }
}

int uakino_resolve(const Source *source, char *stream, int size,
                   char *error, int error_size) {
    resolved_subtitle[0]=0;
    char *page = fetch_uafresh(source->url,error,error_size);
    if (!page) return 0;
    extract_subtitle(page);
    int requests_left=3;
    int found=resolve_embedded(page,stream,size,2,&requests_left,error,error_size);
    free(page);
    if(found)return 1;
    /* UAFlix episodes are publicly playable on the site. A missing stream
       means our extraction did not find the player, not an auth requirement. */
    snprintf(error,error_size,"Не знайдено прямий потік цієї серії. Спробуйте інший плеєр або джерело.");
    return 0;
}

static int episode_order(const void *a, const void *b) {
    const Episode *one=a, *two=b;
    if (one->season != two->season) return one->season - two->season;
    return one->number - two->number;
}

int uakino_episodes(const Source *source, Episode *items, int max,
                    char *error, int error_size) {
    char *html=fetch_uafresh(source->url,error,error_size);
    if (!html) return 0;
    int count=0;
    const char *p=html;
    while (count<max && (p=strstr(p,"href="))) {
        p+=5;
        if (*p!='"' && *p!='\'') continue;
        char quote=*p++;
        const char *end=strchr(p,quote);
        if (!end) break;
        size_t len=(size_t)(end-p);
        char url[1024];
        if (len>15 && len<sizeof(url)) {
            memcpy(url,p,len);url[len]=0;
            const char *token=strstr(url,"/season-");
            int season=0,episode=0;
            if (token && sscanf(token,"/season-%d-episode-%d/",&season,&episode)==2 &&
                season>=1 && season<100 && episode>=1 && episode<1000 &&
                !strncmp(url,"https://uafix.net/",18)) {
                int exists=0;
                for (int i=0;i<count;i++)
                    if (items[i].season==season && items[i].number==episode) exists=1;
                if (!exists) {
                    Episode *new_episode=&items[count++];
                    new_episode->season=season;
                    new_episode->number=episode;
                    snprintf(new_episode->url,sizeof(new_episode->url),"%s",url);
                    snprintf(new_episode->title,sizeof(new_episode->title),
                             "Сезон %d · Серія %d",season,episode);
                }
            }
        }
        p=end+1;
    }
    free(html);
    if (count) {
        qsort(items,(size_t)count,sizeof(*items),episode_order);
        error[0]=0;
    } else snprintf(error,error_size,"No structured episodes in this source");
    return count;
}
