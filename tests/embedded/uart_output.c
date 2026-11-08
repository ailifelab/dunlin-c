/* Reuse the deterministic RTOS harness; exercise production private output()
 * with a recording UART, not a copy of the chunking implementation. */
#include <stddef.h>
int ae_rtos_regression_main(int,char **);
#define main ae_rtos_regression_main
#define uart_write_bytes ignored_uart_write_bytes
#define uart_wait_tx_done ignored_uart_wait_tx_done
#define vTaskDelay ignored_vTaskDelay
#define xSemaphoreGiveRecursive ignored_give_recursive
#include "rtos.c"
#undef main
#undef uart_write_bytes
#undef uart_wait_tx_done
#undef vTaskDelay
#undef xSemaphoreGiveRecursive
#include <locale.h>
#include <wchar.h>
static char received[4096];static size_t received_size;
static int observe_lines,inject;static unsigned line_unlocks;
static void output(void *,const char *,size_t);
static unsigned writes,waits,delays;static int short_write,wait_error;
int uart_write_bytes(int,const void *,size_t);
void vTaskDelay(TickType_t);
int uart_wait_tx_done(int,TickType_t);
int uart_write_bytes(int port,const void *data,size_t size){
 assert(port==UART_NUM_0 && size && size<=16);writes++;
 const char *s=data;const char *lf=memchr(s,'\n',size);assert(!lf || lf==s+size-1);
 mbstate_t state={0};size_t at=0;
 while(at<size){size_t n=mbrlen(s+at,size-at,&state);assert(n!=(size_t)-1 && n!=(size_t)-2 && n);at+=n;}
 assert(received_size+size<sizeof received);memcpy(received+received_size,data,size);received_size+=size;
 return short_write?(int)size-1:(int)size;
}
int uart_wait_tx_done(int port,TickType_t ticks){assert(port==UART_NUM_0 && ticks==pdMS_TO_TICKS(100));waits++;return wait_error;}
void vTaskDelay(TickType_t ticks){assert(ticks==pdMS_TO_TICKS(20));delays++;}
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t sem){
 BaseType_t result=ignored_give_recursive(sem);
 if(observe_lines && !sem->depth){
  assert(received_size && received[received_size-1]=='\n');line_unlocks++;
  if(inject){inject=0;output(NULL,"LOCAL\r\n",7);}
 }
 return result;
}
#define AE_CONSOLE_TEXT_TEST 1
#include "../../dunlin-copilot/esp32/esp32/src/executor.c"
int main(void){
 assert(setlocale(LC_CTYPE,"C.UTF-8") || setlocale(LC_CTYPE,"en_US.UTF-8"));output_lock=xSemaphoreCreateRecursiveMutexStatic(&output_control);
 const char *sample="AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA中文🙂\r\n| [模型] 正在请求模型服务。\r\n[任务结束] status=replied; model=fixture; heap_free=146016; 串口仍可接收命令。\r\n";
 output(NULL,sample,strlen(sample));assert(received_size==strlen(sample) && !memcmp(sample,received,received_size));assert(writes==waits && waits==delays && writes>3);
 writes=waits=delays=0;short_write=1;output(NULL,sample,strlen(sample));assert(writes==1 && !waits && !delays);
 writes=waits=delays=0;short_write=0;wait_error=1;output(NULL,sample,strlen(sample));assert(writes==1 && waits==1 && !delays);
 wait_error=0;received_size=0;observe_lines=1;inject=1;line_unlocks=0;
 ae_esp_console_model_text("中文第一行\n中文第二行");
 const char *interleaved="| 中文第一行\r\nLOCAL\r\n| 中文第二行\r\n";
 assert(received_size==strlen(interleaved) && !memcmp(received,interleaved,received_size) && line_unlocks==3);
 received_size=0;inject=1;line_unlocks=0;
 char long_line[320];memset(long_line,'x',300);strcpy(long_line+300,"\n末行");
 ae_esp_console_model_text(long_line);
 assert(line_unlocks==3 && !memcmp(received,"| ",2));
 for(size_t i=2;i<302;i++)assert(received[i]=='x');
 assert(!memcmp(received+302,"\r\nLOCAL\r\n| 末行\r\n",strlen("\r\nLOCAL\r\n| 末行\r\n")));
 observe_lines=0;
 assert(!output_control.depth);puts("PASS actual UART output preserves UTF8/LF/exact bytes, <=16-byte chunks, bounded drain and pacing; stops on short write/drain error; local frames interleave only between complete model lines including multi-flush lines");
}
