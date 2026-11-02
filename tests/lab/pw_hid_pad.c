/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The PS5 title's controller as Windows games find it without XInput: the
 * HID gamepad winebus's PS5 bus makes from the title's pad. Run by
 * tools/test_wine_hid_pad.py under a host Wine with
 * tests/lab/pw_hid_pad_sink.c preloaded. argv[1] is the sink's pad file as
 * a Windows path; this program writes the states it checks there. argv[2],
 * if given, is the sink's rumble file.
 *
 * Checks, each printed as "ok"/"FAIL" with what was seen:
 * - DirectInput 8 lists exactly one game controller, an Xbox 360 one
 *   (045e:028e), and reads its buttons, sticks and d-pad;
 * - winmm (joyGetPosEx) reads the same pad;
 * - a disconnected pad reads as a neutral one;
 * - Raw Input lists the HID gamepad;
 * - Windows.Gaming.Input (modern Unity) lists one gamepad and reads it, and
 *   its vibration reaches the title's rumble and stops at the haptics
 *   cutoff;
 * - opening a HID device asked the title for controllers 1..3;
 * - a second controller is a second device only while it is connected, reads
 *   apart from the first, and comes back after going.
 * Exit status 0 only when every check passed. */
#define DIRECTINPUT_VERSION 0x0800
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <dinput.h>
#include <mmsystem.h>
#include <roapi.h>
#include <winstring.h>
#define WIDL_using_Windows_Foundation
#define WIDL_using_Windows_Foundation_Collections
#include <windows.foundation.h>
#define WIDL_using_Windows_Gaming_Input
#include <windows.gaming.input.h>
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

static void check_rawinput(void)
{
    RAWINPUTDEVICELIST list[64];
    UINT count=ARRAYSIZE(list),i,gamepads=0;
    char seen[256];

    count=GetRawInputDeviceList(list,&count,sizeof(list[0]));
    for(i=0;count!=(UINT)-1 && i<count;i++) {
        RID_DEVICE_INFO info;
        UINT size=sizeof(info);
        info.cbSize=sizeof(info);
        if(list[i].dwType!=RIM_TYPEHID)continue;
        if(GetRawInputDeviceInfoA(list[i].hDevice,RIDI_DEVICEINFO,&info,&size)==(UINT)-1)continue;
        if(info.hid.usUsagePage==1 && info.hid.usUsage==5 && info.hid.dwVendorId==0x045e && info.hid.dwProductId==0x028e)
            gamepads++;
    }
    snprintf(seen,sizeof(seen),"%d devices, %u Xbox 360 gamepads",(int)count,gamepads);
    check(gamepads>=1,"Raw Input lists the HID gamepad",seen);
}

/* The left motor of the newest rumble the sink wrote, or -1. */
static long last_rumble_left(void)
{
    char line[64];
    long left=-1,value,right;
    FILE *f=rumble_path?fopen(rumble_path,"r"):NULL;
    if(!f)return -1;
    while(fgets(line,sizeof(line),f))
        if(sscanf(line,"%ld %ld",&value,&right)==2)left=value;
    fclose(f);
    return left;
}

