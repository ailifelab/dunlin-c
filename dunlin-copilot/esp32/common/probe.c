#include <dunlin/compact/embedded.h>
static ak_value leaf(void *user,const void *payload,ak_next *next) {
 (void)user;(void)payload;(void)next;return (ak_value){.tag=AK_INT,.as.integer=7};
}
static ak_value wrap(void *user,const void *payload,ak_next *next) {
 (void)user;(void)payload;
 if(!next)return (ak_value){.tag=AK_UNDEFINED};
 ak_value value=ak_next_call(next);
 if(value.tag==AK_INT)value.as.integer++;
 return value;
}
int ae_kernel_probe(ak_context *ctx) {
 if(!ctx)return 0;
 ak_handle handle=ak_on(ak_root(ctx),"embedded/probe",wrap,NULL,0);if(!handle)return 0;
 ak_result r=ak_dispatch(ctx,AK_WATERFALL,"embedded/probe",NULL,leaf,NULL);
 int ok=!r.errors && r.value.tag==AK_INT && r.value.as.integer==8;
 ok=ak_revoke(ctx,handle) && ok;
 return ok;
}
