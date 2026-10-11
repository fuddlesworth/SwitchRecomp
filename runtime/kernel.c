/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* HLE kernel: handles, objects, guest threads (1:1 host threads), sync (events, mutex/condvar,
   address arbiter) and the SVCs games actually call. */
#include "rt.h"

PMutex g_klock;
u64 g_title_id = 0x0100000000001000ull;
u64 g_sysres_size;
int g_exiting;
static u64 g_next_tid = 0x50;

#define R_TIMEOUT     0xEA01
#define R_CANCELLED   0xEC01
#define R_BADHANDLE   0xE401
#define R_BADSTATE    0xFA01
#define R_BADADDR     0xCC01
#define R_BADSIZE     0xCA01
#define R_OUTOFMEM    0xD001
#define R_BADENUM     0xF001
#define R_BADCOMBO    0xE801
#define R_NOTFOUND    0xF201
#define R_OUTOFRANGE  0xEE01
#define R_BADMEMSTATE 0xD401
#define R_BADREGION   0xDC01
#define R_NOTIMPL     0xF601
#define HANDLE_WAIT   0x40000000u
#define H_CUR_PROC    0xFFFF8001u
#define H_CUR_THREAD  0xFFFF8000u
typedef struct { u32 imm, res; u64 lr, a[4], o1; } SvcRec;
static SvcRec g_svc_ring[64]; static u32 g_svc_ring_n;

/* ============================================================ handles */
#define MAXH 4096
static KObj* htab[MAXH]; static u16 hgen[MAXH]; static int hnext=1;
void kobj_ref(KObj* o){ if(o) __atomic_add_fetch(&o->refs,1,__ATOMIC_SEQ_CST); }
void kobj_unref(KObj* o){ if(o && __atomic_sub_fetch(&o->refs,1,__ATOMIC_SEQ_CST)==0 && o->destroy) o->destroy(o); }
u32 h_new(KObj* o){
  pm_lock(&g_klock);
  for(int k=0;k<MAXH;k++){ int i=hnext; hnext=hnext+1>=MAXH? 1 : hnext+1;
    if(!htab[i]){ htab[i]=o; kobj_ref(o); hgen[i]=(u16)((hgen[i]+1)&0x7FFF); if(!hgen[i]) hgen[i]=1; pm_unlock(&g_klock); return ((u32)hgen[i]<<15)|(u32)i; } }
  pm_unlock(&g_klock); fatal("no free handles"); return 0; }
static KObj* h_get_nl(u32 h){
  if(h==H_CUR_THREAD) return (KObj*)kthread_cur();
  u32 i=h&0x7FFF; if(!i || i>=MAXH || !htab[i] || hgen[i]!=(h>>15)) return NULL; return htab[i]; }
KObj* h_get(u32 h){ pm_lock(&g_klock); KObj* o=h_get_nl(h); pm_unlock(&g_klock); return o; }
KObj* h_get_type(u32 h, int t){ KObj* o=h_get(h); return (o && o->type==t)? o : NULL; }
int h_close(u32 h){
  pm_lock(&g_klock); u32 i=h&0x7FFF;
  if(!i || i>=MAXH || !htab[i] || hgen[i]!=(h>>15)){ pm_unlock(&g_klock); return R_BADHANDLE; }
  KObj* o=htab[i]; htab[i]=NULL; pm_unlock(&g_klock); kobj_unref(o); return 0; }

/* ============================================================ waits */
static void waiter_add(KObj* o, KThread* t){
  if(o->nwait==o->capwait){ o->capwait=o->capwait? o->capwait*2 : 4; o->waiters=realloc(o->waiters,sizeof(KThread*)*o->capwait); }
  o->waiters[o->nwait++]=t; }
static void waiter_del(KObj* o, KThread* t){
  for(int i=0;i<o->nwait;i++) if(o->waiters[i]==t){ o->waiters[i]=o->waiters[--o->nwait]; return; } }
static void wake(KThread* t, int result){ t->woken=1; t->wait_result=result; pc_signal(&t->cv); }
static void kobj_signal_nl(KObj* o){ for(int i=0;i<o->nwait;i++) wake(o->waiters[i],0); }
void kobj_signal(KObj* o){ pm_lock(&g_klock); kobj_signal_nl(o); pm_unlock(&g_klock); }
/* call with the lock held. 1 = woken, 0 = timeout */
static int sleep_nl(KThread* t, i64 timeout){
  if(timeout<0){ while(!t->woken) pc_wait(&t->cv,&g_klock,-1); return 1; }
  u64 end=plat_ns()+(u64)timeout;
  while(!t->woken){ u64 now=plat_ns(); if(now>=end) return 0; pc_wait(&t->cv,&g_klock,(i64)(end-now)); }
  return 1; }

/* ============================================================ events */
static int ev_sig(KObj* o){ return ((KEvent*)o)->signaled; }
static void ev_destroy(KObj* o){ free(o->waiters); free(o); }
KEvent* kevent_new(const char* name, int autoclear){
  KEvent* e=calloc(1,sizeof *e); e->o.type=KO_EVENT; e->o.is_signaled=ev_sig; e->o.destroy=ev_destroy; e->o.name=name; e->autoclear=autoclear; return e; }
void kevent_signal(KEvent* e){ pm_lock(&g_klock); e->signaled=1; kobj_signal_nl(&e->o); pm_unlock(&g_klock); }
void kevent_clear(KEvent* e){ pm_lock(&g_klock); e->signaled=0; pm_unlock(&g_klock); }

/* ============================================================ threads */
static KThread* g_threads[256]; static int g_nthreads;
#if defined(__ANDROID__)
#include <pthread.h>
static pthread_key_t g_tkey; static pthread_once_t g_tonce=PTHREAD_ONCE_INIT;
static void tkey_init(void){ pthread_key_create(&g_tkey,NULL); }
KThread* kthread_cur(void){ pthread_once(&g_tonce,tkey_init); return (KThread*)pthread_getspecific(g_tkey); }
static void kthread_set(void* t){ pthread_once(&g_tonce,tkey_init); pthread_setspecific(g_tkey,t); }
#else
KThread* kthread_cur(void){ return (KThread*)tls_thread; }
static void kthread_set(void* t){ tls_thread=t; }
#endif
static int th_sig(KObj* o){ return ((KThread*)o)->state==2; }
static void th_destroy(KObj* o){ (void)o; /* thread objects are never freed */ }
static u64 alloc_tls(void){ u64 a=kmem_alloc(0x1000,MS_TLS,PERM_RW); memset(GP(a),0,0x1000); return a; }

KThread* kthread_create(u64 entry, u64 arg, u64 stack_top, int prio, int core){
  KThread* t=calloc(1,sizeof *t);
  t->o.type=KO_THREAD; t->o.is_signaled=th_sig; t->o.destroy=th_destroy; t->o.name="thread";
  t->entry=entry; t->arg=arg; t->stack_top=stack_top; t->prio=prio; t->core=core<0? 0 : core; t->affinity=1ull<<t->core;
  t->tls=alloc_tls(); pc_init(&t->cv);
  pm_lock(&g_klock); t->id=g_next_tid++; if(g_nthreads<256) g_threads[g_nthreads++]=t; pm_unlock(&g_klock);
  CPU* c=&t->cpu; memset(c,0,sizeof *c);
  c->mem=g_mem; c->disp=g_disp; c->code_lo=g_code_lo; c->code_hi=g_code_hi; c->rt=&g_rtapi; c->thread=t;
  c->tpidrro=t->tls; c->x[0]=arg; c->x[31]=stack_top; c->pc=entry;
  return t;
}

