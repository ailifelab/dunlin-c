#define _POSIX_C_SOURCE 200809L
/* DUNLIN-AGENT-CONTRACTS: real kernel/Loop composition, scripted boundaries.
 * No provider, subprocess, network, credentials, or user workspace is involved. */
#include <dunlin/compact/runtime.h>
#include <dunlin/compact/http.h>
#include <dunlin/compact/session.h>
#include <json-c/json.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct json_object J;
static const char *case_name;
#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr,"FAIL %s %s:%d: %s\n",case_name,__FILE__,__LINE__,#expr); \
    abort(); \
} } while (0)

typedef struct {
    const char *user, *pair, *result, *response, *prior, *absent;
    unsigned users, advance_ms;
    int cancel;
} step;
typedef struct { char type[48], data[160]; size_t size; } record;
typedef struct { const char *key; const void *table; } binding;
typedef struct fixture fixture;
typedef struct { fixture *owner; void *inner; } lease;
struct fixture {
    char directory[64], sessions_path[80];
    ak_context *kernel;
    ak_scope *model_scope, *loop_scope;
    size_t allocations;
    ah_session_service disk;
    dunlin_sessions_v1 sessions;
    ah_tool_service tools;
    dunlin_http_v1 model_context;
    dunlin_model_v1 model;
    dunlin_permission_v1 permission;
    dunlin_clock_v1 clock;
    dunlin_loop_v1 loop;
    binding bindings[6];
    ah_hooks observer;
    ah_limits limits;
    const step *script;
    size_t script_count, position;
    unsigned requests, calls, writes, reads, approvals, opens, closes, active_leases;
    unsigned observer_approvals, observer_clocks;
    int value, allow, cancel, cancel_on_approval, timeout_on_approval, tool_unknown;
    uint64_t milliseconds;
    const char *fail_phase;
    int fault_used, commit_before_failure;
    const char *current_session;
    record events[64], kernel_events[64], audit[192];
    size_t event_count, kernel_event_count, audit_count;
};

static J *field(J *object,const char *key) {
    J *value=NULL;
    if(json_object_is_type(object,json_type_object)) json_object_object_get_ex(object,key,&value);
    return value;
}
static const char *string(J *object,const char *key) {
    J *value=field(object,key);
    return json_object_is_type(value,json_type_string)?json_object_get_string(value):NULL;
}
static int equal(const char *a,const char *b) { return a && b && !strcmp(a,b); }
static void record_at(record *records,size_t *count,size_t capacity,
                      const char *type,const char *data,size_t size) {
    CHECK(*count<capacity && strlen(type)<sizeof(records[0].type));
    CHECK(data && size<sizeof(records[0].data) && strlen(data)==size);
    record *r=&records[(*count)++];
    strcpy(r->type,type); memcpy(r->data,data,size); r->data[size]=0; r->size=size;
}
static void audit(fixture *f,const char *type,const char *data) {
    record_at(f->audit,&f->audit_count,192,type,data,strlen(data));
}
static size_t locate(const fixture *f,const char *type,const char *data) {
    for(size_t i=0;i<f->audit_count;i++)
        if(equal(f->audit[i].type,type) && equal(f->audit[i].data,data)) return i;
    return SIZE_MAX;
}
static unsigned occurrences(const fixture *f,const char *type) {
    unsigned total=0;
    for(size_t i=0;i<f->event_count;i++) total+=equal(f->events[i].type,type);
    return total;
}
static void *allocate(void *user,size_t size) {
    fixture *f=user;
    void *p=malloc(size);
    if(p) f->allocations++;
    return p;
}
static void release(void *user,void *p) {
    fixture *f=user;
    CHECK(p && f->allocations);
    f->allocations--; free(p);
}
static uint64_t now(void *user) { return ((fixture *)user)->milliseconds; }
static int cancelled(void *user) { return ((fixture *)user)->cancel; }
static uint64_t unused_observer_clock(void *user) {
    ((fixture *)user)->observer_clocks++;
    return UINT64_MAX;
}
static int unused_observer_approval(void *user,const char *name,const char *args) {
    (void)name; (void)args;
    fixture *f=user; f->observer_approvals++;
    return !f->allow;
}
static void observe(void *user,const char *type,const char *data,size_t size) {
    fixture *f=user;
    record_at(f->events,&f->event_count,64,type,data,size);
    audit(f,type,data);
}
static ak_value kernel_observe(void *user,const void *payload,ak_next *next) {
    fixture *f=user;
    const dunlin_event_v1 *event=payload;
    CHECK(!next && equal(event->session_id,f->current_session));
    record_at(f->kernel_events,&f->kernel_event_count,64,event->type,event->data,event->length);
    return (ak_value){0};
}
static int approve(void *user,const char *name,const char *arguments) {
    fixture *f=user;
    CHECK(equal(name,"put") && equal(arguments,"{\"value\":7}"));
    f->approvals++; audit(f,"approve",arguments);
    if(f->cancel_on_approval) f->cancel=1;
    if(f->timeout_on_approval) f->milliseconds+=f->limits.task_timeout_ms;
    return f->allow;
}

