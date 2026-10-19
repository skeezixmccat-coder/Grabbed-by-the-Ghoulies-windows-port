// Kernel synchronisation: IRQL, DPCs, timers, interrupts, events and waits
// (included by ghoulies_runtime.cpp).
//
// The console has one CPU: code running at DISPATCH_LEVEL or above (DPCs,
// interrupt service routines, threads that raised IRQL) excludes all other
// such code. Here every thread at IRQL >= DISPATCH_LEVEL holds one
// dispatch lock, which gives the same mutual exclusion. Each thread's IRQL is
// mirrored into its KPCR (+0x24), where the original code reads it.

// Precise unbiased interrupt time (Windows 10+). The plain variant only
// advances once per system clock tick (15.6 ms by default), which made
// vblanks, raster position and guest timers arrive in uneven bursts.
ULONGLONG InterruptTime100ns() {
    using Precise = VOID (WINAPI*)(PULONGLONG);
    static const Precise precise = reinterpret_cast<Precise>(GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "QueryUnbiasedInterruptTimePrecise"));
    if (!precise) Stop(296, "QueryUnbiasedInterruptTimePrecise not available (Windows 10 or later required)");
    ULONGLONG t; precise(&t); return t;
}

CRITICAL_SECTION g_dispatch_lock;
std::atomic<bool> g_dsound_work{false};   // native work queued for the DPC thread (DirectSound stream completions)
thread_local uint8_t t_irql = 0;
constexpr uint8_t kDispatchLevel = 2;

void SetThreadIrqlVariable(uint8_t irql) { t_irql = irql; }
uint8_t CurrentIrqlForDsound() { return t_irql; }

void MirrorIrql() {
    if (!t_kpcr_page) return;
    DWORD old;
    VirtualProtect(t_kpcr_page, 0x1000, PAGE_READWRITE, &old);
    t_kpcr_page[0x24] = t_irql;
    VirtualProtect(t_kpcr_page, 0x1000, PAGE_NOACCESS, &old);
}

uint8_t RaiseIrqlTo(uint8_t irql, const char* caller) {
    uint8_t old = t_irql;
    if (irql < old) Stop(190, "%s: raise to %u below current IRQL %u (IRQL_NOT_GREATER_OR_EQUAL)", caller, irql, old);
    if (old < kDispatchLevel && irql >= kDispatchLevel) EnterCriticalSection(&g_dispatch_lock);
    t_irql = irql; MirrorIrql();
    return old;
}

void LowerIrqlTo(uint8_t irql, const char* caller) {
    if (irql > t_irql) Stop(191, "%s: lower to %u above current IRQL %u (IRQL_NOT_LESS_OR_EQUAL)", caller, irql, t_irql);
    bool release = t_irql >= kDispatchLevel && irql < kDispatchLevel;
    t_irql = irql; MirrorIrql();
    if (release) LeaveCriticalSection(&g_dispatch_lock);
}

// 160 KfRaiseIrql (fastcall, NewIrql in CL) -> old IRQL
uint8_t __fastcall KfRaiseIrql(uint8_t irql) { return RaiseIrqlTo(irql, "KfRaiseIrql"); }
// 161 KfLowerIrql (fastcall, NewIrql in CL)
void __fastcall KfLowerIrql(uint8_t irql) { LowerIrqlTo(irql, "KfLowerIrql"); }
// 129 KeRaiseIrqlToDpcLevel -> old IRQL
uint8_t __stdcall KeRaiseIrqlToDpcLevel() { return RaiseIrqlTo(kDispatchLevel, "KeRaiseIrqlToDpcLevel"); }

#include "kernel_events.inc"

// ---- DPC queue and timers ----
struct ArmedTimer { uint32_t timer; uint32_t dpc; ULONGLONG due_100ns; };
std::vector<ArmedTimer> g_armed_timers;
std::vector<uint32_t> g_dpc_queue;
CRITICAL_SECTION g_timer_lock;
HANDLE g_timer_wake = nullptr;

bool QueueDpc(uint32_t dpc, uint32_t arg1, uint32_t arg2) {
    EnterCriticalSection(&g_timer_lock);
    bool inserted = *At<uint8_t>(dpc + 2) == 0;
    if (inserted) {
        *At<uint8_t>(dpc + 2) = 1;
        *At<uint32_t>(dpc + 0x14) = arg1; *At<uint32_t>(dpc + 0x18) = arg2;
        g_dpc_queue.push_back(dpc);
    }
    LeaveCriticalSection(&g_timer_lock);
    if (inserted) SetEvent(g_timer_wake);
    return inserted;
}

