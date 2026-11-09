#include "posters.h"

#include <SDL.h>
#include <SDL_image.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define POSTER_CACHE_MAX 120
#define MAX_POSTER_BYTES (1024 * 1024)

typedef struct {
    int id;
    GLuint texture;
    unsigned long long last_used;
} PosterEntry;

typedef struct { unsigned char *data; size_t used; } ImageData;
static PosterEntry cache[POSTER_CACHE_MAX];
static int used = 0;
static unsigned long long cache_clock=0;

static size_t receive(char *pointer, size_t size, size_t count, void *ctx) {
    ImageData *image = ctx;
    size_t bytes = size * count;
    if (bytes > MAX_POSTER_BYTES - image->used) return 0;
    void *next = realloc(image->data, image->used + bytes);
    if (!next) return 0;
    image->data = next;
    memcpy(image->data + image->used, pointer, bytes);
    image->used += bytes;
    return bytes;
}

static GLuint download_texture(const char *path) {
    if (!path || path[0] != '/' || strstr(path, "..")) return 0;
    CURL *curl = curl_easy_init();
    if (!curl) return 0;
    char url[512];
    snprintf(url, sizeof(url), "https://image.tmdb.org/t/p/w342%s", path);
    ImageData data = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 4L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 7L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Lampa-nx/0.3");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);
    CURLcode result = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(curl);
    if (result != CURLE_OK || code != 200 || !data.data || !data.used) {
        free(data.data);
        return 0;
    }
    SDL_RWops *rw = SDL_RWFromMem(data.data, (int)data.used);
    SDL_Surface *image = rw ? IMG_Load_RW(rw, 1) : NULL;
    GLuint texture = 0;
    if (image) {
        SDL_Surface *rgba = SDL_ConvertSurfaceFormat(image, SDL_PIXELFORMAT_ABGR8888, 0);
        if (rgba) {
            glGenTextures(1, &texture);
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rgba->w, rgba->h,
                         0, GL_RGBA, GL_UNSIGNED_BYTE, rgba->pixels);
            SDL_FreeSurface(rgba);
        }
        SDL_FreeSurface(image);
    }
    free(data.data);
    return texture;
}

GLuint poster_texture(const Movie *movie, int *budget) {
    if (!movie || !movie->poster[0] || !strcmp(movie->poster, "null")) return 0;
    int identity = movie->series ? -movie->id : movie->id;
    for (int i = 0; i < used; i++) {
        if (cache[i].id == identity) {
            cache[i].last_used=++cache_clock;
            return cache[i].texture;
        }
    }
    if (*budget <= 0) return 0;
    --*budget;
    int full=used==POSTER_CACHE_MAX;
    int slot=full?0:used++;
    if(full){
        for(int i=1;i<used;i++)
            if(cache[i].last_used<cache[slot].last_used)slot=i;
        if(cache[slot].texture)glDeleteTextures(1,&cache[slot].texture);
    }
    PosterEntry *entry = &cache[slot];
    entry->id = identity;
    entry->last_used=++cache_clock;
    entry->texture = download_texture(movie->poster);
    return entry->texture;
}

void posters_destroy(void) {
    for (int i = 0; i < used; i++) {
        if (cache[i].texture) glDeleteTextures(1, &cache[i].texture);
    }
    used = 0;
    cache_clock=0;
}
