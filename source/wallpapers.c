#include "wallpapers.h"
#include "i18n.h"

#include <SDL.h>
#include <SDL_image.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <errno.h>

/* Official Wallhaven SFW API. Selected original images are saved byte-for-byte
   to the SD card. Decoding happens in the worker thread; the GL renderer
   center-crops at draw time, preserving the source image's aspect ratio. */
#define WALLPAPER_API "https://wallhaven.cc/api/v1/search"
#define WALLPAPER_RAW_PREFIX "https://w.wallhaven.cc/full/"
#define WALLPAPER_THUMB_PREFIX "https://th.wallhaven.cc/"
#define WALLPAPER_HOME "sdmc:/switch/uaplay"
#define WALLPAPER_CATALOG WALLPAPER_HOME "/wallpapers.json"
#define WALLPAPER_CATALOG_TMP WALLPAPER_HOME "/wallpapers.json.part"
#define WALLPAPER_FILE WALLPAPER_HOME "/wallpaper.bin"
#define WALLPAPER_TEMP WALLPAPER_HOME "/wallpaper.part"
#define WALLPAPER_BACKUP WALLPAPER_HOME "/wallpaper.bak"
#define WALLPAPER_NAME WALLPAPER_HOME "/wallpaper.txt"
#define WALLPAPER_NAME_TMP WALLPAPER_HOME "/wallpaper.txt.part"
#define WALLPAPER_NAME_BACKUP WALLPAPER_HOME "/wallpaper.txt.bak"
#define CATALOG_BYTES_LIMIT (768u * 1024u)
/* Bound memory and texture allocations for Switch hardware; never resize
   or re-encode a wallpaper. Unsupported originals fail with a clear error. */
#define IMAGE_BYTES_LIMIT (24u * 1024u * 1024u)
#define IMAGE_PIXELS_LIMIT (12u * 1024u * 1024u)

enum { JOB_NONE, JOB_CATALOG, JOB_IMAGE, JOB_RESTORE };
static WallpaperItem catalog[WALLPAPERS_MAX], pending[WALLPAPERS_MAX];
static int count, pending_count, requested, busy, completed, result, result_job;
static int result_index, requested_index, generation, requested_generation, result_generation;
static int api_page=1,api_last_page=1,pending_page=1,pending_last_page=1,revision;
static int current_filter,requested_filter,requested_page=1;
static char current_query[96],requested_query[96];
static Uint32 api_last_request;
static SDL_mutex *mutex;
static SDL_cond *condition;
static SDL_Thread *thread;
static int quitting;
static GLuint background;
static int background_width,background_height,texture_limit;
static SDL_Surface *pending_surface;
static int gallery_pending;
static char selected_name[128], notice[192];
static char download_diagnostic[96];
/* Images are decoded in the worker. Only the OpenGL upload runs on UI. */
enum { PHASE_IDLE, PHASE_DOWNLOAD, PHASE_DECODE };
static SDL_atomic_t image_phase;
static Uint32 image_started_at;

typedef struct {
    unsigned char *data;
    size_t used, capacity, limit;
} Buffer;

static size_t receive(char *data,size_t size,size_t n,void *opaque) {
    Buffer *b=opaque;
    if(size && n>b->limit/size)return 0;
    size_t bytes=size*n;
    if(bytes>b->limit-b->used)return 0;
    if(b->used+bytes+1>b->capacity){
        size_t cap=b->capacity?b->capacity*2:8192;
        while(cap<b->used+bytes+1)cap*=2;
        if(cap>b->limit+1)cap=b->limit+1;
        unsigned char *next=realloc(b->data,cap);
        if(!next)return 0;
        b->data=next;b->capacity=cap;
    }
    memcpy(b->data+b->used,data,bytes);
    b->used+=bytes;b->data[b->used]=0;
    return bytes;
}

