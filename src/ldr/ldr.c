// onimusha.exe — reserves the Xbox image's fixed address range, then hands over to onimusha_host.dll.
//
// Linked at a fixed base of 0x10000 (see CMakeLists.txt). The PE header page, this .text page and the
// .xbox placeholder together cover 0x10000-0x576000, exactly where default.xbe lives, so Windows can never
// put a heap, stack or DLL there. The host then overwrites the whole range — this code included — which is
// why host_main never returns.
#include <windows.h>

#pragma bss_seg(".xbox")
unsigned char xbox_space[0x564000];
#pragma bss_seg()
#pragma comment(linker, "/SECTION:.xbox,ERW")

typedef void (*host_main_fn)(void *lo, void *hi);

void __stdcall start(void)
{
    HMODULE h = LoadLibraryW(L"onimusha_host.dll");
    host_main_fn m = h ? (host_main_fn)GetProcAddress(h, "host_main") : 0;
    if (!m) {
        MessageBoxA(0, "onimusha_host.dll is missing or broken.", "Genma Onimusha", MB_ICONERROR);
        ExitProcess(1);
    }
    m((void *)0x10000, xbox_space + sizeof xbox_space);
}
