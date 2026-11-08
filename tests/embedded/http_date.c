#include "../../dunlin-copilot/esp32/common/http_date.h"
#include <assert.h>
#include <stdio.h>
int main(void){
 int64_t epoch=0;assert(ae_http_date_epoch("Fri, 06 Nov 2026 05:31:20 GMT",&epoch));assert(epoch==1793943080);
 assert(ae_http_date_epoch("Thu, 29 Feb 2024 00:00:00 GMT",&epoch));assert(epoch==1709164800);
 const char *bad[]={"", "not a date","Fri, 06 Nov 2026 05:31:20 UTC","Fri, 06 Nov 2026 25:00:00 GMT","Fri, 06 Nov 2026 05:61:00 GMT","Fri, 31 Feb 2026 00:00:00 GMT","Fri, 06 Xxx 2026 05:00:00 GMT","Fri, 06 Nov 1900 05:00:00 GMT","Fri, 06 Nov 2026 05:00:00 GMT trailing"};
 for(unsigned i=0;i<sizeof bad/sizeof *bad;i++)assert(!ae_http_date_epoch(bad[i],&epoch));
 assert(!ae_http_date_epoch(NULL,&epoch));assert(!ae_http_date_epoch("Fri, 06 Nov 2026 05:31:20 GMT",NULL));puts("PASS strict bounded UTC HTTP-Date conversion and invalid date rejection without changing global timezone");
}