static void curl_setup(CURL *curl,const char *url) {
    curl_easy_reset(curl);
    curl_easy_setopt(curl,CURLOPT_URL,url);
    curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(curl,CURLOPT_USERAGENT,
        "Mozilla/5.0 (Nintendo Switch; UAPlay-nx) AppleWebKit/537.36");
    curl_easy_setopt(curl,CURLOPT_REFERER,"https://wallhaven.cc/");
    curl_easy_setopt(curl,CURLOPT_ACCEPT_ENCODING,"");
    curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION,1L);
    curl_easy_setopt(curl,CURLOPT_MAXREDIRS,3L);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl,CURLOPT_REDIR_PROTOCOLS_STR,"https");
#else
    curl_easy_setopt(curl,CURLOPT_REDIR_PROTOCOLS,(long)CURLPROTO_HTTPS);
#endif
    /* Network outages and failed CDN fallbacks must never leave UAPlay
       showing a busy state for minutes. Every attempt has a hard timeout. */
    curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT,4L);
    curl_easy_setopt(curl,CURLOPT_TIMEOUT,12L);
    curl_easy_setopt(curl,CURLOPT_LOW_SPEED_LIMIT,1024L);
    curl_easy_setopt(curl,CURLOPT_LOW_SPEED_TIME,5L);
#ifdef __SWITCH__
    curl_easy_setopt(curl,CURLOPT_IPRESOLVE,(long)CURL_IPRESOLVE_V4);
#endif
    curl_easy_setopt(curl,CURLOPT_FAILONERROR,1L);
}

static int fetch_data(CURL *curl,const char *url,Buffer *buffer) {
    if(!curl)return 0;
    curl_setup(curl,url);
    curl_easy_setopt(curl,CURLOPT_TIMEOUT,
                     buffer->limit>CATALOG_BYTES_LIMIT?22L:9L);
    curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,receive);
    curl_easy_setopt(curl,CURLOPT_WRITEDATA,buffer);
    CURLcode status=curl_easy_perform(curl);
    long http=0;curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&http);
    int success=status==CURLE_OK && http==200 && buffer->used>0;
    if(!success)snprintf(download_diagnostic,sizeof(download_diagnostic),
                          "HTTP %ld, curl %d",http,(int)status);
    return success;
}

/* Read only required Wallhaven fields; keep original CDN URLs. */
static const char *find_field(const char *begin,const char *end,const char *name){
    char key[48];snprintf(key,sizeof(key),"\"%s\"",name);
    const char *hit=strstr(begin,key);
    if(!hit || hit>=end)return NULL;
    hit+=strlen(key);
    while(hit<end && isspace((unsigned char)*hit))hit++;
    if(hit>=end || *hit++!=':')return NULL;
    while(hit<end && isspace((unsigned char)*hit))hit++;
    return hit<end?hit:NULL;
}
static int json_string(const char *p,const char *end,char *out,size_t size){
    if(!p || p>=end || *p!='"' || !size)return 0;
    p++;size_t n=0;
    while(p<end && *p!='"'){
        unsigned char c=(unsigned char)*p++;
        if(c=='\\'){
            if(p>=end)return 0;
            c=(unsigned char)*p++;
            if(c=='n'||c=='r'||c=='t')c=' ';
            else if(c=='u')return 0; /* no escaped Unicode in filenames/URLs */
        }
        if(c<32 || n+1>=size)return 0;
        out[n++]=(char)c;
    }
    if(p>=end || *p!='"')return 0;
    out[n]=0;
    return n>0;
}
static int number_field(const char *begin,const char *end,const char *field,unsigned *out){
    const char *p=find_field(begin,end,field);
    if(!p || p>=end || !isdigit((unsigned char)*p))return 0;
    unsigned long n=strtoul(p,NULL,10);
    if(n>0xffffffffUL)return 0;
    *out=(unsigned)n;
    return 1;
}
static void tag_field(const char *begin,const char *end,char *out,size_t size){
    const char *p=find_field(begin,end,"tags");size_t n=0;
    out[0]=0;
    if(!p || *p!='[')return;
    for(p++;p<end && *p!=']';p++){
        if(*p!='"')continue;
        char tag[48];
        if(!json_string(p,end,tag,sizeof(tag)))break;
        size_t length=strlen(tag);
        if(length && n+length+3<size){
            if(n)out[n++]=',';
            memcpy(out+n,tag,length);n+=length;out[n]=0;
        }
        p=strchr(p+1,'"');
        if(!p)return;
    }
}

