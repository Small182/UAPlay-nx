#include "catalog.h"

#include <curl/curl.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TMDB_KEY_PATH "sdmc:/switch/lampa-nx/tmdb.key"
/* Public client-side key from yumata/lampa-source src/core/tmdb/tmdb.js.
   Users may override it with their own tmdb.key. */
#define LAMPA_PUBLIC_TMDB_KEY "4ef0d7355d9ffb5151e987764708ce96"
#define UAFRESH_WORKER "https://uakino.ofreshofresh.workers.dev"

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
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Lampa-nx/0.2 (Nintendo Switch)");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &r);
    CURLcode result = curl_easy_perform(curl);
    if (result == CURLE_SSL_CONNECT_ERROR) {
        /* Eden's HTTPS service occasionally rejects a new TLS handshake.
           Clear any partial response and retry once with a fresh connection.
           Keep SSL peer/hostname verification enabled. */
        r.length = 0;
        if (r.data) r.data[0] = 0;
        curl_easy_setopt(curl, CURLOPT_FRESH_CONNECT, 1L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 9L);
        result = curl_easy_perform(curl);
    }
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

/* Case-insensitive substring search including Ukrainian Cyrillic, without
   depending on a console-wide Unicode locale. */
static unsigned folded_point(const unsigned char **cursor){
    const unsigned char *s=*cursor;
    unsigned cp=*s++;
    if(cp>=0xC0 && cp<=0xDF && s[0]){cp=((cp&31)<<6)|(s[0]&63);s++;}
    else if(cp>=0xE0 && cp<=0xEF && s[0] && s[1]){
        cp=((cp&15)<<12)|((s[0]&63)<<6)|(s[1]&63);s+=2;
    }
    *cursor=s;
    if(cp<128)return (unsigned)tolower((unsigned char)cp);
    if(cp>=0x0410 && cp<=0x042F)return cp+0x20;
    if(cp==0x0401)return 0x0451;
    if(cp==0x0404)return 0x0454;
    if(cp==0x0406)return 0x0456;
    if(cp==0x0407)return 0x0457;
    if(cp==0x0490)return 0x0491;
    return cp;
}
static int contains_title(const char *haystack,const char *needle){
    if(!needle || !*needle)return 1;
    for(const unsigned char *h=(const unsigned char*)haystack;*h;){
        const unsigned char *a=h,*b=(const unsigned char*)needle;
        while(*a && *b && folded_point(&a)==folded_point(&b)){}
        if(!*b)return 1;
        folded_point(&h);
    }
    return 0;
}

int catalog_load_page(const char *category, const char *query, int page,
                      Movie *items, int max, char *error, int error_size,
                      int *has_more) {
    if(has_more)*has_more=0;
    if (page<1 || page>500) return 0;
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
        if (!strcmp(category,"cartoon_series"))
            snprintf(url, sizeof(url), "https://api.themoviedb.org/3/discover/tv?api_key=%s&language=uk-UA&sort_by=popularity.desc&with_genres=16",key);
        else if (!strcmp(category,"anime"))
            snprintf(url, sizeof(url), "https://api.themoviedb.org/3/discover/tv?api_key=%s&language=uk-UA&sort_by=popularity.desc&with_genres=16&with_original_language=ja",key);
        else if (!strcmp(category,"cartoons"))
            snprintf(url, sizeof(url), "https://api.themoviedb.org/3/discover/movie?api_key=%s&language=uk-UA&sort_by=popularity.desc&with_genres=16",key);
        else if (!strncmp(category,"genre_",6))
            snprintf(url, sizeof(url), "https://api.themoviedb.org/3/discover/movie?api_key=%s&language=uk-UA&sort_by=popularity.desc&with_genres=%s",key,category+6);
        else {
            const char *path = !strcmp(category, "series") ? "trending/tv/week" :
                               !strcmp(category, "top") ? "movie/top_rated" : "trending/movie/week";
            snprintf(url, sizeof(url), "https://api.themoviedb.org/3/%s?api_key=%s&language=uk-UA", path, key);
        }
    }
    free(q);
    /* The TMDB API defaults to page 1. Explicitly request subsequent pages. */
    size_t used=strlen(url);
    if (used+24<sizeof(url))snprintf(url+used,sizeof(url)-used,"&page=%d",page);
    char *json = request(url, error, error_size);
    int offline_search=0;
    if (!json) {
        /* Never repeat the offline page in background pagination. */
        if(page>1)return 0;
        json = read_offline_catalog(category);
        if (json) {
            offline_search=searching;
            if(searching)snprintf(error,error_size,
                                  "Офлайн-пошук: лише збережені назви (TMDB недоступний)");
            else snprintf(error,error_size,"Offline catalog (network unavailable): %s",category);
        }else if(searching){
            char reason[160];snprintf(reason,sizeof(reason),"%s",error);
            snprintf(error,error_size,"Пошук TMDB недоступний: %.155s",reason);
            return 0;
        }else return 0;
    } else error[0] = 0;
    char total_pages[32]={0};
    json_field(json,json+strlen(json),"total_pages",total_pages,sizeof(total_pages));
    if(has_more && page<atoi(total_pages) && page<500)*has_more=1;
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
        m.series = !strcmp(type, "tv") || (!searching &&
                    (!strcmp(category,"series") ||
                     !strcmp(category,"cartoon_series") ||
                     !strcmp(category,"anime")));
        if (m.id && m.title[0] && strcmp(type, "person") &&
            (!offline_search || contains_title(m.title,query) ||
             contains_title(m.original,query)))items[count++] = m;
        p = end;
    }
    free(json);
    if (!count && !offline_search) snprintf(error, error_size, "No titles found");
    return count;
}

