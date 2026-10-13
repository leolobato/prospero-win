#!/usr/bin/env python3
"""Compile and exercise Wine's actual cached media-disable helper on the host."""
from pathlib import Path
import os
import re
import shutil
import sys
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
class MediaFailure(unittest.TestCase):
 def test_workqueue_has_one_forced_debug_initialization(self):
  patch=(ROOT/'wine/patches/0482-rtworkq-cleanup-groups.patch').read_text()
  after='\n'.join(line[1:] for line in patch.splitlines() if line.startswith((' ','+')) and not line.startswith('+++'))
  body=after[after.index('static HRESULT pool_queue_init'):after.index('static BOOL pool_queue_shutdown')]
  if os.environ.get('PW_TEST_WORKQUEUE_BASELINE')=='1':
   body=body.replace('    list_init(&queue->pending_items);','    list_init(&queue->pending_items);\n    InitializeCriticalSection(&queue->cs);')
  source=r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#define HRESULT int
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
#define S_OK 0
#define RTWQ_STANDARD_WORKQUEUE 0
#define RTWQ_WINDOW_WORKQUEUE 1
#define TP_CALLBACK_PRIORITY_NORMAL 1
#define FIXME(...) ((void)0)
typedef struct {unsigned Version,Size;void *Pool,*CleanupGroup;void (*CleanupGroupCancelCallback)(void);unsigned CallbackPriority;} TP_CALLBACK_ENVIRON_V3;
struct queue_desc {unsigned queue_type;};
struct queue {void *pool;TP_CALLBACK_ENVIRON_V3 envs[3];int pending_items;void *cs;};
static unsigned priorities[3]={0,1,2},allocated,released;
static void standard_queue_cleanup_callback(void) {}
static void *CreateThreadpool(void *unused){(void)unused;return (void*)1;}
static void *CreateThreadpoolCleanupGroup(void){return (void*)2;}
static void list_init(int *list){*list=0;}
static void SetThreadpoolThreadMinimum(void *pool,unsigned n){assert(pool && n==1);}
static void SetThreadpoolThreadMaximum(void *pool,unsigned n){assert(pool && n==1);}
static void InitializeCriticalSection(void **cs){*cs=malloc(48);assert(*cs);++allocated;}
static void DeleteCriticalSection(void **cs){free(*cs);*cs=NULL;++released;}
'''+body+r'''
int main(void){
 struct queue queue={0};struct queue_desc desc={RTWQ_STANDARD_WORKQUEUE};
 assert(pool_queue_init(&desc,&queue)==S_OK);
 /* Actual common init_work_queue initializes the CS after successful ops init.
  * Model forced debug allocation (Windows7), including overwritten allocations. */
 InitializeCriticalSection(&queue.cs);assert(allocated==1);
 DeleteCriticalSection(&queue.cs);assert(released==allocated);return 0;
}
'''
  with tempfile.TemporaryDirectory() as d:
   p=Path(d);(p/'queue.c').write_text(source)
   subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(p/'queue.c'),'-o',str(p/'queue')],check=True)
   subprocess.run([str(p/'queue')],check=True)
 def test_workqueue_cleanup_group_and_exhaustion(self):
  patch=(ROOT/'wine/patches/0482-rtworkq-cleanup-groups.patch').read_text()
  after='\n'.join(line[1:] for line in patch.splitlines() if line.startswith((' ','+')) and not line.startswith('+++'))
  body=after[after.index('static BOOL pool_queue_shutdown'):]
  body=body[:body.index('\nstatic void CALLBACK')]
  failure=after[after.index('        LeaveCriticalSection(&queues_section);\n        shutdown_queue(queue);'):]
  failure=failure[:failure.index('\n    }')]
  # Exercise the actual patched bodies. The baseline switch proves the old
  # omissions fail allocation accounting, rather than checking source strings.
  if os.environ.get('PW_TEST_WORKQUEUE_BASELINE')=='1':
   body=body.replace('    CloseThreadpoolCleanupGroup(queue->envs[0].CleanupGroup);\n','')
   failure=failure.replace('        shutdown_queue(queue);\n','')
  source=r'''