static const char *object_end(const char *p){
    int depth=0,quoted=0,escaped=0;
    for(;*p;p++){
        if(escaped){escaped=0;continue;}
        if(quoted && *p=='\\'){escaped=1;continue;}
        if(*p=='"'){quoted=!quoted;continue;}
        if(quoted)continue;
        if(*p=='{')depth++;
        else if(*p=='}' && --depth==0)return p+1;
    }
    return NULL;
}
static int parse_catalog(const char *json,WallpaperItem *out,int *last){
    if(!json || *json!='{')return 0;
    const char *meta=strstr(json,"\"meta\"");
    *last=1;
    if(meta){
        unsigned total=1;
        if(number_field(meta,meta+strlen(meta),"last_page",&total))
            *last=total>1000?1000:total?total:1;
    }
    const char *data=strstr(json,"\"data\"");
    if(!data || !(data=strchr(data,'[')))return 0;
    const char *p=data+1;
    int used=0;
    while(*p && used<WALLPAPERS_MAX){
        while(isspace((unsigned char)*p) || *p==',')p++;
        if(*p==']')break;
        if(*p!='{')return 0;
        const char *end=object_end(p);
        if(!end)return 0;
        WallpaperItem item={0};
        char purity[12]={0}, category[48]={0};
        unsigned w=0,h=0,bytes=0;
        json_string(find_field(p,end,"purity"),end,purity,sizeof(purity));
        json_string(find_field(p,end,"category"),end,category,sizeof(category));
        if(!strcmp(purity,"sfw") &&
           json_string(find_field(p,end,"id"),end,item.id,sizeof(item.id)) &&
           json_string(find_field(p,end,"path"),end,item.url,sizeof(item.url)) &&
           json_string(find_field(p,end,"large"),end,item.thumbnail,sizeof(item.thumbnail)) &&
           number_field(p,end,"dimension_x",&w) &&
           number_field(p,end,"dimension_y",&h) &&
           number_field(p,end,"file_size",&bytes) &&
           strlen(item.id)==6 &&
           strspn(item.id,"0123456789abcdefghijklmnopqrstuvwxyz")==6 &&
           !strncmp(item.url,WALLPAPER_RAW_PREFIX,strlen(WALLPAPER_RAW_PREFIX)) &&
           !strncmp(item.thumbnail,WALLPAPER_THUMB_PREFIX,strlen(WALLPAPER_THUMB_PREFIX)) &&
           !strstr(item.url,"..") && !strchr(item.url,'?') &&
           !strstr(item.thumbnail,"..") && !strchr(item.thumbnail,'?') &&
           w>=1280 && h>=720 && w<=16384 && h<=16384){
            snprintf(item.name,sizeof(item.name),"Wallhaven %s",item.id);
            snprintf(item.tags,sizeof(item.tags),"%s",category);
            item.width=(int)w;item.height=(int)h;item.bytes=bytes;
            out[used++]=item;
        }
        p=end;
    }
    return used;
}
static int load_json_file(const char *path,Buffer *buffer){
    FILE *f=fopen(path,"rb");
    if(!f)return 0;
    unsigned char chunk[4096];
    size_t n;
    while((n=fread(chunk,1,sizeof(chunk),f))>0){
        if(receive((char*)chunk,1,n,buffer)!=n)break;
    }
    int ok=feof(f) && !ferror(f) && buffer->used>0;
    fclose(f);return ok;
}
static void save_json(const Buffer *buffer){
    FILE *f=fopen(WALLPAPER_CATALOG_TMP,"wb");
    if(!f)return;
    int ok=fwrite(buffer->data,1,buffer->used,f)==buffer->used;
    if(fclose(f)!=0)ok=0;
    if(ok){
        char backup[160];snprintf(backup,sizeof(backup),"%s.bak",WALLPAPER_CATALOG);
        remove(backup);
        int old=rename(WALLPAPER_CATALOG,backup)==0;
        if(rename(WALLPAPER_CATALOG_TMP,WALLPAPER_CATALOG)!=0 && old)
            rename(backup,WALLPAPER_CATALOG);
        else remove(backup);
    }
    remove(WALLPAPER_CATALOG_TMP);
}

