#include <stdio.h>
#include <string.h>
#include "catalog.h"

static void candidate(Source *source,const char *name){
    snprintf(source->title,sizeof(source->title),"%s",name);
}

static int home_test(void){
    Movie m={0};
    snprintf(m.title,sizeof(m.title),"Нарешті вдома");
    snprintf(m.original,sizeof(m.original),"Home");
    Source s[6]={0};
    candidate(&s[0],"UAFlix: Тролі: Чудовий будинок / Trollie: Home Sweet Home");
    candidate(&s[1],"UAFlix: Свій хлопець / Homeboy");
    candidate(&s[2],"UAFlix: Рідний дім / Home Turf");
    candidate(&s[3],"UASerials: Нарешті вдома");
    candidate(&s[4],"UASerials: Прогулянка Римом");
    candidate(&s[5],"UAFlix: 99 будинків / 99 Homes");
    int n=catalog_rank_sources(&m,s,6,1);
    if(n!=1 || strcmp(s[0].title,"UASerials: Нарешті вдома")){
        fprintf(stderr,"Home mismatch: %d results, first %s\n",n,s[0].title);
        return 1;
    }
    return 0;
}

static int partial_test(void){
    Movie m={0};
    snprintf(m.title,sizeof(m.title),"Загублені");
    snprintf(m.original,sizeof(m.original),"Lost");
    Source s[3]={0};
    candidate(&s[0],"UAFlix: Lost Girl");
    candidate(&s[1],"UASerials: Lost");
    candidate(&s[2],"UAFlix: Amazing Documentary");
    int n=catalog_rank_sources(&m,s,3,1);
    if(n!=1 || strcmp(s[0].title,"UASerials: Lost"))return 2;
    return 0;
}

static int fallback_test(void){
    Movie m={0};
    snprintf(m.title,sizeof(m.title),"Залишитися в живих");
    snprintf(m.original,sizeof(m.original),"Lost");
    Source s[2]={0};
    candidate(&s[0],"UAFlix: Невідома назва");
    candidate(&s[1],"UASerials: Інший переклад");
    int n=catalog_rank_sources(&m,s,2,0);
    return n==2?0:3;
}

int main(void){
    int e=home_test();if(e)return e;
    e=partial_test();if(e)return e;
    e=fallback_test();if(e)return e;
    puts("Source relevance ranking: OK");
    return 0;
}
