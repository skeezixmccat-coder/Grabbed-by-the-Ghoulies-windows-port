// DMA-object routines adapted from Cxbx-Reloaded nv2a.cpp.
// Modified for this native port, 2026-10-14 through 2026-10-19; see docs/PORT-PROVENANCE.md.
// This is an open source non-commercial project. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
// ******************************************************************
// *
// *  This file is part of the Cxbx project.
// *
// *  Cxbx and Cxbe are free software; you can redistribute them
// *  and/or modify them under the terms of the GNU General Public
// *  License as published by the Free Software Foundation; either
// *  version 2 of the license, or (at your option) any later version.
// *
// *  This program is distributed in the hope that it will be useful,
// *  but WITHOUT ANY WARRANTY; without even the implied warranty of
// *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// *  GNU General Public License for more details.
// *
// *  You should have recieved a copy of the GNU General Public License
// *  along with this program; see the file COPYING.
// *  If not, write to the Free Software Foundation, Inc.,
// *  59 Temple Place - Suite 330, Bostom, MA 02111-1307, USA.
// *
// *  (c) 2002-2003 Aaron Robinson <caustik@caustik.com>
// * 
// *  This file is heavily based on code from XQEMU
// *  https://github.com/xqemu/xqemu/blob/master/hw/xbox/nv2a/nv2a.c
// *  Copyright (c) 2012 espes
// *  Copyright (c) 2015 Jannik Vogel
// *  Copyright (c) 2018 Matt Borgerson
// *
// *  Contributions for Cxbx-Reloaded
// *  Copyright (c) 2017-2018 Luke Usher <luke.usher@outlook.com>
// *  Copyright (c) 2018 Patrick van Logchem <pvanlogchem@gmail.com>
// *
// *  All rights reserved
// *
// ******************************************************************

// Glue for the ported XQEMU/Cxbx-Reloaded NV2A PGRAPH (OpenGL) renderer.
// Owns the NV2AState, provides DMA-object mapping, pixel converters, the
// WGL offscreen context and the GL loader, and exposes a small C API to the
// runtime (native/runtime/gpu.inc). GPL-2.0 code: see docs/SOURCES.md.
#include "compat.h"
#include <intrin.h>
#include <algorithm>
#include "nv2a_int.h"

NV2AState g_nv2a_gl_state;

// ---- GL loader ----
static HMODULE g_opengl32 = nullptr;
static PROC Load(const char* name) {
    PROC p = wglGetProcAddress(name);
    if (!p || p == reinterpret_cast<PROC>(1) || p == reinterpret_cast<PROC>(2) || p == reinterpret_cast<PROC>(3) || p == reinterpret_cast<PROC>(-1))
        p = GetProcAddress(g_opengl32, name);
    // Debugger-only extension (used only on macOS builds of the reference) may be absent.
    if (!p && strcmp(name, "glFrameTerminatorGREMEDY") != 0) Nv2aGlFail(name, "OpenGL entry point missing", 0);
    return p;
}
#include "gl_loader.inc"

// ---- DMA objects (as in Cxbx nv2a.cpp) ----
DMAObject nv_dma_load(NV2AState *d, xbox::addr_xt dma_obj_address)
{
    assert(dma_obj_address < d->pramin.ramin_size);
    uint32_t *dma_obj = (uint32_t*)(d->pramin.ramin_ptr + dma_obj_address);
    uint32_t flags = ldl_le_p(dma_obj), limit = ldl_le_p(dma_obj + 1), frame = ldl_le_p(dma_obj + 2);
    DMAObject object;
    object.dma_class = GET_MASK(flags, NV_DMA_CLASS);
    object.dma_target = GET_MASK(flags, NV_DMA_TARGET);
    object.address = (frame & NV_DMA_ADDRESS) | GET_MASK(flags, NV_DMA_ADJUST);
    object.limit = limit;
    return object;
}

void *nv_dma_map(NV2AState *d, xbox::addr_xt dma_obj_address, xbox::addr_xt *len)
{
    DMAObject dma = nv_dma_load(d, dma_obj_address);
    dma.address &= 0x07FFFFFF;
    *len = dma.limit;
    return d->vram_ptr + dma.address;
}

