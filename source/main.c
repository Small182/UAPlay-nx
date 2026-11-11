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
#include <GLES2/gl2.h>
#include <mpv/client.h>
#include <mpv/render_gl.h>
#include <curl/curl.h>
#include "catalog.h"
#include "posters.h"
#include "library.h"
#include "playback.h"
#include "downloads.h"

typedef enum { CATALOG, DETAILS, PROVIDERS, SOURCES, PLAYER, SETTINGS,
               SEASONS, EPISODES, TRACK_PICKER, VIDEO_SETTINGS, DOWNLOADS } Page;
static int selected_provider=0;
static int download_selected=0;
static unsigned download_confirm=0;
/* Read the real battery percentage at most once every 15 seconds. */
static int battery_service_ready=0,battery_percentage=-1,battery_on_power=0;
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
    PsmChargerType charger=PsmChargerType_Unconnected;
    battery_on_power=R_SUCCEEDED(psmGetChargerType(&charger)) &&
        charger!=PsmChargerType_Unconnected;
}
static int download_season=0, download_episode=0;
static const char *provider_names[]={"UAFlix","UASerials","Eneyida",
    "UAFilm","UAKino","Mikai (аніме)","AnimeON (аніме)"};

static Episode episodes[EPISODES_MAX];
static int episode_count,season_ids[64],season_count,selected_season,selected_episode;
static int active_episode_variant=-1;
static MediaTrack tracks[TRACKS_MAX];
static int track_count,selected_track,subtitle_picker;
#define SOURCE_AUDIO_TRACK_BASE 10000
#define SOURCE_SUBTITLE_TRACK_BASE 20000
static int selected_video_option;
/* Only player controls useful on the console: speed, volume, subtitle size,
   and HLS quality. Keep mpv's existing option storage intact. */
static const int visible_video_options[]={0,1,4,6};
#define VIDEO_OPTION_COUNT ((int)(sizeof(visible_video_options)/sizeof(visible_video_options[0])))
static double video_options[]={1.0,100.0,0.0,0.0,48.0,0.0,0.0};
static const char *video_props[]={"speed","volume","brightness","contrast",
                                  "sub-font-size","sub-delay","hls-bitrate"};
static const char *video_titles[]={"Швидкість","Гучність","Яскравість","Контраст",
                                   "Розмір субтитрів","Затримка субтитрів","Якість HLS"};
static const double video_steps[]={0.25,5,5,5,4,0.25,1};
static const double video_min[]={0.5,0,-100,-100,20,-10,0};
static const double video_max[]={2.0,130,100,100,100,10,4};
/* HLS bitrate ceilings approximate the requested picture height. Exact
   360/480/720/1080p depends on variant resolutions offered by the source. */
static const char *hls_presets[]={"max","800000","1500000","3000000","6000000"};
static const char *hls_labels[]={"Авто","360p","480p","720p","1080p"};

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
static const char *headings[]={"Популярні фільми","Популярні серіали","Усі фільми",
    "Бойовики","Комедії","Драми","Жахи","Фантастика","Мультфільми",
    "Романтика","Документальні","На майбутнє","Історія",
    "Мультфільми","Мультсеріали","Аніме"};
#define CATEGORY_COUNT ((int)(sizeof(categories)/sizeof(categories[0])))
#define ANIMATION_FIRST 13
#define ANIMATION_LAST 15
static const int home_categories[]={0,1,13,14};
static const char *home_headings[]={"Популярні фільми","Популярні серіали",
                                    "Популярні мультфільми","Популярні мультсеріали"};
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
   12...14: expanded sorting/back controls,
   16...: subsection tabs, 18: clear watch history. */
