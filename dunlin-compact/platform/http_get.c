#include "http_get.h"
#include <curl/curl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

typedef struct {
    char *data;
    size_t used, cap;
    int overflow;
} body_buffer;
typedef struct { int allow_synthetic_public; } socket_policy;

static ah_status text_result(char **out, ah_status status, const char *message) {
    size_t n=strlen(message);
    *out=malloc(n+1);
    if(!*out) return AH_FAILED;
    memcpy(*out,message,n+1);
    return status;
}

static int suffix(const char *s,const char *tail) {
    size_t n=strlen(s),m=strlen(tail);
    return n>=m && !strcmp(s+n-m,tail);
}

int ah_https_url_check(const char *url,char *error,size_t size) {
    const char *why=NULL;
    CURLU *parsed=NULL;
    char *scheme=NULL,*host=NULL,*user=NULL,*password=NULL;
    if(!url || !*url || strlen(url)>2048) why="url must be a nonempty HTTPS URL no longer than 2048 bytes";
    for(const unsigned char *p=(const unsigned char *)(url?url:"");!why&&*p;p++)
        if(*p<32 || *p==127) why="url contains a control character";
    if(!why && !(parsed=curl_url())) why="cannot parse url";
    if(!why && curl_url_set(parsed,CURLUPART_URL,url,CURLU_NON_SUPPORT_SCHEME)!=CURLUE_OK)
        why="url must be an absolute HTTPS URL";
    if(!why && (curl_url_get(parsed,CURLUPART_SCHEME,&scheme,0)!=CURLUE_OK || strcmp(scheme,"https")))
        why="only https URLs are allowed";
    if(!why && (curl_url_get(parsed,CURLUPART_HOST,&host,0)!=CURLUE_OK || !*host))
        why="url must include a host";
    if(!why && (curl_url_get(parsed,CURLUPART_USER,&user,0)==CURLUE_OK ||
                curl_url_get(parsed,CURLUPART_PASSWORD,&password,0)==CURLUE_OK))
        why="url credentials are not allowed";
    if(!why && (!strcmp(host,"localhost") || suffix(host,".localhost")))
        why="local destinations are not allowed";
    if(!why) {
        const char *numeric=host;char unbracketed[INET6_ADDRSTRLEN+1];size_t host_len=strlen(host);
        if(host_len>=2&&host[0]=='['&&host[host_len-1]==']') {
            if(host_len-2>=sizeof unbracketed) why="invalid IP address";
            else {memcpy(unbracketed,host+1,host_len-2);unbracketed[host_len-2]=0;numeric=unbracketed;}
        }
        struct in_addr v4; struct in6_addr v6;
        if(!why&&inet_pton(AF_INET,numeric,&v4)==1) {
            uint32_t a=ntohl(v4.s_addr);
            if((a>>24)==0 || (a>>24)==10 || (a>>24)==127 ||
               (a&0xffc00000u)==0x64400000u || (a&0xffff0000u)==0xa9fe0000u ||
               (a&0xfff00000u)==0xac100000u || (a&0xffff0000u)==0xc0a80000u ||
               (a&0xffffff00u)==0xc0000000u || (a&0xffffff00u)==0xc0000200u ||
               (a&0xfffe0000u)==0xc6120000u || (a&0xffffff00u)==0xc6336400u ||
               (a&0xffffff00u)==0xcb007100u || a>=0xe0000000u)
                why="private, local and non-routable destinations are not allowed";
        } else if(!why&&inet_pton(AF_INET6,numeric,&v6)==1) {
            const unsigned char *a=(const unsigned char *)&v6;
            int mapped=!memcmp(a,"\0\0\0\0\0\0\0\0\0\0\xff\xff",12);
            int global=(a[0]&0xe0)==0x20;
            int documentation=a[0]==0x20&&a[1]==0x01&&a[2]==0x0d&&a[3]==0xb8;
            if(mapped || !global || documentation)
                why="private, local and non-routable destinations are not allowed";
        }
    }
    if(error&&size) snprintf(error,size,"%s",why?why:"");
    curl_free(scheme);curl_free(host);curl_free(user);curl_free(password);curl_url_cleanup(parsed);
    return !why;
}

static int blocked_v4(uint32_t a,int allow_synthetic_public) {
    return (a>>24)==0 || (a>>24)==10 || (a>>24)==127 ||
        (a&0xffc00000u)==0x64400000u || (a&0xffff0000u)==0xa9fe0000u ||
        (a&0xfff00000u)==0xac100000u || (a&0xffff0000u)==0xc0a80000u ||
        (a&0xffffff00u)==0xc0000000u || (a&0xffffff00u)==0xc0000200u ||
        (!allow_synthetic_public && (a&0xfffe0000u)==0xc6120000u) || (a&0xffffff00u)==0xc6336400u ||
        (a&0xffffff00u)==0xcb007100u || a>=0xe0000000u;
}

static int blocked_address(const struct sockaddr *address,const socket_policy *policy) {
    if(address->sa_family==AF_INET) {
        const struct sockaddr_in *v4=(const struct sockaddr_in *)address;
        return blocked_v4(ntohl(v4->sin_addr.s_addr),policy->allow_synthetic_public);
    }
    if(address->sa_family==AF_INET6) {
        const unsigned char *a=(const unsigned char *)&((const struct sockaddr_in6 *)address)->sin6_addr;
        if(!memcmp(a,"\0\0\0\0\0\0\0\0\0\0\xff\xff",12)) {
            uint32_t v4;memcpy(&v4,a+12,sizeof v4);return blocked_v4(ntohl(v4),policy->allow_synthetic_public);
        }
        return (a[0]&0xe0)!=0x20 || (a[0]==0x20&&a[1]==0x01&&a[2]==0x0d&&a[3]==0xb8);
    }
    return 1;
}

