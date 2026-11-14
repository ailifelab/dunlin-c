#include "dunlin/context_budget.h"
#include <stddef.h>
static uint64_t min(uint64_t a,uint64_t b){return a<b?a:b;}
dc_context_status dc_context_budget(const dc_context_capabilities *c,dc_context_range *r){
 if(!c||!r)return DC_CONTEXT_INVALID;*r=(dc_context_range){0};
 if(!c->requested_output||c->overhead_reserve==UINT64_MAX||c->requested_output>UINT64_MAX-c->overhead_reserve-1)return DC_CONTEXT_INVALID;
 r->output=c->output_limit?min(c->requested_output,c->output_limit):c->requested_output;
 if(c->model_limit&&c->deployment_limit)r->maximum=min(c->model_limit,c->deployment_limit);
 else r->maximum=c->model_limit?c->model_limit:c->deployment_limit;
 if(!r->maximum)return DC_CONTEXT_UNKNOWN;
 if(c->client_limit)r->maximum=min(r->maximum,c->client_limit);
 r->minimum=r->output+c->overhead_reserve+1;
 if(r->minimum>r->maximum)return DC_CONTEXT_INVALID;
 r->recommended=min(r->maximum,32768);if(r->recommended<r->minimum)r->recommended=r->minimum;
 r->selected=c->selected_limit?c->selected_limit:r->recommended;
 if(r->selected<r->minimum||r->selected>r->maximum)return DC_CONTEXT_INVALID;
 r->input_budget=r->selected-r->output-c->overhead_reserve;return DC_CONTEXT_VALID;
}

dc_context_status dc_context_editor_budget(const dc_context_capabilities *caps,
    int has_choice, int64_t choice, dc_context_range *range) {
    if (!caps || !range) return DC_CONTEXT_INVALID;
    dc_context_capabilities draft = *caps;
    draft.selected_limit = 0;
    dc_context_status status = dc_context_budget(&draft, range);
    if (status != DC_CONTEXT_VALID || !has_choice) return status;
    uint64_t selected = choice < 0 ? 0 : (uint64_t)choice;
    if (selected < range->minimum) selected = range->minimum;
    if (selected > range->maximum) selected = range->maximum;
    draft.selected_limit = selected;
    return dc_context_budget(&draft, range);
}
