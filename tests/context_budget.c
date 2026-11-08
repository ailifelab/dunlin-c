#include "dunlin/context_budget.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
int main(void){
 dc_context_range r;dc_context_capabilities c={.requested_output=512,.overhead_reserve=128,.client_limit=4194304};
 assert(dc_context_budget(&c,&r)==DC_CONTEXT_UNKNOWN);assert(r.maximum==0);
 c.model_limit=8192;c.deployment_limit=4096;assert(dc_context_budget(&c,&r)==DC_CONTEXT_VALID);assert(r.maximum==4096&&r.minimum==641&&r.selected==4096&&r.input_budget==3456);
 c.selected_limit=4097;assert(dc_context_budget(&c,&r)==DC_CONTEXT_INVALID);c.selected_limit=640;assert(dc_context_budget(&c,&r)==DC_CONTEXT_INVALID);
 c.selected_limit=641;assert(dc_context_budget(&c,&r)==DC_CONTEXT_VALID&&r.input_budget==1);
 c.output_limit=128;c.requested_output=512;c.selected_limit=1024;assert(dc_context_budget(&c,&r)==DC_CONTEXT_VALID&&r.output==128&&r.input_budget==768);
 c.model_limit=UINT64_MAX;c.deployment_limit=0;c.selected_limit=0;c.output_limit=0;c.requested_output=512;assert(dc_context_budget(&c,&r)==DC_CONTEXT_VALID&&r.maximum==4194304&&r.recommended==32768);
 c.overhead_reserve=UINT64_MAX;assert(dc_context_budget(&c,&r)==DC_CONTEXT_INVALID);c.overhead_reserve=UINT64_MAX-1;c.requested_output=2;assert(dc_context_budget(&c,&r)==DC_CONTEXT_INVALID);
 assert(dc_context_budget(NULL,&r)==DC_CONTEXT_INVALID);assert(dc_context_budget(&c,NULL)==DC_CONTEXT_INVALID);
 puts("PASS portable C budget: unknown capability, intersected limits, output/protocol reserves, exact bounds, client capacity and overflow");return 0;
}
