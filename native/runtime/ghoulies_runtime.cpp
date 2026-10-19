// Ghoulies native runtime: maps the original XBE at its original addresses
// inside the launcher's reserved image, provides the Xbox thread environment,
// fills the kernel thunk table (implemented ordinals, explicit failing stubs
// for the rest, no-access guard pages for unimplemented data exports), and
// executes the original entry point natively. Every unsupported dependency
// stops the process with a report. Nothing returns success for missing
// behaviour. See docs/NATIVE-LOADER.md.
//
// Build: tools/build_native.ps1 (MSVC, x86). Run: build/native/ghoulies-native.exe
//   [xbe-path] [report-path] [timeout-seconds]
#define NOMINMAX
#include <windows.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#include <bcrypt.h>
#include <shellapi.h>
#include <intrin.h>
#include <xaudio2.h>
#include <xapobase.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
// GPU-thread profiling accumulators (defined in nv2a_gl.cpp; diagnostics only).
extern "C" unsigned long long g_nv2a_prof[32];
extern "C" bool g_nv2a_profile_enabled;
extern "C" unsigned long long g_nv2a_readbacks[4];
// TSC ticks (QueryPerformanceCounter around every method cost ~6% of the GPU thread).
struct Nv2aProf { int id; unsigned long long t0; Nv2aProf(int i) : id(i), t0(g_nv2a_profile_enabled ? __rdtsc() : 0) {}
    ~Nv2aProf() { if(t0) g_nv2a_prof[id] += __rdtsc() - t0; } };   // at file scope: audio.inc is included inside the runtime namespace
#include <malloc.h>
#include <algorithm>
#include <atomic>
#include <unordered_map>
#include <charconv>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <set>
#include <vector>
#include "../../include/kernel_exports.hpp"

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")

extern "C" IMAGE_DOS_HEADER __ImageBase;

extern "C" void Nv2aGlInit(uint8_t* vram, uint32_t vram_size, uint8_t* ramin, uint32_t ramin_size);
extern "C" uint32_t Nv2aGlRead(uint32_t addr);
extern "C" void Nv2aGlWrite(uint32_t addr, uint32_t value);
extern "C" uint32_t Nv2aGlPendingInterrupts();
extern "C" void Nv2aGlMethod(uint32_t subchannel, uint32_t method, uint32_t parameter);
extern "C" void Nv2aGlFlushSurfaces();
extern "C" bool Nv2aOwnedFault(uintptr_t address, uintptr_t eip);

namespace {

constexpr uint32_t kXbeBase = 0x10000;
constexpr uint32_t kEntryKeyRetail = 0xA8FC57AB;
constexpr uint32_t kThunkKeyRetail = 0x5B6D40B6;
constexpr uint32_t kSectionWritable = 0x1, kSectionPreload = 0x2, kSectionExecutable = 0x4;
constexpr char kExpectedSha256[] = "a2a5bbe51ecc11392612bad30ee4238b796d4d0ed7262e7d23dc4120388294b1";
constexpr int32_t kStatusSuccess = 0;

struct Section { std::string name; uint32_t flags, va, vsize, raw, rsize; };
struct DataGuard { uint32_t ordinal; const char* name; uint8_t* page; };

FILE* g_report = nullptr;
std::vector<Section> g_sections;
std::vector<DataGuard> g_data_guards;
uint32_t g_image_end = 0;
uint32_t g_entry = 0;
uint8_t* g_stubs = nullptr;
size_t g_stub_bytes = 0;
uint8_t* g_fs_trampolines = nullptr;
size_t g_fs_trampoline_bytes = 0;
uint8_t g_launcher_header[0x1000];   // launcher PE header page, saved before the XBE overwrites it
CRITICAL_SECTION g_header_lock;
// Memory accounting for MmQueryStatistics.
std::atomic<uint64_t> g_virtual_committed{0}, g_virtual_reserved{0};
std::atomic<uint32_t> g_contiguous_pages_used{0};
std::atomic<uint32_t> g_xbox_threads{0};
std::atomic<uint32_t> g_file_cache_pages{0};
void SetThreadIrqlVariable(uint8_t irql);
void RegisterKthread(uint32_t kthread);
void RegisterKthreadForThread(uint32_t kthread, DWORD tid);

CRITICAL_SECTION g_log_lock;
DWORD g_kpcr_slot = TLS_OUT_OF_INDEXES;
uint32_t g_tls_zero_fill = 0, g_tls_raw_size = 0, g_tls_index_address = 0;

void Log(const char* fmt, ...) {
    char line[2048];
    va_list args; va_start(args, fmt); vsnprintf(line, sizeof line, fmt, args); va_end(args);
    EnterCriticalSection(&g_log_lock);
    fputs(line, stderr); fputc('\n', stderr); fflush(stderr);
    if (g_report) { fputs(line, g_report); fputc('\n', g_report); fflush(g_report); }
    LeaveCriticalSection(&g_log_lock);
}

// Diagnostic sampling profiler (GH_SAMPLE_PROFILE=1, test runs only): about
// every millisecond the GPU thread is suspended for one GetThreadContext and
// resumed; nothing is allocated or logged while it is suspended. Every 10 s
// the busiest 4-byte-aligned EIP buckets are logged with their module, for
// offline symbolisation against ghoulies_runtime.map.
extern "C" DWORD Nv2aGpuThreadId();
static uint32_t g_rt_lo = 0, g_rt_hi = 0, g_text_lo = 0, g_text_hi = 0;
static DWORD WINAPI SampleProfilerThread(void*) {
    DWORD tid = 0;
    while (!(tid = Nv2aGpuThreadId())) Sleep(100);
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!thread) { Log("sample profiler: cannot open GPU thread (%lu)", GetLastError()); return 0; }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    static uint32_t eips[20000], callers[20000]; size_t n = 0, outside = 0;
    { HMODULE rt = GetModuleHandleW(L"ghoulies_runtime.dll"); auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(rt);
      auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<uint8_t*>(rt) + dos->e_lfanew);
      g_rt_lo = uint32_t(uintptr_t(rt)); g_rt_hi = g_rt_lo + nt->OptionalHeader.SizeOfImage;
      auto* sec = IMAGE_FIRST_SECTION(nt);   // attribute only to code-section return addresses
      for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
          if (!memcmp(sec[i].Name, ".text", 5)) { g_text_lo = g_rt_lo + sec[i].VirtualAddress; g_text_hi = g_text_lo + sec[i].Misc.VirtualSize; } }
    ULONGLONG last = GetTickCount64();
    for (;;) {
        Sleep(1);
        if (SuspendThread(thread) != (DWORD)-1) {
            CONTEXT c{}; c.ContextFlags = CONTEXT_CONTROL;
            BOOL ok = GetThreadContext(thread, &c);
            ResumeThread(thread);
            if (ok && n < _countof(eips)) {
                uint32_t eip = c.Eip; callers[n] = 0;
                // Outside the runtime image (waits, driver): attribute the sample to
                // the nearest runtime return address on the stack, marked by bit 0.
                if ((eip < g_rt_lo || eip >= g_rt_hi) && c.Esp) {
                    const uint32_t* sp = reinterpret_cast<const uint32_t*>(uintptr_t(c.Esp));
                    MEMORY_BASIC_INFORMATION mbi{};
                    if (VirtualQuery(sp, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT) {
                        size_t limit = (uintptr_t(mbi.BaseAddress) + mbi.RegionSize - uintptr_t(sp)) / 4;
                        size_t k = 0;
                        for (; k < limit && k < 2048; ++k) if (sp[k] >= g_text_lo && sp[k] < g_text_hi) { eip = sp[k] | 1u; break; }
                        for (++k; k < limit && k < 4096; ++k) if (sp[k] >= g_text_lo && sp[k] < g_text_hi) { callers[n] = sp[k]; break; }
                    }
                }
                eips[n++] = eip;
                outside += (c.Eip < g_rt_lo || c.Eip >= g_rt_hi);
            }
        }
        if (GetTickCount64() - last < 10000) continue;
        last = GetTickCount64();
        std::unordered_map<uint32_t, uint32_t> hist; std::unordered_map<std::wstring, uint32_t> modules;
        for (size_t i = 0; i < n; ++i) {
            if (eips[i] >= g_rt_lo && eips[i] < g_rt_hi + 1) hist[(eips[i] & 1) ? eips[i] : (eips[i] & ~3u)]++;
            else modules[L"unattributed outside runtime"]++;
        }
        { char b2[64]; snprintf(b2, sizeof b2, "%zu", outside); modules[L"(outside runtime total)"] = uint32_t(outside); (void)b2; }
        std::string line; char buf[96];
        for (auto& kv : modules) { snprintf(buf, sizeof buf, " %ls:%u", kv.first.c_str(), kv.second); line += buf; }
        Log("sample profile: %zu GPU-thread samples by module:%s", n, line.c_str());
        std::vector<std::pair<uint32_t, uint32_t>> top(hist.begin(), hist.end());
        std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.second > b.second; });
        line.clear();
        for (size_t i = 0; i < top.size() && i < 120; ++i) { snprintf(buf, sizeof buf, " %08x:%u", top[i].first, top[i].second); line += buf; }
        Log("sample profile runtime EIPs:%s", line.c_str());
        { std::unordered_map<uint64_t, uint32_t> pairs;
          for (size_t i = 0; i < n; ++i) if (eips[i] & 1) pairs[(uint64_t(eips[i]) << 32) | callers[i]]++;
          std::vector<std::pair<uint64_t, uint32_t>> tp(pairs.begin(), pairs.end());
          std::sort(tp.begin(), tp.end(), [](auto& a, auto& b) { return a.second > b.second; });
          line.clear();
          for (size_t i = 0; i < tp.size() && i < 60; ++i) { snprintf(buf, sizeof buf, " %08x<%08x:%u", uint32_t(tp[i].first >> 32), uint32_t(tp[i].first), tp[i].second); line += buf; }
          Log("sample profile wait/driver callers:%s", line.c_str()); }
        n = 0;
    }
}
static void StartSampleProfiler() {
    if (!getenv("GH_SAMPLE_PROFILE")) return;
    HANDLE t = CreateThread(nullptr, 0, SampleProfilerThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    Log("sample profiler: enabled (diagnostic)");
}

[[noreturn]] void Stop(int code, const char* fmt, ...) {
    char line[2048];
    va_list args; va_start(args, fmt); vsnprintf(line, sizeof line, fmt, args); va_end(args);
    Log("STOP %d: %s", code, line);
    EnterCriticalSection(&g_log_lock);
    if (g_report) { fclose(g_report); g_report = nullptr; }
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
    for (;;) {}
}

std::vector<uint8_t> ReadAll(const wchar_t* path) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) Stop(20, "cannot open XBE (error %lu)", GetLastError());
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(f, &size) || size.QuadPart <= 0 || size.QuadPart > 64ll * 1024 * 1024) Stop(21, "bad XBE size");
    std::vector<uint8_t> bytes(static_cast<size_t>(size.QuadPart));
    DWORD got = 0;
    if (!ReadFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &got, nullptr) || got != bytes.size()) Stop(22, "short XBE read");
    CloseHandle(f);
    return bytes;
}

std::string Sha256Hex(const uint8_t* data, size_t size);
std::string Sha256Hex(const std::vector<uint8_t>& bytes) { return Sha256Hex(bytes.data(), bytes.size()); }
std::string Sha256Hex(const uint8_t* data, size_t size) {
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE hash = nullptr; uint8_t digest[32];
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0 ||
        BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) != 0 ||
        BCryptHashData(hash, const_cast<PUCHAR>(data), static_cast<ULONG>(size), 0) != 0 ||
        BCryptFinishHash(hash, digest, sizeof digest, 0) != 0) Stop(23, "SHA-256 unavailable");
    BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(alg, 0);
    static const char hex[] = "0123456789abcdef";
    std::string out;
    for (uint8_t b : digest) { out += hex[b >> 4]; out += hex[b & 15]; }
    return out;
}

uint32_t U32(const std::vector<uint8_t>& d, uint32_t off) {
    if (off + 4 > d.size()) Stop(24, "XBE read out of range at 0x%x", off);
    uint32_t v; memcpy(&v, d.data() + off, 4); return v;
}

template <typename T> T* At(uint32_t va) { return reinterpret_cast<T*>(static_cast<uintptr_t>(va)); }

bool InRange(uintptr_t a, const void* base, size_t size) {
    auto b = reinterpret_cast<uintptr_t>(base);
    return base && a >= b && a < b + size;
}

const char* SectionName(uint32_t va) {
    if (va >= kXbeBase && va < kXbeBase + 0x1000) return "<headers>";
    for (const auto& s : g_sections) if (va >= s.va && va < s.va + s.vsize) return s.name.c_str();
    if (InRange(va, g_stubs, g_stub_bytes)) return "<kernel stub>";
    if (InRange(va, g_fs_trampolines, g_fs_trampoline_bytes)) return "<fs trampoline>";
    return nullptr;
}

bool Readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return reinterpret_cast<uintptr_t>(p) + n <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

void LogStack(uint32_t esp, int words) {
    for (int i = 0; i < words; ++i) {
        auto* p = reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(esp) + 4u * i);
        if (!Readable(p, 4)) { Log("  [esp+%02x] <unreadable>", 4 * i); break; }
        const char* where = SectionName(*p);
        Log("  [esp+%02x] %08x%s%s", 4 * i, *p, where ? "  " : "", where ? where : "");
    }
}

#include "kernel_crypto.inc"
uint8_t g_title_signature_key[16] = {};

// --- Guarded structures ----------------------------------------------------
// Xbox kernel structures the original code reads (KPCR, ETHREAD) are kept on
// no-access pages. A read at a deliberately provided field offset is allowed
// for one instruction (page opened, trap flag set, page closed on the single
// step); any other offset is reported as an unprovided field.

struct GuardedBlock { uint8_t* page; const char* name; std::atomic<const uint16_t*> fields; size_t field_count; bool writable; void (*refresh)(uint8_t*); };
constexpr size_t kMaxGuardedBlocks = 512;
GuardedBlock g_guarded[kMaxGuardedBlocks];
std::atomic<size_t> g_guarded_count{0};
std::atomic<uint64_t> g_guarded_reads{0};
CRITICAL_SECTION g_guard_lock;
thread_local uint8_t* t_reopened_page = nullptr;
thread_local uint32_t t_kthread = 0;   // this thread's Xbox ETHREAD/KTHREAD address
thread_local uint8_t* t_kpcr_page = nullptr;

