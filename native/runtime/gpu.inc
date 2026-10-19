// NV2A command processing: PFIFO DMA pusher and PGRAPH method dispatch
// (included by ghoulies_runtime.cpp after nv2a.inc).
extern "C" void Nv2aOwnedLock();
extern "C" void Nv2aOwnedUnlock();
extern "C" void Nv2aOwnedFlush(uint32_t physical, uint32_t size);
extern "C" void Nv2aGlBatchLock(bool lock);
extern "C" void Nv2aGlTextureBarrier();
extern "C" void Nv2aGlMethodArray(uint32_t subchannel, uint32_t method, const uint32_t* words, uint32_t count);
extern "C" void Nv2aGlTransformConstants(uint32_t subchannel, uint32_t method, const uint32_t* words, uint32_t count);
extern "C" void Nv2aGlHarvestAllForSpan();
//
// The title's Direct3D writes commands into a push buffer in RAM and moves
// the channel's DMA_PUT. A GPU thread fetches words from DMA_GET to DMA_PUT
// (following jumps, calls and returns), and delivers each method to the
// object bound on its subchannel. Objects are found through RAMHT in PRAMIN.
// Command decoding follows the envytools/XQEMU pusher description.

uint32_t Nv2aReg(uint32_t offset) { uint32_t v = 0; Nv2aStored(offset, 4, v); return v; }
void Nv2aSetReg(uint32_t offset, uint32_t v) { Nv2aStore(offset, v, 4); }

constexpr uint32_t kPfifo = 0x2000;
constexpr uint32_t kCache1Push1 = kPfifo + 0x1204, kCache1DmaPush = kPfifo + 0x1220, kCache1DmaState = kPfifo + 0x1228,
                   kCache1DmaInstance = kPfifo + 0x122C, kCache1DmaPut = kPfifo + 0x1240, kCache1DmaGet = kPfifo + 0x1244,
                   kCache1Ref = kPfifo + 0x1248, kCache1DmaSubroutine = kPfifo + 0x124C, kPfifoMode = kPfifo + 0x504,
                   kPfifoRamht = kPfifo + 0x210, kCache1Push0 = kPfifo + 0x1200, kCache1Pull0 = kPfifo + 0x1250;

HANDLE g_gpu_wake = nullptr;
HANDLE g_gpu_thread_handle = nullptr;
std::atomic<bool> g_gpu_busy{false};

struct GpuObject { uint32_t instance = 0; uint32_t object_class = 0; };
extern struct DisplayMode g_display, g_display_copy;
extern SRWLOCK g_display_lock;
extern std::atomic<uint32_t> g_scanout_start;
GpuObject g_subchannels[8];
uint64_t g_gpu_methods = 0;
uint64_t g_gpu_interrupts_delivered = 0;

struct DmaObject { uint32_t object_class, target, address, limit; };
DmaObject LoadDma(uint32_t instance) {
    if (instance + 12 > 0x100000) Stop(200, "DMA object at PRAMIN %#x out of range", instance);
    uint32_t flags, limit, frame;
    memcpy(&flags, g_pramin + instance, 4); memcpy(&limit, g_pramin + instance + 4, 4); memcpy(&frame, g_pramin + instance + 8, 4);
    return {flags & 0xFFF, (flags >> 16) & 3, (frame & 0xFFFFF000u) | (flags >> 20), limit};
}

uint32_t RamhtLookup(uint32_t handle, uint32_t* engine, uint32_t* channel = nullptr) {
    uint32_t ramht = Nv2aReg(kPfifoRamht);
    uint32_t size = 1u << (((ramht >> 16) & 3) + 12);
    uint32_t base = ((ramht >> 4) & 0x1F) << 12;
    unsigned bits = 0; while ((1u << (bits + 1)) < size) ++bits; bits -= 1;   // ffs(size) - 2
    uint32_t hash = 0, h = handle;
    while (h) { hash ^= h & ((1u << bits) - 1); h >>= bits; }
    hash ^= (Nv2aReg(kCache1Push1) & 0x1F) << (bits - 4);
    if (hash * 8 >= size) Stop(201, "RAMHT hash out of range");
    uint32_t entry_handle, context;
    memcpy(&entry_handle, g_pramin + base + hash * 8, 4); memcpy(&context, g_pramin + base + hash * 8 + 4, 4);
    if (!(context & 0x80000000u) || entry_handle != handle) Stop(202, "RAMHT has no valid entry for object handle %#x", handle);
    if (engine) *engine = (context >> 16) & 3;
    if (channel) *channel = (context >> 24) & 0x1F;
    return (context & 0xFFFF) << 4;
}