static int catalog_hero_focus=0;
static int header_focus_step(int focus,int key){
    if(key==SDL_CONTROLLER_BUTTON_DPAD_LEFT){
        if(focus==6)return 5;
        if(focus==5)return 11;
        if(focus>=8 && focus<=11)return focus-1;
    }else if(key==SDL_CONTROLLER_BUTTON_DPAD_RIGHT){
        if(focus>=7 && focus<=10)return focus+1;
        if(focus==11)return 5;
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
static int settings_selected=0,ui_accent_alt=0,source_mode=0;
static int autoplay_episodes=1;
#define NEXT_EPISODE_DELAY_MS 3000u
static int ui_light_theme=0,ui_theme_enabled=1,ui_gradient_pass=0;
#define THEME_PATH "sdmc:/switch/uaplay/theme.txt"
static void theme_load(void){
    FILE *f=fopen(THEME_PATH,"r");
    if(f){
        ui_light_theme=fgetc(f)=='1';
        /* Older installations have a one-character theme file. For those,
           autoplay remains enabled by default. */
        if(fgetc(f)=='0')autoplay_episodes=0;
        fclose(f);
    }
}
static void theme_save(void){
    mkdir("sdmc:/switch/uaplay",0777);
    FILE *f=fopen(THEME_PATH,"w");
    if(f){
        fputc(ui_light_theme?'1':'0',f);
        fputc(autoplay_episodes?'1':'0',f);
        fputc('\n',f);
        fclose(f);
    }
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
static const char *film_labels[]={"Усі фільми","Бойовики","Драми","Комедії","Документальні"};
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
    int details_requested,details_ready;
    Movie details_request,details_result;
    char category[48],query[192];
    Movie ready_items[CATALOG_PAGE_SIZE];
} CatalogLoader;
static CatalogLoader catalog_loader;
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
        if(loader->details_requested){
            Movie target=loader->details_request;
            loader->details_requested=0;
            SDL_UnlockMutex(loader->mutex);
            catalog_fetch_details(&target);
            SDL_LockMutex(loader->mutex);
            if(!loader->quit){
                loader->details_result=target;
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
            SDL_UnlockMutex(loader->mutex);
            if(shelf<0)continue;
            memset(batch,0,CATALOG_PAGE_SIZE*sizeof(Movie));
            char error[192]={0};
            int count=catalog_load_page_sorted(categories[home_categories[shelf]],
                         "",1,0,batch,CATALOG_PAGE_SIZE,error,sizeof(error),NULL);
            SDL_LockMutex(loader->mutex);
            if(!loader->quit){
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
    if(!loader->thread || page<2 || page>500)return;
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
static void catalog_home_prime(const Movie *movies,int count){
    CatalogLoader *loader=&catalog_loader;
    if(!loader->mutex)return;
    SDL_LockMutex(loader->mutex);
    if(count>CATALOG_PAGE_SIZE)count=CATALOG_PAGE_SIZE;
    home_shelves[0].count=count;
    if(count>0)memcpy(home_shelves[0].items,movies,(size_t)count*sizeof(Movie));
    home_shelves[0].state=3;
    for(int i=1;i<HOME_SHELF_COUNT;i++)home_shelves[i].state=1;
    home_cache_initialized=1;
    home_current_loaded=1;
    if(loader->cond)SDL_CondSignal(loader->cond);
    SDL_UnlockMutex(loader->mutex);
}
static int catalog_home_copy(int shelf,Movie *movies){
    CatalogLoader *loader=&catalog_loader;
    if(!loader->mutex || shelf<0 || shelf>=HOME_SHELF_COUNT)return -1;
    SDL_LockMutex(loader->mutex);
    int count=home_shelves[shelf].state==3?home_shelves[shelf].count:-1;
    if(count>0)memcpy(movies,home_shelves[shelf].items,(size_t)count*sizeof(Movie));
    SDL_UnlockMutex(loader->mutex);
    return count;
}
static void catalog_details_request(const Movie *movie){
    CatalogLoader *loader=&catalog_loader;
    if(!loader->thread || !movie || movie->details_state)return;
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
    loader->details_ready=0;
    SDL_UnlockMutex(loader->mutex);
    if(!ready)return;
    for(int i=0;i<count;i++)if(movies[i].id==result.id &&
                             movies[i].series==result.series){
        movies[i].runtime_minutes=result.runtime_minutes;
        movies[i].season_count=result.season_count;
        movies[i].episode_count=result.episode_count;
        movies[i].details_state=result.details_state;
    }
}
static void catalog_loader_poll(Movie **movies,int *count,int *capacity,int chosen,
                                int category,const char *query){
    CatalogLoader *loader=&catalog_loader;
    if(!loader->mutex)return;
    int batch_count=0,ready=0,more=0;
    int limit=(expanded_catalog || (query && *query))?
               CATALOG_SEARCH_MAX:CATALOG_MAX;
    SDL_LockMutex(loader->mutex);
    if(loader->ready && loader->ready_generation==loader->generation){
        ready=1;batch_count=loader->ready_count;
        more=loader->ready_has_more;
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

static int load_category(int category,const char *query,Movie *movies,char *message,int size) {
    static int previous_category=-1;
    static char previous_query[192];
    int data_category=category==0 && !query[0]?home_categories[home_shelf]:category;
    if(data_category!=previous_category || strcmp(query,previous_query))catalog_sort_mode=0;
    previous_category=data_category;
    snprintf(previous_query,sizeof(previous_query),"%s",query);
    if(category==0 && !query[0] && home_cache_initialized){
        int cached=catalog_home_copy(home_shelf,movies);
        home_current_loaded=cached>=0;
        if(cached<0){
            snprintf(message,(size_t)size,"Завантаження добірки...");
            cached=0;
        }else message[0]=0;
        catalog_loader_reset(data_category,query,cached,0);
        return cached;
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
typedef struct {
    GLuint program, vbo, tex, solid_tex;
    GLint tint, shape, gradient, outline, sampler;
    TTF_Font *font;
    TTF_Font *font_big;
    TTF_Font *font_small;
    TTF_Font *font_tiny;
    TTF_Font *scripts[7];
    TTF_Font *scripts_small[7];
    TTF_Font *scripts_big[7];
    TTF_Font *scripts_tiny[7];
    TTF_Font *japanese[4];
} Paint;
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
                        "uniform vec4 tint;uniform sampler2D image;uniform vec3 shape;"
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
                        "gl_FragColor=texture2D(image,tc)*vec4(rgb,tint.a)*a;}";
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
    glUseProgram(p->program);
    glUniform1i(p->sampler,0);
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
    if(limit>0){
        glEnable(GL_SCISSOR_TEST);
        glScissor(x,720-y-height,limit,height);
    }
    box_alpha(p,x,y,width,height,r,g,b,1.f,0);
    if(limit>0)glDisable(GL_SCISSOR_TEST);
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
static Uint32 ui_press_at=0;
static void selection_frame(Paint *p,int x,int y,int w,int h){
    /* A single rounded SDF ring tracks the underlying button/card corners.
       Separate rectangular strokes left square joints on the Switch UI. */
    float r=ui_light_theme?.69f:.20f;
    float g=ui_light_theme?.13f:.67f;
    float b=ui_light_theme?.20f:.85f;
    int radius=h>=60?18:h>=40?17:12;
    ui_outline_width=3;
    float opacity=.94f+.06f*sinf(SDL_GetTicks()*.005f);
    /* The focused control briefly settles inward when A is pressed. */
    if(SDL_GetTicks()-ui_press_at<95){x++;y++;w-=2;h-=2;}
    box_alpha(p,x-3,y-3,w+6,h+6,r,g,b,opacity,radius);
    ui_outline_width=0;
}
static void button(Paint *p,const char *label,int x,int y,int w,int h,int primary){
    /* Flat fills: no simulated border/shadow in Basic White. */
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
   horizontal inside the transient player controls. Neither displays digits.
   Cyan dot means an external power connection, not necessarily charging. */
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
    if(battery_on_power)
        box_alpha(p,x+17,y-5,5,5,.42f,.91f,.98f,1.f,3);
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
    if(battery_on_power)
        box_alpha(p,bx+26,by-3,7,7,.42f,.91f,.98f,1.f,4);
    ui_theme_enabled=old_theme;
}
static void screen_header(Paint *p,const char *name,const char *context){
    box_alpha(p,0,0,1280,92,.058f,.069f,.083f,1.f,0);
    box_alpha(p,UI_EDGE,23,55,48,.13f,.16f,.20f,1.f,13);
    text_center(p,"‹",UI_EDGE,25,55,p->font_big,.96f,.97f,.99f);
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
}
static int footer_position,footer_total;
static Page footer_page;
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
        HINT("L","Розділ");HINT("R","Розділ");
        HINT("X","Пошук");HINT("−","Меню");HINT("A","Відкрити");
        if(ui_category==0 && !expanded_catalog)HINT("↑↓","Добірка");
        if(expanded_catalog)HINT("B","Згорнути");
        else if(ui_category!=0)HINT("B","Головна");
    }else if(footer_page==DETAILS){
        HINT("X","Вибране");HINT("Y","Переглянуто");
        HINT("A","Натиснути");HINT("B","Назад");
    }else if(footer_page==SETTINGS){
        HINT("↑↓","Пункт");HINT("A","Змінити");HINT("B","Закрити");
    }else if(footer_page==TRACK_PICKER){
        HINT("↑↓","Доріжка");HINT("A","Застосувати");HINT("B","Назад");
    }else if(footer_page==VIDEO_SETTINGS){
        HINT("↑↓","Параметр");HINT("←→","Змінити");HINT("B","Назад");
    }else if(footer_page==SEASONS){
        HINT("↑↓","Сезон");HINT("A","Серії");HINT("B","Назад");
    }else if(footer_page==EPISODES){
        HINT("↑↓","Серія");HINT("A","Дивитися");HINT("Y","Завантажити");HINT("B","Назад");
    }else if(footer_page==SOURCES){
        HINT("↑↓","Вибір");HINT("A","Відкрити");HINT("Y","Завантажити");HINT("B","Назад");
    }else if(footer_page==DOWNLOADS){
        HINT("↑↓","Файл");HINT("A","Відкрити");HINT("X","Видалити");HINT("B","Назад");
    }else{
        HINT("↑↓","Вибір");HINT("A","Відкрити");HINT("B","Назад");
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
        else snprintf(progress_label,sizeof(progress_label),"↓ %u МБ · %d",(unsigned)(downloaded_bytes>>20),pending_downloads);
        text_font(p,progress_label,250,682,175,p->font_small,ar,ag,ab);
    }
    /* Notification occupies its own safe zone, never the last selectable row. */
    if(message && *message && strncmp(message,"Offline catalog",15) &&
       strncmp(message,"Офлайн-пошук",strlen("Офлайн-пошук")) &&
       strncmp(message,"A:",2) &&
       strncmp(message,"B:",2) && strncmp(message,"Вліво",10)){
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
static void draw(Paint *p,Page page,Movie *movies,int n,int chosen,
                 Source *sources,int sn,int source,const char *heading,const char *status) {
    glViewport(0,0,1280,720);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
    ui_theme_enabled=1;
    ui_gradient_pass=1;
    box_alpha(p,0,0,1280,720,.034f,.042f,.054f,1.f,0);
    ui_gradient_pass=0;
    static int previous_page=-1,previous_category=-1,previous_home_shelf=-1;
    static Uint32 transition_at=0;
    if(previous_page!=(int)page ||
       (page==CATALOG && (previous_category!=ui_category ||
        (ui_category==0 && previous_home_shelf!=home_shelf)))){
        previous_page=(int)page;
        previous_category=ui_category;
        transition_at=SDL_GetTicks();
    }
    previous_home_shelf=home_shelf;
    footer_position=page==CATALOG?(n?chosen+1:0):page==SOURCES?(sn?source+1:0):
                    page==EPISODES?(episode_count?selected_episode+1:0):
                    page==SEASONS?(season_count?selected_season+1:0):0;
    footer_total=page==CATALOG?n:page==SOURCES?sn:page==SEASONS?season_count:
                 page==EPISODES && selected_season<season_count?
                    episodes_in_season(season_ids[selected_season]):0;
    footer_page=page;
    int poster_budget=1;
    if (page==CATALOG) {
        box_alpha(p,0,0,1280,80,.059f,.070f,.081f,1.f,0);
        headline(p,"UAPlay",25,18,166);
        const char *tabs[]={"Головна","Серіали","Фільми","Мультфільми","Вибране"};
        const int tab_x[]={205,307,410,502,650};
        const int tab_w[]={96,95,89,144,106};
        for(int i=0;i<5;i++){
            int x=tab_x[i];
            int active=(i==0 && ui_category==0) ||
                       (i==1 && ui_category==1) ||
                       (i==2 && ui_category>=2 && ui_category<=10) ||
                       (i==3 && ui_category>=ANIMATION_FIRST && ui_category<=ANIMATION_LAST) ||
                       (i==4 && (ui_category==11 || ui_category==12));
            if(active)box_alpha(p,x-12,19,tab_w[i],43,.19f,.24f,.28f,1.f,16);
            if(catalog_hero_focus==i+7)selection_frame(p,x-12,19,tab_w[i],43);
            text_center_button(p,tabs[i],x-12,19,tab_w[i],43,
                               p->font_small,.96f,.97f,.99f);
        }
        /* Search is a real touch target, not a missing Unicode glyph. */
        box_alpha(p,767,17,277,47,.13f,.16f,.19f,1.f,15);
        if(catalog_hero_focus==5)selection_frame(p,767,17,277,47);
        search_icon(p,800,38,.13f,.16f,.19f);
        text_font(p,"Пошук",831,17+(47-text_height(p,"Пошук",p->font_small))/2,
                  177,p->font_small,1.f,1.f,1.f);
        box_alpha(p,1058,17,53,47,.13f,.16f,.19f,1.f,15);
        if(catalog_hero_focus==6)selection_frame(p,1058,17,53,47);
        gear_icon(p,1085,40,.13f,.16f,.19f);
        time_t now=time(NULL);
        struct tm *local=localtime(&now);
        char clock_label[32]="--:--";
        if(local)strftime(clock_label,sizeof(clock_label),"%H:%M",local);
        text_font(p,clock_label,1120,20,105,p->font_big,.95f,.97f,.99f);
        if(ui_category==11 || ui_category==12){
            /* Favorites uses a poster-only library layout, without the hero.
               Both tabs stay reachable even when their list is empty. */
            const char *names[]={"На майбутнє","Історія","Завантажені"};
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
            }
            if(ui_category==12){
                if(catalog_hero_focus==19)
                    selection_frame(p,982,101,266,47);
                button(p,clear_history_confirm?"Підтвердити очищення":
                       "Очистити історію",982,101,266,47,0);
            }
            if(!n){
                text_center_button(p,"Тут нічого немає",UI_EDGE,305,1216,110,
                                   p->font_big,.73f,.77f,.82f);
            }
            const int start=chosen/12*12;
            for(int i=start;i<n && i<start+12;i++){
                int k=i-start,x=54+k%6*201,y=162+k/6*245;
                if(i==chosen && catalog_hero_focus==0)
                    selection_frame(p,x,y,138,206);
                poster(p,&movies[i],x,y,138,206,&poster_budget);
                card_title(p,movies[i].title,x,y+209,166);
            }
        }else if(expanded_catalog){
            headline(p,heading,UI_EDGE,97,960);
            const char *sort_labels[]={"Рейтинг","Популярність"};
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
            }
            if(catalog_hero_focus==14)selection_frame(p,1106,98,142,44);
            button(p,"‹  Назад",1106,98,142,44,0);
            int start=chosen/12*12;
            for(int i=start;i<n && i<start+12;i++){
                int k=i-start,x=54+k%6*201,y=153+k/6*246;
                if(i==chosen)selection_frame(p,x,y,138,206);
                poster(p,&movies[i],x,y,138,206,&poster_budget);
            }
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
                         movies[active].series?"Серіал":"Фільм");
                text_muted(p,tag,hero_left,hero_top+50,890);
                wrapped(p,movies[active].overview,hero_left,hero_top+90,949,2,27,
                        p->font_small,.74f,.79f,.84f);
            }else{
                int loading=ui_category==0 && status &&
                            !strncmp(status,"Завантаження добірки",strlen("Завантаження добірки"));
                headline(p,loading?"Завантаження добірки...":"Не знайдено матеріалів",
                         hero_left,hero_top+14,900);
                text_muted(p,loading?"Зачекайте, отримуємо матеріали":
                           "Спробуйте інший розділ або пошуковий запит",
                           hero_left,hero_top+78,920);
            }
            if(n && catalog_hero_focus==1)
                selection_frame(p,hero_left,hero_button_y,205,HERO_BUTTON_H);
            if(n){
                if(catalog_hero_focus==2)
                    selection_frame(p,hero_favorite_x,hero_button_y,218,HERO_BUTTON_H);
                button(p,"▶  Дивитися",hero_left,hero_button_y,205,HERO_BUTTON_H,1);
                button(p,library_favorite(&movies[active])?
                       "✓  У вибраному":"+  У вибране",hero_favorite_x,hero_button_y,
                       218,HERO_BUTTON_H,0);
            }
            int section_tabs=subsection_count(ui_category);
            if(section_tabs){
                const char *animation_tabs[]={"Мультфільми","Мультсеріали","Аніме"};
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
                    text_center_button(p,films?film_labels[i]:animation_tabs[i],x,328,
                                       w,43,p->font_small,
                                       .96f,.97f,.99f);
                }
            }else text(p,heading,UI_EDGE,336,930);
            if(status && (!strncmp(status,"Offline catalog",15) ||
                          !strncmp(status,"Офлайн-пошук",strlen("Офлайн-пошук"))))
                text_tiny(p,"Офлайн",1026,340,77);
            if(n && catalog_hero_focus==3)selection_frame(p,1110,329,138,42);
            button(p,ui_category==0?"Більше  ›":"Усі  ›",1110,329,138,42,0);
            int start=ui_category==0 && chosen>=5?chosen-5:chosen/6*6;
            for(int i=start;i<n && i<start+6;i++){
                int k=i-start,x=53+k*201,y=389;
                if(i==chosen && !catalog_hero_focus)selection_frame(p,x,y,150,221);
                poster(p,&movies[i],x,y,150,221,&poster_budget);
                card_title(p,movies[i].title,x,y+224,164);
            }
        }
        footer(p,status);
    } else if(page==SETTINGS){
        screen_header(p,"Налаштування","UAPlay");
        const char *items[]={"Тема","Інтерфейс","Плеєр","Автовідтворення"};
        for(int i=0;i<4;i++){
            int y=136+i*88;
            box_alpha(p,32,y,417,72,i==settings_selected?.20f:.09f,
                      i==settings_selected?.29f:.11f,
                      i==settings_selected?.37f:.14f,1.f,16);
            if(i==settings_selected){float r,g,b;accent(&r,&g,&b);
                box_alpha(p,32,y+15,5,42,r,g,b,1.f,2);}
            if(i==settings_selected){
                float r,g,b;accent(&r,&g,&b);
                text_font(p,items[i],55,y+(72-text_height(p,items[i],p->font))/2,
                          372,p->font,r,g,b);
                selection_frame(p,32,y,417,72);
            }else text(p,items[i],55,y+(72-text_height(p,items[i],p->font))/2,372);
        }
        box_alpha(p,475,136,773,480,.10f,.12f,.15f,1.f,20);
        headline(p,items[settings_selected],513,174,685);
        char description[240];
        if(settings_selected==0)snprintf(description,sizeof(description),"Оформлення: %s",ui_light_theme?"Світла тема":"Темна тема");
        else if(settings_selected==1){
            if(ui_light_theme)snprintf(description,sizeof(description),"Акцент світлої теми: синій Nintendo Switch");
            else snprintf(description,sizeof(description),"Акцент: %s",ui_accent_alt?"Блакитний":"Бірюзовий");
        }
        else if(settings_selected==2)snprintf(description,sizeof(description),"Швидкість: %.2f · гучність: %.0f",video_options[0],video_options[1]);
        else snprintf(description,sizeof(description),"Наступна серія: %s",autoplay_episodes?"Автоматично":"Вручну");
        text(p,description,516,255,682);
        const char *help[]={"Перемикає темну та світлу теми. Вибір зберігається на SD-карті.",
            "Змінює колір виділення на всіх екранах.",
            "Налаштуйте відео, гучність та субтитри.",
            "Після завершення серії пропонує наступну через 3 секунди. Кнопка B скасовує перехід. Вибір зберігається."};
        wrapped(p,help[settings_selected],517,315,675,3,29,p->font_small,.73f,.78f,.84f);
        button(p,settings_selected==2?"Відкрити параметри":"Змінити",515,537,258,55,1);
        footer(p,status);
    } else if (page==DETAILS) {
        screen_header(p,movies[chosen].series?"Про серіал":"Про фільм","UAPlay");
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
        if(selected->series && selected->details_state>0 &&
           selected->season_count>0 && selected->episode_count>0)
            snprintf(tag,sizeof(tag),"%s  ·  Серіал  ·  Сезонів: %d  ·  Серій: %d",
                     selected->year,selected->season_count,selected->episode_count);
        else if(!selected->series && selected->details_state>0 &&
                selected->runtime_minutes>0){
            if(selected->runtime_minutes<60)
                snprintf(tag,sizeof(tag),"%s  ·  Фільм  ·  %d хв",
                         selected->year,selected->runtime_minutes);
            else snprintf(tag,sizeof(tag),"%s  ·  Фільм  ·  %d год %02d хв",
                          selected->year,selected->runtime_minutes/60,
                          selected->runtime_minutes%60);
        }else snprintf(tag,sizeof(tag),"%s  ·  %s",selected->year,
                       selected->series?"Серіал":"Фільм");
        text(p,tag,66,259,815);
        summary(p,movies[chosen].overview,66,313,815);
        if(!details_button_focus)selection_frame(p,65,526,238,58);
        else selection_frame(p,320,526,255,58);
        button(p,"▶  Дивитися",65,526,238,58,1);
        button(p,library_favorite(&movies[chosen])?"✓  У вибраному":"+  У вибране",
               320,526,255,58,0);
        footer(p,status);
    } else if (page==PROVIDERS) {
        screen_header(p,"Оберіть джерело","Пошук почнеться після вибору");
        for(int i=0;i<7;i++)
            list_row(p,provider_names[i],UI_ROW_Y+i*UI_ROW_STEP,
                     i==selected_provider,NULL);
        footer(p,status);
    } else if (page==SOURCES) {
        screen_header(p,"Джерела відео",NULL);
        if(!sn)
            text_center_button(p,"Нічого не знайдено",UI_EDGE,305,1216,110,
                               p->font_big,.73f,.77f,.82f);
        int first=source/UI_ROWS*UI_ROWS;
        for(int i=first;i<sn && i<first+UI_ROWS;i++){
            list_row(p,sources[i].title,UI_ROW_Y+(i-first)*UI_ROW_STEP,i==source,NULL);
        }
        footer(p,status);
    } else if (page==DOWNLOADS) {
        static DownloadInfo records[DOWNLOADS_MAX];
        int count=downloads_list(records,DOWNLOADS_MAX);
        footer_position=count?download_selected+1:0;
        footer_total=count;
        screen_header(p,"Завантажені","Збережені фільми та серії");
        if(!count)text_center_button(p,"Список порожній. Натисніть Y на джерелі або серії",
                                      UI_EDGE,305,1216,110,p->font_small,.73f,.77f,.82f);
        int first=download_selected/UI_ROWS*UI_ROWS;
        for(int i=first;i<count && i<first+UI_ROWS;i++){
            const DownloadInfo *d=&records[i];
            char name[300],detail[80];
            if(d->label[0])snprintf(name,sizeof(name),"%.150s · %.95s",d->title,d->label);
            else snprintf(name,sizeof(name),"%.190s",d->title);
            unsigned mb=(unsigned)(d->bytes>>20);
            if(d->state==DOWNLOAD_QUEUED)snprintf(detail,sizeof(detail),"У черзі");
            else if(d->state==DOWNLOAD_ACTIVE && d->permille<0)
                snprintf(detail,sizeof(detail),"%u МБ",mb);
            else if(d->state==DOWNLOAD_ACTIVE)
                snprintf(detail,sizeof(detail),"%d%% · %u МБ",d->permille/10,mb);
            else if(d->state==DOWNLOAD_DONE)snprintf(detail,sizeof(detail),"Готово · %u МБ",mb);
            else snprintf(detail,sizeof(detail),"Помилка");
            int y=UI_ROW_Y+(i-first)*UI_ROW_STEP;
            list_row(p,name,y,i==download_selected,detail);
            if(d->state==DOWNLOAD_ACTIVE && d->permille>=0)
                box_alpha(p,UI_EDGE+12,y+UI_ROW_HEIGHT-5,1192*d->permille/1000,
                          3,.30f,.71f,.81f,1.f,1);
        }
        footer(p,status);
    } else if (page==SEASONS) {
        screen_header(p,"Вибір сезону","Оберіть сезон");
        int first=selected_season/UI_ROWS*UI_ROWS;
        for(int i=first;i<season_count && i<first+UI_ROWS;i++){
            char title[96],detail[48];
            snprintf(title,sizeof(title),"Сезон %d",season_ids[i]);
            snprintf(detail,sizeof(detail),"%d серій",episodes_in_season(season_ids[i]));
            list_row(p,title,UI_ROW_Y+(i-first)*UI_ROW_STEP,i==selected_season,detail);
        }
        footer(p,status);
    } else if (page==EPISODES) {
        char header[96];
        snprintf(header,sizeof(header),"Сезон %d · Серії",season_ids[selected_season]);
        screen_header(p,header,"Оберіть серію");
        int idx=0,first=selected_episode/UI_ROWS*UI_ROWS;
        for(int i=0;i<episode_count;i++) {
            if(episodes[i].season!=season_ids[selected_season])continue;
            if(i>0 && episodes[i-1].season==episodes[i].season &&
               episodes[i-1].number==episodes[i].number)continue;
            if(idx>=first && idx<first+UI_ROWS) {
                list_row(p,episodes[i].title,UI_ROW_Y+(idx-first)*UI_ROW_STEP,
                         idx==selected_episode,NULL);
            }
            idx++;
        }
        footer(p,status);
    } else if (page==TRACK_PICKER) {
        screen_header(p,subtitle_picker?"Субтитри":"Аудіодоріжки","Оберіть доріжку");
        int first=selected_track/UI_ROWS*UI_ROWS;
        for(int i=first;i<track_count && i<first+UI_ROWS;i++) {
            list_row(p,tracks[i].label,UI_ROW_Y+(i-first)*UI_ROW_STEP,
                     i==selected_track,tracks[i].selected?"Активна":NULL);
        }
        if (!track_count) text_muted(p,"У цьому відео немає доступних доріжок",60,200,1100);
        footer(p,status);
    } else if (page==VIDEO_SETTINGS) {
        screen_header(p,"Налаштування відео","Плеєр");
        for(int i=0;i<VIDEO_OPTION_COUNT;i++) {
            int option=visible_video_options[i];
            int y=132+i*68;
            box_alpha(p,UI_EDGE,y,1216,60,i==selected_video_option?.20f:.09f,
                      i==selected_video_option?.30f:.11f,
                      i==selected_video_option?.38f:.14f,1.f,13);
            text_font(p,video_titles[option],55,
                      y+(60-text_height(p,video_titles[option],p->font))/2,
                      830,p->font,.96f,.97f,.98f);
            char value[48];
            if(option==6)snprintf(value,sizeof(value),"%s",hls_labels[(int)video_options[option]]);
            else if(option==0)snprintf(value,sizeof(value),"%.2f",video_options[option]);
            else snprintf(value,sizeof(value),"%.0f",video_options[option]);
            text_center_button(p,value,963,y,240,60,p->font,.95f,.96f,.98f);
            if(i==selected_video_option){float r,g,b;accent(&r,&g,&b);
                box_alpha(p,UI_EDGE,y+12,4,36,r,g,b,1.f,2);}
        }
        text_tiny(p,"Якість HLS визначається потоками, які надає джерело.",50,626,1150);
        footer(p,NULL);
    }
    /* Short ease-out fade after a screen/section transition; fully drawn
       frames remain responsive even while this overlay is fading. */
    Uint32 elapsed=SDL_GetTicks()-transition_at;
    if(elapsed<160){
        float remain=1.f-(float)elapsed/160.f;
        box_alpha(p,0,80,1280,UI_FOOTER_TOP-80,.034f,.042f,.054f,
                  .13f*remain*remain,0);
    }
    /* No battery on details, provider, source, season, episode or settings pages. */
    if(page==CATALOG)battery_status_vertical(p,1216,29);
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
static void draw_player_controls(Paint *p,mpv_handle *player) {
    ui_theme_enabled=0; /* Video playback controls remain readable in both themes. */
    double pos=0,duration=0;
    int paused=0;
    if(player){
        mpv_get_property(player,"time-pos",MPV_FORMAT_DOUBLE,&pos);
        mpv_get_property(player,"duration",MPV_FORMAT_DOUBLE,&duration);
        mpv_get_property(player,"pause",MPV_FORMAT_FLAG,&paused);
    }
    glViewport(0,0,1280,720);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
    box_alpha(p,32,27,141,49,.045f,.063f,.079f,.92f,14);
    text_center(p,"‹  Назад",32,37,141,p->font_small,.96f,.97f,.99f);
    if(active_episode_variant>=0 &&
       catalog_next_episode(episodes,episode_count,active_episode_variant)>=0){
        box_alpha(p,869,27,241,49,.045f,.063f,.079f,.92f,14);
        text_center(p,"»  Наступна серія",869,37,241,
                    p->font_small,.96f,.97f,.99f);
    }
    box_alpha(p,0,507,1280,213,.027f,.034f,.044f,.93f,0);
    text_tiny(p,"UAPlay  ·  Відтворення",35,519,430);
    if(active_episode_variant>=0 &&
       catalog_next_episode(episodes,episode_count,active_episode_variant)>=0)
        text_tiny(p,"L — Наступна серія",790,519,310);
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
    text_tiny(p,"↑↓ Гучність",67,606,190);
    box_alpha(p,70,649,178,5,.35f,.42f,.52f,1.f,3);
    int volume=(int)video_options[1];
    if(volume<0)volume=0;if(volume>130)volume=130;
    if(volume>0)box_alpha(p,70,649,volume*178/130,5,.92f,.94f,.97f,1.f,3);
    box_alpha(p,70+volume*178/130-7,641,14,20,.96f,.97f,.98f,1.f,7);
    char volume_label[24];snprintf(volume_label,sizeof(volume_label),"%d%%",volume);
    text_tiny(p,volume_label,122,672,100);
    const int centers[]={354,476,598,832,954,1076};
    const char *icons[]={"−10",paused?"▶":"Ⅱ","+10","","♪","CC"};
    const char *labels[]={"10 с назад","Пауза / пуск","10 с вперед",
                          "Параметри","Аудіо","Субтитри"};
    for(int i=0;i<6;i++){
        const int w=76;
        box_alpha(p,centers[i]-w/2,610,w,58,.18f,.22f,.26f,.94f,15);
        text_center(p,icons[i],centers[i]-w/2,624,w,
                    i==0||i==2?p->font_small:p->font,.98f,.98f,.99f);
        if(i==3)gear_icon(p,centers[i],639,.18f,.22f,.26f);
        text_center(p,labels[i],centers[i]-72,678,144,p->font_tiny,.78f,.84f,.89f);
    }
}
static void draw_next_episode_prompt(Paint *p,const Episode *next,int seconds){
    ui_theme_enabled=0;
    box_alpha(p,0,0,1280,720,.012f,.018f,.027f,.88f,0);
    box_alpha(p,242,190,796,341,.105f,.144f,.194f,.98f,24);
    headline(p,"Наступна серія",282,223,700);
    char episode_name[120];
    snprintf(episode_name,sizeof(episode_name),"Сезон %d · Серія %d",
             next->season,next->number);
    text(p,episode_name,286,302,697);
    char timer[128];
    snprintf(timer,sizeof(timer),"Автозапуск через %d с",seconds);
    text_small(p,timer,286,376,681);
    text_small(p,"A — дивитися зараз     B — скасувати",286,448,690);
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
    if(strstr(url,"zetvideo.net"))
        mpv_set_property_string(player,"referrer","https://zetvideo.net/");
    else if(strstr(url,"ashdi.vip"))
        mpv_set_property_string(player,"referrer","https://ashdi.vip/");
    else if(strstr(url,"hdvbua.pro"))
        mpv_set_property_string(player,"referrer","https://hdvbua.pro/");
    else mpv_set_property_string(player,"referrer","");
}
static int keyboard(char *value,size_t size) {
    SwkbdConfig cfg;
    if(R_FAILED(swkbdCreate(&cfg,0))) return 0;
    swkbdConfigMakePresetDefault(&cfg);
    Result result=swkbdShow(&cfg,value,size);
    swkbdClose(&cfg);
    return R_SUCCEEDED(result) && value[0];
}
int main(void) {
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
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,0);
    SDL_Window *window=SDL_CreateWindow("UAPlay",0,0,1280,720,SDL_WINDOW_OPENGL|SDL_WINDOW_SHOWN);
    if(!window) { SDL_Quit();return 2; }
    SDL_GLContext gl=SDL_GL_CreateContext(window);
    if(!gl) { SDL_DestroyWindow(window);SDL_Quit();return 3; }
    Paint paint={0};
    if(!paint_init(&paint)) goto cleanup;
    posters_init();
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
    n=load_category(category,query,movies,message,sizeof(message));
    Movie playing_movie={0};
    double resume_at=0;
    char pending_subtitle[1024]={0};
    char playing_url[2048]={0};
    Page player_return_page=SOURCES;
    Uint32 video_started_at=0;
    int video_loaded=0;
    int history_playback_committed=0;
    Uint32 controls_until=0;
    int pending_next_episode=-1,next_episode_request=-1;
    Uint32 next_episode_countdown_at=0;
    Uint32 stick_last_time=0;
    int stick_last_key=0;
    int media_playback_mode=0;
    while(running){
        Uint32 frame_started_at=SDL_GetTicks();
        stick_arrow(pad,&stick_last_time,&stick_last_key);
        SDL_Event e;
        while(SDL_PollEvent(&e)){
            if(e.type==SDL_QUIT)running=0;
            int key=-1;
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
                    case SDLK_TAB:key=SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;break;
                    case SDLK_m:key=SDL_CONTROLLER_BUTTON_BACK;break;
                    case SDLK_ESCAPE:key=SDL_CONTROLLER_BUTTON_START;break;
                }
            }
            int tx=-1,ty=-1;
            if(e.type==SDL_FINGERDOWN ||
               (page==PLAYER && e.type==SDL_FINGERMOTION && video_loaded &&
                SDL_GetTicks()<controls_until)){
                tx=(int)(e.tfinger.x*1280);ty=(int)(e.tfinger.y*720);
            }else if(e.type==SDL_MOUSEBUTTONDOWN &&
                     e.button.button==SDL_BUTTON_LEFT && e.button.which!=SDL_TOUCH_MOUSEID){
                int ww=1280,hh=720;
                SDL_GetWindowSize(window,&ww,&hh);
                tx=e.button.x*1280/(ww?ww:1280);
                ty=e.button.y*720/(hh?hh:720);
            }else if(page==PLAYER && e.type==SDL_MOUSEMOTION &&
                     (e.motion.state&SDL_BUTTON_LMASK) && video_loaded &&
                     SDL_GetTicks()<controls_until){
                int ww=1280,hh=720;
                SDL_GetWindowSize(window,&ww,&hh);
                tx=e.motion.x*1280/(ww?ww:1280);
                ty=e.motion.y*720/(hh?hh:720);
            }
            if(tx>=0 && ty>=0){
                if(page==PLAYER){
                    int was_visible=SDL_GetTicks()<controls_until;
                    controls_until=SDL_GetTicks()+3000;
                    if(!was_visible)continue;
                    if(ty>=24 && ty<=83 && tx>=27 && tx<=180)key=SDL_CONTROLLER_BUTTON_B;
                    else if(ty>=24 && ty<=83 && tx>=869 && tx<=1110 &&
                            active_episode_variant>=0 &&
                            catalog_next_episode(episodes,episode_count,
                                                 active_episode_variant)>=0)
                        key=SDL_CONTROLLER_BUTTON_LEFTSHOULDER;
                    else if(ty>=547 && ty<=594 && tx>=118 && tx<=1161){
                        double duration=0;
                        if(player && mpv_get_property(player,"duration",MPV_FORMAT_DOUBLE,&duration)>=0
                           && duration>0){
                            double to=duration*(tx-126)/1028.0;
                            if(to<0)to=0;if(to>duration)to=duration;
                            char at[48];snprintf(at,sizeof(at),"%.2f",to);
                            const char *args[]={"seek",at,"absolute",NULL};
                            mpv_command_async(player,0,args);
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
                    if(ty<78 && tx>=193 && tx<742){
                        const int tab_x[]={193,295,398,490,638};
                        const int tab_w[]={96,95,89,144,106};
                        int next=category;
                        int focused=-1;
                        for(int i=0;i<5;i++)
                            if(tx>=tab_x[i] && tx<tab_x[i]+tab_w[i])
                                {next=category_for_tab(i);focused=i;break;}
                        int reset_home=focused==0 && home_shelf!=0;
                        if(focused==0)home_shelf=0;
                        if(next!=category || reset_home){
                            category=next;query[0]=message[0]=0;
                            n=load_category(category,query,movies,message,sizeof(message));
                            chosen=0;expanded_catalog=0;catalog_hero_focus=0;
                            clear_history_confirm=0;
                        }
                        if(focused>=0)catalog_hero_focus=7+focused;
                        continue;
                    }
                    if(ty<78 && tx>=767 && tx<1044)key=SDL_CONTROLLER_BUTTON_X;
                    else if(ty<78 && tx>=1058 && tx<1111)key=SDL_CONTROLLER_BUTTON_BACK;
                    else if((category==11 || category==12) && !expanded_catalog){
                        if(ty>=101 && ty<148){
                            int tab=-1;
                            if(tx>=32 && tx<228)tab=0;
                            else if(tx>=244 && tx<402)tab=1;
                            else if(tx>=417 && tx<625)tab=2;
                            if(tab==2){page=DOWNLOADS;download_selected=0;download_confirm=0;continue;}
                            if(tab>=0){
                                if(category!=11+tab){
                                    category=11+tab;
                                    query[0]=message[0]=0;
                                    n=load_category(category,query,movies,message,sizeof(message));
                                    chosen=0;
                                }
                                clear_history_confirm=0;
                                catalog_hero_focus=16+tab;
                                continue;
                            }
                            if(category==12 && tx>=982 && tx<1248){
                                catalog_hero_focus=19;
                                key=SDL_CONTROLLER_BUTTON_A;
                            }
                        }
                        if(ty>=162 && ty<613 && tx>=54){
                            int col=(tx-54)/201,row=(ty-162)/245;
                            int index=chosen/12*12+row*6+col;
                            if(col<6 && row<2 && index<n && (tx-54)%201<138 &&
                               (ty-162)%245<206){
                                chosen=index;
                                catalog_hero_focus=0;
                                clear_history_confirm=0;
                                key=SDL_CONTROLLER_BUTTON_A;
                            }
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
                        if(ty>=95 && ty<145 && tx>=1106){expanded_catalog=0;chosen=0;catalog_hero_focus=0;continue;}
                        if(ty>=153 && ty<613 && tx>=54){
                            int col=(tx-54)/201,row=(ty-153)/246;
                            int index=chosen/12*12+row*6+col;
                            if(col<6 && row<2 && index<n && (tx-54)%201<138 &&
                               (ty-153)%246<206){
                                chosen=index;catalog_hero_focus=0;key=SDL_CONTROLLER_BUTTON_A;
                            }
                        }
                    }else{
                        if(ty>=326 && ty<375 && tx>=1095){
                            catalog_hero_focus=3;key=SDL_CONTROLLER_BUTTON_A;
                        }
                        if(subsection_count(category) && ty>=326 && ty<375){
                            const int anim_x[]={32,240,465},anim_w[]={191,208,134};
                            const int film_x[]={32,197,373,513,672},film_w[]={157,168,132,151,225};
                            int film=tab_for_category(category)==2;
                            for(int i=0;i<subsection_count(category);i++){
                                int x=film?film_x[i]:anim_x[i],w=film?film_w[i]:anim_w[i];
                                if(tx<x || tx>=x+w)continue;
                                category=category_for_subsection(category,i);
                                query[0]=message[0]=0;
                                n=load_category(category,query,movies,message,sizeof(message));
                                chosen=0;catalog_hero_focus=16+i;
                                break;
                            }
                            continue;
                        }
                        if(ty>=252 && ty<303 && tx>=261 &&
                           tx<(category==11?323:479) && n){
                            catalog_hero_focus=2;
                            library_toggle_favorite(&movies[chosen]);
                            if(category==11){
                                n=load_category(category,query,movies,message,sizeof(message));
                                if(chosen>=n)chosen=n?n-1:0;
                                if(!n)catalog_hero_focus=7+tab_for_category(category);
                            }
                            continue;
                        }
                        if(ty>=252 && ty<303 && tx>=44 && tx<249 && n){
                            catalog_hero_focus=1;
                            key=SDL_CONTROLLER_BUTTON_A;
                        }
                        else if(ty>=389 && ty<661 && tx>=53){
                            int start=category==0 && !query[0] && chosen>=5?
                                      chosen-5:chosen/6*6;
                            int col=(tx-53)/201,index=start+col;
                            if(col<6 && index<n && (tx-53)%201<164){
                                chosen=index;catalog_hero_focus=0;key=SDL_CONTROLLER_BUTTON_A;
                            }
                        }
                    }
                }else if(page==PROVIDERS){
                    int slot=(ty-UI_ROW_Y)/UI_ROW_STEP;
                    if(tx>=UI_EDGE && tx<1280-UI_EDGE && ty>=UI_ROW_Y &&
                       (ty-UI_ROW_Y)%UI_ROW_STEP<UI_ROW_HEIGHT &&
                       slot>=0 && slot<7){
                        selected_provider=slot;
                        key=SDL_CONTROLLER_BUTTON_A;
                    }else if(ty<92 && tx<100)key=SDL_CONTROLLER_BUTTON_B;
                }else if(page==DOWNLOADS){
                    int slot=(ty-UI_ROW_Y)/UI_ROW_STEP;
                    int index=download_selected/UI_ROWS*UI_ROWS+slot;
                    if(tx>=UI_EDGE && tx<1280-UI_EDGE && ty>=UI_ROW_Y &&
                       (ty-UI_ROW_Y)%UI_ROW_STEP<UI_ROW_HEIGHT &&
                       slot>=0 && slot<UI_ROWS){
                        DownloadInfo d[DOWNLOADS_MAX];
                        int count=downloads_list(d,DOWNLOADS_MAX);
                        if(index<count){download_selected=index;key=SDL_CONTROLLER_BUTTON_A;}
                    }else if(ty<92 && tx<100)key=SDL_CONTROLLER_BUTTON_B;
                }else if(page==SOURCES){
                    int slot=(ty-UI_ROW_Y)/UI_ROW_STEP,new_source=source/UI_ROWS*UI_ROWS+slot;
                    if(ty>=UI_ROW_Y && (ty-UI_ROW_Y)%UI_ROW_STEP<UI_ROW_HEIGHT &&
                       slot>=0 && slot<UI_ROWS && new_source<sn){
                        source=new_source;key=SDL_CONTROLLER_BUTTON_A;
                    }else if(ty<92 && tx<100)key=SDL_CONTROLLER_BUTTON_B;
                }else if(page==SEASONS){
                    int slot=(ty-UI_ROW_Y)/UI_ROW_STEP;
                    int next=selected_season/UI_ROWS*UI_ROWS+slot;
                    if(ty>=UI_ROW_Y && (ty-UI_ROW_Y)%UI_ROW_STEP<UI_ROW_HEIGHT &&
                       slot>=0 && slot<UI_ROWS && next<season_count){
                        selected_season=next;key=SDL_CONTROLLER_BUTTON_A;
                    }else if(ty<92 && tx<100)key=SDL_CONTROLLER_BUTTON_B;
                }else if(page==EPISODES){
                    int slot=(ty-UI_ROW_Y)/UI_ROW_STEP;
                    int episode=selected_episode/UI_ROWS*UI_ROWS+slot;
                    if(ty>=UI_ROW_Y && (ty-UI_ROW_Y)%UI_ROW_STEP<UI_ROW_HEIGHT &&
                       slot>=0 && slot<UI_ROWS && selected_season<season_count &&
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
                    int slot=(ty-UI_ROW_Y)/UI_ROW_STEP;
                    int new_track=selected_track/UI_ROWS*UI_ROWS+slot;
                    if(ty>=UI_ROW_Y && (ty-UI_ROW_Y)%UI_ROW_STEP<UI_ROW_HEIGHT &&
                       slot>=0 && slot<UI_ROWS && new_track<track_count){
                        selected_track=new_track;key=SDL_CONTROLLER_BUTTON_A;
                    }else if(ty<92 && tx<100)key=SDL_CONTROLLER_BUTTON_B;
                }else if(page==VIDEO_SETTINGS){
                    int slot=(ty-132)/68;
                    if(ty>=132 && (ty-132)%68<60 && slot>=0 && slot<VIDEO_OPTION_COUNT){
                        selected_video_option=slot;
                        key=tx<640?SDL_CONTROLLER_BUTTON_DPAD_LEFT:SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
                    }else if(ty<92 && tx<100)key=SDL_CONTROLLER_BUTTON_B;
                }else if(page==SETTINGS){
                    if(tx>=32 && tx<449 && ty>=136 && ty<472 && (ty-136)%88<72){
                        settings_selected=(ty-136)/88;
                        if(settings_selected>=4)settings_selected=3;
                        key=SDL_CONTROLLER_BUTTON_A;
                    }else if(ty>=537 && ty<592 && tx>=515 && tx<773)key=SDL_CONTROLLER_BUTTON_A;
                    else if(ty<92 && tx<100)key=SDL_CONTROLLER_BUTTON_B;
                }
            }
            if(page==PLAYER && key>=0)controls_until=SDL_GetTicks()+3000;
            if(key<0)continue;
            if(clear_history_confirm && !(page==CATALOG && category==12 &&
               catalog_hero_focus==19 && key==SDL_CONTROLLER_BUTTON_A)){
                clear_history_confirm=0;
                if(!strncmp(message,"Натисніть A ще раз",strlen("Натисніть A ще раз")))
                    message[0]=0;
            }
            if(key==SDL_CONTROLLER_BUTTON_A && page!=PLAYER)ui_press_at=SDL_GetTicks();
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
                    if (player && video_loaded &&
                        mpv_get_property(player,"time-pos",MPV_FORMAT_DOUBLE,&position)>=0 &&
                        position>=15.0)
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
                    command(player,"stop",NULL);page=player_return_page;
                }
                if(key==SDL_CONTROLLER_BUTTON_LEFTSHOULDER &&
                   active_episode_variant>=0 && player_return_page==EPISODES)
                    next_episode_request=catalog_next_episode(
                        episodes,episode_count,active_episode_variant);
                if(key==SDL_CONTROLLER_BUTTON_A)command(player,"cycle","pause");
                int fast=pad && SDL_GameControllerGetAttached(pad) &&
                    SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
                if(key==SDL_CONTROLLER_BUTTON_DPAD_LEFT)seek(player,fast?"-60":"-10");
                if(key==SDL_CONTROLLER_BUTTON_DPAD_RIGHT)seek(player,fast?"60":"10");
                if(key==SDL_CONTROLLER_BUTTON_DPAD_UP)change_volume(player,1);
                if(key==SDL_CONTROLLER_BUTTON_DPAD_DOWN)change_volume(player,-1);
                if(key==SDL_CONTROLLER_BUTTON_X || key==SDL_CONTROLLER_BUTTON_Y){
                    subtitle_picker=key==SDL_CONTROLLER_BUTTON_X;
                    track_count=playback_tracks(player,subtitle_picker,tracks,TRACKS_MAX);
                    /* Even if an external subtitle failed to load into mpv,
                       keep the provider's caption URL available for retry. */
                    if(subtitle_picker)
                        for(int i=0;i<uakino_resolved_subtitle_count() &&
                            track_count<TRACKS_MAX;i++){
                            if(playback_has_external_subtitle(player,
                                   uakino_resolved_subtitle_url(i)))continue;
                            MediaTrack *choice=&tracks[track_count++];
                            choice->id=-(SOURCE_SUBTITLE_TRACK_BASE+i+1);
                            choice->selected=0;
                            snprintf(choice->label,sizeof(choice->label),
                                     "Зовнішні: %.165s",
                                     uakino_resolved_subtitle_label(i));
                        }
                    /* PlayerJS can offer separately encoded dubbed versions
                       as distinct stream URLs, not as native mpv tracks. */
                    if(!subtitle_picker && uakino_resolved_audio_count()>1)
                        for(int i=0;i<uakino_resolved_audio_count() &&
                            track_count<TRACKS_MAX;i++){
                            MediaTrack *choice=&tracks[track_count++];
                            choice->id=-(SOURCE_AUDIO_TRACK_BASE+i+1);
                            choice->selected=!strcmp(playing_url,
                                                      uakino_resolved_audio_url(i));
                            snprintf(choice->label,sizeof(choice->label),
                                     "Озвучка: %.165s",
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
                            snprintf(choice->label,sizeof(choice->label),"Озвучка: %.165s",
                                     episodes[i].voice[0]?episodes[i].voice:"Основна");
                        }
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
                    if(subtitle_picker && id<=-(SOURCE_SUBTITLE_TRACK_BASE+1) &&
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
                            configure_stream(player,url);
                            command(player,"loadfile",url);
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
                            configure_stream(player,url);
                            mpv_set_property_string(player,"pause","no");
                            command(player,"loadfile",url);
                            video_started_at=SDL_GetTicks();video_loaded=0;
                            controls_until=SDL_GetTicks()+3000;
                            page=PLAYER;
                        }
                    }else if(playback_select_track(player,subtitle_picker,id)){
                        snprintf(message,sizeof(message),"%s: вибрано %s",
                                 subtitle_picker?"Субтитри":"Аудіо",
                                 tracks[selected_track].label);
                        page=PLAYER;
                    }else snprintf(message,sizeof(message),"Не вдалося перемкнути доріжку");
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
                        if(playing_url[0] && !video_settings_from_menu){
                            double pos=0;
                            if(mpv_get_property(player,"time-pos",MPV_FORMAT_DOUBLE,&pos)>=0)
                                resume_at=pos;
                            snprintf(pending_subtitle,sizeof(pending_subtitle),"%s",
                                     uakino_resolved_subtitle());
                            command(player,"loadfile",playing_url);
                        }
                    } else playback_set_value(player,video_props[i],video_options[i]);
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
                            snprintf(label,sizeof(label),"Сезон %d · Серія %d",ep->season,ep->number);
                            int added=downloads_add(&movies[chosen],ep->season,ep->number,label,url);
                            snprintf(message,sizeof(message),"%s",added>0?"Додано до завантажень":
                                     added==0?"Уже в завантаженнях":"Черга завантажень заповнена");
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
                            configure_stream(player,url);
                            command(player,"loadfile",url);
                            video_started_at=SDL_GetTicks();video_loaded=0;
                            controls_until=SDL_GetTicks()+3000;
                            player_return_page=EPISODES;page=PLAYER;
                        }
                    } else if(!render) snprintf(message,sizeof(message),"Плеєр mpv не ініціалізовано");
                }
                continue;
            }
            if(page==DOWNLOADS){
                static DownloadInfo rows[DOWNLOADS_MAX];
                int count=downloads_list(rows,DOWNLOADS_MAX);
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
                    unsigned id=rows[download_selected].id;
                    if(download_confirm!=id){
                        download_confirm=id;
                        snprintf(message,sizeof(message),"Натисніть X ще раз для видалення");
                    }else{
                        downloads_remove(id);download_confirm=0;
                        snprintf(message,sizeof(message),"Видалено");
                    }
                }
                if(key==SDL_CONTROLLER_BUTTON_A && count){
                    const DownloadInfo *info=&rows[download_selected];
                    static Download item;
                    if(info->state==DOWNLOAD_FAILED){
                        downloads_retry(info->id);
                        snprintf(message,sizeof(message),"Завантаження повторюється");
                    }else if(info->state!=DOWNLOAD_DONE)
                        snprintf(message,sizeof(message),"Файл ще завантажується");
                    else if(!render)snprintf(message,sizeof(message),"Плеєр mpv не ініціалізовано");
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
                        configure_stream(player,file_path);
                        command(player,"loadfile",file_path);
                        video_started_at=SDL_GetTicks();video_loaded=0;
                        controls_until=SDL_GetTicks()+3000;
                        player_return_page=DOWNLOADS;page=PLAYER;
                    }
                }
                continue;
            }
            if(page==SETTINGS){
                if(key==SDL_CONTROLLER_BUTTON_B || key==SDL_CONTROLLER_BUTTON_BACK){page=CATALOG;continue;}
                if(key==SDL_CONTROLLER_BUTTON_DPAD_UP && settings_selected>0)settings_selected--;
                if(key==SDL_CONTROLLER_BUTTON_DPAD_DOWN && settings_selected<3)settings_selected++;
                if(key==SDL_CONTROLLER_BUTTON_A){
                    if(settings_selected==0){ui_light_theme=!ui_light_theme;theme_save();}
                    else if(settings_selected==1)ui_accent_alt=!ui_accent_alt;
                    else if(settings_selected==2){
                        selected_video_option=0;
                        video_settings_from_menu=1;
                        page=VIDEO_SETTINGS;
                    }else if(settings_selected==3){autoplay_episodes=!autoplay_episodes;theme_save();}
                }
                continue;
            }
            if(key==SDL_CONTROLLER_BUTTON_START){running=0;continue;}
            if(page==CATALOG){
                if(key==SDL_CONTROLLER_BUTTON_BACK){page=SETTINGS;continue;}
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
                    expanded_catalog=0;chosen=0;catalog_hero_focus=0;continue;
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
                    }else if(catalog_hero_focus>=5 && catalog_hero_focus<=11){
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
                   catalog_hero_focus<=14){
                    if(key==SDL_CONTROLLER_BUTTON_DPAD_LEFT && catalog_hero_focus>12)
                        catalog_hero_focus--;
                    if(key==SDL_CONTROLLER_BUTTON_DPAD_RIGHT && catalog_hero_focus<14)
                        catalog_hero_focus++;
                    if(key==SDL_CONTROLLER_BUTTON_DPAD_UP)
                        catalog_hero_focus=7+tab_for_category(category);
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
                }else if(catalog_hero_focus>=5 && catalog_hero_focus<=11){
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
                        if(!expanded_catalog && n>5 &&
                           (category==0 && !query[0] ? chosen>=5 : chosen%6==5))
                            catalog_hero_focus=3; /* Up from rightmost poster. */
                        else if(chosen>=6)chosen-=6;
                        else if(expanded_catalog)
                            catalog_hero_focus=12+sort_button_for_mode(catalog_sort_mode);
                        else if(!n)catalog_hero_focus=7+tab_for_category(category);
                        else catalog_hero_focus=subsection_count(category)?
                            focused_subsection(category):(chosen%6>=4?3:1);
                    }
                    if(key==SDL_CONTROLLER_BUTTON_DPAD_DOWN && chosen+6<n)chosen+=6;
                    else if(key==SDL_CONTROLLER_BUTTON_DPAD_DOWN && !n)
                        catalog_hero_focus=7+tab_for_category(category);
                }
                if(key==SDL_CONTROLLER_BUTTON_A && (n || catalog_hero_focus>=3)){
                    if(expanded_catalog && catalog_hero_focus>=12 &&
                       catalog_hero_focus<=14){
                        if(catalog_hero_focus==14){
                            expanded_catalog=0;catalog_hero_focus=0;chosen=0;
                        }else{
                            int next=sort_mode_for_button(catalog_hero_focus-12);
                            if(next!=catalog_sort_mode){
                                catalog_sort_mode=next;
                                n=load_category(category,query,movies,message,sizeof(message));
                                chosen=0;
                            }
                        }
                    }else if(!expanded_catalog && category==12 && catalog_hero_focus==19){
                        if(!n){
                            clear_history_confirm=0;
                            snprintf(message,sizeof(message),"Історія вже порожня");
                        }else if(!clear_history_confirm){
                            clear_history_confirm=1;
                            snprintf(message,sizeof(message),
                                     "Натисніть A ще раз, щоб очистити всю історію");
                        }else{
                            int removed=library_clear_history();
                            n=load_category(category,query,movies,message,sizeof(message));
                            chosen=0;catalog_hero_focus=17;clear_history_confirm=0;
                            snprintf(message,sizeof(message),
                                     "Історію очищено (%d записів)",removed);
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
                        if(category==0 && !query[0]){
                            category=home_shelf==0?2:home_categories[home_shelf];
                            expanded_catalog=1;
                            query[0]=message[0]=0;
                            n=load_category(category,query,movies,message,sizeof(message));
                        }else expanded_catalog=1;
                        chosen=0;catalog_hero_focus=0;message[0]=0;
                    }else if(!expanded_catalog && catalog_hero_focus==2){
                        library_toggle_favorite(&movies[chosen]);
                        snprintf(message,sizeof(message),"%s",library_favorite(&movies[chosen])?
                                 "Додано у вибране":"Видалено з вибраного");
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
                    int tab=tab_for_category(category);
                    tab=(tab+(key==SDL_CONTROLLER_BUTTON_RIGHTSHOULDER?1:4))%5;
                    category=category_for_tab(tab);
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
                             "Додано у вибране (B — назад)":"Видалено з вибраного (B — назад)");
                }
                if(key==SDL_CONTROLLER_BUTTON_Y){
                    library_mark_watched(&movies[chosen]);
                    snprintf(message,sizeof(message),"Додано в історію переглядів (B — назад)");
                }
                if(key==SDL_CONTROLLER_BUTTON_A){
                    if(details_button_focus){
                        library_toggle_favorite(&movies[chosen]);
                        snprintf(message,sizeof(message),"%s",library_favorite(&movies[chosen])?
                                 "Додано у вибране":"Видалено з вибраного");
                    }else{
                        message[0]=0;
                        selected_provider=0;page=PROVIDERS;
                    }
                }
            }else if(page==PROVIDERS){
                if(key==SDL_CONTROLLER_BUTTON_B){page=DETAILS;message[0]=0;}
                if(key==SDL_CONTROLLER_BUTTON_DPAD_UP && selected_provider>0)
                    selected_provider--;
                if(key==SDL_CONTROLLER_BUTTON_DPAD_DOWN && selected_provider<6)
                    selected_provider++;
                if(key==SDL_CONTROLLER_BUTTON_A){
                    message[0]=0;
                    sn=uakino_search_provider(&movies[chosen],sources,UAKINO_MAX,
                                              selected_provider+1,message,sizeof(message));
                    sn=catalog_rank_sources(&movies[chosen],sources,sn,source_mode);
                    if(!sn)snprintf(message,sizeof(message),"Нічого не знайдено");
                    source=0;page=SOURCES;
                }
            }else if(page==SOURCES){
                if(key==SDL_CONTROLLER_BUTTON_B){page=PROVIDERS;message[0]=0;}
                if(key==SDL_CONTROLLER_BUTTON_Y && sn){
                    if(movies[chosen].series)
                        snprintf(message,sizeof(message),"Відкрийте серіал і виберіть серію для завантаження");
                    else {
                        char url[2048]={0};
                        if(uakino_resolve(&sources[source],url,sizeof(url),message,sizeof(message))){
                            int added=downloads_add(&movies[chosen],0,0,"",url);
                            snprintf(message,sizeof(message),"%s",added>0?"Додано до завантажень":
                                     added==0?"Уже в завантаженнях":"Черга завантажень заповнена");
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
                    }
                    char url[2048]={0};
                    message[0]=0;
                    if(!render){
                        snprintf(message,sizeof(message),"Плеєр mpv не ініціалізовано");
                    }else if(uakino_resolve(&sources[source],url,sizeof(url),message,sizeof(message))){
                        snprintf(pending_subtitle,sizeof(pending_subtitle),"%s",uakino_resolved_subtitle());
                        playing_movie=movies[chosen];
                        resume_at=library_progress(&playing_movie);
                        snprintf(playing_url,sizeof(playing_url),"%s",url);
                        apply_hls_quality(player);
                        configure_stream(player,url);
                        command(player,"loadfile",url);
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
                if(event->event_id==MPV_EVENT_FILE_LOADED)video_loaded=1;
                if(event->event_id==MPV_EVENT_FILE_LOADED && resume_at>1){
                    char seconds[40];snprintf(seconds,sizeof(seconds),"%.1f",resume_at);
                    const char *args[]={"seek",seconds,"absolute",NULL};
                    mpv_command_async(player,0,args);
                    resume_at=0;
                }
                if(event->event_id==MPV_EVENT_END_FILE && page==PLAYER){
                    mpv_event_end_file *end=(mpv_event_end_file*)event->data;
                    if(end && end->reason==MPV_END_FILE_REASON_ERROR){
                        snprintf(message,sizeof(message),"Помилка відео: %s (%d)",
                                 mpv_error_string(end->error),end->error);
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
        /* Opening a source (or an invalid stream) must not put a film into
           History. Record it only after mpv really plays at least 15 seconds
           of media, even if the user never exits with the B button. */
        if(page==PLAYER && video_loaded && !history_playback_committed && player){
            double seconds=0;
            if(mpv_get_property(player,"time-pos",MPV_FORMAT_DOUBLE,&seconds)>=0 &&
               seconds>=15.0){
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
                configure_stream(player,url);
                mpv_set_property_string(player,"pause","no");
                command(player,"loadfile",url);
                video_started_at=SDL_GetTicks();video_loaded=0;
                controls_until=SDL_GetTicks()+3000;
            }else page=EPISODES;
        }
        catalog_loader_poll(&movies,&n,&movie_capacity,chosen,category,query);
        catalog_details_poll(movies,n);
        if(page==CATALOG && category==0 && !expanded_catalog && !query[0] &&
           !home_current_loaded){
            int count=catalog_home_copy(home_shelf,movies);
            if(count>=0){
                n=count;chosen=0;home_current_loaded=1;message[0]=0;
            }
        }
        /* While the user watches a video without touching the controls, the
           Switch otherwise treats it as idle and progressively dims the
           display. libnx's media playback state inhibits that idle dimming
           and auto-sleep only for the duration of video playback. */
        int watching_video=(page==PLAYER || page==TRACK_PICKER ||
                            (page==VIDEO_SETTINGS && !video_settings_from_menu));
        downloads_pause(watching_video);
        if(watching_video!=media_playback_mode){
            appletSetMediaPlaybackState(watching_video!=0);
            media_playback_mode=watching_video;
        }
        battery_status_poll();
        if(page==PLAYER && render){
            ui_theme_enabled=0;
            mpv_opengl_fbo fbo={.fbo=0,.w=1280,.h=720};int flip=1;
            mpv_render_param frame[]={{MPV_RENDER_PARAM_OPENGL_FBO,&fbo},
                                      {MPV_RENDER_PARAM_FLIP_Y,&flip},
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
            if(!video_loaded){
                box(&paint,0,0,1280,720,.035f,.044f,.059f);
                box_alpha(&paint,35,33,168,53,.10f,.15f,.22f,.95f,14);
                text(&paint,"‹  Назад",54,44,141);
                Uint32 tick=SDL_GetTicks()/90;
                for(int i=0;i<12;i++){
                    const float sx[]={0,.5f,.866f,1,.866f,.5f,0,-.5f,-.866f,-1,-.866f,-.5f};
                    const float sy[]={-1,-.866f,-.5f,0,.5f,.866f,1,.866f,.5f,0,-.5f,-.866f};
                    int age=(i+12-(tick%12))%12;
                    float bright=.22f+(12-age)*.047f;
                    box_alpha(&paint,640+(int)(sx[i]*42)-6,340+(int)(sy[i]*42)-6,
                              13,13,bright,bright,bright,1.f,6);
                }
                text_color(&paint,"Завантаження відео",507,426,310,.76f,.78f,.82f);
                if(video_started_at && SDL_GetTicks()-video_started_at>30000){
                    snprintf(message,sizeof(message),"Час очікування відео вичерпано (30 с)");
                    command(player,"stop",NULL);page=player_return_page;
                }
            }else if(render_result<0){
                snprintf(message,sizeof(message),"Помилка відеорендерера mpv: %s",mpv_error_string(render_result));
                command(player,"stop",NULL);page=player_return_page;
            }
            /* The battery meter belongs to the player controls overlay:
               show and hide both at the same time while playing. */
            if(video_loaded && SDL_GetTicks()<controls_until){
                draw_player_controls(&paint,player);
                battery_status_horizontal(&paint,1187,28);
            }
            if(pending_next_episode>=0){
                Uint32 elapsed=SDL_GetTicks()-next_episode_countdown_at;
                int remaining=elapsed<NEXT_EPISODE_DELAY_MS?
                    (int)((NEXT_EPISODE_DELAY_MS-elapsed+999u)/1000u):0;
                draw_next_episode_prompt(&paint,&episodes[pending_next_episode],remaining);
            }

        }else{
            ui_category=category;
            /* The focused card takes priority; the remaining worker slots
               prefetch the visible row and a part of the next row. */
            if(page==CATALOG && n>0)
                posters_prefetch(movies,n,chosen,1);
            const char *heading=query[0]?"Результати пошуку":headings[category];
            if(category==0 && !query[0] && !expanded_catalog)
                heading=home_headings[home_shelf];
            else if(expanded_catalog && !query[0]){
                if(category==1)heading="Усі серіали";
                else if(category==13)heading="Усі мультфільми";
                else if(category==14)heading="Усі мультсеріали";
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
        Uint32 frame_budget=page==PLAYER?16u:33u;
        if(frame_elapsed<frame_budget)SDL_Delay(frame_budget-frame_elapsed);
    }
    if(media_playback_mode)appletSetMediaPlaybackState(false);
    downloads_shutdown();
    catalog_loader_shutdown();
    free(movies);
    if(render)mpv_render_context_free(render);
    if(player)mpv_terminate_destroy(player);
    if(pad)SDL_GameControllerClose(pad);
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
    TTF_CloseFont(paint.font);TTF_Quit();
    glDeleteTextures(1,&paint.tex);
    glDeleteBuffers(1,&paint.vbo);
    glDeleteProgram(paint.program);
cleanup:
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(window);SDL_Quit();if(battery_service_ready)psmExit();romfsExit();
    if (R_SUCCEEDED(network_result)) { curl_global_cleanup(); socketExit(); }
    return 0;
}