// 119 KeInsertQueueDpc(Dpc, SystemArgument1, SystemArgument2) -> BOOLEAN
uint32_t __stdcall KeInsertQueueDpc(uint32_t dpc, uint32_t arg1, uint32_t arg2) { return QueueDpc(dpc, arg1, arg2) ? 1 : 0; }

// 137 KeRemoveQueueDpc(Dpc) -> BOOLEAN
uint32_t __stdcall KeRemoveQueueDpc(uint32_t dpc) {
    EnterCriticalSection(&g_timer_lock);
    bool removed = false;
    for (size_t i = 0; i < g_dpc_queue.size(); ++i)
        if (g_dpc_queue[i] == dpc) { g_dpc_queue.erase(g_dpc_queue.begin() + i); removed = true; break; }
    if (removed) *At<uint8_t>(dpc + 2) = 0;
    LeaveCriticalSection(&g_timer_lock);
    return removed ? 1 : 0;
}

DWORD WINAPI DpcDispatcherThread(void*) {
    CreateThreadEnv(nullptr, 0, __readfsdword(0x04), kDispatchLevel, true);   // runs only DPCs; holds the dispatch lock while running each one
    for (;;) {
        DWORD wait = INFINITE;
        std::vector<ArmedTimer> due;
        std::vector<uint32_t> dpcs;
        EnterCriticalSection(&g_timer_lock);
        ULONGLONG now = InterruptTime100ns();
        for (size_t i = 0; i < g_armed_timers.size();) {
            if (g_armed_timers[i].due_100ns <= now) { due.push_back(g_armed_timers[i]); g_armed_timers.erase(g_armed_timers.begin() + i); continue; }
            ULONGLONG ms = (g_armed_timers[i].due_100ns - now + 9999) / 10000;
            if (ms < wait) wait = static_cast<DWORD>(ms);
            ++i;
        }
        for (const auto& t : due) { *At<uint8_t>(t.timer + 3) = 0; }
        LeaveCriticalSection(&g_timer_lock);
        for (const auto& t : due) {
            SignalObject(t.timer);
            if (t.dpc) QueueDpc(t.dpc, static_cast<uint32_t>(now), static_cast<uint32_t>(now >> 32));
        }
        EnterCriticalSection(&g_timer_lock);
        dpcs.swap(g_dpc_queue);
        for (uint32_t d : dpcs) *At<uint8_t>(d + 2) = 0;
        LeaveCriticalSection(&g_timer_lock);
        for (uint32_t d : dpcs) {
            uint32_t routine = *At<uint32_t>(d + 0x0C), context = *At<uint32_t>(d + 0x10);
            uint32_t a1 = *At<uint32_t>(d + 0x14), a2 = *At<uint32_t>(d + 0x18);
            EnterCriticalSection(&g_dispatch_lock);
            reinterpret_cast<void (__stdcall*)(uint32_t, uint32_t, uint32_t, uint32_t)>(static_cast<uintptr_t>(routine))(d, context, a1, a2);
            LeaveCriticalSection(&g_dispatch_lock);
        }
        if (g_dsound_work.exchange(false)) {
            EnterCriticalSection(&g_dispatch_lock);
            DsoundWork();
            LeaveCriticalSection(&g_dispatch_lock);
        }
        if (due.empty() && dpcs.empty()) WaitForSingleObject(g_timer_wake, wait);
    }
}