static int load_catalog(CURL *curl,int filter,const char *user_query,int page){
    char keywords[200]={0};
    const char *preset=filter==1?"dark":filter==2?"minimal":"";
    if(*preset && *user_query)snprintf(keywords,sizeof(keywords),"%s %s",preset,user_query);
    else snprintf(keywords,sizeof(keywords),"%s",*preset?preset:user_query);
    char *escaped=curl?curl_easy_escape(curl,keywords,0):NULL;
    if(!escaped)return 0;
    char url[850];
    int size=snprintf(url,sizeof(url),
        WALLPAPER_API "?purity=100&categories=%s&ratios=16x9&atleast=1280x720"
        "&sorting=%s&page=%d&q=%s",
        filter==3?"010":"110",*keywords?"relevance":"toplist",page,escaped);
    curl_free(escaped);
    if(size<=0 || (size_t)size>=sizeof(url))return 0;
    Buffer data={.limit=CATALOG_BYTES_LIMIT};
    int ok=fetch_data(curl,url,&data);
    pending_last_page=1;
    pending_count=ok?parse_catalog((const char*)data.data,pending,&pending_last_page):0;
    if(pending_count && !filter && !*user_query && page==1)save_json(&data);
    free(data.data);
    if(!pending_count && !filter && !*user_query && page==1){
        Buffer cached={.limit=CATALOG_BYTES_LIMIT};
        if(load_json_file(WALLPAPER_CATALOG,&cached))
            pending_count=parse_catalog((const char*)cached.data,pending,&pending_last_page);
        free(cached.data);
    }
    pending_page=page;
    return pending_count>0;
}

/* Downloads the original file as-is. No proxy resizing, image encoding,
   center cropping, or other 720p processing occurs. */
static int save_image(CURL *curl,const WallpaperItem *item){
    if(!item ||
       strncmp(item->url,WALLPAPER_RAW_PREFIX,strlen(WALLPAPER_RAW_PREFIX)))
        return 0;
    Buffer bytes={.limit=IMAGE_BYTES_LIMIT};
    int ok=fetch_data(curl,item->url,&bytes);
    if(ok){
        FILE *file=fopen(WALLPAPER_TEMP,"wb");
        ok=file && fwrite(bytes.data,1,bytes.used,file)==bytes.used;
        if(file && fclose(file)!=0)ok=0;
        if(!ok)snprintf(download_diagnostic,sizeof(download_diagnostic),
                        "SD write failed");
    }
    free(bytes.data);
    if(!ok)remove(WALLPAPER_TEMP);
    return ok;
}

/* CPU-only image decode in the background worker. The source file remains
   unchanged on disk. Rendering performs a UV center-crop without resampling
   or allocating a secondary, reduced-resolution image. */
static SDL_Surface *decode_wallpaper(const char *path){
    FILE *file=fopen(path,"rb");
    if(!file)return NULL;
    SDL_RWops *rw=SDL_RWFromFP(file,1);
    if(!rw){fclose(file);return NULL;}
    SDL_Surface *decoded=IMG_Load_RW(rw,1);
    if(!decoded)return NULL;
    unsigned long long pixels=(unsigned long long)decoded->w*(unsigned)decoded->h;
    if(decoded->w<1 || decoded->h<1 ||
       pixels>IMAGE_PIXELS_LIMIT ||
       decoded->w>texture_limit || decoded->h>texture_limit){
        SDL_FreeSurface(decoded);
        return NULL;
    }
    if(decoded->format->format==SDL_PIXELFORMAT_ABGR8888)
        return decoded;
    SDL_Surface *rgba=SDL_ConvertSurfaceFormat(
        decoded,SDL_PIXELFORMAT_ABGR8888,0);
    SDL_FreeSurface(decoded);
    return rgba;
}
/* OpenGL operations only on the UI thread. Worker supplies a fully decoded,
   format-converted surface, so the UI does not decode or resize it. */
