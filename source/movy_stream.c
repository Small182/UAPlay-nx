#include "movy_stream.h"
#include "catalog.h"

#include <arpa/inet.h>
#include <curl/curl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

enum { MAX_RESOURCES=24000, MAX_PLAYLIST=1024*1024 };
typedef struct {
    char *url;
    const char *suffix;
    char *playlist;
    size_t playlist_size;
} Resource;
typedef struct {
    char *text;
    size_t length,capacity;
} Playlist;

static int listener=-1;
static unsigned port;
static pthread_t worker;
static atomic_int stopping;
static Resource resources[MAX_RESOURCES];
static unsigned resource_count;

static int send_all(int fd,const char *data,size_t size){
    while(size && !atomic_load(&stopping)){
#ifdef MSG_NOSIGNAL
        ssize_t n=send(fd,data,size,MSG_NOSIGNAL);
#else
        ssize_t n=send(fd,data,size,0);
#endif
        if(n<=0)return 0;
        data+=n;
        size-=(size_t)n;
    }
    return size==0;
}

static void response(int fd,int status,const char *type,long long size){
    char header[160];
    int n=snprintf(header,sizeof(header),
                   "HTTP/1.1 %d %s\r\nConnection: close\r\nContent-Type: %s\r\n",
                   status,status==200?"OK":"Unavailable",type);
    if(size>=0)n+=snprintf(header+n,sizeof(header)-(size_t)n,"Content-Length: %lld\r\n",size);
    memcpy(header+n,"\r\n",2);
    send_all(fd,header,(size_t)n+2);
}

static size_t write_playlist(char *data,size_t size,size_t count,void *opaque){
    Playlist *p=opaque;
    size_t n=size*count;
    if(n>MAX_PLAYLIST-p->length)return 0;
    if(p->length+n+1>p->capacity){
        size_t capacity=(p->length+n+1)*2;
        char *buffer=realloc(p->text,capacity);
        if(!buffer)return 0;
        p->text=buffer;
        p->capacity=capacity;
    }
    memcpy(p->text+p->length,data,n);
    p->length+=n;
    p->text[p->length]=0;
    return n;
}

typedef struct {
    int fd,started;
    CURL *curl;
} Segment;

static size_t write_segment(char *data,size_t size,size_t count,void *opaque){
    Segment *segment=opaque;
    if(!segment->started){
        long code=0;
        curl_easy_getinfo(segment->curl,CURLINFO_RESPONSE_CODE,&code);
        if(code<200 || code>=300)return 0;
        curl_off_t length=-1;
        curl_easy_getinfo(segment->curl,CURLINFO_CONTENT_LENGTH_DOWNLOAD_T,&length);
        response(segment->fd,200,"video/mp4",(long long)length);
        segment->started=1;
    }
    return send_all(segment->fd,data,size*count)?size*count:0;
}

static int cancelled(void *unused,curl_off_t a,curl_off_t b,curl_off_t c,curl_off_t d){
    (void)unused;(void)a;(void)b;(void)c;(void)d;
    return atomic_load(&stopping);
}

static const char *resource_suffix(const char *url){
    size_t end=strcspn(url,"?#");
    if(end>=5 && !memcmp(url+end-5,".m3u8",5))return "playlist.m3u8";
    if(end>=4 && !memcmp(url+end-4,".mp4",4))return "init.mp4";
    if(end>=4 && !memcmp(url+end-4,".m4s",4))return "segment.m4s";
    return NULL;
}

static int append_resource(Playlist *out,const char *base,const char *ref,size_t length){
    char reference[2048];
    if(!length || length>=sizeof(reference) || resource_count>=MAX_RESOURCES)return 0;
    memcpy(reference,ref,length);
    reference[length]=0;
    CURLU *parsed=curl_url();
    if(!parsed)return 0;
    char *url=NULL;
    if(!curl_url_set(parsed,CURLUPART_URL,base,0) &&
       !curl_url_set(parsed,CURLUPART_URL,reference,0))
        curl_url_get(parsed,CURLUPART_URL,&url,0);
    curl_url_cleanup(parsed);
    const char *suffix=url && !strncmp(url,"https://",8)?resource_suffix(url):NULL;
    if(!suffix){curl_free(url);return 0;}
    char *copy=strdup(url);
    curl_free(url);
    if(!copy)return 0;
    unsigned id=resource_count++;
    resources[id]=(Resource){.url=copy,.suffix=suffix};
    char local[96];
    int n=snprintf(local,sizeof(local),"http://127.0.0.1:%u/%u/%s",port,id,suffix);
    return write_playlist(local,1,(size_t)n,out)==(size_t)n;
}