// 149 KeSetTimer(timer, DueTime (LARGE_INTEGER), Dpc) -> BOOLEAN was inserted.
uint32_t __stdcall KeSetTimer(uint32_t timer, uint32_t due_low, int32_t due_high, uint32_t dpc) {
    int64_t due = (static_cast<int64_t>(due_high) << 32) | due_low;
    if (due >= 0) {
        // Absolute due time: KeQuerySystemTime units (100 ns since 1601, host
        // system clock). Converted once to the interrupt-time schedule; an
        // already passed time expires immediately. (Later host clock changes
        // do not re-time an armed absolute timer.)
        FILETIME ft; GetSystemTimeAsFileTime(&ft);
        int64_t now = static_cast<int64_t>((static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
        due = -std::max<int64_t>(0, due - now);
    }
    if ((*At<uint8_t>(timer) & 0xFE) != 8) Stop(105, "KeSetTimer(%08x): not an initialized timer", timer);
    EnterCriticalSection(&g_timer_lock);
    bool was_inserted = false;
    for (size_t i = 0; i < g_armed_timers.size(); ++i)
        if (g_armed_timers[i].timer == timer) { g_armed_timers.erase(g_armed_timers.begin() + i); was_inserted = true; break; }
    ULONGLONG due_at = InterruptTime100ns() + static_cast<ULONGLONG>(-due);
    g_armed_timers.push_back({timer, dpc, due_at});
    *At<uint8_t>(timer + 3) = 1; *At<int32_t>(timer + 4) = 0;
    *At<uint32_t>(timer + 0x10) = static_cast<uint32_t>(due_at); *At<uint32_t>(timer + 0x14) = static_cast<uint32_t>(due_at >> 32);
    *At<uint32_t>(timer + 0x20) = dpc; *At<int32_t>(timer + 0x24) = 0;
    LeaveCriticalSection(&g_timer_lock);
    ResetEvent(WaitObjectFor(timer, "KeSetTimer"));
    SetEvent(g_timer_wake);
    return was_inserted ? 1 : 0;
}

// 97 KeCancelTimer(timer) -> BOOLEAN was inserted
uint32_t __stdcall KeCancelTimer(uint32_t timer) {
    EnterCriticalSection(&g_timer_lock);
    bool was_inserted = false;
    for (size_t i = 0; i < g_armed_timers.size(); ++i)
        if (g_armed_timers[i].timer == timer) { g_armed_timers.erase(g_armed_timers.begin() + i); was_inserted = true; break; }
    *At<uint8_t>(timer + 3) = 0;
    LeaveCriticalSection(&g_timer_lock);
    return was_inserted ? 1 : 0;
}

// ---- Events and waits ----
// 145 KeSetEvent(Event, Increment, Wait) -> previous SignalState
int32_t __stdcall KeSetEvent(uint32_t event, uint32_t increment, uint32_t wait) {
    if (wait & 0xFF) Stop(193, "KeSetEvent(%08x): Wait=TRUE not provided", event);
    int32_t previous = *At<int32_t>(event + 4);
    SignalObject(event);
    return previous;
}

int32_t WaitOn(HANDLE h, uint32_t object, const int64_t* timeout, const char* caller) {
    if (t_irql >= kDispatchLevel && !(timeout && *timeout == 0)) Stop(194, "%s: blocking wait at IRQL %u", caller, t_irql);
    DWORD ms = INFINITE;
    if (timeout) {
        if (*timeout > 0) Stop(195, "%s: absolute timeouts not provided", caller);
        ms = static_cast<DWORD>((-*timeout + 9999) / 10000);
    }
    DWORD r = WaitForSingleObject(h, ms);
    if (r == WAIT_TIMEOUT) return 0x102;   // STATUS_TIMEOUT
    if (r != WAIT_OBJECT_0) Stop(196, "%s: wait failed (%lu)", caller, GetLastError());
    if (object) { uint8_t type = *At<uint8_t>(object); if (type == 1 || type == 9) *At<int32_t>(object + 4) = 0; }
    return kStatusSuccess;
}

// 159 KeWaitForSingleObject(Object, WaitReason, WaitMode, Alertable, Timeout*)
int32_t __stdcall KeWaitForSingleObject(uint32_t object, uint32_t reason, uint32_t mode, uint32_t alertable, int64_t* timeout) {
    return WaitOn(WaitObjectFor(object, "KeWaitForSingleObject"), object, timeout, "KeWaitForSingleObject");
}

// 99 KeDelayExecutionThread(WaitMode, Alertable, Interval*)
int32_t __stdcall KeDelayExecutionThread(uint32_t mode, uint32_t alertable, int64_t* interval) {
    if (!interval || *interval > 0) Stop(197, "KeDelayExecutionThread: absolute intervals not provided");
    if (t_irql >= kDispatchLevel) Stop(194, "KeDelayExecutionThread at IRQL %u", t_irql);
    Sleep(static_cast<DWORD>((-*interval + 9999) / 10000));
    return kStatusSuccess;
}

// 151 KeStallExecutionProcessor(MicroSeconds): busy wait.
void __stdcall KeStallExecutionProcessor(uint32_t microseconds) {
    LARGE_INTEGER f, start, now; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&start);
    LONGLONG ticks = static_cast<LONGLONG>(microseconds) * f.QuadPart / 1000000;
    do { QueryPerformanceCounter(&now); YieldProcessor(); } while (now.QuadPart - start.QuadPart < ticks);
}

// 142/139 KeSaveFloatingPointState / KeRestoreFloatingPointState: on the
// console these protect a thread's x87/SSE state while DPC-level code uses
// the FPU. Here DPCs and interrupt routines run on their own Windows threads,
// whose FPU state is separate, so there is nothing to save; success.
int32_t __stdcall KeSaveFloatingPointState(uint32_t) { return kStatusSuccess; }
int32_t __stdcall KeRestoreFloatingPointState(uint32_t) { return kStatusSuccess; }

// ---- Interrupts ----
// Vector = 0x30 + bus level; IRQL = 27 - bus level (console HAL mapping).
// KINTERRUPT: +0 ServiceRoutine, +4 ServiceContext, +8 BusInterruptLevel,
// +0xC Irql, +0x10 Connected, +0x11 ShareVector, +0x12 Mode.
uint32_t g_connected_interrupts[32];   // by bus level
CRITICAL_SECTION g_interrupt_lock;
HANDLE g_interrupt_wake = nullptr;

// 44 HalGetInterruptVector(BusInterruptLevel, Irql*) -> vector
uint32_t __stdcall HalGetInterruptVector(uint32_t level, uint8_t* irql) {
    if (level > 26) return 0;
    if (irql) *irql = static_cast<uint8_t>(27 - level);
    return 0x30 + level;
}

// 109 KeInitializeInterrupt(Interrupt, ServiceRoutine, ServiceContext, Vector, Irql, InterruptMode, ShareVector)
void __stdcall KeInitializeInterrupt(uint32_t interrupt, uint32_t routine, uint32_t context, uint32_t vector, uint32_t irql, uint32_t mode, uint32_t share) {
    if (vector < 0x30 || vector > 0x30 + 26) Stop(198, "KeInitializeInterrupt: vector %#x", vector);
    *At<uint32_t>(interrupt) = routine; *At<uint32_t>(interrupt + 4) = context;
    *At<uint32_t>(interrupt + 8) = vector - 0x30; *At<uint32_t>(interrupt + 0xC) = irql & 0xFF;
    *At<uint8_t>(interrupt + 0x10) = 0; *At<uint8_t>(interrupt + 0x11) = static_cast<uint8_t>(share);
    *At<uint8_t>(interrupt + 0x12) = static_cast<uint8_t>(mode);
}

// 98 KeConnectInterrupt(Interrupt) -> BOOLEAN
uint32_t __stdcall KeConnectInterrupt(uint32_t interrupt) {
    uint32_t level = *At<uint32_t>(interrupt + 8);
    EnterCriticalSection(&g_interrupt_lock);
    bool ok = level < 32 && !g_connected_interrupts[level] && !*At<uint8_t>(interrupt + 0x10);
    if (ok) { g_connected_interrupts[level] = interrupt; *At<uint8_t>(interrupt + 0x10) = 1; }
    LeaveCriticalSection(&g_interrupt_lock);
    Log("KeConnectInterrupt(%08x, bus level %u, IRQL %u, routine %08x)%s", interrupt, level, *At<uint32_t>(interrupt + 0xC),
        *At<uint32_t>(interrupt), ok ? "" : ": refused");
    if (ok) SetEvent(g_interrupt_wake);
    return ok ? 1 : 0;
}

// 100 KeDisconnectInterrupt(Interrupt) -> BOOLEAN
uint32_t __stdcall KeDisconnectInterrupt(uint32_t interrupt) {
    uint32_t level = *At<uint32_t>(interrupt + 8);
    EnterCriticalSection(&g_interrupt_lock);
    bool ok = level < 32 && g_connected_interrupts[level] == interrupt;
    if (ok) { g_connected_interrupts[level] = 0; *At<uint8_t>(interrupt + 0x10) = 0; }
    LeaveCriticalSection(&g_interrupt_lock);
    return ok ? 1 : 0;
}

// 153 KeSynchronizeExecution(Interrupt, SynchronizeRoutine, Context) -> BOOLEAN
uint32_t __stdcall KeSynchronizeExecution(uint32_t interrupt, uint32_t routine, uint32_t context) {
    uint8_t old = RaiseIrqlTo(static_cast<uint8_t>(*At<uint32_t>(interrupt + 0xC)), "KeSynchronizeExecution");
    uint32_t r = reinterpret_cast<uint8_t (__stdcall*)(uint32_t)>(static_cast<uintptr_t>(routine))(context);
    LowerIrqlTo(old, "KeSynchronizeExecution");
    return r & 0xFF;
}

// Calls the routine connected at a bus level, as the interrupt would:
// at its IRQL, holding the dispatch lock. Returns false if none connected.
bool DeliverInterrupt(uint32_t level) {
    EnterCriticalSection(&g_interrupt_lock);
    uint32_t interrupt = g_connected_interrupts[level];
    LeaveCriticalSection(&g_interrupt_lock);
    if (!interrupt) return false;
    uint8_t old = RaiseIrqlTo(static_cast<uint8_t>(*At<uint32_t>(interrupt + 0xC)), "interrupt");
    reinterpret_cast<uint8_t (__stdcall*)(uint32_t, uint32_t)>(static_cast<uintptr_t>(*At<uint32_t>(interrupt)))(interrupt, *At<uint32_t>(interrupt + 4));
    LowerIrqlTo(old, "interrupt");
    return true;
}

bool Nv2aIrqAsserted();
void Nv2aVblank();
constexpr uint32_t kGpuBusLevel = 3;

// GPU interrupt source: raises vertical blank every NTSC field and delivers
// the GPU interrupt while it is asserted and connected.
DWORD WINAPI InterruptThread(void*) {
    CreateThreadEnv(nullptr, 0, __readfsdword(0x04));
    const ULONGLONG frame = Nv2aFieldPeriod100ns();
    ULONGLONG next = InterruptTime100ns() + frame;
    // High-resolution waitable timer: field edges within about 0.5 ms rather
    // than the 15.6 ms default tick; g_interrupt_wake still wakes it early.
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) Stop(296, "cannot create high-resolution vblank timer (%lu)", GetLastError());
    for (;;) {
        ULONGLONG now = InterruptTime100ns();
        if (now < next) {
            LARGE_INTEGER due; due.QuadPart = -static_cast<LONGLONG>(next - now);
            if (!SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) Stop(296, "vblank timer failed (%lu)", GetLastError());
            HANDLE waits[2] = {g_interrupt_wake, timer};
            WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        }
        now = InterruptTime100ns();
        if (now >= next) {
            Nv2aVblank(); next += frame;
            if (next < now) {
                // T008 observes the existing late-wake reschedule; pacing is unchanged.
                if (g_timing_audit) { ++g_audit_vblank_resyncs; g_audit_vblank_phase_lost.fetch_add(now + frame - next); }
                next = now + frame;
            }
        }
        for (int guard = 0; guard < 64 && Nv2aIrqAsserted(); ++guard) { if (!DeliverInterrupt(kGpuBusLevel)) break; ++g_gpu_interrupts_delivered; }
    }
}

