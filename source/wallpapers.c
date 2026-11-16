#include "wallpapers.h"
#include "i18n.h"

#include <SDL.h>
#include <SDL_image.h>
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* HDQWalls permits people to download wallpapers through their browsers,
   but prohibits scraping/direct integration without written permission.
   Accordingly UAPlay reads only files the user placed on their SD card. */
#define WALLPAPER_HOME "sdmc:/switch/uaplay"
#define WALLPAPER_DIR WALLPAPER_HOME "/wallpapers"
#define WALLPAPER_FILE WALLPAPER_HOME "/wallpaper.bin"
#define WALLPAPER_TEMP WALLPAPER_HOME "/wallpaper.part"
#define WALLPAPER_BACKUP WALLPAPER_HOME "/wallpaper.bak"
#define WALLPAPER_NAME WALLPAPER_HOME "/wallpaper.txt"
#define WALLPAPER_NAME_TMP WALLPAPER_HOME "/wallpaper.txt.part"
#define WALLPAPER_NAME_BACKUP WALLPAPER_HOME "/wallpaper.txt.bak"
#define MAX_IMAGE_BYTES (24u * 1024u * 1024u)
#define MAX_IMAGE_PIXELS (12u * 1024u * 1024u)

enum { JOB_NONE, JOB_SCAN, JOB_APPLY };
static WallpaperItem items[WALLPAPERS_MAX], scanned[WALLPAPERS_MAX];
static int count,scanned_count,request,busy,done,success,done_job,done_index;
static int selected_index,sequence,request_sequence,done_sequence,revision;
static SDL_mutex *mutex;
static SDL_cond *signal;
static SDL_Thread *worker;
static int quitting;
static GLuint background;
static char selected_name[128],notice[192];

static int file_extension(const char *name){
    const char *dot=strrchr(name,'.');
    if(!dot || dot==name)return 0;
    char ext[10]={0};size_t len=strlen(dot);
    if(len>=sizeof(ext))return 0;
    for(size_t i=0;i<len;i++)ext[i]=(char)tolower((unsigned char)dot[i]);
    return !strcmp(ext,".jpg") || !strcmp(ext,".jpeg") ||
           !strcmp(ext,".png") || !strcmp(ext,".webp");
}
static int compare_items(const void *a,const void *b){
    return strcmp(((const WallpaperItem*)a)->name,
                  ((const WallpaperItem*)b)->name);
}
static int scan_directory(void){
    mkdir("sdmc:/switch",0777);
    mkdir(WALLPAPER_HOME,0777);
    mkdir(WALLPAPER_DIR,0777);
    DIR *dir=opendir(WALLPAPER_DIR);
    scanned_count=0;
    if(!dir)return 0;
    struct dirent *entry;
    while((entry=readdir(dir)) && scanned_count<WALLPAPERS_MAX){
        const char *name=entry->d_name;
        size_t length=strlen(name);
        if(!length || length>=sizeof(scanned[0].name) ||
           name[0]=='.' || strchr(name,'/') || strchr(name,'\\') ||
           !file_extension(name))continue;
        WallpaperItem item={0};
        int length_url=snprintf(item.url,sizeof(item.url),WALLPAPER_DIR "/%s",name);
        if(length_url<=0 || (size_t)length_url>=sizeof(item.url))continue;
        struct stat info;
        if(stat(item.url,&info)!=0 || !S_ISREG(info.st_mode) ||
           info.st_size<=0 || (unsigned long long)info.st_size>MAX_IMAGE_BYTES)continue;
        snprintf(item.name,sizeof(item.name),"%s",name);
        item.bytes=(unsigned)info.st_size;
        scanned[scanned_count++]=item;
    }
    closedir(dir);
    qsort(scanned,(size_t)scanned_count,sizeof(scanned[0]),compare_items);
    return 1;
}
/* Keep the original source file untouched. The selected copy survives
   removal/renaming of imported files and can be used offline. */
static int copy_file(const char *source){
    if(strncmp(source,WALLPAPER_DIR "/",sizeof(WALLPAPER_DIR)) ||
       strstr(source,".."))return 0;
    FILE *in=fopen(source,"rb");
    if(!in)return 0;
    FILE *out=fopen(WALLPAPER_TEMP,"wb");
    int ok=out!=NULL;
    unsigned char chunk[64*1024];
    size_t total=0,used;
    while(ok && (used=fread(chunk,1,sizeof(chunk),in))>0){
        if(used>MAX_IMAGE_BYTES-total ||
           fwrite(chunk,1,used,out)!=used){ok=0;break;}
        total+=used;
    }
    if(ferror(in) || !total)ok=0;
    fclose(in);
    if(out && fclose(out)!=0)ok=0;
    if(!ok)remove(WALLPAPER_TEMP);
    return ok;
}
/* Decoding is bounded: the full-size image never becomes a GL texture.
   A centered, aspect-preserving crop is scaled to Switch resolution. */
