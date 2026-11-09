#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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
               SEASONS, EPISODES, TRACK_PICKER, VIDEO_SETTINGS,
               LOGIN_USER, LOGIN_PASSWORD } Page;
static char login_user[128],login_password[128];
static int login_selected,login_upper;
static const char login_keys[]="abcdefghijklmnopqrstuvwxyz0123456789@._-!#$%&*+?/";

static Episode episodes[EPISODES_MAX];
static int episode_count,season_ids[64],season_count,selected_season,selected_episode;
static MediaTrack tracks[TRACKS_MAX];
static int track_count,selected_track,subtitle_picker;
static int selected_video_option;
static double video_options[]={1.0,100.0,0.0,0.0,48.0,0.0,2.0};
static const char *video_props[]={"speed","volume","brightness","contrast",
                                  "sub-font-size","sub-delay","hls-bitrate"};
static const char *video_titles[]={"Швидкість","Гучність","Яскравість","Контраст",
                                   "Розмір субтитрів","Затримка субтитрів","Якість HLS"};
static const double video_steps[]={0.25,5,5,5,4,0.25,1};
static const double video_min[]={0.5,0,-100,-100,20,-10,0};
static const double video_max[]={2.0,130,100,100,100,10,4};
static const char *hls_presets[]={"max","min","1000000","2000000","4000000"};
static const char *hls_labels[]={"Авто / найвища","Економія","До 1 Мбіт/с",
                                 "До 2 Мбіт/с","До 4 Мбіт/с"};
#define VIDEO_OPTION_COUNT 7

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
static int expanded_catalog=0;

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
    GLint tint, shape;
    TTF_Font *font;
    TTF_Font *scripts[7];
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
        if(idx>=0 && p->scripts[idx])return p->scripts[idx];
    }
    return p->font;
}

static const char *vs = "attribute vec2 pos;attribute vec2 uv;varying vec2 tc;"
                        "void main(){gl_Position=vec4(pos,0.,1.);tc=uv;}";
