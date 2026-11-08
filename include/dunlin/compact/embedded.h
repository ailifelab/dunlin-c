#ifndef DUNLIN_EMBEDDED_H
#define DUNLIN_EMBEDDED_H
/* Small-device host API. No JSON, POSIX, TLS, heap or desktop runtime headers. */
#include "kernel.h"
#ifdef __cplusplus
extern "C" {
#endif
#define AE_ABI 1u
#define AE_LINE_MAX 384u
#define AE_DATA_MAX 96u
#define AE_REPLY_MAX 192u
#define AE_POOL_SLOTS 24u
#define AE_SLOT_BYTES 256u
#define AE_IO_SERVICE "embedded/io"
typedef enum { AE_OK, AE_INVALID, AE_DENIED, AE_UNSUPPORTED, AE_BUSY, AE_IO_ERROR, AE_LIMIT } ae_status;
typedef enum { AE_GPIO_READ, AE_GPIO_WRITE, AE_PWM, AE_PWM_STOP, AE_UART_OPEN,
 AE_UART_SEND, AE_UART_READ, AE_UART_CLOSE, AE_WIFI_SET, AE_WIFI_CONNECT,
 AE_WIFI_DISCONNECT, AE_WIFI_STATUS, AE_WIFI_FORGET, AE_BLE_NAME, AE_BLE_START,
 AE_BLE_STOP, AE_BLE_STATUS, AE_BLE_SEND, AE_BLE_READ, AE_NET_SEND, AE_DIAGNOSTICS } ae_operation;
typedef struct {
 ae_operation op;
 unsigned pin, value, frequency, tx, rx, baud, port;
 unsigned char data[AE_DATA_MAX]; size_t length;
 char name[33], secret[65], address[16];
} ae_request;
typedef struct { ae_status status; char text[AE_REPLY_MAX]; } ae_reply;
/* call() is serialized. Borrowed request is valid only for call. HAL must not log
 * secret or retain its pointer. Replies never contain credentials. */
typedef struct { unsigned abi; void *user; ae_reply (*call)(void *,const ae_request *); } ae_io_v1;
typedef struct { const ae_io_v1 *hal; uint64_t allowed_pins, valid_pins; ae_io_v1 service; } ae_io_plugin;
extern const ak_plugin ae_io_descriptor;
/* Clears pin grants on unload. No promise to reverse electrical/network effects. */
int ae_io_allow(ae_io_plugin *, unsigned pin, int allow);

typedef union { max_align_t alignment; unsigned char bytes[AE_POOL_SLOTS*AE_SLOT_BYTES]; } ae_storage;
typedef struct { ae_storage arena; unsigned char used[AE_POOL_SLOTS]; size_t live, peak, rejected; } ae_pool;
void *ae_pool_alloc(void *,size_t);
void ae_pool_free(void *,void *);
/* Effect-free startup probe of the actual shared kernel, releases all handles. */
int ae_kernel_probe(ak_context *);

typedef void (*ae_write_fn)(void *,const char *,size_t);
/* Optional shell host adapter: enqueue instead of executing on the RX task. */
typedef ae_reply (*ae_submit_fn)(void *,const char *,size_t);
typedef struct {
 ak_scope *scope; ae_io_plugin *io; ae_write_fn write; void *user;
 ae_submit_fn submit; void *submit_user;
 char line[AE_LINE_MAX]; size_t length; unsigned dropping, swallow_lf, escape;
} ae_serial;
/* External host can execute one line without serial framing. No shell expansion.
 * Exact line length excludes NUL. Whitespace, double quotes and \\ escapes only. */
ae_reply ae_execute(ak_scope *,ae_io_plugin *,const char *,size_t);
void ae_serial_init(ae_serial *,ak_scope *,ae_io_plugin *,ae_write_fn,void *);
void ae_serial_set_submit(ae_serial *,ae_submit_fn,void *);
void ae_serial_feed(ae_serial *,const unsigned char *,size_t);
const char *ae_status_name(ae_status);
#ifdef __cplusplus
}
#endif
#endif