void swr_fpcr_apply(CPU* c);
static void thread_body(void* p){
  KThread* t=p; kthread_set(t); CPU* c=&t->cpu;
  swr_fpcr_apply(c);
  LOGV("[thread %llu] starts at %llx\n",(unsigned long long)t->id,(unsigned long long)c->pc);
  for(;;){
    if(UNLIKELY(c->pc==0)){ /* entry function returned */
      extern void svc_exit_thread(KThread* t); svc_exit_thread(t); }
    GuestFn f=swr_lookup(c,c->pc); f(c,c->pc);
  }
}
void kthread_start(KThread* t){
  t->state=1;
  if(plat_thread(thread_body,t,(size_t)64<<20)) fatal("could not create a system thread");
}
void svc_exit_thread(KThread* t){
  pm_lock(&g_klock); t->state=2; kobj_signal_nl(&t->o); pm_unlock(&g_klock);
  LOGV("[thread %llu] exits\n",(unsigned long long)t->id);
  plat_thread_exit();
}

void kernel_init(void){ pm_init(&g_klock); }

/* main thread: stack size from npdm, x0=0, x1=own handle */
static KThread* g_main;
void kernel_run_main(u64 entry, u64 stack_size, int prio, int core){
  stack_size=(stack_size+0xFFF)&~0xFFFull;
  u64 st=mem_find_free(STACK_BASE,STACK_BASE+STACK_SIZE,stack_size+0x2000,0x1000)+0x1000;
  mem_map(st,stack_size,MS_STACK,PERM_RW);
  KThread* t=kthread_create(entry,0,st+stack_size,prio,core);
  t->is_main=1; g_main=t; strcpy(t->name,"main");
  u32 h=h_new(&t->o); t->cpu.x[1]=h;
  kthread_start(t);
}

void process_exit(int code){
  g_exiting=1; LOG("The game exited (code %d)\n",code);
  fflush(NULL); exit(code);
}

/* ============================================================ kernel mutex arbitration */
static KThread* g_mwait;     /* waiters on a mutex (wait_kind 2), condvar (3) or address (4) */
static void wl_add(KThread* t){ t->next_waiter=g_mwait; g_mwait=t; }
static void wl_del(KThread* t){ KThread** p=&g_mwait; while(*p){ if(*p==t){ *p=t->next_waiter; t->next_waiter=NULL; return; } p=&(*p)->next_waiter; } }
static int wl_has(KThread* t){ for(KThread* w=g_mwait;w;w=w->next_waiter) if(w==t) return 1; return 0; }
/* best priority first (lowest number), FIFO within */
static KThread* wl_best(int kind, u64 key){
  KThread* best=NULL;
  for(KThread* w=g_mwait;w;w=w->next_waiter){
    if(w->wait_kind!=kind) continue;
    if((kind==2 && w->wait_addr!=key) || (kind==3 && w->wait_key!=key) || (kind==4 && w->wait_addr!=key)) continue;
    if(!best || w->prio<=best->prio) best=w;       /* list is reversed so <= picks the oldest */
  }
  return best; }
static int wl_count(int kind, u64 key){ int n=0;
  for(KThread* w=g_mwait;w;w=w->next_waiter) if(w->wait_kind==kind && ((kind==3? w->wait_key : w->wait_addr)==key)) n++; return n; }

static u32* U32(u64 a){ return (u32*)GP(a); }
static void unlock_nl(u64 addr){
  KThread* n=wl_best(2,addr);
  if(!n){ __atomic_store_n(U32(addr),0,__ATOMIC_SEQ_CST); return; }
  wl_del(n); n->wait_kind=0;
  int more=wl_count(2,addr)>0;
  __atomic_store_n(U32(addr),n->wait_tag|(more? HANDLE_WAIT : 0),__ATOMIC_SEQ_CST);
  wake(n,0);
}
/* Guards against a stale owner tag. Code built by old tcc emulated LDXR/STXR with a private spinlock, which
   isn't atomic vs the kernel's writes, so the game could clobber our release and leave a dead owner behind.
   Owner is bogus if the mutex word is 0, the thread exited, or it's itself waiting on this mutex / a condvar on it. */
static int g_heal_n;
static int owner_is_stale_nl(u32 tagv, u64 addr){
  u32 o=tagv&~HANDLE_WAIT; if(!o) return 1;
  if(o==H_CUR_THREAD) return 0;                                   /* not sure, leave it */
  KObj* ob=h_get_nl(o); if(!ob || ob->type!=KO_THREAD) return 0;  /* same thread; never steal from a real owner */
  KThread* ot=(KThread*)ob; if(ot->state==2) return 1;
  if((ot->wait_kind==2 || ot->wait_kind==3) && ot->wait_addr==addr && wl_has(ot)) return 1;
  return 0; }
static void heal_note(const char* what, u64 addr, u32 v){
  if(g_heal_n++<20 || !(g_heal_n&1023)) WARN("kernel: mutex %llx repaired (%s, value %x) [%d times]\n",(unsigned long long)addr,what,v,g_heal_n); }
static u32 svc_arbitrate_lock(KThread* t, u32 owner, u64 addr, u32 tag){
  if(addr&3) return R_BADADDR;
  pm_lock(&g_klock);
  u32 v=__atomic_load_n(U32(addr),__ATOMIC_SEQ_CST);
  if(v!=(owner|HANDLE_WAIT)){ pm_unlock(&g_klock); return 0; }
  if(owner_is_stale_nl(v,addr)){
    /* already released: hand it to us, keep the waiters bit if anyone else is queued */
    u32 nv=tag|(wl_count(2,addr)? HANDLE_WAIT : 0);
    if(__atomic_compare_exchange_n(U32(addr),&v,nv,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST)) heal_note("on request",addr,v);
    pm_unlock(&g_klock); return 0; }   /* caller rereads the word and retries */
  t->wait_kind=2; t->wait_addr=addr; t->wait_tag=tag; t->woken=0; wl_add(t);
  sleep_nl(t,-1);
  u32 r=t->wait_result; pm_unlock(&g_klock); return r; }
static u32 svc_arbitrate_unlock(u64 addr){
  if(addr&3) return R_BADADDR;
  pm_lock(&g_klock); unlock_nl(addr); pm_unlock(&g_klock); return 0; }
static u32 svc_wait_cv(KThread* t, u64 maddr, u64 key, u32 tag, i64 timeout){
  pm_lock(&g_klock);
  unlock_nl(maddr);
  if(timeout==0){ pm_unlock(&g_klock); return R_TIMEOUT; }
  t->wait_kind=3; t->wait_key=key; t->wait_addr=maddr; t->wait_tag=tag; t->woken=0; wl_add(t);
  __atomic_store_n(U32(key),1,__ATOMIC_SEQ_CST);
  int ok=sleep_nl(t,timeout);
  u32 r;
  if(!ok){ if(wl_has(t)){ wl_del(t); } t->wait_kind=0; r=R_TIMEOUT;
    if(!wl_count(3,key)) __atomic_store_n(U32(key),0,__ATOMIC_SEQ_CST); }
  else r=t->wait_result;
  pm_unlock(&g_klock); return r; }