int catalog_load(const char *category, const char *query, Movie *items, int max,
                 char *error, int error_size) {
    return catalog_load_page(category,query,1,items,max,error,error_size,NULL);
}

/* Use UAFlix and UASerials sources. The experimental UAKino.best provider
   is disabled: it produced unreliable search failures on the console. */
static char *fetch_uafresh(const char *upstream, char *error, int error_size) {
    /* Native homebrew does not have browser CORS restrictions. A direct
       request originates from the Switch user's Ukrainian network, unlike
       the Cloudflare worker; UAFlix episode availability is geo-dependent.
       However, video hosts such as Zetvideo can return an HTTP 200 page
       containing an nginx 404 *body* from some regions. For iframe hosts,
       prefer the UAFresh proxy, then fall back to direct access. */
    int direct_first=!strncmp(upstream,"https://uafix.net/",18);
    int video_host=!strncmp(upstream,"https://zetvideo.net/",21) ||
                   !strncmp(upstream,"https://hdvbua.pro/",19) ||
                   !strncmp(upstream,"https://ashdi.vip/",18);
    if(direct_first){
        char *direct=request(upstream,error,error_size);
        if(direct)return direct;
        /* Existing public UAFresh worker remains a normal network fallback. */
    }
    char *encoded = encode(upstream);
    if (!encoded) { snprintf(error, error_size, "URL encoding failed"); return NULL; }
    /* Public proxy only: never read, store, or forward authentication data. */
    size_t length = strlen(encoded) + strlen(UAFRESH_WORKER) + 24;
    char *url = malloc(length);
    if (!url) { free(encoded); return NULL; }
    snprintf(url, length, UAFRESH_WORKER "/proxy?url=%s", encoded);
    free(encoded);
    char *html = request(url, error, error_size);
    free(url);
    if(!html && video_host)return request(upstream,error,error_size);
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
                    }else if(provider==1){
                        /* UASerials search cards put the human title in
                           .th-title, often followed by the English name.
                           Never label a source with its raw numeric slug. */
                        const char *heading=strstr(end,"class=\"th-title");
                        if(heading && heading-end<900 &&
                           strncmp(heading,"class=\"th-title-oname",21)){
                            const char *opening=strchr(heading,'>');
                            if(opening && opening-heading<140){
                                title_start=opening+1;
                                title_end=strchr(title_start,'<');
                                if(!title_end || title_end-title_start>300)
                                    title_start=NULL;
                            }
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
    /* Localized and original titles may differ radically (Lost / Загублені).
       Search each provider in Ukrainian first, then try the original only if
       the localized search returned no matches for that provider. Leave room
       for both providers even when one returns many similar titles. */
    const char *localized=movie->title;
    const char *original=movie->original;
    error[0]=0;
    int provider_limit=max>2?max/2:max;
    search_provider("https://uafix.net", "UAFlix", 0, localized,movie->series,
                    items, &count, provider_limit, error, error_size);
    if(count==0 && original[0] && strcmp(original,localized))
        search_provider("https://uafix.net", "UAFlix", 0, original,movie->series,
                        items, &count, provider_limit, error, error_size);
    int before=count;
    search_provider("https://uaserials.my", "UASerials", 1, localized,movie->series,
                    items, &count, max, error, error_size);
    if(count==before && original[0] && strcmp(original,localized))
        search_provider("https://uaserials.my", "UASerials", 1, original,movie->series,
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
    /* A Playerjs playlist leaf already contains the actual media URL. */
    if((strstr(source->url,".m3u8") || strstr(source->url,".mp4") ||
        strstr(source->url,".mpd")) &&
       (!strncmp(source->url,"https://",8) || !strncmp(source->url,"http://",7))){
        snprintf(stream,(size_t)size,"%s",source->url);
        error[0]=0;
        return 1;
    }
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

int catalog_next_episode(const Episode *items,int count,int current){
    if(!items || current<0 || current>=count)return -1;
    const Episode *now=&items[current];
    int best=-1;
    for(int i=0;i<count;i++){
        const Episode *candidate=&items[i];
        if(candidate->season<now->season ||
           (candidate->season==now->season && candidate->number<=now->number))
            continue;
        if(best<0 || candidate->season<items[best].season ||
           (candidate->season==items[best].season &&
            candidate->number<items[best].number))best=i;
    }
    if(best<0)return -1;
    /* Each dubbing is usually a different streaming URL. Keep the preferred
       voice on advancing to a new episode whenever that variant exists. */
    if(now->voice[0])for(int i=0;i<count;i++)
        if(items[i].season==items[best].season &&
           items[i].number==items[best].number &&
           !strcmp(items[i].voice,now->voice))return i;
    return best;
}

/* Playerjs serials often contain a JSON tree voice -> season -> episode in
   their iframe's `file` field. Unlike UAFlix episode cards, these have no
   /season-N-episode-M/ hrefs (e.g. South Park and many animated shows). */
static const char *json_ws(const char *p){
    while(*p && isspace((unsigned char)*p))p++;
    return p;
}
static const char *json_string(const char *p,char *out,size_t size){
    if(*p!='"')return NULL;
    p++;size_t n=0;
    while(*p && *p!='"'){
        unsigned char c=(unsigned char)*p++;
        if(c=='\\' && *p){
            c=(unsigned char)*p++;
            if(c=='n'||c=='r'||c=='t')c=' ';
            else if(c=='u'){
                for(int i=0;i<4 && isxdigit((unsigned char)*p);i++)p++;
                continue;
            }
        }
        if(n+1<size)out[n++]=(char)c;
    }
    out[n]=0;
    return *p=='"'?p+1:NULL;
}
static const char *json_skip(const char *p,int depth){
    p=json_ws(p);
    if(depth>16 || !*p)return NULL;
    if(*p=='"'){
        p++;while(*p && *p!='"'){
            if(*p=='\\' && p[1])p++;
            p++;
        }
        return *p=='"'?p+1:NULL;
    }
    if(*p=='[' || *p=='{'){
        char end=*p=='['?']':'}';p++;
        while(*(p=json_ws(p)) && *p!=end){
            if(*p==':' || *p==','){p++;continue;}
            p=json_skip(p,depth+1);
            if(!p)return NULL;
        }
        return *p==end?p+1:NULL;
    }
    while(*p && *p!=',' && *p!=']' && *p!='}' && !isspace((unsigned char)*p))p++;
    return p;
}
static int title_number(const char *title){
    while(*title && !isdigit((unsigned char)*title))title++;
    return *title?atoi(title):0;
}
static int is_season_title(const char *title){
    return strstr(title,"Сезон") || strstr(title,"сезон") ||
           strstr(title,"Season") || strstr(title,"season");
}
static int is_episode_title(const char *title){
    return strstr(title,"Серія") || strstr(title,"серія") ||
           strstr(title,"Episode") || strstr(title,"episode");
}
static void playlist_add(Episode *items,int *count,int max,int season,int episode,
                         const char *voice,const char *url){
    if(!url || !*url || *count>=max)return;
    const char *direct=strstr(url,"https://");
    if(!direct)direct=strstr(url,"http://");
    if(!direct || (!strstr(direct,".m3u8") && !strstr(direct,".mp4") &&
                   !strstr(direct,".mpd")))return;
    const char *end=direct;
    while(*end && *end!=',' && *end!='|' && *end!=' ' && *end!='"' &&
          *end!='\'' && end-direct<1023)end++;
    size_t length=(size_t)(end-direct);
    if(length<15 || length>=sizeof(items[0].url))return;
    if(season<1)season=1;
    if(episode<1)episode=1;
    for(int i=0;i<*count;i++)
        if(items[i].season==season && items[i].number==episode &&
           !strcmp(items[i].voice,voice?voice:""))return;
    Episode *entry=&items[(*count)++];
    memset(entry,0,sizeof(*entry));
    entry->season=season;entry->number=episode;
    memcpy(entry->url,direct,length);entry->url[length]=0;
    snprintf(entry->voice,sizeof(entry->voice),"%s",voice && *voice?voice:"Основна");
    snprintf(entry->title,sizeof(entry->title),"Сезон %d · Серія %d",season,episode);
}
static const char *playlist_walk(const char *p,int season,int episode,
                                 const char *voice,int ordinal,int depth,
                                 Episode *items,int *count,int max){
    p=json_ws(p);
    if(depth>16 || !*p || *count>=max)return json_skip(p,depth);
    if(*p=='['){
        p++;int index=0;
        while(*(p=json_ws(p)) && *p!=']'){
            if(*p==','){p++;continue;}
            const char *next=playlist_walk(p,season,episode,voice,++index,depth+1,
                                           items,count,max);
            if(!next || next<=p)return NULL;
            p=next;
        }
        return *p==']'?p+1:NULL;
    }
    if(*p!='{')return json_skip(p,depth);
    const char *cursor=p+1,*folder=NULL;
    char title[192]={0},file[2048]={0};
    while(*(cursor=json_ws(cursor)) && *cursor!='}'){
        if(*cursor==','){cursor++;continue;}
        char key[48];
        cursor=json_string(cursor,key,sizeof(key));
        if(!cursor)return NULL;
        cursor=json_ws(cursor);
        if(*cursor++!=':')return NULL;
        cursor=json_ws(cursor);
        if(!strcmp(key,"title") && *cursor=='"')
            cursor=json_string(cursor,title,sizeof(title));
        else if(!strcmp(key,"file") && *cursor=='"')
            cursor=json_string(cursor,file,sizeof(file));
        else {
            if(!strcmp(key,"folder") && (*cursor=='[' || *cursor=='{'))folder=cursor;
            cursor=json_skip(cursor,depth+1);
        }
        if(!cursor)return NULL;
    }
    if(*cursor!='}')return NULL;
    int s=season,e=episode;
    const char *v=voice;
    int num=title_number(title);
    if(is_season_title(title) && num>0){s=num;e=0;}
    else if(is_episode_title(title) && num>0)e=num;
    else if(depth<=2 && title[0] && !file[0])v=title;
    if(file[0])playlist_add(items,count,max,s,e?e:ordinal,v,file);
    if(folder)playlist_walk(folder,s,e,v,0,depth+1,items,count,max);
    return cursor+1;
}
static int playlist_from_html(const char *html,Episode *items,int max){
    const char *p=html;int count=0;
    while((p=strstr(p,"file"))!=NULL && !count){
        const char *value=json_ws(p+4);
        if(*value!=':' && *value!='='){p+=4;continue;}
        value=json_ws(value+1);
        char *decoded=NULL;
        if(*value=='\'' || *value=='"'){
            char quote=*value++;
            size_t capacity=strlen(value)+1;
            if(capacity>3*1024*1024){p+=4;continue;}
            decoded=malloc(capacity);
            if(!decoded)return 0;
            size_t n=0;
            while(*value && *value!=quote){
                char c=*value++;
                if(c=='\\' && *value){
                    c=*value++;
                    if(c=='n'||c=='r'||c=='t')c=' ';
                }
                decoded[n++]=c;
            }
            decoded[n]=0;
        }else if(*value=='[' || *value=='{'){
            const char *end=json_skip(value,0);
            if(end && (size_t)(end-value)<3*1024*1024){
                decoded=malloc((size_t)(end-value)+1);
                if(!decoded)return 0;
                memcpy(decoded,value,(size_t)(end-value));
                decoded[end-value]=0;
            }
        }
        if(decoded){
            const char *start=json_ws(decoded);
            if(*start=='[' || *start=='{')
                playlist_walk(start,1,0,"Основна",0,0,items,&count,max);
            free(decoded);
        }
        p+=4;
    }
    return count;
}

/* UAFlix keeps earlier episodes on /sezon-N/ pages for many long-running
   serials. The original catalog scanned only the first (usually newest)
   page, so whole seasons were absent from the Switch picker. */
static int uaflix_href(const char *begin,size_t length,char *url,size_t size){
    static const char host[]="https://uafix.net";
    if(length==0 || length>=size-25)return 0;
    if(!strncmp(begin,host,strlen(host)))
        snprintf(url,size,"%.*s",(int)length,begin);
    else if(begin[0]=='/' && (length<2 || begin[1]!='/'))
        snprintf(url,size,"%s%.*s",host,(int)length,begin);
    else if(!strncmp(begin,"//uafix.net/",13))
        snprintf(url,size,"https:%.*s",(int)length,begin);
    else return 0;
    return 1;
}
static void parse_uaflix_episodes(const char *html,const char *root,
                                  Episode *items,int *count,int max,
                                  char season_urls[][1024],int *seasons,int max_seasons){
    const char *p=html;
    while(*count<max && (p=strstr(p,"href="))){
        p+=5;
        if(*p!='"' && *p!='\'')continue;
        char quote=*p++;
        const char *end=strchr(p,quote);
        if(!end)break;
        char url[1024];
        if(uaflix_href(p,(size_t)(end-p),url,sizeof(url)) &&
           !strncmp(url,root,strlen(root))){
            const char *ep=strstr(url+strlen(root),"season-");
            int season=0,number=0;
            if(ep && sscanf(ep,"season-%d-episode-%d/",&season,&number)==2 &&
               season>=1 && season<=100 && number>=1 && number<=1000){
                int exists=0;
                for(int i=0;i<*count;i++)
                    if(items[i].season==season && items[i].number==number)exists=1;
                if(!exists){
                    Episode *entry=&items[(*count)++];
                    memset(entry,0,sizeof(*entry));
                    entry->season=season;entry->number=number;
                    snprintf(entry->url,sizeof(entry->url),"%s",url);
                    snprintf(entry->title,sizeof(entry->title),
                             "Сезон %d · Серія %d",season,number);
                }
            }else if(season_urls && seasons && *seasons<max_seasons){
                const char *season_part=strstr(url+strlen(root),"sezon-");
                int season_number=0;
                if(season_part && sscanf(season_part,"sezon-%d/",&season_number)==1 &&
                   season_number>0 && season_number<=100){
                    int exists=0;
                    for(int i=0;i<*seasons;i++)
                        if(!strcmp(season_urls[i],url))exists=1;
                    if(!exists)snprintf(season_urls[(*seasons)++],1024,"%s",url);
                }
            }
        }
        p=end+1;
    }
}

int uakino_episodes(const Source *source, Episode *items, int max,
                    char *error, int error_size) {
    char *html=fetch_uafresh(source->url,error,error_size);
    if (!html) return 0;
    int count=0;
    if(!strncmp(source->url,"https://uafix.net/serials/",25)){
        char season_urls[40][1024];int season_pages=0;
        parse_uaflix_episodes(html,source->url,items,&count,max,
                              season_urls,&season_pages,40);
        /* Fetch the season archive pages even when the main page lists some
           episodes. Those may only cover the latest season or latest items. */
        for(int i=0;i<season_pages && count<max;i++){
            char *archive=fetch_uafresh(season_urls[i],error,error_size);
            if(!archive)continue;
            parse_uaflix_episodes(archive,source->url,items,&count,max,
                                  NULL,NULL,0);
            free(archive);
        }
    }
    if(!count){
        count=playlist_from_html(html,items,max);
        const char *next=html;
        for(int attempt=0;!count && attempt<4;attempt++){
            char iframe[1024]={0};
            next=find_embed_after(next,iframe,sizeof(iframe));
            if(!next)break;
            char *player_html=fetch_uafresh(iframe,error,error_size);
            if(player_html){
                count=playlist_from_html(player_html,items,max);
                free(player_html);
            }
        }
    }
    free(html);
    if (count) {
        qsort(items,(size_t)count,sizeof(*items),episode_order);
        error[0]=0;
    } else snprintf(error,error_size,"Плеєр не надав доступний перелік серій. Спробуйте інше джерело.");
    return count;
}
