#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <stdint.h>
#include <sys/stat.h>
#include <switch.h>
#include <SDL.h>
#include <SDL_ttf.h>
#include <GLES3/gl3.h>
#include <mpv/client.h>
#include <mpv/render_gl.h>
#include <curl/curl.h>
#include "i18n.h"
#include "catalog.h"
#include "posters.h"
#include "library.h"
#include "playback.h"
#include "downloads.h"
#include "subdl.h"
#include "movy_stream.h"
#include "iptv.h"
#include "wallpapers.h"
#include <ctype.h>
#include "update.h"
#include "version.h"

typedef enum { CATALOG, DETAILS, PROVIDERS, SOURCES, PLAYER, SETTINGS,
               SEASONS, EPISODES, TRACK_PICKER, VIDEO_SETTINGS, DOWNLOADS,
               PROVIDER_SETTINGS, SUBDL_PICKER, UPDATE_DIALOG, IPTV_CHANNELS, IPTV_COUNTRIES, IPTV_MORE_COUNTRIES, IPTV_GENRES, WALLPAPERS, APPEARANCE } Page;
static int selected_provider=0;
static int provider_settings_selected=0;
static int download_selected=0;
static unsigned download_confirm=0;
/* Read the real battery percentage at most once every 15 seconds. */
static int battery_service_ready=0,battery_percentage=-1;
static void battery_status_poll(void){
    static Uint32 last_checked=0;
    static int first=1;
    if(!battery_service_ready)return;
    Uint32 now=SDL_GetTicks();
    if(!first && now-last_checked<15000u)return;
    first=0;last_checked=now;
    u32 amount=0;
    if(R_SUCCEEDED(psmGetBatteryChargePercentage(&amount)) && amount<=100)
        battery_percentage=(int)amount;
    else battery_percentage=-1;
}
static int download_season=0, download_episode=0;
static const struct {
    int id;
    UiText name;
} providers[]={
    {1,{"UAFlix","UAFlix"}},{2,{"UASerials","UASerials"}},{3,{"Eneyida","Eneyida"}},
    {4,{"UAFilm","UAFilm"}},{5,{"UAKino","UAKino"}},{6,{"Mikai (аніме)","Mikai (anime)"}},
    {7,{"AnimeON (аніме)","AnimeON (anime)"}},
    {UAKINO_PROVIDER_MOVY,{"Movy (тестове)","Movy (experimental)"}}
};
enum { PROVIDER_COUNT=sizeof(providers)/sizeof(providers[0]),
       MOVY_PROVIDER_BIT=1u<<(UAKINO_PROVIDER_MOVY-1),
       LEGACY_PROVIDER_MASK=(1u<<UAKINO_PROVIDER_MOVY)-1u,
       ALL_PROVIDERS=0x7fu|MOVY_PROVIDER_BIT };
/* Bit positions follow the persisted provider IDs, including the slot
   retired between AnimeON and Movy. */
static unsigned enabled_providers=ALL_PROVIDERS & ~MOVY_PROVIDER_BIT;

static int provider_for_row(int row){
    if(row<0)return -1;
    for(int i=0;i<PROVIDER_COUNT;i++)
        if((enabled_providers & (1u<<(providers[i].id-1))) && row--==0)
            return providers[i].id;
    return -1;
}

static Episode episodes[EPISODES_MAX];
static int episode_count,season_ids[64],season_count,selected_season,selected_episode;
static int active_episode_variant=-1;
static MediaTrack tracks[TRACKS_MAX];
static int track_count,selected_track,subtitle_picker;
#define SOURCE_AUDIO_TRACK_BASE 10000
#define SOURCE_SUBTITLE_TRACK_BASE 20000
#define SUBDL_SEARCH_TRACK (-30000)
static struct {
    SDL_Thread *thread;
    SDL_atomic_t done;
    Movie movie;
    int season,episode,downloading,success,count,selected;
    SubdlSubtitle results[SUBDL_MAX];
    char file[SUBDL_FILE_SIZE],error[256];
} subdl_job;

static int subdl_worker(void *unused){
    (void)unused;
    if(subdl_job.downloading)
        subdl_job.success=subdl_download(&subdl_job.results[subdl_job.selected],
                                         subdl_job.file,sizeof(subdl_job.file),
                                         subdl_job.error,sizeof(subdl_job.error));
    else
        subdl_job.count=subdl_search(&subdl_job.movie,subdl_job.season,subdl_job.episode,
                                     subdl_job.results,subdl_job.error,sizeof(subdl_job.error));
    SDL_AtomicSet(&subdl_job.done,1);
    return 0;
}

static void subdl_start(int downloading){
    subdl_job.downloading=downloading;
    subdl_job.error[0]=0;
    SDL_AtomicSet(&subdl_job.done,0);
    subdl_job.thread=SDL_CreateThread(subdl_worker,"subdl",NULL);
    if(!subdl_job.thread)
        snprintf(subdl_job.error,sizeof(subdl_job.error),"%s",
                 TR("Не вдалося запустити SubDL.","Could not start SubDL."));
}

static int selected_video_option;
/* Only player controls useful on the console: speed, volume, subtitle size,
   subtitle delay and HLS quality. Keep mpv's existing option storage intact. */
static const int visible_video_options[]={0,1,4,5,6};
#define VIDEO_OPTION_COUNT ((int)(sizeof(visible_video_options)/sizeof(visible_video_options[0])))
static double video_options[]={1.0,100.0,0.0,0.0,48.0,0.0,0.0};
static const char *video_props[]={"speed","volume","brightness","contrast",
                                  "sub-font-size","sub-delay","hls-bitrate"};
static const UiText video_titles[]={{"Швидкість","Speed"},{"Гучність","Volume"},{"Яскравість","Brightness"},{"Контраст","Contrast"},
                                   {"Розмір субтитрів","Subtitle size"},{"Затримка субтитрів","Subtitle delay"},{"Якість HLS","HLS quality"}};
static const double video_steps[]={0.25,5,5,5,4,0.25,1};
static const double video_min[]={0.5,0,-100,-100,20,-10,0};
static const double video_max[]={2.0,130,100,100,100,10,4};
/* HLS bitrate ceilings approximate the requested picture height. Exact
   360/480/720/1080p depends on variant resolutions offered by the source. */
static const char *hls_presets[]={"max","800000","1500000","3000000","6000000"};
static const char *hls_labels[]={"","360p","480p","720p","1080p"};

static void rebuild_seasons(void) {
    season_count=0;
    for (int i=0;i<episode_count;i++) {
        int seen=0;
        for(int j=0;j<season_count;j++) if(season_ids[j]==episodes[i].season)seen=1;
        if(!seen && season_count<64)season_ids[season_count++]=episodes[i].season;
    }
    selected_season=selected_episode=0;
}
static int episodes_in_season(int season) {
    int count=0;
    for(int i=0;i<episode_count;i++)if(episodes[i].season==season){
        if(i>0 && episodes[i-1].season==season &&
           episodes[i-1].number==episodes[i].number)continue;
        count++;
    }
    return count;
}
static Episode *selected_episode_item(void) {
    if(selected_season>=season_count)return NULL;
    int season=season_ids[selected_season],nth=0;
    for(int i=0;i<episode_count;i++) if(episodes[i].season==season) {
        if(i>0 && episodes[i-1].season==season &&
           episodes[i-1].number==episodes[i].number)continue;
        if(nth++==selected_episode)return &episodes[i];
    }
    return NULL;
}
static void focus_episode_index(int index){
    if(index<0 || index>=episode_count)return;
    int season=episodes[index].season,number=episodes[index].number;
    for(int i=0;i<season_count;i++)if(season_ids[i]==season){
        selected_season=i;break;
    }
    selected_episode=0;
    for(int i=0;i<episode_count;i++)if(episodes[i].season==season){
        if(i>0 && episodes[i-1].season==season &&
           episodes[i-1].number==episodes[i].number)continue;
        if(episodes[i].number==number)break;
        selected_episode++;
    }
}

static const char *categories[]={"movies","series","films","genre_28","genre_35",
    "genre_18","genre_27","genre_878","genre_16","genre_10749","genre_99",
    "favorites","history","cartoons","cartoon_series","anime"};
static const UiText headings[]={{"Популярні фільми","Popular movies"},{"Популярні серіали","Popular TV shows"},{"Усі фільми","All movies"},
    {"Бойовики","Action"},{"Комедії","Comedy"},{"Драми","Drama"},{"Жахи","Horror"},{"Фантастика","Sci-fi"},{"Мультфільми","Animation"},
    {"Романтика","Romance"},{"Документальні","Documentaries"},{"На майбутнє","Watchlist"},{"Історія","History"},
    {"Мультфільми","Animation"},{"Мультсеріали","Animated series"},{"Аніме","Anime"}};
#define CATEGORY_COUNT ((int)(sizeof(categories)/sizeof(categories[0])))
#define ANIMATION_FIRST 13
#define ANIMATION_LAST 15
static const int home_categories[]={0,1,13,14};
static const UiText home_headings[]={{"Популярні фільми","Popular movies"},{"Популярні серіали","Popular TV shows"},
                                    {"Популярні мультфільми","Popular animated movies"},{"Популярні мультсеріали","Popular animated series"}};
#define HOME_SHELF_COUNT ((int)(sizeof(home_categories)/sizeof(home_categories[0])))
typedef struct {
    Movie items[CATALOG_PAGE_SIZE];
    int count;
    int state; /* 0 unknown, 1 queued, 2 loading, 3 ready */
} HomeShelf;
static HomeShelf home_shelves[HOME_SHELF_COUNT];
static int home_shelf=0,home_cache_initialized=0,home_current_loaded=0;
static int expanded_catalog=0;
/* 0: posters, 1: Watch, 2: Favorite, 3: All,
   5: search, 6: settings, 7...11: main navigation,
   12...14: expanded sorting/back controls, 15: IPTV,
   16...: subsection tabs, 18: clear watch history. */
static int catalog_hero_focus=0;
enum { CATALOG_BACK_FOCUS=14, IPTV_TAB_FOCUS=15 };
static const struct {int focus,x,w;} main_tabs[]={
    {7,193,96},{8,295,95},{9,398,89},{10,490,144},{IPTV_TAB_FOCUS,638,76},{11,720,110}
};
enum { MAIN_TAB_COUNT=sizeof(main_tabs)/sizeof(main_tabs[0]) };

static int main_tab_index(int focus){
    for(int i=0;i<MAIN_TAB_COUNT;i++)if(main_tabs[i].focus==focus)return i;
    return -1;
}

static int main_tab_at(int x){
    for(int i=0;i<MAIN_TAB_COUNT;i++)
        if(x>=main_tabs[i].x && x<main_tabs[i].x+main_tabs[i].w)return main_tabs[i].focus;
    return 0;
}

static int header_focus_step(int focus,int key){
    int tab=main_tab_index(focus);
    if(key==SDL_CONTROLLER_BUTTON_DPAD_LEFT){
        if(focus==6)return 5;
        if(focus==5)return main_tabs[MAIN_TAB_COUNT-1].focus;
        if(tab>0)return main_tabs[tab-1].focus;
    }else if(key==SDL_CONTROLLER_BUTTON_DPAD_RIGHT){
        if(tab>=0)return tab+1<MAIN_TAB_COUNT?main_tabs[tab+1].focus:5;
        if(focus==5)return 6;
    }
    return focus;
}
static int clear_history_confirm=0;
/* 0 popularity, 1 TMDB rating. */
static int catalog_sort_mode=0;
/* Visible order is Rating, Popularity; API modes are Popularity=0, Rating=1. */
static int sort_mode_for_button(int index){
    return index==0?1:0;
}
static int sort_button_for_mode(int mode){
    return mode==1?0:1;
}
/* 0: Watch, 1: Add to favorites (details buttons). */
static int details_button_focus=0;
enum { SETTING_APPEARANCE, SETTING_PLAYER, SETTING_AUTOPLAY,
       SETTING_PROVIDERS, SETTING_LANGUAGE, SETTING_COUNT };
enum { APPEARANCE_THEME, APPEARANCE_ACCENT, APPEARANCE_WALLPAPERS,
       APPEARANCE_DEFAULT, APPEARANCE_COUNT };
enum { SETTINGS_Y=138, SETTINGS_ROW_STEP=81, SETTINGS_ROW_HEIGHT=64 };
static int settings_selected=0,appearance_selected=0,ui_accent_alt=0,source_mode=0;
static int autoplay_episodes=1;
#define NEXT_EPISODE_DELAY_MS 3000u
static int ui_light_theme=0,ui_theme_enabled=1,ui_gradient_pass=0;
#define THEME_PATH "sdmc:/switch/uaplay/theme.txt"
#define THEME_TMP THEME_PATH ".tmp"
#define THEME_BACKUP THEME_PATH ".bak"
static void theme_load(void){
    FILE *f=fopen(THEME_PATH,"r");
    if(!f)f=fopen(THEME_BACKUP,"r");
    if(f){
        ui_light_theme=fgetc(f)=='1';
        int autoplay=fgetc(f);
        if(autoplay=='0')autoplay_episodes=0;
        /* Legacy files only store the theme and optionally autoplay. */
        if(autoplay=='0' || autoplay=='1'){
            unsigned mask;
            if(fscanf(f," %u",&mask)==1 && mask<=LEGACY_PROVIDER_MASK){
                enabled_providers=mask & ALL_PROVIDERS;
                char language[8];
                if(fscanf(f," %7s",language)==1 && !strcmp(language,"en"))
                    ui_set_language(UI_ENGLISH);
                /* Aether used the same bit before the Movy setting existed. */
                char provider_version[8];
                if(fscanf(f," %7s",provider_version)!=1 || strcmp(provider_version,"movy"))
                    enabled_providers&=~MOVY_PROVIDER_BIT;
            }
        }
        fclose(f);
    }
}
static int theme_save(void){
    mkdir("sdmc:/switch",0777);
    mkdir("sdmc:/switch/uaplay",0777);
    FILE *f=fopen(THEME_TMP,"w");
    if(!f)return 0;
    int written=fprintf(f,"%d%d\n%u\n%s\nmovy\n",ui_light_theme,autoplay_episodes,
                        enabled_providers,TR("uk","en"));
    if(fclose(f)!=0 || written<0){
        remove(THEME_TMP);
        return 0;
    }
    /* FAT may reject replacing an existing file with rename(). */
    struct stat previous;
    int had_previous=stat(THEME_PATH,&previous)==0;
    if(had_previous){
        remove(THEME_BACKUP);
        if(rename(THEME_PATH,THEME_BACKUP)!=0){
            remove(THEME_TMP);
            return 0;
        }
    }
    if(rename(THEME_TMP,THEME_PATH)!=0){
        if(had_previous)rename(THEME_BACKUP,THEME_PATH);
        remove(THEME_TMP);
        return 0;
    }
    remove(THEME_BACKUP);
    return 1;
}
static int video_settings_from_menu=0;
static int ui_category=0;
static int tab_for_category(int category){
    if(category==0)return 0;
    if(category==1)return 1;
    if(category>=ANIMATION_FIRST && category<=ANIMATION_LAST)return 3;
    if(category==11 || category==12)return 4;
    return 2;
}
static int category_for_tab(int tab){
    const int destinations[]={0,1,2,ANIMATION_FIRST,11};
    return tab>=0 && tab<5?destinations[tab]:0;
}
static const int film_categories[]={2,3,5,4,10};
static const UiText film_labels[]={{"Усі фільми","All movies"},{"Бойовики","Action"},{"Драми","Drama"},{"Комедії","Comedy"},{"Документальні","Documentaries"}};
#define FILM_TAB_COUNT ((int)(sizeof(film_categories)/sizeof(film_categories[0])))
static int film_tab(int category){
    for(int i=0;i<FILM_TAB_COUNT;i++)if(film_categories[i]==category)return i;
    return 0;
}
static int subsection_count(int category){
    return tab_for_category(category)==2?FILM_TAB_COUNT:
           tab_for_category(category)==3?3:
           tab_for_category(category)==4?2:0;
}
static int category_for_subsection(int category,int index){
    return tab_for_category(category)==2?film_categories[index]:
           tab_for_category(category)==4?11+index:ANIMATION_FIRST+index;
}
static int focused_subsection(int category){
    return 16+(tab_for_category(category)==2?film_tab(category):
               tab_for_category(category)==4?category-11:
               category-ANIMATION_FIRST);
}

/* TMDB returns only 20 titles per page. Fetch the remaining pages on an SDL
   worker so gamepad navigation and drawing never wait for ten HTTP requests. */
typedef struct {
    SDL_Thread *thread;
    SDL_mutex *mutex;
    SDL_cond *cond;
    int quit,requested,generation,request_generation,request_page,request_sort;
    int ready,ready_generation,ready_count,ready_page,ready_has_more;
    int details_requested,details_ready,locale_generation;
    Movie details_request,details_result;
    char details_genre[64];
    char category[48],query[192],ready_error[192];
    Movie ready_items[CATALOG_PAGE_SIZE];
} CatalogLoader;
static CatalogLoader catalog_loader;
static int displayed_genre_id;
static char displayed_genre[64];
static int catalog_next_page=2,catalog_loading=0,catalog_has_more=0;
static int compare_movie_sort(const void *a,const void *b){
    const Movie *left=a,*right=b;
    if(catalog_sort_mode==1){
        if(left->rating<right->rating)return 1;
        if(left->rating>right->rating)return -1;
    }else{
        if(left->popularity<right->popularity)return 1;
        if(left->popularity>right->popularity)return -1;
    }
    return 0;
}

/* Home prefetch and detail lookups share the existing TMDB worker: both use
   its curl connection, so parallel HTTP calls never race on the same handle. */
static int home_work_queued(void){
    for(int i=0;i<HOME_SHELF_COUNT;i++)
        if(home_shelves[i].state==1)return 1;
    return 0;
}

static int catalog_worker(void *opaque){
    CatalogLoader *loader=opaque;
    Movie *batch=calloc(CATALOG_PAGE_SIZE,sizeof(Movie));
    if(!batch)return 0;
    for(;;){
        SDL_LockMutex(loader->mutex);
        while(!loader->quit && !loader->requested &&
              !loader->details_requested && !home_work_queued())
            SDL_CondWait(loader->cond,loader->mutex);
        if(loader->quit){SDL_UnlockMutex(loader->mutex);break;}
        if(loader->details_requested && !(loader->requested && loader->request_page==1)){
            Movie target=loader->details_request;
            int locale_generation=loader->locale_generation;
            loader->details_requested=0;
            SDL_UnlockMutex(loader->mutex);
            char genre[sizeof(loader->details_genre)];
            catalog_fetch_details(&target,genre,sizeof(genre));
            SDL_LockMutex(loader->mutex);
            if(!loader->quit && locale_generation==loader->locale_generation){
                loader->details_result=target;
                snprintf(loader->details_genre,sizeof(loader->details_genre),"%s",genre);
                loader->details_ready=1;
            }
            SDL_UnlockMutex(loader->mutex);
            continue;
        }
        if(!loader->requested){
            int shelf=-1;
            for(int i=0;i<HOME_SHELF_COUNT;i++)
                if(home_shelves[i].state==1){shelf=i;break;}
            if(shelf>=0)home_shelves[shelf].state=2;
            int locale_generation=loader->locale_generation;
            SDL_UnlockMutex(loader->mutex);
            if(shelf<0)continue;
            memset(batch,0,CATALOG_PAGE_SIZE*sizeof(Movie));
            char error[192]={0};
            int count=catalog_load_page_sorted(categories[home_categories[shelf]],
                         "",1,0,batch,CATALOG_PAGE_SIZE,error,sizeof(error),NULL);
            SDL_LockMutex(loader->mutex);
            if(!loader->quit && locale_generation==loader->locale_generation){
                home_shelves[shelf].count=count;
                if(count>0)memcpy(home_shelves[shelf].items,batch,
                                  (size_t)count*sizeof(Movie));
                home_shelves[shelf].state=3;
            }
            SDL_UnlockMutex(loader->mutex);
            continue;
        }
        char category[48],query[192];
        snprintf(category,sizeof(category),"%s",loader->category);
        snprintf(query,sizeof(query),"%s",loader->query);
        int page=loader->request_page,generation=loader->request_generation;
        int sort_mode=loader->request_sort;
        loader->requested=0;
        SDL_UnlockMutex(loader->mutex);
        memset(batch,0,CATALOG_PAGE_SIZE*sizeof(Movie));
        char error[192]={0};
        int more=0;
        int count=catalog_load_page_sorted(category,query,page,sort_mode,batch,
                                           CATALOG_PAGE_SIZE,error,sizeof(error),&more);
        SDL_LockMutex(loader->mutex);
        if(!loader->quit && generation==loader->generation){
            loader->ready=1;
            loader->ready_generation=generation;
            loader->ready_page=page;
            loader->ready_count=count;
            loader->ready_has_more=more;
            snprintf(loader->ready_error,sizeof(loader->ready_error),"%s",error);
            if(count>0)memcpy(loader->ready_items,batch,count*sizeof(Movie));
        }
        SDL_UnlockMutex(loader->mutex);
    }
    free(batch);
    return 0;
}
static void catalog_loader_init(void){
    CatalogLoader *loader=&catalog_loader;
    loader->mutex=SDL_CreateMutex();
    loader->cond=SDL_CreateCond();
    if(loader->mutex && loader->cond)
        loader->thread=SDL_CreateThread(catalog_worker,"tmdb-pages",loader);
}
static void catalog_loader_request(int page,int category,const char *query){
    CatalogLoader *loader=&catalog_loader;
    if(!loader->thread || page<1 || page>500)return;
    SDL_LockMutex(loader->mutex);
    loader->requested=1;
    loader->request_generation=loader->generation;
    loader->request_page=page;
    loader->request_sort=catalog_sort_mode;
    snprintf(loader->category,sizeof(loader->category),"%s",categories[category]);
    snprintf(loader->query,sizeof(loader->query),"%s",query);
    SDL_CondSignal(loader->cond);
    SDL_UnlockMutex(loader->mutex);
    catalog_loading=1;
}
static void catalog_loader_reset(int category,const char *query,int count,int online){
    CatalogLoader *loader=&catalog_loader;
    catalog_next_page=2;
    catalog_loading=0;
    catalog_has_more=online && count>0 &&
        strcmp(categories[category],"favorites") &&
        strcmp(categories[category],"history");
    if(!loader->mutex)return;
    SDL_LockMutex(loader->mutex);
    loader->generation++;
    loader->ready=0;
    loader->requested=0;
    SDL_UnlockMutex(loader->mutex);
    if(catalog_has_more && count<CATALOG_MAX)
        catalog_loader_request(catalog_next_page++,category,query);
}
static void catalog_invalidate_language(int category,const char *query){
    CatalogLoader *loader=&catalog_loader;
    catalog_loader_reset(category,query,0,0);
    home_cache_initialized=0;
    home_current_loaded=0;
    displayed_genre_id=0;
    if(!loader->mutex)return;
    SDL_LockMutex(loader->mutex);
    loader->locale_generation++;
    loader->details_requested=0;
    loader->details_ready=0;
    for(int i=0;i<HOME_SHELF_COUNT;i++){
        home_shelves[i].state=0;
        home_shelves[i].count=0;
    }
    SDL_UnlockMutex(loader->mutex);
}

static void catalog_home_prime(const Movie *movies,int count){
    CatalogLoader *loader=&catalog_loader;
    if(!loader->mutex)return;
    SDL_LockMutex(loader->mutex);
    if(count>CATALOG_PAGE_SIZE)count=CATALOG_PAGE_SIZE;
    home_shelves[home_shelf].count=count;
    if(count>0)memcpy(home_shelves[home_shelf].items,movies,(size_t)count*sizeof(Movie));
    for(int i=0;i<HOME_SHELF_COUNT;i++)home_shelves[i].state=i==home_shelf?3:1;
    home_cache_initialized=1;
    home_current_loaded=1;
    if(loader->cond)SDL_CondSignal(loader->cond);
    SDL_UnlockMutex(loader->mutex);
}
static int catalog_home_copy(int shelf,Movie *movies,int start,int capacity){
    CatalogLoader *loader=&catalog_loader;
    if(!loader->mutex || shelf<0 || shelf>=HOME_SHELF_COUNT)return -1;
    SDL_LockMutex(loader->mutex);
    int count=home_shelves[shelf].state==3?home_shelves[shelf].count:-1;
    if(count>=0){
        count=count>start?count-start:0;
        if(count>capacity)count=capacity;
        if(count>0)memcpy(movies,home_shelves[shelf].items+start,(size_t)count*sizeof(Movie));
    }
    SDL_UnlockMutex(loader->mutex);
    return count;
}
static void catalog_details_request(const Movie *movie){
    CatalogLoader *loader=&catalog_loader;
    if(!loader->thread || !movie || movie->details_state<0 ||
       (movie->details_state>0 && (movie->series || movie->id==displayed_genre_id)))return;
    SDL_LockMutex(loader->mutex);
    loader->details_request=*movie;
    loader->details_requested=1;
    SDL_CondSignal(loader->cond);
    SDL_UnlockMutex(loader->mutex);
}
static void catalog_details_poll(Movie *movies,int count){
    CatalogLoader *loader=&catalog_loader;
    if(!loader->mutex)return;
    SDL_LockMutex(loader->mutex);
    int ready=loader->details_ready;
    Movie result=loader->details_result;
    if(ready && !result.series){
        displayed_genre_id=result.id;
        snprintf(displayed_genre,sizeof(displayed_genre),"%s",loader->details_genre);
    }
    loader->details_ready=0;
    SDL_UnlockMutex(loader->mutex);
    if(!ready)return;
    for(int i=0;i<count;i++)if(movies[i].id==result.id &&
                             movies[i].series==result.series)
        movies[i]=result;
}
static void catalog_loader_poll(Movie **movies,int *count,int *capacity,int chosen,
                                int category,const char *query,char *message,size_t message_size){
    CatalogLoader *loader=&catalog_loader;
    if(!loader->mutex)return;
    int batch_count=0,ready=0,more=0;
    int limit=(expanded_catalog || (query && *query))?
               CATALOG_SEARCH_MAX:CATALOG_MAX;
    SDL_LockMutex(loader->mutex);
    if(loader->ready && loader->ready_generation==loader->generation){
        ready=1;batch_count=loader->ready_count;
        more=loader->ready_has_more;
        if(loader->ready_page==1){
            *count=0;
            snprintf(message,message_size,"%s",loader->ready_error);
        }
        if(*count+batch_count>*capacity && *capacity<limit){
            int next=*capacity+CATALOG_MAX;
            if(next>limit)next=limit;
            Movie *resized=realloc(*movies,(size_t)next*sizeof(Movie));
            if(resized){*movies=resized;*capacity=next;}
            else more=0;
        }
        for(int i=0;i<batch_count && *count<*capacity && *count<limit;i++){
            int duplicate=0;
            for(int k=0;k<*count;k++)
                if((*movies)[k].id==loader->ready_items[i].id &&
                   (*movies)[k].series==loader->ready_items[i].series){duplicate=1;break;}
            if(!duplicate)(*movies)[(*count)++]=loader->ready_items[i];
        }
        loader->ready=0;
    }
    SDL_UnlockMutex(loader->mutex);
    if(ready){
        catalog_loading=0;
        catalog_has_more=more;
    }
    /* Keep the initial 200-item background budget, then advance further only
       when the user scrolls toward the end of the expanded/search catalog. */
    if(catalog_has_more && !catalog_loading && catalog_next_page<=500 &&
       *count<limit && (*count<CATALOG_MAX ||
          ((expanded_catalog || (query && *query)) && *count-chosen<=42)))
        catalog_loader_request(catalog_next_page++,category,query);
}
static void catalog_loader_shutdown(void){
    CatalogLoader *loader=&catalog_loader;
    if(loader->mutex){
        SDL_LockMutex(loader->mutex);
        loader->quit=1;
        if(loader->cond)SDL_CondSignal(loader->cond);
        SDL_UnlockMutex(loader->mutex);
    }
    if(loader->thread)SDL_WaitThread(loader->thread,NULL);
    if(loader->cond)SDL_DestroyCond(loader->cond);
    if(loader->mutex)SDL_DestroyMutex(loader->mutex);
}

static int load_category_mode(int category,const char *query,Movie *movies,char *message,int size,
                              int cached_count) {
    static int previous_category=-1;
    static char previous_query[192];
    int data_category=category==0 && !query[0]?home_categories[home_shelf]:category;
    if(data_category!=previous_category || strcmp(query,previous_query))catalog_sort_mode=0;
    previous_category=data_category;
    snprintf(previous_query,sizeof(previous_query),"%s",query);
    if(category==0 && !query[0] && home_cache_initialized){
        int cached=catalog_home_copy(home_shelf,movies,0,CATALOG_PAGE_SIZE);
        home_current_loaded=cached>=0;
        if(cached<0){
            snprintf(message,(size_t)size,TR("Завантаження добірки...","Loading collection..."));
            cached=0;
        }else message[0]=0;
        catalog_loader_reset(data_category,query,cached,0);
        return cached;
    }
    if(cached_count>0){
        message[0]=0;
        catalog_loader_reset(data_category,query,cached_count,1);
        return cached_count;
    }
    if(cached_count==0 && catalog_loader.thread){
        catalog_loader_reset(data_category,query,0,0);
        catalog_loader_request(1,data_category,query);
        snprintf(message,(size_t)size,"%s",TR("Завантаження каталогу…","Loading catalog…"));
        return 0;
    }
    int count=0;
    if (!strcmp(categories[category],"favorites")) {
        message[0]=0;
        count=library_list_to_watch(movies,CATALOG_LOCAL_MAX);
    }else if (!strcmp(categories[category],"history")) {
        message[0]=0;
        count=library_list(movies,CATALOG_LOCAL_MAX,0);
    }else{
        count=catalog_load_page_sorted(categories[data_category],query,1,catalog_sort_mode,
                                       movies,CATALOG_MAX,message,size,NULL);
    }
    if(category==0 && !query[0])catalog_home_prime(movies,count);
    if(category==11 || category==12)
        count=catalog_filter_countries(movies,count);
    if(count>1 && ((query && *query) || category==11 || category==12))
        qsort(movies,(size_t)count,sizeof(Movie),compare_movie_sort);
    catalog_loader_reset(data_category,query,count,category!=0 && !message[0]);
    return count;
}
static int load_category(int category,const char *query,Movie *movies,char *message,int size){
    return load_category_mode(category,query,movies,message,size,-1);
}
typedef struct {
    GLuint program, vbo, tex, solid_tex;
    GLint tint, shape, gradient, outline, sampler, uv_crop, panel_gradient;
    SDL_Rect clip;
    TTF_Font *font;
    TTF_Font *font_big;
    TTF_Font *font_clock;
    TTF_Font *font_small;
    TTF_Font *font_tiny;
    TTF_Font *scripts[7];
    TTF_Font *scripts_small[7];
    TTF_Font *scripts_big[7];
    TTF_Font *scripts_tiny[7];
    TTF_Font *japanese[4];
} Paint;
static void paint_clip(Paint *p,int x,int y,int w,int h){
    p->clip=(SDL_Rect){x,y,w,h};
    if(w>0 && h>0){
        glEnable(GL_SCISSOR_TEST);
        glScissor(x,720-y-h,w,h);
    }else glDisable(GL_SCISSOR_TEST);
}
static const char *script_fonts[]={"Tamil","Telugu","Devanagari","Malayalam",
                                  "Bengali","Arabic","Thai"};
static TTF_Font *choose_font(Paint *p,const char *s){
    const unsigned char *u=(const unsigned char*)s;
    while(*u){
        unsigned cp=*u++;
        if(cp>=0xC0 && cp<0xE0 && *u){cp=((cp&31)<<6)|(*u++&63);}
        else if(cp>=0xE0 && cp<0xF0 && u[0] && u[1]){
            cp=((cp&15)<<12)|((u[0]&63)<<6)|(u[1]&63);u+=2;
        }else if(cp>=0xF0 && cp<0xF8 && u[0] && u[1] && u[2])u+=3;
        int idx=cp>=0x0B80 && cp<=0x0BFF?0:
                cp>=0x0C00 && cp<=0x0C7F?1:
                cp>=0x0900 && cp<=0x097F?2:
                cp>=0x0D00 && cp<=0x0D7F?3:
                cp>=0x0980 && cp<=0x09FF?4:
                cp>=0x0600 && cp<=0x06FF?5:
                cp>=0x0E00 && cp<=0x0E7F?6:-1;
        if(cp>=0x3040 && cp<=0x30ff || cp>=0x3400 && cp<=0x9fff ||
           cp>=0xff00 && cp<=0xffef){
            int size=p->font==p->font_big?2:p->font==p->font_small?1:
                     p->font==p->font_tiny?3:0;
            if(p->japanese[size])return p->japanese[size];
        }
        if(idx>=0){
            TTF_Font *fallback=p->font==p->font_big?p->scripts_big[idx]:
                p->font==p->font_small?p->scripts_small[idx]:
                p->font==p->font_tiny?p->scripts_tiny[idx]:p->scripts[idx];
            if(fallback)return fallback;
        }
    }
    return p->font;
}