constexpr uint16_t kKpcrFields[] = {0x04, 0x1C, 0x20, 0x24, 0x28, 0x58, 0x278};
constexpr uint16_t kEthreadFields[] = {0x28, 0x12C};
// A suspended thread has an identity before its host bootstrap executes.
// TLS lives on the eventual host stack; reading it before bootstrap remains
// explicitly unsupported, rather than supplying a fabricated null TLS value.
constexpr uint16_t kPendingEthreadFields[] = {0xFFFF, 0x12C};

void RegisterGuarded(uint8_t* page, const char* name, const uint16_t* fields, size_t count, bool writable = false, void (*refresh)(uint8_t*) = nullptr) {
    EnterCriticalSection(&g_guard_lock);
    size_t n = g_guarded_count.load();
    if (n >= kMaxGuardedBlocks) Stop(60, "too many guarded structures");
    auto& block=g_guarded[n]; block.page=page; block.name=name;
    block.field_count=count; block.writable=writable; block.refresh=refresh;
    block.fields.store(fields,std::memory_order_release);
    g_guarded_count.store(n + 1, std::memory_order_release);
    LeaveCriticalSection(&g_guard_lock);
    DWORD old;
    if (!VirtualProtect(page, 0x1000, PAGE_NOACCESS, &old)) Stop(61, "cannot guard %s", name);
}

const GuardedBlock* GuardedFor(uintptr_t address) {
    size_t n = g_guarded_count.load(std::memory_order_acquire);
    for (size_t i = 0; i < n; ++i) if (InRange(address, g_guarded[i].page, 0x1000)) return &g_guarded[i];
    return nullptr;
}

uint32_t PrepareEthread(DWORD tid) {
    uint8_t* ethread=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x1000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    if(!ethread) Stop(64,"cannot allocate suspended Xbox thread identity");
    memcpy(ethread+0x12C,&tid,4);
    RegisterGuarded(ethread,"ETHREAD",kPendingEthreadFields,_countof(kPendingEthreadFields));
    uint32_t address=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ethread));
    RegisterKthreadForThread(address,tid);
    return address;
}
void PublishEthreadTls(uint8_t* ethread) {
    EnterCriticalSection(&g_guard_lock);
    for(size_t i=0;i<g_guarded_count.load();++i) if(g_guarded[i].page==ethread) {
        g_guarded[i].fields.store(kEthreadFields,std::memory_order_release);
        LeaveCriticalSection(&g_guard_lock); return;
    }
    Stop(64,"prepared Xbox thread identity was not guarded");
}

// --- Xbox thread environment -------------------------------------------------
// Layout established from the original code (docs/NATIVE-LOADER.md):
// the startup code computes TlsDataSize = align16(raw + zero fill) + 4 and
// stores TlsIndex = -TlsDataSize/4. The Xbox kernel places the TLS block at
// the top of each created thread's stack: KTHREAD.TlsData = block and
// NtTib.StackBase = block + TlsDataSize, so StackBase[TlsIndex] is block[0].
// XAPI's thread routine then writes block[0] = block + 4 and initialises the
// data itself. The initial (entry) thread has no TLS block.
//
// KPCR (per thread, as fs: is per thread here):
//   +0x04 NtTib.StackBase, +0x1C SelfPcr, +0x20 Prcb -> +0x28,
//   +0x24 Irql (PASSIVE_LEVEL), +0x28 PrcbData.CurrentThread,
//   +0x58 PrcbData.DpcRoutineActive (0),
//   +0x278 PrcbData.DebugMonitorData (NULL: retail, no debug monitor)
// ETHREAD: +0x28 Tcb.TlsData, +0x12C UniqueThread.

void CreateThreadEnv(uint8_t* tls_block, uint32_t tls_data_size, uint32_t stack_base, uint8_t irql = 0, bool dpc_thread = false, uint32_t prepared_kthread = 0) {
    if (tls_block) {
        uint32_t expected = 4 + ((g_tls_zero_fill + g_tls_raw_size + 15) & ~15u);
        if (tls_data_size != expected) Stop(62, "TlsDataSize %#x does not match XBE TLS layout (%#x)", tls_data_size, expected);
        int32_t index = *At<int32_t>(g_tls_index_address);
        if (index * 4 + static_cast<int32_t>(tls_data_size) != 0) Stop(63, "TLS index %d does not address a %#x-byte block below the stack base", index, tls_data_size);
        stack_base = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(tls_block)) + tls_data_size;
    }
    uint8_t* kpcr = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    uint8_t* ethread = prepared_kthread ? At<uint8_t>(prepared_kthread) :
        static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if(prepared_kthread) {
        DWORD old;
        if(!VirtualProtect(ethread,0x1000,PAGE_READWRITE,&old)) Stop(64,"cannot initialize prepared ETHREAD TLS");
    }
    if (!kpcr || !ethread) Stop(64, "cannot allocate Xbox thread environment");
    auto put = [](uint8_t* base, uint32_t off, uint32_t value) { memcpy(base + off, &value, 4); };
    auto addr = [](const void* p) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)); };
    put(kpcr, 0x04, stack_base);
    put(kpcr, 0x1C, addr(kpcr));
    put(kpcr, 0x20, addr(kpcr + 0x28));
    kpcr[0x24] = irql;
    put(kpcr, 0x28, addr(ethread));
    put(kpcr, 0x58, dpc_thread ? 1u : 0u);
    put(kpcr, 0x278, 0);
    put(ethread, 0x28, addr(tls_block));
    put(ethread, 0x12C, GetCurrentThreadId());
    RegisterGuarded(kpcr, "KPCR", kKpcrFields, _countof(kKpcrFields));
    if(prepared_kthread) {
        DWORD old;
        if(!VirtualProtect(ethread,0x1000,PAGE_NOACCESS,&old)) Stop(61,"cannot guard prepared ETHREAD");
        PublishEthreadTls(ethread);
    } else RegisterGuarded(ethread, "ETHREAD", kEthreadFields, _countof(kEthreadFields));
    if (!TlsSetValue(g_kpcr_slot, kpcr)) Stop(65, "cannot bind KPCR to thread");
    t_kthread = addr(ethread);
    t_kpcr_page = kpcr;
    SetThreadIrqlVariable(irql);
    RegisterKthread(addr(ethread));
}

// --- Original RDTSC sites / console cycle clock -------------------------------
[[noreturn]] void NativeClockFail(const char* why) { Stop(231, "console clock: %s", why); }
#include "native_clock.inc"
#include "frame_timing_research.inc"
struct NativeTscSite { uint32_t address; const char* sha256; };
const NativeTscSite kNativeTscSites[] = {
#include "rdtsc_sites.inc"
};
std::atomic<uint64_t> g_tsc_reads[_countof(kNativeTscSites)]{};

unsigned PatchTscSites(uint32_t base, uint32_t size) {
    unsigned patched = 0;
    for (const auto& site : kNativeTscSites) {
        if (site.address < base || static_cast<uint64_t>(site.address) + 16 > static_cast<uint64_t>(base) + size) continue;
        auto* code = At<uint8_t>(site.address);
        if (Sha256Hex(code, 16) != site.sha256 || code[0] != 0x0f || code[1] != 0x31)
            Stop(231, "RDTSC site %08x differs from the verified original", site.address);
        // UD2 has the same length. No adjacent instruction is relocated and
        // no on-disk game bytes change. The VEH preserves RDTSC's exact ABI.
        code[1] = 0x0b;
        if (!FlushInstructionCache(GetCurrentProcess(), code, 2))
            Stop(231, "cannot flush RDTSC patch at %08x", site.address);
        Log("clock: patched original RDTSC %08x (%s), 16-byte SHA-256 %s", site.address, SectionName(site.address), site.sha256);
        ++patched;
    }
    return patched;
}

bool EmulateOriginalTsc(DWORD code, CONTEXT* c) {
    if (code != EXCEPTION_ILLEGAL_INSTRUCTION) return false;
    for (size_t i=0; i<_countof(kNativeTscSites); ++i) {
        if (c->Eip != kNativeTscSites[i].address) continue;
        if (g_tsc_reads[i].fetch_add(1, std::memory_order_relaxed) == 0)
            Log("clock: first original RDTSC at %08x (%s), %llu Hz", c->Eip, SectionName(c->Eip), kXboxTscFrequency);
        TraceTimingCounter(c);
        return HandleNativeTscFault(code, c, true);
    }
    return false;
}

// --- fs: access redirection ----------------------------------------------------
// Original code reads Xbox KPCR fields through fs:. fs: here is the Windows
// TEB (NT_TIB exception list and stack limit keep their meaning and are left
// alone). Each read of an Xbox-specific offset is redirected to a trampoline
// that loads this thread's KPCR from a Windows TLS slot and reads the field.

void PatchFsSites() {
    static const uint8_t kOffsets[] = {0x04, 0x18, 0x1C, 0x20, 0x24, 0x28, 0x2C, 0x58};
    struct Site { uint8_t* at; uint8_t length; uint8_t reg; uint8_t offset; bool byte_zx; };
    std::vector<Site> sites;
    for (const auto& s : g_sections) {
        if (!(s.flags & kSectionPreload) || !(s.flags & kSectionExecutable)) continue;
        uint8_t* b = At<uint8_t>(s.va);
        for (uint32_t i = 0; i + 8 <= s.rsize; ++i) {
            if (b[i] != 0x64) continue;
            Site site{b + i, 0, 0, 0, false};
            uint32_t disp; uint32_t at;
            if (b[i + 1] == 0xA1) { site.length = 6; site.reg = 0; at = i + 2; }
            else if (b[i + 1] == 0x8B && (b[i + 2] & 0xC7) == 0x05) { site.length = 7; site.reg = (b[i + 2] >> 3) & 7; at = i + 3; }
            else if (b[i + 1] == 0x0F && b[i + 2] == 0xB6 && (b[i + 3] & 0xC7) == 0x05) { site.length = 8; site.reg = (b[i + 3] >> 3) & 7; site.byte_zx = true; at = i + 4; }
            else continue;
            memcpy(&disp, b + at, 4);
            if (disp > 0xFF || !memchr(kOffsets, static_cast<int>(disp), sizeof kOffsets)) continue;
            if (site.reg == 4) Stop(70, "fs: read into ESP at %08x", s.va + i);
            site.offset = static_cast<uint8_t>(disp);
            sites.push_back(site);
            i += site.length - 1;
        }
    }
    g_fs_trampoline_bytes = sites.size() * 32;
    g_fs_trampolines = static_cast<uint8_t*>(VirtualAlloc(nullptr, g_fs_trampoline_bytes ? g_fs_trampoline_bytes : 1, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    if (!g_fs_trampolines) Stop(71, "cannot allocate fs trampolines");
    if (g_kpcr_slot >= 64) Stop(72, "KPCR TLS slot %lu is outside the TEB's direct slots", g_kpcr_slot);
    uint32_t slot_offset = 0xE10 + 4 * g_kpcr_slot;
    for (size_t n = 0; n < sites.size(); ++n) {
        const Site& s = sites[n];
        uint8_t* t = g_fs_trampolines + n * 32; size_t k = 0;
        t[k++] = 0x64; t[k++] = 0x8B; t[k++] = static_cast<uint8_t>(0x05 | (s.reg << 3)); memcpy(t + k, &slot_offset, 4); k += 4; // mov reg, fs:[slot]
        if (s.byte_zx) { t[k++] = 0x0F; t[k++] = 0xB6; }  else { t[k++] = 0x8B; }                                          // movzx reg, byte / mov reg, dword
        t[k++] = static_cast<uint8_t>(0x40 | (s.reg << 3) | s.reg); t[k++] = s.offset;                                          //   [reg + offset]
        int32_t back = static_cast<int32_t>(reinterpret_cast<uintptr_t>(s.at + s.length) - reinterpret_cast<uintptr_t>(t + k + 5));
        t[k++] = 0xE9; memcpy(t + k, &back, 4); k += 4;                                                                      // jmp back
        int32_t to = static_cast<int32_t>(reinterpret_cast<uintptr_t>(t) - reinterpret_cast<uintptr_t>(s.at + 5));
        s.at[0] = 0xE9; memcpy(s.at + 1, &to, 4);
        for (uint8_t j = 5; j < s.length; ++j) s.at[j] = 0x90;
        Log("  fs site %08x: %s fs:[%#04x] -> reg %u (%s)", static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s.at)),
            s.byte_zx ? "movzx byte" : "mov dword", s.offset, s.reg, SectionName(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s.at))));
    }
    DWORD old;
    VirtualProtect(g_fs_trampolines, g_fs_trampoline_bytes ? g_fs_trampoline_bytes : 1, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    Log("fs: %zu Xbox KPCR reads redirected", sites.size());
}

// --- Threads -------------------------------------------------------------------
// Windows reads the main image's PE header (at 0x10000) whenever a thread is
// created, on a path this runtime cannot redirect (the WOW64 layer). The XBE
// header lives in that page, so ntdll!NtCreateThreadEx is hooked: the saved
// launcher header is swapped in for the duration of each thread creation,
// under a lock, and the XBE header restored afterwards. This covers threads
// created by this runtime and by Windows components (XAudio2, drivers).
using NtCreateThreadExFn = LONG (NTAPI*)(HANDLE*, ACCESS_MASK, void*, HANDLE, void*, void*, ULONG, SIZE_T, SIZE_T, SIZE_T, void*);
NtCreateThreadExFn g_nt_create_thread_ex = nullptr;   // trampoline to the original stub

LONG NTAPI HookNtCreateThreadEx(HANDLE* thread, ACCESS_MASK access, void* attributes, HANDLE process, void* start, void* argument,
                                ULONG flags, SIZE_T zero_bits, SIZE_T stack_size, SIZE_T max_stack, void* attribute_list) {
    auto* page = At<uint8_t>(kXbeBase);
    static uint8_t xbe_header[0x1000];
    EnterCriticalSection(&g_header_lock);
    memcpy(xbe_header, page, sizeof xbe_header);
    memcpy(page, g_launcher_header, sizeof g_launcher_header);
    LONG status = g_nt_create_thread_ex(thread, access, attributes, process, start, argument, flags, zero_bits, stack_size, max_stack, attribute_list);
    memcpy(page, xbe_header, sizeof xbe_header);
    LeaveCriticalSection(&g_header_lock);
    return status;
}