static u32 svc_signal_cv(u64 key, i32 count){
  pm_lock(&g_klock);
  int n=0;
  while(count<=0 || n<count){
    KThread* w=wl_best(3,key); if(!w) break;
    wl_del(w); n++;
    u64 ma=w->wait_addr;
    for(;;){
      u32 v=__atomic_load_n(U32(ma),__ATOMIC_SEQ_CST);
      if(v==0){ u32 z=0; if(__atomic_compare_exchange_n(U32(ma),&z,w->wait_tag,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST)){ w->wait_kind=0; wake(w,0); break; } }
      else { if(__atomic_compare_exchange_n(U32(ma),&v,v|HANDLE_WAIT,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST)){ w->wait_kind=2; wl_add(w); break; } }
    }
  }
  if(!wl_count(3,key)) __atomic_store_n(U32(key),0,__ATOMIC_SEQ_CST);
  pm_unlock(&g_klock); return 0; }

/* per-frame sweep for mutex waiters stuck behind a bogus owner, or whose waiters bit got cleared
   (the owner then unlocks without calling us) */
void kernel_heal_mutexes(void){
  pm_lock(&g_klock);
  for(int guard=0; guard<64; guard++){
    int changed=0;
    for(KThread* w=g_mwait; w; w=w->next_waiter){
      if(w->wait_kind!=2) continue;
      u64 a=w->wait_addr; u32 v=__atomic_load_n(U32(a),__ATOMIC_SEQ_CST);
      if(owner_is_stale_nl(v,a)){
        KThread* n=wl_best(2,a); if(!n) continue;
        wl_del(n); int more=wl_count(2,a)>0; wl_add(n);
        u32 nv=n->wait_tag|(more? HANDLE_WAIT : 0);
        if(__atomic_compare_exchange_n(U32(a),&v,nv,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST)){
          wl_del(n); n->wait_kind=0; wake(n,0); heal_note("stale owner",a,v); changed=1; break; }
      } else if(!(v&HANDLE_WAIT)){
        if(__atomic_compare_exchange_n(U32(a),&v,v|HANDLE_WAIT,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST)) heal_note("lost waiter flag",a,v);
      }
    }
    if(!changed) break;
  }
  pm_unlock(&g_klock); }

/* ============================================================ address arbitration */
static u32 svc_wait_addr(KThread* t, u64 addr, u32 type, i32 value, i64 timeout){
  if(addr&3) return R_BADADDR;
  pm_lock(&g_klock);
  i32* p=(i32*)GP(addr);
  i32 cur=__atomic_load_n(p,__ATOMIC_SEQ_CST);
  switch(type){
    case 0: if(cur>=value){ pm_unlock(&g_klock); return R_BADSTATE; } break;
    case 1: for(;;){ if(cur>=value){ pm_unlock(&g_klock); return R_BADSTATE; }
              if(__atomic_compare_exchange_n(p,&cur,cur-1,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST)) break; } break;
    case 2: if(cur!=value){ pm_unlock(&g_klock); return R_BADSTATE; } break;
    default: pm_unlock(&g_klock); return R_BADENUM;
  }
  if(timeout==0){ pm_unlock(&g_klock); return R_TIMEOUT; }
  t->wait_kind=4; t->wait_addr=addr; t->woken=0; wl_add(t);
  int ok=sleep_nl(t,timeout);
  u32 r= ok? t->wait_result : R_TIMEOUT;
  if(!ok && wl_has(t)) wl_del(t);
  t->wait_kind=0;
  pm_unlock(&g_klock); return r; }
static void wake_addr_nl(u64 addr, i32 count){
  int n=0; while(count<=0 || n<count){ KThread* w=wl_best(4,addr); if(!w) break; wl_del(w); w->wait_kind=0; wake(w,0); n++; } }
static u32 svc_signal_addr(u64 addr, u32 type, i32 value, i32 count){
  if(addr&3) return R_BADADDR;
  { static int said[3]; if(type<3 && !said[type]){ said[type]=1; LOG("kernel: SignalToAddress type %u in use\n",type); } }
  pm_lock(&g_klock);
  i32* p=(i32*)GP(addr);
  if(type==1){ i32 exp=value; if(!__atomic_compare_exchange_n(p,&exp,value+1,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST)){ pm_unlock(&g_klock); return R_BADSTATE; } }
  else if(type==2){
    int total=wl_count(4,addr); i32 nv;
    if(count<=0) nv= total>0? value-2 : value+1;
    else if(total>0){ /* same as hw kernel: look at up to count+1 waiters */
      int k=0; for(int i=1;i<total;i++){ if(k++>=count) break; }
      nv= k==0? value+1 : k<=count? value-1 : value; }
    else nv=value+1;
    i32 exp=value;
    if(nv!=value){ if(!__atomic_compare_exchange_n(p,&exp,nv,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST)){ pm_unlock(&g_klock); return R_BADSTATE; } }
    else if(__atomic_load_n(p,__ATOMIC_SEQ_CST)!=value){ pm_unlock(&g_klock); return R_BADSTATE; }
  } else if(type!=0){ pm_unlock(&g_klock); return R_BADENUM; }
  wake_addr_nl(addr,count);
  pm_unlock(&g_klock); return 0; }

/* ============================================================ WaitSynchronization */
static u32 svc_wait_sync(KThread* t, u64 hptr, u32 n, i64 timeout, u32* out_idx){
  if(n>64) return R_OUTOFRANGE;
  KObj* objs[64]; u32 hs[64];
  for(u32 i=0;i<n;i++) hs[i]=gr32(hptr+i*4);
  pm_lock(&g_klock);
  for(u32 i=0;i<n;i++){ objs[i]=h_get_nl(hs[i]); if(!objs[i] || !objs[i]->is_signaled){ pm_unlock(&g_klock); if(!objs[i]){ static u32 nw; if(nw++<20 || !(nw&0xFFFF)) WARN("WaitSynchronization: invalid handle %x (%u times)\n",hs[i],nw); } return R_BADHANDLE; } }
  if(t->cancel){ t->cancel=0; pm_unlock(&g_klock); return R_CANCELLED; }
  u64 end= timeout<0? 0 : plat_ns()+(u64)timeout;
  for(;;){
    for(u32 i=0;i<n;i++) if(objs[i]->is_signaled(objs[i])){ *out_idx=i; pm_unlock(&g_klock); return 0; }
    if(timeout==0){ pm_unlock(&g_klock); return R_TIMEOUT; }
    i64 rem=-1; if(timeout>0){ u64 now=plat_ns(); if(now>=end){ pm_unlock(&g_klock); return R_TIMEOUT; } rem=(i64)(end-now); }
    t->woken=0; t->wait_kind=1; t->wait_name=objs[0]->name; t->wait_timeout=timeout;
    for(u32 i=0;i<n;i++) waiter_add(objs[i],t);
    sleep_nl(t,rem);
    for(u32 i=0;i<n;i++) waiter_del(objs[i],t);
    t->wait_kind=0;
    if(t->cancel){ t->cancel=0; pm_unlock(&g_klock); return R_CANCELLED; }
  }
}