static const char *vs = "attribute vec2 pos;attribute vec2 uv;varying vec2 tc;"
                        "void main(){gl_Position=vec4(pos,0.,1.);tc=uv;}";
static const char *fs = "precision mediump float;varying vec2 tc;"
                        "uniform vec4 tint;uniform vec4 uv_crop;uniform sampler2D image;uniform vec3 shape;"
                        "uniform float panel_gradient;"
                        "uniform float gradient;uniform float outline;"
                        "void main(){vec2 p=tc*shape.xy;vec2 q=abs(p-shape.xy*.5)"
                        "-shape.xy*.5+shape.z;float d=length(max(q,0.0))"
                        "+min(max(q.x,q.y),0.0)-shape.z;"
                        "float a=1.0-smoothstep(-.7,.9,d);"
                        "if(outline>0.0){float inside=1.0-smoothstep(-.7,.9,d+outline);"
                        "a=max(a-inside,0.0);}"
                        "vec3 rgb=tint.rgb;"
                        /* Switch Basic White / Basic Black: gently shaded,
                           neutral surfaces rather than blue-black or beige. */
                        "if(gradient>1.5)rgb=mix(vec3(.957,.960,.965),"
                        "vec3(.902,.907,.914),tc.y);"
                        "else if(gradient>.5)rgb=mix(vec3(.205,.208,.213),"
                        "vec3(.135,.139,.145),tc.y);"
                        "float edge=smoothstep(0.0,0.40,min(tc.x,1.0-tc.x));"
                        "float fade=mix(0.80,1.0,edge)*(0.91+0.09*tc.y);"
                        "float opacity=mix(1.0,fade,panel_gradient);"
                        "gl_FragColor=texture2D(image,uv_crop.xy+tc*uv_crop.zw)*vec4(rgb,tint.a*opacity)*a;}";
static void *proc(void *ctx, const char *name) {
    (void)ctx; return SDL_GL_GetProcAddress(name);
}
static GLuint compile(GLenum type, const char *source) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &source, NULL);
    glCompileShader(s);
    return s;
}
static int paint_init(Paint *p) {
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    GLuint vert=compile(GL_VERTEX_SHADER, vs), frag=compile(GL_FRAGMENT_SHADER, fs);
    p->program=glCreateProgram();
    glAttachShader(p->program,vert); glAttachShader(p->program,frag);
    glBindAttribLocation(p->program,0,"pos");
    glBindAttribLocation(p->program,1,"uv");
    glLinkProgram(p->program);
    glDeleteShader(vert); glDeleteShader(frag);
    GLint linked=0;
    glGetProgramiv(p->program,GL_LINK_STATUS,&linked);
    if (!linked) return 0;
    p->tint=glGetUniformLocation(p->program,"tint");
    p->shape=glGetUniformLocation(p->program,"shape");
    p->gradient=glGetUniformLocation(p->program,"gradient");
    p->outline=glGetUniformLocation(p->program,"outline");
    p->sampler=glGetUniformLocation(p->program,"image");
    p->uv_crop=glGetUniformLocation(p->program,"uv_crop");
    p->panel_gradient=glGetUniformLocation(p->program,"panel_gradient");
    glUseProgram(p->program);
    glUniform1i(p->sampler,0);
    glUniform4f(p->uv_crop,0.f,0.f,1.f,1.f);
    glUniform1f(p->panel_gradient,0.f);
    glGenBuffers(1,&p->vbo);
    glGenTextures(1,&p->tex);
    p->solid_tex=p->tex;
    glBindTexture(GL_TEXTURE_2D,p->tex);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    unsigned char white[4]={255,255,255,255};
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,white);
    if (TTF_Init()) return 0;
    /* Inter has SF/Helvetica-like proportions and includes Ukrainian glyphs. */
    p->font=TTF_OpenFont("romfs:/InterVariable.ttf",22);
    p->font_big=TTF_OpenFont("romfs:/InterVariable.ttf",31);
    p->font_clock=TTF_OpenFont("romfs:/InterVariable.ttf",34);
    p->font_small=TTF_OpenFont("romfs:/InterVariable.ttf",19);
    p->font_tiny=TTF_OpenFont("romfs:/InterVariable.ttf",15);
    for(int i=0;i<7;i++){
        char path[96];snprintf(path,sizeof(path),"romfs:/NotoSans%s.ttf",script_fonts[i]);
        p->scripts[i]=TTF_OpenFont(path,22);
        p->scripts_small[i]=TTF_OpenFont(path,19);
        p->scripts_big[i]=TTF_OpenFont(path,31);
        p->scripts_tiny[i]=TTF_OpenFont(path,15);
    }
    const int jp_sizes[]={22,19,31,15};
    for(int i=0;i<4;i++)
        p->japanese[i]=TTF_OpenFont("romfs:/NotoSansCJKjp-Regular.otf",jp_sizes[i]);
    return p->font != NULL;
}
static int ui_outline_width=0;
static void box_alpha(Paint *p,int x,int y,int w,int h,float r,float g,float b,float a,int radius) {
    float x0=x/640.f-1.f,x1=(x+w)/640.f-1.f;
    float y0=1.f-y/360.f,y1=1.f-(y+h)/360.f;
    float vertices[]={x0,y0,0,0, x1,y0,1,0, x0,y1,0,1,
                      x1,y0,1,0, x1,y1,1,1, x0,y1,0,1};
    glUseProgram(p->program);
    /* Neutral Switch-inspired surfaces. Distinctly colored accents and
       bright buttons keep their original colors. This runs only for flat UI
       rectangles, never movie posters, textures or video overlays. */
    if(ui_theme_enabled && ui_light_theme && p->tex==p->solid_tex && !ui_gradient_pass){
        float top=fmaxf(r,fmaxf(g,b)),bottom=fminf(r,fminf(g,b));
        if(top<=.55f && top-bottom<=.22f){
            float v=.2126f*r+.7152f*g+.0722f*b;
            r=g=b=fminf(.992f,.93f+.24f*v);
        }else{r=1.f-r;g=1.f-g;b=1.f-b;}
    }else if(ui_theme_enabled && !ui_light_theme &&
             p->tex==p->solid_tex && !ui_gradient_pass){
        float top=fmaxf(r,fmaxf(g,b)),bottom=fminf(r,fminf(g,b));
        if(top<=.55f && top-bottom<=.22f){
            float v=.2126f*r+.7152f*g+.0722f*b;
            r=g=b=.19f+.52f*v;
        }
    }
    /* Panels become translucent when a wallpaper is selected. The test for
       solid_tex deliberately leaves text glyphs, posters, and previews opaque. */
    int soft_panel=ui_theme_enabled && wallpapers_texture() &&
        p->tex==p->solid_tex && !ui_gradient_pass &&
        !ui_outline_width && w>=65 && h>=30;
    if(soft_panel)a*=ui_light_theme?.43f:.67f;
    /* Opacity falls smoothly toward the sides and top; text, previews and
       posters remain fully opaque because they use their own textures. */
    glUniform1f(p->panel_gradient,soft_panel?1.f:0.f);
    glUniform4f(p->tint,r,g,b,a);
    glUniform3f(p->shape,(float)w,(float)h,(float)radius);
    glUniform1f(p->gradient,ui_gradient_pass?(ui_light_theme?2.f:1.f):0.f);
    glUniform1f(p->outline,(float)ui_outline_width);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D,p->tex);
    glBindBuffer(GL_ARRAY_BUFFER,p->vbo);
    glBufferData(GL_ARRAY_BUFFER,sizeof(vertices),vertices,GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);glEnableVertexAttribArray(1);
    glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,4*sizeof(float),(void*)0);
    glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,4*sizeof(float),(void*)(2*sizeof(float)));
    glDrawArrays(GL_TRIANGLES,0,6);
    /* Nintendo Switch Basic White: a restrained 1px graphite outline for
       buttons, input fields and panels. Avoid icons, full-screen fills,
       posters and the separate turquoise focus ring. Reuse this geometry
       instead of allocating another texture or buffer. */
    if(ui_theme_enabled && ui_light_theme && p->tex==p->solid_tex &&
       !ui_gradient_pass && !ui_outline_width &&
       radius>=10 && w>=52 && h>=38){
        glUniform1f(p->outline,1.2f);
        glUniform4f(p->tint,.27f,.28f,.30f,.46f);
        glDrawArrays(GL_TRIANGLES,0,6);
    }
}
/* Center-crop wallpaper thumbnails with texture coordinates while keeping
   rounded card corners in local quad coordinates (no aspect distortion). */
static void wallpaper_image_cover(Paint *p,GLuint image,int image_w,int image_h,
                                  int x,int y,int w,int h,int corner_radius){
    if(!image || image_w<1 || image_h<1 || w<1 || h<1)return;
    float u=0.f,v=0.f,uw=1.f,vh=1.f;
    if((long long)image_w*h>(long long)image_h*w){
        uw=(float)((double)image_h*w/((double)image_w*h));
        u=(1.f-uw)*.5f;
    }else{
        vh=(float)((double)image_w*h/((double)image_h*w));
        v=(1.f-vh)*.5f;
    }
    GLuint old=p->tex;
    p->tex=image;
    glUseProgram(p->program);
    glUniform4f(p->uv_crop,u,v,uw,vh);
    box_alpha(p,x,y,w,h,1.f,1.f,1.f,1.f,corner_radius);
    glUniform4f(p->uv_crop,0.f,0.f,1.f,1.f);
    p->tex=old;
}
static void box(Paint *p,int x,int y,int w,int h,float r,float g,float b) {
    box_alpha(p,x,y,w,h,r,g,b,1.f,w>100 && h>40?14:5);
}

/* Text labels used to allocate, upload and delete a GL texture on every
   frame. Keep an LRU of white glyph masks instead: colors are applied when
   drawing, so the same label also works in both UI themes. */
#define TEXT_CACHE_ENTRIES 256
#define TEXT_CACHE_BYTES (12u * 1024u * 1024u)
typedef struct {
    char *label;
    TTF_Font *font;
    GLuint texture;
    int width,height;
    size_t bytes;
    unsigned long long stamp;
} TextTexture;
static TextTexture text_cache[TEXT_CACHE_ENTRIES];
static size_t text_cache_bytes;
static unsigned long long text_cache_clock;

static void text_cache_release(int slot){
    TextTexture *entry=&text_cache[slot];
    if(entry->texture)glDeleteTextures(1,&entry->texture);
    free(entry->label);
    if(entry->bytes<=text_cache_bytes)text_cache_bytes-=entry->bytes;
    memset(entry,0,sizeof(*entry));
}
static void text_cache_destroy(void){
    for(int i=0;i<TEXT_CACHE_ENTRIES;i++)text_cache_release(i);
}
static int text_cache_lru(void){
    int oldest=0;
    for(int i=0;i<TEXT_CACHE_ENTRIES;i++){
        if(!text_cache[i].label)return i;
        if(text_cache[i].stamp<text_cache[oldest].stamp)oldest=i;
    }
    return oldest;
}
static int text_cache_oldest_occupied(void){
    int oldest=-1;
    for(int i=0;i<TEXT_CACHE_ENTRIES;i++)
        if(text_cache[i].label &&
           (oldest<0 || text_cache[i].stamp<text_cache[oldest].stamp))oldest=i;
    return oldest;
}
static GLuint create_glyph_texture(const SDL_Surface *rgba){
    GLuint texture=0;
    glGenTextures(1,&texture);
    if(!texture)return 0;
    glBindTexture(GL_TEXTURE_2D,texture);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,rgba->w,rgba->h,0,
                 GL_RGBA,GL_UNSIGNED_BYTE,rgba->pixels);
    return texture;
}
static void text_color(Paint *p,const char *str,int x,int y,int limit,
                       float r,float g,float b) {
    if (!str || !*str) return;
    if(ui_theme_enabled){
        float top=fmaxf(r,fmaxf(g,b)),bottom=fminf(r,fminf(g,b));
        if(top-bottom<=.22f){
            float tone=.2126f*r+.7152f*g+.0722f*b;
            r=g=b=ui_light_theme?1.f-tone:tone;
        }else if(ui_light_theme){r=1.f-r;g=1.f-g;b=1.f-b;}
    }
    TTF_Font *font=choose_font(p,str);
    if(!font)return;
    TextTexture *found=NULL;
    for(int i=0;i<TEXT_CACHE_ENTRIES;i++){
        TextTexture *entry=&text_cache[i];
        if(entry->font==font && entry->label && !strcmp(entry->label,str)){
            found=entry;break;
        }
    }
    GLuint transient_texture=0;
    int width=0,height=0;
    if(found){
        found->stamp=++text_cache_clock;
        width=found->width;height=found->height;
    }else{
        SDL_Color color={255,255,255,255};
        SDL_Surface *surface=TTF_RenderUTF8_Blended(font,str,color);
        if(!surface)return;
        SDL_Surface *rgba=SDL_ConvertSurfaceFormat(surface,SDL_PIXELFORMAT_ABGR8888,0);
        SDL_FreeSurface(surface);
        if(!rgba)return;
        width=rgba->w;height=rgba->h;
        size_t bytes=(size_t)width*(size_t)height*4u;
        if(bytes>0 && bytes<=TEXT_CACHE_BYTES){
            int slot=text_cache_lru();
            text_cache_release(slot);
            while(text_cache_bytes+bytes>TEXT_CACHE_BYTES){
                int victim=text_cache_oldest_occupied();
                if(victim<0)break;
                text_cache_release(victim);
            }
            char *copy=malloc(strlen(str)+1);
            if(copy){
                strcpy(copy,str);
                TextTexture *entry=&text_cache[slot];
                entry->texture=create_glyph_texture(rgba);
                if(entry->texture){
                    entry->label=copy;entry->font=font;
                    entry->width=width;entry->height=height;
                    entry->bytes=bytes;entry->stamp=++text_cache_clock;
                    text_cache_bytes+=bytes;
                    found=entry;
                }else free(copy);
            }
        }
        if(!found)transient_texture=create_glyph_texture(rgba);
        SDL_FreeSurface(rgba);
    }
    GLuint texture=found?found->texture:transient_texture;
    if(!texture)return;
    GLuint previous=p->tex;p->tex=texture;
    /* Never shrink a long title: clip it at the card edge instead. */
    SDL_Rect clip=p->clip;
    if(limit>0){
        SDL_Rect label={x,y,limit,height},visible=label;
        if(clip.w && !SDL_IntersectRect(&clip,&label,&visible)){
            p->tex=previous;
            if(transient_texture)glDeleteTextures(1,&transient_texture);
            return;
        }
        paint_clip(p,visible.x,visible.y,visible.w,visible.h);
    }
    box_alpha(p,x,y,width,height,r,g,b,1.f,0);
    if(limit>0)paint_clip(p,clip.x,clip.y,clip.w,clip.h);
    p->tex=previous;
    if(transient_texture)glDeleteTextures(1,&transient_texture);
}
static void text(Paint *p,const char *str,int x,int y,int limit){
    text_color(p,str,x,y,limit,1,1,1);
}
static void headline(Paint *p,const char *str,int x,int y,int limit){
    TTF_Font *normal=p->font;
    if(p->font_big)p->font=p->font_big;
    text(p,str,x,y,limit);
    p->font=normal;
}
/* A single 8-point scale keeps every screen aligned and touch targets stable. */
enum { UI_EDGE=32, UI_FOOTER_TOP=668, UI_ROW_Y=130,
       UI_ROW_HEIGHT=50, UI_ROW_STEP=58, UI_ROWS=8 };
enum { POSTER_COLUMNS=6, POSTER_ROW_Y=389, POSTER_ROW_BOTTOM=661,
       POSTER_GRID_Y=153, LIBRARY_GRID_Y=162, POSTER_GRID_BOTTOM=613 };
enum { POSTER_STEP=201, GRID_ITEMS=2*POSTER_COLUMNS, GRID_PAGE_STEP=512,
       SHELF_STEP=288, SCROLL_MS=220, PRESS_MS=150, PRESS_PREVIEW_MS=46 };

typedef enum { TOUCH_NONE, TOUCH_TAP, SWIPE_LEFT, SWIPE_RIGHT,
               SWIPE_UP, SWIPE_DOWN } TouchAction;
typedef struct {
    SDL_TouchID device;
    SDL_FingerID finger;
    Page page;
    int x,y,dx,dy,active,moved,axis;
} TouchGesture;

static TouchAction touch_gesture(TouchGesture *gesture,const SDL_TouchFingerEvent *event,
                                 Page page,int x,int y){
    if(event->type==SDL_FINGERDOWN && !gesture->active){
        *gesture=(TouchGesture){.device=event->touchId,.finger=event->fingerId,
                                .page=page,.x=x,.y=y,.active=1};
        return TOUCH_NONE;
    }
    if(!gesture->active || gesture->device!=event->touchId ||
       gesture->finger!=event->fingerId)return TOUCH_NONE;

    int dx=gesture->dx=x-gesture->x,dy=gesture->dy=y-gesture->y;
    int ax=abs(dx),ay=abs(dy);
    if(ax>16 || ay>16)gesture->moved=1;
    if(!gesture->axis && gesture->moved){
        if(4*ax>=5*ay)gesture->axis=1;
        else if(4*ay>=5*ax)gesture->axis=2;
    }
    if(event->type!=SDL_FINGERUP)return TOUCH_NONE;
    gesture->active=0;
    if(!gesture->moved)return TOUCH_TAP;
    if(gesture->axis==1 && ax>=52)return dx<0?SWIPE_LEFT:SWIPE_RIGHT;
    if(gesture->axis==2 && ay>=52)return dy<0?SWIPE_UP:SWIPE_DOWN;
    return TOUCH_NONE;
}

static int swipe_page_selection(int selected,int count,int page_size,int direction){
    if(!count)return 0;
    if(selected>=count)selected=count-1;
    int first=selected/page_size*page_size;
    int next=first+direction*page_size;
    return next>=0 && next<count?next:selected;
}

static int poster_row_start(int chosen,int is_home){
    return is_home && chosen>=POSTER_COLUMNS-1?
           chosen-(POSTER_COLUMNS-1):chosen/POSTER_COLUMNS*POSTER_COLUMNS;
}

typedef struct {
    int ready,dragging;
    float position,from,target,drag_origin;
    Uint32 started_at;
} ScrollMotion;
static ScrollMotion content_motion,shelf_motion;
static Uint32 ui_now;
static int ui_animating,ui_hold_scroll;
static const TouchGesture *ui_touch;

static int scroll_offset(ScrollMotion *motion,int target,const int *drag,int limit,int max_step){
    if(!motion->ready){
        *motion=(ScrollMotion){.ready=1,.position=target,.from=target,.target=target};
    }
    if(ui_hold_scroll){
        motion->from=motion->position;
        motion->started_at=ui_now;
        return (int)lroundf(motion->position);
    }
    if(drag){
        if(!motion->dragging)motion->drag_origin=motion->position;
        float step=fmaxf(-max_step,fminf(max_step,-*drag));
        float position=motion->drag_origin+step;
        /* A short resistance at either edge also makes failed swipes settle smoothly. */
        float bounded=fmaxf(0.f,fminf(limit,position));
        motion->position=bounded+fmaxf(-32.f,fminf(32.f,(position-bounded)*.2f));
        motion->dragging=1;
        ui_animating=1;
    }else{
        if(motion->dragging || motion->target!=target){
            motion->from=motion->position;
            motion->target=target;
            motion->started_at=ui_now;
            motion->dragging=0;
        }
        float t=fminf(1.f,(ui_now-motion->started_at)/(float)SCROLL_MS);
        float remaining=1.f-t;
        motion->position=motion->target+(motion->from-motion->target)*remaining*remaining*remaining;
        if(t<1.f && motion->position!=motion->target)ui_animating=1;
    }
    return (int)lroundf(motion->position);
}

static const int *touch_drag(Page page,int axis,int top,int bottom){
    const TouchGesture *touch=ui_touch;
    if(!touch || !touch->active || touch->page!=page || touch->axis!=axis ||
       touch->x<UI_EDGE || touch->x>=1280-UI_EDGE || touch->y<top || touch->y>=bottom)
        return NULL;
    return axis==1?&touch->dx:&touch->dy;
}

static void text_font(Paint *p,const char *s,int x,int y,int limit,
                      TTF_Font *font,float r,float g,float b){
    TTF_Font *prev=p->font;
    if(font)p->font=font;
    text_color(p,s,x,y,limit,r,g,b);
    p->font=prev;
}
static void text_small(Paint *p,const char *s,int x,int y,int limit){
    text_font(p,s,x,y,limit,p->font_small,1,1,1);
}
static void text_muted(Paint *p,const char *s,int x,int y,int limit){
    text_font(p,s,x,y,limit,p->font_small,.70f,.75f,.81f);
}
static void text_tiny(Paint *p,const char *s,int x,int y,int limit){
    text_font(p,s,x,y,limit,p->font_tiny,.78f,.83f,.88f);
}
/* Avoid repeated HarfBuzz shaping for identical labels every frame.
   Fixed capacity and bounded allocations prevent unbounded memory growth. */
#define MEASURE_CACHE_ENTRIES 512
typedef struct { TTF_Font *font; unsigned hash; char *label; int width,height; } MeasureEntry;
static MeasureEntry measure_cache[MEASURE_CACHE_ENTRIES];
static unsigned measure_hash(const char *label){
    unsigned hash=2166136261u;
    for(const unsigned char *p=(const unsigned char*)label;*p;p++)
        hash=(hash^*p)*16777619u;
    return hash;
}
static void cached_measure(TTF_Font *font,const char *label,int *w,int *h){
    *w=*h=0;
    if(!font || !label)return;
    unsigned hash=measure_hash(label);
    MeasureEntry *e=&measure_cache[(hash^((unsigned)(uintptr_t)font>>4))%MEASURE_CACHE_ENTRIES];
    if(e->font==font && e->hash==hash && e->label && !strcmp(e->label,label)){
        *w=e->width;*h=e->height;return;
    }
    TTF_SizeUTF8(font,label,w,h);
    char *copy=malloc(strlen(label)+1);
    if(!copy)return;
    strcpy(copy,label);
    free(e->label);
    *e=(MeasureEntry){font,hash,copy,*w,*h};
}
static void measure_cache_destroy(void){
    for(int i=0;i<MEASURE_CACHE_ENTRIES;i++){
        free(measure_cache[i].label);
        measure_cache[i].label=NULL;
    }
}
static int measure(Paint *p,const char *s,TTF_Font *font){
    TTF_Font *prev=p->font;p->font=font;
    int w=0,h=0;
    cached_measure(choose_font(p,s),s,&w,&h);
    p->font=prev;
    return w;
}
static int text_height(Paint *p,const char *s,TTF_Font *font){
    TTF_Font *previous=p->font;
    p->font=font;
    int w=0,h=0;
    cached_measure(choose_font(p,s),s,&w,&h);
    p->font=previous;
    return h;
}
static void text_center(Paint *p,const char *s,int x,int y,int width,TTF_Font *font,
                        float r,float g,float b){
    int w=measure(p,s,font);
    int left=x+(width-w)/2;
    if(left<x)left=x;
    text_font(p,s,left,y,width-(left-x),font,r,g,b);
}
static void text_center_button(Paint *p,const char *s,int x,int y,int width,int height,
                               TTF_Font *font,float r,float g,float b){
    int glyph_height=text_height(p,s,font);
    int top=y+(height-glyph_height)/2;
    text_center(p,s,x,top,width,font,r,g,b);
}
/* Word-based wrapping avoids slicing a UTF-8 code point into replacement boxes. */
static void wrapped(Paint *p,const char *s,int x,int y,int width,
                    int max_lines,int line_height,TTF_Font *font,
                    float r,float g,float b){
    if(!s || !*s)return;
    char copy[1024],line[512]={0};
    snprintf(copy,sizeof(copy),"%s",s);
    char *save=NULL,*word=strtok_r(copy," \n\t",&save);
    int row=0;
    while(word && row<max_lines){
        char test[512];
        snprintf(test,sizeof(test),"%s%s%s",line,line[0]?" ":"",word);
        if(line[0] && measure(p,test,font)>width){
            text_font(p,line,x,y+row*line_height,width,font,r,g,b);
            row++;line[0]=0;
            if(row>=max_lines)break;
            snprintf(line,sizeof(line),"%s",word);
        }else snprintf(line,sizeof(line),"%s",test);
        word=strtok_r(NULL," \n\t",&save);
    }
    if(row<max_lines && line[0])text_font(p,line,x,y+row*line_height,width,font,r,g,b);
}
static void summary(Paint *p,const char *description,int x,int y,int width){
    wrapped(p,description,x,y,width,3,28,p->font_small,.72f,.77f,.82f);
}
static void card_title(Paint *p,const char *title,int x,int y,int width){
    wrapped(p,title,x,y,width,2,24,p->font_small,.95f,.96f,.98f);
}
static void accent(float *r,float *g,float *b){
    if(ui_light_theme){
        /* Pre-inverted Switch blue; actual output approximately #4465D5. */
        *r=.73f;*g=.60f;*b=.16f;
        return;
    }
    /* Basic Black uses mint/turquoise for active items. */
    *r=ui_accent_alt?.23f:.24f;
    *g=ui_accent_alt?.68f:.84f;
    *b=ui_accent_alt?.88f:.73f;
}
static SDL_Rect ui_focus_rect,ui_press_rect;
static SDL_Point ui_press_point;
static Page ui_press_page;
static Uint32 ui_press_at;
static int ui_press_active,ui_press_is_touch;
static Page ui_draw_page;

static float press_amount(int x,int y,int w,int h){
    SDL_Rect rect={x,y,w,h};
    if(ui_touch && ui_touch->active && !ui_touch->moved && ui_touch->page==ui_draw_page &&
       SDL_PointInRect(&(SDL_Point){ui_touch->x,ui_touch->y},&rect))
        return 1.f;
    if(!ui_press_active || ui_draw_page!=ui_press_page)return 0.f;
    if(ui_press_is_touch ? !SDL_PointInRect(&ui_press_point,&rect) :
       !SDL_RectEquals(&rect,&ui_press_rect))return 0.f;
    Uint32 elapsed=ui_now-ui_press_at;
    return elapsed<PRESS_MS?1.f-elapsed/(float)PRESS_MS:0.f;
}

static void press_preview(Paint *p,int x,int y,int w,int h,int radius){
    float amount=press_amount(x,y,w,h);
    if(amount<=0.f)return;
    ui_animating=1;
    ui_outline_width=2;
    box_alpha(p,x+2,y+2,w-4,h-4,.22f,.71f,.86f,.85f*amount,radius);
    ui_outline_width=0;
}

static const int player_button_centers[]={354,476,598,832,954,1076};
static int pressed_player_button(int key,SDL_Rect *rect){
    const int keys[]={SDL_CONTROLLER_BUTTON_DPAD_LEFT,SDL_CONTROLLER_BUTTON_A,
                      SDL_CONTROLLER_BUTTON_DPAD_RIGHT,SDL_CONTROLLER_BUTTON_BACK,
                      SDL_CONTROLLER_BUTTON_Y,SDL_CONTROLLER_BUTTON_X};
    for(int i=0;i<6;i++)if(keys[i]==key){
        *rect=(SDL_Rect){player_button_centers[i]-38,610,76,58};
        return 1;
    }
    return 0;
}

static void selection_frame(Paint *p,int x,int y,int w,int h){
    /* A single rounded SDF ring tracks the underlying button/card corners.
       Separate rectangular strokes left square joints on the Switch UI. */
    float r=ui_light_theme?.69f:.20f;
    float g=ui_light_theme?.13f:.67f;
    float b=ui_light_theme?.20f:.85f;
    int radius=h>=60?18:h>=40?17:12;
    ui_outline_width=3;
    float opacity=.94f+.06f*sinf(ui_now*.005f);
    ui_focus_rect=(SDL_Rect){x,y,w,h};
    box_alpha(p,x-3,y-3,w+6,h+6,r,g,b,opacity,radius);
    ui_outline_width=0;
}
static void button(Paint *p,const char *label,int x,int y,int w,int h,int primary){
    int inset=(int)lroundf(3.f*press_amount(x,y,w,h));
    if(inset)ui_animating=1;
    x+=inset;y+=inset;w-=2*inset;h-=2*inset;
    float secondary=ui_light_theme?.11f:.15f;
    box_alpha(p,x,y,w,h,primary?.89f:secondary,primary?.91f:secondary,
              primary?.94f:secondary,1.f,14);
    text_center_button(p,label,x,y,w,h,p->font_small,
                       primary?.08f:.95f,primary?.10f:.96f,primary?.13f:.98f);
}
/* Solid vector polygons for crisp icons, independent of font glyph support. */
static void polygon(Paint *p,const float *points,int count,float r,float g,float b){
    if(count<3 || count>60)return;
    float vertices[(60+2)*4];
    float cx=0,cy=0;
    for(int i=0;i<count;i++){cx+=points[2*i];cy+=points[2*i+1];}
    cx/=count;cy/=count;
    vertices[0]=cx/640.f-1.f;vertices[1]=1.f-cy/360.f;
    vertices[2]=vertices[3]=.5f;
    for(int i=0;i<=count;i++){
        int j=i%count;
        vertices[(i+1)*4]=points[j*2]/640.f-1.f;
        vertices[(i+1)*4+1]=1.f-points[j*2+1]/360.f;
        vertices[(i+1)*4+2]=vertices[(i+1)*4+3]=.5f;
    }
    if(ui_theme_enabled && ui_light_theme){r=1.f-r;g=1.f-g;b=1.f-b;}
    glUseProgram(p->program);
    glUniform4f(p->tint,r,g,b,1.f);
    glUniform3f(p->shape,100.f,100.f,0.f);
    glUniform1f(p->gradient,0.f);
    glUniform1f(p->outline,0.f);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D,p->solid_tex);
    glBindBuffer(GL_ARRAY_BUFFER,p->vbo);
    glBufferData(GL_ARRAY_BUFFER,(count+2)*4*sizeof(float),vertices,GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);glEnableVertexAttribArray(1);
    glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,4*sizeof(float),(void*)0);
    glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,4*sizeof(float),(void*)(2*sizeof(float)));
    glDrawArrays(GL_TRIANGLE_FAN,0,count+2);
}
static void search_icon(Paint *p,int cx,int cy,float bg_r,float bg_g,float bg_b){
    const float fg=.96f;
    box_alpha(p,cx-11,cy-11,22,22,fg,fg,fg,1.f,11);
    box_alpha(p,cx-7,cy-7,14,14,bg_r,bg_g,bg_b,1.f,7);
    const float handle[]={cx+7,cy+7,cx+11,cy+3,cx+20,cy+12,cx+16,cy+16};
    polygon(p,handle,4,fg,fg,fg);
}
/* Eight sturdy teeth + circular opening, matching the supplied gear image. */
static void gear_icon(Paint *p,int cx,int cy,float bg_r,float bg_g,float bg_b){
    const float fg=.96f,pi=3.14159265f;
    float teeth[8*6*2];
    for(int k=0;k<8;k++){
        const float angles[]={-.37f,-.22f,-.15f,.15f,.22f,.37f};
        const float radii[]={10.5f,10.5f,15.f,15.f,10.5f,10.5f};
        for(int j=0;j<6;j++){
            float angle=k*pi/4.f+angles[j];
            teeth[(k*6+j)*2]=cx+cosf(angle)*radii[j];
            teeth[(k*6+j)*2+1]=cy+sinf(angle)*radii[j];
        }
    }
    polygon(p,teeth,48,fg,fg,fg);
    box_alpha(p,cx-5,cy-5,10,10,bg_r,bg_g,bg_b,1.f,5);
}
/* Battery indicators: compact vertical on the catalog header, compact
   horizontal inside transient player controls. Neither displays digits. */
