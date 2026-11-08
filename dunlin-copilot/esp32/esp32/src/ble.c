#include "hal.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nimble/nimble_npl.h"
#include "esp_heap_caps.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
static atomic_int wanted,ready,connection=ATOMIC_VAR_INIT(BLE_HS_CONN_HANDLE_NONE),subscribed;
static int initialized,failed;
static atomic_int advertising_error;
static struct ble_npl_event control_event;
static uint8_t own_type;
static uint16_t tx_handle;
static char device_name[33]="DUNLIN";
static unsigned char received[AE_DATA_MAX];static size_t received_size;
static portMUX_TYPE rx_lock=portMUX_INITIALIZER_UNLOCKED;
static int access_rx(uint16_t conn,uint16_t attr,struct ble_gatt_access_ctxt *ctx,void *arg) {
 (void)conn;(void)attr;(void)arg;
 if(!atomic_load(&wanted))return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
 size_t n=OS_MBUF_PKTLEN(ctx->om);if(n>AE_DATA_MAX)return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
 unsigned char data[AE_DATA_MAX];uint16_t used;
 if(ble_hs_mbuf_to_flat(ctx->om,data,sizeof data,&used))return BLE_ATT_ERR_UNLIKELY;
 portENTER_CRITICAL(&rx_lock);
 if(!atomic_load(&wanted) || received_size+used>sizeof received){portEXIT_CRITICAL(&rx_lock);return BLE_ATT_ERR_INSUFFICIENT_RES;}
 memcpy(received+received_size,data,used);received_size+=used;portEXIT_CRITICAL(&rx_lock);return 0;
}
static int access_tx(uint16_t conn,uint16_t attr,struct ble_gatt_access_ctxt *ctx,void *arg) {
 (void)conn;(void)attr;(void)arg;return os_mbuf_append(ctx->om,"DUNLIN",4)?BLE_ATT_ERR_INSUFFICIENT_RES:0;
}
static const struct ble_gatt_svc_def services[]={
 {.type=BLE_GATT_SVC_TYPE_PRIMARY,.uuid=BLE_UUID16_DECLARE(0xffe0),.characteristics=(struct ble_gatt_chr_def[]){
  {.uuid=BLE_UUID16_DECLARE(0xffe1),.access_cb=access_rx,.flags=BLE_GATT_CHR_F_WRITE},
  {.uuid=BLE_UUID16_DECLARE(0xffe2),.access_cb=access_tx,.val_handle=&tx_handle,.flags=BLE_GATT_CHR_F_READ|BLE_GATT_CHR_F_NOTIFY},
  {0}}},{0}};