/* ============================================================ memory */
static u32 svc_query(u64 info, u64 addr){
  MemBlock b; mem_query(addr,&b);
  gw64(info,b.addr); gw64(info+8,b.size); gw32(info+16,b.state&0xFF); gw32(info+20,b.attr); gw32(info+24,b.perm&7);
  gw32(info+28,b.ipc); gw32(info+32,b.dev); gw32(info+36,0);
  return 0; }
static u32 svc_map_memory(u64 dst, u64 src, u64 size){
  if((dst|src|size)&0xFFF || !size) return R_BADADDR;
  int in_stack= dst>=STACK_BASE && dst+size<=STACK_BASE+STACK_SIZE;
  int in_alias= dst>=ALIAS_BASE && dst+size<=ALIAS_BASE+ALIAS_SIZE;
  if(!in_stack && !in_alias) return R_BADREGION;
  if(!mem_is_free(dst,size)) return R_BADMEMSTATE;
  if(mem_map(dst,size,in_stack? MS_STACK : MS_NORMAL,PERM_RW)) return R_OUTOFMEM;
  memcpy(GP(dst),GP(src),size);
  mem_set_attr(src,size,1,1);         /* locked */
  mem_set_state(src,size,0xFFFFFFFFu,PERM_NONE);
  return 0; }
static u32 svc_unmap_memory(u64 dst, u64 src, u64 size){
  if((dst|src|size)&0xFFF || !size) return R_BADADDR;
  memcpy(GP(src),GP(dst),size);
  mem_unmap(dst,size);
  mem_set_attr(src,size,1,0);
  mem_set_state(src,size,0xFFFFFFFFu,PERM_RW);
  return 0; }
static void shm_destroy(KObj* o){ KShmem* s=(KShmem*)o; if(!s->gaddr) free(s->host); free(o->waiters); free(s); }
KShmem* kshmem_new(u64 size, const char* name){
  KShmem* s=calloc(1,sizeof *s); s->o.type=KO_SHMEM; s->o.destroy=shm_destroy; s->o.name=name;
  s->size=(size+0xFFF)&~0xFFFull; s->host=calloc(1,s->size); return s; }
u8* kshmem_ptr(KShmem* s){ return s->gaddr? GP(s->gaddr) : s->host; }
static u32 svc_map_shm(u32 h, u64 addr, u64 size, u32 perm){
  KShmem* s=(KShmem*)h_get_type(h,KO_SHMEM); if(!s) return R_BADHANDLE;
  if((addr|size)&0xFFF || !size) return R_BADADDR;
  if(size<s->size) size=s->size;
  if(mem_map(addr,size,MS_SHARED,perm&3)) return R_BADMEMSTATE;
  memcpy(GP(addr),s->host,s->size);
  s->gaddr=addr;
  return 0; }
static u32 svc_unmap_shm(u32 h, u64 addr, u64 size){
  KShmem* s=(KShmem*)h_get_type(h,KO_SHMEM); if(!s) return R_BADHANDLE;
  if(s->gaddr==addr){ memcpy(s->host,GP(addr),s->size); s->gaddr=0; }
  mem_unmap(addr,size); return 0; }
static void tmem_destroy(KObj* o){ KTmem* t=(KTmem*)o; mem_set_state(t->addr,t->size,0xFFFFFFFFu,PERM_RW); free(o->waiters); free(o); }

/* ============================================================ GetInfo */
/* application memory size. Retail gives 3.2 GB; memory_gb can raise it (yuzu calls this extended memory).
   TotK defaults to 5.2 GB, 3.2 isn't enough for it and its allocator returns NULL during load. */
int g_opt_mem_gb;
static u64 mem_total(void){
  if(g_opt_mem_gb>=8) return 0x1CD500000ull;
  if(g_opt_mem_gb>=6) return 0x14D500000ull;
  if(g_opt_mem_gb>=4 || g_opt_mem_gb<0) return 0xCD500000ull;
  if(g_title_id==0x0100F2C0115B6000ull) return 0x14D500000ull;     /* Tears of the Kingdom */
  return 0xCD500000ull; }
/* svcMapPhysicalMemory pages count as used, like on hw */
static u64 g_phys_mapped;
static u64 used_memory(void){ return g_heap_size+g_phys_mapped+0x10000000ull; }
void mem_report(char* out, int n){ snprintf(out,n,"heap %llu MB, mapped physical memory %llu MB, used %llu MB of %llu MB",
  (unsigned long long)(g_heap_size>>20),(unsigned long long)(g_phys_mapped>>20),(unsigned long long)(used_memory()>>20),(unsigned long long)(mem_total()>>20)); }
static u32 svc_get_info_(u32 id0, u32 h, u64 id1, u64* out);
static u32 svc_get_info(u32 id0, u32 h, u64 id1, u64* out){
  u32 r=svc_get_info_(id0,h,id1,out);
  static u8 seen[64]; if(id0<64 && !seen[id0]){ seen[id0]=1; LOG("GetInfo %u/%llu -> %llx\n",id0,(unsigned long long)id1,(unsigned long long)*out); }
  return r; }
static u32 svc_get_info_(u32 id0, u32 h, u64 id1, u64* out){
  switch(id0){
    case 0: *out=0xF; return 0;                                  /* CoreMask */
    case 1: *out=0xFFFFFFFFFFFFFFFFull>>(64-60)<<4; *out=0xFFFFFFFFFFFFFFF0ull; return 0;
    case 2: *out=ALIAS_BASE; return 0;
    case 3: *out=ALIAS_SIZE; return 0;
    case 4: *out=HEAP_BASE; return 0;
    case 5: *out=HEAP_SIZE; return 0;
    case 6: *out=mem_total(); return 0;                          /* total app memory */
    case 7: *out=used_memory(); return 0;
    case 8: *out=0; return 0;
    case 9: *out=0; return 0;                                    /* resource limit handle 0 */
    case 10: *out=0; return 0;
    case 11: { u64 r=plat_ns()*0x9E3779B97F4A7C15ull^(id1*0xC2B2AE3D27D4EB4Full); *out=r; return 0; }
    case 12: *out=AS_BASE; return 0;
    case 13: *out=AS_SIZE-AS_BASE; return 0;
    case 14: *out=STACK_BASE; return 0;
    case 15: *out=STACK_SIZE; return 0;
    case 16: *out=g_sysres_size; return 0;                       /* SystemResourceSize (npdm) */
    case 17: *out=g_sysres_size? 0x80000ull : 0; return 0;
    case 18: *out=g_title_id; return 0;
    case 19: *out=0; return 0;
    case 20: { static u64 uectx; if(!uectx) uectx=kmem_alloc(0x1000,MS_TLS,PERM_RW); *out=uectx; return 0; }
    case 21: *out=mem_total(); return 0;
    case 22: *out=used_memory(); return 0;
    case 23: *out=1; return 0;
    case 24: { int live=0; for(int i=0;i<g_nthreads;i++) if(g_threads[i] && g_threads[i]->state!=2) live++; *out=(u64)(live<96? 96-live : 0); return 0; }  /* FreeThreadCount: Unity skips its job threads if this is 0 */
    case 0xF0000002: { KThread* t=kthread_cur(); (void)t; *out=plat_ns()*192/10000; return 0; }
    case 26: *out=1; return 0;
    case 27: case 28: *out=0; return 0;
    default: WARN("GetInfo %u/%llu not implemented\n",id0,(unsigned long long)id1); *out=0; return 0;
  }
  (void)h;
}

