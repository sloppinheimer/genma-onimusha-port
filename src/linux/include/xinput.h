// Linux: XInput doesn't exist; controllers come through SDL3 (input.c). The types it names, and calls that find no pad.
#pragma once
typedef struct { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } XINPUT_GAMEPAD;
typedef struct { DWORD dwPacketNumber; XINPUT_GAMEPAD Gamepad; } XINPUT_STATE;
typedef struct { WORD wLeftMotorSpeed, wRightMotorSpeed; } XINPUT_VIBRATION;
typedef struct { BYTE Type, SubType; WORD Flags; XINPUT_GAMEPAD Gamepad; XINPUT_VIBRATION Vibration; } XINPUT_CAPABILITIES;
#define XINPUT_GAMEPAD_A 0x1000
#define XINPUT_GAMEPAD_B 0x2000
#define XINPUT_GAMEPAD_X 0x4000
#define XINPUT_GAMEPAD_Y 0x8000
#define XINPUT_GAMEPAD_LEFT_SHOULDER 0x0100
#define XINPUT_GAMEPAD_RIGHT_SHOULDER 0x0200
static inline DWORD XInputGetState(DWORD i, XINPUT_STATE *s) { (void)i; (void)s; return ERROR_DEVICE_NOT_CONNECTED; }
static inline DWORD XInputSetState(DWORD i, XINPUT_VIBRATION *v) { (void)i; (void)v; return ERROR_DEVICE_NOT_CONNECTED; }
static inline DWORD XInputGetCapabilities(DWORD i, DWORD f, XINPUT_CAPABILITIES *c) { (void)i; (void)f; (void)c; return ERROR_DEVICE_NOT_CONNECTED; }
