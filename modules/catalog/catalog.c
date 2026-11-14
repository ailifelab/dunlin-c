#define _XOPEN_SOURCE 700
#include "../../desktop/extensions.h"
#include "../../desktop/catalog.h"
#include "../../desktop/skills.h"
#include "dunlin/compact/json.h"
#include "dunlin/compact/agent.h"
#include "dunlin/content_hash.h"
#include <openssl/evp.h>
#include <curl/curl.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <dirent.h>
#include <stdint.h>
#include <strings.h>
typedef struct json_object J;
static J *get(J *o,const char *k){J *v=NULL;if(o)json_object_object_get_ex(o,k,&v);return v;}
static const char *str(J *o,const char *k){J *v=get(o,k);if(!json_object_is_type(v,json_type_string))return NULL;const char *p=json_object_get_string(v);return p&&strlen(p)==(size_t)json_object_get_string_len(v)?p:NULL;}
static int eq(const char *a,const char *b){return a&&b&&!strcmp(a,b);}
static int text(const char *s,size_t n){return s&&*s&&strlen(s)<=n&&ah_utf8_text(s,strlen(s));}
static int ident(const char *s){return text(s,128)&&strspn(s,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-:/@+")==strlen(s);}
static int hex(const char *s,size_t n){return s&&strlen(s)==n&&strspn(s,"0123456789abcdef")==n;}
static int unhex(const char *s,unsigned char *p,size_t n){if(!hex(s,n*2))return 0;for(size_t i=0;i<n;i++){char b[3]={s[i*2],s[i*2+1],0};p[i]=(unsigned char)strtoul(b,NULL,16);}return 1;}
static sqlite3_stmt *prep(dc_store *s,const char *v){sqlite3_stmt *q=NULL;return sqlite3_prepare_v2(s->db,v,-1,&q,NULL)==SQLITE_OK?q:NULL;}
static void db_bind(sqlite3_stmt *q,int i,const char *v){sqlite3_bind_text(q,i,v?v:"",-1,SQLITE_TRANSIENT);}
static int step(sqlite3_stmt *q){int ok=q&&sqlite3_step(q)==SQLITE_DONE;sqlite3_finalize(q);return ok;}
static int sql(dc_store *s,const char *v){return sqlite3_exec(s->db,v,NULL,NULL,NULL)==SQLITE_OK;}
static J *reply(int ok,const char *error,J *data){J *r=json_object_new_object();json_object_object_add(r,"ok",json_object_new_boolean(ok));if(error)json_object_object_add(r,"error",json_object_new_string(error));if(data)json_object_object_add(r,"data",data);return r;}
static J *fail(const char *e){return reply(0,e,NULL);}
static int https(const char *url){
 if(!url||strlen(url)>2048||strncmp(url,"https://",8)||strpbrk(url,"\r\n"))return 0;
 CURLU *u=curl_url();if(!u)return 0;char *host=NULL,*user=NULL,*password=NULL,*fragment=NULL;
 int ok=curl_url_set(u,CURLUPART_URL,url,0)==CURLUE_OK&&curl_url_get(u,CURLUPART_HOST,&host,0)==CURLUE_OK&&host&&*host;
 if(curl_url_get(u,CURLUPART_USER,&user,0)==CURLUE_OK||curl_url_get(u,CURLUPART_PASSWORD,&password,0)==CURLUE_OK||curl_url_get(u,CURLUPART_FRAGMENT,&fragment,0)==CURLUE_OK)ok=0;
 curl_free(host);curl_free(user);curl_free(password);curl_free(fragment);curl_url_cleanup(u);return ok;
}
#include "source_config.h"
int dc_catalog_schema(dc_store *s){return sql(s,
 "CREATE TABLE IF NOT EXISTS community_indexes(url TEXT PRIMARY KEY,payload TEXT NOT NULL,updated INTEGER NOT NULL);"
 "CREATE TABLE IF NOT EXISTS catalog_sources(id TEXT PRIMARY KEY,url TEXT NOT NULL UNIQUE,title TEXT NOT NULL,public_key TEXT NOT NULL,catalog_id TEXT NOT NULL DEFAULT '',revision INTEGER NOT NULL DEFAULT -1,payload TEXT,etag TEXT NOT NULL DEFAULT '',verified_at INTEGER NOT NULL DEFAULT 0,error TEXT NOT NULL DEFAULT '',enabled INTEGER NOT NULL DEFAULT 1);"
 "CREATE TABLE IF NOT EXISTS catalog_versions(source TEXT NOT NULL,package TEXT NOT NULL,version TEXT NOT NULL,digest TEXT NOT NULL,manifest_digest TEXT NOT NULL,kind TEXT NOT NULL,PRIMARY KEY(source,package,version));"
 "CREATE TABLE IF NOT EXISTS catalog_packages(source TEXT NOT NULL,package TEXT NOT NULL,version TEXT NOT NULL,digest TEXT NOT NULL,kind TEXT NOT NULL,manifest TEXT NOT NULL,path TEXT NOT NULL,installed INTEGER NOT NULL DEFAULT(unixepoch()),PRIMARY KEY(source,package,version));");}
static const char *compatibility(dc_store *,J *,const char *);
static J *sources(dc_store *s){
 sqlite3_stmt *q=prep(s,"SELECT id,url,title,public_key,catalog_id,revision,verified_at,error,enabled,payload FROM catalog_sources ORDER BY title,id");if(!q)return fail("storage_error");J *a=json_object_new_array();int rc;
 const char *keys[]={"id","url","title","publicKey","catalogID","revision","verifiedAt","error","enabled"};
 while((rc=sqlite3_step(q))==SQLITE_ROW){J *o=json_object_new_object();for(int i=0;i<9;i++)json_object_object_add(o,keys[i],i==5||i==6?json_object_new_int64(sqlite3_column_int64(q,i)):i==8?json_object_new_boolean(sqlite3_column_int(q,i)):json_object_new_string((const char*)sqlite3_column_text(q,i)));const char *raw=(const char*)sqlite3_column_text(q,9);J *p=raw?dunlin_json_parse(raw,strlen(raw)):NULL;json_object_object_add(o,"expiresAt",json_object_new_int64(json_object_get_int64(get(p,"expiresAt"))));J *entries=get(p,"entries");for(size_t j=0;entries&&j<json_object_array_length(entries);j++){J *e=json_object_array_get_idx(entries,j),*versions=get(e,"versions");for(size_t k=0;versions&&k<json_object_array_length(versions);k++){J *v=json_object_array_get_idx(versions,k);const char *reason=compatibility(s,v,str(e,"kind"));if(!sqlite3_column_int(q,8))reason="source_disabled";if(json_object_get_int64(get(p,"expiresAt"))<=(int64_t)time(NULL))reason="catalog_expired";J *ws=get(p,"withdrawals");for(size_t w=0;ws&&w<json_object_array_length(ws);w++){J *item=json_object_array_get_idx(ws,w);if(eq(str(item,"packageId"),str(e,"packageId"))&&eq(str(item,"version"),str(v,"version")))reason="version_withdrawn";}json_object_object_add(v,"installable",json_object_new_boolean(!reason));json_object_object_add(v,"reason",json_object_new_string(reason?reason:""));sqlite3_stmt *installed=prep(s,"SELECT 1 FROM catalog_packages WHERE source=? AND package=? AND version=?");if(installed){db_bind(installed,1,(const char*)sqlite3_column_text(q,0));db_bind(installed,2,str(e,"packageId"));db_bind(installed,3,str(v,"version"));}int found=installed&&sqlite3_step(installed)==SQLITE_ROW;sqlite3_finalize(installed);json_object_object_add(v,"installed",json_object_new_boolean(found));}}
 json_object_object_add(o,"entries",get(p,"entries")?json_object_get(get(p,"entries")):json_object_new_array());json_object_put(p);json_object_array_add(a,o);}
 sqlite3_finalize(q);if(rc!=SQLITE_DONE){json_object_put(a);return fail("storage_error");}return reply(1,NULL,a);
}
static char *base64(J *object,const char *key,size_t max,size_t *n){
 const char *b=str(object,key);if(!b)return NULL;size_t length=strlen(b);if(!length||length%4||length>max*4/3+8||strspn(b,"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=")!=length)return NULL;
 size_t padding=b[length-1]=='='?(b[length-2]=='='?2:1):0;if(memchr(b,'=',length-padding))return NULL;
 unsigned char *out=malloc(length/4*3+1);if(!out)return NULL;int decoded=EVP_DecodeBlock(out,(const unsigned char*)b,(int)length);if(decoded<0||(size_t)decoded<padding||(size_t)decoded-padding>max){free(out);return NULL;}*n=(size_t)decoded-padding;out[*n]=0;return (char*)out;
}
static int verify(const char *public_key,const char *sig,const char *payload,size_t n){
 unsigned char key[32],signature[64];if(!unhex(public_key,key,32)||!unhex(sig,signature,64))return 0;EVP_PKEY *p=EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519,NULL,key,32);EVP_MD_CTX *ctx=EVP_MD_CTX_new();int ok=p&&ctx&&EVP_DigestVerifyInit(ctx,NULL,NULL,NULL,p)==1&&EVP_DigestVerify(ctx,signature,64,(const unsigned char*)payload,n)==1;EVP_MD_CTX_free(ctx);EVP_PKEY_free(p);return ok;
}
static int list(J *p,const char *key,size_t max){J *a=get(p,key);return json_object_is_type(a,json_type_array)&&json_object_array_length(a)<=max;}
static int valid_manifest_ref(J *v){
 J *src=get(v,"artifactSource");return text(str(v,"version"),128)&&hex(str(v,"digest"),64)&&hex(str(v,"manifestDigest"),64)&&eq(str(src,"type"),"https_immutable_archive")&&eq(str(src,"format"),"dunlin.files.v1")&&https(str(src,"url"))&&list(v,"platforms",8)&&list(v,"architectures",8)&&list(v,"dependencies",128)&&list(v,"capabilities",128)&&json_object_is_type(get(v,"engines"),json_type_object);
}
static int valid_payload(J *p){
 int64_t now=(int64_t)time(NULL),issued=json_object_get_int64(get(p,"issuedAt")),expires=json_object_get_int64(get(p,"expiresAt"));
 if(!json_object_is_type(get(p,"issuedAt"),json_type_int)||!json_object_is_type(get(p,"expiresAt"),json_type_int)||!eq(str(p,"protocol"),"dunlin.catalog")||!json_object_is_type(get(p,"major"),json_type_int)||json_object_get_int(get(p,"major"))!=1||!ident(str(p,"catalogId"))||!text(str(p,"title"),256)||!text(str(p,"publisher"),256)||!ident(str(p,"keyId"))||!json_object_is_type(get(p,"revision"),json_type_int)||json_object_get_int64(get(p,"revision"))<0||issued<=0||issued>now+300||expires<=now||expires<=issued||!list(p,"entries",500)||!list(p,"withdrawals",500))return 0;
 /* v1 currently uses one revision-bound page. Never silently mix pagination. */
 J *next=get(p,"nextPage");if(next&&!json_object_is_type(next,json_type_null)&&!eq(json_object_get_string(next),""))return 0;
 J *entries=get(p,"entries");for(size_t i=0;i<json_object_array_length(entries);i++){
  J *e=json_object_array_get_idx(entries,i);const char *kind=str(e,"kind");if(!ident(str(e,"packageId"))||(!eq(kind,"skill")&&!eq(kind,"plugin")&&!eq(kind,"theme"))||!text(str(e,"displayName"),256)||!text(str(e,"publisher"),256)||!text(str(e,"license"),256)||!list(e,"versions",50))return 0;
  for(size_t j=0;j<i;j++)if(eq(str(e,"packageId"),str(json_object_array_get_idx(entries,j),"packageId")))return 0;
  J *versions=get(e,"versions");for(size_t j=0;j<json_object_array_length(versions);j++){J *v=json_object_array_get_idx(versions,j);if(!valid_manifest_ref(v))return 0;for(size_t k=0;k<j;k++)if(eq(str(v,"version"),str(json_object_array_get_idx(versions,k),"version")))return 0;}
 }
 J *withdrawals=get(p,"withdrawals");for(size_t i=0;i<json_object_array_length(withdrawals);i++){J *w=json_object_array_get_idx(withdrawals,i);if(!ident(str(w,"packageId"))||!text(str(w,"version"),128)||!text(str(w,"reason"),2048))return 0;}
 return 1;
}
static J *accept_catalog(dc_store *s,const char *id,const char *raw,const char *etag){
 sqlite3_stmt *q=prep(s,"SELECT public_key,catalog_id,revision,payload FROM catalog_sources WHERE id=? AND enabled=1");if(q)db_bind(q,1,id);if(!q||sqlite3_step(q)!=SQLITE_ROW){sqlite3_finalize(q);return fail("source_missing_or_disabled");}
 char *key=strdup((const char*)sqlite3_column_text(q,0)),*catalog=strdup((const char*)sqlite3_column_text(q,1)),*old=sqlite3_column_type(q,3)==SQLITE_NULL?NULL:strdup((const char*)sqlite3_column_text(q,3));int64_t revision=sqlite3_column_int64(q,2);sqlite3_finalize(q);
 J *envelope=raw&&strlen(raw)<=4194304?dunlin_json_parse(raw,strlen(raw)):NULL;size_t n=0;char *payload=eq(str(envelope,"algorithm"),"Ed25519")?base64(envelope,"payload",2097152,&n):NULL;
 const char *error="invalid_signature_or_envelope";J *p=NULL;
 if(!key||!catalog||(old&&!*old))goto done;
 if(!payload||!verify(key,str(envelope,"signature"),payload,n))goto done;
 p=dunlin_json_parse(payload,n);error="invalid_or_expired_catalog";if(!valid_payload(p))goto done;
 error="catalog_identity_changed";if(*catalog&&!eq(catalog,str(p,"catalogId")))goto done;
 int64_t next=json_object_get_int64(get(p,"revision"));error="catalog_revision_rollback_or_conflict";if(next<revision||(next==revision&&(!old||strcmp(old,payload))))goto done;
 J *old_payload=old?dunlin_json_parse(old,strlen(old)):NULL;J *old_entries=get(old_payload,"entries"),*new_entries=get(p,"entries");int immutable=1;
 for(size_t a=0;old_entries&&a<json_object_array_length(old_entries)&&immutable;a++){J *oe=json_object_array_get_idx(old_entries,a);for(size_t b=0;b<json_object_array_length(new_entries)&&immutable;b++){J *ne=json_object_array_get_idx(new_entries,b);if(!eq(str(oe,"packageId"),str(ne,"packageId")))continue;if(!eq(str(oe,"kind"),str(ne,"kind"))){immutable=0;break;}J *ov=get(oe,"versions"),*nv=get(ne,"versions");for(size_t c=0;c<json_object_array_length(ov)&&immutable;c++)for(size_t d=0;d<json_object_array_length(nv);d++){J *x=json_object_array_get_idx(ov,c),*y=json_object_array_get_idx(nv,d);if(eq(str(x,"version"),str(y,"version"))&&(!eq(str(x,"digest"),str(y,"digest"))||!eq(str(x,"manifestDigest"),str(y,"manifestDigest")))){immutable=0;break;}}}}
 json_object_put(old_payload);error="immutable_catalog_version_changed";if(!immutable)goto done;
 error="storage_error";if(!sql(s,"BEGIN IMMEDIATE"))goto done;
 q=prep(s,"UPDATE catalog_sources SET catalog_id=?,revision=?,payload=?,etag=?,verified_at=unixepoch(),error='' WHERE id=?");if(q){db_bind(q,1,str(p,"catalogId"));sqlite3_bind_int64(q,2,next);db_bind(q,3,payload);db_bind(q,4,etag);db_bind(q,5,id);}error=step(q)?NULL:"storage_error";
 if(!error){for(size_t a=0;a<json_object_array_length(new_entries)&&!error;a++){J *e=json_object_array_get_idx(new_entries,a),*vs=get(e,"versions");for(size_t b=0;b<json_object_array_length(vs)&&!error;b++){J *v=json_object_array_get_idx(vs,b);q=prep(s,"INSERT INTO catalog_versions VALUES(?,?,?,?,?,?) ON CONFLICT(source,package,version) DO UPDATE SET digest=excluded.digest WHERE catalog_versions.digest=excluded.digest AND catalog_versions.manifest_digest=excluded.manifest_digest AND catalog_versions.kind=excluded.kind");if(q){db_bind(q,1,id);db_bind(q,2,str(e,"packageId"));db_bind(q,3,str(v,"version"));db_bind(q,4,str(v,"digest"));db_bind(q,5,str(v,"manifestDigest"));db_bind(q,6,str(e,"kind"));}if(!step(q)||sqlite3_changes(s->db)!=1)error="immutable_catalog_version_changed";}}}
 if(!error){J *withdrawals=get(p,"withdrawals");char source[160];snprintf(source,sizeof source,"catalog:%s",id);for(size_t w=0;w<json_object_array_length(withdrawals)&&!error;w++){J *item=json_object_array_get_idx(withdrawals,w);q=prep(s,"UPDATE skill_versions SET quarantined=1 WHERE source=? AND package=? AND version=?");if(q){db_bind(q,1,source);db_bind(q,2,str(item,"packageId"));db_bind(q,3,str(item,"version"));}if(!step(q))error="storage_error";q=prep(s,"UPDATE extension_versions SET quarantined=1,error='catalog_withdrawal' WHERE source=? AND package=? AND version=?");if(q){db_bind(q,1,source);db_bind(q,2,str(item,"packageId"));db_bind(q,3,str(item,"version"));}if(!step(q))error="storage_error";}}
 if(error){sql(s,"ROLLBACK");}else if(!sql(s,"COMMIT")){sql(s,"ROLLBACK");error="storage_error";}
 done:
 free(key);free(catalog);free(old);free(payload);json_object_put(envelope);json_object_put(p);
 if(error){q=prep(s,"UPDATE catalog_sources SET error=? WHERE id=?");if(q){db_bind(q,1,error);db_bind(q,2,id);}step(q);return fail(error);}dc_extensions_enforce(s,NULL);return sources(s);
}
typedef struct {char *bytes;size_t used,limit;long archive_timeout_ms;char etag[256];const dc_hooks *hooks;} transfer;
static size_t receive(char *p,size_t size,size_t count,void *ctx){transfer *t=ctx;if(size&&count>SIZE_MAX/size)return 0;size_t n=size*count;if(n>t->limit-t->used)return 0;char *next=realloc(t->bytes,t->used+n+1);if(!next)return 0;t->bytes=next;memcpy(next+t->used,p,n);t->used+=n;next[t->used]=0;return n;}
static size_t receive_header(char *p,size_t size,size_t count,void *ctx){transfer *t=ctx;if(size&&count>SIZE_MAX/size)return 0;size_t n=size*count;if(n>6&&!strncasecmp(p,"etag: ",6)){size_t k=n-6;while(k&&(p[6+k-1]=='\r'||p[6+k-1]=='\n'))k--;if(k<sizeof t->etag&&!memchr(p+6,0,k)){memcpy(t->etag,p+6,k);t->etag[k]=0;}}return n;}
static int progress(void *ctx,curl_off_t a,curl_off_t b,curl_off_t c,curl_off_t d){(void)a;(void)b;(void)c;(void)d;transfer *t=ctx;return t->hooks&&t->hooks->cancelled?t->hooks->cancelled(t->hooks->user):0;}
static int fetch(const char *url,const char *etag,transfer *t,long *status){
 if(!https(url))return 0;CURL *c=curl_easy_init();if(!c)return 0;struct curl_slist *headers=curl_slist_append(NULL,t->archive_timeout_ms?"Accept: application/octet-stream":"Accept: application/json");char conditional[300];if(etag&&*etag&&!strpbrk(etag,"\r\n")){snprintf(conditional,sizeof conditional,"If-None-Match: %s",etag);struct curl_slist *next=curl_slist_append(headers,conditional);if(!next){curl_slist_free_all(headers);curl_easy_cleanup(c);return 0;}headers=next;}
#define SET(k,v) if(curl_easy_setopt(c,k,v)!=CURLE_OK)goto failed
 const char *ca=getenv("SSL_CERT_FILE");
 if(ca&&*ca)SET(CURLOPT_CAINFO,ca);
 SET(CURLOPT_URL,url);SET(CURLOPT_HTTPHEADER,headers);SET(CURLOPT_FOLLOWLOCATION,0L);SET(CURLOPT_SSL_VERIFYPEER,1L);SET(CURLOPT_SSL_VERIFYHOST,2L);SET(CURLOPT_NOSIGNAL,1L);SET(CURLOPT_CONNECTTIMEOUT_MS,5000L);SET(CURLOPT_TIMEOUT_MS,t->archive_timeout_ms?t->archive_timeout_ms:60000L);SET(CURLOPT_WRITEFUNCTION,receive);SET(CURLOPT_WRITEDATA,t);SET(CURLOPT_HEADERFUNCTION,receive_header);SET(CURLOPT_HEADERDATA,t);SET(CURLOPT_NOPROGRESS,0L);SET(CURLOPT_XFERINFOFUNCTION,progress);SET(CURLOPT_XFERINFODATA,t);
#if LIBCURL_VERSION_NUM >= 0x075500
 SET(CURLOPT_PROTOCOLS_STR,"https");
#else
 SET(CURLOPT_PROTOCOLS,(long)CURLPROTO_HTTPS);
#endif
 {CURLcode rc=curl_easy_perform(c);curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,status);curl_slist_free_all(headers);curl_easy_cleanup(c);return rc==CURLE_OK;}
 failed:curl_slist_free_all(headers);curl_easy_cleanup(c);return 0;
#undef SET
}
static J *refresh(dc_store *s,const char *id,const dc_hooks *hooks){
 sqlite3_stmt *q=prep(s,"SELECT url,etag,payload FROM catalog_sources WHERE id=? AND enabled=1");if(q)db_bind(q,1,id);if(!q||sqlite3_step(q)!=SQLITE_ROW){sqlite3_finalize(q);return fail("source_missing_or_disabled");}char *url=strdup((const char*)sqlite3_column_text(q,0)),*etag=strdup((const char*)sqlite3_column_text(q,1));const char *raw=(const char*)sqlite3_column_text(q,2);J *cached=raw?dunlin_json_parse(raw,strlen(raw)):NULL;sqlite3_finalize(q);
 transfer t={.limit=4194304,.hooks=hooks};long status=0;int ok=url&&etag&&fetch(url,etag,&t,&status);free(url);free(etag);J *result=NULL;
 if(ok&&status==304)result=valid_payload(cached)?sources(s):fail("cached_catalog_expired");else if(ok&&status==200)result=accept_catalog(s,id,t.bytes,t.etag);else result=fail(progress(&t,0,0,0,0)?"cancelled":"catalog_download_failed");free(t.bytes);json_object_put(cached);return result;
}
static int path_ok(const char *p){if(!text(p,512)||p[0]=='/'||strchr(p,'\\'))return 0;const char *a=p;for(const char *q=p;;q++){if(*q=='/'||!*q){size_t n=(size_t)(q-a);if(!n||(n==1&&a[0]=='.')||(n==2&&a[0]=='.'&&a[1]=='.'))return 0;if(!*q)break;a=q+1;}}return 1;}
static int write_file(int root,const char *path,const char *bytes,size_t n){
 char copy[513];snprintf(copy,sizeof copy,"%s",path);int dir=dup(root);if(dir<0)return 0;char *at=copy,*slash=NULL;while((slash=strchr(at,'/'))){*slash=0;if(mkdirat(dir,at,0700)&&errno!=EEXIST){close(dir);return 0;}int next=openat(dir,at,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);close(dir);if(next<0)return 0;dir=next;at=slash+1;}
 int fd=openat(dir,at,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);close(dir);if(fd<0)return 0;size_t used=0;while(used<n){ssize_t k=write(fd,bytes+used,n-used);if(k<0&&errno==EINTR)continue;if(k<=0)break;used+=(size_t)k;}int ok=used==n&&!fsync(fd);if(close(fd))ok=0;return ok;
}
static int includes(J *a,const char *s){for(size_t i=0;a&&i<json_object_array_length(a);i++)if(eq(json_object_get_string(json_object_array_get_idx(a,i)),s))return 1;return 0;}
static const char *compatibility(dc_store *s,J *version,const char *kind){
#if defined(__APPLE__)
 const char *platform="macos";
#elif defined(_WIN32)
 const char *platform="windows";
#else
 const char *platform="linux";
#endif
#if defined(__aarch64__) || defined(__arm64__)
 const char *arch="arm64";
#else
 const char *arch="x86_64";
#endif
 if(!includes(get(version,"platforms"),platform)||!includes(get(version,"architectures"),arch))return "platform_or_architecture_unsupported";
 if(!eq(str(get(version,"engines"),"dunlin"),"desktop@1"))return "unknown_dunlin_abi";
 if(json_object_array_length(get(version,"dependencies")))return "dependencies_not_resolved";
 if(eq(kind,"skill"))return json_object_array_length(get(version,"capabilities"))?"skill_execution_capability_requires_plugin":NULL;
 /* Executable packages never become installable just because the UI enables a switch. */
 return dc_extension_gate(s,get(version,"engines"),kind);
}
static void clear_directory(int fd){
 DIR *dir=fdopendir(dup(fd));if(!dir)return;struct dirent *e;while((e=readdir(dir))){if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;struct stat st;if(fstatat(fd,e->d_name,&st,AT_SYMLINK_NOFOLLOW))continue;if(S_ISDIR(st.st_mode)){int child=openat(fd,e->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(child>=0){clear_directory(child);close(child);unlinkat(fd,e->d_name,AT_REMOVEDIR);}}else unlinkat(fd,e->d_name,0);}closedir(dir);
}
static J *install(dc_store *s,J *r,const dc_hooks *hooks){
 char staging[PATH_MAX]={0};int keep_staging=0;void *extension_scope=NULL;
 const char *id=str(r,"source"),*package=str(r,"package"),*version=str(r,"version");if(!ident(id)||!ident(package)||!text(version,128))return fail("invalid_package_reference");sqlite3_stmt *q=prep(s,"SELECT payload FROM catalog_sources WHERE id=? AND enabled=1");if(q)db_bind(q,1,id);const char *raw=q&&sqlite3_step(q)==SQLITE_ROW?(const char*)sqlite3_column_text(q,0):NULL;J *payload=raw?dunlin_json_parse(raw,strlen(raw)):NULL;sqlite3_finalize(q);if(!valid_payload(payload)){json_object_put(payload);return fail("catalog_missing_or_expired");}
 J *entry=NULL,*chosen=NULL,*entries=get(payload,"entries");for(size_t i=0;i<json_object_array_length(entries);i++){J *e=json_object_array_get_idx(entries,i);if(eq(str(e,"packageId"),package)){entry=e;J *versions=get(e,"versions");for(size_t j=0;j<json_object_array_length(versions);j++){J *v=json_object_array_get_idx(versions,j);if(eq(str(v,"version"),version))chosen=v;}}}
 const char *error="version_missing";if(!chosen)goto done;J *withdrawals=get(payload,"withdrawals");for(size_t i=0;i<json_object_array_length(withdrawals);i++){J *w=json_object_array_get_idx(withdrawals,i);if(eq(str(w,"packageId"),package)&&eq(str(w,"version"),version)){error="version_withdrawn";goto done;}}
 error=compatibility(s,chosen,str(entry,"kind"));if(error)goto done;
 /* Import accepts the identical signed archive bytes for offline operation. */
 transfer t={.limit=12582912,.hooks=hooks};long status=0;const char *provided=str(r,"archive");char *owned=NULL;size_t length=0;
 if(provided){length=strlen(provided);if(length>t.limit){error="archive_exceeds_limit";goto done;}}else{if(!fetch(str(get(chosen,"artifactSource"),"url"),NULL,&t,&status)||status!=200){free(t.bytes);error="package_download_failed";goto done;}owned=t.bytes;provided=owned;length=t.used;}
 char digest[65];dc_sha256(provided,length,digest);if(!eq(digest,str(chosen,"digest"))){free(owned);error="package_digest_mismatch";goto done;}J *archive=dunlin_json_parse(provided,length);free(owned);J *manifest=get(archive,"manifest"),*files=get(archive,"files");const char *manifest_raw=str(archive,"manifestJSON");J *exact=manifest_raw?dunlin_json_parse(manifest_raw,strlen(manifest_raw)):NULL;
 if(!exact||!json_object_equal(exact,manifest)){error="manifest_bytes_missing_or_mismatch";goto archive_done;}dc_sha256(manifest_raw,strlen(manifest_raw),digest);
 if(!eq(digest,str(chosen,"manifestDigest"))||!eq(str(archive,"protocol"),"dunlin.files")||json_object_get_int(get(archive,"major"))!=1||!eq(str(manifest,"packageId"),package)||!eq(str(manifest,"version"),version)||!eq(str(manifest,"kind"),str(entry,"kind"))||!json_object_equal(get(manifest,"engines"),get(chosen,"engines"))||!json_object_equal(get(manifest,"capabilities"),get(chosen,"capabilities"))||!json_object_equal(get(manifest,"platforms"),get(chosen,"platforms"))||!json_object_equal(get(manifest,"architectures"),get(chosen,"architectures"))||!json_object_equal(get(manifest,"dependencies"),get(chosen,"dependencies"))||!list(archive,"files",128)){error="manifest_contract_mismatch";goto archive_done;}
 char parent[PATH_MAX];if(snprintf(parent,sizeof parent,"%s/catalog-staging",s->directory)>=(int)sizeof parent){error="path_too_long";goto archive_done;}if(mkdir(parent,0700)&&errno!=EEXIST){error="staging_unavailable";goto archive_done;}struct stat st;if(lstat(parent,&st)||!S_ISDIR(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&077)){error="staging_unavailable";goto archive_done;}snprintf(staging,sizeof staging,"%s/package-XXXXXX",parent);if(!mkdtemp(staging)){error="staging_unavailable";goto archive_done;}
 int dir=open(staging,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);size_t total=0;int ok=dir>=0;for(size_t i=0;ok&&i<json_object_array_length(files);i++){J *f=json_object_array_get_idx(files,i);const char *path=str(f,"path");size_t n=0;char *data=base64(f,"data",1048576,&n);if(!data&&eq(str(f,"data"),"")){data=strdup("");n=0;}if(data)dc_sha256(data,n,digest);ok=path_ok(path)&&data&&total+n<=8388608&&eq(digest,str(f,"digest"))&&write_file(dir,path,data,n);free(data);total+=n;}
 if(dir>=0){if(fsync(dir))ok=0;close(dir);}if(!ok){error="unsafe_or_incomplete_archive";goto archive_done;}
 {extension_scope=dc_extensions_scope_enter(s);if(!extension_scope){error="extension_state_unavailable";goto archive_done;}if(!sql(s,"BEGIN IMMEDIATE")){error="storage_error";goto archive_done;}char source[160];snprintf(source,sizeof source,"catalog:%s",id);J *result=eq(str(entry,"kind"),"skill")?dc_skills_import_verified(s,staging,source,package,version):dc_extension_import(s,source,staging,manifest);if(!json_object_get_boolean(get(result,"ok"))){error="package_archive_import_failed";json_object_put(result);sql(s,"ROLLBACK");goto archive_done;}json_object_put(result);
 q=prep(s,"INSERT INTO catalog_packages(source,package,version,digest,kind,manifest,path) VALUES(?,?,?,?,?,?,?) ON CONFLICT(source,package,version) DO NOTHING");if(q){db_bind(q,1,id);db_bind(q,2,package);db_bind(q,3,version);db_bind(q,4,str(chosen,"digest"));db_bind(q,5,str(entry,"kind"));db_bind(q,6,manifest_raw);db_bind(q,7,staging);}error=step(q)?NULL:"storage_error";keep_staging=!error&&sqlite3_changes(s->db)>0;if(error||!sql(s,"COMMIT")){sql(s,"ROLLBACK");error="storage_error";}}
 archive_done:dc_extensions_scope_leave(extension_scope);if(*staging&&(!keep_staging||error)){int cleanup=open(staging,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(cleanup>=0){clear_directory(cleanup);close(cleanup);rmdir(staging);}}json_object_put(exact);json_object_put(archive);
 done:json_object_put(payload);return error?fail(error):reply(1,NULL,json_object_new_object());
}
#include "discovery.h"
#include "npm_install.h"
J *dc_catalog_request(dc_store *s,J *r,const dc_hooks *hooks){
 const char *op=str(r,"op");if(!op)return fail("invalid_catalog_request");if(eq(op,"catalog.npm.versions"))return npm_versions(r,hooks);if(eq(op,"catalog.npm.install"))return npm_install(s,r,hooks);if(!strncmp(op,"catalog.community.",18))return discovery_request(s,r,hooks);if(eq(op,"catalog.config")){char path[PATH_MAX];if(!source_path(s,path))return fail("source_config_path_invalid");J *data=json_object_new_object();json_object_object_add(data,"path",json_object_new_string(path));return reply(1,NULL,data);}const char *config_error=source_config_sync(s);if(config_error)return fail(config_error);if(eq(op,"catalog.list"))return sources(s);const char *id=str(r,"id");
 if(eq(op,"catalog.add")){
  const char *url=str(r,"url"),*key=str(r,"publicKey"),*title=str(r,"title");if(!ident(id)||!https(url)||!hex(key,64)||!text(title,256))return fail("invalid_source_or_public_key");if(!sql(s,"BEGIN IMMEDIATE"))return fail("storage_error");sqlite3_stmt *q=prep(s,"INSERT INTO catalog_sources(id,url,title,public_key) VALUES(?,?,?,?)");if(q){db_bind(q,1,id);db_bind(q,2,url);db_bind(q,3,title);db_bind(q,4,key);}if(!step(q)){sql(s,"ROLLBACK");return fail("source_already_exists_or_storage_error");}const char *error=source_config_finish(s);return error?fail(error):sources(s);
 }
 if(eq(op,"catalog.install"))return install(s,r,hooks);if(!ident(id))return fail("invalid_source_id");
 if(eq(op,"catalog.verify"))return accept_catalog(s,id,str(r,"envelope"),NULL);
 if(eq(op,"catalog.refresh"))return refresh(s,id,hooks);
 if(eq(op,"catalog.remove")||eq(op,"catalog.restore")||eq(op,"catalog.rename")){
  if(eq(op,"catalog.rename")&&!text(str(r,"title"),256))return fail("invalid_source_or_public_key");
  if(!sql(s,"BEGIN IMMEDIATE"))return fail("storage_error");
  sqlite3_stmt *q=prep(s,eq(op,"catalog.rename")?"UPDATE catalog_sources SET title=? WHERE id=?":"UPDATE catalog_sources SET enabled=? WHERE id=?");
  if(q){if(eq(op,"catalog.rename"))db_bind(q,1,str(r,"title"));else sqlite3_bind_int(q,1,eq(op,"catalog.restore"));db_bind(q,2,id);}
  if(!step(q)||sqlite3_changes(s->db)!=1){sql(s,"ROLLBACK");return fail("source_missing_or_storage_error");}
  const char *error=source_config_finish(s);return error?fail(error):sources(s);
 }
 return fail("unsupported_catalog_operation");
}
char *dc_catalog_call(dc_store *s,const char *raw,const dc_hooks *hooks){
 if(!s||!raw||strlen(raw)>16777216)return NULL;J *r=dunlin_json_parse(raw,strlen(raw));J *out=json_object_is_type(r,json_type_object)?dc_catalog_request(s,r,hooks):fail("invalid_catalog_json");char *result=out?strdup(json_object_to_json_string_ext(out,JSON_C_TO_STRING_PLAIN)):NULL;json_object_put(out);json_object_put(r);return result;
}
