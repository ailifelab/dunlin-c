/* Repeat existing behavioral fixtures in one process, not subprocess RSS sampling. */
#include <stdio.h>
#ifdef REPEAT_KERNEL
int dunlin_fixture_main(int,char **);
#else
int dunlin_fixture_main(void);
#endif
int main(void){
 for(unsigned i=0;i<100;i++){
#ifdef REPEAT_KERNEL
 char *args[]={"kernel-repeat",NULL};int rc=dunlin_fixture_main(1,args);
#else
 int rc=dunlin_fixture_main();
#endif
 if(rc)return rc;
 }
 puts("PASS 100 same-process complete fixture cycles; existing tracked-allocation and lease teardown assertions retained");return 0;
}