static void battery_fill_color(float *r,float *g,float *b){
    *r=.22f;*g=.88f;*b=.49f;
    if(battery_percentage>=0 && battery_percentage<=15){
        *r=.94f;*g=.31f;*b=.29f;
    }else if(battery_percentage>=0 && battery_percentage<=30){
        *r=.96f;*g=.74f;*b=.27f;
    }
}
static void battery_status_vertical(Paint *p,int x,int y){
    int old_theme=ui_theme_enabled;
    ui_theme_enabled=0;
    float r,g,b;battery_fill_color(&r,&g,&b);
    /* Compact 19x32 vertical battery aligned with the catalog clock,
       matching the reference screenshot without changing other controls. */
    box_alpha(p,x+6,y-4,7,4,.96f,.97f,.98f,1.f,2);
    box_alpha(p,x,y,19,32,.96f,.97f,.98f,1.f,4);
    box_alpha(p,x+2,y+2,15,28,.055f,.074f,.092f,1.f,3);
    if(battery_percentage>=0){
        int filled=(28*battery_percentage+99)/100;
        if(filled>28)filled=28;
        if(filled>0)box_alpha(p,x+2,y+30-filled,15,filled,r,g,b,1.f,2);
    }
    ui_theme_enabled=old_theme;
}
static void battery_status_horizontal(Paint *p,int x,int y){
    int old_theme=ui_theme_enabled;
    ui_theme_enabled=0;
    float r,g,b;battery_fill_color(&r,&g,&b);
    /* Match existing dark, rounded player overlay; icon only. */
    box_alpha(p,x,y,57,43,.047f,.067f,.088f,.88f,12);
    int bx=x+11,by=y+13;
    box_alpha(p,bx,by,28,16,.96f,.97f,.98f,1.f,3);
    box_alpha(p,bx+2,by+2,24,12,.055f,.074f,.092f,1.f,2);
    box_alpha(p,bx+28,by+5,4,6,.96f,.97f,.98f,1.f,2);
    if(battery_percentage>=0){
        int filled=(24*battery_percentage+99)/100;
        if(filled>24)filled=24;
        if(filled>0)box_alpha(p,bx+2,by+2,filled,12,r,g,b,1.f,2);
    }
    ui_theme_enabled=old_theme;
}
static void screen_header(Paint *p,const char *name,const char *context){
    box_alpha(p,0,0,1280,92,.058f,.069f,.083f,1.f,0);
    box_alpha(p,UI_EDGE,23,55,48,.13f,.16f,.20f,1.f,13);
    text_center(p,"‹",UI_EDGE,25,55,p->font_big,.96f,.97f,.99f);
    press_preview(p,UI_EDGE,23,55,48,13);
    headline(p,name,105,23,860);
    if(context && *context && strcmp(context,"UAPlay"))
        text_tiny(p,context,979,38,264);
}
static void list_row(Paint *p,const char *title,int y,int selected,
                     const char *detail){
    box_alpha(p,UI_EDGE,y,1216,UI_ROW_HEIGHT,
              selected?.20f:.085f,selected?.30f:.105f,
              selected?.38f:.135f,1.f,12);
    if(selected){
        float r,g,b;accent(&r,&g,&b);
        box_alpha(p,UI_EDGE,y+9,4,UI_ROW_HEIGHT-18,r,g,b,1.f,2);
        selection_frame(p,UI_EDGE,y,1216,UI_ROW_HEIGHT);
    }
    text(p,title,UI_EDGE+22,y+9,detail?915:1155);
    if(detail)text_muted(p,detail,1052,y+13,178);
    press_preview(p,UI_EDGE,y,1216,UI_ROW_HEIGHT,12);
}
static const char *const iptv_quick_countries[]={"UA","US","DE","UK","PL","IT"};
enum { IPTV_QUICK_COUNT=sizeof(iptv_quick_countries)/sizeof(iptv_quick_countries[0]),
       IPTV_COLUMNS=6, IPTV_PAGE_SIZE=2*IPTV_COLUMNS, IPTV_GRID_X=38, IPTV_GRID_Y=177,
       IPTV_STEP_X=202, IPTV_STEP_Y=211, IPTV_CARD_SIZE=184,
       IPTV_PAGE_STEP=2*IPTV_STEP_Y, IPTV_GRID_BOTTOM=IPTV_GRID_Y+IPTV_PAGE_STEP,
       IPTV_FILTER_Y=101, IPTV_FILTER_H=46,
       IPTV_START_TIMEOUT_MS=12000, IPTV_STABLE_MS=3000 };
typedef enum {
    IPTV_FOCUS_GRID, IPTV_FOCUS_HEADER, IPTV_FOCUS_COUNTRY, IPTV_FOCUS_GENRES, IPTV_FOCUS_FAVORITES
} IptvFocus;
static const struct {int x,w;} iptv_filters[]={{32,364},{412,354},{782,236}};
static struct {
    SDL_Thread *thread;
    SDL_atomic_t done;
    IptvCatalog catalog,pending,favorites;
    int result,from_cache,cached,loaded_country,requested_country;
    int country,genre,selected,filter_row,initialized,show_favorites;
    IptvFocus focus;
    int more_countries[IPTV_MAX_COUNTRIES],more_count;
    int *visible,count;
    int failures;
    int64_t entry;
    Uint32 playing_since;
} tv;

static const IptvChannel *iptv_channel(int row){
    return tv.show_favorites?&tv.favorites.channels[row]:
                            &tv.catalog.channels[tv.visible[row]];
}

static void iptv_filter(void){
    tv.count=0;
    if(tv.show_favorites)tv.count=tv.favorites.count;
    else if(tv.loaded_country==tv.country)
        for(int i=0;i<tv.catalog.count;i++){
            const IptvChannel *channel=&tv.catalog.channels[i];
            if(tv.genre==IPTV_ALL || (channel->genres & (UINT32_C(1)<<tv.genre)))
                tv.visible[tv.count++]=i;
        }
    tv.selected=0;
}

static int iptv_worker(void *unused){
    (void)unused;
    tv.result=iptv_load(tv.requested_country,&tv.pending,&tv.from_cache);
    SDL_AtomicSet(&tv.done,1);
    return 0;
}

static int iptv_request_country(void){
    if(tv.thread)return 1;
    tv.requested_country=tv.country;
    SDL_AtomicSet(&tv.done,0);
    tv.thread=SDL_CreateThread(iptv_worker,"iptv",NULL);
    return tv.thread!=NULL;
}

static int iptv_finish(void){
    if(!tv.thread || !SDL_AtomicGet(&tv.done))return 0;
    SDL_WaitThread(tv.thread,NULL);
    tv.thread=NULL;
    if(tv.requested_country!=tv.country){
        iptv_free(&tv.pending);
        if(tv.loaded_country==tv.country)return 1;
        return iptv_request_country()?0:-1;
    }
    if(tv.result!=IPTV_OK)return -1;
    int *visible=tv.pending.count?malloc((size_t)tv.pending.count*sizeof(*visible)):NULL;
    if(tv.pending.count && !visible){
        iptv_free(&tv.pending);
        return -1;
    }
    iptv_free(&tv.catalog);
    free(tv.visible);
    tv.catalog=tv.pending;
    tv.pending=(IptvCatalog){0};
    tv.visible=visible;
    tv.loaded_country=tv.requested_country;
    tv.cached=tv.from_cache;
    if(!tv.show_favorites)iptv_filter();
    return 1;
}

static void iptv_open(void){
    tv.focus=IPTV_FOCUS_HEADER;
    catalog_hero_focus=IPTV_TAB_FOCUS;
    if(tv.initialized)return;
    int n;
    const IptvCountry *countries=iptv_countries(&n);
    tv.country=iptv_country_index("UA");
    tv.loaded_country=-1;
    tv.genre=IPTV_ALL;
    for(int i=0;i<n;i++){
        bool quick=false;
        for(int j=0;j<IPTV_QUICK_COUNT;j++)
            if(!strcmp(countries[i].code,iptv_quick_countries[j])){quick=true;break;}
        if(!quick)tv.more_countries[tv.more_count++]=i;
    }
    iptv_favorites_load(&tv.favorites);
    tv.initialized=1;
    iptv_request_country();
}

static int iptv_filter_count(Page page){
    if(page==IPTV_COUNTRIES)return IPTV_QUICK_COUNT+1;
    if(page==IPTV_MORE_COUNTRIES)return tv.more_count;
    int n;
    iptv_genres(&n);
    return n+1;
}

static int footer_position,footer_total;
static Page footer_page;
static int offline_catalog_status(const char *message){
    if(!message)return 0;
    const char *uk="Офлайн-пошук",*en="Offline search";
    return !strncmp(message,"Offline catalog",15) ||
           !strncmp(message,uk,strlen(uk)) || !strncmp(message,en,strlen(en));
}

static void footer(Paint *p,const char *message) {
    box_alpha(p,0,UI_FOOTER_TOP,1280,720-UI_FOOTER_TOP,.047f,.057f,.070f,1.f,0);
    /* Monochrome Switch-style controller legend, independent of UI accent. */
    float ar=.62f,ag=.64f,ab=.66f;
    box_alpha(p,UI_EDGE,UI_FOOTER_TOP,1216,2,ar,ag,ab,.78f,0);
    char position[30];
    snprintf(position,sizeof(position),"%d / %d",footer_position,footer_total);
    text_font(p,footer_total && !(footer_page==CATALOG && expanded_catalog)?position:"UAPLAY",43,682,140,
              p->font_small,ar,ag,ab);
    const char *keys[6],*labels[6];int count=0;
#define HINT(k,l) do{keys[count]=k;labels[count]=l;count++;}while(0)
    if(footer_page==CATALOG){
        HINT("L",TR("Розділ","Section"));HINT("R",TR("Розділ","Section"));
        HINT("X",TR("Пошук","Search"));HINT("−",TR("Меню","Menu"));HINT("A",TR("Відкрити","Open"));
        if(ui_category==0 && !expanded_catalog)HINT("↑↓",TR("Добірка","Collection"));
        if(expanded_catalog)HINT("B",TR("Назад","Back"));
        else if(ui_category!=0)HINT("B",TR("Головна","Home"));
    }else if(footer_page==IPTV_CHANNELS){
        HINT("L/R",tv.focus==IPTV_FOCUS_HEADER?TR("Розділ","Section"):TR("Сторінка","Page"));
        HINT("Y",TR("★ Вибране","★ Favorites"));
        HINT("A",tv.focus==IPTV_FOCUS_GRID?TR("Дивитися","Watch"):TR("Відкрити","Open"));
        HINT("B",TR("Назад","Back"));
    }else if(footer_page==IPTV_COUNTRIES || footer_page==IPTV_MORE_COUNTRIES || footer_page==IPTV_GENRES){
        HINT("↑↓",TR("Вибір","Select"));HINT("L/R",TR("Сторінка","Page"));
        HINT("A",TR("Застосувати","Apply"));HINT("B",TR("Назад","Back"));
    }else if(footer_page==DETAILS){
        HINT("X",TR("Вибране","Favorites"));HINT("Y",TR("Переглянуто","Watched"));
        HINT("A",TR("Натиснути","Select"));HINT("B",TR("Назад","Back"));
    }else if(footer_page==SETTINGS || footer_page==APPEARANCE){
        HINT("↑↓",TR("Пункт","Item"));HINT("A",TR("Вибрати","Select"));
        HINT("B",footer_page==APPEARANCE?TR("Назад","Back"):TR("Закрити","Close"));
    }else if(footer_page==WALLPAPERS){
        HINT("L/R",TR("Сторінка","Page"));HINT("X",TR("Пошук","Search"));
        HINT("Y",TR("Стандартні","Default"));HINT("A",TR("Застосувати","Apply"));
        HINT("B",TR("Назад","Back"));
    }else if(footer_page==PROVIDER_SETTINGS){
        HINT("↑↓",TR("Джерело","Provider"));HINT("A",TR("Перемкнути","Toggle"));HINT("B",TR("Назад","Back"));
    }else if(footer_page==TRACK_PICKER || footer_page==SUBDL_PICKER){
        HINT("↑↓",TR("Доріжка","Track"));HINT("A",TR("Застосувати","Apply"));HINT("B",TR("Назад","Back"));
    }else if(footer_page==VIDEO_SETTINGS){
        HINT("↑↓",TR("Параметр","Option"));HINT("←→",TR("Змінити","Change"));HINT("B",TR("Назад","Back"));
    }else if(footer_page==SEASONS){
        HINT("↑↓",TR("Сезон","Season"));HINT("A",TR("Серії","Episodes"));HINT("B",TR("Назад","Back"));
    }else if(footer_page==EPISODES){
        HINT("↑↓",TR("Серія","Episode"));HINT("A",TR("Дивитися","Watch"));HINT("Y",TR("Завантажити","Download"));HINT("B",TR("Назад","Back"));
    }else if(footer_page==SOURCES){
        HINT("↑↓",TR("Вибір","Select"));HINT("A",TR("Відкрити","Open"));HINT("Y",TR("Завантажити","Download"));HINT("B",TR("Назад","Back"));
    }else if(footer_page==DOWNLOADS){
        HINT("↑↓",TR("Файл","File"));HINT("A",TR("Відкрити","Open"));HINT("X",TR("Видалити","Delete"));HINT("B",TR("Назад","Back"));
    }else{
        HINT("↑↓",TR("Вибір","Select"));HINT("A",TR("Відкрити","Open"));HINT("B",TR("Назад","Back"));
    }
#undef HINT
    int widths[6]={0},total=0;
    for(int i=0;i<count;i++){
        int badge_width=measure(p,keys[i],p->font_small)+12;
        if(badge_width<30)badge_width=30;
        widths[i]=badge_width+8+measure(p,labels[i],p->font_small)+20;
        total+=widths[i];
    }
    total+=(count-1)*12;
    int x=1244-total;
    if(x<204)x=204;
    for(int i=0;i<count;i++){
        int badge_width=measure(p,keys[i],p->font_small)+12;
        if(badge_width<30)badge_width=30;
        int round=(!strcmp(keys[i],"A")||!strcmp(keys[i],"B")||
                   !strcmp(keys[i],"X")||!strcmp(keys[i],"Y"));
        box_alpha(p,x,681,badge_width,28,ar,ag,ab,1.f,round?14:5);
        text_center(p,keys[i],x,682,badge_width,p->font_small,.09f,.11f,.13f);
        text_font(p,labels[i],x+badge_width+8,681,widths[i]-badge_width-8,
                  p->font_small,ar,ag,ab);
        x+=widths[i]+12;
    }
    /* Background download indicator using the same footer typography. */
    int download_percent=0; unsigned long long downloaded_bytes=0;
    int pending_downloads=downloads_progress(&download_percent,&downloaded_bytes);
    if(pending_downloads){
        char progress_label[80];
        if(download_percent>=0)
            snprintf(progress_label,sizeof(progress_label),"↓ %d%% · %d",download_percent/10,pending_downloads);
        else snprintf(progress_label,sizeof(progress_label),TR("↓ %u МБ · %d","↓ %u MB · %d"),(unsigned)(downloaded_bytes>>20),pending_downloads);
        text_font(p,progress_label,250,682,175,p->font_small,ar,ag,ab);
    }
    /* Notification occupies its own safe zone, never the last selectable row. */
    if(message && *message && !offline_catalog_status(message) &&
       strncmp(message,"A:",2) && strncmp(message,"B:",2)){
        box_alpha(p,UI_EDGE,617,1216,43,.11f,.16f,.19f,.98f,11);
        text_small(p,message,50,626,1170);
    }
}
static void poster(Paint *p,const Movie *movie,int x,int y,int width,int height,int *budget) {
    GLuint image=poster_texture(movie,budget);
    if(!image) {
        box(p,x,y,width,height,.15f,.19f,.23f);
        text(p,"UAPLAY",x+12,y+height/2-15,width-25);
        return;
    }
    GLuint previous=p->tex;
    p->tex=image;
    box(p,x,y,width,height,1,1,1);
    p->tex=previous;
    if(movie->rating>0.01f){
        char score[28];
        snprintf(score,sizeof(score),"%.1f",movie->rating);
        int badge_width=50;
        box_alpha(p,x+5,y+height-34,badge_width,28,.035f,.047f,.061f,.91f,7);
        text_center_button(p,score,x+5,y+height-34,badge_width,28,
                           p->font_tiny,.98f,.96f,.88f);
    }
}

static void draw_poster_row(Paint *p,const Movie *movies,int count,int offset,int y,
                            int chosen,int *budget){
    if(y>=POSTER_ROW_BOTTOM || y+POSTER_ROW_BOTTOM-POSTER_ROW_Y<=POSTER_ROW_Y)return;
    int first=offset/POSTER_STEP-1;
    if(first<0)first=0;
    for(int i=first;i<count && i<first+POSTER_COLUMNS+3;i++){
        int x=53+i*POSTER_STEP-offset;
        if(x+164<=UI_EDGE || x>=1280-UI_EDGE)continue;
        if(i==chosen)selection_frame(p,x,y,150,221);
        poster(p,&movies[i],x,y,150,221,budget);
        card_title(p,movies[i].title,x,y+224,164);
        press_preview(p,x,y,150,221,14);
    }
}

static void draw_poster_grid(Paint *p,const Movie *movies,int count,int chosen,int library,
                             int *budget){
    int top=library?LIBRARY_GRID_Y:POSTER_GRID_Y;
    int bottom=library?POSTER_ROW_BOTTOM:POSTER_GRID_BOTTOM;
    int target=chosen/GRID_ITEMS*GRID_PAGE_STEP;
    int limit=count?(count-1)/GRID_ITEMS*GRID_PAGE_STEP:0;
    int offset=scroll_offset(&content_motion,target,touch_drag(CATALOG,2,top,bottom),
                             limit,GRID_PAGE_STEP);
    paint_clip(p,0,top-5,1280,bottom-top+8);
    int first=(offset/GRID_PAGE_STEP-1)*GRID_ITEMS;
    if(first<0)first=0;
    for(int i=first;i<count && i<first+3*GRID_ITEMS;i++){
        int k=i%GRID_ITEMS,x=54+k%POSTER_COLUMNS*POSTER_STEP;
        int y=top+k/POSTER_COLUMNS*(library?245:246)+i/GRID_ITEMS*GRID_PAGE_STEP-offset;
        if(y+255<top || y>=bottom)continue;
        if(i==chosen && !catalog_hero_focus)selection_frame(p,x,y,138,206);
        poster(p,&movies[i],x,y,138,206,budget);
        if(library)card_title(p,movies[i].title,x,y+209,166);
        press_preview(p,x,y,138,206,14);
    }
    paint_clip(p,0,0,0,0);
}

static int list_offset(Paint *p,Page page,int selected,int count){
    if(selected>=count)selected=count?count-1:0;
    int target=selected/UI_ROWS*UI_ROWS*UI_ROW_STEP;
    int limit=count?(count-1)/UI_ROWS*UI_ROWS*UI_ROW_STEP:0;
    int bottom=UI_ROW_Y+UI_ROWS*UI_ROW_STEP;
    paint_clip(p,0,UI_ROW_Y-5,1280,bottom-UI_ROW_Y+5);
    return scroll_offset(&content_motion,target,touch_drag(page,2,UI_ROW_Y,bottom),
                         limit,UI_ROWS*UI_ROW_STEP);
}

static int first_visible_row(int offset){
    int first=offset/UI_ROW_STEP-1;
    return first>0?first:0;
}

static int touched_grid_poster(int x,int y,int top,int row_step,int bottom){
    if(x<54 || y<top-5 || y>=bottom)return -1;
    int col=(x-54)/POSTER_STEP;
    if(col>=POSTER_COLUMNS || (x-54)%POSTER_STEP>=138)return -1;
    int absolute_y=y-top+(int)lroundf(content_motion.position);
    if(absolute_y<0)return -1;
    int within_page=absolute_y%GRID_PAGE_STEP;
    int row=within_page/row_step;
    if(row>=2 || within_page%row_step>=206)return -1;
    return absolute_y/GRID_PAGE_STEP*GRID_ITEMS+row*POSTER_COLUMNS+col;
}

static int touched_list_row(int x,int y){
    if(x<UI_EDGE || x>=1280-UI_EDGE || y<UI_ROW_Y-5 || y>=UI_ROW_Y+UI_ROWS*UI_ROW_STEP)
        return -1;
    int absolute_y=y-UI_ROW_Y+(int)lroundf(content_motion.position);
    if(absolute_y<0 || absolute_y%UI_ROW_STEP>=UI_ROW_HEIGHT)return -1;
    return absolute_y/UI_ROW_STEP;
}
static int iptv_touched_card(int x,int y){
    if(y<IPTV_GRID_Y-5 || y>=IPTV_GRID_BOTTOM)return -1;
    x-=IPTV_GRID_X;
    y+=(int)lroundf(content_motion.position)-IPTV_GRID_Y;
    if(x<0 || y<0 || x/IPTV_STEP_X>=IPTV_COLUMNS ||
       x%IPTV_STEP_X>=IPTV_CARD_SIZE || y%IPTV_STEP_Y>=IPTV_CARD_SIZE)return -1;
    int row=y/IPTV_STEP_Y*IPTV_COLUMNS+x/IPTV_STEP_X;
    return row<tv.count?row:-1;
}


/* Wallpaper grid: 3 columns by 2 rows, vertically paged. */
enum { WALL_COLS=3,WALL_PAGE=6,WALL_X=32,WALL_Y=163,WALL_W=382,
       WALL_CARD_H=214,WALL_STEP_X=417,WALL_STEP_Y=228,WALL_PAGE_SHIFT=456 };
static int wall_selected,wall_filter,wall_focus,wall_count,wall_rows[WALLPAPERS_MAX];
static char wall_query[96];
enum { WALL_ANIM_MS=250 };
static int wall_anim_from=-1,wall_anim_to=-1,wall_anim_dir=1,wall_api_dir;
static int wall_api_column;
static Uint32 wall_anim_started;
static int wall_mouse_active,wall_mouse_x,wall_mouse_y;
static const UiText wall_tags[]={{"Усі","All"},{"Темні","Dark"},{"Мінімалізм","Minimal"},{"Аніме","Anime"}};
/* Wallhaven performs text/category filtering on the server. */
static void wall_rebuild(void){
    wall_count=wallpapers_count();
    if(wall_count>WALLPAPERS_MAX)wall_count=WALLPAPERS_MAX;
    for(int i=0;i<wall_count;i++)wall_rows[i]=i;
    if(wall_selected>=wall_count)wall_selected=wall_count?wall_count-1:0;
}
static void wall_load_filter(int next){
    if(wallpapers_browse(next,wall_query,1)){
        wall_filter=next;wall_selected=0;wall_focus=1;
        wall_anim_from=-1;wall_anim_to=-1;
    }
}
/* Vertically scroll one six-item screen and preserve the selected column. */
static void wall_start_animation(int from,int to,int direction){
    wall_anim_from=from;wall_anim_to=to;wall_anim_dir=direction;
    wall_anim_started=SDL_GetTicks();ui_animating=1;
}
static void wall_move_page(int direction){
    if(wallpapers_busy() || !wall_count)return;
    int first=wall_selected/WALL_PAGE*WALL_PAGE;
    int column=wall_selected%WALL_COLS;
    if(direction<0 && first==0 && wallpapers_current_page()>1){
        if(wallpapers_browse(wall_filter,wall_query,wallpapers_current_page()-1)){
            wall_api_dir=direction;wall_api_column=column;
        }
    }else if(direction>0 && first+WALL_PAGE>=wall_count &&
             wallpapers_current_page()<wallpapers_last_page()){
        if(wallpapers_browse(wall_filter,wall_query,wallpapers_current_page()+1)){
            wall_api_dir=direction;wall_api_column=column;
        }
    }else{
        int next=first+direction*WALL_PAGE;
        if(next<0 || next>=wall_count)return;
        int target=next+(direction<0?WALL_COLS:0)+column;
        wall_selected=target<wall_count?target:wall_count-1;
        wall_start_animation(first,next,direction);
    }
}
static int wall_hit(int x,int y){
    if(x<WALL_X || y<WALL_Y)return -1;
    int col=(x-WALL_X)/WALL_STEP_X,row=(y-WALL_Y)/WALL_STEP_Y;
    if(col>=WALL_COLS || row>=2 || (x-WALL_X)%WALL_STEP_X>=WALL_W ||
       (y-WALL_Y)%WALL_STEP_Y>=WALL_CARD_H)return -1;
    int i=wall_selected/WALL_PAGE*WALL_PAGE+row*WALL_COLS+col;
    return i<wall_count?i:-1;
}
static void wall_draw_grid(Paint *p,int start,int offset_y,int *budget,int focused){
    for(int n=start;n<wall_count && n<start+WALL_PAGE;n++){
        const WallpaperItem *item=wallpapers_item(wall_rows[n]);
        if(!item)continue;
        int x=WALL_X+((n-start)%WALL_COLS)*WALL_STEP_X;
        int y=WALL_Y+((n-start)/WALL_COLS)*WALL_STEP_Y+offset_y;
        box_alpha(p,x,y,WALL_W,WALL_CARD_H,.13f,.16f,.19f,1.f,14);
        int image_w=0,image_h=0;
        GLuint tex=logo_texture(item->thumbnail,budget,&image_w,&image_h);
        if(tex)wallpaper_image_cover(p,tex,image_w,image_h,
                                      x+5,y+5,WALL_W-10,174,11);
        else text_center_button(p,"UAPlay",x+8,y+75,WALL_W-16,38,
                                p->font_small,.7f,.74f,.79f);
        if(focused && n==wall_selected && !wall_focus)
            selection_frame(p,x,y,WALL_W,WALL_CARD_H);
        text_font(p,item->name,x+14,y+185,WALL_W-28,p->font_small,1,1,1);
    }
}
static void draw_wallpapers(Paint *p){
    screen_header(p,TR("Шпалери","Wallpapers"),"Wallhaven");
    for(int i=0;i<4;i++){
        int x=32+i*158;
        box_alpha(p,x,101,145,46,i==wall_filter?.20f:.09f,.19f,.25f,1.f,13);
        if(wall_focus && wall_filter==i)selection_frame(p,x,101,145,46);
        text_center_button(p,ui_text(wall_tags[i]),x,101,145,46,p->font_small,1,1,1);
    }
    box_alpha(p,674,101,360,46,.09f,.13f,.16f,1.f,12);
    text_small(p,*wall_query?wall_query:TR("X: Пошук","X: Search"),690,109,330);
    box_alpha(p,1050,101,196,46,.13f,.17f,.20f,1.f,12);
    text_center_button(p,TR("Y: Стандартні","Y: Default"),1050,101,196,46,p->font_small,1,1,1);
    int budget=1,start=wall_selected/WALL_PAGE*WALL_PAGE;
    paint_clip(p,WALL_X,WALL_Y,1216,455);
    Uint32 elapsed=ui_now-wall_anim_started;
    if(wall_anim_to==start && elapsed<WALL_ANIM_MS){
        float t=elapsed/(float)WALL_ANIM_MS;
        float remain=1.f-t;
        float eased=1.f-remain*remain*remain;
        if(wall_anim_from>=0)
            wall_draw_grid(p,wall_anim_from,
                           -(int)(wall_anim_dir*WALL_PAGE_SHIFT*eased),&budget,0);
        int distance=wall_anim_from>=0?WALL_PAGE_SHIFT:140;
        wall_draw_grid(p,start,
                       (int)(wall_anim_dir*distance*(1.f-eased)),&budget,1);
        ui_animating=1;
    }else{
        wall_anim_from=-1;wall_anim_to=-1;
        wall_draw_grid(p,start,0,&budget,1);
    }
    paint_clip(p,0,0,0,0);
    if(!wall_count && !wallpapers_busy())
        text_center_button(p,TR("Зображення не знайдено","No wallpapers found"),
                           50,340,1180,55,p->font_small,.8f,.85f,.9f);
    char page_label[120];
    snprintf(page_label,sizeof(page_label),
             TR("Wallhaven %d/%d · Сторінка %d/%d","Wallhaven %d/%d · Page %d/%d"),
             wallpapers_current_page(),wallpapers_last_page(),
             wall_count?wall_selected/WALL_PAGE+1:0,
             (wall_count+WALL_PAGE-1)/WALL_PAGE);
    text_small(p,page_label,46,632,700);
    /* Only show actual errors, never service success or progress messages. */
    footer(p,wallpapers_busy()?"":wallpapers_message());
}
static void draw_iptv(Paint *p,Page page,const char *status){
    int country_count,genre_count;
    const IptvCountry *countries=iptv_countries(&country_count);
    const UiText *genres=iptv_genres(&genre_count);
    if(page==IPTV_CHANNELS){
        char country_label[180],genre_label[180];
        snprintf(country_label,sizeof(country_label),TR("Країна: %s","Country: %s"),ui_text(countries[tv.country].name));
        snprintf(genre_label,sizeof(genre_label),TR("Жанри: %s","Genres: %s"),
                 tv.genre==IPTV_ALL?TR("Усі","All"):ui_text(genres[tv.genre]));
        const char *labels[]={country_label,genre_label,
            tv.show_favorites?TR("Усі канали","All channels"):TR("Вибране","Favorites")};
        for(int i=0;i<3;i++){
            int x=iptv_filters[i].x,w=iptv_filters[i].w;
            if((int)tv.focus==IPTV_FOCUS_COUNTRY+i)selection_frame(p,x,IPTV_FILTER_Y,w,IPTV_FILTER_H);
            button(p,labels[i],x,IPTV_FILTER_Y,w,IPTV_FILTER_H,i==2 && tv.show_favorites);
        }
        if(tv.cached && tv.loaded_country==tv.country && !tv.show_favorites)
            text_small(p,TR("Збережений список","Cached list"),1030,111,205);
        if(!tv.count){
            const char *empty=tv.show_favorites?TR("У вибраному поки немає каналів","No favorite channels yet"):
                tv.thread?TR("Завантаження каналів…","Loading channels…"):
                          TR("Каналів не знайдено","No channels found");
            text_center_button(p,empty,UI_EDGE,271,1216,96,p->font_big,.73f,.77f,.82f);
        }
        int target=tv.selected/IPTV_PAGE_SIZE*IPTV_PAGE_STEP;
        int limit=tv.count?(tv.count-1)/IPTV_PAGE_SIZE*IPTV_PAGE_STEP:0;
        int offset=scroll_offset(&content_motion,target,
            touch_drag(IPTV_CHANNELS,2,IPTV_GRID_Y,IPTV_GRID_BOTTOM),limit,IPTV_PAGE_STEP);
        paint_clip(p,0,IPTV_GRID_Y-5,1280,IPTV_GRID_BOTTOM-IPTV_GRID_Y+5);
        int first=(offset/IPTV_PAGE_STEP-1)*IPTV_PAGE_SIZE;
        if(first<0)first=0;
        int budget=1;
        for(int i=first;i<tv.count && i<first+3*IPTV_PAGE_SIZE;i++){
            int x=IPTV_GRID_X+i%IPTV_COLUMNS*IPTV_STEP_X;
            int y=IPTV_GRID_Y+i/IPTV_COLUMNS*IPTV_STEP_Y-offset;
            if(y+IPTV_STEP_Y<=IPTV_GRID_Y-5 || y>=IPTV_GRID_BOTTOM)continue;
            const IptvChannel *channel=iptv_channel(i);
            if(i==tv.selected && tv.focus==IPTV_FOCUS_GRID)
                selection_frame(p,x,y,IPTV_CARD_SIZE,IPTV_CARD_SIZE);
            box(p,x,y,IPTV_CARD_SIZE,IPTV_CARD_SIZE,.19f,.22f,.26f);
            int w=0,h=0;
            GLuint logo=logo_texture(channel->logo,&budget,&w,&h);
            if(logo && w>0 && h>0){
                float scale=(IPTV_CARD_SIZE-32)/(float)(w>h?w:h);
                int rw=(int)fmaxf(1,w*scale),rh=(int)fmaxf(1,h*scale);
                GLuint previous=p->tex;
                p->tex=logo;
                box_alpha(p,x+(IPTV_CARD_SIZE-rw)/2,y+(IPTV_CARD_SIZE-rh)/2,rw,rh,1,1,1,1,0);
                p->tex=previous;
            }else text_center_button(p,"TV",x,y,IPTV_CARD_SIZE,IPTV_CARD_SIZE,
                                     p->font_big,.75f,.79f,.82f);
            bool favorite=iptv_favorite_index(&tv.favorites,channel->id)>=0;
            box_alpha(p,x+IPTV_CARD_SIZE-39,y+7,31,31,.10f,.13f,.17f,.93f,10);
            text_center_button(p,favorite?"★":"☆",x+IPTV_CARD_SIZE-39,y+7,31,31,
                               p->font_small,.96f,.88f,.62f);
            press_preview(p,x,y,IPTV_CARD_SIZE,IPTV_CARD_SIZE,14);
        }
        paint_clip(p,0,0,0,0);
        if(tv.count)text_muted(p,iptv_channel(tv.selected)->title,54,635,1150);
        footer_position=tv.count?tv.selected/IPTV_PAGE_SIZE+1:0;
        footer_total=(tv.count+IPTV_PAGE_SIZE-1)/IPTV_PAGE_SIZE;
    }else{
        bool country=page!=IPTV_GENRES;
        int count=iptv_filter_count(page),selected=tv.filter_row;
        screen_header(p,page==IPTV_MORE_COUNTRIES?TR("Більше країн","More countries"):
                         country?TR("Країна","Country"):TR("Жанри","Genres"),NULL);
        int offset=list_offset(p,page,selected,count),first=first_visible_row(offset);
        for(int row=first;row<count && row<first+UI_ROWS+2;row++){
            const char *name,*detail=NULL;
            if(!country)name=row?ui_text(genres[row-1]):TR("Усі","All");
            else if(page==IPTV_COUNTRIES && row==IPTV_QUICK_COUNT)
                name=TR("Більше країн","More countries");
            else{
                int index=page==IPTV_MORE_COUNTRIES?tv.more_countries[row]:
                          iptv_country_index(iptv_quick_countries[row]);
                name=ui_text(countries[index].name);
                detail=countries[index].code;
            }
            list_row(p,name,UI_ROW_Y+row*UI_ROW_STEP-offset,row==selected,detail);
        }
        paint_clip(p,0,0,0,0);
        footer_position=selected+1;
        footer_total=count;
    }
    footer(p,status);
}

