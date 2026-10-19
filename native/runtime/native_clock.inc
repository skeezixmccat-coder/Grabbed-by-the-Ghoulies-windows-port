// Shared by the x86 runtime and the executable clock regression fixture.
// Independently authored. Windows QPC contract: docs/SOURCES.md.
// Xbox rate is the retail XAPI QueryPerformanceFrequency result, also recorded
// by Cxbx-Reloaded. Epoch zero is this process's emulated console boot.
constexpr uint64_t kXboxTscFrequency = 733333333ull;
uint64_t g_tsc_host_frequency = 0, g_tsc_host_origin = 0;

uint64_t ScaleXboxTsc(uint64_t elapsed, uint64_t frequency) {
    // Split whole seconds before multiplying; elapsed*733333333 would overflow
    // after only hours on a fast host counter. Only the final 64-bit TSC may
    // wrap, exactly as an unsigned hardware counter does.
    if (!frequency || frequency > UINT64_MAX / kXboxTscFrequency)
        NativeClockFail("host counter frequency outside exact integer scaling range");
    return (elapsed / frequency) * kXboxTscFrequency +
           ((elapsed % frequency) * kXboxTscFrequency) / frequency;
}

void InitializeNativeClock() {
    LARGE_INTEGER frequency{}, origin{};
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 ||
        !QueryPerformanceCounter(&origin) || origin.QuadPart < 0)
        NativeClockFail("cannot initialize Windows performance counter");
    ScaleXboxTsc(0, static_cast<uint64_t>(frequency.QuadPart)); // validate scaling range
    g_tsc_host_frequency = static_cast<uint64_t>(frequency.QuadPart);
    g_tsc_host_origin = static_cast<uint64_t>(origin.QuadPart);
}

uint64_t ReadXboxTsc() {
    LARGE_INTEGER now{};
    if (!QueryPerformanceCounter(&now) || now.QuadPart < 0 ||
        static_cast<uint64_t>(now.QuadPart) < g_tsc_host_origin)
        NativeClockFail("Windows performance counter failed or preceded console boot");
    return ScaleXboxTsc(static_cast<uint64_t>(now.QuadPart) - g_tsc_host_origin, g_tsc_host_frequency);
}

bool HandleNativeTscFault(DWORD code, CONTEXT* c, bool verified_site) {
    // Only the loader's SHA-verified two-byte RDTSC->UD2 patches are ours.
    // Unrelated invalid instructions must retain the normal explicit fault path.
    if (code != EXCEPTION_ILLEGAL_INSTRUCTION || !verified_site) return false;
    const uint8_t* ip = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(c->Eip));
    if (ip[0] != 0x0f || ip[1] != 0x0b)
        NativeClockFail("verified RDTSC trap site no longer contains UD2");
    uint64_t cycles = ReadXboxTsc();
    // RDTSC changes only EDX:EAX and advances EIP by two. In particular ECX,
    // the stack, arithmetic flags and the guest floating-point state survive.
    c->Eax = static_cast<uint32_t>(cycles);
    c->Edx = static_cast<uint32_t>(cycles >> 32);
    c->Eip += 2;
    return true;
}
