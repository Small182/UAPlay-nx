#include <assert.h>
#include <stdio.h>
#include "../source/catalog.c"

/* Host smoke test; no TMDB key or network connection required.
   gcc -O2 tests/details_metadata_smoke.c source/i18n.c -lcurl -o /tmp/uaplay-details-smoke */
int main(void){
    Movie movie={0};
    movie.id=42;
    catalog_parse_details_json(&movie,
        "{\"id\":42,\"runtime\":127,\"number_of_episodes\":0}");
    assert(movie.details_state==1);
    assert(movie.runtime_minutes==127);
    assert(movie.season_count==0 && movie.episode_count==0);

    Movie series={0};
    series.id=77;
    series.series=1;
    catalog_parse_details_json(&series,
        "{\"id\":77,\"number_of_seasons\":5,\"number_of_episodes\":62}");
    assert(series.details_state==1);
    assert(series.runtime_minutes==0);
    assert(series.season_count==5 && series.episode_count==62);
    puts("TMDB details metadata: PASS");
    return 0;
}