static const char *fs = "precision mediump float;varying vec2 tc;"
                        "uniform vec4 tint;uniform sampler2D image;uniform vec3 shape;"
                        "void main(){vec2 p=tc*shape.xy;vec2 q=abs(p-shape.xy*.5)"
                        "-shape.xy*.5+shape.z;float d=length(max(q,0.0))"
                        "+min(max(q.x,q.y),0.0)-shape.z;"
                        "float a=1.0-smoothstep(-1.0,1.5,d);"
                        "gl_FragColor=texture2D(image,tc)*tint*a;}";
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
    for(int i=0;i<7;i++){
        char path[96];snprintf(path,sizeof(path),"romfs:/NotoSans%s.ttf",script_fonts[i]);
        p->scripts[i]=TTF_OpenFont(path,25);
    }
    return p->font != NULL;
}
static void box_alpha(Paint *p,int x,int y,int w,int h,float r,float g,float b,float a,int radius) {
    float x0=x/640.f-1.f,x1=(x+w)/640.f-1.f;
    float y0=1.f-y/360.f,y1=1.f-(y+h)/360.f;
    float vertices[]={x0,y0,0,0, x1,y0,1,0, x0,y1,0,1,
                      x1,y0,1,0, x1,y1,1,1, x0,y1,0,1};
    glUseProgram(p->program);
    glUniform4f(p->tint,r,g,b,a);
    glUniform3f(p->shape,(float)w,(float)h,(float)radius);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D,p->tex);
    glBindBuffer(GL_ARRAY_BUFFER,p->vbo);
    glBufferData(GL_ARRAY_BUFFER,sizeof(vertices),vertices,GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);glEnableVertexAttribArray(1);
    glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,4*sizeof(float),(void*)0);
    glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,4*sizeof(float),(void*)(2*sizeof(float)));
    glDrawArrays(GL_TRIANGLES,0,6);
}
static void box(Paint *p,int x,int y,int w,int h,float r,float g,float b) {
    box_alpha(p,x,y,w,h,r,g,b,1.f,w>100 && h>40?14:5);
}
static void text_color(Paint *p,const char *str,int x,int y,int limit,
                       float r,float g,float b) {
    if (!str || !*str) return;
    SDL_Color color={255,255,255,255};
    SDL_Surface *surface=TTF_RenderUTF8_Blended(choose_font(p,str),str,color);
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
        /* Never shrink a long title: clip it at the card edge instead. */
        if(limit>0){
            glEnable(GL_SCISSOR_TEST);
            glScissor(x,720-y-rgba->h,limit,rgba->h);
        }
        box_alpha(p,x,y,rgba->w,rgba->h,r,g,b,1.f,0);
        if(limit>0)glDisable(GL_SCISSOR_TEST);
        p->tex=previous;glDeleteTextures(1,&tex);
        SDL_FreeSurface(rgba);
    }
    SDL_FreeSurface(surface);
}
static void text(Paint *p,const char *str,int x,int y,int limit){
    text_color(p,str,x,y,limit,1,1,1);
}
static void card_title(Paint *p,const char *title,int x,int y,int width){
    if(!title || !*title)return;
    char first[192]={0},second[192]={0};
    snprintf(first,sizeof(first),"%s",title);
    int w=0,h=0;
    TTF_Font *font=choose_font(p,first);
    if(TTF_SizeUTF8(font,first,&w,&h)==0 && w>width){
        int break_at=-1;
        for(int i=1;first[i];i++)if(first[i]==' '){
            char saved=first[i];first[i]=0;
            int test=0;
            TTF_SizeUTF8(font,first,&test,&h);
            first[i]=saved;
            if(test<=width)break_at=i;
        }
        if(break_at>0){
            snprintf(second,sizeof(second),"%s",first+break_at+1);
            first[break_at]=0;
        }
    }
    text(p,first,x,y,width);
    if(second[0])text(p,second,x,y+30,width);
}
static int footer_position,footer_total;
static void footer(Paint *p,const char *message) {
    box_alpha(p,0,663,1280,57,.045f,.055f,.07f,.99f,0);
    box_alpha(p,30,668,1220,2,.89f,.68f,.34f,1.f,0);
    char position[30];
    snprintf(position,sizeof(position),"%d / %d",footer_position,footer_total);
    text_color(p,footer_total?position:"UAPLAY",76,682,140,.91f,.71f,.38f);
    const char *keys[]={"L","R","Y","X","A","B"};
    const char *labels[]={"Розділи","Розділи","Меню","Опції","Вибрати","Назад"};
    int at[]={232,387,548,708,872,1040};
    for(int i=0;i<6;i++){
        int x=at[i];
        box_alpha(p,x,684,32,26,.91f,.71f,.38f,1.f,i>=2?14:5);
        text_color(p,keys[i],x+7,680,27,.10f,.11f,.13f);
        text_color(p,labels[i],x+37,681,125,.91f,.71f,.38f);
    }
    /* Errors stay readable without replacing the controller legend. */
    if(message && *message && strncmp(message,"A:",2) &&
       strncmp(message,"B:",2) && strncmp(message,"Вліво",10)){
        box_alpha(p,25,620,1230,38,.10f,.13f,.17f,.96f,8);
        text(p,message,42,621,1195);
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
}
static void draw(Paint *p,Page page,Movie *movies,int n,int chosen,
                 Source *sources,int sn,int source,const char *heading,const char *status) {
    glViewport(0,0,1280,720);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
    box(p,0,0,1280,720,.035f,.044f,.068f);
    box_alpha(p,0,0,1280,8,.18f,.72f,.86f,1.f,0);
    footer_position=page==CATALOG?chosen+1:0;
    footer_total=page==CATALOG?n:0;
    int poster_budget=1;
    if (page==CATALOG || page==SETTINGS) {
        box_alpha(p,0,0,1280,89,.095f,.105f,.115f,.97f,0);
        text(p,"‹",24,18,50);
        text(p,"☰",76,18,55);
        text(p,"Головна  -  TMDB",145,18,510);
        text(p,"⌕",885,18,55);
        text(p,"⚙",1030,18,50);
        time_t now=time(NULL);
        struct tm *local=localtime(&now);
        char clock_label[32]="--:--";
        if(local)strftime(clock_label,sizeof(clock_label),"%H:%M",local);
        text(p,clock_label,1138,18,110);
        text(p,heading,28,115,1020);
        box_alpha(p,1110,112,137,48,.08f,.09f,.11f,1.f,23);
        text(p,expanded_catalog && page==CATALOG?"Назад":"Ще  ›",1129,121,114);
        if(expanded_catalog && page==CATALOG){
            int start=chosen/12*12;
            for(int i=start;i<n && i<start+12;i++){
                int k=i-start,x=29+k%6*208,y=170+k/6*244;
                if(i==chosen)box_alpha(p,x-4,y-4,199,204,.94f,.95f,.97f,1.f,17);
                poster(p,&movies[i],x,y,191,196,&poster_budget);
                card_title(p,movies[i].title,x,y+199,190);
            }
        }else{
            int start=chosen/6*6;
            for(int i=start;i<n && i<start+6;i++){
                int k=i-start,x=29+k*208,y=179;
                if(i==chosen)box_alpha(p,x-4,y-4,199,309,.94f,.95f,.97f,1.f,17);
                poster(p,&movies[i],x,y,191,301,&poster_budget);
                box_alpha(p,x,y+268,191,33,.02f,.04f,.06f,.76f,10);
                card_title(p,movies[i].title,x,y+315,190);
                text(p,movies[i].year,x,y+375,190);
            }
            text(p,"Інші у категорії",29,585,800);
            for(int i=start+6;i<n && i<start+12;i++){
                int k=i-start-6;
                poster(p,&movies[i],29+k*208,626,191,301,&poster_budget);
            }
        }
        if(page==SETTINGS){
            box_alpha(p,0,0,1280,720,0,0,0,.49f,0);
            box_alpha(p,734,0,546,720,.15f,.16f,.17f,1.f,0);
            text(p,"Налаштування",774,20,480);
            const char *items[]={"▣  Інтерфейс","☷  Каталог","▶  Плеєр",
                                 "◎  Джерела UAFresh","◉  UAFlix — акаунт"};
            for(int i=0;i<5;i++){
                int y=112+i*82;
                box_alpha(p,767,y,477,68,i==4?.22f:.15f,i==4?.30f:.16f,
                          i==4?.34f:.17f,.99f,13);
                text(p,items[i],789,y+16,440);
            }
            text(p,uaflix_logged_in()?"UAFlix: вхід виконано":"UAFlix: вхід необов'язковий",773,524,465);
            box_alpha(p,770,566,229,62,.21f,.49f,.60f,1.f,16);
            box_alpha(p,1009,566,235,62,.22f,.24f,.27f,1.f,16);
            text(p,"A  Увійти",788,579,200);
            text(p,"Y  Вийти",1032,579,195);
        }
        footer(p,*status?status:(page==SETTINGS?
                "A: Вхід UAFlix   Y: Вихід   B: Закрити":
                "A: Деталі   X: Пошук   L/R: Розділи   -: Меню"));
    } else if (page==DETAILS) {
        box_alpha(p,25,16,160,43,.13f,.19f,.26f,.92f,16);
        text(p,"‹  Назад",42,20,150);
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
        box_alpha(p,340,591,340,60,.19f,.55f,.66f,.97f,20);
        text(p,"▶  Дивитися",370,605,290);
        box_alpha(p,705,591,250,60,.13f,.21f,.31f,.97f,20);
        text(p,"♡  В обране",732,605,220);
        footer(p,*status?status:"A: Джерела відео   X: Обране   Y: Переглянуто   B: Назад");
    } else if (page==SOURCES) {
        text(p,"‹",18,45,42);
        text(p,"UAKino / UAFresh — джерела",40,45,1100);
        int first=source/10*10;
        for (int i=first;i<sn && i<first+10;i++) {
            int y=110+(i-first)*51;
            box(p,35,y,1200,46,i==source?.22f:.11f,i==source?.42f:.14f,i==source?.51f:.19f);
            text(p,sources[i].title,50,y+8,1160);
        }
        footer(p,*status?status:"A: Відкрити відео    B: Назад");
    } else if (page==SEASONS) {
        text(p,"‹",18,45,42);
        text(p,"UAPLAY — вибір сезону",42,45,1120);
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
        text(p,"‹",18,45,42);
        char heading[128];
        snprintf(heading,sizeof(heading),"UAPLAY — сезон %d: вибір серії",season_ids[selected_season]);
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
        text(p,subtitle_picker?"UAPLAY — субтитри":"UAPLAY — аудіодоріжки",45,50,1110);
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
        text(p,"UAPLAY — параметри відео",45,50,1140);
        for(int i=0;i<VIDEO_OPTION_COUNT;i++) {
            int y=130+i*70;
            box(p,40,y,1180,62,i==selected_video_option?.22f:.10f,
                i==selected_video_option?.40f:.14f,i==selected_video_option?.49f:.19f);
            char label[256];
            if(i==6)snprintf(label,sizeof(label),"%s: %s",video_titles[i],
                              hls_labels[(int)video_options[i]]);
            else snprintf(label,sizeof(label),"%s: %.2f",video_titles[i],video_options[i]);
            text(p,label,56,y+16,1130);
        }
        footer(p,"Вліво / вправо: змінити параметр   B: Назад");
    } else if(page==LOGIN_USER || page==LOGIN_PASSWORD){
        int password=page==LOGIN_PASSWORD;
        text(p,password?"UAPLAY — пароль UAFlix":"UAPLAY — вхід до UAFlix",65,42,1110);
        box_alpha(p,65,109,1150,73,.13f,.17f,.23f,1.f,14);
        char display[140]={0};
        if(password){
            size_t len=strlen(login_password);
            if(len>sizeof(display)-1)len=sizeof(display)-1;
            memset(display,'*',len);
        }else snprintf(display,sizeof(display),"%s",login_user);
        text(p,*display?display:(password?"Введіть пароль":"Введіть логін або email"),85,128,1090);
        int count=(int)strlen(login_keys);
        for(int i=0;i<count;i++){
            int x=70+i%12*94,y=210+i/12*65;
            box_alpha(p,x,y,84,54,i==login_selected?.24f:.12f,
                      i==login_selected?.46f:.16f,i==login_selected?.54f:.22f,1.f,11);
            char letter[2]={login_keys[i],0};
            if(login_upper && letter[0]>='a' && letter[0]<='z')letter[0]-=32;
            text(p,letter,x+30,y+12,55);
        }
        box_alpha(p,70,557,1140,80,.11f,.16f,.22f,.96f,14);
        text(p,password?"Y  Увійти":"Y  Далі",91,580,230);
        text(p,"B  Стерти",334,580,215);
        text(p,"X  Регістр",568,580,240);
        text(p,"+  Скасувати",858,580,315);
        footer(p,"Введення без системної клавіатури — безпечний режим");
    }
}
static void command(mpv_handle *player,const char *verb,const char *arg) {
    const char *args[]={verb,arg,NULL};
    if(player) mpv_command_async(player,0,args);
}
static void apply_hls_quality(mpv_handle *player) {
    if(!player)return;
    const char *args[]={"set","hls-bitrate",hls_presets[(int)video_options[6]],NULL};
    mpv_command_async(player,0,args);
}
static void seek(mpv_handle *player,const char *seconds) {
    const char *args[]={"seek",seconds,"relative",NULL};
    if(player) mpv_command_async(player,0,args);
}
static void draw_player_controls(Paint *p,mpv_handle *player) {
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
    box_alpha(p,31,29,172,49,.09f,.16f,.25f,.82f,13);
    text(p,"‹  Назад",49,34,145);
    box_alpha(p,160,493,960,166,.09f,.18f,.30f,.86f,20);
    text(p,"◖",181,513,36);
    box_alpha(p,222,534,131,6,.36f,.51f,.66f,1.f,3);
    int volume=(int)(video_options[1]*130.0/130.0);
    if(volume<0)volume=0;if(volume>130)volume=130;
    if(volume>0)box_alpha(p,222,534,volume,6,.85f,.91f,.97f,1.f,3);
    box_alpha(p,222+volume-7,527,14,20,.94f,.96f,1.f,1.f,7);
    text(p,"◀◀",418,505,87);
    text(p,paused?"▶":"Ⅱ",599,504,60);
    text(p,"▶▶",750,505,87);
    text(p,"⚙",921,505,50);
    text(p,"♫",990,505,46);
    text(p,"☷",1062,505,46);
    box_alpha(p,235,614,810,7,.36f,.51f,.66f,1.f,4);
    int filled=duration>0?(int)(810.0*pos/duration):0;
    if(filled<0)filled=0;if(filled>810)filled=810;
    if(filled>0)box_alpha(p,235,614,filled,7,.85f,.91f,.97f,1.f,4);
    box_alpha(p,235+filled-8,607,16,21,.94f,.96f,1.f,1.f,8);
    char left[24],right[24];
    snprintf(left,sizeof(left),"%02d:%02d",(int)pos/60,(int)pos%60);
    snprintf(right,sizeof(right),"%02d:%02d",(int)duration/60,(int)duration%60);
    text(p,left,174,601,90);
    text(p,right,1050,601,85);
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
    Result network_result = socketInitializeDefault();
    if (R_SUCCEEDED(network_result)) curl_global_init(CURL_GLOBAL_DEFAULT);
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS,"1");
    if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMECONTROLLER)) return 1;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,0);
    SDL_Window *window=SDL_CreateWindow("UAPlay",0,0,1280,720,SDL_WINDOW_OPENGL|SDL_WINDOW_SHOWN);
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
        mpv_set_option_string(player,"network-timeout","20");
        mpv_set_option_string(player,"sub-visibility","yes");
        mpv_set_option_string(player,"sub-auto","fuzzy");
        mpv_set_option_string(player,"cache","yes");
        mpv_set_option_string(player,"demuxer-max-bytes","16MiB");
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
    Movie movies[CATALOG_MAX]={0};
    Source sources[UAKINO_MAX]={0};
    int n=0,sn=0,chosen=0,source=0,category=0,running=1;
    Page page=CATALOG;
    char query[192]={0},message[256]={0};
    n=load_category(category,query,movies,message,sizeof(message));
    Movie playing_movie={0};
    double resume_at=0;
    char pending_subtitle[1024]={0};
    char playing_url[2048]={0};
    Page player_return_page=SOURCES;
    Uint32 video_started_at=0;
    int video_loaded=0;
    Uint32 controls_until=0;
    Uint32 stick_last_time=0;
    int stick_last_key=0;
    while(running){
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
                if(page==LOGIN_USER || page==LOGIN_PASSWORD){
                    if(tx>=70 && tx<1198 && ty>=210 && ty<535){
                        int col=(tx-70)/94,row=(ty-210)/65;
                        int pick=row*12+col;
                        if(col<12 && pick<(int)strlen(login_keys)){
                            login_selected=pick;key=SDL_CONTROLLER_BUTTON_A;
                        }
                    }else if(ty>=557 && ty<641){
                        if(tx<313)key=SDL_CONTROLLER_BUTTON_Y;
                        else if(tx<550)key=SDL_CONTROLLER_BUTTON_B;
                        else if(tx<830)key=SDL_CONTROLLER_BUTTON_X;
                        else key=SDL_CONTROLLER_BUTTON_START;
                    }
                }else if(page==PLAYER){
                    int was_visible=SDL_GetTicks()<controls_until;
                    controls_until=SDL_GetTicks()+3000;
                    if(!was_visible)continue;
                    if(ty<105 && tx<310)key=SDL_CONTROLLER_BUTTON_B;
                    else if(ty>=596 && ty<=649 && tx>=224 && tx<=1054){
                        double duration=0;
                        if(player && mpv_get_property(player,"duration",MPV_FORMAT_DOUBLE,&duration)>=0
                           && duration>0){
                            double to=duration*(tx-235)/810.0;
                            if(to<0)to=0;if(to>duration)to=duration;
                            char at[48];snprintf(at,sizeof(at),"%.2f",to);
                            const char *args[]={"seek",at,"absolute",NULL};
                            mpv_command_async(player,0,args);
                        }
                        continue;
                    }else if(ty>=495 && ty<579){
                        if(tx>=214 && tx<367){
                            double volume=(tx-222)*130.0/130.0;
                            if(volume<0)volume=0;if(volume>130)volume=130;
                            video_options[1]=volume;
                            playback_set_value(player,"volume",volume);
                            continue;
                        }
                        if(tx>=402 && tx<525)key=SDL_CONTROLLER_BUTTON_DPAD_LEFT;
                        else if(tx>=565 && tx<698)key=SDL_CONTROLLER_BUTTON_A;
                        else if(tx>=735 && tx<854)key=SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
                        else if(tx>=907 && tx<977)key=SDL_CONTROLLER_BUTTON_BACK;
                        else if(tx>=977 && tx<1043)key=SDL_CONTROLLER_BUTTON_Y;
                        else if(tx>=1043 && tx<1112)key=SDL_CONTROLLER_BUTTON_X;
                    }else continue;
                }else if(page==CATALOG){
                    if(ty>=108 && ty<168 && tx>=1080){
                        expanded_catalog=!expanded_catalog;
                        chosen=0;continue;
                    }
                    if(expanded_catalog && ty>=170 && ty<650 && tx>=29){
                        int col=(tx-29)/208,row=(ty-170)/244;
                        int new_chosen=chosen/12*12+row*6+col;
                        if(col<6 && row<2 && new_chosen<n){
                            chosen=new_chosen;key=SDL_CONTROLLER_BUTTON_A;
                        }
                    }else if(!expanded_catalog && ty>=179 && ty<550 && tx>=29){
                        int col=(tx-29)/208;
                        int new_chosen=chosen/6*6+col;
                        if(col<6 && new_chosen<n){
                            chosen=new_chosen;key=SDL_CONTROLLER_BUTTON_A;
                        }
                    }else if(ty<90 && tx>980 && tx<1090)key=SDL_CONTROLLER_BUTTON_BACK;
                    else if(ty<90 && tx>=860 && tx<970)key=SDL_CONTROLLER_BUTTON_X;
                    else if(ty<90 && tx<130)key=SDL_CONTROLLER_BUTTON_BACK;
                    else if(ty>665 && tx>800)key=SDL_CONTROLLER_BUTTON_BACK;
                }else if(page==SOURCES){
                    int slot=(ty-110)/51,new_source=source/10*10+slot;
                    if(ty>=110 && slot>=0 && slot<10 && new_source<sn){
                        source=new_source;key=SDL_CONTROLLER_BUTTON_A;
                    }else if(ty<100)key=SDL_CONTROLLER_BUTTON_B;
                }else if(page==SEASONS){
                    int slot=(ty-125)/48;
                    if(ty>=125 && slot>=0 && slot<season_count && slot<10){
                        selected_season=slot;key=SDL_CONTROLLER_BUTTON_A;
                    }else if(ty<100)key=SDL_CONTROLLER_BUTTON_B;
                }else if(page==EPISODES){
                    int slot=(ty-118)/51;
                    int episode=selected_episode/10*10+slot;
                    if(ty>=118 && slot>=0 && slot<10 && selected_season<season_count &&
                       episode<episodes_in_season(season_ids[selected_season])){
                        selected_episode=episode;key=SDL_CONTROLLER_BUTTON_A;
                    }else if(ty<100)key=SDL_CONTROLLER_BUTTON_B;
                }else if(page==DETAILS){
                    if(ty<68 && tx<220)key=SDL_CONTROLLER_BUTTON_B;
                    else if(ty>=591 && ty<655 && tx>=705 && tx<960)
                        key=SDL_CONTROLLER_BUTTON_X;
                    else if(ty>=591 && ty<655 && tx>=340 && tx<680)
                        key=SDL_CONTROLLER_BUTTON_A;
                    else if(ty>=665)key=SDL_CONTROLLER_BUTTON_B;
                }else if(page==TRACK_PICKER){
                    int slot=(ty-125)/50,new_track=selected_track/10*10+slot;
                    if(ty>=125 && slot>=0 && slot<10 && new_track<track_count){
                        selected_track=new_track;key=SDL_CONTROLLER_BUTTON_A;
                    }else if(ty<100)key=SDL_CONTROLLER_BUTTON_B;
                }else if(page==VIDEO_SETTINGS){
                    int slot=(ty-130)/70;
                    if(ty>=130 && slot>=0 && slot<VIDEO_OPTION_COUNT){
                        selected_video_option=slot;
                        key=tx<640?SDL_CONTROLLER_BUTTON_DPAD_LEFT:SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
                    }else if(ty<100)key=SDL_CONTROLLER_BUTTON_B;
                }else if(page==SETTINGS){
                    if(ty>566 && ty<632 && tx>770 && tx<1009)key=SDL_CONTROLLER_BUTTON_A;
                    else if(ty>566 && ty<632 && tx>=1009)key=SDL_CONTROLLER_BUTTON_Y;
                    else if(tx<734 || ty<92 || ty>665)key=SDL_CONTROLLER_BUTTON_B;
                }
            }
            if(page==PLAYER && key>=0)controls_until=SDL_GetTicks()+3000;
            if(key<0)continue;
            if(page==LOGIN_USER || page==LOGIN_PASSWORD){
                if(key==SDL_CONTROLLER_BUTTON_START || key==SDL_CONTROLLER_BUTTON_BACK){
                    memset(login_password,0,sizeof(login_password));page=SETTINGS;continue;
                }
                if(key==SDL_CONTROLLER_BUTTON_DPAD_LEFT && login_selected>0)login_selected--;
                if(key==SDL_CONTROLLER_BUTTON_DPAD_RIGHT && login_selected<(int)strlen(login_keys)-1)login_selected++;
                if(key==SDL_CONTROLLER_BUTTON_DPAD_UP && login_selected>=12)login_selected-=12;
                if(key==SDL_CONTROLLER_BUTTON_DPAD_DOWN && login_selected+12<(int)strlen(login_keys))login_selected+=12;
                if(key==SDL_CONTROLLER_BUTTON_X)login_upper=!login_upper;
                char *field=page==LOGIN_USER?login_user:login_password;
                if(key==SDL_CONTROLLER_BUTTON_B){
                    size_t length=strlen(field);if(length)field[length-1]=0;
                }
                if(key==SDL_CONTROLLER_BUTTON_A){
                    size_t length=strlen(field);
                    if(length<sizeof(login_user)-1){
                        char c=login_keys[login_selected];
                        if(login_upper && c>='a' && c<='z')c-=32;
                        field[length]=c;field[length+1]=0;
                    }
                }
                if(key==SDL_CONTROLLER_BUTTON_Y && *field){
                    if(page==LOGIN_USER){page=LOGIN_PASSWORD;login_selected=0;}
                    else{
                        uaflix_login(login_user,login_password,message,sizeof(message));
                        memset(login_password,0,sizeof(login_password));
                        page=SETTINGS;
                    }
                }
                continue;
            }
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
                    if(playback_select_track(player,subtitle_picker,tracks[selected_track].id)){
                        snprintf(message,sizeof(message),"%s: вибрано %s",
                                 subtitle_picker?"Субтитри":"Аудіо",
                                 tracks[selected_track].label);
                        page=PLAYER;
                    }else snprintf(message,sizeof(message),"Не вдалося перемкнути доріжку");
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
                    if(i==6){
                        apply_hls_quality(player);
                        if(playing_url[0]){
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
                            snprintf(pending_subtitle,sizeof(pending_subtitle),"%s",uakino_resolved_subtitle());
                            playing_movie=movies[chosen];
                            resume_at=0;
                            library_mark_watched(&playing_movie);
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
            if(page==SETTINGS){
                if(key==SDL_CONTROLLER_BUTTON_B || key==SDL_CONTROLLER_BUTTON_BACK)page=CATALOG;
                if(key==SDL_CONTROLLER_BUTTON_A){
                    memset(login_user,0,sizeof(login_user));
                    memset(login_password,0,sizeof(login_password));
                    login_selected=login_upper=0;
                    page=LOGIN_USER;
                }
                if(key==SDL_CONTROLLER_BUTTON_Y){
                    uaflix_logout();
                    snprintf(message,sizeof(message),"UAFlix: вихід виконано");
                }
                continue;
            }
            if(key==SDL_CONTROLLER_BUTTON_START){running=0;continue;}
            if(page==CATALOG){
                if(key==SDL_CONTROLLER_BUTTON_BACK){page=SETTINGS;continue;}
                if(key==SDL_CONTROLLER_BUTTON_RIGHTSTICK){
                    expanded_catalog=!expanded_catalog;chosen=0;continue;
                }
                if(key==SDL_CONTROLLER_BUTTON_B && expanded_catalog){
                    expanded_catalog=0;chosen=0;continue;
                }
                if(key==SDL_CONTROLLER_BUTTON_DPAD_LEFT && chosen>0)chosen--;
                if(key==SDL_CONTROLLER_BUTTON_DPAD_RIGHT && chosen<n-1)chosen++;
                if(key==SDL_CONTROLLER_BUTTON_DPAD_UP && chosen>=6)chosen-=6;
                if(key==SDL_CONTROLLER_BUTTON_DPAD_DOWN && chosen+6<n)chosen+=6;
                if(key==SDL_CONTROLLER_BUTTON_A && n){message[0]=0;page=DETAILS;}
                if(key==SDL_CONTROLLER_BUTTON_LEFTSHOULDER || key==SDL_CONTROLLER_BUTTON_RIGHTSHOULDER){
                    category=(category+(key==SDL_CONTROLLER_BUTTON_RIGHTSHOULDER?1:CATEGORY_COUNT-1))%CATEGORY_COUNT;
                    expanded_catalog=0;
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
                    if(!render){
                        snprintf(message,sizeof(message),"Плеєр mpv не ініціалізовано");
                    }else if(uakino_resolve(&sources[source],url,sizeof(url),message,sizeof(message))){
                        snprintf(pending_subtitle,sizeof(pending_subtitle),"%s",uakino_resolved_subtitle());
                        playing_movie=movies[chosen];
                        resume_at=library_progress(&playing_movie);
                        library_mark_watched(&playing_movie);
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
                if(event->event_id==MPV_EVENT_FILE_LOADED && pending_subtitle[0]){
                    const char *args[]={"sub-add",pending_subtitle,"auto","Українські", "uk",NULL};
                    mpv_command_async(player,0,args);
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
                    }
                }
                if(event->event_id==MPV_EVENT_LOG_MESSAGE){
                    mpv_event_log_message *log=(mpv_event_log_message*)event->data;
                    if(log && log->text) fprintf(stderr,"mpv [%s]: %s",log->prefix,log->text);
                }
            }
        }
        if(page==PLAYER && render){
            mpv_opengl_fbo fbo={.fbo=0,.w=1280,.h=720};int flip=1;
            mpv_render_param frame[]={{MPV_RENDER_PARAM_OPENGL_FBO,&fbo},
                                      {MPV_RENDER_PARAM_FLIP_Y,&flip},
                                      {MPV_RENDER_PARAM_INVALID,NULL}};
            int render_result=mpv_render_context_render(render,frame);
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
            if(video_loaded && SDL_GetTicks()<controls_until)draw_player_controls(&paint,player);
        }else draw(&paint,page,movies,n,chosen,sources,sn,source,headings[category],message);
        SDL_GL_SwapWindow(window);SDL_Delay(16);
    }
    if(render)mpv_render_context_free(render);
    if(player)mpv_terminate_destroy(player);
    if(pad)SDL_GameControllerClose(pad);
    posters_destroy();
    for(int i=0;i<7;i++)if(paint.scripts[i])TTF_CloseFont(paint.scripts[i]);
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