// ---- Pixel converters used by convert_texture_data ----
static inline uint8_t Clamp8(int v) { return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); }
static inline void YuvToBgra(int y, int u, int v, uint8_t* out) {
    int c = y - 16, d = u - 128, e = v - 128;
    out[2] = Clamp8((298 * c + 409 * e + 128) >> 8);
    out[1] = Clamp8((298 * c - 100 * d - 208 * e + 128) >> 8);
    out[0] = Clamp8((298 * c + 516 * d + 128) >> 8);
    out[3] = 255;
}
void __R6G5B5ToARGBRow_C(const uint8_t* src, uint8_t* dst, int width) {
    for (int x = 0; x < width; ++x) {
        uint16_t p = static_cast<uint16_t>(src[2 * x] | (src[2 * x + 1] << 8));
        uint32_t r = (p >> 10) & 0x3F, g = (p >> 5) & 0x1F, b = p & 0x1F;
        dst[4 * x + 0] = static_cast<uint8_t>((b << 3) | (b >> 2));
        dst[4 * x + 1] = static_cast<uint8_t>((g << 3) | (g >> 2));
        dst[4 * x + 2] = static_cast<uint8_t>((r << 2) | (r >> 4));
        dst[4 * x + 3] = 255;
    }
}
void ____YUY2ToARGBRow_C(const uint8_t* src, uint8_t* dst, int width) {
    for (int x = 0; x + 1 < width; x += 2) {
        int y0 = src[2 * x], u = src[2 * x + 1], y1 = src[2 * x + 2], v = src[2 * x + 3];
        YuvToBgra(y0, u, v, dst + 4 * x); YuvToBgra(y1, u, v, dst + 4 * x + 4);
    }
}
void ____UYVYToARGBRow_C(const uint8_t* src, uint8_t* dst, int width) {
    for (int x = 0; x + 1 < width; x += 2) {
        int u = src[2 * x], y0 = src[2 * x + 1], v = src[2 * x + 2], y1 = src[2 * x + 3];
        YuvToBgra(y0, u, v, dst + 4 * x); YuvToBgra(y1, u, v, dst + 4 * x + 4);
    }
}

static unsigned Nv2aTextureAnisotropy(GLenum,bool,GLenum,GLenum);
static void Nv2aApplyAnisotropy(GLenum,unsigned);

// ---- The ported PGRAPH ----
#include "EmuNV2A_PGRAPH.cpp"

// ---- WGL offscreen context ----
struct GloContext { HWND window; HDC dc; HGLRC rc; HWND window2; HDC dc2; HGLRC rc2; HWND window3; HDC dc3; HGLRC rc3; };
// The asynchronous presenter's shared context (resolution.inc).
HDC g_gl_present_dc = nullptr; HGLRC g_gl_present_rc = nullptr;