void HookThreadCreation() {
    auto* stub = reinterpret_cast<uint8_t*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateThreadEx"));
    if (!stub || stub[0] != 0xB8) Stop(53, "ntdll!NtCreateThreadEx does not start with the expected system-call stub");
    auto* tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 16, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    memcpy(tramp, stub, 5);                                                       // mov eax, service number
    int32_t back = static_cast<int32_t>(reinterpret_cast<uintptr_t>(stub + 5) - reinterpret_cast<uintptr_t>(tramp + 10));
    tramp[5] = 0xE9; memcpy(tramp + 6, &back, 4);
    g_nt_create_thread_ex = reinterpret_cast<NtCreateThreadExFn>(tramp);
    DWORD old;
    VirtualProtect(stub, 5, PAGE_EXECUTE_READWRITE, &old);
    int32_t to = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&HookNtCreateThreadEx) - reinterpret_cast<uintptr_t>(stub + 5));
    stub[0] = 0xE9; memcpy(stub + 1, &to, 4);
    VirtualProtect(stub, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), stub, 5);
    Log("thread creation hooked (ntdll!NtCreateThreadEx)");
}

uint32_t g_watch_address = 0;
std::atomic<uint32_t> g_watch_hits{0};
DWORD_PTR g_guest_cpu_mask = 0;

HANDLE CreateNativeThread(SIZE_T stack_reserve, LPTHREAD_START_ROUTINE start, void* arg, bool suspended, bool guest_cpu = true) {
    // Arm before the thread first runs, so allocation/initialization writes are
    // covered too. DR7 L0=1, RW0=01 (write), LEN0=11 (four bytes), per x86.
    // All guest-code executors share one logical CPU. Device/driver workers
    // may use the process mask: the Xbox GPU is concurrent with its CPU too.
    // Suspend first so no guest instruction runs before affinity is installed.
    DWORD flags = STACK_SIZE_PARAM_IS_A_RESERVATION | CREATE_SUSPENDED;
    HANDLE thread = CreateThread(nullptr, stack_reserve, start, arg, flags, nullptr);
    if (thread && guest_cpu) {
        if (!g_guest_cpu_mask || !SetThreadAffinityMask(thread, g_guest_cpu_mask))
            Stop(27, "cannot set guest thread CPU affinity (%lu)", GetLastError());
        Log("thread %lu: guest CPU affinity %#lx", GetThreadId(thread), static_cast<unsigned long>(g_guest_cpu_mask));
    }
    if (thread && g_watch_address) {
        CONTEXT c{}; c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (!GetThreadContext(thread, &c)) Stop(230, "watch: GetThreadContext failed (%lu)", GetLastError());
        if (c.Dr7 & 0xFF) Stop(230, "watch: thread already has debug breakpoints");
        c.Dr0 = g_watch_address; c.Dr6 = 0; c.Dr7 = 0x000D0001;
        if (!SetThreadContext(thread, &c)) Stop(230, "watch: SetThreadContext failed (%lu)", GetLastError());
        Log("watch: armed %08x on thread %lu before start", g_watch_address, GetThreadId(thread));
    }
    if (thread && !suspended && ResumeThread(thread) == DWORD(-1)) Stop(230, "ResumeThread failed (%lu)", GetLastError());
    return thread;
}

// Every Xbox thread is tracked (own duplicated handle) so the watchdog can
// report all of them; the original code may close its own handles.
struct TrackedThread { HANDLE handle; DWORD id; };
std::vector<TrackedThread> g_threads;
CRITICAL_SECTION g_threads_lock;

void TrackThread(HANDLE thread) {
    HANDLE own = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), thread, GetCurrentProcess(), &own, 0, FALSE, DUPLICATE_SAME_ACCESS)) Stop(85, "cannot track thread");
    EnterCriticalSection(&g_threads_lock);
    g_threads.push_back({own, GetThreadId(own)});
    LeaveCriticalSection(&g_threads_lock);
}

struct XboxThreadStart { uint32_t start_routine, start_context, system_routine, tls_data_size, kthread; };

DWORD WINAPI XboxThreadProc(void* p) {
    XboxThreadStart s = *static_cast<XboxThreadStart*>(p);
    delete static_cast<XboxThreadStart*>(p);
    // TLS block at the top of this thread's stack, end 16-byte aligned.
    uint8_t* tls_block = nullptr;
    if (s.tls_data_size) {
        auto* raw = static_cast<uint8_t*>(_alloca(s.tls_data_size + 32));
        uintptr_t end = (reinterpret_cast<uintptr_t>(raw) + s.tls_data_size + 16) & ~uintptr_t{15};
        tls_block = reinterpret_cast<uint8_t*>(end - s.tls_data_size);
        memset(tls_block, 0, s.tls_data_size);
    }
    CreateThreadEnv(tls_block, s.tls_data_size, __readfsdword(0x04),0,false,s.kthread);
    Log("xbox thread %lu: system routine %08x, start %08x, context %08x", GetCurrentThreadId(), s.system_routine, s.start_routine, s.start_context);
    if (s.system_routine) reinterpret_cast<void (__stdcall*)(uint32_t, uint32_t)>(static_cast<uintptr_t>(s.system_routine))(s.start_routine, s.start_context);
    else reinterpret_cast<void (__stdcall*)(uint32_t)>(static_cast<uintptr_t>(s.start_routine))(s.start_context);
    // As in the Xbox kernel's system-thread startup, a returning routine terminates the thread.
    Log("xbox thread %lu: routine returned; terminating thread", GetCurrentThreadId());
    ExitThread(0);
}

#include "xbox_io.inc"
#include "nv2a.inc"
#include "presentation.inc"
#include "gpu.inc"

// --- Kernel functions --------------------------------------------------------------

// 255 PsCreateSystemThreadEx
int32_t __stdcall PsCreateSystemThreadEx(uint32_t* thread_handle, uint32_t extension_size, uint32_t kernel_stack_size,
                                         uint32_t tls_data_size, uint32_t* thread_id, uint32_t start_routine, uint32_t start_context,
                                         uint32_t create_suspended, uint32_t debugger_thread, uint32_t system_routine) {
    Log("PsCreateSystemThreadEx(handle*=%p ext=%#x stack=%#x tls=%#x tid*=%p start=%08x ctx=%08x suspended=%u debugger=%u system=%08x)",
        static_cast<void*>(thread_handle), extension_size, kernel_stack_size, tls_data_size, static_cast<void*>(thread_id),
        start_routine, start_context, create_suspended & 0xFF, debugger_thread & 0xFF, system_routine);
    if (extension_size) Stop(80, "PsCreateSystemThreadEx: thread extension (%#x bytes) not provided", extension_size);
    if (debugger_thread & 0xFF) Stop(81, "PsCreateSystemThreadEx: debugger threads not provided");
    if (!thread_handle) Stop(82, "PsCreateSystemThreadEx: null handle pointer");
    // Xbox threads have a single stack of KernelStackSize; reserve at least 1 MiB here.
    SIZE_T stack = kernel_stack_size > 0x100000 ? kernel_stack_size : 0x100000;
    auto* start = new XboxThreadStart{start_routine, start_context, system_routine, tls_data_size,0};
    // Publish the kernel object before returning even for CREATE_SUSPENDED.
    // XAPI configures Bink's priority via ObReferenceObjectByHandle before
    // NtResumeThread; waiting for that suspended thread to register deadlocks.
    HANDLE thread = CreateNativeThread(stack, XboxThreadProc, start, true);
    if (!thread) Stop(83, "PsCreateSystemThreadEx: CreateThread failed (error %lu)", GetLastError());
    start->kthread=PrepareEthread(GetThreadId(thread));
    TrackThread(thread);
    ++g_xbox_threads;
    XObject object; object.kind = XKind::Thread; object.host = thread; object.name = "<thread>";
    *thread_handle = NewHandle(object);
    if (thread_id) *thread_id = GetThreadId(thread);
    if(!(create_suspended&0xFF) && ResumeThread(thread)==DWORD(-1))
        Stop(83,"cannot start initialized Xbox thread (%lu)",GetLastError());
    return kStatusSuccess;
}

// 187 NtClose
int32_t __stdcall NtClose(uint32_t handle) {
    XObject* o = Lookup(handle, "NtClose");
    if (o->host && !CloseHandle(o->host)) Stop(84, "NtClose(%08x): host handle close failed", handle);
    EnterCriticalSection(&g_handle_lock);
    g_handles.erase(handle);
    LeaveCriticalSection(&g_handle_lock);
    return kStatusSuccess;
}

// 258 PsTerminateSystemThread
[[noreturn]] void __stdcall PsTerminateSystemThread(int32_t exit_status) {
    Log("PsTerminateSystemThread(%#x) on thread %lu", static_cast<uint32_t>(exit_status), GetCurrentThreadId());
    ExitThread(static_cast<DWORD>(exit_status));
}

// Critical sections. Xbox RTL_CRITICAL_SECTION (0x1C bytes):
//   +0x00 DISPATCHER_HEADER of a synchronization event (type 1, size 4, empty
//   wait list pointing at +0x08), +0x10 LockCount, +0x14 RecursionCount,
//   +0x18 OwningThread (KTHREAD). Each one is backed by a Windows critical
//   section; the Xbox fields are kept current for the owner. LockCount does not
//   count waiters (recorded limitation).
struct NativeCriticalSection { CRITICAL_SECTION cs; uint32_t depth = 0; uint32_t owner = 0; };
std::unordered_map<uint32_t, NativeCriticalSection*> g_critical_sections;
SRWLOCK g_critical_lock = SRWLOCK_INIT;

bool CriticalHeaderValid(uint32_t address) {
    auto* w = At<uint32_t>(address);
    return !(address & 3) && w[0] == 0x00040001u && w[1] == 0 && w[2] == address + 8 && w[3] == address + 8;
}

NativeCriticalSection* CriticalFor(uint32_t address, const char* caller) {
    AcquireSRWLockShared(&g_critical_lock);
    auto found = g_critical_sections.find(address);
    NativeCriticalSection* cs = found == g_critical_sections.end() ? nullptr : found->second;
    ReleaseSRWLockShared(&g_critical_lock);
    if (cs) return cs;
    if (!CriticalHeaderValid(address)) Stop(90, "%s(%08x): not an initialized Xbox critical section", caller, address);
    auto* w = At<int32_t>(address);
    if (w[4] != -1 || w[5] != 0 || w[6] != 0) Stop(91, "%s(%08x): statically initialized section is not free", caller, address);
    AcquireSRWLockExclusive(&g_critical_lock);
    auto& slot = g_critical_sections[address];
    if (!slot) { slot = new NativeCriticalSection; InitializeCriticalSection(&slot->cs); }
    cs = slot;
    ReleaseSRWLockExclusive(&g_critical_lock);
    return cs;
}

void PublishCritical(uint32_t address, const NativeCriticalSection* cs) {
    auto* w = At<int32_t>(address);
    w[4] = static_cast<int32_t>(cs->depth) - 1; w[5] = static_cast<int32_t>(cs->depth); w[6] = static_cast<int32_t>(cs->owner);
}

// 291 RtlInitializeCriticalSection
void __stdcall RtlInitializeCriticalSection(uint32_t address) {
    if (address & 3) Stop(92, "RtlInitializeCriticalSection(%08x): unaligned", address);
    AcquireSRWLockExclusive(&g_critical_lock);
    auto& slot = g_critical_sections[address];
    if (slot && slot->depth) Stop(93, "RtlInitializeCriticalSection(%08x): section is owned", address);
    if (!slot) { slot = new NativeCriticalSection; InitializeCriticalSection(&slot->cs); }
    ReleaseSRWLockExclusive(&g_critical_lock);
    auto* w = At<uint32_t>(address);
    w[0] = 0x00040001u; w[1] = 0; w[2] = address + 8; w[3] = address + 8;
    PublishCritical(address, slot);
}

// 277 RtlEnterCriticalSection
void __stdcall RtlEnterCriticalSection(uint32_t address) {
    if (!t_kthread) Stop(94, "RtlEnterCriticalSection(%08x): caller has no Xbox thread", address);
    NativeCriticalSection* cs = CriticalFor(address, "RtlEnterCriticalSection");
    EnterCriticalSection(&cs->cs);
    if (cs->depth && cs->owner != t_kthread) Stop(95, "RtlEnterCriticalSection(%08x): ownership ledger inconsistent", address);
    cs->owner = t_kthread; ++cs->depth;
    PublishCritical(address, cs);
}

// 294 RtlLeaveCriticalSection
void __stdcall RtlLeaveCriticalSection(uint32_t address) {
    NativeCriticalSection* cs = CriticalFor(address, "RtlLeaveCriticalSection");
    if (!cs->depth || cs->owner != t_kthread) Stop(96, "RtlLeaveCriticalSection(%08x): caller does not own the section", address);
    if (--cs->depth == 0) cs->owner = 0;
    PublishCritical(address, cs);
    LeaveCriticalSection(&cs->cs);
}

