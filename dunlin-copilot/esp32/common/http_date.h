#ifndef AE_HTTP_DATE_H
#define AE_HTTP_DATE_H
#include <stdint.h>
#include <stddef.h>
int ae_http_date_epoch(const char *,int64_t *);
int ae_date_shanghai(int64_t,char *,size_t);
#endif
