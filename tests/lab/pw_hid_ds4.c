/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The PS5 title's controller as a DualShock 4 (WINE_PS5_PAD=dualshock4,
 * patches 0990-0992), as games that read a PlayStation controller find it:
 * a raw HID device with the controller's own reports. Run by
 * tools/test_wine_hid_pad.py --dualshock4 under a host Wine with
 * tests/lab/pw_hid_pad_sink.c preloaded. argv[1] is the sink's pad file as a
 * Windows path, argv[2] its rumble file.
 *
 * Checks, each printed as "ok"/"FAIL" with what was seen:
 * - one HID device 054c:09cc, "Wireless Controller", with no &IG_ in its
 *   path, and no Xbox 360 controller beside it;
 * - its report lengths (input 64, output 32) and feature reports 0x02 (a
 *   calibration SDL accepts) and 0x12 (a nonzero address);
 * - input report 0x01: sticks, buttons, hat, triggers, the touchpad's
 *   click, Share and the finger from the sink, a counter that moves while
 *   the state stays, reports a reader gets, and a neutral report for a
 *   disconnected pad;
 * - output report 0x05: rumble reaches the title's motors (large left, small
 *   right) only when its flags ask for it;
 * - XInput reports no controller;
 * - Raw Input and DirectInput list the controller;
 * - a second controller is a second DualShock 4 only while it is connected,
 *   with its own address, listed after the first, reading and rumbling apart.
 * Exit status 0 only when every check passed. */
#define DIRECTINPUT_VERSION 0x0800
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <setupapi.h>
#include <shlwapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <dinput.h>
#include <xinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *pad_path, *rumble_path;
static int failures;

static void check(int ok,const char *what,const char *seen)
{
    printf("%s %s (%s)\n",ok?"ok":"FAIL",what,seen);
    if(!ok)failures++;
}

/* XINPUT_GAMEPAD bits, and the sink's extras. */
enum { UP=0x1,DOWN=0x2,LEFT=0x4,RIGHT=0x8,START=0x10,BACK=0x20,L3=0x40,LB=0x100,A=0x1000,B=0x2000,X=0x4000,Y=0x8000 };
enum { TOUCHPAD=0x1,SHARE=0x2 };

static void set_pad(int connected,unsigned buttons,int lt,int rt,int lx,int ly,int rx,int ry,
                    unsigned extra,int touching,int touch_x,int touch_y)
{
    FILE *f=fopen(pad_path,"w");
    if(!f) { printf("FAIL cannot write %s\n",pad_path); exit(2); }
    fprintf(f,"%d %u %d %d %d %d %d %d %u %d %d %d\n",connected,buttons,lt,rt,lx,ly,rx,ry,extra,touching,touch_x,touch_y);
    fclose(f);
    Sleep(100);  /* the bus reports every few milliseconds */
}

static HANDLE device=INVALID_HANDLE_VALUE;
static HIDP_CAPS caps;

