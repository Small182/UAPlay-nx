#include <assert.h>
#include <stdio.h>

/* Host-side smoke test for the same release-date guard used by TMDB pages. */
#include "../source/catalog.c"

int main(void){
    const char *today="2026-11-10";
    assert(already_released("2026-11-09",today));
    assert(already_released("2026-11-10",today));
    assert(!already_released("2026-11-11",today));
    assert(!already_released("2027-02-14",today));
    assert(!already_released("",today));
    puts("TMDB future-release exclusion OK");
    return 0;
}
