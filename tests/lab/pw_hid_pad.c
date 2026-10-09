/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The PS5 title's controller as Windows games find it without XInput: the
 * HID gamepad winebus's PS5 bus makes from the title's pad. Run by
 * tools/test_wine_hid_pad.py under a host Wine whose winedevice has
 * tests/lab/pw_hid_pad_sink.c preloaded. argv[1] is the sink's pad file as
 * a Windows path; this program writes the states it checks there.
 *
 * Checks, each printed as "ok"/"FAIL" with what was seen:
 * - DirectInput 8 lists exactly one game controller, an Xbox 360 one
 *   (045e:028e), and reads its buttons, sticks and d-pad;
 * - winmm (joyGetPosEx) reads the same pad;
 * - a disconnected pad reads as a neutral one.
 * Exit status 0 only when every check passed. */
#define DIRECTINPUT_VERSION 0x0800
#define COBJMACROS
#include <windows.h>
#include <dinput.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *pad_path;
static int failures;

static void check(int ok,const char *what,const char *seen)
{
    printf("%s %s (%s)\n",ok?"ok":"FAIL",what,seen);
    if(!ok)failures++;
}

/* XINPUT_GAMEPAD bits. */
enum { UP=0x1,DOWN=0x2,LEFT=0x4,RIGHT=0x8,START=0x10,BACK=0x20,A=0x1000,B=0x2000,X=0x4000,Y=0x8000 };

static void set_pad(int connected,unsigned buttons,int lt,int rt,int lx,int ly,int rx,int ry)
{
    FILE *f=fopen(pad_path,"w");
    if(!f) { printf("FAIL cannot write %s\n",pad_path); exit(2); }
    fprintf(f,"%d %u %d %d %d %d %d %d\n",connected,buttons,lt,rt,lx,ly,rx,ry);
    fclose(f);
    Sleep(200);  /* the bus polls every few milliseconds; reports then pass through hidclass */
}

static IDirectInput8W *dinput;
static GUID found_instance;
static int found;
static DWORD found_product;
static WCHAR found_name[MAX_PATH];

static BOOL CALLBACK enum_device(const DIDEVICEINSTANCEW *instance,void *context)
{
    (void)context;
    if(!found++) {
        found_instance=instance->guidInstance;
        found_product=instance->guidProduct.Data1;
        lstrcpynW(found_name,instance->tszProductName,MAX_PATH);
    }
    return DIENUM_CONTINUE;
}