static void append_details_metadata(char *text,size_t size,const char *part){
    if(!part[0])return;
    size_t used=strlen(text);
    snprintf(text+used,size-used,"%s%s",used?"  ·  ":"",part);
}
static void format_details_metadata(char *text,size_t size,const Movie *movie,const char *genre){
    snprintf(text,size,"%s",movie->year);
    char part[48];
    if(movie->series){
        if(movie->season_count>0){
            snprintf(part,sizeof(part),TR("Сезонів: %d","Seasons: %d"),movie->season_count);
            append_details_metadata(text,size,part);
        }
        if(movie->episode_count>0){
            snprintf(part,sizeof(part),TR("Серій: %d","Episodes: %d"),movie->episode_count);
            append_details_metadata(text,size,part);
        }
    }else{
        append_details_metadata(text,size,genre);
        int minutes=movie->runtime_minutes;
        if(minutes>0){
            if(minutes<60)snprintf(part,sizeof(part),TR("%d хв","%d min"),minutes);
            else snprintf(part,sizeof(part),TR("%d год %02d хв","%d h %02d min"),
                          minutes/60,minutes%60);
            append_details_metadata(text,size,part);
        }
    }
}
static void draw(Paint *p,Page page,Movie *movies,int n,int chosen,
                 Source *sources,int sn,int source,const char *heading,const char *status) {
    glViewport(0,0,1280,720);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
    ui_theme_enabled=1;
    ui_gradient_pass=1;
    box_alpha(p,0,0,1280,720,.034f,.042f,.054f,1.f,0);
    ui_gradient_pass=0;
    GLuint wall=wallpapers_texture();
    if(wall && page!=PLAYER){
        int theme=ui_theme_enabled;
        ui_theme_enabled=0;
        /* Keep the original resolution; shader UV center crop fills 16:9
           without changing the file or stretching the image. */
        wallpaper_image_cover(p,wall,wallpapers_width(),wallpapers_height(),
                              0,0,1280,720,0);
        box_alpha(p,0,0,1280,720,.02f,.025f,.03f,
                  ui_light_theme?.20f:.64f,0);
        ui_theme_enabled=theme;
    }
    static int previous_page=-1,previous_category=-1,previous_expanded=-1;
    static int previous_sort=-1,painted_shelf=-1,old_shelf_offset=0;
    static Uint32 transition_at=0;
    if(previous_page!=(int)page ||
       (page==CATALOG && (previous_category!=ui_category ||
        previous_expanded!=expanded_catalog || previous_sort!=catalog_sort_mode))){
        previous_page=(int)page;
        previous_category=ui_category;
        previous_expanded=expanded_catalog;
        previous_sort=catalog_sort_mode;
        content_motion=(ScrollMotion){0};
        shelf_motion=(ScrollMotion){0};
        painted_shelf=-1;
        transition_at=ui_now;
    }
    footer_position=page==CATALOG?(n?chosen+1:0):page==SOURCES?(sn?source+1:0):
                    page==EPISODES?(episode_count?selected_episode+1:0):
                    page==SEASONS?(season_count?selected_season+1:0):0;
    footer_total=page==CATALOG?n:page==SOURCES?sn:page==SEASONS?season_count:
                 page==EPISODES && selected_season<season_count?
                    episodes_in_season(season_ids[selected_season]):0;
    footer_page=page;
    int poster_budget=1;
    if(page==WALLPAPERS){draw_wallpapers(p);return;}
    if (page==CATALOG || page==IPTV_CHANNELS) {
        box_alpha(p,0,0,1280,80,.059f,.070f,.081f,1.f,0);
        headline(p,"UAPlay",25,18,166);
        const char *tabs[]={TR("Головна","Home"),TR("Серіали","TV Shows"),TR("Фільми","Movies"),
            TR("Мультфільми","Animation"),"IPTV",TR("Вибране","Favorites")};
        int active=page==IPTV_CHANNELS?IPTV_TAB_FOCUS:7+tab_for_category(ui_category);
        int focused=page==CATALOG || tv.focus==IPTV_FOCUS_HEADER?catalog_hero_focus:0;
        for(int i=0;i<MAIN_TAB_COUNT;i++){
            int x=main_tabs[i].x,w=main_tabs[i].w,focus=main_tabs[i].focus;
            if(focus==active)box_alpha(p,x,19,w,43,.19f,.24f,.28f,1.f,16);
            if(focus==focused)selection_frame(p,x,19,w,43);
            text_center_button(p,tabs[i],x,19,w,43,
                               p->font_small,.96f,.97f,.99f);
            press_preview(p,x,19,w,43,16);
        }
        /* Search is a real touch target, not a missing Unicode glyph. */
        box_alpha(p,887,17,157,47,.13f,.16f,.19f,1.f,15);
        if(focused==5)selection_frame(p,887,17,157,47);
        search_icon(p,915,38,.13f,.16f,.19f);
        text_font(p,TR("Пошук","Search"),946,17+(47-text_height(p,TR("Пошук","Search"),p->font_small))/2,
                  88,p->font_small,1.f,1.f,1.f);
        press_preview(p,887,17,157,47,15);
        box_alpha(p,1058,17,53,47,.13f,.16f,.19f,1.f,15);
        if(focused==6)selection_frame(p,1058,17,53,47);
        gear_icon(p,1085,40,.13f,.16f,.19f);
        press_preview(p,1058,17,53,47,15);
        time_t now=time(NULL);
        struct tm *local=localtime(&now);
        char clock_label[32]="--:--";
        if(local)strftime(clock_label,sizeof(clock_label),"%H:%M",local);
        text_font(p,clock_label,1116,18,106,
                  p->font_clock?p->font_clock:p->font_big,.95f,.97f,.99f);
        if(page==IPTV_CHANNELS){
            battery_status_vertical(p,1218,27);
            draw_iptv(p,page,status);
            return;
        }
        if(ui_category==11 || ui_category==12){
            /* Favorites uses a poster-only library layout, without the hero.
               Both tabs stay reachable even when their list is empty. */
            const char *names[]={TR("На майбутнє","Watchlist"),TR("Історія","History"),TR("Завантажені","Downloads")};
            const int tab_x[]={32,244,417},tab_w[]={196,158,208};
            for(int i=0;i<3;i++){
                int selected=i<2 && ui_category==11+i;
                box_alpha(p,tab_x[i],101,tab_w[i],47,
                          selected?.20f:.12f,selected?.28f:.15f,
                          selected?.35f:.19f,1.f,13);
                if(catalog_hero_focus==16+i)
                    selection_frame(p,tab_x[i],101,tab_w[i],47);
                text_center_button(p,names[i],tab_x[i],101,tab_w[i],47,
                                   p->font_small,.96f,.97f,.99f);
                press_preview(p,tab_x[i],101,tab_w[i],47,13);
            }
            if(ui_category==12){
                if(catalog_hero_focus==19)
                    selection_frame(p,982,101,266,47);
                button(p,clear_history_confirm?TR("Підтвердити очищення","Confirm clearing"):
                       TR("Очистити історію","Clear history"),982,101,266,47,0);
            }
            if(!n){
                text_center_button(p,TR("Тут нічого немає","Nothing here yet"),UI_EDGE,305,1216,110,
                                   p->font_big,.73f,.77f,.82f);
            }
            draw_poster_grid(p,movies,n,chosen,1,&poster_budget);
        }else if(expanded_catalog){
            headline(p,heading,UI_EDGE,97,960);
            const char *sort_labels[]={TR("Рейтинг","Rating"),TR("Популярність","Popularity")};
            const int sort_x[]={785,928},sort_w[]={132,159};
            for(int i=0;i<2;i++){
                int active=catalog_sort_mode==sort_mode_for_button(i);
                box_alpha(p,sort_x[i],98,sort_w[i],44,
                          active?.20f:.10f,
                          active?.27f:.14f,
                          active?.33f:.18f,1.f,12);
                if(catalog_hero_focus==12+i)
                    selection_frame(p,sort_x[i],98,sort_w[i],44);
                text_center_button(p,sort_labels[i],sort_x[i],98,sort_w[i],44,
                                   p->font_small,.96f,.97f,.99f);
                press_preview(p,sort_x[i],98,sort_w[i],44,12);
            }
            if(catalog_hero_focus==CATALOG_BACK_FOCUS)selection_frame(p,1106,98,142,44);
            button(p,TR("‹  Назад","‹  Back"),1106,98,142,44,0);
            draw_poster_grid(p,movies,n,chosen,0,&poster_budget);
            if(n)text_muted(p,movies[chosen].title,54,635,1150);
        }else{
            int active=chosen>=0 && chosen<n?chosen:0;
            enum { HERO_Y=92, HERO_W=1216, HERO_H=222, HERO_PAD=12,
                   HERO_POSTER_W=132, HERO_BUTTON_H=50, HERO_BUTTON_GAP=12 };
            const int hero_left=UI_EDGE+HERO_PAD;
            const int hero_top=HERO_Y+HERO_PAD;
            const int hero_bottom=HERO_Y+HERO_H-HERO_PAD;
            const int hero_poster_x=UI_EDGE+HERO_W-HERO_PAD-HERO_POSTER_W;
            const int hero_button_y=hero_bottom-HERO_BUTTON_H;
            const int hero_favorite_x=hero_left+205+HERO_BUTTON_GAP;
            box_alpha(p,UI_EDGE,HERO_Y,HERO_W,HERO_H,.09f,.11f,.14f,1.f,22);
            if(n){
                /* All four inner edges use the same 12px inset, following
                   the user's corrected Switch-style reference layout. */
                poster(p,&movies[active],hero_poster_x,hero_top,
                       HERO_POSTER_W,hero_bottom-hero_top,&poster_budget);
                headline(p,movies[active].title,hero_left,hero_top,967);
                char tag[110];
                snprintf(tag,sizeof(tag),"%s  ·  %s",movies[active].year,
                         movies[active].series?TR("Серіал","TV Show"):TR("Фільм","Movie"));
                text_muted(p,tag,hero_left,hero_top+50,890);
                wrapped(p,movies[active].overview,hero_left,hero_top+90,949,2,27,
                        p->font_small,.74f,.79f,.84f);
            }else{
                int loading=ui_category==0 && home_cache_initialized && !home_current_loaded;
                headline(p,loading?TR("Завантаження добірки...","Loading collection..."):TR("Не знайдено матеріалів","No titles found"),
                         hero_left,hero_top+14,900);
                text_muted(p,loading?TR("Зачекайте, отримуємо матеріали","Fetching titles…"):
                           TR("Спробуйте інший розділ або пошуковий запит","Try another section or search term"),
                           hero_left,hero_top+78,920);
            }
            if(n && catalog_hero_focus==1)
                selection_frame(p,hero_left,hero_button_y,205,HERO_BUTTON_H);
            if(n){
                if(catalog_hero_focus==2)
                    selection_frame(p,hero_favorite_x,hero_button_y,218,HERO_BUTTON_H);
                button(p,TR("▶  Дивитися","▶  Watch"),hero_left,hero_button_y,205,HERO_BUTTON_H,1);
                button(p,library_favorite(&movies[active])?
                       TR("✓  У вибраному","✓  In favorites"):TR("+  У вибране","+  Add to favorites"),hero_favorite_x,hero_button_y,
                       218,HERO_BUTTON_H,0);
            }
            int section_tabs=subsection_count(ui_category);
            if(section_tabs){
                const char *animation_tabs[]={TR("Мультфільми","Animation"),TR("Мультсеріали","Animated series"),TR("Аніме","Anime")};
                const int anim_x[]={32,240,465},anim_w[]={191,208,134};
                const int film_x[]={32,197,373,513,672},film_w[]={157,168,132,151,225};
                int films=tab_for_category(ui_category)==2;
                for(int i=0;i<section_tabs;i++){
                    int selected=ui_category==category_for_subsection(ui_category,i);
                    int x=films?film_x[i]:anim_x[i],w=films?film_w[i]:anim_w[i];
                    box_alpha(p,x,328,w,43,
                              selected?.20f:.12f,selected?.28f:.15f,
                              selected?.35f:.19f,1.f,13);
                    if(catalog_hero_focus==i+16)
                        selection_frame(p,x,328,w,43);
                    text_center_button(p,films?ui_text(film_labels[i]):animation_tabs[i],x,328,
                                       w,43,p->font_small,
                                       .96f,.97f,.99f);
                    press_preview(p,x,328,w,43,13);
                }
            }else text(p,heading,UI_EDGE,336,930);
            if(offline_catalog_status(status))
                text_tiny(p,TR("Офлайн","Offline"),1026,340,77);
            if(n && catalog_hero_focus==3)selection_frame(p,1110,329,138,42);
            button(p,ui_category==0?TR("Більше  ›","More  ›"):TR("Усі  ›","All  ›"),1110,329,138,42,0);
            int home=ui_category==0;
            int target=poster_row_start(chosen,home)*POSTER_STEP;
            int max_start=home?(n>POSTER_COLUMNS?n-POSTER_COLUMNS:0):
                          (n?(n-1)/POSTER_COLUMNS*POSTER_COLUMNS:0);
            if(home && painted_shelf!=home_shelf){
                old_shelf_offset=painted_shelf>=0?(int)lroundf(content_motion.position):0;
                painted_shelf=home_shelf;
                content_motion=(ScrollMotion){0};
            }
            int shelf_offset=home?scroll_offset(&shelf_motion,home_shelf*SHELF_STEP,
                touch_drag(CATALOG,2,POSTER_ROW_Y,POSTER_ROW_BOTTOM),
                (HOME_SHELF_COUNT-1)*SHELF_STEP,SHELF_STEP):0;
            int offset=scroll_offset(&content_motion,target,
                                     touch_drag(CATALOG,1,POSTER_ROW_Y,POSTER_ROW_BOTTOM),
                                     max_start*POSTER_STEP,
                                     (POSTER_COLUMNS-!home)*POSTER_STEP);
            paint_clip(p,UI_EDGE,POSTER_ROW_Y-5,1280-2*UI_EDGE,
                       POSTER_ROW_BOTTOM-POSTER_ROW_Y+5);
            int y=POSTER_ROW_Y+(home?home_shelf*SHELF_STEP-shelf_offset:0);
            draw_poster_row(p,movies,n,offset,y,catalog_hero_focus?-1:chosen,&poster_budget);
            if(home && shelf_offset!=home_shelf*SHELF_STEP){
                static Movie preview[POSTER_COLUMNS+1];
                int other=shelf_offset/SHELF_STEP;
                if(other==home_shelf)other++;
                int preview_offset=shelf_motion.dragging?0:old_shelf_offset;
                int start=preview_offset>0?preview_offset/POSTER_STEP:0;
                int count=catalog_home_copy(other,preview,start,POSTER_COLUMNS+1);
                draw_poster_row(p,preview,count,preview_offset-start*POSTER_STEP,
                                POSTER_ROW_Y+other*SHELF_STEP-shelf_offset,-1,&poster_budget);
            }
            paint_clip(p,0,0,0,0);
        }
        footer(p,status);
    } else if(page==SETTINGS || page==APPEARANCE){
        int appearance=page==APPEARANCE;
        screen_header(p,appearance?TR("Зовнішній вигляд","Appearance"):
                      TR("Налаштування","Settings"),"UAPlay");
        const char *main_items[]={
            TR("Зовнішній вигляд","Appearance"),TR("Плеєр","Player"),
            TR("Автовідтворення","Autoplay"),TR("Джерела","Providers"),
            TR("Мова / Language","Language / Мова")
        };
        const char *appearance_items[]={
            TR("Тема","Theme"),TR("Колір акценту","Accent color"),
            TR("Шпалери","Wallpapers"),TR("Вимкнути шпалери","Disable wallpapers")
        };
        const char **items=appearance?appearance_items:main_items;
        int selected=appearance?appearance_selected:settings_selected;
        int rows=appearance?APPEARANCE_COUNT:SETTING_COUNT;
        for(int i=0;i<rows;i++){
            int y=SETTINGS_Y+i*SETTINGS_ROW_STEP;
            box_alpha(p,32,y,417,SETTINGS_ROW_HEIGHT,
                      i==selected?.20f:.09f,i==selected?.29f:.11f,
                      i==selected?.37f:.14f,1.f,16);
            if(i==selected){
                float r,g,b;accent(&r,&g,&b);
                box_alpha(p,32,y+11,5,42,r,g,b,1.f,2);
            }
            int label_y=y+(SETTINGS_ROW_HEIGHT-text_height(p,items[i],p->font))/2;
            if(i==selected){
                float r,g,b;accent(&r,&g,&b);
                text_font(p,items[i],55,label_y,372,p->font,r,g,b);
                selection_frame(p,32,y,417,SETTINGS_ROW_HEIGHT);
            }else text(p,items[i],55,label_y,372);
            press_preview(p,32,y,417,SETTINGS_ROW_HEIGHT,16);
        }
        box_alpha(p,475,136,773,480,.10f,.12f,.15f,1.f,20);
        headline(p,items[selected],513,174,685);
        char description[240]={0};
        const char *help=NULL,*action=TR("Змінити","Change");
        if(appearance){
            switch(selected){
                case APPEARANCE_THEME:
                    snprintf(description,sizeof(description),TR("Тема: %s","Theme: %s"),
                             ui_light_theme?TR("Світла","Light"):TR("Темна","Dark"));
                    help=TR("Перемикає світлу й темну теми. Вибір зберігається на SD-карті.",
                            "Switch between light and dark themes. The choice is saved.");
                    break;
                case APPEARANCE_ACCENT:
                    snprintf(description,sizeof(description),"%s",
                             ui_light_theme?TR("Акцент: синій Nintendo Switch","Accent: Nintendo Switch blue"):
                             ui_accent_alt?TR("Акцент: блакитний","Accent: Light blue"):
                                           TR("Акцент: бірюзовий","Accent: Turquoise"));
                    help=TR("Змінює колір виділення на кнопках та інших елементах інтерфейсу.",
                            "Change the highlight color of buttons and interface controls.");
                    break;
                case APPEARANCE_WALLPAPERS:
                    snprintf(description,sizeof(description),"%s",
                             *wallpapers_selected()?wallpapers_selected():
                             TR("Стандартний фон","Default background"));
                    help=TR("Галерея Wallhaven із пошуком, фільтрами та свайпами.",
                            "Browse Wallhaven wallpapers using search, filters and swipes.");
                    action=TR("Відкрити галерею","Open gallery");
                    break;
                case APPEARANCE_DEFAULT:
                    snprintf(description,sizeof(description),"%s",
                             *wallpapers_selected()?TR("Шпалери увімкнено","Wallpaper enabled"):
                             TR("Уже стандартний фон","Default background already active"));
                    help=TR("Вимикає шпалери й відновлює стандартний фон навіть після перезапуску.",
                            "Disable wallpaper and restore the default background after restart.");
                    action=TR("Вимкнути","Disable");
                    break;
            }
        }else{
            switch(selected){
                case SETTING_APPEARANCE:
                    snprintf(description,sizeof(description),"%s · %s",
                             ui_light_theme?TR("Світла тема","Light theme"):TR("Темна тема","Dark theme"),
                             *wallpapers_selected()?TR("Шпалери увімкнено","Wallpaper enabled"):
                             TR("Стандартний фон","Default background"));
                    help=TR("У цьому розділі зібрано теми, кольори та шпалери.",
                            "Customize themes, accent colors and wallpapers here.");
                    action=TR("Відкрити","Open");
                    break;
                case SETTING_PLAYER:
                    snprintf(description,sizeof(description),
                             TR("Швидкість: %.2f · гучність: %.0f","Speed: %.2f · Volume: %.0f"),
                             video_options[0],video_options[1]);
                    help=TR("Налаштуйте відео, гучність та субтитри.",
                            "Adjust playback, volume, and subtitles.");
                    action=TR("Відкрити параметри","Open player settings");
                    break;
                case SETTING_AUTOPLAY:
                    snprintf(description,sizeof(description),TR("Наступна серія: %s","Next episode: %s"),
                             autoplay_episodes?TR("Автоматично","Automatic"):TR("Вручну","Manual"));
                    help=TR("Автоматично пропонує наступну серію після завершення поточної.",
                            "Automatically queue the next episode after playback.");
                    break;
                case SETTING_PROVIDERS:
                    help=TR("Виберіть джерела, у яких UAPlay може шукати відео.",
                            "Choose which providers UAPlay uses to search for videos.");
                    action=TR("Відкрити джерела","Manage providers");
                    break;
                case SETTING_LANGUAGE:
                    snprintf(description,sizeof(description),"%s",TR("Українська","English"));
                    help=TR("Змінює мову інтерфейсу, назв і описів TMDb.",
                            "Change the interface language and TMDb titles and descriptions.");
                    action=TR("English","Українська");
                    break;
            }
        }
        text(p,description,516,255,682);
        if(help)wrapped(p,help,517,315,675,3,29,p->font_small,.73f,.78f,.84f);
        button(p,action,515,537,258,55,1);
        footer(p,status);
    } else if (page==DETAILS) {
        screen_header(p,movies[chosen].series?TR("Про серіал","About the show"):TR("Про фільм","About the movie"),"UAPlay");
        enum { DETAILS_X=32, DETAILS_Y=109, DETAILS_W=1216, DETAILS_H=507,
               DETAILS_PAD=14, DETAILS_POSTER_H=DETAILS_H-2*DETAILS_PAD,
               DETAILS_POSTER_W=DETAILS_POSTER_H*2/3,
               DETAILS_POSTER_X=DETAILS_X+DETAILS_W-DETAILS_PAD-DETAILS_POSTER_W };
        box_alpha(p,DETAILS_X,DETAILS_Y,DETAILS_W,DETAILS_H,.09f,.11f,.14f,1.f,21);
        /* Original 2:3 poster ratio, with identical 14 px gaps on the top,
           right and bottom edges of the information panel. */
        poster(p,&movies[chosen],DETAILS_POSTER_X,DETAILS_Y+DETAILS_PAD,
               DETAILS_POSTER_W,DETAILS_POSTER_H,&poster_budget);
        headline(p,movies[chosen].title,65,138,820);
        text_muted(p,movies[chosen].original,67,198,815);
        char tag[192];
        const Movie *selected=&movies[chosen];
        format_details_metadata(tag,sizeof(tag),selected,
                                selected->id==displayed_genre_id?displayed_genre:"");
        text(p,tag,66,259,815);
        summary(p,movies[chosen].overview,66,313,815);
        if(!details_button_focus)selection_frame(p,65,526,238,58);
        else selection_frame(p,320,526,255,58);
        button(p,TR("▶  Дивитися","▶  Watch"),65,526,238,58,1);
        button(p,library_favorite(&movies[chosen])?TR("✓  У вибраному","✓  In favorites"):TR("+  У вибране","+  Add to favorites"),
               320,526,255,58,0);
        footer(p,status);
    } else if(page==IPTV_CHANNELS || page==IPTV_COUNTRIES || page==IPTV_MORE_COUNTRIES || page==IPTV_GENRES){
        draw_iptv(p,page,status);
    } else if (page==PROVIDERS || page==PROVIDER_SETTINGS) {
        int configuring=page==PROVIDER_SETTINGS;
        screen_header(p,configuring?TR("Джерела","Providers"):TR("Оберіть джерело","Choose a provider"),NULL);
        if(!configuring && !enabled_providers)
            text_center_button(p,TR("Увімкніть джерело в Налаштуваннях → Джерела","Enable a provider in Settings → Providers"),
                               UI_EDGE,305,1216,110,p->font_small,.73f,.77f,.82f);
        int row=0;
        for(int i=0;i<PROVIDER_COUNT;i++){
            int enabled=(enabled_providers & (1u<<(providers[i].id-1)))!=0;
            if(!configuring && !enabled)continue;
            int selected=configuring?i==provider_settings_selected:row==selected_provider;
            list_row(p,ui_text(providers[i].name),UI_ROW_Y+row*UI_ROW_STEP,selected,
                     configuring?(enabled?TR("Увімкнено","Enabled"):TR("Вимкнено","Disabled")):NULL);
            row++;
        }
        footer(p,status);
    } else if (page==SOURCES) {
        screen_header(p,TR("Джерела відео","Video sources"),NULL);
        if(!sn)
            text_center_button(p,TR("Нічого не знайдено","No results found"),UI_EDGE,305,1216,110,
                               p->font_big,.73f,.77f,.82f);
        int offset=list_offset(p,page,source,sn),first=first_visible_row(offset);
        for(int i=first;i<sn && i<first+UI_ROWS+2;i++){
            list_row(p,sources[i].title,UI_ROW_Y+i*UI_ROW_STEP-offset,i==source,NULL);
        }
        paint_clip(p,0,0,0,0);
        footer(p,status);
    } else if (page==DOWNLOADS) {
        static DownloadInfo records[DOWNLOADS_MAX];
        int count=downloads_list(records,DOWNLOADS_MAX);
        footer_position=count?download_selected+1:0;
        footer_total=count;
        screen_header(p,TR("Завантажені","Downloads"),TR("Збережені фільми та серії","Saved movies and episodes"));
        if(!count)text_center_button(p,TR("Список порожній. Натисніть Y на джерелі або серії","No downloads yet. Press Y on a source or an episode."),
                                      UI_EDGE,305,1216,110,p->font_small,.73f,.77f,.82f);
        int offset=list_offset(p,page,download_selected,count),first=first_visible_row(offset);
        for(int i=first;i<count && i<first+UI_ROWS+2;i++){
            const DownloadInfo *d=&records[i];
            char name[300],detail[80];
            if(d->season>0 && d->episode>0)
                snprintf(name,sizeof(name),TR("%.150s · Сезон %d · Серія %d",
                                              "%.150s · Season %d · Episode %d"),
                         d->title,d->season,d->episode);
            else if(d->label[0])snprintf(name,sizeof(name),"%.150s · %.95s",d->title,d->label);
            else snprintf(name,sizeof(name),"%.190s",d->title);
            unsigned mb=(unsigned)(d->bytes>>20);
            if(d->state==DOWNLOAD_QUEUED)snprintf(detail,sizeof(detail),TR("У черзі","Queued"));
            else if(d->state==DOWNLOAD_ACTIVE && d->permille<0)
                snprintf(detail,sizeof(detail),TR("%u МБ","%u MB"),mb);
            else if(d->state==DOWNLOAD_ACTIVE)
                snprintf(detail,sizeof(detail),TR("%d%% · %u МБ","%d%% · %u MB"),d->permille/10,mb);
            else if(d->state==DOWNLOAD_DONE)snprintf(detail,sizeof(detail),TR("Готово · %u МБ","Done · %u MB"),mb);
            else snprintf(detail,sizeof(detail),TR("Помилка","Failed"));
            int y=UI_ROW_Y+i*UI_ROW_STEP-offset;
            list_row(p,name,y,i==download_selected,detail);
            if(d->state==DOWNLOAD_ACTIVE && d->permille>=0)
                box_alpha(p,UI_EDGE+12,y+UI_ROW_HEIGHT-5,1192*d->permille/1000,
                          3,.30f,.71f,.81f,1.f,1);
        }
        paint_clip(p,0,0,0,0);
        footer(p,status);
    } else if (page==SEASONS) {
        screen_header(p,TR("Вибір сезону","Seasons"),TR("Оберіть сезон","Choose a season"));
        int offset=list_offset(p,page,selected_season,season_count),first=first_visible_row(offset);
        for(int i=first;i<season_count && i<first+UI_ROWS+2;i++){
            char title[96],detail[48];
            snprintf(title,sizeof(title),TR("Сезон %d","Season %d"),season_ids[i]);
            snprintf(detail,sizeof(detail),TR("%d серій","Episodes: %d"),episodes_in_season(season_ids[i]));
            list_row(p,title,UI_ROW_Y+i*UI_ROW_STEP-offset,i==selected_season,detail);
        }
        paint_clip(p,0,0,0,0);
        footer(p,status);
    } else if (page==EPISODES) {
        char header[96];
        snprintf(header,sizeof(header),TR("Сезон %d · Серії","Season %d · Episodes"),season_ids[selected_season]);
        screen_header(p,header,TR("Оберіть серію","Choose an episode"));
        int count=episodes_in_season(season_ids[selected_season]);
        int offset=list_offset(p,page,selected_episode,count),first=first_visible_row(offset);
        int idx=0;
        for(int i=0;i<episode_count;i++) {
            if(episodes[i].season!=season_ids[selected_season])continue;
            if(i>0 && episodes[i-1].season==episodes[i].season &&
               episodes[i-1].number==episodes[i].number)continue;
            if(idx>=first && idx<first+UI_ROWS+2) {
                list_row(p,episodes[i].title,UI_ROW_Y+idx*UI_ROW_STEP-offset,
                         idx==selected_episode,NULL);
            }
            idx++;
        }
        paint_clip(p,0,0,0,0);
        footer(p,status);
    } else if (page==TRACK_PICKER) {
        screen_header(p,subtitle_picker?TR("Субтитри","Subtitles"):TR("Аудіодоріжки","Audio tracks"),TR("Оберіть доріжку","Choose a track"));
        int offset=list_offset(p,page,selected_track,track_count),first=first_visible_row(offset);
        for(int i=first;i<track_count && i<first+UI_ROWS+2;i++) {
            list_row(p,tracks[i].label,UI_ROW_Y+i*UI_ROW_STEP-offset,
                     i==selected_track,tracks[i].selected?TR("Активна","Active"):NULL);
        }
        paint_clip(p,0,0,0,0);
        if (!track_count) text_muted(p,TR("У цьому відео немає доступних доріжок","No tracks are available for this video"),60,200,1100);
        footer(p,status);
    } else if (page==SUBDL_PICKER) {
        screen_header(p,"SubDL",TR("Українська · English","Ukrainian · English"));
        if(subdl_job.thread){
            text_muted(p,subdl_job.downloading?
                       TR("Завантаження субтитрів…","Downloading subtitles…"):
                       TR("Пошук субтитрів…","Searching for subtitles…"),60,200,1100);
        }else{
            int offset=list_offset(p,page,subdl_job.selected,subdl_job.count);
            int first=first_visible_row(offset);
            for(int i=first;i<subdl_job.count && i<first+UI_ROWS+2;i++){
                const SubdlSubtitle *subtitle=&subdl_job.results[i];
                const char *language=!strcmp(subtitle->language,"uk")?
                                     TR("Українська","Ukrainian"):TR("Англійська","English");
                char label[220];
                snprintf(label,sizeof(label),"%s · %s%s",language,subtitle->release,
                         subtitle->hearing_impaired?" [SDH]":"");
                list_row(p,label,UI_ROW_Y+i*UI_ROW_STEP-offset,i==subdl_job.selected,NULL);
            }
            paint_clip(p,0,0,0,0);
            if(subdl_job.error[0])text_muted(p,subdl_job.error,60,subdl_job.count?610:200,1150);
        }
        footer(p,NULL);
    } else if (page==VIDEO_SETTINGS) {
        screen_header(p,TR("Налаштування відео","Video settings"),TR("Плеєр","Player"));
        for(int i=0;i<VIDEO_OPTION_COUNT;i++) {
            int option=visible_video_options[i];
            int y=132+i*68;
            box_alpha(p,UI_EDGE,y,1216,60,i==selected_video_option?.20f:.09f,
                      i==selected_video_option?.30f:.11f,
                      i==selected_video_option?.38f:.14f,1.f,13);
            text_font(p,ui_text(video_titles[option]),55,
                      y+(60-text_height(p,ui_text(video_titles[option]),p->font))/2,
                      830,p->font,.96f,.97f,.98f);
            char value[48];
            if(option==6){
                int quality=(int)video_options[option];
                snprintf(value,sizeof(value),"%s",quality?hls_labels[quality]:TR("Авто","Auto"));
            }
            else if(option==0)snprintf(value,sizeof(value),"%.2f",video_options[option]);
            else if(option==5)snprintf(value,sizeof(value),TR("%+.2f с","%+.2f s"),video_options[option]);
            else snprintf(value,sizeof(value),"%.0f",video_options[option]);
            text_center_button(p,value,963,y,240,60,p->font,.95f,.96f,.98f);
            if(i==selected_video_option){float r,g,b;accent(&r,&g,&b);
                box_alpha(p,UI_EDGE,y+12,4,36,r,g,b,1.f,2);}
            press_preview(p,UI_EDGE,y,1216,60,13);
        }
        text_tiny(p,TR("Якість HLS визначається потоками, які надає джерело.","HLS quality depends on the streams offered by the provider."),50,626,1150);
        footer(p,NULL);
    }
    /* Short ease-out fade after a screen/section transition; fully drawn
       frames remain responsive even while this overlay is fading. */
    Uint32 elapsed=ui_now-transition_at;
    if(elapsed<160){
        ui_animating=1;
        float remain=1.f-(float)elapsed/160.f;
        box_alpha(p,0,80,1280,UI_FOOTER_TOP-80,.034f,.042f,.054f,
                  .13f*remain*remain,0);
    }
    /* No battery on details, provider, source, season, episode or settings pages. */
    if(page==CATALOG)battery_status_vertical(p,1218,27);
}
static void command(mpv_handle *player,const char *verb,const char *arg) {
    const char *args[]={verb,arg,NULL};
    if(player) mpv_command_async(player,0,args);
}
static void apply_hls_quality(mpv_handle *player) {
    if(!player)return;
    mpv_set_property_string(player,"options/hls-bitrate",
                            hls_presets[(int)video_options[6]]);
}
static void seek(mpv_handle *player,const char *seconds) {
    const char *args[]={"seek",seconds,"relative",NULL};
    if(player) mpv_command_async(player,0,args);
}
static void change_volume(mpv_handle *player,int direction){
    double volume=video_options[1]+direction*5.0;
    if(volume<0)volume=0;
    if(volume>130)volume=130;
    if(volume==video_options[1])return;
    video_options[1]=volume;
    playback_set_value(player,"volume",volume);
}
static void draw_player_controls(Paint *p,mpv_handle *player,double offset,const char *live_title) {
    ui_theme_enabled=0; /* Video playback controls remain readable in both themes. */
    double pos=0,duration=0;
    int paused=0;
    if(player){
        if(!live_title){
            mpv_get_property(player,"time-pos",MPV_FORMAT_DOUBLE,&pos);
            mpv_get_property(player,"duration",MPV_FORMAT_DOUBLE,&duration);
            pos+=offset;
            duration+=offset;
        }
        mpv_get_property(player,"pause",MPV_FORMAT_FLAG,&paused);
    }
    glViewport(0,0,1280,720);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
    box_alpha(p,32,27,141,49,.045f,.063f,.079f,.92f,14);
    text_center(p,TR("‹  Назад","‹  Back"),32,37,141,p->font_small,.96f,.97f,.99f);
    if(active_episode_variant>=0 &&
       catalog_next_episode(episodes,episode_count,active_episode_variant)>=0){
        box_alpha(p,869,27,241,49,.045f,.063f,.079f,.92f,14);
        text_center(p,TR("»  Наступна серія","»  Next episode"),869,37,241,
                    p->font_small,.96f,.97f,.99f);
    }
    box_alpha(p,0,507,1280,213,.027f,.034f,.044f,.93f,0);
    if(live_title)text(p,live_title,35,530,1140);
    else text_tiny(p,TR("UAPlay  ·  Відтворення","UAPlay  ·  Playback"),35,519,430);
    if(active_episode_variant>=0 &&
       catalog_next_episode(episodes,episode_count,active_episode_variant)>=0)
        text_tiny(p,TR("L — Наступна серія","L — Next episode"),790,519,310);
    if(live_title){
        text_small(p,TR("●  Прямий ефір","●  Live"),35,569,320);
    }else{
        box_alpha(p,126,566,1028,6,.34f,.39f,.44f,1.f,3);
        int filled=duration>0?(int)(1028.0*pos/duration):0;
        if(filled<0)filled=0;if(filled>1028)filled=1028;
        if(filled>0)box_alpha(p,126,566,filled,6,.92f,.94f,.97f,1.f,3);
        box_alpha(p,126+filled-8,559,16,20,.95f,.96f,.98f,1.f,8);
        char left[24],right[24];
        snprintf(left,sizeof(left),"%02d:%02d",(int)pos/60,(int)pos%60);
        snprintf(right,sizeof(right),"%02d:%02d",(int)duration/60,(int)duration%60);
        text_small(p,left,35,552,85);
        text_small(p,right,1167,552,88);
    }
    text_tiny(p,TR("↑↓ Гучність","↑↓ Volume"),67,606,190);
    box_alpha(p,70,649,178,5,.35f,.42f,.52f,1.f,3);
    int volume=(int)video_options[1];
    if(volume<0)volume=0;if(volume>130)volume=130;
    if(volume>0)box_alpha(p,70,649,volume*178/130,5,.92f,.94f,.97f,1.f,3);
    box_alpha(p,70+volume*178/130-7,641,14,20,.96f,.97f,.98f,1.f,7);
    char volume_label[24];snprintf(volume_label,sizeof(volume_label),"%d%%",volume);
    text_tiny(p,volume_label,122,672,100);
    const char *icons[]={"−10",paused?"▶":"Ⅱ","+10","","♪","CC"};
    const char *labels[]={TR("10 с назад","Back 10 s"),TR("Пауза / пуск","Play / pause"),TR("10 с вперед","Forward 10 s"),
                          TR("Параметри","Settings"),TR("Аудіо","Audio"),TR("Субтитри","Subtitles")};
    if(live_title){
        icons[0]="‹";icons[2]="›";
        labels[0]=TR("Попередній канал","Previous channel");
        labels[2]=TR("Наступний канал","Next channel");
    }
    for(int i=0;i<6;i++){
        const int w=76;
        int center=player_button_centers[i];
        int inset=(int)lroundf(3.f*press_amount(center-w/2,610,w,58));
        box_alpha(p,center-w/2+inset,610+inset,w-2*inset,58-2*inset,.18f,.22f,.26f,.94f,15);
        text_center(p,icons[i],center-w/2,624,w,
                    i==0||i==2?p->font_small:p->font,.98f,.98f,.99f);
        if(i==3)gear_icon(p,center,639,.18f,.22f,.26f);
        text_center(p,labels[i],center-72,678,144,p->font_tiny,.78f,.84f,.89f);
    }
}
static GLuint video_framebuffer(GLuint *texture){
    GLuint fbo=0;
    glGenTextures(1,texture);
    glBindTexture(GL_TEXTURE_2D,*texture);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,1280,720,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    glGenFramebuffers(1,&fbo);
    glBindFramebuffer(GL_FRAMEBUFFER,fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,*texture,0);
    GLenum status=glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER,0);
    if(status==GL_FRAMEBUFFER_COMPLETE)return fbo;
    glDeleteFramebuffers(1,&fbo);
    glDeleteTextures(1,texture);
    *texture=0;
    return 0;
}
static void draw_next_episode_prompt(Paint *p,const Episode *next,int seconds){
    ui_theme_enabled=0;
    box_alpha(p,0,0,1280,720,.012f,.018f,.027f,.88f,0);
    box_alpha(p,242,190,796,341,.105f,.144f,.194f,.98f,24);
    headline(p,TR("Наступна серія","Next episode"),282,223,700);
    char episode_name[120];
    snprintf(episode_name,sizeof(episode_name),TR("Сезон %d · Серія %d","Season %d · Episode %d"),
             next->season,next->number);
    text(p,episode_name,286,302,697);
    char timer[128];
    snprintf(timer,sizeof(timer),TR("Автозапуск через %d с","Starting in %d s"),seconds);
    text_small(p,timer,286,376,681);
    text_small(p,TR("A — дивитися зараз     B — скасувати","A — Watch now     B — Cancel"),286,448,690);
}
typedef enum { UPDATE_OFFER, UPDATE_DETAILS, UPDATE_DOWNLOADING,
               UPDATE_FAILED } UpdateScreen;