// Provided kernel data exports: a declared retail environment, each object on
// a guarded page that permits only the listed field offsets.
struct ProvidedData { uint32_t ordinal; const char* name; const uint16_t* fields; size_t field_count; void (*init)(uint8_t*); bool writable = false; void (*refresh)(uint8_t*) = nullptr; };
ULONGLONG g_boot_tick = GetTickCount64();
constexpr uint16_t kHardwareInfoFields[] = {0x00, 0x01, 0x02, 0x03};   // Flags (dword or byte reads)
constexpr uint16_t kSingleDwordField[] = {0x00};
constexpr uint16_t kAnsiStringFields[] = {0x00, 0x02, 0x04};
char g_image_file_name[] = "\\Device\\CdRom0\\default.xbe";
constexpr uint16_t kKrnlVersionFields[] = {0x00, 0x02, 0x04, 0x06};
constexpr uint16_t kSignatureKeyFields[] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
const ProvidedData kProvidedData[] = {
    {325, "XboxSignatureKey", kSignatureKeyFields, _countof(kSignatureKeyFields), [](uint8_t* p) { memcpy(p,g_title_signature_key,16); }},
    // 322 XboxHardwareInfo: Flags = 0 (retail: no devkit 0x2, no arcade 0x8 flag).
    {322, "XboxHardwareInfo", kHardwareInfoFields, _countof(kHardwareInfoFields), [](uint8_t* p) { memset(p, 0, 8); }},
    // 40 HalDiskCachePartitionCount: 3 utility-cache partitions (3-5), as on retail drives.
    {40, "HalDiskCachePartitionCount", kSingleDwordField, _countof(kSingleDwordField), [](uint8_t* p) { const uint32_t v = 3; memcpy(p, &v, 4); }},
    // 164 LaunchDataPage: NULL (declared cold boot from the disc; no launch data).
    {164, "LaunchDataPage", kSingleDwordField, _countof(kSingleDwordField), [](uint8_t* p) { memset(p, 0, 4); }, true},
    // 326 XeImageFileName: ANSI_STRING for the launched image (declared cold boot from the disc).
    {326, "XeImageFileName", kAnsiStringFields, _countof(kAnsiStringFields), [](uint8_t* p) {
        uint16_t length = static_cast<uint16_t>(strlen(g_image_file_name)), maximum = static_cast<uint16_t>(length + 1);
        uint32_t buffer = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_image_file_name));
        memcpy(p, &length, 2); memcpy(p + 2, &maximum, 2); memcpy(p + 4, &buffer, 4); }},
    // 156 KeTickCount: milliseconds since the runtime started, current at every read.
    {156, "KeTickCount", kSingleDwordField, _countof(kSingleDwordField), [](uint8_t* p) { memset(p, 0, 4); }, false,
     [](uint8_t* p) { const uint32_t v = static_cast<uint32_t>(GetTickCount64() - g_boot_tick); memcpy(p, &v, 4); }},
    // 356 HalBootSMCVideoMode: AV pack read from the SMC at boot; declared 1 (HDTV component pack).
    {356, "HalBootSMCVideoMode", kSingleDwordField, _countof(kSingleDwordField), [](uint8_t* p) { const uint32_t v = 1; memcpy(p, &v, 4); }},
    // 324 XboxKrnlVersion: 1.0.5838.1, the final retail kernel.
    {324, "XboxKrnlVersion", kKrnlVersionFields, _countof(kKrnlVersionFields), [](uint8_t* p) {
        const uint16_t v[4] = {1, 0, 5838, 1}; memcpy(p, v, sizeof v); }},
};

// --- Declared native replacements of original functions --------------------
// Only where the original requires console-privileged operations with no
// Windows equivalent. Each replacement is installed at the function entry
// only after the whole original function's bytes match a recorded SHA-256.

// 0x151755 (XAPI) rewrites the GDT code descriptor for selector 8 to end below
// the kernel INIT section (sgdt/cli/far jump; privileged) and saves the old
// descriptor in 0x549510/0x549514. 0x151b9c exchanges that descriptor. Code
// segment limits have no Windows equivalent: the replacements keep a virtual
// descriptor, initially the console's flat ring-0 code descriptor.
constexpr uint64_t kFlatCodeDescriptor = 0x00CF9B000000FFFFull;
uint64_t g_virtual_code_descriptor = kFlatCodeDescriptor;

void __cdecl ProvisionCodeDescriptor() {
    *At<uint32_t>(0x549510) = static_cast<uint32_t>(g_virtual_code_descriptor);
    *At<uint32_t>(0x549514) = static_cast<uint32_t>(g_virtual_code_descriptor >> 32);
    Log("native policy: 0x151755 code-descriptor provisioning (virtual; saved %016llx)", g_virtual_code_descriptor);
}

uint64_t __stdcall ExchangeCodeDescriptor(uint32_t low, uint32_t high) {
    uint64_t previous = g_virtual_code_descriptor;
    g_virtual_code_descriptor = (static_cast<uint64_t>(high) << 32) | low;
    Log("native policy: 0x151b9c code-descriptor exchange (virtual; %016llx -> %016llx)", previous, g_virtual_code_descriptor);
    return previous;
}

#include "input.inc"
#include "audio.inc"

struct Replacement { uint32_t address; uint32_t size; const char* sha256; const void* function; const char* what; };
const Replacement kReplacements[] = {
    {0x151755, 100, "9b8751d90fb7700f27500ea2f52941c107c45fd09007aa3ce3da1a0f884061e3", reinterpret_cast<const void*>(&ProvisionCodeDescriptor), "XAPI code-descriptor provisioning"},
    {0x151b9c, 37, "215d53936fc2f2a05b21b1198a9864c75ea55594b865d1c4f06f7c80f7857bb4", reinterpret_cast<const void*>(&ExchangeCodeDescriptor), "XAPI code-descriptor exchange"},
    // XPP input entry points (identified by XbSymbolDatabase; first 32 bytes verified).
    {0x1fd0e3, 32, "624ab8cc53c4996276a310dfdd9daaac755b343ba59405f0af1010d6da10d1f3", reinterpret_cast<const void*>(&XppInitDevices), "XInitDevices"},
    {0x1fdd35, 32, "267db7322e729f648d227b5b421bac055fddbefe24f55bca9568aff73d4f776b", reinterpret_cast<const void*>(&XppGetDevices), "XGetDevices"},
    {0x1fdd57, 32, "f7f872f991fc05ccd6fc1bdb57c28d0155c68ddb3e037a9d31474ce5afb36a17", reinterpret_cast<const void*>(&XppGetDeviceChanges), "XGetDeviceChanges"},
    {0x1fdc06, 32, "03f47d3f3442b78c8cb1b8ce1221d7684714240d92d6bfeed97ad1fa3029a904", reinterpret_cast<const void*>(&XppInputOpen), "XInputOpen"},
    {0x1fdc5c, 32, "7d501faecfc7f6e565b848c426eb2fee2a7b59309dda9dff6d715524a1bd6a3f", reinterpret_cast<const void*>(&XppInputClose), "XInputClose"},
    {0x1fdc68, 32, "cdce8eaf97f70e35fedec56a3ea379e45df7ef886bf3002c64bdbdbc9b47bece", reinterpret_cast<const void*>(&XppInputGetState), "XInputGetState"},
    {0x1fdcdb, 32, "d56433e5fc813ada94d9206ceb736e5bf6a19d3d6e83c4c074974e26fb9f5683", reinterpret_cast<const void*>(&XppInputSetState), "XInputSetState"},
};

void InstallReplacements() {
    for (const auto& r : kReplacements) {
        std::string hash = Sha256Hex(At<uint8_t>(r.address), r.size);
        if (hash != r.sha256) Stop(75, "replacement target %08x (%s) does not match its recorded bytes", r.address, r.what);
        uint8_t* at = At<uint8_t>(r.address);
        int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(r.function) - reinterpret_cast<uintptr_t>(at + 5));
        at[0] = 0xE9; memcpy(at + 1, &rel, 4);
        Log("replaced %08x (%u bytes, %s)", r.address, r.size, r.what);
    }
}

// 47 HalRegisterShutdownNotification. HAL_SHUTDOWN_REGISTRATION: +0x00
// NotificationRoutine, +0x04 Priority (signed), +0x08 LIST_ENTRY. Kept in a
// list ordered by descending priority; notifications run only at shutdown.
uint32_t g_shutdown_head[2];   // LIST_ENTRY (Flink, Blink)
CRITICAL_SECTION g_shutdown_lock;

void __stdcall HalRegisterShutdownNotification(uint32_t record, uint32_t register_flag) {
    auto addr = [](const void* p) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)); };
    if (!record || (record & 3)) Stop(100, "HalRegisterShutdownNotification(%08x): bad record", record);
    EnterCriticalSection(&g_shutdown_lock);
    uint32_t head = addr(g_shutdown_head);
    if (!g_shutdown_head[0]) { g_shutdown_head[0] = head; g_shutdown_head[1] = head; }
    uint32_t entry = record + 8;
    if (register_flag & 0xFF) {
        int32_t priority = *At<int32_t>(record + 4);
        uint32_t next = g_shutdown_head[0];
        while (next != head && *At<int32_t>(next - 8 + 4) >= priority) next = *At<uint32_t>(next);
        uint32_t prev = *At<uint32_t>(next + 4);
        *At<uint32_t>(entry) = next; *At<uint32_t>(entry + 4) = prev;
        *At<uint32_t>(prev) = entry; *At<uint32_t>(next + 4) = entry;
        Log("shutdown notification registered: routine %08x priority %d", *At<uint32_t>(record), priority);
    } else {
        uint32_t next = *At<uint32_t>(entry), prev = *At<uint32_t>(entry + 4);
        if (!next || !prev || *At<uint32_t>(prev) != entry || *At<uint32_t>(next + 4) != entry) Stop(101, "HalRegisterShutdownNotification: record %08x is not registered", record);
        *At<uint32_t>(prev) = next; *At<uint32_t>(next + 4) = prev;
        Log("shutdown notification unregistered: %08x", record);
    }
    LeaveCriticalSection(&g_shutdown_lock);
}

// 107 KeInitializeDpc. KDPC (0x1C): +0x00 Type (CSHORT 0x13), +0x02 Inserted,
// +0x04 LIST_ENTRY, +0x0C DeferredRoutine, +0x10 DeferredContext,
// +0x14/+0x18 SystemArgument1/2.
void __stdcall KeInitializeDpc(uint32_t dpc, uint32_t routine, uint32_t context) {
    if (!dpc || (dpc & 3)) Stop(102, "KeInitializeDpc(%08x): bad object", dpc);
    *At<uint16_t>(dpc) = 0x13; *At<uint8_t>(dpc + 2) = 0;
    *At<uint32_t>(dpc + 0x0C) = routine; *At<uint32_t>(dpc + 0x10) = context;
}

// 113 KeInitializeTimerEx. KTIMER (0x28): DISPATCHER_HEADER (+0 Type 8|9,
// +1 Absolute, +2 Size 10, +3 Inserted, +4 SignalState, +8 WaitListHead),
// +0x10 DueTime, +0x18 TimerListEntry, +0x20 Dpc, +0x24 Period.
void __stdcall KeInitializeTimerEx(uint32_t timer, uint32_t type) {
    if (!timer || (timer & 3) || type > 1) Stop(103, "KeInitializeTimerEx(%08x, %u): unsupported", timer, type);
    auto* b = At<uint8_t>(timer);
    b[0] = static_cast<uint8_t>(8 + type); b[1] = 0; b[2] = 10; b[3] = 0;
    *At<int32_t>(timer + 4) = 0;
    *At<uint32_t>(timer + 8) = timer + 8; *At<uint32_t>(timer + 12) = timer + 8;
    *At<uint32_t>(timer + 0x20) = 0; *At<int32_t>(timer + 0x24) = 0;
}

#include "kernel_sync.inc"

// 24 ExQueryNonVolatileSetting(index, Type*, Value, ValueLength, ResultLength*).
// Console EEPROM settings are not available; each provided index has a
// declared value describing a North American retail console (not recovered
// EEPROM). Every other index stops the run.
struct DeclaredSetting { uint32_t index; uint32_t value; const char* meaning; };
const DeclaredSetting kSettings[] = {
    {0x07, 1, "XC_LANGUAGE: English"},
    {0x08, 0x00010000u, "XC_VIDEO: widescreen"},
    {0x09, 0, "XC_AUDIO: stereo"},
    {0x0A, 0, "XC_P_CONTROL_GAMES: no restriction"},
    {0x0C, 0, "XC_P_CONTROL_MOVIES: no restriction"},
    {0x11, 0, "XC_MISC: 0 (as in the lifted-trace profiles)"},
    {0x12, 1, "XC_DVD_REGION: 1"},
    {0x103, 0x00400100u, "XC_FACTORY_AV_REGION: NTSC-M"},
    {0x104, 1, "XC_FACTORY_GAME_REGION: North America"},
};

int32_t __stdcall ExQueryNonVolatileSetting(uint32_t index, uint32_t* type, uint32_t* value, uint32_t length, uint32_t* result_length) {
    const DeclaredSetting* setting = nullptr;
    for (const auto& d : kSettings) if (d.index == index) setting = &d;
    if (!setting) Stop(106, "ExQueryNonVolatileSetting(index %#x): setting not provided", index);
    if (result_length) *result_length = 4;
    if (length < 4) return static_cast<int32_t>(0xC0000023);   // STATUS_BUFFER_TOO_SMALL
    if (!type || !value) Stop(107, "ExQueryNonVolatileSetting: null output");
    *type = 4;     // REG_DWORD
    *value = setting->value;
    Log("ExQueryNonVolatileSetting(%#x) -> %#x (%s)", index, setting->value, setting->meaning);
    return kStatusSuccess;
}

// Memory accounting for MmQueryStatistics: see the counters above.

// 184 NtAllocateVirtualMemory(BaseAddress*, ZeroBits, RegionSize*, AllocationType, Protect)
int32_t __stdcall NtAllocateVirtualMemory(uint32_t* base, uint32_t zero_bits, uint32_t* size, uint32_t type, uint32_t protect) {
    constexpr uint32_t kSupportedTypes = MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN | 0x800000 /* MEM_NOZERO: Windows zeroes anyway */;
    if (!base || !size || zero_bits || (type & ~kSupportedTypes) || !(type & (MEM_RESERVE | MEM_COMMIT)))
        Stop(110, "NtAllocateVirtualMemory(base*=%p zero=%u type=%#x protect=%#x): unsupported request", static_cast<void*>(base), zero_bits, type, protect);
    void* result = VirtualAlloc(reinterpret_cast<void*>(static_cast<uintptr_t>(*base)), *size, type & ~0x800000u, protect);
    if (!result) Stop(111, "NtAllocateVirtualMemory(base %08x size %#x type %#x protect %#x) failed (error %lu)", *base, *size, type, protect, GetLastError());
    MEMORY_BASIC_INFORMATION mbi{};
    VirtualQuery(result, &mbi, sizeof mbi);
    if (reinterpret_cast<uintptr_t>(result) >= 0x80000000u) Stop(112, "NtAllocateVirtualMemory returned %p above the Xbox user range", result);
    Log("NtAllocateVirtualMemory(base %08x size %#x type %#x protect %#x) -> %p size %#lx", *base, *size, type, protect, result,
        static_cast<unsigned long>(mbi.RegionSize));
    *base = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(result));
    if (type & MEM_COMMIT) *size = static_cast<uint32_t>((*size + 0xFFF) & ~0xFFFu);
    else *size = static_cast<uint32_t>((*size + 0xFFFF) & ~0xFFFFu);
    if (type & MEM_COMMIT) g_virtual_committed += *size;
    if ((type & MEM_RESERVE) || !*base) g_virtual_reserved += *size;
    return kStatusSuccess;
}

// Kernel Rtl routines whose Xbox signature and structure layouts (STRING /
// ANSI_STRING / UNICODE_STRING: USHORT Length, USHORT MaximumLength, PVOID
// Buffer) are identical to Windows ntdll: the thunk points at ntdll directly.
struct NtdllPassthrough { uint32_t ordinal; const char* name; };
const NtdllPassthrough kNtdllPassthroughs[] = {
    {269, "RtlCompareMemoryUlong"},
    {279, "RtlEqualString"},
    {289, "RtlInitAnsiString"},
    {301, "RtlNtStatusToDosError"},
    {308, "RtlUnicodeStringToAnsiString"},
};

