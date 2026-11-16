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

/* Official Wallhaven SFW search API. Full-size sources are downloaded only
   after explicit selection, resized to 1280x720 locally and saved offline.
   A remote transform/Wallhaven thumbnail is fallback for oversized images. */
#define WALLPAPER_API "https://wallhaven.cc/api/v1/search"
#define WALLPAPER_RAW_PREFIX "https://w.wallhaven.cc/full/"
#define WALLPAPER_THUMB_PREFIX "https://th.wallhaven.cc/"
#define WALLPAPER_HOME "sdmc:/switch/uaplay"
#define WALLPAPER_CATALOG WALLPAPER_HOME "/wallpapers.json"
#define WALLPAPER_CATALOG_TMP WALLPAPER_HOME "/wallpapers.json.part"
#define WALLPAPER_FILE WALLPAPER_HOME "/wallpaper.bin"
#define WALLPAPER_TEMP WALLPAPER_HOME "/wallpaper.part"
#define WALLPAPER_SCALED WALLPAPER_HOME "/wallpaper.720p.part"
#define WALLPAPER_BACKUP WALLPAPER_HOME "/wallpaper.bak"
#define WALLPAPER_NAME WALLPAPER_HOME "/wallpaper.txt"
#define WALLPAPER_NAME_TMP WALLPAPER_HOME "/wallpaper.txt.part"
#define WALLPAPER_NAME_BACKUP WALLPAPER_HOME "/wallpaper.txt.bak"
#define CATALOG_BYTES_LIMIT (768u * 1024u)
/* Accept full-resolution 1080p/4K wallpaper downloads while bounding RAM.
   The decoded result is cropped/scaled to the Switch display once. */
#define IMAGE_BYTES_LIMIT (24u * 1024u * 1024u)
#define IMAGE_PROXY "https://images.weserv.nl/?url="
#define IMAGE_PROXY_LIMIT (5u * 1024u * 1024u)
#define IMAGE_PIXELS_LIMIT (12u * 1024u * 1024u)

enum { JOB_NONE, JOB_CATALOG, JOB_IMAGE };
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
static char selected_name[128], notice[192];
static char download_diagnostic[96];

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
    /* 18s max for a direct image; 9s for API/thumbnail/proxy attempts. */
    curl_easy_setopt(curl,CURLOPT_TIMEOUT,
                     buffer->limit>IMAGE_PROXY_LIMIT?18L:9L);
    curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,receive);
    curl_easy_setopt(curl,CURLOPT_WRITEDATA,buffer);
    CURLcode status=curl_easy_perform(curl);
    long http=0;curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&http);
    int success=status==CURLE_OK && http==200 && buffer->used>0;
    if(!success)snprintf(download_diagnostic,sizeof(download_diagnostic),
                          "HTTP %ld, curl %d",http,(int)status);
    return success;
}

/* The JSON catalog is generated by Awesome Wallpapers. Decode only the
   string fields we need and reject any non-GitHub image URL. */
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

/* Download from Wallhaven first. No third-party resizer is required for
   720p/1080p/4K images; perform the scale locally on the GL thread.
   For 8K+ originals try a proxy, then the Wallhaven preview as fallback. */