static int rewrite_playlist(Playlist *out,Playlist *input,const char *base){
    if(!input->text || strncmp(input->text,"#EXTM3U",7))return 0;
    char *save=NULL;
    for(char *line=strtok_r(input->text,"\r\n",&save);line;line=strtok_r(NULL,"\r\n",&save)){
        if(*line!='#'){
            if(!append_resource(out,base,line,strlen(line)))return 0;
        }else{
            const char *uri=strstr(line,"URI=\"");
            if(uri){
                uri+=5;
                const char *end=strchr(uri,'"');
                if(!end || write_playlist(line,1,(size_t)(uri-line),out)!=(size_t)(uri-line) ||
                   !append_resource(out,base,uri,(size_t)(end-uri)) ||
                   write_playlist((char *)end,1,strlen(end),out)!=strlen(end))return 0;
            }else if(write_playlist(line,1,strlen(line),out)!=strlen(line))return 0;
        }
        if(write_playlist("\n",1,1,out)!=1)return 0;
    }
    return 1;
}

static void serve(int fd,CURL *curl,Resource *resource){
    if(resource->playlist){
        response(fd,200,"application/vnd.apple.mpegurl",(long long)resource->playlist_size);
        send_all(fd,resource->playlist,resource->playlist_size);
        return;
    }
    curl_easy_reset(curl);
    curl_easy_setopt(curl,CURLOPT_URL,resource->url);
    curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION,1L);
    curl_easy_setopt(curl,CURLOPT_MAXREDIRS,4L);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl,CURLOPT_REDIR_PROTOCOLS_STR,"https");
#else
    curl_easy_setopt(curl,CURLOPT_REDIR_PROTOCOLS,CURLPROTO_HTTPS);
#endif
    curl_easy_setopt(curl,CURLOPT_REFERER,MOVY_ORIGIN "/");
    struct curl_slist *headers=curl_slist_append(NULL,"Origin: " MOVY_ORIGIN);
    curl_easy_setopt(curl,CURLOPT_HTTPHEADER,headers);
    curl_easy_setopt(curl,CURLOPT_ACCEPT_ENCODING,"");
    curl_easy_setopt(curl,CURLOPT_HTTP_VERSION,CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl,CURLOPT_USERAGENT,"Mozilla/5.0 (Nintendo Switch; UAPlay/1.0)");
#ifdef __SWITCH__
    curl_easy_setopt(curl,CURLOPT_IPRESOLVE,(long)CURL_IPRESOLVE_V4);
#endif
    curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT,9L);
    curl_easy_setopt(curl,CURLOPT_TIMEOUT,18L);
    curl_easy_setopt(curl,CURLOPT_FAILONERROR,1L);
    curl_easy_setopt(curl,CURLOPT_NOPROGRESS,0L);
    curl_easy_setopt(curl,CURLOPT_XFERINFOFUNCTION,cancelled);
    if(strcmp(resource->suffix,"playlist.m3u8")){
        Segment segment={.fd=fd,.curl=curl};
        curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,write_segment);
        curl_easy_setopt(curl,CURLOPT_WRITEDATA,&segment);
        CURLcode code=curl_easy_perform(curl);
        if(code!=CURLE_OK && !segment.started && !atomic_load(&stopping)){
            curl_easy_setopt(curl,CURLOPT_FRESH_CONNECT,1L);
            curl_easy_perform(curl);
        }
        if(!segment.started)response(fd,502,"text/plain",0);
        curl_easy_setopt(curl,CURLOPT_HTTPHEADER,NULL);
        curl_slist_free_all(headers);
        return;
    }
    Playlist input={0},out={0};
    curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,write_playlist);
    curl_easy_setopt(curl,CURLOPT_WRITEDATA,&input);
    CURLcode code=curl_easy_perform(curl);
    if(code!=CURLE_OK && !input.length && !atomic_load(&stopping)){
        curl_easy_setopt(curl,CURLOPT_FRESH_CONNECT,1L);
        code=curl_easy_perform(curl);
    }
    char *effective=NULL;
    curl_easy_getinfo(curl,CURLINFO_EFFECTIVE_URL,&effective);
    unsigned before=resource_count;
    if(code==CURLE_OK && effective && rewrite_playlist(&out,&input,effective)){
        resource->playlist=out.text;
        resource->playlist_size=out.length;
        out.text=NULL;
        response(fd,200,"application/vnd.apple.mpegurl",(long long)resource->playlist_size);
        send_all(fd,resource->playlist,resource->playlist_size);
    }else{
        while(resource_count>before)free(resources[--resource_count].url);
        long status=0;
        curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&status);
        fprintf(stderr,"Movy playlist: curl=%d, HTTP=%ld, bytes=%lu\n",
                (int)code,status,(unsigned long)input.length);
        response(fd,502,"text/plain",0);
    }
    free(input.text);
    free(out.text);
    curl_easy_setopt(curl,CURLOPT_HTTPHEADER,NULL);
    curl_slist_free_all(headers);
}

