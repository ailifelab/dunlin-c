#include "secret_transport.h"
#include "dunlin/compact/model.h"
#include <stdio.h>
#include <string.h>
static int target(dc_secret_transport *t,const char *url){size_t n=strlen(t->endpoint);return url&&!strncmp(url,t->endpoint,n)&&(!url[n]||url[n]=='/');}
static dunlin_http_result post(void *ctx,const dunlin_http_request *r,long *status){
 dc_secret_transport *t=ctx;if(!r||!status||!target(t,r->url))return DUNLIN_HTTP_FAILED;dunlin_http_request request=*r;request.bearer=t->secret&&*t->secret?t->secret:NULL;return dunlin_curl_http.post(dunlin_curl_http.user,&request,status);
}
static dunlin_http_result post2(void *ctx,const dunlin_http_request_v2 *r,long *status){
 dc_secret_transport *t=ctx;if(!r||!status||!target(t,r->request.url)||r->header_count>15||(r->header_count&&!r->headers))return DUNLIN_HTTP_FAILED;dunlin_http_request_v2 request=*r;dunlin_http_header headers[16];for(size_t i=0;i<r->header_count;i++)headers[i]=r->headers[i];
 request.request.bearer=NULL;
 if(t->secret&&*t->secret){if(t->header){headers[request.header_count++]=(dunlin_http_header){t->header==1?"x-goog-api-key":"x-api-key",t->secret};request.headers=headers;}else request.request.bearer=t->secret;}
 return dunlin_curl_http_v2.post(dunlin_curl_http_v2.user,&request,status);
}
int dc_secret_transport_init(dc_secret_transport *t,const char *endpoint,const char *protocol,const char *mode,const char *secret){
 if(!t||!endpoint||!dunlin_model_url_valid(endpoint)||strlen(endpoint)>=sizeof t->endpoint||(mode&&strcmp(mode,"auto")&&strcmp(mode,"bearer")&&strcmp(mode,"none"))||(mode&&!strcmp(mode,"none")&&secret&&*secret)||(secret&&(strlen(secret)>16384||strpbrk(secret,"\r\n"))))return 0;
 *t=(dc_secret_transport){.secret=secret};snprintf(t->endpoint,sizeof t->endpoint,"%s",endpoint);size_t n=strlen(t->endpoint);while(n&&t->endpoint[n-1]=='/')t->endpoint[--n]=0;
 int native_header=!mode||strcmp(mode,"bearer");t->header=native_header&&protocol&&!strcmp(protocol,"gemini")?1:native_header&&protocol&&!strcmp(protocol,"anthropic")?2:0;
 t->v1=(dunlin_http_v1){post,t};t->v2=(dunlin_http_v2){post2,t};return 1;
}