static int save_image(CURL *curl,const WallpaperItem *item){
    if(!item ||
       strncmp(item->url,WALLPAPER_RAW_PREFIX,strlen(WALLPAPER_RAW_PREFIX)))
        return 0;
    const int safe_original=
        (unsigned long long)item->width*(unsigned)item->height<=IMAGE_PIXELS_LIMIT &&
        item->bytes>0 && item->bytes<=12u*1024u*1024u;
    Buffer bytes={.limit=IMAGE_BYTES_LIMIT};
    /* Try the real Wallhaven image first whenever it is Switch-safe.
       If its CDN fails, use the remote 720p transform, then a thumbnail.
       Each fallback is time-bounded; no infinite busy spinner. */
    int ok=safe_original && fetch_data(curl,item->url,&bytes);
    if(!ok){
        free(bytes.data);
        bytes=(Buffer){.limit=IMAGE_PROXY_LIMIT};
        char resized[640];
        int len=snprintf(resized,sizeof(resized),
            IMAGE_PROXY "%s&w=1280&h=720&fit=cover&output=jpg&q=84",
            item->url+strlen("https://"));
        ok=len>0 && (size_t)len<sizeof(resized) &&
           fetch_data(curl,resized,&bytes);
    }
    if(!ok && !strncmp(item->thumbnail,WALLPAPER_THUMB_PREFIX,
                       strlen(WALLPAPER_THUMB_PREFIX))){
        free(bytes.data);
        bytes=(Buffer){.limit=IMAGE_PROXY_LIMIT};
        ok=fetch_data(curl,item->thumbnail,&bytes);
    }
    if(ok){
        FILE *f=fopen(WALLPAPER_TEMP,"wb");
        ok=f && fwrite(bytes.data,1,bytes.used,f)==bytes.used;
        if(f && fclose(f)!=0)ok=0;
    }
    free(bytes.data);
    if(!ok)remove(WALLPAPER_TEMP);
    return ok;
}
static int background_load(const char *path,GLuint *output,const char *save_path){
    FILE *file=fopen(path,"rb");
    if(!file)return 0;
    SDL_RWops *rw=SDL_RWFromFP(file,1);
    if(!rw){fclose(file);return 0;}
    SDL_Surface *original=IMG_Load_RW(rw,1);
    if(!original)return 0;
    unsigned long long pixels=(unsigned long long)original->w*(unsigned)original->h;
    if(original->w<1 || original->h<1 || pixels>IMAGE_PIXELS_LIMIT){
        SDL_FreeSurface(original);return 0;
    }
    /* Avoid a second full-size RGBA copy; the 4K source is already decoded. */
    SDL_Surface *scaled=SDL_CreateRGBSurfaceWithFormat(0,1280,720,32,SDL_PIXELFORMAT_ABGR8888);
    if(!scaled){SDL_FreeSurface(original);return 0;}
    SDL_Rect crop={0,0,original->w,original->h};
    if((long long)crop.w*720>=(long long)crop.h*1280){
        crop.w=(int)((long long)crop.h*1280/720);
        crop.x=(original->w-crop.w)/2;
    }else{
        crop.h=(int)((long long)crop.w*720/1280);
        crop.y=(original->h-crop.h)/2;
    }
    SDL_SetSurfaceBlendMode(original,SDL_BLENDMODE_NONE);
    int ok=SDL_BlitScaled(original,&crop,scaled,NULL)==0;
    SDL_FreeSurface(original);
    if(!ok){SDL_FreeSurface(scaled);return 0;}
    /* Persist the *720p* image, not a huge original that must be decoded
       again on every startup. SDL_image saves JPEG; BMP is a safe fallback. */
    if(save_path && IMG_SaveJPG(scaled,save_path,85)!=0 &&
       SDL_SaveBMP(scaled,save_path)!=0){
        SDL_FreeSurface(scaled);
        return 0;
    }
    GLuint texture=0;
    glGenTextures(1,&texture);
    if(texture){
        glBindTexture(GL_TEXTURE_2D,texture);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1280,720,0,GL_RGBA,GL_UNSIGNED_BYTE,scaled->pixels);
    }
    SDL_FreeSurface(scaled);
    *output=texture;
    return texture!=0;
}