static void find_device(void)
{
    SP_DEVICE_INTERFACE_DATA iface={.cbSize=sizeof(iface)};
    char buffer[1024],seen[1024]="",paths[600]="";
    SP_DEVICE_INTERFACE_DETAIL_DATA_A *detail=(void *)buffer;
    GUID hid;
    HDEVINFO set;
    int ds4=0,xbox=0,ig=0;
    WCHAR product[128]={0};
    HIDD_ATTRIBUTES attributes={.Size=sizeof(attributes)};
    PHIDP_PREPARSED_DATA preparsed;

    HidD_GetHidGuid(&hid);
    /* winebus makes the device when the HID drivers start: give them a moment. */
    for(int tries=0;tries<50 && !ds4;tries++) {
        if(tries)Sleep(100);
        set=SetupDiGetClassDevsA(&hid,NULL,NULL,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE);
        xbox=ig=0;paths[0]=0;
        for(DWORD i=0;SetupDiEnumDeviceInterfaces(set,NULL,&hid,i,&iface);i++) {
            detail->cbSize=sizeof(*detail);
            if(!SetupDiGetDeviceInterfaceDetailA(set,&iface,detail,sizeof(buffer),NULL,NULL))continue;
            CharLowerA(detail->DevicePath);
            if(strlen(paths)+strlen(detail->DevicePath)+2<sizeof(paths)) {
                strcat(paths,detail->DevicePath);strcat(paths," ");
            }
            if(strstr(detail->DevicePath,"&ig_"))ig++;
            if(strstr(detail->DevicePath,"vid_045e"))xbox++;
            if(!strstr(detail->DevicePath,"vid_054c&pid_09cc"))continue;
            if(!ds4++)
                device=CreateFileA(detail->DevicePath,GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,
                                   NULL,OPEN_EXISTING,0,NULL);
        }
        SetupDiDestroyDeviceInfoList(set);
    }
    snprintf(seen,sizeof(seen),"%d DualShock 4, %d Xbox, %d &ig_: %s",ds4,xbox,ig,paths);
    check(ds4==1 && !xbox && !ig,"one DualShock 4 HID device, no XInput gamepad interface",seen);
    if(device==INVALID_HANDLE_VALUE)return;

    HidD_GetAttributes(device,&attributes);
    HidD_GetProductString(device,product,sizeof(product));
    snprintf(seen,sizeof(seen),"%04x:%04x %ls",attributes.VendorID,attributes.ProductID,product);
    check(attributes.VendorID==0x054c && attributes.ProductID==0x09cc && !lstrcmpW(product,L"Wireless Controller"),
          "its attributes and product string",seen);
    if(HidD_GetPreparsedData(device,&preparsed)) {
        HidP_GetCaps(preparsed,&caps);
        HidD_FreePreparsedData(preparsed);
    }
    snprintf(seen,sizeof(seen),"usage %04x:%04x, input %u, output %u, feature %u",caps.UsagePage,caps.Usage,
             caps.InputReportByteLength,caps.OutputReportByteLength,caps.FeatureReportByteLength);
    check(caps.UsagePage==1 && caps.Usage==5 && caps.InputReportByteLength==64 && caps.OutputReportByteLength==32 &&
          caps.FeatureReportByteLength>=49,"a gamepad with the DualShock 4's report lengths",seen);
}

static short load16(const BYTE *p) { return (short)(p[0]|p[1]<<8); }

static void check_features(void)
{
    BYTE data[256];
    char seen[256];
    BOOL got;

    /* SDL: scale = 16 * (speed+ + speed-) / (|pitch+| + |pitch-|), accepted within 0.5 of 1, bias at most 1024. */
    memset(data,0,sizeof(data));data[0]=0x02;
    got=HidD_GetFeature(device,data,caps.FeatureReportByteLength);
    {
        double scale=16.0*(load16(data+19)+load16(data+21))/(abs(load16(data+7))+abs(load16(data+9)));
        double accel=2.0*8192/(load16(data+23)-load16(data+25));
        snprintf(seen,sizeof(seen),"got %d, bias %d %d %d, gyro scale %.3f, accel scale %.3f",got,load16(data+1),
                 load16(data+3),load16(data+5),scale,accel);
        check(got && !load16(data+1) && scale>0.99 && scale<1.01 && accel>0.99 && accel<1.01,
              "feature 0x02: a calibration with a scale of 1",seen);
    }
    memset(data,0,sizeof(data));data[0]=0x12;
    got=HidD_GetFeature(device,data,caps.FeatureReportByteLength);
    snprintf(seen,sizeof(seen),"got %d, address %02x:%02x:%02x:%02x:%02x:%02x",got,data[6],data[5],data[4],data[3],data[2],data[1]);
    check(got && (data[1]|data[2]|data[3]|data[4]|data[5]|data[6]),"feature 0x12: the controller's address",seen);
}

/* The newest input report the device sent (HidD_GetInputReport). */
static int read_report(BYTE *report)
{
    memset(report,0,64);
    report[0]=0x01;
    return HidD_GetInputReport(device,report,caps.InputReportByteLength);
}