static J *schema(void *user) {
    (void)user;
    J *result=json_tokener_parse("["
        "{\"type\":\"function\",\"function\":{\"name\":\"put\",\"parameters\":{\"type\":\"object\",\"properties\":{\"value\":{\"type\":\"integer\"}},\"required\":[\"value\"]}}},"
        "{\"type\":\"function\",\"function\":{\"name\":\"get\",\"parameters\":{\"type\":\"object\",\"properties\":{}}}}]");
    CHECK(result); return result;
}
static ah_status check_tool(void *user,const char *name,J *args,int *approval,int *effect,
                            char *error,size_t size) {
    (void)user;
    *approval=*effect=0;
    if(equal(name,"get") && json_object_object_length(args)==0) return AH_DONE;
    if(equal(name,"put") && json_object_object_length(args)==1 &&
       json_object_is_type(field(args,"value"),json_type_int)) {
        *approval=*effect=1;
        return AH_DONE;
    }
    snprintf(error,size,"unknown fixture tool or invalid arguments");
    return AH_FAILED;
}
static ah_status call_tool(void *user,const char *name,J *args,const ah_hooks *hooks,
                           size_t max_output,unsigned timeout,char **output) {
    fixture *f=user;
    CHECK(hooks && hooks->now_ms && timeout>0 && timeout<=f->limits.tool_timeout_ms);
    CHECK(hooks->now_ms(hooks->user)==f->milliseconds && max_output>=16);
    f->calls++;
    if(equal(name,"put")) {
        CHECK(f->allow && f->approvals>0 && json_object_get_int(field(args,"value"))==7);
        f->writes++; f->value=json_object_get_int(field(args,"value"));
        audit(f,"effect","put");
    } else { CHECK(equal(name,"get")); f->reads++; audit(f,"read","get"); }
    char text[32]; snprintf(text,sizeof(text),"%d",f->value);
    *output=strdup(text); CHECK(*output);
    return f->tool_unknown?AH_UNKNOWN:AH_DONE;
}

/* A genuine disk checkpoint is the only source for each new lease. The wrapper
 * never retains the Loop's mutable JSON pointer as a pretend committed snapshot. */
static ah_status open_session(void *user,const char *id,void **out,J **state) {
    fixture *f=user;
    lease *l=calloc(1,sizeof(*l)); CHECK(l);
    ah_status status=f->disk.open(f->disk.ctx,id,&l->inner,state);
    if(status!=AH_DONE) {free(l);return status;}
    l->owner=f; *out=l; f->opens++; f->active_leases++;
    return AH_DONE;
}
static int save_session(void *user,J *state) {
    lease *l=user; fixture *f=l->owner;
    J *messages=field(state,"messages");
    size_t count=json_object_array_length(messages);
    J *last=count?json_object_array_get_idx(messages,count-1):NULL;
    const char *phase="proposal",*id="";
    if(string(state,"pending")) {phase="pending"; id=string(state,"pending");}
    else if(!json_object_get_boolean(field(state,"active")) && field(state,"status")) phase="terminal";
    else if(equal(string(last,"role"),"tool")) {phase="result";id=string(last,"tool_call_id");}
    else if(equal(string(last,"role"),"user"))
        phase=json_object_get_int(field(state,"rounds"))?"model-round":"input";
    int fail=!f->fault_used && equal(f->fail_phase,phase);
    if(fail && !f->commit_before_failure) {
        f->fault_used=1; audit(f,"save.failed",phase); return -1;
    }
    int status=f->disk.save(l->inner,state);
    CHECK(status==0);
    char label[48]; snprintf(label,sizeof(label),"save.%s",phase); audit(f,label,id);
    if(fail) {
        f->fault_used=1; audit(f,"save.committed-failure",phase); return -1;
    }
    return status;
}
static void close_session(void *user) {
    lease *l=user; fixture *f=l->owner;
    CHECK(f->active_leases); f->active_leases--; f->closes++;
    f->disk.close(l->inner); free(l);
}
static dunlin_model_status model(const dunlin_model_config *config,J *messages,J *tools,
    const dunlin_model_events *events,J **answer,char *error,size_t size) {
    (void)error; (void)size;
    fixture *f=config->http->user;
    CHECK(f->position<f->script_count);
    const step *s=&f->script[f->position++]; f->requests++;
    CHECK(json_object_array_length(tools)==2 && config->timeout_ms>0);
    unsigned users=0; const char *latest=NULL; int prior=!s->prior,pair=!s->pair;
    for(size_t i=0;i<json_object_array_length(messages);i++) {
        J *message=json_object_array_get_idx(messages,i);
        if(equal(string(message,"role"),"user")) {
            latest=string(message,"content"); users++;
            if(equal(latest,s->prior)) prior=1;
            CHECK(!s->absent || !equal(latest,s->absent));
        }
        if(equal(string(message,"tool_call_id"),s->pair)) {
            CHECK(equal(string(message,"role"),"tool") && equal(string(message,"content"),s->result));
            pair=1;
        }
    }
    CHECK(equal(latest,s->user) && users==s->users && prior && pair);
    audit(f,"model",s->user);
    f->milliseconds+=s->advance_ms;
    if(s->cancel) f->cancel=1;
    if(s->advance_ms>=f->limits.task_timeout_ms || s->cancel)
        CHECK(events && events->cancelled && events->cancelled(events->user));
    *answer=json_tokener_parse(s->response); CHECK(*answer);
    return DUNLIN_MODEL_OK;
}

