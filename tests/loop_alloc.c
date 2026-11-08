/* Fault the real Loop JSON constructors/attachments, not the fixture services. */
#define main dunlin_existing_contracts_main
#include "agent_contracts.c"
#undef main
static size_t attempt,fail_at;static int triggered;
static int fail(void){if(++attempt==fail_at){triggered=1;return 1;}return 0;}
J *dunlin_loop_test_object(void){return fail()?NULL:json_object_new_object();}
J *dunlin_loop_test_array(void){return fail()?NULL:json_object_new_array();}
J *dunlin_loop_test_string(const char *s){return fail()?NULL:json_object_new_string(s);}
J *dunlin_loop_test_int(int value){return fail()?NULL:json_object_new_int(value);}
J *dunlin_loop_test_bool(json_bool value){return fail()?NULL:json_object_new_boolean(value);}
int dunlin_loop_test_add(J *o,const char *k,J *v){return fail()?-1:json_object_object_add(o,k,v);}
int dunlin_loop_test_push(J *o,J *v){return fail()?-1:json_object_array_add(o,v);}
static ah_status fail_first_execution(void *user,const char *name,J *args,const ah_hooks *hooks,size_t cap,unsigned timeout,char **out){
 ah_status status=call_tool(user,name,args,hooks,cap,timeout,out);
 if(((fixture *)user)->calls==1){free(*out);*out=strdup("fixture execution error");CHECK(*out);return AH_EXEC_FAILED;}
 return status;
}
static size_t once(size_t failure,int write){
 fixture f;begin_fixture(&f,"Loop checked JSON allocation and ownership");
 const step hello[]={{.user="hello",.users=1,.response=FINAL}};
 const step repair_steps[]={
  {.user="store seven",.users=1,.response=PUT},
  {.user="store seven",.users=1,.pair="put-1",.result="fixture execution error",.response="{\"role\":\"assistant\",\"tool_calls\":[{\"id\":\"put-2\",\"type\":\"function\",\"function\":{\"name\":\"put\",\"arguments\":\"{\\\"value\\\":7}\"}}]}"},
  {.user="store seven",.users=1,.pair="put-2",.result="7",.response=FINAL}
 };
 if(write==2){f.tools.call=fail_first_execution;script(&f,repair_steps,3);}
 else script(&f,write?write_steps:hello,write?2:1);
 f.current_session="fault";
 attempt=triggered=0;fail_at=failure;
 ah_result result=dunlin_turn(ak_root(f.kernel),&f.limits,&f.observer,"fault",write?"store seven":"hello");
 size_t total=attempt;fail_at=0;
 if(triggered){CHECK(result.status==AH_FAILED || result.status==AH_UNKNOWN);CHECK(f.writes<=(unsigned)(write==2?2:1));}
 else CHECK(result.status==AH_DONE);
 free(result.text);
 CHECK(!f.active_leases && f.opens==f.closes);
 /* If an effect's result could not be checkpointed, its durable pending ID
  * must still prevent automatic replay. */
 void *l=NULL;J *state=NULL;CHECK(f.disk.open(f.disk.ctx,"fault",&l,&state)==AH_DONE);int pending=string(state,"pending")!=NULL;json_object_put(state);f.disk.close(l);
 if(pending){unsigned calls=f.calls,requests=f.requests;result=dunlin_turn(ak_root(f.kernel),&f.limits,&f.observer,"fault",NULL);CHECK(result.status==AH_UNKNOWN && calls==f.calls && requests==f.requests);free(result.text);}
 script(&f,hello,1);turn(&f,"recovery","hello",AH_DONE,"observed 7");end_fixture(&f);return total;
}
int main(void){
 size_t hello=once(0,0),write=once(0,1),repair=once(0,2);
 for(size_t i=1;i<=hello;i++)once(i,0);
 for(size_t i=1;i<=write;i++)once(i,1);
 for(size_t i=1;i<=repair;i++)once(i,2);
 printf("PASS Loop JSON fault injection: %zu greeting + %zu effect + %zu execution-repair positions; no crash, closed leases, tracked kernel frees, recovery and durable unknown non-replay\n",hello,write,repair);return 0;
}
