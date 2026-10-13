#!/usr/bin/env python3
"""Execute actual PS5 alert-owner helpers against host kqueue, not Wine handles."""
from pathlib import Path
import subprocess,sys,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class AlertKqueue(unittest.TestCase):
 def test_live_owner_descriptor_lifecycle(self):
  if sys.platform!='darwin':self.skipTest('real host kqueue requires macOS/FreeBSD')
  patch=(ROOT/'wine/patches/0964-ntdll-ps5-retire-thread-alert-kqueues.patch').read_text()
  start=patch.index('+/* The slot indexes a live owner')
  end=patch.index('+#else\n static union tid_alert_entry *get_tid_alert_entry',start)
  helpers='\n'.join(line[1:] for line in patch[start:end].splitlines() if line.startswith('+'))
  source=r'''
#include <sys/types.h>
#include <sys/event.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <signal.h>
#include <assert.h>
#include <fcntl.h>
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define ERR(...) ((void)0)
#define TRUE 1
#define FALSE 0
#define STATUS_INVALID_CID 10
#define STATUS_INVALID_HANDLE 11
#define STATUS_SUCCESS 0
#define ULongToHandle(t) ((void*)(uintptr_t)(t))
typedef void *HANDLE;typedef int BOOL;typedef int NTSTATUS;
struct thread_data {unsigned tid;int tid_alert_kqueue;};
union tid_alert_entry {struct thread_data *owner;};
#define TID_ALERT_BLOCK_SIZE (65536/sizeof(union tid_alert_entry))
static union tid_alert_entry *tid_alert_blocks[4096];
static sigset_t server_block_set;
static unsigned handle_to_index(HANDLE h,unsigned *block){unsigned i=((unsigned)(uintptr_t)h>>2)-1;*block=i/TID_ALERT_BLOCK_SIZE;return i%TID_ALERT_BLOCK_SIZE;}
static int fail_mapping;
static void *anon_mmap_alloc(size_t n,int p){if(fail_mapping){errno=ENOMEM;return MAP_FAILED;}return mmap(NULL,n,p,MAP_ANON|MAP_PRIVATE,-1,0);}
static unsigned opened,closed;static int fail_init,fail_allocate,fire_signal;static volatile sig_atomic_t signal_seen;
static void termination_probe(int sig){(void)sig;++signal_seen;}
static int counted_kqueue(void){if(fire_signal){assert(!pthread_kill(pthread_self(),SIGUSR1));assert(!signal_seen);}if(fail_allocate){errno=EMFILE;return -1;}int fd=kqueue();if(fd>=0)++opened;return fd;}
static int counted_close(int fd){int r=close(fd);if(!r)++closed;return r;}
static int checked_kevent(int fd,const struct kevent *changes,int n,struct kevent *events,int count,const struct timespec *t){
 if(fail_init && n && changes[0].flags&EV_ADD){errno=EINVAL;return -1;}
 return kevent(fd,changes,n,events,count,t);
}
#define kqueue counted_kqueue
#define close counted_close
#define kevent(...) checked_kevent(__VA_ARGS__)
'''+helpers+r'''
static void await_alert(struct thread_data *data){
 int fd=ps5_thread_alert_kqueue(data);assert(fd>=0);struct kevent ev;struct timespec timeout={2,0};
 assert(kevent(fd,NULL,0,&ev,1,&timeout)==1 && ev.ident==1);
}
static void *waiter(void *arg){struct thread_data *d=arg;await_alert(d);ps5_free_thread_alert(d);return NULL;}
static void *alerter(void *arg){unsigned tid=*(unsigned*)arg;for(unsigned i=0;i<10000;++i){int s=ps5_alert_thread(ULongToHandle(tid));assert(s==STATUS_SUCCESS||s==STATUS_INVALID_CID);}return NULL;}
int main(void){
 sigemptyset(&server_block_set);sigaddset(&server_block_set,SIGUSR1);struct rlimit limit={128,128};assert(!setrlimit(RLIMIT_NOFILE,&limit));
 for(unsigned i=0;i<1000;++i){struct thread_data d={4*(i+1),0};ps5_init_thread_alert(&d);pthread_t worker;assert(!pthread_create(&worker,NULL,waiter,&d));assert(ps5_alert_thread(ULongToHandle(d.tid))==STATUS_SUCCESS);pthread_join(worker,NULL);ps5_free_thread_alert(&d);assert(!d.tid_alert_kqueue && opened==closed);assert(ps5_alert_thread(ULongToHandle(d.tid))==STATUS_INVALID_CID);}
 unsigned mappings_opened=opened;struct thread_data unmapped={4*(TID_ALERT_BLOCK_SIZE+1),0};fail_mapping=1;ps5_init_thread_alert(&unmapped);assert(ps5_thread_alert_kqueue(&unmapped)==-1 && opened==mappings_opened);assert(ps5_alert_thread(ULongToHandle(unmapped.tid))==STATUS_INVALID_CID);fail_mapping=0;ps5_init_thread_alert(&unmapped);assert(ps5_alert_thread(ULongToHandle(unmapped.tid))==STATUS_SUCCESS);await_alert(&unmapped);ps5_free_thread_alert(&unmapped);
 struct sigaction action={0};action.sa_handler=termination_probe;sigemptyset(&action.sa_mask);assert(!sigaction(SIGUSR1,&action,NULL));struct thread_data signaled={4024,0};ps5_init_thread_alert(&signaled);fire_signal=1;assert(ps5_thread_alert_kqueue(&signaled)>=0 && signal_seen==1);fire_signal=0;ps5_free_thread_alert(&signaled);
 struct thread_data bad={4008,0};ps5_init_thread_alert(&bad);fail_init=1;assert(ps5_thread_alert_kqueue(&bad)==-1 && !bad.tid_alert_kqueue && opened==closed);fail_init=0;fail_allocate=1;assert(ps5_thread_alert_kqueue(&bad)==-1 && !bad.tid_alert_kqueue);fail_allocate=0;ps5_free_thread_alert(&bad);
 struct thread_data old={4012,0},fresh={4012,0};ps5_init_thread_alert(&old);assert(ps5_alert_thread(ULongToHandle(old.tid))==STATUS_SUCCESS);int oldfd=old.tid_alert_kqueue-1;ps5_init_thread_alert(&fresh);assert(ps5_alert_thread(ULongToHandle(fresh.tid))==STATUS_SUCCESS);int newfd=fresh.tid_alert_kqueue-1;assert(oldfd!=newfd);ps5_free_thread_alert(&old);assert(fcntl(newfd,F_GETFD)>=0);await_alert(&fresh);ps5_free_thread_alert(&fresh);
 struct thread_data concurrent={4016,0};ps5_init_thread_alert(&concurrent);pthread_t a,b;assert(!pthread_create(&a,NULL,waiter,&concurrent));assert(!pthread_create(&b,NULL,alerter,&concurrent.tid));pthread_join(a,NULL);pthread_join(b,NULL);assert(opened==closed);
 int saved=dup(0);assert(saved>=0);assert(!close(0));unsigned before_closed=closed;struct thread_data zero={4020,0};ps5_init_thread_alert(&zero);assert(ps5_thread_alert_kqueue(&zero)==0);assert(ps5_alert_thread(ULongToHandle(zero.tid))==STATUS_SUCCESS);await_alert(&zero);ps5_free_thread_alert(&zero);assert(closed==before_closed+1);assert(dup2(saved,0)==0);assert(!close(saved));
 assert(!ps5_tid_alert_slot(ULongToHandle(0),FALSE));
 printf("PASS 1000 real pthread lifecycles; signal deferral; registration OOM; init/allocation failure; late/dead alerts; TID generation reuse; concurrent alert/exit; fd0; no descriptor growth\n");return 0;
}
'''
  with tempfile.TemporaryDirectory() as d:
   p=Path(d);(p/'alert.c').write_text(source)
   subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread',str(p/'alert.c'),'-o',str(p/'alert')],check=True)
   subprocess.run([str(p/'alert')],check=True,timeout=20)
if __name__=='__main__':unittest.main()
