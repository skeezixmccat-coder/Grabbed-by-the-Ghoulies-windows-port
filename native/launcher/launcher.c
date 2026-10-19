/* Ghoulies native launcher (N1).
 *
 * Its only job is to own the address range the original XBE needs
 * (0x10000-0x572000) so nothing else in the process can take it. The image
 * base is fixed at 0x10000 and a large uninitialised array extends the image
 * past 0x600000. The runtime DLL then maps the XBE over this range, which
 * includes this launcher's own headers and code, so control never returns
 * here. No CRT: nothing in this file may run after GhRun is called.
 */
#include <windows.h>

/* Linker options live in tools/build_native.ps1 (MSVC rejects most of them as
 * source directives): /BASE:0x10000 /FIXED /DYNAMICBASE:NO /LARGEADDRESSAWARE
 * /ENTRY:LauncherEntry /SUBSYSTEM:CONSOLE /NODEFAULTLIB kernel32.lib */

#define XBE_RANGE_RESERVE 0x600000u

/* Uninitialised: occupies address space in the image but no file bytes. */
#pragma bss_seg(".xbeimg")
static unsigned char g_xbe_range[XBE_RANGE_RESERVE];
#pragma bss_seg()

typedef void (__cdecl *GhRunFn)(void);

static void Fail(const char* message, DWORD length) {
    DWORD written;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), message, length, &written, NULL);
    ExitProcess(10);
}

void __cdecl LauncherEntry(void) {
    HMODULE runtime = LoadLibraryW(L"ghoulies_runtime.dll");
    GhRunFn run;
    if (!runtime) Fail("launcher: cannot load ghoulies_runtime.dll\r\n", 44);
    run = (GhRunFn)GetProcAddress(runtime, "GhRun");
    if (!run) Fail("launcher: GhRun export missing\r\n", 32);
    g_xbe_range[0] = 0; /* keep the reservation referenced */
    run(); /* never returns: the runtime maps the XBE over this image */
    ExitProcess(11);
}
