// Linux entry point: what onimusha.exe and its fixed base do on Windows (src/ldr/ldr.c). The Xbox image's address range
// (0x10000-0x576000) is taken first thing, before any library can map something there, then the host runs as it
// does on Windows. It's taken with VirtualAlloc so compat's VirtualQuery knows it without reading /proc: the
// texture code asks about the game's palettes and texture headers there every frame.
#include <stdio.h>
#include <stdint.h>

void host_main(void *lo, void *hi);
void compat_init(int argc, char **argv);

int main(int argc, char **argv)
{
    uint8_t *lo = (uint8_t *)0x10000, *hi = (uint8_t *)0x576000;
    void *p = VirtualAlloc(lo, hi - lo, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (p != lo) {
        fprintf(stderr, "Genma Onimusha: can't map the game's address range at 0x10000 (got %p). "
                        "Is vm.mmap_min_addr above 65536?\n", p);
        return 1;
    }
    compat_init(argc, argv);
    host_main(lo, hi);
    return 0;
}
