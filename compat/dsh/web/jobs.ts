// Structural client jobs face. C owns identity, lineage, source and cancel.
export function installJobs(root:any,request:any,observation:any,current:string){
 const state=observation({rows:{},observed:{},errors:{}}),watched=new Map<string,number>(),observed=new Map<string,{sessionId:string,count:number}>()
 let disposed=false,reading=false,timer:number|undefined
 const call=async(method:string,args:any)=>{const response=await request('native.jobs.'+method,args);return response?.value??response}
 const publish=(section:string,key:string,value:any)=>{if(disposed)return;const before=state.getSnapshot();if(JSON.stringify(before[section][key])===JSON.stringify(value))return;const next={...before,[section]:{...before[section]}};if(value===undefined)delete next[section][key];else next[section][key]=value;state.set(next)}
 async function refresh(){
  if(disposed||reading)return;reading=true
  try{
   for(const sessionId of [...watched.keys()]){
    try{let before=0,rows:any[]=[],bytes=0;const seen=new Set<string>();do{
     if(disposed||!watched.has(sessionId))break
     const page=await call('list',{sessionId,before});if(!Array.isArray(page.rows))throw Error('Invalid C job roster')
     for(const row of page.rows){if(seen.has(row.id))throw Error('Repeated C job identity');seen.add(row.id);rows.push(row)}
     bytes+=JSON.stringify(page.rows).length;if(bytes>16777216)throw Error('任务列表超过工作台显示上限，请在协作管理中筛选')
     if(page.next!==undefined&&(!Number.isSafeInteger(page.next)||page.next<=0||(before&&page.next>=before)))throw Error('Invalid C job cursor');before=page.next??0
    }while(before)
    if(!disposed&&watched.has(sessionId)){publish('rows',sessionId,rows);publish('errors',sessionId,undefined)}
    }catch(error){if(watched.has(sessionId))publish('errors',sessionId,String(error))}
   }
   for(const [id,row] of [...observed]){
    try{const value=await call('observe',{sessionId:row.sessionId,id});if(observed.get(id)===row)publish('observed',id,value)}
    catch(error){if(observed.get(id)===row)publish('observed',id,{text:'',gapBefore:false,streaming:false,error:String(error)})}
   }
  }finally{reading=false;if(!disposed&&(watched.size||observed.size))timer=window.setTimeout(()=>void refresh(),750)}
 }
 function wake(){if(disposed||reading)return;clearTimeout(timer);void refresh()}
 const service={state,refresh:wake,watchRows(sessionId:string){if(disposed)return()=>{};watched.set(sessionId,(watched.get(sessionId)??0)+1);wake();let released=false;return()=>{if(released)return;released=true;const n=(watched.get(sessionId)??1)-1;if(n)watched.set(sessionId,n);else{watched.delete(sessionId);publish('rows',sessionId,undefined);publish('errors',sessionId,undefined)}}},observe(sessionId:string|undefined,id:string){if(disposed)return()=>{};sessionId??=current;const old=observed.get(id);if(old&&old.sessionId!==sessionId)throw Error('Job observation owner conflict');const row=old??{sessionId,count:0};row.count++;observed.set(id,row);wake();let released=false;return()=>{if(released)return;released=true;if(--row.count===0){observed.delete(id);publish('observed',id,undefined)}}},async kill(sessionId:string,id:string){if(disposed)throw Error('Jobs surface disposed');await call('kill',{sessionId,id});wake()}}
 root.provide('jobs',service);root.effect(()=>()=>{disposed=true;clearTimeout(timer);watched.clear();observed.clear()});return service
}
