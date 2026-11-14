/* Optional private reverse channel. Only the thread already waiting for this
 * worker can admit a callback; child code cannot invoke arbitrary native APIs. */
static const char *native_send(dc_worker *w,const char *bytes,size_t length,uint64_t deadline,const dc_hooks *hooks){
 for(size_t offset=0;offset<length;){
  if(cancelled(hooks))return "cancelled";if(now()>=deadline)return "timeout";
  struct pollfd fd={w->native_socket,POLLOUT,0};int ready=poll(&fd,1,50);if(ready<0&&errno==EINTR)continue;if(ready<0||fd.revents&(POLLHUP|POLLERR|POLLNVAL))return "native_channel_disconnected";if(!(fd.revents&POLLOUT))continue;
#ifdef MSG_NOSIGNAL
  ssize_t n=send(w->native_socket,bytes+offset,length-offset,MSG_NOSIGNAL);
#else
  ssize_t n=send(w->native_socket,bytes+offset,length-offset,0);
#endif
  if(n<0&&(errno==EAGAIN||errno==EINTR))continue;if(n<=0)return "native_channel_write_failed";offset+=(size_t)n;
 }return NULL;
}
static const char *native_read(dc_worker *w,uint64_t deadline,const dc_hooks *hooks){
 if(!w->native_active||!w->native_buffer)return "native_callback_outside_call";
 for(;;){
  char *newline=memchr(w->native_buffer,'\n',w->native_used);
  if(!newline){
   if(w->native_used>=FRAME_LIMIT)return "native_frame_exceeds_limit";
   ssize_t n=read(w->native_socket,w->native_buffer+w->native_used,FRAME_LIMIT-w->native_used);
   if(n<0&&errno==EINTR)continue;if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK))return NULL;if(n<=0)return "native_channel_disconnected";w->native_used+=(size_t)n;continue;
  }
  size_t length=(size_t)(newline-w->native_buffer);J *request=dunlin_json_parse(w->native_buffer,length);size_t consumed=length+1;memmove(w->native_buffer,w->native_buffer+consumed,w->native_used-consumed);w->native_used-=consumed;
  const char *method=str(request,"method");J *major=get(request,"major"),*sequence=get(request,"sequence"),*rpc=get(request,"rpcId");
  int wake=eq(str(request,"protocol"),"dunlin.native.wake");
  int valid=(wake||eq(str(request,"protocol"),"dunlin.native.call"))&&json_object_is_type(major,json_type_int)&&json_object_get_int(major)==1&&eq(str(request,"nonce"),w->nonce)&&json_object_is_type(sequence,json_type_int)&&json_object_get_int64(sequence)>0&&w->native_seq<INT64_MAX&&json_object_get_int64(sequence)==(int64_t)(w->native_seq+1)&&json_object_is_type(rpc,json_type_int)&&json_object_get_int64(rpc)==(wake?0:(int64_t)w->seq)&&(wake||(method&&*method&&strlen(method)<=128&&strspn(method,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-")==strlen(method)&&json_object_is_type(get(request,"arguments"),json_type_object)));
  if(!valid){json_object_put(request);return "native_request_mismatch";}
  if(++w->native_count>128){json_object_put(request);return "native_callback_limit";}w->native_seq++;
  if(cancelled(hooks)){json_object_put(request);return "cancelled";}
  if(wake){
   /* A suspended SDK continuation can wake at an async boundary. It waits
    * for this next C request and learns its RPC id before sending an actual
    * admission. Waking never dispatches a native method or grants authority. */
   J *ready=json_object_new_object();json_object_object_add(ready,"protocol",json_object_new_string("dunlin.native.ready"));json_object_object_add(ready,"major",json_object_new_int(1));json_object_object_add(ready,"nonce",json_object_new_string(w->nonce));json_object_object_add(ready,"sequence",json_object_get(sequence));json_object_object_add(ready,"rpcId",json_object_new_int64((int64_t)w->seq));const char *raw=json_object_to_json_string_ext(ready,JSON_C_TO_STRING_PLAIN);const char *error=native_send(w,raw,strlen(raw),deadline,hooks);if(!error)error=native_send(w,"\n",1,deadline,hooks);json_object_put(ready);json_object_put(request);if(error)return error;continue;
  }
  const char *raw=json_object_to_json_string_ext(request,JSON_C_TO_STRING_PLAIN);char *result=w->native_fn?w->native_fn(w->native_user,raw,hooks):reply(0,"native_callback_unavailable",NULL,"not_started");
  J *value=result&&strlen(result)<FRAME_LIMIT-512?dunlin_json_parse(result,strlen(result)):NULL;free(result);
  if(!json_object_is_type(value,json_type_object)){json_object_put(value);json_object_put(request);return "native_callback_invalid_result";}
  J *response=json_object_new_object();json_object_object_add(response,"protocol",json_object_new_string("dunlin.native.reply"));json_object_object_add(response,"major",json_object_new_int(1));json_object_object_add(response,"nonce",json_object_new_string(w->nonce));json_object_object_add(response,"sequence",json_object_get(sequence));json_object_object_add(response,"rpcId",json_object_get(rpc));json_object_object_add(response,"result",value);
  raw=json_object_to_json_string_ext(response,JSON_C_TO_STRING_PLAIN);const char *error=strlen(raw)<FRAME_LIMIT?native_send(w,raw,strlen(raw),deadline,hooks):"native_frame_exceeds_limit";if(!error)error=native_send(w,"\n",1,deadline,hooks);json_object_put(response);json_object_put(request);if(error)return error;
  if(!w->native_used)return NULL;
 }
}
