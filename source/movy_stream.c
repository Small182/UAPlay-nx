#include "movy_stream.h"
#include "catalog.h"

#include <arpa/inet.h>
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
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

enum { MAX_RESOURCES=24000, MAX_PLAYLIST=1024*1024, MAX_SEGMENT=16*1024*1024 };
typedef struct {
    char *url;
    const char *suffix;
    char *cached;
    size_t cached_size;
} Resource;
typedef struct {
    char *text;
    size_t length,capacity,limit;
} Buffer;

static int listener=-1;
static unsigned port;
static long preferred_bandwidth;
static pthread_t worker;
static atomic_int stopping;
static Resource resources[MAX_RESOURCES];
static unsigned resource_count;

static int send_all(int fd,const char *data,size_t size){
    int idle_polls=0;
    while(size && !atomic_load(&stopping)){
        /* Keep BSD service buffers small and give the player time to drain
           its socket when the decoder temporarily pauses reading. */
        size_t chunk=size<16384?size:16384;
#ifdef MSG_NOSIGNAL
        ssize_t n=send(fd,data,chunk,MSG_NOSIGNAL);
#else
        ssize_t n=send(fd,data,chunk,0);
#endif
        if(n<0 && errno==EINTR)continue;
        if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)){
            struct pollfd client={.fd=fd,.events=POLLOUT};
            int ready=poll(&client,1,1000);
            if(ready<0 && errno==EINTR)continue;
            if(ready>0 && (client.revents&POLLOUT) &&
               !(client.revents&(POLLERR|POLLHUP|POLLNVAL)))continue;
            if(!ready && ++idle_polls<15)continue;
        }
        if(n<=0)return 0;
        idle_polls=0;
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

static size_t write_buffer(char *data,size_t size,size_t count,void *opaque){
    Buffer *p=opaque;
    size_t n=size*count;
    if(n>p->limit-p->length)return 0;
    if(p->length+n+1>p->capacity){
        size_t capacity=(p->length+n+1)*2;
        if(capacity>p->limit+1)capacity=p->limit+1;
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

static char *absolute_https(const char *base,const char *ref){
    CURLU *parsed=curl_url();
    if(!parsed)return NULL;
    char *url=NULL;
    if(!curl_url_set(parsed,CURLUPART_URL,base,0) &&
       !curl_url_set(parsed,CURLUPART_URL,ref,0))
        curl_url_get(parsed,CURLUPART_URL,&url,0);
    curl_url_cleanup(parsed);
    if(url && strncmp(url,"https://",8)){curl_free(url);return NULL;}
    return url;
}

static int append_resource(Buffer *out,const char *base,const char *ref,size_t length){
    char reference[2048];
    if(!length || length>=sizeof(reference) || resource_count>=MAX_RESOURCES)return 0;
    memcpy(reference,ref,length);
    reference[length]=0;
    char *url=absolute_https(base,reference);
    const char *suffix=url && !strncmp(url,"https://",8)?resource_suffix(url):NULL;
    if(!suffix){curl_free(url);return 0;}
    char *copy=strdup(url);
    curl_free(url);
    if(!copy)return 0;
    unsigned id=resource_count++;
    resources[id]=(Resource){.url=copy,.suffix=suffix};
    char local[96];
    int n=snprintf(local,sizeof(local),"http://127.0.0.1:%u/%u/%s",port,id,suffix);
    return write_buffer(local,1,(size_t)n,out)==(size_t)n;
}

static int rewrite_playlist(Buffer *out,Buffer *input,const char *base){
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
                if(!end || write_buffer(line,1,(size_t)(uri-line),out)!=(size_t)(uri-line) ||
                   !append_resource(out,base,uri,(size_t)(end-uri)) ||
                   write_buffer((char *)end,1,strlen(end),out)!=strlen(end))return 0;
            }else if(write_buffer(line,1,strlen(line),out)!=strlen(line))return 0;
        }
        if(write_buffer("\n",1,1,out)!=1)return 0;
    }
    return 1;
}

/* Movy advertises muxed renditions up to 4K. Present one media playlist so
   libavformat does not have to open every rendition just to start playback. */
static char *choose_variant(char *manifest,const char *base,long ceiling){
    char *save=NULL,*best=NULL,*lowest=NULL;
    long bandwidth=0,best_rate=0,lowest_rate=0;
    for(char *line=strtok_r(manifest,"\r\n",&save);line;line=strtok_r(NULL,"\r\n",&save)){
        if(!strncmp(line,"#EXT-X-STREAM-INF:",18)){
            const char *rate=strstr(line,"BANDWIDTH=");
            if(rate && rate>line && (rate[-1]==',' || rate[-1]==':') &&
               !strstr(line,",AUDIO="))
                bandwidth=strtol(rate+10,NULL,10);
            else bandwidth=0;
        }else if(*line!='#' && bandwidth>0){
            if(!lowest || bandwidth<lowest_rate){lowest=line;lowest_rate=bandwidth;}
            if(bandwidth<=ceiling && bandwidth>best_rate){best=line;best_rate=bandwidth;}
            bandwidth=0;
        }
    }
    return best || lowest?absolute_https(base,best?best:lowest):NULL;
}

