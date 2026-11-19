#include <assert.h>
#include <stdio.h>
#include <sys/stat.h>
#include "platform.h"

#include "library.h"

/* Run in a fresh temporary working directory. */
int main(void){
    mkdir("sdmc:",0777);
    mkdir("sdmc:/switch",0777);
    mkdir(UAPLAY_PREVIOUS_DATA_DIR,0777);
    FILE *old=fopen(UAPLAY_PREVIOUS_DATA_DIR "/library.tsv","wb");
    assert(old);
    fputs("19995|0|1|0|0.0|Аватар|Avatar|2009|/poster.jpg\n",old);
    fclose(old);

    Movie avatar={.id=19995,.series=0};
    library_init();
    assert(library_favorite(&avatar));
    FILE *migrated=fopen("sdmc:/switch/uaplay/library.tsv","rb");
    assert(migrated);
    fclose(migrated);
    struct stat info;
    assert(stat(UAPLAY_PREVIOUS_DATA_DIR,&info)!=0);
    library_init();
    assert(library_favorite(&avatar));
    /* An interrupted update may leave only the last complete backup. */
    assert(rename("sdmc:/switch/uaplay/library.tsv",
                  "sdmc:/switch/uaplay/library.bak")==0);
    library_init();
    assert(library_favorite(&avatar));
    /* An existing library wins; the transferred legacy copy remains recoverable. */
    assert(mkdir(UAPLAY_PREVIOUS_DATA_DIR,0777)==0);
    old=fopen(UAPLAY_PREVIOUS_DATA_DIR "/library.bak","wb");
    assert(old && fputs("legacy copy",old)>=0 && fclose(old)==0);
    library_init();
    assert(library_favorite(&avatar));
    assert(stat(UAPLAY_PREVIOUS_DATA_DIR,&info)!=0);
    assert(stat(UAPLAY_DATA_DIR "/migrated-data-1/library.bak",&info)==0);
    puts("UAPlay favorites survive legacy storage migration: OK");
    return 0;
}