static void check_wgi(void)
{
    static const WCHAR name[]=L"Windows.Gaming.Input.Gamepad";
    __FIVectorView_1_Windows__CGaming__CInput__CGamepad *gamepads=NULL;
    IGamepadStatics *statics=NULL;
    IGamepad *gamepad=NULL;
    struct GamepadReading reading;
    struct GamepadVibration vibration={0};
    char seen[256];
    HSTRING string;
    UINT32 size=0;
    HRESULT hr;
    long rumble;

    RoInitialize(RO_INIT_MULTITHREADED);
    WindowsCreateString(name,ARRAYSIZE(name)-1,&string);
    hr=RoGetActivationFactory(string,&IID_IGamepadStatics,(void **)&statics);
    WindowsDeleteString(string);
    /* It looks for controllers on its own thread once the factory exists, so ask again for a
     * while, as a game does every frame. */
    for(int tries=0;SUCCEEDED(hr) && size==0 && tries<100;tries++) {
        if(gamepads) { IVectorView_Gamepad_Release(gamepads); gamepads=NULL; Sleep(20); }
        hr=IGamepadStatics_get_Gamepads(statics,&gamepads);
        if(SUCCEEDED(hr))hr=IVectorView_Gamepad_get_Size(gamepads,&size);
    }
    snprintf(seen,sizeof(seen),"hr %#lx, %u gamepads",hr,size);
    check(SUCCEEDED(hr) && size==1,"Windows.Gaming.Input lists one gamepad",seen);
    if(size<1)goto done;
    IVectorView_Gamepad_GetAt(gamepads,0,&gamepad);

    set_pad(1,Y,0,255,0,0,32767,0);
    hr=IGamepad_GetCurrentReading(gamepad,&reading);
    snprintf(seen,sizeof(seen),"hr %#lx, buttons %#x, right trigger %.2f, right stick x %.2f",
             hr,(unsigned)reading.Buttons,reading.RightTrigger,reading.RightThumbstickX);
    check(SUCCEEDED(hr) && (reading.Buttons&GamepadButtons_Y) && reading.RightTrigger>0.9 &&
          reading.RightThumbstickX>0.9,"Windows.Gaming.Input reads Y, the right trigger and the right stick",seen);

    vibration.LeftMotor=1.0;
    vibration.RightMotor=0.5;
    hr=IGamepad_put_Vibration(gamepad,vibration);
    Sleep(200);
    rumble=last_rumble_left();
    snprintf(seen,sizeof(seen),"hr %#lx, left motor %ld",hr,rumble);
    check(SUCCEEDED(hr) && rumble>60000,"Windows.Gaming.Input vibration reaches the title's rumble",seen);
    /* The HID haptics report's cutoff (1 s by default) stops a vibration nobody renews. */
    Sleep(1300);
    rumble=last_rumble_left();
    snprintf(seen,sizeof(seen),"left motor %ld",rumble);
    check(rumble==0,"the title's rumble stops at the haptics cutoff",seen);

done:
    if(gamepad)IGamepad_Release(gamepad);
    if(gamepads)IVectorView_Gamepad_Release(gamepads);
    if(statics)IGamepadStatics_Release(statics);
}

static void set_extra(int slot,int connected,unsigned buttons)
{
    char path[MAX_PATH+8];
    FILE *f;

    snprintf(path,sizeof(path),"%s.%d",pad_path,slot);
    f=fopen(path,"w");
    if(!f) { printf("FAIL cannot write %s\n",path); exit(2); }
    fprintf(f,"%d %u 0 0 0 0 0 0\n",connected,buttons);
    fclose(f);
}

static GUID extra_instances[8];
static int extra_found;

static BOOL CALLBACK enum_extra(const DIDEVICEINSTANCEW *instance,void *context)
{
    (void)context;
    if(extra_found<8)extra_instances[extra_found]=instance->guidInstance;
    extra_found++;
    return DIENUM_CONTINUE;
}