static GLuint upload_wallpaper(SDL_Surface *surface){
    if(!surface || surface->w<1 || surface->h<1 ||
       surface->w>texture_limit || surface->h>texture_limit)
        return 0;
    GLuint texture=0;
    glGenTextures(1,&texture);
    if(!texture)return 0;
    glBindTexture(GL_TEXTURE_2D,texture);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,surface->w,surface->h,0,
                 GL_RGBA,GL_UNSIGNED_BYTE,surface->pixels);
    if(glGetError()!=GL_NO_ERROR){
        glDeleteTextures(1,&texture);
        return 0;
    }
    return texture;
}

static int commit_selection(const char *name){
    FILE *config=fopen(WALLPAPER_NAME_TMP,"wb");
    if(!config)return 0;
    int ok=fprintf(config,"%s\n",name)>0;
    if(fclose(config)!=0)ok=0;
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
    remove(WALLPAPER_BACKUP);
    remove(WALLPAPER_NAME_BACKUP);
    return 1;
}


static int worker_main(void *opaque){
    (void)opaque;
    CURL *curl=curl_easy_init();
    for(;;){
        SDL_LockMutex(mutex);
        while(!quitting && (!requested || completed))SDL_CondWait(condition,mutex);
        if(quitting){SDL_UnlockMutex(mutex);break;}
        int job=requested,index=requested_index,serial=requested_generation;
        int filter=requested_filter,page=requested_page;
        char query[96];
        snprintf(query,sizeof(query),"%s",requested_query);
        requested=0;busy=job;
        WallpaperItem selected={0};
        if(job==JOB_IMAGE && index>=0 && index<count)
            selected=catalog[index];
        SDL_UnlockMutex(mutex);
        SDL_Surface *surface=NULL;
        int ok=0;
        if(job==JOB_CATALOG){
            ok=load_catalog(curl,filter,query,page);
        }else if(job==JOB_IMAGE && selected.url[0]){
            SDL_AtomicSet(&image_phase,PHASE_DOWNLOAD);
            ok=save_image(curl,&selected);
            if(ok){
                SDL_AtomicSet(&image_phase,PHASE_DECODE);
                surface=decode_wallpaper(WALLPAPER_TEMP);
                ok=surface!=NULL;
                if(!ok)snprintf(download_diagnostic,sizeof(download_diagnostic),
                                "Image format, size or memory limit");
            }
        }else if(job==JOB_RESTORE){
            surface=decode_wallpaper(WALLPAPER_FILE);
            ok=surface!=NULL;
        }
        SDL_LockMutex(mutex);
        pending_surface=surface;
        result=ok;result_job=job;result_index=index;result_generation=serial;
        completed=1;busy=0;
        SDL_UnlockMutex(mutex);
    }
    if(curl)curl_easy_cleanup(curl);
    return 0;
}
void wallpapers_init(void){
    mkdir("sdmc:/switch",0777);
    mkdir(WALLPAPER_HOME,0777);
    glGetIntegerv(GL_MAX_TEXTURE_SIZE,&texture_limit);
    if(texture_limit<1)texture_limit=4096;
    FILE *file=fopen(WALLPAPER_NAME,"rb");
    if(!file)file=fopen(WALLPAPER_NAME_BACKUP,"rb");
    if(file){
        if(fgets(selected_name,sizeof(selected_name),file))
            selected_name[strcspn(selected_name,"\r\n")]=0;
        fclose(file);
    }
    mutex=SDL_CreateMutex();
    condition=SDL_CreateCond();
    if(mutex && condition)thread=SDL_CreateThread(worker_main,"wallpapers",NULL);
    if(thread && selected_name[0]){
        SDL_LockMutex(mutex);
        requested=JOB_RESTORE;
        requested_generation=++generation;
        SDL_CondSignal(condition);
        SDL_UnlockMutex(mutex);
    }
    notice[0]=0;
}

