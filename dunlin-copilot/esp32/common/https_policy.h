#ifndef AE_HTTPS_POLICY_H
#define AE_HTTPS_POLICY_H
#include <stddef.h>
/* Parses a bounded HTTPS authority and applies only the selected host policy.
 * Success still requires the transport's CA/hostname verification. No allocation. */
int ae_https_permitted(const char *,int,const char *const *,size_t,char *,size_t);
#endif