enum { UPDATE_CHECK_JOB, UPDATE_NOTES_JOB, UPDATE_FILE_JOB,
       UPDATE_NOTE_ROWS=14, UPDATE_LINE_SIZE=192, UPDATE_MAX_LINES=256 };

static struct {
    SDL_Thread *thread;
    SDL_atomic_t done;
    atomic_uint progress;
    atomic_int cancel;
    int job,result,available,focused,first_line,line_count,cancelled,notes_loaded;
    UpdateScreen screen;
    UpdateRelease release;
    char path[1024];
    char notes[UPDATE_NOTES_SIZE];
    char lines[UPDATE_MAX_LINES][UPDATE_LINE_SIZE];
} app_update;

static int update_worker(void *unused){
    (void)unused;
    if(app_update.job==UPDATE_CHECK_JOB)
        app_update.result=update_check(&app_update.release);
    else if(app_update.job==UPDATE_NOTES_JOB)
        app_update.result=update_details(&app_update.release,app_update.notes,sizeof(app_update.notes));
    else app_update.result=update_download(&app_update.release,app_update.path,
                                          &app_update.progress,&app_update.cancel);
    SDL_AtomicSet(&app_update.done,1);
    return 0;
}

static int update_start(int job){
    app_update.job=job;
    SDL_AtomicSet(&app_update.done,0);
    atomic_store(&app_update.cancel,0);
    atomic_store(&app_update.progress,0);
    app_update.cancelled=0;
    app_update.thread=SDL_CreateThread(update_worker,"update",NULL);
    return app_update.thread!=NULL;
}

/* Keep paragraph boundaries and break long lines at a UTF-8 boundary. */
static void update_wrap_notes(Paint *p){
    const char *cursor=app_update.notes;
    app_update.line_count=0;
    app_update.first_line=0;
    while(*cursor && app_update.line_count<(int)(sizeof(app_update.lines)/sizeof(app_update.lines[0]))){
        while(*cursor==' ' || *cursor=='#')cursor++;
        char *line=app_update.lines[app_update.line_count++];
        size_t bytes=0,last_space=0;
        while(cursor[bytes] && cursor[bytes]!='\n'){
            size_t next=bytes+1;
            while(((unsigned char)cursor[next]&0xc0)==0x80)next++;
            if(next>=UPDATE_LINE_SIZE)break;
            memcpy(line+bytes,cursor+bytes,next-bytes);
            line[next]=0;
            if(bytes && measure(p,line,p->font_small)>1100)break;
            if(cursor[bytes]==' ')last_space=bytes;
            bytes=next;
        }
        if(cursor[bytes] && cursor[bytes]!='\n' && last_space)
            bytes=last_space;
        line[bytes]=0;
        cursor+=bytes;
        if(*cursor=='\n')cursor++;
    }
}

static void draw_update(Paint *p){
    ui_theme_enabled=0;
    box(p,0,0,1280,720,.055f,.069f,.091f);
    if(app_update.screen==UPDATE_DETAILS){
        screen_header(p,TR("Деталі оновлення","Update details"),app_update.release.version);
        if(app_update.thread)
            text_small(p,TR("Завантаження опису…","Loading release notes…"),90,125,1080);
        else if(!app_update.line_count)
            text_small(p,app_update.notes_loaded?TR("Опис релізу відсутній","No release notes"):
                       TR("Не вдалося завантажити опис. A — повторити","Could not load notes. A — Retry"),90,125,1080);
        for(int i=0;i<UPDATE_NOTE_ROWS && app_update.first_line+i<app_update.line_count;i++)
            text_small(p,app_update.lines[app_update.first_line+i],90,118+i*34,1100);
        text_small(p,TR("↑↓ — Прокрутити     B — Назад","↑↓ — Scroll     B — Back"),90,658,1100);
        return;
    }
    box_alpha(p,210,175,860,370,.095f,.125f,.170f,1.f,24);
    if(app_update.screen==UPDATE_DOWNLOADING){
        headline(p,TR("Завантаження оновлення","Downloading update"),258,215,760);
        unsigned percent=atomic_load(&app_update.progress);
        box_alpha(p,262,332,756,10,.30f,.34f,.40f,1.f,5);
        if(percent)box_alpha(p,262,332,756*(int)percent/100,10,.88f,.92f,.98f,1.f,5);
        char status[24];
        snprintf(status,sizeof(status),"%u%%",percent);
        text_small(p,status,262,357,145);
        text_small(p,app_update.cancelled?TR("Скасування…","Canceling…"):TR("B — Скасувати","B — Cancel"),262,445,755);
    }else if(app_update.screen==UPDATE_FAILED){
        headline(p,TR("Не вдалося оновити UAPlay","Could not update UAPlay"),258,215,760);
        const char *reason=app_update.result==UPDATE_INVALID?
            TR("Файл не пройшов перевірку цілісності.","The file failed verification."):
            app_update.result==UPDATE_SD?TR("Перевірте доступ і вільне місце на SD-карті.",
                                            "Check SD card access and free space."):
            TR("Не вдалося завантажити файл із GitHub.","Could not download the file from GitHub.");
        text_small(p,reason,262,312,755);
        text_small(p,TR("A — Повторити     B — Назад","A — Retry     B — Back"),262,445,755);
    }else{
        headline(p,TR("Доступне оновлення UAPlay","UAPlay update available"),258,215,760);
        char label[96];
        snprintf(label,sizeof(label),TR("Версія %s → %s","Version %s → %s"),UAPLAY_VERSION,app_update.release.version);
        text(p,label,262,304,750);
        text_small(p,TR("Після встановлення застосунок закриється.","The app will close after installation."),262,357,755);
        button(p,TR("Встановити","Install"),262,439,248,56,app_update.focused==0);
        button(p,TR("Деталі","Details"),524,439,198,56,app_update.focused==1);
        button(p,TR("Пізніше","Later"),736,439,198,56,app_update.focused==2);
    }
}


static int native_button(int button) {
    /* SDL uses Xbox-style face button names; Switch prints the opposite labels. */
    switch(button) {
        case SDL_CONTROLLER_BUTTON_A:return SDL_CONTROLLER_BUTTON_B;
        case SDL_CONTROLLER_BUTTON_B:return SDL_CONTROLLER_BUTTON_A;
        case SDL_CONTROLLER_BUTTON_X:return SDL_CONTROLLER_BUTTON_Y;
        case SDL_CONTROLLER_BUTTON_Y:return SDL_CONTROLLER_BUTTON_X;
        default:return button;
    }
}
static void stick_arrow(SDL_GameController *pad,Uint32 *last_time,int *last_key) {
    if(!pad || !SDL_GameControllerGetAttached(pad))return;
    const int deadzone=19000;
    int lx=SDL_GameControllerGetAxis(pad,SDL_CONTROLLER_AXIS_LEFTX);
    int ly=SDL_GameControllerGetAxis(pad,SDL_CONTROLLER_AXIS_LEFTY);
    int rx=SDL_GameControllerGetAxis(pad,SDL_CONTROLLER_AXIS_RIGHTX);
    int ry=SDL_GameControllerGetAxis(pad,SDL_CONTROLLER_AXIS_RIGHTY);
    int key=0;
    if(abs(ly)>deadzone || abs(lx)>deadzone)
        key=abs(ly)>=abs(lx)?(ly>0?SDLK_DOWN:SDLK_UP):(lx>0?SDLK_RIGHT:SDLK_LEFT);
    else if(abs(ry)>deadzone || abs(rx)>deadzone)
        key=abs(ry)>=abs(rx)?(ry>0?SDLK_DOWN:SDLK_UP):(rx>0?SDLK_RIGHT:SDLK_LEFT);
    Uint32 now=SDL_GetTicks();
    if(!key){*last_key=0;return;}
    if(key==*last_key && now-*last_time<175)return;
    *last_key=key;*last_time=now;
    SDL_Event event={0};event.type=SDL_KEYDOWN;event.key.keysym.sym=key;
    SDL_PushEvent(&event);
}
static void configure_stream(mpv_handle *player,const char *url) {
    if(!player)return;
    mpv_set_property_string(player,"user-agent","Mozilla/5.0 (Nintendo Switch; UAPlay/1.0)");
    mpv_set_property_string(player,"referrer",stream_referer(url));
}
typedef struct {
    Uint32 seek_started;
    double seek_target;
    double offset;
    char external_subtitle[2048];
    int retried;
    int quality;
    int sliced;
} MovyPlayback;

static void load_movy_stream(mpv_handle *player,const char *url){
    configure_stream(player,url);
    const char *args[]={"loadfile",url,"replace",
        "demuxer-lavf-o=%65%http_persistent=0,http_multiple=0,http_seekable=0,seg_max_retry=2,network-timeout=60",NULL};
    mpv_command_async(player,0,args);
}

static void play_stream_at_quality(mpv_handle *player,const char *url,int quality){
    if(!player)return;
    char local[96];
    if(movy_stream_url(url)){
        static const long movy_bitrates[]={3600000,1000000,1800000,3600000,6800000};
        long bitrate=movy_bitrates[quality];
        if(movy_stream_open(url,bitrate,local,sizeof(local))){
            load_movy_stream(player,local);
            return;
        }
    }else movy_stream_close();
    configure_stream(player,url);
    command(player,"loadfile",url);
}
static void play_stream(mpv_handle *player,const char *url,MovyPlayback *state){
    if(state->offset>0)playback_set_value(player,"sub-delay",video_options[5]);
    *state=(MovyPlayback){.quality=(int)video_options[6]};
    play_stream_at_quality(player,url,state->quality);
}

static void play_iptv_channel(mpv_handle *player,char *url,size_t size,
                              MovyPlayback *movy,Uint32 *started,int *loaded){
    const IptvChannel *channel=iptv_channel(tv.selected);
    snprintf(url,size,"%s",channel->url);
    if(movy->offset>0)playback_set_value(player,"sub-delay",video_options[5]);
    *movy=(MovyPlayback){0};
    movy_stream_close();
    mpv_set_property_string(player,"user-agent",channel->agent && channel->agent[0]?
                            channel->agent:"Mozilla/5.0 (Nintendo Switch; UAPlay/1.0)");
    mpv_set_property_string(player,"referrer",channel->referrer?channel->referrer:"");
    mpv_set_property_string(player,"pause","no");
    apply_hls_quality(player);
    tv.entry=0;
    tv.playing_since=0;
    command(player,"loadfile",url);
    *started=SDL_GetTicks();
    *loaded=0;
}

static bool iptv_skip_failed(mpv_handle *player,char *url,size_t size,
                             MovyPlayback *movy,Uint32 *started,int *loaded){
    if(++tv.failures>=tv.count)return false;
    tv.selected=(tv.selected+1)%tv.count;
    play_iptv_channel(player,url,size,movy,started,loaded);
    return true;
}