static void check_input(void)
{
    BYTE report[64],again[64];
    char seen[512];
    int x,y,got;

    set_pad(1,A|LB|START,255,0,32767,32767,-32768,-32768,TOUCHPAD|SHARE,1,1919,1079);
    got=read_report(report);
    x=report[36]|(report[37]&0x0f)<<8;
    y=report[37]>>4|report[38]<<4;
    snprintf(seen,sizeof(seen),"got %d, id %u, sticks %u %u %u %u, buttons %02x %02x %02x, triggers %u %u, battery %02x, "
             "touch %02x %d,%d, second %02x",got,report[0],report[1],report[2],report[3],report[4],report[5],report[6],
             report[7]&3,report[8],report[9],report[30],report[35],x,y,report[39]);
    check(got && report[0]==1 && report[1]==255 && report[2]==0 && report[3]==0 && report[4]==255 &&
          report[5]==(0x20|8) && report[6]==(0x01|0x04|0x10|0x20) && (report[7]&3)==0x02 && report[8]==255 &&
          !report[9] && report[30]==0x1b && !(report[35]&0x80) && x==1919 && y==942 && (report[39]&0x80) &&
          !(report[31]&0x04),
          "report 0x01: Cross, L1, L2, Share, Options, the touchpad's click, sticks, the finger",seen);
    Sleep(150);  /* an unchanged state is sent again every 100 ms */
    read_report(again);
    snprintf(seen,sizeof(seen),"counter %u then %u, timestamp %u then %u",report[7]>>2,again[7]>>2,
             report[10]|report[11]<<8,again[10]|again[11]<<8);
    check(report[7]>>2!=again[7]>>2 && (report[10]|report[11]<<8)!=(again[10]|again[11]<<8),
          "the report counter and timestamp move",seen);

    {
        BYTE queued[64];
        DWORD read=0;
        got=ReadFile(device,queued,caps.InputReportByteLength,&read,NULL);
        snprintf(seen,sizeof(seen),"got %d, %lu bytes, id %u",got,read,queued[0]);
        check(got && read==64 && queued[0]==1,"ReadFile delivers 64-byte input reports",seen);
    }

    set_pad(1,B|X|Y|L3|UP|RIGHT,0,128,0,0,0,0,0,0,0,0);
    got=read_report(report);
    snprintf(seen,sizeof(seen),"got %d, sticks %u %u, buttons %02x %02x, R2 %u, touch %02x",got,report[1],report[2],
             report[5],report[6],report[9],report[35]);
    check(got && report[1]==0x80 && report[2]==0x80 && report[5]==(0x10|0x40|0x80|1) && report[6]==(0x40|0x08) &&
          report[9]==128 && (report[35]&0x80),"Square, Circle, Triangle, L3, R2, the hat up-right, no finger",seen);

    /* A disconnected pad, or one without extras, reads neutral: sticks centred, hat centred. */
    set_pad(0,A,255,255,32767,0,0,0,TOUCHPAD,1,10,10);
    got=read_report(report);
    snprintf(seen,sizeof(seen),"got %d, sticks %u %u %u %u, buttons %02x %02x %02x, triggers %u %u, touch %02x",got,
             report[1],report[2],report[3],report[4],report[5],report[6],report[7]&3,report[8],report[9],report[35]);
    check(got && report[1]==0x80 && report[2]==0x80 && report[5]==8 && !report[6] && !(report[7]&3) && !report[8] &&
          (report[35]&0x80),"a disconnected pad reads neutral",seen);
}

/* The newest rumble the sink wrote, as "left right", or "none". */
static void last_rumble(long *left,long *right)
{
    char line[64];
    long l,r;
    FILE *f=fopen(rumble_path,"r");
    *left=*right=-1;
    if(!f)return;
    while(fgets(line,sizeof(line),f))
        if(sscanf(line,"%ld %ld",&l,&r)==2) { *left=l; *right=r; }
    fclose(f);
}

static void check_output(void)
{
    BYTE report[64];
    DWORD written=0;
    char seen[256];
    long left,right;
    BOOL ok;

    memset(report,0,sizeof(report));
    report[0]=0x05;report[1]=0x01;report[4]=0x40;report[5]=0xff;
    ok=WriteFile(device,report,caps.OutputReportByteLength,&written,NULL);
    Sleep(50);
    last_rumble(&left,&right);
    snprintf(seen,sizeof(seen),"write %d, %lu bytes, left %ld right %ld",ok,written,left,right);
    check(ok && left==65535 && right==0x40*257,"output 0x05 runs the motors, large left, small right",seen);

    /* The light bar alone leaves the motors as they are. */
    memset(report,0,sizeof(report));
    report[0]=0x05;report[1]=0x02;report[6]=0xff;
    ok=HidD_SetOutputReport(device,report,caps.OutputReportByteLength);
    Sleep(50);
    last_rumble(&left,&right);
    snprintf(seen,sizeof(seen),"set %d, left %ld right %ld",ok,left,right);
    check(ok && left==65535 && right==0x40*257,"a light bar report leaves the motors",seen);

    memset(report,0,sizeof(report));
    report[0]=0x05;report[1]=0x07;
    ok=WriteFile(device,report,caps.OutputReportByteLength,&written,NULL);
    Sleep(50);
    last_rumble(&left,&right);
    snprintf(seen,sizeof(seen),"write %d, left %ld right %ld",ok,left,right);
    check(ok && !left && !right,"a report with zero speeds stops them",seen);
}