static int mount(ak_scope *scope,void *data) {
    binding *b=data; return !ak_provide(scope,b->key,1,b->table);
}
#define SERVICE(key) { .abi=AK_ABI,.name="contract-" key, \
    .provides=(const ak_key[]){{key,1},{0}},.start=mount }
static const ak_dep dependencies[]={
    {"model",1,0},{"tools",1,0},{"sessions",1,0},{"permission",1,0},{"clock",1,0},{0}};
static const ak_plugin providers[]={
    SERVICE("model"),SERVICE("tools"),SERVICE("sessions"),SERVICE("permission"),SERVICE("clock"),
    {.abi=AK_ABI,.name="contract-default-loop",.provides=(const ak_key[]){{"loop",1},{0}},
     .deps=dependencies,.start=mount}
};
static void mount_app(fixture *f) {
    ak_platform host={.user=f,.alloc=allocate,.free=release,.now_ms=now};
    f->kernel=ak_create(&host); CHECK(f->kernel);
    f->model_context=(dunlin_http_v1){.user=f}; /* Callback context, never HTTP. */
    f->model=(dunlin_model_v1){.complete=model,.config={.base_url="http://offline.invalid",
        .model="scripted-contract",.timeout_ms=5000,.http=&f->model_context}};
    f->tools=(ah_tool_service){f,schema,check_tool,call_tool};
    f->sessions=(dunlin_sessions_v1){.service={f,open_session,save_session,close_session}};
    f->permission=(dunlin_permission_v1){f,approve};
    f->clock=(dunlin_clock_v1){now,f}; f->loop=(dunlin_loop_v1){ah_agent_run};
    f->bindings[0]=(binding){"model",&f->model};
    f->bindings[1]=(binding){"tools",&f->tools};
    f->bindings[2]=(binding){"sessions",&f->sessions};
    f->bindings[3]=(binding){"permission",&f->permission};
    f->bindings[4]=(binding){"clock",&f->clock};
    f->bindings[5]=(binding){"loop",&f->loop};
    CHECK(ak_install(f->kernel,&providers[5],&f->bindings[5],&f->loop_scope)==AK_OK);
    CHECK(ak_status(f->loop_scope)==AK_WAITING);
    for(size_t i=0;i<5;i++) {
        ak_scope *scope=NULL;
        CHECK(ak_install(f->kernel,&providers[i],&f->bindings[i],&scope)==AK_OK);
        if(!i) f->model_scope=scope;
    }
    CHECK(ak_status(f->loop_scope)==AK_ACTIVE);
    CHECK(ak_on(ak_root(f->kernel),"agent/event",kernel_observe,f,0));
    f->observer=(ah_hooks){.user=f,.cancelled=cancelled,.approve=unused_observer_approval,
        .event=observe,.now_ms=unused_observer_clock};
}
static void begin_fixture(fixture *f,const char *name) {
    case_name=name; memset(f,0,sizeof(*f));
    strcpy(f->directory,"/tmp/dunlin-contracts-XXXXXX"); CHECK(mkdtemp(f->directory));
    snprintf(f->sessions_path,sizeof(f->sessions_path),"%s/sessions",f->directory);
    CHECK(!ah_linux_sessions(&f->disk,f->sessions_path));
    f->allow=1; f->limits=ah_default_limits(); f->limits.task_timeout_ms=1000;
    mount_app(f);
}
static void unmount_app(fixture *f) {
    CHECK(!f->active_leases && f->opens==f->closes);
    CHECK(ak_destroy(f->kernel)==AK_OK); f->kernel=NULL;
    CHECK(!f->allocations && !f->observer_approvals && !f->observer_clocks);
}
static void restart_app(fixture *f) {
    unmount_app(f); ah_linux_sessions_close(&f->disk);
    CHECK(!ah_linux_sessions(&f->disk,f->sessions_path)); mount_app(f);
}
static void end_fixture(fixture *f) {
    unmount_app(f); ah_linux_sessions_close(&f->disk);
    DIR *directory=opendir(f->sessions_path); CHECK(directory);
    struct dirent *item;
    while((item=readdir(directory))) {
        if(!strcmp(item->d_name,".") || !strcmp(item->d_name,"..")) continue;
        CHECK(!unlinkat(dirfd(directory),item->d_name,0));
    }
    CHECK(!closedir(directory) && !rmdir(f->sessions_path) && !rmdir(f->directory));
    printf("PASS %s\n",case_name);
}
static void script(fixture *f,const step *steps,size_t count) {
    f->script=steps;f->script_count=count;f->position=0;
}
static void turn_using(fixture *f,const char *id,const char *input,ah_status expected,
                       const char *text,const ah_tool_service *adapter) {
    f->current_session=id; f->event_count=f->kernel_event_count=f->audit_count=0;
    ah_result result=adapter?
        dunlin_turn_with_tools(ak_root(f->kernel),&f->limits,&f->observer,id,input,NULL,adapter):
        dunlin_turn(ak_root(f->kernel),&f->limits,&f->observer,id,input);
    if(result.status!=expected || (text && !equal(result.text,text)))
        fprintf(stderr,"Observed %s: %s; wanted %s: %s\n",ah_status_name(result.status),
            result.text?result.text:"(null)",ah_status_name(expected),text?text:"(any)");
    CHECK(result.status==expected && (!text || equal(result.text,text))); free(result.text);
    CHECK(f->event_count==f->kernel_event_count && f->event_count>=2);
    for(size_t i=0;i<f->event_count;i++) {
        CHECK(equal(f->events[i].type,f->kernel_events[i].type));
        CHECK(equal(f->events[i].data,f->kernel_events[i].data));
        CHECK(f->events[i].size==f->kernel_events[i].size);
    }
    CHECK(occurrences(f,"turn.start")==1 && occurrences(f,"turn.end")==1);
    CHECK(equal(f->events[0].type,"turn.start") && equal(f->events[f->event_count-1].type,"turn.end"));
    CHECK(equal(f->events[f->event_count-1].data,ah_status_name(expected)));
    CHECK(!f->active_leases && f->opens==f->closes);
}
static void turn(fixture *f,const char *id,const char *input,ah_status expected,const char *text) {
    turn_using(f,id,input,expected,text,NULL);
}
static J *checkpoint(fixture *f,const char *id) {
    void *l=NULL;J *state=NULL;
    CHECK(f->disk.open(f->disk.ctx,id,&l,&state)==AH_DONE && state);
    f->disk.close(l); return state;
}