static int advertise(void);
static int gap_event(struct ble_gap_event *event,void *arg) {
 (void)arg;
 switch(event->type) {
 case BLE_GAP_EVENT_CONNECT:
  if(!event->connect.status) {
   atomic_store(&connection,event->connect.conn_handle);
   if(!atomic_load(&wanted))ble_gap_terminate(event->connect.conn_handle,BLE_ERR_REM_USER_CONN_TERM);
  }
  else if(atomic_load(&wanted))advertise();
  break;
 case BLE_GAP_EVENT_DISCONNECT:
  atomic_store(&connection,BLE_HS_CONN_HANDLE_NONE);atomic_store(&subscribed,0);
  if(atomic_load(&wanted))advertise();
  break;
 case BLE_GAP_EVENT_SUBSCRIBE:
  if(event->subscribe.attr_handle==tx_handle)atomic_store(&subscribed,event->subscribe.cur_notify);
  break;
 default:break;
 }
 return 0;
}
static int advertise(void) {
 struct ble_hs_adv_fields fields={0};fields.flags=BLE_HS_ADV_F_DISC_GEN|BLE_HS_ADV_F_BREDR_UNSUP;
 fields.name=(const uint8_t *)device_name;fields.name_len=(uint8_t)strlen(device_name);fields.name_is_complete=1;
 /* Legacy advertisement is 31 bytes including field overhead. */
 int e=ble_gap_adv_set_fields(&fields);if(e){atomic_store(&advertising_error,e);return e;}
 struct ble_gap_adv_params params={0};params.conn_mode=BLE_GAP_CONN_MODE_UND;params.disc_mode=BLE_GAP_DISC_MODE_GEN;
 e=ble_gap_adv_start(own_type,NULL,BLE_HS_FOREVER,&params,gap_event,NULL);
 atomic_store(&advertising_error,e);return e;
}
/* GAP transitions run only on the NimBLE host event queue, including stop vs sync. */
static void apply_control(struct ble_npl_event *event) {
 (void)event;
 if(atomic_load(&ready)!=1)return;
 if(atomic_load(&wanted)) {
  if(atomic_load(&connection)==BLE_HS_CONN_HANDLE_NONE && !ble_gap_adv_active())advertise();
 } else {
  ble_gap_adv_stop();int c=atomic_load(&connection);
  if(c!=BLE_HS_CONN_HANDLE_NONE)ble_gap_terminate(c,BLE_ERR_REM_USER_CONN_TERM);
 }
}
static void synced(void) {
 if(ble_hs_id_infer_auto(0,&own_type)){atomic_store(&ready,-1);return;}
 atomic_store(&ready,1);if(atomic_load(&wanted))advertise();
}
static void host_task(void *arg) {(void)arg;nimble_port_run();vTaskDelete(NULL);}
ae_reply ae_esp_ble(const ae_request *r) {
 if(r->op==AE_BLE_NAME) {
  if(initialized)return (ae_reply){AE_BUSY,"set BLE name before first start; reboot to rename"};
  size_t n=strlen(r->name);if(!n || n>24)return (ae_reply){AE_INVALID,"BLE name: 1..24 bytes"};
  memcpy(device_name,r->name,n+1);return (ae_reply){AE_OK,"BLE name configured in RAM"};
 }
 if(r->op==AE_BLE_STATUS) {
  ae_reply out={AE_OK,""};snprintf(out.text,sizeof out.text,"BLE initialized=%d ready=%d wanted=%d connected=%d subscribed=%d adv_error=%d",initialized,atomic_load(&ready),atomic_load(&wanted),atomic_load(&connection)!=BLE_HS_CONN_HANDLE_NONE,atomic_load(&subscribed),atomic_load(&advertising_error));return out;
 }
 if(r->op==AE_BLE_START) {
  if(failed)return (ae_reply){AE_IO_ERROR,"BLE initialization failed; reboot before retry"};
  if(atomic_load(&wanted))return (ae_reply){AE_OK,"BLE already requested"};
  atomic_store(&wanted,1);
  if(!initialized) {
   if(ae_esp_radio_prepare()!=ESP_OK){atomic_store(&wanted,0);return (ae_reply){AE_IO_ERROR,"SDK radio NVS unavailable; not erased"};}
   if(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)<80*1024 || heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)<16*1024){atomic_store(&wanted,0);return (ae_reply){AE_LIMIT,"insufficient internal heap for BLE"};}
   if(nimble_port_init()!=ESP_OK){atomic_store(&wanted,0);failed=1;return (ae_reply){AE_IO_ERROR,"BLE init failed"};}
   ble_svc_gap_init();ble_svc_gatt_init();
   if(ble_svc_gap_device_name_set(device_name) || ble_gatts_count_cfg(services) || ble_gatts_add_svcs(services)) {
    atomic_store(&wanted,0);failed=1;nimble_port_deinit();return (ae_reply){AE_IO_ERROR,"BLE GATT init failed"};
   }
   ble_hs_cfg.sync_cb=synced;
   ble_npl_event_init(&control_event,apply_control,NULL);
   if(xTaskCreate(host_task,"dunlin_ble",4096,NULL,5,NULL)!=pdPASS) {
    atomic_store(&wanted,0);failed=1;nimble_port_deinit();return (ae_reply){AE_LIMIT,"BLE host task allocation failed"};
   }
   initialized=1;
   return (ae_reply){AE_OK,"BLE start requested; use bt status"};
  }
  if(atomic_load(&ready)!=1){atomic_store(&wanted,0);return (ae_reply){AE_BUSY,"BLE not synchronized"};}
  ble_npl_eventq_put(nimble_port_get_dflt_eventq(),&control_event);
  return (ae_reply){AE_OK,"BLE advertising requested; use bt status"};
 }
 if(r->op==AE_BLE_STOP) {
  atomic_store(&wanted,0);if(initialized)ble_npl_eventq_put(nimble_port_get_dflt_eventq(),&control_event);
  portENTER_CRITICAL(&rx_lock);received_size=0;memset(received,0,sizeof received);portEXIT_CRITICAL(&rx_lock);
  return (ae_reply){AE_OK,"BLE stop requested; host resources retained"};
 }
 if(r->op==AE_BLE_READ) {
  ae_reply out={AE_OK,""};unsigned char copy[64];size_t n;
  portENTER_CRITICAL(&rx_lock);n=received_size<sizeof copy?received_size:sizeof copy;memcpy(copy,received,n);memmove(received,received+n,received_size-n);received_size-=n;portEXIT_CRITICAL(&rx_lock);
  for(size_t i=0;i<n;i++)snprintf(out.text+2*i,sizeof out.text-2*i,"%02x",copy[i]);
  return out;
 }
 if(r->op==AE_BLE_SEND) {
  int c=atomic_load(&connection);if(!atomic_load(&wanted) || c==BLE_HS_CONN_HANDLE_NONE || !atomic_load(&subscribed))return (ae_reply){AE_INVALID,"BLE peer must connect and enable TX notifications"};
  if(r->length>(size_t)(ble_att_mtu(c)-3))return (ae_reply){AE_LIMIT,"payload exceeds negotiated ATT MTU minus 3"};
  struct os_mbuf *buffer=ble_hs_mbuf_from_flat(r->data,r->length);if(!buffer)return (ae_reply){AE_LIMIT,"BLE packet pool exhausted"};
  int e=ble_gatts_notify_custom(c,tx_handle,buffer);
  return e?(ae_reply){AE_IO_ERROR,"BLE notify failed; delivery unconfirmed"}:(ae_reply){AE_OK,"BLE notify queued; delivery unconfirmed"};
 }
 return (ae_reply){AE_UNSUPPORTED,"BLE operation unavailable"};
}
