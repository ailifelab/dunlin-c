// WebKit can decode VP9 color while discarding the Matroska BlockAdditional
// alpha plane. Keep the original media clock and composite the actual mask.
type Element = { id: number, start: number, data: number, end: number }
function elements(bytes: Uint8Array, start: number, end: number): Element[] {
  function vint(at: number, size: boolean) {
    let flag=128, width=1
    while(flag && !(bytes[at]&flag)){ flag>>=1; width++ }
    if(!flag || width>(size?8:4) || at+width>end) throw Error('Invalid WebM integer')
    let value=size?bytes[at]&(flag-1):bytes[at], unknown=size && value===flag-1
    for(let i=1;i<width;i++){value=value*256+bytes[at+i];unknown=unknown && bytes[at+i]===255}
    return {width,value:unknown?Infinity:value}
  }
  const result:Element[]=[]
  for(let at=start;at<end;){
    const id=vint(at,false),size=vint(at+id.width,true),data=at+id.width+size.width
    const next=size.value===Infinity?end:data+size.value
    if(next>end || result.length>=32768) throw Error('Invalid WebM element')
    result.push({id:id.value,start:at,data,end:next});at=next
  }
  return result
}
export function alphaVideo(bytes:Uint8Array):Blob|undefined {
  if(bytes.length>16777216)return
  const children=(e:Element)=>elements(bytes,e.data,e.end)
  const child=(e:Element,id:number)=>children(e).find(x=>x.id===id)
  const raw=(e:Element)=>bytes.subarray(e.start,e.end)
  function element(id:number[],parts:BlobPart[]):Blob{
    const payload=new Blob(parts);let size=payload.size,width=1
    while(size>=2**(7*width)-1)width++
    const header=new Uint8Array(id.length+width);header.set(id)
    for(let i=width-1;i>=0;i--){header[id.length+i]=size%256;size=Math.floor(size/256)}
    header[id.length]|=1<<(8-width)
    return new Blob([header,payload])
  }
  const top=elements(bytes,0,bytes.length),header=top.find(e=>e.id===0x1a45dfa3),segment=top.find(e=>e.id===0x18538067)
  if(!header||!segment)return
  const items=children(segment),info=items.find(x=>x.id===0x1549a966),tracks=items.find(x=>x.id===0x1654ae6b)
  if(!info||!tracks)return
  const track=children(tracks).find(e=>e.id===0xae&&child(e,0xe0)),video=track&&child(track,0xe0)
  if(!track||!video||!child(video,0x53c0))return
  const cleanVideo=element([0xe0],children(video).filter(e=>e.id!==0x53c0).map(raw))
  const cleanTrack=element([0xae],children(track).map(e=>e.id===0xe0?cleanVideo:raw(e)))
  const parts:BlobPart[]=[raw(header),new Uint8Array([0x18,0x53,0x80,0x67,0xff]),raw(info),element([0x16,0x54,0xae,0x6b],[cleanTrack])]
  let frames=0
  for(const cluster of items.filter(e=>e.id===0x1f43b675)){
    const blocks:BlobPart[]=[]
    for(const group of children(cluster)){
      if(group.id===0xe7){blocks.push(raw(group));continue}
      if(group.id!==0xa0)continue
      const block=child(group,0xa1),adds=child(group,0x75a1),more=adds&&child(adds,0xa6),alpha=more&&child(more,0xa5)
      if(!block||!alpha||bytes[block.data]!==0x81||(bytes[block.data+3]&6))continue
      const prefix=bytes.slice(block.data,block.data+4)
      if(!child(group,0xfb))prefix[3]|=128
      blocks.push(element([0xa3],[prefix,bytes.subarray(alpha.data,alpha.end)]));frames++
    }
    parts.push(element([0x1f,0x43,0xb6,0x75],blocks))
  }
  return frames?new Blob(parts,{type:'video/webm'}):undefined
}

export function installPetAlpha(){
  const active=new Map<HTMLVideoElement,()=>void>()
  function attach(video:HTMLVideoElement){
    const canvas=document.createElement('canvas'),mask=document.createElement('video'),scratch=document.createElement('canvas')
    const ctx=canvas.getContext('2d',{willReadFrequently:true}),alpha=scratch.getContext('2d',{willReadFrequently:true})
    if(!ctx||!alpha)return
    let url='',source='',generation=0,frame=0,lastColor=-1,lastMask=-1
    mask.muted=true;mask.playsInline=true;mask.preload='auto'
    // The original remains the hit target and retains upstream playback events.
    canvas.style.cssText='position:absolute;inset:0;width:100%;height:100%;object-fit:contain;pointer-events:none'
    function reset(){generation++;video.style.removeProperty('filter');canvas.remove();mask.pause();mask.removeAttribute('src');mask.load();if(url)URL.revokeObjectURL(url);url=''}
    async function load(){
      const next=video.currentSrc||video.src;if(!next||next===source)return
      reset();source=next;lastColor=lastMask=-1;const current=generation
      video.style.filter='opacity(0)'
      try{
        const response=await fetch(next);if(!response.ok)throw Error('Video unavailable')
        const blob=alphaVideo(new Uint8Array(await response.arrayBuffer()))
        if(current!==generation)return
        if(!blob){video.style.removeProperty('filter');return}
        url=URL.createObjectURL(blob);mask.src=url;mask.loop=video.loop
        await mask.play()
      }catch{if(current===generation)video.style.removeProperty('filter')}
    }
    function draw(){
      frame=requestAnimationFrame(draw)
      if(!video.isConnected||!url||!video.videoWidth||mask.readyState<2||video.readyState<2)return
      if(video.paused!==mask.paused){if(video.paused)mask.pause();else void mask.play().catch(()=>{})}
      if(Math.abs(mask.currentTime-video.currentTime)>0.10){if(!mask.seeking)mask.currentTime=video.currentTime;return}
      const w=video.videoWidth,h=video.videoHeight
      if(w*h>2073600)return
      if(canvas.width!==w||canvas.height!==h){canvas.width=scratch.width=w;canvas.height=scratch.height=h}
      const css=getComputedStyle(video);canvas.style.opacity=css.opacity;canvas.style.transform=css.transform
      if(css.opacity==='0'||(video.currentTime===lastColor&&mask.currentTime===lastMask))return
      lastColor=video.currentTime;lastMask=mask.currentTime
      ctx!.drawImage(video,0,0,w,h);alpha!.drawImage(mask,0,0,w,h)
      const color=ctx!.getImageData(0,0,w,h),plane=alpha!.getImageData(0,0,w,h)
      for(let i=0;i<color.data.length;i+=4)color.data[i+3]=plane.data[i]
      ctx!.putImageData(color,0,0);if(!canvas.isConnected)video.after(canvas)
      video.style.filter='opacity(0)'
    }
    video.addEventListener('loadeddata',load);void load();draw()
    active.set(video,()=>{generation++;video.removeEventListener('loadeddata',load);cancelAnimationFrame(frame);reset()})
  }
  function scan(){
    for(const [v,stop] of active)if(!v.isConnected){stop();active.delete(v)}
    for(const v of document.querySelectorAll<HTMLVideoElement>('video.dsh-pet-video'))if(!active.has(v))attach(v)
  }
  const observer=new MutationObserver(scan);observer.observe(document.body,{childList:true,subtree:true});scan()
  window.addEventListener('pagehide',()=>{observer.disconnect();for(const stop of active.values())stop();active.clear()},{once:true})
}
