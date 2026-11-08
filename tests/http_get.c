#include "../dunlin-compact/platform/http_get.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc,char **argv) {
    char error[160];
    assert(ah_https_url_check("https://example.com/path?q=public",error,sizeof error));
    const char *blocked[]={
        "http://example.com/","file:///etc/passwd","https://localhost/","https://host.localhost/",
        "https://127.0.0.1/","https://10.0.0.1/","https://169.254.169.254/latest/meta-data/",
        "https://192.168.1.1/","https://[::1]/","https://[fc00::1]/","https://[2001:db8::1]/",
        "https://user:secret@example.com/","https://example.com/\nnext"
    };
    for(size_t i=0;i<sizeof blocked/sizeof *blocked;i++) {
        if(ah_https_url_check(blocked[i],error,sizeof error)) fprintf(stderr,"unexpected allowed URL: %s\n",blocked[i]);
        assert(!ah_https_url_check(blocked[i],error,sizeof error));
    }
    if(argc==2) {
        char *body=NULL;
        ah_status status=ah_https_get(argv[1],NULL,32768,10000,&body);
        if(status!=AH_DONE) fprintf(stderr,"live GET failed: status=%d detail=%s\n",status,body?body:"(none)");
        assert(status==AH_DONE&&body&&*body&&ah_utf8_text(body,strlen(body)));
        free(body);
    }
    puts(argc==2?"PASS readonly HTTPS policy and live GET":"PASS readonly HTTPS URL policy");
    return 0;
}