/* ============================================================ SVC */
/* nearest exported symbol via each module's dynsym */
typedef struct { u64 addr; u32 name_off; u32 mod; } GSym;
static GSym* g_gsyms; static int g_ngsyms; static u64 g_gstr[16];
static int gsym_cmp(const void* a, const void* b){ u64 x=((const GSym*)a)->addr, y=((const GSym*)b)->addr; return x<y? -1 : x>y; }
static void gsym_load(void){
  static int done; if(done) return; done=1;
  for(u32 m=0;m<g_mod->nmods && m<16;m++){ u64 base=g_mod->mods[m].base; if(!base) continue;
    MemBlock b; mem_query(base,&b); if(b.state==MS_FREE) continue;
    u64 mod0=base+gr32(base+4); if(gr32(mod0)!=0x30444F4D) continue;
    u64 dyn=mod0+(u64)(i64)(i32)gr32(mod0+4), symtab=0, strtab=0, hash=0;
    for(int i=0;i<256;i++){ u64 tag=gr64(dyn+i*16), val=gr64(dyn+i*16+8); if(!tag) break; if(tag==6) symtab=val; else if(tag==5) strtab=val; else if(tag==4) hash=val; }
    if(!symtab||!strtab) continue;
    u64 n= hash? gr32(base+hash+4) : (strtab>symtab? (strtab-symtab)/24 : 0); if(n>2000000) n=0;
    g_gstr[m]=base+strtab;
    g_gsyms=realloc(g_gsyms,(g_ngsyms+n)*sizeof(GSym));
    for(u64 i=1;i<n;i++){ u64 s=base+symtab+i*24; u64 v=gr64(s+8); u16 sh=*(u16*)GP(s+6); if(!v || !sh) continue;
      g_gsyms[g_ngsyms].addr=base+v; g_gsyms[g_ngsyms].name_off=gr32(s); g_gsyms[g_ngsyms].mod=m; g_ngsyms++; } }
  if(g_ngsyms) qsort(g_gsyms,g_ngsyms,sizeof(GSym),gsym_cmp);
}
static const char* gsym(u64 a){
  static char buf[4][200]; static int k; char* o=buf[k++&3]; o[0]=0;
  gsym_load(); int lo=0, hi=g_ngsyms-1, best=-1;
  while(lo<=hi){ int mid=(lo+hi)/2; if(g_gsyms[mid].addr<=a){ best=mid; lo=mid+1; } else hi=mid-1; }
  if(best<0 || a-g_gsyms[best].addr>0x20000) return o;
  snprintf(o,200," <%.150s+%llx>",(const char*)GP(g_gstr[g_gsyms[best].mod]+g_gsyms[best].name_off),(unsigned long long)(a-g_gsyms[best].addr));
  return o; }
static void guest_backtrace(CPU* c){
  WARN("  pc=%llx lr=%llx sp=%llx fp=%llx\n",(unsigned long long)c->pc,(unsigned long long)c->x[30],(unsigned long long)c->x[31],(unsigned long long)c->x[29]);
  WARN("  pc%s  lr%s  x16->%s  x17%s\n",gsym(c->pc),gsym(c->x[30]),c->x[16]>=0x10000 && c->x[16]<AS_SIZE-8? gsym(gr64(c->x[16])) : "",gsym(c->x[17]));
  u64 fp=c->x[29];
  for(int i=0;i<24 && fp>=AS_BASE && fp<AS_SIZE && !(fp&7);i++){
    MemBlock b; mem_query(fp,&b); if(b.state==MS_FREE || b.state==MS_INACCESSIBLE) break;
    u64 lr=gr64(fp+8); WARN("   #%d %llx%s\n",i,(unsigned long long)lr,gsym(lr)); fp=gr64(fp); }
}
/* imports rtld couldn't resolve (left at 0) */
static void dump_unresolved(void){
  int shown=0;
  for(u32 m=0;m<g_mod->nmods;m++){ u64 base=g_mod->mods[m].base; if(!base) continue;
    MemBlock b; mem_query(base,&b); if(b.state==MS_FREE) continue;
    u64 mod0=base+gr32(base+4); if(gr32(mod0)!=0x30444F4D) continue;
    u64 dyn=mod0+(u64)(i64)(i32)gr32(mod0+4);
    u64 symtab=0,strtab=0,jmprel=0,pltsz=0,rela=0,relasz=0;
    for(int i=0;i<256;i++){ u64 tag=gr64(dyn+i*16), val=gr64(dyn+i*16+8); if(!tag) break;
      if(tag==6) symtab=val; else if(tag==5) strtab=val; else if(tag==23) jmprel=val; else if(tag==2) pltsz=val; else if(tag==7) rela=val; else if(tag==8) relasz=val; }
    if(!symtab||!strtab) continue;
    u64 tabs[2][2]={{jmprel,pltsz},{rela,relasz}};
    for(int t=0;t<2;t++){ if(!tabs[t][0]) continue;
      for(u64 o=0;o+24<=tabs[t][1];o+=24){ u64 r=base+tabs[t][0]+o; u64 off=gr64(r), info=gr64(r+8); u32 ty=(u32)info, si=(u32)(info>>32);
        if(!si || (ty!=1026 && ty!=1025 && ty!=257)) continue;
        if(gr64(base+off)!=0) continue;
        u64 s=base+symtab+si*24ull; const char* nm=(const char*)GP(base+strtab+gr32(s)); u8 inf=*(u8*)GP(s+4);
        if(nm[0]=='_' && nm[1]=='_' && (inf>>4)==2) continue;          /* linker symbols */
        if(shown++<80) WARN("  unresolved in %s: %s%s (GOT %llx)\n",g_mod->mods[m].name,nm,(inf>>4)==2?" [weak]":"",(unsigned long long)(base+off)); } } }
  if(shown) WARN("  %d unresolved imports in total\n",shown);
}
void kernel_dump_regs(CPU* c){
  if(c->pc<0x1000 || getenv("SWR_UNRES")) dump_unresolved();
  for(int i=0;i<31;i+=4) WARN("  x%-2d=%016llx x%-2d=%016llx x%-2d=%016llx x%-2d=%016llx\n",i,(unsigned long long)c->x[i],i+1,(unsigned long long)c->x[i+1],i+2,(unsigned long long)c->x[i+2],i+3,(unsigned long long)(i+3<32?c->x[i+3]:0));
  guest_backtrace(c);
  /* code around lr */
  { u64 lr=c->x[30]&~3ull; MemBlock cb; mem_query(lr,&cb);
    if(cb.state!=MS_FREE && lr>0x100) for(u64 base=lr-0xC0; base<lr+0x40; base+=0x40){
      char line[400]; int n=snprintf(line,sizeof line,"  code %llx:",(unsigned long long)base);
      for(int w=0;w<16;w++) n+=snprintf(line+n,sizeof line-n," %08x",gr32(base+w*4));
      WARN("%s\n",line); } }
  /* what the regs point at (objects, vtables) */
  for(int r=0;r<29;r++){ u64 a=c->x[r]; if(a<0x10000 || a>=AS_SIZE-64 || (a&7)) continue; MemBlock b; mem_query(a,&b); if(b.state==MS_FREE || b.state==MS_INACCESSIBLE) continue;
    WARN("  mem x%d %llx: %llx %llx %llx %llx\n",r,(unsigned long long)a,(unsigned long long)gr64(a),(unsigned long long)gr64(a+8),(unsigned long long)gr64(a+16),(unsigned long long)gr64(a+24));
    u64 v=gr64(a); if(v>=0x10000 && v<AS_SIZE-64 && !(v&7)){ MemBlock vb; mem_query(v,&vb); if(vb.state!=MS_FREE && vb.state!=MS_INACCESSIBLE){
      { char sl[1200]; int n=0; for(int e=0;e<8;e++){ const char* s=gsym(gr64(v+e*8)); if(s[0]) n+=snprintf(sl+n,sizeof sl-n," [%d]%s",e,s); if(n>1000) break; } if(n) WARN("    vtable:%s\n",sl); }
      WARN("    -> %llx: %llx %llx %llx %llx %llx %llx %llx %llx\n",(unsigned long long)v,(unsigned long long)gr64(v),(unsigned long long)gr64(v+8),(unsigned long long)gr64(v+16),(unsigned long long)gr64(v+24),(unsigned long long)gr64(v+32),(unsigned long long)gr64(v+40),(unsigned long long)gr64(v+48),(unsigned long long)gr64(v+56)); } } }
  WARN("  last SVCs (num args -> result out1, called from):\n");
  for(u32 k=g_svc_ring_n>40? g_svc_ring_n-40 : 0; k<g_svc_ring_n; k++){ SvcRec* r=&g_svc_ring[k&63];
    WARN("    %02x %llx %llx %llx %llx -> %x %llx  lr=%llx\n",r->imm,(unsigned long long)r->a[0],(unsigned long long)r->a[1],(unsigned long long)r->a[2],(unsigned long long)r->a[3],r->res,(unsigned long long)r->o1,(unsigned long long)r->lr); }
  extern char g_ipc_ring[32][80]; extern u32 g_ipc_ring_n;
  WARN("  last IPC calls:\n");
  for(u32 k=g_ipc_ring_n>32? g_ipc_ring_n-32 : 0; k<g_ipc_ring_n; k++) WARN("    %s\n",g_ipc_ring[k&31]);
  /* code around the last svcs, usually the check that failed before an abort */
  { u64 seen[6]; int ns=0;
    for(u32 k=g_svc_ring_n; k>0 && k+12>g_svc_ring_n && ns<4; k--){ u64 lr=g_svc_ring[(k-1)&63].lr; int dup=0; for(int j=0;j<ns;j++) if(seen[j]==lr) dup=1; if(dup) continue; seen[ns++]=lr;
      for(u64 base=lr-0x100; base<lr+0x100; base+=0x40){ MemBlock cb; mem_query(base,&cb); if(cb.state==MS_FREE) break;
        char line[400]; int n=snprintf(line,sizeof line,"  code %llx:",(unsigned long long)base);
        for(int w=0;w<16;w++) n+=snprintf(line+n,sizeof line-n," %08x",gr32(base+w*4));
        WARN("%s\n",line); } } }
  /* and around the callers */
  u64 fp=c->x[29];
  for(int i=0;i<8 && fp>=AS_BASE && fp<AS_SIZE && !(fp&7);i++){
    MemBlock b; mem_query(fp,&b); if(b.state==MS_FREE || b.state==MS_INACCESSIBLE) break;
    u64 lr=gr64(fp+8); fp=gr64(fp); MemBlock cb; mem_query(lr-0x60,&cb); if(cb.state==MS_FREE) continue;
    for(u64 base=lr-0x140; base<lr+0x40; base+=0x40){
      char line[400]; int n=snprintf(line,sizeof line,"  code %llx:",(unsigned long long)base);
      for(int w=0;w<16;w++) n+=snprintf(line+n,sizeof line-n," %08x",gr32(base+w*4));
      WARN("%s\n",line); } }
}