void GpuMethod(uint32_t subchannel, uint32_t method, uint32_t parameter);
struct PushHeader { uint32_t at, word; };
PushHeader g_push_history[16] = {};
uint32_t g_push_history_next = 0;

// Runs the DMA pusher until DMA_GET reaches DMA_PUT.
extern "C" uint64_t Nv2aGlHarvestAll();
// Guest writes are harvested when a new DMA_PUT is observed: the title writes
// vertex/texture/surface data before it advances PUT over the commands that
// use them, so every command is processed after the harvest that covers its
// data. (Previously every draw harvested the whole 64 MiB window.)
uint32_t g_harvested_put = 0xFFFFFFFFu;
// The harvest a new PUT requires is taken lazily, at the span's first read of
// page generations (texture fetch, vertex mirror, surface check) instead of
// unconditionally: spans that read no guest memory skip the 64 MiB scan.
bool g_harvest_pending = false;
// Diagnostics: texture barriers by cause ([method >> 2] below 0x200 on Kelvin,
// [128] other Kelvin methods, [129] other classes).
extern "C" unsigned long long g_barrier_causes[130] = {};
// Words are processed in batches with DMA_GET/STATE written back once per
// batch (the title only uses DMA_GET to reclaim push-buffer space, so a
// slightly stale value is conservative). Methods that can block or that the
// title synchronises on (every method below 0x200, all non-Kelvin classes and
// the semaphore methods) see exactly the reference's register state: the
// position before the word is published before they run, and the batch ends
// after them.
inline bool PusherMethodSynchronises(uint32_t sub, uint32_t method) {
    return method < 0x200 || g_subchannels[sub].object_class != 0x97 || method == 0x1d6c || method == 0x1d70;
}
void RunPusher() {
    for (;;) {
        EnterCriticalSection(&g_nv2a_lock);
        uint32_t push0 = Nv2aReg(kCache1Push0), dma_push = Nv2aReg(kCache1DmaPush);
        uint32_t get = Nv2aReg(kCache1DmaGet), put = Nv2aReg(kCache1DmaPut);
        uint32_t state = Nv2aReg(kCache1DmaState), subroutine = Nv2aReg(kCache1DmaSubroutine);
        uint32_t instance = (Nv2aReg(kCache1DmaInstance) & 0xFFFF) << 4;
        LeaveCriticalSection(&g_nv2a_lock);
        if (!(push0 & 1) || !(dma_push & 1) || (dma_push & 0x1000) || get == put) return;
        if (put != g_harvested_put) { g_harvested_put = put; Nv2aGlTextureBarrier(); g_harvest_pending = true; }
        DmaObject dma = LoadDma(instance);
        bool end_batch = false, batch_locked = false;
        for (unsigned batch = 0; batch < 256 && get != put && !end_batch; ++batch) {
        if (get >= dma.limit + 1) Stop(203, "push buffer DMA_GET %#x beyond DMA limit %#x", get, dma.limit);
        {   // Fast path: a non-increasing run of Kelvin index words (ARRAY_ELEMENT16/32)
            // is appended in one call instead of one method dispatch per word.
            uint32_t run = (state >> 18) & 0x7FF;
            uint32_t m = ((state >> 2) & 0x7FF) << 2, sb = (state >> 13) & 7;
            if (run > 1 && (state & 1) && (m == 0x1800 || m == 0x1808 || m == 0x1818) && g_subchannels[sb].object_class == 0x97) {
                uint32_t avail = get < put ? (put - get) / 4 : run;   // PUT behind GET: the words before the wrap jump were all published
                uint32_t limit_words = (dma.limit + 1 - get) / 4;
                uint32_t n = std::min(std::min(run, avail), limit_words);
                if (n > 1) {
                    if (!batch_locked) { Nv2aGlBatchLock(true); batch_locked = true; }
                    Nv2aGlMethodArray(sb, m, At<uint32_t>(0x80000000u + ((dma.address + get) & 0x07FFFFFFu)), n);
                    get += n * 4;
                    state = (state & ~(0x7FFu << 18)) | ((run - n) << 18);
                    batch += n - 1;
                    continue;
                }
            }
            // Fast path: an increasing run of SET_TRANSFORM_PROGRAM (0xb00..0xb7c) or
            // SET_TRANSFORM_CONSTANT (0xb80..0xbfc) words, within its method range.
            if (run > 1 && !(state & 1) && m >= 0xb00 && m < 0xc00 && g_subchannels[sb].object_class == 0x97) {
                uint32_t avail = get < put ? (put - get) / 4 : run;   // PUT behind GET: the words before the wrap jump were all published
                uint32_t limit_words = (dma.limit + 1 - get) / 4;
                uint32_t n = std::min(std::min(std::min(run, avail), limit_words), ((m < 0xb80 ? 0xb80u : 0xc00u) - m) / 4);
                if (n > 1) {
                    if (!batch_locked) { Nv2aGlBatchLock(true); batch_locked = true; }
                    Nv2aGlTransformConstants(sb, m, At<uint32_t>(0x80000000u + ((dma.address + get) & 0x07FFFFFFu)), n);
                    g_gpu_methods += n;
                    get += n * 4;
                    state = (state & ~(0x7FFu << 18)) | ((run - n) << 18);
                    state = (state & ~(0x7FFu << 2)) | (((m + 4 * n) >> 2) << 2);
                    batch += n - 1;
                    continue;
                }
            }
        }
        uint32_t word = *At<uint32_t>(0x80000000u + ((dma.address + get) & 0x07FFFFFFu));
        uint32_t word_get = get, word_state = state;
        get += 4;
        uint32_t count = (state >> 18) & 0x7FF;
        if (count) {
            uint32_t method = ((state >> 2) & 0x7FF) << 2, sub = (state >> 13) & 7;
            // A Kelvin NO_OPERATION with parameter 0 has no guest-visible effect
            // (non-zero raises the software notify, below 0x200 as usual), so it
            // neither ends the batch nor forces a texture re-harvest.
            bool synchronises = PusherMethodSynchronises(sub, method) &&
                                !(method == 0x100 && word == 0 && g_subchannels[sub].object_class == 0x97);
            // The PGRAPH lock is held across non-synchronising methods; it is
            // released before g_nv2a_lock is taken (MMIO emulation nests them
            // the other way) and before any synchronising method.
            if (synchronises && batch_locked) { Nv2aGlBatchLock(false); batch_locked = false; }
            if (!synchronises && !batch_locked) { Nv2aGlBatchLock(true); batch_locked = true; }
            if (synchronises) {
                EnterCriticalSection(&g_nv2a_lock);
                Nv2aSetReg(kCache1DmaGet, word_get); Nv2aSetReg(kCache1DmaState, word_state); Nv2aSetReg(kCache1DmaSubroutine, subroutine);
                LeaveCriticalSection(&g_nv2a_lock);
                end_batch = true;
            }
            GpuMethod(sub, method, word);
            // Texture re-harvest only where the GPU can wait for the CPU (flip stall,
            // wait-for-idle, semaphore acquire, other classes); semaphore releases and
            // notify no-ops (~3,000/s) let the CPU observe progress but cannot make
            // already-queued commands see later CPU writes, and each full harvest
            // scanned 64 MiB (~13% of the GPU thread). New PUTs still harvest.
            // WAIT_FOR_IDLE (~1,100/s) and other classes' methods (blits, ~800/s) are
            // GPU-side waits: no CPU write can become visible to commands queued
            // before the next PUT through them, and each forced a 64 MiB re-harvest
            // at the next texture fetch. Kept for the flip methods (once per frame).
            // GH_STRICT_BARRIERS=1 restores the barrier at every synchronising method.
            static const bool strict = [] { char v[2] = {}; return GetEnvironmentVariableA("GH_STRICT_BARRIERS", v, sizeof v) == 1 && v[0] == '1'; }();
            bool gpu_side_wait = g_subchannels[sub].object_class != 0x97 || method == 0x110;
            if (synchronises && !(g_subchannels[sub].object_class == 0x97 && (method == 0x1d70 || method == 0x100)) && (strict || !gpu_side_wait)) {
                Nv2aGlTextureBarrier();
                ++g_barrier_causes[g_subchannels[sub].object_class != 0x97 ? 129 : method < 0x200 ? method >> 2 : 128];
            }
            if (!(state & 1)) state = (state & ~(0x7FFu << 2)) | (((method + 4) >> 2) << 2);   // increasing
            state = (state & ~(0x7FFu << 18)) | ((count - 1) << 18);
        } else if ((word & 0xE0000003u) == 0x20000000u) { get = word & 0x1FFFFFFFu;                 // old jump
        } else if ((word & 3) == 1) { get = word & 0xFFFFFFFCu;                                      // jump
        } else if ((word & 3) == 2) {                                                               // call
            if (subroutine & 1) Stop(204, "push buffer nested call at %#x", get - 4);
            subroutine = get | 1; get = word & 0xFFFFFFFCu;
        } else if (word == 0x00020000u) {                                                           // return
            if (!(subroutine & 1)) Stop(205, "push buffer return without call at %#x", get - 4);
            get = subroutine & 0xFFFFFFFCu; subroutine = 0;
        } else if ((word & 0xE0030003u) == 0 || (word & 0xE0030003u) == 0x40000000u) {              // (non-)increasing methods
            g_push_history[g_push_history_next++ % 16] = {get - 4, word};
            state = ((word & 0x1FFC)) | (((word >> 13) & 7) << 13) | (((word >> 18) & 0x7FF) << 18) | ((word & 0x40000000u) ? 1u : 0u);
        } else {
            for (uint32_t a = (get >= 0x84 ? get - 0x84 : 0); a < get + 0x20; a += 4)
                Log("  push %#07x: %08x%s", a, *At<uint32_t>(0x80000000u + ((dma.address + a) & 0x07FFFFFFu)), a == get - 4 ? "  <==" : "");
            for (int i = 0; i < 16; ++i) { const auto& h = g_push_history[(g_push_history_next + i) % 16]; if (h.word) Log("  header at %#x: %08x", h.at, h.word); }
            { uint32_t dev = *At<uint32_t>(0x181e08); if (dev) { uint32_t tp = *At<uint32_t>(dev + 0x30);
              Log("  d3d device %#x: put %#x limit %#x cpu time %#x, gpu time pointer %#x", dev, *At<uint32_t>(dev), *At<uint32_t>(dev + 4), *At<uint32_t>(dev + 0x2c), tp);
              if (tp < 0xFD000000u) Log("  gpu time %#x", *At<uint32_t>(tp));
              Log("  d3d push start %#x end %#x, last get %#x (pgraph %#x), user block %#x", *At<uint32_t>(dev + 0x24), *At<uint32_t>(dev + 0x28), *At<uint32_t>(dev + 0x58), *At<uint32_t>(dev + 0x5c), *At<uint32_t>(dev + 0x1c10)); } }
            Log("  push DMA instance %#x base %#x limit %#x state %#x", instance, dma.address, dma.limit, state);
            Stop(206, "push buffer word %08x at %#x not recognised (put %#x)", word, get - 4, put);
        }
        }
        if (batch_locked) Nv2aGlBatchLock(false);
        EnterCriticalSection(&g_nv2a_lock);
        Nv2aSetReg(kCache1DmaGet, get); Nv2aSetReg(kCache1DmaState, state); Nv2aSetReg(kCache1DmaSubroutine, subroutine);
        LeaveCriticalSection(&g_nv2a_lock);
    }
}