#include <assert.h>
#include <stdlib.h>
#define BOOL int
#define FALSE 0
#define TRUE 1
#define WARN(...) ((void)0)
#define E_OUTOFMEMORY 7
struct environment { void *CleanupGroup; };
struct queue { void *pool; struct environment envs[3]; };
static unsigned members,groups,pools,critical_sections,locks;
static int queues_section;
static void CloseThreadpoolCleanupGroupMembers(void *group,int cancel,void *context) {
 assert(group && !cancel && !context && !members && !groups && !pools);++members;
}
static void __attribute__((unused)) CloseThreadpoolCleanupGroup(void *group) {assert(group && members==1 && !groups && !pools);++groups;}
static void CloseThreadpool(void *pool) {assert(pool && members==1 && groups==1 && !pools);++pools;}
static void LeaveCriticalSection(int *cs) {assert(cs==&queues_section && locks==1);--locks;}
'''+body+r'''
static void __attribute__((unused)) shutdown_queue(struct queue *queue) {assert(!locks && critical_sections==1);assert(pool_queue_shutdown(queue));--critical_sections;}
static void checked_free(struct queue *queue) {assert(!queue->pool && !critical_sections && groups==1 && pools==1);free(queue);}
#define free checked_free
static int exhausted(struct queue *queue) {
'''+failure+r'''
}
static void init(struct queue *queue) {members=groups=pools=0;queue->pool=queue;for(unsigned i=0;i<3;++i)queue->envs[i].CleanupGroup=queue;}
int main(void) {
 struct queue queue={0};init(&queue);assert(pool_queue_shutdown(&queue));
 assert(members==1 && groups==1 && pools==1 && !queue.pool);
 assert(!pool_queue_shutdown(&queue));assert(groups==1);
 struct queue *allocated=calloc(1,sizeof(*allocated));init(allocated);critical_sections=locks=1;
 assert(exhausted(allocated)==E_OUTOFMEMORY);assert(!critical_sections && !locks);return 0;
}
'''
  with tempfile.TemporaryDirectory() as d:
   p=Path(d);(p/'queue.c').write_text(source)
   subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(p/'queue.c'),'-o',str(p/'queue')],check=True)
   subprocess.run([str(p/'queue')],check=True)
 def test_hresult_names_compile_against_windows_headers(self):
  compiler=shutil.which('x86_64-w64-mingw32-gcc')
  if not compiler:self.skipTest('MinGW headers/compiler unavailable')
  patch=(ROOT/'wine/patches/0481-winegstreamer-media-failure.patch').read_text()
  added='\n'.join(line[1:] for line in patch.splitlines() if line.startswith('+') and not line.startswith('+++'))
  constants=sorted(set(re.findall(r'\b(?:MF_E|NS_E)_[A-Z_]+\b',added)))
  with tempfile.TemporaryDirectory() as d:
   p=Path(d);source=p/'constants.c'
   source.write_text('#include <windows.h>\n#include <mferror.h>\n#include <nserror.h>\nHRESULT media_errors[]={'+','.join(constants)+'};\n')
   subprocess.run([compiler,'-Wall','-Wextra','-Werror','-c',str(source),'-o',str(p/'constants.o')],check=True)
 def test_parser_cache_cleanup_is_idempotent(self):
  patch=(ROOT/'wine/patches/0481-winegstreamer-media-failure.patch').read_text()
  start=patch.index('+static void parser_clear_input_cache')
  end=patch.index(' static NTSTATUS wg_parser_disconnect',start)
  helper='\n'.join(line[1:] for line in patch[start:end].splitlines() if line.startswith('+'))
  source=r'''