// 327 XeLoadSection / 328 XeUnloadSection. XBE section header (0x38):
// +0 Flags, +4 VirtualAddress, +8 VirtualSize, +0xC PointerToRawData,
// +0x10 SizeOfRawData, +0x14 SectionName, +0x18 SectionReferenceCount.
// Loading copies the section's raw bytes from the original XBE and zero-fills
// the remainder when its reference count goes from 0 to 1; unloading zeroes it
// at 0. (Pages stay committed: they belong to the launcher image.)
std::vector<uint8_t> g_xbe_file;
CRITICAL_SECTION g_section_lock;

int32_t __stdcall XeLoadSection(uint32_t header) {
    if (header < kXbeBase || header >= kXbeBase + 0x1000) Stop(160, "XeLoadSection(%08x): not an XBE section header", header);
    auto* h = At<uint32_t>(header);
    uint32_t va = h[1], vsize = h[2], raw = h[3], rsize = h[4];
    if (va < kXbeBase || va + vsize > g_image_end || rsize > vsize || raw + rsize > g_xbe_file.size()) Stop(161, "XeLoadSection(%08x): bad section", header);
    EnterCriticalSection(&g_section_lock);
    uint32_t count = h[6]++;
    if (count == 0) {
        memcpy(At<void>(va), g_xbe_file.data() + raw, rsize);
        memset(At<uint8_t>(va) + rsize, 0, vsize - rsize);
        PatchTscSites(va, rsize); // reapply after a section is unloaded/reloaded
        PatchTimingResearch(va, rsize);
    }
    LeaveCriticalSection(&g_section_lock);
    Log("XeLoadSection(%s @ %08x, %#x bytes): references %u -> %u", At<const char>(h[5]), va, vsize, count, count + 1);
    return kStatusSuccess;
}

int32_t __stdcall XeUnloadSection(uint32_t header) {
    if (header < kXbeBase || header >= kXbeBase + 0x1000) Stop(162, "XeUnloadSection(%08x): not an XBE section header", header);
    auto* h = At<uint32_t>(header);
    EnterCriticalSection(&g_section_lock);
    if (h[6] == 0) Stop(163, "XeUnloadSection(%s): section is not loaded", At<const char>(h[5]));
    uint32_t count = --h[6];
    if (count == 0) memset(At<void>(h[1]), 0, h[2]);
    LeaveCriticalSection(&g_section_lock);
    Log("XeUnloadSection(%s): references -> %u", At<const char>(h[5]), count);
    return kStatusSuccess;
}


// Contiguous memory. The console maps its 64 MiB of RAM at 0x80000000 +
// physical address; contiguous allocations come from that window. Modelled
// by reserving 0x80000000-0x84000000 (large-address-aware process) and a page
// allocator; physical address = virtual - 0x80000000.
constexpr uint32_t kContiguousBase = 0x80000000u, kContiguousPages = 0x4000;   // 64 MiB
uint32_t g_contiguous_owner[kContiguousPages];   // first page of each allocation holds its page count; others 0xFFFFFFFF
CRITICAL_SECTION g_contiguous_lock;

