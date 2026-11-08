#include "../../dunlin-copilot/esp32/common/https_policy.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
int main(void){
 const char *hosts[]={"example.com","api.open-meteo.com"};char host[96];
 const char *good[]={"https://api.ip.sb/ip","HTTPS://EXAMPLE.COM/","https://EXAMPLE.com:443/a?q=a@b#part","https://example.com","https://example.com?x=1","https://example.com:8443/","https://1.1.1.1/","https://[2001:db8::1]:443/"};
 for(size_t i=0;i<sizeof good/sizeof *good;i++)assert(ae_https_permitted(good[i],1,hosts,2,host,sizeof host));
 assert(ae_https_permitted("https://EXAMPLE.com:443/",0,hosts,2,host,sizeof host) && !strcmp(host,"example.com"));
 assert(!ae_https_permitted("https://api.ip.sb/ip",0,hosts,2,host,sizeof host));
 assert(!ae_https_permitted("https://example.com.evil.test/",0,hosts,2,host,sizeof host));
 assert(!ae_https_permitted("https://evil.example.com/",0,hosts,2,host,sizeof host));
 const char *bad[]={"http://example.com/","ftp://example.com/","https:///x","https://user@example.com/","https://example.com@evil.test/","https://example.com%2fevil/","https://example.com:0/","https://example.com:65536/","https://example.com:/","https://example.com:443:80/","https://example.com\\evil/","https://example.com/\r\nHeader:x","https://example.com/a b","https://[::1/","https://[foo]/","https://[]/","https://[::1]evil/"};
 for(size_t i=0;i<sizeof bad/sizeof *bad;i++)assert(!ae_https_permitted(bad[i],1,hosts,2,host,sizeof host));
 char tiny[2];assert(!ae_https_permitted(good[0],1,hosts,2,tiny,sizeof tiny));char longurl[800];memset(longurl,'a',sizeof longurl);memcpy(longurl,"https://example.com/",20);longurl[799]=0;assert(!ae_https_permitted(longurl,1,hosts,2,host,sizeof host));
 assert(!ae_https_permitted(NULL,1,hosts,2,host,sizeof host));puts("PASS trusted HTTPS/optional exact allowlist, bounded URL and authority/port/control/userinfo negatives; TLS still separate");
}
