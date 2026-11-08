#include "py/runtime.h"
#include "py/gc.h"
#include "shared/runtime/gchelper.h"
#include <stdlib.h>
extern void dunlin_mp_write(const char *,size_t);
void mp_hal_stdout_tx_strn_cooked(const char *s,size_t n){dunlin_mp_write(s,n);}
void gc_collect(void){gc_collect_start();gc_helper_collect_regs_and_stack();gc_collect_end();}
void nlr_jump_fail(void *value){(void)value;abort();}
/* Embedded profile has memory streams, never a filesystem-backed open(). */
static mp_obj_t blocked_open(size_t n,const mp_obj_t *args,mp_map_t *kw){
 (void)n;(void)args;(void)kw;mp_raise_msg(&mp_type_OSError,MP_ERROR_TEXT("filesystem is unavailable in this embedded profile"));
}
MP_DEFINE_CONST_FUN_OBJ_KW(mp_builtin_open_obj,0,blocked_open);
