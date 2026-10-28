// onimusha.exe — reserves the Xbox image's fixed address range, then hands over to onimusha_host.dll.
//
// Linked at a fixed base of 0x10000 (see CMakeLists.txt). The PE header page, this .text page and the
// .xbox placeholder together cover 0x10000-0x576000, exactly where default.xbe lives, so Windows can never
// put a heap, stack or DLL there. The host then overwrites the whole range — this code included — which is
// why host_main never returns.
//
// The placeholder is plain read-write data and host_main an ordinary import: the host makes the range executable
// when it maps the game (host.c map_xbe). An executable data section and a LoadLibrary + GetProcAddress jump are
// what antivirus machine learning looks for in a malware loader; without onimusha_host.dll Windows says it's missing.
#include <windows.h>

#pragma bss_seg(".xbox")
unsigned char xbox_space[0x564000];
#pragma bss_seg()

__declspec(dllimport) void host_main(void *lo, void *hi);

void __stdcall start(void)
{
    host_main((void *)0x10000, xbox_space + sizeof xbox_space);
}