static curl_socket_t open_public(void *unused,curlsocktype purpose,struct curl_sockaddr *address) {
    const socket_policy *policy=unused;
    if(purpose!=CURLSOCKTYPE_IPCXN || !policy || blocked_address(&address->addr,policy)) return CURL_SOCKET_BAD;
    return socket(address->family,address->socktype,address->protocol);
}

static size_t receive(char *data,size_t size,size_t count,void *opaque) {
    body_buffer *body=opaque;
    if(count && size>SIZE_MAX/count) {body->overflow=1;return 0;}
    size_t n=size*count;
    if(n>body->cap-body->used) {body->overflow=1;return 0;}
    memcpy(body->data+body->used,data,n);body->used+=n;
    return n;
}

static int progress(void *opaque,curl_off_t a,curl_off_t b,curl_off_t c,curl_off_t d) {
    (void)a;(void)b;(void)c;(void)d;
    const ah_hooks *hooks=opaque;
    return hooks&&hooks->cancelled&&hooks->cancelled(hooks->user);
}

ah_status ah_https_get(const char *url,const ah_hooks *hooks,size_t cap,unsigned timeout,char **out) {
    char error[160];*out=NULL;
    if(!ah_https_url_check(url,error,sizeof error)) return text_result(out,AH_INVALID_ARGUMENTS,error);
    if(!cap || cap==SIZE_MAX) return text_result(out,AH_BUDGET,"invalid output limit");
    CURL *easy=curl_easy_init();
    body_buffer body={malloc(cap+1),0,cap,0};
    socket_policy policy={1};
    CURLU *parsed=curl_url();char *host=NULL;
    if(!parsed || curl_url_set(parsed,CURLUPART_URL,url,0)!=CURLUE_OK ||
       curl_url_get(parsed,CURLUPART_HOST,&host,0)!=CURLUE_OK) policy.allow_synthetic_public=0;
    else {
        struct in_addr v4;struct in6_addr v6;const char *numeric=host;char plain[INET6_ADDRSTRLEN+1];size_t n=strlen(host);
        if(n>=2&&host[0]=='['&&host[n-1]==']'&&n-2<sizeof plain){memcpy(plain,host+1,n-2);plain[n-2]=0;numeric=plain;}
        if(inet_pton(AF_INET,numeric,&v4)==1||inet_pton(AF_INET6,numeric,&v6)==1)policy.allow_synthetic_public=0;
    }
    curl_free(host);curl_url_cleanup(parsed);
    if(!easy || !body.data) {curl_easy_cleanup(easy);free(body.data);return AH_FAILED;}
    CURLcode configured=CURLE_OK;
#define SET(option,value) do {if(!configured) configured=curl_easy_setopt(easy,option,value);} while(0)
    SET(CURLOPT_URL,url);SET(CURLOPT_HTTPGET,1L);
#if LIBCURL_VERSION_NUM >= 0x075500
    SET(CURLOPT_PROTOCOLS_STR,"https");SET(CURLOPT_REDIR_PROTOCOLS_STR,"https");
#else
    SET(CURLOPT_PROTOCOLS,(long)CURLPROTO_HTTPS);SET(CURLOPT_REDIR_PROTOCOLS,(long)CURLPROTO_HTTPS);
#endif
    SET(CURLOPT_FOLLOWLOCATION,0L);SET(CURLOPT_FAILONERROR,1L);
    SET(CURLOPT_SSL_VERIFYPEER,1L);SET(CURLOPT_SSL_VERIFYHOST,2L);
    SET(CURLOPT_NETRC,(long)CURL_NETRC_IGNORED);SET(CURLOPT_PROXY,"");
    SET(CURLOPT_NOSIGNAL,1L);SET(CURLOPT_TIMEOUT_MS,(long)timeout);
    SET(CURLOPT_CONNECTTIMEOUT_MS,(long)(timeout<10000?timeout:10000));
    SET(CURLOPT_USERAGENT,"DunlinC/0.2.5 readonly-http-get");
    SET(CURLOPT_OPENSOCKETFUNCTION,open_public);SET(CURLOPT_OPENSOCKETDATA,&policy);
    SET(CURLOPT_WRITEFUNCTION,receive);SET(CURLOPT_WRITEDATA,&body);
    SET(CURLOPT_XFERINFOFUNCTION,progress);SET(CURLOPT_XFERINFODATA,(void *)hooks);SET(CURLOPT_NOPROGRESS,0L);
#undef SET
    CURLcode rc=configured?configured:curl_easy_perform(easy);long status=0;
    curl_easy_getinfo(easy,CURLINFO_RESPONSE_CODE,&status);curl_easy_cleanup(easy);
    if(rc==CURLE_OK && status>=200 && status<300 && ah_utf8_text(body.data,body.used)) {
        body.data[body.used]=0;*out=body.data;return AH_DONE;
    }
    free(body.data);
    if(body.overflow) return text_result(out,AH_BUDGET,"HTTPS response exceeds output limit");
    if(rc==CURLE_ABORTED_BY_CALLBACK) return text_result(out,AH_CANCELLED,"HTTPS GET cancelled");
    if(rc==CURLE_OPERATION_TIMEDOUT) return text_result(out,AH_TIMEOUT,"HTTPS GET timed out");
    if(rc==CURLE_OK && status>=200 && status<300) return text_result(out,AH_FAILED,"HTTPS response is not valid UTF-8 text");
    snprintf(error,sizeof error,"HTTPS GET failed (curl=%d, status=%ld)",(int)rc,status);
    return text_result(out,AH_FAILED,error);
}