// ---- Handle-based dispatcher objects (NtCreateEvent / NtCreateMutant / waits) ----
// Unnamed objects only; each is backed by a Windows event or mutex. Thread
// handles are waitable too (Windows thread handles).
int32_t __stdcall NtCreateEvent(uint32_t* handle, uint32_t attributes, uint32_t type, uint32_t initial) {
    if (attributes && *At<uint32_t>(attributes + 4)) Stop(270, "NtCreateEvent: named events not provided");
    HANDLE e = CreateEventW(nullptr, type == 0 ? TRUE : FALSE, (initial & 0xFF) ? TRUE : FALSE, nullptr);
    XObject o; o.kind = XKind::Event; o.host = e; o.name = "<event>";
    *handle = NewHandle(o);
    return kStatusSuccess;
}

void SetXboxEventHandle(uint32_t handle) {
    XObject* o = Lookup(handle, "event signal");
    if (o->kind != XKind::Event) Stop(271, "handle %#x is not an event", handle);
    SetEvent(o->host);
}

// 225 NtSetEvent(Handle, PreviousState*)
int32_t __stdcall NtSetEvent(uint32_t handle, int32_t* previous) {
    XObject* o = Lookup(handle, "NtSetEvent");
    if (o->kind != XKind::Event) Stop(271, "NtSetEvent(%#x): not an event", handle);
    if (previous) *previous = WaitForSingleObject(o->host, 0) == WAIT_OBJECT_0 ? 1 : 0;   // note: consumes a synchronization event's state
    SetEvent(o->host);
    return kStatusSuccess;
}

