#include <dunlin/compact/embedded.h>
#include <string.h>
static ae_reply dispatch(void *user,const ae_request *r) {
 ae_io_plugin *p=user;
 if(!r)return (ae_reply){AE_INVALID,"missing request"};
 if(r->op<AE_GPIO_READ || r->op>AE_DIAGNOSTICS || r->length>AE_DATA_MAX)
  return (ae_reply){AE_INVALID,"invalid request"};
 if(!memchr(r->name,0,sizeof r->name) || !memchr(r->secret,0,sizeof r->secret) || !memchr(r->address,0,sizeof r->address))
  return (ae_reply){AE_INVALID,"unterminated request string"};
 if(r->op==AE_NET_SEND && (!r->port || r->port>65535 || !r->address[0]))return (ae_reply){AE_INVALID,"invalid network address/port"};
 if(r->op==AE_WIFI_SET) {
  size_t n=strlen(r->secret);
  if(!r->name[0] || (n && n<8))return (ae_reply){AE_INVALID,"SSID required; WPA password 8..63 bytes, 64 hex, or empty"};
  if(n==64)for(size_t i=0;i<n;i++)if(!((r->secret[i]>='0' && r->secret[i]<='9') || (r->secret[i]>='a' && r->secret[i]<='f') || (r->secret[i]>='A' && r->secret[i]<='F')))return (ae_reply){AE_INVALID,"64-byte PSK must be hexadecimal"};
 }
 if(r->op==AE_BLE_NAME && (!r->name[0] || strlen(r->name)>24))return (ae_reply){AE_INVALID,"BLE name: 1..24 bytes"};
 if(r->op<=AE_PWM_STOP) {
  if(r->pin>=64 || !(p->allowed_pins & (UINT64_C(1)<<r->pin)))
   return (ae_reply){AE_DENIED,"pin not granted; use io allow PIN"};
 }
 if(r->op==AE_UART_OPEN && (r->tx>=64 || r->rx>=64 || r->tx==r->rx ||
    !(p->allowed_pins&(UINT64_C(1)<<r->tx)) || !(p->allowed_pins&(UINT64_C(1)<<r->rx))))
  return (ae_reply){AE_DENIED,"UART pins not granted"};
 if((r->op==AE_GPIO_WRITE && r->value>1) || (r->op==AE_PWM && (!r->frequency || r->frequency>20000 || r->value>1023)) ||
    (r->op==AE_UART_OPEN && (r->baud<1200 || r->baud>115200)))
  return (ae_reply){AE_INVALID,"IO value out of range"};
 return p->hal->call(p->hal->user,r);
}
static int start(ak_scope *s,void *config) {
 ae_io_plugin *p=config;
 if(!p || !p->hal || p->hal->abi!=AE_ABI || !p->hal->call)return -1;
 p->allowed_pins=0;p->service=(ae_io_v1){AE_ABI,p,dispatch};
 return ak_provide(s,AE_IO_SERVICE,AE_ABI,&p->service)?0:-1;
}
static void stop(ak_scope *s,void *config) {(void)s;((ae_io_plugin *)config)->allowed_pins=0;}
static const ak_key provides[]={{AE_IO_SERVICE,AE_ABI},{NULL,0}};
const ak_plugin ae_io_descriptor={AK_ABI,"embedded-io",provides,NULL,NULL,start,stop,NULL};
int ae_io_allow(ae_io_plugin *p,unsigned pin,int allow) {
 if(!p || pin>=64 || !(p->valid_pins&(UINT64_C(1)<<pin)))return 0;
 if(allow)p->allowed_pins|=UINT64_C(1)<<pin;else p->allowed_pins&=~(UINT64_C(1)<<pin);
 return 1;
}