#define PUT "{\"role\":\"assistant\",\"tool_calls\":[{\"id\":\"put-1\",\"type\":\"function\",\"function\":{\"name\":\"put\",\"arguments\":\"{\\\"value\\\":7}\"}}]}"
#define GET "{\"role\":\"assistant\",\"tool_calls\":[{\"id\":\"get-1\",\"type\":\"function\",\"function\":{\"name\":\"get\",\"arguments\":\"{}\"}}]}"
#define FINAL "{\"role\":\"assistant\",\"content\":\"observed 7\"}"
static const step write_steps[]={
    {.user="store seven",.users=1,.response=PUT},
    {.user="store seven",.users=1,.pair="put-1",.result="7",.response=FINAL}
};
static ah_status pipeline_pre(void *user,const char *id,const char *name,const J *args,J **replacement,char *error,size_t size) {
    fixture *f=user;(void)error;(void)size;CHECK(equal(id,"put-1")&&equal(name,"put"));
    audit(f,"pipeline.pre",json_object_to_json_string_ext((J *)args,JSON_C_TO_STRING_PLAIN));
    *replacement=json_tokener_parse("{\"value\":7}");CHECK(*replacement);return AH_DONE;
}
static ah_status pipeline_post(void *user,const char *id,const char *name,ah_status outcome,const char *output,char **context) {
    fixture *f=user;CHECK(equal(id,"put-1")&&equal(name,"put")&&outcome==AH_DONE&&equal(output,"7"));
    audit(f,"pipeline.post",output);if(equal(f->fail_phase,"pipeline-post"))return AH_FAILED;
    *context=strdup("pipeline-context");CHECK(*context);return AH_DONE;
}
static void pipeline_finalized(void *user,const char *id,const char *name,ah_status outcome) {
    fixture *f=user;CHECK(equal(id,"put-1")&&equal(name,"put")&&outcome==AH_DONE);audit(f,"pipeline.finalized",id);
}
static void pipeline_v2(void) {
    fixture f;begin_fixture(&f,"versioned tool pipeline revalidates, preserves permission and finalizes after commit");
    static const step steps[]={
        {.user="store six",.users=1,.response="{\"role\":\"assistant\",\"tool_calls\":[{\"id\":\"put-1\",\"type\":\"function\",\"function\":{\"name\":\"put\",\"arguments\":\"{\\\"value\\\":6}\"}}]}"},
        {.user="store six",.users=1,.pair="put-1",.result="7\npipeline-context",.response=FINAL}
    };
    script(&f,steps,2);
    ah_tool_pipeline_v1 pipeline={&f,pipeline_pre,pipeline_post,pipeline_finalized};
    ah_agent_v2 agent={{.model=model,.model_config=&f.model.config,.tools=&f.tools,.sessions=&f.sessions.service,
        .hooks={.user=&f,.cancelled=cancelled,.approve=approve,.event=observe,.now_ms=now},.limits=f.limits},&pipeline};
    ah_result result=ah_agent_run_v2(&agent,"pipeline","store six");
    CHECK(result.status==AH_DONE&&equal(result.text,"observed 7"));free(result.text);
    CHECK(f.writes==1&&f.approvals==1&&f.value==7);
    size_t pre=locate(&f,"pipeline.pre","{\"value\":6}"),approval=locate(&f,"approve","{\"value\":7}"),
        pending=locate(&f,"save.pending","put-1"),post=locate(&f,"pipeline.post","7"),
        saved=locate(&f,"save.result","put-1"),finalized=locate(&f,"pipeline.finalized","put-1");
    CHECK(pre<approval&&approval<pending&&pending<post&&post<saved&&saved<finalized);
    end_fixture(&f);

    begin_fixture(&f,"pipeline unknown outcome never posts or finalizes");f.tool_unknown=1;script(&f,write_steps,1);
    pipeline=(ah_tool_pipeline_v1){&f,pipeline_pre,pipeline_post,pipeline_finalized};
    agent=(ah_agent_v2){{.model=model,.model_config=&f.model.config,.tools=&f.tools,.sessions=&f.sessions.service,
        .hooks={.user=&f,.cancelled=cancelled,.approve=approve,.event=observe,.now_ms=now},.limits=f.limits},&pipeline};
    result=ah_agent_run_v2(&agent,"pipeline-unknown","store seven");CHECK(result.status==AH_UNKNOWN);free(result.text);
    CHECK(locate(&f,"pipeline.post","7")==SIZE_MAX&&locate(&f,"pipeline.finalized","put-1")==SIZE_MAX);
    J *state=checkpoint(&f,"pipeline-unknown");CHECK(equal(string(state,"pending"),"put-1")&&equal(string(state,"pending_arguments"),"{\"value\":7}"));json_object_put(state);
    end_fixture(&f);

    begin_fixture(&f,"pipeline result save failure does not finalize or replay effect");f.fail_phase="result";script(&f,write_steps,1);
    pipeline=(ah_tool_pipeline_v1){&f,pipeline_pre,pipeline_post,pipeline_finalized};
    agent=(ah_agent_v2){{.model=model,.model_config=&f.model.config,.tools=&f.tools,.sessions=&f.sessions.service,
        .hooks={.user=&f,.cancelled=cancelled,.approve=approve,.event=observe,.now_ms=now},.limits=f.limits},&pipeline};
    result=ah_agent_run_v2(&agent,"pipeline-save","store seven");CHECK(result.status==AH_UNKNOWN);free(result.text);
    CHECK(f.writes==1&&locate(&f,"pipeline.post","7")!=SIZE_MAX&&locate(&f,"pipeline.finalized","put-1")==SIZE_MAX);
    unsigned calls=f.calls;result=ah_agent_run_v2(&agent,"pipeline-save",NULL);CHECK(result.status==AH_UNKNOWN&&f.calls==calls);free(result.text);
    end_fixture(&f);

    begin_fixture(&f,"post pipeline failure cannot roll back a committed effect");f.fail_phase="pipeline-post";
    const step post_failure[]={{.user="store seven",.users=1,.response=PUT},{.user="store seven",.users=1,.pair="put-1",.result="7",.response=FINAL}};
    script(&f,post_failure,2);pipeline=(ah_tool_pipeline_v1){&f,pipeline_pre,pipeline_post,pipeline_finalized};
    agent=(ah_agent_v2){{.model=model,.model_config=&f.model.config,.tools=&f.tools,.sessions=&f.sessions.service,
        .hooks={.user=&f,.cancelled=cancelled,.approve=approve,.event=observe,.now_ms=now},.limits=f.limits},&pipeline};
    result=ah_agent_run_v2(&agent,"pipeline-post-failure","store seven");
    CHECK(result.status==AH_DONE&&equal(result.text,"observed 7")&&f.writes==1);free(result.text);
    CHECK(occurrences(&f,"tool.pipeline_error")==1&&locate(&f,"pipeline.finalized","put-1")!=SIZE_MAX);
    end_fixture(&f);
}

