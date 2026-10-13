#!/usr/bin/env python3
"""Check title-safe media timing/polling without invoking payload syscalls."""
from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class Adapter(unittest.TestCase):
 def test_deadlines_polling_and_close_range(self):
  with tempfile.TemporaryDirectory() as d:
   p=Path(d)
   (p/'test.c').write_text(r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#ifndef TIMER_ABSTIME
#define TIMER_ABSTIME 1
#endif
static struct timespec now={10,900000000},sleep_request;
static int interrupt_sleep,poll_timeout,closed;
static int fake_clock_gettime(clockid_t id,struct timespec *ts) { (void)id; *ts=now; return 0; }
static int fake_nanosleep(const struct timespec *req,struct timespec *rem) {
 sleep_request=*req;
 if(interrupt_sleep) { if(rem) {rem->tv_sec=0; rem->tv_nsec=5;} errno=EINTR; return -1; }
 now.tv_sec+=req->tv_sec;now.tv_nsec+=req->tv_nsec;
 if(now.tv_nsec>=1000000000) {++now.tv_sec;now.tv_nsec-=1000000000;} return 0;
}
static int fake_poll(struct pollfd *fd,nfds_t n,int timeout) {(void)fd;(void)n;poll_timeout=timeout;return 7;}
static int fake_getrlimit(int resource,struct rlimit *limit) {(void)resource;limit->rlim_cur=8;limit->rlim_max=8;return 0;}
static int fake_close(int fd) {assert(fd==3+closed);++closed;errno=EBADF;return -1;}
static ssize_t __wrap_readlink(const char *path,char *buffer,size_t count) {(void)path;(void)count;buffer[0]='x';return 1;}
#define fork pw_test_fork
#define readlink pw_test_readlink
#define clock_gettime fake_clock_gettime
#define nanosleep fake_nanosleep
#define poll fake_poll
#define getrlimit fake_getrlimit
#define close fake_close
#define clock_nanosleep pw_test_clock_nanosleep
#define ppoll pw_test_ppoll
#define closefrom pw_test_closefrom
#include "pw_media_prx_libc.c"
int main(void) {
 struct timespec req={11,100000000},rem={17,19}; sigset_t mask;
 assert(clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&req,&rem)==0);
 assert(sleep_request.tv_sec==0 && sleep_request.tv_nsec==200000000);
 assert(rem.tv_sec==17 && rem.tv_nsec==19);
 req.tv_sec=9;assert(clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&req,&rem)==0);
 req.tv_sec=0;req.tv_nsec=-1;assert(clock_nanosleep(CLOCK_MONOTONIC,0,&req,&rem)==EINVAL);
 req.tv_nsec=1000000000;assert(clock_nanosleep(CLOCK_MONOTONIC,0,&req,&rem)==EINVAL);
 req.tv_nsec=100;interrupt_sleep=1;assert(clock_nanosleep(CLOCK_MONOTONIC,0,&req,&rem)==EINTR);assert(rem.tv_nsec==5);
 assert(clock_nanosleep(CLOCK_MONOTONIC,8,&req,&rem)==EINVAL);
 req.tv_nsec=1;assert(ppoll(0,0,&req,0)==7 && poll_timeout==1);
 assert(ppoll(0,0,0,0)==7 && poll_timeout==-1);
 req.tv_nsec=-1;assert(ppoll(0,0,&req,0)==-1 && errno==EINVAL);
 sigemptyset(&mask);assert(ppoll(0,0,0,&mask)==-1 && errno==ENOTSUP);
 closefrom(3);assert(closed==5);
 assert(fork()==-1 && errno==ENOSYS);
 char b=0;assert(readlink("virtual",&b,1)==1 && b=='x');
 return 0;
}
''')
   r=subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I'+str(ROOT/'wine/ps5'),str(p/'test.c'),'-o',str(p/'test')],capture_output=True,text=True)
   self.assertEqual(r.returncode,0,r.stderr)
   r=subprocess.run([str(p/'test')],capture_output=True,text=True);self.assertEqual(r.returncode,0,r.stderr)
if __name__=='__main__':unittest.main()