// 192 NtCreateMutant(Handle*, ObjectAttributes, InitialOwner)
int32_t __stdcall NtCreateMutant(uint32_t* handle, uint32_t attributes, uint32_t owner) {
    if (attributes && *At<uint32_t>(attributes + 4)) Stop(272, "NtCreateMutant: named mutants not provided");
    XObject o; o.kind = XKind::Mutant; o.host = CreateMutexW(nullptr, (owner & 0xFF) ? TRUE : FALSE, nullptr); o.name = "<mutant>";
    *handle = NewHandle(o);
    return kStatusSuccess;
}

// 221 NtReleaseMutant(Handle, PreviousCount*)
int32_t __stdcall NtReleaseMutant(uint32_t handle, int32_t* previous) {
    XObject* o = Lookup(handle, "NtReleaseMutant");
    if (o->kind != XKind::Mutant) Stop(273, "NtReleaseMutant(%#x): not a mutant", handle);
    if (!ReleaseMutex(o->host)) return static_cast<int32_t>(0xC0000046);   // STATUS_MUTANT_NOT_OWNED
    if (previous) *previous = 0;
    return kStatusSuccess;
}

HANDLE WaitableHost(uint32_t handle, const char* caller) {
    XObject* o = Lookup(handle, caller);
    if (o->kind != XKind::Event && o->kind != XKind::Mutant && o->kind != XKind::Thread) Stop(274, "%s(%#x): object is not waitable here", caller, handle);
    return o->host;
}

