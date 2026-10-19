// ---- Deferred render-surface downloads (native port) ----
// The reference reads a dirty render surface back into guest RAM at every
// CPU-visibility point (surface switches, semaphore releases): measured at
// ~44 MB per frame, most of the renderer's time. Here a visibility point
// instead hands the surface's guest pages to its GL texture: the pages are
// made inaccessible and the texture owns their contents. The first access
// completes the download into guest RAM before the access proceeds:
//  - guest/host code faulting on the pages (vectored handler, any thread);
//  - the renderer before it reads guest memory itself (texture and vertex
//    uploads, scan-out) through Nv2aOwnedFlush;
//  - host I/O that hands guest buffers to the OS (Nv2aOwnedFlushGuest);
//  - retiring the texture.
// Off the GPU thread the read is made on a shared GL context by a download
// thread, after the fence placed at the visibility point, so guest code sees
// at least what the reference would have downloaded there.
// The pages are stamped with a new write generation at the visibility point
// (as the reference download did) and the completed copy is removed from
// write watch under the same lock as harvests, so it is not taken for a CPU
// write. GH_SYNC_DOWNLOADS=1 restores immediate downloads for comparison.
#include <atomic>
#include <vector>
extern "C" unsigned long long g_nv2a_render_serial;
namespace {
struct OwnedSurface {
    uint32_t addr, len, width, height, pitch, bpp; GLenum format, type; bool swizzle; GLuint tex; GLsync fence; uint64_t gen;
    std::vector<uint8_t> staging;
    bool superseded = false;   // a bound surface was initialised from this texture on the GPU (Nv2aOwnedMarkSuperseded)
    unsigned long long render_serial = 0;   // g_nv2a_render_serial when the fence was inserted
};
// Counts GPU writes into surface textures (draws, clears, blits, GPU surface
// initialisation; EmuNV2A_PGRAPH.cpp). A refreshed visibility point with no
// such write since the record's fence keeps that fence: it still covers every
// command that wrote the texture.
static unsigned long long g_owned_refresh_kept = 0;
CRITICAL_SECTION g_owned_lock;
std::vector<OwnedSurface*> g_owned;
std::atomic<uint32_t> g_owned_count{0};
DWORD g_owned_gpu_tid = 0;
bool g_owned_sync = false;
HANDLE g_owned_request = nullptr, g_owned_done = nullptr;
OwnedSurface* g_owned_job = nullptr;
HDC g_owned_dc = nullptr; HGLRC g_owned_rc = nullptr;
uintptr_t g_runtime_lo = 0, g_runtime_hi = 0;
unsigned long long g_owned_stats[5] = {};   // registrations, refreshed, completed on GPU thread, completed by download thread, faults
const char* g_owned_reason = "?";
std::unordered_map<std::string, unsigned long long> g_owned_reasons;
// Fences replaced at refreshed visibility points (~50 per frame) are deleted
// together once per frame: each glDeleteSync is a synchronisation point with
// the driver's command thread (~5% of the GPU thread when done one by one).
static std::vector<GLsync> g_owned_dead_fences;
static void Nv2aOwnedReapFences() {
    EnterCriticalSection(&g_owned_lock);
    for (GLsync f : g_owned_dead_fences) glDeleteSync(f);
    g_owned_dead_fences.clear();
    LeaveCriticalSection(&g_owned_lock);
}
// Fences are shared between records: visibility points with no GPU write into
// a surface texture in between (colour and zeta registered together, ~1.75
// per point) take one fence (each glFenceSync is also a driver sync point).
// Reference counts under g_owned_lock; released fences go to the dead list.
static std::unordered_map<GLsync, unsigned> g_owned_fence_refs;
static GLsync g_owned_last_fence = nullptr;
static unsigned long long g_owned_last_fence_serial = 0, g_owned_fences_created = 0, g_owned_fences_shared = 0;
static GLsync OwnedAcquireFence() {
    if (g_owned_last_fence && g_owned_last_fence_serial == g_nv2a_render_serial) {
        ++g_owned_fence_refs[g_owned_last_fence]; ++g_owned_fences_shared;
        return g_owned_last_fence;
    }
    GLsync f; { Nv2aProf fence_prof(23); f = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0); }
    { Nv2aProf flush_prof(20); glFlush(); }
    g_owned_fence_refs[f] = 1; ++g_owned_fences_created;
    g_owned_last_fence = f; g_owned_last_fence_serial = g_nv2a_render_serial;
    return f;
}
static void OwnedReleaseFence(GLsync f) {
    if (!f) return;
    auto it = g_owned_fence_refs.find(f);
    if (it == g_owned_fence_refs.end()) Nv2aGlUnknown("deferred download fence released twice");
    if (--it->second) return;
    g_owned_fence_refs.erase(it);
    if (f == g_owned_last_fence) g_owned_last_fence = nullptr;
    g_owned_dead_fences.push_back(f);
}
inline uint32_t PageLo(uint32_t a) { return a & ~0xFFFu; }
inline uint32_t PageHi(uint32_t a, uint32_t n) { return (a + n + 0xFFFu) & ~0xFFFu; }
}
extern "C" DWORD Nv2aGpuThreadId() { return g_owned_gpu_tid; }
extern "C" void Nv2aOwnedLock() { EnterCriticalSection(&g_owned_lock); }
extern "C" void Nv2aOwnedUnlock() { LeaveCriticalSection(&g_owned_lock); }
extern "C" uint64_t Nv2aGlStampRange(uint32_t physical, uint32_t size);

