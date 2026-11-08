/* Trusted host bindings; transport policy is enforced outside the interpreter. */
#include "py/runtime.h"
#include <stddef.h>
#include <string.h>
#include <stdio.h>
extern int dunlin_mp_http(const char *,char *,size_t);
static char response[8192];
static mp_obj_t http_get(mp_obj_t url){
 if(!dunlin_mp_http(mp_obj_str_get_str(url),response,sizeof response))mp_raise_msg_varg(&mp_type_OSError,MP_ERROR_TEXT("%s"),response);
 return mp_obj_new_str(response,strlen(response));
}
static MP_DEFINE_CONST_FUN_OBJ_1(http_obj,http_get);
static mp_obj_t quote(mp_obj_t text){
 size_t n;const unsigned char *s=(const unsigned char *)mp_obj_str_get_data(text,&n);char out[769];size_t used=0;
 if(n>256)mp_raise_ValueError(MP_ERROR_TEXT("URL value too long"));
 for(size_t i=0;i<n;i++){
  unsigned c=s[i];if((c>='a'&&c<='z') || (c>='A'&&c<='Z') || (c>='0'&&c<='9') || c=='-' || c=='_' || c=='.' || c=='~')out[used++]=(char)c;
  else used+=(size_t)snprintf(out+used,sizeof out-used,"%%%02X",c);
 }return mp_obj_new_str(out,used);
}
static MP_DEFINE_CONST_FUN_OBJ_1(quote_obj,quote);
extern int dunlin_mp_led(const char *,unsigned,unsigned,unsigned,unsigned,char *,size_t);
extern void dunlin_mp_info(char *,size_t);
static mp_obj_t info(void){char text[2048];dunlin_mp_info(text,sizeof text);return mp_obj_new_str(text,strlen(text));}
static MP_DEFINE_CONST_FUN_OBJ_0(info_obj,info);
static mp_obj_t led(size_t count,const mp_obj_t *args){
 const char *mode=mp_obj_str_get_str(args[0]);
 if(strcmp(mode,"off") && strcmp(mode,"on") && strcmp(mode,"solid") && strcmp(mode,"blink") && strcmp(mode,"breathe") && strcmp(mode,"rainbow"))mp_raise_ValueError(MP_ERROR_TEXT("LED mode must be off,on,solid,blink,breathe,rainbow"));
 unsigned values[]={32,32,32,1000};
 for(size_t i=1;i<count;i++){mp_int_t n=mp_obj_get_int(args[i]);if(n<0 || (i<4?n>255:n<200 || n>5000))mp_raise_ValueError(MP_ERROR_TEXT("LED value out of range"));values[i-1]=(unsigned)n;}
 char text[256];if(!dunlin_mp_led(mode,values[0],values[1],values[2],values[3],text,sizeof text))mp_raise_msg(&mp_type_OSError,MP_ERROR_TEXT("LED unavailable or driver error"));
 return mp_obj_new_str(text,strlen(text));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(led_obj,1,5,led);
static const mp_rom_map_elem_t globals[]={
 {MP_ROM_QSTR(MP_QSTR___name__),MP_ROM_QSTR(MP_QSTR_dunlin)},
 {MP_ROM_QSTR(MP_QSTR_http_get),MP_ROM_PTR(&http_obj)},
 {MP_ROM_QSTR(MP_QSTR_quote),MP_ROM_PTR(&quote_obj)},
 {MP_ROM_QSTR(MP_QSTR_info),MP_ROM_PTR(&info_obj)},
 {MP_ROM_QSTR(MP_QSTR_led),MP_ROM_PTR(&led_obj)},
};
static MP_DEFINE_CONST_DICT(module_globals,globals);
const mp_obj_module_t dunlin_module={.base={&mp_type_module},.globals=(mp_obj_dict_t *)&module_globals};
MP_REGISTER_MODULE(MP_QSTR_dunlin,dunlin_module);