int32_t WaitHandles(uint32_t count, const uint32_t* handles, bool all, const int64_t* timeout, const char* caller) {
    if (t_irql >= kDispatchLevel && !(timeout && *timeout == 0)) Stop(194, "%s: blocking wait at IRQL %u", caller, t_irql);
    if (count == 0 || count > 64) Stop(275, "%s: %u handles", caller, count);
    HANDLE hs[64];
    for (uint32_t i = 0; i < count; ++i) hs[i] = WaitableHost(handles[i], caller);
    DWORD ms = INFINITE;
    if (timeout) { if (*timeout > 0) Stop(195, "%s: absolute timeouts not provided", caller); ms = static_cast<DWORD>((-*timeout + 9999) / 10000); }
    DWORD r = WaitForMultipleObjects(count, hs, all ? TRUE : FALSE, ms);
    if (r == WAIT_TIMEOUT) return 0x102;
    if (r >= WAIT_OBJECT_0 && r < WAIT_OBJECT_0 + count) return static_cast<int32_t>(r - WAIT_OBJECT_0);   // STATUS_WAIT_0 + n
    if (r >= WAIT_ABANDONED_0 && r < WAIT_ABANDONED_0 + count) return static_cast<int32_t>(0x80 + (r - WAIT_ABANDONED_0));
    Stop(196, "%s: wait failed (%lu)", caller, GetLastError());
}