static void check_xinput(void)
{
    XINPUT_STATE state;
    XINPUT_VIBRATION vibration={65535,65535};
    char seen[128];
    DWORD get,set;
    long left,right;

    set_pad(1,A,0,0,0,0,0,0,0,0,0,0);
    get=XInputGetState(0,&state);
    set=XInputSetState(0,&vibration);
    XInputEnable(FALSE);
    XInputEnable(TRUE);
    last_rumble(&left,&right);
    snprintf(seen,sizeof(seen),"get %lu, set %lu, rumble %ld %ld",get,set,left,right);
    check(get==ERROR_DEVICE_NOT_CONNECTED && set==ERROR_DEVICE_NOT_CONNECTED && !left && !right,
          "XInput reports no controller and leaves the motors",seen);
}

static void check_rawinput(void)
{
    RAWINPUTDEVICELIST list[64];
    UINT count=ARRAYSIZE(list),i,found=0;
    char seen[128];

    count=GetRawInputDeviceList(list,&count,sizeof(list[0]));
    for(i=0;count!=(UINT)-1 && i<count;i++) {
        RID_DEVICE_INFO info;
        UINT size=sizeof(info);
        info.cbSize=sizeof(info);
        if(list[i].dwType!=RIM_TYPEHID)continue;
        if(GetRawInputDeviceInfoA(list[i].hDevice,RIDI_DEVICEINFO,&info,&size)==(UINT)-1)continue;
        if(info.hid.usUsagePage==1 && info.hid.usUsage==5 && info.hid.dwVendorId==0x054c && info.hid.dwProductId==0x09cc)
            found++;
    }
    snprintf(seen,sizeof(seen),"%d devices, %u DualShock 4",(int)count,found);
    check(found==1,"Raw Input lists the DualShock 4",seen);
}

static int dinput_found;
static DWORD dinput_product;

static BOOL CALLBACK enum_device(const DIDEVICEINSTANCEW *instance,void *context)
{
    (void)context;
    if(!dinput_found++)dinput_product=instance->guidProduct.Data1;
    return DIENUM_CONTINUE;
}

static void check_dinput(void)
{
    IDirectInput8W *dinput;
    char seen[128];
    HRESULT hr=DirectInput8Create(GetModuleHandleW(NULL),DIRECTINPUT_VERSION,&IID_IDirectInput8W,(void **)&dinput,NULL);

    if(SUCCEEDED(hr)) {
        hr=IDirectInput8_EnumDevices(dinput,DI8DEVCLASS_GAMECTRL,enum_device,NULL,DIEDFL_ATTACHEDONLY);
        IDirectInput8_Release(dinput);
    }
    snprintf(seen,sizeof(seen),"hr %#lx, %d devices, product %08lx",hr,dinput_found,dinput_product);
    check(SUCCEEDED(hr) && dinput_found==1 && dinput_product==MAKELONG(0x054c,0x09cc),
          "DirectInput lists one DualShock 4",seen);
}

