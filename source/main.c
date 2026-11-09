#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

typedef enum { CATALOG, DETAILS, SOURCES, PLAYER, SETTINGS,
               SEASONS, EPISODES, TRACK_PICKER, VIDEO_SETTINGS } Page;

static Episode episodes[EPISODES_MAX];
static int episode_count,season_ids[64],season_count,selected_season,selected_episode;
static MediaTrack tracks[TRACKS_MAX];
static int track_count,selected_track,subtitle_picker;
static int selected_video_option;
static double video_options[]={1.0,100.0,0.0,0.0,48.0,0.0};
static const char *video_props[]={"speed","volume","brightness","contrast",
                                  "sub-font-size","sub-delay"};
static const char *video_titles[]={"Швидкість","Гучність","Яскравість","Контраст",
                                   "Розмір субтитрів","Затримка субтитрів"};
static const double video_steps[]={0.25,5,5,5,4,0.25};
static const double video_min[]={0.5,0,-100,-100,20,-10};
static const double video_max[]={2.0,130,100,100,100,10};
#define VIDEO_OPTION_COUNT 6

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
    for(int i=0;i<episode_count;i++) if(episodes[i].season==season)count++;
    return count;
}
static Episode *selected_episode_item(void) {
    if(selected_season>=season_count)return NULL;
    int season=season_ids[selected_season],nth=0;
    for(int i=0;i<episode_count;i++) if(episodes[i].season==season) {
        if(nth++==selected_episode)return &episodes[i];
    }
    return NULL;
}

static const char *categories[]={"movies","series","top","genre_28","genre_35",
    "genre_18","genre_27","genre_878","genre_16","genre_10749","genre_99",
    "favorites","history"};
static const char *headings[]={"Популярні фільми","Популярні серіали","Найкращі фільми",
    "Бойовики","Комедії","Драми","Жахи","Фантастика","Анімація",
    "Романтика","Документальні","Обране","Історія переглядів"};
#define CATEGORY_COUNT ((int)(sizeof(categories)/sizeof(categories[0])))

static int load_category(int category,const char *query,Movie *movies,char *message,int size) {
    if (!strcmp(categories[category],"favorites")) {
        message[0]=0;
        return library_list(movies,CATALOG_MAX,1);
    }
    if (!strcmp(categories[category],"history")) {
        message[0]=0;
        return library_list(movies,CATALOG_MAX,0);
    }
    return catalog_load(categories[category],query,movies,CATALOG_MAX,message,size);
}
typedef struct {
    GLuint program, vbo, tex;
    GLint tint;
    TTF_Font *font;
} Paint;

static const char *vs = "attribute vec2 pos;attribute vec2 uv;varying vec2 tc;"
                        "void main(){gl_Position=vec4(pos,0.,1.);tc=uv;}";