static int image_texture(const char *path,GLuint *texture_out){
    FILE *file=fopen(path,"rb");
    if(!file)return 0;
    SDL_RWops *rw=SDL_RWFromFP(file,1);
    if(!rw){fclose(file);return 0;}
    SDL_Surface *original=IMG_Load_RW(rw,1);
    if(!original)return 0;
    if(original->w<1 || original->h<1 ||
       (unsigned long long)original->w*(unsigned)original->h>MAX_IMAGE_PIXELS){
        SDL_FreeSurface(original);return 0;
    }
    SDL_Surface *rgba=SDL_ConvertSurfaceFormat(original,SDL_PIXELFORMAT_ABGR8888,0);
    SDL_FreeSurface(original);
    if(!rgba)return 0;
    SDL_Surface *scaled=SDL_CreateRGBSurfaceWithFormat(0,1280,720,32,SDL_PIXELFORMAT_ABGR8888);
    if(!scaled){SDL_FreeSurface(rgba);return 0;}
    SDL_Rect crop={0,0,rgba->w,rgba->h};
    if((long long)crop.w*720>=(long long)crop.h*1280){
        crop.w=(int)((long long)crop.h*1280/720);
        crop.x=(rgba->w-crop.w)/2;
    }else{
        crop.h=(int)((long long)crop.w*720/1280);
        crop.y=(rgba->h-crop.h)/2;
    }
    SDL_SetSurfaceBlendMode(rgba,SDL_BLENDMODE_NONE);
    int ok=SDL_BlitScaled(rgba,&crop,scaled,NULL)==0;
    SDL_FreeSurface(rgba);
    if(!ok){SDL_FreeSurface(scaled);return 0;}
    GLuint tex=0;glGenTextures(1,&tex);
    if(tex){
        glBindTexture(GL_TEXTURE_2D,tex);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1280,720,0,GL_RGBA,GL_UNSIGNED_BYTE,scaled->pixels);
    }
    SDL_FreeSurface(scaled);
    *texture_out=tex;
    return tex!=0;
}
static int commit_image(const char *name){
    FILE *f=fopen(WALLPAPER_NAME_TMP,"wb");
    if(!f)return 0;
    int ok=fprintf(f,"%s\n",name)>0;
    if(fclose(f)!=0)ok=0;
    if(!ok){remove(WALLPAPER_NAME_TMP);return 0;}
    remove(WALLPAPER_BACKUP);
    int had_image=rename(WALLPAPER_FILE,WALLPAPER_BACKUP)==0;
    if(rename(WALLPAPER_TEMP,WALLPAPER_FILE)!=0){
        if(had_image)rename(WALLPAPER_BACKUP,WALLPAPER_FILE);
        remove(WALLPAPER_NAME_TMP);return 0;
    }
    remove(WALLPAPER_NAME_BACKUP);
    int had_name=rename(WALLPAPER_NAME,WALLPAPER_NAME_BACKUP)==0;
    if(rename(WALLPAPER_NAME_TMP,WALLPAPER_NAME)!=0){
        remove(WALLPAPER_FILE);
        if(had_image)rename(WALLPAPER_BACKUP,WALLPAPER_FILE);
        if(had_name)rename(WALLPAPER_NAME_BACKUP,WALLPAPER_NAME);
        remove(WALLPAPER_NAME_TMP);return 0;
    }
    remove(WALLPAPER_NAME_BACKUP);
    remove(WALLPAPER_BACKUP);
    return 1;
}
static int run_worker(void *context){
    (void)context;
    for(;;){
        SDL_LockMutex(mutex);
        while(!quitting && !request)SDL_CondWait(signal,mutex);
        if(quitting){SDL_UnlockMutex(mutex);break;}
        int job=request,index=selected_index,seq=request_sequence;
        request=0;busy=job;
        char path[384]={0};
        if(job==JOB_APPLY && index>=0 && index<count)
            snprintf(path,sizeof(path),"%s",items[index].url);
        SDL_UnlockMutex(mutex);
        int ok=job==JOB_SCAN?scan_directory() :
               job==JOB_APPLY && path[0]?copy_file(path):0;
        SDL_LockMutex(mutex);
        done=1;success=ok;done_job=job;done_index=index;done_sequence=seq;
        busy=0;
        SDL_UnlockMutex(mutex);
    }
    return 0;
}
void wallpapers_init(void){
    mkdir("sdmc:/switch",0777);
    mkdir(WALLPAPER_HOME,0777);
    mkdir(WALLPAPER_DIR,0777);
    FILE *f=fopen(WALLPAPER_NAME,"rb");
    if(!f)f=fopen(WALLPAPER_NAME_BACKUP,"rb");
    if(f){
        if(fgets(selected_name,sizeof(selected_name),f)){
            selected_name[strcspn(selected_name,"\r\n")]=0;
            if(!image_texture(WALLPAPER_FILE,&background))selected_name[0]=0;
        }
        fclose(f);
    }
    mutex=SDL_CreateMutex();
    signal=SDL_CreateCond();
    if(mutex && signal)worker=SDL_CreateThread(run_worker,"wallpapers",NULL);
    notice[0]=0;
}
void wallpapers_open(void){
    if(!mutex || !worker)return;
    SDL_LockMutex(mutex);
    if(!request && !busy && !done){
        request=JOB_SCAN;
        request_sequence=++sequence;
        SDL_CondSignal(signal);
        snprintf(notice,sizeof(notice),"%s",
                 TR("Пошук шпалер на SD-карті…","Scanning wallpapers on the SD card…"));
    }
    SDL_UnlockMutex(mutex);
}
void wallpapers_poll(void){
    if(!mutex)return;
    SDL_LockMutex(mutex);
    int finished=done,job=done_job,ok=success,index=done_index,seq=done_sequence;
    if(finished){
        done=0;
        if(job==JOB_SCAN){
            count=ok?scanned_count:0;
            if(count)memcpy(items,scanned,(size_t)count*sizeof(items[0]));
            revision++;
        }
    }
    SDL_UnlockMutex(mutex);
    if(!finished)return;
    if(job==JOB_SCAN){
        snprintf(notice,sizeof(notice),"%s",ok?
            (count?TR("A: застосувати · X: пошук · Y: стандартний фон",
                      "A: Apply · X: Search · Y: Default"):
                   TR("Скопіюйте JPG/PNG до switch/uaplay/wallpapers/",
                      "Copy JPG/PNG into switch/uaplay/wallpapers/")):
            TR("Не вдалося прочитати папку шпалер.","Could not read wallpaper directory."));
    }else if(job==JOB_APPLY){
        if(seq!=sequence){remove(WALLPAPER_TEMP);return;}
        GLuint tex=0;
        int valid=ok && image_texture(WALLPAPER_TEMP,&tex);
        if(valid && index>=0 && index<count && commit_image(items[index].name)){
            if(background)glDeleteTextures(1,&background);
            background=tex;
            snprintf(selected_name,sizeof(selected_name),"%s",items[index].name);
            snprintf(notice,sizeof(notice),"%s",
                     TR("Шпалери застосовано й збережено.","Wallpaper saved and applied."));
        }else{
            if(tex)glDeleteTextures(1,&tex);
            remove(WALLPAPER_TEMP);
            snprintf(notice,sizeof(notice),"%s",
                     TR("Не вдалося відкрити зображення. Спробуйте 1280×720 JPG.",
                        "Could not open image. Try a 1280x720 JPG."));
        }
    }
}
int wallpapers_count(void){return count;}
int wallpapers_revision(void){return revision;}
const WallpaperItem *wallpapers_item(int index){return index>=0 && index<count?&items[index]:NULL;}
int wallpapers_busy(void){
    if(!mutex)return 0;
    SDL_LockMutex(mutex);
    int working=request || busy;
    SDL_UnlockMutex(mutex);
    return working;
}
int wallpapers_apply(int index){
    if(!mutex || index<0 || index>=count || wallpapers_busy())return 0;
    SDL_LockMutex(mutex);
    request=JOB_APPLY;selected_index=index;request_sequence=++sequence;
    SDL_CondSignal(signal);
    snprintf(notice,sizeof(notice),"%s",
             TR("Застосування шпалер…","Applying wallpaper…"));
    SDL_UnlockMutex(mutex);
    return 1;
}
void wallpapers_reset(void){
    if(mutex){SDL_LockMutex(mutex);++sequence;SDL_UnlockMutex(mutex);}
    if(background)glDeleteTextures(1,&background);
    background=0;
    selected_name[0]=0;
    remove(WALLPAPER_FILE);remove(WALLPAPER_BACKUP);
    remove(WALLPAPER_NAME);remove(WALLPAPER_NAME_BACKUP);
    snprintf(notice,sizeof(notice),"%s",
             TR("Відновлено стандартний фон.","Default background restored."));
}
GLuint wallpapers_texture(void){return background;}
const char *wallpapers_selected(void){return selected_name;}
const char *wallpapers_message(void){return notice;}
void wallpapers_shutdown(void){
    if(mutex){
        SDL_LockMutex(mutex);quitting=1;
        if(signal)SDL_CondSignal(signal);
        SDL_UnlockMutex(mutex);
    }
    if(worker)SDL_WaitThread(worker,NULL);
    if(signal)SDL_DestroyCond(signal);
    if(mutex)SDL_DestroyMutex(mutex);
    signal=NULL;mutex=NULL;worker=NULL;
    if(background)glDeleteTextures(1,&background);
    background=0;
    remove(WALLPAPER_TEMP);
}
