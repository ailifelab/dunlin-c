#include <dunlin/compact/embedded.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned calls;static ae_request last;static int fail;
static ae_reply hal(void *unused,const ae_request *r) {(void)unused;calls++;last=*r;return fail?(ae_reply){AE_IO_ERROR,"driver failed"}:(ae_reply){AE_OK,"observed"};}
static char queued_line[AE_LINE_MAX];
static ae_reply queued(void *user,const char *line,size_t n){(void)user;assert(n<sizeof queued_line);memcpy(queued_line,line,n);queued_line[n]=0;return (ae_reply){AE_OK,"queued"};}
static char captured[8192];static size_t used;
static void write_out(void *u,const char *s,size_t n) {(void)u;assert(used+n<sizeof captured);memcpy(captured+used,s,n);used+=n;captured[used]=0;}
static ae_reply run(ak_scope *s,ae_io_plugin *p,const char *line) {return ae_execute(s,p,line,strlen(line));}
int main(void) {
 ae_pool pool={0};ak_platform host={.user=&pool,.alloc=ae_pool_alloc,.free=ae_pool_free};
 ak_context *ctx=ak_create(&host);assert(ctx);assert(ae_kernel_probe(ctx));ae_io_v1 device={AE_ABI,NULL,hal};
 ae_io_plugin io={.hal=&device,.valid_pins=(UINT64_C(1)<<18)|(UINT64_C(1)<<19)};ak_scope *plugin;
 assert(ak_install(ctx,&ae_io_descriptor,&io,&plugin)==AK_OK);ak_scope *s=ak_root(ctx);
 assert(run(s,&io,"gpio write 18 1").status==AE_DENIED && calls==0);
 assert(run(s,&io,"io allow 18").status==AE_OK);assert(run(s,&io,"io allow 6").status==AE_DENIED);
 assert(run(s,&io,"gpio write 18 2").status==AE_INVALID && calls==0);
 assert(run(s,&io,"pwm 18 99999999999999999999 20").status==AE_INVALID);
 assert(run(s,&io,"pwm 18 0 20").status==AE_INVALID);
 assert(run(s,&io,"pwm 18 1000 512").status==AE_OK && last.op==AE_PWM && last.frequency==1000 && last.value==512);
 assert(run(s,&io,"uart open 9600 18 19").status==AE_DENIED);
 assert(run(s,&io,"io allow 19").status==AE_OK);assert(run(s,&io,"uart open 9600 18 19").status==AE_OK);
 assert(run(s,&io,"uart send 00aaff").status==AE_OK && last.length==3 && last.data[1]==0xaa);
 unsigned before=calls;assert(run(s,&io,"uart send 0xx").status==AE_INVALID && calls==before);
 assert(run(s,&io,"wifi set \"a b\" \"secret x\"").status==AE_OK && !strcmp(last.name,"a b") && !strcmp(last.secret,"secret x"));
 assert(run(s,&io,"wifi set x \"\"").status==AE_OK && last.secret[0]==0);
 assert(run(s,&io,"wifi set x \"unterminated").status==AE_INVALID);
 assert(run(s,&io,"bt name \"test name\"").status==AE_OK && last.op==AE_BLE_NAME);
 assert(run(s,&io,"bt read").status==AE_OK && last.op==AE_BLE_READ);
 assert(run(s,&io,"net send 127.0.0.1 1234 000102").status==AE_OK && last.port==1234);
 fail=1;assert(run(s,&io,"gpio read 18").status==AE_IO_ERROR);fail=0;
 const ae_io_v1 *api=ak_get(s,AE_IO_SERVICE,AE_ABI);ae_request invalid={.op=AE_NET_SEND,.port=70000};before=calls;
 assert(api->call(api->user,&invalid).status==AE_INVALID && calls==before);
 invalid.op=AE_WIFI_SET;memset(invalid.name,'x',sizeof invalid.name);
 assert(api->call(api->user,&invalid).status==AE_INVALID && calls==before);
 char garbage[AE_LINE_MAX];unsigned random=1973;
 for(unsigned i=0;i<20000;i++) {
  size_t len=i%AE_LINE_MAX;
  for(size_t j=0;j<len;j++){random=random*1664525u+1013904223u;garbage[j]=(char)(random>>24);}
  (void)ae_execute(s,&io,garbage,len);
 }
 size_t live=pool.live,peak=pool.peak;
 for(unsigned i=0;i<10000;i++)assert(run(s,&io,"gpio read 18").status==AE_OK);
 assert(pool.live==live && pool.peak==peak);
 ae_serial serial;ae_serial_init(&serial,s,&io,write_out,NULL);
 const unsigned char input[]="gpio read 18\r\n";before=calls;
 for(size_t i=0;i<sizeof input-1;i++)ae_serial_feed(&serial,input+i,1);
 assert(calls==before+1);
 unsigned char huge[AE_LINE_MAX+20];memset(huge,'a',sizeof huge);ae_serial_feed(&serial,huge,sizeof huge);ae_serial_feed(&serial,(unsigned char *)"\ngpio read 18\n",14);
 assert(calls==before+2 && strstr(captured,"line discarded"));
 const unsigned char erase[]="gpio read 19\b8\n";ae_serial_feed(&serial,erase,sizeof erase-1);assert(last.pin==18);
 before=calls;const unsigned char control[]="gpio read 18\x1b[A\n";ae_serial_feed(&serial,control,sizeof control-1);assert(calls==before);
 const unsigned char cancel[]="gpio read 18\003\n";ae_serial_feed(&serial,cancel,sizeof cancel-1);assert(calls==before);
 const unsigned char wifi[]="wifi set ssid SECRET_PASSWORD\n";ae_serial_feed(&serial,wifi,sizeof wifi-1);assert(!strstr(captured,"SECRET_PASSWORD"));
 for(size_t i=0;i<sizeof serial.line;i++)assert(serial.line[i]==0);
 before=calls;ae_serial_set_submit(&serial,queued,NULL);
 ae_serial_feed(&serial,(unsigned char *)"gpio read 18\n",13);
 assert(calls==before && !strcmp(queued_line,"gpio read 18"));
 const unsigned char chinese[]="你好界\b！\n";
 for(size_t i=0;i<sizeof chinese-1;i++)ae_serial_feed(&serial,chinese+i,1);
 assert(!strcmp(queued_line,"你好！"));
 const unsigned char emoji[]="a🙂\177b\n";ae_serial_feed(&serial,emoji,sizeof emoji-1);assert(!strcmp(queued_line,"ab"));
 ae_serial_set_submit(&serial,NULL,NULL);
 assert(ak_remove(plugin)==AK_OK && !ak_get(s,AE_IO_SERVICE,AE_ABI) && io.allowed_pins==0);
 assert(run(s,&io,"gpio read 18").status==AE_UNSUPPORTED);
 assert(ak_destroy(ctx)==AK_OK && pool.live==0);
 void *blocks[AE_POOL_SLOTS];for(size_t i=0;i<AE_POOL_SLOTS;i++){blocks[i]=ae_pool_alloc(&pool,AE_SLOT_BYTES);assert(blocks[i]);}
 assert(!ae_pool_alloc(&pool,1) && !ae_pool_alloc(&pool,AE_POOL_SLOTS*AE_SLOT_BYTES+1));
 for(size_t i=0;i<AE_POOL_SLOTS;i++){ae_pool_free(&pool,blocks[i]);}
 assert(pool.live==0);
 void *large=ae_pool_alloc(&pool,AE_SLOT_BYTES+1);assert(large && pool.live==2);ae_pool_free(&pool,large);assert(pool.live==0);
 puts("PASS bounded parser/serial, exact IO requests, board grants, generic errors, no secret echo, 10000 stable commands, unload and allocator exhaustion");
 printf("fixed pool=%zu shell=%zu io=%zu bytes; peak kernel slots=%zu (before exhaustion)\n",sizeof pool,sizeof serial,sizeof io,peak);
 return 0;
}
