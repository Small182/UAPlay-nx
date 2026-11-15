#include "update.h"
#include "version.h"

#include <curl/curl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <errno.h>

#ifdef __SWITCH__
#include <switch.h>
#else
#include <openssl/sha.h>
#endif

static int nro_path(const char *path) {
    size_t length=strlen(path);
    return length>4 && !strcasecmp(path+length-4,".nro");
}

/* hbmenu and the NSP forwarder pass the path of the loaded NRO as argv[0]. */
int update_path(const char *arg,char *path,size_t size) {
    if(!arg || !nro_path(arg))return 0;
    const char *absolute=!strncmp(arg,"sdmc:/",6)?arg+5:arg;
    if(*absolute!='/' || strstr(absolute,"/../") || strstr(absolute,"/./") ||
       strchr(absolute,':') || strchr(absolute,'\\'))return 0;
    int written=snprintf(path,size,"sdmc:%s",absolute);
    if(written<=0 || (size_t)written>=size)return 0;
    struct stat st;
    if(stat(path,&st)!=0 || !S_ISREG(st.st_mode))return 0;
    FILE *file=fopen(path,"rb");
    if(!file)return 0;
    char magic[4];
    int ok=fseek(file,16,SEEK_SET)==0 && fread(magic,1,sizeof(magic),file)==sizeof(magic) &&
           !memcmp(magic,"NRO0",sizeof(magic));
    fclose(file);
    return ok;
}

typedef struct {
    FILE *file;
    size_t bytes,limit;
    atomic_uint *progress;
    int write_error;
#ifdef __SWITCH__
    Sha256Context hash;
#else
    SHA256_CTX hash;
#endif
} Transfer;

static size_t write_asset(char *data,size_t size,size_t count,void *opaque) {
    Transfer *transfer=opaque;
    size_t bytes=size*count;
    if(bytes>transfer->limit-transfer->bytes)return 0;
    if(fwrite(data,1,bytes,transfer->file)!=bytes){
        transfer->write_error=1;
        return 0;
    }
#ifdef __SWITCH__
    sha256ContextUpdate(&transfer->hash,data,bytes);
#else
    SHA256_Update(&transfer->hash,data,bytes);
#endif
    transfer->bytes+=bytes;
    atomic_store(transfer->progress,(unsigned)(100ULL*transfer->bytes/transfer->limit));
    return bytes;
}

static int downloading(void *opaque,curl_off_t a,curl_off_t b,curl_off_t c,curl_off_t d) {
    (void)a;(void)b;(void)c;(void)d;
    return atomic_load((atomic_int *)opaque);
}

int update_download(const UpdateRelease *release,const char *path,
                    atomic_uint *progress,atomic_int *cancel) {
    if(!release->size || release->size>UPDATE_MAX_NRO_SIZE)return UPDATE_INVALID;
    char temp[1100],url[256];
    snprintf(temp,sizeof(temp),"%s.update",path);
    snprintf(url,sizeof(url),
             "https://github.com/Small182/UAPlay-nx/releases/download/%s/UAPlay.nro",release->tag);
    FILE *file=fopen(temp,"wb");
    if(!file)return UPDATE_SD;
    CURL *curl=curl_easy_init();
    Transfer transfer={.file=file,.limit=release->size,.progress=progress};
#ifdef __SWITCH__
    sha256ContextCreate(&transfer.hash);
#else
    SHA256_Init(&transfer.hash);
#endif
    CURLcode result=CURLE_FAILED_INIT;
    if(curl){
        curl_easy_setopt(curl,CURLOPT_URL,url);
        curl_easy_setopt(curl,CURLOPT_USERAGENT,"UAPlay/" UAPLAY_VERSION);
        curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION,1L);
        curl_easy_setopt(curl,CURLOPT_MAXREDIRS,4L);
#if LIBCURL_VERSION_NUM >= 0x075500
        curl_easy_setopt(curl,CURLOPT_REDIR_PROTOCOLS_STR,"https");
#else
        curl_easy_setopt(curl,CURLOPT_REDIR_PROTOCOLS,(long)CURLPROTO_HTTPS);
#endif
        curl_easy_setopt(curl,CURLOPT_FAILONERROR,1L);
        curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);
        curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT,8L);
        curl_easy_setopt(curl,CURLOPT_LOW_SPEED_LIMIT,1024L);
        curl_easy_setopt(curl,CURLOPT_LOW_SPEED_TIME,30L);
        curl_easy_setopt(curl,CURLOPT_TIMEOUT,900L);
#ifdef __SWITCH__
        curl_easy_setopt(curl,CURLOPT_IPRESOLVE,(long)CURL_IPRESOLVE_V4);
#endif
        curl_easy_setopt(curl,CURLOPT_NOPROGRESS,0L);
        curl_easy_setopt(curl,CURLOPT_XFERINFOFUNCTION,downloading);
        curl_easy_setopt(curl,CURLOPT_XFERINFODATA,cancel);
        curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,write_asset);
        curl_easy_setopt(curl,CURLOPT_WRITEDATA,&transfer);
        result=curl_easy_perform(curl);
        curl_easy_cleanup(curl);
    }
    int closed=fclose(file)==0;
    int status=UPDATE_NETWORK;
    if(!closed || transfer.write_error)status=UPDATE_SD;
    else if(result==CURLE_OK && transfer.bytes==release->size){
        unsigned char hash[32];
        char actual[65];
#ifdef __SWITCH__
        sha256ContextGetHash(&transfer.hash,hash);
#else
        SHA256_Final(hash,&transfer.hash);
#endif
        for(size_t i=0;i<sizeof(hash);i++)snprintf(actual+2*i,3,"%02x",hash[i]);
        if(!strcasecmp(actual,release->digest)){
            char magic[4];
            file=fopen(temp,"rb");
            int valid=file && fseek(file,16,SEEK_SET)==0 &&
                      fread(magic,1,sizeof(magic),file)==sizeof(magic) &&
                      !memcmp(magic,"NRO0",sizeof(magic));
            if(file)fclose(file);
            if(valid)return UPDATE_OK;
        }
        status=UPDATE_INVALID;
    }
    remove(temp);
    return status;
}

/* Switch renames do not overwrite an existing file. Keep the old binary
   beside the new one and restore it if the second rename fails. */
int update_install(const char *path) {
    char temp[1100],backup[1100];
    snprintf(temp,sizeof(temp),"%s.update",path);
    snprintf(backup,sizeof(backup),"%s.bak",path);
    if(remove(backup)!=0 && errno!=ENOENT)return 0;
    if(rename(path,backup)!=0)return 0;
    if(rename(temp,path)==0)return 1;
    rename(backup,path);
    return 0;
}
