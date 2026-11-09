#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include <SDL.h>
#include <SDL_ttf.h>
#include <GLES2/gl2.h>
#include <mpv/client.h>
#include <mpv/render_gl.h>
#include "catalog.h"

typedef enum { CATALOG, DETAILS, SOURCES, PLAYER } Page;
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
static void draw(Paint *p,Page page,Movie *movies,int n,int chosen,
                 Source *sources,int sn,int source,const char *heading,const char *status) {
    glViewport(0,0,1280,720);
    box(p,0,0,1280,720,.055f,.064f,.08f);
    if (page==CATALOG) {
        text(p,"LAMPA NX",28,18,330);
        text(p,heading,28,80,1050);
        int start=chosen/8*8;
        for (int i=start;i<n && i<start+8;i++) {
            int k=i-start,x=28+k%4*311,y=139+k/4*248;
            box(p,x,y,292,226,i==chosen?.22f:.11f,i==chosen?.42f:.14f,i==chosen?.51f:.19f);
            text(p,movies[i].title,x+15,y+52,260);
            text(p,movies[i].original,x+15,y+105,260);
            text(p,movies[i].year,x+15,y+168,250);
        }
        footer(p,*status?status:"A: Деталі    X: Пошук    Y: Оновити    L/R: Категорія    +: Вихід");
    } else if (page==DETAILS) {
        text(p,movies[chosen].title,40,70,1180);
        text(p,movies[chosen].original,40,127,1150);
        text(p,movies[chosen].year,40,177,300);
        const char *s=movies[chosen].overview;
        for (int y=245;*s && y<580;y+=40) {
            size_t len=strlen(s);if(len>90){len=90;while(len>20 && s[len]!=' ')len--;}
            char line[128]={0};memcpy(line,s,len);
            text(p,line,40,y,1170);
            s+=len;while(*s==' ')s++;
        }
        footer(p,*status?status:"A: Шукати на UAKino / UAFresh    B: Назад");
    } else if (page==SOURCES) {
        text(p,"UAKino / UAFresh",40,45,1100);
        for (int i=0;i<sn && i<10;i++) {
            int y=110+i*51;
            box(p,35,y,1200,46,i==source?.22f:.11f,i==source?.42f:.14f,i==source?.51f:.19f);
            text(p,sources[i].url,50,y+8,1160);
        }
        footer(p,*status?status:"A: Відкрити відео    B: Назад");
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
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS,"1");
    if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMECONTROLLER)) return 1;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,0);
    SDL_Window *window=SDL_CreateWindow("Lampa NX",0,0,1280,720,SDL_WINDOW_OPENGL|SDL_WINDOW_SHOWN);
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
    const char *categories[]={"movies","series","top"};
    const char *headings[]={"Популярні фільми","Популярні серіали","Найкращі фільми"};
    char query[192]={0},message[256]={0};
    n=catalog_load(categories[category],query,movies,CATALOG_MAX,message,sizeof(message));
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
                    case SDLK_ESCAPE:key=SDL_CONTROLLER_BUTTON_START;break;
                }
            }
            if(key<0)continue;
            if(page==PLAYER){
                if(key==SDL_CONTROLLER_BUTTON_B){command(player,"stop",NULL);page=SOURCES;}
                if(key==SDL_CONTROLLER_BUTTON_A)command(player,"cycle","pause");
                if(key==SDL_CONTROLLER_BUTTON_DPAD_LEFT)seek(player,"-10");
                if(key==SDL_CONTROLLER_BUTTON_DPAD_RIGHT)seek(player,"10");
                continue;
            }
            if(key==SDL_CONTROLLER_BUTTON_START){running=0;continue;}
            if(page==CATALOG){
                if(key==SDL_CONTROLLER_BUTTON_DPAD_LEFT && chosen>0)chosen--;
                if(key==SDL_CONTROLLER_BUTTON_DPAD_RIGHT && chosen<n-1)chosen++;
                if(key==SDL_CONTROLLER_BUTTON_DPAD_UP && chosen>=4)chosen-=4;
                if(key==SDL_CONTROLLER_BUTTON_DPAD_DOWN && chosen+4<n)chosen+=4;
                if(key==SDL_CONTROLLER_BUTTON_A && n){message[0]=0;page=DETAILS;}
                if(key==SDL_CONTROLLER_BUTTON_LEFTSHOULDER || key==SDL_CONTROLLER_BUTTON_RIGHTSHOULDER){
                    category=(category+(key==SDL_CONTROLLER_BUTTON_RIGHTSHOULDER?1:2))%3;
                    query[0]=message[0]=0;
                    n=catalog_load(categories[category],query,movies,CATALOG_MAX,message,sizeof(message));chosen=0;
                }
                if(key==SDL_CONTROLLER_BUTTON_X){
                    char new_query[192]={0};
                    if(keyboard(new_query,sizeof(new_query))){
                        snprintf(query,sizeof(query),"%s",new_query);
                        message[0]=0;
                        n=catalog_load(categories[category],query,movies,CATALOG_MAX,message,sizeof(message));chosen=0;
                    }
                }
                if(key==SDL_CONTROLLER_BUTTON_Y){
                    message[0]=0;
                    n=catalog_load(categories[category],query,movies,CATALOG_MAX,message,sizeof(message));chosen=0;
                }
            }else if(page==DETAILS){
                if(key==SDL_CONTROLLER_BUTTON_B){page=CATALOG;message[0]=0;}
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
                    char url[2048]={0};
                    message[0]=0;
                    if(render && uakino_resolve(&sources[source],url,sizeof(url),message,sizeof(message))){
                        command(player,"loadfile",url);page=PLAYER;
                    }
                }
            }
        }
        if(player)while(mpv_wait_event(player,0)->event_id!=MPV_EVENT_NONE){}
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
    TTF_CloseFont(paint.font);TTF_Quit();
    glDeleteTextures(1,&paint.tex);
    glDeleteBuffers(1,&paint.vbo);
    glDeleteProgram(paint.program);
cleanup:
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(window);SDL_Quit();romfsExit();
    return 0;
}