#include <stdlib.h>
#include <assert.h>
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
struct chunk { void *data; };
struct wg_parser { struct chunk input_cache_chunks[4]; };
static unsigned releases;
static void counted_free(void *data) { if(data)++releases;free(data); }
#define free counted_free
'''+helper+r'''
int main(void) {
 struct wg_parser parser={0};
 for(unsigned i=0;i<4;++i){parser.input_cache_chunks[i].data=malloc(512*1024);assert(parser.input_cache_chunks[i].data);}
 parser_clear_input_cache(&parser);assert(releases==4);
 for(unsigned i=0;i<4;++i)assert(!parser.input_cache_chunks[i].data);
 parser_clear_input_cache(&parser);assert(releases==4);return 0;
}
'''
  with tempfile.TemporaryDirectory() as d:
   p=Path(d);(p/'cleanup.c').write_text(source)
   # Xcode 26.3 ASan deadlocks during dyld initialization on this macOS host.
   # Linux runs retain ASan/UBSan; allocation/release accounting runs everywhere.
   sanitizer=[] if sys.platform=='darwin' else ['-fsanitize=address,undefined']
   subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',*sanitizer,str(p/'cleanup.c'),'-o',str(p/'cleanup')],check=True)
   subprocess.run([str(p/'cleanup')],check=True)
 def test_real_gstreamer_caps_ownership(self):
  import shlex
  flags=subprocess.run(['pkg-config','--cflags','--libs','gstreamer-1.0'],capture_output=True,text=True)
  if flags.returncode:self.skipTest('host GStreamer development library unavailable')
  patch=(ROOT/'wine/patches/0481-winegstreamer-media-failure.patch').read_text()
  additions=[line[1:].strip() for line in patch.splitlines() if line.startswith('+') and not line.startswith('+++')]
  take=next(line for line in additions if 'gst_caps_take(&stream->desired_caps' in line)
  current=next(line for line in additions if 'gst_caps_replace(&stream->current_caps, caps)' in line)
  cleanup='\n'.join(dict.fromkeys(line for line in additions if 'gst_caps_replace(&stream->' in line and ', NULL)' in line))
  query=next(line for line in additions if line=='gst_caps_unref(result);')
  source=r'''
#include <gst/gst.h>
#include <assert.h>
static unsigned destroyed;
static void gone(gpointer data,GstMiniObject *obj){(void)data;(void)obj;++destroyed;}
static GstCaps *new_caps(void){GstCaps *caps=gst_caps_new_empty();gst_mini_object_weak_ref(GST_MINI_OBJECT(caps),gone,NULL);return caps;}
struct stream { GstCaps *desired_caps,*current_caps; };
static GstCaps *wg_format_to_caps(void *format){(void)format;return new_caps();}
int main(void){
 gst_init(NULL,NULL);struct stream value={0},*stream=&value;void *format=NULL;
'''+take+'\n'+take+r'''
 assert(destroyed==1);
 GstCaps *caps=new_caps();
'''+current+r'''
 gst_caps_unref(caps);caps=new_caps();
'''+current+r'''
 gst_caps_unref(caps);assert(destroyed==2);
'''+cleanup+r'''
 assert(destroyed==4);GstCaps *result=new_caps();GstQuery *query=gst_query_new_caps(NULL);
 gst_query_set_caps_result(query,result);
'''+query+r'''
 assert(destroyed==4);gst_query_unref(query);assert(destroyed==5);return 0;
}
'''
  with tempfile.TemporaryDirectory() as d:
   p=Path(d);(p/'caps.c').write_text(source)
   subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(p/'caps.c'),'-o',str(p/'caps'),*shlex.split(flags.stdout)],check=True)
   subprocess.run([str(p/'caps')],check=True)
 def test_descriptor_creator_reference_is_transferred(self):
  patch=(ROOT/'wine/patches/0481-winegstreamer-media-failure.patch').read_text().splitlines()
  start=next(i for i,line in enumerate(patch) if 'object->duration = max(' in line)
  end=next(i for i in range(start,len(patch)) if 'object->stream_count++;' in patch[i])
  block='\n'.join(line[1:] for line in patch[start:end+1] if line.startswith((' ','+')))
  source=r'''