static void check_dinput(HWND window)
{
    IDirectInputDevice8W *device;
    DIJOYSTATE2 state;
    char seen[512];
    HRESULT hr;

    hr=DirectInput8Create(GetModuleHandleW(NULL),DIRECTINPUT_VERSION,&IID_IDirectInput8W,(void **)&dinput,NULL);
    snprintf(seen,sizeof(seen),"hr %#lx",hr);
    check(hr==DI_OK,"DirectInput8Create",seen);
    if(hr!=DI_OK)return;
    hr=IDirectInput8_EnumDevices(dinput,DI8DEVCLASS_GAMECTRL,enum_device,NULL,DIEDFL_ATTACHEDONLY);
    snprintf(seen,sizeof(seen),"hr %#lx, %d devices, first %.64ls, product %08lx",hr,found,found_name,found_product);
    check(hr==DI_OK && found==1 && found_product==MAKELONG(0x045e,0x028e),"DirectInput lists one Xbox 360 controller",seen);
    if(!found)return;

    hr=IDirectInput8_CreateDevice(dinput,&found_instance,&device,NULL);
    if(SUCCEEDED(hr))hr=IDirectInputDevice8_SetDataFormat(device,&c_dfDIJoystick2);
    if(SUCCEEDED(hr))hr=IDirectInputDevice8_SetCooperativeLevel(device,window,DISCL_BACKGROUND|DISCL_NONEXCLUSIVE);
    if(SUCCEEDED(hr))hr=IDirectInputDevice8_Acquire(device);
    snprintf(seen,sizeof(seen),"hr %#lx",hr);
    check(SUCCEEDED(hr),"DirectInput opens the controller",seen);
    if(FAILED(hr))return;

    set_pad(1,A|UP,255,0,32767,0,0,-32768);
    IDirectInputDevice8_Poll(device);
    hr=IDirectInputDevice8_GetDeviceState(device,sizeof(state),&state);
    snprintf(seen,sizeof(seen),"hr %#lx, buttons %02x %02x %02x %02x, x %ld y %ld z %ld rx %ld ry %ld rz %ld, pov %lu",
             hr,state.rgbButtons[0],state.rgbButtons[1],state.rgbButtons[2],state.rgbButtons[3],
             state.lX,state.lY,state.lZ,state.lRx,state.lRy,state.lRz,state.rgdwPOV[0]);
    check(hr==DI_OK && (state.rgbButtons[0]&0x80) && !(state.rgbButtons[1]&0x80) && state.lX>60000 &&
          state.rgdwPOV[0]==0,"DirectInput reads A, the left stick right and d-pad up",seen);

    set_pad(1,B|RIGHT,0,0,-32768,32767,0,0);
    IDirectInputDevice8_Poll(device);
    hr=IDirectInputDevice8_GetDeviceState(device,sizeof(state),&state);
    snprintf(seen,sizeof(seen),"hr %#lx, buttons %02x %02x, x %ld y %ld, pov %lu",
             hr,state.rgbButtons[0],state.rgbButtons[1],state.lX,state.lY,state.rgdwPOV[0]);
    check(hr==DI_OK && !(state.rgbButtons[0]&0x80) && (state.rgbButtons[1]&0x80) && state.lX<5000 &&
          state.lY<5000 && state.rgdwPOV[0]==9000,"DirectInput reads B, the left stick up-left and d-pad right",seen);

    set_pad(0,0,0,0,0,0,0,0);
    IDirectInputDevice8_Poll(device);
    hr=IDirectInputDevice8_GetDeviceState(device,sizeof(state),&state);
    snprintf(seen,sizeof(seen),"hr %#lx, buttons %02x %02x, x %ld y %ld, pov %lu",
             hr,state.rgbButtons[0],state.rgbButtons[1],state.lX,state.lY,state.rgdwPOV[0]);
    check(hr==DI_OK && !(state.rgbButtons[0]&0x80) && state.lX>30000 && state.lX<35000 &&
          LOWORD(state.rgdwPOV[0])==0xffff,"A disconnected pad reads neutral",seen);

    IDirectInputDevice8_Unacquire(device);
    IDirectInputDevice8_Release(device);
}

static void check_winmm(void)
{
    JOYINFOEX info;
    char seen[512];
    UINT devices=joyGetNumDevs();
    MMRESULT result;

    memset(&info,0,sizeof(info));
    info.dwSize=sizeof(info);
    info.dwFlags=JOY_RETURNALL;
    /* winmm opens the controller on the first call and then sees the reports that follow. */
    joyGetPosEx(JOYSTICKID1,&info);
    set_pad(1,X,0,0,0,32767,0,0);
    result=joyGetPosEx(JOYSTICKID1,&info);
    snprintf(seen,sizeof(seen),"%u devices, result %u, buttons %#lx, x %lu y %lu, pov %lu",
             devices,result,info.dwButtons,info.dwXpos,info.dwYpos,info.dwPOV);
    check(result==JOYERR_NOERROR && (info.dwButtons&4) && info.dwYpos<5000,"winmm reads X and the left stick up",seen);
}

int main(int argc,char **argv)
{
    HWND window;

    if(argc<2) { printf("usage: pw_hid_pad PADFILE\n"); return 2; }
    pad_path=argv[1];
    setvbuf(stdout,NULL,_IONBF,0);
    window=CreateWindowA("STATIC","pw_hid_pad",WS_POPUP,0,0,1,1,NULL,NULL,NULL,NULL);
    set_pad(1,0,0,0,0,0,0,0);
    check_dinput(window);
    check_winmm();
    if(dinput)IDirectInput8_Release(dinput);
    printf("%s\n",failures?"FAILED":"PASSED");
    return failures?1:0;
}
