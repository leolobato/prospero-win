#!/usr/bin/env python3
"""Exercise the probe's real class64 enumeration/retry/calibration code."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
class HandleCounter(unittest.TestCase):
 def test_enumeration_bounds_retry_and_calibration(self):
  probe=(ROOT/'tools/pw_mf_decode_probe.c').read_text()
  code=probe[probe.index('struct probe_handle_entry'):probe.index('static BOOL resources')]
  prelude=r'''
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <wchar.h>
#include <assert.h>
#define BOOL int
#define TRUE 1
#define FALSE 0
#define NTAPI
#define NTSTATUS int32_t
#define SYSTEM_INFORMATION_CLASS int
#define PVOID void *
#define PULONG uint32_t *
typedef uint32_t DWORD,ULONG;
typedef uint16_t USHORT;
typedef uintptr_t ULONG_PTR;
typedef void *HANDLE,*HMODULE;
typedef void (*FARPROC)(void);
static unsigned mode,queries,events;
static HMODULE GetModuleHandleW(const wchar_t *name);
static FARPROC GetProcAddress(HMODULE module,const char *name);
static DWORD GetCurrentProcessId(void);
static HANDLE CreateEventW(void *security,int manual,int state,const wchar_t *name);
static int CloseHandle(HANDLE event);
'''
  mocks=r'''
static NTSTATUS fake_query(SYSTEM_INFORMATION_CLASS cls,PVOID buffer,ULONG size,PULONG needed) {
 struct probe_handles *info=buffer;
 assert(cls==64);++queries;
 if(mode==1 && size==65536){*needed=131072;return (NTSTATUS)0xc0000004;}
 if(mode==2){*needed=70*1024*1024;return (NTSTATUS)0xc0000004;}
 if(mode==3){*needed=1;return 0;}
 if(mode==4)return (NTSTATUS)0xc0000022;
 unsigned count=4+events;
 *needed=offsetof(struct probe_handles,entries)+count*sizeof(info->entries[0]);
 info->count=mode==5?count+1:count;
 for(unsigned i=0;i<count;++i)info->entries[i].pid=(i==1 || i==3)?88:77;
 return 0;
}
static HMODULE GetModuleHandleW(const wchar_t *name) {(void)name;return (void *)1;}
static FARPROC GetProcAddress(HMODULE module,const char *name) {
 FARPROC address;NTSTATUS (*fn)(SYSTEM_INFORMATION_CLASS,PVOID,ULONG,PULONG)=fake_query;
 (void)module;(void)name;memcpy(&address,&fn,sizeof(address));return address;
}
static DWORD GetCurrentProcessId(void){return 77;}
static HANDLE CreateEventW(void *security,int manual,int state,const wchar_t *name) {
 (void)security;(void)manual;(void)state;(void)name;return (void *)(uintptr_t)++events;
}
static int CloseHandle(HANDLE event){assert(event);--events;return 1;}
int main(void) {
 DWORD count=999;
 assert(process_handles(&count) && count==2);
 mode=1;queries=0;assert(process_handles(&count) && count==2 && queries==2);
 for(mode=2;mode<=5;++mode){count=999;assert(!process_handles(&count) && count==999);}
 mode=0;assert(calibrate_handles() && !events);
 mode=5;assert(!calibrate_handles() && !events);
 return 0;
}
'''
  with tempfile.TemporaryDirectory() as d:
   p=Path(d);(p/'handles.c').write_text(prelude+code+mocks)
   subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(p/'handles.c'),'-o',str(p/'handles')],check=True)
   subprocess.run([str(p/'handles')],check=True)
if __name__=='__main__':unittest.main()
