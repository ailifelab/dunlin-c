#ifndef DUNLIN_LIVE_MUTEX_INTERNAL_H
#define DUNLIN_LIVE_MUTEX_INTERNAL_H
/* Private portable lock for the independent process-local live preview. */
#ifdef _WIN32
#include <windows.h>
typedef CRITICAL_SECTION extension_mutex;
static int extension_mutex_init(extension_mutex *m){return InitializeCriticalSectionEx(m,0,0)!=0;}
static void extension_mutex_lock(extension_mutex *m){EnterCriticalSection(m);}
static void extension_mutex_unlock(extension_mutex *m){LeaveCriticalSection(m);}
static void extension_mutex_destroy(extension_mutex *m){DeleteCriticalSection(m);}
#else
#include <pthread.h>
typedef pthread_mutex_t extension_mutex;
static int extension_mutex_init(extension_mutex *m){pthread_mutexattr_t attr;if(pthread_mutexattr_init(&attr))return 0;int rc=pthread_mutexattr_settype(&attr,PTHREAD_MUTEX_RECURSIVE);if(!rc)rc=pthread_mutex_init(m,&attr);pthread_mutexattr_destroy(&attr);return rc==0;}
static void extension_mutex_lock(extension_mutex *m){pthread_mutex_lock(m);}
static void extension_mutex_unlock(extension_mutex *m){pthread_mutex_unlock(m);}
static void extension_mutex_destroy(extension_mutex *m){pthread_mutex_destroy(m);}
#endif
#endif
