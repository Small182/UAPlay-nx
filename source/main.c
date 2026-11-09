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
    "Романтика","Документальні","Вибране","Історія переглядів"};
#define CATEGORY_COUNT ((int)(sizeof(categories)/sizeof(categories[0])))
static int expanded_catalog=0;
/* 0: posters, 1: Watch, 2: Add to favorites, 3: All (catalog controls). */
static int catalog_hero_focus=0;
/* 0: Watch, 1: Add to favorites (details buttons). */
static int details_button_focus=0;
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
    TTF_Font *font_small;
    TTF_Font *font_tiny;
    TTF_Font *scripts[7];
    TTF_Font *scripts_small[7];
    TTF_Font *scripts_big[7];
    TTF_Font *scripts_tiny[7];
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
static int measure(Paint *p,const char *s,TTF_Font *font){
    TTF_Font *prev=p->font;p->font=font;
    int w=0,h=0;
    TTF_SizeUTF8(choose_font(p,s),s,&w,&h);
    p->font=prev;
    return w;
}
static void text_center(Paint *p,const char *s,int x,int y,int width,TTF_Font *font,
                        float r,float g,float b){
    int w=measure(p,s,font);
    int left=x+(width-w)/2;
    if(left<x)left=x;
    text_font(p,s,left,y,width-(left-x),font,r,g,b);
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
    *r=ui_accent_alt?.43f:.89f;
    *g=ui_accent_alt?.73f:.68f;
    *b=ui_accent_alt?.84f:.39f;
}
static void selection_frame(Paint *p,int x,int y,int w,int h){
    float r,g,b;accent(&r,&g,&b);
    box_alpha(p,x-4,y-4,w+8,h+8,r,g,b,1.f,18);
}
static void button(Paint *p,const char *label,int x,int y,int w,int h,int primary){
    box_alpha(p,x,y,w,h,primary?.89f:.15f,primary?.91f:.18f,
              primary?.94f:.22f,1.f,14);
    text_center(p,label,x,y+(h-23)/2,w,p->font_small,
                primary?.08f:.95f,primary?.10f:.96f,primary?.13f:.98f);
}
/* UI icons are drawn with primitives: no missing-font placeholder squares. */
static void gear_icon(Paint *p,int cx,int cy,float bg_r,float bg_g,float bg_b){
    const float fg=.95f;
    box_alpha(p,cx-3,cy-18,6,36,fg,fg,fg,1.f,2);
    box_alpha(p,cx-18,cy-3,36,6,fg,fg,fg,1.f,2);
    box_alpha(p,cx-13,cy-13,26,26,fg,fg,fg,1.f,13);
    box_alpha(p,cx-6,cy-6,12,12,bg_r,bg_g,bg_b,1.f,6);
}
static void screen_header(Paint *p,const char *name,const char *context){
    box_alpha(p,0,0,1280,92,.058f,.069f,.083f,1.f,0);
    box_alpha(p,UI_EDGE,23,55,48,.13f,.16f,.20f,1.f,13);
    text_center(p,"‹",UI_EDGE,25,55,p->font_big,.96f,.97f,.99f);
    headline(p,name,105,23,860);
    if(context && *context)text_tiny(p,context,979,38,264);
}
static void list_row(Paint *p,const char *title,int y,int selected,
                     const char *detail){
    box_alpha(p,UI_EDGE,y,1216,UI_ROW_HEIGHT,
              selected?.20f:.085f,selected?.30f:.105f,
              selected?.38f:.135f,1.f,12);
    if(selected){
        float r,g,b;accent(&r,&g,&b);
        box_alpha(p,UI_EDGE,y+9,4,UI_ROW_HEIGHT-18,r,g,b,1.f,2);
    }
    text(p,title,UI_EDGE+22,y+9,detail?915:1155);
    if(detail)text_muted(p,detail,1052,y+13,178);
}
static int footer_position,footer_total;
static Page footer_page;
static void footer(Paint *p,const char *message) {
    box_alpha(p,0,UI_FOOTER_TOP,1280,720-UI_FOOTER_TOP,.047f,.057f,.070f,1.f,0);
    /* Monochrome Switch-style controller legend, independent of UI accent. */
    float ar=.94f,ag=.95f,ab=.97f;
    box_alpha(p,UI_EDGE,UI_FOOTER_TOP,1216,2,ar,ag,ab,.78f,0);
    char position[30];
    snprintf(position,sizeof(position),"%d / %d",footer_position,footer_total);
    text_font(p,footer_total?position:"UAPLAY",43,682,140,
              p->font_small,ar,ag,ab);
    const char *keys[6],*labels[6];int count=0;
#define HINT(k,l) do{keys[count]=k;labels[count]=l;count++;}while(0)
    if(footer_page==CATALOG){
        HINT("L","Розділ");HINT("R","Розділ");
        HINT("X","Пошук");HINT("−","Меню");HINT("A","Відкрити");
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
        HINT("↑↓","Серія");HINT("A","Дивитися");HINT("B","Назад");
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
}
static void draw(Paint *p,Page page,Movie *movies,int n,int chosen,
                 Source *sources,int sn,int source,const char *heading,const char *status) {
    glViewport(0,0,1280,720);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
    box(p,0,0,1280,720,.034f,.042f,.054f);
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
        headline(p,"UAPlay",32,18,175);
        const char *tabs[]={"Головна","Серіали","Фільми","Вибране"};
        for(int i=0;i<4;i++){
            int x=229+i*131;
            int active=(i==0 && ui_category==0) ||
                       (i==1 && ui_category==1) || (i==2 && ui_category>=2 && ui_category<11) ||
                       (i==3 && ui_category>=11);
            if(active)box_alpha(p,x-12,19,124,43,.19f,.24f,.28f,1.f,16);
            text_small(p,tabs[i],x,27,114);
        }
        /* Search is a real touch target, not a missing Unicode glyph. */
        box_alpha(p,767,17,277,47,.13f,.16f,.19f,1.f,15);
        box_alpha(p,787,27,22,22,.91f,.93f,.96f,1.f,11);
        box_alpha(p,791,31,14,14,.13f,.16f,.19f,1.f,7);
        box_alpha(p,805,44,11,5,.91f,.93f,.96f,1.f,2);
        text_small(p,"Пошук",831,27,177);
        box_alpha(p,1058,17,53,47,.13f,.16f,.19f,1.f,15);
        gear_icon(p,1085,40,.13f,.16f,.19f);
        time_t now=time(NULL);
        struct tm *local=localtime(&now);
        char clock_label[32]="--:--";
        if(local)strftime(clock_label,sizeof(clock_label),"%H:%M",local);
        text_small(p,clock_label,1154,29,105);
        if(expanded_catalog){
            headline(p,heading,UI_EDGE,97,960);
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
            box_alpha(p,UI_EDGE,92,1216,222,.09f,.11f,.14f,1.f,22);
            if(n){
                poster(p,&movies[active],1080,102,135,202,&poster_budget);
                headline(p,movies[active].title,61,106,967);
                char tag[110];
                snprintf(tag,sizeof(tag),"%s  ·  %s  ·  TMDB",movies[active].year,
                         movies[active].series?"Серіал":"Фільм");
                text_muted(p,tag,63,159,890);
                wrapped(p,movies[active].overview,63,198,949,2,27,
                        p->font_small,.74f,.79f,.84f);
            }else{
                headline(p,"Не знайдено матеріалів",63,121,900);
                text_muted(p,"Спробуйте інший розділ або пошуковий запит",63,185,920);
            }
            if(n && catalog_hero_focus==1)selection_frame(p,62,264,180,39);
            if(n && catalog_hero_focus==2)selection_frame(p,254,264,189,39);
            button(p,"▶  Дивитися",62,264,180,39,1);
            button(p,n && library_favorite(&movies[active])?
                   "✓  У вибраному":"+  До списку",254,264,189,39,0);
            text(p,heading,UI_EDGE,336,930);
            if(status && (!strncmp(status,"Offline catalog",15) ||
                          !strncmp(status,"Офлайн-пошук",strlen("Офлайн-пошук"))))
                text_tiny(p,"Офлайн",1026,340,77);
            if(n && catalog_hero_focus==3)selection_frame(p,1110,329,138,42);
            button(p,"Усі  ›",1110,329,138,42,0);
            int start=chosen/6*6;
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
        const char *items[]={"Інтерфейс","Плеєр","Джерела"};
        for(int i=0;i<3;i++){
            int y=136+i*88;
            box_alpha(p,32,y,417,72,i==settings_selected?.20f:.09f,
                      i==settings_selected?.29f:.11f,
                      i==settings_selected?.37f:.14f,1.f,16);
            if(i==settings_selected){float r,g,b;accent(&r,&g,&b);
                box_alpha(p,32,y+15,5,42,r,g,b,1.f,2);}
            text(p,items[i],55,y+17,372);
        }
        box_alpha(p,475,136,773,480,.10f,.12f,.15f,1.f,20);
        headline(p,items[settings_selected],513,174,685);
        char description[240];
        if(settings_selected==0)snprintf(description,sizeof(description),"Акцент: %s",ui_accent_alt?"Блакитний":"Золотистий");
        else if(settings_selected==1)snprintf(description,sizeof(description),"Швидкість: %.2f · гучність: %.0f",video_options[0],video_options[1]);
        else snprintf(description,sizeof(description),"Пріоритет: %s",source_mode==0?"Усі джерела":source_mode==1?"UAFlix":"UASerials");
        text(p,description,516,255,682);
        const char *help[]={"Змінює колір виділення на всіх екранах.",
            "Налаштуйте відео, гучність та субтитри.",
            "Оберіть порядок відображення джерел відео."};
        wrapped(p,help[settings_selected],517,315,675,3,29,p->font_small,.73f,.78f,.84f);
        button(p,settings_selected==1?"Відкрити параметри":"Змінити",515,537,258,55,1);
        footer(p,status);
    } else if (page==DETAILS) {
        screen_header(p,movies[chosen].series?"Про серіал":"Про фільм","UAPlay");
        box_alpha(p,32,109,1216,507,.09f,.11f,.14f,1.f,21);
        poster(p,&movies[chosen],978,121,229,344,&poster_budget);
        headline(p,movies[chosen].title,65,138,872);
        text_muted(p,movies[chosen].original,67,198,859);
        char tag[100];snprintf(tag,sizeof(tag),"%s    ·    %s",movies[chosen].year,
                                movies[chosen].series?"Серіал":"Фільм");
        text(p,tag,66,259,835);
        summary(p,movies[chosen].overview,66,313,858);
        if(!details_button_focus)selection_frame(p,65,526,238,58);
        else selection_frame(p,320,526,255,58);
        button(p,"▶  Дивитися",65,526,238,58,1);
        button(p,library_favorite(&movies[chosen])?"✓  У вибраному":"+  До списку",
               320,526,255,58,0);
        footer(p,status);
    } else if (page==SOURCES) {
        screen_header(p,"Джерела відео","UAFlix · UASerials");
        int first=source/UI_ROWS*UI_ROWS;
        for(int i=first;i<sn && i<first+UI_ROWS;i++){
            list_row(p,sources[i].title,UI_ROW_Y+(i-first)*UI_ROW_STEP,i==source,NULL);
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
            int y=132+i*68;
            box_alpha(p,UI_EDGE,y,1216,60,i==selected_video_option?.20f:.09f,
                      i==selected_video_option?.30f:.11f,
                      i==selected_video_option?.38f:.14f,1.f,13);
            text(p,video_titles[i],55,y+15,830);
            char value[48];
            if(i==6)snprintf(value,sizeof(value),"%s",hls_labels[(int)video_options[i]]);
            else if(i==0 || i==5)snprintf(value,sizeof(value),"%.2f",video_options[i]);
            else snprintf(value,sizeof(value),"%.0f",video_options[i]);
            text_center(p,value,963,y+15,240,p->font,.95f,.96f,.98f);
            if(i==selected_video_option){float r,g,b;accent(&r,&g,&b);
                box_alpha(p,UI_EDGE,y+12,4,36,r,g,b,1.f,2);}
        }
        text_tiny(p,"Якість HLS визначається потоками, які надає джерело.",50,626,1150);
        footer(p,NULL);
    }
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
    box_alpha(p,32,27,141,49,.045f,.063f,.079f,.92f,14);
    text_center(p,"‹  Назад",32,37,141,p->font_small,.96f,.97f,.99f);
    box_alpha(p,0,507,1280,213,.027f,.034f,.044f,.93f,0);
    text_tiny(p,"UAPlay  ·  Відтворення",35,519,430);
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
    text_tiny(p,"Гучність",67,606,190);
    box_alpha(p,70,649,178,5,.35f,.42f,.52f,1.f,3);
    int volume=(int)video_options[1];
    if(volume<0)volume=0;if(volume>130)volume=130;
    if(volume>0)box_alpha(p,70,649,volume*178/130,5,.92f,.94f,.97f,1.f,3);
    box_alpha(p,70+volume*178/130-7,641,14,20,.96f,.97f,.98f,1.f,7);
    char volume_label[24];snprintf(volume_label,sizeof(volume_label),"%d%%",volume*100/130);
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
                    if(ty>=24 && ty<=83 && tx>=27 && tx<=180)key=SDL_CONTROLLER_BUTTON_B;
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
                    if(ty<78 && tx>=217 && tx<742){
                        int tab=(tx-217)/131;
                        int next=tab==1?1:tab==2?2:tab==3?11:0;
                        if(next!=category){
                            category=next;query[0]=message[0]=0;
                            n=load_category(category,query,movies,message,sizeof(message));
                            chosen=0;expanded_catalog=0;catalog_hero_focus=0;
                        }
                        continue;
                    }
                    if(ty<78 && tx>=767 && tx<1044)key=SDL_CONTROLLER_BUTTON_X;
                    else if(ty<78 && tx>=1058 && tx<1111)key=SDL_CONTROLLER_BUTTON_BACK;
                    else if(expanded_catalog){
                        if(ty>=95 && ty<145 && tx>=1090){expanded_catalog=0;chosen=0;catalog_hero_focus=0;continue;}
                        if(ty>=153 && ty<613 && tx>=54){
                            int col=(tx-54)/201,row=(ty-153)/246;
                            int index=chosen/12*12+row*6+col;
                            if(col<6 && row<2 && index<n && (tx-54)%201<138 &&
                               (ty-153)%246<206){
                                chosen=index;catalog_hero_focus=0;key=SDL_CONTROLLER_BUTTON_A;
                            }
                        }
                    }else{
                        if(ty>=326 && ty<375 && tx>=1095){expanded_catalog=1;chosen=0;catalog_hero_focus=0;continue;}
                        if(ty>=260 && ty<307 && tx>=254 && tx<444 && n){
                            catalog_hero_focus=2;
                            library_toggle_favorite(&movies[chosen]);continue;
                        }
                        if(ty>=260 && ty<307 && tx>=62 && tx<242 && n){
                            catalog_hero_focus=1;
                            key=SDL_CONTROLLER_BUTTON_A;
                        }
                        else if(ty>=389 && ty<661 && tx>=53){
                            int col=(tx-53)/201,index=chosen/6*6+col;
                            if(col<6 && index<n && (tx-53)%201<164){
                                chosen=index;catalog_hero_focus=0;key=SDL_CONTROLLER_BUTTON_A;
                            }
                        }
                    }
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
                    if(tx>=32 && tx<449 && ty>=136 && ty<384 && (ty-136)%88<72){
                        settings_selected=(ty-136)/88;
                        if(settings_selected>=3)settings_selected=2;
                        key=SDL_CONTROLLER_BUTTON_A;
                    }else if(ty>=537 && ty<592 && tx>=515 && tx<773)key=SDL_CONTROLLER_BUTTON_A;
                    else if(ty<92 && tx<100)key=SDL_CONTROLLER_BUTTON_B;
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
                if(key==SDL_CONTROLLER_BUTTON_DPAD_DOWN && settings_selected<2)settings_selected++;
                if(key==SDL_CONTROLLER_BUTTON_A){
                    if(settings_selected==0)ui_accent_alt=!ui_accent_alt;
                    else if(settings_selected==1){
                        selected_video_option=0;
                        video_settings_from_menu=1;
                        page=VIDEO_SETTINGS;
                    }else if(settings_selected==2)source_mode=(source_mode+1)%3;
                }
                continue;
            }
            if(key==SDL_CONTROLLER_BUTTON_START){running=0;continue;}
            if(page==CATALOG){
                if(key==SDL_CONTROLLER_BUTTON_BACK){page=SETTINGS;continue;}
                if(key==SDL_CONTROLLER_BUTTON_RIGHTSTICK){
                    expanded_catalog=!expanded_catalog;chosen=0;catalog_hero_focus=0;continue;
                }
                if(key==SDL_CONTROLLER_BUTTON_B && expanded_catalog){
                    expanded_catalog=0;chosen=0;catalog_hero_focus=0;continue;
                }
                if(key==SDL_CONTROLLER_BUTTON_B && category!=0){
                    category=0;query[0]=message[0]=0;
                    n=load_category(category,query,movies,message,sizeof(message));
                    chosen=0;catalog_hero_focus=0;continue;
                }
                if(!expanded_catalog && catalog_hero_focus){
                    if(key==SDL_CONTROLLER_BUTTON_DPAD_LEFT && catalog_hero_focus>1)
                        catalog_hero_focus--;
                    if(key==SDL_CONTROLLER_BUTTON_DPAD_RIGHT && catalog_hero_focus<3)
                        catalog_hero_focus++;
                    if(key==SDL_CONTROLLER_BUTTON_DPAD_DOWN)catalog_hero_focus=0;
                }else{
                    if(key==SDL_CONTROLLER_BUTTON_DPAD_LEFT && chosen>0)chosen--;
                    if(key==SDL_CONTROLLER_BUTTON_DPAD_RIGHT && chosen<n-1)chosen++;
                    if(key==SDL_CONTROLLER_BUTTON_DPAD_UP){
                        if(chosen>=6)chosen-=6;
                        else if(!expanded_catalog && n)
                            catalog_hero_focus=chosen%6>=4?3:1;
                    }
                    if(key==SDL_CONTROLLER_BUTTON_DPAD_DOWN && chosen+6<n)chosen+=6;
                }
                if(key==SDL_CONTROLLER_BUTTON_A && n){
                    if(!expanded_catalog && catalog_hero_focus==3){
                        expanded_catalog=1;chosen=0;catalog_hero_focus=0;message[0]=0;
                    }else if(!expanded_catalog && catalog_hero_focus==2){
                        library_toggle_favorite(&movies[chosen]);
                        snprintf(message,sizeof(message),"%s",library_favorite(&movies[chosen])?
                                 "Додано у вибране":"Видалено з вибраного");
                    }else{message[0]=0;details_button_focus=0;page=DETAILS;}
                }
                if(key==SDL_CONTROLLER_BUTTON_LEFTSHOULDER || key==SDL_CONTROLLER_BUTTON_RIGHTSHOULDER){
                    category=(category+(key==SDL_CONTROLLER_BUTTON_RIGHTSHOULDER?1:CATEGORY_COUNT-1))%CATEGORY_COUNT;
                    expanded_catalog=0;catalog_hero_focus=0;
                    query[0]=message[0]=0;
                    n=load_category(category,query,movies,message,sizeof(message));chosen=0;
                }
                if(key==SDL_CONTROLLER_BUTTON_X){
                    char new_query[192]={0};
                    if(keyboard(new_query,sizeof(new_query))){
                        snprintf(query,sizeof(query),"%s",new_query);
                        message[0]=0;
                        n=load_category(category,query,movies,message,sizeof(message));chosen=0;catalog_hero_focus=0;
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
                    if(category>=CATEGORY_COUNT-2){
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
                        sn=uakino_search(&movies[chosen],sources,UAKINO_MAX,message,sizeof(message));
                        prioritize_sources(sources,sn);
                        source=0;page=SOURCES;
                    }
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
    for(int i=0;i<7;i++){
        if(paint.scripts[i])TTF_CloseFont(paint.scripts[i]);
        if(paint.scripts_small[i])TTF_CloseFont(paint.scripts_small[i]);
        if(paint.scripts_big[i])TTF_CloseFont(paint.scripts_big[i]);
        if(paint.scripts_tiny[i])TTF_CloseFont(paint.scripts_tiny[i]);
    }
    if(paint.font_tiny)TTF_CloseFont(paint.font_tiny);
    if(paint.font_small)TTF_CloseFont(paint.font_small);
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