HANDLE g_gpu_ready = nullptr;
extern "C" long long g_gpu_idle_ticks = 0;   // QPC ticks the GPU thread waited for work (frame timeline)
DWORD WINAPI GpuThread(void*) {
    Nv2aGlInit(At<uint8_t>(0x80000000u), 0x04000000u, g_pramin, 0x100000);
    SetEvent(g_gpu_ready);
    for (;;) {
        { Nv2aProf idle(9); LARGE_INTEGER a, b; QueryPerformanceCounter(&a); WaitForSingleObject(g_gpu_wake, 50); QueryPerformanceCounter(&b); g_gpu_idle_ticks += b.QuadPart - a.QuadPart; }
        g_gpu_busy = true;
        { Nv2aProf push(8); RunPusher(); }
        g_gpu_busy = false;
    }
}

// ---- PGRAPH: the ported XQEMU/Cxbx-Reloaded OpenGL renderer (native/runtime/nv2a_gl) ----
extern "C" void Nv2aGlInit(uint8_t* vram, uint32_t vram_size, uint8_t* ramin, uint32_t ramin_size);
extern "C" uint32_t Nv2aGlRead(uint32_t addr);
extern "C" void Nv2aGlWrite(uint32_t addr, uint32_t value);
extern "C" uint32_t Nv2aGlPendingInterrupts();
extern "C" void Nv2aGlMethod(uint32_t subchannel, uint32_t method, uint32_t parameter);
extern "C" void Nv2aGlBindObject(uint32_t subchannel, uint32_t instance, uint32_t channel_id);
extern "C" void Nv2aGlFlushSurfaces();

