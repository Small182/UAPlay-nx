#include <assert.h>
#include <stdio.h>
#include <sys/stat.h>
#include "library.h"

/* Run in a fresh temporary working directory. The old data is deliberately
   left in place after migration so users can recover it if needed. */
int main(void){
    mkdir("sdmc:",0777);
    mkdir("sdmc:/switch",0777);
    mkdir("sdmc:/switch/uaflix",0777);
    FILE *old=fopen("sdmc:/switch/uaflix/library.tsv","wb");
    assert(old);
    fputs("19995|0|1|0|0.0|Аватар|Avatar|2009|/poster.jpg\n",old);
    fclose(old);

    Movie avatar={.id=19995,.series=0};
    library_init();
    assert(library_favorite(&avatar));
    FILE *migrated=fopen("sdmc:/switch/uaplay/library.tsv","rb");
    assert(migrated);
    fclose(migrated);
    old=fopen("sdmc:/switch/uaflix/library.tsv","rb");
    assert(old);
    fclose(old);
    library_init();
    assert(library_favorite(&avatar));
    puts("UAPlay favorites survive legacy storage migration: OK");
    return 0;
}
