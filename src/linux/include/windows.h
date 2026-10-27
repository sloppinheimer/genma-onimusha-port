// Linux: <windows.h> for the host code. DXVK Native's own Windows headers (its windows_base.h and the D3D9 API, with
// their calls plain cdecl), then the port's compatibility layer (../compat.h): the slice of Win32 and the NT native
// API the host uses, done on POSIX. Force-included first in every file (linux.cmake), so <windows.h> anywhere after
// is this file again.
#pragma once
#include <windows_base.h>   // DXVK Native's (its own windows.h is these two)
#include <unknwn.h>
// DXVK's headers are written for C++: in C its THIS/THIS_ would leave the object out of every COM call's arguments.
#undef THIS_
#undef THIS
#define THIS_ INTERFACE *This,
#define THIS INTERFACE *This
#include <d3d9.h>
// DXVK's headers empty __stdcall, and its D3D9 calls are declared now, while it's empty. From here on it means what it
// does on Windows: everything the game calls, and everything it calls back, is stdcall or fastcall.
#undef __stdcall
#define __stdcall __attribute__((stdcall))
#define __fastcall __attribute__((fastcall))
#define __cdecl __attribute__((cdecl))
#include "../compat.h"