static void *relay_worker(void *opaque){
    CURL *curl=opaque;
    struct pollfd waiting={.fd=listener,.events=POLLIN};
    const struct timeval timeout={.tv_sec=2};
    while(!atomic_load(&stopping)){
        if(poll(&waiting,1,100)<=0 || !(waiting.revents&POLLIN))continue;
        int client=accept(listener,NULL,NULL);
        if(client<0)continue;
        setsockopt(client,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
        setsockopt(client,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
        char request[2048],suffix[32];
        size_t size=0;
        while(size+1<sizeof(request) && !atomic_load(&stopping)){
            ssize_t n=recv(client,request+size,sizeof(request)-1-size,0);
            if(n<=0)break;
            size+=(size_t)n;
            request[size]=0;
            if(strstr(request,"\r\n\r\n"))break;
        }
        unsigned id;
        if(size && sscanf(request,"GET /%u/%31s",&id,suffix)==2 && id<resource_count &&
           !strcmp(suffix,resources[id].suffix))
            serve(client,curl,&resources[id]);
        else response(client,404,"text/plain",0);
        close(client);
    }
    curl_easy_cleanup(curl);
    return NULL;
}

void movy_stream_close(void){
    if(listener<0)return;
    atomic_store(&stopping,1);
    pthread_join(worker,NULL);
    close(listener);
    listener=-1;
    while(resource_count){
        Resource *resource=&resources[--resource_count];
        free(resource->url);
        free(resource->playlist);
        *resource=(Resource){0};
    }
}

int movy_stream_open(const char *upstream,char *url,size_t size){
    if(listener>=0 && !strcmp(resources[0].url,upstream)){
        int n=snprintf(url,size,"http://127.0.0.1:%u/0/playlist.m3u8",port);
        return n>0 && (size_t)n<size;
    }
    movy_stream_close();
    if(!movy_stream_url(upstream))return 0;
    int fd=socket(AF_INET,SOCK_STREAM,0);
    if(fd<0)return 0;
    struct sockaddr_in address={.sin_family=AF_INET,.sin_addr={.s_addr=htonl(INADDR_LOOPBACK)}};
#ifdef __SWITCH__
    address.sin_len=sizeof(address);
#endif
    socklen_t length=sizeof(address);
    if(bind(fd,(struct sockaddr *)&address,sizeof(address))<0 ||
       getsockname(fd,(struct sockaddr *)&address,&length)<0 || listen(fd,4)<0){
        close(fd);
        return 0;
    }
    port=ntohs(address.sin_port);
    int n=snprintf(url,size,"http://127.0.0.1:%u/0/playlist.m3u8",port);
    char *root=strdup(upstream);
    CURL *curl=curl_easy_init();
    if(n<=0 || (size_t)n>=size || !root || !curl){
        free(root);
        if(curl)curl_easy_cleanup(curl);
        close(fd);
        return 0;
    }
    resources[0]=(Resource){.url=root,.suffix="playlist.m3u8"};
    resource_count=1;
    listener=fd;
    atomic_store(&stopping,0);
    if(pthread_create(&worker,NULL,relay_worker,curl)!=0){
        close(fd);
        listener=-1;
        resource_count=0;
        curl_easy_cleanup(curl);
        free(root);
        return 0;
    }
    return 1;
}
