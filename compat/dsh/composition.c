#include "environment.h"
#include "dunlin/extension_services.h"
/* Optional desktop composition root; remove this unit and the build switch to
 * omit DSH without changing Conversation/Identity/Index/Skills/Grant services. */
void dc_desktop_install_dsh(dc_store *store){
 static const dc_environment_provider_v1 provider={1,"node-dsh","0.2.1-rc.3",
  "DSH 0.2.1-rc.3 · Cordis 4.0.5 · Node 26.13.0",dc_dsh_prepare};
 if(dc_extensions_register_environment(store,&provider))dc_extensions_register_detector(store,dc_dsh_detect);
}