/* The other HID APIs with a second controller connected: each lists it, and reads it apart. */
static void check_players_elsewhere(void)
{
    static const WCHAR name[]=L"Windows.Gaming.Input.Gamepad";
    __FIVectorView_1_Windows__CGaming__CInput__CGamepad *gamepads=NULL;
    IGamepadStatics *statics=NULL;
    RAWINPUTDEVICELIST list[64];
    UINT count=ARRAYSIZE(list),i,xbox=0;
    char seen[256];
    JOYINFOEX first,second;
    HSTRING string;
    UINT32 size=0;

    count=GetRawInputDeviceList(list,&count,sizeof(list[0]));
    for(i=0;count!=(UINT)-1 && i<count;i++) {
        RID_DEVICE_INFO info;
        UINT bytes=sizeof(info);
        info.cbSize=sizeof(info);
        if(list[i].dwType!=RIM_TYPEHID)continue;
        if(GetRawInputDeviceInfoA(list[i].hDevice,RIDI_DEVICEINFO,&info,&bytes)==(UINT)-1)continue;
        if(info.hid.dwVendorId==0x045e && info.hid.dwProductId==0x028e && info.hid.usUsage==5)xbox++;
    }
    snprintf(seen,sizeof(seen),"%u Xbox 360 gamepads",xbox);
    check(xbox==2,"Raw Input lists both controllers",seen);

    memset(&first,0,sizeof(first));first.dwSize=sizeof(first);first.dwFlags=JOY_RETURNALL;
    second=first;
    joyConfigChanged(0);
    joyGetPosEx(JOYSTICKID1,&first);
    joyGetPosEx(JOYSTICKID2,&second);
    set_pad(1,0,0,0,0,0,0,0);
    set_extra(1,1,A);
    Sleep(800);
    {
        MMRESULT one=joyGetPosEx(JOYSTICKID1,&first),two=joyGetPosEx(JOYSTICKID2,&second);
        snprintf(seen,sizeof(seen),"joystick 1 result %u buttons %#lx, joystick 2 result %u buttons %#lx",one,first.dwButtons,two,second.dwButtons);
        check(one==JOYERR_NOERROR && two==JOYERR_NOERROR && !(first.dwButtons&1) && (second.dwButtons&1),
              "winmm reads the second controller as joystick 2",seen);
    }
    set_extra(1,1,0);

    /* A hosted game has no plugplay service: Windows.Gaming.Input looks once, at its start (check_at_start). */
    if(GetEnvironmentVariableA("WINE_NTOSKRNL_HOSTED",NULL,0))return;
    WindowsCreateString(name,ARRAYSIZE(name)-1,&string);
    RoGetActivationFactory(string,&IID_IGamepadStatics,(void **)&statics);
    WindowsDeleteString(string);
    for(i=0;statics && i<100 && size!=2;i++) {
        if(gamepads) { IVectorView_Gamepad_Release(gamepads); gamepads=NULL; Sleep(50); }
        if(SUCCEEDED(IGamepadStatics_get_Gamepads(statics,&gamepads)))IVectorView_Gamepad_get_Size(gamepads,&size);
    }
    snprintf(seen,sizeof(seen),"%u gamepads",size);
    check(size==2,"Windows.Gaming.Input lists both controllers",seen);
    if(gamepads)IVectorView_Gamepad_Release(gamepads);
    if(statics)IGamepadStatics_Release(statics);
}

/* Windows.Gaming.Input with both controllers there when it first looks. */
static void check_wgi_two(void)
{
    static const WCHAR name[]=L"Windows.Gaming.Input.Gamepad";
    __FIVectorView_1_Windows__CGaming__CInput__CGamepad *gamepads=NULL;
    IGamepadStatics *statics=NULL;
    char seen[256],path[MAX_PATH+8];
    HSTRING string;
    UINT32 size=0,i;
    int who=-1;

    RoInitialize(RO_INIT_MULTITHREADED);
    WindowsCreateString(name,ARRAYSIZE(name)-1,&string);
    RoGetActivationFactory(string,&IID_IGamepadStatics,(void **)&statics);
    WindowsDeleteString(string);
    for(i=0;statics && i<100 && size!=2;i++) {
        if(gamepads) { IVectorView_Gamepad_Release(gamepads); gamepads=NULL; Sleep(50); }
        if(SUCCEEDED(IGamepadStatics_get_Gamepads(statics,&gamepads)))IVectorView_Gamepad_get_Size(gamepads,&size);
    }
    snprintf(seen,sizeof(seen),"%u gamepads",size);
    check(size==2,"Windows.Gaming.Input lists both controllers",seen);
    if(size==2) {
        IGamepad *pads[2];
        struct GamepadReading reading[2];
        struct GamepadVibration vibration={0};
        long rumble=-1;
        IVectorView_Gamepad_GetAt(gamepads,0,&pads[0]);
        IVectorView_Gamepad_GetAt(gamepads,1,&pads[1]);
        set_extra(1,1,Y);
        Sleep(800);
        IGamepad_GetCurrentReading(pads[0],&reading[0]);
        IGamepad_GetCurrentReading(pads[1],&reading[1]);
        if((reading[0].Buttons&GamepadButtons_Y)!=(reading[1].Buttons&GamepadButtons_Y))
            who=(reading[0].Buttons&GamepadButtons_Y)?0:1;
        snprintf(seen,sizeof(seen),"gamepad 0 buttons %#x, gamepad 1 buttons %#x",(unsigned)reading[0].Buttons,(unsigned)reading[1].Buttons);
        check(who>=0,"Windows.Gaming.Input reads one controller's button on one gamepad",seen);
        set_extra(1,1,0);
        if(who>=0) {
            FILE *f;
            char line[64];
            long value,right;
            HRESULT hr;
            /* Only this run's: the file outlives a program, and the next would pass on it. */
            snprintf(path,sizeof(path),"%s.1",rumble_path?rumble_path:"");
            f=fopen(path,"w");
            if(f)fclose(f);
            vibration.LeftMotor=1.0;
            hr=IGamepad_put_Vibration(pads[who],vibration);
            snprintf(seen,sizeof(seen),"hr %#lx",hr);
            check(SUCCEEDED(hr),"Windows.Gaming.Input accepts the second gamepad's vibration",seen);
            Sleep(300);
            f=fopen(path,"r");
            while(f && fgets(line,sizeof(line),f))
                if(sscanf(line,"%ld %ld",&value,&right)==2 && value>rumble)rumble=value;
            if(f)fclose(f);
            snprintf(seen,sizeof(seen),"strongest left motor on controller 1: %ld",rumble);
            check(rumble>60000,"its vibration reaches that controller's rumble",seen);
            vibration.LeftMotor=0;
            IGamepad_put_Vibration(pads[who],vibration);
        }
        IGamepad_Release(pads[0]);
        IGamepad_Release(pads[1]);
    }
    if(gamepads)IVectorView_Gamepad_Release(gamepads);
    if(statics)IGamepadStatics_Release(statics);
}