static void OwnedReadTexture(OwnedSurface& o) {
    size_t row = static_cast<size_t>(o.width) * o.bpp;
    o.staging.resize(row * o.height);
    GLint pa = 4, rl = 0, pbo = 0;
    glGetIntegerv(GL_PACK_ALIGNMENT, &pa); glGetIntegerv(GL_PACK_ROW_LENGTH, &rl); glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pbo);
    if (pbo) glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1); glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    { GLenum pending = glGetError(); if (pending != GL_NO_ERROR) Nv2aGlUnknown("GL error %#x pending before a deferred surface download", pending); }
    { Nv2aProf download(13);
      Nv2aReadSurface(o.tex, o.width, o.height, o.format, o.type, o.bpp, o.staging.data()); }
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) Nv2aGlUnknown("deferred surface download failed: GL %#x texture %u format %#x type %#x %ux%u at %#x",
                                            error, o.tex, o.format, o.type, o.width, o.height, o.addr);
    glPixelStorei(GL_PACK_ALIGNMENT, pa); glPixelStorei(GL_PACK_ROW_LENGTH, rl);
    if (pbo) glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
}
static void OwnedStore(const OwnedSurface& o) {
    Nv2aProf copy(14);
    uint8_t* dst = g_nv2a_gl_state.vram_ptr + o.addr;
    size_t row = static_cast<size_t>(o.width) * o.bpp;
    if (!o.swizzle) {
        for (uint32_t y = 0; y < o.height; ++y) memcpy(dst + static_cast<size_t>(y) * o.pitch, o.staging.data() + (o.height - 1 - y) * row, row);
    } else {
        std::vector<uint8_t> linear(static_cast<size_t>(o.pitch) * o.height);
        for (uint32_t y = 0; y < o.height; ++y) memcpy(linear.data() + static_cast<size_t>(y) * o.pitch, o.staging.data() + (o.height - 1 - y) * row, row);
        swizzle_rect(linear.data(), o.width, o.height, dst, o.pitch, o.bpp);
    }
}
static DWORD WINAPI OwnedDownloadThread(void*) {
    if (!wglMakeCurrent(g_owned_dc, g_owned_rc)) Nv2aGlUnknown("download thread cannot make its shared GL context current (%lu)", GetLastError());
    for (;;) {
        WaitForSingleObject(g_owned_request, INFINITE);
        OwnedSurface& o = *g_owned_job;
        for (;;) {
            GLenum r = glClientWaitSync(o.fence, 0, 1000000000ull);
            if (r == GL_ALREADY_SIGNALED || r == GL_CONDITION_SATISFIED) break;
            if (r != GL_TIMEOUT_EXPIRED) Nv2aGlUnknown("download thread fence wait failed (%#x)", r);
        }
        OwnedReadTexture(o);
        SetEvent(g_owned_done);   // the requester (holding g_owned_lock) releases the fence
    }
}
// Completes every pending surface whose pages overlap [lo, hi) (closed over
// shared pages, so no unprotected page still belongs to a pending surface).
// Caller holds g_owned_lock.
static void OwnedCompleteLocked(uint32_t lo, uint32_t hi) {
    std::vector<OwnedSurface*> todo;
    for (bool grew = true; grew;) {
        grew = false;
        for (size_t i = 0; i < g_owned.size(); ++i) {
            OwnedSurface* o = g_owned[i];
            uint32_t a = PageLo(o->addr), b = PageHi(o->addr, o->len);
            if (a < hi && b > lo) { lo = std::min(lo, a); hi = std::max(hi, b); todo.push_back(o); g_owned.erase(g_owned.begin() + i); grew = true; break; }
        }
    }
    if (todo.empty()) return;
    for (OwnedSurface* o : todo) { char k[160]; snprintf(k, sizeof k, "%s %#x %ux%u", g_owned_reason, o->addr, o->width, o->height); g_owned_reasons[k]++; }
    bool gpu = GetCurrentThreadId() == g_owned_gpu_tid;
    for (OwnedSurface* o : todo) {
        if (gpu) { OwnedReadTexture(*o); ++g_owned_stats[2]; }
        else { g_owned_job = o; SetEvent(g_owned_request); WaitForSingleObject(g_owned_done, INFINITE); ++g_owned_stats[3]; }
    }
    // The requested flush range can include unowned pages between/beside
    // the pending surfaces. Preserve their CPU-write generations BEFORE
    // ResetWriteWatch removes our own copies over this same broad range.
    // Otherwise a cutscene/room reload can change texture bytes while their
    // cached GL image continues to appear current. The lock is recursive.
    Nv2aGlHarvestRange(lo, hi - lo);
    void* base = g_nv2a_gl_state.vram_ptr + lo;
    DWORD old = 0;
    if (!VirtualProtect(base, hi - lo, PAGE_READWRITE, &old)) Nv2aGlUnknown("cannot reopen deferred surface pages %#x-%#x (%lu)", lo, hi, GetLastError());
    for (OwnedSurface* o : todo) OwnedStore(*o);
    if (ResetWriteWatch(base, hi - lo) != 0) Nv2aGlUnknown("ResetWriteWatch failed over %#x-%#x", lo, hi);
    // Fences: deleted here on the GPU thread, or by the download thread after its wait.
    for (OwnedSurface* o : todo) OwnedReleaseFence(o->fence);
    for (OwnedSurface* o : todo) delete o;
    g_owned_count.store(static_cast<uint32_t>(g_owned.size()), std::memory_order_release);
}

