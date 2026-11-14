import assert from 'node:assert/strict';
export async function calendarActions({w,button,pause,legacy,session}){
 // Constructed legacy source in this explicitly owned fixture. The calendar
 // itself reads actual C through the original published backend/query service.
 // jsdom has no animation engine or active animations.
 if(!w.Element.prototype.getAnimations)w.Element.prototype.getAnimations=()=>[];
 if(!w.Element.prototype.scrollTo)w.Element.prototype.scrollTo=function(options){this.scrollTop=options?.top??this.scrollTop;this.scrollLeft=options?.left??this.scrollLeft};
 await legacy(session);
 const tab=button('任务日程看板');assert.ok(tab);tab.click();
 let notice;for(let attempt=0;attempt<70;attempt++){await pause(100);notice=w.document.querySelector('#note');if(notice&&!notice.hidden&&notice.textContent.includes('部分历史没有逐事件时间'))break}
 if(!notice||notice.hidden){
  const response=await w.fetch('/_dsh/dsh-project-manager/api/gantt?warm=1');
  console.log('Calendar diagnostic',JSON.stringify({notice:notice?.outerHTML,headings:[...w.document.querySelectorAll('h1')].map(x=>x.textContent),response:await response.json()}).slice(0,4000));
 }
 assert.ok(notice&&!notice.hidden,'original calendar omitted backend partial-history status element');
 assert.match(notice.textContent,/部分历史没有逐事件时间/);assert.equal(notice.getAttribute('role'),'status');
 assert.ok(!notice.textContent.includes('1970'),'calendar invented an unknown activity date');
 // Exercise the published delegated card handler with WebKit's idle API absent.
 // This isolated DOM card avoids depending on the local calendar date.
 const panel=w.document.querySelector('#exBody');assert.ok(panel);
 const card=w.document.createElement('div');card.className='exi';card.tabIndex=0;panel.append(card);
 const errors=[];const capture=event=>errors.push(String(event.error??event.message));
 w.addEventListener('error',capture);
 const idle=Object.getOwnPropertyDescriptor(w,'requestIdleCallback');
 try{
  Object.defineProperty(w,'requestIdleCallback',{value:undefined,configurable:true});
  card.dispatchEvent(new w.MouseEvent('mousedown',{button:2,buttons:2,bubbles:true,cancelable:true}));
  await pause(40);
  assert.equal(w.document.activeElement,card,'deferred card focus did not run');
  assert.deepEqual(errors,[]);
 }finally{
  if(idle)Object.defineProperty(w,'requestIdleCallback',idle);else delete w.requestIdleCallback;
  w.removeEventListener('error',capture);card.remove();
 }
 return {secondaryButtonFocus:true,originalCalendar:true,realCLegacySource:true,partialHistoryNoticeVisible:true};
}
