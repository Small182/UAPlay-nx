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
    char *q = query && *query ? encode(query) : NULL;
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

/* In November 2026 UAFresh explicitly excludes the old uakino.best source.
   Its current enabled providers include UAFlix and UASerials. We use the
   plugin's public worker proxy, but execute no remote JavaScript. */
static char *fetch_uafresh(const char *upstream, char *error, int error_size) {
    char *encoded = encode(upstream);
    if (!encoded) { snprintf(error, error_size, "URL encoding failed"); return NULL; }
    size_t length = strlen(encoded) + strlen(UAFRESH_WORKER) + 24;
    char *url = malloc(length);
    if (!url) { free(encoded); return NULL; }
    snprintf(url, length, UAFRESH_WORKER "/proxy?url=%s", encoded);
    free(encoded);
    char *html = request(url, error, error_size);
    free(url);
    return html;
}

static int looks_like_result(const char *url, const char *host, int provider) {
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
        const char *slug_end = strchr(end+1, '/');
        if (!slug_end || slug_end[1] != 0 || slug_end == end+1) return 0;
        for (const char *p=end+1; p<slug_end; p++)
            if (!(isalnum((unsigned char)*p) || *p=='-')) return 0;
        return 1;
    }
    /* UASerials: /2389-avatar-2009.html */
    const char *p = path+1;
    if (!isdigit((unsigned char)*p)) return 0;
    while (isdigit((unsigned char)*p)) p++;
    return *p == '-' && strstr(p, ".html") != NULL;
}

static void search_provider(const char *host, const char *name, int provider,
                            const char *query, Source *items, int *count, int limit,
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
    while (*count < limit && (p = strstr(p, "href=")) != NULL) {
        p += 5;
        if (*p != '"' && *p != '\'') continue;
        char quote = *p++;
        const char *end = strchr(p, quote);
        if (!end) break;
        size_t n = (size_t)(end-p);
        char candidate[1024];
        if (n > 12 && n < sizeof(candidate)) {
            memcpy(candidate, p, n);
            candidate[n]=0;
            if (looks_like_result(candidate, host, provider)) {
                int exists=0;
                for (int i=0; i<*count; i++)
                    if (!strcmp(items[i].url,candidate)) exists=1;
                if (!exists) {
                    Source *dst = &items[(*count)++];
                    snprintf(dst->url,sizeof(dst->url),"%s",candidate);
                    snprintf(dst->title,sizeof(dst->title),"%s: %s",name,candidate+strlen(host)+1);
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
    search_provider("https://uafix.net", "UAFlix", 0, search,
                    items, &count, max, error, error_size);
    search_provider("https://uaserials.my", "UASerials", 1, search,
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

static int find_embed(const char *html, char *url, int size) {
    const char *hosts[]={"https://zetvideo.net/vod/", "https://hdvbua.pro/",
                          "https://ashdi.vip/", "https://tortuga."};
    for (size_t i=0; i<sizeof(hosts)/sizeof(hosts[0]); i++) {
        const char *p=strstr(html,hosts[i]);
        if (!p) continue;
        const char *end=p;
        while (*end && *end!='"' && *end!='\'' && *end!='<' &&
               *end!=' ' && end-p<size-1) end++;
        if (end==p || !*end || end-p>=size) continue;
        memcpy(url,p,(size_t)(end-p));url[end-p]=0;
        if (!strstr(url,"trailer") && !strstr(url,"youtube")) return 1;
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
    if (extract_media(page,stream,size)) { free(page);return 1; }
    char embed[1024];
    int found = find_embed(page,embed,sizeof(embed));
    free(page);
    if (found) {
        char *player = fetch_uafresh(embed,error,error_size);
        if (!player) return 0;
        extract_subtitle(player);
        found = extract_media(player,stream,size);
        free(player);
        if (found) return 1;
    }
    snprintf(error,error_size,"Лише трейлер або немає потоку. Поверніться та виберіть інше джерело.");
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
