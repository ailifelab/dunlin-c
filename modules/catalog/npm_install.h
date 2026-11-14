/* Explicit npm package installation: TLS + registry SHA-512, no lifecycle scripts.
 * Bounded tar/gzip extraction is independent of the interpreter and UI. */
#include <zlib.h>
#include "../../desktop/package_limits.h"
int dc_npm_extract(int root,const char *bytes,size_t length);
static const char *community_npm_name(const char *repo){
 const char *repos[]={"https://github.com/dsh-market/dsh-market","https://github.com/omdsh-dev/DSH-better-sidebar","https://github.com/shanliuling/dsh-image-gen","https://github.com/PerryLink/dsh-checkpoint-rewind","https://github.com/zhijun-dai/Catppuccin-dsh-theme","https://github.com/hoyyang/dsh-project-manager"};
 const char *names[]={"dshmarket","dsh-better-sidebar","dsh-image-gen","dsh-checkpoint-rewind","dsh-catppuccin","dsh-project-manager"};
 for(size_t i=0;i<sizeof repos/sizeof *repos;i++)if(eq(repo,repos[i]))return names[i];return NULL;
}
/* An index suggests a package; only registry metadata binds it to a repository.
 * Never turn an index URL or an install command into a download target. */
static int npm_component(const char *s,size_t n){
 return n && s[0]!='.' && s[0]!='_' && strspn(s,"abcdefghijklmnopqrstuvwxyz0123456789._-")==n;
}
static int npm_name_valid(const char *s){
 if(!s||!*s||strlen(s)>214)return 0;
 if(*s!='@')return npm_component(s,strlen(s));
 const char *slash=strchr(++s,'/');return slash&&npm_component(s,(size_t)(slash-s))&&npm_component(slash+1,strlen(slash+1));
}
static const char *npm_candidate(J *r){
 const char *repo=str(r,"repository"),*known=community_npm_name(repo),*name=str(r,"package");
 if(known)return !name||eq(name,known)?known:NULL;
 return https(repo)&&npm_name_valid(name)?name:NULL;
}
static int npm_same_repository(J *metadata,const char *repo){
 J *field=get(metadata,"repository");const char *raw=json_object_is_type(field,json_type_string)?json_object_get_string(field):str(field,"url");
 if(!raw||!repo)return 0;
 if(!strncmp(raw,"git+",4))raw+=4;
 if(!https(raw)||!https(repo))return 0;
 size_t n=strlen(raw),m=strlen(repo);
 while(n&&raw[n-1]=='/')n--;while(m&&repo[m-1]=='/')m--;
 if(n>4&&!strncmp(raw+n-4,".git",4))n-=4;if(m>4&&!strncmp(repo+m-4,".git",4))m-=4;
 return n==m&&!strncmp(raw,repo,n);
}
static const char *npm_export(J *value){
 if(json_object_is_type(value,json_type_string))return json_object_get_string(value);
 const char *p=str(value,"require");return p?p:str(value,"default");
}
static const char *npm_path(const char *p){
 if(!p)return NULL;if(!strncmp(p,"./",2))p+=2;
 return *p&&strlen(p)<=256&&*p!='/'&&!strstr(p,"..")&&!strstr(p,"//")&&strspn(p,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-/")==strlen(p)?p:NULL;
}
static int npm_version(const char *s){return s&&*s&&strlen(s)<96&&strspn(s,"0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ.-+")==strlen(s);}
static J *npm_versions(J *r,const dc_hooks *hooks){
 const char *name=npm_candidate(r);if(!name)return fail("npm_package_or_version_unsupported");char url[512];snprintf(url,sizeof url,"https://registry.npmjs.org/%s",name);transfer t={.limit=4194304,.hooks=hooks};long status=0;int ok=fetch(url,NULL,&t,&status);J *m=ok&&status==200?dunlin_json_parse(t.bytes,t.used):NULL;free(t.bytes);J *versions=get(m,"versions");if(!json_object_is_type(versions,json_type_object)||!eq(str(m,"name"),name)){json_object_put(m);return fail(status==404?"npm_version_not_found":"npm_metadata_download_failed");}if(!community_npm_name(str(r,"repository"))&&!npm_same_repository(m,str(r,"repository"))){json_object_put(m);return fail("npm_repository_mismatch");}J *a=json_object_new_array();const char *latest=str(get(m,"dist-tags"),"latest");if(npm_version(latest)&&get(versions,latest))json_object_array_add(a,json_object_new_string(latest));json_object_object_foreach(versions,key,value){(void)value;if(npm_version(key)&&!eq(key,latest)&&json_object_array_length(a)<512)json_object_array_add(a,json_object_new_string(key));}json_object_put(m);return reply(1,NULL,a);
}
static J *npm_install(dc_store *s,J *r,const dc_hooks *hooks){
 const char *name=npm_candidate(r),*version=str(r,"version");if(!name||!npm_version(version))return fail("npm_package_or_version_unsupported");
 J *engines=json_object_new_object();json_object_object_add(engines,"dunlin",json_object_new_string("desktop@1"));json_object_object_add(engines,"host",json_object_new_string("node-dsh"));json_object_object_add(engines,"dsh",json_object_new_string("0.2.1-rc.3"));const char *gate=dc_extension_gate(s,engines,"plugin");if(gate){json_object_put(engines);return fail(gate);}
 char url[512];snprintf(url,sizeof url,"https://registry.npmjs.org/%s/%s",name,version);transfer meta={.limit=4194304,.hooks=hooks};long status=0;J *m=NULL,*result=NULL;char *unpacked=NULL;char staging[PATH_MAX]={0};const char *error="npm_metadata_download_failed";
 if(!fetch(url,NULL,&meta,&status)||status!=200){if(status==404)error="npm_version_not_found";goto done;}m=dunlin_json_parse(meta.bytes,meta.used);if(!eq(str(m,"name"),name)||!eq(str(m,"version"),version)){error="npm_metadata_invalid";goto done;}
 if(!community_npm_name(str(r,"repository"))&&!npm_same_repository(m,str(r,"repository"))){error="npm_repository_mismatch";goto done;}
 const char *integrity=str(get(m,"dist"),"integrity"),*tarball=str(get(m,"dist"),"tarball");if(!integrity||strncmp(integrity,"sha512-",7)||strlen(integrity+7)!=88||strspn(integrity+7,"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=")!=88||strcmp(integrity+7+86,"==")||!tarball||strncmp(tarball,"https://registry.npmjs.org/",27)||!https(tarball)){error="npm_integrity_missing";goto done;}
 transfer tar={.limit=DC_PACKAGE_MAX_ARCHIVE,.archive_timeout_ms=300000L,.hooks=hooks};status=0;if(!fetch(tarball,NULL,&tar,&status)||status!=200){free(tar.bytes);error="npm_archive_download_failed";goto done;}
 unsigned char digest[64],expected[128];unsigned int digest_size=0;int bytes=EVP_DecodeBlock(expected,(const unsigned char*)integrity+7,(int)strlen(integrity+7));if(bytes!=66||!EVP_Digest(tar.bytes,tar.used,digest,&digest_size,EVP_sha512(),NULL)||digest_size!=64||memcmp(digest,expected,64)){free(tar.bytes);error="npm_integrity_failed";goto done;}
 unpacked=malloc(DC_PACKAGE_MAX_TOTAL);if(!unpacked){free(tar.bytes);error="out_of_memory";goto done;}z_stream z={0};z.next_in=(unsigned char*)tar.bytes;z.avail_in=(uInt)tar.used;z.next_out=(unsigned char*)unpacked;z.avail_out=DC_PACKAGE_MAX_TOTAL;int initialized=inflateInit2(&z,15+16)==Z_OK;int inflated=initialized?inflate(&z,Z_FINISH):Z_DATA_ERROR;size_t length=z.total_out;int complete=initialized&&inflated==Z_STREAM_END&&z.avail_in==0;if(initialized)inflateEnd(&z);free(tar.bytes);if(!complete){error="npm_archive_exceeds_limit_or_invalid";goto done;}
 if(snprintf(staging,sizeof staging,"%s/npm-install-XXXXXX",s->directory)>=(int)sizeof staging||!mkdtemp(staging)){staging[0]=0;error="npm_staging_failed";goto done;}
 int root=open(staging,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(root<0){error="npm_staging_failed";goto done;}int valid=dc_npm_extract(root,unpacked,length);
 close(root);if(!valid){error="npm_archive_layout_unsupported";goto done;}
 char package_path[PATH_MAX];snprintf(package_path,sizeof package_path,"%s/package.json",staging);FILE *f=fopen(package_path,"rb");char pkg_bytes[32769];size_t n=f?fread(pkg_bytes,1,32768,f):0;if(f)fclose(f);J *pkg=dunlin_json_parse(pkg_bytes,n);int known=community_npm_name(str(r,"repository"))!=NULL;const char *entry=npm_path(str(pkg,"main"));if(!entry&&!known)entry=npm_path(npm_export(get(get(pkg,"exports"),".")));
 const char *client_path=known?(eq(name,"dshmarket")?"client/client.js":eq(name,"dsh-checkpoint-rewind")?"client/index.js":"lib/client.js"):npm_path(npm_export(get(get(pkg,"exports"),"./client")));
 if(!known&&!client_path&&get(get(pkg,"dsh"),"client")){char candidate[PATH_MAX];snprintf(candidate,sizeof candidate,"%s/lib/client.js",staging);struct stat st;if(lstat(candidate,&st)==0&&S_ISREG(st.st_mode))client_path="lib/client.js";}
 if(!eq(str(pkg,"name"),name)||!eq(str(pkg,"version"),version)||!entry||strlen(entry)>256){json_object_put(pkg);error="npm_entry_invalid";goto done;}
 char adapter_version[160];snprintf(adapter_version,sizeof adapter_version,"%s%sdunlin.3",version,strchr(version,'+')?".":"+");
 J *manifest=json_object_new_object();json_object_object_add(manifest,"protocol",json_object_new_string("dunlin.extension"));json_object_object_add(manifest,"major",json_object_new_int(1));json_object_object_add(manifest,"packageId",json_object_new_string(name));json_object_object_add(manifest,"version",json_object_new_string(adapter_version));json_object_object_add(manifest,"upstreamVersion",json_object_new_string(version));json_object_object_add(manifest,"adapterRevision",json_object_new_int(3));json_object_object_add(manifest,"displayName",json_object_new_string(name));json_object_object_add(manifest,"kind",json_object_new_string("plugin"));json_object_object_add(manifest,"engines",json_object_get(engines));json_object_object_add(manifest,"entry",json_object_new_string(entry));json_object_object_add(manifest,"requires",json_object_new_array());if(!known)json_object_object_add(manifest,"repository",json_object_new_string(str(r,"repository")));if(client_path){J *client=json_object_new_object(),*slots=json_object_new_array();json_object_object_add(client,"format",json_object_new_string("dsh-lazy-cjs@1"));json_object_object_add(client,"entry",json_object_new_string(client_path));json_object_array_add(slots,json_object_new_string(eq(name,"dsh-catppuccin")?"settings.general.item":"dunlin.workbench"));json_object_object_add(client,"slots",slots);json_object_object_add(manifest,"client",client);}json_object_object_add(manifest,"dependencies",json_object_new_array());J *caps=json_object_new_array();json_object_array_add(caps,json_object_new_string("trusted-full-host"));json_object_object_add(manifest,"capabilities",caps);json_object_put(pkg);
 snprintf(package_path,sizeof package_path,"%s/dunlin.json",staging);f=fopen(package_path,"wb");const char *raw=json_object_to_json_string_ext(manifest,JSON_C_TO_STRING_PLAIN);int saved=f&&fwrite(raw,1,strlen(raw),f)==strlen(raw);if(f&&fclose(f))saved=0;json_object_put(manifest);if(!saved){error="npm_staging_failed";goto done;}char source[256];snprintf(source,sizeof source,"npm:%s",name);result=dc_extension_import(s,source,staging,NULL);
 done:if(*staging){int dir=open(staging,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(dir>=0){clear_directory(dir);close(dir);}rmdir(staging);}json_object_put(engines);json_object_put(m);free(meta.bytes);free(unpacked);return result?result:fail(hooks&&hooks->cancelled&&hooks->cancelled(hooks->user)?"cancelled":error);
}
