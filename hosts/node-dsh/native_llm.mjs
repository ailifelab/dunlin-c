// Structural facade only: configured model metadata and text results cross this
// boundary. Protocol execution and native credentials stay in the C executor.
export function nativeLLM(native){
 let catalog,call;
 const messages={native_helper_provider_failed:'模型服务返回错误，请检查模型配置或稍后重试',native_helper_timeout:'生成标题超时，请稍后重试',native_helper_cancelled:'本次生成已取消',native_helper_lease_closed:'任务界面已关闭，本次生成已取消',native_helper_credentials_unavailable:'无法读取此模型的凭据，请在模型设置中检查',native_helper_result_receipt_unknown:'生成结果的记录未能确认，请先查看插件日志，系统不会自动重试',native_helper_request_mismatch:'标题请求与本次任务不一致，未执行模型调用'};
 const message=code=>messages[code]??code??'模型辅助服务暂不可用';
 function invoke(method,args={}){const value=native.call(method,args);if(!value?.ok)throw Object.assign(Error(message(value?.error)),{code:value?.error??'DUNLIN_HELPER_REFUSED'});return value.data}
 function models(){const current=native.context()?.nativeCall;if(!current)throw Error('Current native request required');if(current!==call){catalog=invoke('helper.models');call=current}return catalog}
 function info(provider,model){if(provider!=='dunlin-native')throw Error('Unknown native provider');const row=models().find(x=>x.id===model);if(!row)throw Error('Native model changed');const caps=row.capabilities??[],efforts=[];if(caps.includes('none'))efforts.push({id:'none',name:'不思考'});if(caps.includes('levels'))for(const id of ['low','medium','high'])efforts.push({id,name:id});if(caps.includes('xhigh'))efforts.push({id:'xhigh',name:'xhigh'});return efforts.length?{reasoning:{efforts,...(efforts.some(e=>e.id===row.defaultEffort)?{defaultEffort:row.defaultEffort}:{})}}:{};}
 return Object.freeze({
  listProviders(){return models().length?[{id:'dunlin-native',name:'Dunlin C'}]:[]},
  async listModels(provider){if(provider!=='dunlin-native')throw Error('Unknown native provider');return models().map(({id,name})=>({id,name}))},
  async resolveModelInfo(provider,model,signal){signal?.throwIfAborted();return info(provider,model)},
  async prepareCall(route,signal,controls){
   signal?.throwIfAborted();info(route.provider,route.model);const selected=controls?.({...route})??route;
   return {config:Object.freeze({}),async *stream(request){
    const activeSignal=request.signal??signal;activeSignal?.throwIfAborted();
    const messages=request.messages,parts=messages?.[0]?.content;
    if(!Array.isArray(messages)||messages.length!==1||messages[0].role!=='user'||!Array.isArray(parts)||parts.length!==1||parts[0].type!=='text'||typeof parts[0].text!=='string')throw Error('Text-only helper request required');
    const {id}=invoke('helper.submit',{provider:route.provider,model:route.model,maxTokens:route.maxTokens,effort:selected.reasoningEffort??'',purpose:request.purpose,system:request.system,session:request.sessionId??'',input:parts[0].text});
    let settled=false;
    try{
     for(;;){
      if(activeSignal?.aborted){invoke('helper.cancel',{id});settled=true;activeSignal.throwIfAborted()}
      const result=invoke('helper.poll',{id});
      if(result.state==='reported'){settled=true;if(result.text)yield {type:'text-delta',text:result.text};yield {type:'finish',reason:{kind:'stop'}};return}
      if(['failed','cancelled','unknown'].includes(result.state)){settled=true;yield {type:'finish',reason:{kind:result.state==='cancelled'?'aborted':'error',failure:{code:result.error||'DUNLIN_HELPER_FAILED',message:message(result.error)||'模型辅助调用失败'}}};return}
      await new Promise(resolve=>setTimeout(resolve,100));
     }
    }finally{if(!settled)invoke('helper.cancel',{id})}
   }};
  }
 });
}
