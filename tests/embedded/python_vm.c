#include "py/runtime.h"
#include "py/compile.h"
#include "py/gc.h"
#include "py/stackctrl.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <assert.h>
static char heap[512*1024],result[2048];static size_t used;static clock_t deadline;
void dunlin_mp_write(const char *s,size_t n){size_t room=sizeof result-1-used;if(n>room)n=room;memcpy(result+used,s,n);used+=n;result[used]=0;}
void dunlin_mp_poll(void){if(clock()>deadline)mp_raise_msg(&mp_type_RuntimeError,MP_ERROR_TEXT("Python execution timeout"));}
int dunlin_mp_http(const char *url,char *out,size_t cap){
 const char *data=strstr(url,"geocoding-api")?"{\"results\":[{\"name\":\"Shanghai\",\"country\":\"CN\",\"latitude\":31.22,\"longitude\":121.45}]}":strstr(url,"api.open-meteo.com")?"{\"timezone\":\"Asia/Shanghai\",\"current\":{\"time\":\"2026-11-06T12:00\",\"temperature_2m\":18.5},\"current_units\":{\"temperature_2m\":\"C\"}}":NULL;
 if(!data){snprintf(out,cap,"fixture host denied");return 0;}snprintf(out,cap,"%s",data);return 1;
}
int dunlin_mp_led(const char *mode,unsigned r,unsigned g,unsigned b,unsigned ms,char *out,size_t cap){(void)mode;(void)r;(void)g;(void)b;(void)ms;snprintf(out,cap,"fixture applied");return 1;}
void dunlin_mp_info(char *out,size_t cap){snprintf(out,cap,"{\"active_model\":\"fixture\"}");}
static int run(const char *source){
 int top;mp_stack_set_top(&top);mp_stack_set_limit(128*1024);gc_init(heap,heap+sizeof heap);mp_init();used=0;result[0]=0;deadline=clock()+CLOCKS_PER_SEC/5;
 int ok=0;nlr_buf_t nlr;if(nlr_push(&nlr)==0){mp_lexer_t *lex=mp_lexer_new_from_str_len(MP_QSTR__lt_stdin_gt_,source,strlen(source),0);qstr name=lex->source_name;mp_parse_tree_t tree=mp_parse(lex,MP_PARSE_FILE_INPUT);mp_call_function_0(mp_compile(&tree,name,true));ok=1;nlr_pop();}else mp_obj_print_exception(&mp_plat_print,MP_OBJ_FROM_PTR(nlr.ret_val));mp_deinit();return ok;
}
int main(void){
 assert(run("print(f'value={1+2}')"));assert(strstr(result,"value=3"));
 assert(run("import json, dunlin\nprint(json.loads(dunlin.info())['active_model'])\nprint(dunlin.quote('上海'))"));assert(strstr(result,"fixture") && strstr(result,"%E4%B8%8A%E6%B5%B7"));
 FILE *f=fopen("embedded/python/weather.py","rb");assert(f);char code[5000];size_t n=fread(code,1,4000,f);fclose(f);strcpy(code+n,"\nprint(weather('上海'))\n");int weather_ok=run(code);if(!weather_ok)fprintf(stderr,"weather fixture error: %s\n",result);assert(weather_ok);assert(strstr(result,"18.5") && strstr(result,"MicroPython"));
 assert(!run("while True: pass"));assert(strstr(result,"timeout"));
 assert(!run("raise ValueError('expected')"));assert(strstr(result,"expected"));
 assert(!run("x='x'*2000000"));assert(strstr(result,"MemoryError"));
 assert(!run("import os"));assert(strstr(result,"ImportError"));
 assert(!run("import dunlin\ndunlin.led('solid',256,0,0)"));
 assert(run("a=['keep']*40000\nimport gc\ngc.collect()\nprint(a[0],len(a))"));assert(strstr(result,"40000"));
 assert(run("print('recovered')"));assert(strstr(result,"recovered"));
 puts("PASS actual MicroPython embed: arithmetic, JSON, Unicode quote, Python weather with transport fixture, timeout, exception, OOM, blocked imports, LED bounds, GC roots and recovery");
}