void ReserveContiguousWindow() {
    // RAM always exists on the console: the whole window is committed.
    // MEM_WRITE_WATCH lets the renderer see which pages the title (or the
    // runtime on its behalf) wrote, replacing the reference renderer's
    // "always dirty" surface uploads and uncached texture uploads (gpu.inc).
    void* p = VirtualAlloc(At<void>(kContiguousBase), kContiguousPages * 0x1000u, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
    if (p != At<void>(kContiguousBase)) Stop(170, "cannot reserve the 0x80000000 contiguous-memory window (error %lu)", GetLastError());
    InitializeCriticalSection(&g_contiguous_lock);
    // Kernel-owned top of RAM: GPU instance memory (pages 0x3FE0-0x3FEF) and
    // the page-frame database (0x3FF0-0x3FFF), as in the console layout.
    g_contiguous_owner[0x3FE0] = 32;
    for (uint32_t i = 1; i < 32; ++i) g_contiguous_owner[0x3FE0 + i] = 0xFFFFFFFFu;
    g_contiguous_pages_used += 32;
    Log("contiguous memory window %08x-%08x reserved", kContiguousBase, kContiguousBase + kContiguousPages * 0x1000u);
}

extern "C" void Nv2aOwnedFlushGuest(uint32_t va, uint32_t size);
// 166 MmAllocateContiguousMemoryEx(bytes, lowest, highest physical, alignment, protect)
uint32_t __stdcall MmAllocateContiguousMemoryEx(uint32_t bytes, uint32_t lowest, uint32_t highest, uint32_t alignment, uint32_t protect) {
    uint32_t pages = (bytes + 0xFFF) / 0x1000;
    uint32_t align_pages = alignment > 0x1000 ? alignment / 0x1000 : 1;
    if (!pages || (alignment & (alignment - 1))) Stop(171, "MmAllocateContiguousMemoryEx(%#x, align %#x): unsupported", bytes, alignment);
    uint32_t first = (lowest + 0xFFF) / 0x1000, last = highest >= kContiguousPages * 0x1000u ? kContiguousPages - 1 : highest / 0x1000;
    EnterCriticalSection(&g_contiguous_lock);
    uint32_t found = UINT32_MAX;
    for (uint32_t start = (first + align_pages - 1) / align_pages * align_pages; start + pages - 1 <= last; start += align_pages) {
        bool free = true;
        for (uint32_t i = 0; i < pages && free; ++i) if (g_contiguous_owner[start + i]) free = false;
        if (free) { found = start; break; }
    }
    if (found != UINT32_MAX) {
        g_contiguous_owner[found] = pages;
        for (uint32_t i = 1; i < pages; ++i) g_contiguous_owner[found + i] = 0xFFFFFFFFu;
    }
    LeaveCriticalSection(&g_contiguous_lock);
    if (found == UINT32_MAX) { Log("MmAllocateContiguousMemoryEx(%#x): no space", bytes); return 0; }
    uint32_t address = kContiguousBase + found * 0x1000;
    // Pages of a freed allocation can still belong to a pending renderer-owned
    // surface (PAGE_NOACCESS until first access). Committing below resets their
    // protection and the memset would bypass the fault, leaving the record to be
    // completed later over the new owner's data (long-session texture noise /
    // blank textures after many scene loads). Complete such records first.
    Nv2aOwnedFlushGuest(address, pages * 0x1000u);
    DWORD protection = (protect & 0xFF) ? (protect & 0xFF) : PAGE_READWRITE;   // cache attribute bits (0x200/0x400) do not apply here
    if (!VirtualAlloc(At<void>(address), pages * 0x1000u, MEM_COMMIT, protection)) Stop(172, "contiguous commit failed (error %lu)", GetLastError());
    g_contiguous_pages_used += pages;
    memset(At<void>(address), 0, pages * 0x1000u);
    Log("MmAllocateContiguousMemoryEx(%#x, phys %#x-%#x, align %#x, protect %#x) -> %08x", bytes, lowest, highest, alignment, protect, address);
    return address;
}

// 165 MmAllocateContiguousMemory(bytes)
uint32_t __stdcall MmAllocateContiguousMemory(uint32_t bytes) {
    return MmAllocateContiguousMemoryEx(bytes, 0, 0xFFFFFFFFu, 0, PAGE_READWRITE);
}

uint32_t ContiguousPage(uint32_t address, const char* caller) {
    if (address < kContiguousBase || address >= kContiguousBase + kContiguousPages * 0x1000u || (address & 0xFFF))
        Stop(173, "%s(%08x): not a contiguous allocation", caller, address);
    uint32_t page = (address - kContiguousBase) / 0x1000;
    if (!g_contiguous_owner[page] || g_contiguous_owner[page] == 0xFFFFFFFFu) Stop(173, "%s(%08x): not the start of an allocation", caller, address);
    return page;
}

// 171 MmFreeContiguousMemory
void __stdcall MmFreeContiguousMemory(uint32_t address) {
    EnterCriticalSection(&g_contiguous_lock);
    uint32_t page = ContiguousPage(address, "MmFreeContiguousMemory"), pages = g_contiguous_owner[page];
    for (uint32_t i = 0; i < pages; ++i) g_contiguous_owner[page + i] = 0;
    LeaveCriticalSection(&g_contiguous_lock);
    g_contiguous_pages_used -= pages;
}

// 181 MmQueryStatistics(MM_STATISTICS*): Length, TotalPhysicalPages,
// AvailablePages, VirtualMemoryBytesCommitted, VirtualMemoryBytesReserved,
// CachePagesCommitted, PoolPagesCommitted, StackPagesCommitted,
// ImagePagesCommitted. Accounting of a 64 MiB console: the XBE image,
// contiguous allocations, the title's committed virtual memory and one
// 64 KiB stack per Xbox thread are charged against 16384 pages.
int32_t __stdcall MmQueryStatistics(uint32_t* stats) {
    if (!stats || stats[0] != 0x24) Stop(175, "MmQueryStatistics: unexpected structure length %u", stats ? stats[0] : 0);
    uint32_t image_pages = (g_image_end - kXbeBase + 0xFFF) / 0x1000;
    uint32_t stack_pages = g_xbox_threads.load() * 16;
    uint32_t virtual_pages = static_cast<uint32_t>(g_virtual_committed.load() / 0x1000);
    uint32_t used = image_pages + stack_pages + virtual_pages + g_contiguous_pages_used.load() + g_file_cache_pages.load();
    if (used > kContiguousPages) Stop(176, "MmQueryStatistics: accounted %u pages exceed 64 MiB", used);
    stats[1] = kContiguousPages;
    stats[2] = kContiguousPages - used;
    stats[3] = static_cast<uint32_t>(g_virtual_committed.load());
    stats[4] = static_cast<uint32_t>(g_virtual_reserved.load());
    stats[5] = g_file_cache_pages.load(); stats[6] = 0;
    stats[7] = stack_pages;
    stats[8] = image_pages;
    Log("MmQueryStatistics: %u of %u pages available (image %u, stacks %u, virtual %u, contiguous %u)",
        stats[2], stats[1], image_pages, stack_pages, virtual_pages, g_contiguous_pages_used.load());
    return kStatusSuccess;
}

// 173 MmGetPhysicalAddress: contiguous window only.
uint32_t __stdcall MmGetPhysicalAddress(uint32_t address) {
    if (address < kContiguousBase || address >= kContiguousBase + kContiguousPages * 0x1000u)
        Stop(174, "MmGetPhysicalAddress(%08x): only contiguous-window addresses have a modelled physical address", address);
    return address - kContiguousBase;
}

// 168 MmClaimGpuInstanceMemory(NumberOfBytes, NumberOfPaddingBytes*) -> end
// of the GPU instance memory (0x83FF0000); the 64 KiB page-frame database
// above it is the padding.
uint32_t __stdcall MmClaimGpuInstanceMemory(uint32_t bytes, uint32_t* padding) {
    if (padding) *padding = 0x10000;
    if (bytes != 0xFFFFFFFFu && bytes > 0x10000) Stop(177, "MmClaimGpuInstanceMemory(%#x): more than the 64 KiB instance memory", bytes);
    Log("MmClaimGpuInstanceMemory(%#x) -> 83ff0000 (padding 0x10000)", bytes);
    return 0x83FF0000u;
}

// 178 MmPersistContiguousMemory(address, bytes, persist): keeps memory across a
// title reboot on the console; there is no reboot here, so only recorded.
void __stdcall MmPersistContiguousMemory(uint32_t address, uint32_t bytes, uint32_t persist) {
    Log("MmPersistContiguousMemory(%08x, %#x, %u): recorded (no reboot is modelled)", address, bytes, persist & 0xFF);
}

// 180 MmQueryAllocationSize
uint32_t __stdcall MmQueryAllocationSize(uint32_t address) {
    EnterCriticalSection(&g_contiguous_lock);
    uint32_t pages = g_contiguous_owner[ContiguousPage(address, "MmQueryAllocationSize")];
    LeaveCriticalSection(&g_contiguous_lock);
    return pages * 0x1000u;
}

// 37 FscSetCacheSize(NumberOfCachePages): the console reserves that much RAM
// for its file-system cache. Host files are cached by Windows; the pages are
// charged to the 64 MiB accounting (CachePagesCommitted).
int32_t __stdcall FscSetCacheSize(uint32_t pages) {
    if (pages > 0x2000) Stop(178, "FscSetCacheSize(%u pages): implausibly large", pages);
    g_file_cache_pages = pages;
    Log("FscSetCacheSize(%u pages)", pages);
    return kStatusSuccess;
}

// 128 KeQuerySystemTime: 100 ns units since 1601-01-01 UTC, the same epoch on both systems.
void __stdcall KeQuerySystemTime(uint64_t* time) {
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    *time = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

// 199 NtFreeVirtualMemory(BaseAddress*, RegionSize*, FreeType)
int32_t __stdcall NtFreeVirtualMemory(uint32_t* base, uint32_t* size, uint32_t type) {
    if (!base || !size || (type != MEM_DECOMMIT && type != MEM_RELEASE)) Stop(113, "NtFreeVirtualMemory(type %#x): unsupported", type);
    uint32_t address = *base, bytes = *size;
    if (address >= kContiguousBase) Stop(114, "NtFreeVirtualMemory(%08x): contiguous window", address);
    MEMORY_BASIC_INFORMATION mbi{};
    VirtualQuery(At<void>(address), &mbi, sizeof mbi);
    if (type == MEM_RELEASE) {
        if (bytes) Stop(115, "NtFreeVirtualMemory(MEM_RELEASE) with nonzero size");
        bytes = static_cast<uint32_t>(mbi.RegionSize);
    }
    uint32_t start = address & ~0xFFFu, end = (address + bytes + 0xFFF) & ~0xFFFu;
    if (!VirtualFree(At<void>(start), type == MEM_RELEASE ? 0 : end - start, type)) {
        DWORD e = GetLastError();
        Log("NtFreeVirtualMemory(%08x, %#x, %#x) failed (error %lu)", address, bytes, type, e);
        return static_cast<int32_t>(0xC000009F);   // STATUS_INVALID_PAGE_PROTECTION... (unexpected) 
    }
    if (mbi.State == MEM_COMMIT) g_virtual_committed -= std::min<uint64_t>(g_virtual_committed.load(), end - start);
    if (type == MEM_RELEASE) g_virtual_reserved -= std::min<uint64_t>(g_virtual_reserved.load(), end - start);
    *base = start; *size = end - start;
    return kStatusSuccess;
}

// 217 NtQueryVirtualMemory(BaseAddress, MEMORY_BASIC_INFORMATION*): same 28-byte layout as Windows.
int32_t __stdcall NtQueryVirtualMemory(uint32_t address, MEMORY_BASIC_INFORMATION* info) {
    if (!VirtualQuery(At<void>(address), info, sizeof *info)) return static_cast<int32_t>(0xC000000D);   // STATUS_INVALID_PARAMETER
    return kStatusSuccess;
}

struct KernelFunction { uint32_t ordinal; const void* function; const char* name; };
const KernelFunction kKernelFunctions[] = {
    {335, reinterpret_cast<const void*>(&XcSHAInit), "XcSHAInit"},
    {336, reinterpret_cast<const void*>(&XcSHAUpdate), "XcSHAUpdate"},
    {337, reinterpret_cast<const void*>(&XcSHAFinal), "XcSHAFinal"},
    {340, reinterpret_cast<const void*>(&XcHMAC), "XcHMAC"},
    {1, reinterpret_cast<const void*>(&AvGetSavedDataAddress), "AvGetSavedDataAddress"},
    {2, reinterpret_cast<const void*>(&AvSendTVEncoderOption), "AvSendTVEncoderOption"},
    {3, reinterpret_cast<const void*>(&AvSetDisplayMode), "AvSetDisplayMode"},
    {4, reinterpret_cast<const void*>(&AvSetSavedDataAddress), "AvSetSavedDataAddress"},
    {15, reinterpret_cast<const void*>(&ExAllocatePoolWithTag), "ExAllocatePoolWithTag"},
    {17, reinterpret_cast<const void*>(&ExFreePool), "ExFreePool"},
    {23, reinterpret_cast<const void*>(&ExQueryPoolBlockSize), "ExQueryPoolBlockSize"},
    {24, reinterpret_cast<const void*>(&ExQueryNonVolatileSetting), "ExQueryNonVolatileSetting"},
    {46, reinterpret_cast<const void*>(&HalReadWritePCISpace), "HalReadWritePCISpace"},
    {47, reinterpret_cast<const void*>(&HalRegisterShutdownNotification), "HalRegisterShutdownNotification"},
    {107, reinterpret_cast<const void*>(&KeInitializeDpc), "KeInitializeDpc"},
    {113, reinterpret_cast<const void*>(&KeInitializeTimerEx), "KeInitializeTimerEx"},
    {128, reinterpret_cast<const void*>(&KeQuerySystemTime), "KeQuerySystemTime"},
    {149, reinterpret_cast<const void*>(&KeSetTimer), "KeSetTimer"},
    {37, reinterpret_cast<const void*>(&FscSetCacheSize), "FscSetCacheSize"},
    {44, reinterpret_cast<const void*>(&HalGetInterruptVector), "HalGetInterruptVector"},
    {97, reinterpret_cast<const void*>(&KeCancelTimer), "KeCancelTimer"},
    {98, reinterpret_cast<const void*>(&KeConnectInterrupt), "KeConnectInterrupt"},
    {99, reinterpret_cast<const void*>(&KeDelayExecutionThread), "KeDelayExecutionThread"},
    {100, reinterpret_cast<const void*>(&KeDisconnectInterrupt), "KeDisconnectInterrupt"},
    {109, reinterpret_cast<const void*>(&KeInitializeInterrupt), "KeInitializeInterrupt"},
    {119, reinterpret_cast<const void*>(&KeInsertQueueDpc), "KeInsertQueueDpc"},
    {129, reinterpret_cast<const void*>(&KeRaiseIrqlToDpcLevel), "KeRaiseIrqlToDpcLevel"},
    {137, reinterpret_cast<const void*>(&KeRemoveQueueDpc), "KeRemoveQueueDpc"},
    {139, reinterpret_cast<const void*>(&KeRestoreFloatingPointState), "KeRestoreFloatingPointState"},
    {142, reinterpret_cast<const void*>(&KeSaveFloatingPointState), "KeSaveFloatingPointState"},
    {145, reinterpret_cast<const void*>(&KeSetEvent), "KeSetEvent"},
    {151, reinterpret_cast<const void*>(&KeStallExecutionProcessor), "KeStallExecutionProcessor"},
    {153, reinterpret_cast<const void*>(&KeSynchronizeExecution), "KeSynchronizeExecution"},
    {159, reinterpret_cast<const void*>(&KeWaitForSingleObject), "KeWaitForSingleObject"},
    {160, reinterpret_cast<const void*>(&KfRaiseIrql), "KfRaiseIrql"},
    {161, reinterpret_cast<const void*>(&KfLowerIrql), "KfLowerIrql"},
    {165, reinterpret_cast<const void*>(&MmAllocateContiguousMemory), "MmAllocateContiguousMemory"},
    {168, reinterpret_cast<const void*>(&MmClaimGpuInstanceMemory), "MmClaimGpuInstanceMemory"},
    {166, reinterpret_cast<const void*>(&MmAllocateContiguousMemoryEx), "MmAllocateContiguousMemoryEx"},
    {171, reinterpret_cast<const void*>(&MmFreeContiguousMemory), "MmFreeContiguousMemory"},
    {173, reinterpret_cast<const void*>(&MmGetPhysicalAddress), "MmGetPhysicalAddress"},
    {175, reinterpret_cast<const void*>(&MmLockUnlockBufferPages), "MmLockUnlockBufferPages"},
    {176, reinterpret_cast<const void*>(&MmLockUnlockPhysicalPage), "MmLockUnlockPhysicalPage"},
    {178, reinterpret_cast<const void*>(&MmPersistContiguousMemory), "MmPersistContiguousMemory"},
    {180, reinterpret_cast<const void*>(&MmQueryAllocationSize), "MmQueryAllocationSize"},
    {181, reinterpret_cast<const void*>(&MmQueryStatistics), "MmQueryStatistics"},
    {184, reinterpret_cast<const void*>(&NtAllocateVirtualMemory), "NtAllocateVirtualMemory"},
    {67, reinterpret_cast<const void*>(&IoCreateSymbolicLink), "IoCreateSymbolicLink"},
    {69, reinterpret_cast<const void*>(&IoDeleteSymbolicLink), "IoDeleteSymbolicLink"},
    {187, reinterpret_cast<const void*>(&NtClose), "NtClose"},
    {143, reinterpret_cast<const void*>(&KeSetBasePriorityThread), "KeSetBasePriorityThread"},
    {189, reinterpret_cast<const void*>(&NtCreateEvent), "NtCreateEvent"},
    {190, reinterpret_cast<const void*>(&NtCreateFile), "NtCreateFile"},
    {192, reinterpret_cast<const void*>(&NtCreateMutant), "NtCreateMutant"},
    {196, reinterpret_cast<const void*>(&NtDeviceIoControlFile), "NtDeviceIoControlFile"},
    {198, reinterpret_cast<const void*>(&NtFlushBuffersFile), "NtFlushBuffersFile"},
    {199, reinterpret_cast<const void*>(&NtFreeVirtualMemory), "NtFreeVirtualMemory"},
    {200, reinterpret_cast<const void*>(&NtFsControlFile), "NtFsControlFile"},
    {202, reinterpret_cast<const void*>(&NtOpenFile), "NtOpenFile"},
    {203, reinterpret_cast<const void*>(&NtOpenSymbolicLinkObject), "NtOpenSymbolicLinkObject"},
    {207, reinterpret_cast<const void*>(&NtQueryDirectoryFile), "NtQueryDirectoryFile"},
    {210, reinterpret_cast<const void*>(&NtQueryFullAttributesFile), "NtQueryFullAttributesFile"},
    {211, reinterpret_cast<const void*>(&NtQueryInformationFile), "NtQueryInformationFile"},
    {215, reinterpret_cast<const void*>(&NtQuerySymbolicLinkObject), "NtQuerySymbolicLinkObject"},
    {217, reinterpret_cast<const void*>(&NtQueryVirtualMemory), "NtQueryVirtualMemory"},
    {218, reinterpret_cast<const void*>(&NtQueryVolumeInformationFile), "NtQueryVolumeInformationFile"},
    {219, reinterpret_cast<const void*>(&NtReadFile), "NtReadFile"},
    {221, reinterpret_cast<const void*>(&NtReleaseMutant), "NtReleaseMutant"},
    {224, reinterpret_cast<const void*>(&NtResumeThread), "NtResumeThread"},
    {225, reinterpret_cast<const void*>(&NtSetEvent), "NtSetEvent"},
    {226, reinterpret_cast<const void*>(&NtSetInformationFile), "NtSetInformationFile"},
    {231, reinterpret_cast<const void*>(&NtSuspendThread), "NtSuspendThread"},
    {233, reinterpret_cast<const void*>(&NtWaitForSingleObject), "NtWaitForSingleObject"},
    {234, reinterpret_cast<const void*>(&NtWaitForSingleObjectEx), "NtWaitForSingleObjectEx"},
    {235, reinterpret_cast<const void*>(&NtWaitForMultipleObjectsEx), "NtWaitForMultipleObjectsEx"},
    {236, reinterpret_cast<const void*>(&NtWriteFile), "NtWriteFile"},
    {327, reinterpret_cast<const void*>(&XeLoadSection), "XeLoadSection"},
    {328, reinterpret_cast<const void*>(&XeUnloadSection), "XeUnloadSection"},
    {277, reinterpret_cast<const void*>(&RtlEnterCriticalSection), "RtlEnterCriticalSection"},
    {291, reinterpret_cast<const void*>(&RtlInitializeCriticalSection), "RtlInitializeCriticalSection"},
    {294, reinterpret_cast<const void*>(&RtlLeaveCriticalSection), "RtlLeaveCriticalSection"},
    {238, reinterpret_cast<const void*>(&NtYieldExecution), "NtYieldExecution"},
    {246, reinterpret_cast<const void*>(&ObReferenceObjectByHandle), "ObReferenceObjectByHandle"},
    {250, reinterpret_cast<const void*>(&ObfDereferenceObject), "ObfDereferenceObject"},
    {255, reinterpret_cast<const void*>(&PsCreateSystemThreadEx), "PsCreateSystemThreadEx"},
    {258, reinterpret_cast<const void*>(&PsTerminateSystemThread), "PsTerminateSystemThread"},
};

// --- Kernel thunks -------------------------------------------------------------

extern "C" [[noreturn]] void __cdecl ReportUnimplementedKernelCall(const uint32_t* frame) {
    // frame[0] = ordinal pushed by the stub, frame[1] = return address in the
    // original code, frame[2..] = the caller's stdcall arguments.
    uint32_t ordinal = frame[0], ret = frame[1];
    const char* where = SectionName(ret);
    Log("unimplemented kernel function: ordinal %u, called from %08x (%s), thread %lu", ordinal, ret - 6, where ? where : "?", GetCurrentThreadId());
    for (int i = 0; i < 10; ++i) Log("  arg%d = %08x", i, frame[2 + i]);
    Log("  stack (code addresses):");
    for (int i = 2; i < 1400; ++i) {
        if (!Readable(frame + i, 4)) break;
        const char* w = SectionName(frame[i]);
        if (w && frame[i] >= 0x11000 && frame[i] < 0x171020) Log("    [+%03x] %08x %s", 4 * i, frame[i], w);
    }
    Stop(3, "unimplemented kernel ordinal %u", ordinal);
}

extern "C" __declspec(naked) void UnimplementedKernelCall() {
    __asm {
        mov eax, esp
        push eax
        call ReportUnimplementedKernelCall
        int 3
    }
}

void FillKernelThunks(uint32_t thunk_va) {
    auto* thunk = At<uint32_t>(thunk_va);
    size_t count = 0;
    while (thunk[count]) ++count;
    g_stub_bytes = count * 16;
    g_stubs = static_cast<uint8_t*>(VirtualAlloc(nullptr, g_stub_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    if (!g_stubs) Stop(30, "cannot allocate kernel stubs");
    size_t stubs = 0, implemented = 0;
    for (size_t i = 0; i < count; ++i) {
        uint32_t raw = thunk[i];
        if (!(raw & 0x80000000u)) Stop(31, "thunk %zu is not an ordinal import (%08x)", i, raw);
        uint32_t ordinal = raw & 0x7fffffffu;
        const KernelFunction* fn = nullptr;
        for (const auto& k : kKernelFunctions) if (k.ordinal == ordinal) fn = &k;
        if (fn) {
            thunk[i] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(fn->function));
            ++implemented;
            continue;
        }
        const NtdllPassthrough* pass = nullptr;
        for (const auto& k : kNtdllPassthroughs) if (k.ordinal == ordinal) pass = &k;
        if (pass) {
            FARPROC target = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), pass->name);
            if (!target) Stop(34, "ntdll has no %s", pass->name);
            thunk[i] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(target));
            ++implemented;
            continue;
        }
        const ProvidedData* data = nullptr;
        for (const auto& k : kProvidedData) if (k.ordinal == ordinal) data = &k;
        if (data) {
            auto* page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
            if (!page) Stop(33, "cannot allocate %s", data->name);
            data->init(page);
            RegisterGuarded(page, data->name, data->fields, data->field_count, data->writable, data->refresh);
            thunk[i] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(page));
            ++implemented;
            continue;
        }
        if (const char* name = ghoulies::kernel_data_name(ordinal)) {
            auto* page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS));
            if (!page) Stop(32, "cannot allocate data guard");
            g_data_guards.push_back({ordinal, name, page});
            thunk[i] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(page));
            continue;
        }
        uint8_t* stub = g_stubs + i * 16;
        stub[0] = 0x68; memcpy(stub + 1, &ordinal, 4);                       // push ordinal
        int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&UnimplementedKernelCall) - reinterpret_cast<uintptr_t>(stub + 10));
        stub[5] = 0xE9; memcpy(stub + 6, &rel, 4);                           // jmp UnimplementedKernelCall
        thunk[i] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(stub));
        ++stubs;
    }
    DWORD old;
    VirtualProtect(g_stubs, g_stub_bytes, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), g_stubs, g_stub_bytes);
    Log("kernel thunks: %zu imports, %zu implemented, %zu failing function stubs, %zu guarded data exports",
        count, implemented, stubs, g_data_guards.size());
}

