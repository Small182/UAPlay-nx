#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Keep the examples independent of live TMDB and exercise the real parser. */
#include "../source/catalog.c"

static int has_ru(const char *json,const char *field){
    return json_has_ru(json,json+strlen(json),field);
}

int main(void){
    assert(has_ru("{\"origin_country\":[\"UA\",\"RU\"]}","origin_country")==1);
    assert(has_ru("{\"origin_country\":[\"UA\",\"PL\"]}","origin_country")==0);
    assert(has_ru("{\"origin_country\":[]}","origin_country")==0);
    assert(has_ru("{\"title\":\"Film\"}","origin_country")==-1);
    assert(has_ru("{\"production_countries\":[{\"iso_3166_1\":\"RU\",\"name\":\"Russia\"}]}",
                  "production_countries")==1);
    assert(has_ru("{\"production_countries\":[{\"iso_3166_1\":\"UA\",\"name\":\"Ukraine\"}]}",
                  "production_countries")==0);
    puts("TMDB country detection OK");
    return 0;
}
