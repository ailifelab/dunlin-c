import {randomUUID} from 'node:crypto';
// Private, owner-bound asynchronous responses. This scheduler never replays a
// request; C retains the original running intent until it sees a terminal receipt.
export function httpJobs(){
 const jobs=new Map();
 const unavailable=()=>Object.assign(Error('HTTP operation unavailable'),{code:-32602});
 return {
  async start(owner,args,invoke){
   // Unread receipts cannot expire: C still owns a durable running intent.
   // Both pending work and retained receipts share this bounded capacity.
   if(jobs.size>=32||[...jobs.values()].filter(r=>r.owner===owner).length>=8)throw Object.assign(Error('HTTP operation capacity exceeded'),{code:-32602});
   const id=randomUUID(),row={owner,surface:args._surface,done:false,cancelled:false,response:null,error:null,expires:Date.now()+120000};jobs.set(id,row);
   let operation;try{operation=invoke()}catch(error){jobs.delete(id);throw error}row.abort=()=>operation.abort?.();
   const settled=Promise.resolve(operation).then(response=>{row.response=response;row.done=true;row.expires=Date.now()+120000;return {response}},error=>{row.error=error;row.done=true;row.expires=Date.now()+120000;return {error}});row.settled=settled;
   let timer;const first=await Promise.race([settled,new Promise(resolve=>{timer=setTimeout(()=>resolve({pending:true}),100)})]);clearTimeout(timer);
   if(first.pending)return {pendingHandle:id};jobs.delete(id);if(first.error)throw first.error;return first.response;
  },
  read(owner,args){
   const row=jobs.get(args.id);if(!row||row.owner!==owner||row.surface!==args._surface)throw unavailable();
   row.expires=Date.now()+120000;
   if(args.release){row.cancelled=!row.done;if(!row.done)row.abort();jobs.delete(args.id);return {ready:true,cancelled:row.cancelled}}
   if(!row.done)return {ready:false};
   if(row.error)throw row.error;return {ready:true,response:row.response};
  },
  async close(owner,surface,all=false){
   const selected=[...jobs].filter(([,r])=>r.owner===owner&&(all||r.surface===surface));for(const [id,row]of selected){if(!row.done)row.abort();jobs.delete(id)}
   let timer;await Promise.race([Promise.all(selected.map(([,row])=>row.settled)),new Promise((_,reject)=>{timer=setTimeout(()=>reject(Error('HTTP operation did not stop on disposal')),2000)})]).finally(()=>clearTimeout(timer));
  }
 };
}