// --- Fault reporting -----------------------------------------------------------

extern "C" void Nv2aGlCounters(unsigned long long* out);
// Last-chance report for faults outside the original code (runtime, host
// libraries), which the vectored handler leaves to their own handlers.
LONG WINAPI UnhandledFault(EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* er = ep->ExceptionRecord; const CONTEXT* c = ep->ContextRecord;
    HMODULE module = nullptr; char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(c->Eip)), &module)) GetModuleFileNameA(module, name, MAX_PATH);
    Log("unhandled host exception %08x at EIP %08x (%s +%#x), thread %lu, address %08x", er->ExceptionCode, c->Eip, name,
        module ? static_cast<unsigned>(c->Eip - reinterpret_cast<uintptr_t>(module)) : 0u, GetCurrentThreadId(),
        er->NumberParameters > 1 ? static_cast<uint32_t>(er->ExceptionInformation[1]) : 0u);
    Log("  eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x esp=%08x",
        c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    LogStack(c->Esp, 16);
    Stop(5, "unhandled host exception");
}

LONG CALLBACK Vectored(EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    CONTEXT* c = ep->ContextRecord;
    DWORD code = er->ExceptionCode;
    if (HandleTimingAudit(code, c)) return EXCEPTION_CONTINUE_EXECUTION;
    if (EmulateOriginalTsc(code, c)) return EXCEPTION_CONTINUE_EXECUTION;
    if (code == 0xE06D7363 || code == 0x406D1388 || code == DBG_PRINTEXCEPTION_C || code == 0x4001000A) return EXCEPTION_CONTINUE_SEARCH;

    if (code == EXCEPTION_SINGLE_STEP) {
        bool handled = HandleTimingMovementWatch(c);
        // A data breakpoint traps after the writing instruction. It may also
        // coincide with the trap used to close one of our guarded pages.
        if (g_watch_address && c->Dr0 == g_watch_address && (c->Dr6 & 1)) {
            uint32_t hit = ++g_watch_hits;
            const char* where = SectionName(c->Eip);
            Log("watch: hit %u address %08x value %08x next EIP %08x (%s), thread %lu",
                hit, g_watch_address, *At<uint32_t>(g_watch_address), c->Eip, where ? where : "host", GetCurrentThreadId());
            Log("  eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x esp=%08x",
                c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
            LogStack(c->Esp, 12);
            c->Dr6 &= ~1u;
            handled = true;
        }
        // Close a guarded page reopened for one permitted read. WOW64 does not
        // always supply DR6.BS when no hardware breakpoint is armed.
        if (t_reopened_page) {
            DWORD old;
            if (!VirtualProtect(t_reopened_page, 0x1000, PAGE_NOACCESS, &old)) Stop(230, "watch: cannot close guarded page");
            t_reopened_page = nullptr;
            c->EFlags &= ~0x100u;
            c->Dr6 &= ~(1u << 14);
            handled = true;
        }
        if (handled) return EXCEPTION_CONTINUE_EXECUTION;
    }

    uintptr_t fault_address = (code == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) ? er->ExceptionInformation[1] : 0;
    const GuardedBlock* block = fault_address ? GuardedFor(fault_address) : nullptr;
    if (block) {
        uint32_t offset = static_cast<uint32_t>(fault_address - reinterpret_cast<uintptr_t>(block->page));
        bool permitted = er->ExceptionInformation[0] == 0 || (block->writable && er->ExceptionInformation[0] == 1);
        bool known = false;
        for (size_t i = 0; i < block->field_count; ++i) if (block->fields.load(std::memory_order_acquire)[i] == offset) known = true;
        if (permitted && known && !t_reopened_page) {
            DWORD old;
            if (block->refresh) { VirtualProtect(block->page, 0x1000, PAGE_READWRITE, &old); block->refresh(block->page); }
            VirtualProtect(block->page, 0x1000, block->writable ? PAGE_READWRITE : PAGE_READONLY, &old);
            t_reopened_page = block->page;
            c->EFlags |= 0x100;   // trap after this one instruction
            g_guarded_reads.fetch_add(1, std::memory_order_relaxed);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        const char* where = SectionName(c->Eip);
        Log("unprovided Xbox %s field: %s of offset +%#x at EIP %08x (%s), thread %lu", block->name,
            er->ExceptionInformation[0] == 0 ? "read" : "write", offset, c->Eip, where ? where : "?", GetCurrentThreadId());
        Log("  eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x esp=%08x",
            c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
        LogStack(c->Esp, 12);
        Stop(9, "unprovided Xbox %s field +%#x", block->name, offset);
    }

    // Renderer-owned surface pages (deferred downloads): complete, then retry.
    if (code == EXCEPTION_ACCESS_VIOLATION && fault_address >= kContiguousBase && fault_address < kContiguousBase + kContiguousPages * 0x1000u &&
        Nv2aOwnedFault(fault_address, c->Eip)) return EXCEPTION_CONTINUE_EXECUTION;
    if (code == EXCEPTION_PRIV_INSTRUCTION && c->Eip >= kXbeBase && c->Eip < g_image_end && EmulatePortIo(c)) return EXCEPTION_CONTINUE_EXECUTION;
    if (((fault_address >= kNv2aBase && fault_address < kNv2aBase + kNv2aSize) || (fault_address >= kApuBase && fault_address < kApuBase + kApuSize)) && code == EXCEPTION_ACCESS_VIOLATION) {
        if (EmulateMmio(c, static_cast<uint32_t>(fault_address))) return EXCEPTION_CONTINUE_EXECUTION;
        auto* bip = At<const uint8_t>(c->Eip);
        Stop(184, "NV2A access by an unemulated instruction at EIP %08x (%s): %02x %02x %02x %02x %02x %02x, address %08x",
             c->Eip, SectionName(c->Eip) ? SectionName(c->Eip) : "?", bip[0], bip[1], bip[2], bip[3], bip[4], bip[5], static_cast<uint32_t>(fault_address));
    }
    const DataGuard* guard = nullptr;
    if (fault_address) for (const auto& g : g_data_guards) if (InRange(fault_address, g.page, 0x1000)) guard = &g;
    bool in_original = c->Eip >= kXbeBase && c->Eip < g_image_end;
    if (!in_original && !guard && !SectionName(c->Eip)) {   // not ours to judge; note access violations
        static std::atomic<int> noted{0};
        if (code == EXCEPTION_ACCESS_VIOLATION && noted++ < 3) {
            HMODULE module = nullptr; char name[MAX_PATH] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(c->Eip)), &module)) GetModuleFileNameA(module, name, MAX_PATH);
            Log("first-chance host access violation at EIP %08x (%s +%#x), thread %lu, %s of %08x", c->Eip, name,
                module ? static_cast<unsigned>(c->Eip - reinterpret_cast<uintptr_t>(module)) : 0u, GetCurrentThreadId(),
                er->ExceptionInformation[0] ? "write" : "read", static_cast<uint32_t>(fault_address));
            Log("  eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x esp=%08x",
                c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
            LogStack(c->Esp, 16);
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const char* where = SectionName(c->Eip);
    Log("exception %08x at EIP %08x (%s), thread %lu", code, c->Eip, where ? where : "?", GetCurrentThreadId());
    if (code == EXCEPTION_ACCESS_VIOLATION) {
        const char* kind = er->ExceptionInformation[0] == 0 ? "read" : er->ExceptionInformation[0] == 1 ? "write" : "execute";
        Log("  %s of %08x", kind, static_cast<uint32_t>(fault_address));
        if (guard) Log("  -> kernel data export %u %s is not implemented (offset +%u)", guard->ordinal, guard->name,
                       static_cast<unsigned>(fault_address - reinterpret_cast<uintptr_t>(guard->page)));
    }
    auto* ip = At<const uint8_t>(c->Eip);
    if (Readable(ip, 8)) {
        Log("  bytes %02x %02x %02x %02x %02x %02x %02x %02x", ip[0], ip[1], ip[2], ip[3], ip[4], ip[5], ip[6], ip[7]);
        if (ip[0] == 0x64) Log("  -> fs-segment access not redirected (Xbox KPCR layout expected)");
    }
    Log("  eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x esp=%08x eflags=%08x",
        c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp, c->EFlags);
    LogStack(c->Esp, 16);
    Stop(4, "unsupported fault in original code");
}

// --- Startup -------------------------------------------------------------------

DWORD WINAPI TitleThread(void*) {
    CreateThreadEnv(nullptr, 0, __readfsdword(0x04));   // entry thread: no TLS block
    Log("entering original entry point %08x on thread %lu", g_entry, GetCurrentThreadId());
    reinterpret_cast<void (__cdecl*)()>(static_cast<uintptr_t>(g_entry))();
    // The console runs the entry point as a system thread; returning terminates that thread.
    Log("original entry point returned on thread %lu; terminating thread", GetCurrentThreadId());
    ExitThread(0);
}

// The XBE header must occupy 0x10000, overwriting the launcher's PE header.
// Windows reads the main image header later (CreateThread takes default stack
// sizes from it), so point the PEB's image base at this DLL's valid header.
void RedirectProcessImageHeader() {
    auto* peb = reinterpret_cast<uint8_t*>(__readfsdword(0x30));
    auto** image_base = reinterpret_cast<void**>(peb + 0x08);   // x86 PEB.ImageBaseAddress
    if (*image_base != At<void>(kXbeBase)) Stop(48, "unexpected process image base %p", *image_base);
    *image_base = &__ImageBase;
    Log("process image header redirected from %08x to runtime DLL %p", kXbeBase, static_cast<void*>(&__ImageBase));

    // Under WOW64 the 64-bit PEB also records the image base and is read when
    // any thread is created (by this runtime or by Windows components such as
    // XAudio2). Redirect it too, after checking its current value.
    struct Pbi64 { LONG ExitStatus; ULONG pad; uint64_t PebBaseAddress, AffinityMask; LONG BasePriority; ULONG pad2; uint64_t UniqueProcessId, InheritedFromUniqueProcessId; };
    using QueryFn = LONG (NTAPI*)(HANDLE, ULONG, void*, ULONG, ULONG*);
    using ReadFn = LONG (NTAPI*)(HANDLE, uint64_t, void*, uint64_t, uint64_t*);
    using WriteFn = LONG (NTAPI*)(HANDLE, uint64_t, const void*, uint64_t, uint64_t*);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto query = reinterpret_cast<QueryFn>(GetProcAddress(ntdll, "NtWow64QueryInformationProcess64"));
    auto read = reinterpret_cast<ReadFn>(GetProcAddress(ntdll, "NtWow64ReadVirtualMemory64"));
    auto write = reinterpret_cast<WriteFn>(GetProcAddress(ntdll, "NtWow64WriteVirtualMemory64"));
    BOOL wow64 = FALSE; IsWow64Process(GetCurrentProcess(), &wow64);
    if (!wow64) return;
    if (!query || !read || !write) Stop(48, "WOW64 PEB access functions missing");
    Pbi64 pbi{}; ULONG got = 0;
    if (query(GetCurrentProcess(), 0 /* ProcessBasicInformation */, &pbi, sizeof pbi, &got) < 0) Stop(48, "cannot query the 64-bit PEB");
    uint64_t current = 0, done = 0;
    LONG rs = read(GetCurrentProcess(), pbi.PebBaseAddress + 0x10, &current, 8, &done);
    if (rs < 0 || done != 8) {
        // Fallback: the 64-bit PEB of a WOW64 process lies below 4 GiB; read it directly if mapped.
        if (pbi.PebBaseAddress < 0xFFFFFFFFull && Readable(reinterpret_cast<void*>(static_cast<uintptr_t>(pbi.PebBaseAddress + 0x10)), 8)) {
            auto* p64 = reinterpret_cast<uint64_t*>(static_cast<uintptr_t>(pbi.PebBaseAddress + 0x10));
            if (*p64 != kXbeBase) Stop(48, "unexpected 64-bit PEB image base %llx", *p64);
            DWORD old; VirtualProtect(p64, 8, PAGE_READWRITE, &old);
            *p64 = reinterpret_cast<uintptr_t>(&__ImageBase);
            Log("64-bit PEB image base redirected directly (PEB64 %llx)", pbi.PebBaseAddress);
            return;
        }
        Stop(48, "cannot read the 64-bit PEB image base (status %#lx, PEB64 %llx, query size %lu)", static_cast<unsigned long>(rs), pbi.PebBaseAddress, got);
    }
    if (current != kXbeBase) Stop(48, "unexpected 64-bit PEB image base %llx", current);
    uint64_t dll = reinterpret_cast<uintptr_t>(&__ImageBase);
    if (write(GetCurrentProcess(), pbi.PebBaseAddress + 0x10, &dll, 8, &done) < 0 || done != 8) Stop(48, "cannot write the 64-bit PEB image base");
    Log("64-bit PEB image base redirected to runtime DLL (PEB64 %llx)", pbi.PebBaseAddress);
}

void MapXbe(const std::vector<uint8_t>& d) {
    if (d.size() < 0x178 || memcmp(d.data(), "XBEH", 4) != 0) Stop(40, "not an XBE");
    uint32_t base = U32(d, 0x104), headers = U32(d, 0x108), image = U32(d, 0x10c);
    if (base != kXbeBase) Stop(41, "unexpected XBE base %08x", base);
    if (headers > d.size() || headers > 0x1000) Stop(42, "unexpected header size %x", headers);
    g_image_end = base + image;

    // The launcher image must own every page of the range.
    for (uintptr_t a = base; a < g_image_end;) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi) || mbi.Type != MEM_IMAGE ||
            reinterpret_cast<uintptr_t>(mbi.AllocationBase) != base)
            Stop(43, "address %08x is not inside the launcher reservation", static_cast<uint32_t>(a));
        a = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    }
    DWORD old;
    if (!VirtualProtect(At<void>(base), image, PAGE_EXECUTE_READWRITE, &old))
        Stop(44, "cannot make XBE range writable (error %lu)", GetLastError());
    memcpy(g_launcher_header, At<void>(base), sizeof g_launcher_header);
    RedirectProcessImageHeader();
    HookThreadCreation();
    memset(At<void>(base), 0, image);
    memcpy(At<void>(base), d.data(), headers);

    uint32_t count = U32(d, 0x11c), table = U32(d, 0x120) - base;
    size_t loaded = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t o = table + i * 56;
        Section s{"", U32(d, o), U32(d, o + 4), U32(d, o + 8), U32(d, o + 12), U32(d, o + 16)};
        uint32_t name_off = U32(d, o + 20) - base;
        if (name_off >= d.size()) Stop(45, "bad section name");
        s.name = reinterpret_cast<const char*>(d.data() + name_off);
        if (s.va < base || s.va + s.vsize > g_image_end || s.rsize > s.vsize || s.raw + s.rsize > d.size()) Stop(46, "section %s out of range", s.name.c_str());
        if (s.flags & kSectionPreload) {
            memcpy(At<void>(s.va), d.data() + s.raw, s.rsize);
            ++loaded;
        }
        g_sections.push_back(s);
    }
    Log("mapped XBE %08x-%08x: %zu of %u sections preloaded (others load via XeLoadSection)", base, g_image_end, loaded, count);

    // TLS directory (header +0x12C): raw start, raw end, index address, callbacks, zero fill, characteristics.
    uint32_t tls = U32(d, 0x12c);
    auto* t = At<uint32_t>(tls);
    g_tls_raw_size = t[1] - t[0]; g_tls_index_address = t[2]; g_tls_zero_fill = t[4];
    if (g_tls_raw_size) Stop(49, "XBE TLS raw template (%#x bytes) not provided", g_tls_raw_size);
    if (t[3]) Stop(49, "XBE TLS callbacks not provided");
    Log("TLS: zero fill %#x, raw %#x, index at %08x", g_tls_zero_fill, g_tls_raw_size, g_tls_index_address);

    g_entry = U32(d, 0x128) ^ kEntryKeyRetail;
    uint32_t thunk = U32(d, 0x158) ^ kThunkKeyRetail;
    if (g_entry < base || g_entry >= g_image_end || thunk < base || thunk >= g_image_end) Stop(47, "entry/thunk outside image");
    Log("entry %08x, kernel thunk table %08x", g_entry, thunk);
    // Native profile key policy matches the reference's no-keys.bin default:
    // all-zero XboxCertificateKey, HMAC-SHA1 over certificate SignatureKey,
    // first sixteen digest bytes. It supports this isolated native save
    // identity; it is NOT a retail-console identity or imported-save promise.
    uint32_t cert = U32(d,0x118);
    if(cert < base || uint64_t(cert-base)+0xd0 > headers || U32(d,cert-base)<0xd0)
        Stop(290,"XBE certificate signature key outside headers");
    uint8_t certificate_key[16]={}, signature_digest[20];
    XcHMAC(certificate_key,16,d.data()+cert-base+0xc0,16,nullptr,0,signature_digest);
    memcpy(g_title_signature_key,signature_digest,16);
    Log("save identity: per-title HMAC-SHA1, reference zero certificate-key policy; native-profile saves only");
    FillKernelThunks(thunk);
    // All reviewed sites reside in preloaded CPU-code sections for this XBE.
    unsigned patched_tsc = 0;
    for (const auto& section : g_sections)
        if ((section.flags & (kSectionPreload | kSectionExecutable)) == (kSectionPreload | kSectionExecutable))
            patched_tsc += PatchTscSites(section.va, section.rsize);
    if (patched_tsc != _countof(kNativeTscSites)) Stop(231, "not all reviewed RDTSC sites were loaded");
    for (const auto& section : g_sections)
        if ((section.flags & (kSectionPreload | kSectionExecutable)) == (kSectionPreload | kSectionExecutable))
            PatchTimingResearch(section.va, section.rsize);
    InstallReplacements();
    InstallDsound();
    PatchFsSites();
    FlushInstructionCache(GetCurrentProcess(), At<void>(base), image);
}

} // namespace

