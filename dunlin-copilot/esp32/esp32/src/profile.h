#ifndef AE_ESP_PROFILE_H
#define AE_ESP_PROFILE_H
#include "esp_err.h"
typedef struct {char ssid[33],password[65];} ae_saved_wifi;
typedef struct {char url[128],model[96],key[256],effort[8];} ae_saved_model;
/* 1 found/validated, 0 absent, -1 storage error/invalid record. */
int ae_profile_load_wifi(ae_saved_wifi *);
int ae_profile_load_model(ae_saved_model *);
esp_err_t ae_profile_save_wifi(const char *,const char *);
esp_err_t ae_profile_save_model(const char *,const char *,const char *);
esp_err_t ae_profile_save_model_options(const char *,const char *,const char *,const char *);
esp_err_t ae_profile_forget(const char *);
int ae_profile_restoring_wifi(void);
void ae_esp_agent_restore(void);
void ae_esp_profile_start_wifi(void *);
#endif
