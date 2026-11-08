#ifndef DUNLIN_PYTHON_HOST_H
#define DUNLIN_PYTHON_HOST_H
#include <stddef.h>
typedef struct {
 void (*log)(const char *);
 void (*text)(const char *);
 void (*info)(char *,size_t);
 int (*led)(const char *,unsigned,unsigned,unsigned,unsigned,char *,size_t);
 int (*ip_ready)(void);
 int (*sync_clock)(unsigned);
} ap_python_host;
void ae_python_start(const ap_python_host *);
void ae_python_status(char *,size_t);
int ae_python_exec(const char *,char *,size_t);
int ae_python_active(void);
int ae_python_available(void);
const char *ae_python_network_policy(void);
int ae_python_network(const char *,char *,size_t);
int ae_python_allow(const char *,char *,size_t);
void ae_python_hosts(char *,size_t);
int ae_python_queue(const char *,char *,size_t);
#endif