extern "C" __declspec(noreturn) void Nv2aGlFail(const char* what, const char* file, int line) {
    Stop(227, "gpu renderer: check failed: %s (%s:%d) after %llu methods", what, file, line, g_gpu_methods);
}
extern "C" __declspec(noreturn) void Nv2aGlUnknown(const char* fmt, ...) {
    char line[512]; va_list args; va_start(args, fmt); vsnprintf(line, sizeof line, fmt, args); va_end(args);
    Stop(228, "gpu renderer: %s (after %llu methods)", line, g_gpu_methods);
}
extern "C" void Nv2aGlLog(const char* fmt, ...) {
    char line[512]; va_list args; va_start(args, fmt); vsnprintf(line, sizeof line, fmt, args); va_end(args);
    Log("gpu: %s", line);
}
extern HANDLE g_interrupt_wake;
extern "C" void update_irq(struct NV2AState*) { SetEvent(g_interrupt_wake); }

// Frame capture for internal (silent) verification: the scan-out framebuffer
// at each flip is written as a BMP into the run's evidence folder (first 30
// frames, then every eighth).
std::wstring g_frame_dir;
uint64_t g_frames = 0;
std::atomic<uint64_t> g_presented_frames{0};
bool CaptureDisabled() { static const bool off = GetEnvironmentVariableW(L"GH_NO_FRAMES", nullptr, 0) != 0; return off; }   // diagnostic runs skip frame/WAV files
void SaveFrame(uint32_t address, uint32_t width, uint32_t height, uint32_t pitch) {
    if (g_frame_dir.empty() || CaptureDisabled()) return;
    // T008 traced A/B runs keep sparse wall-clock captures instead of writing
    // every eighth frame (which would double capture load in the 60 fps arm).
    if (g_timing_trace) {
        static ULONGLONG next = 0;
        ULONGLONG now = GetTickCount64();
        if (now < next) return;
        next = now + 2000;
    }
    if (!width || !height || width > 4096 || height > 4096 || pitch < width * 4 ||
        static_cast<uint64_t>(address) + static_cast<uint64_t>(height - 1) * pitch + width * 4 > 0x04000000u)
        Stop(229, "frame capture outside 64 MiB RAM: address %#x, %ux%u, pitch %u", address, width, height, pitch);
    static std::vector<uint32_t> scaled;
    unsigned scale = Nv2aGlRenderScale();
    scaled.resize(size_t(width) * height * scale * scale);
    bool high = width == 640 && height == 480 && Nv2aGlReadScanout(address, pitch, scaled.data());
    if (high) { width *= scale; height *= scale; }
    wchar_t name[64]; swprintf(name, 64, L"\\frame-%06llu.bmp", g_frames);
    FILE* f = nullptr; _wfopen_s(&f, (g_frame_dir + name).c_str(), L"wb");
    if (!f) Stop(229, "cannot open frame capture %llu", g_frames);
    uint32_t row = width * 4, image = row * height;
    uint8_t header[54] = {'B', 'M'};
    uint32_t file_size = 54 + image, offset = 54, info = 40, planes_bpp = 1 | (32 << 16);
    int32_t w = static_cast<int32_t>(width), h = -static_cast<int32_t>(height);
    memcpy(header + 2, &file_size, 4); memcpy(header + 10, &offset, 4); memcpy(header + 14, &info, 4);
    memcpy(header + 18, &w, 4); memcpy(header + 22, &h, 4); memcpy(header + 26, &planes_bpp, 4); memcpy(header + 34, &image, 4);
    if (!high) Nv2aOwnedFlush(address, (height - 1) * pitch + row);
    if (fwrite(header, 1, 54, f) != 54) Stop(229, "cannot write frame header");
    for (uint32_t y = 0; y < height; ++y)
        if (fwrite(high ? reinterpret_cast<const uint8_t*>(scaled.data() + size_t(height - 1 - y) * width) : At<uint8_t>(0x80000000u + address + y * pitch), 1, row, f) != row) Stop(229, "cannot write frame pixels");
    if (fclose(f)) Stop(229, "cannot finish frame capture");
}