// Drops pending records of a texture without downloading them, because the
// caller is about to overwrite every byte they cover with a new pending
// record (GPU 2D blit into the same destination). Pages stay inaccessible;
// the caller holds g_owned_lock across this and the new registration, so no
// fault can observe the gap. Records in a download job were already removed.
static void Nv2aOwnedDiscardTextureLocked(GLuint tex, uint32_t addr, uint32_t len) {
    for (size_t i = 0; i < g_owned.size();) {
        OwnedSurface* o = g_owned[i];
        if (o->tex == tex && o->addr == addr && o->len == len) { OwnedReleaseFence(o->fence); delete o; g_owned.erase(g_owned.begin() + i); }
        else ++i;
    }
    g_owned_count.store(static_cast<uint32_t>(g_owned.size()), std::memory_order_release);
}

static void Nv2aOwnedMarkSuperseded(uint32_t addr, uint32_t len, GLuint tex) {
    EnterCriticalSection(&g_owned_lock);
    for (OwnedSurface* o : g_owned) if (o->addr == addr && o->len == len && o->tex == tex) o->superseded = true;
    LeaveCriticalSection(&g_owned_lock);
}

// Called by the renderer at a visibility point instead of downloading.
// Returns the write generation the surface now matches.
static uint64_t Nv2aOwnedRegister(uint32_t addr, uint32_t len, uint32_t width, uint32_t height, uint32_t pitch, uint32_t bpp,
                                  GLenum format, GLenum type, bool swizzle, GLuint tex) {
    if (!tex) Nv2aGlUnknown("visibility point for surface %#x without a texture", addr);
    if (addr >= g_nv2a_gl_state.vram_size || len > g_nv2a_gl_state.vram_size - addr) Nv2aGlUnknown("surface %#x+%#x outside RAM", addr, len);
    EnterCriticalSection(&g_owned_lock);
    ++g_owned_stats[0];
    for (OwnedSurface* o : g_owned) {
        // The same texture over the same pages, still unread since the last
        // visibility point. A narrower clip (linear) leaves the texture's other
        // columns as they were downloaded-equivalent then, so the record keeps
        // the wider extent; a changed linear/swizzle type over an identical
        // extent rewrites every byte, so the latest type wins. Either way the
        // memory reached is what the two downloads in sequence would leave.
        bool same_storage = o->addr == addr && o->len == len && o->pitch == pitch && o->bpp == bpp && o->format == format && o->type == type && o->tex == tex;
        if (same_storage && !o->swizzle && !swizzle && (o->width != width || o->height != height)) {
            o->width = std::max(o->width, width); o->height = std::max(o->height, height);
        } else if (same_storage && o->swizzle != swizzle && o->width == width && o->height == height) {
            o->swizzle = swizzle;
        }
        if (o->addr == addr && o->len == len && o->width >= width && o->height >= height && o->pitch == pitch && o->bpp == bpp &&
            o->format == format && o->type == type && o->swizzle == swizzle && o->tex == tex) {
            ++g_owned_stats[1];
            if (o->render_serial == g_nv2a_render_serial) {   // nothing drawn since: the fence stands
                ++g_owned_refresh_kept;
                uint64_t gen = o->gen;
                LeaveCriticalSection(&g_owned_lock);
                return gen;
            }
            o->render_serial = g_nv2a_render_serial;
            GLsync f = OwnedAcquireFence();
            OwnedReleaseFence(o->fence);
            o->fence = f;
            uint64_t gen = o->gen;
            LeaveCriticalSection(&g_owned_lock);
            return gen;
        }
    }
    // A pending record that a bound surface was initialised from on the GPU is
    // superseded by that surface's own registration over the same bytes: its
    // content is already inside the surface, so drop it instead of downloading.
    for (size_t i = 0; i < g_owned.size();) {
        OwnedSurface* o = g_owned[i];
        if (o->superseded && o->addr == addr && o->len == len && o->tex != tex) { OwnedReleaseFence(o->fence); delete o; g_owned.erase(g_owned.begin() + i); }
        else ++i;
    }
    g_owned_count.store(static_cast<uint32_t>(g_owned.size()), std::memory_order_release);
    uint32_t lo = PageLo(addr), hi = PageHi(addr, len);
    { static int logged = 0;
      for (OwnedSurface* o : g_owned) if (logged < 24 && PageLo(o->addr) < hi && PageHi(o->addr, o->len) > lo) { ++logged;
        Nv2aGlLog("overlap: new %#x+%#x %ux%u pitch %u bpp %u fmt %#x type %#x swz %d tex %u; old %#x+%#x %ux%u pitch %u bpp %u fmt %#x type %#x swz %d tex %u",
                  addr, len, width, height, pitch, bpp, format, type, swizzle, tex, o->addr, o->len, o->width, o->height, o->pitch, o->bpp, o->format, o->type, o->swizzle, o->tex); } }
    g_owned_reason = "register-overlap";
    OwnedCompleteLocked(lo, hi);
    {   // Diagnostic: CPU writes to the surface's memory since it was last matched
        // (bound or downloaded) while the GPU held undownloaded drawing for it.
        Nv2aGlHarvestRange(addr, len);
        if (Nv2aGlRangeGeneration(addr, len) > g_owned_register_surface_gen) {
            static unsigned long long conflicts = 0;
            if (++conflicts <= 40 || conflicts % 500 == 0)
                Nv2aGlLog("surface conflict %llu: CPU wrote %#x+%#x (%ux%u fmt %#x) since the GPU last matched it; frame %llu",
                          conflicts, addr, len, width, height, format, g_nv2a_present_epoch);
        }
    }
    auto* o = new OwnedSurface{addr, len, width, height, pitch, bpp, format, type, swizzle, tex, nullptr, 0, {}, false, g_nv2a_render_serial};
    { Nv2aProf stamp_prof(21); o->gen = Nv2aGlStampRange(addr, len); }
    DWORD old = 0;
    Nv2aProf protect_prof(22);
    if (!VirtualProtect(g_nv2a_gl_state.vram_ptr + lo, hi - lo, PAGE_NOACCESS, &old))
        Nv2aGlUnknown("cannot close deferred surface pages %#x-%#x (%lu)", lo, hi, GetLastError());
    o->fence = OwnedAcquireFence();
    g_owned.push_back(o);
    g_owned_count.store(static_cast<uint32_t>(g_owned.size()), std::memory_order_release);
    if (g_owned_stats[0] % 4000 == 0)
        Nv2aGlLog("deferred downloads: %llu visibility points (%llu refreshed, %llu kept their fence), completed %llu on GPU thread, %llu by download thread, %llu faults, %u pending; %llu surface-to-texture copies, %llu surface-texture evictions",
                  g_owned_stats[0], g_owned_stats[1], g_owned_refresh_kept, g_owned_stats[2], g_owned_stats[3], g_owned_stats[4], static_cast<unsigned>(g_owned.size()), g_direct_texture_copies, g_direct_texture_evictions);
    if (g_owned_stats[0] % 20000 == 0) for (auto& kv : g_owned_reasons) if (kv.second > g_owned_stats[0] / 200) Nv2aGlLog("  completed %llu x %s", kv.second, kv.first.c_str());
    uint64_t gen = o->gen;
    LeaveCriticalSection(&g_owned_lock);
    return gen;
}
// Completes pending surfaces overlapping a physical range before it is read.
static void Nv2aOwnedFlushFor(const char* reason, uint32_t physical, uint32_t size);
extern "C" void Nv2aOwnedFlush(uint32_t physical, uint32_t size) { Nv2aOwnedFlushFor("flush", physical, size); }
static void Nv2aOwnedFlushFor(const char* reason, uint32_t physical, uint32_t size) {
    if (!g_owned_count.load(std::memory_order_acquire) || !size) return;
    EnterCriticalSection(&g_owned_lock);
    g_owned_reason = reason;
    OwnedCompleteLocked(PageLo(physical), PageHi(physical, size));
    LeaveCriticalSection(&g_owned_lock);
}
// Same, for a guest virtual range handed to host APIs (only the contiguous window is owned).
extern "C" void Nv2aOwnedFlushGuest(uint32_t va, uint32_t size) {
    if (!g_owned_count.load(std::memory_order_acquire) || !size) return;
    uint64_t lo = std::max<uint64_t>(va, 0x80000000u), hi = std::min<uint64_t>(uint64_t(va) + size, 0x84000000u);
    if (lo < hi) Nv2aOwnedFlush(static_cast<uint32_t>(lo - 0x80000000u), static_cast<uint32_t>(hi - lo));
}
static void Nv2aOwnedRetireTexture(GLuint tex) {
    Nv2aForgetScanout(tex);
    if (!tex || !g_owned_count.load(std::memory_order_acquire)) return;
    EnterCriticalSection(&g_owned_lock);
    for (bool again = true; again;) {
        again = false;
        for (OwnedSurface* o : g_owned) if (o->tex == tex) { g_owned_reason = "retire"; OwnedCompleteLocked(PageLo(o->addr), PageHi(o->addr, o->len)); again = true; break; }
    }
    LeaveCriticalSection(&g_owned_lock);
}
// Vectored-handler entry for an access violation inside guest RAM.
// Returns true when the access should be retried.
extern "C" bool Nv2aOwnedFault(uintptr_t address, uintptr_t eip) {
    uint32_t physical = static_cast<uint32_t>(address - 0x80000000u);
    EnterCriticalSection(&g_owned_lock);
    bool owned = false;
    for (OwnedSurface* o : g_owned) if (PageLo(o->addr) <= physical && physical < PageHi(o->addr, o->len)) owned = true;
    if (owned) {
        if (GetCurrentThreadId() == g_owned_gpu_tid && !(eip >= g_runtime_lo && eip < g_runtime_hi))
            Nv2aGlUnknown("GPU thread read a deferred surface page %#x from EIP %08x outside the runtime (an explicit flush is missing)", physical, static_cast<unsigned>(eip));
        ++g_owned_stats[4];
        g_owned_reason = "fault";
        OwnedCompleteLocked(PageLo(physical), PageLo(physical) + 0x1000u);
        LeaveCriticalSection(&g_owned_lock);
        return true;
    }
    LeaveCriticalSection(&g_owned_lock);
    MEMORY_BASIC_INFORMATION m{};
    return VirtualQuery(reinterpret_cast<void*>(address), &m, sizeof m) && m.Protect != PAGE_NOACCESS && m.State == MEM_COMMIT;
}
extern "C" IMAGE_DOS_HEADER __ImageBase;
static void Nv2aOwnedInit(GloContext* c) {
    InitializeCriticalSection(&g_owned_lock);
    g_owned_gpu_tid = GetCurrentThreadId();
    char sync[2] = {};
    g_owned_sync = GetEnvironmentVariableA("GH_SYNC_DOWNLOADS", sync, sizeof sync) == 1 && sync[0] == '1';
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<uint8_t*>(&__ImageBase) + __ImageBase.e_lfanew);
    g_runtime_lo = reinterpret_cast<uintptr_t>(&__ImageBase); g_runtime_hi = g_runtime_lo + nt->OptionalHeader.SizeOfImage;
    g_owned_dc = c->dc2; g_owned_rc = c->rc2;
    g_owned_request = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_owned_done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!CreateThread(nullptr, 0x100000, OwnedDownloadThread, nullptr, 0, nullptr)) Nv2aGlUnknown("cannot create the surface download thread");
}
// A texture whose memory is exactly one pending surface with the same texel
// layout can be produced on the GPU from the surface's texture (rows flipped:
// surfaces are stored bottom-up) instead of completing the download and
// uploading it again. Returns the surface texture, or 0.
static GLuint Nv2aOwnedMatch(uint32_t physical, uint32_t length, uint32_t width, uint32_t height, uint32_t bpp,
                             GLenum format, GLenum type, bool swizzled, uint32_t pitch) {
    if (!g_owned_count.load(std::memory_order_acquire) || g_owned_sync) return 0;
    EnterCriticalSection(&g_owned_lock);
    GLuint tex = 0;
    for (OwnedSurface* o : g_owned)
        if (o->addr == physical && o->len == length && o->width == width && o->height == height && o->bpp == bpp &&
            o->format == format && o->type == type && o->swizzle == swizzled && (swizzled ? o->pitch == width * bpp : o->pitch == pitch))
            tex = o->tex;
    LeaveCriticalSection(&g_owned_lock);
    return tex;
}
// A linear 32-bit colour texture over exactly one pending, linear Z24S8
// surface (the game reads its depth buffer as A8R8G8B8, e.g. the reaper's
// colour-drain effect). Returns the depth surface texture, or 0.
static GLuint Nv2aOwnedMatchDepth(uint32_t physical, uint32_t length, uint32_t width, uint32_t height, uint32_t pitch) {
    if (!g_owned_count.load(std::memory_order_acquire) || g_owned_sync) return 0;
    EnterCriticalSection(&g_owned_lock);
    GLuint tex = 0;
    for (OwnedSurface* o : g_owned)
        if (o->addr == physical && o->len == length && o->width == width && o->height == height && o->bpp == 4 &&
            o->format == GL_DEPTH_STENCIL && o->type == GL_UNSIGNED_INT_24_8 && !o->swizzle && o->pitch == pitch)
            tex = o->tex;
    LeaveCriticalSection(&g_owned_lock);
    return tex;
}
// Diagnostics: describe pending records overlapping a range.
static void Nv2aOwnedDescribe(uint32_t addr, uint32_t len, char* out, size_t cap) {
    int n = 0; out[0] = 0;
    EnterCriticalSection(&g_owned_lock);
    for (OwnedSurface* o : g_owned) if (o->addr < addr + len && addr < o->addr + o->len && n < (int)cap - 120)
        n += snprintf(out + n, cap - n, " [%#x+%#x %ux%u pitch %u bpp %u fmt %#x type %#x swz %d tex %u%s]", o->addr, o->len, o->width, o->height, o->pitch, o->bpp, o->format, o->type, o->swizzle, o->tex, o->superseded ? " superseded" : "");
    LeaveCriticalSection(&g_owned_lock);
}
// Read the current scan-out synchronously without publishing it into guest
// RAM. The GPU thread already holds PGRAPH; retain ownership under its lock
// until the host copy is complete. CPU access still completes the record via
// the existing fault/flush path. No display latency or visibility change.
extern "C" bool Nv2aGlReadScanout(uint32_t physical, uint32_t pitch, uint32_t* pixels) {
    if (g_render_scale > 1) return Nv2aGlReadScaledScanout(physical, pitch, pixels);
    if (!g_owned_count.load(std::memory_order_acquire) || g_owned_sync || pitch != 2560) return false;
    EnterCriticalSection(&g_owned_lock);
    OwnedSurface* match = nullptr;
    for (OwnedSurface* o : g_owned)
        if (o->addr == physical && o->len == 640 * 480 * 4 && o->width == 640 && o->height == 480 &&
            o->pitch == pitch && o->bpp == 4 && !o->swizzle && o->format == GL_BGRA &&
            o->type == GL_UNSIGNED_INT_8_8_8_8_REV) match = o;
    if (!match) { LeaveCriticalSection(&g_owned_lock); return false; }
    if (GetCurrentThreadId() != g_owned_gpu_tid) {
        LeaveCriticalSection(&g_owned_lock);
        Nv2aGlUnknown("scan-out texture read requires the GPU thread");
    }
    GLint align, row, pbo;
    glGetIntegerv(GL_PACK_ALIGNMENT, &align); glGetIntegerv(GL_PACK_ROW_LENGTH, &row);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0); glPixelStorei(GL_PACK_ALIGNMENT, 1); glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glGetTextureSubImage(match->tex, 0, 0, 0, 0, 640, 480, 1, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV,
                         640 * 480 * 4, pixels);
    GLenum error = glGetError();
    glPixelStorei(GL_PACK_ALIGNMENT, align); glPixelStorei(GL_PACK_ROW_LENGTH, row); glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
    LeaveCriticalSection(&g_owned_lock);
    if (error != GL_NO_ERROR) Nv2aGlUnknown("scan-out texture read failed (GL %#x)", error);
    return true; // Bottom-up BGRA, as in the existing scaled scan-out reader.
}
// Conservative shortcut guard: page overlap must use the ordinary
// completion/direct-copy path, including the palette visibility range.
static bool Nv2aOwnedPendingRange(uint32_t addr, uint32_t size) {
    if (!size || !g_owned_count.load(std::memory_order_acquire)) return false;
    EnterCriticalSection(&g_owned_lock);
    bool pending=false;
    for (auto* o : g_owned) if (PageLo(o->addr)<PageHi(addr,size) && PageHi(o->addr,o->len)>PageLo(addr)) { pending=true; break; }
    LeaveCriticalSection(&g_owned_lock);
    return pending;
}
// The packed GL_UNSIGNED_INT_24_8 texel (depth << 8 | stencil) is the guest's
// Z24S8 memory word, which an A8R8G8B8 fetch reads as A,R,G = depth bytes and
// B = stencil (the same bytes the download path would publish). Move it on the
// GPU through a pixel buffer into a colour texture, then row-flip it into the
// direct texture like any surface copy. No CPU wait.
static void Nv2aOwnedCopyFlipped(GLuint src, GLuint dst, uint32_t width, uint32_t height);
static unsigned long long g_depth_color_copies = 0;
// The old packed PBO read can serialize depth conversion in the driver.
// Copy the same D24S8 image into immutable helper storage, then read depth and
// stencil through separate views. No guest ownership, fences or source texture
// parameters change. The final image has the same packed bytes and row flip.
static bool Nv2aLegacyDepthCopy() {
    static const bool legacy=[] {
        char v[8]={}; DWORD n=GetEnvironmentVariableA("GH_GPU_LEGACY_DEPTH_COPY",v,sizeof v);
        if(n && (n!=1 || v[0]!='1')) Nv2aGlUnknown("GH_GPU_LEGACY_DEPTH_COPY must be 1 or unset");
        return n!=0;
    }();return legacy;
}
static unsigned long long g_depth_compute_copies=0;
static void Nv2aDepthColorCompute(GLuint src,GLuint dst,uint32_t width,uint32_t height) {
    static GLuint program=0, storage=0, views[2]={}, fb[2]={};static uint32_t cw=0,ch=0;
    const uint32_t w=width*g_render_scale,h=height*g_render_scale;
    if(!program) {
        const char* code=R"GLSL(#version 430 core
layout(local_size_x=16,local_size_y=16) in;
layout(binding=4) uniform sampler2D depthTex;
layout(binding=5) uniform usampler2D stencilTex;
layout(rgba8,binding=0) writeonly uniform image2D colorImage;
void main() {
    ivec2 p=ivec2(gl_GlobalInvocationID.xy), size=imageSize(colorImage);
    if(any(greaterThanEqual(p,size)))return;
    // Invert correctly rounded D24 UNORM -> float32 without a rounded
    // multiply by 0xffffff. Scaling by 2^24 is exact: below 0.5 truncation
    // recovers z; at/above 0.5 the float lattice represents z+1 instead.
    // The GL fixture exhaustively checks all 2^24 inputs, including both ends.
    uint z=uint(texelFetch(depthTex,p,0).r*16777216.0);
    z-=uint(z>=8388608u);
    uint st=texelFetch(stencilTex,p,0).r;
    uvec4 bytes=uvec4((z>>8)&255u,z&255u,st,(z>>16)&255u);
    imageStore(colorImage,ivec2(p.x,size.y-1-p.y),vec4(bytes)/255.0);
})GLSL";
        GLuint shader=glCreateShader(GL_COMPUTE_SHADER);glShaderSource(shader,1,&code,nullptr);glCompileShader(shader);
        GLint ok=0;glGetShaderiv(shader,GL_COMPILE_STATUS,&ok);
        if(!ok){char log[2048]={};glGetShaderInfoLog(shader,sizeof log,nullptr,log);Nv2aGlUnknown("depth conversion shader: %s",log);}
        program=glCreateProgram();glAttachShader(program,shader);glLinkProgram(program);glDeleteShader(shader);
        glGetProgramiv(program,GL_LINK_STATUS,&ok);
        if(!ok){char log[2048]={};glGetProgramInfoLog(program,sizeof log,nullptr,log);Nv2aGlUnknown("depth conversion program: %s",log);}
    }
    GLint active=0,oldProgram=0,tex[2]={},sampler[2]={},image[6]={};
    glGetIntegerv(GL_ACTIVE_TEXTURE,&active);glGetIntegerv(GL_CURRENT_PROGRAM,&oldProgram);
    const GLenum imageNames[]={GL_IMAGE_BINDING_NAME,GL_IMAGE_BINDING_LEVEL,GL_IMAGE_BINDING_LAYERED,
        GL_IMAGE_BINDING_LAYER,GL_IMAGE_BINDING_ACCESS,GL_IMAGE_BINDING_FORMAT};
    for(unsigned i=0;i<6;i++)glGetIntegeri_v(imageNames[i],0,&image[i]);
    for(unsigned i=0;i<2;i++) {
        glActiveTexture(GL_TEXTURE4+i);glGetIntegerv(GL_TEXTURE_BINDING_2D,&tex[i]);
        glGetIntegeri_v(GL_SAMPLER_BINDING,4+i,&sampler[i]);glBindSampler(4+i,0);
    }
    if(cw!=w || ch!=h) {
        glDeleteTextures(2,views);glDeleteTextures(1,&storage);
        glGenTextures(1,&storage);glBindTexture(GL_TEXTURE_2D,storage);
        glTexStorage2D(GL_TEXTURE_2D,1,GL_DEPTH24_STENCIL8,w,h);
        // The immutable one-level helper is complete for both shader views.
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glGenTextures(2,views);
        for(unsigned i=0;i<2;i++) {
            glTextureView(views[i],GL_TEXTURE_2D,storage,GL_DEPTH24_STENCIL8,0,1,0,1);
            glBindTexture(GL_TEXTURE_2D,views[i]);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D,GL_DEPTH_STENCIL_TEXTURE_MODE,i?GL_STENCIL_INDEX:GL_DEPTH_COMPONENT);
        }
        cw=w;ch=h;
    }
    // Original render surfaces can be mutable and sampler-incomplete. A
    // depth/stencil blit needs framebuffer completeness, not sampler state.
    if(!fb[0]) {
        glCreateFramebuffers(2,fb);
        for(auto f:fb){glNamedFramebufferReadBuffer(f,GL_NONE);glNamedFramebufferDrawBuffer(f,GL_NONE);}
    }
    GLboolean scissor=glIsEnabled(GL_SCISSOR_TEST);if(scissor)glDisable(GL_SCISSOR_TEST);
    glNamedFramebufferTexture(fb[0],GL_DEPTH_STENCIL_ATTACHMENT,src,0);
    glNamedFramebufferTexture(fb[1],GL_DEPTH_STENCIL_ATTACHMENT,storage,0);
    glBlitNamedFramebuffer(fb[0],fb[1],0,0,w,h,0,0,w,h,GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT,GL_NEAREST);
    glNamedFramebufferTexture(fb[0],GL_DEPTH_STENCIL_ATTACHMENT,0,0);
    glNamedFramebufferTexture(fb[1],GL_DEPTH_STENCIL_ATTACHMENT,0,0);
    if(scissor)glEnable(GL_SCISSOR_TEST);
    for(unsigned i=0;i<2;i++){glActiveTexture(GL_TEXTURE4+i);glBindTexture(GL_TEXTURE_2D,views[i]);}
    // The caller allocates a complete one-level RGBA8 direct texture. Order
    // writes after preceding draws that may still be sampling this destination.
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    glUseProgram(program);glBindImageTexture(0,dst,0,GL_FALSE,0,GL_WRITE_ONLY,GL_RGBA8);
    glDispatchCompute((w+15)/16,(h+15)/16,1);
    // Consumers include texture fetches, framebuffer blits and captures. A
    // later dispatch/copy may overwrite the helper resources after these reads.
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT|GL_FRAMEBUFFER_BARRIER_BIT|
        GL_TEXTURE_UPDATE_BARRIER_BIT|GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    glBindImageTexture(0,image[0],image[1],image[2],image[3],image[4],image[5]);
    glUseProgram(oldProgram);
    for(unsigned i=0;i<2;i++){glActiveTexture(GL_TEXTURE4+i);glBindTexture(GL_TEXTURE_2D,tex[i]);glBindSampler(4+i,sampler[i]);}
    glActiveTexture(active); // Units 0-3 and the renderer's active-unit cache are unchanged.
    ++g_depth_compute_copies;
    if(Nv2aGlDebugChecks())pgraph_check_gl_errors("in shader depth-to-color conversion");
}
static void Nv2aOwnedCopyDepthAsColor(GLuint src, GLuint dst, uint32_t width, uint32_t height, GLenum dst_format) {
    // RGB8 is not an image-load/store format: preserve its existing conversion.
    if(dst_format==GL_RGBA8 && !Nv2aLegacyDepthCopy()) { Nv2aDepthColorCompute(src,dst,width,height);++g_depth_color_copies;return; }
    static GLuint pbo = 0, tmp = 0; static uint32_t tw = 0, th = 0; static size_t pbo_size = 0;
    const uint32_t w = width * g_render_scale, h = height * g_render_scale; const size_t bytes = size_t(w) * h * 4;
    GLint pack = 0, unpack = 0, pa = 4, pr = 0, ua = 4, ur = 0, us = 0, up = 0, uh = 0;
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack); glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack);
    glGetIntegerv(GL_PACK_ALIGNMENT, &pa); glGetIntegerv(GL_PACK_ROW_LENGTH, &pr);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &ua); glGetIntegerv(GL_UNPACK_ROW_LENGTH, &ur);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &us); glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &up); glGetIntegerv(GL_UNPACK_IMAGE_HEIGHT, &uh);
    if (!pbo) glGenBuffers(1, &pbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
    if (pbo_size < bytes) { glBufferData(GL_PIXEL_PACK_BUFFER, bytes, nullptr, GL_STREAM_COPY); pbo_size = bytes; }
    glPixelStorei(GL_PACK_ALIGNMENT, 4); glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glGetTextureSubImage(src, 0, 0, 0, 0, w, h, 1, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, (GLsizei)bytes, nullptr);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pack);
    glPixelStorei(GL_PACK_ALIGNMENT, pa); glPixelStorei(GL_PACK_ROW_LENGTH, pr);
    if (!tmp) glGenTextures(1, &tmp);
    Nv2aForeignBindTexture(GL_TEXTURE_2D, tmp);
    if (tw != w || th != h) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, nullptr);
        tw = w; th = h;
    }
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4); glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0); glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0); glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, nullptr);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpack);
    glPixelStorei(GL_UNPACK_ALIGNMENT, ua); glPixelStorei(GL_UNPACK_ROW_LENGTH, ur);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, us); glPixelStorei(GL_UNPACK_SKIP_PIXELS, up); glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, uh);
    Nv2aOwnedCopyFlipped(tmp, dst, width, height);
    ++g_depth_color_copies;
    if (Nv2aGlDebugChecks()) { GLenum error = glGetError();
        if (error != GL_NO_ERROR) Nv2aGlUnknown("depth-to-colour texture copy failed: GL %#x (%ux%u, texture %u -> %u)", error, width, height, src, dst); }
}
static void Nv2aOwnedCopyFlipped(GLuint src, GLuint dst, uint32_t width, uint32_t height) {
    if (!Nv2aLegacyState()) {
        // Direct-state-access blits leave both caller FBO bindings untouched.
        // Identical attachments, extent, row flip, filter and scissor semantics
        // to the bind/restore path below; ownership/fence publication is unchanged.
        static GLuint named[2] = {};
        if (!named[0]) glCreateFramebuffers(2, named);
        // A valid draw scissor cache implies GL_SCISSOR_TEST is enabled.
        // Clears invalidate it; temporary helper changes restore it and this
        // function is not called from inside those temporary changes.
        GLboolean scissor = g_scissor_viewport_valid ? GL_TRUE : glIsEnabled(GL_SCISSOR_TEST);
        if (scissor) glDisable(GL_SCISSOR_TEST);
        glNamedFramebufferTexture(named[0], GL_COLOR_ATTACHMENT0, src, 0);
        glNamedFramebufferTexture(named[1], GL_COLOR_ATTACHMENT0, dst, 0);
        glBlitNamedFramebuffer(named[0], named[1], 0, 0, width * g_render_scale, height * g_render_scale,
            0, height * g_render_scale, width * g_render_scale, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glNamedFramebufferTexture(named[0], GL_COLOR_ATTACHMENT0, 0, 0);
        glNamedFramebufferTexture(named[1], GL_COLOR_ATTACHMENT0, 0, 0);
        if (scissor) glEnable(GL_SCISSOR_TEST);
        if (Nv2aGlDebugChecks()) pgraph_check_gl_errors("in direct surface-to-texture copy");
        return;
    }
    static GLuint fbo[2] = {};
    if (!fbo[0]) glGenFramebuffers(2, fbo);
    GLint read = 0, draw = 0; glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read); glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    if (scissor) glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo[0]); glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, src, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo[1]); glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst, 0);
    // The destination keeps render scale (direct textures are allocated scaled).
    glBlitFramebuffer(0, 0, width * g_render_scale, height * g_render_scale, 0, height * g_render_scale, width * g_render_scale, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, read); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    // Errors are caught by the per-frame check (or here with GH_GPU_DEBUG=1).
    if (Nv2aGlDebugChecks()) { GLenum error = glGetError();
        if (error != GL_NO_ERROR) Nv2aGlUnknown("surface-to-texture copy failed: GL %#x (%ux%u, texture %u -> %u)", error, width, height, src, dst); }
}