GloContext* glo_context_create() {
    WNDCLASSW wc{}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"GhouliesGpu";
    RegisterClassW(&wc);
    auto* c = new GloContext{};
    c->window = CreateWindowExW(0, wc.lpszClassName, L"Ghoulies GPU (offscreen)", WS_POPUP, 0, 0, 16, 16, nullptr, nullptr, wc.hInstance, nullptr);
    if (!c->window) Nv2aGlFail("CreateWindow", "offscreen GL window", 0);
    c->dc = GetDC(c->window);
    PIXELFORMATDESCRIPTOR pfd{}; pfd.nSize = sizeof pfd; pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER; pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32; pfd.cDepthBits = 24; pfd.cStencilBits = 8;
    int format = ChoosePixelFormat(c->dc, &pfd);
    if (!format || !SetPixelFormat(c->dc, format, &pfd)) Nv2aGlFail("SetPixelFormat", "offscreen GL window", 0);
    c->rc = wglCreateContext(c->dc);
    if (!c->rc) Nv2aGlFail("wglCreateContext", "offscreen GL context", 0);
    // Native port: a second context sharing the renderer's objects, made current
    // on the surface download thread (surface_owner.inc).
    c->window2 = CreateWindowExW(0, wc.lpszClassName, L"Ghoulies GPU downloads (offscreen)", WS_POPUP, 0, 0, 16, 16, nullptr, nullptr, wc.hInstance, nullptr);
    if (!c->window2) Nv2aGlFail("CreateWindow", "offscreen GL download window", 0);
    c->dc2 = GetDC(c->window2);
    if (!SetPixelFormat(c->dc2, format, &pfd)) Nv2aGlFail("SetPixelFormat", "offscreen GL download window", 0);
    c->rc2 = wglCreateContext(c->dc2);
    if (!c->rc2 || !wglShareLists(c->rc, c->rc2)) Nv2aGlFail("wglShareLists", "shared GL download context", 0);
    // A third shared context, made current on the presenter thread (resolution.inc).
    c->window3 = CreateWindowExW(0, wc.lpszClassName, L"Ghoulies GPU presenter (offscreen)", WS_POPUP, 0, 0, 16, 16, nullptr, nullptr, wc.hInstance, nullptr);
    if (!c->window3) Nv2aGlFail("CreateWindow", "offscreen GL presenter window", 0);
    c->dc3 = GetDC(c->window3);
    if (!SetPixelFormat(c->dc3, format, &pfd)) Nv2aGlFail("SetPixelFormat", "offscreen GL presenter window", 0);
    c->rc3 = wglCreateContext(c->dc3);
    if (!c->rc3 || !wglShareLists(c->rc, c->rc3)) Nv2aGlFail("wglShareLists", "shared GL presenter context", 0);
    g_gl_present_dc = c->dc3; g_gl_present_rc = c->rc3;
    if (!wglMakeCurrent(c->dc, c->rc)) Nv2aGlFail("wglMakeCurrent", "offscreen GL context", 0);
    g_opengl32 = GetModuleHandleW(L"opengl32.dll");
    LoadGlFunctions();
    Nv2aInvalidateTextureUnits();
    return c;
}
void glo_set_current(GloContext* c) { if (c) wglMakeCurrent(c->dc, c->rc); else wglMakeCurrent(nullptr, nullptr); }
void glo_context_destroy(GloContext* c) { if (!c) return; wglMakeCurrent(nullptr, nullptr); wglDeleteContext(c->rc); ReleaseDC(c->window, c->dc); DestroyWindow(c->window); delete c; }
bool glo_check_extension(const char* name) {
    GLint n = 0; glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (GLint i = 0; i < n; ++i) { const char* e = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, i)); if (e && !strcmp(e, name)) return true; }
    return false;
}
#include "anisotropy.inc"
#include "surface_owner.inc"
extern "C" void Nv2aGlReapFences() { Nv2aOwnedReapFences(); }
extern "C" unsigned long long g_nv2a_prof[32] = {};
extern "C" bool g_nv2a_profile_enabled = false;
extern "C" unsigned long long g_nv2a_readbacks[4] = {};
void glo_readpixels(GLenum gl_format, GLenum gl_type, unsigned int bpp, unsigned int stride, unsigned int width, unsigned int height, void* data) {
    Nv2aProf prof(0);
    assert(stride % bpp == 0);
    GLint rl, pa; glGetIntegerv(GL_PACK_ROW_LENGTH, &rl); glGetIntegerv(GL_PACK_ALIGNMENT, &pa);
    // Keep completion synchronous at the existing visibility points. Download
    // into ordinary host memory, then flip/copy each row into guest RAM once.
    // Passing MEM_WRITE_WATCH guest pages to the driver forces it through the
    // guest write tracking and the old in-place flip writes those pages again.
    static thread_local std::vector<uint8_t> staging;
    const size_t row_bytes = static_cast<size_t>(width) * bpp;
    staging.resize(row_bytes * height);
    ++g_nv2a_readbacks[0]; g_nv2a_readbacks[1] += row_bytes * height;
    g_nv2a_readbacks[(gl_format == GL_DEPTH_COMPONENT || gl_format == GL_DEPTH_STENCIL) ? 3 : 2] += row_bytes * height;
    glPixelStorei(GL_PACK_ROW_LENGTH, width); glPixelStorei(GL_PACK_ALIGNMENT, 1);
    { Nv2aProf download(13);
      if (g_render_scale == 1) glReadPixels(0, 0, width, height, gl_format, gl_type, staging.data());
      else {
          GLenum attachment = gl_format == GL_DEPTH_STENCIL ? GL_DEPTH_STENCIL_ATTACHMENT :
                              gl_format == GL_DEPTH_COMPONENT ? GL_DEPTH_ATTACHMENT : GL_COLOR_ATTACHMENT0;
          GLint texture = 0;
          glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, attachment, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &texture);
          if (!texture) Nv2aGlUnknown("scaled readback requires a texture attachment");
          Nv2aReadSurface(texture, width, height, gl_format, gl_type, bpp, staging.data());
      }
    }
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) Nv2aGlUnknown("surface readback failed: GL %#x format %#x type %#x %ux%u", error, gl_format, gl_type, width, height);
    { Nv2aProf copy(14);
      uint8_t* out = static_cast<uint8_t*>(data);
      for (unsigned y = 0; y < height; ++y)
          memcpy(out + static_cast<size_t>(y) * stride, staging.data() + (height - y - 1) * row_bytes, row_bytes);
    }
    glPixelStorei(GL_PACK_ROW_LENGTH, rl); glPixelStorei(GL_PACK_ALIGNMENT, pa);
}