/* How many controllers DirectInput lists once the count settles on want, up to a few seconds. */
static int count_until(int want)
{
    for(int i=0;i<40;i++) {
        extra_found=0;
        IDirectInput8_EnumDevices(dinput,DI8DEVCLASS_GAMECTRL,enum_extra,NULL,DIEDFL_ATTACHEDONLY);
        if(extra_found==want)break;
        Sleep(200);
    }
    return extra_found;
}

static void check_players(HWND window)
{
    char path[MAX_PATH+8],seen[256];
    IDirectInputDevice8W *devices[2]={NULL,NULL};
    DIJOYSTATE2 state[2];
    unsigned mask=0;
    FILE *f;
    int count,other=-1;

    if(!dinput)return;
    snprintf(path,sizeof(path),"%s.asked",pad_path);
    f=fopen(path,"r");
    if(f){if(fscanf(f,"%u",&mask)!=1)mask=0;fclose(f);}
    snprintf(seen,sizeof(seen),"mask %#x",mask);
    check(mask==0xe,"opening a HID device asks the title for controllers 1..3",seen);

    set_pad(1,0,0,0,0,0,0,0);
    set_extra(1,0,0);
    count=count_until(1);
    snprintf(seen,sizeof(seen),"%d devices",count);
    check(count==1,"a controller that is not connected has no device",seen);

    set_extra(1,1,0);
    count=count_until(2);
    snprintf(seen,sizeof(seen),"%d devices",count);
    check(count==2,"a connected second controller is a second device",seen);
    if(count!=2)return;
    check(memcmp(&extra_instances[0],&extra_instances[1],sizeof(GUID))!=0,"the two devices have their own instances","");

    for(int i=0;i<2;i++) {
        HRESULT hr=IDirectInput8_CreateDevice(dinput,&extra_instances[i],&devices[i],NULL);
        if(SUCCEEDED(hr))hr=IDirectInputDevice8_SetDataFormat(devices[i],&c_dfDIJoystick2);
        if(SUCCEEDED(hr))hr=IDirectInputDevice8_SetCooperativeLevel(devices[i],window,DISCL_BACKGROUND|DISCL_NONEXCLUSIVE);
        if(SUCCEEDED(hr))hr=IDirectInputDevice8_Acquire(devices[i]);
        snprintf(seen,sizeof(seen),"device %d, hr %#lx",i,hr);
        check(SUCCEEDED(hr),"DirectInput opens both controllers",seen);
        if(FAILED(hr))goto done;
    }
    set_pad(1,0,0,0,0,0,0,0);
    set_extra(1,1,A);
    Sleep(300);
    for(int i=0;i<2;i++) {
        IDirectInputDevice8_Poll(devices[i]);
        IDirectInputDevice8_GetDeviceState(devices[i],sizeof(state[i]),&state[i]);
    }
    snprintf(seen,sizeof(seen),"A on device 0 %d, device 1 %d",state[0].rgbButtons[0]>>7,state[1].rgbButtons[0]>>7);
    check((state[0].rgbButtons[0]&0x80)!=(state[1].rgbButtons[0]&0x80),"one controller's button is on one device only",seen);
    /* Games take the first device for player 1. */
    check(!(state[0].rgbButtons[0]&0x80) && (state[1].rgbButtons[0]&0x80),"controller 0 is listed first",seen);
    other=(state[0].rgbButtons[0]&0x80)?0:1;
    set_pad(1,A,0,0,0,0,0,0);
    set_extra(1,1,0);
    Sleep(800);   /* a hosted 32-bit program's reports take longer */
    for(int i=0;i<2;i++) {
        IDirectInputDevice8_Poll(devices[i]);
        IDirectInputDevice8_GetDeviceState(devices[i],sizeof(state[i]),&state[i]);
    }
    snprintf(seen,sizeof(seen),"device %d A %d, device %d A %d",other,state[other].rgbButtons[0]>>7,!other,state[!other].rgbButtons[0]>>7);
    check(!(state[other].rgbButtons[0]&0x80) && (state[!other].rgbButtons[0]&0x80),"the first controller's button is on the other device",seen);
    set_pad(1,0,0,0,0,0,0,0);

    /* A game may still hold the device when its controller goes, and when it comes back. */
    set_extra(1,0,0);
    count=count_until(1);
    snprintf(seen,sizeof(seen),"%d devices",count);
    check(count==1,"a device goes while a program has it open",seen);
    IDirectInputDevice8_Poll(devices[0]);
    IDirectInputDevice8_GetDeviceState(devices[0],sizeof(state[0]),&state[0]);
    IDirectInputDevice8_Poll(devices[1]);
    IDirectInputDevice8_GetDeviceState(devices[1],sizeof(state[1]),&state[1]);
    set_extra(1,1,0);
    count=count_until(2);
    snprintf(seen,sizeof(seen),"%d devices",count);
    check(count==2,"it comes back while the old handle is still open",seen);

done:
    for(int i=0;i<2;i++)
        if(devices[i]) { IDirectInputDevice8_Unacquire(devices[i]); IDirectInputDevice8_Release(devices[i]); }

    set_extra(1,0,0);
    count=count_until(1);
    snprintf(seen,sizeof(seen),"%d devices",count);
    check(count==1,"the device goes with its controller",seen);
    set_extra(1,1,0);
    count=count_until(2);
    snprintf(seen,sizeof(seen),"%d devices",count);
    check(count==2,"a controller that comes back has a device again",seen);
    check_players_elsewhere();
    set_extra(1,0,0);
    count_until(1);
}

