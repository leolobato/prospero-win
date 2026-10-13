/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Exercise the actual PE -> Unix media bridge, compiled for i386 and x86_64.
 * Supply a local H.264/AAC MP4 path, or place h264-aac.mp4 beside the EXE.
 * No presentation claim is made. */
#define COBJMACROS
#include <windows.h>
#include <stdio.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <propvarutil.h>
#include <initguid.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <wchar.h>
#include <wmsdk.h>
#include <nserror.h>
#include <string.h>
#include <winternl.h>
#include <stddef.h>
DEFINE_GUID(probe_handler_iid,0xbb420aa4,0x765b,0x4a1f,0x91,0xfe,0xd6,0xa8,0xa1,0x43,0x92,0x4c);
DEFINE_GUID(probe_handler,0x317df618,0x5e5a,0x468a,0x9f,0x15,0xd8,0x27,0xa9,0xa0,0x81,0x62);
DEFINE_GUID(probe_h264,0x62ce7e72,0x4c71,0x4d20,0xb1,0x5d,0x45,0x28,0x31,0xa8,0x7d,0x9d);
DEFINE_GUID(probe_aac,0x32d186a7,0x218f,0x4c75,0x88,0x76,0xdd,0x77,0x27,0x3a,0x89,0x99);
static volatile LONG progress;
DEFINE_GUID(probe_mp4,0x271c3902,0x6095,0x4c45,0xa2,0x2f,0x20,0x09,0x18,0x16,0xee,0x9e);
DEFINE_GUID(probe_asf,0x41457294,0x644c,0x4298,0xa2,0x8a,0xbd,0x69,0xf2,0xc0,0xcf,0x3b);
static DWORD WINAPI watchdog(void *event)
{
    LONG previous=InterlockedCompareExchange(&progress,0,0);
    while (WaitForSingleObject(event,90000)==WAIT_TIMEOUT) {
        LONG current=InterlockedCompareExchange(&progress,0,0);
        if (current!=previous) { previous=current; continue; }
        fprintf(stderr,"MF probe timed out after 90 seconds\n"); fflush(stderr);
        ExitProcess(124);
    }
    return 0;
}
static int activate(const char *name, REFCLSID clsid, REFIID iid)
{
    IUnknown *object=NULL;
    HRESULT hr=CoCreateInstance(clsid,NULL,CLSCTX_INPROC_SERVER,iid,(void **)&object);
    printf("activate %s: %08lx\n",name,(unsigned long)hr); fflush(stdout);
    if (object) IUnknown_Release(object);
    return SUCCEEDED(hr);
}
struct frame_record { LONGLONG timestamp; ULONGLONG hash; DWORD size; };
static int decode(const WCHAR *path, BOOL video, BOOL cancel, BOOL quiet)
{
    IMFSourceReader *reader=NULL;
    IMFMediaType *type=NULL;
    DWORD stream=video?MF_SOURCE_READER_FIRST_VIDEO_STREAM:MF_SOURCE_READER_FIRST_AUDIO_STREAM;
    const char *name=video?"video":"audio";
    HRESULT hr=MFCreateSourceReaderFromURL(path,NULL,&reader);
    int passed=0;
    struct frame_record *frames=NULL;
    unsigned frame_count=0,frame_capacity=0;
    LONGLONG tolerance=1000000,initial_timestamp=-1;
    ULONGLONG initial_hash=0;
    if (FAILED(hr)) goto done;
    hr=IMFSourceReader_SetStreamSelection(reader,MF_SOURCE_READER_ALL_STREAMS,FALSE);
    if (FAILED(hr)) goto done;
    hr=IMFSourceReader_SetStreamSelection(reader,stream,TRUE);
    if (FAILED(hr)) goto done;
    hr=MFCreateMediaType(&type);
    if (FAILED(hr)) goto done;
    hr=IMFMediaType_SetGUID(type,&MF_MT_MAJOR_TYPE,video?&MFMediaType_Video:&MFMediaType_Audio);
    if (FAILED(hr)) goto done;
    hr=IMFMediaType_SetGUID(type,&MF_MT_SUBTYPE,video?&MFVideoFormat_NV12:&MFAudioFormat_PCM);
    if (FAILED(hr)) goto done;
    hr=IMFSourceReader_SetCurrentMediaType(reader,stream,NULL,type);
    if (FAILED(hr)) goto done;
    IMFMediaType_Release(type); type=NULL;
    hr=IMFSourceReader_GetCurrentMediaType(reader,stream,&type);
    if (FAILED(hr)) goto done;
    if (video) {
        UINT64 size=0;
        hr=IMFMediaType_GetUINT64(type,&MF_MT_FRAME_SIZE,&size);
        if (FAILED(hr) || !(size>>32) || !(UINT32)size) goto done;
        UINT64 rate=0;
        if (SUCCEEDED(IMFMediaType_GetUINT64(type,&MF_MT_FRAME_RATE,&rate)) && (rate>>32) && (UINT32)rate)
            tolerance=(10000000LL*(UINT32)rate+(rate>>32)-1)/(rate>>32);
        printf("video dimensions: %lu x %lu\n",(unsigned long)(size>>32),(unsigned long)(UINT32)size);
    } else {
        UINT32 rate=0,channels=0,bits=0;
        hr=IMFMediaType_GetUINT32(type,&MF_MT_AUDIO_SAMPLES_PER_SECOND,&rate);
        if (FAILED(hr)) goto done;
        hr=IMFMediaType_GetUINT32(type,&MF_MT_AUDIO_NUM_CHANNELS,&channels);
        if (FAILED(hr)) goto done;
        hr=IMFMediaType_GetUINT32(type,&MF_MT_AUDIO_BITS_PER_SAMPLE,&bits);
        if (FAILED(hr) || !rate || !channels || !bits) goto done;
        printf("audio format: %lu Hz, %lu channels, %lu bits\n",(unsigned long)rate,(unsigned long)channels,(unsigned long)bits);
    }
    /* Decode to EOS, then seek to zero and decode again with the same reader. */
    for (unsigned pass=0;pass<(cancel?1u:3u);++pass) {
        unsigned samples=0,reads=0;
        ULONGLONG bytes=0,checksum=0;
        LONGLONG previous=-1,first=-1,requested=0;
        BOOL eos=FALSE;
        if (pass) {
            PROPVARIANT position;
            PropVariantInit(&position); position.vt=VT_I8; position.hVal.QuadPart=0;
            if (pass==2) {
                PROPVARIANT duration;
                PropVariantInit(&duration);
                hr=IMFSourceReader_GetPresentationAttribute(reader,MF_SOURCE_READER_MEDIASOURCE,&MF_PD_DURATION,&duration);
                if (FAILED(hr) || duration.vt!=VT_UI8) { PropVariantClear(&duration); goto done; }
                position.hVal.QuadPart=duration.uhVal.QuadPart-duration.uhVal.QuadPart/4;
                if (!position.hVal.QuadPart) { PropVariantClear(&duration); hr=E_FAIL; goto done; }
                PropVariantClear(&duration);
            }
            requested=position.hVal.QuadPart;
            hr=IMFSourceReader_SetCurrentPosition(reader,&GUID_NULL,&position);
            if (FAILED(hr)) goto done;
        }
        while (samples<(cancel?5u:100000u) && reads++<100000u) {
            IMFSample *sample=NULL;
            IMFMediaBuffer *buffer=NULL;
            DWORD flags=0,actual=0,size=0;
            BOOL preroll=FALSE;
            LONGLONG timestamp=0;
            BYTE *data=NULL;
            hr=IMFSourceReader_ReadSample(reader,stream,0,&actual,&flags,&timestamp,&sample);
            if (FAILED(hr) || (flags&MF_SOURCE_READERF_ERROR)) { if(sample) IMFSample_Release(sample); goto done; }
            if (sample) {
                if (timestamp<0 || timestamp<=previous) { IMFSample_Release(sample); hr=E_FAIL; goto done; }
                previous=timestamp;
                preroll=pass==2 && timestamp<requested-tolerance;
                hr=IMFSample_ConvertToContiguousBuffer(sample,&buffer);
                if (SUCCEEDED(hr)) hr=IMFMediaBuffer_Lock(buffer,&data,NULL,&size);
                if (SUCCEEDED(hr)) {
                    ULONGLONG hash=14695981039346656037ULL;
                    for (DWORD i=0;i<size;++i) { checksum+=data[i]; hash=(hash^data[i])*1099511628211ULL; }
                    if (!samples && !preroll) {
                        if (first<0) first=timestamp;
                        if (!pass) { initial_timestamp=timestamp; initial_hash=hash; }
                        else if ((pass==1 && (timestamp>initial_timestamp+tolerance || timestamp<initial_timestamp-tolerance || (video && hash!=initial_hash))) ||
                                 (pass==2 && timestamp>requested+tolerance)) {
                            printf("seek segment mismatch pass=%u requested=%lld timestamp=%lld tolerance=%lld\n",pass,requested,timestamp,tolerance); hr=E_FAIL;
                        }
                    }
                    if (video && SUCCEEDED(hr)) {
                        if (!pass) {
                            if (frame_count==frame_capacity) {
                                unsigned capacity=frame_capacity?frame_capacity*2:64;
                                struct frame_record *new_frames=realloc(frames,capacity*sizeof(*frames));
                                if (!new_frames) hr=E_OUTOFMEMORY;
                                else { frames=new_frames; frame_capacity=capacity; }
                            }
                            if (SUCCEEDED(hr)) frames[frame_count++]=(struct frame_record){timestamp,hash,size};
                        } else {
                            unsigned i;
                            for (i=0;i<frame_count;++i)
                                if (frames[i].timestamp<=timestamp && timestamp<=frames[i].timestamp+tolerance &&
                                    frames[i].hash==hash && frames[i].size==size) break;
                            if (i==frame_count) {
                                printf("seek content mismatch pass=%u timestamp=%lld recorded=%d actual_hash=%llu expected_hash=%llu\n",
                                    pass,timestamp,i<frame_count,hash,i<frame_count?frames[i].hash:0); hr=E_FAIL;
                            }
                        }
                    }
                    if (!preroll) { bytes+=size; ++samples; }
                    IMFMediaBuffer_Unlock(buffer);
                }
                if (buffer) IMFMediaBuffer_Release(buffer);
                IMFSample_Release(sample);
                if (FAILED(hr)) goto done;
            }
            if (flags&MF_SOURCE_READERF_ENDOFSTREAM) { eos=TRUE; break; }
        }
        printf("%s pass %u: samples=%u bytes=%llu checksum=%llu first=%lld last=%lld eos=%d\n",name,pass,samples,bytes,checksum,first,previous,eos); fflush(stdout);
        if ((!cancel && !eos) || !samples || !bytes || !checksum) { hr=E_FAIL; goto done; }
    }
    if (cancel && FAILED(hr=IMFSourceReader_Flush(reader,MF_SOURCE_READER_ALL_STREAMS))) goto done;
    passed=1;
done:
    if (!quiet || !passed) printf("%s result: %s hr=%08lx\n",name,passed?"PASS":"FAIL",(unsigned long)hr);
    fflush(stdout);
    if (type) IMFMediaType_Release(type);
    if (reader) IMFSourceReader_Release(reader);
    free(frames);
    return passed;
}
struct probe_handle_entry {
    void *object;
    ULONG_PTR pid,handle;
    ULONG access;
    USHORT backtrace,type;
    ULONG attributes,reserved;
};
struct probe_handles { ULONG_PTR count,reserved; struct probe_handle_entry entries[1]; };
_Static_assert(sizeof(struct probe_handle_entry)==(sizeof(void *)==8?40:28),"class64 entry ABI");
_Static_assert(offsetof(struct probe_handles,entries)==2*sizeof(void *),"class64 header ABI");
static BOOL process_handles(DWORD *count)
{
    HMODULE module=GetModuleHandleW(L"ntdll.dll");
    FARPROC address=module?GetProcAddress(module,"NtQuerySystemInformation"):NULL;
    NTSTATUS (NTAPI *query)(SYSTEM_INFORMATION_CLASS,PVOID,ULONG,PULONG)=NULL;
    ULONG size=65536;
    memcpy(&query,&address,sizeof(query));
    if (!query) return FALSE;
    for (unsigned retry=0;retry<10 && size<=64*1024*1024;++retry) {
        struct probe_handles *info=malloc(size);
        ULONG needed=0;
        NTSTATUS status;
        if (!info) return FALSE;
        status=query((SYSTEM_INFORMATION_CLASS)64,info,size,&needed);
        if (!status) {
            DWORD result=0,pid=GetCurrentProcessId();
            BOOL valid=needed>=offsetof(struct probe_handles,entries) && needed<=size &&
                info->count<=(needed-offsetof(struct probe_handles,entries))/sizeof(info->entries[0]);
            if (valid) for (ULONG_PTR i=0;i<info->count;++i) if (info->entries[i].pid==pid) ++result;
            free(info);
            if (!valid) return FALSE;
            *count=result;
            return TRUE;
        }
        free(info);
        if ((ULONG)status!=0xc0000004 && (ULONG)status!=0xc0000023) return FALSE;
        if (size>32*1024*1024 || needed>64*1024*1024) return FALSE;
        size=needed>size*2?needed:size*2;
    }
    return FALSE;
}
static BOOL calibrate_handles(void)
{
    DWORD before=0,during=0,after=0;
    HANDLE events[3]={0};
    BOOL ok=process_handles(&before);
    for (unsigned i=0;i<3 && ok;++i) ok=(events[i]=CreateEventW(NULL,FALSE,FALSE,NULL))!=NULL;
    if (ok) ok=process_handles(&during) && during==before+3;
    for (unsigned i=0;i<3;++i) if(events[i]) CloseHandle(events[i]);
    if (ok) ok=process_handles(&after) && after==before;
    printf("handles calibration before=%lu during=%lu after=%lu valid=%d\n",
        (unsigned long)before,(unsigned long)during,(unsigned long)after,ok);
    return ok;
}
static BOOL resources(unsigned cycle, BOOL cancel)
{
    PROCESS_MEMORY_COUNTERS memory={0};
    DWORD handles=0,threads=0,pid=GetCurrentProcessId();
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
    THREADENTRY32 entry={0};
    static BOOL calibrated;
    BOOL valid=TRUE;
    memory.cb=sizeof(memory); entry.dwSize=sizeof(entry);
    if (!cycle) calibrated=calibrate_handles();
    if (snapshot!=INVALID_HANDLE_VALUE) {
        if (Thread32First(snapshot,&entry)) do { if (entry.th32OwnerProcessID==pid) ++threads; } while (Thread32Next(snapshot,&entry));
        else valid=FALSE;
        CloseHandle(snapshot);
    } else valid=FALSE;
    if (!GetProcessMemoryInfo(GetCurrentProcess(),&memory,sizeof(memory)) ||
        !calibrated || !process_handles(&handles)) valid=FALSE;
    printf("resources cycle=%u cancel=%d commit=%llu handles=%lu counters_valid=%d windows_threads=%lu\n",
        cycle,cancel,(unsigned long long)memory.PagefileUsage,(unsigned long)handles,valid,(unsigned long)threads);
    fflush(stdout);
    return valid;
}
static int expect_error(const WCHAR *path)
{
    IMFSourceReader *reader=NULL;
    IMFMediaType *type=NULL;
    HRESULT hr=MFCreateSourceReaderFromURL(path,NULL,&reader);
    if (SUCCEEDED(hr) && !reader) hr=E_UNEXPECTED;
    if (SUCCEEDED(hr)) {
        hr=MFCreateMediaType(&type);
        if (SUCCEEDED(hr)) hr=IMFMediaType_SetGUID(type,&MF_MT_MAJOR_TYPE,&MFMediaType_Video);
        if (SUCCEEDED(hr)) hr=IMFMediaType_SetGUID(type,&MF_MT_SUBTYPE,&MFVideoFormat_NV12);
        if (SUCCEEDED(hr)) hr=IMFSourceReader_SetCurrentMediaType(reader,MF_SOURCE_READER_FIRST_VIDEO_STREAM,NULL,type);
        if (SUCCEEDED(hr)) {
            IMFSample *sample=NULL;
            DWORD flags=0;
            LONGLONG timestamp=0;
            hr=IMFSourceReader_ReadSample(reader,MF_SOURCE_READER_FIRST_VIDEO_STREAM,0,NULL,&flags,&timestamp,&sample);
            if (sample) IMFSample_Release(sample);
        }
    }
    if (type) IMFMediaType_Release(type);
    if (reader) IMFSourceReader_Release(reader);
    printf("expected-error hr=%08lx pass=%d\n",(unsigned long)hr,
        hr==MF_E_UNSUPPORTED_BYTESTREAM_TYPE || hr==MF_E_INVALIDMEDIATYPE || hr==MF_E_TOPO_CODEC_NOT_FOUND);
    fflush(stdout);
    return hr==MF_E_UNSUPPORTED_BYTESTREAM_TYPE || hr==MF_E_INVALIDMEDIATYPE || hr==MF_E_TOPO_CODEC_NOT_FOUND;
}
static int source_ownership(const WCHAR *path)
{
    IMFSourceResolver *resolver=NULL;
    IMFMediaSource *source=NULL;
    IMFPresentationDescriptor *presentation=NULL;
    IMFStreamDescriptor *descriptors[16]={0};
    IUnknown *object=NULL;
    MF_OBJECT_TYPE object_type=MF_OBJECT_INVALID;
    HRESULT hr=MFCreateSourceResolver(&resolver);
    DWORD count=0;
    int ok=0;
    if (SUCCEEDED(hr)) hr=IMFSourceResolver_CreateObjectFromURL(resolver,path,MF_RESOLUTION_MEDIASOURCE,NULL,&object_type,&object);
    if (SUCCEEDED(hr) && object_type==MF_OBJECT_MEDIASOURCE && object)
        hr=IUnknown_QueryInterface(object,&IID_IMFMediaSource,(void **)&source);
    else if (SUCCEEDED(hr)) hr=E_UNEXPECTED;
    if (object) { IUnknown_Release(object); object=NULL; }
    if (SUCCEEDED(hr)) hr=IMFMediaSource_CreatePresentationDescriptor(source,&presentation);
    if (SUCCEEDED(hr)) hr=IMFPresentationDescriptor_GetStreamDescriptorCount(presentation,&count);
    if (SUCCEEDED(hr) && (!count || count>16)) hr=E_UNEXPECTED;
    if (SUCCEEDED(hr)) for (DWORD i=0;i<count;++i) {
        BOOL selected=FALSE;
        hr=IMFPresentationDescriptor_GetStreamDescriptorByIndex(presentation,i,&selected,&descriptors[i]);
        if (FAILED(hr)) break;
    }
    if (presentation) IMFPresentationDescriptor_Release(presentation);
    if (source) { IMFMediaSource_Shutdown(source); IMFMediaSource_Release(source); }
    if (resolver) IMFSourceResolver_Release(resolver);
    ok=SUCCEEDED(hr);
    for (unsigned i=0;i<16;++i) if (descriptors[i]) {
        ULONG refs=IMFStreamDescriptor_Release(descriptors[i]);
        printf("descriptor %u remaining_refs=%lu\n",i,(unsigned long)refs);
        if (refs) ok=0;
    }
    printf("source ownership pass=%d hr=%08lx\n",ok,(unsigned long)hr);fflush(stdout);
    return ok;
}
static int disabled_activation(void)
{
    const GUID *classes[]={&probe_handler,&probe_mp4,&probe_asf};
    IWMSyncReader *reader=NULL;
    int ok=1;
    HRESULT hr;
    for (unsigned i=0;i<sizeof(classes)/sizeof(classes[0]);++i) {
        IUnknown *object=NULL;
        hr=CoCreateInstance(classes[i],NULL,CLSCTX_INPROC_SERVER,&probe_handler_iid,(void **)&object);
        printf("disabled handler %u hr=%08lx null=%d\n",i,(unsigned long)hr,!object);
        ok&=hr==MF_E_UNSUPPORTED_BYTESTREAM_TYPE && !object;
        if (object) IUnknown_Release(object);
    }
    HMODULE module=LoadLibraryW(L"wmvcore.dll");
    FARPROC address=module?GetProcAddress(module,"WMCreateSyncReader"):NULL;
    HRESULT (WINAPI *create_reader)(IUnknown *,DWORD,IWMSyncReader **)=NULL;
    _Static_assert(sizeof(create_reader)==sizeof(address),"Windows function pointer ABI");
    memcpy(&create_reader,&address,sizeof(create_reader));
    hr=create_reader?create_reader(NULL,0,&reader):HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    printf("disabled WM reader hr=%08lx null=%d\n",(unsigned long)hr,!reader);
    ok&=hr==NS_E_UNSUPPORTED_SOURCETYPE && !reader;
    if (reader) IWMSyncReader_Release(reader);
    if (module) FreeLibrary(module);
    fflush(stdout);
    return ok;
}
int wmain(int argc,WCHAR **argv)
{
    HANDLE stop=CreateEventW(NULL,TRUE,FALSE,NULL),thread;
    WCHAR input[MAX_PATH];
    DWORD length;
    HRESULT hr;
    int ok=1;
    unsigned full_cycles=50;
    WCHAR count[16],*end;
    DWORD count_length=GetEnvironmentVariableW(L"PW_MF_PROBE_FULL_CYCLES",count,16);
    BOOL disabled=argc==2 && !wcscmp(argv[1],L"--disabled");
    if (count_length) {
        unsigned long value=count_length<16?wcstoul(count,&end,10):0;
        if (!value || value>1000 || *end) { CloseHandle(stop); return 2; }
        full_cycles=value;
    }
    BOOL cycles=argc==3 && !wcscmp(argv[1],L"--cycles");
    BOOL negative=argc==3 && !wcscmp(argv[1],L"--expect-error");
    BOOL failed_cycles=argc==3 && !wcscmp(argv[1],L"--error-cycles");
    BOOL ownership=argc==3 && !wcscmp(argv[1],L"--ownership");
    if (argc>3 || (argc==3 && !cycles && !negative && !failed_cycles && !ownership) || !stop) return 2;
    if (argc>=2) length=GetFullPathNameW(argv[argc-1],MAX_PATH,input,NULL);
    else {
        WCHAR *name;
        length=GetModuleFileNameW(NULL,input,MAX_PATH);
        if (!length || length>=MAX_PATH) return 2;
        name=wcsrchr(input,L'\\');
        if (!name || (size_t)(name-input)+1+wcslen(L"h264-aac.mp4")>=MAX_PATH) return 2;
        wcscpy(name+1,L"h264-aac.mp4");
    }
    if (!length || length>=MAX_PATH) return 2;
    thread=CreateThread(NULL,0,watchdog,stop,0,NULL);
    if (!thread) { CloseHandle(stop); return 2; }
    hr=CoInitializeEx(NULL,COINIT_MULTITHREADED);
    if (FAILED(hr)) return 3;
    hr=MFStartup(MF_VERSION,MFSTARTUP_FULL);
    printf("PE bits=%u MFStartup=%08lx\n",(unsigned)(8*sizeof(void *)),(unsigned long)hr); fflush(stdout);
    if (SUCCEEDED(hr)) {
        if (ownership) ok=source_ownership(input);
        else if (disabled) ok=disabled_activation();
        else if (negative) ok=expect_error(input);
        else if (failed_cycles) {
            ok&=resources(0,TRUE);
            for (unsigned i=1;i<=50 && ok;++i) {
                InterlockedIncrement(&progress);
                ok&=expect_error(input);
                if (!(i%10)) ok&=resources(i,TRUE);
            }
        }
        else {
        ok&=activate("byte-stream-handler",&probe_handler,&probe_handler_iid);
        ok&=activate("H264",&probe_h264,&IID_IMFTransform);
        ok&=activate("AAC",&probe_aac,&IID_IMFTransform);
        if (cycles) {
            ok&=resources(0,FALSE);
            for (unsigned i=1;i<=full_cycles+20 && ok;++i) {
                BOOL cancel=i>full_cycles;
                InterlockedIncrement(&progress);
                ok&=decode(input,TRUE,cancel,TRUE);
                if (ok) ok&=decode(input,FALSE,cancel,TRUE);
                if (!(i%10)) ok&=resources(i,cancel);
            }
        } else {
            ok&=decode(input,TRUE,FALSE,FALSE);
            ok&=decode(input,FALSE,FALSE,FALSE);
        }
        }
        MFShutdown();
    } else ok=0;
    CoUninitialize();
    SetEvent(stop); WaitForSingleObject(thread,INFINITE); CloseHandle(thread); CloseHandle(stop);
    printf("MF bridge result: %s (presentation untested)\n",ok?"PASS":"FAIL"); fflush(stdout);
    return ok?0:1;
}