extern "C" void Nv2aDrawDumpFrame();
extern "C" unsigned long long g_nv2a_present_epoch = 0;
extern "C" void Nv2aGlPresent(struct NV2AState*) {
    Nv2aProf prof(12);
    Nv2aDrawDumpFrame();
    ++g_nv2a_present_epoch;
    ++g_frames;
    g_presented_frames.store(g_frames);
    // Called at flips with the PGRAPH lock held: must not take g_nv2a_lock (see nv2a.inc g_display_lock).
    uint32_t start = g_scanout_start.load(std::memory_order_acquire);
    AcquireSRWLockShared(&g_display_lock); DisplayMode display = g_display_copy; ReleaseSRWLockShared(&g_display_lock);
    // No scan-out is defined before AvSetDisplayMode; physical zero is the
    // title's fence/push-buffer memory, not a valid boot framebuffer.
    if (display.set && (g_present_enabled || g_frames <= 30 || g_frames % 8 == 0 || getenv("GH_FRAME_BURST"))) {
        // Observed retail mode: 640x480, 32-bit format 0x12, pitch 2560.
        // Other modes need their own decoded dimensions/format before capture.
        if (display.mode != 0x180f0d12 || display.format != 0x12 || display.pitch != 2560)
            Stop(229, "frame capture mode %#x format %#x pitch %u not provided", display.mode, display.format, display.pitch);
        PresentScanout(start,display.pitch);
        // GH_FRAME_BURST=first,count additionally captures every frame in that range (diagnostic).
        static uint64_t burst_first = 0, burst_count = 0; static bool burst_read = false;
        if (!burst_read) { burst_read = true; char v[64] = {}; if (GetEnvironmentVariableA("GH_FRAME_BURST", v, sizeof v)) sscanf_s(v, "%llu,%llu", &burst_first, &burst_count); }
        bool burst = burst_count && g_frames >= burst_first && g_frames < burst_first + burst_count;
        if (g_frames <= 30 || g_frames % 8 == 0 || burst) SaveFrame(start, 640, 480, display.pitch);
    }
    if (g_frames == 1 || g_frames % 600 == 0) Log("gpu: frame %llu presented (scan-out %08x)", g_frames, start);
}