// 233 NtWaitForSingleObject(Handle, Alertable, Timeout*)
int32_t __stdcall NtWaitForSingleObject(uint32_t handle, uint32_t alertable, int64_t* timeout) { return WaitHandles(1, &handle, false, timeout, "NtWaitForSingleObject"); }
// 234 NtWaitForSingleObjectEx(Handle, WaitMode, Alertable, Timeout*)
int32_t __stdcall NtWaitForSingleObjectEx(uint32_t handle, uint32_t mode, uint32_t alertable, int64_t* timeout) { return WaitHandles(1, &handle, false, timeout, "NtWaitForSingleObjectEx"); }
// 235 NtWaitForMultipleObjectsEx(Count, Handles*, WaitType, WaitMode, Alertable, Timeout*)
int32_t __stdcall NtWaitForMultipleObjectsEx(uint32_t count, uint32_t* handles, uint32_t type, uint32_t mode, uint32_t alertable, int64_t* timeout) {
    return WaitHandles(count, handles, type == 0, timeout, "NtWaitForMultipleObjectsEx");
}
// 238 NtYieldExecution
int32_t __stdcall NtYieldExecution() { SwitchToThread(); return kStatusSuccess; }
// 224 NtResumeThread / 231 NtSuspendThread (Handle, PreviousSuspendCount*)
int32_t __stdcall NtResumeThread(uint32_t handle, uint32_t* previous) {
    XObject* o = Lookup(handle, "NtResumeThread"); if (o->kind != XKind::Thread) Stop(276, "NtResumeThread: not a thread");
    DWORD r = ResumeThread(o->host); if (previous) *previous = r; return kStatusSuccess;
}
int32_t __stdcall NtSuspendThread(uint32_t handle, uint32_t* previous) {
    XObject* o = Lookup(handle, "NtSuspendThread"); if (o->kind != XKind::Thread) Stop(276, "NtSuspendThread: not a thread");
    DWORD r = SuspendThread(o->host); if (previous) *previous = r; return kStatusSuccess;
}

// Thread objects: KTHREAD address <-> Windows thread, registered by each Xbox thread.
std::unordered_map<uint32_t, DWORD> g_kthread_to_tid;
std::unordered_map<DWORD, uint32_t> g_tid_to_kthread;
CRITICAL_SECTION g_kthread_lock;
void RegisterKthreadForThread(uint32_t kthread, DWORD tid) {
    EnterCriticalSection(&g_kthread_lock);
    g_kthread_to_tid[kthread] = tid; g_tid_to_kthread[tid] = kthread;
    LeaveCriticalSection(&g_kthread_lock);
}
void RegisterKthread(uint32_t kthread) { RegisterKthreadForThread(kthread,GetCurrentThreadId()); }

// 246 ObReferenceObjectByHandle(Handle, ObjectType*, Object**): thread handles -> KTHREAD.
int32_t __stdcall ObReferenceObjectByHandle(uint32_t handle, uint32_t type, uint32_t* object) {
    XObject* o = Lookup(handle, "ObReferenceObjectByHandle");
    if (o->kind != XKind::Thread) Stop(277, "ObReferenceObjectByHandle(%#x): only thread objects provided", handle);
    DWORD tid = GetThreadId(o->host);
    EnterCriticalSection(&g_kthread_lock);
    auto f = g_tid_to_kthread.find(tid);
    uint32_t k = f == g_tid_to_kthread.end() ? 0 : f->second;
    LeaveCriticalSection(&g_kthread_lock);
    if(!k) Stop(278,"ObReferenceObjectByHandle: thread %lu has no published identity",tid);
    *object=k; return kStatusSuccess;
}
// 250 ObfDereferenceObject (fastcall, Object in ECX): references are not counted here.
void __fastcall ObfDereferenceObject(uint32_t) {}

// 143 KeSetBasePriorityThread(KTHREAD*, Increment) -> previous increment.
int32_t __stdcall KeSetBasePriorityThread(uint32_t kthread, int32_t increment) {
    EnterCriticalSection(&g_kthread_lock);
    auto f = g_kthread_to_tid.find(kthread);
    DWORD tid = f == g_kthread_to_tid.end() ? 0 : f->second;
    LeaveCriticalSection(&g_kthread_lock);
    if (!tid) Stop(279, "KeSetBasePriorityThread(%08x): unknown thread", kthread);
    HANDLE h = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, tid);
    int previous = GetThreadPriority(h);
    int p = increment >= 15 ? THREAD_PRIORITY_TIME_CRITICAL : increment <= -15 ? THREAD_PRIORITY_IDLE : std::max(-2, std::min(2, static_cast<int>(increment)));
    SetThreadPriority(h, p);
    CloseHandle(h);
    return previous;
}

void QueueDsoundWork() { g_dsound_work = true; SetEvent(g_timer_wake); }

void InitializeKernelSync() {
    InitializeCriticalSection(&g_kthread_lock);
    InitializeCriticalSection(&g_dispatch_lock);
    InitializeCriticalSection(&g_wait_lock);
    InitializeCriticalSection(&g_interrupt_lock);
    g_interrupt_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}