extern KShmem* kshmem_new(u64 size, const char* name);

static void swr_svc_(CPU* c, u32 imm);
void swr_svc(CPU* c, u32 imm){
  u64 a0=c->x[0],a1=c->x[1],a2=c->x[2];
  u64 a3=c->x[3];
  swr_svc_(c,imm);
  if(imm!=0x1E && imm!=0x0B && imm!=0x10){ u32 i=__atomic_fetch_add(&g_svc_ring_n,1,__ATOMIC_RELAXED)&63; SvcRec* r=&g_svc_ring[i];
    r->imm=imm; r->lr=c->x[30]; r->a[0]=a0; r->a[1]=a1; r->a[2]=a2; r->a[3]=a3; r->res=(u32)c->x[0]; r->o1=c->x[1]; }
  static u32 nbad; if((u32)c->x[0]==R_BADHANDLE && imm!=0x16 && (nbad++<20 || !(nbad&0xFFFF))) WARN("SVC 0x%x with invalid handle (args %llx %llx %llx) pc=%llx\n",imm,(unsigned long long)a0,(unsigned long long)a1,(unsigned long long)a2,(unsigned long long)c->pc);
}
static void swr_svc_(CPU* c, u32 imm){
  KThread* t=kthread_cur();
  u64* x=c->x;
  switch(imm){
  case 0x01: { u64 a=mem_heap_set(x[1]); if(!a && x[1]){ x[0]=R_OUTOFMEM; return; } x[0]=0; x[1]=HEAP_BASE; return; }
  case 0x02: mem_set_state(x[0],x[1],0xFFFFFFFFu,(u32)x[2]); x[0]=0; return;
  case 0x03: mem_set_attr(x[0],x[1],(u32)x[2],(u32)x[3]); x[0]=0; return;
  case 0x04: x[0]=svc_map_memory(x[0],x[1],x[2]); return;
  case 0x05: x[0]=svc_unmap_memory(x[0],x[1],x[2]); return;
  case 0x06: x[0]=svc_query(x[0],x[2]); x[1]=0; return;
  case 0x07: process_exit(0); return;
  case 0x08: {
    int prio=(int)(u32)x[4], core=(int)(i32)(u32)x[5];
    if(core==-2) core=0;
    KThread* n=kthread_create(x[1],x[2],x[3],prio,core);
    u32 h=h_new(&n->o); x[0]=0; x[1]=h;
    LOGV("CreateThread entry=%llx stack=%llx prio=%d core=%d -> %x\n",(unsigned long long)x[1],(unsigned long long)n->stack_top,prio,core,h);
    return; }
  case 0x09: { KThread* n=(KThread*)h_get_type((u32)x[0],KO_THREAD); if(!n){ x[0]=R_BADHANDLE; return; }
    if(n->state==0) kthread_start(n); x[0]=0; return; }
  case 0x0A: svc_exit_thread(t); return;
  case 0x0B: { i64 ns=(i64)x[0]; t->sleep_lr=x[30]; t->sleep_n++; memcpy(t->sleep_x,x,sizeof t->sleep_x);
    if(ns<=0) plat_yield(); else plat_sleep_ns(ns); return; }
  case 0x0C: { KThread* n=(KThread*)h_get_type((u32)x[1],KO_THREAD); if(!n){ x[0]=R_BADHANDLE; return; } x[0]=0; x[1]=(u32)n->prio; return; }
  case 0x0D: { KThread* n=(KThread*)h_get_type((u32)x[0],KO_THREAD); if(!n){ x[0]=R_BADHANDLE; return; } n->prio=(int)(u32)x[1]; x[0]=0; return; }
  case 0x0E: { KThread* n=(KThread*)h_get_type((u32)x[2],KO_THREAD); if(!n){ x[0]=R_BADHANDLE; return; } x[0]=0; x[1]=(u32)n->core; x[2]=n->affinity; return; }
  case 0x0F: { KThread* n=(KThread*)h_get_type((u32)x[0],KO_THREAD); if(!n){ x[0]=R_BADHANDLE; return; }
    i32 core=(i32)(u32)x[1]; if(core>=0) n->core=core; if(x[2]) n->affinity=x[2]; x[0]=0; return; }
  case 0x10: x[0]=(u32)(t? t->core : 0); return;
  case 0x11: { KEvent* e=(KEvent*)h_get_type((u32)x[0],KO_EVENT); if(!e){ x[0]=R_BADHANDLE; return; } kevent_signal(e); x[0]=0; return; }
  case 0x12: case 0x17: { KObj* o=h_get((u32)x[0]); if(!o){ x[0]=R_BADHANDLE; return; }
    if(o->type==KO_EVENT){ KEvent* e=(KEvent*)o; pm_lock(&g_klock); int was=e->signaled; e->signaled=0; pm_unlock(&g_klock);
      x[0]= (imm==0x17 && !was)? R_BADSTATE : 0; return; }
    x[0]=0; return; }
  case 0x13: x[0]=svc_map_shm((u32)x[0],x[1],x[2],(u32)x[3]); return;
  case 0x14: x[0]=svc_unmap_shm((u32)x[0],x[1],x[2]); return;
  case 0x15: { KTmem* m=calloc(1,sizeof *m); m->o.type=KO_TMEM; m->o.destroy=tmem_destroy; m->o.name="tmem"; m->addr=x[1]; m->size=x[2]; m->perm=(u32)x[3];
    x[0]=0; x[1]=h_new(&m->o); return; }
  case 0x16: x[0]=((u32)x[0]==H_CUR_THREAD||(u32)x[0]==H_CUR_PROC)? 0 : (u32)h_close((u32)x[0]); return;
  case 0x18: { u32 idx=0; u32 r=svc_wait_sync(t,x[1],(u32)x[2],(i64)x[3],&idx); x[0]=r; x[1]=idx; return; }
  case 0x19: { KThread* n=(KThread*)h_get_type((u32)x[0],KO_THREAD); if(!n){ x[0]=R_BADHANDLE; return; }
    pm_lock(&g_klock); if(n->wait_kind==1) wake(n,R_CANCELLED); n->cancel=1; pm_unlock(&g_klock); x[0]=0; return; }
  case 0x1A: x[0]=svc_arbitrate_lock(t,(u32)x[0],x[1],(u32)x[2]); return;
  case 0x1B: x[0]=svc_arbitrate_unlock(x[0]); return;
  case 0x1C: x[0]=svc_wait_cv(t,x[0],x[1],(u32)x[2],(i64)x[3]); return;
  case 0x1D: x[0]=svc_signal_cv(x[0],(i32)(u32)x[1]); return;
  case 0x1E: x[0]=c->rt->ticks(); return;
  case 0x1F: { char name[16]={0}; memcpy(name,GP(x[1]),12);
    if(!strcmp(name,"sm:")){ SvcObj* o=svc_open("sm:"); x[0]=0; x[1]=session_new(o); return; }
    WARN("ConnectToNamedPort(%s) unknown\n",name); x[0]=R_NOTFOUND; return; }
  case 0x21: ipc_handle(t,(u32)x[0],t->tls); return;
  case 0x22: ipc_handle(t,(u32)x[2],x[0]); return;
  case 0x24: x[0]=0; x[1]=0x51; return;
  case 0x25: { KObj* o=h_get((u32)x[1]); KThread* n=(o&&o->type==KO_THREAD)? (KThread*)o : t; x[0]=0; x[1]=n->id; return; }
  case 0x26: {
    u32 reason=(u32)x[0];
    if(reason&0x80000000u){ LOGV("svcBreak (notice) %x\n",reason); x[0]=0; return; }
    WARN("\n*** The game called svcBreak (reason=%x, info=%llx size=%llx) ***\n",reason,(unsigned long long)x[1],(unsigned long long)x[2]);
    if(x[1] && x[2]>=4 && x[2]<0x1000){ u8 tmp[64]; memcpy(tmp,GP(x[1]),x[2]<64?x[2]:64); WARN("  data: "); for(u64 i=0;i<(x[2]<64?x[2]:64);i++) WARN("%02x",tmp[i]); WARN("\n"); }
    kernel_dump_regs(c);
    /* strings pointed to by regs, often the abort message */
    for(int r=0;r<31;r++){ u64 a=c->x[r]; if(a<0x10000 || a>=AS_SIZE-256) continue; MemBlock b; mem_query(a,&b); if(b.state==MS_FREE) continue;
      const char* p=(const char*)GP(a); int n=0; while(n<200 && p[n]>=32 && p[n]<127) n++;
      if(n>=4 && (p[n]==0 || n==200)) WARN("  x%d -> \"%.*s\"\n",r,n,p); }
    fatal("The game stopped (svcBreak). See the .log for details.");
    return; }
  case 0x27: { u64 n=x[1]; if(n>4096) n=4096; char buf[4100]; memcpy(buf,GP(x[0]),n); buf[n]=0; LOG("[game] %s%s",buf,(n&&buf[n-1]=='\n')?"":"\n"); x[0]=0; return; }
  case 0x29: { u64 o=0; x[0]=svc_get_info((u32)x[1],(u32)x[2],x[3],&o); x[1]=o; return; }
  case 0x2A: case 0x2B: case 0x5D: case 0x5E: case 0x5F: case 0x36: x[0]=0; return;
  case 0x2C: { u64 a=x[0], s=x[1]; if((a|s)&0xFFF){ x[0]=R_BADADDR; return; }
    /* only map the holes */
    for(u64 p=a;p<a+s;){ MemBlock b; mem_query(p,&b); u64 e=b.addr+b.size; if(e>a+s) e=a+s;
      if(b.state==MS_FREE){ mem_map(p,e-p,MS_NORMAL,PERM_RW); g_phys_mapped+=e-p; } p=e; }
    x[0]=0; return; }
  case 0x2D: { u64 a=x[0], s=x[1];
    for(u64 p=a;p<a+s;){ MemBlock b; mem_query(p,&b); u64 e=b.addr+b.size; if(e>a+s) e=a+s; if(e<=p) break;
      if(b.state==MS_NORMAL){ u64 n=e-p; g_phys_mapped= g_phys_mapped>n? g_phys_mapped-n : 0; } p=e; }
    mem_unmap(a,s); x[0]=0; return; }
  case 0x2E: x[0]=R_NOTFOUND; return;
  case 0x2F: x[0]=R_NOTFOUND; return;
  case 0x30: case 0x37: { u32 which=(u32)x[2]; x[0]=0; x[1]= which==0? mem_total() : which==1? 800 : which==2? 800 : which==3? 256 : 64; return; }
  case 0x31: { u32 which=(u32)x[2]; x[0]=0; x[1]= which==0? used_memory() : which==1? (u64)g_nthreads : 16; return; }
  case 0x32: x[0]=0; return;
  case 0x33: { KThread* n=(KThread*)h_get_type((u32)x[1],KO_THREAD); if(!n){ x[0]=R_BADHANDLE; return; }
    u8* p=GP(x[0]); memset(p,0,0x320); memcpy(p,n->cpu.x,29*8); memcpy(p+29*8,&n->cpu.x[29],8); memcpy(p+30*8,&n->cpu.x[30],8);
    memcpy(p+31*8,&n->cpu.x[31],8); memcpy(p+32*8,&n->cpu.pc,8); memcpy(p+0x110,n->cpu.v,0x200); x[0]=0; return; }
  case 0x34: x[0]=svc_wait_addr(t,x[0],(u32)x[1],(i32)(u32)x[2],(i64)x[3]); return;
  case 0x35: x[0]=svc_signal_addr(x[0],(u32)x[1],(i32)(u32)x[2],(i32)(u32)x[3]); return;
  case 0x45: { KEvent* e=kevent_new("user event",0); u32 w=h_new(&e->o), r=h_new(&e->o); x[0]=0; x[1]=w; x[2]=r; return; }
  case 0x4B: { x[0]=R_NOTIMPL; return; }
  case 0x6F: { /* GetSystemInfo(type, handle, subtype). newer SDKs abort on boot if this returns 0 */
    u32 ty=(u32)x[1]; u64 sub=x[3], o=0;
    if(ty==0) o= sub==0? mem_total() : sub==1? 0x1FB00000ull : sub==2? 0x29D00000ull : 0x6000000ull;   /* pool physical size */
    else if(ty==1) o= sub==0? used_memory() : 0x1000000ull;                                            /* pool used */
    else if(ty==2) o= sub==0? 1 : 0x50;                                                                  /* initial process id range */
    x[0]=0; x[1]=o; return; }
  default:
    WARN("SVC 0x%x not implemented (pc=%llx)\n",imm,(unsigned long long)c->pc);
    x[0]=R_NOTIMPL; return;
  }
}