int wallpapers_browse(int filter,const char *query,int page){
    if(!mutex || !thread || page<1 || page>1000 || filter<0 || filter>3)return 0;
    if(!query)query="";
    SDL_LockMutex(mutex);
    int blocked=requested || busy || completed ||
                (api_last_request && SDL_GetTicks()-api_last_request<1250u);
    if(blocked){SDL_UnlockMutex(mutex);return 0;}
    requested_filter=filter;requested_page=page;
    snprintf(requested_query,sizeof(requested_query),"%s",query);
    requested=JOB_CATALOG;
    requested_generation=++generation;
    api_last_request=SDL_GetTicks();
    snprintf(notice,sizeof(notice),"%s",TR("Завантаження Wallhaven…","Loading Wallhaven…"));
    SDL_CondSignal(condition);
    SDL_UnlockMutex(mutex);
    return 1;
}
void wallpapers_open(void){
    if(count)return;
    /* Startup restoration may still be decoding the saved original.
       Queue gallery discovery rather than dropping the first open request. */
    gallery_pending=1;
    if(wallpapers_browse(current_filter,current_query,api_page))
        gallery_pending=0;
}
int wallpapers_current_page(void){return api_page;}
int wallpapers_last_page(void){return api_last_page;}
int wallpapers_revision(void){return revision;}

