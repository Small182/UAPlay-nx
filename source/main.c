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
               SEASONS, EPISODES, TRACK_PICKER, VIDEO_SETTINGS } Page;

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
static int settings_selected=0,ui_accent_alt=0,source_mode=0;
static int video_settings_from_menu=0;
static int ui_category=0;
static void prioritize_sources(Source *items,int count){
    if(source_mode==0)return;
    for(int i=0;i<count;i++)for(int j=i+1;j<count;j++){
        int a=(source_mode==1?strstr(items[i].title,"UAFlix")!=NULL:
                               strstr(items[i].title,"UASerials")!=NULL);
        int b=(source_mode==1?strstr(items[j].title,"UAFlix")!=NULL:
                               strstr(items[j].title,"UASerials")!=NULL);
        if(b&&!a){Source temp=items[i];items[i]=items[j];items[j]=temp;}
    }
}

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
    TTF_Font *font_big;
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
    p->font_big=TTF_OpenFont("romfs:/DejaVuSans.ttf",37);
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
static void headline(Paint *p,const char *str,int x,int y,int limit){
    TTF_Font *normal=p->font;
    if(p->font_big)p->font=p->font_big;
    text(p,str,x,y,limit);
    p->font=normal;
}
static void summary(Paint *p,const char *description,int x,int y,int width){
    char line[220]={0};size_t take=0;
    const char *s=description;
    for(int row=0;row<3 && s && *s;row++){
        size_t n=strlen(s);if(n>75)n=75;
        if(s[n] && n>30){while(n>30 && s[n]!=' ')n--;}
        if(n>=sizeof(line))n=sizeof(line)-1;
        memcpy(line,s,n);line[n]=0;
        text_color(p,line,x,y+row*32,width,.78f,.80f,.84f);
        s+=n;while(*s==' ')s++;
        take+=n;if(take>210)break;
    }
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
static Page footer_page;
static void footer(Paint *p,const char *message) {
    box_alpha(p,0,663,1280,57,.045f,.055f,.07f,.99f,0);
    float ar=ui_accent_alt?.39f:.89f,ag=ui_accent_alt?.74f:.68f,
          ab=ui_accent_alt?.87f:.34f;
    box_alpha(p,30,668,1220,2,ar,ag,ab,1.f,0);
    char position[30];
    snprintf(position,sizeof(position),"%d / %d",footer_position,footer_total);
    text_color(p,footer_total?position:"UAPLAY",62,682,125,ar,ag,ab);
    const char *keys[6],*labels[6];int count=0;
#define HINT(k,l) do{keys[count]=k;labels[count]=l;count++;}while(0)
    if(footer_page==CATALOG){
        HINT("L","Попередній");HINT("R","Наступний");
        HINT("X","Пошук");HINT("-","Налаштування");HINT("A","Відкрити");
        HINT("B",expanded_catalog?"Згорнути":"Назад");
    }else if(footer_page==SETTINGS){
        HINT("↑↓","Пункт");HINT("A","Змінити");HINT("B","Закрити");
    }else if(footer_page==TRACK_PICKER){
        HINT("↑↓","Доріжка");HINT("A","Застосувати");HINT("B","Назад");
    }else if(footer_page==VIDEO_SETTINGS){
        HINT("↑↓","Параметр");HINT("←→","Змінити");HINT("B","Назад");
    }else{
        HINT("↑↓","Вибір");HINT("A","Відкрити");HINT("B","Назад");
    }
#undef HINT
    int start=count>=6?170:270;
    int step=count>=6?178:235;
    for(int i=0;i<count;i++){
        int x=start+i*step;
        box_alpha(p,x,685,36,26,ar,ag,ab,1.f,10);
        text_color(p,keys[i],x+5,681,31,.10f,.11f,.13f);
        text_color(p,labels[i],x+43,681,step-48,ar,ag,ab);
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
    box_alpha(p,0,0,1280,5,ui_accent_alt?.18f:.81f,
              ui_accent_alt?.72f:.64f,ui_accent_alt?.86f:.37f,1.f,0);
    footer_position=page==CATALOG?chosen+1:0;
    footer_total=page==CATALOG?n:0;
    footer_page=page;
    int poster_budget=1;
    if (page==CATALOG || page==SETTINGS) {
        box_alpha(p,0,0,1280,78,.052f,.058f,.070f,.99f,0);
        headline(p,"UAPlay",25,15,160);
        const char *tabs[]={"Головна","Серіали","Фільми","Моє"};
        for(int i=0;i<4;i++){
            int x=225+i*131;
            if((i==0 && ui_category!=1 && ui_category<11) || (i==1 && ui_category==1) ||
               (i==2 && ui_category==0) || (i==3 && ui_category>=11))
                box_alpha(p,x-11,15,125,46,.18f,.21f,.25f,1.f,22);
            text(p,tabs[i],x,25,116);
        }
        box_alpha(p,764,16,282,49,.15f,.17f,.21f,1.f,17);
        /* Draw the search glyph in geometry: the Switch font lacks U+2315. */
        box_alpha(p,784,25,25,25,.91f,.93f,.96f,1.f,13);
        box_alpha(p,789,30,15,15,.15f,.17f,.21f,1.f,8);
        box_alpha(p,806,47,6,6,.91f,.93f,.96f,1.f,2);
        box_alpha(p,810,51,6,6,.91f,.93f,.96f,1.f,2);
        text(p,"Пошук",828,25,175);
        box_alpha(p,1060,16,56,49,.15f,.17f,.21f,1.f,17);
        text(p,"⚙",1073,23,44);
        time_t now=time(NULL);
        struct tm *local=localtime(&now);
        char clock_label[32]="--:--";
        if(local)strftime(clock_label,sizeof(clock_label),"%H:%M",local);
        text(p,clock_label,1154,24,104);
        if(expanded_catalog && page==CATALOG){
            headline(p,heading,32,90,850);
            box_alpha(p,1115,94,132,46,.15f,.18f,.22f,1.f,22);
            text(p,"‹  Назад",1130,102,106);
            int start=chosen/12*12;
            for(int i=start;i<n && i<start+12;i++){
                int k=i-start,x=44+k%6*201,y=150+k/6*244;
                if(i==chosen)box_alpha(p,x-5,y-5,175,246,.92f,.93f,.95f,1.f,17);
                poster(p,&movies[i],x,y,165,236,&poster_budget);
            }
        }else{
            int active=chosen<n?chosen:0;
            box_alpha(p,27,91,1226,243,.085f,.103f,.13f,1.f,23);
            if(n){
                poster(p,&movies[active],1039,97,169,231,&poster_budget);
                headline(p,movies[active].title,57,109,875);
                char tag[110];
                snprintf(tag,sizeof(tag),"%s  ·  %s  ·  TMDB",movies[active].year,
                         movies[active].series?"Серіал":"Фільм");
                text_color(p,tag,59,158,640,.72f,.75f,.79f);
                summary(p,movies[active].overview,58,199,890);
            }
            box_alpha(p,56,291,175,39,.88f,.89f,.91f,1.f,17);
            text_color(p,"▶  Дивитися",71,294,152,.09f,.10f,.12f);
            box_alpha(p,245,291,179,39,.25f,.27f,.31f,1.f,17);
            text(p,"＋  Моє",272,294,137);
            headline(p,heading,32,345,920);
            box_alpha(p,1103,346,146,43,.15f,.18f,.22f,1.f,21);
            text(p,"Усі  ›",1137,353,95);
            int start=chosen/6*6;
            for(int i=start;i<n && i<start+6;i++){
                int k=i-start,x=44+k*201,y=403;
                if(i==chosen)box_alpha(p,x-5,y-5,175,246,.92f,.93f,.95f,1.f,17);
                poster(p,&movies[i],x,y,165,236,&poster_budget);
            }
        }
        if(page==SETTINGS){
            box_alpha(p,0,0,1280,720,.055f,.063f,.076f,1.f,0);
            headline(p,"Налаштування UAPlay",45,35,950);
            const char *items[]={"Інтерфейс","Каталог","Плеєр","Джерела"};
            for(int i=0;i<4;i++){
                int y=141+i*87;
                box_alpha(p,45,y,441,68,i==settings_selected?.30f:.12f,
                          i==settings_selected?.34f:.14f,
                          i==settings_selected?.39f:.18f,.99f,13);
                text(p,items[i],70,y+18,385);
            }
            char description[240];
            if(settings_selected==0)snprintf(description,sizeof(description),"Акцент: %s",ui_accent_alt?"Блакитний":"Золотистий");
            else if(settings_selected==1)snprintf(description,sizeof(description),"Поточний розділ: %s",heading);
            else if(settings_selected==2)snprintf(description,sizeof(description),"Швидкість: %.2f · гучність: %.0f",video_options[0],video_options[1]);
            else snprintf(description,sizeof(description),"Порядок: %s",source_mode==0?"Усі":source_mode==1?"UAFlix спочатку":"UASerials спочатку");
            box_alpha(p,518,141,720,485,.105f,.12f,.15f,1.f,19);
            headline(p,items[settings_selected],555,178,635);
            text(p,description,555,254,640);
            const char *help[]={"Перемикання кольору виділення у всьому застосунку.",
                "Виберіть розділ фільмів і серіалів.",
                "Швидкість, гучність, якість та параметри субтитрів.",
                "Яке джерело показувати першим у списку."};
            summary(p,help[settings_selected],557,314,620);
            box_alpha(p,555,534,239,65,.88f,.89f,.91f,1.f,15);
            text_color(p,"Змінити",577,552,194,.09f,.10f,.12f);
        }
        footer(p,*status?status:"A: Деталі   X: Пошук   L/R: Розділи   -: Меню");
    } else if (page==DETAILS) {
        box_alpha(p,0,0,1280,80,.052f,.058f,.070f,1.f,0);
        text(p,"‹  До каталогу",37,28,250);
        box_alpha(p,28,101,1225,510,.09f,.104f,.126f,1.f,23);
        poster(p,&movies[chosen],964,112,241,364,&poster_budget);
        headline(p,movies[chosen].title,60,143,850);
        text_color(p,movies[chosen].original,64,208,835,.67f,.72f,.79f);
        char tag[100];snprintf(tag,sizeof(tag),"%s    ·    %s",movies[chosen].year,
                                movies[chosen].series?"Серіал":"Фільм");
        text(p,tag,65,267,680);
        summary(p,movies[chosen].overview,63,330,850);
        box_alpha(p,61,505,245,66,.91f,.92f,.94f,1.f,17);
        text_color(p,"▶  Дивитися",79,523,210,.09f,.10f,.12f);
        box_alpha(p,328,505,250,66,.25f,.28f,.33f,1.f,17);
        text(p,library_favorite(&movies[chosen])?"✓  В обраному":"＋  Мій список",349,523,210);
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
        int idx=0,first=selected_episode/9*9;
        for(int i=0;i<episode_count;i++) {
            if(episodes[i].season!=season_ids[selected_season])continue;
            if(idx>=first && idx<first+9) {
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
    box_alpha(p,29,25,168,51,.05f,.07f,.10f,.84f,14);
    text(p,"‹  Назад",47,33,141);
    box_alpha(p,0,530,1280,190,.025f,.028f,.037f,.91f,0);
    box_alpha(p,112,582,1056,6,.30f,.32f,.37f,1.f,3);
    int filled=duration>0?(int)(1056.0*pos/duration):0;
    if(filled<0)filled=0;if(filled>1056)filled=1056;
    if(filled>0)box_alpha(p,112,582,filled,6,.93f,.94f,.96f,1.f,3);
    box_alpha(p,112+filled-8,575,16,20,.95f,.95f,.97f,1.f,8);
    char left[24],right[24];
    snprintf(left,sizeof(left),"%02d:%02d",(int)pos/60,(int)pos%60);
    snprintf(right,sizeof(right),"%02d:%02d",(int)duration/60,(int)duration%60);
    text(p,left,32,570,88);
    text(p,right,1177,570,90);
    box_alpha(p,89,675,157,5,.35f,.42f,.52f,1.f,3);
    int volume=(int)video_options[1];
    if(volume<0)volume=0;if(volume>130)volume=130;
    if(volume>0)box_alpha(p,89,675,volume*157/130,5,.92f,.94f,.97f,1.f,3);
    box_alpha(p,89+volume*157/130-7,667,14,20,.96f,.97f,.98f,1.f,7);
    const int centers[]={415,540,665,855,960,1075};
    const char *icons[]={"−10",paused?"▶":"Ⅱ","+10","⚙","Аудіо","Субт."};
    for(int i=0;i<6;i++){
        int w=i<3?86:92;
        box_alpha(p,centers[i]-w/2,629,w,56,.16f,.19f,.24f,.91f,15);
        int left_offset=i==0||i==2?27:i==4||i==5?35:13;
        text(p,icons[i],centers[i]-left_offset,643,w-5);
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
                if(page==PLAYER){
                    int was_visible=SDL_GetTicks()<controls_until;
                    controls_until=SDL_GetTicks()+3000;
                    if(!was_visible)continue;
                    if(ty<105 && tx<310)key=SDL_CONTROLLER_BUTTON_B;
                    else if(ty>=561 && ty<=613 && tx>=103 && tx<=1176){
                        double duration=0;
                        if(player && mpv_get_property(player,"duration",MPV_FORMAT_DOUBLE,&duration)>=0
                           && duration>0){
                            double to=duration*(tx-112)/1056.0;
                            if(to<0)to=0;if(to>duration)to=duration;
                            char at[48];snprintf(at,sizeof(at),"%.2f",to);
                            const char *args[]={"seek",at,"absolute",NULL};
                            mpv_command_async(player,0,args);
                        }
                        continue;
                    }else if(ty>=625 && ty<702){
                        if(tx>=75 && tx<260 && ty>=659){
                            double volume=(tx-89)*130.0/157.0;
                            if(volume<0)volume=0;if(volume>130)volume=130;
                            video_options[1]=volume;
                            playback_set_value(player,"volume",volume);
                            continue;
                        }
                        if(tx>=372 && tx<458)key=SDL_CONTROLLER_BUTTON_DPAD_LEFT;
                        else if(tx>=497 && tx<583)key=SDL_CONTROLLER_BUTTON_A;
                        else if(tx>=622 && tx<708)key=SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
                        else if(tx>=809 && tx<901)key=SDL_CONTROLLER_BUTTON_BACK;
                        else if(tx>=914 && tx<1006)key=SDL_CONTROLLER_BUTTON_Y;
                        else if(tx>=1029 && tx<1121)key=SDL_CONTROLLER_BUTTON_X;
                    }else continue;
                }else if(page==CATALOG){
                    if(ty<78 && tx>=225 && tx<748){
                        int tab=(tx-225)/131;
                        int next=tab==1?1:tab==3?11:0;
                        if(next!=category){
                            category=next;query[0]=message[0]=0;
                            n=load_category(category,query,movies,message,sizeof(message));
                            chosen=0;expanded_catalog=0;
                        }
                        continue;
                    }
                    if(ty<78 && tx>=764 && tx<1046)key=SDL_CONTROLLER_BUTTON_X;
                    else if(ty<78 && tx>=1060 && tx<1118)key=SDL_CONTROLLER_BUTTON_BACK;
                    else if(expanded_catalog){
                        if(ty>=85 && ty<144 && tx>=1090){expanded_catalog=0;chosen=0;continue;}
                        if(ty>=150 && ty<635 && tx>=44){
                            int col=(tx-44)/201,row=(ty-150)/244;
                            int index=chosen/12*12+row*6+col;
                            if(col<6 && row<2 && index<n){chosen=index;key=SDL_CONTROLLER_BUTTON_A;}
                        }
                    }else{
                        if(ty>=340 && ty<394 && tx>=1080){expanded_catalog=1;chosen=0;continue;}
                        if(ty>=291 && ty<335 && tx>=245 && tx<424 && n){
                            library_toggle_favorite(&movies[chosen]);continue;
                        }
                        if(ty>=291 && ty<335 && tx>=56 && tx<231 && n)
                            key=SDL_CONTROLLER_BUTTON_A;
                        else if(ty>=403 && ty<640 && tx>=44){
                            int col=(tx-44)/201,index=chosen/6*6+col;
                            if(col<6 && index<n){chosen=index;key=SDL_CONTROLLER_BUTTON_A;}
                        }
                    }
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
                    int episode=selected_episode/9*9+slot;
                    if(ty>=118 && slot>=0 && slot<9 && selected_season<season_count &&
                       episode<episodes_in_season(season_ids[selected_season])){
                        selected_episode=episode;key=SDL_CONTROLLER_BUTTON_A;
                    }else if(ty<100)key=SDL_CONTROLLER_BUTTON_B;
                }else if(page==DETAILS){
                    if(ty<85 && tx<310)key=SDL_CONTROLLER_BUTTON_B;
                    else if(ty>=505 && ty<574 && tx>=328 && tx<579)
                        key=SDL_CONTROLLER_BUTTON_X;
                    else if(ty>=505 && ty<574 && tx>=61 && tx<308)
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
                    if(tx>=45 && tx<488 && ty>=141 && ty<493){
                        settings_selected=(ty-141)/87;
                        if(settings_selected>=4)settings_selected=3;
                        key=SDL_CONTROLLER_BUTTON_A;
                    }else if(ty>=534 && ty<600 && tx>=555 && tx<795)key=SDL_CONTROLLER_BUTTON_A;
                    else if(ty<125 || ty>665)key=SDL_CONTROLLER_BUTTON_B;
                }
            }
            if(page==PLAYER && key>=0)controls_until=SDL_GetTicks()+3000;
            if(key<0)continue;
            if(page==PLAYER){
                if(key==SDL_CONTROLLER_BUTTON_B){
                    double position=0;
                    if (player && mpv_get_property(player,"time-pos",MPV_FORMAT_DOUBLE,&position)>=0)
                        library_set_progress(&playing_movie,position);
                    command(player,"stop",NULL);page=player_return_page;
                }
                if(key==SDL_CONTROLLER_BUTTON_A)command(player,"cycle","pause");
                int fast=pad && SDL_GameControllerGetAttached(pad) &&
                    SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
                if(key==SDL_CONTROLLER_BUTTON_DPAD_LEFT)seek(player,fast?"-60":"-10");
                if(key==SDL_CONTROLLER_BUTTON_DPAD_RIGHT)seek(player,fast?"60":"10");
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
                if(key==SDL_CONTROLLER_BUTTON_B || key==SDL_CONTROLLER_BUTTON_BACK){
                    page=video_settings_from_menu?SETTINGS:PLAYER;
                    video_settings_from_menu=0;
                }
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
                if(key==SDL_CONTROLLER_BUTTON_B || key==SDL_CONTROLLER_BUTTON_BACK){page=CATALOG;continue;}
                if(key==SDL_CONTROLLER_BUTTON_DPAD_UP && settings_selected>0)settings_selected--;
                if(key==SDL_CONTROLLER_BUTTON_DPAD_DOWN && settings_selected<3)settings_selected++;
                if(key==SDL_CONTROLLER_BUTTON_A){
                    if(settings_selected==0)ui_accent_alt=!ui_accent_alt;
                    else if(settings_selected==1){
                        category=(category+1)%CATEGORY_COUNT;
                        n=load_category(category,query,movies,message,sizeof(message));
                        chosen=0;expanded_catalog=0;page=CATALOG;
                    }else if(settings_selected==2){
                        selected_video_option=0;
                        video_settings_from_menu=1;
                        page=VIDEO_SETTINGS;
                    }else if(settings_selected==3)source_mode=(source_mode+1)%3;
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
                    prioritize_sources(sources,sn);
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
        }else{
            ui_category=category;
            draw(&paint,page,movies,n,chosen,sources,sn,source,headings[category],message);
        }
        SDL_GL_SwapWindow(window);SDL_Delay(16);
    }
    if(render)mpv_render_context_free(render);
    if(player)mpv_terminate_destroy(player);
    if(pad)SDL_GameControllerClose(pad);
    posters_destroy();
    for(int i=0;i<7;i++)if(paint.scripts[i])TTF_CloseFont(paint.scripts[i]);
    if(paint.font_big)TTF_CloseFont(paint.font_big);
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