static int commit_selection(const char *name){
    FILE *config=fopen(WALLPAPER_NAME_TMP,"wb");
    if(!config)return 0;
    int ok=fprintf(config,"%s\n",name)>0;
    if(fclose(config)!=0)ok=0;
    if(!ok){remove(WALLPAPER_NAME_TMP);return 0;}
    remove(WALLPAPER_BACKUP);
    int had_image=rename(WALLPAPER_FILE,WALLPAPER_BACKUP)==0;
    if(rename(WALLPAPER_SCALED,WALLPAPER_FILE)!=0){
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
        if(job==JOB_IMAGE && index>=0 && index<count)selected=catalog[index];
        SDL_UnlockMutex(mutex);
        int ok=job==JOB_CATALOG?load_catalog(curl,filter,query,page):
            job==JOB_IMAGE && selected.url[0]?save_image(curl,&selected):0;
        SDL_LockMutex(mutex);
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
    FILE *f=fopen(WALLPAPER_NAME,"rb");
    if(!f)f=fopen(WALLPAPER_NAME_BACKUP,"rb");
    if(f){
        if(fgets(selected_name,sizeof(selected_name),f)){
            selected_name[strcspn(selected_name,"\r\n")]=0;
            if(!background_load(WALLPAPER_FILE,&background,NULL))
                selected_name[0]=0;
        }
        fclose(f);
    }
    mutex=SDL_CreateMutex();
    condition=SDL_CreateCond();
    if(mutex && condition)thread=SDL_CreateThread(worker_main,"wallpapers",NULL);
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
    if(!count)wallpapers_browse(current_filter,current_query,api_page);
}
int wallpapers_current_page(void){return api_page;}
int wallpapers_last_page(void){return api_last_page;}
int wallpapers_revision(void){return revision;}

void wallpapers_poll(void){
    if(!mutex)return;
    SDL_LockMutex(mutex);
    int done=completed,job=result_job,ok=result,index=result_index,serial=result_generation;
    if(done){
        completed=0;SDL_CondSignal(condition);
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
    if(!done)return;
    if(job==JOB_CATALOG){
        snprintf(notice,sizeof(notice),"%s",ok?
            TR("Wallhaven: натисніть A для застосування.","Wallhaven: press A to apply."):
            TR("Wallhaven недоступний або пошук порожній.","Wallhaven unavailable or no results."));
    }else if(job==JOB_IMAGE){
        if(serial!=generation){remove(WALLPAPER_TEMP);return;}
        GLuint texture=0;
        int valid=ok && background_load(WALLPAPER_TEMP,&texture,WALLPAPER_SCALED);
        if(valid && index>=0 && index<count && commit_selection(catalog[index].name)){
            if(background)glDeleteTextures(1,&background);
            background=texture;
            snprintf(selected_name,sizeof(selected_name),"%s",catalog[index].name);
            remove(WALLPAPER_TEMP);
            snprintf(notice,sizeof(notice),"%s",
                     TR("Шпалери 1280×720 застосовано й збережено.","1280x720 wallpaper saved and applied."));
        }else{
            if(texture)glDeleteTextures(1,&texture);
            remove(WALLPAPER_TEMP);
            remove(WALLPAPER_SCALED);
            snprintf(notice,sizeof(notice),"%s (%s)",
                     TR("Не вдалося застосувати шпалери","Could not apply wallpaper"),
                     download_diagnostic[0]?download_diagnostic:
                     (ok?"decode":"unknown"));
        }
    }
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
    /* Every catalog image, including large 4K/8K originals, is requested
       as a Switch-sized image. The worker bounds both download and decode. */
    SDL_LockMutex(mutex);
    download_diagnostic[0]=0;
    requested=JOB_IMAGE;requested_index=index;requested_generation=++generation;
    snprintf(notice,sizeof(notice),"%s",TR("Завантаження шпалер…","Downloading wallpaper…"));
    SDL_CondSignal(condition);SDL_UnlockMutex(mutex);
    return 1;
}
void wallpapers_reset(void){
    if(mutex){SDL_LockMutex(mutex);generation++;SDL_UnlockMutex(mutex);}
    if(background)glDeleteTextures(1,&background);
    background=0;selected_name[0]=0;
    remove(WALLPAPER_NAME);remove(WALLPAPER_NAME_BACKUP);
    remove(WALLPAPER_FILE);remove(WALLPAPER_BACKUP);
    snprintf(notice,sizeof(notice),"%s",
             TR("Відновлено стандартний фон.","Default background restored."));
}
GLuint wallpapers_texture(void){return background;}
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
    if(background)glDeleteTextures(1,&background);
    background=0;
    remove(WALLPAPER_TEMP);
    remove(WALLPAPER_SCALED);
}
