#ifndef UAPLAY_IPTV_H
#define UAPLAY_IPTV_H

#include <stdbool.h>
#include <stdint.h>
#include "i18n.h"

typedef struct {
    const char *id, *title, *url, *agent, *referrer, *groups, *logo;
    uint32_t genres;
    int country;
} IptvChannel;

typedef struct {
    char *playlist;
    IptvChannel *channels;
    int count;
} IptvCatalog;

typedef struct {
    char code[3];
    UiText name;
} IptvCountry;

enum { IPTV_ALL=-1, IPTV_MAX_COUNTRIES=256, IPTV_MAX_GENRES=32 };
enum { IPTV_OK, IPTV_NETWORK, IPTV_INVALID, IPTV_MEMORY };

const IptvCountry *iptv_countries(int *count);
const UiText *iptv_genres(int *count);
int iptv_country_index(const char *code);
int iptv_load(int country, IptvCatalog *catalog, int *from_cache);
void iptv_favorites_load(IptvCatalog *catalog);
int iptv_favorite_index(const IptvCatalog *favorites,const char *id);
bool iptv_favorite_toggle(IptvCatalog *favorites,const IptvChannel *channel);
void iptv_free(IptvCatalog *catalog);

#endif