/* The paths of the DualShock 4s there are now, in the order programs list them. */
static int list_ds4(char paths[][512],int max)
{
    GUID hid;
    HDEVINFO set;
    SP_DEVICE_INTERFACE_DATA data;
    int found=0;

    HidD_GetHidGuid(&hid);
    set=SetupDiGetClassDevsA(&hid,NULL,NULL,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE);
    if(set==INVALID_HANDLE_VALUE)return 0;
    data.cbSize=sizeof(data);
    for(DWORD i=0;SetupDiEnumDeviceInterfaces(set,NULL,&hid,i,&data);i++) {
        char buffer[sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A)+512];
        SP_DEVICE_INTERFACE_DETAIL_DATA_A *detail=(void *)buffer;
        detail->cbSize=sizeof(*detail);
        if(!SetupDiGetDeviceInterfaceDetailA(set,&data,detail,sizeof(buffer),NULL,NULL))continue;
        if(!StrStrIA(detail->DevicePath,"vid_054c&pid_09cc"))continue;
        if(found<max)lstrcpynA(paths[found],detail->DevicePath,512);
        found++;
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

static int ds4_until(char paths[][512],int want)
{
    int found=0;
    for(int i=0;i<40;i++) {
        found=list_ds4(paths,4);
        if(found==want)break;
        Sleep(200);
    }
    return found;
}

static void set_second(int connected,unsigned buttons)
{
    char path[MAX_PATH+8];
    FILE *f;

    snprintf(path,sizeof(path),"%s.1",pad_path);
    f=fopen(path,"w");
    if(!f) { printf("FAIL cannot write %s\n",path); exit(2); }
    fprintf(f,"%d %u 0 0 0 0 0 0 0 0 0 0\n",connected,buttons);
    fclose(f);
}

static void check_second(void)
{
    char paths[4][512],seen[256],path[MAX_PATH+8],line[64];
    BYTE first[7]={0x81},second[7]={0x81},report[64]={0x01},output[32]={0x05,0x01};
    HANDLE handles[2];
    int found,cross[2];
    long left=-1,value,right;
    FILE *f;

    set_pad(1,0,0,0,0,0,0,0,0,0,0,0);
    set_second(0,0);
    found=ds4_until(paths,1);
    snprintf(seen,sizeof(seen),"%d devices",found);
    check(found==1,"a controller that is not connected has no DualShock 4",seen);
    set_second(1,0);
    found=ds4_until(paths,2);
    snprintf(seen,sizeof(seen),"%d devices",found);
    check(found==2,"a connected second controller is a second DualShock 4",seen);
    if(found!=2)return;
    for(int i=0;i<2;i++)
        handles[i]=CreateFileA(paths[i],GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);
    if(handles[0]==INVALID_HANDLE_VALUE || handles[1]==INVALID_HANDLE_VALUE) {
        check(0,"both DualShock 4s open","");
        return;
    }
    HidD_GetFeature(handles[0],first,sizeof(first));
    HidD_GetFeature(handles[1],second,sizeof(second));
    snprintf(seen,sizeof(seen),"low bytes %02x, %02x",first[1],second[1]);
    check(first[1]!=second[1] && !memcmp(first+2,second+2,5),"each has its own address",seen);
    set_second(1,0x1000);   /* A, the second controller's cross */
    Sleep(400);
    for(int i=0;i<2;i++) {
        report[0]=0x01;
        HidD_GetInputReport(handles[i],report,sizeof(report));
        cross[i]=(report[5]&0x20)!=0;
    }
    snprintf(seen,sizeof(seen),"cross on device 0 %d, device 1 %d",cross[0],cross[1]);
    check(!cross[0] && cross[1],"the second controller's button is on the second device",seen);
    snprintf(path,sizeof(path),"%s.1",rumble_path);
    f=fopen(path,"w");
    if(f)fclose(f);
    output[5]=255;
    HidD_SetOutputReport(handles[1],output,sizeof(output));
    Sleep(200);
    f=fopen(path,"r");
    while(f && fgets(line,sizeof(line),f))
        if(sscanf(line,"%ld %ld",&value,&right)==2)left=value;
    if(f)fclose(f);
    snprintf(seen,sizeof(seen),"left motor on controller 1: %ld",left);
    check(left>60000,"its output report reaches its own rumble",seen);
    CloseHandle(handles[0]);
    CloseHandle(handles[1]);
    set_second(0,0);
    found=ds4_until(paths,1);
    snprintf(seen,sizeof(seen),"%d devices",found);
    check(found==1,"the DualShock 4 goes with its controller",seen);
}

int main(int argc,char **argv)
{
    if(argc<3) { printf("usage: pw_hid_ds4 PADFILE RUMBLEFILE\n"); return 2; }
    pad_path=argv[1];
    rumble_path=argv[2];
    setvbuf(stdout,NULL,_IONBF,0);
    /* A hosted game that looks once, with a second controller there from the start. */
    if(argc>3 && !strcmp(argv[3],"at-start")) {
        char paths[4][512],seen[64];
        int found=list_ds4(paths,4);
        snprintf(seen,sizeof(seen),"%d devices",found);
        check(found==2,"the first look finds a controller there from the start",seen);
        return failures?1:0;
    }
    set_pad(1,0,0,0,0,0,0,0,0,0,0,0);
    find_device();
    if(device!=INVALID_HANDLE_VALUE) {
        check_features();
        check_input();
        check_output();
        CloseHandle(device);
    }
    check_xinput();
    check_rawinput();
    check_dinput();
    check_second();
    printf("%s\n",failures?"FAILED":"PASSED");
    return failures?1:0;
}
