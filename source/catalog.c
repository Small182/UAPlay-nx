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
        snprintf(error, error_size, "HTTP %ld: %s", status, curl_easy_strerror(result));
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
        const char *path = !strcmp(category, "series") ? "trending/tv/week" :
                           !strcmp(category, "top") ? "movie/top_rated" : "trending/movie/week";
        snprintf(url, sizeof(url), "https://api.themoviedb.org/3/%s?api_key=%s&language=uk-UA", path, key);
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

/* The UAFresh JS plugin can't run inside libnx: bridge its HTTP proxy and
   retrieve UAKino candidates without evaluating arbitrary JavaScript. */
int uakino_search(const Movie *movie, Source *items, int max,
                  char *error, int error_size) {
    char *query = encode(movie->original[0] ? movie->original : movie->title);
    if (!query) return 0;
    char target[1024];
    snprintf(target, sizeof(target), "https://uakino.best/index.php?do=search&subaction=search&story=%s", query);
    free(query);
    char *encoded = encode(target);
    if (!encoded) return 0;
    char url[1700];
    snprintf(url, sizeof(url), UAFRESH_WORKER "/proxy?url=%s", encoded);
    free(encoded);
    char *html = request(url, error, error_size);
    if (!html) return 0;
    int count = 0;
    const char *p = html;
    while (count < max && count < UAKINO_MAX && (p = strstr(p, "href=\""))) {
        p += 6;
        const char *end = strchr(p, '"');
        if (!end) break;
        size_t n = (size_t)(end - p);
        if (n > 10 && n < 950 && (strstr(p, "/films/") == p || strstr(p, "https://uakino.best/") == p)) {
            char link[1024];
            memcpy(link, p, n); link[n] = 0;
            int duplicate = 0;
            for (int i = 0; i < count; i++) if (!strcmp(link, items[i].url)) duplicate = 1;
            if (!duplicate) {
                snprintf(items[count].url, sizeof(items[count].url), "%s%s",
                         link[0] == '/' ? "https://uakino.best" : "", link);
                snprintf(items[count].title, sizeof(items[count].title), "UAKino #%d", count + 1);
                count++;
            }
        }
        p = end + 1;
    }
    free(html);
    if (!count) snprintf(error, error_size, "UAKino found no matching pages (or blocked search)");
    return count;
}

int uakino_resolve(const Source *source, char *stream, int size,
                   char *error, int error_size) {
    char *encoded = encode(source->url);
    if (!encoded) return 0;
    char proxy[1800];
    snprintf(proxy, sizeof(proxy), UAFRESH_WORKER "/proxy?url=%s", encoded);
    free(encoded);
    char *html = request(proxy, error, error_size);
    if (!html) return 0;
    /* Only accept an explicit direct video URL; never send an HTML page to mpv. */
    const char *extensions[] = {".m3u8", ".mp4", ".mpd"};
    int found = 0;
    for (int e = 0; e < 3 && !found; e++) {
        const char *ext = strstr(html, extensions[e]);
        while (ext) {
            const char *start = ext;
            while (start > html && ext - start < 1000 && *start != '"' && *start != '\'' && *start != ' ') start--;
            if (*start == '"' || *start == '\'') start++;
            if (!strncmp(start, "https://", 8) || !strncmp(start, "http://", 7)) {
                const char *finish = ext + strlen(extensions[e]);
                while (*finish && *finish != '"' && *finish != '\'' && *finish != '<' && *finish != ' ') finish++;
                if (finish - start < size) {
                    memcpy(stream, start, (size_t)(finish - start));
                    stream[finish - start] = 0;
                    found = 1;
                }
                break;
            }
            ext = strstr(ext + strlen(extensions[e]), extensions[e]);
        }
    }
    free(html);
    if (!found) snprintf(error, error_size, "This UAKino page has no direct playable stream");
    return found;
}