#include <assert.h>
#include <stddef.h>
struct descriptor { unsigned refs; };
struct stream { struct descriptor *descriptor; };
struct source { long duration;struct descriptor *descriptors[2];struct stream *streams[2];unsigned stream_count; };
static void IMFStreamDescriptor_AddRef(struct descriptor *desc){++desc->refs;}
static void IMFStreamDescriptor_Release(struct descriptor *desc){assert(desc->refs);--desc->refs;}
static long wg_parser_stream_get_duration(void *stream){(void)stream;return 1;}
#define max(a,b) ((a)>(b)?(a):(b))
static void store(struct source *object,unsigned i,struct descriptor *descriptor,struct stream *stream) {
 void *wg_stream=NULL;
'''+block+r'''
}
int main(void) {
 for(unsigned partial_failure=0;partial_failure<2;++partial_failure){
  struct source object={0};struct descriptor desc={1};struct stream stream={&desc};
  IMFStreamDescriptor_AddRef(&desc);store(&object,0,&desc,&stream);assert(desc.refs==2);
  /* Both Shutdown and later-stream failure release array and stream owners. */
  assert(object.stream_count==1 && object.descriptors[0]==&desc);
  IMFStreamDescriptor_Release(object.descriptors[0]);
  IMFStreamDescriptor_Release(object.streams[0]->descriptor);assert(!desc.refs);
 }
 return 0;
}
'''
  with tempfile.TemporaryDirectory() as d:
   p=Path(d);(p/'descriptor.c').write_text(source)
   subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(p/'descriptor.c'),'-o',str(p/'descriptor')],check=True)
   subprocess.run([str(p/'descriptor')],check=True)
 def test_cached_exact_disable_value(self):
  patch=(ROOT/'wine/patches/0481-winegstreamer-media-failure.patch').read_text()
  start=patch.index('+static BOOL CALLBACK media_disabled_once')
  end=patch.index(' BOOL init_gstreamer(void)',start)
  helper='\n'.join(line[1:] for line in patch[start:end].splitlines() if line.startswith('+'))
  stubs=r'''
#include <stdlib.h>
#include <stddef.h>
#include <wchar.h>
#include <string.h>
#define CALLBACK
#define BOOL int
#define TRUE 1
#define FALSE 0
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
#define INIT_ONCE_STATIC_INIT 0
typedef int INIT_ONCE;
typedef wchar_t WCHAR;
static unsigned GetEnvironmentVariableW(const wchar_t *name,wchar_t *value,unsigned size) {
 const char *env=getenv("PW_MEDIA_DISABLE"); unsigned length;
 (void)name; if(!env)return 0; length=strlen(env);
 if(length>=size)return length+1;
 for(unsigned i=0;i<=length;++i)value[i]=(unsigned char)env[i];return length;
}
static int InitOnceExecuteOnce(INIT_ONCE *once,int (*fn)(INIT_ONCE *,void *,void **),void *arg,void **ctx) {
 if(!*once){if(!fn(once,arg,ctx))return 0;*once=1;}return 1;
}
'''
  main='\nint main(int argc,char **argv){int first=media_disabled();setenv("PW_MEDIA_DISABLE",first?"0":"1",1);return argc!=2 || first!=atoi(argv[1]) || media_disabled()!=first;}\n'
  with tempfile.TemporaryDirectory() as d:
   p=Path(d);(p/'test.c').write_text(stubs+helper+main)
   subprocess.run(['cc','-std=c11','-D_POSIX_C_SOURCE=200809L',str(p/'test.c'),'-o',str(p/'test')],check=True)
   for value,expected in ((None,0),('',0),('1',1),('0',0),('01',0),('true',0),('11',0)):
    env=dict(os.environ);env.pop('PW_MEDIA_DISABLE',None)
    if value is not None:env['PW_MEDIA_DISABLE']=value
    self.assertEqual(subprocess.run([str(p/'test'),str(expected)],env=env).returncode,0,value)
if __name__=='__main__':unittest.main()