/* dump code around an address */
static void kernel_dump_code(u64 ra){
  u64 a=(ra&~3ull)-0x180; MemBlock b; mem_query(ra,&b); if(b.state==MS_FREE || b.state==MS_INACCESSIBLE || !(b.perm&1)) return;
  if(a<b.addr) a=b.addr;
  LOG("    code %llx:",(unsigned long long)a);
  for(int i=0;i<0x70 && a+i*4<b.addr+b.size;i++) LOG("%s%08x",(i%16==0)?"\n     ":" ",gr32(a+i*4));
  LOG("\n"); }
/* threads.txt: per-thread state, used when the screen stops changing */
void kernel_dump_threads(void){
  static const char* kinds[]={"running/in svc","objects","mutex","condvar","address"};
  LOG("---- game thread state ----\n");
  for(int i=0;i<g_nthreads;i++){ KThread* t=g_threads[i]; if(!t || t->state==2) continue;
    int k=t->wait_kind; if(k<0||k>4) k=0;
    LOG("thread %llu '%s' prio %d: %s", (unsigned long long)t->id, t->name, t->prio, kinds[k]);
    if(k==1) LOG(" [%s%s]", t->wait_name? t->wait_name : "?", t->wait_timeout<0? "" : " with timeout");
    if(k>=2) LOG(" [addr %llx value %x]", (unsigned long long)t->wait_addr, (k==2||k==3)? gr32(t->wait_addr) : 0u);
    LOG("  lr=%llx pc=%llx sp=%llx\n",(unsigned long long)t->cpu.x[30],(unsigned long long)t->cpu.pc,(unsigned long long)t->cpu.x[31]);
    { u64 fp=t->cpu.x[29]; LOG("    stack:");
      for(int i=0;i<10 && fp>=AS_BASE && fp<AS_SIZE && !(fp&7);i++){ MemBlock b; mem_query(fp,&b); if(b.state==MS_FREE || b.state==MS_INACCESSIBLE) break;
        LOG(" %llx",(unsigned long long)gr64(fp+8)); fp=gr64(fp); }
      LOG("\n"); }
  }
  /* threads spinning on SleepThread: dump regs + nearby code */
  { {
    for(int i=0;i<g_nthreads;i++){ KThread* t=g_threads[i]; if(!t || t->state==2 || t->wait_kind!=0 || !t->sleep_n) continue;
      LOG("thread %llu looping on SleepThread (%llu times) from lr=%llx\n    regs:",(unsigned long long)t->id,(unsigned long long)t->sleep_n,(unsigned long long)t->sleep_lr);
      for(int r=0;r<31;r++) LOG("%s x%d=%llx",(r%6==0)?"\n     ":"",r,(unsigned long long)t->sleep_x[r]);
      LOG("\n");
      for(int r=0;r<31;r++){ u64 a=t->sleep_x[r]&~7ull; MemBlock b; if(a<AS_BASE || a>=AS_SIZE) continue; mem_query(a,&b);
        if(b.state==MS_FREE || b.state==MS_INACCESSIBLE || !(b.perm&1)) continue;
        LOG("    mem x%d [%llx]:",r,(unsigned long long)a); for(int j=0;j<8;j++) LOG(" %llx",(unsigned long long)gr64(a+j*8)); LOG("\n"); }
      u64 fp=t->cpu.x[29], ras[4]; int nra=0; ras[nra++]=t->sleep_lr;
      for(int j=0;j<3 && fp>=AS_BASE && fp<AS_SIZE && !(fp&7);j++){ MemBlock b; mem_query(fp,&b); if(b.state==MS_FREE || b.state==MS_INACCESSIBLE) break; ras[nra++]=gr64(fp+8); fp=gr64(fp); }
      for(int j=0;j<nra;j++) kernel_dump_code(ras[j]); }
    KThread* m=g_main; if(m){ u64 fp=m->cpu.x[29]; for(int j=0;j<4 && fp>=AS_BASE && fp<AS_SIZE && !(fp&7);j++){ MemBlock b; mem_query(fp,&b); if(b.state==MS_FREE || b.state==MS_INACCESSIBLE) break; kernel_dump_code(gr64(fp+8)); fp=gr64(fp); } } } }
  { { extern char g_ipc_ring[32][80]; extern u32 g_ipc_ring_n;
      LOG("  last SVCs:\n");
      for(u32 q=g_svc_ring_n>40? g_svc_ring_n-40 : 0; q<g_svc_ring_n; q++){ SvcRec* r=&g_svc_ring[q&63];
        LOG("    %02x %llx %llx %llx %llx -> %x %llx  lr=%llx\n",r->imm,(unsigned long long)r->a[0],(unsigned long long)r->a[1],(unsigned long long)r->a[2],(unsigned long long)r->a[3],r->res,(unsigned long long)r->o1,(unsigned long long)r->lr); }
      LOG("  last IPCs:\n");
      for(u32 q=g_ipc_ring_n>32? g_ipc_ring_n-32 : 0; q<g_ipc_ring_n; q++) LOG("    %s\n",g_ipc_ring[q&31]); } }
  LOG("---------------------------------------\n"); fflush(NULL);
}
