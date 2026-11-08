#include <dunlin/compact/embedded.h>
#include <string.h>
_Static_assert(AE_SLOT_BYTES % _Alignof(max_align_t)==0,"pool slot alignment");
void *ae_pool_alloc(void *user,size_t size) {
 ae_pool *p=user;
 if(!p || !size || size>AE_POOL_SLOTS*AE_SLOT_BYTES) {if(p)p->rejected++;return NULL;}
 size_t slots=(size+AE_SLOT_BYTES-1)/AE_SLOT_BYTES;
 for(size_t i=0;i+slots<=AE_POOL_SLOTS;i++) {
  size_t free=0;while(free<slots && !p->used[i+free])free++;
  if(free!=slots)continue;
  p->used[i]=(unsigned char)slots;
  for(size_t j=1;j<slots;j++)p->used[i+j]=255;
  p->live+=slots;if(p->live>p->peak)p->peak=p->live;
  memset(p->arena.bytes+i*AE_SLOT_BYTES,0,slots*AE_SLOT_BYTES);return p->arena.bytes+i*AE_SLOT_BYTES;
 }
 p->rejected++;return NULL;
}
void ae_pool_free(void *user,void *memory) {
 ae_pool *p=user;if(!p || !memory)return;
 for(size_t i=0;i<AE_POOL_SLOTS;i++) if(memory==p->arena.bytes+i*AE_SLOT_BYTES && p->used[i] && p->used[i]!=255) {
  size_t slots=p->used[i];memset(p->arena.bytes+i*AE_SLOT_BYTES,0,slots*AE_SLOT_BYTES);
  memset(p->used+i,0,slots);p->live-=slots;return;
 }
}