static int movy_open_at(mpv_handle *player,MovyPlayback *state,double target){
    char local[96];
    if(!movy_stream_seek(target,&state->offset,local,sizeof(local)))return 0;
    state->seek_started=SDL_GetTicks();
    state->seek_target=target;
    state->sliced=1;
    load_movy_stream(player,local);
    return 1;
}
static void movy_seek_to(mpv_handle *player,MovyPlayback *state,double target,int *loaded){
    state->external_subtitle[0]=0;
    char *subtitle=mpv_get_property_string(player,"current-tracks/sub/external-filename");
    if(subtitle){
        snprintf(state->external_subtitle,sizeof(state->external_subtitle),"%s",subtitle);
        mpv_free(subtitle);
    }
    state->retried=0;
    /* Reopen the playlist on a fragment boundary. FFmpeg can reach a
       stale fragment index when seeking within an open fMP4 playlist. */
    if(movy_open_at(player,state,target)){
        *loaded=0;
        return;
    }
    char seconds[48];
    snprintf(seconds,sizeof(seconds),"%.2f",target);
    state->seek_started=SDL_GetTicks();
    state->seek_target=target;
    const char *args[]={"seek",seconds,"absolute+keyframes",NULL};
    mpv_command_async(player,0,args);
}
static int retry_movy(mpv_handle *player,MovyPlayback *state,const char *url,
                      int loaded,double *resume,Uint32 *started){
    if(!movy_stream_url(url) || state->retried || (loaded && !state->seek_started))return 0;
    state->retried=1;
    if(state->seek_started){
        if(movy_open_at(player,state,state->seek_target))return 1;
        *resume=state->seek_target;
        state->seek_started=0;
    }
    state->offset=0;
    state->sliced=0;
    if(!loaded && state->quality==0)state->quality=2;
    play_stream_at_quality(player,url,state->quality);
    *started=SDL_GetTicks();
    return 1;
}
static int keyboard(char *value,size_t size) {
    SwkbdConfig cfg;
    if(R_FAILED(swkbdCreate(&cfg,0))) return 0;
    swkbdConfigMakePresetDefault(&cfg);
    Result result=swkbdShow(&cfg,value,size);
    swkbdClose(&cfg);
    return R_SUCCEEDED(result) && value[0];
}
int main(int argc,char **argv) {
    romfsInit();
    library_init();
    theme_load();
    Result network_result = socketInitializeDefault();
    if (R_SUCCEEDED(network_result)) curl_global_init(CURL_GLOBAL_DEFAULT);
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS,"1");
    if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMECONTROLLER)) return 1;
    battery_service_ready=R_SUCCEEDED(psmInitialize());
    battery_status_poll();
    catalog_http_init();
    downloads_init();
    int can_update=R_SUCCEEDED(network_result) && argc>0 &&
                   update_path(argv[0],app_update.path,sizeof(app_update.path));
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,0);
    SDL_Window *window=SDL_CreateWindow("UAPlay",0,0,1280,720,SDL_WINDOW_OPENGL|SDL_WINDOW_SHOWN);
    if(!window) { SDL_Quit();return 2; }
    SDL_GLContext gl=SDL_GL_CreateContext(window);
    if(!gl) { SDL_DestroyWindow(window);SDL_Quit();return 3; }
    Paint paint={0};
    int install_update=0;
    if(!paint_init(&paint)) goto cleanup;
    posters_init();
    wallpapers_init();
    SDL_GameController *pad=NULL;
    for(int i=0;i<SDL_NumJoysticks();i++)
        if(SDL_IsGameController(i)){pad=SDL_GameControllerOpen(i);break;}
    mpv_handle *player=mpv_create();
    mpv_render_context *render=NULL;
    if(player){
        mpv_set_option_string(player,"vo","libmpv");
        mpv_set_option_string(player,"hwdec","auto-safe");
        mpv_set_option_string(player,"terminal","no");
        mpv_set_option_string(player,"network-timeout","20");
        /* The Switch's libass build has no system font provider. mpv reads
           subfont.ttf from this read-only configuration directory. */
        mpv_set_option_string(player,"config","yes");
        mpv_set_option_string(player,"config-dir","romfs:/");
        mpv_set_option_string(player,"sub-font","DejaVu Sans");
        mpv_set_option_string(player,"sub-visibility","yes");
        mpv_set_option_string(player,"sub-auto","fuzzy");
        mpv_set_option_string(player,"cache","yes");
        /* Retain enough buffering for high bitrate streams while constraining
           old frames that remain in the back-buffer after seeking. */
        mpv_set_option_string(player,"demuxer-max-bytes","16MiB");
        mpv_set_option_string(player,"demuxer-max-back-bytes","2MiB");
        mpv_set_option_string(player,"video-sync","audio");
        mpv_request_log_messages(player,"warn");
        if(mpv_initialize(player)>=0){
            mpv_opengl_init_params gl_init={.get_proc_address=proc};
            mpv_render_param init[]={{MPV_RENDER_PARAM_API_TYPE,MPV_RENDER_API_TYPE_OPENGL},
                                    {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS,&gl_init},
                                    {MPV_RENDER_PARAM_INVALID,NULL}};
            mpv_render_context_create(&render,player,init);
        }
    }
    int movie_capacity=CATALOG_LOCAL_MAX;
    Movie *movies=calloc((size_t)movie_capacity,sizeof(Movie));
    if(!movies)goto cleanup;
    Source sources[UAKINO_MAX]={0};
    int n=0,sn=0,chosen=0,source=0,category=0,running=1;
    int search_return_category=0,search_return_expanded=0;
    int search_return_shelf=0,search_return_chosen=0;
    Page page=CATALOG;
    char query[192]={0},message[256]={0};
    catalog_loader_init();
    if(can_update)update_start(UPDATE_CHECK_JOB);
    n=load_category(category,query,movies,message,sizeof(message));
    Movie playing_movie={0};
    double resume_at=0;
    char pending_subtitle[1024]={0};
    char playing_url[2048]={0};
    Page player_return_page=SOURCES;
    Uint32 video_started_at=0;
    int video_loaded=0;
    MovyPlayback movy={0};
    GLuint movy_texture=0,movy_fbo=0;
    int movy_frame_valid=0;
    int history_playback_committed=0;
    Uint32 controls_until=0;
    int pending_next_episode=-1,next_episode_request=-1;
    Uint32 next_episode_countdown_at=0;
    Uint32 stick_last_time=0;
    int stick_last_key=0;
    int media_playback_mode=0;
    int catalog_language_changed=0;
    TouchGesture touch={0};
    ui_touch=&touch;
    int pending_key=-1;
    Page pending_key_page=CATALOG;
    static DownloadInfo download_rows[DOWNLOADS_MAX];
    Page update_return=CATALOG;
    while(running){
        Uint32 frame_started_at=SDL_GetTicks();
        int old_wall_revision=wallpapers_revision();
        wallpapers_poll();
        if(old_wall_revision!=wallpapers_revision()){
            if(wall_api_dir){
                wall_selected=wall_api_dir<0?
                    ((wallpapers_count()-1)/WALL_PAGE)*WALL_PAGE+
                       WALL_COLS+wall_api_column:wall_api_column;
            }
            wall_rebuild();
            if(wall_api_dir){
                wall_start_animation(-1,wall_selected/WALL_PAGE*WALL_PAGE,wall_api_dir);
                wall_api_dir=0;
            }
        }
        if(app_update.thread && SDL_AtomicGet(&app_update.done)){
            SDL_WaitThread(app_update.thread,NULL);
            app_update.thread=NULL;
            if(app_update.job==UPDATE_CHECK_JOB)app_update.available=app_update.result;
            else if(app_update.job==UPDATE_NOTES_JOB){
                app_update.notes_loaded=app_update.result;
                if(app_update.notes_loaded)update_wrap_notes(&paint);
            }else if(app_update.result==UPDATE_OK && !app_update.cancelled){
                install_update=1;
                break;
            }else{
                app_update.screen=app_update.cancelled?UPDATE_OFFER:UPDATE_FAILED;
            }
        }
        if(app_update.available && page==CATALOG){
            app_update.available=0;
            app_update.screen=UPDATE_OFFER;
            update_return=page;
            page=UPDATE_DIALOG;
        }
        if(page==IPTV_CHANNELS || page==IPTV_COUNTRIES || page==IPTV_MORE_COUNTRIES || page==IPTV_GENRES){
            int refreshed=iptv_finish();
            if(refreshed)snprintf(message,sizeof(message),"%s",refreshed>0?"":
                TR("Не вдалося завантажити список IPTV","Could not load the IPTV list"));
        }
        stick_arrow(pad,&stick_last_time,&stick_last_key);
        SDL_Event e;
        for(;;){
            int replay=pending_key>=0;
            if(replay){
                if(ui_press_page==page && SDL_GetTicks()-ui_press_at<PRESS_PREVIEW_MS)break;
                e=(SDL_Event){0};
            }else if(!SDL_PollEvent(&e))break;
            if(e.type==SDL_QUIT)running=0;
            int key=replay && pending_key_page==page?pending_key:-1;
            pending_key=-1;
            if(e.type==SDL_CONTROLLERBUTTONDOWN)key=native_button(e.cbutton.button);
            if(e.type==SDL_KEYDOWN){
                switch(e.key.keysym.sym){
                    case SDLK_RETURN:key=SDL_CONTROLLER_BUTTON_A;break;
                    case SDLK_BACKSPACE:key=SDL_CONTROLLER_BUTTON_B;break;
                    case SDLK_UP:key=SDL_CONTROLLER_BUTTON_DPAD_UP;break;
                    case SDLK_DOWN:key=SDL_CONTROLLER_BUTTON_DPAD_DOWN;break;
                    case SDLK_LEFT:key=SDL_CONTROLLER_BUTTON_DPAD_LEFT;break;
                    case SDLK_RIGHT:key=SDL_CONTROLLER_BUTTON_DPAD_RIGHT;break;
                    case SDLK_x:key=SDL_CONTROLLER_BUTTON_X;break;
                    case SDLK_y:key=SDL_CONTROLLER_BUTTON_Y;break;
                    case SDLK_z:key=SDL_CONTROLLER_BUTTON_RIGHTSTICK;break;
                    case SDLK_TAB:key=(e.key.keysym.mod & KMOD_SHIFT)?
                        SDL_CONTROLLER_BUTTON_LEFTSHOULDER:SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;break;
                    case SDLK_m:key=SDL_CONTROLLER_BUTTON_BACK;break;
                    case SDLK_ESCAPE:key=SDL_CONTROLLER_BUTTON_START;break;
                }
            }
            if(touch.page!=page || key>=0 || e.type==SDL_APP_WILLENTERBACKGROUND ||
               (e.type==SDL_WINDOWEVENT && e.window.event==SDL_WINDOWEVENT_FOCUS_LOST))
                touch.active=0;
            if(page==UPDATE_DIALOG){
                if(e.type==SDL_FINGERUP || (e.type==SDL_MOUSEBUTTONUP && e.button.which!=SDL_TOUCH_MOUSEID)){
                    int x=e.type==SDL_FINGERUP?(int)(e.tfinger.x*1280):e.button.x;
                    int y=e.type==SDL_FINGERUP?(int)(e.tfinger.y*720):e.button.y;
                    if(app_update.screen==UPDATE_OFFER && y>=439 && y<495 && x>=262 && x<934){
                        app_update.focused=x<524?0:x<736?1:2;
                        key=SDL_CONTROLLER_BUTTON_A;
                    }else if(app_update.screen==UPDATE_DETAILS && y<92 && x<100)
                        key=SDL_CONTROLLER_BUTTON_B;
                }
                if(app_update.screen==UPDATE_DOWNLOADING){
                    if(key==SDL_CONTROLLER_BUTTON_B && app_update.thread){
                        app_update.cancelled=1;
                        atomic_store(&app_update.cancel,1);
                    }
                    continue;
                }
                if(app_update.screen==UPDATE_DETAILS){
                    if(key==SDL_CONTROLLER_BUTTON_B)app_update.screen=UPDATE_OFFER;
                    if(key==SDL_CONTROLLER_BUTTON_A && !app_update.notes_loaded && !app_update.thread)
                        update_start(UPDATE_NOTES_JOB);
                    if(key==SDL_CONTROLLER_BUTTON_DPAD_UP && app_update.first_line>0)app_update.first_line--;
                    if(key==SDL_CONTROLLER_BUTTON_DPAD_DOWN &&
                       app_update.first_line+UPDATE_NOTE_ROWS<app_update.line_count)app_update.first_line++;
                    continue;
                }
                if(app_update.screen==UPDATE_FAILED){
                    if(key==SDL_CONTROLLER_BUTTON_B)app_update.screen=UPDATE_OFFER;
                    if(key!=SDL_CONTROLLER_BUTTON_A)continue;
                    app_update.focused=0;
                }
                if(key==SDL_CONTROLLER_BUTTON_X){
                    app_update.screen=UPDATE_DETAILS;
                    if(!app_update.thread && !app_update.notes_loaded)update_start(UPDATE_NOTES_JOB);
                    continue;
                }
                if(key==SDL_CONTROLLER_BUTTON_DPAD_LEFT && app_update.focused>0)app_update.focused--;
                if(key==SDL_CONTROLLER_BUTTON_DPAD_RIGHT && app_update.focused<2)app_update.focused++;
                if(key==SDL_CONTROLLER_BUTTON_B ||
                   (key==SDL_CONTROLLER_BUTTON_A && app_update.focused==2))page=update_return;
                else if(key==SDL_CONTROLLER_BUTTON_A && app_update.focused==1){
                    app_update.screen=UPDATE_DETAILS;
                    if(!app_update.thread && !app_update.notes_loaded)update_start(UPDATE_NOTES_JOB);
                }else if(key==SDL_CONTROLLER_BUTTON_A && !app_update.thread){
                    if(update_start(UPDATE_FILE_JOB))app_update.screen=UPDATE_DOWNLOADING;
                    else{
                        app_update.result=UPDATE_NETWORK;
                        app_update.screen=UPDATE_FAILED;
                    }
                }
                continue;
            }
            int tx=-1,ty=-1;
            if(e.type==SDL_FINGERDOWN || e.type==SDL_FINGERMOTION || e.type==SDL_FINGERUP){
                int x=(int)(e.tfinger.x*1280),y=(int)(e.tfinger.y*720);
                if(page==PLAYER){
                    if(e.type==SDL_FINGERDOWN ||
                       (e.type==SDL_FINGERMOTION && video_loaded && SDL_GetTicks()<controls_until)){
                        tx=x;ty=y;
                    }
                }else{
                    /* Activate the existing touch targets only after a tap is released. */
                    TouchAction action=touch_gesture(&touch,&e.tfinger,page,x,y);
                    if(action==TOUCH_TAP){
                        tx=touch.x;ty=touch.y;
                    }else if(action!=TOUCH_NONE && touch.x>=UI_EDGE && touch.x<1280-UI_EDGE){
                        int vertical=action==SWIPE_UP || action==SWIPE_DOWN;
                        int direction=action==SWIPE_LEFT || action==SWIPE_UP?1:-1;
                        if(page==CATALOG){
                            int library=category==11 || category==12;
                            int home=category==0 && !expanded_catalog && !query[0];
                            if(library || expanded_catalog){
                                int top=library?LIBRARY_GRID_Y:POSTER_GRID_Y;
                                int bottom=library?POSTER_ROW_BOTTOM:POSTER_GRID_BOTTOM;
                                if(vertical && touch.y>=top && touch.y<bottom){
                                    chosen=swipe_page_selection(chosen,n,2*POSTER_COLUMNS,direction);
                                    catalog_hero_focus=0;
                                    if(clear_history_confirm)message[0]=0;
                                    clear_history_confirm=0;
                                }
                            }else if(touch.y>=POSTER_ROW_Y && touch.y<POSTER_ROW_BOTTOM){
                                if(vertical && home){
                                    int next=home_shelf+direction;
                                    if(next>=0 && next<HOME_SHELF_COUNT){
                                        home_shelf=next;
                                        n=load_category(category,query,movies,message,sizeof(message));
                                        chosen=0;
                                        catalog_hero_focus=0;
                                    }
                                }else if(!vertical && n){
                                    if(home){
                                        int step=POSTER_COLUMNS-1;
                                        int last=poster_row_start(chosen,1)+step;
                                        int next=last+direction*step;
                                        chosen=next<0?0:next>=n?n-1:next;
                                    }else chosen=swipe_page_selection(chosen,n,POSTER_COLUMNS,direction);
                                    catalog_hero_focus=0;
                                }
                            }
                        }else if(page==WALLPAPERS && vertical &&
                                  touch.y>=WALL_Y && touch.y<618){
                            /* Vertical swipe like the film grid. */
                            wall_focus=0;
                            wall_move_page(direction);
                        }else if(page==IPTV_CHANNELS && vertical &&
                                 touch.y>=IPTV_GRID_Y && touch.y<IPTV_GRID_BOTTOM){
                            tv.focus=IPTV_FOCUS_GRID;
                            tv.selected=swipe_page_selection(tv.selected,tv.count,IPTV_PAGE_SIZE,direction);
                        }else if(vertical && touch.y>=UI_ROW_Y && touch.y<UI_ROW_Y+UI_ROWS*UI_ROW_STEP){
                            int *selected=NULL,count=0;
                            switch(page){
                                case SOURCES:selected=&source;count=sn;break;
                                case IPTV_COUNTRIES:case IPTV_MORE_COUNTRIES:case IPTV_GENRES:
                                    selected=&tv.filter_row;count=iptv_filter_count(page);break;
                                case SEASONS:selected=&selected_season;count=season_count;break;
                                case EPISODES:
                                    selected=&selected_episode;
                                    if(selected_season<season_count)
                                        count=episodes_in_season(season_ids[selected_season]);
                                    break;
                                case TRACK_PICKER:selected=&selected_track;count=track_count;break;
                                case SUBDL_PICKER:
                                    if(!subdl_job.thread){
                                        selected=&subdl_job.selected;count=subdl_job.count;
                                    }
                                    break;
                                case DOWNLOADS:
                                    selected=&download_selected;
                                    count=downloads_list(download_rows,DOWNLOADS_MAX);
                                    if(download_confirm)message[0]=0;
                                    download_confirm=0;
                                    break;
                                default:break;
                            }
                            if(selected)*selected=swipe_page_selection(*selected,count,UI_ROWS,direction);
                        }
                    }
                }
            }else if(page==WALLPAPERS && e.type==SDL_MOUSEBUTTONDOWN &&
                     e.button.button==SDL_BUTTON_LEFT && e.button.which!=SDL_TOUCH_MOUSEID){
                touch.active=0;
                int ww=1280,hh=720;
                SDL_GetWindowSize(window,&ww,&hh);
                wall_mouse_x=e.button.x*1280/(ww?ww:1280);
                wall_mouse_y=e.button.y*720/(hh?hh:720);
                wall_mouse_active=1;
            }else if(page==WALLPAPERS && e.type==SDL_MOUSEBUTTONUP &&
                     e.button.button==SDL_BUTTON_LEFT && e.button.which!=SDL_TOUCH_MOUSEID){
                if(wall_mouse_active){
                    wall_mouse_active=0;
                    int ww=1280,hh=720;
                    SDL_GetWindowSize(window,&ww,&hh);
                    int end_x=e.button.x*1280/(ww?ww:1280);
                    int end_y=e.button.y*720/(hh?hh:720);
                    int dx=end_x-wall_mouse_x,dy=end_y-wall_mouse_y;
                    int ax=abs(dx),ay=abs(dy);
                    if(wall_mouse_y>=WALL_Y && wall_mouse_y<618 &&
                       wall_mouse_x>=WALL_X && wall_mouse_x<1248 &&
                       ay>=52 && 4*ay>=5*ax){
                        wall_focus=0;
                        wall_move_page(dy<0?1:-1);
                        continue;
                    }
                    if(ax<=20 && ay<=20){
                        tx=wall_mouse_x;ty=wall_mouse_y;
                    }
                }
            }else if(e.type==SDL_MOUSEBUTTONDOWN &&
                     e.button.button==SDL_BUTTON_LEFT && e.button.which!=SDL_TOUCH_MOUSEID){
                touch.active=0;
                int ww=1280,hh=720;
                SDL_GetWindowSize(window,&ww,&hh);
                tx=e.button.x*1280/(ww?ww:1280);
                ty=e.button.y*720/(hh?hh:720);
            }else if(page==PLAYER && e.type==SDL_MOUSEMOTION &&
                     e.motion.which!=SDL_TOUCH_MOUSEID &&
                     (e.motion.state&SDL_BUTTON_LMASK) && video_loaded &&
                     SDL_GetTicks()<controls_until){
                int ww=1280,hh=720;
                SDL_GetWindowSize(window,&ww,&hh);
                tx=e.motion.x*1280/(ww?ww:1280);
                ty=e.motion.y*720/(hh?hh:720);
            }
            if(tx>=0 && ty>=0){
                if(page!=PLAYER){
                    ui_press_at=SDL_GetTicks();
                    ui_press_active=1;
                    ui_press_page=page;
                    ui_press_is_touch=1;
                    ui_press_point=(SDL_Point){tx,ty};
                }
                if(page==PLAYER){
                    int was_visible=SDL_GetTicks()<controls_until;
                    controls_until=SDL_GetTicks()+3000;
                    if(!was_visible && video_loaded)continue;
                    if(ty>=24 && ty<=86 && tx>=27 && tx<=203)key=SDL_CONTROLLER_BUTTON_B;
                    else if(!video_loaded)continue;
                    else if(ty>=24 && ty<=83 && tx>=869 && tx<=1110 &&
                            active_episode_variant>=0 &&
                            catalog_next_episode(episodes,episode_count,
                                                 active_episode_variant)>=0)
                        key=SDL_CONTROLLER_BUTTON_LEFTSHOULDER;
                    else if(player_return_page!=IPTV_CHANNELS && ty>=547 && ty<=594 && tx>=118 && tx<=1161){
                        double duration=0;
                        if(player && video_loaded &&
                           mpv_get_property(player,"duration",MPV_FORMAT_DOUBLE,&duration)>=0 &&
                           duration>0){
                            duration+=movy.offset;
                            double to=duration*(tx-126)/1028.0;
                            if(to<0)to=0;if(to>duration)to=duration;
                            char at[48];snprintf(at,sizeof(at),"%.2f",to);
                            if(movy_stream_url(playing_url)){
                                movy_seek_to(player,&movy,fmin(to,fmax(0,duration-1)),&video_loaded);
                            }else{
                                const char *args[]={"seek",at,"absolute",NULL};
                                mpv_command_async(player,0,args);
                            }
                        }
                        continue;
                    }else if(ty>=603 && ty<703){
                        if(tx>=60 && tx<259 && ty>=632 && ty<674){
                            double volume=(tx-70)*130.0/178.0;
                            if(volume<0)volume=0;if(volume>130)volume=130;
                            video_options[1]=volume;
                            playback_set_value(player,"volume",volume);
                            continue;
                        }
                        if(tx>=307 && tx<401)key=SDL_CONTROLLER_BUTTON_DPAD_LEFT;
                        else if(tx>=429 && tx<523)key=SDL_CONTROLLER_BUTTON_A;
                        else if(tx>=551 && tx<645)key=SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
                        else if(tx>=785 && tx<879)key=SDL_CONTROLLER_BUTTON_BACK;
                        else if(tx>=907 && tx<1001)key=SDL_CONTROLLER_BUTTON_Y;
                        else if(tx>=1029 && tx<1123)key=SDL_CONTROLLER_BUTTON_X;
                    }else continue;
                }else if(page==CATALOG){
                    int tab=ty<78?main_tab_at(tx):0;
                    if(tab){
                        catalog_hero_focus=tab;
                        if(tab==IPTV_TAB_FOCUS || category_for_tab(tab-7)!=category ||
                           (tab==7 && home_shelf))key=SDL_CONTROLLER_BUTTON_A;
                        else continue;
                    }
                    else if(ty<78 && tx>=887 && tx<1044)key=SDL_CONTROLLER_BUTTON_X;
                    else if(ty<78 && tx>=1058 && tx<1111)key=SDL_CONTROLLER_BUTTON_BACK;
                    else if((category==11 || category==12) && !expanded_catalog){
                        if(ty>=101 && ty<148){
                            int tab=-1;
                            if(tx>=32 && tx<228)tab=0;
                            else if(tx>=244 && tx<402)tab=1;
                            else if(tx>=417 && tx<625)tab=2;
                            if(tab>=0){
                                clear_history_confirm=0;
                                catalog_hero_focus=16+tab;
                                if(tab==2 || category!=11+tab)key=SDL_CONTROLLER_BUTTON_A;
                                else continue;
                            }
                            if(category==12 && tx>=982 && tx<1248){
                                catalog_hero_focus=19;
                                key=SDL_CONTROLLER_BUTTON_A;
                            }
                        }
                        int index=touched_grid_poster(tx,ty,LIBRARY_GRID_Y,245,POSTER_ROW_BOTTOM);
                        if(index>=0 && index<n){
                            chosen=index;
                            catalog_hero_focus=0;
                            clear_history_confirm=0;
                            key=SDL_CONTROLLER_BUTTON_A;
                        }
                    }
                    else if(expanded_catalog){
                        if(ty>=98 && ty<142 && tx>=785 && tx<1087){
                            const int sort_x[]={785,928},sort_w[]={132,159};
                            for(int i=0;i<2;i++)
                                if(tx>=sort_x[i] && tx<sort_x[i]+sort_w[i]){
                                    catalog_hero_focus=12+i;key=SDL_CONTROLLER_BUTTON_A;break;
                                }
                        }
                        if(ty>=95 && ty<145 && tx>=1106){
                            catalog_hero_focus=CATALOG_BACK_FOCUS;key=SDL_CONTROLLER_BUTTON_A;
                        }
                        int index=touched_grid_poster(tx,ty,POSTER_GRID_Y,246,POSTER_GRID_BOTTOM);
                        if(index>=0 && index<n){
                            chosen=index;catalog_hero_focus=0;key=SDL_CONTROLLER_BUTTON_A;
                        }
                    }else{
                        if(ty>=326 && ty<375 && tx>=1095){
                            catalog_hero_focus=3;key=SDL_CONTROLLER_BUTTON_A;
                        }
                        else if(subsection_count(category) && ty>=326 && ty<375){
                            const int anim_x[]={32,240,465},anim_w[]={191,208,134};
                            const int film_x[]={32,197,373,513,672},film_w[]={157,168,132,151,225};
                            int film=tab_for_category(category)==2;
                            for(int i=0;i<subsection_count(category);i++){
                                int x=film?film_x[i]:anim_x[i],w=film?film_w[i]:anim_w[i];
                                if(tx<x || tx>=x+w)continue;
                                catalog_hero_focus=16+i;
                                key=SDL_CONTROLLER_BUTTON_A;
                                break;
                            }
                        }
                        if(ty>=252 && ty<303 && tx>=261 &&
                           tx<(category==11?323:479) && n){
                            catalog_hero_focus=2;
                            key=SDL_CONTROLLER_BUTTON_A;
                        }
                        if(ty>=252 && ty<303 && tx>=44 && tx<249 && n){
                            catalog_hero_focus=1;
                            key=SDL_CONTROLLER_BUTTON_A;
                        }
                        else if(ty>=POSTER_ROW_Y && ty<POSTER_ROW_BOTTOM &&
                                tx>=UI_EDGE && tx<1280-UI_EDGE){
                            int y=category==0?
                                ty-home_shelf*SHELF_STEP+(int)lroundf(shelf_motion.position):ty;
                            int x=tx-53+(int)lroundf(content_motion.position);
                            int index=x/POSTER_STEP;
                            if(y>=POSTER_ROW_Y && y<POSTER_ROW_BOTTOM && x>=0 &&
                               index<n && x%POSTER_STEP<164){
                                chosen=index;catalog_hero_focus=0;key=SDL_CONTROLLER_BUTTON_A;
                            }
                        }
                    }
                }else if(page==PROVIDERS || page==PROVIDER_SETTINGS){
                    int slot=(ty-UI_ROW_Y)/UI_ROW_STEP;
                    if(tx>=UI_EDGE && tx<1280-UI_EDGE && ty>=UI_ROW_Y &&
                       (ty-UI_ROW_Y)%UI_ROW_STEP<UI_ROW_HEIGHT &&
                       slot<PROVIDER_COUNT &&
                       (page==PROVIDER_SETTINGS || provider_for_row(slot)>=0)){
                        if(page==PROVIDER_SETTINGS)provider_settings_selected=slot;
                        else selected_provider=slot;
                        key=SDL_CONTROLLER_BUTTON_A;
                    }else if(ty<92 && tx<100)key=SDL_CONTROLLER_BUTTON_B;
                }else if(page==IPTV_CHANNELS){
                    int tab=ty<78?main_tab_at(tx):0;
                    if(tab){
                        catalog_hero_focus=tab;
                        tv.focus=IPTV_FOCUS_HEADER;
                        if(tab!=IPTV_TAB_FOCUS){page=CATALOG;key=SDL_CONTROLLER_BUTTON_A;}
                    }
                    else if(ty<78 && tx>=887 && tx<1044){
                        page=CATALOG;key=SDL_CONTROLLER_BUTTON_X;
                    }else if(ty<78 && tx>=1058 && tx<1111){
                        settings_selected=0;page=SETTINGS;continue;
                    }else if(ty>=IPTV_FILTER_Y && ty<IPTV_FILTER_Y+IPTV_FILTER_H){
                        for(int i=0;i<3;i++)
                            if(tx>=iptv_filters[i].x && tx<iptv_filters[i].x+iptv_filters[i].w){
                                tv.focus=IPTV_FOCUS_COUNTRY+i;
                                key=SDL_CONTROLLER_BUTTON_A;
                                break;
                            }
                    }else{
                        int row=iptv_touched_card(tx,ty);
                        if(row>=0){
                            tv.selected=row;
                            tv.focus=IPTV_FOCUS_GRID;
                            int local_x=(tx-IPTV_GRID_X)%IPTV_STEP_X;
                            int local_y=(ty-IPTV_GRID_Y+(int)lroundf(content_motion.position))%IPTV_STEP_Y;
                            key=local_x>=IPTV_CARD_SIZE-42 && local_y<42?
                                SDL_CONTROLLER_BUTTON_Y:SDL_CONTROLLER_BUTTON_A;
                        }
                    }
                }else if(page==IPTV_COUNTRIES || page==IPTV_MORE_COUNTRIES || page==IPTV_GENRES){
                    if(ty<77 && tx<100)key=SDL_CONTROLLER_BUTTON_B;
                    else{
                        int row=touched_list_row(tx,ty);
                        if(row>=0 && row<iptv_filter_count(page)){
                            tv.filter_row=row;
                            key=SDL_CONTROLLER_BUTTON_A;
                        }
                    }
                }else if(page==WALLPAPERS){
                    if(ty<92 && tx<100)key=SDL_CONTROLLER_BUTTON_B;
                    else if(ty>=101 && ty<147 && tx>=32 && tx<651){
                        int filter=(tx-32)/158;
                        if(filter>=0 && filter<4)wall_load_filter(filter);
                        continue;
                    }else if(ty>=101 && ty<147 && tx>=674 && tx<1034){
                        key=SDL_CONTROLLER_BUTTON_X;
                    }else if(ty>=101 && ty<147 && tx>=1050)key=SDL_CONTROLLER_BUTTON_Y;
                    else{
                        int index=wall_hit(tx,ty);
                        if(index>=0){wall_selected=index;wall_focus=0;key=SDL_CONTROLLER_BUTTON_A;}
                    }
                }else if(page==DOWNLOADS){
                    int index=touched_list_row(tx,ty);
                    if(index>=0){
                        int count=downloads_list(download_rows,DOWNLOADS_MAX);
                        if(index<count){download_selected=index;key=SDL_CONTROLLER_BUTTON_A;}
                    }else if(ty<92 && tx<100)key=SDL_CONTROLLER_BUTTON_B;
                }else if(page==SOURCES){
                    int new_source=touched_list_row(tx,ty);
                    if(new_source>=0 && new_source<sn){
                        source=new_source;key=SDL_CONTROLLER_BUTTON_A;
                    }else if(ty<92 && tx<100)key=SDL_CONTROLLER_BUTTON_B;
                }else if(page==SEASONS){
                    int next=touched_list_row(tx,ty);
                    if(next>=0 && next<season_count){
                        selected_season=next;key=SDL_CONTROLLER_BUTTON_A;
                    }else if(ty<92 && tx<100)key=SDL_CONTROLLER_BUTTON_B;
                }else if(page==EPISODES){
                    int episode=touched_list_row(tx,ty);
                    if(episode>=0 && selected_season<season_count &&
                       episode<episodes_in_season(season_ids[selected_season])){
                        selected_episode=episode;key=SDL_CONTROLLER_BUTTON_A;
                    }else if(ty<92 && tx<100)key=SDL_CONTROLLER_BUTTON_B;
                }else if(page==DETAILS){
                    if(ty<92 && tx<100)key=SDL_CONTROLLER_BUTTON_B;
                    else if(ty>=526 && ty<585 && tx>=320 && tx<575){
                        details_button_focus=1;
                        key=SDL_CONTROLLER_BUTTON_X;
                    }else if(ty>=526 && ty<585 && tx>=65 && tx<303){
                        details_button_focus=0;
                        key=SDL_CONTROLLER_BUTTON_A;
                    }
                    else if(ty>=UI_FOOTER_TOP)key=SDL_CONTROLLER_BUTTON_B;
                }else if(page==TRACK_PICKER){
                    int new_track=touched_list_row(tx,ty);
                    if(new_track>=0 && new_track<track_count){
                        selected_track=new_track;key=SDL_CONTROLLER_BUTTON_A;
                    }else if(ty<92 && tx<100)key=SDL_CONTROLLER_BUTTON_B;
                }else if(page==SUBDL_PICKER){
                    int row=touched_list_row(tx,ty);
                    if(!subdl_job.thread && row>=0 && row<subdl_job.count){
                        subdl_job.selected=row;key=SDL_CONTROLLER_BUTTON_A;
                    }else if(ty<92 && tx<100)key=SDL_CONTROLLER_BUTTON_B;
                }else if(page==VIDEO_SETTINGS){
                    int slot=(ty-132)/68;
                    if(ty>=132 && (ty-132)%68<60 && slot>=0 && slot<VIDEO_OPTION_COUNT){
                        selected_video_option=slot;
                        key=tx<640?SDL_CONTROLLER_BUTTON_DPAD_LEFT:SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
                    }else if(ty<92 && tx<100)key=SDL_CONTROLLER_BUTTON_B;
                }else if(page==SETTINGS || page==APPEARANCE){
                    int row=ty-SETTINGS_Y;
                    if(tx>=32 && tx<449 && row>=0 && row<(page==APPEARANCE?APPEARANCE_COUNT:SETTING_COUNT)*SETTINGS_ROW_STEP &&
                       row%SETTINGS_ROW_STEP<SETTINGS_ROW_HEIGHT){
                        if(page==APPEARANCE)appearance_selected=row/SETTINGS_ROW_STEP;
                        else settings_selected=row/SETTINGS_ROW_STEP;
                        key=SDL_CONTROLLER_BUTTON_A;
                    }else if(ty>=537 && ty<592 && tx>=515 && tx<773)key=SDL_CONTROLLER_BUTTON_A;
                    else if(ty<92 && tx<100)key=SDL_CONTROLLER_BUTTON_B;
                }
            }
            if(page==PLAYER && key>=0)controls_until=SDL_GetTicks()+3000;
            if(key<0)continue;
            if(!replay){
                int tapped=tx>=0 && ty>=0;
                SDL_Rect rect=ui_focus_rect;
                int focused=key==SDL_CONTROLLER_BUTTON_A && rect.w>0;
                int player_button=page==PLAYER && video_loaded && pending_next_episode<0 &&
                                  pressed_player_button(key,&rect);
                if(tapped || focused || player_button){
                    ui_press_at=SDL_GetTicks();
                    ui_press_active=1;
                    ui_press_page=page;
                    ui_press_is_touch=tapped && page!=PLAYER;
                    ui_press_point=(SDL_Point){tx,ty};
                    ui_press_rect=rect;
                    /* Let a control paint before actions that open another screen
                       or start synchronous provider work. */
                    if((page!=PLAYER && ((key==SDL_CONTROLLER_BUTTON_A && (tapped || focused)) ||
                       (tapped && (key==SDL_CONTROLLER_BUTTON_B ||
                                   key==SDL_CONTROLLER_BUTTON_X ||
                                   key==SDL_CONTROLLER_BUTTON_BACK)))) ||
                       (player_button && (key==SDL_CONTROLLER_BUTTON_Y ||
                                          key==SDL_CONTROLLER_BUTTON_X ||
                                          key==SDL_CONTROLLER_BUTTON_BACK))){
                        pending_key=key;
                        pending_key_page=page;
                        break;
                    }
                }
            }
            if(clear_history_confirm && !(page==CATALOG && category==12 &&
               catalog_hero_focus==19 && key==SDL_CONTROLLER_BUTTON_A)){
                clear_history_confirm=0;
                message[0]=0;
            }
            if(page==PLAYER && pending_next_episode>=0){
                if(key==SDL_CONTROLLER_BUTTON_A){
                    next_episode_request=pending_next_episode;
                    pending_next_episode=-1;
                }else if(key==SDL_CONTROLLER_BUTTON_B){
                    pending_next_episode=-1;
                    page=EPISODES;
                }
                /* Ignore normal seek/pause actions during the countdown. */
                continue;
            }
            if(page==PLAYER){
                if(key==SDL_CONTROLLER_BUTTON_B){
                    double position=0;
                    if (player_return_page!=IPTV_CHANNELS && player && video_loaded && !movy.seek_started &&
                        mpv_get_property(player,"time-pos",MPV_FORMAT_DOUBLE,&position)>=0 &&
                        (position+=movy.offset)>=15.0)
                        { /* Position belongs to a particular episode. */
                            if(player_return_page==DOWNLOADS && download_episode)
                                library_set_episode_progress(&playing_movie,
                                    download_season,download_episode,position);
                            else if(active_episode_variant>=0 && player_return_page==EPISODES)
                                library_set_episode_progress(&playing_movie,
                                    episodes[active_episode_variant].season,
                                    episodes[active_episode_variant].number,position);
                            else library_set_progress(&playing_movie,position);
                        }
                    pending_next_episode=-1;
                    resume_at=0;
                    movy.seek_started=0;
                    command(player,"stop",NULL);page=player_return_page;
                }
                if(key==SDL_CONTROLLER_BUTTON_LEFTSHOULDER &&
                   active_episode_variant>=0 && player_return_page==EPISODES)
                    next_episode_request=catalog_next_episode(
                        episodes,episode_count,active_episode_variant);
                if(key==SDL_CONTROLLER_BUTTON_A)command(player,"cycle","pause");
                int fast=pad && SDL_GameControllerGetAttached(pad) &&
                    SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
                if(key==SDL_CONTROLLER_BUTTON_DPAD_LEFT || key==SDL_CONTROLLER_BUTTON_DPAD_RIGHT){
                    int direction=key==SDL_CONTROLLER_BUTTON_DPAD_RIGHT?1:-1;
                    if(player_return_page==IPTV_CHANNELS){
                        if(video_loaded && tv.count>1){
                            tv.selected=(tv.selected+tv.count+direction)%tv.count;
                            tv.failures=0;
                            play_iptv_channel(player,playing_url,sizeof(playing_url),&movy,
                                              &video_started_at,&video_loaded);
                        }
                    }else if(movy_stream_url(playing_url)){
                        double position=0,duration=0;
                        if(video_loaded && !movy.seek_started && mpv_get_property(player,"time-pos",MPV_FORMAT_DOUBLE,&position)>=0 &&
                           mpv_get_property(player,"duration",MPV_FORMAT_DOUBLE,&duration)>=0){
                            position+=movy.offset;
                            duration+=movy.offset;
                            double target=position+direction*(fast?60:10);
                            movy_seek_to(player,&movy,fmax(0,fmin(target,duration-1)),&video_loaded);
                        }
                    }else seek(player,direction>0?(fast?"60":"10"):(fast?"-60":"-10"));
                }
                if(key==SDL_CONTROLLER_BUTTON_DPAD_UP)change_volume(player,1);
                if(key==SDL_CONTROLLER_BUTTON_DPAD_DOWN)change_volume(player,-1);
                if(key==SDL_CONTROLLER_BUTTON_X || key==SDL_CONTROLLER_BUTTON_Y){
                    subtitle_picker=key==SDL_CONTROLLER_BUTTON_X;
                    int subdl_available=subtitle_picker && video_loaded && playing_movie.id>0 &&
                        (!playing_movie.series || active_episode_variant>=0 ||
                         (player_return_page==DOWNLOADS && download_episode>0));
                    int track_limit=TRACKS_MAX-subdl_available;
                    track_count=playback_tracks(player,subtitle_picker,tracks,track_limit);
                    /* Even if an external subtitle failed to load into mpv,
                       keep the provider's caption URL available for retry. */
                    if(subtitle_picker && player_return_page!=IPTV_CHANNELS)
                        for(int i=0;i<uakino_resolved_subtitle_count() &&
                            track_count<track_limit;i++){
                            if(playback_has_external_subtitle(player,
                                   uakino_resolved_subtitle_url(i)))continue;
                            MediaTrack *choice=&tracks[track_count++];
                            choice->id=-(SOURCE_SUBTITLE_TRACK_BASE+i+1);
                            choice->selected=0;
                            snprintf(choice->label,sizeof(choice->label),
                                     TR("Зовнішні: %.165s","External: %.165s"),
                                     uakino_resolved_subtitle_label(i));
                        }
                    /* PlayerJS can offer separately encoded dubbed versions
                       as distinct stream URLs, not as native mpv tracks. */
                    if(!subtitle_picker && player_return_page!=IPTV_CHANNELS && uakino_resolved_audio_count()>1)
                        for(int i=0;i<uakino_resolved_audio_count() &&
                            track_count<TRACKS_MAX;i++){
                            MediaTrack *choice=&tracks[track_count++];
                            choice->id=-(SOURCE_AUDIO_TRACK_BASE+i+1);
                            choice->selected=!strcmp(playing_url,
                                                      uakino_resolved_audio_url(i));
                            snprintf(choice->label,sizeof(choice->label),
                                     TR("Озвучка: %.165s","Dub: %.165s"),
                                     uakino_resolved_audio_label(i));
                        }
                    /* In Playerjs playlists, a different dub is often a
                       different HLS URL, not an mpv-embedded audio track. */
                    if(!subtitle_picker && active_episode_variant>=0 &&
                       active_episode_variant<episode_count){
                        Episode *active=&episodes[active_episode_variant];
                        int variants=0;
                        for(int i=0;i<episode_count;i++)
                            if(episodes[i].season==active->season &&
                               episodes[i].number==active->number)variants++;
                        if(variants>1)for(int i=0;i<episode_count && track_count<TRACKS_MAX;i++){
                            if(episodes[i].season!=active->season ||
                               episodes[i].number!=active->number)continue;
                            MediaTrack *choice=&tracks[track_count++];
                            choice->id=-(i+1);
                            choice->selected=i==active_episode_variant;
                            snprintf(choice->label,sizeof(choice->label),TR("Озвучка: %.165s","Dub: %.165s"),
                                     episodes[i].voice[0]?episodes[i].voice:TR("Основна","Main"));
                        }
                    }
                    if(subdl_available){
                        MediaTrack *choice=&tracks[track_count++];
                        choice->id=SUBDL_SEARCH_TRACK;
                        choice->selected=0;
                        snprintf(choice->label,sizeof(choice->label),"%s",
                                 TR("Шукати субтитри в SubDL","Search subtitles on SubDL"));
                    }
                    selected_track=0;
                    for(int i=0;i<track_count;i++) if(tracks[i].selected)selected_track=i;
                    page=TRACK_PICKER;
                }
                if(key==SDL_CONTROLLER_BUTTON_BACK){
                    selected_video_option=0;
                    page=VIDEO_SETTINGS;
                }
                continue;
            }
            if(page==TRACK_PICKER){
                if(key==SDL_CONTROLLER_BUTTON_B)page=PLAYER;
                if(key==SDL_CONTROLLER_BUTTON_DPAD_UP && selected_track>0)selected_track--;
                if(key==SDL_CONTROLLER_BUTTON_DPAD_DOWN && selected_track<track_count-1)selected_track++;
                if(key==SDL_CONTROLLER_BUTTON_A && track_count){
                    int id=tracks[selected_track].id;
                    if(subtitle_picker && id==SUBDL_SEARCH_TRACK){
                        if(!subdl_job.thread){
                            message[0]=0;
                            subdl_job.movie=playing_movie;
                            subdl_job.season=0;
                            subdl_job.episode=0;
                            if(player_return_page==DOWNLOADS){
                                subdl_job.season=download_season;
                                subdl_job.episode=download_episode;
                            }else if(player_return_page==EPISODES && active_episode_variant>=0){
                                const Episode *active=&episodes[active_episode_variant];
                                subdl_job.season=active->season;
                                subdl_job.episode=active->number;
                            }
                            subdl_job.count=subdl_job.selected=0;
                            subdl_start(0);
                            page=SUBDL_PICKER;
                        }else snprintf(message,sizeof(message),"%s",
                                       TR("Попередній запит SubDL ще триває.",
                                          "The previous SubDL request is still running."));
                    }else if(subtitle_picker && id<=-(SOURCE_SUBTITLE_TRACK_BASE+1) &&
                       id>=-(SOURCE_SUBTITLE_TRACK_BASE+UAKINO_PLAYER_TRACK_MAX)){
                        int index=-id-SOURCE_SUBTITLE_TRACK_BASE-1;
                        const char *args[]={"sub-add",
                            uakino_resolved_subtitle_url(index),"select",
                            uakino_resolved_subtitle_label(index),"",NULL};
                        mpv_command_async(player,0,args);
                        mpv_set_property_string(player,"sub-visibility","yes");
                        page=PLAYER;
                    }else if(!subtitle_picker && id<=-(SOURCE_AUDIO_TRACK_BASE+1) &&
                       id>=-(SOURCE_AUDIO_TRACK_BASE+UAKINO_PLAYER_TRACK_MAX)){
                        int variant=-id-SOURCE_AUDIO_TRACK_BASE-1;
                        const char *url=uakino_resolved_audio_url(variant);
                        if(url[0] && strcmp(playing_url,url)){
                            double position=0;
                            if(player)mpv_get_property(player,"time-pos",
                                                       MPV_FORMAT_DOUBLE,&position);
                            resume_at=position>0?position:0;
                            snprintf(playing_url,sizeof(playing_url),"%s",url);
                            snprintf(pending_subtitle,sizeof(pending_subtitle),"%s",
                                     uakino_resolved_subtitle());
                            apply_hls_quality(player);
                            play_stream(player,url,&movy);
                            video_started_at=SDL_GetTicks();video_loaded=0;
                            controls_until=SDL_GetTicks()+3000;
                        }
                        page=PLAYER;
                    }else if(!subtitle_picker && id<0 && -id<=episode_count){
                        int variant=-id-1;
                        if(variant==active_episode_variant){page=PLAYER;continue;}
                        Source alternate={0};
                        snprintf(alternate.url,sizeof(alternate.url),"%s",episodes[variant].url);
                        char url[2048]={0};message[0]=0;
                        if(uakino_resolve(&alternate,url,sizeof(url),message,sizeof(message))){
                            double position=0;
                            if(player)mpv_get_property(player,"time-pos",MPV_FORMAT_DOUBLE,&position);
                            resume_at=position>0?position:0;
                            snprintf(playing_url,sizeof(playing_url),"%s",url);
                            snprintf(pending_subtitle,sizeof(pending_subtitle),"%s",uakino_resolved_subtitle());
                            active_episode_variant=variant;
                            apply_hls_quality(player);
                            mpv_set_property_string(player,"pause","no");
                            play_stream(player,url,&movy);
                            video_started_at=SDL_GetTicks();video_loaded=0;
                            controls_until=SDL_GetTicks()+3000;
                            page=PLAYER;
                        }
                    }else if(playback_select_track(player,subtitle_picker,id)){
                        snprintf(message,sizeof(message),TR("%s: вибрано %s","%s: selected %s"),
                                 subtitle_picker?TR("Субтитри","Subtitles"):TR("Аудіо","Audio"),
                                 tracks[selected_track].label);
                        page=PLAYER;
                    }else snprintf(message,sizeof(message),TR("Не вдалося перемкнути доріжку","Could not switch tracks"));
                }
                continue;
            }
            if(page==SUBDL_PICKER){
                if(key==SDL_CONTROLLER_BUTTON_B)page=TRACK_PICKER;
                else if(!subdl_job.thread){
                    if(key==SDL_CONTROLLER_BUTTON_DPAD_UP && subdl_job.selected>0)
                        subdl_job.selected--;
                    if(key==SDL_CONTROLLER_BUTTON_DPAD_DOWN && subdl_job.selected<subdl_job.count-1)
                        subdl_job.selected++;
                    if(key==SDL_CONTROLLER_BUTTON_A && subdl_job.count)
                        subdl_start(1);
                }
                continue;
            }
            if(page==VIDEO_SETTINGS){
                if(key==SDL_CONTROLLER_BUTTON_B || key==SDL_CONTROLLER_BUTTON_BACK){
                    page=video_settings_from_menu?SETTINGS:PLAYER;
                    video_settings_from_menu=0;
                }
                if(key==SDL_CONTROLLER_BUTTON_DPAD_UP && selected_video_option>0)selected_video_option--;
                if(key==SDL_CONTROLLER_BUTTON_DPAD_DOWN && selected_video_option<VIDEO_OPTION_COUNT-1)selected_video_option++;
                int direction=(key==SDL_CONTROLLER_BUTTON_DPAD_RIGHT)-(key==SDL_CONTROLLER_BUTTON_DPAD_LEFT);
                if(direction){
                    int i=visible_video_options[selected_video_option];
                    video_options[i]+=direction*video_steps[i];
                    if(video_options[i]<video_min[i])video_options[i]=video_min[i];
                    if(video_options[i]>video_max[i])video_options[i]=video_max[i];
                    if(i==6){
                        apply_hls_quality(player);
                        if(playing_url[0] && !video_settings_from_menu && player_return_page==IPTV_CHANNELS){
                            tv.failures=0;
                            play_iptv_channel(player,playing_url,sizeof(playing_url),&movy,
                                              &video_started_at,&video_loaded);
                        }else if(playing_url[0] && !video_settings_from_menu){
                            double pos=0;
                            if(mpv_get_property(player,"time-pos",MPV_FORMAT_DOUBLE,&pos)>=0)
                                resume_at=pos+movy.offset;
                            snprintf(pending_subtitle,sizeof(pending_subtitle),"%s",
                                     uakino_resolved_subtitle());
                            video_started_at=SDL_GetTicks();video_loaded=0;
                            play_stream(player,playing_url,&movy);
                        }
                    }else{
                        double value=video_options[i];
                        if(i==5 && movy.external_subtitle[0])value-=movy.offset;
                        playback_set_value(player,video_props[i],value);
                    }
                }
                continue;
            }
            if(page==SEASONS){
                if(key==SDL_CONTROLLER_BUTTON_B)page=SOURCES;
                if(key==SDL_CONTROLLER_BUTTON_DPAD_UP && selected_season>0)selected_season--;
                if(key==SDL_CONTROLLER_BUTTON_DPAD_DOWN && selected_season<season_count-1)selected_season++;
                if(key==SDL_CONTROLLER_BUTTON_A && season_count){
                    selected_episode=0;page=EPISODES;
                }
                continue;
            }
            if(page==EPISODES){
                if(key==SDL_CONTROLLER_BUTTON_B)page=SEASONS;
                if(key==SDL_CONTROLLER_BUTTON_Y){
                    Episode *ep=selected_episode_item();
                    if(ep){
                        Source episode_source={0};
                        snprintf(episode_source.url,sizeof(episode_source.url),"%s",ep->url);
                        char url[2048]={0};
                        if(uakino_resolve(&episode_source,url,sizeof(url),message,sizeof(message))){
                            char label[96];
                            snprintf(label,sizeof(label),TR("Сезон %d · Серія %d","Season %d · Episode %d"),ep->season,ep->number);
                            int added=downloads_add(&movies[chosen],ep->season,ep->number,label,url);
                            snprintf(message,sizeof(message),"%s",added>0?TR("Додано до завантажень","Added to downloads"):
                                     added==0?TR("Уже в завантаженнях","Already in downloads"):TR("Черга завантажень заповнена","Download queue is full"));
                        }
                    }
                    continue;
                }
                if(key==SDL_CONTROLLER_BUTTON_DPAD_UP && selected_episode>0)selected_episode--;
                if(key==SDL_CONTROLLER_BUTTON_DPAD_DOWN &&
                   selected_episode<episodes_in_season(season_ids[selected_season])-1)selected_episode++;
                if(key==SDL_CONTROLLER_BUTTON_A){
                    Episode *episode=selected_episode_item();
                    if(episode && render){
                        Source episode_source={0};
                        snprintf(episode_source.url,sizeof(episode_source.url),"%s",episode->url);
                        char url[2048]={0};message[0]=0;
                        if(uakino_resolve(&episode_source,url,sizeof(url),message,sizeof(message))){
                            active_episode_variant=(int)(episode-episodes);
                            snprintf(pending_subtitle,sizeof(pending_subtitle),"%s",uakino_resolved_subtitle());
                            playing_movie=movies[chosen];
                            int saved_season=0,saved_episode=0;
                            double saved_seconds=0;
                            resume_at=(library_last_episode(&playing_movie,
                                       &saved_season,&saved_episode,&saved_seconds) &&
                                       saved_season==episode->season &&
                                       saved_episode==episode->number)?saved_seconds:0;
                            snprintf(playing_url,sizeof(playing_url),"%s",url);
                            apply_hls_quality(player);
                            play_stream(player,url,&movy);
                            video_started_at=SDL_GetTicks();video_loaded=0;
                            controls_until=SDL_GetTicks()+3000;
                            player_return_page=EPISODES;page=PLAYER;
                        }
                    } else if(!render) snprintf(message,sizeof(message),TR("Плеєр mpv не ініціалізовано","The mpv player could not start"));
                }
                continue;
            }
            if(page==DOWNLOADS){
                int count=downloads_list(download_rows,DOWNLOADS_MAX);
                if(download_selected>=count)download_selected=count?count-1:0;
                if(key==SDL_CONTROLLER_BUTTON_B){
                    page=CATALOG;category=11;expanded_catalog=0;
                    query[0]=message[0]=0;
                    n=load_category(category,query,movies,message,sizeof(message));
                    chosen=0;catalog_hero_focus=18;download_confirm=0;continue;
                }
                if(key==SDL_CONTROLLER_BUTTON_DPAD_UP && download_selected>0)
                    {download_selected--;download_confirm=0;}
                if(key==SDL_CONTROLLER_BUTTON_DPAD_DOWN && download_selected<count-1)
                    {download_selected++;download_confirm=0;}
                if(key==SDL_CONTROLLER_BUTTON_X && count){
                    unsigned id=download_rows[download_selected].id;
                    if(download_confirm!=id){
                        download_confirm=id;
                        snprintf(message,sizeof(message),TR("Натисніть X ще раз для видалення","Press X again to delete"));
                    }else{
                        downloads_remove(id);download_confirm=0;
                        snprintf(message,sizeof(message),TR("Видалено","Deleted"));
                    }
                }
                if(key==SDL_CONTROLLER_BUTTON_A && count){
                    const DownloadInfo *info=&download_rows[download_selected];
                    static Download item;
                    if(info->state==DOWNLOAD_FAILED){
                        downloads_retry(info->id);
                        snprintf(message,sizeof(message),TR("Завантаження повторюється","Retrying download"));
                    }else if(info->state!=DOWNLOAD_DONE)
                        snprintf(message,sizeof(message),TR("Файл ще завантажується","The file is still downloading"));
                    else if(!render)snprintf(message,sizeof(message),TR("Плеєр mpv не ініціалізовано","The mpv player could not start"));
                    else if(downloads_get(info->id,&item)){
                        char file_path[256];
                        downloads_play_path(&item,file_path,sizeof(file_path));
                        playing_movie=item.movie;
                        active_episode_variant=-1;
                        download_season=item.season;download_episode=item.episode;
                        int last_season=0,last_episode=0;double pos=0;
                        resume_at=!item.episode?library_progress(&playing_movie):
                            (library_last_episode(&playing_movie,&last_season,&last_episode,&pos)
                             && last_season==item.season && last_episode==item.episode)?pos:0;
                        pending_subtitle[0]=0;message[0]=0;
                        snprintf(playing_url,sizeof(playing_url),"%s",file_path);
                        play_stream(player,file_path,&movy);
                        video_started_at=SDL_GetTicks();video_loaded=0;
                        controls_until=SDL_GetTicks()+3000;
                        player_return_page=DOWNLOADS;page=PLAYER;
                    }
                }
                continue;
            }
            if(page==WALLPAPERS){
                if(key==SDL_CONTROLLER_BUTTON_B || key==SDL_CONTROLLER_BUTTON_BACK){
                    page=APPEARANCE;continue;
                }
                if(key==SDL_CONTROLLER_BUTTON_X){
                    char search[sizeof(wall_query)];
                    snprintf(search,sizeof(search),"%s",wall_query);
                    if(keyboard(search,sizeof(search))){
                        if(wallpapers_browse(wall_filter,search,1)){
                            snprintf(wall_query,sizeof(wall_query),"%s",search);
                            wall_selected=0;wall_focus=0;
                        }
                    }else if(wall_query[0] && wallpapers_browse(wall_filter,"",1)){
                        wall_query[0]=0;wall_selected=0;wall_focus=0;
                    }
                }
                if(key==SDL_CONTROLLER_BUTTON_Y)wallpapers_reset();
                if(key==SDL_CONTROLLER_BUTTON_LEFTSHOULDER){
                    wall_focus=0;wall_move_page(-1);
                }
                if(key==SDL_CONTROLLER_BUTTON_RIGHTSHOULDER){
                    wall_focus=0;wall_move_page(1);
                }
                /* Match film-grid navigation with D-pad or analog stick. */
                if(key==SDL_CONTROLLER_BUTTON_DPAD_UP){
                    if(!wall_focus){
                        int first=wall_selected/WALL_PAGE*WALL_PAGE;
                        if(wall_selected>=first+WALL_COLS)
                            wall_selected-=WALL_COLS;
                        else if(first || wallpapers_current_page()>1)
                            wall_move_page(-1);
                        else wall_focus=1;
                    }
                }
                if(key==SDL_CONTROLLER_BUTTON_DPAD_DOWN){
                    if(wall_focus)wall_focus=0;
                    else{
                        int first=wall_selected/WALL_PAGE*WALL_PAGE;
                        int next=wall_selected+WALL_COLS;
                        if(next<wall_count && next<first+WALL_PAGE)
                            wall_selected=next;
                        else wall_move_page(1);
                    }
                }
                if(key==SDL_CONTROLLER_BUTTON_DPAD_LEFT){
                    if(wall_focus)wall_load_filter((wall_filter+3)%4);
                    else if(wall_selected>0){
                        int from=wall_selected/WALL_PAGE*WALL_PAGE;
                        --wall_selected;
                        int to=wall_selected/WALL_PAGE*WALL_PAGE;
                        if(from!=to)wall_start_animation(from,to,-1);
                    }else wall_move_page(-1);
                }
                if(key==SDL_CONTROLLER_BUTTON_DPAD_RIGHT){
                    if(wall_focus)wall_load_filter((wall_filter+1)%4);
                    else if(wall_selected+1<wall_count){
                        int from=wall_selected/WALL_PAGE*WALL_PAGE;
                        ++wall_selected;
                        int to=wall_selected/WALL_PAGE*WALL_PAGE;
                        if(from!=to)wall_start_animation(from,to,1);
                    }else wall_move_page(1);
                }
                if(key==SDL_CONTROLLER_BUTTON_A && !wall_focus &&
                   wall_count && wall_selected<wall_count)
                    wallpapers_apply(wall_rows[wall_selected]);
                continue;
            }
            if(page==APPEARANCE){
                if(key==SDL_CONTROLLER_BUTTON_B || key==SDL_CONTROLLER_BUTTON_BACK){
                    page=SETTINGS;
                    continue;
                }
                if(key==SDL_CONTROLLER_BUTTON_DPAD_UP && appearance_selected>0)
                    appearance_selected--;
                if(key==SDL_CONTROLLER_BUTTON_DPAD_DOWN &&
                   appearance_selected<APPEARANCE_COUNT-1)appearance_selected++;
                if(key==SDL_CONTROLLER_BUTTON_A){
                    if(appearance_selected==APPEARANCE_THEME){
                        ui_light_theme=!ui_light_theme;
                        theme_save();
                    }else if(appearance_selected==APPEARANCE_ACCENT){
                        ui_accent_alt=!ui_accent_alt;
                    }else if(appearance_selected==APPEARANCE_WALLPAPERS){
                        wall_selected=0;wall_focus=0;
                        wallpapers_open();
                        wall_rebuild();
                        page=WALLPAPERS;
                    }else if(appearance_selected==APPEARANCE_DEFAULT){
                        wallpapers_reset();
                        snprintf(message,sizeof(message),"%s",
                                 TR("Стандартний фон відновлено","Default background restored"));
                    }
                }
                continue;
            }
            if(page==SETTINGS){
                if(key==SDL_CONTROLLER_BUTTON_B || key==SDL_CONTROLLER_BUTTON_BACK){
                    if(catalog_language_changed){
                        n=load_category(category,query,movies,message,sizeof(message));
                        if(chosen>=n)chosen=n?n-1:0;
                        catalog_language_changed=0;
                    }
                    page=CATALOG;
                    continue;
                }
                if(key==SDL_CONTROLLER_BUTTON_DPAD_UP && settings_selected>0)settings_selected--;
                if(key==SDL_CONTROLLER_BUTTON_DPAD_DOWN && settings_selected<SETTING_COUNT-1)
                    settings_selected++;
                if(key==SDL_CONTROLLER_BUTTON_A){
                    if(settings_selected==SETTING_APPEARANCE){
                        appearance_selected=0;
                        page=APPEARANCE;
                    }else if(settings_selected==SETTING_PLAYER){
                        selected_video_option=0;
                        video_settings_from_menu=1;
                        page=VIDEO_SETTINGS;
                    }else if(settings_selected==SETTING_AUTOPLAY){
                        autoplay_episodes=!autoplay_episodes;theme_save();
                    }else if(settings_selected==SETTING_PROVIDERS){
                        provider_settings_selected=0;
                        message[0]=0;
                        page=PROVIDER_SETTINGS;
                    }else if(settings_selected==SETTING_LANGUAGE){
                        ui_set_language(ui_language()==UI_ENGLISH?UI_UKRAINIAN:UI_ENGLISH);
                        catalog_invalidate_language(category,query);
                        catalog_language_changed=1;
                        message[0]=0;
                        if(!theme_save())
                            snprintf(message,sizeof(message),"%s",
                                     TR("Не вдалося зберегти мову на SD-карті",
                                        "Could not save the language to the SD card"));
                    }
                }
                continue;
            }
            if(page==PROVIDER_SETTINGS){
                if(key==SDL_CONTROLLER_BUTTON_B || key==SDL_CONTROLLER_BUTTON_BACK){
                    message[0]=0;
                    page=SETTINGS;
                }
                if(key==SDL_CONTROLLER_BUTTON_DPAD_UP && provider_settings_selected>0)
                    provider_settings_selected--;
                if(key==SDL_CONTROLLER_BUTTON_DPAD_DOWN && provider_settings_selected<PROVIDER_COUNT-1)
                    provider_settings_selected++;
                if(key==SDL_CONTROLLER_BUTTON_A){
                    enabled_providers^=1u<<(providers[provider_settings_selected].id-1);
                    message[0]=0;
                    if(!theme_save())
                        snprintf(message,sizeof(message),TR("Не вдалося зберегти джерела на SD-карті","Could not save providers to the SD card"));
                }
                continue;
            }
            if(key==SDL_CONTROLLER_BUTTON_START){running=0;continue;}
            if(page==IPTV_CHANNELS){
                if(key==SDL_CONTROLLER_BUTTON_B){
                    if(tv.focus!=IPTV_FOCUS_HEADER){
                        tv.focus=IPTV_FOCUS_HEADER;
                        catalog_hero_focus=IPTV_TAB_FOCUS;
                        continue;
                    }
                    message[0]=0;page=CATALOG;
                    catalog_hero_focus=IPTV_TAB_FOCUS;
                    continue;
                }
                if(key==SDL_CONTROLLER_BUTTON_X)tv.focus=IPTV_FOCUS_GENRES;
                if(key==SDL_CONTROLLER_BUTTON_BACK)tv.focus=IPTV_FOCUS_FAVORITES;
                if(key==SDL_CONTROLLER_BUTTON_X || key==SDL_CONTROLLER_BUTTON_BACK)
                    key=SDL_CONTROLLER_BUTTON_A;
                if(key==SDL_CONTROLLER_BUTTON_Y && tv.count && tv.focus==IPTV_FOCUS_GRID){
                    int row=tv.selected;
                    if(iptv_favorite_toggle(&tv.favorites,iptv_channel(row))){
                        message[0]=0;
                        if(tv.show_favorites){
                            iptv_filter();
                            tv.selected=row<tv.count?row:tv.count?tv.count-1:0;
                            if(!tv.count)tv.focus=IPTV_FOCUS_FAVORITES;
                        }
                    }else snprintf(message,sizeof(message),"%s",
                        TR("Не вдалося зберегти вибране IPTV","Could not save IPTV favorites"));
                }else if(key==SDL_CONTROLLER_BUTTON_A && tv.focus>=IPTV_FOCUS_COUNTRY){
                    message[0]=0;
                    if(tv.focus==IPTV_FOCUS_FAVORITES){
                        tv.show_favorites=!tv.show_favorites;
                        iptv_filter();
                        content_motion=(ScrollMotion){0};
                        if(!tv.show_favorites && tv.loaded_country!=tv.country)iptv_request_country();
                        continue;
                    }
                    page=tv.focus==IPTV_FOCUS_COUNTRY?IPTV_COUNTRIES:IPTV_GENRES;
                    tv.filter_row=page==IPTV_GENRES?tv.genre+1:IPTV_QUICK_COUNT;
                    if(page==IPTV_COUNTRIES)
                        for(int i=0;i<IPTV_QUICK_COUNT;i++)
                            if(iptv_country_index(iptv_quick_countries[i])==tv.country){tv.filter_row=i;break;}
                }else if(tv.focus!=IPTV_FOCUS_HEADER &&
                         (key==SDL_CONTROLLER_BUTTON_LEFTSHOULDER || key==SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)){
                    bool right=key==SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;
                    tv.selected=swipe_page_selection(tv.selected,tv.count,IPTV_PAGE_SIZE,right?1:-1);
                    if(tv.count)tv.focus=IPTV_FOCUS_GRID;
                }else if(tv.focus==IPTV_FOCUS_HEADER){
                    if(key==SDL_CONTROLLER_BUTTON_LEFTSHOULDER || key==SDL_CONTROLLER_BUTTON_RIGHTSHOULDER){
                        catalog_hero_focus=key==SDL_CONTROLLER_BUTTON_LEFTSHOULDER?10:11;
                        page=CATALOG;
                        key=SDL_CONTROLLER_BUTTON_A;
                    }else if(key==SDL_CONTROLLER_BUTTON_A && catalog_hero_focus!=IPTV_TAB_FOCUS){
                        page=CATALOG;
                    }else if(key==SDL_CONTROLLER_BUTTON_DPAD_DOWN || key==SDL_CONTROLLER_BUTTON_A){
                        tv.focus=IPTV_FOCUS_COUNTRY;
                        if(!tv.count && !tv.show_favorites && tv.loaded_country!=tv.country)iptv_request_country();
                    }else catalog_hero_focus=header_focus_step(catalog_hero_focus,key);
                    if(page==IPTV_CHANNELS)continue;
                }else if(tv.focus>=IPTV_FOCUS_COUNTRY){
                    if(key==SDL_CONTROLLER_BUTTON_DPAD_LEFT && tv.focus>IPTV_FOCUS_COUNTRY)tv.focus--;
                    if(key==SDL_CONTROLLER_BUTTON_DPAD_RIGHT && tv.focus<IPTV_FOCUS_FAVORITES)tv.focus++;
                    if(key==SDL_CONTROLLER_BUTTON_DPAD_UP){
                        tv.focus=IPTV_FOCUS_HEADER;
                        catalog_hero_focus=IPTV_TAB_FOCUS;
                    }
                    if(key==SDL_CONTROLLER_BUTTON_DPAD_DOWN && tv.count)tv.focus=IPTV_FOCUS_GRID;
                }else if(key==SDL_CONTROLLER_BUTTON_DPAD_UP){
                    if(tv.selected>=IPTV_COLUMNS)tv.selected-=IPTV_COLUMNS;
                    else tv.focus=IPTV_FOCUS_COUNTRY+(tv.selected%IPTV_COLUMNS)/2;
                }else if(key==SDL_CONTROLLER_BUTTON_DPAD_DOWN && tv.selected+IPTV_COLUMNS<tv.count)
                    tv.selected+=IPTV_COLUMNS;
                else if(key==SDL_CONTROLLER_BUTTON_DPAD_LEFT && tv.selected>0)tv.selected--;
                else if(key==SDL_CONTROLLER_BUTTON_DPAD_RIGHT && tv.selected+1<tv.count)tv.selected++;
                else if(key==SDL_CONTROLLER_BUTTON_A && tv.count){
                    if(!render){
                        snprintf(message,sizeof(message),"%s",
                            TR("Плеєр mpv не ініціалізовано","The mpv player could not start"));
                        continue;
                    }
                    message[0]=pending_subtitle[0]=0;
                    playing_movie=(Movie){0};
                    active_episode_variant=-1;
                    resume_at=0;
                    pending_next_episode=next_episode_request=-1;
                    player_return_page=IPTV_CHANNELS;
                    tv.failures=0;
                    play_iptv_channel(player,playing_url,sizeof(playing_url),&movy,
                                      &video_started_at,&video_loaded);
                    controls_until=SDL_GetTicks()+3000;
                    page=PLAYER;
                }
                if(page!=CATALOG)continue;
            }
            if(page==IPTV_COUNTRIES || page==IPTV_MORE_COUNTRIES || page==IPTV_GENRES){
                int count=iptv_filter_count(page);
                if(key==SDL_CONTROLLER_BUTTON_DPAD_UP && tv.filter_row>0)tv.filter_row--;
                if(key==SDL_CONTROLLER_BUTTON_DPAD_DOWN && tv.filter_row+1<count)tv.filter_row++;
                if(key==SDL_CONTROLLER_BUTTON_LEFTSHOULDER)
                    tv.filter_row=swipe_page_selection(tv.filter_row,count,UI_ROWS,-1);
                if(key==SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)
                    tv.filter_row=swipe_page_selection(tv.filter_row,count,UI_ROWS,1);
                if(key==SDL_CONTROLLER_BUTTON_A){
                    if(page==IPTV_COUNTRIES && tv.filter_row==IPTV_QUICK_COUNT){
                        page=IPTV_MORE_COUNTRIES;tv.filter_row=0;continue;
                    }
                    if(page==IPTV_GENRES)tv.genre=tv.filter_row-1;
                    else tv.country=page==IPTV_MORE_COUNTRIES?tv.more_countries[tv.filter_row]:
                                    iptv_country_index(iptv_quick_countries[tv.filter_row]);
                    tv.show_favorites=0;
                    if(tv.country!=tv.loaded_country && !iptv_request_country())
                        snprintf(message,sizeof(message),"%s",
                             TR("Не вдалося завантажити список IPTV","Could not load the IPTV list"));
                    iptv_filter();
                    if(tv.count)tv.focus=IPTV_FOCUS_GRID;
                    page=IPTV_CHANNELS;
                }else if(key==SDL_CONTROLLER_BUTTON_B){
                    if(page==IPTV_MORE_COUNTRIES){
                        page=IPTV_COUNTRIES;tv.filter_row=IPTV_QUICK_COUNT;
                    }else page=IPTV_CHANNELS;
                }
                continue;
            }
            if(page==CATALOG){
                if(key==SDL_CONTROLLER_BUTTON_A && catalog_hero_focus==IPTV_TAB_FOCUS){
                    iptv_open();
                    message[0]=0;page=IPTV_CHANNELS;continue;
                }
                if(key==SDL_CONTROLLER_BUTTON_BACK){page=SETTINGS;continue;}
                if(expanded_catalog && catalog_hero_focus==CATALOG_BACK_FOCUS && key==SDL_CONTROLLER_BUTTON_A)
                    key=SDL_CONTROLLER_BUTTON_B;
                if(key==SDL_CONTROLLER_BUTTON_B && query[0]){
                    category=search_return_category;
                    expanded_catalog=search_return_expanded;
                    home_shelf=search_return_shelf;
                    query[0]=message[0]=0;
                    n=load_category(category,query,movies,message,sizeof(message));
                    chosen=search_return_chosen<n?search_return_chosen:0;
                    catalog_hero_focus=0;
                    continue;
                }
                if(key==SDL_CONTROLLER_BUTTON_A && catalog_hero_focus==5)
                    key=SDL_CONTROLLER_BUTTON_X;
                else if(key==SDL_CONTROLLER_BUTTON_A && catalog_hero_focus==6){
                    settings_selected=0;page=SETTINGS;continue;
                }
                if(key==SDL_CONTROLLER_BUTTON_RIGHTSTICK){
                    if(category==11 || category==12)continue;
                    if(category==0 && !expanded_catalog && !query[0]){
                        catalog_hero_focus=3;key=SDL_CONTROLLER_BUTTON_A;
                    }else{
                    expanded_catalog=!expanded_catalog;chosen=0;catalog_hero_focus=0;continue;
                    }
                }
                if(key==SDL_CONTROLLER_BUTTON_B && expanded_catalog){
                    expanded_catalog=0;
                    chosen=0;
                    catalog_hero_focus=7+tab_for_category(category);
                    continue;
                }
                if(key==SDL_CONTROLLER_BUTTON_B && category!=0){
                    category=0;home_shelf=0;query[0]=message[0]=0;
                    n=load_category(category,query,movies,message,sizeof(message));
                    chosen=0;catalog_hero_focus=0;continue;
                }
                if(category==0 && !expanded_catalog && !query[0] &&
                   catalog_hero_focus==0 &&
                   (key==SDL_CONTROLLER_BUTTON_DPAD_UP ||
                    key==SDL_CONTROLLER_BUTTON_DPAD_DOWN)){
                    if(key==SDL_CONTROLLER_BUTTON_DPAD_UP && n>5 && chosen>=5){
                        /* The rightmost visible poster points to More. The
                           horizontal row shifts once the selection reaches
                           its sixth card, keeping that card on the right. */
                        catalog_hero_focus=3;
                        continue;
                    }
                    int next=home_shelf+(key==SDL_CONTROLLER_BUTTON_DPAD_DOWN?1:-1);
                    if(next>=0 && next<HOME_SHELF_COUNT){
                        home_shelf=next;
                        n=load_category(category,query,movies,message,sizeof(message));
                        chosen=0;catalog_hero_focus=0;
                    }else if(key==SDL_CONTROLLER_BUTTON_DPAD_UP)
                        catalog_hero_focus=n?1:7;
                    continue;
                }
                if(category==0 && !expanded_catalog && !query[0] &&
                   catalog_hero_focus==3 &&
                   key==SDL_CONTROLLER_BUTTON_DPAD_UP && home_shelf>0){
                    /* One more Up returns to the previous vertical shelf. */
                    home_shelf--;
                    n=load_category(category,query,movies,message,sizeof(message));
                    chosen=0;catalog_hero_focus=0;
                    continue;
                }
                if(!expanded_catalog && (category==11 || category==12)){
                    if(catalog_hero_focus>=16){
                        if(catalog_hero_focus==18 && key==SDL_CONTROLLER_BUTTON_A){
                            page=DOWNLOADS;download_selected=0;download_confirm=0;
                            message[0]=0;continue;
                        }
                        if(key==SDL_CONTROLLER_BUTTON_DPAD_LEFT && catalog_hero_focus>16)
                            catalog_hero_focus--;
                        if(key==SDL_CONTROLLER_BUTTON_DPAD_RIGHT){
                            if(catalog_hero_focus<18)catalog_hero_focus++;
                            else if(category==12 && catalog_hero_focus==18)catalog_hero_focus=19;
                        }
                        if(key==SDL_CONTROLLER_BUTTON_DPAD_UP)catalog_hero_focus=11;
                        if(key==SDL_CONTROLLER_BUTTON_DPAD_DOWN && n)
                            catalog_hero_focus=0;
                    }else if((catalog_hero_focus>=5 && catalog_hero_focus<=11) || catalog_hero_focus==IPTV_TAB_FOCUS){
                        catalog_hero_focus=header_focus_step(catalog_hero_focus,key);
                        if(key==SDL_CONTROLLER_BUTTON_DPAD_DOWN)
                            catalog_hero_focus=focused_subsection(category);
                    }else{
                        if(key==SDL_CONTROLLER_BUTTON_DPAD_LEFT && chosen>0)chosen--;
                        if(key==SDL_CONTROLLER_BUTTON_DPAD_RIGHT && chosen<n-1)chosen++;
                        if(key==SDL_CONTROLLER_BUTTON_DPAD_UP){
                            if(chosen>=6)chosen-=6;
                            else catalog_hero_focus=focused_subsection(category);
                        }
                        if(key==SDL_CONTROLLER_BUTTON_DPAD_DOWN){
                            if(chosen+6<n)chosen+=6;
                            else if(!n)catalog_hero_focus=focused_subsection(category);
                        }
                    }
                }else if(expanded_catalog && catalog_hero_focus>=12 &&
                   catalog_hero_focus<=CATALOG_BACK_FOCUS){
                    if(key==SDL_CONTROLLER_BUTTON_DPAD_LEFT && catalog_hero_focus>12)
                        catalog_hero_focus--;
                    if(key==SDL_CONTROLLER_BUTTON_DPAD_RIGHT && catalog_hero_focus<CATALOG_BACK_FOCUS)
                        catalog_hero_focus++;
                    if(key==SDL_CONTROLLER_BUTTON_DPAD_UP){
                        int first=chosen/GRID_ITEMS*GRID_ITEMS;
                        if(catalog_hero_focus==CATALOG_BACK_FOCUS && first>0){
                            chosen=first-POSTER_COLUMNS+chosen%POSTER_COLUMNS;
                            catalog_hero_focus=0;
                        }else catalog_hero_focus=7+tab_for_category(category);
                    }
                    if(key==SDL_CONTROLLER_BUTTON_DPAD_DOWN)catalog_hero_focus=0;
                }else if(!expanded_catalog && catalog_hero_focus>=16){
                    int count=subsection_count(category);
                    if(key==SDL_CONTROLLER_BUTTON_DPAD_LEFT && catalog_hero_focus>16)
                        catalog_hero_focus--;
                    if(key==SDL_CONTROLLER_BUTTON_DPAD_RIGHT){
                        if(catalog_hero_focus<16+count-1)catalog_hero_focus++;
                        else catalog_hero_focus=3; /* From the last tab to All. */
                    }
                    if(key==SDL_CONTROLLER_BUTTON_DPAD_UP)
                        catalog_hero_focus=n?1:7+tab_for_category(category);
                    if(key==SDL_CONTROLLER_BUTTON_DPAD_DOWN)catalog_hero_focus=0;
                }else if((catalog_hero_focus>=5 && catalog_hero_focus<=11) || catalog_hero_focus==IPTV_TAB_FOCUS){
                    catalog_hero_focus=header_focus_step(catalog_hero_focus,key);
                    if(key==SDL_CONTROLLER_BUTTON_DPAD_DOWN)
                        catalog_hero_focus=expanded_catalog?12+sort_button_for_mode(catalog_sort_mode):
                            (subsection_count(category)?focused_subsection(category):n?1:0);
                }else if(!expanded_catalog && catalog_hero_focus){
                    if(key==SDL_CONTROLLER_BUTTON_DPAD_LEFT){
                        if(catalog_hero_focus==3 && subsection_count(category))
                            catalog_hero_focus=16+subsection_count(category)-1;
                        else if(catalog_hero_focus>1)catalog_hero_focus--;
                    }
                    if(key==SDL_CONTROLLER_BUTTON_DPAD_RIGHT && catalog_hero_focus<3)
                        catalog_hero_focus++;
                    if(key==SDL_CONTROLLER_BUTTON_DPAD_UP)
                        catalog_hero_focus=7+tab_for_category(category);
                    if(key==SDL_CONTROLLER_BUTTON_DPAD_DOWN)
                        catalog_hero_focus=subsection_count(category)?
                            focused_subsection(category):0;
                }else{
                    if(key==SDL_CONTROLLER_BUTTON_DPAD_LEFT && chosen>0)chosen--;
                    if(key==SDL_CONTROLLER_BUTTON_DPAD_RIGHT && chosen<n-1)chosen++;
                    if(key==SDL_CONTROLLER_BUTTON_DPAD_UP){
                        if(expanded_catalog){
                            if(n && chosen%GRID_ITEMS<POSTER_COLUMNS &&
                               (chosen%POSTER_COLUMNS==POSTER_COLUMNS-1 || chosen==n-1))
                                catalog_hero_focus=CATALOG_BACK_FOCUS;
                            else if(chosen>=POSTER_COLUMNS)chosen-=POSTER_COLUMNS;
                            else catalog_hero_focus=12+sort_button_for_mode(catalog_sort_mode);
                        }else if(category!=0){
                            int last=poster_row_start(chosen,0)+POSTER_COLUMNS-1;
                            if(last>=n)last=n-1;
                            if(!n)catalog_hero_focus=7+tab_for_category(category);
                            else if(chosen==last)catalog_hero_focus=3;
                            else catalog_hero_focus=subsection_count(category)?
                                focused_subsection(category):1;
                        }else if(n>5 && (!query[0]?chosen>=5:chosen%6==5))
                            catalog_hero_focus=3; /* Up from rightmost poster. */
                        else if(chosen>=6)chosen-=6;
                        else if(!n)catalog_hero_focus=7+tab_for_category(category);
                        else catalog_hero_focus=subsection_count(category)?
                            focused_subsection(category):(chosen%6>=4?3:1);
                    }
                    if(key==SDL_CONTROLLER_BUTTON_DPAD_DOWN &&
                       (expanded_catalog || category==0) && chosen+POSTER_COLUMNS<n)
                        chosen+=POSTER_COLUMNS;
                    else if(key==SDL_CONTROLLER_BUTTON_DPAD_DOWN && !n)
                        catalog_hero_focus=7+tab_for_category(category);
                }
                if(key==SDL_CONTROLLER_BUTTON_A && (n || catalog_hero_focus>=3)){
                    if(expanded_catalog && catalog_hero_focus>=12 &&
                       catalog_hero_focus<=13){
                        int next=sort_mode_for_button(catalog_hero_focus-12);
                        if(next!=catalog_sort_mode){
                            catalog_sort_mode=next;
                            n=load_category(category,query,movies,message,sizeof(message));
                            chosen=0;
                        }
                    }else if(!expanded_catalog && category==12 && catalog_hero_focus==19){
                        if(!n){
                            clear_history_confirm=0;
                            snprintf(message,sizeof(message),TR("Історія вже порожня","History is already empty"));
                        }else if(!clear_history_confirm){
                            clear_history_confirm=1;
                            snprintf(message,sizeof(message),
                                     TR("Натисніть A ще раз, щоб очистити всю історію","Press A again to clear all history"));
                        }else{
                            int removed=library_clear_history();
                            n=load_category(category,query,movies,message,sizeof(message));
                            chosen=0;catalog_hero_focus=17;clear_history_confirm=0;
                            snprintf(message,sizeof(message),
                                     TR("Історію очищено (%d записів)","History cleared (%d removed)"),removed);
                        }
                    }else if(catalog_hero_focus>=7 && catalog_hero_focus<=11){
                        category=category_for_tab(catalog_hero_focus-7);
                        if(category==0)home_shelf=0;
                        query[0]=message[0]=0;
                        n=load_category(category,query,movies,message,sizeof(message));
                        chosen=0;expanded_catalog=0;catalog_hero_focus=0;
                    }else if(!expanded_catalog && catalog_hero_focus>=16){
                        category=category_for_subsection(category,catalog_hero_focus-16);
                        query[0]=message[0]=0;
                        n=load_category(category,query,movies,message,sizeof(message));
                        chosen=0;
                    }else if(!expanded_catalog && catalog_hero_focus==3){
                        message[0]=0;
                        if(category==0 && !query[0]){
                            category=home_shelf==0?2:home_categories[home_shelf];
                            n=load_category_mode(category,query,movies,message,
                                                 sizeof(message),home_shelf==0?0:n);
                        }
                        expanded_catalog=1;
                        chosen=0;catalog_hero_focus=0;
                    }else if(!expanded_catalog && catalog_hero_focus==2){
                        library_toggle_favorite(&movies[chosen]);
                        snprintf(message,sizeof(message),"%s",library_favorite(&movies[chosen])?
                                 TR("Додано у вибране","Added to favorites"):TR("Видалено з вибраного","Removed from favorites"));
                        if(category==11){
                            n=load_category(category,query,movies,message,sizeof(message));
                            if(chosen>=n)chosen=n?n-1:0;
                            if(!n)catalog_hero_focus=7+tab_for_category(category);
                        }
                    }else{
                        message[0]=0;details_button_focus=0;page=DETAILS;
                        catalog_details_request(&movies[chosen]);
                    }
                }
                if(key==SDL_CONTROLLER_BUTTON_LEFTSHOULDER || key==SDL_CONTROLLER_BUTTON_RIGHTSHOULDER){
                    int tab=main_tab_index(7+tab_for_category(category));
                    int step=key==SDL_CONTROLLER_BUTTON_RIGHTSHOULDER?1:-1;
                    int next=main_tabs[(tab+step+MAIN_TAB_COUNT)%MAIN_TAB_COUNT].focus;
                    if(next==IPTV_TAB_FOCUS){
                        iptv_open();
                        message[0]=0;page=IPTV_CHANNELS;continue;
                    }
                    category=category_for_tab(next-7);
                    if(category==0)home_shelf=0;
                    expanded_catalog=0;catalog_hero_focus=0;
                    query[0]=message[0]=0;
                    n=load_category(category,query,movies,message,sizeof(message));chosen=0;
                }
                if(key==SDL_CONTROLLER_BUTTON_X){
                    char new_query[192]={0};
                    if(keyboard(new_query,sizeof(new_query))){
                        if(!query[0]){
                            search_return_category=category;
                            search_return_expanded=expanded_catalog;
                            search_return_shelf=home_shelf;
                            search_return_chosen=chosen;
                        }
                        /* Search spans movies and TV shows, regardless of the
                           section where the on-screen keyboard was opened. */
                        category=0;
                        snprintf(query,sizeof(query),"%s",new_query);
                        message[0]=0;
                        n=load_category(category,query,movies,message,sizeof(message));
                        chosen=0;catalog_hero_focus=0;
                        expanded_catalog=1;
                    }
                }
                if(key==SDL_CONTROLLER_BUTTON_Y){
                    message[0]=0;
                    n=load_category(category,query,movies,message,sizeof(message));chosen=0;catalog_hero_focus=0;
                }
            }else if(page==DETAILS){
                if(key==SDL_CONTROLLER_BUTTON_DPAD_LEFT)details_button_focus=0;
                if(key==SDL_CONTROLLER_BUTTON_DPAD_RIGHT)details_button_focus=1;
                if(key==SDL_CONTROLLER_BUTTON_B){
                    page=CATALOG;message[0]=0;
                    if(category==11 || category==12 ||
                       category>=CATEGORY_COUNT-2){
                        n=load_category(category,query,movies,message,sizeof(message));
                        if(chosen>=n)chosen=n>0?n-1:0;
                    }
                }
                if(key==SDL_CONTROLLER_BUTTON_X){
                    library_toggle_favorite(&movies[chosen]);
                    snprintf(message,sizeof(message),"%s",library_favorite(&movies[chosen])?
                             TR("Додано у вибране (B — назад)","Added to favorites (B — Back)"):TR("Видалено з вибраного (B — назад)","Removed from favorites (B — Back)"));
                }
                if(key==SDL_CONTROLLER_BUTTON_Y){
                    library_mark_watched(&movies[chosen]);
                    snprintf(message,sizeof(message),TR("Додано в історію переглядів (B — назад)","Added to watch history (B — Back)"));
                }
                if(key==SDL_CONTROLLER_BUTTON_A){
                    if(details_button_focus){
                        library_toggle_favorite(&movies[chosen]);
                        snprintf(message,sizeof(message),"%s",library_favorite(&movies[chosen])?
                                 TR("Додано у вибране","Added to favorites"):TR("Видалено з вибраного","Removed from favorites"));
                    }else{
                        message[0]=0;
                        selected_provider=0;page=PROVIDERS;
                    }
                }
            }else if(page==PROVIDERS){
                if(key==SDL_CONTROLLER_BUTTON_B){page=DETAILS;message[0]=0;}
                if(key==SDL_CONTROLLER_BUTTON_DPAD_UP && selected_provider>0)
                    selected_provider--;
                if(key==SDL_CONTROLLER_BUTTON_DPAD_DOWN && provider_for_row(selected_provider+1)>=0)
                    selected_provider++;
                if(key==SDL_CONTROLLER_BUTTON_A){
                    int provider=provider_for_row(selected_provider);
                    if(provider<0)continue;
                    message[0]=0;
                    Movie lookup=movies[chosen];
                    if(provider!=UAKINO_PROVIDER_MOVY)
                        catalog_use_ukrainian_search_title(&lookup);
                    sn=uakino_search_provider(&lookup,sources,UAKINO_MAX,
                                              provider,message,sizeof(message));
                    sn=catalog_rank_sources(&lookup,sources,sn,source_mode);
                    if(!sn && (!message[0] || provider!=UAKINO_PROVIDER_MOVY))
                        snprintf(message,sizeof(message),TR("Нічого не знайдено","No results found"));
                    source=0;page=SOURCES;
                }
            }else if(page==SOURCES){
                if(key==SDL_CONTROLLER_BUTTON_B){page=PROVIDERS;message[0]=0;}
                if(key==SDL_CONTROLLER_BUTTON_Y && sn){
                    if(movies[chosen].series)
                        snprintf(message,sizeof(message),TR("Відкрийте серіал і виберіть серію для завантаження","Open the show and choose an episode to download"));
                    else {
                        char url[2048]={0};
                        if(uakino_resolve(&sources[source],url,sizeof(url),message,sizeof(message))){
                            int added=downloads_add(&movies[chosen],0,0,"",url);
                            snprintf(message,sizeof(message),"%s",added>0?TR("Додано до завантажень","Added to downloads"):
                                     added==0?TR("Уже в завантаженнях","Already in downloads"):TR("Черга завантажень заповнена","Download queue is full"));
                        }
                    }
                }
                if(key==SDL_CONTROLLER_BUTTON_DPAD_UP && source>0)source--;
                if(key==SDL_CONTROLLER_BUTTON_DPAD_DOWN && source<sn-1)source++;
                if(key==SDL_CONTROLLER_BUTTON_A && sn){
                    active_episode_variant=-1;
                    if(movies[chosen].series){
                        message[0]=0;
                        episode_count=uakino_episodes(&sources[source],episodes,
                                                      EPISODES_MAX,message,sizeof(message));
                        if(episode_count){
                            rebuild_seasons();
                            int last_season=0,last_episode=0;
                            if(library_last_episode(&movies[chosen],&last_season,
                                                    &last_episode,NULL))
                                for(int i=0;i<episode_count;i++)
                                    if(episodes[i].season==last_season &&
                                       episodes[i].number==last_episode){
                                        focus_episode_index(i);break;
                                    }
                            page=SEASONS;continue;
                        }
                        if(!strncmp(sources[source].url,MOVY_SERIES_URL,sizeof(MOVY_SERIES_URL)-1))
                            continue;
                    }
                    char url[2048]={0};
                    message[0]=0;
                    if(!render){
                        snprintf(message,sizeof(message),TR("Плеєр mpv не ініціалізовано","The mpv player could not start"));
                    }else if(uakino_resolve(&sources[source],url,sizeof(url),message,sizeof(message))){
                        snprintf(pending_subtitle,sizeof(pending_subtitle),"%s",uakino_resolved_subtitle());
                        playing_movie=movies[chosen];
                        resume_at=library_progress(&playing_movie);
                        snprintf(playing_url,sizeof(playing_url),"%s",url);
                        apply_hls_quality(player);
                        play_stream(player,url,&movy);
                        video_started_at=SDL_GetTicks();video_loaded=0;
                        controls_until=SDL_GetTicks()+3000;
                        player_return_page=SOURCES;page=PLAYER;
                    }
                }
            }
        }
        if(player){
            for (;;) {
                mpv_event *event=mpv_wait_event(player,0);
                if(event->event_id==MPV_EVENT_NONE)break;
                if(event->event_id==MPV_EVENT_START_FILE){
                    if(player_return_page==IPTV_CHANNELS && event->data)
                        tv.entry=((mpv_event_start_file *)event->data)->playlist_entry_id;
                    if(!movy.seek_started)movy_frame_valid=0;
                    history_playback_committed=0;
                    /* Eden and some GLES drivers may recycle renderer-side
                       resources during source switches. Discard old glyph
                       handles once per file rather than keeping stale GL
                       textures for the whole application session. */
                    text_cache_destroy();
                }
                if(event->event_id==MPV_EVENT_FILE_LOADED && pending_subtitle[0]){
                    /* 'auto' can leave an external subtitle unselected, even
                       when the file has no embedded captions. Explicitly
                       select the first offered track; keep others available
                       in the subtitle picker. */
                    for(int i=0;i<uakino_resolved_subtitle_count();i++){
                        const char *args[]={"sub-add",
                            uakino_resolved_subtitle_url(i),
                            i==0?"select":"auto",
                            uakino_resolved_subtitle_label(i),"",NULL};
                        mpv_command_async(player,0,args);
                    }
                    pending_subtitle[0]=0;
                }
                int movy_source=movy_stream_url(playing_url);
                bool live=player_return_page==IPTV_CHANNELS;
                int file_ready=event->event_id==((movy_source || live)?
                              MPV_EVENT_PLAYBACK_RESTART:MPV_EVENT_FILE_LOADED);
                if(live && file_ready && !video_loaded){
                    tv.playing_since=SDL_GetTicks();
                    controls_until=tv.playing_since+3000;
                }
                if(file_ready)video_loaded=1;
                if(event->event_id==MPV_EVENT_FILE_LOADED && movy.seek_started && movy.sliced){
                    double delay=video_options[5];
                    if(movy.external_subtitle[0])
                        playback_add_subtitle(player,movy.external_subtitle,"","",delay-movy.offset);
                    else playback_set_value(player,"sub-delay",delay);
                }
                if(file_ready && page==PLAYER && resume_at>1 && !movy.seek_started){
                    if(movy_source){
                        double duration=0;
                        if(mpv_get_property(player,"duration",MPV_FORMAT_DOUBLE,&duration)>=0 && duration>0)
                            resume_at=fmin(resume_at,fmax(0,duration-1));
                        movy_seek_to(player,&movy,resume_at,&video_loaded);
                    }else{
                        char seconds[40];snprintf(seconds,sizeof(seconds),"%.1f",resume_at);
                        const char *args[]={"seek",seconds,"absolute",NULL};
                        mpv_command_async(player,0,args);
                    }
                    resume_at=0;
                }else if(file_ready && movy.seek_started){
                    double position=0;
                    if(movy.sliced ||
                       (mpv_get_property(player,"time-pos",MPV_FORMAT_DOUBLE,&position)>=0 &&
                        fabs(position-movy.seek_target)<8)){
                        movy.seek_started=0;
                        controls_until=SDL_GetTicks()+3000;
                    }
                }
                if(event->event_id==MPV_EVENT_END_FILE &&
                   (page==PLAYER || page==SUBDL_PICKER || page==TRACK_PICKER ||
                    (page==VIDEO_SETTINGS && !video_settings_from_menu &&
                     player_return_page==IPTV_CHANNELS))){
                    mpv_event_end_file *end=(mpv_event_end_file*)event->data;
                    if(end && live){
                        if(end->playlist_entry_id==tv.entry &&
                           (end->reason==MPV_END_FILE_REASON_ERROR || end->reason==MPV_END_FILE_REASON_EOF)){
                            if(iptv_skip_failed(player,playing_url,sizeof(playing_url),&movy,
                                               &video_started_at,&video_loaded))page=PLAYER;
                            else{
                                snprintf(message,sizeof(message),"%s",
                                         TR("У цьому списку немає доступних ефірів","No live streams available in this list"));
                                page=IPTV_CHANNELS;
                            }
                        }
                        continue;
                    }
                    int interrupted_seek=end && end->reason==MPV_END_FILE_REASON_EOF && movy.seek_started;
                    if(end && (end->reason==MPV_END_FILE_REASON_ERROR || interrupted_seek)){
                        if(retry_movy(player,&movy,playing_url,video_loaded,&resume_at,&video_started_at)){
                            page=PLAYER;
                            video_loaded=0;
                            continue;
                        }
                        snprintf(message,sizeof(message),TR("Помилка відео: %s (%d)","Video error: %s (%d)"),
                                 mpv_error_string(end->error),end->error);
                        if(interrupted_seek)
                            snprintf(message,sizeof(message),"%s",TR("Movy: перемотування перервано","Movy: seek interrupted"));
                        page=player_return_page;
                    }else if(end && end->reason==MPV_END_FILE_REASON_EOF){
                        if(player_return_page==DOWNLOADS && download_episode)
                            library_set_episode_progress(&playing_movie,download_season,download_episode,0);
                        else if(player_return_page==EPISODES && active_episode_variant>=0)
                            library_set_episode_progress(&playing_movie,
                                episodes[active_episode_variant].season,
                                episodes[active_episode_variant].number,0);
                        int following=player_return_page==EPISODES &&
                                      active_episode_variant>=0 ?
                            catalog_next_episode(episodes,episode_count,
                                                 active_episode_variant):-1;
                        if(autoplay_episodes && following>=0){
                            page=PLAYER;
                            pending_next_episode=following;
                            next_episode_countdown_at=SDL_GetTicks();
                        }else page=player_return_page;
                    }
                }
                if(event->event_id==MPV_EVENT_LOG_MESSAGE){
                    mpv_event_log_message *log=(mpv_event_log_message*)event->data;
                    if(log && log->text) fprintf(stderr,"mpv [%s]: %s",log->prefix,log->text);
                }
            }
        }
        if(subdl_job.thread && SDL_AtomicGet(&subdl_job.done)){
            SDL_WaitThread(subdl_job.thread,NULL);
            subdl_job.thread=NULL;
            if(page==SUBDL_PICKER && subdl_job.downloading && subdl_job.success){
                const SubdlSubtitle *subtitle=&subdl_job.results[subdl_job.selected];
                if(playback_add_subtitle(player,subdl_job.file,subtitle->release,
                                         subtitle->language,video_options[5]-movy.offset)){
                    if(movy_stream_url(playing_url))
                        snprintf(movy.external_subtitle,sizeof(movy.external_subtitle),"%s",subdl_job.file);
                    controls_until=SDL_GetTicks()+3000;
                    page=PLAYER;
                }else snprintf(subdl_job.error,sizeof(subdl_job.error),"%s",
                               TR("Плеєр не зміг відкрити субтитри.",
                                  "The player could not open the subtitles."));
            }
        }
        /* Opening a source (or an invalid stream) must not put a film into
           History. Record it only after mpv really plays at least 15 seconds
           of media, even if the user never exits with the B button. */
        if(page==PLAYER && player_return_page!=IPTV_CHANNELS && video_loaded &&
           !movy.seek_started && !history_playback_committed && player){
            double seconds=0;
            if(mpv_get_property(player,"time-pos",MPV_FORMAT_DOUBLE,&seconds)>=0 &&
               (seconds+=movy.offset)>=15.0){
                if(player_return_page==DOWNLOADS && download_episode)
                    library_set_episode_progress(&playing_movie,
                        download_season,download_episode,seconds);
                else if(player_return_page==EPISODES && active_episode_variant>=0)
                    library_set_episode_progress(&playing_movie,
                        episodes[active_episode_variant].season,
                        episodes[active_episode_variant].number,seconds);
                else library_set_progress(&playing_movie,seconds);
                history_playback_committed=1;
            }
        }
        if(page!=PLAYER){pending_next_episode=-1;next_episode_request=-1;}
        if(page==PLAYER && pending_next_episode>=0 &&
           SDL_GetTicks()-next_episode_countdown_at>=NEXT_EPISODE_DELAY_MS){
            next_episode_request=pending_next_episode;
            pending_next_episode=-1;
        }
        if(page==PLAYER && next_episode_request>=0){
            int index=next_episode_request;
            next_episode_request=-1;
            Source upcoming={0};
            snprintf(upcoming.url,sizeof(upcoming.url),"%s",episodes[index].url);
            char url[2048]={0};
            message[0]=0;
            if(uakino_resolve(&upcoming,url,sizeof(url),message,sizeof(message))){
                focus_episode_index(index);
                active_episode_variant=index;
                resume_at=0;
                snprintf(pending_subtitle,sizeof(pending_subtitle),"%s",
                         uakino_resolved_subtitle());
                snprintf(playing_url,sizeof(playing_url),"%s",url);
                apply_hls_quality(player);
                mpv_set_property_string(player,"pause","no");
                play_stream(player,url,&movy);
                video_started_at=SDL_GetTicks();video_loaded=0;
                controls_until=SDL_GetTicks()+3000;
            }else page=EPISODES;
        }
        catalog_loader_poll(&movies,&n,&movie_capacity,chosen,category,query,message,sizeof(message));
        catalog_details_poll(movies,n);
        if(page==CATALOG && category==0 && !expanded_catalog && !query[0] &&
           !home_current_loaded){
            int count=catalog_home_copy(home_shelf,movies,0,CATALOG_PAGE_SIZE);
            if(count>=0){
                n=count;chosen=0;home_current_loaded=1;message[0]=0;
            }
        }
        /* While the user watches a video without touching the controls, the
           Switch otherwise treats it as idle and progressively dims the
           display. libnx's media playback state inhibits that idle dimming
           and auto-sleep only for the duration of video playback. */
        int watching_video=(page==PLAYER || page==TRACK_PICKER || page==SUBDL_PICKER ||
                            (page==VIDEO_SETTINGS && !video_settings_from_menu));
        downloads_pause(watching_video);
        if(watching_video!=media_playback_mode){
            appletSetMediaPlaybackState(watching_video!=0);
            media_playback_mode=watching_video;
        }
        battery_status_poll();
        ui_now=SDL_GetTicks();
        ui_draw_page=page;
        ui_focus_rect=(SDL_Rect){0};
        ui_animating=0;
        ui_hold_scroll=pending_key>=0 ||
                       (touch.active && touch.page==page && !touch.moved);
        if(page==PLAYER && movy.seek_started && SDL_GetTicks()-movy.seek_started>35000u){
            if(retry_movy(player,&movy,playing_url,video_loaded,&resume_at,&video_started_at))
                video_loaded=0;
            else{
                movy.seek_started=0;
                snprintf(message,sizeof(message),"%s",TR("Movy: сервер не відповів під час перемотування","Movy: server timed out while seeking"));
                command(player,"stop",NULL);
                page=player_return_page;
            }
        }
        if(page==PLAYER && player_return_page==IPTV_CHANNELS){
            Uint32 now=SDL_GetTicks();
            if(video_loaded && tv.playing_since && now-tv.playing_since>=IPTV_STABLE_MS)
                tv.failures=0;
            if(!video_loaded && now-video_started_at>=IPTV_START_TIMEOUT_MS &&
               !iptv_skip_failed(player,playing_url,sizeof(playing_url),&movy,&video_started_at,&video_loaded)){
                snprintf(message,sizeof(message),"%s",
                         TR("У цьому списку немає доступних ефірів","No live streams available in this list"));
                command(player,"stop",NULL);
                page=IPTV_CHANNELS;
            }
        }
        if(page==PLAYER && render){
            ui_theme_enabled=0;
            int movy_source=movy_stream_url(playing_url);
            int seeking=movy.seek_started!=0;
            if(movy_source && !movy_fbo && video_loaded && !seeking)
                movy_fbo=video_framebuffer(&movy_texture);
            int keep_frame=movy_source && movy_fbo && (video_loaded || seeking);
            mpv_opengl_fbo fbo={.fbo=keep_frame?(int)movy_fbo:0,.w=1280,.h=720,
                                .internal_format=keep_frame?GL_RGBA8:0};
            int flip=1,skip=keep_frame && seeking;
            mpv_render_param frame[]={{MPV_RENDER_PARAM_OPENGL_FBO,&fbo},
                                      {MPV_RENDER_PARAM_FLIP_Y,&flip},
                                      {MPV_RENDER_PARAM_SKIP_RENDERING,&skip},
                                      {MPV_RENDER_PARAM_INVALID,NULL}};
            int render_result=mpv_render_context_render(render,frame);
            /* libmpv explicitly does not preserve all OpenGL state (viewport,
               scissor, blend factors). Without restoring it, transparent
               text glyphs may render as solid white rectangles in Eden. */
            glBindFramebuffer(GL_FRAMEBUFFER,0);
            glViewport(0,0,1280,720);
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_CULL_FACE);
            glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
            glEnable(GL_BLEND);
            glBlendEquation(GL_FUNC_ADD);
            glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
            if(keep_frame && !skip && render_result>=0)movy_frame_valid=1;
            if(keep_frame && movy_frame_valid){
                glBindFramebuffer(GL_READ_FRAMEBUFFER,movy_fbo);
                glBlitFramebuffer(0,0,1280,720,0,0,1280,720,GL_COLOR_BUFFER_BIT,GL_NEAREST);
                glBindFramebuffer(GL_READ_FRAMEBUFFER,0);
            }
            if(seeking && !movy_frame_valid)box(&paint,0,0,1280,720,0.f,0.f,0.f);
            if(!video_loaded && !seeking){
                box(&paint,0,0,1280,720,.035f,.044f,.059f);
                box_alpha(&paint,35,33,168,53,.10f,.15f,.22f,.95f,14);
                text(&paint,TR("‹  Назад","‹  Back"),54,44,141);
                const int loading_center_x=640;
                Uint32 tick=SDL_GetTicks()/90;
                for(int i=0;i<12;i++){
                    const float sx[]={0,.5f,.866f,1,.866f,.5f,0,-.5f,-.866f,-1,-.866f,-.5f};
                    const float sy[]={-1,-.866f,-.5f,0,.5f,.866f,1,.866f,.5f,0,-.5f,-.866f};
                    int age=(i+12-(tick%12))%12;
                    float bright=.22f+(12-age)*.047f;
                    box_alpha(&paint,loading_center_x+(int)(sx[i]*42)-6,340+(int)(sy[i]*42)-6,
                              13,13,bright,bright,bright,1.f,6);
                }
                const char *loading=player_return_page==IPTV_CHANNELS?iptv_channel(tv.selected)->title:
                                    TR("Завантаження відео","Loading video");
                text_center(&paint,loading,loading_center_x-470,426,940,paint.font,.76f,.78f,.82f);
                unsigned timeout=movy_stream_url(playing_url)?60000u:30000u;
                if(player_return_page!=IPTV_CHANNELS && !movy.seek_started && video_started_at &&
                   SDL_GetTicks()-video_started_at>timeout &&
                   !retry_movy(player,&movy,playing_url,0,&resume_at,&video_started_at)){
                    snprintf(message,sizeof(message),TR("Час очікування відео вичерпано (%u с)","Video timed out (%u s)"),timeout/1000);
                    command(player,"stop",NULL);page=player_return_page;
                }
            }else if(render_result<0 && !seeking){
                snprintf(message,sizeof(message),TR("Помилка відеорендерера mpv: %s","mpv video renderer error: %s"),mpv_error_string(render_result));
                command(player,"stop",NULL);page=player_return_page;
            }
            /* The battery meter belongs to the player controls overlay:
               show and hide both at the same time while playing. */
            if(video_loaded && SDL_GetTicks()<controls_until){
                const char *live_title=player_return_page==IPTV_CHANNELS?
                    iptv_channel(tv.selected)->title:NULL;
                draw_player_controls(&paint,player,movy.offset,live_title);
                battery_status_horizontal(&paint,1187,28);
            }
            if(pending_next_episode>=0){
                Uint32 elapsed=SDL_GetTicks()-next_episode_countdown_at;
                int remaining=elapsed<NEXT_EPISODE_DELAY_MS?
                    (int)((NEXT_EPISODE_DELAY_MS-elapsed+999u)/1000u):0;
                draw_next_episode_prompt(&paint,&episodes[pending_next_episode],remaining);
            }

        }else if(page==UPDATE_DIALOG){
            draw_update(&paint);
        }else{
            ui_category=category;
            /* The focused card takes priority; the remaining worker slots
               prefetch the visible row and a part of the next row. */
            if(page==CATALOG && n>0)
                posters_prefetch(movies,n,chosen,1);
            const char *heading=query[0]?TR("Результати пошуку","Search results"):ui_text(headings[category]);
            if(category==0 && !query[0] && !expanded_catalog)
                heading=ui_text(home_headings[home_shelf]);
            else if(expanded_catalog && !query[0]){
                if(category==1)heading=TR("Усі серіали","All TV shows");
                else if(category==13)heading=TR("Усі мультфільми","All animated movies");
                else if(category==14)heading=TR("Усі мультсеріали","All animated series");
            }
            draw(&paint,page,movies,n,chosen,sources,sn,source,heading,message);
            if(page==CATALOG && n>0){
                int start=expanded_catalog?(chosen/12)*12:
                          category==0 && !query[0] && chosen>=5?chosen-5:
                          (chosen/6)*6;
                posters_prefetch(movies,n,start,expanded_catalog?18:12);
            }
        }
        SDL_GL_SwapWindow(window);
        /* Do not stack a fixed sleep on top of the time that Eden/Switch
           already spent drawing or waiting for VSync. */
        Uint32 frame_elapsed=SDL_GetTicks()-frame_started_at;
        Uint32 frame_budget=page==PLAYER || ui_animating || pending_key>=0?16u:33u;
        if(frame_elapsed<frame_budget)SDL_Delay(frame_budget-frame_elapsed);
    }
    if(app_update.thread){
        atomic_store(&app_update.cancel,1);
        SDL_WaitThread(app_update.thread,NULL);
    }
    if(!install_update && app_update.job==UPDATE_FILE_JOB){
        char temp[1100];
        snprintf(temp,sizeof(temp),"%s.update",app_update.path);
        remove(temp);
    }
    if(install_update){
        ui_theme_enabled=0;
        box(&paint,0,0,1280,720,.055f,.069f,.091f);
        text_center(&paint,TR("Встановлення оновлення…","Installing update…"),
                    0,330,1280,paint.font_big,.96f,.97f,.98f);
        SDL_GL_SwapWindow(window);
    }
    if(media_playback_mode)appletSetMediaPlaybackState(false);
    if(subdl_job.thread)SDL_WaitThread(subdl_job.thread,NULL);
    if(tv.thread)SDL_WaitThread(tv.thread,NULL);
    iptv_free(&tv.pending);
    iptv_free(&tv.catalog);
    iptv_free(&tv.favorites);
    free(tv.visible);
    downloads_shutdown();
    catalog_loader_shutdown();
    free(movies);
    if(render)mpv_render_context_free(render);
    if(movy_fbo)glDeleteFramebuffers(1,&movy_fbo);
    if(movy_texture)glDeleteTextures(1,&movy_texture);
    if(player)mpv_terminate_destroy(player);
    movy_stream_close();
    if(pad)SDL_GameControllerClose(pad);
    wallpapers_shutdown();
    posters_destroy();
    catalog_http_cleanup();
    text_cache_destroy();
    measure_cache_destroy();
    for(int i=0;i<7;i++){
        if(paint.scripts[i])TTF_CloseFont(paint.scripts[i]);
        if(paint.scripts_small[i])TTF_CloseFont(paint.scripts_small[i]);
        if(paint.scripts_big[i])TTF_CloseFont(paint.scripts_big[i]);
        if(paint.scripts_tiny[i])TTF_CloseFont(paint.scripts_tiny[i]);
    }
    for(int i=0;i<4;i++)if(paint.japanese[i])TTF_CloseFont(paint.japanese[i]);
    if(paint.font_tiny)TTF_CloseFont(paint.font_tiny);
    if(paint.font_small)TTF_CloseFont(paint.font_small);
    if(paint.font_big)TTF_CloseFont(paint.font_big);
    if(paint.font_clock)TTF_CloseFont(paint.font_clock);
    TTF_CloseFont(paint.font);TTF_Quit();
    glDeleteTextures(1,&paint.tex);
    glDeleteBuffers(1,&paint.vbo);
    glDeleteProgram(paint.program);
cleanup:
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(window);SDL_Quit();if(battery_service_ready)psmExit();romfsExit();
    if(install_update && !update_install(app_update.path)){
        consoleInit(NULL);
        puts("Could not install UAPlay.nro. Check the SD card and UAPlay.nro.bak.\nPress B to exit.");
        PadState exit_pad;
        padConfigureInput(1,HidNpadStyleSet_NpadStandard);
        padInitializeDefault(&exit_pad);
        while(appletMainLoop()){
            padUpdate(&exit_pad);
            if(padGetButtonsDown(&exit_pad)&HidNpadButton_B)break;
            consoleUpdate(NULL);
        }
        consoleExit(NULL);
    }
    if (R_SUCCEEDED(network_result)) { curl_global_cleanup(); socketExit(); }
    return 0;
}