static const char *fs = "precision mediump float;varying vec2 tc;"
                        "uniform vec4 tint;uniform sampler2D image;"
                        "void main(){gl_FragColor=texture2D(image,tc)*tint;}";
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
    glGenBuffers(1,&p->vbo);
    glGenTextures(1,&p->tex);
    glBindTexture(GL_TEXTURE_2D,p->tex);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    unsigned char white[4]={255,255,255,255};
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,white);
    if (TTF_Init()) return 0;
    p->font=TTF_OpenFont("romfs:/DejaVuSans.ttf",25);
    return p->font != NULL;
}
static void box(Paint *p,int x,int y,int w,int h,float r,float g,float b) {
    float x0=x/640.f-1.f,x1=(x+w)/640.f-1.f;
    float y0=1.f-y/360.f,y1=1.f-(y+h)/360.f;
    float vertices[]={x0,y0,0,0, x1,y0,1,0, x0,y1,0,1,
                      x1,y0,1,0, x1,y1,1,1, x0,y1,0,1};
    glUseProgram(p->program);
    glUniform4f(p->tint,r,g,b,1);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D,p->tex);
    glBindBuffer(GL_ARRAY_BUFFER,p->vbo);
    glBufferData(GL_ARRAY_BUFFER,sizeof(vertices),vertices,GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);glEnableVertexAttribArray(1);
    glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,4*sizeof(float),(void*)0);
    glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,4*sizeof(float),(void*)(2*sizeof(float)));
    glDrawArrays(GL_TRIANGLES,0,6);
}
static void text(Paint *p,const char *str,int x,int y,int limit) {
    if (!str || !*str) return;
    SDL_Color color={255,255,255,255};
    SDL_Surface *surface=TTF_RenderUTF8_Blended(p->font,str,color);
    if (!surface) return;
    SDL_Surface *rgba=SDL_ConvertSurfaceFormat(surface,SDL_PIXELFORMAT_ABGR8888,0);
    if (rgba) {
        GLuint tex;
        glGenTextures(1,&tex);glBindTexture(GL_TEXTURE_2D,tex);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,rgba->w,rgba->h,0,GL_RGBA,GL_UNSIGNED_BYTE,rgba->pixels);
        GLuint previous=p->tex; p->tex=tex;
        float scale=rgba->w>limit?(float)limit/rgba->w:1.f;
        box(p,x,y,(int)(rgba->w*scale),(int)(rgba->h*scale),1,1,1);
        p->tex=previous;glDeleteTextures(1,&tex);
        SDL_FreeSurface(rgba);
    }
    SDL_FreeSurface(surface);
}
static void footer(Paint *p,const char *message) {
    box(p,0,666,1280,54,.12f,.14f,.17f);
    text(p,message,22,675,1230);
}
static void poster(Paint *p,const Movie *movie,int x,int y,int width,int height,int *budget) {
    GLuint image=poster_texture(movie,budget);
    if(!image) {
        box(p,x,y,width,height,.15f,.19f,.23f);
        text(p,"UAFLIX",x+12,y+height/2-15,width-25);
        return;
    }
    GLuint previous=p->tex;
    p->tex=image;
    box(p,x,y,width,height,1,1,1);
    p->tex=previous;
}
static void draw(Paint *p,Page page,Movie *movies,int n,int chosen,
                 Source *sources,int sn,int source,const char *heading,const char *status) {
    glViewport(0,0,1280,720);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
    box(p,0,0,1280,720,.055f,.064f,.08f);
    int poster_budget=1;
    if (page==CATALOG) {
        text(p,"UAFLIX",28,18,330);
        text(p,heading,28,80,1050);
        int start=chosen/8*8;
        for (int i=start;i<n && i<start+8;i++) {
            int k=i-start,x=28+k%4*311,y=139+k/4*248;
            box(p,x,y,292,226,i==chosen?.22f:.11f,i==chosen?.42f:.14f,i==chosen?.51f:.19f);
            poster(p,&movies[i],x+10,y+10,127,206,&poster_budget);
            text(p,movies[i].title,x+147,y+36,134);
            text(p,movies[i].original,x+147,y+110,134);
            text(p,movies[i].year,x+147,y+176,130);
        }
        footer(p,*status?status:"A: Деталі  X: Пошук  L/R: Розділи  -: Налаштування  +: Вихід");
    } else if (page==DETAILS) {
        text(p,movies[chosen].title,340,70,870);
        text(p,movies[chosen].original,340,127,860);
        text(p,movies[chosen].year,340,177,300);
        poster(p,&movies[chosen],40,70,250,390,&poster_budget);
        const char *s=movies[chosen].overview;
        for (int y=245;*s && y<580;y+=40) {
            size_t len=strlen(s);if(len>90){len=90;while(len>20 && s[len]!=' ')len--;}
            char line[128]={0};memcpy(line,s,len);
            text(p,line,340,y,900);
            s+=len;while(*s==' ')s++;
        }
        footer(p,*status?status:"A: UAFresh  X: В обране  Y: Переглянуто  B: Назад");
    } else if (page==SOURCES) {
        text(p,"UAKino / UAFresh — джерела",40,45,1100);
        int first=source/10*10;
        for (int i=first;i<sn && i<first+10;i++) {
            int y=110+(i-first)*51;
            box(p,35,y,1200,46,i==source?.22f:.11f,i==source?.42f:.14f,i==source?.51f:.19f);
            text(p,sources[i].title,50,y+8,1160);
        }
        footer(p,*status?status:"A: Відкрити відео    B: Назад");
    } else if (page==SETTINGS) {
        text(p,"UAFLIX — налаштування",45,50,1100);
        text(p,"Без акаунтів CUB та TorrServer",45,130,1100);
        text(p,"Каталог і пошук: TMDB українською",45,200,1100);
        text(p,"Джерела відео: UAFresh (UAFlix, UASerials)",45,260,1100);
        text(p,"Плеєр: libmpv / FFmpeg, HLS",45,320,1100);
        text(p,"Управління відео: A пауза, X субтитри, Y аудіо",45,385,1100);
        text(p,"Обране та історія зберігаються на SD-карті",45,450,1100);
        footer(p,"B: Повернутися до каталогу");
    } else if (page==SEASONS) {
        text(p,"UAFLIX — вибір сезону",42,45,1120);
        for(int i=0;i<season_count && i<10;i++) {
            int y=125+i*48;
            box(p,40,y,1180,43,i==selected_season?.22f:.10f,
                i==selected_season?.40f:.14f,i==selected_season?.49f:.19f);
            char label[128];
            snprintf(label,sizeof(label),"Сезон %d  ·  %d серій",season_ids[i],
                     episodes_in_season(season_ids[i]));
            text(p,label,57,y+5,1090);
        }
        footer(p,*status?status:"A: Серії сезону   B: Повернутися до джерел");
    } else if (page==EPISODES) {
        char heading[128];
        snprintf(heading,sizeof(heading),"UAFLIX — сезон %d: вибір серії",season_ids[selected_season]);
        text(p,heading,42,45,1130);
        int idx=0,first=selected_episode/10*10;
        for(int i=0;i<episode_count;i++) {
            if(episodes[i].season!=season_ids[selected_season])continue;
            if(idx>=first && idx<first+10) {
                int y=118+(idx-first)*51;
                box(p,40,y,1190,46,idx==selected_episode?.22f:.10f,
                    idx==selected_episode?.40f:.14f,idx==selected_episode?.49f:.19f);
                text(p,episodes[i].title,56,y+7,1130);
            }
            idx++;
        }
        footer(p,*status?status:"A: Дивитися серію   B: Вибір сезону");
    } else if (page==TRACK_PICKER) {
        text(p,subtitle_picker?"UAFLIX — субтитри":"UAFLIX — аудіодоріжки",45,50,1110);
        int first=selected_track/10*10;
        for(int i=first;i<track_count && i<first+10;i++) {
            int y=125+(i-first)*50;
            box(p,38,y,1190,44,i==selected_track?.22f:.10f,
                i==selected_track?.40f:.14f,i==selected_track?.49f:.19f);
            text(p,tracks[i].label,55,y+7,1140);
        }
        if (!track_count) text(p,"Немає доступних доріжок",60,200,1050);
        footer(p,"A: Вибрати   B: Назад до відео");
    } else if (page==VIDEO_SETTINGS) {
        text(p,"UAFLIX — параметри відео",45,50,1140);
        for(int i=0;i<VIDEO_OPTION_COUNT;i++) {
            int y=130+i*70;
            box(p,40,y,1180,62,i==selected_video_option?.22f:.10f,
                i==selected_video_option?.40f:.14f,i==selected_video_option?.49f:.19f);
            char label[256];
            snprintf(label,sizeof(label),"%s: %.2f",video_titles[i],video_options[i]);
            text(p,label,56,y+16,1130);
        }
        footer(p,"Вліво / вправо: змінити параметр   B: Назад");
    }
}
static void command(mpv_handle *player,const char *verb,const char *arg) {
    const char *args[]={verb,arg,NULL};
    if(player) mpv_command_async(player,0,args);
}
static void seek(mpv_handle *player,const char *seconds) {
    const char *args[]={"seek",seconds,"relative",NULL};
    if(player) mpv_command_async(player,0,args);
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
    Result network_result = socketInitializeDefault();
    if (R_SUCCEEDED(network_result)) curl_global_init(CURL_GLOBAL_DEFAULT);
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS,"1");
    if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMECONTROLLER)) return 1;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,0);
    SDL_Window *window=SDL_CreateWindow("Uaflix",0,0,1280,720,SDL_WINDOW_OPENGL|SDL_WINDOW_SHOWN);
    if(!window) { SDL_Quit();return 2; }
    SDL_GLContext gl=SDL_GL_CreateContext(window);
    if(!gl) { SDL_DestroyWindow(window);SDL_Quit();return 3; }
    Paint paint={0};
    if(!paint_init(&paint)) goto cleanup;
    SDL_GameController *pad=NULL;
    for(int i=0;i<SDL_NumJoysticks();i++)
        if(SDL_IsGameController(i)){pad=SDL_GameControllerOpen(i);break;}
    mpv_handle *player=mpv_create();
    mpv_render_context *render=NULL;
    if(player){
        mpv_set_option_string(player,"vo","libmpv");
        mpv_set_option_string(player,"hwdec","auto-safe");
        mpv_set_option_string(player,"terminal","no");
        if(mpv_initialize(player)>=0){
            mpv_opengl_init_params gl_init={.get_proc_address=proc};
            mpv_render_param init[]={{MPV_RENDER_PARAM_API_TYPE,MPV_RENDER_API_TYPE_OPENGL},
                                    {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS,&gl_init},
                                    {MPV_RENDER_PARAM_INVALID,NULL}};
            mpv_render_context_create(&render,player,init);
        }
    }
    Movie movies[CATALOG_MAX]={0};
    Source sources[UAKINO_MAX]={0};
    int n=0,sn=0,chosen=0,source=0,category=0,running=1;
    Page page=CATALOG;
    char query[192]={0},message[256]={0};
    n=load_category(category,query,movies,message,sizeof(message));
    Movie playing_movie={0};
    double resume_at=0;
    Page player_return_page=SOURCES;
    while(running){
        SDL_Event e;
        while(SDL_PollEvent(&e)){
            if(e.type==SDL_QUIT)running=0;
            int key=-1;
            if(e.type==SDL_CONTROLLERBUTTONDOWN)key=e.cbutton.button;
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
            if(key<0)continue;
            if(page==PLAYER){
                if(key==SDL_CONTROLLER_BUTTON_B){
                    double position=0;
                    if (player && mpv_get_property(player,"time-pos",MPV_FORMAT_DOUBLE,&position)>=0)
                        library_set_progress(&playing_movie,position);
                    command(player,"stop",NULL);page=player_return_page;
                }
                if(key==SDL_CONTROLLER_BUTTON_A)command(player,"cycle","pause");
                if(key==SDL_CONTROLLER_BUTTON_DPAD_LEFT)seek(player,"-10");
                if(key==SDL_CONTROLLER_BUTTON_DPAD_RIGHT)seek(player,"10");
                if(key==SDL_CONTROLLER_BUTTON_X || key==SDL_CONTROLLER_BUTTON_Y){
                    subtitle_picker=key==SDL_CONTROLLER_BUTTON_X;
                    track_count=playback_tracks(player,subtitle_picker,tracks,TRACKS_MAX);
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
                    playback_select_track(player,subtitle_picker,tracks[selected_track].id);
                    page=PLAYER;
                }
                continue;
            }
            if(page==VIDEO_SETTINGS){
                if(key==SDL_CONTROLLER_BUTTON_B || key==SDL_CONTROLLER_BUTTON_BACK)page=PLAYER;
                if(key==SDL_CONTROLLER_BUTTON_DPAD_UP && selected_video_option>0)selected_video_option--;
                if(key==SDL_CONTROLLER_BUTTON_DPAD_DOWN && selected_video_option<VIDEO_OPTION_COUNT-1)selected_video_option++;
                int direction=(key==SDL_CONTROLLER_BUTTON_DPAD_RIGHT)-(key==SDL_CONTROLLER_BUTTON_DPAD_LEFT);
                if(direction){
                    int i=selected_video_option;
                    video_options[i]+=direction*video_steps[i];
                    if(video_options[i]<video_min[i])video_options[i]=video_min[i];
                    if(video_options[i]>video_max[i])video_options[i]=video_max[i];
                    playback_set_value(player,video_props[i],video_options[i]);
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
                            playing_movie=movies[chosen];
                            resume_at=0;
                            library_mark_watched(&playing_movie);
                            command(player,"loadfile",url);
                            player_return_page=EPISODES;page=PLAYER;
                        }
                    }
                }
                continue;
            }
            if(page==SETTINGS){
                if(key==SDL_CONTROLLER_BUTTON_B || key==SDL_CONTROLLER_BUTTON_BACK)page=CATALOG;
                continue;
            }
            if(key==SDL_CONTROLLER_BUTTON_START){running=0;continue;}
            if(page==CATALOG){
                if(key==SDL_CONTROLLER_BUTTON_BACK){page=SETTINGS;continue;}
                if(key==SDL_CONTROLLER_BUTTON_DPAD_LEFT && chosen>0)chosen--;
                if(key==SDL_CONTROLLER_BUTTON_DPAD_RIGHT && chosen<n-1)chosen++;
                if(key==SDL_CONTROLLER_BUTTON_DPAD_UP && chosen>=4)chosen-=4;
                if(key==SDL_CONTROLLER_BUTTON_DPAD_DOWN && chosen+4<n)chosen+=4;
                if(key==SDL_CONTROLLER_BUTTON_A && n){message[0]=0;page=DETAILS;}
                if(key==SDL_CONTROLLER_BUTTON_LEFTSHOULDER || key==SDL_CONTROLLER_BUTTON_RIGHTSHOULDER){
                    category=(category+(key==SDL_CONTROLLER_BUTTON_RIGHTSHOULDER?1:CATEGORY_COUNT-1))%CATEGORY_COUNT;
                    query[0]=message[0]=0;
                    n=load_category(category,query,movies,message,sizeof(message));chosen=0;
                }
                if(key==SDL_CONTROLLER_BUTTON_X){
                    char new_query[192]={0};
                    if(keyboard(new_query,sizeof(new_query))){
                        snprintf(query,sizeof(query),"%s",new_query);
                        message[0]=0;
                        n=load_category(category,query,movies,message,sizeof(message));chosen=0;
                    }
                }
                if(key==SDL_CONTROLLER_BUTTON_Y){
                    message[0]=0;
                    n=load_category(category,query,movies,message,sizeof(message));chosen=0;
                }
            }else if(page==DETAILS){
                if(key==SDL_CONTROLLER_BUTTON_B){
                    page=CATALOG;message[0]=0;
                    if(category>=CATEGORY_COUNT-2){
                        n=load_category(category,query,movies,message,sizeof(message));
                        if(chosen>=n)chosen=n>0?n-1:0;
                    }
                }
                if(key==SDL_CONTROLLER_BUTTON_X){
                    library_toggle_favorite(&movies[chosen]);
                    snprintf(message,sizeof(message),"%s",library_favorite(&movies[chosen])?
                             "Додано в обране (B — назад)":"Видалено з обраного (B — назад)");
                }
                if(key==SDL_CONTROLLER_BUTTON_Y){
                    library_mark_watched(&movies[chosen]);
                    snprintf(message,sizeof(message),"Додано в історію переглядів (B — назад)");
                }
                if(key==SDL_CONTROLLER_BUTTON_A){
                    message[0]=0;
                    sn=uakino_search(&movies[chosen],sources,UAKINO_MAX,message,sizeof(message));
                    source=0;page=SOURCES;
                }
            }else if(page==SOURCES){
                if(key==SDL_CONTROLLER_BUTTON_B){page=DETAILS;message[0]=0;}
                if(key==SDL_CONTROLLER_BUTTON_DPAD_UP && source>0)source--;
                if(key==SDL_CONTROLLER_BUTTON_DPAD_DOWN && source<sn-1)source++;
                if(key==SDL_CONTROLLER_BUTTON_A && sn){
                    if(movies[chosen].series){
                        message[0]=0;
                        episode_count=uakino_episodes(&sources[source],episodes,
                                                      EPISODES_MAX,message,sizeof(message));
                        if(episode_count){
                            rebuild_seasons();page=SEASONS;continue;
                        }
                    }
                    char url[2048]={0};
                    message[0]=0;
                    if(render && uakino_resolve(&sources[source],url,sizeof(url),message,sizeof(message))){
                        playing_movie=movies[chosen];
                        resume_at=library_progress(&playing_movie);
                        library_mark_watched(&playing_movie);
                        command(player,"loadfile",url);
                        player_return_page=SOURCES;page=PLAYER;
                    }
                }
            }
        }
        if(player){
            for (;;) {
                mpv_event *event=mpv_wait_event(player,0);
                if(event->event_id==MPV_EVENT_NONE)break;
                if(event->event_id==MPV_EVENT_FILE_LOADED && resume_at>1){
                    char seconds[40];snprintf(seconds,sizeof(seconds),"%.1f",resume_at);
                    const char *args[]={"seek",seconds,"absolute",NULL};
                    mpv_command_async(player,0,args);
                    resume_at=0;
                }
            }
        }
        if(page==PLAYER && render){
            mpv_opengl_fbo fbo={.fbo=0,.w=1280,.h=720};int flip=1;
            mpv_render_param frame[]={{MPV_RENDER_PARAM_OPENGL_FBO,&fbo},
                                      {MPV_RENDER_PARAM_FLIP_Y,&flip},
                                      {MPV_RENDER_PARAM_INVALID,NULL}};
            mpv_render_context_render(render,frame);
        }else draw(&paint,page,movies,n,chosen,sources,sn,source,headings[category],message);
        SDL_GL_SwapWindow(window);SDL_Delay(16);
    }
    if(render)mpv_render_context_free(render);
    if(player)mpv_terminate_destroy(player);
    if(pad)SDL_GameControllerClose(pad);
    posters_destroy();
    TTF_CloseFont(paint.font);TTF_Quit();
    glDeleteTextures(1,&paint.tex);
    glDeleteBuffers(1,&paint.vbo);
    glDeleteProgram(paint.program);
cleanup:
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(window);SDL_Quit();romfsExit();
    if (R_SUCCEEDED(network_result)) { curl_global_cleanup(); socketExit(); }
    return 0;
}