void GpuMethod(uint32_t subchannel, uint32_t method, uint32_t parameter) {
    ++g_gpu_methods;
    if (method == 0) {
        uint32_t engine = 0, channel = 0;
        uint32_t instance = RamhtLookup(parameter, &engine, &channel);
        if (engine != 1) Stop(207, "object %#x is not a graphics object (engine %u)", parameter, engine);
        uint32_t flags; memcpy(&flags, g_pramin + instance, 4);
        g_subchannels[subchannel] = {instance, flags & 0xFF};
        Nv2aGlBindObject(subchannel, instance, channel);
        return;
    }
    if (method >= 0x180 && method < 0x200 && parameter) parameter = RamhtLookup(parameter, nullptr);
    Nv2aGlMethod(subchannel, method, parameter);
}

void StartGpu() {
    g_gpu_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_gpu_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_gpu_thread_handle = CreateNativeThread(0x100000, GpuThread, nullptr, false, false);
    if (!g_gpu_thread_handle) Stop(209, "cannot create GPU thread");
    if (WaitForSingleObject(g_gpu_ready, 20000) != WAIT_OBJECT_0) Stop(209, "GPU renderer did not initialise");
}

extern "C" void Nv2aGlDiagnostics(uint32_t* out);
namespace {
void GpuDiagnostics() {
    uint32_t d[8] = {};
    Nv2aGlDiagnostics(d);
    Log("gpu: %llu presents (flip methods), scan-out %08x", g_frames, Nv2aReg(0x600800));
    Log("gpu: %llu methods; pgraph intr pending %#x enabled %#x, fifo %#x, ctx_control %#x ctx_user %#x; pmc enable %#x pending %#x; %llu interrupts delivered",
        g_gpu_methods, d[0], d[1], d[2], d[3], d[4], g_nv2a.pmc_intr_en, Nv2aPmcPending(), g_gpu_interrupts_delivered);
}
}

