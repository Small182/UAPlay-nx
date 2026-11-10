#include "posters.h"

#include <SDL.h>
#include <SDL_image.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Reduce GPU memory from up to roughly 80 MB to about 30 MB. */
#define POSTER_CACHE_MAX 48
#define MAX_POSTER_BYTES (1024 * 1024)

typedef struct {
    int id;
    GLuint texture;
    unsigned long long last_used;
} PosterEntry;
typedef struct { unsigned char *data; size_t used; } ImageData;
typedef struct {
    SDL_Thread *thread;
    SDL_mutex *mutex;
    SDL_cond *cond;
    int quit,pending,busy,request_id;
    char request_path[256];
    int ready,completed_id;
    ImageData completed_image;
} PosterLoader;

static PosterEntry cache[POSTER_CACHE_MAX];
static PosterLoader loader;
static int used;
static unsigned long long cache_clock;

static size_t receive(char *pointer,size_t size,size_t count,void *ctx){
    ImageData *image=ctx;
    if(size && count>MAX_POSTER_BYTES/size)return 0;
    size_t bytes=size*count;
    if(image->used>MAX_POSTER_BYTES || bytes>MAX_POSTER_BYTES-image->used)return 0;
    void *next=realloc(image->data,image->used+bytes);
    if(!next)return 0;
    image->data=next;
    memcpy(image->data+image->used,pointer,bytes);
    image->used+=bytes;
    return bytes;
}

/* Download raw bytes on a worker; decode and upload on the GL/UI thread. */
static ImageData download_image(const char *path){
    ImageData data={0};
    if(!path || path[0]!='/' || strstr(path,".."))return data;
    CURL *curl=curl_easy_init();
    if(!curl)return data;
    char url[512];
    snprintf(url,sizeof(url),"https://image.tmdb.org/t/p/w342%s",path);
    curl_easy_setopt(curl,CURLOPT_URL,url);
    curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION,1L);
    curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT,4L);
    curl_easy_setopt(curl,CURLOPT_TIMEOUT,7L);
    curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(curl,CURLOPT_USERAGENT,"UAPlay-nx/1.0");
    curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,receive);
    curl_easy_setopt(curl,CURLOPT_WRITEDATA,&data);
    CURLcode result=curl_easy_perform(curl);
    long code=0;
    curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&code);
    curl_easy_cleanup(curl);
    if(result!=CURLE_OK || code!=200 || !data.used){
        free(data.data);data.data=NULL;data.used=0;
    }
    return data;
}

static int download_worker(void *opaque){
    PosterLoader *task=opaque;
    for(;;){
        SDL_LockMutex(task->mutex);
        while(!task->quit && !task->pending)
            SDL_CondWait(task->cond,task->mutex);
        if(task->quit){SDL_UnlockMutex(task->mutex);break;}
        char path[256];
        snprintf(path,sizeof(path),"%s",task->request_path);
        int id=task->request_id;
        task->pending=0;task->busy=1;
        SDL_UnlockMutex(task->mutex);

        ImageData image=download_image(path);

        SDL_LockMutex(task->mutex);
        task->busy=0;
        if(!task->quit){
            task->completed_image=image;
            task->completed_id=id;
            task->ready=1;
        }else free(image.data);
        SDL_UnlockMutex(task->mutex);
    }
    return 0;
}

void posters_init(void){
    loader.mutex=SDL_CreateMutex();
    loader.cond=SDL_CreateCond();
    if(loader.mutex && loader.cond)
        loader.thread=SDL_CreateThread(download_worker,"poster-download",&loader);
}

static GLuint image_texture(const ImageData *data){
    if(!data->data || !data->used)return 0;
    SDL_RWops *rw=SDL_RWFromConstMem(data->data,(int)data->used);
    SDL_Surface *image=rw?IMG_Load_RW(rw,1):NULL;
    if(!image)return 0;
    SDL_Surface *rgba=SDL_ConvertSurfaceFormat(image,SDL_PIXELFORMAT_ABGR8888,0);
    SDL_FreeSurface(image);
    if(!rgba)return 0;
    GLuint texture=0;
    glGenTextures(1,&texture);
    if(texture){
        glBindTexture(GL_TEXTURE_2D,texture);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,rgba->w,rgba->h,
                     0,GL_RGBA,GL_UNSIGNED_BYTE,rgba->pixels);
    }
    SDL_FreeSurface(rgba);
    return texture;
}

static void cache_store(int id,GLuint texture){
    int slot=used;
    if(used==POSTER_CACHE_MAX){
        slot=0;
        for(int i=1;i<used;i++)
            if(cache[i].last_used<cache[slot].last_used)slot=i;
        if(cache[slot].texture)glDeleteTextures(1,&cache[slot].texture);
    }else used++;
    cache[slot].id=id;
    cache[slot].texture=texture;
    cache[slot].last_used=++cache_clock;
}

static void collect_completed(void){
    if(!loader.mutex)return;
    SDL_LockMutex(loader.mutex);
    if(!loader.ready){SDL_UnlockMutex(loader.mutex);return;}
    ImageData image=loader.completed_image;
    int id=loader.completed_id;
    loader.completed_image=(ImageData){0};
    loader.ready=0;
    SDL_UnlockMutex(loader.mutex);
    GLuint texture=image_texture(&image);
    free(image.data);
    /* Cache failed requests too, preventing repeated 7-second retries. */
    cache_store(id,texture);
}

GLuint poster_texture(const Movie *movie,int *budget){
    if(!movie || !movie->poster[0] || !strcmp(movie->poster,"null"))return 0;
    collect_completed();
    int identity=movie->series?-movie->id:movie->id;
    for(int i=0;i<used;i++){
        if(cache[i].id==identity){
            cache[i].last_used=++cache_clock;
            return cache[i].texture;
        }
    }
    if(!budget || *budget<=0 || !loader.thread)return 0;
    SDL_LockMutex(loader.mutex);
    if(!loader.pending && !loader.busy && !loader.ready){
        loader.request_id=identity;
        snprintf(loader.request_path,sizeof(loader.request_path),"%s",movie->poster);
        loader.pending=1;
        --*budget;
        SDL_CondSignal(loader.cond);
    }
    SDL_UnlockMutex(loader.mutex);
    return 0;
}

void posters_destroy(void){
    if(loader.mutex){
        SDL_LockMutex(loader.mutex);
        loader.quit=1;
        if(loader.cond)SDL_CondSignal(loader.cond);
        SDL_UnlockMutex(loader.mutex);
    }
    if(loader.thread)SDL_WaitThread(loader.thread,NULL);
    free(loader.completed_image.data);
    if(loader.cond)SDL_DestroyCond(loader.cond);
    if(loader.mutex)SDL_DestroyMutex(loader.mutex);
    memset(&loader,0,sizeof(loader));
    for(int i=0;i<used;i++)
        if(cache[i].texture)glDeleteTextures(1,&cache[i].texture);
    used=0;cache_clock=0;
}