static void composition(void) {
    fixture f;begin_fixture(&f,"assembled default Loop, effects, events, history and isolation");
    script(&f,write_steps,2); turn(&f,"alpha","store seven",AH_DONE,"observed 7");
    CHECK(f.position==2 && f.value==7 && f.writes==1 && f.approvals==1);
    /* Assert only causally necessary boundaries, not every internal event/save. */
    size_t approval=locate(&f,"approve","{\"value\":7}"), pending=locate(&f,"save.pending","put-1");
    size_t pre=locate(&f,"tool.pre_execute","put-1"),start=locate(&f,"tool.start","put-1"),effect=locate(&f,"effect","put");
    size_t post=locate(&f,"tool.post_execute","put-1"),saved=locate(&f,"save.result","put-1");
    size_t finalized=locate(&f,"tool.finalized","put-1"),result=locate(&f,"tool.result","put-1");
    CHECK(approval<pending && pending<pre && pre<start && start<effect && effect<post && post<saved &&
          saved<finalized && finalized<result && result!=SIZE_MAX);
    const step other[]={ {.user="separate",.users=1,.absent="store seven",.response=FINAL} };
    script(&f,other,1); turn(&f,"beta","separate",AH_DONE,"observed 7"); CHECK(f.position==1);
    restart_app(&f);
    const step follow[]={
        {.user="read again",.users=2,.prior="store seven",.absent="separate",.response=GET},
        {.user="read again",.users=2,.prior="store seven",.absent="separate",.pair="get-1",.result="7",.response=FINAL}
    };
    script(&f,follow,2);turn(&f,"alpha","read again",AH_DONE,"observed 7");
    CHECK(f.position==2 && f.writes==1 && f.reads==1 && f.value==7);
    unsigned requests=f.requests,calls=f.calls;
    restart_app(&f);script(&f,NULL,0);
    turn(&f,"alpha",NULL,AH_DONE,"observed 7");turn(&f,"alpha",NULL,AH_DONE,"observed 7");
    CHECK(f.requests==requests && f.calls==calls && !occurrences(&f,"tool.start"));
    end_fixture(&f);
}
static void denied(void) {
    fixture f;begin_fixture(&f,"installed permission owns approval; deny and missing callback have no effects");
    f.allow=0;script(&f,write_steps,1);
    turn(&f,"denied","store seven",AH_DENIED,"permission denied");
    CHECK(f.position==1 && f.approvals==1 && !f.calls && !f.value && !occurrences(&f,"tool.start"));
    f.allow=1;f.permission.approve=NULL;script(&f,write_steps,1);
    turn(&f,"no-provider","store seven",AH_DENIED,"permission denied");
    CHECK(!f.calls && f.approvals==1 && !f.observer_approvals);
    end_fixture(&f);
}
typedef struct { fixture *owner; unsigned schemas,checks,calls; } adapted_tools;
static J *adapter_schema(void *user) {
    adapted_tools *a=user;a->schemas++;return schema(a->owner);
}
static ah_status adapter_check(void *user,const char *name,J *args,int *approval,
    int *effect,char *error,size_t size) {
    adapted_tools *a=user;a->checks++;
    return check_tool(a->owner,name,args,approval,effect,error,size);
}
static ah_status adapter_call(void *user,const char *name,J *args,const ah_hooks *hooks,
    size_t max_output,unsigned timeout,char **output) {
    adapted_tools *a=user;a->calls++;
    CHECK(equal(name,"get") && !json_object_object_length(args));
    CHECK(hooks && hooks->now_ms(hooks->user)==a->owner->milliseconds && max_output>=2 && timeout);
    *output=strdup("11");CHECK(*output);return AH_DONE;
}
static void tool_adapter(void) {
    fixture f;begin_fixture(&f,"host tool adapter replaces schema/check/call without mutating registered tools");
    adapted_tools a={.owner=&f};
    const ah_tool_service adapter={&a,adapter_schema,adapter_check,adapter_call};
    const step steps[]={
        {.user="adapted read",.users=1,.response=GET},
        {.user="adapted read",.users=1,.pair="get-1",.result="11",
         .response="{\"role\":\"assistant\",\"content\":\"adapter observed 11\"}"}
    };
    script(&f,steps,2);turn_using(&f,"adapted","adapted read",AH_DONE,"adapter observed 11",&adapter);
    CHECK(f.position==2 && a.schemas==1 && a.checks==1 && a.calls==1 && !f.calls);
    CHECK(ak_get(ak_root(f.kernel),"tools",1)==&f.tools && f.value==0);
    turn(&f,"adapted",NULL,AH_DONE,"adapter observed 11");
    CHECK(a.calls==1 && !f.calls);end_fixture(&f);
}
static void save_failures(void) {
    const char *before[]={"input","model-round","proposal","pending"};
    for(size_t i=0;i<sizeof(before)/sizeof(*before);i++) {
        fixture f;begin_fixture(&f,before[i]); f.fail_phase=before[i]; script(&f,write_steps,1);
        turn(&f,"before-effect","store seven",AH_FAILED,NULL);
        CHECK(f.fault_used && !f.calls && !f.value && !occurrences(&f,"tool.pre_execute") && !occurrences(&f,"tool.start"));
        CHECK(f.requests==(i>=2?1u:0u));
        /* Release is observable: a fresh lease can immediately open this ID. */
        void *l=NULL;J *state=NULL;
        CHECK(f.disk.open(f.disk.ctx,"before-effect",&l,&state)==AH_DONE);
        f.disk.close(l);json_object_put(state);end_fixture(&f);
    }
    fixture f;begin_fixture(&f,"result save failure after effect persists unknown and forbids replay");
    f.fail_phase="result";script(&f,write_steps,1);
    turn(&f,"unknown","store seven",AH_UNKNOWN,"cannot checkpoint tool result");
    CHECK(f.fault_used && f.writes==1 && f.value==7 && occurrences(&f,"tool.post_execute")==1 &&
          !occurrences(&f,"tool.finalized") && !occurrences(&f,"tool.result"));
    J *state=checkpoint(&f,"unknown"); CHECK(equal(string(state,"pending"),"put-1"));json_object_put(state);
    unsigned requests=f.requests,calls=f.calls; restart_app(&f);script(&f,NULL,0);
    turn(&f,"unknown",NULL,AH_UNKNOWN,NULL);turn(&f,"unknown",NULL,AH_UNKNOWN,NULL);
    turn(&f,"unknown","another request",AH_UNKNOWN,NULL);
    CHECK(f.requests==requests && f.calls==calls && f.writes==1 && f.value==7);
    end_fixture(&f);

    begin_fixture(&f,"tool unknown outcome emits no post/finalized/result and retains pending");
    f.tool_unknown=1;script(&f,write_steps,1);
    turn(&f,"tool-unknown","store seven",AH_UNKNOWN,"7");
    CHECK(f.writes==1 && occurrences(&f,"tool.pre_execute")==1 && occurrences(&f,"tool.start")==1 &&
          occurrences(&f,"tool.outcome_unknown")==1 && !occurrences(&f,"tool.post_execute") &&
          !occurrences(&f,"tool.finalized") && !occurrences(&f,"tool.result"));
    state=checkpoint(&f,"tool-unknown");CHECK(equal(string(state,"pending"),"put-1"));json_object_put(state);
    end_fixture(&f);

    begin_fixture(&f,"pure read result save failure reports failed without inventing unknown effects");
    f.fail_phase="result";
    const step read[]={ {.user="read",.users=1,.response=GET} };
    script(&f,read,1);turn(&f,"read-failure","read",AH_FAILED,"cannot checkpoint tool result");
    CHECK(f.fault_used && f.reads==1 && !f.writes);
    state=checkpoint(&f,"read-failure");CHECK(!field(state,"pending"));json_object_put(state);
    end_fixture(&f);

    begin_fixture(&f,"commit then save error recovers paired result without repeating effect");
    f.fail_phase="result";f.commit_before_failure=1;script(&f,write_steps,2);
    turn(&f,"committed","store seven",AH_UNKNOWN,"cannot checkpoint tool result");
    CHECK(f.fault_used && f.writes==1 && f.position==1);
    restart_app(&f);turn(&f,"committed",NULL,AH_DONE,"observed 7");
    CHECK(f.position==2 && f.writes==1 && f.calls==1 && !occurrences(&f,"tool.start"));
    end_fixture(&f);

    begin_fixture(&f,"terminal save failure never reports completed; paired effects survive retry");
    f.fail_phase="terminal";script(&f,write_steps,2);
    turn(&f,"final-save","store seven",AH_FAILED,"cannot persist session outcome");
    CHECK(f.fault_used && f.writes==1 && f.position==2);
    const step retry[]={ {.user="store seven",.users=1,.pair="put-1",.result="7",.response=FINAL} };
    restart_app(&f);script(&f,retry,1);turn(&f,"final-save",NULL,AH_DONE,"observed 7");
    CHECK(f.position==1 && f.writes==1 && f.calls==1 && !occurrences(&f,"tool.start"));
    end_fixture(&f);
}
static void interruptions(void) {
    fixture f;begin_fixture(&f,"cancellation and deadline checked after proposal and approval");
    step cancelled_step=write_steps[0];cancelled_step.cancel=1;
    script(&f,&cancelled_step,1);turn(&f,"model-cancel","store seven",AH_CANCELLED,"cancelled");
    CHECK(!f.calls && !f.approvals);f.cancel=0;
    step expired=write_steps[0];expired.advance_ms=f.limits.task_timeout_ms;
    script(&f,&expired,1);turn(&f,"model-timeout","store seven",AH_TIMEOUT,"timeout");
    CHECK(!f.calls && !f.approvals);
    f.cancel_on_approval=1;script(&f,write_steps,1);
    turn(&f,"approval-cancel","store seven",AH_CANCELLED,"cancelled");
    CHECK(!f.calls && f.approvals==1);f.cancel=0;f.cancel_on_approval=0;
    f.timeout_on_approval=1;script(&f,write_steps,1);
    turn(&f,"approval-timeout","store seven",AH_TIMEOUT,"timeout");
    CHECK(!f.calls && f.approvals==2 && !occurrences(&f,"tool.start"));end_fixture(&f);

    begin_fixture(&f,"round budget retains paired effect and terminal resume does not dispatch");
    f.limits.max_rounds=1;script(&f,write_steps,1);
    turn(&f,"one-round","store seven",AH_BUDGET,"model round limit reached");
    CHECK(f.requests==1 && f.writes==1 && f.value==7);
    restart_app(&f);script(&f,NULL,0);
    turn(&f,"one-round",NULL,AH_BUDGET,"model round limit reached");
    CHECK(f.requests==1 && f.writes==1);end_fixture(&f);
}
static void revocation(void) {
    fixture f;begin_fixture(&f,"idle model plugin removal suspends Loop; replacement reactivates it");
    CHECK(ak_remove(f.model_scope)==AK_OK && ak_status(f.loop_scope)==AK_WAITING);
    CHECK(!ak_get(ak_root(f.kernel),"model",1) && !ak_get(ak_root(f.kernel),"loop",1));
    ah_result result=dunlin_turn(ak_root(f.kernel),&f.limits,&f.observer,"missing","hello");
    CHECK(result.status==AH_FAILED && !f.requests && !f.opens && !f.event_count);free(result.text);
    CHECK(ak_install(f.kernel,&providers[0],&f.bindings[0],&f.model_scope)==AK_OK);
    CHECK(ak_status(f.loop_scope)==AK_ACTIVE);
    const step hello[]={ {.user="hello",.users=1,.response=FINAL} };
    script(&f,hello,1);turn(&f,"replacement","hello",AH_DONE,"observed 7");
    CHECK(f.position==1 && !f.calls);end_fixture(&f);
}
int main(void) {
    composition();pipeline_v2();denied();tool_adapter();save_failures();interruptions();revocation();
    puts("PASS DUNLIN-AGENT-CONTRACTS: deterministic C application composition and persistence boundaries");
    return 0;
}