// ---- Guest memory write tracking for the renderer ----
// Page generations over the 64 MiB contiguous window: each harvest takes the
// pages written since the previous harvest (Windows write watch, reset on
// read) and stamps them with a new generation. A renderer object stores the
// generation current when it last matched guest memory; it is stale when any
// page of its range carries a newer stamp. Harvests run on the GPU thread only.
namespace {
uint64_t g_write_generation = 1;
std::vector<uint64_t> g_page_generation(0x4000, 1);
std::vector<void*> g_watch_addresses(0x4000);
}
// Harvests only the pages of one range (cost proportional to its size);
// pages outside it keep their write-watch bits for a later harvest.
// Harvests hold the deferred-download lock: a completed download's copy is
// removed from write watch under it and must not be seen as a CPU write.
struct OwnedLockScope { OwnedLockScope() { Nv2aOwnedLock(); } ~OwnedLockScope() { Nv2aOwnedUnlock(); } };
extern "C" uint64_t Nv2aGlHarvestRange(uint32_t physical, uint32_t size) {
    Nv2aProf prof(11);
    if (!size) return g_write_generation;
    OwnedLockScope owned_lock;
    if (physical >= 0x04000000u || size > 0x04000000u - physical) Stop(233, "renderer range %#x+%#x outside guest memory", physical, size);
    uint32_t first = physical & ~0xFFFu, last = (physical + size + 0xFFFu) & ~0xFFFu;
    ULONG_PTR count = g_watch_addresses.size(); ULONG granularity = 0;
    if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, At<void>(0x80000000u + first), last - first, g_watch_addresses.data(), &count, &granularity) != 0 || granularity != 0x1000)
        Stop(232, "GetWriteWatch over guest memory failed (error %lu, granularity %lu)", GetLastError(), granularity);
    if (count) {
        ++g_write_generation;
        for (ULONG_PTR i = 0; i < count; ++i)
            g_page_generation[(reinterpret_cast<uintptr_t>(g_watch_addresses[i]) - 0x80000000u) >> 12] = g_write_generation;
    }
    return g_write_generation;
}
extern "C" unsigned long long g_harvest_all_calls = 0;
extern "C" uint64_t Nv2aGlHarvestAll() { ++g_harvest_all_calls; g_harvest_pending = false; return Nv2aGlHarvestRange(0, 0x04000000u); }
// Stamps every page of a range with a new generation (the renderer is about
// to own its contents), after harvesting the range's pending CPU writes.
extern "C" uint64_t Nv2aGlStampRange(uint32_t physical, uint32_t size) {
    OwnedLockScope owned_lock;
    Nv2aGlHarvestRange(physical, size);
    ++g_write_generation;
    for (uint32_t page = physical >> 12; page < ((physical + size + 0xFFFu) >> 12); ++page) g_page_generation[page] = g_write_generation;
    return g_write_generation;
}