// ---- C API for the runtime ----
static void APIENTRY NativeGlError(GLenum, GLenum type, GLuint id, GLenum, GLsizei, const GLchar* message, const void*) {
    if (type == GL_DEBUG_TYPE_ERROR) Nv2aGlUnknown("GL validation %u: %s", id, message);
}
extern "C" void Nv2aGlInit(uint8_t* vram, uint32_t vram_size, uint8_t* ramin, uint32_t ramin_size) {
    char profile[8]={}; DWORD pn=GetEnvironmentVariableA("GH_GPU_PROFILE",profile,sizeof profile);
    if(pn && (pn!=1 || profile[0]!='1')) Nv2aGlUnknown("GH_GPU_PROFILE must be 1 or unset");
    g_nv2a_profile_enabled=pn!=0;
    Nv2aGlLog("detailed GPU method profiling %s",g_nv2a_profile_enabled?"enabled":"disabled");
    Nv2aResolutionInit();
    NV2AState* d = &g_nv2a_gl_state;
    d->vram_ptr = vram; d->vram_size = vram_size;
    d->pramin.ramin_ptr = ramin; d->pramin.ramin_size = ramin_size;
    d->pgraph.opengl_enabled = true;
    pgraph_init(d);                           // creates the GL context and releases it
    glo_set_current(d->pgraph.gl_context);    // the calling (GPU) thread owns it from here
    Nv2aOwnedInit(d->pgraph.gl_context);
    Nv2aAnisotropyInit();
    char debug[2]={};
    if (GetEnvironmentVariableA("GH_GPU_DEBUG", debug, sizeof debug)==1 && debug[0]=='1') {
        auto callback=reinterpret_cast<PFNGLDEBUGMESSAGECALLBACKPROC>(Load("glDebugMessageCallback"));
        glEnable(GL_DEBUG_OUTPUT); glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS); callback(NativeGlError,nullptr);
    }
    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    extern void Nv2aGlLog(const char*, ...);
    Nv2aGlLog("OpenGL renderer: %s, %s", renderer ? renderer : "?", version ? version : "?");
    Nv2aGlLog("internal rendering: %ux (%ux%u scan-out); guest remains 640x480", g_render_scale, 640 * g_render_scale, 480 * g_render_scale);
}
extern "C" uint32_t Nv2aGlRead(uint32_t addr) { return pgraph_read(&g_nv2a_gl_state, addr); }
extern "C" void Nv2aGlWrite(uint32_t addr, uint32_t value) { pgraph_write(&g_nv2a_gl_state, addr, value); }
static void Nv2aGlLogMethodProfile();
extern "C" void Nv2aGlCounters(unsigned long long* out) { Nv2aGlLogMethodProfile(); out[0] = g_nv2a_shader_compiles; out[1] = g_nv2a_texture_uploads; out[2] = g_nv2a_surface_uploads;
    for(unsigned i=0;Nv2aGlTraceEnabled() && i<2048;++i) if(g_readback_reasons[i]) Nv2aGlLog("readback reason method %#x count %llu", i*4, g_readback_reasons[i]);
}
extern "C" void Nv2aGlDiagnostics(uint32_t* out) {
    PGRAPHState* pg = &g_nv2a_gl_state.pgraph;
    out[0] = pg->pending_interrupts; out[1] = pg->enabled_interrupts;
    out[2] = pg->regs[NV_PGRAPH_FIFO]; out[3] = pg->regs[NV_PGRAPH_CTX_CONTROL]; out[4] = pg->regs[NV_PGRAPH_CTX_USER];
}
extern "C" uint32_t Nv2aGlPendingInterrupts() { return g_nv2a_gl_state.pgraph.pending_interrupts & g_nv2a_gl_state.pgraph.enabled_interrupts; }
// As the reference PFIFO puller: binding an object switches PGRAPH to the
// object's channel (raising the context-switch interrupt the title's ISR
// services) and waits for FIFO access; every method waits for FIFO access.
extern "C" void Nv2aGlBindObject(uint32_t subchannel, uint32_t instance, uint32_t channel_id) {
    PGRAPHState* pg = &g_nv2a_gl_state.pgraph;
    qemu_mutex_lock(&pg->pgraph_lock);
    pgraph_switch_context(&g_nv2a_gl_state, channel_id);
    pgraph_wait_fifo_access(&g_nv2a_gl_state);
    pgraph_handle_method(&g_nv2a_gl_state, subchannel, 0, instance);
    qemu_mutex_unlock(&pg->pgraph_lock);
}
// The pusher may hold the PGRAPH lock across a batch of non-synchronising
// methods (gpu.inc RunPusher); the lock is then entered exactly once, so the
// condition waits inside methods still release it.
extern "C" void Nv2aGlBatchLock(bool lock) {
    PGRAPHState* pg = &g_nv2a_gl_state.pgraph;
    if (lock == g_pgraph_batch_held) Nv2aGlUnknown("PGRAPH batch lock state already %d", (int)lock);
    if (lock) qemu_mutex_lock(&pg->pgraph_lock); else qemu_mutex_unlock(&pg->pgraph_lock);
    g_pgraph_batch_held = lock;
}
// Profiling: TSC cycles and counts per method (summed over subchannels).
static unsigned long long g_method_cycles[0x800], g_method_counts[0x800];
extern "C" void Nv2aGlMethod(uint32_t subchannel, uint32_t method, uint32_t parameter) {
    // Keep exactly the same lock, FIFO-access wait and dispatch semantics.
    // Five RDTSCs plus 64-bit counters per command measurably burden x86 draws.
    if(!g_nv2a_profile_enabled) {
        PGRAPHState* pg=&g_nv2a_gl_state.pgraph;
        bool held=g_pgraph_batch_held;
        if(!held) qemu_mutex_lock(&pg->pgraph_lock);
        pgraph_wait_fifo_access(&g_nv2a_gl_state);
        pgraph_handle_method(&g_nv2a_gl_state,subchannel,method,parameter);
        if(!held) qemu_mutex_unlock(&pg->pgraph_lock);
        return;
    }
    Nv2aProf prof(6);
    unsigned long long t0 = __rdtsc();
    PGRAPHState* pg = &g_nv2a_gl_state.pgraph;
    bool held = g_pgraph_batch_held;
    if (!held) qemu_mutex_lock(&pg->pgraph_lock);
    pgraph_wait_fifo_access(&g_nv2a_gl_state);
    unsigned long long t1 = __rdtsc();
    pgraph_handle_method(&g_nv2a_gl_state, subchannel, method, parameter);
    if (!held) qemu_mutex_unlock(&pg->pgraph_lock);
    unsigned long long t2 = __rdtsc();
    // Pseudo-slots: 0x7fd lock+FIFO-access wait (all methods), 0x7fe BEGIN, 0x7ff END.
    unsigned slot = (method >> 2) & 0x7FF;
    if (method == 0x17fc) slot = parameter ? 0x7FE : 0x7FF;
    g_method_cycles[slot] += t2 - t1; g_method_counts[slot]++;
    g_method_cycles[0x7FD] += t1 - t0; g_method_counts[0x7FD]++;
}
// Bulk index data (pusher fast path): the same effect as count calls of
// Nv2aGlMethod(ARRAY_ELEMENT16/32) under the held batch lock.
extern "C" void Nv2aGlMethodArray(uint32_t subchannel, uint32_t method, const uint32_t* words, uint32_t count) {
    unsigned long long t0 = g_nv2a_profile_enabled ? __rdtsc() : 0;
    PGRAPHState* pg = &g_nv2a_gl_state.pgraph;
    if (!g_pgraph_batch_held) Nv2aGlUnknown("index fast path without the batch lock");
    pgraph_wait_fifo_access(&g_nv2a_gl_state);
    if (method == 0x1818) {   // INLINE_ARRAY: vertex words appended as the per-method path does
        if (pg->inline_array_length + (size_t)count > NV2A_MAX_BATCH_LENGTH)
            Nv2aGlUnknown("inline array batch exceeds %u words", (unsigned)NV2A_MAX_BATCH_LENGTH);
        memcpy(pg->inline_array + pg->inline_array_length, words, (size_t)count * 4);
        pg->inline_array_length += count;
        if(g_nv2a_profile_enabled) { g_method_cycles[method >> 2] += __rdtsc() - t0; g_method_counts[method >> 2] += count; }
        return;
    }
    uint32_t per = method == 0x1800 ? 2u : 1u;
    if (pg->inline_elements_length + (size_t)count * per > NV2A_MAX_BATCH_LENGTH)
        Nv2aGlUnknown("inline element batch exceeds %u indices", (unsigned)NV2A_MAX_BATCH_LENGTH);
    uint16_t* out = pg->inline_elements + pg->inline_elements_length;   // 16-bit storage, as the per-method path
    if (method == 0x1800) for (uint32_t i = 0; i < count; ++i) { uint32_t w = words[i]; out[2*i] = (uint16_t)(w & 0xFFFF); out[2*i+1] = (uint16_t)(w >> 16); }
    else for (uint32_t i = 0; i < count; ++i) out[i] = (uint16_t)words[i];
    pg->inline_elements_length += count * per;
    (void)subchannel;
    unsigned slot = method >> 2;
        if(g_nv2a_profile_enabled) { g_method_cycles[slot] += __rdtsc() - t0; g_method_counts[slot] += count; }
}
void pgraph_load_transform_constants(NV2AState *d, unsigned subchannel, uint32_t method, const uint32_t* words, uint32_t count);
// Bulk transform constants (pusher fast path), under the held batch lock.
extern "C" void Nv2aGlTransformConstants(uint32_t subchannel, uint32_t method, const uint32_t* words, uint32_t count) {
    unsigned long long t0 = g_nv2a_profile_enabled ? __rdtsc() : 0;
    if (!g_pgraph_batch_held) Nv2aGlUnknown("transform constant fast path without the batch lock");
    pgraph_load_transform_constants(&g_nv2a_gl_state, subchannel, method, words, count);
    unsigned slot = (method >> 2) & 0x7FF;
        if(g_nv2a_profile_enabled) { g_method_cycles[slot] += __rdtsc() - t0; g_method_counts[slot] += count; }
}
extern "C" unsigned long long g_harvest_all_calls, g_harvest_span, g_harvest_tex, g_harvest_vtx_invalidate;
extern "C" unsigned long long g_barrier_causes[130];
extern "C" void Nv2aGlFrameTimelineLog();
extern "C" unsigned long long g_harvest_all_calls, g_harvest_span, g_harvest_tex, g_harvest_vtx_invalidate;
static void Nv2aGlLogMethodProfile() {
    static unsigned long long last_c[0x800], last_n[0x800];
    std::vector<std::pair<unsigned long long, unsigned>> v; unsigned long long total = 0;
    for (unsigned i = 0; i < 0x800; ++i) { unsigned long long d = g_method_cycles[i] - last_c[i]; total += d; if (d) v.push_back({d, i}); }
    std::sort(v.rbegin(), v.rend());
    char line[600]; int n = 0;
    for (size_t k = 0; k < v.size() && k < 12; ++k) { unsigned i = v[k].second; unsigned long long cnt = g_method_counts[i] - last_n[i];
        n += snprintf(line + n, sizeof line - n, " %#x:%.0f%%/%lluk/%.0fns", i * 4, 100.0 * v[k].first / (total ? total : 1), cnt / 1000, cnt ? v[k].first / 3.0 / cnt : 0.0); }
    {   // Most frequent methods (fast-path words included).
        std::vector<std::pair<unsigned long long, unsigned>> c;
        for (unsigned i = 0; i < 0x7FD; ++i) { unsigned long long cnt = g_method_counts[i] - last_n[i]; if (cnt) c.push_back({cnt, i}); }
        std::sort(c.rbegin(), c.rend());
        for (size_t part = 0; part < 3; ++part) {
            char cl[480]; int m = 0; cl[0] = 0;
            for (size_t k = part * 24; k < c.size() && k < part * 24 + 24 && m < 440; ++k) m += snprintf(cl + m, sizeof cl - m, " %#x:%lluk", c[k].second * 4, c[k].first / 1000);
            if (m) Nv2aGlLog("method counts %zu:%s", part, cl);
        }
    }
    for (unsigned i = 0; i < 0x800; ++i) { last_c[i] = g_method_cycles[i]; last_n[i] = g_method_counts[i]; }
    if(g_nv2a_profile_enabled) Nv2aGlLog("method profile (%.2f Gcycles):%s", total / 1e9, line);
    Nv2aGlLog("vertex mirror: %llu uploads, %llu KiB total", g_vertex_mirror_stats[0], g_vertex_mirror_stats[1] >> 10);
    { Nv2aGlLog("full harvests: %llu total (PUT spans %llu, texture spans %llu, vertex-cache invalidations %llu)", g_harvest_all_calls, g_harvest_span, g_harvest_tex, g_harvest_vtx_invalidate);
      char b[600]; int n = 0; b[0] = 0;
      for (int i = 0; i < 130 && n < 560; ++i) if (g_barrier_causes[i])
          n += snprintf(b + n, sizeof b - n, " %s%#x:%llu", i == 128 ? "kelvin-other" : i == 129 ? "other-class" : "", i < 128 ? i << 2 : 0, g_barrier_causes[i]);
      Nv2aGlLog("texture barriers by method:%s", b); }
    { static unsigned long long last_ns = 0, last_n = 0; unsigned long long dn = g_gpu_frames - last_n;
      Nv2aGlLog("gpu timestamps: %llu frames, flip-to-flip %.1f ms average", dn, dn ? (g_gpu_frame_ns - last_ns) / 1e6 / dn : 0.0);
      last_ns = g_gpu_frame_ns; last_n = g_gpu_frames; }
    Nv2aGlFrameTimelineLog();
    Nv2aGlLog("state cache: attributes %llu calls / %llu skips, uniforms %llu calls / %llu skips; legacy state %u", g_attrib_calls, g_attrib_skips, g_uniform_calls, g_uniform_skips, Nv2aLegacyState());
    Nv2aGlLog("depth color copies: %llu total, %llu shader; legacy depth %u",g_depth_color_copies,g_depth_compute_copies,Nv2aLegacyDepthCopy());
    Nv2aGlLog("fixed state groups: %llu submitted, %llu skipped; legacy fixed %u",g_fixed_group_calls,g_fixed_group_skips,Nv2aLegacyFixedState());
    Nv2aGlLog("vertex constant cache: %llu vectors skipped; legacy constants %u",g_vsh_cached_vectors,Nv2aLegacyConstants());
    Nv2aGlLog("shader program aliases: %llu redundant rebinds/full uploads skipped",g_program_alias_reuses);
    Nv2aGlLog("vertex constants: %llu calls, %llu vectors; legacy draws %u", g_vsh_upload_calls, g_vsh_upload_vectors, Nv2aLegacyDraws());
    Nv2aGlLog("visibility points: %llu (refreshed %llu, kept their fence %llu), %u pending; fences created %llu, shared %llu",
              g_owned_stats[0], g_owned_stats[1], g_owned_refresh_kept, g_owned_count.load(), g_owned_fences_created, g_owned_fences_shared);
}
// Download dirty render surfaces to RAM (before the display or the title reads them).
extern "C" void Nv2aGlFlushSurfaces() {
    PGRAPHState* pg = &g_nv2a_gl_state.pgraph;
    qemu_mutex_lock(&pg->pgraph_lock);
    { extern int g_download_site; g_download_site = 189; pgraph_update_surface(&g_nv2a_gl_state, false, true, true); g_download_site = 0; }
    qemu_mutex_unlock(&pg->pgraph_lock);
}