extern "C" __declspec(dllexport) void __cdecl GhRun() {
    InitializeCriticalSection(&g_log_lock);
    InitializeCriticalSection(&g_header_lock);
    InitializeCriticalSection(&g_guard_lock);
    InitializeCriticalSection(&g_threads_lock);
    InitializeCriticalSection(&g_shutdown_lock);
    InitializeCriticalSection(&g_timer_lock);
    g_timer_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    InitializeKernelSync();
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc < 2 || !argv[1][0]) Stop(20, "select your game image through the launcher, or supply the verified XBE path");
    const wchar_t* xbe_path = argv[1];
    if (argc > 2) {
        if (_wfopen_s(&g_report, argv[2], L"w") || !g_report) Stop(229, "cannot open evidence report");
        std::wstring report_path = argv[2];
        size_t slash = report_path.find_last_of(L"\\/");
        g_frame_dir = slash == std::wstring::npos ? L"." : report_path.substr(0, slash);
    }
    wchar_t watch[32] = {};
    DWORD watch_length = GetEnvironmentVariableW(L"GH_WATCH", watch, 32);
    if (watch_length >= 32) Stop(230, "GH_WATCH value exceeds the diagnostic address buffer");
    if (watch_length) {
        wchar_t* end = nullptr;
        unsigned long address = wcstoul(watch, &end, 16);
        if (end == watch || *end || (address & 3) || address < 0x80000000u || address > 0x83FFFFFCu)
            Stop(230, "GH_WATCH must name an aligned four-byte address in contiguous RAM");
        g_watch_address = address;
    }
    DWORD timeout_ms = argc > 3 ? static_cast<DWORD>(_wtoi(argv[3])) * 1000u : 30000u;

    Log("Ghoulies native runtime N5 (NV2A LLE)");
    // 1 ms scheduler resolution for guest sleeps/timed waits (default 15.6 ms).
    if (timeBeginPeriod(1) != TIMERR_NOERROR) Stop(296, "timeBeginPeriod(1) failed");
    std::vector<uint8_t> xbe = ReadAll(xbe_path);
    std::string hash = Sha256Hex(xbe);
    Log("XBE %zu bytes, SHA-256 %s", xbe.size(), hash.c_str());
    if (hash != kExpectedSha256) Stop(25, "XBE is not the recorded original");
    InitializeTimingResearch();

    g_kpcr_slot = TlsAlloc();
    if (g_kpcr_slot == TLS_OUT_OF_INDEXES) Stop(26, "no TLS slot for KPCR");
    AddVectoredExceptionHandler(1, Vectored);
    SetUnhandledExceptionFilter(UnhandledFault);
    // Preserve the single Xbox CPU for title, system, DPC and ISR threads.
    // Host GPU/driver/audio workers inherit the available process CPU mask.
    // GH_SINGLE_CORE_HOST=1 is the old scheduling mode for controlled A/B runs.
    DWORD_PTR process_mask = 0, system_mask = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask) || !process_mask)
        Stop(27, "cannot read process CPU affinity (%lu)", GetLastError());
    g_guest_cpu_mask = process_mask & (~process_mask + 1);
    wchar_t legacy[2] = {};
    DWORD legacy_length = GetEnvironmentVariableW(L"GH_SINGLE_CORE_HOST", legacy, _countof(legacy));
    if (legacy_length && (legacy_length != 1 || legacy[0] != L'1')) Stop(27, "GH_SINGLE_CORE_HOST must be 1 or unset");
    if (legacy_length && !SetProcessAffinityMask(GetCurrentProcess(), g_guest_cpu_mask))
        Stop(27, "cannot select single-core host test mode");
    Log("CPU affinity: guest %#lx, host %#lx", static_cast<unsigned long>(g_guest_cpu_mask),
        static_cast<unsigned long>(legacy_length ? g_guest_cpu_mask : process_mask));
    InitializeCriticalSection(&g_section_lock);
    ReserveContiguousWindow();
    g_pool_heap = HeapCreate(0, 0, 0);
    ReserveNv2a();
    InitializeNativeClock();
    Log("clock: console TSC %llu Hz, host QPC %llu Hz, boot-relative epoch", kXboxTscFrequency, g_tsc_host_frequency);
    StartGpu();
    InitializeIo();
    MapXbe(xbe);
    g_xbe_file = std::move(xbe);
    InitializePresentation();
    StartSampleProfiler();
    HANDLE dpc_thread = CreateNativeThread(0x100000, DpcDispatcherThread, nullptr, false);
    if (!dpc_thread) Stop(51, "cannot create DPC dispatcher thread");
    SetThreadPriority(dpc_thread, THREAD_PRIORITY_TIME_CRITICAL);
    CloseHandle(dpc_thread);
    HANDLE interrupt_thread = CreateNativeThread(0x100000, InterruptThread, nullptr, false);
    if (!interrupt_thread) Stop(52, "cannot create interrupt thread");
    SetThreadPriority(interrupt_thread, THREAD_PRIORITY_TIME_CRITICAL);
    CloseHandle(interrupt_thread);

    HANDLE thread = CreateNativeThread(0x100000, TitleThread, nullptr, false);
    if (!thread) Stop(50, "cannot create title thread (error %lu)", GetLastError());
    TrackThread(thread);
    CloseHandle(thread);

    // Watchdog: run until every Xbox thread has ended or the timeout expires,
    // then report where each live thread is.
    ULONGLONG deadline = GetTickCount64() + timeout_ms;
    for (;;) {
        Sleep(100);
        EnterCriticalSection(&g_threads_lock);
        std::vector<TrackedThread> threads = g_threads;
        LeaveCriticalSection(&g_threads_lock);
        size_t alive = 0;
        for (const auto& t : threads) if (WaitForSingleObject(t.handle, 0) == WAIT_TIMEOUT) ++alive;
        if (!alive) Stop(7, "all %zu Xbox threads ended without a report", threads.size());
        static ULONGLONG next_memory_log = GetTickCount64() + 10000;
        if (GetTickCount64() >= next_memory_log) {
            next_memory_log += 10000;
            PROCESS_MEMORY_COUNTERS_EX pmc{}; pmc.cb = sizeof pmc;
            GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc);
            MEMORY_BASIC_INFORMATION mbi{}; SIZE_T largest = 0, free_total = 0;
            for (uintptr_t a = 0x10000; a < 0xFFFF0000u && VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi); a = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize)
                if (mbi.State == MEM_FREE) { free_total += mbi.RegionSize; largest = std::max(largest, mbi.RegionSize); }
            unsigned long long gl_counts[3] = {}; Nv2aGlCounters(gl_counts);
            { static unsigned long long last[32] = {}, last_tsc = 0; static LARGE_INTEGER last_qpc{}; char line[600]; int n = 0;
              LARGE_INTEGER fq, qpc; QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&qpc); unsigned long long tsc = __rdtsc();
              double interval_ticks = last_tsc ? double(tsc - last_tsc) : 0;   // the slots count TSC ticks over this interval
              (void)fq; last_tsc = tsc; last_qpc = qpc;
              static const char* names[32] = {"readpix","surface","textures","stateupd","drawend","clear","method","shaders","pusher","idle","surfpart","harvest","flip","readgpu","readcopy",
                                              "draw","vtxattr","membuf","uniforms","gldraw","ownflush","ownstamp","ownprot","fence"};
              for (int i = 0; i < 24; ++i) { n += snprintf(line + n, sizeof line - n, " %s %.0f%%", names[i], interval_ticks > 0 ? 100.0 * (g_nv2a_prof[i] - last[i]) / interval_ticks : 0.0); last[i] = g_nv2a_prof[i]; }
              if(g_nv2a_profile_enabled) Log("gpu time:%s", line); else Log("gpu time: detailed method profiling disabled (GH_GPU_PROFILE=1 to enable)"); }
            { // CPU use over the interval: all Xbox threads (one shared logical CPU) and the GPU thread.
              auto cpu = [](HANDLE h) { FILETIME c, e, k, u; if (!GetThreadTimes(h, &c, &e, &k, &u)) return 0ull;
                  return ((unsigned long long)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((unsigned long long)u.dwHighDateTime << 32 | u.dwLowDateTime); };
              static std::unordered_map<DWORD, unsigned long long> last_cpu; static unsigned long long last_gpu = 0;
              unsigned long long guest = 0; std::string top; std::vector<std::pair<unsigned long long, DWORD>> per;
              for (const auto& t : threads) { unsigned long long v = cpu(t.handle), d = v - last_cpu[t.id]; last_cpu[t.id] = v; guest += d; per.push_back({d, t.id}); }
              std::sort(per.rbegin(), per.rend());
              char buf[200]; int n = 0;
              for (size_t i = 0; i < per.size() && i < 4; ++i) n += snprintf(buf + n, sizeof buf - n, " %lu:%.0f%%", per[i].second, per[i].first / 1e6);
              unsigned long long g = cpu(g_gpu_thread_handle); double gpu_pct = (g - last_gpu) / 1e6; last_gpu = g;
              static unsigned long long last_vblanks = 0; unsigned long long vb = g_vblanks.load();
              Log("cpu: Xbox threads %.0f%% of one CPU (busiest%s), GPU thread %.0f%%; %llu vblanks", guest / 1e6, buf, gpu_pct, vb - last_vblanks); last_vblanks = vb; }
            { uint32_t st = 0, bu = 0, mx = 0; AudioLiveCounts(st, bu, mx); Log("audio: live streams %u, buffers %u, MIXIN (3D sources) %u", st, bu, mx); }
            Log("readbacks: %llu calls, %llu MiB colour, %llu MiB depth", g_nv2a_readbacks[0], g_nv2a_readbacks[2] >> 20, g_nv2a_readbacks[3] >> 20);
            Log("memory: private %zu MiB, free address space %zu MiB (largest %zu MiB), gpu methods %llu, shader compiles %llu, texture uploads %llu, surface uploads %llu, presents %llu",
                pmc.PrivateUsage >> 20, free_total >> 20, largest >> 20, g_gpu_methods, gl_counts[0], gl_counts[1], gl_counts[2], g_frames);
        }
        if (GetTickCount64() < deadline) continue;
        Log("timeout after %lu s: %zu of %zu Xbox threads running; %llu guarded reads", timeout_ms / 1000, alive, threads.size(), g_guarded_reads.load());
        // Diagnostics acquire GPU/audio locks; a suspended guest can own them.
        // Collect these live snapshots before suspending for register/stack dumps.
        GpuDiagnostics();
        AudioDiagnostics();
        for (const auto& t : threads) SuspendThread(t.handle);
        for (const auto& t : threads) {
            if (WaitForSingleObject(t.handle, 0) != WAIT_TIMEOUT) { Log("thread %lu: ended", t.id); continue; }
            CONTEXT c{}; c.ContextFlags = CONTEXT_FULL;
            GetThreadContext(t.handle, &c);
            const char* where = SectionName(c.Eip);
            Log("thread %lu: EIP %08x (%s)", t.id, c.Eip, where ? where : "outside image");
            LogStack(c.Esp, 12);
        }
        Stop(6, "timeout: no unsupported dependency reached");
    }
}
