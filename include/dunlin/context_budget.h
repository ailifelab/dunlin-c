#ifndef DUNLIN_CONTEXT_BUDGET_H
#define DUNLIN_CONTEXT_BUDGET_H
#include <stdint.h>
typedef struct {
 uint64_t model_limit, deployment_limit, selected_limit, client_limit;
 uint64_t output_limit, requested_output, overhead_reserve;
} dc_context_capabilities;
typedef struct {uint64_t minimum,maximum,recommended,selected,output,input_budget;} dc_context_range;
typedef enum {DC_CONTEXT_VALID=0,DC_CONTEXT_UNKNOWN=1,DC_CONTEXT_INVALID=2} dc_context_status;
/* Allocation-free C foundation, no database/model/desktop dependency.
 * Zero capability means unknown; never manufactures a guaranteed upper bound.
 * Units are tokens only when supplied by a compatible tokenizer/provider. */
dc_context_status dc_context_budget(const dc_context_capabilities *,dc_context_range *);
#endif