/* A hosted game that looks once, with a second controller there from the start. */
static int check_at_start(void)
{
    HRESULT hr=DirectInput8Create(GetModuleHandleW(NULL),DIRECTINPUT_VERSION,&IID_IDirectInput8W,(void **)&dinput,NULL);
    char seen[64];

    if(hr!=DI_OK){printf("FAIL DirectInput8Create\n");return 1;}
    extra_found=0;
    IDirectInput8_EnumDevices(dinput,DI8DEVCLASS_GAMECTRL,enum_extra,NULL,DIEDFL_ATTACHEDONLY);
    snprintf(seen,sizeof(seen),"%d devices",extra_found);
    check(extra_found==2,"the first look finds a controller there from the start",seen);
    IDirectInput8_Release(dinput);
    check_wgi_two();
    return failures?1:0;
}

int main(int argc,char **argv)
{
    HWND window;

    if(argc<2) { printf("usage: pw_hid_pad PADFILE [RUMBLEFILE]\n"); return 2; }
    pad_path=argv[1];
    rumble_path=argc>2?argv[2]:NULL;
    setvbuf(stdout,NULL,_IONBF,0);
    if(argc>3 && !strcmp(argv[3],"at-start"))return check_at_start();
    window=CreateWindowA("STATIC","pw_hid_pad",WS_POPUP,0,0,1,1,NULL,NULL,NULL,NULL);
    set_pad(1,0,0,0,0,0,0,0);
    check_dinput(window);
    check_winmm();
    check_rawinput();
    check_wgi();
    check_players(window);
    if(dinput)IDirectInput8_Release(dinput);
    printf("%s\n",failures?"FAILED":"PASSED");
    return failures?1:0;
}