void wallpapers_poll(void){
    if(!mutex)return;
    SDL_LockMutex(mutex);
    int done=completed,job=result_job,ok=result,index=result_index,serial=result_generation;
    SDL_Surface *surface=NULL;
    if(done){
        surface=pending_surface;
        pending_surface=NULL;
        completed=0;
        SDL_CondSignal(condition);
        if(job==JOB_CATALOG && ok){
            count=pending_count;
            memcpy(catalog,pending,(size_t)count*sizeof(catalog[0]));
            api_page=pending_page;api_last_page=pending_last_page;
            current_filter=requested_filter;
            snprintf(current_query,sizeof(current_query),"%s",requested_query);
            revision++;
        }
    }
    SDL_UnlockMutex(mutex);
    if(!done){
        int phase=SDL_AtomicGet(&image_phase);
        if(phase==PHASE_DOWNLOAD || phase==PHASE_DECODE){
            unsigned seconds=(SDL_GetTicks()-image_started_at)/1000u;
            snprintf(notice,sizeof(notice),
                phase==PHASE_DOWNLOAD?
                TR("Отримання оригіналу… %u с","Downloading original… %u s"):
                TR("Відкриття зображення… %u с","Decoding image… %u s"),
                seconds);
        }
        if(gallery_pending &&
           wallpapers_browse(current_filter,current_query,api_page))
            gallery_pending=0;
        return;
    }
    if(job==JOB_IMAGE)SDL_AtomicSet(&image_phase,PHASE_IDLE);
    if(job==JOB_CATALOG){
        snprintf(notice,sizeof(notice),"%s",ok?
            TR("Wallhaven: натисніть A для застосування.","Wallhaven: press A to apply."):
            TR("Wallhaven недоступний або пошук порожній.","Wallhaven unavailable or no results."));
    }else if(job==JOB_IMAGE || job==JOB_RESTORE){
        if(serial!=generation){
            if(surface)SDL_FreeSurface(surface);
            if(job==JOB_IMAGE)remove(WALLPAPER_TEMP);
        }else{
            GLuint texture=ok?upload_wallpaper(surface):0;
            if(job==JOB_RESTORE){
                if(texture){
                    if(background)glDeleteTextures(1,&background);
                    background=texture;
                    background_width=surface->w;
                    background_height=surface->h;
                }else selected_name[0]=0;
            }else if(texture && index>=0 && index<count &&
                     commit_selection(catalog[index].name)){
                if(background)glDeleteTextures(1,&background);
                background=texture;
                background_width=surface->w;
                background_height=surface->h;
                snprintf(selected_name,sizeof(selected_name),"%s",catalog[index].name);
                snprintf(notice,sizeof(notice),"%s",
                         TR("Оригінальні шпалери встановлено.","Original wallpaper applied."));
            }else{
                if(texture)glDeleteTextures(1,&texture);
                remove(WALLPAPER_TEMP);
                snprintf(notice,sizeof(notice),"%s (%s)",
                         TR("Не вдалося застосувати шпалери","Could not apply wallpaper"),
                         download_diagnostic[0]?download_diagnostic:
                         TR("Формат або розмір не підтримується",
                            "Unsupported image size or format"));
            }
            if(surface)SDL_FreeSurface(surface);
        }
    }
    if(gallery_pending &&
       wallpapers_browse(current_filter,current_query,api_page))
        gallery_pending=0;
}
int wallpapers_count(void){return count;}
const WallpaperItem *wallpapers_item(int i){return i>=0 && i<count?&catalog[i]:NULL;}
int wallpapers_busy(void){
    if(!mutex)return 0;
    SDL_LockMutex(mutex);int pending_work=requested||busy||completed;
    SDL_UnlockMutex(mutex);return pending_work;
}
int wallpapers_apply(int index){
    if(!mutex || index<0 || index>=count || wallpapers_busy())return 0;
    const WallpaperItem *item=&catalog[index];
    if(item->width<1 || item->height<1 ||
       item->width>texture_limit || item->height>texture_limit ||
       (unsigned long long)item->width*(unsigned)item->height>IMAGE_PIXELS_LIMIT ||
       item->bytes==0 || item->bytes>IMAGE_BYTES_LIMIT){
        snprintf(notice,sizeof(notice),"%s",
                 TR("Оригінал завеликий для пам’яті Switch.",
                    "Original exceeds Switch image memory limits."));
        return 0;
    }
    SDL_LockMutex(mutex);
    download_diagnostic[0]=0;
    image_started_at=SDL_GetTicks();
    SDL_AtomicSet(&image_phase,PHASE_DOWNLOAD);
    requested=JOB_IMAGE;requested_index=index;requested_generation=++generation;
    snprintf(notice,sizeof(notice),"%s",TR("Завантаження шпалер…","Downloading wallpaper…"));
    SDL_CondSignal(condition);SDL_UnlockMutex(mutex);
    return 1;
}
void wallpapers_reset(void){
    if(mutex){SDL_LockMutex(mutex);generation++;SDL_UnlockMutex(mutex);}
    SDL_AtomicSet(&image_phase,PHASE_IDLE);
    if(background)glDeleteTextures(1,&background);
    background=0;background_width=background_height=0;selected_name[0]=0;
    remove(WALLPAPER_NAME);remove(WALLPAPER_NAME_BACKUP);
    remove(WALLPAPER_FILE);remove(WALLPAPER_BACKUP);
    snprintf(notice,sizeof(notice),"%s",
             TR("Відновлено стандартний фон.","Default background restored."));
}
GLuint wallpapers_texture(void){return background;}
int wallpapers_width(void){return background_width;}
int wallpapers_height(void){return background_height;}
const char *wallpapers_selected(void){return selected_name;}
const char *wallpapers_message(void){return notice;}
void wallpapers_shutdown(void){
    if(mutex){
        SDL_LockMutex(mutex);quitting=1;
        if(condition)SDL_CondSignal(condition);
        SDL_UnlockMutex(mutex);
    }
    if(thread)SDL_WaitThread(thread,NULL);
    if(condition)SDL_DestroyCond(condition);
    if(mutex)SDL_DestroyMutex(mutex);
    condition=NULL;mutex=NULL;thread=NULL;
    if(pending_surface)SDL_FreeSurface(pending_surface);
    pending_surface=NULL;
    if(background)glDeleteTextures(1,&background);
    background=0;
    remove(WALLPAPER_TEMP);
}