extern "C" uint64_t Nv2aGlCurrentGeneration() { if (g_harvest_pending) Nv2aGlHarvestAllForSpan(); return g_write_generation; }
// File reads land in guest memory through Windows (ReadFile/NtReadFile), not
// through guest stores. Mark the destination written explicitly so cached GL
// textures/vertex mirrors over it refresh, whether or not write watch saw it.
extern "C" void Nv2aGlMarkGuestWritten(uint32_t va, uint32_t size) {
    if (!size) return;
    uint64_t lo = std::max<uint64_t>(va, 0x80000000u), hi = std::min<uint64_t>(uint64_t(va) + size, 0x84000000u);
    if (lo < hi) Nv2aGlStampRange(static_cast<uint32_t>(lo - 0x80000000u), static_cast<uint32_t>(hi - lo));
}
extern "C" uint64_t Nv2aGlRangeGeneration(uint32_t physical, uint32_t size) {
    if (!size) return 0;
    if (physical >= 0x04000000u || size > 0x04000000u - physical) Stop(233, "renderer range %#x+%#x outside guest memory", physical, size);
    if (g_harvest_pending) Nv2aGlHarvestAllForSpan();
    uint64_t newest = 0;
    for (uint32_t page = physical >> 12, last = (physical + size - 1) >> 12; page <= last; ++page) newest = std::max(newest, g_page_generation[page]);
    return newest;
}

// Expensive shader/texture/readback diagnostics are opt-in for hidden runs.
extern "C" bool Nv2aGlTraceEnabled() {
    static const bool enabled = [] { char value[2] = {}; return GetEnvironmentVariableA("GH_GPU_TRACE", value, sizeof value) == 1 && value[0] == '1'; }();
    return enabled;
}

// Diagnostic: write a linear 32-bit texture from guest memory to the evidence folder.
extern "C" void Nv2aGlDumpTexture(uint32_t phys, unsigned w, unsigned h, unsigned pitch, unsigned fmt, unsigned count) {
    if (!Nv2aGlTraceEnabled() || g_frame_dir.empty() || phys + static_cast<uint64_t>(pitch) * h > 0x04000000u) return;
    static int dumps = 0; if (++dumps > 60) return;
    wchar_t name[96]; swprintf(name, 96, L"\\tex-%08x-%ux%u-f%02x-%u.bmp", phys, w, h, fmt, count);
    FILE* f = nullptr; _wfopen_s(&f, (g_frame_dir + name).c_str(), L"wb"); if (!f) return;
    uint32_t row = w * 4, image = row * h; uint8_t header[54] = {'B', 'M'};
    uint32_t file_size = 54 + image, offset = 54, info = 40, planes_bpp = 1 | (32 << 16); int32_t ww = w, hh = -static_cast<int32_t>(h);
    memcpy(header + 2, &file_size, 4); memcpy(header + 10, &offset, 4); memcpy(header + 14, &info, 4);
    memcpy(header + 18, &ww, 4); memcpy(header + 22, &hh, 4); memcpy(header + 26, &planes_bpp, 4); memcpy(header + 34, &image, 4);
    fwrite(header, 1, 54, f);
    for (unsigned y = 0; y < h; ++y) fwrite(At<uint8_t>(0x80000000u + phys + y * pitch), 1, row, f);
    fclose(f);
}

// Diagnostic: append generated shader source to the evidence folder.
// Generated vertex shader source by GL program (for the F9 draw dump).
static std::unordered_map<unsigned, std::string> g_vertex_sources;
extern "C" void Nv2aGlDumpProgram(unsigned program, const char* why) {
    if (g_frame_dir.empty()) return;
    static std::set<unsigned> written; if (!written.insert(program).second) return;
    auto it = g_vertex_sources.find(program);
    FILE* f = nullptr; _wfopen_s(&f, (g_frame_dir + L"\\dump-shaders.txt").c_str(), L"ab"); if (!f) return;
    fprintf(f, "===== vertex program %u (%s) =====\n%s\n", program, why, it != g_vertex_sources.end() ? it->second.c_str() : "<source not recorded>"); fclose(f);
}
extern "C" void Nv2aGlDumpShader(const char* kind, const char* code) {
    { unsigned prog = 0; if (sscanf(kind, "vert program %u", &prog) == 1) g_vertex_sources[prog] = code; }
    if (!Nv2aGlTraceEnabled() || g_frame_dir.empty()) return;
    static int n = 0; if (++n > 200) return;
    FILE* f = nullptr; _wfopen_s(&f, (g_frame_dir + L"\\shaders.txt").c_str(), L"ab"); if (!f) return;
    fprintf(f, "===== %s %d =====\n%s\n", kind, n, code); fclose(f);
}