static int fetch(CURL *curl,const char *url,Buffer *out){
    curl_easy_setopt(curl,CURLOPT_URL,url);
    curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,write_buffer);
    curl_easy_setopt(curl,CURLOPT_WRITEDATA,out);
    for(int attempt=0;attempt<2 && !atomic_load(&stopping);attempt++){
        out->length=0;
        curl_easy_setopt(curl,CURLOPT_FRESH_CONNECT,(long)attempt);
        CURLcode code=curl_easy_perform(curl);
        long status=0;
        curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&status);
        if(code==CURLE_OK && out->length && status>=200 && status<300)return 1;
        if((status>=400 && status<500 && status!=408 && status!=429) ||
           code==CURLE_WRITE_ERROR)break;
    }
    return 0;
}

static void serve(int fd,CURL *curl,Resource *resource){
    int playlist=!strcmp(resource->suffix,"playlist.m3u8");
    const char *type=playlist?"application/vnd.apple.mpegurl":"video/mp4";
    if(resource->cached){
        response(fd,200,type,(long long)resource->cached_size);
        send_all(fd,resource->cached,resource->cached_size);
        return;
    }
    Buffer input={.limit=playlist?MAX_PLAYLIST:MAX_SEGMENT};
    curl_easy_setopt(curl,CURLOPT_TIMEOUT,playlist?14L:24L);
    int ok=fetch(curl,resource->url,&input);
    if(ok && !playlist){
        response(fd,200,type,(long long)input.length);
        send_all(fd,input.text,input.length);
        /* Seeking reopens the fMP4 initialization data. Reuse it locally. */
        if(!strcmp(resource->suffix,"init.mp4") && input.length<=65536){
            resource->cached=input.text;
            resource->cached_size=input.length;
            input.text=NULL;
        }
    }else if(ok){
        Buffer out={.limit=MAX_PLAYLIST};
        char *effective=NULL;
        curl_easy_getinfo(curl,CURLINFO_EFFECTIVE_URL,&effective);
        if(effective && !strncmp(input.text,"#EXTM3U",7) &&
           strstr(input.text,"#EXT-X-STREAM-INF:")){
            char *variant=choose_variant(input.text,effective,preferred_bandwidth);
            ok=variant && fetch(curl,variant,&input);
            curl_free(variant);
            curl_easy_getinfo(curl,CURLINFO_EFFECTIVE_URL,&effective);
        }
        unsigned before=resource_count;
        if(ok && effective && strstr(input.text,"#EXTINF:") &&
           rewrite_playlist(&out,&input,effective)){
            resource->cached=out.text;
            resource->cached_size=out.length;
            response(fd,200,"application/vnd.apple.mpegurl",(long long)out.length);
            send_all(fd,out.text,out.length);
        }else{
            while(resource_count>before)free(resources[--resource_count].url);
            free(out.text);
            ok=0;
        }
    }
    if(!ok){
        long status=0;
        curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&status);
        fprintf(stderr,"Movy %s unavailable (HTTP %ld)\n",playlist?"playlist":"segment",status);
        response(fd,502,"text/plain",0);
    }
    free(input.text);
}

static void *relay_worker(void *opaque){
    CURL *curl=opaque;
    struct curl_slist *headers=curl_slist_append(NULL,"Origin: " MOVY_ORIGIN);
    curl_easy_setopt(curl,CURLOPT_HTTPHEADER,headers);
    curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION,1L);
    curl_easy_setopt(curl,CURLOPT_MAXREDIRS,4L);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl,CURLOPT_REDIR_PROTOCOLS_STR,"https");
#else
    curl_easy_setopt(curl,CURLOPT_REDIR_PROTOCOLS,CURLPROTO_HTTPS);
#endif
    curl_easy_setopt(curl,CURLOPT_REFERER,MOVY_ORIGIN "/");
    curl_easy_setopt(curl,CURLOPT_ACCEPT_ENCODING,"");
    curl_easy_setopt(curl,CURLOPT_HTTP_VERSION,CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl,CURLOPT_USERAGENT,"Mozilla/5.0 (Nintendo Switch; UAPlay/1.0)");
#ifdef __SWITCH__
    curl_easy_setopt(curl,CURLOPT_IPRESOLVE,(long)CURL_IPRESOLVE_V4);
#endif
    curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT,6L);
    curl_easy_setopt(curl,CURLOPT_FAILONERROR,1L);
    curl_easy_setopt(curl,CURLOPT_NOPROGRESS,0L);
    curl_easy_setopt(curl,CURLOPT_XFERINFOFUNCTION,cancelled);
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
        int flags=fcntl(client,F_GETFL,0);
        if(flags>=0)fcntl(client,F_SETFL,flags|O_NONBLOCK);
        if(size && sscanf(request,"GET /%u/%31s",&id,suffix)==2 && id<resource_count &&
           !strcmp(suffix,resources[id].suffix))
            serve(client,curl,&resources[id]);
        else response(client,404,"text/plain",0);
        close(client);
    }
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
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
        free(resource->cached);
        *resource=(Resource){0};
    }
}

int movy_stream_open(const char *upstream,long bandwidth,char *url,size_t size){
    if(listener>=0 && preferred_bandwidth==bandwidth && !strcmp(resources[0].url,upstream)){
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
    preferred_bandwidth=bandwidth;
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
