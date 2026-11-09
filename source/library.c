#include "library.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define LIBRARY_DIR "sdmc:/switch/uaflix"
#define LIBRARY_FILE LIBRARY_DIR "/library.tsv"
#define LIBRARY_TMP LIBRARY_DIR "/library.tmp"
#define MAX_LIBRARY_ITEMS 256

typedef struct {
    Movie movie;
    int favorite;
    int watched;
    double progress;
} Entry;

static Entry entries[MAX_LIBRARY_ITEMS];
static int total;

static Entry *entry_for(const Movie *movie, int create) {
    for (int i=0; i<total; i++) {
        if (entries[i].movie.id == movie->id &&
            entries[i].movie.series == movie->series) return &entries[i];
    }
    if (!create || total == MAX_LIBRARY_ITEMS) return NULL;
    Entry *new_entry = &entries[total++];
    memset(new_entry,0,sizeof(*new_entry));
    new_entry->movie=*movie;
    return new_entry;
}

static void clean_field(char *out, size_t capacity, const char *in) {
    size_t n=0;
    while (*in && n+1<capacity) {
        unsigned char c=(unsigned char)*in++;
        if (c=='\n' || c=='\r' || c=='\t' || c=='|') c=' ';
        out[n++]=(char)c;
    }
    out[n]=0;
}

static void library_save(void) {
    mkdir("sdmc:/switch",0777);
    mkdir(LIBRARY_DIR,0777);
    FILE *file=fopen(LIBRARY_TMP,"wb");
    if (!file) return;
    for (int i=0;i<total;i++) {
        Entry *e=&entries[i];
        if (!e->favorite && !e->watched) continue;
        char title[192], original[192], poster[256];
        clean_field(title,sizeof(title),e->movie.title);
        clean_field(original,sizeof(original),e->movie.original);
        clean_field(poster,sizeof(poster),e->movie.poster);
        fprintf(file,"%d|%d|%d|%d|%.1f|%s|%s|%s|%s\n",
                e->movie.id,e->movie.series,e->favorite,e->watched,
                e->progress,title,original,e->movie.year,poster);
    }
    if (fclose(file)==0) rename(LIBRARY_TMP,LIBRARY_FILE);
}

void library_init(void) {
    total=0;
    FILE *file=fopen(LIBRARY_FILE,"rb");
    if (!file) return;
    char line[1200];
    while (total<MAX_LIBRARY_ITEMS && fgets(line,sizeof(line),file)) {
        char *fields[9]={0}, *p=line;
        for (int i=0;i<9;i++) {
            fields[i]=p;
            if (i==8) break;
            char *separator=strchr(p,'|');
            if (!separator) { fields[0]=NULL; break; }
            *separator=0;p=separator+1;
        }
        if (!fields[0]) continue;
        Entry *entry=&entries[total++];
        memset(entry,0,sizeof(*entry));
        entry->movie.id=atoi(fields[0]);
        entry->movie.series=atoi(fields[1]);
        entry->favorite=atoi(fields[2]);
        entry->watched=atoi(fields[3]);
        entry->progress=atof(fields[4]);
        snprintf(entry->movie.title,sizeof(entry->movie.title),"%s",fields[5]);
        snprintf(entry->movie.original,sizeof(entry->movie.original),"%s",fields[6]);
        snprintf(entry->movie.year,sizeof(entry->movie.year),"%s",fields[7]);
        snprintf(entry->movie.poster,sizeof(entry->movie.poster),"%s",fields[8]);
        entry->movie.poster[strcspn(entry->movie.poster,"\r\n")]=0;
    }
    fclose(file);
}

int library_favorite(const Movie *movie) {
    Entry *e=entry_for(movie,0);
    return e && e->favorite;
}
void library_toggle_favorite(const Movie *movie) {
    Entry *e=entry_for(movie,1);
    if (e) {e->favorite=!e->favorite;library_save();}
}
void library_mark_watched(const Movie *movie) {
    Entry *e=entry_for(movie,1);
    if (e) {e->watched=1;library_save();}
}
void library_set_progress(const Movie *movie,double seconds) {
    Entry *e=entry_for(movie,1);
    if (e) {e->watched=1;e->progress=seconds>0?seconds:0;library_save();}
}
double library_progress(const Movie *movie) {
    Entry *e=entry_for(movie,0);
    return e?e->progress:0;
}
int library_list(Movie *items,int capacity,int favorites) {
    int count=0;
    for (int i=total-1;i>=0 && count<capacity;i--) {
        if ((favorites && entries[i].favorite) || (!favorites && entries[i].watched))
            items[count++]=entries[i].movie;
    }
    return count;
}
