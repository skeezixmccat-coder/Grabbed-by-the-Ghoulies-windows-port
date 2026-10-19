// Modified for the Ghoulies native Windows preview, 2026-10-14 through 2026-10-19.
// Port integration, validation and rendering changes: see docs/PORT-PROVENANCE.md.
#include <set>
#include <emmintrin.h>
#include "compat.h"
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
// *  This file is heavily based on code from XQEMU
// *  https://github.com/xqemu/xqemu/blob/master/hw/xbox/nv2a/nv2a_pgraph.c
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

// FIXME
#define qemu_mutex_lock_iothread()
#define qemu_mutex_unlock_iothread()
extern "C" __declspec(noreturn) void Nv2aGlUnknown(const char* fmt, ...);
extern "C" void Nv2aGlLog(const char* fmt, ...);
extern "C" bool Nv2aGlTraceEnabled();
extern "C" void Nv2aGlDumpTexture(uint32_t phys, unsigned w, unsigned h, unsigned pitch, unsigned fmt, unsigned count);
extern "C" uint64_t Nv2aGlHarvestRange(uint32_t physical, uint32_t size);
extern "C" uint64_t Nv2aGlHarvestAll();
extern "C" uint64_t Nv2aGlStampRange(uint32_t physical, uint32_t size);
extern "C" uint64_t Nv2aGlCurrentGeneration();
// Guest writes are harvested once at the start of each draw and clear (the
// title writes vertex/texture/surface data before it kicks the commands that
// use it); checks in between read the harvested page generations.
extern "C" uint64_t Nv2aGlRangeGeneration(uint32_t physical, uint32_t size);
#include <unordered_map>
#include "texture_units.inc"
#include "resolution.inc"
// A pusher span ends at a newly published PUT or any guest-visible
// synchronising method. Between those boundaries the guest cannot legally
// recycle bytes consumed by the queued draws. Cache range harvests only
// within that span; calls outside a held pusher batch always harvest.
static bool g_pgraph_batch_held = false;
static uint64_t g_texture_fetch_epoch = 1;
static std::unordered_map<uint64_t, uint64_t> g_texture_harvests;
extern "C" void Nv2aGlTextureBarrier() { ++g_texture_fetch_epoch; }
// Within a held span, the first texture fetch harvests all of guest memory
// once (one GetWriteWatch over 64 MiB); later fetches in the same span need
// no system call. Per-range calls cost ~a quarter of the GPU thread's time
// in draw-heavy rooms (~1,700 draws per frame).
static uint64_t g_full_harvest_epoch = 0;
// The pusher's harvest at a newly published PUT also serves the span it opens
// (it was followed by a second full harvest at the first texture fetch).
extern "C" unsigned long long g_harvest_span = 0, g_harvest_tex = 0, g_harvest_vtx_invalidate = 0;
extern "C" void Nv2aGlHarvestAllForSpan() { Nv2aGlHarvestAll(); ++g_harvest_span; g_full_harvest_epoch = g_texture_fetch_epoch; }
static void Nv2aHarvestTexture(uint32_t addr, uint32_t size) {
    if (g_pgraph_batch_held) {
        if (g_full_harvest_epoch != g_texture_fetch_epoch) { Nv2aGlHarvestAll(); ++g_harvest_tex; g_full_harvest_epoch = g_texture_fetch_epoch; }
        return;
    }
    uint64_t key = (uint64_t(addr) << 32) | size;
    if (g_pgraph_batch_held) {
        auto it = g_texture_harvests.find(key);
        if (it != g_texture_harvests.end() && it->second == g_texture_fetch_epoch) return;
    }
    Nv2aGlHarvestRange(addr, size);
    if (g_pgraph_batch_held) {
        if (g_texture_harvests.size() >= 65536) g_texture_harvests.clear();
        g_texture_harvests[key] = g_texture_fetch_epoch;
    }
}

unsigned long long g_surf_stats[4] = {};
#include <unordered_map>
int g_download_site = 0;
unsigned long long g_gpu_frame_ns = 0, g_gpu_frames = 0;   // flip-to-flip GPU timestamps
extern "C" unsigned long long g_nv2a_render_serial = 0;   // GPU writes into surface textures (surface_owner.inc)
// Frame timeline (QPC, GPU thread): per flip, the pusher's work since the
// previous flip (minus its waits for new commands), those waits, the FLIP_STALL
// wait and the present. Separately for all frames and for slow ones (> 40 ms).
extern "C" long long g_gpu_idle_ticks;
struct Nv2aTimeline { unsigned long long n = 0; double work = 0, idle = 0, stall = 0, present = 0, total = 0; };
static Nv2aTimeline g_tl_all, g_tl_slow;
static unsigned long long g_tl_over18=0, g_tl_over25=0;
static double g_tl_max=0;
static long long g_tl_prev_exit = 0, g_tl_prev_idle = 0;
extern "C" void Nv2aGlFrameTimelineLog() {
    auto f = [](const Nv2aTimeline& t, char* b, size_t n) {
        double k = t.n ? 1.0 / t.n : 0;
        snprintf(b, n, "%llu frames: work %.1f, waiting for commands %.1f, flip stall %.1f, present %.1f, total %.1f ms",
                 t.n, t.work * k, t.idle * k, t.stall * k, t.present * k, t.total * k);
    };
    char a[200], s[200]; f(g_tl_all, a, sizeof a); f(g_tl_slow, s, sizeof s);
    Nv2aGlLog("frame timeline: all %s; slow (>40 ms) %s; async presents %llu, dropped %llu", a, s, g_present_async, g_present_dropped);
    Nv2aGlLog("frame pacing: %llu frames, >18ms %llu, >25ms %llu, max %.2f ms (GPU-thread flip intervals, not monitor delivery)",
        g_tl_all.n, g_tl_over18, g_tl_over25, g_tl_max);
    g_tl_over18=g_tl_over25=0; g_tl_max=0;
    g_tl_all = {}; g_tl_slow = {};
}
unsigned long long g_vertex_mirror_stats[2] = {};   // glBufferSubData calls, bytes
// Native port: draw-state caches (OpenGL_draw_state_update); anything else that
// changes the cached GL state clears these.
static bool g_fixed_gl_state_valid = false, g_scissor_viewport_valid = false;
static bool Nv2aGlDebugChecks() {
    static const bool enabled = [] { char v[2] = {}; return GetEnvironmentVariableA("GH_GPU_DEBUG", v, sizeof v) == 1 && v[0] == '1'; }();
    return enabled;
}
static void pgraph_check_gl_errors(const char* where) {
    GLenum e = glGetError();
    if (e != GL_NO_ERROR) Nv2aGlUnknown("GL error %#x %s", e, where);
}
static void pgraph_debug_check_gl(int line) {
    if (!Nv2aGlDebugChecks()) return;
    GLenum e = glGetError();
    if (e != GL_NO_ERROR) Nv2aGlUnknown("GL error %#x at EmuNV2A_PGRAPH.cpp:%d", e, line);
}
unsigned long long g_direct_texture_copies = 0;
unsigned long long g_direct_texture_evictions = 0;
// Deferred surface downloads (surface_owner.inc, included after this file).
static uint64_t Nv2aOwnedRegister(uint32_t addr, uint32_t len, uint32_t width, uint32_t height, uint32_t pitch, uint32_t bpp,
                                  GLenum format, GLenum type, bool swizzle, GLuint tex);
static void Nv2aOwnedRetireTexture(GLuint tex);
static uint64_t g_owned_register_surface_gen = 0;   // the surface's memory generation before this visibility point
static GLuint Nv2aOwnedMatch(uint32_t physical, uint32_t length, uint32_t width, uint32_t height, uint32_t bpp,
                             GLenum format, GLenum type, bool swizzled, uint32_t pitch);
static bool Nv2aOwnedPendingRange(uint32_t addr, uint32_t size);
static void Nv2aOwnedDescribe(uint32_t addr, uint32_t len, char* out, size_t cap);
static GLuint Nv2aOwnedMatchDepth(uint32_t physical, uint32_t length, uint32_t width, uint32_t height, uint32_t pitch);
static void Nv2aOwnedCopyDepthAsColor(GLuint src, GLuint dst, uint32_t width, uint32_t height, GLenum dst_format);
static void Nv2aOwnedCopyFlipped(GLuint src, GLuint dst, uint32_t width, uint32_t height);
static void Nv2aOwnedDiscardTextureLocked(GLuint tex, uint32_t addr, uint32_t len);
static void Nv2aOwnedMarkSuperseded(uint32_t addr, uint32_t len, GLuint tex);
static unsigned long long g_surface_inherits = 0;
extern "C" void Nv2aOwnedLock(); extern "C" void Nv2aOwnedUnlock();
extern "C" unsigned Nv2aGlRenderScale();
extern "C" void Nv2aOwnedFlush(uint32_t physical, uint32_t size);
static void Nv2aOwnedFlushFor(const char* reason, uint32_t physical, uint32_t size);
namespace { extern bool g_owned_sync; }
static std::unordered_map<int, unsigned long long> g_download_counts;
extern "C" unsigned long long g_nv2a_present_epoch;
unsigned long long g_nv2a_shader_compiles = 0, g_nv2a_texture_uploads = 0, g_nv2a_surface_uploads = 0;
extern "C" void Nv2aGlPresent(NV2AState* d);
extern "C" void Nv2aGlReapFences();
extern "C" void update_irq(NV2AState *d);
void *nv_dma_map(NV2AState *d, xbox::addr_xt dma_obj_address, xbox::addr_xt *len);
DMAObject nv_dma_load(NV2AState *d, xbox::addr_xt dma_obj_address);

// Xbox uses 4 KiB pages
#define TARGET_PAGE_BITS 12
#define TARGET_PAGE_SIZE (1 << TARGET_PAGE_BITS)
#define TARGET_PAGE_MASK ~(TARGET_PAGE_SIZE - 1)
#define TARGET_PAGE_ALIGN(addr) (((addr) + TARGET_PAGE_SIZE - 1) & TARGET_PAGE_MASK)

static const GLenum pgraph_texture_min_filter_map[] = {
    0,
    GL_NEAREST,
    GL_LINEAR,
    GL_NEAREST_MIPMAP_NEAREST,
    GL_LINEAR_MIPMAP_NEAREST,
    GL_NEAREST_MIPMAP_LINEAR,
    GL_LINEAR_MIPMAP_LINEAR,
    GL_LINEAR, /* TODO: Convolution filter... */
};

static const GLenum pgraph_texture_mag_filter_map[] = {
    0,
    GL_NEAREST,
    GL_LINEAR,
    0,
    GL_LINEAR /* TODO: Convolution filter... */
};

// These NV2A shader modes have no texture fetch of their own. A retained
// texture-enable bit does not make stale sampler state an input to NONE,
// PASSTHRU, CLIPPLANE, DOT_ZW or DOTPRODUCT. Their scalar/colour computations
// are handled by psh_translate. All other modes reach their normal validation.
static bool pgraph_texture_shader_fetches(uint32_t program, unsigned stage) {
    switch ((program >> (stage * 5)) & 31) {
    case 0: case 4: case 5: case 0x0a: case 0x11: return false;
    default: return true;
    }
}

static const GLenum pgraph_texture_addr_map[] = {
    0,
    GL_REPEAT,
    GL_MIRRORED_REPEAT,
    GL_CLAMP_TO_EDGE,
    GL_CLAMP_TO_BORDER,
    // GL_CLAMP
};

static const GLenum pgraph_blend_factor_map[] = {
    GL_ZERO,
    GL_ONE,
    GL_SRC_COLOR,
    GL_ONE_MINUS_SRC_COLOR,
    GL_SRC_ALPHA,
    GL_ONE_MINUS_SRC_ALPHA,
    GL_DST_ALPHA,
    GL_ONE_MINUS_DST_ALPHA,
    GL_DST_COLOR,
    GL_ONE_MINUS_DST_COLOR,
    GL_SRC_ALPHA_SATURATE,
    0,
    GL_CONSTANT_COLOR,
    GL_ONE_MINUS_CONSTANT_COLOR,
    GL_CONSTANT_ALPHA,
    GL_ONE_MINUS_CONSTANT_ALPHA,
};

static const GLenum pgraph_blend_equation_map[] = {
    GL_FUNC_SUBTRACT,
    GL_FUNC_REVERSE_SUBTRACT,
    GL_FUNC_ADD,
    GL_MIN,
    GL_MAX,
    GL_FUNC_REVERSE_SUBTRACT,
    GL_FUNC_ADD,
};

static const GLenum pgraph_blend_logicop_map[] = {
    GL_CLEAR,
    GL_AND,
    GL_AND_REVERSE,
    GL_COPY,
    GL_AND_INVERTED,
    GL_NOOP,
    GL_XOR,
    GL_OR,
    GL_NOR,
    GL_EQUIV,
    GL_INVERT,
    GL_OR_REVERSE,
    GL_COPY_INVERTED,
    GL_OR_INVERTED,
    GL_NAND,
    GL_SET,
};

static const GLenum pgraph_cull_face_map[] = {
    0,
    GL_FRONT,
    GL_BACK,
    GL_FRONT_AND_BACK
};

static const GLenum pgraph_depth_func_map[] = {
    GL_NEVER,
    GL_LESS,
    GL_EQUAL,
    GL_LEQUAL,
    GL_GREATER,
    GL_NOTEQUAL,
    GL_GEQUAL,
    GL_ALWAYS,
};

static const GLenum pgraph_stencil_func_map[] = {
    GL_NEVER,
    GL_LESS,
    GL_EQUAL,
    GL_LEQUAL,
    GL_GREATER,
    GL_NOTEQUAL,
    GL_GEQUAL,
    GL_ALWAYS,
};

static const GLenum pgraph_stencil_op_map[] = {
    0,
    GL_KEEP,
    GL_ZERO,
    GL_REPLACE,
    GL_INCR,
    GL_DECR,
    GL_INVERT,
    GL_INCR_WRAP,
    GL_DECR_WRAP,
};

enum FormatEncoding {
	linear = 0,
	swizzled, // for all NV097_SET_TEXTURE_FORMAT_*_SZ_*
	compressed // for all NV097_SET_TEXTURE_FORMAT_*_DXT*
};

typedef struct ColorFormatInfo {
    unsigned int bytes_per_pixel; // Derived from the total number of channel bits
    FormatEncoding encoding;
    GLint gl_internal_format;
    GLenum gl_format; // == 0 for compressed formats
    GLenum gl_type;
    GLint *gl_swizzle_mask; // == nullptr when gl_internal_format, gl_format and gl_type are sufficient
} ColorFormatInfo;

// Resulting gl_internal_format, gl_format and gl_type values, for formats handled by convert_texture_data()
#define GL_CONVERT_TEXTURE_DATA_RESULTING_FORMAT GL_RGBA8, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV

static GLint gl_swizzle_mask_111R[4] = { GL_ONE, GL_ONE, GL_ONE, GL_RED };
static GLint gl_swizzle_mask_1GR1[4] = { GL_ONE, GL_GREEN, GL_RED, GL_ONE };
static GLint gl_swizzle_mask_BGRA[4] = { GL_BLUE, GL_GREEN, GL_RED, GL_ALPHA }; // Used in cxbx_gl_render_overlays()
static GLint gl_swizzle_mask_G1R1[4] = { GL_GREEN, GL_ONE, GL_RED, GL_ONE };
static GLint gl_swizzle_mask_RRR1[4] = { GL_RED, GL_RED, GL_RED, GL_ONE };
static GLint gl_swizzle_mask_RRRG[4] = { GL_RED, GL_RED, GL_RED, GL_GREEN };
static GLint gl_swizzle_mask_RRRR[4] = { GL_RED, GL_RED, GL_RED, GL_RED };

// Note : Avoid designated initializers to facilitate C++ builds
static const ColorFormatInfo kelvin_color_format_map[256] = {
    //0x00 [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_Y8] =
        {1, swizzled, GL_R8, GL_RED, GL_UNSIGNED_BYTE,
         gl_swizzle_mask_RRR1},
    //0x01 [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_AY8] =
        {1, swizzled, GL_R8, GL_RED, GL_UNSIGNED_BYTE,
         gl_swizzle_mask_RRRR},
    //0x02 [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A1R5G5B5] =
        {2, swizzled, GL_RGB5_A1, GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV},
    //0x03 [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_X1R5G5B5] =
        {2, swizzled, GL_RGB5, GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV},
    //0x04 [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A4R4G4B4] =
        {2, swizzled, GL_RGBA4, GL_BGRA, GL_UNSIGNED_SHORT_4_4_4_4_REV},
    //0x05 [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R5G6B5] =
        {2, swizzled, GL_RGB565, GL_RGB, GL_UNSIGNED_SHORT_5_6_5},
    //0x06 [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8] =
        {4, swizzled, GL_RGBA8, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV},
    //0x07 [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_X8R8G8B8] =
        {4, swizzled, GL_RGB8, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV},
	//0x08 [?] =
		{},
	//0x09 [?] =
		{},
	//0x0A [?] =
		{},

    /* paletted texture */
    //0x0B [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8] = // See convert_texture_data
        {1, swizzled, GL_CONVERT_TEXTURE_DATA_RESULTING_FORMAT},

    //0x0C [NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5] =
        {4, compressed, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, 0, GL_RGBA},
	//0x0D [?] =
		{},
    //0x0E [NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT23_A8R8G8B8] =
        {4, compressed, GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, 0, GL_RGBA},
    //0x0F [NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT45_A8R8G8B8] =
        {4, compressed, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, 0, GL_RGBA},
    //0x10 [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A1R5G5B5] =
        {2, linear, GL_RGB5_A1, GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV},
    //0x11 [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R5G6B5] =
        {2, linear, GL_RGB565, GL_RGB, GL_UNSIGNED_SHORT_5_6_5},
    //0x12 [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8] =
        {4, linear, GL_RGBA8, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV},
    //0x13 [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_Y8] =
        {1, linear, GL_R8, GL_RED, GL_UNSIGNED_BYTE,
         gl_swizzle_mask_RRR1},
	//0x14 [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_SY8] =
        {1, linear, GL_R8_SNORM, GL_RED, GL_BYTE,
         gl_swizzle_mask_RRR1}, // TODO : Verify
	//0x15 [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X7SY9] = // See convert_texture_data FIXME
		{2, linear, GL_CONVERT_TEXTURE_DATA_RESULTING_FORMAT}, // TODO : Verify
	//0x16 [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R8B8] =
        {2, linear, GL_RG8, GL_RG, GL_UNSIGNED_BYTE,
         gl_swizzle_mask_G1R1}, // TODO : Verify
	//0x17 [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_G8B8] =
        {2, linear, GL_RG8, GL_RG, GL_UNSIGNED_BYTE,
         gl_swizzle_mask_1GR1}, // TODO : Verify
	//0x18 [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_SG8SB8] =
        {2, linear, GL_RG8_SNORM, GL_RG, GL_BYTE,
         gl_swizzle_mask_1GR1}, // TODO : Verify

    //0x19 [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8] =
        {1, swizzled, GL_R8, GL_RED, GL_UNSIGNED_BYTE,
         gl_swizzle_mask_111R},
    //0x1A [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8Y8] =
        {2, swizzled, GL_RG8, GL_RG, GL_UNSIGNED_BYTE,
         gl_swizzle_mask_RRRG},
    //0x1B [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_AY8] =
        {1, linear, GL_R8, GL_RED, GL_UNSIGNED_BYTE,
         gl_swizzle_mask_RRRR},
    //0x1C [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X1R5G5B5] =
        {2, linear, GL_RGB5, GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV},
    //0x1D [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A4R4G4B4] =
        {2, linear, GL_RGBA4, GL_BGRA, GL_UNSIGNED_SHORT_4_4_4_4_REV}, // TODO : Verify this is truely linear
    //0x1E [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X8R8G8B8] =
        {4, linear, GL_RGB8, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV},
    //0x1F [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8] =
        {1, linear, GL_R8, GL_RED, GL_UNSIGNED_BYTE,
         gl_swizzle_mask_111R},
    //0x20 [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8Y8] =
        {2, linear, GL_RG8, GL_RG, GL_UNSIGNED_BYTE,
         gl_swizzle_mask_RRRG},
	//0x21 [?] =
		{},
	//0x22 [?] =
		{},
	//0x23 [?] =
		{},
	//0x24 [NV097_SET_TEXTURE_FORMAT_COLOR_LC_IMAGE_CR8YB8CB8YA8] = // See convert_texture_data calling ____UYVYToARGBRow_C
		{2, linear, GL_CONVERT_TEXTURE_DATA_RESULTING_FORMAT}, // TODO : Verify
	//0x25 [NV097_SET_TEXTURE_FORMAT_COLOR_LC_IMAGE_YB8CR8YA8CB8] = // See convert_texture_data calling ____YUY2ToARGBRow_C
		{2, linear, GL_CONVERT_TEXTURE_DATA_RESULTING_FORMAT}, // TODO : Verify
	//0x26 [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8CR8CB8Y8] = // See convert_texture_data FIXME
		{2, linear, GL_CONVERT_TEXTURE_DATA_RESULTING_FORMAT}, // TODO : Verify

    //0x27 [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R6G5B5] = // See convert_texture_data calling __R6G5B5ToARGBRow_C
        {2, swizzled, GL_CONVERT_TEXTURE_DATA_RESULTING_FORMAT}, // TODO : Verify
    //0x28 [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_G8B8] =
        {2, swizzled, GL_RG8, GL_RG, GL_UNSIGNED_BYTE,
         gl_swizzle_mask_1GR1},
    //0x29 [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R8B8] =
        {2, swizzled, GL_RG8, GL_RG, GL_UNSIGNED_BYTE,
         gl_swizzle_mask_G1R1},
	//0x2A [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_DEPTH_X8_Y24_FIXED] =
		{4, swizzled, GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8}, // TODO : Verify
	//0x2B [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_DEPTH_X8_Y24_FLOAT] =
		{4, swizzled, GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_FLOAT_32_UNSIGNED_INT_24_8_REV}, // TODO : Verify
	//0x2C [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_DEPTH_Y16_FIXED] =
		{2, swizzled, GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT}, // TODO : Verify
	//0x2D [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_DEPTH_Y16_FLOAT] =
		{2, swizzled, GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT, GL_FLOAT}, // TODO : Verify


    /* TODO: format conversion */
    //0x2E [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_X8_Y24_FIXED] =
        {4, linear, GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8},
	//0x2F [?] =
		{},
    //0x30 [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_Y16_FIXED] =
        {2, linear, GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT},
	//0x31 [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_Y16_FLOAT] =
        {2, linear, GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT, GL_FLOAT}, // TODO : Verify
	//0x32 [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_Y16] =
        {2, swizzled, GL_R16, GL_RED, GL_UNSIGNED_SHORT, // TODO : Verify
         gl_swizzle_mask_RRR1},
	//0x33 [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_YB16YA16] =
        {4, swizzled, GL_RG16, GL_RG, GL_UNSIGNED_SHORT, // TODO : Verify
         gl_swizzle_mask_RRRG},
	//0x34 [NV097_SET_TEXTURE_FORMAT_COLOR_LC_IMAGE_A4V6YB6A4U6YA6] = // TODO : handle in convert_texture_data
		{2, linear, GL_CONVERT_TEXTURE_DATA_RESULTING_FORMAT}, // TODO : Verify
    //0x35 [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_Y16] =
        {2, linear, GL_R16, GL_RED, GL_UNSIGNED_SHORT,
         gl_swizzle_mask_RRR1},
	//0x36 [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_YB16YA16] =
        {4, linear, GL_RG16, GL_RG, GL_UNSIGNED_SHORT, // TODO : Verify
         gl_swizzle_mask_RRRG},
	//0x37 [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R6G5B5] = // See convert_texture_data calling __R6G5B5ToARGBRow_C
        {2, linear, GL_CONVERT_TEXTURE_DATA_RESULTING_FORMAT}, // TODO : Verify
	//0x38 [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R5G5B5A1] =
		{2, swizzled, GL_RGB5_A1, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1}, // TODO : Verify
	//0x39 [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R4G4B4A4] =
		{2, swizzled, GL_RGBA, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4}, // TODO : Verify
    //0x3A [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8B8G8R8] =
        {4, swizzled, GL_RGBA8, GL_RGBA, GL_UNSIGNED_INT_8_8_8_8_REV}, // TODO : Verify
	//0x3B [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_B8G8R8A8] =
		{4, swizzled, GL_RGBA8, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8}, // TODO : Verify

    //0x3C [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R8G8B8A8] =
        {4, swizzled, GL_RGBA8, GL_RGBA, GL_UNSIGNED_INT_8_8_8_8},
	//0x3D [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R5G5B5A1] =
		{2, linear, GL_RGB5_A1, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1}, // TODO : Verify
	//0x3E [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R4G4B4A4] =
        {2, linear, GL_RGBA, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4}, // TODO : Verify

    //0x3F [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8B8G8R8] =
        {4, linear, GL_RGBA8, GL_RGBA, GL_UNSIGNED_INT_8_8_8_8_REV}, // TODO : Verify
    //0x40 [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_B8G8R8A8] =
        {4, linear, GL_RGBA8, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8}, // TODO : Verify
    //0x41 [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R8G8B8A8] =
        {4, linear, GL_RGBA8, GL_RGBA, GL_UNSIGNED_INT_8_8_8_8}, // TODO : Verify
};

typedef struct SurfaceColorFormatInfo {
    unsigned int bytes_per_pixel;
    GLint gl_internal_format;
    GLenum gl_format;
    GLenum gl_type;
} SurfaceColorFormatInfo;

// Note : Avoid designated initializers to facilitate C++ builds
static const SurfaceColorFormatInfo kelvin_surface_color_format_map[16] = {
	//0x00 [?] = 
		{},
    //0x01 [NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5] =
        {2, GL_RGB5_A1, GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV},
	//0x02 [NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_O1R5G5B5] =
		{},
    //0x03 [NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5] =
        {2, GL_RGB565, GL_RGB, GL_UNSIGNED_SHORT_5_6_5},
    //0x04 [NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8] =
        {4, GL_RGBA8, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV},
	//0x05 [NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_O8R8G8B8] =
		{},
	//0x06 [NV097_SET_SURFACE_FORMAT_COLOR_LE_X1A7R8G8B8_Z1A7R8G8B8] =
		{},
	//0x07 [NV097_SET_SURFACE_FORMAT_COLOR_LE_X1A7R8G8B8_O1A7R8G8B8] =
		{},
    //0x08 [NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8] =
        {4, GL_RGBA8, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV},
	//0x09 [NV097_SET_SURFACE_FORMAT_COLOR_LE_B8] =
		{1, GL_R8, GL_RED, GL_UNSIGNED_BYTE}, // PatrickvL guesstimate
	//0x0A [NV097_SET_SURFACE_FORMAT_COLOR_LE_G8B8] =
		{2, GL_RG8, GL_RG, GL_UNSIGNED_BYTE}, // PatrickvL guesstimate; xemu uses GL_UNSIGNED_SHORT?
	//0x0B [?] = 
		{},
	//0x0C [?] = 
		{},
	//0x0D [?] = 
		{},
	//0x0E [?] = 
		{},
	//0x0F [?] = 
		{}
};

void (*pgraph_draw_arrays)(NV2AState *d);
void (*pgraph_draw_inline_buffer)(NV2AState *d);
void (*pgraph_draw_inline_array)(NV2AState *d);
void (*pgraph_draw_inline_elements)(NV2AState *d);
void (*pgraph_draw_state_update)(NV2AState *d);
void (*pgraph_draw_clear)(NV2AState *d);

//static void pgraph_set_context_user(NV2AState *d, uint32_t value);
void pgraph_handle_method(NV2AState *d, unsigned int subchannel, unsigned int method, uint32_t parameter);
static void pgraph_log_method(unsigned int subchannel, unsigned int graphics_class, unsigned int method, uint32_t parameter);
static void pgraph_allocate_inline_buffer_vertices(PGRAPHState *pg, unsigned int attr);
static void pgraph_finish_inline_buffer_vertex(PGRAPHState *pg);
static void pgraph_update_shader_constants(PGRAPHState *pg, ShaderBinding *binding, bool binding_changed, bool vertex_program, bool fixed_function);
static void pgraph_bind_shaders(PGRAPHState *pg);
static bool pgraph_get_framebuffer_dirty(PGRAPHState *pg);
static bool pgraph_get_color_write_enabled(PGRAPHState *pg);
static bool pgraph_get_zeta_write_enabled(PGRAPHState *pg);
static void pgraph_set_surface_dirty(PGRAPHState *pg, bool color, bool zeta);
static void pgraph_update_surface_part(NV2AState *d, bool upload, bool color);
static void pgraph_update_surface(NV2AState *d, bool upload, bool color_write, bool zeta_write);
static void pgraph_bind_textures(NV2AState *d);
static void pgraph_apply_anti_aliasing_factor(PGRAPHState *pg, unsigned int *width, unsigned int *height);
static void pgraph_get_surface_dimensions(PGRAPHState *pg, unsigned int *width, unsigned int *height);
static void pgraph_update_memory_buffer(NV2AState *d, hwaddr addr, hwaddr size, bool f);
static void pgraph_bind_vertex_attributes(NV2AState *d, unsigned int num_elements, bool inline_data, unsigned int inline_stride);
static unsigned int pgraph_bind_inline_array(NV2AState *d);

static float convert_f16_to_float(uint16_t f16);
static float convert_f24_to_float(uint32_t f24);
static uint8_t* convert_texture_data(const unsigned int color_format, const uint8_t *data, const uint8_t *palette_data, const unsigned int width, const unsigned int height, const unsigned int depth, const unsigned int row_pitch, const unsigned int slice_pitch);
static int upload_gl_texture(GLenum gl_target, const TextureShape s, const uint8_t *texture_data, const uint8_t *palette_data);
static TextureBinding* generate_texture(const TextureShape s, const uint8_t *texture_data, const uint8_t *palette_data);
static guint texture_key_hash(gconstpointer key);
static gboolean texture_key_equal(gconstpointer a, gconstpointer b);
static gpointer texture_key_retrieve(gpointer key, gpointer user_data, GError **error);
static void texture_key_destroy(gpointer data);
static void texture_binding_destroy(gpointer data);
static guint shader_hash(gconstpointer key);
static gboolean shader_equal(gconstpointer a, gconstpointer b);
static unsigned int kelvin_map_stencil_op(uint32_t parameter);
static unsigned int kelvin_map_polygon_mode(uint32_t parameter);
static unsigned int kelvin_map_texgen(uint32_t parameter, unsigned int channel);
static uint64_t fnv_hash(const uint8_t *data, size_t len);
static uint64_t fast_hash(const uint8_t *data, size_t len, unsigned int samples);

/* PGRAPH - accelerated 2d/3d drawing engine */

// RDI address is a DWORD index (GET_MASK removes the two low address bits).
// Layout: Nouveau nv20_gr_tile/init and nxdk pb_prepare_tiles; independently
// confirmed by retail D3D 0x17f740..0x17f840. See docs/SOURCES.md for revisions.
static unsigned int pgraph_rdi_memory_register(unsigned int address)
{
    if (address == 0) return 0x9a4; // PFB_CFG0 copy
    if (address == 1) return 0x9a8; // PFB_CFG1 copy
    if (address == 3) return NV_PGRAPH_ZCOMP_OFFSET;
    if (address >= 4 && address < 12) return NV_PGRAPH_TTILE((address - 4));
    if (address >= 12 && address < 20) return NV_PGRAPH_TLIMIT((address - 12));
    if (address >= 20 && address < 28) return NV_PGRAPH_TSIZE((address - 20));
    if (address >= 36 && address < 44) return NV_PGRAPH_ZCOMP((address - 36));
    Nv2aGlUnknown("RDI memory address %#x not provided", address * 4);
}

static uint32_t pgraph_rdi_read(PGRAPHState *pg,
                                unsigned int select, unsigned int address)
{
    switch (select) {
    case 0xea:
        pgraph_rdi_memory_register(address); // validate before indexing
        if (!pg->rdi_memory_written[address])
            Nv2aGlUnknown("RDI memory read before initialization at %#x", address * 4);
        return pg->rdi_memory[address];
    case 0x3d:
        if (address >= 15 || !pg->rdi_fd_written[address])
            Nv2aGlUnknown("RDI FD read before initialization or out of range at %#x", address * 4);
        return pg->rdi_fd[address];
    case RDI_INDEX_VTX_CONSTANTS0:
    case RDI_INDEX_VTX_CONSTANTS1:
        assert((address / 4) < NV2A_VERTEXSHADER_CONSTANTS);
        return pg->vsh_constants[address / 4][3 - address % 4];
    default:
        Nv2aGlUnknown("RDI read select %#x address %#x not provided", select, address);
    }
}

static void pgraph_rdi_write(PGRAPHState *pg,
                             unsigned int select, unsigned int address,
                             uint32_t val)
{
    switch (select) {
    case 0xea: {
        unsigned int reg = pgraph_rdi_memory_register(address);
        // Cxbx's renderer exposes logical linear VRAM rather than physical DRAM
        // partition/tile packing. D3D synchronizes the memory-controller copies
        // before drawing; require that protocol instead of accepting divergent
        // RDI-only configurations with unknown address-translation semantics.
        if (val != pg->regs[reg])
            Nv2aGlUnknown("RDI memory %#x value %#x differs from PGRAPH %#x = %#x",
                          address * 4, val, reg, pg->regs[reg]);
        // NV20 ZCOMP bit31 enables compression; bit26 selects 32-bit storage
        // (Nouveau nv20_fb_tile_init, nxdk pb_assign_tile). The reference's
        // logical VRAM stores expanded Z24S8, as does our GL_DEPTH24_STENCIL8
        // surface. Accept tag base zero only, and validate depth format/range at
        // surface use below. Physical tag RAM and Z offset remapping are absent.
        if ((address == 3 && val != 0) ||
            (address >= 36 && val != 0 && val != 0x84000000))
            Nv2aGlUnknown("RDI Z configuration %#x = %#x not provided", address * 4, val);
        pg->rdi_memory[address] = val;
        pg->rdi_memory_written[address] = true;
        break;
    }
    case 0x3d:
        // Envytools pgraph_state.cc identifies 15 FD patch/tessellation words.
        // Nouveau/nxdk and retail D3D clear all 15 on reset/context switch.
        // This renderer has no FD execution or in-flight FD work: only its reset
        // state is supported; nonzero state and unhandled FD methods still stop.
        if (address >= 15 || val != 0)
            Nv2aGlUnknown("RDI FD state address %#x value %#x not provided", address * 4, val);
        pg->rdi_fd[address] = val;
        pg->rdi_fd_written[address] = true;
        break;
    case 0xe0:
    case 0xdf:
        // D3D's software-method case 8 (ISR 0x17c1b6), sent only by
        // D3DDevice_SetRenderState_Dxt1NoiseEnable (XbSymbolDatabase scan,
        // 0x1715e0), writes the same value to RDI 0xe0:0x50 and 0xdf:0x08:
        // the DXT1 decompression noise enable (D3DRS_DXT1NOISEENABLE). Accept
        // exactly that pair with values 0/1. The noise itself is not modelled:
        // sampling a DXT1 texture while it is enabled stops at texture binding.
        if (!((select == 0xe0 && address == 0x14) || (select == 0xdf && address == 0x02)) || val > 1)
            Nv2aGlUnknown("RDI write select %#x address %#x value %#x not provided", select, address, val);
        pg->dxt1_noise[select == 0xe0 ? 0 : 1] = val;
        pg->dxt1_noise_written[select == 0xe0 ? 0 : 1] = true;
        break;
    case RDI_INDEX_VTX_CONSTANTS0:
    case RDI_INDEX_VTX_CONSTANTS1:
        assert(false); /* Untested */
        break;
    default:
        Nv2aGlUnknown("RDI write select %#x address %#x value %#x not provided", select, address, val);
    }
}

// Register access (replaces the Cxbx DEVICE_READ32/WRITE32 handlers; same semantics).
extern "C" uint32_t pgraph_read(NV2AState *d, uint32_t addr)
{
	PGRAPHState *pg = &d->pgraph;
	qemu_mutex_lock(&pg->pgraph_lock);
	uint32_t result = 0;
	switch (addr) {
	case NV_PGRAPH_INTR: result = pg->pending_interrupts; break;
	case NV_PGRAPH_INTR_EN: result = pg->enabled_interrupts; break;
	case NV_PGRAPH_RDI_DATA: {
		unsigned int select = GET_MASK(pg->regs[NV_PGRAPH_RDI_INDEX], NV_PGRAPH_RDI_INDEX_SELECT);
		int address = GET_MASK(pg->regs[NV_PGRAPH_RDI_INDEX], NV_PGRAPH_RDI_INDEX_ADDRESS);
		result = pgraph_rdi_read(pg, select, address);
		assert(address < GET_MASK(NV_PGRAPH_RDI_INDEX_ADDRESS, NV_PGRAPH_RDI_INDEX_ADDRESS));
		SET_MASK(pg->regs[NV_PGRAPH_RDI_INDEX], NV_PGRAPH_RDI_INDEX_ADDRESS, address + 1);
		break;
	}
	default: result = pg->regs[addr]; break;
	}
	qemu_mutex_unlock(&pg->pgraph_lock);
	return result;
}

extern "C" void update_irq(NV2AState *d);

extern "C" void pgraph_write(NV2AState *d, uint32_t addr, uint32_t value)
{
	PGRAPHState *pg = &d->pgraph;
	qemu_mutex_lock(&pg->pgraph_lock);
	switch (addr) {
	case NV_PGRAPH_INTR:
		pg->pending_interrupts &= ~value;
		qemu_cond_broadcast(&pg->interrupt_cond);
		break;
	case NV_PGRAPH_INTR_EN:
		pg->enabled_interrupts = value;
		break;
	case NV_PGRAPH_INCREMENT:
		if (value & NV_PGRAPH_INCREMENT_READ_3D) {
			SET_MASK(pg->regs[NV_PGRAPH_SURFACE], NV_PGRAPH_SURFACE_READ_3D,
				(GET_MASK(pg->regs[NV_PGRAPH_SURFACE], NV_PGRAPH_SURFACE_READ_3D) + 1)
				% GET_MASK(pg->regs[NV_PGRAPH_SURFACE], NV_PGRAPH_SURFACE_MODULO_3D));
			qemu_cond_broadcast(&pg->flip_3d);
		}
		break;
	case NV_PGRAPH_RDI_DATA: {
		unsigned int select = GET_MASK(pg->regs[NV_PGRAPH_RDI_INDEX], NV_PGRAPH_RDI_INDEX_SELECT);
		int address = GET_MASK(pg->regs[NV_PGRAPH_RDI_INDEX], NV_PGRAPH_RDI_INDEX_ADDRESS);
		pgraph_rdi_write(pg, select, address, value);
		assert(address < GET_MASK(NV_PGRAPH_RDI_INDEX_ADDRESS, NV_PGRAPH_RDI_INDEX_ADDRESS));
		SET_MASK(pg->regs[NV_PGRAPH_RDI_INDEX], NV_PGRAPH_RDI_INDEX_ADDRESS, address + 1);
		break;
	}
	case NV_PGRAPH_CHANNEL_CTX_TRIGGER: {
		xbox::addr_xt context_address = GET_MASK(pg->regs[NV_PGRAPH_CHANNEL_CTX_POINTER], NV_PGRAPH_CHANNEL_CTX_POINTER_INST) << 4;
		if (value & NV_PGRAPH_CHANNEL_CTX_TRIGGER_READ_IN) {
			uint8_t *context_ptr = d->pramin.ramin_ptr + context_address;
			pg->regs[NV_PGRAPH_CTX_USER] = ldl_le_p((uint32_t*)context_ptr);
		}
		break;
	}
	default:
		pg->regs[addr] = value;
		break;
	}
	if (addr == NV_PGRAPH_FIFO) qemu_cond_broadcast(&pg->fifo_access_cond);
	qemu_mutex_unlock(&pg->pgraph_lock);
}

// SWATH_WIDTH (method 0x9f8, SETUPRASTER bits 25..27) consumption.
// Swath width selects how the rasteriser walks the screen in swaths. With
// antialiasing off, every pixel's coverage, interpolants, depth/stencil test and
// blend are computed independently of traversal order, and primitive order is
// preserved by the FIFO; so traversal order cannot change the stored image. This
// matches the evidence: nxdk 14d5ee97 sets it with AA off ("SwathWidth=128"),
// the nxdk_pgraph_tests author (issue 98) found no differentiated output and
// ties it to AA mode changes, and both reference renderers (Cxbx 585c49a,
// xemu 36334d95) ignore it. With a supersampled surface (CENTER_CORNER_2,
// SQUARE_OFFSET_4) the same argument holds per sample: the port renders such
// surfaces at the scaled size, as both references do, and every sample is
// still computed independently of traversal order. Multisample control must
// be disabled (enforced at 0x1d7c) and point/line/polygon smoothing off
// (enforced below), since coverage-based AA would need a real sample model.
static void pgraph_require_swath_raster_semantics(PGRAPHState *pg)
{
    if (!pg->swath_width_written)
        Nv2aGlUnknown("raster submission with unestablished reset SWATH_WIDTH");
    if (pg->surface_shape.anti_aliasing > NV097_SET_SURFACE_FORMAT_ANTI_ALIASING_SQUARE_OFFSET_4)
        Nv2aGlUnknown("SWATH_WIDTH %#x with surface antialiasing mode %u not provided",
            pg->swath_width_parameter, pg->surface_shape.anti_aliasing);
    if (pg->regs[NV_PGRAPH_SETUPRASTER] & (7u << 9))
        Nv2aGlUnknown("SWATH_WIDTH %#x with point/line/polygon smoothing (SETUPRASTER %#x) not provided",
            pg->swath_width_parameter, pg->regs[NV_PGRAPH_SETUPRASTER]);
}


// NV097_SET_ZMIN_MAX_CONTROL consumption. Envytools f102b823 MthdKelvinDepthClamp
// establishes the latch (bit0 -> SETUPRASTER bit30, bit8 -> SETUPRASTER bit5,
// bit4 -> Z_CONFIG bit4, here ZCOMPRESSOCCLUDE as in xemu 36334d95); nxdk
// 14d5ee97 names them CULL_NEAR_FAR, ZCLAMP_CLAMP and CULL_IGNORE_W. This older
// port's vertex shader maps [ZCLIPMIN, ZCLIPMAX] onto GL clip space with W=1, so
// GL near/far clipping removes exactly the fragments whose screen-space z lies
// outside the range: the CULL_NEAR_FAR + ZCLAMP_CULL behaviour (xemu's
// fragment-discard form). The depth value GL stores is (z-min)/(max-min), which
// equals the hardware's normalised integer depth only when min = 0 and max is the
// zeta format's full scale. Every other configuration stops.
static void pgraph_require_depth_control_semantics(PGRAPHState *pg)
{
    uint32_t raster = pg->regs[NV_PGRAPH_SETUPRASTER];
    uint32_t zconfig = pg->regs[NV_PGRAPH_ZCOMPRESSOCCLUDE];
    if (pg->regs[NV_PGRAPH_SETUPRASTER] & (7u << 9))
        Nv2aGlUnknown("point/line/polygon smoothing (SETUPRASTER %#x) not provided", pg->regs[NV_PGRAPH_SETUPRASTER]);
    if (pg->polygon_stipple_enable)
        Nv2aGlUnknown("polygon stipple not provided");
    if (pg->primitive_mode == NV097_SET_BEGIN_END_OP_POINTS)
        Nv2aGlUnknown("point primitives not provided (POINT_SIZE %#x, point params %u)",
            pg->regs[0x198c], pg->point_params_enable);
    if (!pg->zmin_max_control_written)
        Nv2aGlUnknown("raster submission with unestablished reset ZMIN_MAX_CONTROL");
    if (!(raster & (1u << 30)) || (raster & (1u << 5)) || (zconfig & (1u << 4)))
        Nv2aGlUnknown("depth control (ZMIN_MAX_CONTROL %#x) other than near/far cull without clamp not provided",
            pg->zmin_max_control_parameter);
    uint32_t control_0 = pg->regs[NV_PGRAPH_CONTROL_0];
    bool depth_used = (control_0 & NV_PGRAPH_CONTROL_0_ZENABLE) || (control_0 & NV_PGRAPH_CONTROL_0_ZWRITEENABLE);
    if (!depth_used) return;
    float zclip_min = *(float*)&pg->regs[NV_PGRAPH_ZCLIPMIN];
    float zclip_max = *(float*)&pg->regs[NV_PGRAPH_ZCLIPMAX];
    if (raster & NV_PGRAPH_SETUPRASTER_Z_FORMAT)
        Nv2aGlUnknown("floating-point depth buffer values not provided");
    if (control_0 & NV_PGRAPH_CONTROL_0_Z_PERSPECTIVE_ENABLE)
        Nv2aGlUnknown("w-buffered depth values not provided");
    float full_scale;
    switch (pg->surface_shape.zeta_format) {
    case NV097_SET_SURFACE_FORMAT_ZETA_Z16: full_scale = 65535.0f; break;
    case NV097_SET_SURFACE_FORMAT_ZETA_Z24S8: full_scale = 16777215.0f; break;
    default: Nv2aGlUnknown("depth use with zeta format %#x not provided", pg->surface_shape.zeta_format);
    }
    if (zclip_min != 0.0f || zclip_max != full_scale)
        Nv2aGlUnknown("depth clip range [%g, %g] other than [0, %g]: stored depth values not provided",
            zclip_min, zclip_max, full_scale);
}


// T008: retain the state actually submitted to the renderer's single VAO.
// Array-buffer binding is part of a pointer's identity. Generic values become
// unspecified after drawing with that attribute enabled, so enabling an array
// always invalidates its cached constant, even if the enable call is skipped.
static bool Nv2aLegacyState() {
    static const bool legacy = [] {
        char v[8] = {}; DWORD n = GetEnvironmentVariableA("GH_GPU_LEGACY_STATE", v, sizeof v);
        if (n && (n != 1 || v[0] != '1')) Nv2aGlUnknown("GH_GPU_LEGACY_STATE must be 1 or unset");
        return n != 0;
    }();
    return legacy;
}
// GL_ARRAY_BUFFER binding is context state, independent of the VAO. Every
// production writer on this context uses this helper; buffers live until teardown.
static GLuint g_array_buffer=0;
static bool g_array_buffer_valid=false;
static void Nv2aBindArrayBuffer(GLuint buffer) {
    if (!Nv2aLegacyState() && g_array_buffer_valid && g_array_buffer==buffer) return;
    glBindBuffer(GL_ARRAY_BUFFER,buffer);
    g_array_buffer=buffer;g_array_buffer_valid=true;
}
struct Nv2aAttribState {
    bool enable_valid, enabled, pointer_valid, value_valid;
    GLuint buffer; GLint count; GLenum type; GLboolean normalized;
    GLsizei stride; const void* pointer; GLfloat value[4];
};
static Nv2aAttribState g_attrib_state[NV2A_VERTEXSHADER_ATTRIBUTES] = {};
static unsigned long long g_attrib_calls = 0, g_attrib_skips = 0;
static void Nv2aAttribEnable(unsigned i, bool enabled) {
    auto& c = g_attrib_state[i];
    if (enabled) c.value_valid = false;
    if (!Nv2aLegacyState() && c.enable_valid && c.enabled == enabled) { ++g_attrib_skips; return; }
    if (enabled) glEnableVertexAttribArray(i); else glDisableVertexAttribArray(i);
    c.enabled = enabled; c.enable_valid = true; ++g_attrib_calls;
}
static void Nv2aAttribPointer(unsigned i, GLuint buffer, GLint count, GLenum type,
                             GLboolean normalized, GLsizei stride, const void* pointer) {
    auto& c = g_attrib_state[i];
    if (!Nv2aLegacyState() && c.pointer_valid && c.buffer == buffer && c.count == count &&
        c.type == type && c.normalized == normalized && c.stride == stride && c.pointer == pointer) {
        ++g_attrib_skips; return;
    }
    // Caller binds this buffer before the pointer call, including on cache hits.
    glVertexAttribPointer(i, count, type, normalized, stride, pointer);
    c.buffer=buffer; c.count=count; c.type=type; c.normalized=normalized;
    c.stride=stride; c.pointer=pointer; c.pointer_valid=true; ++g_attrib_calls;
}
static void Nv2aAttribValue(unsigned i, const GLfloat* value) {
    auto& c = g_attrib_state[i];
    if (!Nv2aLegacyState() && c.value_valid && !memcmp(c.value,value,sizeof c.value)) { ++g_attrib_skips; return; }
    glVertexAttrib4fv(i,value); memcpy(c.value,value,sizeof c.value); c.value_valid=true; ++g_attrib_calls;
}
static unsigned long long g_uniform_calls = 0, g_uniform_skips = 0;
static bool Nv2aUniformChanged(ShaderBinding* binding, unsigned slot, const void* data, size_t bytes) {
    assert(slot < 82 && bytes <= 64);
    auto& c = binding->uniform_cache->slots[slot];
    if (!Nv2aLegacyState() && c.bytes == bytes && !memcmp(c.data,data,bytes)) { ++g_uniform_skips; return false; }
    memcpy(c.data,data,bytes); c.bytes=bytes; ++g_uniform_calls; return true;
}
static void Nv2aUniform1f(ShaderBinding* b, unsigned slot, GLint loc, GLfloat x) {
    if (Nv2aUniformChanged(b,slot,&x,sizeof x)) glUniform1f(loc,x);
}
static void Nv2aUniform2f(ShaderBinding* b, unsigned slot, GLint loc, GLfloat x, GLfloat y) {
    GLfloat v[2]={x,y}; if (Nv2aUniformChanged(b,slot,v,sizeof v)) glUniform2f(loc,x,y);
}
static void Nv2aUniform4fv(ShaderBinding* b, unsigned slot, GLint loc, const GLfloat* v) {
    if (Nv2aUniformChanged(b,slot,v,16)) glUniform4fv(loc,1,v);
}
static void Nv2aUniform3fv(ShaderBinding* b, unsigned slot, GLint loc, const GLfloat* v) {
    if (Nv2aUniformChanged(b,slot,v,12)) glUniform3fv(loc,1,v);
}
static void Nv2aUniform4i(ShaderBinding* b, unsigned slot, GLint loc, GLint x, GLint y, GLint z, GLint w) {
    GLint v[4]={x,y,z,w}; if (Nv2aUniformChanged(b,slot,v,sizeof v)) glUniform4i(loc,x,y,z,w);
}

void OpenGL_draw_end(NV2AState *d); // forward declaration

// Diagnostic draw dump: F9 in the game window records the next frame's draws
// (registers that decide visibility, textures, and an occlusion count of the
// samples each draw passed) and logs them at the following flip. Off otherwise.
extern "C" volatile long g_draw_dump_request = 0;
extern "C" void Nv2aGlDumpProgram(unsigned program, const char* why);
struct Nv2aDumpDraw { GLuint query; uint32_t prim, count, program, csv0d, c0, c1, c2, blend, raster, combctl, shaderprog, surf, zeta;
                      uint32_t texctl0[4], texfmt[4], texoff[4]; char attrs[400]; };
static int g_dump_state = 0; static GLuint g_dump_query = 0; static std::vector<Nv2aDumpDraw> g_dump_draws;
extern "C" void Nv2aDrawDumpFrame() {
    if (g_dump_state == 1) {
        g_dump_state = 0;
        Nv2aGlLog("draw dump: %zu draws in frame %llu", g_dump_draws.size(), g_nv2a_present_epoch);
        unsigned i = 0;
        for (auto& d : g_dump_draws) {
            GLint samples = -1;
            if (d.query) { GLuint r = 0; glGetQueryObjectuiv(d.query, GL_QUERY_RESULT, &r); samples = (GLint)r; glDeleteQueries(1, &d.query); }
            char tex[200] = ""; int n = 0;
            for (int t = 0; t < 4; ++t) if (d.texctl0[t] & NV_PGRAPH_TEXCTL0_0_ENABLE)
                n += snprintf(tex + n, sizeof tex - n, " t%d fmt %08x @%x ctl %08x", t, d.texfmt[t], d.texoff[t], d.texctl0[t]);
            if (samples == 0 && d.count >= 100) Nv2aGlDumpProgram(d.program, "zero-sample draw");
            else if (samples > 0 && d.count >= 100) Nv2aGlDumpProgram(d.program, "visible draw");
            Nv2aGlLog("dump %u: samples %d prim %u n %u prog %u csv0d %08x | c0 %08x c1 %08x c2 %08x blend %08x raster %08x comb %08x shprog %08x | surf %x zeta %x |%s",
                i++, samples, d.prim, d.count, d.program, d.csv0d, d.c0, d.c1, d.c2, d.blend, d.raster, d.combctl, d.shaderprog, d.surf, d.zeta, tex);
            Nv2aGlLog("dump %u attrs:%s", i - 1, d.attrs);
        }
        g_dump_draws.clear();
    }
    { static const unsigned long long at = getenv("GH_DRAW_DUMP_FRAME") ? strtoull(getenv("GH_DRAW_DUMP_FRAME"), nullptr, 10) : 0;   // test trigger
      if (at && g_nv2a_present_epoch == at) InterlockedExchange(&g_draw_dump_request, 1); }
    if (InterlockedExchange(&g_draw_dump_request, 0)) { g_dump_state = 1; g_dump_draws.clear(); Nv2aGlLog("draw dump: recording frame %llu", g_nv2a_present_epoch + 1); }
}
static void Nv2aDumpBegin(PGRAPHState* pg) {
    g_dump_query = 0;
    if (g_dump_state != 1 || pg->zpass_pixel_count_enable || g_dump_draws.size() >= 4000) return;
    glGenQueries(1, &g_dump_query); glBeginQuery(GL_SAMPLES_PASSED, g_dump_query);
}
static void Nv2aDumpEnd(PGRAPHState* pg) {
    if (g_dump_state != 1 || g_dump_draws.size() >= 4000) return;
    if (g_dump_query) glEndQuery(GL_SAMPLES_PASSED);
    Nv2aDumpDraw d{}; d.query = g_dump_query; g_dump_query = 0;
    d.prim = pg->primitive_mode;
    if (pg->draw_arrays_length) for (unsigned k = 0; k < pg->draw_arrays_length; ++k) d.count += pg->gl_draw_arrays_count[k];
    else d.count = pg->inline_elements_length ? pg->inline_elements_length : pg->inline_buffer_length ? pg->inline_buffer_length : pg->inline_array_length;
    d.program = pg->shader_binding ? pg->shader_binding->gl_program : 0;
    d.csv0d = pg->regs[NV_PGRAPH_CSV0_D]; d.c0 = pg->regs[NV_PGRAPH_CONTROL_0]; d.c1 = pg->regs[NV_PGRAPH_CONTROL_1]; d.c2 = pg->regs[NV_PGRAPH_CONTROL_2];
    d.blend = pg->regs[NV_PGRAPH_BLEND]; d.raster = pg->regs[NV_PGRAPH_SETUPRASTER]; d.combctl = pg->regs[NV_PGRAPH_COMBINECTL]; d.shaderprog = pg->regs[NV_PGRAPH_SHADERPROG];
    d.surf = pg->surface_color.bound_valid ? pg->surface_color.bound_addr : 0; d.zeta = pg->surface_zeta.bound_valid ? pg->surface_zeta.bound_addr : 0;
    for (int t = 0; t < 4; ++t) { d.texctl0[t] = pg->regs[NV_PGRAPH_TEXCTL0_0 + t*4]; d.texfmt[t] = pg->regs[NV_PGRAPH_TEXFMT0 + t*4]; d.texoff[t] = pg->regs[NV_PGRAPH_TEXOFFSET0 + t*4]; }
    { int n = 0; for (int a = 0; a < NV2A_VERTEXSHADER_ATTRIBUTES && n < (int)sizeof d.attrs - 40; ++a) { const VertexAttribute& va = pg->vertex_attributes[a];
        if (va.count) n += snprintf(d.attrs + n, sizeof d.attrs - n, " v%d:t%u c%u s%u @%x%s", a, va.format, va.count, va.stride, va.offset, va.dma_select ? "B" : "");
        else if (va.inline_value[0] || va.inline_value[1] || va.inline_value[2]) n += snprintf(d.attrs + n, sizeof d.attrs - n, " v%d:inline(%g,%g,%g,%g)", a, va.inline_value[0], va.inline_value[1], va.inline_value[2], va.inline_value[3]); } }
    g_dump_draws.push_back(d);
}

void OpenGL_draw_arrays(NV2AState *d)
{
	PGRAPHState *pg = &d->pgraph;
    pgraph_require_swath_raster_semantics(pg);
    pgraph_require_depth_control_semantics(pg);

	assert(pg->opengl_enabled);
	assert(pg->shader_binding);

	pgraph_bind_vertex_attributes(d, pg->draw_arrays_max_count,
		false, 0);
	Nv2aProf gldraw_scope(19);
    if (!Nv2aLegacyState() && pg->draw_arrays_length == 1) {
        glDrawArrays(pg->shader_binding->gl_primitive_mode, pg->gl_draw_arrays_start[0], pg->gl_draw_arrays_count[0]);
    } else {
	glMultiDrawArrays(pg->shader_binding->gl_primitive_mode,
		pg->gl_draw_arrays_start,
		pg->gl_draw_arrays_count,
		pg->draw_arrays_length);
    }

	OpenGL_draw_end(d);
}

void OpenGL_draw_inline_buffer(NV2AState *d)
{
	PGRAPHState *pg = &d->pgraph;
    pgraph_require_swath_raster_semantics(pg);
    pgraph_require_depth_control_semantics(pg);

	assert(pg->opengl_enabled);
	assert(pg->shader_binding);

	for (unsigned int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
		VertexAttribute *vertex_attribute = &pg->vertex_attributes[i];

		if (vertex_attribute->inline_buffer) {

			Nv2aBindArrayBuffer(
				vertex_attribute->gl_inline_buffer);
			glBufferData(GL_ARRAY_BUFFER,
				pg->inline_buffer_length
				* sizeof(float) * 4,
				vertex_attribute->inline_buffer,
				GL_DYNAMIC_DRAW);

			/* Clear buffer for next batch */
			if (Nv2aLegacyState()) { g_free(vertex_attribute->inline_buffer_storage); vertex_attribute->inline_buffer_storage = nullptr; }
			vertex_attribute->inline_buffer = NULL;

			Nv2aAttribPointer(i, vertex_attribute->gl_inline_buffer, 4, GL_FLOAT, GL_FALSE, 0, 0);
			Nv2aAttribEnable(i, true);
		}
		else {
			Nv2aAttribEnable(i, false);
			Nv2aAttribValue(i, vertex_attribute->inline_value);
		}
	}

	Nv2aProf gldraw_scope(19);
	glDrawArrays(pg->shader_binding->gl_primitive_mode,
		0, pg->inline_buffer_length);

	OpenGL_draw_end(d);
}

void OpenGL_draw_inline_array(NV2AState *d)
{
	PGRAPHState *pg = &d->pgraph;
    pgraph_require_swath_raster_semantics(pg);
    pgraph_require_depth_control_semantics(pg);

	assert(pg->opengl_enabled);
	assert(pg->shader_binding);

	unsigned int index_count = pgraph_bind_inline_array(d);
	Nv2aProf gldraw_scope(19);
	glDrawArrays(pg->shader_binding->gl_primitive_mode,
		0, index_count);

	OpenGL_draw_end(d);
}

void OpenGL_draw_inline_elements(NV2AState *d)
{
	PGRAPHState *pg = &d->pgraph;
    pgraph_require_swath_raster_semantics(pg);
    pgraph_require_depth_control_semantics(pg);

	assert(pg->opengl_enabled);
	assert(pg->shader_binding);

	uint32_t max_element = 0;
	uint32_t min_element = (uint32_t)-1;
	for (unsigned int i = 0; i < pg->inline_elements_length; i++) {
		max_element = MAX(pg->inline_elements[i], max_element);
		min_element = MIN(pg->inline_elements[i], min_element);
	}
	pgraph_bind_vertex_attributes(d, max_element + 1, false, 0);

	// Native port: indices stream through a persistently mapped, coherent ring
	// (GL 4.4 buffer storage): written directly, no map/unmap per draw (that
	// round trip was ~30% of the GPU thread in draw-heavy scenes, e.g. 2,500
	// small draws per frame during ghoulie death effects). The ring has two
	// halves; entering a half waits for the fence placed when it was last left.
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, pg->gl_element_buffer);
	static const GLsizeiptr ring_size = 8 << 20, half_size = ring_size / 2;
	static uint8_t* ring_ptr = nullptr;
	static GLsizeiptr ring_offset = 0;
	static GLsync half_fence[2] = {};
	if (!ring_ptr) {
		const GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
		glBufferStorage(GL_ELEMENT_ARRAY_BUFFER, ring_size, nullptr, flags);
		ring_ptr = (uint8_t*)glMapBufferRange(GL_ELEMENT_ARRAY_BUFFER, 0, ring_size, flags);
		if (!ring_ptr) Nv2aGlUnknown("cannot map the persistent index ring (GL %#x)", glGetError());
	}
	GLsizeiptr index_bytes = ((GLsizeiptr)(pg->inline_elements_length * sizeof(pg->inline_elements[0])) + 3) & ~(GLsizeiptr)3;
	if (index_bytes > half_size) Nv2aGlUnknown("inline element batch of %u indices exceeds the index ring", pg->inline_elements_length);
	// At either exact half end the previous allocation still belongs to
	// the half just filled. Fence it before entering the other half.
	int half = ring_offset ? (int)((ring_offset - 1) / half_size) : 0;
	if (ring_offset - half * half_size + index_bytes > half_size) {   // move to the start of the next half
		int next = (half + 1) & 1;
		half_fence[half] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
		if (half_fence[next]) {
			for (;;) {
				GLenum r = glClientWaitSync(half_fence[next], GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
				if (r == GL_ALREADY_SIGNALED || r == GL_CONDITION_SATISFIED) break;
				if (r != GL_TIMEOUT_EXPIRED) Nv2aGlUnknown("index ring fence wait failed (%#x)", r);
			}
			glDeleteSync(half_fence[next]); half_fence[next] = nullptr;
		}
		ring_offset = next * half_size;
	}
	memcpy(ring_ptr + ring_offset, pg->inline_elements, pg->inline_elements_length * sizeof(pg->inline_elements[0]));
	GLsizeiptr index_offset = ring_offset;
	ring_offset += index_bytes;

	Nv2aProf gldraw_scope(19);
	glDrawRangeElements(pg->shader_binding->gl_primitive_mode,
		min_element, max_element,
		pg->inline_elements_length,
		GL_UNSIGNED_SHORT, // Cxbx-Reloaded TODO : Restore GL_UNSIGNED_INT once HLE_draw_inline_elements can draw using uint32_t
		(void*)(uintptr_t)index_offset);

	OpenGL_draw_end(d);
}

static GLfloat g_line_limits[2] = {};
static GLfloat g_line_width = -1.0f;
static void pgraph_apply_line_width(PGRAPHState *pg)
{
    uint32_t raster = pg->regs[NV_PGRAPH_SETUPRASTER];
    bool lines = (pg->primitive_mode >= PRIM_TYPE_LINES && pg->primitive_mode <= PRIM_TYPE_LINE_STRIP) ||
        GET_MASK(raster, NV_PGRAPH_SETUPRASTER_FRONTFACEMODE) == POLY_MODE_LINE ||
        GET_MASK(raster, NV_PGRAPH_SETUPRASTER_BACKFACEMODE) == POLY_MODE_LINE;
    if (!lines) return; // line width has no effect on filled polygons or points
    unsigned int width = GET_MASK(raster, 0x001ff000u);
    // Zero is a valid latched NV20 state, but its rasterization isn't established
    // here. Defer that explicit failure until a draw actually consumes it.
    if (!width) Nv2aGlUnknown("zero-width line rasterization not provided");
    GLfloat limits[2];
    if (Nv2aLegacyState()) {
        glGetFloatv(glIsEnabled(GL_LINE_SMOOTH) ? GL_SMOOTH_LINE_WIDTH_RANGE : GL_ALIASED_LINE_WIDTH_RANGE, limits);
    } else {
        // Line smoothing is explicitly unsupported. The aliased capability is
        // invariant for this context; querying it per line stalls the driver.
        limits[0] = g_line_limits[0]; limits[1] = g_line_limits[1];
    }
    float pixels = width / 8.0f; // nxdk's NV20 line-width encoding
    if (pixels < limits[0])
        Nv2aGlUnknown("line width %g below host range %g..%g", pixels, limits[0], limits[1]);
    pg->line_width_px = pixels;
    pg->wide_lines = pixels * g_render_scale > limits[1];   // expanded by the geometry shader
    if (pg->wide_lines && !(pg->primitive_mode >= PRIM_TYPE_LINES && pg->primitive_mode <= PRIM_TYPE_LINE_STRIP))
        Nv2aGlUnknown("polygon line mode wider than the host range (%g px) not provided", pixels);
    if (!pg->wide_lines && (Nv2aLegacyState() || g_line_width != pixels * g_render_scale)) {
        glLineWidth(pixels * g_render_scale); g_line_width = pixels * g_render_scale;
    }
    pgraph_debug_check_gl(__LINE__);   // per-frame check otherwise
}

// Material/effect changes need only the fixed-state groups whose inputs
// changed. Clears invalidate all groups through g_fixed_gl_state_valid, and
// temporary helpers restore their GL state. No ownership/visibility change.
static unsigned long long g_fixed_group_calls=0, g_fixed_group_skips=0;
static bool Nv2aLegacyFixedState() {
    static const bool legacy=[] {
        char v[8]={}; DWORD n=GetEnvironmentVariableA("GH_GPU_LEGACY_FIXED_STATE",v,sizeof v);
        if(n && (n!=1 || v[0]!='1')) Nv2aGlUnknown("GH_GPU_LEGACY_FIXED_STATE must be 1 or unset");
        return n!=0;
    }();
    return legacy;
}
static void Nv2aApplyFixedState(PGRAPHState* pg, bool delta) {
    uint32_t control_0=pg->regs[NV_PGRAPH_CONTROL_0], control_1=pg->regs[NV_PGRAPH_CONTROL_1];
    bool depth_test=!!(control_0 & NV_PGRAPH_CONTROL_0_ZENABLE);
    bool stencil_test=!!(control_1 & NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE);
	// Native port: the fixed-function GL state below is a pure function of
	// these registers; skip re-issuing it when they repeat (the clear path,
	// which changes masks/dither/scissor, invalidates the cache).
	struct FixedGlKey { uint32_t control_0, control_1, control_2, blend, blend_color, setupraster, zfactor, zbias; };
	FixedGlKey fixed_key = { control_0, control_1, pg->regs[NV_PGRAPH_CONTROL_2], pg->regs[NV_PGRAPH_BLEND], pg->regs[NV_PGRAPH_BLENDCOLOR],
	                         pg->regs[NV_PGRAPH_SETUPRASTER], pg->regs[NV_PGRAPH_ZOFFSETFACTOR], pg->regs[NV_PGRAPH_ZOFFSETBIAS] };
    if (!Nv2aLegacyState()) {
        fixed_key.control_0 &= NV_PGRAPH_CONTROL_0_ZENABLE | NV_PGRAPH_CONTROL_0_ZFUNC |
            NV_PGRAPH_CONTROL_0_DITHERENABLE | NV_PGRAPH_CONTROL_0_ZWRITEENABLE |
            NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE | NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE |
            NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE | NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE;
        fixed_key.setupraster &= NV_PGRAPH_SETUPRASTER_CULLENABLE | NV_PGRAPH_SETUPRASTER_CULLCTRL |
            NV_PGRAPH_SETUPRASTER_FRONTFACE | NV_PGRAPH_SETUPRASTER_POFFSETFILLENABLE |
            NV_PGRAPH_SETUPRASTER_POFFSETLINEENABLE | NV_PGRAPH_SETUPRASTER_POFFSETPOINTENABLE;
    }
	static FixedGlKey last_fixed_key;
    const FixedGlKey previous = last_fixed_key;
    const bool force = !g_fixed_gl_state_valid || !delta;
	if (!g_fixed_gl_state_valid || memcmp(&fixed_key, &last_fixed_key, sizeof fixed_key)) {
	last_fixed_key = fixed_key;
	g_fixed_gl_state_valid = true;
	bool alpha = control_0 & NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE;
	bool red = control_0 & NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE;
	bool green = control_0 & NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE;
	bool blue = control_0 & NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE;
    const uint32_t color_mask = NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE |
        NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE | NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE | NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE;
    if (force || ((fixed_key.control_0 ^ previous.control_0) & color_mask)) { ++g_fixed_group_calls;
        glColorMask(red, green, blue, alpha); } else ++g_fixed_group_skips;
    if (force || ((fixed_key.control_0 ^ previous.control_0) & NV_PGRAPH_CONTROL_0_ZWRITEENABLE)) { ++g_fixed_group_calls;
        glDepthMask(!!(control_0 & NV_PGRAPH_CONTROL_0_ZWRITEENABLE)); } else ++g_fixed_group_skips;
    if (force || ((fixed_key.control_1 ^ previous.control_1) & NV_PGRAPH_CONTROL_1_STENCIL_MASK_WRITE)) { ++g_fixed_group_calls;
        glStencilMask(GET_MASK(control_1, NV_PGRAPH_CONTROL_1_STENCIL_MASK_WRITE)); } else ++g_fixed_group_skips;

    if (force || (fixed_key.blend != previous.blend || fixed_key.blend_color != previous.blend_color)) { ++g_fixed_group_calls;
	uint32_t blend = pg->regs[NV_PGRAPH_BLEND];
	if (blend & NV_PGRAPH_BLEND_EN) {
		glEnable(GL_BLEND);
		uint32_t sfactor = GET_MASK(blend,
			NV_PGRAPH_BLEND_SFACTOR);
		uint32_t dfactor = GET_MASK(blend,
			NV_PGRAPH_BLEND_DFACTOR);
		assert(sfactor < ARRAY_SIZE(pgraph_blend_factor_map));
		assert(dfactor < ARRAY_SIZE(pgraph_blend_factor_map));
		glBlendFunc(pgraph_blend_factor_map[sfactor],
			pgraph_blend_factor_map[dfactor]);

		uint32_t equation = GET_MASK(blend,
			NV_PGRAPH_BLEND_EQN);
		assert(equation < ARRAY_SIZE(pgraph_blend_equation_map));
		glBlendEquation(pgraph_blend_equation_map[equation]);

		uint32_t blend_color = pg->regs[NV_PGRAPH_BLENDCOLOR];
		glBlendColor(((blend_color >> 16) & 0xFF) / 255.0f, /* red */
			((blend_color >> 8) & 0xFF) / 255.0f,  /* green */
			(blend_color & 0xFF) / 255.0f,         /* blue */
			((blend_color >> 24) & 0xFF) / 255.0f);/* alpha */
	}
	else {
		glDisable(GL_BLEND);
	}

    } else ++g_fixed_group_skips;

	/* Face culling */
	uint32_t setupraster = pg->regs[NV_PGRAPH_SETUPRASTER];
    if (force || ((fixed_key.setupraster ^ previous.setupraster) & (NV_PGRAPH_SETUPRASTER_CULLENABLE | NV_PGRAPH_SETUPRASTER_CULLCTRL))) { ++g_fixed_group_calls;
	if (setupraster
		& NV_PGRAPH_SETUPRASTER_CULLENABLE) {
		uint32_t cull_face = GET_MASK(setupraster,
			NV_PGRAPH_SETUPRASTER_CULLCTRL);
		assert(cull_face < ARRAY_SIZE(pgraph_cull_face_map));
		glCullFace(pgraph_cull_face_map[cull_face]);
		glEnable(GL_CULL_FACE);
	}
	else {
		glDisable(GL_CULL_FACE);
	}

    } else ++g_fixed_group_skips;

	/* Front-face select */
    if (force || ((fixed_key.setupraster ^ previous.setupraster) & NV_PGRAPH_SETUPRASTER_FRONTFACE)) { ++g_fixed_group_calls;
	glFrontFace(setupraster
		& NV_PGRAPH_SETUPRASTER_FRONTFACE
		? GL_CCW : GL_CW);

    } else ++g_fixed_group_skips;

    if (force || ((fixed_key.setupraster ^ previous.setupraster) & (NV_PGRAPH_SETUPRASTER_POFFSETFILLENABLE | NV_PGRAPH_SETUPRASTER_POFFSETLINEENABLE | NV_PGRAPH_SETUPRASTER_POFFSETPOINTENABLE) || fixed_key.zfactor != previous.zfactor || fixed_key.zbias != previous.zbias)) { ++g_fixed_group_calls;
	/* Polygon offset */
	/* FIXME: GL implementation-specific, maybe do this in VS? */
	if (setupraster &
		NV_PGRAPH_SETUPRASTER_POFFSETFILLENABLE) {
		glEnable(GL_POLYGON_OFFSET_FILL);
	}
	else {
		glDisable(GL_POLYGON_OFFSET_FILL);
	}
	if (setupraster &
		NV_PGRAPH_SETUPRASTER_POFFSETLINEENABLE) {
		glEnable(GL_POLYGON_OFFSET_LINE);
	}
	else {
		glDisable(GL_POLYGON_OFFSET_LINE);
	}
	if (setupraster &
		NV_PGRAPH_SETUPRASTER_POFFSETPOINTENABLE) {
		glEnable(GL_POLYGON_OFFSET_POINT);
	}
	else {
		glDisable(GL_POLYGON_OFFSET_POINT);
	}
	if (setupraster &
		(NV_PGRAPH_SETUPRASTER_POFFSETFILLENABLE |
			NV_PGRAPH_SETUPRASTER_POFFSETLINEENABLE |
			NV_PGRAPH_SETUPRASTER_POFFSETPOINTENABLE)) {
		GLfloat zfactor = *(float*)&pg->regs[NV_PGRAPH_ZOFFSETFACTOR];
		GLfloat zbias = *(float*)&pg->regs[NV_PGRAPH_ZOFFSETBIAS];
		glPolygonOffset(zfactor, zbias);
	}

    } else ++g_fixed_group_skips;

    if (force || ((fixed_key.control_0 ^ previous.control_0) & (NV_PGRAPH_CONTROL_0_ZENABLE | NV_PGRAPH_CONTROL_0_ZFUNC))) { ++g_fixed_group_calls;
	/* Depth testing */
	if (depth_test) {
		glEnable(GL_DEPTH_TEST);

		uint32_t depth_func = GET_MASK(control_0,
			NV_PGRAPH_CONTROL_0_ZFUNC);
		assert(depth_func < ARRAY_SIZE(pgraph_depth_func_map));
		glDepthFunc(pgraph_depth_func_map[depth_func]);
	}
	else {
		glDisable(GL_DEPTH_TEST);
	}

    } else ++g_fixed_group_skips;

    if (force || ((fixed_key.control_1 ^ previous.control_1) & (NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE | NV_PGRAPH_CONTROL_1_STENCIL_FUNC | NV_PGRAPH_CONTROL_1_STENCIL_REF | NV_PGRAPH_CONTROL_1_STENCIL_MASK_READ) || fixed_key.control_2 != previous.control_2)) { ++g_fixed_group_calls;
	if (stencil_test) {
		glEnable(GL_STENCIL_TEST);

		uint32_t stencil_func = GET_MASK(control_1,
			NV_PGRAPH_CONTROL_1_STENCIL_FUNC);
		uint32_t stencil_ref = GET_MASK(control_1,
			NV_PGRAPH_CONTROL_1_STENCIL_REF);
		uint32_t func_mask = GET_MASK(control_1,
			NV_PGRAPH_CONTROL_1_STENCIL_MASK_READ);
		uint32_t control2 = pg->regs[NV_PGRAPH_CONTROL_2];
		uint32_t op_fail = GET_MASK(control2,
			NV_PGRAPH_CONTROL_2_STENCIL_OP_FAIL);
		uint32_t op_zfail = GET_MASK(control2,
			NV_PGRAPH_CONTROL_2_STENCIL_OP_ZFAIL);
		uint32_t op_zpass = GET_MASK(control2,
			NV_PGRAPH_CONTROL_2_STENCIL_OP_ZPASS);

		assert(stencil_func < ARRAY_SIZE(pgraph_stencil_func_map));
		assert(op_fail < ARRAY_SIZE(pgraph_stencil_op_map));
		assert(op_zfail < ARRAY_SIZE(pgraph_stencil_op_map));
		assert(op_zpass < ARRAY_SIZE(pgraph_stencil_op_map));

		glStencilFunc(
			pgraph_stencil_func_map[stencil_func],
			stencil_ref,
			func_mask);

		glStencilOp(
			pgraph_stencil_op_map[op_fail],
			pgraph_stencil_op_map[op_zfail],
			pgraph_stencil_op_map[op_zpass]);

	}
	else {
		glDisable(GL_STENCIL_TEST);
	}

    } else ++g_fixed_group_skips;

    if (force || ((fixed_key.control_0 ^ previous.control_0) & NV_PGRAPH_CONTROL_0_DITHERENABLE)) { ++g_fixed_group_calls;
	/* Dither */
	/* FIXME: GL implementation dependent */
	if (control_0 &
		NV_PGRAPH_CONTROL_0_DITHERENABLE) {
		glEnable(GL_DITHER);
	}
	else {
		glDisable(GL_DITHER);
	}
    } else ++g_fixed_group_skips;

	}
}

void OpenGL_draw_state_update(NV2AState *d)
{
    Nv2aProf prof_scope(3);
    // guest writes are harvested per DMA_PUT (gpu.inc RunPusher)
	PGRAPHState *pg = &d->pgraph;
    pgraph_require_swath_raster_semantics(pg);
    pgraph_require_depth_control_semantics(pg);
    pgraph_apply_line_width(pg);
    if (!(pg->regs[NV_PGRAPH_CONTROL_3] & 0x80)) Nv2aGlUnknown("flat-shaded drawing not provided");

	assert(pg->opengl_enabled);

	NV2A_GL_DGROUP_BEGIN("NV097_SET_BEGIN_END: 0x%x", pg->primitive_mode);

	uint32_t control_0 = pg->regs[NV_PGRAPH_CONTROL_0];
	uint32_t control_1 = pg->regs[NV_PGRAPH_CONTROL_1];

	bool depth_test = control_0
		& NV_PGRAPH_CONTROL_0_ZENABLE;
	bool stencil_test = control_1
		& NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE;

	pgraph_update_surface(d, true, true, depth_test || stencil_test);

    Nv2aApplyFixedState(pg, !Nv2aLegacyFixedState());
	pgraph_bind_shaders(pg);
	pgraph_bind_textures(d);
	// Rect (texel-addressed) stages divide by the GL size; render-scaled copies are larger.
	for (int t = 0; t < NV2A_MAX_TEXTURES; ++t) {
		GLint loc = pg->shader_binding->tex_scale_loc[t];
		if (loc >= 0) Nv2aUniform1f(pg->shader_binding, 78 + t, loc, pg->texture_binding[t] ? (float)pg->texture_binding[t]->scale : 1.0f);
	}

	//glDisableVertexAttribArray(NV2A_VERTEX_ATTR_DIFFUSE);
	//glVertexAttrib4f(NV2A_VERTEX_ATTR_DIFFUSE, 1.0f, 1.0f, 1.0f, 1.0f);

	unsigned int width, height;
	pgraph_get_surface_dimensions(pg, &width, &height);
	// Native port: drawing is limited to the surface clip rectangle.
	{
		unsigned int cx = pg->surface_shape.clip_x, cy = height - (pg->surface_shape.clip_y + pg->surface_shape.clip_height);
		unsigned int cw = pg->surface_shape.clip_width, ch = pg->surface_shape.clip_height;
		if (pg->surface_type == NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE) { cx = 0; cy = 0; cw = width; ch = height; }
		pgraph_apply_anti_aliasing_factor(pg, &cx, &cy);
		pgraph_apply_anti_aliasing_factor(pg, &cw, &ch);
		pgraph_apply_anti_aliasing_factor(pg, &width, &height);
		static unsigned int last_rect[6]; unsigned int rect[6] = {cx, cy, cw, ch, width, height};
		if (!g_scissor_viewport_valid || memcmp(rect, last_rect, sizeof rect)) {
			memcpy(last_rect, rect, sizeof rect); g_scissor_viewport_valid = true;
			glEnable(GL_SCISSOR_TEST);
			glScissor(cx * g_render_scale, cy * g_render_scale, cw * g_render_scale, ch * g_render_scale);
			glViewport(0, 0, width * g_render_scale, height * g_render_scale);
		}
	}
	if (Nv2aGlTraceEnabled() && pg->surface_type == NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE) { static int logged = 0; if (logged < 12) { logged++;
		Nv2aGlLog("RT draw: color %#x prim %u control0 %#x control1 %#x control2 %#x blend %#x setupraster %#x csv0_d %#x shaderprog %#x combinectl %#x zeta fmt %u zeta off %#x program %u",
			pg->surface_color.offset, pg->primitive_mode, pg->regs[NV_PGRAPH_CONTROL_0], pg->regs[NV_PGRAPH_CONTROL_1], pg->regs[NV_PGRAPH_CONTROL_2],
			pg->regs[NV_PGRAPH_BLEND], pg->regs[NV_PGRAPH_SETUPRASTER], pg->regs[NV_PGRAPH_CSV0_D], pg->regs[NV_PGRAPH_SHADERPROG], pg->regs[NV_PGRAPH_COMBINECTL],
			pg->surface_shape.zeta_format, pg->surface_zeta.offset, pg->shader_binding ? pg->shader_binding->gl_program : 0);
		Nv2aGlLog("  factor0 %08x %08x %08x %08x factor1 %08x %08x %08x %08x tex0 fmt %#x off %#x tex1 fmt %#x off %#x tex2 fmt %#x off %#x",
			pg->regs[NV_PGRAPH_COMBINEFACTOR0], pg->regs[NV_PGRAPH_COMBINEFACTOR0 + 4], pg->regs[NV_PGRAPH_COMBINEFACTOR0 + 8], pg->regs[NV_PGRAPH_COMBINEFACTOR0 + 12],
			pg->regs[NV_PGRAPH_COMBINEFACTOR1], pg->regs[NV_PGRAPH_COMBINEFACTOR1 + 4], pg->regs[NV_PGRAPH_COMBINEFACTOR1 + 8], pg->regs[NV_PGRAPH_COMBINEFACTOR1 + 12],
			pg->regs[NV_PGRAPH_TEXFMT0], pg->regs[NV_PGRAPH_TEXOFFSET0], pg->regs[NV_PGRAPH_TEXFMT0 + 4], pg->regs[NV_PGRAPH_TEXOFFSET0 + 4],
			pg->regs[NV_PGRAPH_TEXFMT0 + 8], pg->regs[NV_PGRAPH_TEXOFFSET0 + 8]);
		for (int u = 0; u < 3; u++) {
			if (!pg->texture_binding[u]) continue;
			Nv2aForeignActiveTexture(GL_TEXTURE0 + u);
			GLint w = 0, h = 0, base = 0, maxl = 0, minf = 0;
			GLenum tgt = pg->texture_binding[u]->gl_target;
			glGetTexParameteriv(tgt, GL_TEXTURE_BASE_LEVEL, &base); glGetTexParameteriv(tgt, GL_TEXTURE_MAX_LEVEL, &maxl);
			glGetTexParameteriv(tgt, GL_TEXTURE_MIN_FILTER, &minf);
			glGetTexLevelParameteriv(tgt, base, GL_TEXTURE_WIDTH, &w); glGetTexLevelParameteriv(tgt, base, GL_TEXTURE_HEIGHT, &h);
			double mean[4] = {0,0,0,0};
			if (w > 0 && h > 0 && w * h <= 1024 * 1024) {
				uint8_t* px = (uint8_t*)malloc(w * h * 4);
				glGetTexImage(tgt, base, GL_RGBA, GL_UNSIGNED_BYTE, px);
				for (int q = 0; q < w * h; q++) for (int c = 0; c < 4; c++) mean[c] += px[q * 4 + c];
				for (int c = 0; c < 4; c++) mean[c] /= (w * h);
				free(px);
			}
			Nv2aGlLog("  unit %d tex %u target %#x base %d max %d minfilter %#x %dx%d mean rgba %.1f %.1f %.1f %.1f", u, pg->texture_binding[u]->gl_texture, tgt, base, maxl, minf, w, h, mean[0], mean[1], mean[2], mean[3]);
		}
		Nv2aGlLog("  windowclip x0 %08x y0 %08x x1 %08x y1 %08x setupraster wct %d", pg->regs[NV_PGRAPH_WINDOWCLIPX0], pg->regs[NV_PGRAPH_WINDOWCLIPY0],
			pg->regs[NV_PGRAPH_WINDOWCLIPX0 + 4], pg->regs[NV_PGRAPH_WINDOWCLIPY0 + 4], (pg->regs[NV_PGRAPH_SETUPRASTER] >> 31) & 1); } }

	/* Visibility testing */
	if (pg->zpass_pixel_count_enable) {
		GLuint gl_query;
		glGenQueries(1, &gl_query);
		pg->gl_zpass_pixel_count_query_count++;
		pg->gl_zpass_pixel_count_queries = (GLuint*)g_realloc(
			pg->gl_zpass_pixel_count_queries,
			sizeof(GLuint) * pg->gl_zpass_pixel_count_query_count);
		pg->gl_zpass_pixel_count_queries[
			pg->gl_zpass_pixel_count_query_count - 1] = gl_query;
		glBeginQuery(GL_SAMPLES_PASSED, gl_query);
	}

    Nv2aDumpBegin(pg);
}

void OpenGL_draw_end(NV2AState *d)
{
    Nv2aProf prof_scope(4);
	PGRAPHState *pg = &d->pgraph;

	assert(pg->opengl_enabled);


    // Opt-in bounded RT readback diagnostics; no guest state is changed.
    if (Nv2aGlTraceEnabled() && pg->surface_type == NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE) {
        static unsigned traced = 0;
        if (traced++ < 12) {
            unsigned w, h; pgraph_get_surface_dimensions(pg, &w, &h);
            std::vector<uint8_t> pixels(w*h*4);
            glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            uint64_t sum[4] = {}; unsigned nz=0, xmin=w, ymin=h, xmax=0, ymax=0;
            for (unsigned y=0;y<h;y++) for (unsigned x=0;x<w;x++) {
                const uint8_t* p=&pixels[(y*w+x)*4];
                for (unsigned c=0;c<4;c++) sum[c]+=p[c];
                if (p[0] || p[1] || p[2] || p[3]) { nz++; xmin=MIN(xmin,x); ymin=MIN(ymin,y); xmax=MAX(xmax,x); ymax=MAX(ymax,y); }
            }
            Nv2aGlLog("RT result program %u %ux%u rgba %.3f %.3f %.3f %.3f nonzero %u bbox %u,%u..%u,%u", pg->shader_binding->gl_program,w,h,
                (double)sum[0]/(w*h),(double)sum[1]/(w*h),(double)sum[2]/(w*h),(double)sum[3]/(w*h),nz,xmin,ymin,xmax,ymax);
        }
    }

    Nv2aDumpEnd(pg);
	/* End of visibility testing */
	if (pg->zpass_pixel_count_enable) {
		glEndQuery(GL_SAMPLES_PASSED);
	}
    // Native port: glGetError synchronises with the driver, so draws are
    // checked individually only with GH_GPU_DEBUG=1; otherwise every frame
    // (pgraph_check_gl_errors at flip) still stops on any error.
    if (Nv2aGlDebugChecks()) { GLenum e = glGetError();
      if (e != GL_NO_ERROR) Nv2aGlUnknown("GL error %#x after draw (primitive %u, program %u)", e, pg->primitive_mode, pg->shader_binding ? pg->shader_binding->gl_program : 0); }

	NV2A_GL_DGROUP_END();
}

void OpenGL_draw_clear(NV2AState *d)
{
	g_fixed_gl_state_valid = false; g_scissor_viewport_valid = false;
    ++g_nv2a_render_serial;
    Nv2aProf prof_scope(5);
    // guest writes are harvested per DMA_PUT (gpu.inc RunPusher)
	PGRAPHState *pg = &d->pgraph;
    pgraph_require_swath_raster_semantics(pg);

	assert(pg->opengl_enabled);

	NV2A_DPRINTF("---------PRE CLEAR ------\n");
	GLbitfield gl_mask = 0;

	bool write_color = (pg->clear_surface & NV097_CLEAR_SURFACE_COLOR);
	bool write_zeta =
		(pg->clear_surface & (NV097_CLEAR_SURFACE_Z | NV097_CLEAR_SURFACE_STENCIL));

	if (write_zeta) {
		uint32_t clear_zstencil =
			d->pgraph.regs[NV_PGRAPH_ZSTENCILCLEARVALUE];
		GLint gl_clear_stencil;
		GLfloat gl_clear_depth;

		/* FIXME: Put these in some lookup table */
		const float f16_max = 511.9375f;
		/* FIXME: 7 bits of mantissa unused. maybe use full buffer? */
		const float f24_max = 3.4027977E38f;

		switch (pg->surface_shape.zeta_format) {
		case NV097_SET_SURFACE_FORMAT_ZETA_Z16: {
			if (pg->clear_surface & NV097_CLEAR_SURFACE_Z) {
				gl_mask |= GL_DEPTH_BUFFER_BIT;
				uint16_t z = clear_zstencil & 0xFFFF;
				if (pg->surface_shape.z_format) {
					gl_clear_depth = convert_f16_to_float(z) / f16_max;
					assert(false); /* FIXME: Untested */
				}
				else {
					gl_clear_depth = z / (float)0xFFFF;
				}
			}
			break;
		}
		case NV097_SET_SURFACE_FORMAT_ZETA_Z24S8: {
			if (pg->clear_surface & NV097_CLEAR_SURFACE_STENCIL) {
				gl_mask |= GL_STENCIL_BUFFER_BIT;
				gl_clear_stencil = clear_zstencil & 0xFF;
			}
			if (pg->clear_surface & NV097_CLEAR_SURFACE_Z) {
				gl_mask |= GL_DEPTH_BUFFER_BIT;
				uint32_t z = clear_zstencil >> 8;
				if (pg->surface_shape.z_format) {
					gl_clear_depth = convert_f24_to_float(z) / f24_max;
					assert(false); /* FIXME: Untested */
				}
				else {
					gl_clear_depth = z / (float)0xFFFFFF;
				}
			}
			break;
		}
		default:
			fprintf(stderr, "Unknown zeta surface format: 0x%x\n", pg->surface_shape.zeta_format);
			assert(false);
			break;
		}

		if (gl_mask & GL_DEPTH_BUFFER_BIT) {
			glDepthMask(GL_TRUE);
			glClearDepth(gl_clear_depth);
		}

		if (gl_mask & GL_STENCIL_BUFFER_BIT) {
			glStencilMask(0xff);
			glClearStencil(gl_clear_stencil);
		}
	}

	if (write_color) {
		gl_mask |= GL_COLOR_BUFFER_BIT;
		glColorMask((pg->clear_surface & NV097_CLEAR_SURFACE_R)
			? GL_TRUE : GL_FALSE,
			(pg->clear_surface & NV097_CLEAR_SURFACE_G)
			? GL_TRUE : GL_FALSE,
			(pg->clear_surface & NV097_CLEAR_SURFACE_B)
			? GL_TRUE : GL_FALSE,
			(pg->clear_surface & NV097_CLEAR_SURFACE_A)
			? GL_TRUE : GL_FALSE);
		uint32_t clear_color = d->pgraph.regs[NV_PGRAPH_COLORCLEARVALUE];

		/* Handle RGB */
		GLfloat red, green, blue;
		switch (pg->surface_shape.color_format) {
		case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5:
		case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_O1R5G5B5:
			red = ((clear_color >> 10) & 0x1F) / 31.0f;
			green = ((clear_color >> 5) & 0x1F) / 31.0f;
			blue = (clear_color & 0x1F) / 31.0f;
			assert(false); /* Untested */
			break;
		case NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5:
			red = ((clear_color >> 11) & 0x1F) / 31.0f;
			green = ((clear_color >> 5) & 0x3F) / 63.0f;
			blue = (clear_color & 0x1F) / 31.0f;
			break;
		case NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8:
		case NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_O8R8G8B8:
		case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1A7R8G8B8_Z1A7R8G8B8:
		case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1A7R8G8B8_O1A7R8G8B8:
		case NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8:
			red = ((clear_color >> 16) & 0xFF) / 255.0f;
			green = ((clear_color >> 8) & 0xFF) / 255.0f;
			blue = (clear_color & 0xFF) / 255.0f;
			break;
		case NV097_SET_SURFACE_FORMAT_COLOR_LE_B8:
		case NV097_SET_SURFACE_FORMAT_COLOR_LE_G8B8:
			/* Xbox D3D doesn't support clearing those */
		default:
			red = 1.0f;
			green = 0.0f;
			blue = 1.0f;
			fprintf(stderr, "CLEAR_SURFACE for color_format 0x%x unsupported",
				pg->surface_shape.color_format);
			assert(false);
			break;
		}

		/* Handle alpha */
		GLfloat alpha;
		switch (pg->surface_shape.color_format) {
			/* FIXME: CLEAR_SURFACE seems to work like memset, so maybe we
			*        also have to clear non-alpha bits with alpha value?
			*        As GL doesn't own those pixels we'd have to do this on
			*        our own in xbox memory.
			*/
		case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1A7R8G8B8_Z1A7R8G8B8:
		case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1A7R8G8B8_O1A7R8G8B8:
			alpha = ((clear_color >> 24) & 0x7F) / 127.0f;
			assert(false); /* Untested */
			break;
		case NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8:
			alpha = ((clear_color >> 24) & 0xFF) / 255.0f;
			break;
		default:
			alpha = 1.0f;
			break;
		}

		glClearColor(red, green, blue, alpha);
	}

	if (gl_mask) {
		pgraph_update_surface(d, true, write_color, write_zeta);

		glEnable(GL_SCISSOR_TEST);

		unsigned int xmin = GET_MASK(pg->regs[NV_PGRAPH_CLEARRECTX],
			NV_PGRAPH_CLEARRECTX_XMIN);
		unsigned int xmax = GET_MASK(pg->regs[NV_PGRAPH_CLEARRECTX],
			NV_PGRAPH_CLEARRECTX_XMAX);
		unsigned int ymin = GET_MASK(pg->regs[NV_PGRAPH_CLEARRECTY],
			NV_PGRAPH_CLEARRECTY_YMIN);
		unsigned int ymax = GET_MASK(pg->regs[NV_PGRAPH_CLEARRECTY],
			NV_PGRAPH_CLEARRECTY_YMAX);

		unsigned int scissor_x = xmin;
		unsigned int clear_w, clear_h;
		pgraph_get_surface_dimensions(pg, &clear_w, &clear_h);
		unsigned int scissor_y = clear_h - ymax - 1;

		unsigned int scissor_width = xmax - xmin + 1;
		unsigned int scissor_height = ymax - ymin + 1;

		pgraph_apply_anti_aliasing_factor(pg, &scissor_x, &scissor_y);
		pgraph_apply_anti_aliasing_factor(pg, &scissor_width, &scissor_height);

		/* FIXME: Should this really be inverted instead of ymin? */
		glScissor(scissor_x * g_render_scale, scissor_y * g_render_scale, scissor_width * g_render_scale, scissor_height * g_render_scale);

		/* FIXME: Respect window clip?!?! */

		NV2A_DPRINTF("------------------CLEAR 0x%x %d,%d - %d,%d  %x---------------\n",
			parameter, xmin, ymin, xmax, ymax, d->pgraph.regs[NV_PGRAPH_COLORCLEARVALUE]);

		/* Dither */
		/* FIXME: Maybe also disable it here? + GL implementation dependent */
		if (pg->regs[NV_PGRAPH_CONTROL_0] &
			NV_PGRAPH_CONTROL_0_DITHERENABLE) {
			glEnable(GL_DITHER);
		}
		else {
			glDisable(GL_DITHER);
		}

		glClear(gl_mask);

		glDisable(GL_SCISSOR_TEST);
	}

	pgraph_set_surface_dirty(pg, write_color, write_zeta);
}

void OpenGL_init_pgraph_plugins()
{
	pgraph_draw_arrays = OpenGL_draw_arrays;
	pgraph_draw_inline_buffer = OpenGL_draw_inline_buffer;
	pgraph_draw_inline_array = OpenGL_draw_inline_array;
	pgraph_draw_inline_elements = OpenGL_draw_inline_elements;
	pgraph_draw_state_update = OpenGL_draw_state_update;
	pgraph_draw_clear = OpenGL_draw_clear;
}

static uint64_t g_readback_reasons[2048] = {};
static uint32_t g_current_method = 0;
// Native port: GPU 2D blit for the title's per-frame full-frame copy of the
// bound colour surface into another buffer (observed: back buffer 640x480
// -> 0x98c000 / 0x860000, A8R8G8B8). Copying on the CPU forced a full
// readback of the scaled surface every frame and re-uploaded the result as
// a 1x image. Here the copy stays in GL at render scale and the destination
// pages become a pending owned surface (any CPU access or texture/vertex
// fetch completes the download exactly as for a rendered surface). Other
// blits use the CPU path. GH_CPU_BLITS=1 forces the CPU path for comparison.
static unsigned long long g_gpu_blits = 0, g_cpu_blits = 0;
static bool Nv2aTryGpuBlit(NV2AState* d, uint32_t src, uint32_t dst, unsigned bpp) {
    PGRAPHState* pg = &d->pgraph;
    ImageBlitState* b = &pg->image_blit;
    ContextSurfaces2DState* cs = &pg->context_surfaces_2d;
    Surface* s = &pg->surface_color;
    static const bool disabled = getenv("GH_CPU_BLITS") != nullptr;
    if (disabled || g_owned_sync || !pg->opengl_enabled || bpp != 4 || cs->color_format != NV062_SET_COLOR_FORMAT_LE_A8R8G8B8) return false;
    if (b->in_x || b->in_y || b->out_x || b->out_y || !b->width || !b->height) return false;
    if (!pg->gl_color_buffer || !s->bound_valid || s->bound_swizzle || s->bound_fmt != GL_RGBA8 || s->buffer_dirty) return false;
    if (s->bound_addr != src || s->bound_pitch != cs->source_pitch || cs->dest_pitch != cs->source_pitch) return false;
    if (b->width > s->bound_w || b->height != s->bound_h || b->width * 4u > cs->dest_pitch) return false;
    const uint32_t len = cs->dest_pitch * b->height;
    if (dst < src + len && src < dst + len) return false;
    if (dst >= d->vram_size || len > d->vram_size - dst) return false;
    Surface* z = &pg->surface_zeta;
    if (z->bound_valid && dst < z->bound_addr + z->bound_pitch * z->bound_h && z->bound_addr < dst + len) return false;
    // The GL surface must still match guest memory (no CPU write to the source since).
    Nv2aGlHarvestRange(src, len);
    if (Nv2aGlRangeGeneration(src, len) > s->memory_gen) return false;

    struct BlitTarget { uint32_t addr, w, h, pitch; GLuint tex; };
    static std::vector<BlitTarget> targets;
    const unsigned scale = Nv2aGlRenderScale();
    BlitTarget* t = nullptr;
    for (auto& c : targets) if (c.addr == dst && c.w == b->width && c.h == b->height && c.pitch == cs->dest_pitch) t = &c;
    if (!t) {
        if (targets.size() >= 8) { Nv2aOwnedRetireTexture(targets[0].tex); Nv2aForeignDeleteTextures(1, &targets[0].tex); targets.erase(targets.begin()); }
        BlitTarget c{dst, b->width, b->height, cs->dest_pitch, 0};
        glGenTextures(1, &c.tex);
        Nv2aForeignBindTexture(GL_TEXTURE_2D, c.tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, b->width * scale, b->height * scale, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, nullptr);
        targets.push_back(c); t = &targets.back();
    }
    ++g_nv2a_render_serial;
    Nv2aOwnedLock();   // recursive; spans discard + copy + registration so no fault sees a gap
    Nv2aOwnedDiscardTextureLocked(t->tex, dst, len);
    static GLuint fbo[2] = {};
    if (!fbo[0]) glGenFramebuffers(2, fbo);
    GLint read = 0, draw = 0; glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read); glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST); if (scissor) glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo[0]); glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, pg->gl_color_buffer, 0);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo[1]); glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->tex, 0);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    // Both are stored as surfaces (rows flipped by the same height), so the copy is direct.
    glBlitFramebuffer(0, 0, b->width * scale, b->height * scale, 0, 0, b->width * scale, b->height * scale, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, read); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    if (Nv2aGlDebugChecks()) { GLenum e = glGetError(); if (e != GL_NO_ERROR) Nv2aGlUnknown("GPU blit failed: GL %#x", e); }
    Nv2aGlHarvestRange(dst, len);
    g_owned_register_surface_gen = Nv2aGlRangeGeneration(dst, len);   // the blit overwrites; not a conflict
    Nv2aOwnedRegister(dst, len, b->width, b->height, cs->dest_pitch, 4, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, false, t->tex);
    Nv2aOwnedUnlock();
    if (++g_gpu_blits % 3000 == 0) Nv2aGlLog("2D blits: %llu on GPU, %llu on CPU", g_gpu_blits, g_cpu_blits);
    return true;
}

void pgraph_handle_method(NV2AState *d,
							unsigned int subchannel,
							unsigned int method,
							uint32_t parameter)
{
    g_current_method = method;
	unsigned int i;
	unsigned int slot;

    PGRAPHState *pg = &d->pgraph;

    bool channel_valid =
        d->pgraph.regs[NV_PGRAPH_CTX_CONTROL] & NV_PGRAPH_CTX_CONTROL_CHID;
    assert(channel_valid);

    unsigned channel_id = GET_MASK(pg->regs[NV_PGRAPH_CTX_USER], NV_PGRAPH_CTX_USER_CHID);

	ContextSurfaces2DState *context_surfaces_2d = &pg->context_surfaces_2d;
	ImageBlitState *image_blit = &pg->image_blit;
	KelvinState *kelvin = &pg->kelvin;

    assert(subchannel < 8);

	if (method == NV_SET_OBJECT) {
        assert(parameter < d->pramin.ramin_size);
        uint8_t *obj_ptr = d->pramin.ramin_ptr + parameter;

        uint32_t ctx_1 = ldl_le_p((uint32_t*)obj_ptr);
        uint32_t ctx_2 = ldl_le_p((uint32_t*)(obj_ptr+4));
        uint32_t ctx_3 = ldl_le_p((uint32_t*)(obj_ptr+8));
        uint32_t ctx_4 = ldl_le_p((uint32_t*)(obj_ptr+12));
        uint32_t ctx_5 = parameter;

        pg->regs[NV_PGRAPH_CTX_CACHE1 + subchannel * 4] = ctx_1;
        pg->regs[NV_PGRAPH_CTX_CACHE2 + subchannel * 4] = ctx_2;
        pg->regs[NV_PGRAPH_CTX_CACHE3 + subchannel * 4] = ctx_3;
        pg->regs[NV_PGRAPH_CTX_CACHE4 + subchannel * 4] = ctx_4;
        pg->regs[NV_PGRAPH_CTX_CACHE5 + subchannel * 4] = ctx_5;
    }

    // is this right?
    pg->regs[NV_PGRAPH_CTX_SWITCH1] = pg->regs[NV_PGRAPH_CTX_CACHE1 + subchannel * 4];
    pg->regs[NV_PGRAPH_CTX_SWITCH2] = pg->regs[NV_PGRAPH_CTX_CACHE2 + subchannel * 4];
    pg->regs[NV_PGRAPH_CTX_SWITCH3] = pg->regs[NV_PGRAPH_CTX_CACHE3 + subchannel * 4];
    pg->regs[NV_PGRAPH_CTX_SWITCH4] = pg->regs[NV_PGRAPH_CTX_CACHE4 + subchannel * 4];
    pg->regs[NV_PGRAPH_CTX_SWITCH5] = pg->regs[NV_PGRAPH_CTX_CACHE5 + subchannel * 4];

    uint32_t graphics_class = GET_MASK(pg->regs[NV_PGRAPH_CTX_SWITCH1],
                                       NV_PGRAPH_CTX_SWITCH1_GRCLASS);

	// Logging is slow.. disable for now..
	//pgraph_log_method(subchannel, graphics_class, method, parameter);

    if (subchannel != 0) {
        // catches context switching issues on xbox d3d
        assert(graphics_class != 0x97);
    }

    /* ugly switch for now */
    switch (graphics_class) {

    case NV_MEMORY_TO_MEMORY_FORMAT:
        // Method 0 has already loaded the object's four PRAMIN context words
        // into CTX_CACHE/SWITCH above. NV0039 DMA setup layout: envytools
        // rnndb/graph/nv_m2mf.xml. Binding/setup is distinct from a transfer.
        switch (method) {
        case NV_SET_OBJECT:
            if (pg->m2mf_object && pg->m2mf_object != parameter)
                Nv2aGlUnknown("multiple M2MF object contexts not provided");
            pg->m2mf_object = parameter; break;
        case 0x180: pg->m2mf_dma_notify = parameter; break;
        case 0x184: pg->m2mf_dma_input = parameter; break;
        case 0x188: pg->m2mf_dma_output = parameter; break;
        default: Nv2aGlUnknown("M2MF method %#x parameter %#x not provided", method, parameter);
        }
        break;

    case NV_CONTEXT_PATTERN: {
		switch (method) {
        case NV_SET_OBJECT: break; // context words loaded above
		case NV044_SET_MONOCHROME_COLOR0:
			pg->regs[NV_PGRAPH_PATT_COLOR0] = parameter;
			break;
        default: Nv2aGlUnknown("pattern method %#x parameter %#x not provided", method, parameter);
		}
		
		break;
	}

    case NV_CONTEXT_SURFACES_2D: {
		switch (method) {
		case NV062_SET_OBJECT:
			context_surfaces_2d->object_instance = parameter;
			break;
		case NV062_SET_CONTEXT_DMA_IMAGE_SOURCE:
			context_surfaces_2d->dma_image_source = parameter;
			break;
		case NV062_SET_CONTEXT_DMA_IMAGE_DESTIN:
			context_surfaces_2d->dma_image_dest = parameter;
			break;
		case NV062_SET_COLOR_FORMAT:
			context_surfaces_2d->color_format = parameter;
			break;
		case NV062_SET_PITCH:
			context_surfaces_2d->source_pitch = parameter & 0xFFFF;
			context_surfaces_2d->dest_pitch = parameter >> 16;
			break;
		case NV062_SET_OFFSET_SOURCE:
			context_surfaces_2d->source_offset = parameter & 0x07FFFFFF;
			break;
		case NV062_SET_OFFSET_DESTIN:
			context_surfaces_2d->dest_offset = parameter & 0x07FFFFFF;
			break;
		default:
			Nv2aGlUnknown("Unknown NV_CONTEXT_SURFACES_2D Method: 0x%08X", method);
		}
	
		break; 
	}
	
	case NV_IMAGE_BLIT: {
		switch (method) {
		case NV09F_SET_OBJECT:
			image_blit->object_instance = parameter;
			break;
		case NV09F_SET_CONTEXT_SURFACES:
			image_blit->context_surfaces = parameter;
			break;
		case 0x184: case 0x188: case 0x18C: case 0x190: case 0x194: case 0x198:
			// Native port: retain bindings. SRCCOPY accepts unbound/NV01_NULL
			// colour-key and clip contexts; actual effects still fail below.
			image_blit->context_objects[(method - 0x184) / 4] = parameter;
			break;
		case NV09F_SET_OPERATION:
			image_blit->operation = parameter;
			break;
		case NV09F_CONTROL_POINT_IN:
			image_blit->in_x = parameter & 0xFFFF;
			image_blit->in_y = parameter >> 16;
			break;
		case NV09F_CONTROL_POINT_OUT:
			image_blit->out_x = parameter & 0xFFFF;
			image_blit->out_y = parameter >> 16;
			break;
		case NV09F_SIZE:
			image_blit->width = parameter & 0xFFFF;
			image_blit->height = parameter >> 16;

			/* I guess this kicks it off? */
			if (image_blit->operation == NV09F_SET_OPERATION_SRCCOPY) {

				NV2A_GL_DPRINTF(true, "NV09F_SET_OPERATION_SRCCOPY");


				ContextSurfaces2DState *context_surfaces = context_surfaces_2d;
				assert(context_surfaces->object_instance
					== image_blit->context_surfaces);
                // NV01_NULL (class 0x30) disables the context, just like the
                // unbound/reset instance. Envytools f102b823 hwtest/
                // pgraph_mthd_grobj.cc: nv04_pgraph_set_ctx clears the effect-enable bit
                // for this class; MthdCtxChroma/Clip select the key/clip bits.
                // Retail D3D binds this object after START (instance 0x14d80).
                for (unsigned i = 0; i < 2; ++i) {
                    uint32_t object = image_blit->context_objects[i];
                    if (!object) continue;
                    if ((object & 15) || object > d->pramin.ramin_size - 4)
                        Nv2aGlUnknown("blit context %#x invalid instance %#x", 0x184 + i*4, object);
                    uint32_t cls = ldl_le_p(reinterpret_cast<uint32_t*>(d->pramin.ramin_ptr + object)) & 0xfff;
                    if (cls != 0x30)
                        Nv2aGlUnknown("blit %s context class %#x instance %#x not provided", i ? "clip" : "colour key", cls, object);
                }

				unsigned int bytes_per_pixel;
				switch (context_surfaces->color_format) {
				case NV062_SET_COLOR_FORMAT_LE_Y8:
					bytes_per_pixel = 1;
					break;
				case NV062_SET_COLOR_FORMAT_LE_R5G6B5:
					bytes_per_pixel = 2;
					break;
				case NV062_SET_COLOR_FORMAT_LE_A8R8G8B8:
					bytes_per_pixel = 4;
					break;
				default:
					printf("Unknown blit surface format: 0x%x\n", context_surfaces->color_format);
					assert(false);
					break;
				}

				xbox::addr_xt source_dma_len, dest_dma_len;
				uint8_t *source, *dest;

				source = (uint8_t*)nv_dma_map(d, context_surfaces->dma_image_source,
												&source_dma_len);
				assert(context_surfaces->source_offset < source_dma_len);
				source += context_surfaces->source_offset;

				dest = (uint8_t*)nv_dma_map(d, context_surfaces->dma_image_dest,
												&dest_dma_len);
				assert(context_surfaces->dest_offset < dest_dma_len);
				dest += context_surfaces->dest_offset;

				NV2A_DPRINTF("  - 0x%tx -> 0x%tx\n", source - d->vram_ptr,
														dest - d->vram_ptr);
				{ // Diagnostics: each distinct blit shape once (cheap).
					struct BlitKey { uint32_t s, dd, sp, dp, w, h, fmt; bool operator<(const BlitKey& o) const { return memcmp(this, &o, sizeof *this) < 0; } };
					static std::set<BlitKey> seen;
					BlitKey k{ uint32_t(source - d->vram_ptr), uint32_t(dest - d->vram_ptr), context_surfaces->source_pitch, context_surfaces->dest_pitch,
					           image_blit->width, image_blit->height, context_surfaces->color_format };
					if (seen.size() < 300 && seen.insert(k).second)
						Nv2aGlLog("blit: %#x+(%u,%u) pitch %u -> %#x+(%u,%u) pitch %u, %ux%u fmt %#x; bound colour %#x zeta %#x",
							k.s, image_blit->in_x, image_blit->in_y, k.sp, k.dd, image_blit->out_x, image_blit->out_y, k.dp, k.w, k.h, k.fmt,
							pg->surface_color.bound_valid ? pg->surface_color.bound_addr : 0, pg->surface_zeta.bound_valid ? pg->surface_zeta.bound_addr : 0);
				}

				if (Nv2aTryGpuBlit(d, uint32_t(source - d->vram_ptr), uint32_t(dest - d->vram_ptr), bytes_per_pixel)) break;
				++g_cpu_blits;
				// Native port: the 2D engine reads and writes guest memory, which
				// is coherent with 3D rendering on hardware. Make the bound colour/
				// zeta surfaces' pending GL drawing visible first (deferred download
				// registration; the memmove below then faults it in). Otherwise the
				// blit read stale RAM and its write made the next surface upload
				// discard the GPU drawing (pause-menu page replaced by garbage).
				{ g_download_site = 0x9F; pgraph_update_surface(d, false, true, true); g_download_site = 0; }

				unsigned int y;
				for (y = 0; y<image_blit->height; y++) {
					uint8_t *source_row = source
						+ (image_blit->in_y + y) * context_surfaces->source_pitch
						+ image_blit->in_x * bytes_per_pixel;

					uint8_t *dest_row = dest
						+ (image_blit->out_y + y) * context_surfaces->dest_pitch
						+ image_blit->out_x * bytes_per_pixel;

					memmove(dest_row, source_row,
						image_blit->width * bytes_per_pixel);
				}
				// The copy is a CPU write: mark it for later fetches in this pusher
				// span (blit methods no longer force a texture barrier).
				Nv2aGlStampRange(uint32_t(dest - d->vram_ptr) + image_blit->out_y * context_surfaces->dest_pitch,
				                 image_blit->height * context_surfaces->dest_pitch);

			} else {
				assert(false);
			}

			break;
		default:
			Nv2aGlUnknown("Unknown NV_IMAGE_BLIT Method: 0x%08X", method);
		}
		break;
	}

	case NV_KELVIN_PRIMITIVE: {
		switch (method) {
		case NV097_SET_OBJECT:
			kelvin->object_instance = parameter;
			break;

		case NV097_NO_OPERATION:
			/* The bios uses nop as a software method call -
			 * it seems to expect a notify interrupt if the parameter isn't 0.
			 * According to a nouveau guy it should still be a nop regardless
			 * of the parameter. It's possible a debug register enables this,
			 * but nothing obvious sticks out. Weird.
			 */
			if (parameter != 0) {
				assert(!(pg->pending_interrupts & NV_PGRAPH_INTR_ERROR));

				SET_MASK(pg->regs[NV_PGRAPH_TRAPPED_ADDR],
					NV_PGRAPH_TRAPPED_ADDR_CHID, channel_id);
				SET_MASK(pg->regs[NV_PGRAPH_TRAPPED_ADDR],
					NV_PGRAPH_TRAPPED_ADDR_SUBCH, subchannel);
				SET_MASK(pg->regs[NV_PGRAPH_TRAPPED_ADDR],
					NV_PGRAPH_TRAPPED_ADDR_MTHD, method);
				pg->regs[NV_PGRAPH_TRAPPED_DATA_LOW] = parameter;
				pg->regs[NV_PGRAPH_NSOURCE] = NV_PGRAPH_NSOURCE_NOTIFICATION; /* TODO: check this */
				pg->pending_interrupts |= NV_PGRAPH_INTR_ERROR;

				qemu_mutex_unlock(&pg->pgraph_lock);
				qemu_mutex_lock_iothread();
				update_irq(d);
				qemu_mutex_lock(&pg->pgraph_lock);
				qemu_mutex_unlock_iothread();

				while (pg->pending_interrupts & NV_PGRAPH_INTR_ERROR) {
					qemu_cond_wait(&pg->interrupt_cond, &pg->pgraph_lock);
				}
			}
			break;

		case NV097_WAIT_FOR_IDLE:
			{ g_download_site = 1600; pgraph_update_surface(d, false, true, true); g_download_site = 0; }
			break;


		case NV097_SET_FLIP_READ:
			SET_MASK(pg->regs[NV_PGRAPH_SURFACE], NV_PGRAPH_SURFACE_READ_3D,
				parameter);
			break;
		case NV097_SET_FLIP_WRITE:
			SET_MASK(pg->regs[NV_PGRAPH_SURFACE], NV_PGRAPH_SURFACE_WRITE_3D,
				parameter);
			break;
		case NV097_SET_FLIP_MODULO:
			SET_MASK(pg->regs[NV_PGRAPH_SURFACE], NV_PGRAPH_SURFACE_MODULO_3D,
				parameter);
			break;
		case NV097_FLIP_INCREMENT_WRITE: {
			NV2A_DPRINTF("flip increment write %d -> ",
				GET_MASK(pg->regs[NV_PGRAPH_SURFACE],
					NV_PGRAPH_SURFACE_WRITE_3D));
			SET_MASK(pg->regs[NV_PGRAPH_SURFACE],
				NV_PGRAPH_SURFACE_WRITE_3D,
				(GET_MASK(pg->regs[NV_PGRAPH_SURFACE],
					NV_PGRAPH_SURFACE_WRITE_3D) + 1)
				% GET_MASK(pg->regs[NV_PGRAPH_SURFACE],
					NV_PGRAPH_SURFACE_MODULO_3D));
			NV2A_DPRINTF("%d\n",
				GET_MASK(pg->regs[NV_PGRAPH_SURFACE],
					NV_PGRAPH_SURFACE_WRITE_3D));

#ifdef __APPLE__
			if (glFrameTerminatorGREMEDY) {
				glFrameTerminatorGREMEDY();
			}
#endif // __APPLE__
			break;
		}
		case NV097_FLIP_STALL: {
			LARGE_INTEGER tl_enter; QueryPerformanceCounter(&tl_enter); long long tl_idle = g_gpu_idle_ticks;
			{ g_download_site = 1638; pgraph_update_surface(d, false, true, true); g_download_site = 0; }


			while (true) {
				uint32_t surface = pg->regs[NV_PGRAPH_SURFACE];
				NV2A_DPRINTF("flip stall read: %d, write: %d, modulo: %d\n",
					GET_MASK(surface, NV_PGRAPH_SURFACE_READ_3D),
					GET_MASK(surface, NV_PGRAPH_SURFACE_WRITE_3D),
					GET_MASK(surface, NV_PGRAPH_SURFACE_MODULO_3D));

				if (GET_MASK(surface, NV_PGRAPH_SURFACE_READ_3D)
					!= GET_MASK(surface, NV_PGRAPH_SURFACE_WRITE_3D)) {
					break;
				}

				qemu_cond_wait(&pg->flip_3d, &pg->pgraph_lock);
			}

			// TODO: Remove this when the AMD crash is solved in vblank_thread
			pgraph_check_gl_errors("in the frame before this flip");
			{   // Diagnostic: GPU timestamp at each flip, read four frames later (no stall).
				static GLuint q[8]; static uint64_t flips = 0, last_ts = 0;
				if (!q[0]) glGenQueries(8, q);
				if (flips >= 4) {
					GLuint64 ts = 0; glGetQueryObjectui64v(q[(flips - 4) % 8], GL_QUERY_RESULT, &ts);
					if (last_ts) { g_gpu_frame_ns += ts - last_ts; ++g_gpu_frames; }
					last_ts = ts;
				}
				glQueryCounter(q[flips % 8], GL_TIMESTAMP); ++flips;
			}
			LARGE_INTEGER tl_stalled; QueryPerformanceCounter(&tl_stalled);
			Nv2aGlReapFences();
			Nv2aGlPresent(d);
			{   LARGE_INTEGER tl_exit, fq; QueryPerformanceCounter(&tl_exit); QueryPerformanceFrequency(&fq);
				if (g_tl_prev_exit) {
					double ms = 1000.0 / fq.QuadPart;
					double idle = (tl_idle - g_tl_prev_idle) * ms, total = (tl_exit.QuadPart - g_tl_prev_exit) * ms;
					double work = (tl_enter.QuadPart - g_tl_prev_exit) * ms - idle;
                    g_tl_over18 += total > 18.0; g_tl_over25 += total > 25.0; g_tl_max = std::max(g_tl_max, total);
					double stall = (tl_stalled.QuadPart - tl_enter.QuadPart) * ms, present = (tl_exit.QuadPart - tl_stalled.QuadPart) * ms;
					for (Nv2aTimeline* t : { &g_tl_all, total > 40.0 ? &g_tl_slow : nullptr }) if (t) {
						++t->n; t->work += work; t->idle += idle; t->stall += stall; t->present += present; t->total += total; }
				}
				g_tl_prev_exit = tl_exit.QuadPart; g_tl_prev_idle = g_gpu_idle_ticks; }
			NV2A_DPRINTF("flip stall done\n");
			break;
		}

		case NV097_SET_CONTEXT_DMA_NOTIFIES:
			pg->dma_notifies = parameter;
			break;
		case NV097_SET_CONTEXT_DMA_A:
			pg->dma_a = parameter;
			break;
		case NV097_SET_CONTEXT_DMA_B:
			pg->dma_b = parameter;
			break;
		case NV097_SET_CONTEXT_DMA_STATE:
			pg->dma_state = parameter;
			break;
		case NV097_SET_CONTEXT_DMA_COLOR:
			/* try to get any straggling draws in before the surface's changed :/ */
			{ g_download_site = 1675; pgraph_update_surface(d, false, true, true); g_download_site = 0; }

			pg->dma_color = parameter;
			break;
		case NV097_SET_CONTEXT_DMA_ZETA:
			pg->dma_zeta = parameter;
			break;
		case NV097_SET_CONTEXT_DMA_VERTEX_A:
			pg->dma_vertex_a = parameter;
			break;
		case NV097_SET_CONTEXT_DMA_VERTEX_B:
			pg->dma_vertex_b = parameter;
			break;
		case NV097_SET_CONTEXT_DMA_SEMAPHORE:
			pg->dma_semaphore = parameter;
			break;
		case NV097_SET_CONTEXT_DMA_REPORT:
			pg->dma_report = parameter;
			break;

		case NV097_SET_SURFACE_CLIP_HORIZONTAL:
            // This is a state assignment, not a fence. An identical clip
            // retains the same target and leaves pending draws dirty for the
            // next real visibility point (idle, semaphore, flip or rebind).
            if (pg->surface_shape.clip_x == (parameter & 0xffff) &&
                pg->surface_shape.clip_width == (parameter >> 16)) break;
			{ g_download_site = 1701; pgraph_update_surface(d, false, true, true); g_download_site = 0; }

			pg->surface_shape.clip_x =
				GET_MASK(parameter, NV097_SET_SURFACE_CLIP_HORIZONTAL_X);
			pg->surface_shape.clip_width =
				GET_MASK(parameter, NV097_SET_SURFACE_CLIP_HORIZONTAL_WIDTH);
			break;
		case NV097_SET_SURFACE_CLIP_VERTICAL:
            // This is a state assignment, not a fence. An identical clip
            // retains the same target and leaves pending draws dirty for the
            // next real visibility point (idle, semaphore, flip or rebind).
            if (pg->surface_shape.clip_y == (parameter & 0xffff) &&
                pg->surface_shape.clip_height == (parameter >> 16)) break;
			{ g_download_site = 1714; pgraph_update_surface(d, false, true, true); g_download_site = 0; }
			
			pg->surface_shape.clip_y =
				GET_MASK(parameter, NV097_SET_SURFACE_CLIP_VERTICAL_Y);
			pg->surface_shape.clip_height =
				GET_MASK(parameter, NV097_SET_SURFACE_CLIP_VERTICAL_HEIGHT);
			break;
		case NV097_SET_SURFACE_FORMAT:
            // State assignment does not synchronize unchanged storage.
            if (pg->surface_shape.color_format == GET_MASK(parameter, NV097_SET_SURFACE_FORMAT_COLOR) &&
                pg->surface_shape.zeta_format == GET_MASK(parameter, NV097_SET_SURFACE_FORMAT_ZETA) &&
                pg->surface_type == GET_MASK(parameter, NV097_SET_SURFACE_FORMAT_TYPE) &&
                pg->surface_shape.anti_aliasing == GET_MASK(parameter, NV097_SET_SURFACE_FORMAT_ANTI_ALIASING) &&
                pg->surface_shape.log_width == GET_MASK(parameter, NV097_SET_SURFACE_FORMAT_WIDTH) &&
                pg->surface_shape.log_height == GET_MASK(parameter, NV097_SET_SURFACE_FORMAT_HEIGHT)) break;
			{ g_download_site = 1729; pgraph_update_surface(d, false, true, true); g_download_site = 0; }

			pg->surface_shape.color_format =
				GET_MASK(parameter, NV097_SET_SURFACE_FORMAT_COLOR);
			pg->surface_shape.zeta_format =
				GET_MASK(parameter, NV097_SET_SURFACE_FORMAT_ZETA);
			pg->surface_type =
				GET_MASK(parameter, NV097_SET_SURFACE_FORMAT_TYPE);
			pg->surface_shape.anti_aliasing =
				GET_MASK(parameter, NV097_SET_SURFACE_FORMAT_ANTI_ALIASING);
			pg->surface_shape.log_width =
				GET_MASK(parameter, NV097_SET_SURFACE_FORMAT_WIDTH);
			pg->surface_shape.log_height =
				GET_MASK(parameter, NV097_SET_SURFACE_FORMAT_HEIGHT);
			break;
		case NV097_SET_SURFACE_PITCH:
            if (pg->surface_color.pitch == GET_MASK(parameter, NV097_SET_SURFACE_PITCH_COLOR) &&
                pg->surface_zeta.pitch == GET_MASK(parameter, NV097_SET_SURFACE_PITCH_ZETA)) break;
			{ g_download_site = 1747; pgraph_update_surface(d, false, true, true); g_download_site = 0; }

			pg->surface_color.pitch =
				GET_MASK(parameter, NV097_SET_SURFACE_PITCH_COLOR);
			pg->surface_zeta.pitch =
				GET_MASK(parameter, NV097_SET_SURFACE_PITCH_ZETA);
        pg->surface_color.buffer_dirty = true;
        pg->surface_zeta.buffer_dirty = true;
			break;
		case NV097_SET_SURFACE_COLOR_OFFSET:
            if (pg->surface_color.offset == parameter) break;
			{ g_download_site = 1758; pgraph_update_surface(d, false, true, true); g_download_site = 0; }

			pg->surface_color.offset = parameter;
        pg->surface_color.buffer_dirty = true;
			break;
		case NV097_SET_SURFACE_ZETA_OFFSET:
            if (pg->surface_zeta.offset == parameter) break;
			{ g_download_site = 1765; pgraph_update_surface(d, false, true, true); g_download_site = 0; }

			pg->surface_zeta.offset = parameter;
        pg->surface_zeta.buffer_dirty = true;
			break;

		CASE_8(NV097_SET_COMBINER_ALPHA_ICW, 4) :
			slot = (method - NV097_SET_COMBINER_ALPHA_ICW) / 4;
			pg->regs[NV_PGRAPH_COMBINEALPHAI0 + slot * 4] = parameter;
			break;

		case NV097_SET_COMBINER_SPECULAR_FOG_CW0:
			pg->regs[NV_PGRAPH_COMBINESPECFOG0] = parameter;
			break;

		case NV097_SET_COMBINER_SPECULAR_FOG_CW1:
			pg->regs[NV_PGRAPH_COMBINESPECFOG1] = parameter;
			break;

		CASE_4(NV097_SET_TEXTURE_ADDRESS, 64):
			slot = (method - NV097_SET_TEXTURE_ADDRESS) / 64;
			pg->regs[NV_PGRAPH_TEXADDRESS0 + slot * 4] = parameter;
			break;
		case NV097_SET_CONTROL0: {
            // Reasserting stencil/depth interpretation is not an idle/fence.
            if (bool(pg->regs[NV_PGRAPH_CONTROL_0] & NV_PGRAPH_CONTROL_0_STENCIL_WRITE_ENABLE) == bool(parameter & NV097_SET_CONTROL0_STENCIL_WRITE_ENABLE) &&
                GET_MASK(pg->regs[NV_PGRAPH_SETUPRASTER], NV_PGRAPH_SETUPRASTER_Z_FORMAT) == GET_MASK(parameter, NV097_SET_CONTROL0_Z_FORMAT) &&
                bool(pg->regs[NV_PGRAPH_CONTROL_0] & NV_PGRAPH_CONTROL_0_Z_PERSPECTIVE_ENABLE) == bool(parameter & NV097_SET_CONTROL0_Z_PERSPECTIVE_ENABLE) &&
                GET_MASK(pg->regs[NV_PGRAPH_CONTROL_0], NV_PGRAPH_CONTROL_0_CSCONVERT) == GET_MASK(parameter, NV097_SET_CONTROL0_COLOR_SPACE_CONVERT)) break;
			{ g_download_site = 1794; pgraph_update_surface(d, false, true, true); g_download_site = 0; }

			bool stencil_write_enable =
				parameter & NV097_SET_CONTROL0_STENCIL_WRITE_ENABLE;
			SET_MASK(pg->regs[NV_PGRAPH_CONTROL_0],
				NV_PGRAPH_CONTROL_0_STENCIL_WRITE_ENABLE,
				stencil_write_enable);

			uint32_t z_format = GET_MASK(parameter, NV097_SET_CONTROL0_Z_FORMAT);
			SET_MASK(pg->regs[NV_PGRAPH_SETUPRASTER],
				NV_PGRAPH_SETUPRASTER_Z_FORMAT, z_format);

			bool z_perspective =
				parameter & NV097_SET_CONTROL0_Z_PERSPECTIVE_ENABLE;
			SET_MASK(pg->regs[NV_PGRAPH_CONTROL_0],
				NV_PGRAPH_CONTROL_0_Z_PERSPECTIVE_ENABLE,
				z_perspective);

			int color_space_convert =
				GET_MASK(parameter, NV097_SET_CONTROL0_COLOR_SPACE_CONVERT);
			SET_MASK(pg->regs[NV_PGRAPH_CONTROL_0],
				NV_PGRAPH_CONTROL_0_CSCONVERT,
				color_space_convert);
			break;
		}

		case NV097_SET_FOG_MODE: {
			/* FIXME: There is also NV_PGRAPH_CSV0_D_FOG_MODE */
			unsigned int mode;
			switch (parameter) {
			case NV097_SET_FOG_MODE_V_LINEAR:
				mode = NV_PGRAPH_CONTROL_3_FOG_MODE_LINEAR; break;
			case NV097_SET_FOG_MODE_V_EXP:
				mode = NV_PGRAPH_CONTROL_3_FOG_MODE_EXP; break;
			case NV097_SET_FOG_MODE_V_EXP2:
				mode = NV_PGRAPH_CONTROL_3_FOG_MODE_EXP2; break;
			case NV097_SET_FOG_MODE_V_EXP_ABS:
				mode = NV_PGRAPH_CONTROL_3_FOG_MODE_EXP_ABS; break;
			case NV097_SET_FOG_MODE_V_EXP2_ABS:
				mode = NV_PGRAPH_CONTROL_3_FOG_MODE_EXP2_ABS; break;
			case NV097_SET_FOG_MODE_V_LINEAR_ABS:
				mode = NV_PGRAPH_CONTROL_3_FOG_MODE_LINEAR_ABS; break;
			default:
				assert(false);
				break;
			}
			SET_MASK(pg->regs[NV_PGRAPH_CONTROL_3], NV_PGRAPH_CONTROL_3_FOG_MODE,
				mode);
			break;
		}
		case NV097_SET_FOG_GEN_MODE: {
			unsigned int mode;
			switch (parameter) {
			case NV097_SET_FOG_GEN_MODE_V_SPEC_ALPHA:
				mode = NV_PGRAPH_CSV0_D_FOGGENMODE_SPEC_ALPHA; break;
			case NV097_SET_FOG_GEN_MODE_V_RADIAL:
				mode = NV_PGRAPH_CSV0_D_FOGGENMODE_RADIAL; break;
			case NV097_SET_FOG_GEN_MODE_V_PLANAR:
				mode = NV_PGRAPH_CSV0_D_FOGGENMODE_PLANAR; break;
			case NV097_SET_FOG_GEN_MODE_V_ABS_PLANAR:
				mode = NV_PGRAPH_CSV0_D_FOGGENMODE_ABS_PLANAR; break;
			case NV097_SET_FOG_GEN_MODE_V_FOG_X:
				mode = NV_PGRAPH_CSV0_D_FOGGENMODE_FOG_X; break;
			default:
				assert(false);
				break;
			}
			SET_MASK(pg->regs[NV_PGRAPH_CSV0_D], NV_PGRAPH_CSV0_D_FOGGENMODE, mode);
			break;
		}
		case NV097_SET_FOG_ENABLE:
			/*
			FIXME: There is also:
			SET_MASK(pg->regs[NV_PGRAPH_CSV0_D], NV_PGRAPH_CSV0_D_FOGENABLE,
			parameter);
			*/
			SET_MASK(pg->regs[NV_PGRAPH_CONTROL_3], NV_PGRAPH_CONTROL_3_FOGENABLE,
				parameter);
			break;
		case NV097_SET_FOG_COLOR: {
			/* parameter channels are ABGR, PGRAPH channels are ARGB */
			uint8_t alpha = GET_MASK(parameter, NV097_SET_FOG_COLOR_ALPHA);
			uint8_t blue = GET_MASK(parameter, NV097_SET_FOG_COLOR_BLUE);
			uint8_t green = GET_MASK(parameter, NV097_SET_FOG_COLOR_GREEN);
			uint8_t red = GET_MASK(parameter, NV097_SET_FOG_COLOR_RED);
			SET_MASK(pg->regs[NV_PGRAPH_FOGCOLOR], NV_PGRAPH_FOGCOLOR_ALPHA, alpha);
			SET_MASK(pg->regs[NV_PGRAPH_FOGCOLOR], NV_PGRAPH_FOGCOLOR_RED, red);
			SET_MASK(pg->regs[NV_PGRAPH_FOGCOLOR], NV_PGRAPH_FOGCOLOR_GREEN, green);
			SET_MASK(pg->regs[NV_PGRAPH_FOGCOLOR], NV_PGRAPH_FOGCOLOR_BLUE, blue);
			break;
		}
		case NV097_SET_WINDOW_CLIP_TYPE:
			SET_MASK(pg->regs[NV_PGRAPH_SETUPRASTER],
				NV_PGRAPH_SETUPRASTER_WINDOWCLIPTYPE, parameter);
			break;
		CASE_8(NV097_SET_WINDOW_CLIP_HORIZONTAL, 4):
			slot = (method - NV097_SET_WINDOW_CLIP_HORIZONTAL) / 4;
			pg->regs[NV_PGRAPH_WINDOWCLIPX0 + slot * 4] = parameter;
			break;
		CASE_8(NV097_SET_WINDOW_CLIP_VERTICAL, 4):
			slot = (method - NV097_SET_WINDOW_CLIP_VERTICAL) / 4;
			pg->regs[NV_PGRAPH_WINDOWCLIPY0 + slot * 4] = parameter;
			break;
		case NV097_SET_ALPHA_TEST_ENABLE:
			SET_MASK(pg->regs[NV_PGRAPH_CONTROL_0],
				NV_PGRAPH_CONTROL_0_ALPHATESTENABLE, parameter);
			break;
		case NV097_SET_BLEND_ENABLE:
			SET_MASK(pg->regs[NV_PGRAPH_BLEND], NV_PGRAPH_BLEND_EN, parameter);
			break;
		case NV097_SET_CULL_FACE_ENABLE:
			SET_MASK(pg->regs[NV_PGRAPH_SETUPRASTER],
				NV_PGRAPH_SETUPRASTER_CULLENABLE,
				parameter);
			break;
		case NV097_SET_DEPTH_TEST_ENABLE:
			// Test-case : Whiplash
			SET_MASK(pg->regs[NV_PGRAPH_CONTROL_0], NV_PGRAPH_CONTROL_0_ZENABLE,
				parameter);
			break;
		case NV097_SET_DITHER_ENABLE:
			SET_MASK(pg->regs[NV_PGRAPH_CONTROL_0],
				NV_PGRAPH_CONTROL_0_DITHERENABLE, parameter);
			break;
		case NV097_SET_LIGHTING_ENABLE:
			SET_MASK(pg->regs[NV_PGRAPH_CSV0_C], NV_PGRAPH_CSV0_C_LIGHTING,
				parameter);
			break;
		case NV097_SET_SKIN_MODE:
			SET_MASK(pg->regs[NV_PGRAPH_CSV0_D], NV_PGRAPH_CSV0_D_SKIN,
				parameter);
			break;
		case NV097_SET_STENCIL_TEST_ENABLE:
			SET_MASK(pg->regs[NV_PGRAPH_CONTROL_1],
				NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE, parameter);
			break;
		case NV097_SET_POLY_OFFSET_POINT_ENABLE:
			SET_MASK(pg->regs[NV_PGRAPH_SETUPRASTER],
				NV_PGRAPH_SETUPRASTER_POFFSETPOINTENABLE, parameter);
			break;
		case NV097_SET_POLY_OFFSET_LINE_ENABLE:
			SET_MASK(pg->regs[NV_PGRAPH_SETUPRASTER],
				NV_PGRAPH_SETUPRASTER_POFFSETLINEENABLE, parameter);
			break;
		case NV097_SET_POLY_OFFSET_FILL_ENABLE:
			SET_MASK(pg->regs[NV_PGRAPH_SETUPRASTER],
				NV_PGRAPH_SETUPRASTER_POFFSETFILLENABLE, parameter);
			break;
		case NV097_SET_ALPHA_FUNC:
			SET_MASK(pg->regs[NV_PGRAPH_CONTROL_0],
				NV_PGRAPH_CONTROL_0_ALPHAFUNC, parameter & 0xF);
			break;
		case NV097_SET_ALPHA_REF:
			SET_MASK(pg->regs[NV_PGRAPH_CONTROL_0],
				NV_PGRAPH_CONTROL_0_ALPHAREF, parameter);
			break;
		case NV097_SET_BLEND_FUNC_SFACTOR: {
			unsigned int factor;
			switch (parameter) {
			case NV097_SET_BLEND_FUNC_SFACTOR_V_ZERO:
				factor = NV_PGRAPH_BLEND_SFACTOR_ZERO; break;
			case NV097_SET_BLEND_FUNC_SFACTOR_V_ONE:
				factor = NV_PGRAPH_BLEND_SFACTOR_ONE; break;
			case NV097_SET_BLEND_FUNC_SFACTOR_V_SRC_COLOR:
				factor = NV_PGRAPH_BLEND_SFACTOR_SRC_COLOR; break;
			case NV097_SET_BLEND_FUNC_SFACTOR_V_ONE_MINUS_SRC_COLOR:
				factor = NV_PGRAPH_BLEND_SFACTOR_ONE_MINUS_SRC_COLOR; break;
			case NV097_SET_BLEND_FUNC_SFACTOR_V_SRC_ALPHA:
				factor = NV_PGRAPH_BLEND_SFACTOR_SRC_ALPHA; break;
			case NV097_SET_BLEND_FUNC_SFACTOR_V_ONE_MINUS_SRC_ALPHA:
				factor = NV_PGRAPH_BLEND_SFACTOR_ONE_MINUS_SRC_ALPHA; break;
			case NV097_SET_BLEND_FUNC_SFACTOR_V_DST_ALPHA:
				factor = NV_PGRAPH_BLEND_SFACTOR_DST_ALPHA; break;
			case NV097_SET_BLEND_FUNC_SFACTOR_V_ONE_MINUS_DST_ALPHA:
				factor = NV_PGRAPH_BLEND_SFACTOR_ONE_MINUS_DST_ALPHA; break;
			case NV097_SET_BLEND_FUNC_SFACTOR_V_DST_COLOR:
				factor = NV_PGRAPH_BLEND_SFACTOR_DST_COLOR; break;
			case NV097_SET_BLEND_FUNC_SFACTOR_V_ONE_MINUS_DST_COLOR:
				factor = NV_PGRAPH_BLEND_SFACTOR_ONE_MINUS_DST_COLOR; break;
			case NV097_SET_BLEND_FUNC_SFACTOR_V_SRC_ALPHA_SATURATE:
				factor = NV_PGRAPH_BLEND_SFACTOR_SRC_ALPHA_SATURATE; break;
			case NV097_SET_BLEND_FUNC_SFACTOR_V_CONSTANT_COLOR:
				factor = NV_PGRAPH_BLEND_SFACTOR_CONSTANT_COLOR; break;
			case NV097_SET_BLEND_FUNC_SFACTOR_V_ONE_MINUS_CONSTANT_COLOR:
				factor = NV_PGRAPH_BLEND_SFACTOR_ONE_MINUS_CONSTANT_COLOR; break;
			case NV097_SET_BLEND_FUNC_SFACTOR_V_CONSTANT_ALPHA:
				factor = NV_PGRAPH_BLEND_SFACTOR_CONSTANT_ALPHA; break;
			case NV097_SET_BLEND_FUNC_SFACTOR_V_ONE_MINUS_CONSTANT_ALPHA:
				factor = NV_PGRAPH_BLEND_SFACTOR_ONE_MINUS_CONSTANT_ALPHA; break;
			default:
				fprintf(stderr, "Unknown blend source factor: 0x%x\n", parameter);
				assert(false);
				break;
			}
			SET_MASK(pg->regs[NV_PGRAPH_BLEND], NV_PGRAPH_BLEND_SFACTOR, factor);

			break;
		}

		case NV097_SET_BLEND_FUNC_DFACTOR: {
			unsigned int factor;
			switch (parameter) {
			case NV097_SET_BLEND_FUNC_DFACTOR_V_ZERO:
				factor = NV_PGRAPH_BLEND_DFACTOR_ZERO; break;
			case NV097_SET_BLEND_FUNC_DFACTOR_V_ONE:
				factor = NV_PGRAPH_BLEND_DFACTOR_ONE; break;
			case NV097_SET_BLEND_FUNC_DFACTOR_V_SRC_COLOR:
				factor = NV_PGRAPH_BLEND_DFACTOR_SRC_COLOR; break;
			case NV097_SET_BLEND_FUNC_DFACTOR_V_ONE_MINUS_SRC_COLOR:
				factor = NV_PGRAPH_BLEND_DFACTOR_ONE_MINUS_SRC_COLOR; break;
			case NV097_SET_BLEND_FUNC_DFACTOR_V_SRC_ALPHA:
				factor = NV_PGRAPH_BLEND_DFACTOR_SRC_ALPHA; break;
			case NV097_SET_BLEND_FUNC_DFACTOR_V_ONE_MINUS_SRC_ALPHA:
				factor = NV_PGRAPH_BLEND_DFACTOR_ONE_MINUS_SRC_ALPHA; break;
			case NV097_SET_BLEND_FUNC_DFACTOR_V_DST_ALPHA:
				factor = NV_PGRAPH_BLEND_DFACTOR_DST_ALPHA; break;
			case NV097_SET_BLEND_FUNC_DFACTOR_V_ONE_MINUS_DST_ALPHA:
				factor = NV_PGRAPH_BLEND_DFACTOR_ONE_MINUS_DST_ALPHA; break;
			case NV097_SET_BLEND_FUNC_DFACTOR_V_DST_COLOR:
				factor = NV_PGRAPH_BLEND_DFACTOR_DST_COLOR; break;
			case NV097_SET_BLEND_FUNC_DFACTOR_V_ONE_MINUS_DST_COLOR:
				factor = NV_PGRAPH_BLEND_DFACTOR_ONE_MINUS_DST_COLOR; break;
			case NV097_SET_BLEND_FUNC_DFACTOR_V_SRC_ALPHA_SATURATE:
				factor = NV_PGRAPH_BLEND_DFACTOR_SRC_ALPHA_SATURATE; break;
			case NV097_SET_BLEND_FUNC_DFACTOR_V_CONSTANT_COLOR:
				factor = NV_PGRAPH_BLEND_DFACTOR_CONSTANT_COLOR; break;
			case NV097_SET_BLEND_FUNC_DFACTOR_V_ONE_MINUS_CONSTANT_COLOR:
				factor = NV_PGRAPH_BLEND_DFACTOR_ONE_MINUS_CONSTANT_COLOR; break;
			case NV097_SET_BLEND_FUNC_DFACTOR_V_CONSTANT_ALPHA:
				factor = NV_PGRAPH_BLEND_DFACTOR_CONSTANT_ALPHA; break;
			case NV097_SET_BLEND_FUNC_DFACTOR_V_ONE_MINUS_CONSTANT_ALPHA:
				factor = NV_PGRAPH_BLEND_DFACTOR_ONE_MINUS_CONSTANT_ALPHA; break;
			default:
				fprintf(stderr, "Unknown blend destination factor: 0x%x\n", parameter);
				assert(false);
				break;
			}
			SET_MASK(pg->regs[NV_PGRAPH_BLEND], NV_PGRAPH_BLEND_DFACTOR, factor);

			break;
		}

		case NV097_SET_BLEND_COLOR:
			pg->regs[NV_PGRAPH_BLENDCOLOR] = parameter;
			break;

		case NV097_SET_BLEND_EQUATION: {
			unsigned int equation;
			switch (parameter) {
			case NV097_SET_BLEND_EQUATION_V_FUNC_SUBTRACT:
				equation = 0; break;
			case NV097_SET_BLEND_EQUATION_V_FUNC_REVERSE_SUBTRACT:
				equation = 1; break;
			case NV097_SET_BLEND_EQUATION_V_FUNC_ADD:
				equation = 2; break;
			case NV097_SET_BLEND_EQUATION_V_MIN:
				equation = 3; break;
			case NV097_SET_BLEND_EQUATION_V_MAX:
				equation = 4; break;
			case NV097_SET_BLEND_EQUATION_V_FUNC_REVERSE_SUBTRACT_SIGNED:
				equation = 5; break;
			case NV097_SET_BLEND_EQUATION_V_FUNC_ADD_SIGNED:
				equation = 6; break;
			default:
				assert(false);
				break;
			}
			SET_MASK(pg->regs[NV_PGRAPH_BLEND], NV_PGRAPH_BLEND_EQN, equation);

			break;
		}

		case NV097_SET_DEPTH_FUNC:
			// Test-case : Whiplash
			SET_MASK(pg->regs[NV_PGRAPH_CONTROL_0], NV_PGRAPH_CONTROL_0_ZFUNC,
				parameter & 0xF);
			break;

		case NV097_SET_COLOR_MASK: {
			pg->surface_color.write_enabled_cache |= pgraph_get_color_write_enabled(pg);

			bool alpha = parameter & NV097_SET_COLOR_MASK_ALPHA_WRITE_ENABLE;
			bool red = parameter & NV097_SET_COLOR_MASK_RED_WRITE_ENABLE;
			bool green = parameter & NV097_SET_COLOR_MASK_GREEN_WRITE_ENABLE;
			bool blue = parameter & NV097_SET_COLOR_MASK_BLUE_WRITE_ENABLE;
			SET_MASK(pg->regs[NV_PGRAPH_CONTROL_0],
				NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE, alpha);
			SET_MASK(pg->regs[NV_PGRAPH_CONTROL_0],
				NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE, red);
			SET_MASK(pg->regs[NV_PGRAPH_CONTROL_0],
				NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE, green);
			SET_MASK(pg->regs[NV_PGRAPH_CONTROL_0],
				NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE, blue);
			break;
		}
		case NV097_SET_DEPTH_MASK:
			pg->surface_zeta.write_enabled_cache |= pgraph_get_zeta_write_enabled(pg);

			SET_MASK(pg->regs[NV_PGRAPH_CONTROL_0],
				NV_PGRAPH_CONTROL_0_ZWRITEENABLE, parameter);
			break;
		case NV097_SET_STENCIL_MASK:
			SET_MASK(pg->regs[NV_PGRAPH_CONTROL_1],
				NV_PGRAPH_CONTROL_1_STENCIL_MASK_WRITE, parameter);
			break;
		case NV097_SET_STENCIL_FUNC:
			SET_MASK(pg->regs[NV_PGRAPH_CONTROL_1],
				NV_PGRAPH_CONTROL_1_STENCIL_FUNC, parameter & 0xF);
			break;
		case NV097_SET_STENCIL_FUNC_REF:
			SET_MASK(pg->regs[NV_PGRAPH_CONTROL_1],
				NV_PGRAPH_CONTROL_1_STENCIL_REF, parameter);
			break;
		case NV097_SET_STENCIL_FUNC_MASK:
			SET_MASK(pg->regs[NV_PGRAPH_CONTROL_1],
				NV_PGRAPH_CONTROL_1_STENCIL_MASK_READ, parameter);
			break;
		case NV097_SET_STENCIL_OP_FAIL:
			SET_MASK(pg->regs[NV_PGRAPH_CONTROL_2],
				NV_PGRAPH_CONTROL_2_STENCIL_OP_FAIL,
				kelvin_map_stencil_op(parameter));
			break;
		case NV097_SET_STENCIL_OP_ZFAIL:
			SET_MASK(pg->regs[NV_PGRAPH_CONTROL_2],
				NV_PGRAPH_CONTROL_2_STENCIL_OP_ZFAIL,
				kelvin_map_stencil_op(parameter));
			break;
		case NV097_SET_STENCIL_OP_ZPASS:
			SET_MASK(pg->regs[NV_PGRAPH_CONTROL_2],
				NV_PGRAPH_CONTROL_2_STENCIL_OP_ZPASS,
				kelvin_map_stencil_op(parameter));
			break;

		case NV097_SET_POLYGON_OFFSET_SCALE_FACTOR:
			pg->regs[NV_PGRAPH_ZOFFSETFACTOR] = parameter;
			break;
		case NV097_SET_POLYGON_OFFSET_BIAS:
			pg->regs[NV_PGRAPH_ZOFFSETBIAS] = parameter;
			break;
		case NV097_SET_FRONT_POLYGON_MODE:
			SET_MASK(pg->regs[NV_PGRAPH_SETUPRASTER],
				NV_PGRAPH_SETUPRASTER_FRONTFACEMODE,
				kelvin_map_polygon_mode(parameter));
			break;
		case NV097_SET_BACK_POLYGON_MODE:
			SET_MASK(pg->regs[NV_PGRAPH_SETUPRASTER],
				NV_PGRAPH_SETUPRASTER_BACKFACEMODE,
				kelvin_map_polygon_mode(parameter));
			break;
		case NV097_SET_CLIP_MIN:
			pg->regs[NV_PGRAPH_ZCLIPMIN] = parameter;
			break;
		case NV097_SET_CLIP_MAX:
			pg->regs[NV_PGRAPH_ZCLIPMAX] = parameter;
			break;
		case NV097_SET_CULL_FACE: {
			unsigned int face;
			switch (parameter) {
			case NV097_SET_CULL_FACE_V_FRONT:
				face = NV_PGRAPH_SETUPRASTER_CULLCTRL_FRONT; break;
			case NV097_SET_CULL_FACE_V_BACK:
				face = NV_PGRAPH_SETUPRASTER_CULLCTRL_BACK; break;
			case NV097_SET_CULL_FACE_V_FRONT_AND_BACK:
				face = NV_PGRAPH_SETUPRASTER_CULLCTRL_FRONT_AND_BACK; break;
			default:
				assert(false);
				break;
			}
			SET_MASK(pg->regs[NV_PGRAPH_SETUPRASTER],
				NV_PGRAPH_SETUPRASTER_CULLCTRL,
				face);
			break;
		}
		case NV097_SET_FRONT_FACE: {
			bool ccw;
			switch (parameter) {
			case NV097_SET_FRONT_FACE_V_CW:
				ccw = false; break;
			case NV097_SET_FRONT_FACE_V_CCW:
				ccw = true; break;
			default:
				fprintf(stderr, "Unknown front face: 0x%x\n", parameter);
				assert(false);
				break;
			}
			SET_MASK(pg->regs[NV_PGRAPH_SETUPRASTER],
				NV_PGRAPH_SETUPRASTER_FRONTFACE,
				ccw ? 1 : 0);
			break;
		}
		case NV097_SET_NORMALIZATION_ENABLE:
			SET_MASK(pg->regs[NV_PGRAPH_CSV0_C],
				NV_PGRAPH_CSV0_C_NORMALIZATION_ENABLE,
				parameter);
			break;

		case NV097_SET_LIGHT_ENABLE_MASK:
			SET_MASK(pg->regs[NV_PGRAPH_CSV0_D],
				NV_PGRAPH_CSV0_D_LIGHTS,
				parameter);
			break;

		CASE_4(NV097_SET_TEXGEN_S, 16) : {
			slot = (method - NV097_SET_TEXGEN_S) / 16;
			unsigned int reg = (slot < 2) ? NV_PGRAPH_CSV1_A
				: NV_PGRAPH_CSV1_B;
			unsigned int mask = (slot % 2) ? NV_PGRAPH_CSV1_A_T1_S
				: NV_PGRAPH_CSV1_A_T0_S;
			SET_MASK(pg->regs[reg], mask, kelvin_map_texgen(parameter, 0));
			break;
		}
		CASE_4(NV097_SET_TEXGEN_T, 16) : {
			slot = (method - NV097_SET_TEXGEN_T) / 16;
			unsigned int reg = (slot < 2) ? NV_PGRAPH_CSV1_A
				: NV_PGRAPH_CSV1_B;
			unsigned int mask = (slot % 2) ? NV_PGRAPH_CSV1_A_T1_T
				: NV_PGRAPH_CSV1_A_T0_T;
			SET_MASK(pg->regs[reg], mask, kelvin_map_texgen(parameter, 1));
			break;
		}
		CASE_4(NV097_SET_TEXGEN_R, 16) : {
			slot = (method - NV097_SET_TEXGEN_R) / 16;
			unsigned int reg = (slot < 2) ? NV_PGRAPH_CSV1_A
				: NV_PGRAPH_CSV1_B;
			unsigned int mask = (slot % 2) ? NV_PGRAPH_CSV1_A_T1_R
				: NV_PGRAPH_CSV1_A_T0_R;
			SET_MASK(pg->regs[reg], mask, kelvin_map_texgen(parameter, 2));
			break;
		}
		CASE_4(NV097_SET_TEXGEN_Q, 16) : {
			slot = (method - NV097_SET_TEXGEN_Q) / 16;
			unsigned int reg = (slot < 2) ? NV_PGRAPH_CSV1_A
				: NV_PGRAPH_CSV1_B;
			unsigned int mask = (slot % 2) ? NV_PGRAPH_CSV1_A_T1_Q
				: NV_PGRAPH_CSV1_A_T0_Q;
			SET_MASK(pg->regs[reg], mask, kelvin_map_texgen(parameter, 3));
			break;
		}
		CASE_4(NV097_SET_TEXTURE_MATRIX_ENABLE, 4) :
			slot = (method - NV097_SET_TEXTURE_MATRIX_ENABLE) / 4;
			pg->texture_matrix_enable[slot] = parameter;
			break;

		CASE_16(NV097_SET_PROJECTION_MATRIX, 4) : {
			slot = (method - NV097_SET_PROJECTION_MATRIX) / 4;
			// pg->projection_matrix[slot] = *(float*)&parameter;
			unsigned int row = NV_IGRAPH_XF_XFCTX_PMAT0 + slot / 4;
			pg->vsh_constants[row][slot % 4] = parameter;
			pg->vsh_constants_dirty[row] = true;
			break;
		}

		CASE_64(NV097_SET_MODEL_VIEW_MATRIX, 4) : {
			slot = (method - NV097_SET_MODEL_VIEW_MATRIX) / 4;
			unsigned int matnum = slot / 16;
			unsigned int entry = slot % 16;
			unsigned int row = NV_IGRAPH_XF_XFCTX_MMAT0 + matnum * 8 + entry / 4;
			pg->vsh_constants[row][entry % 4] = parameter;
			pg->vsh_constants_dirty[row] = true;
			break;
		}

		CASE_64(NV097_SET_INVERSE_MODEL_VIEW_MATRIX, 4) : {
			slot = (method - NV097_SET_INVERSE_MODEL_VIEW_MATRIX) / 4;
			unsigned int matnum = slot / 16;
			unsigned int entry = slot % 16;
			unsigned int row = NV_IGRAPH_XF_XFCTX_IMMAT0 + matnum * 8 + entry / 4;
			pg->vsh_constants[row][entry % 4] = parameter;
			pg->vsh_constants_dirty[row] = true;
			break;
		}

		CASE_16(NV097_SET_COMPOSITE_MATRIX, 4) : {
			slot = (method - NV097_SET_COMPOSITE_MATRIX) / 4;
			unsigned int row = NV_IGRAPH_XF_XFCTX_CMAT0 + slot / 4;
			pg->vsh_constants[row][slot % 4] = parameter;
			pg->vsh_constants_dirty[row] = true;
			break;
		}

		CASE_64(NV097_SET_TEXTURE_MATRIX, 4) : {
			slot = (method - NV097_SET_TEXTURE_MATRIX) / 4;
			unsigned int tex = slot / 16;
			unsigned int entry = slot % 16;
			unsigned int row = NV_IGRAPH_XF_XFCTX_T0MAT + tex * 8 + entry / 4;
			pg->vsh_constants[row][entry % 4] = parameter;
			pg->vsh_constants_dirty[row] = true;
			break;
		}

		CASE_3(NV097_SET_FOG_PARAMS, 4) :
			slot = (method - NV097_SET_FOG_PARAMS) / 4;
			pg->regs[NV_PGRAPH_FOGPARAM0 + slot * 4] = parameter;
			/* Cxbx note: slot = 2 is right after slot = 1 */
			pg->ltctxa[NV_IGRAPH_XF_LTCTXA_FOG_K][slot] = parameter;
			pg->ltctxa_dirty[NV_IGRAPH_XF_LTCTXA_FOG_K] = true;
			break;

		/* Handles NV097_SET_TEXGEN_PLANE_S,T,R,Q */
		CASE_64(NV097_SET_TEXGEN_PLANE_S, 4) : {
			slot = (method - NV097_SET_TEXGEN_PLANE_S) / 4;
			unsigned int tex = slot / 16;
			unsigned int entry = slot % 16;
			unsigned int row = NV_IGRAPH_XF_XFCTX_TG0MAT + tex * 8 + entry / 4;
			pg->vsh_constants[row][entry % 4] = parameter;
			pg->vsh_constants_dirty[row] = true;
			break;
		}

		case NV097_SET_TEXGEN_VIEW_MODEL:
			SET_MASK(pg->regs[NV_PGRAPH_CSV0_D], NV_PGRAPH_CSV0_D_TEXGEN_REF,
				parameter);
			break;

		CASE_4(NV097_SET_FOG_PLANE, 4):
			slot = (method - NV097_SET_FOG_PLANE) / 4;
			pg->vsh_constants[NV_IGRAPH_XF_XFCTX_FOG][slot] = parameter;
			pg->vsh_constants_dirty[NV_IGRAPH_XF_XFCTX_FOG] = true;
			break;

		CASE_3(NV097_SET_SCENE_AMBIENT_COLOR, 4):
			slot = (method - NV097_SET_SCENE_AMBIENT_COLOR) / 4;
			// ??
			pg->ltctxa[NV_IGRAPH_XF_LTCTXA_FR_AMB][slot] = parameter;
			pg->ltctxa_dirty[NV_IGRAPH_XF_LTCTXA_FR_AMB] = true;
			break;

		CASE_4(NV097_SET_VIEWPORT_OFFSET, 4):
			slot = (method - NV097_SET_VIEWPORT_OFFSET) / 4;
			pg->vsh_constants[NV_IGRAPH_XF_XFCTX_VPOFF][slot] = parameter;
			pg->vsh_constants_dirty[NV_IGRAPH_XF_XFCTX_VPOFF] = true;
			break;

		CASE_4(NV097_SET_EYE_POSITION, 4):
			slot = (method - NV097_SET_EYE_POSITION) / 4;
			pg->vsh_constants[NV_IGRAPH_XF_XFCTX_EYEP][slot] = parameter;
			pg->vsh_constants_dirty[NV_IGRAPH_XF_XFCTX_EYEP] = true;
			break;

		CASE_8(NV097_SET_COMBINER_FACTOR0, 4):
			slot = (method - NV097_SET_COMBINER_FACTOR0) / 4;
			pg->regs[NV_PGRAPH_COMBINEFACTOR0 + slot * 4] = parameter;
			break;

		CASE_8(NV097_SET_COMBINER_FACTOR1, 4):
			slot = (method - NV097_SET_COMBINER_FACTOR1) / 4;
			pg->regs[NV_PGRAPH_COMBINEFACTOR1 + slot * 4] = parameter;
			break;

		CASE_8(NV097_SET_COMBINER_ALPHA_OCW, 4):
			slot = (method - NV097_SET_COMBINER_ALPHA_OCW) / 4;
			pg->regs[NV_PGRAPH_COMBINEALPHAO0 + slot * 4] = parameter;
			break;

		CASE_8(NV097_SET_COMBINER_COLOR_ICW, 4):
			slot = (method - NV097_SET_COMBINER_COLOR_ICW) / 4;
			pg->regs[NV_PGRAPH_COMBINECOLORI0 + slot * 4] = parameter;
			break;

		CASE_4(NV097_SET_VIEWPORT_SCALE, 4):
			slot = (method - NV097_SET_VIEWPORT_SCALE) / 4;
			pg->vsh_constants[NV_IGRAPH_XF_XFCTX_VPSCL][slot] = parameter;
			pg->vsh_constants_dirty[NV_IGRAPH_XF_XFCTX_VPSCL] = true;
			break;

		CASE_32(NV097_SET_TRANSFORM_PROGRAM, 4) : {

			slot = (method - NV097_SET_TRANSFORM_PROGRAM) / 4;

			int program_load = GET_MASK(pg->regs[NV_PGRAPH_CHEOPS_OFFSET],
				NV_PGRAPH_CHEOPS_OFFSET_PROG_LD_PTR);

			assert(program_load < NV2A_MAX_TRANSFORM_PROGRAM_LENGTH);
			pg->program_data[program_load][slot % 4] = parameter;

			if (slot % 4 == 3) {
				SET_MASK(pg->regs[NV_PGRAPH_CHEOPS_OFFSET],
					NV_PGRAPH_CHEOPS_OFFSET_PROG_LD_PTR, program_load + 1);
			}

			break;
		}

		CASE_32(NV097_SET_TRANSFORM_CONSTANT, 4): {

			slot = (method - NV097_SET_TRANSFORM_CONSTANT) / 4;

			int const_load = GET_MASK(pg->regs[NV_PGRAPH_CHEOPS_OFFSET],
									  NV_PGRAPH_CHEOPS_OFFSET_CONST_LD_PTR);

			assert(const_load < NV2A_VERTEXSHADER_CONSTANTS);
			// VertexShaderConstant *vsh_constant = &pg->vsh_constants[const_load];
			pg->vsh_constants_dirty[const_load] |=
				(parameter != pg->vsh_constants[const_load][slot%4]);
			pg->vsh_constants[const_load][slot%4] = parameter;

			if (slot % 4 == 3) {
				SET_MASK(pg->regs[NV_PGRAPH_CHEOPS_OFFSET],
						 NV_PGRAPH_CHEOPS_OFFSET_CONST_LD_PTR, const_load+1);
			}
			break;
		}

		CASE_3(NV097_SET_VERTEX3F, 4) : {
			slot = (method - NV097_SET_VERTEX3F) / 4;
			VertexAttribute *vertex_attribute =
				&pg->vertex_attributes[NV2A_VERTEX_ATTR_POSITION];
			pgraph_allocate_inline_buffer_vertices(pg, NV2A_VERTEX_ATTR_POSITION);
			vertex_attribute->inline_value[slot] = *(float*)&parameter;
			vertex_attribute->inline_value[3] = 1.0f;
			if (slot == 2) {
				pgraph_finish_inline_buffer_vertex(pg);
			}
			break;
		}

		/* Handles NV097_SET_BACK_LIGHT_* */
		CASE_128(NV097_SET_BACK_LIGHT_AMBIENT_COLOR, 4): {
			slot = (method - NV097_SET_BACK_LIGHT_AMBIENT_COLOR) / 4;
			unsigned int part = NV097_SET_BACK_LIGHT_AMBIENT_COLOR / 4 + slot % 16;
			slot /= 16; /* [Light index] */
			assert(slot < 8);
			switch(part * 4) {
			CASE_3(NV097_SET_BACK_LIGHT_AMBIENT_COLOR, 4):
				part -= NV097_SET_BACK_LIGHT_AMBIENT_COLOR / 4;
				pg->ltctxb[NV_IGRAPH_XF_LTCTXB_L0_BAMB + slot*6][part] = parameter;
				pg->ltctxb_dirty[NV_IGRAPH_XF_LTCTXB_L0_BAMB + slot*6] = true;
				break;
			CASE_3(NV097_SET_BACK_LIGHT_DIFFUSE_COLOR, 4):
				part -= NV097_SET_BACK_LIGHT_DIFFUSE_COLOR / 4;
				pg->ltctxb[NV_IGRAPH_XF_LTCTXB_L0_BDIF + slot*6][part] = parameter;
				pg->ltctxb_dirty[NV_IGRAPH_XF_LTCTXB_L0_BDIF + slot*6] = true;
				break;
			CASE_3(NV097_SET_BACK_LIGHT_SPECULAR_COLOR, 4):
				part -= NV097_SET_BACK_LIGHT_SPECULAR_COLOR / 4;
				pg->ltctxb[NV_IGRAPH_XF_LTCTXB_L0_BSPC + slot*6][part] = parameter;
				pg->ltctxb_dirty[NV_IGRAPH_XF_LTCTXB_L0_BSPC + slot*6] = true;
				break;
			default:
				assert(false);
				break;
			}
			break;
		}
		/* Handles all the light source props except for NV097_SET_BACK_LIGHT_* */
		CASE_256(NV097_SET_LIGHT_AMBIENT_COLOR, 4): {
			slot = (method - NV097_SET_LIGHT_AMBIENT_COLOR) / 4;
			unsigned int part = NV097_SET_LIGHT_AMBIENT_COLOR / 4 + slot % 32;
			slot /= 32; /* [Light index] */
			assert(slot < 8);
			switch(part * 4) {
			CASE_3(NV097_SET_LIGHT_AMBIENT_COLOR, 4):
				part -= NV097_SET_LIGHT_AMBIENT_COLOR / 4;
				pg->ltctxb[NV_IGRAPH_XF_LTCTXB_L0_AMB + slot*6][part] = parameter;
				pg->ltctxb_dirty[NV_IGRAPH_XF_LTCTXB_L0_AMB + slot*6] = true;
				break;
			CASE_3(NV097_SET_LIGHT_DIFFUSE_COLOR, 4):
				part -= NV097_SET_LIGHT_DIFFUSE_COLOR / 4;
				pg->ltctxb[NV_IGRAPH_XF_LTCTXB_L0_DIF + slot*6][part] = parameter;
				pg->ltctxb_dirty[NV_IGRAPH_XF_LTCTXB_L0_DIF + slot*6] = true;
				break;
			CASE_3(NV097_SET_LIGHT_SPECULAR_COLOR, 4):
				part -= NV097_SET_LIGHT_SPECULAR_COLOR / 4;
				pg->ltctxb[NV_IGRAPH_XF_LTCTXB_L0_SPC + slot*6][part] = parameter;
				pg->ltctxb_dirty[NV_IGRAPH_XF_LTCTXB_L0_SPC + slot*6] = true;
				break;
			case NV097_SET_LIGHT_LOCAL_RANGE:
				pg->ltc1[NV_IGRAPH_XF_LTC1_r0 + slot][0] = parameter;
				pg->ltc1_dirty[NV_IGRAPH_XF_LTC1_r0 + slot] = true;
				break;
			CASE_3(NV097_SET_LIGHT_INFINITE_HALF_VECTOR, 4):
				part -= NV097_SET_LIGHT_INFINITE_HALF_VECTOR / 4;
				pg->light_infinite_half_vector[slot][part] = *(float*)&parameter;
				break;
			CASE_3(NV097_SET_LIGHT_INFINITE_DIRECTION, 4):
				part -= NV097_SET_LIGHT_INFINITE_DIRECTION / 4;
				pg->light_infinite_direction[slot][part] = *(float*)&parameter;
				break;
			CASE_3(NV097_SET_LIGHT_SPOT_FALLOFF, 4):
				part -= NV097_SET_LIGHT_SPOT_FALLOFF / 4;
				pg->ltctxa[NV_IGRAPH_XF_LTCTXA_L0_K + slot*2][part] = parameter;
				pg->ltctxa_dirty[NV_IGRAPH_XF_LTCTXA_L0_K + slot*2] = true;
				break;
			CASE_4(NV097_SET_LIGHT_SPOT_DIRECTION, 4):
				part -= NV097_SET_LIGHT_SPOT_DIRECTION / 4;
				pg->ltctxa[NV_IGRAPH_XF_LTCTXA_L0_SPT + slot*2][part] = parameter;
				pg->ltctxa_dirty[NV_IGRAPH_XF_LTCTXA_L0_SPT + slot*2] = true;
				break;
			CASE_3(NV097_SET_LIGHT_LOCAL_POSITION, 4):
				part -= NV097_SET_LIGHT_LOCAL_POSITION / 4;
				pg->light_local_position[slot][part] = *(float*)&parameter;
				break;
			CASE_3(NV097_SET_LIGHT_LOCAL_ATTENUATION, 4):
				part -= NV097_SET_LIGHT_LOCAL_ATTENUATION / 4;
				pg->light_local_attenuation[slot][part] = *(float*)&parameter;
				break;
			default:
				assert(false);
				break;
			}
			break;
		}

		CASE_4(NV097_SET_VERTEX4F, 4): {
			slot = (method - NV097_SET_VERTEX4F) / 4;
			VertexAttribute *vertex_attribute =
				&pg->vertex_attributes[NV2A_VERTEX_ATTR_POSITION];
			pgraph_allocate_inline_buffer_vertices(pg, NV2A_VERTEX_ATTR_POSITION);
			vertex_attribute->inline_value[slot] = *(float*)&parameter;
			if (slot == 3) {
				pgraph_finish_inline_buffer_vertex(pg);
			}
			break;
		}

        case NV097_INVALIDATE_VERTEX_CACHE_FILE:
            // Cxbx XbConvert names 0x1710; Envytools MthdKelvinUnkcf0
            // accepts zero only. nxdk lighting tests use it after changing
            // vertex data. Invalidate both native vertex caches so the next
            // fetch observes guest memory, including packed-CMP conversion.
            // Mid-BEGIN invalidation needs ordered partial-batch submission.
            if (parameter || pg->draw_begin_active)
                Nv2aGlUnknown("vertex cache invalidation parameter %#x or inside BEGIN not provided", parameter);
            Nv2aGlHarvestAll(); ++g_harvest_vtx_invalidate;
            memset(pg->vertex_memory_generation, 0, sizeof(pg->vertex_memory_generation));
            for (auto& attribute : pg->vertex_attributes) attribute.converted_elements=0;
            break;

		CASE_16(NV097_SET_VERTEX_DATA_ARRAY_FORMAT, 4): {

			slot = (method - NV097_SET_VERTEX_DATA_ARRAY_FORMAT) / 4;
			VertexAttribute *vertex_attribute = &pg->vertex_attributes[slot];

			vertex_attribute->format =
				GET_MASK(parameter, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE);
			vertex_attribute->count =
				GET_MASK(parameter, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_SIZE);
			vertex_attribute->stride =
				GET_MASK(parameter, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_STRIDE);

			NV2A_DPRINTF("vertex data array format=%d, count=%d, stride=%d\n",
				vertex_attribute->format,
				vertex_attribute->count,
				vertex_attribute->stride);

			vertex_attribute->gl_count = vertex_attribute->count;

			switch (vertex_attribute->format) {
			case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_D3D:
				vertex_attribute->gl_type = GL_UNSIGNED_BYTE;
				vertex_attribute->gl_normalize = GL_TRUE;
				vertex_attribute->size = 1;
				assert(vertex_attribute->count == 4);
				// https://www.opengl.org/registry/specs/ARB/vertex_array_bgra.txt
				vertex_attribute->gl_count = GL_BGRA;
				vertex_attribute->needs_conversion = false;
				break;
			case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_OGL:
				vertex_attribute->gl_type = GL_UNSIGNED_BYTE;
				vertex_attribute->gl_normalize = GL_TRUE;
				vertex_attribute->size = 1;
				vertex_attribute->needs_conversion = false;
				break;
			case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S1:
				vertex_attribute->gl_type = GL_SHORT;
				vertex_attribute->gl_normalize = GL_TRUE;
				vertex_attribute->size = 2;
				vertex_attribute->needs_conversion = false;
				break;
			case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F:
				vertex_attribute->gl_type = GL_FLOAT;
				vertex_attribute->gl_normalize = GL_FALSE;
				vertex_attribute->size = 4;
				vertex_attribute->needs_conversion = false;
				break;
			case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S32K:
				vertex_attribute->gl_type = GL_SHORT;
				vertex_attribute->gl_normalize = GL_FALSE;
				vertex_attribute->size = 2;
				vertex_attribute->needs_conversion = false;
				break;
			case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_CMP:
				/* 3 signed, normalized components packed in 32-bits. (11,11,10) */
				vertex_attribute->size = 4;
				vertex_attribute->gl_type = GL_FLOAT;
				vertex_attribute->gl_normalize = GL_FALSE;
				vertex_attribute->needs_conversion = true;
				vertex_attribute->converted_size = sizeof(float);
				vertex_attribute->converted_count = 3 * vertex_attribute->count;
				break;
			default:
				fprintf(stderr, "Unknown vertex type: 0x%x\n", vertex_attribute->format);
				assert(false);
				break;
			}

			if (vertex_attribute->needs_conversion) {
				vertex_attribute->converted_elements = 0;
			} else {
				if (vertex_attribute->converted_buffer) {
					g_free(vertex_attribute->converted_buffer);
					vertex_attribute->converted_buffer = NULL;
				}
			}

			break;
		}

		CASE_16(NV097_SET_VERTEX_DATA_ARRAY_OFFSET, 4): {

			slot = (method - NV097_SET_VERTEX_DATA_ARRAY_OFFSET) / 4;

			pg->vertex_attributes[slot].dma_select =
				parameter & 0x80000000;
			pg->vertex_attributes[slot].offset =
				parameter & 0x7fffffff;

			pg->vertex_attributes[slot].converted_elements = 0;

			break;
		}

		case NV097_SET_LOGIC_OP_ENABLE:
			SET_MASK(pg->regs[NV_PGRAPH_BLEND],
					 NV_PGRAPH_BLEND_LOGICOP_ENABLE, parameter);
			break;

		case NV097_SET_LOGIC_OP:
			SET_MASK(pg->regs[NV_PGRAPH_BLEND],
					 NV_PGRAPH_BLEND_LOGICOP, parameter & 0xF);
			break;

		case NV097_CLEAR_REPORT_VALUE:

			/* FIXME: Does this have a value in parameter? Also does this (also?) modify
			 *        the report memory block?
			 */
			if (pg->gl_zpass_pixel_count_query_count) {
				if (pg->opengl_enabled) {
					glDeleteQueries(pg->gl_zpass_pixel_count_query_count,
									pg->gl_zpass_pixel_count_queries);
				}
				pg->gl_zpass_pixel_count_query_count = 0;
			}
			pg->zpass_pixel_count_result = 0;

			break;

		case NV097_SET_ZPASS_PIXEL_COUNT_ENABLE:
			pg->zpass_pixel_count_enable = parameter;
			break;

		case NV097_GET_REPORT:
            // The upstream path used a fixed placeholder timestamp. Do not
            // publish that as hardware time; provide PTIMER report semantics
            // before enabling this command.
            Nv2aGlUnknown("GET_REPORT timestamp/completion semantics not provided (parameter %#x)", parameter);
            break;

		case NV097_SET_BEGIN_END: {
			uint32_t control_0 = pg->regs[NV_PGRAPH_CONTROL_0];
			uint32_t control_1 = pg->regs[NV_PGRAPH_CONTROL_1];

			bool depth_test = control_0
				& NV_PGRAPH_CONTROL_0_ZENABLE;
			bool stencil_test = control_1
				& NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE;

			if (parameter == NV097_SET_BEGIN_END_OP_END) {
                if (!pg->draw_begin_active) Nv2aGlUnknown("END without BEGIN not provided");
                Nv2aProf draw_scope(15);

				if (pg->draw_arrays_length) {

					NV2A_GL_DPRINTF(false, "Draw Arrays");

					assert(pg->inline_buffer_length == 0);
					assert(pg->inline_array_length == 0);
					assert(pg->inline_elements_length == 0);

					if (pgraph_draw_arrays != nullptr) {
						pgraph_draw_arrays(d);
					}
				} else if (pg->inline_buffer_length) {

					NV2A_GL_DPRINTF(false, "Inline Buffer");

					assert(pg->draw_arrays_length == 0);
					assert(pg->inline_array_length == 0);
					assert(pg->inline_elements_length == 0);

					if (pgraph_draw_inline_buffer != nullptr) {
						pgraph_draw_inline_buffer(d);
					}
				} else if (pg->inline_array_length) {

					NV2A_GL_DPRINTF(false, "Inline Array");

					assert(pg->draw_arrays_length == 0);
					assert(pg->inline_buffer_length == 0);
					assert(pg->inline_elements_length == 0);

					if (pgraph_draw_inline_array != nullptr) {
						pgraph_draw_inline_array(d);
					}
				} else if (pg->inline_elements_length) {

					NV2A_GL_DPRINTF(false, "Inline Elements");

					assert(pg->draw_arrays_length == 0);
					assert(pg->inline_buffer_length == 0);
					assert(pg->inline_array_length == 0);

					if (pgraph_draw_inline_elements != nullptr) {
						pgraph_draw_inline_elements(d);
					}
				} else {
                    // Zero submitted vertices produce no primitives. As xemu
                    // 36334d95 gl/draw.c, complete visibility-query bookkeeping
                    // even for an empty batch; do not invent a draw or dirty RAM.
                    if (pg->opengl_enabled) OpenGL_draw_end(d);
                    pg->draw_begin_active=false;
                    break;
				}
                pg->draw_begin_active=false;
                ++g_nv2a_render_serial;
                pgraph_set_surface_dirty(pg, true, depth_test || stencil_test);
            } else {
                if (pg->draw_begin_active) Nv2aGlUnknown("nested BEGIN not provided");

                assert(parameter <= NV097_SET_BEGIN_END_OP_POLYGON);

				pg->primitive_mode = parameter;
                pg->draw_begin_active=true;

				if (pgraph_draw_state_update != nullptr) {
					pgraph_draw_state_update(d);
				}

				pg->inline_elements_length = 0;
				pg->inline_array_length = 0;
				pg->inline_buffer_length = 0;
				pg->draw_arrays_length = 0;
				pg->draw_arrays_max_count = 0;
			}

			break;
		}
		CASE_4(NV097_SET_TEXTURE_OFFSET, 64):
			slot = (method - NV097_SET_TEXTURE_OFFSET) / 64;
			pg->regs[NV_PGRAPH_TEXOFFSET0 + slot * 4] = parameter;
			pg->texture_dirty[slot] = true;
			break;
		CASE_4(NV097_SET_TEXTURE_FORMAT, 64): {
			slot = (method - NV097_SET_TEXTURE_FORMAT) / 64;

			bool dma_select =
				GET_MASK(parameter, NV097_SET_TEXTURE_FORMAT_CONTEXT_DMA) == 2;
			bool cubemap =
				parameter & NV097_SET_TEXTURE_FORMAT_CUBEMAP_ENABLE;
			bool border_source =
				parameter & NV097_SET_TEXTURE_FORMAT_BORDER_SOURCE;
			unsigned int dimensionality =
				GET_MASK(parameter, NV097_SET_TEXTURE_FORMAT_DIMENSIONALITY);
			unsigned int color_format =
				GET_MASK(parameter, NV097_SET_TEXTURE_FORMAT_COLOR);
			unsigned int levels =
				GET_MASK(parameter, NV097_SET_TEXTURE_FORMAT_MIPMAP_LEVELS);
			unsigned int log_width =
				GET_MASK(parameter, NV097_SET_TEXTURE_FORMAT_BASE_SIZE_U);
			unsigned int log_height =
				GET_MASK(parameter, NV097_SET_TEXTURE_FORMAT_BASE_SIZE_V);
			unsigned int log_depth =
				GET_MASK(parameter, NV097_SET_TEXTURE_FORMAT_BASE_SIZE_P);

			uint32_t *reg = &pg->regs[NV_PGRAPH_TEXFMT0 + slot * 4];
			SET_MASK(*reg, NV_PGRAPH_TEXFMT0_CONTEXT_DMA, dma_select);
			SET_MASK(*reg, NV_PGRAPH_TEXFMT0_CUBEMAPENABLE, cubemap);
			SET_MASK(*reg, NV_PGRAPH_TEXFMT0_BORDER_SOURCE, border_source);
			SET_MASK(*reg, NV_PGRAPH_TEXFMT0_DIMENSIONALITY, dimensionality);
			SET_MASK(*reg, NV_PGRAPH_TEXFMT0_COLOR, color_format);
			SET_MASK(*reg, NV_PGRAPH_TEXFMT0_MIPMAP_LEVELS, levels);
			SET_MASK(*reg, NV_PGRAPH_TEXFMT0_BASE_SIZE_U, log_width);
			SET_MASK(*reg, NV_PGRAPH_TEXFMT0_BASE_SIZE_V, log_height);
			SET_MASK(*reg, NV_PGRAPH_TEXFMT0_BASE_SIZE_P, log_depth);

			pg->texture_dirty[slot] = true;
			break;
		}
		CASE_4(NV097_SET_TEXTURE_CONTROL0, 64):
			slot = (method - NV097_SET_TEXTURE_CONTROL0) / 64;
			pg->regs[NV_PGRAPH_TEXCTL0_0 + slot*4] = parameter;
			break;
		CASE_4(NV097_SET_TEXTURE_CONTROL1, 64):
			slot = (method - NV097_SET_TEXTURE_CONTROL1) / 64;
			pg->regs[NV_PGRAPH_TEXCTL1_0 + slot*4] = parameter;
			break;
		CASE_4(NV097_SET_TEXTURE_FILTER, 64):
			slot = (method - NV097_SET_TEXTURE_FILTER) / 64;
			pg->regs[NV_PGRAPH_TEXFILTER0 + slot * 4] = parameter;
			break;
		CASE_4(NV097_SET_TEXTURE_IMAGE_RECT, 64):
			slot = (method - NV097_SET_TEXTURE_IMAGE_RECT) / 64;
			pg->regs[NV_PGRAPH_TEXIMAGERECT0 + slot * 4] = parameter;
			pg->texture_dirty[slot] = true;
			break;
		CASE_4(NV097_SET_TEXTURE_PALETTE, 64): {
			slot = (method - NV097_SET_TEXTURE_PALETTE) / 64;

			bool dma_select =
				GET_MASK(parameter, NV097_SET_TEXTURE_PALETTE_CONTEXT_DMA) == 1;
			unsigned int length =
				GET_MASK(parameter, NV097_SET_TEXTURE_PALETTE_LENGTH);
			unsigned int offset =
				GET_MASK(parameter, NV097_SET_TEXTURE_PALETTE_OFFSET);

			uint32_t *reg = &pg->regs[NV_PGRAPH_TEXPALETTE0 + slot * 4];
			SET_MASK(*reg, NV_PGRAPH_TEXPALETTE0_CONTEXT_DMA, dma_select);
			SET_MASK(*reg, NV_PGRAPH_TEXPALETTE0_LENGTH, length);
			SET_MASK(*reg, NV_PGRAPH_TEXPALETTE0_OFFSET, offset);

			pg->texture_dirty[slot] = true;
			break;
		}

		CASE_4(NV097_SET_TEXTURE_BORDER_COLOR, 64):
			slot = (method - NV097_SET_TEXTURE_BORDER_COLOR) / 64;
			pg->regs[NV_PGRAPH_BORDERCOLOR0 + slot * 4] = parameter;
			break;
		CASE_4(NV097_SET_TEXTURE_SET_BUMP_ENV_MAT + 0x0, 64):
		CASE_4(NV097_SET_TEXTURE_SET_BUMP_ENV_MAT + 0x4, 64):
		CASE_4(NV097_SET_TEXTURE_SET_BUMP_ENV_MAT + 0x8, 64):
		CASE_4(NV097_SET_TEXTURE_SET_BUMP_ENV_MAT + 0xc, 64):
			slot = (method - NV097_SET_TEXTURE_SET_BUMP_ENV_MAT) / 4;
			assert((slot / 16) > 0);
			slot -= 16;
			pg->bump_env_matrix[slot / 16][slot % 4] = *(float*)&parameter;
			break;

		CASE_4(NV097_SET_TEXTURE_SET_BUMP_ENV_SCALE, 64):
			slot = (method - NV097_SET_TEXTURE_SET_BUMP_ENV_SCALE) / 64;
			assert(slot > 0);
			slot--;
			pg->regs[NV_PGRAPH_BUMPSCALE1 + slot * 4] = parameter;
			break;
		CASE_4(NV097_SET_TEXTURE_SET_BUMP_ENV_OFFSET, 64):
			slot = (method - NV097_SET_TEXTURE_SET_BUMP_ENV_OFFSET) / 64;
			assert(slot > 0);
			slot--;
			pg->regs[NV_PGRAPH_BUMPOFFSET1 + slot * 4] = parameter;
			break;

		case NV097_ARRAY_ELEMENT16:
			//LOG_TEST_CASE("NV2A_VB_ELEMENT_U16");	
			// Test-case : Turok (in main menu)	
			// Test-case : Hunter Redeemer	
			// Test-case : Otogi (see https://github.com/Cxbx-Reloaded/Cxbx-Reloaded/pull/1113#issuecomment-385593814)
			assert(pg->inline_elements_length < NV2A_MAX_BATCH_LENGTH);
			pg->inline_elements[
				pg->inline_elements_length++] = parameter & 0xFFFF;
			pg->inline_elements[
				pg->inline_elements_length++] = parameter >> 16;
			break;
		case NV097_ARRAY_ELEMENT32:
			//LOG_TEST_CASE("NV2A_VB_ELEMENT_U32");	
			// Test-case : Turok (in main menu)
			assert(pg->inline_elements_length < NV2A_MAX_BATCH_LENGTH);
			pg->inline_elements[
				pg->inline_elements_length++] = parameter;
			break;
		case NV097_DRAW_ARRAYS: {

			unsigned int start = GET_MASK(parameter, NV097_DRAW_ARRAYS_START_INDEX);
			unsigned int count = GET_MASK(parameter, NV097_DRAW_ARRAYS_COUNT)+1;

			pg->draw_arrays_max_count = MAX(pg->draw_arrays_max_count, start + count);

			assert(pg->draw_arrays_length < ARRAY_SIZE(pg->gl_draw_arrays_start));

			/* Attempt to connect primitives */
			if (pg->draw_arrays_length > 0) {
				unsigned int last_start =
					pg->gl_draw_arrays_start[pg->draw_arrays_length - 1];
				GLsizei* last_count =
					&pg->gl_draw_arrays_count[pg->draw_arrays_length - 1];
				if (start == (last_start + *last_count)) {
					*last_count += count;
					break;
				}
			}

			pg->gl_draw_arrays_start[pg->draw_arrays_length] = start;
			pg->gl_draw_arrays_count[pg->draw_arrays_length] = count;
			pg->draw_arrays_length++;
			break;
		}
		case NV097_INLINE_ARRAY:
			assert(pg->inline_array_length < NV2A_MAX_BATCH_LENGTH);
			pg->inline_array[
				pg->inline_array_length++] = parameter;
			break;
		CASE_3(NV097_SET_EYE_VECTOR, 4):
			slot = (method - NV097_SET_EYE_VECTOR) / 4;
			pg->regs[NV_PGRAPH_EYEVEC0 + slot * 4] = parameter;
			break;

		CASE_32(NV097_SET_VERTEX_DATA2F_M, 4): {
			slot = (method - NV097_SET_VERTEX_DATA2F_M) / 4;
			unsigned int part = slot % 2;
			slot /= 2;
			VertexAttribute *vertex_attribute = &pg->vertex_attributes[slot];
			pgraph_allocate_inline_buffer_vertices(pg, slot);
			vertex_attribute->inline_value[part] = *(float*)&parameter;
			/* FIXME: Should these really be set to 0.0 and 1.0 ? Conditions? */
			vertex_attribute->inline_value[2] = 0.0f;
			vertex_attribute->inline_value[3] = 1.0f;
			if ((slot == 0) && (part == 1)) {
				pgraph_finish_inline_buffer_vertex(pg);
			}
			break;
		}
		CASE_64(NV097_SET_VERTEX_DATA4F_M, 4): {
			slot = (method - NV097_SET_VERTEX_DATA4F_M) / 4;
			unsigned int part = slot % 4;
			slot /= 4;
			VertexAttribute *vertex_attribute = &pg->vertex_attributes[slot];
			pgraph_allocate_inline_buffer_vertices(pg, slot);
			vertex_attribute->inline_value[part] = *(float*)&parameter;
			if ((slot == 0) && (part == 3)) {
				pgraph_finish_inline_buffer_vertex(pg);
			}
			break;
		}
		CASE_16(NV097_SET_VERTEX_DATA2S, 4): {
			slot = (method - NV097_SET_VERTEX_DATA2S) / 4;
			assert(false); /* FIXME: Untested! */
			VertexAttribute *vertex_attribute = &pg->vertex_attributes[slot];
			pgraph_allocate_inline_buffer_vertices(pg, slot);
			vertex_attribute->inline_value[0] = (float)(int16_t)(parameter & 0xFFFF);
			vertex_attribute->inline_value[1] = (float)(int16_t)(parameter >> 16);
			vertex_attribute->inline_value[2] = 0.0f;
			vertex_attribute->inline_value[3] = 1.0f;
			if (slot == 0) {
				pgraph_finish_inline_buffer_vertex(pg);
			}
			break;
		}
		CASE_16(NV097_SET_VERTEX_DATA4UB, 4) : {
			slot = (method - NV097_SET_VERTEX_DATA4UB) / 4;
			VertexAttribute *vertex_attribute = &pg->vertex_attributes[slot];
			pgraph_allocate_inline_buffer_vertices(pg, slot);
			vertex_attribute->inline_value[0] = (parameter & 0xFF) / 255.0f;
			vertex_attribute->inline_value[1] = ((parameter >> 8) & 0xFF) / 255.0f;
			vertex_attribute->inline_value[2] = ((parameter >> 16) & 0xFF) / 255.0f;
			vertex_attribute->inline_value[3] = ((parameter >> 24) & 0xFF) / 255.0f;
			if (slot == 0) {
				pgraph_finish_inline_buffer_vertex(pg);
				assert(false); /* FIXME: Untested */
			}
			break;
		}
		CASE_32(NV097_SET_VERTEX_DATA4S_M, 4) : {
			slot = (method - NV097_SET_VERTEX_DATA4S_M) / 4;
			unsigned int part = slot % 2;
			slot /= 2;
			assert(false); /* FIXME: Untested! */
			VertexAttribute *vertex_attribute = &pg->vertex_attributes[slot];
			pgraph_allocate_inline_buffer_vertices(pg, slot);
			/* FIXME: Is mapping to [-1,+1] correct? */
			vertex_attribute->inline_value[part * 2 + 0] = ((int16_t)(parameter & 0xFFFF)
														 * 2.0f + 1) / 65535.0f;
			vertex_attribute->inline_value[part * 2 + 1] = ((int16_t)(parameter >> 16)
														 * 2.0f + 1) / 65535.0f;
			if ((slot == 0) && (part == 1)) {
				pgraph_finish_inline_buffer_vertex(pg);
			}
			break;
		}
		case NV097_SET_SEMAPHORE_OFFSET:
			pg->regs[NV_PGRAPH_SEMAPHOREOFFSET] = parameter;
			break;
		case NV097_BACK_END_WRITE_SEMAPHORE_RELEASE: {
			{ g_download_site = 2974; pgraph_update_surface(d, false, true, true); g_download_site = 0; }

			//qemu_mutex_unlock(&pg->pgraph_lock);
			//qemu_mutex_lock_iothread();

			uint32_t semaphore_offset = pg->regs[NV_PGRAPH_SEMAPHOREOFFSET];

			xbox::addr_xt semaphore_dma_len;
			uint8_t *semaphore_data = (uint8_t*)nv_dma_map(d, pg->dma_semaphore,
				&semaphore_dma_len);
			assert(semaphore_offset < semaphore_dma_len);
			semaphore_data += semaphore_offset;

			stl_le_p((uint32_t*)semaphore_data, parameter);

			//qemu_mutex_lock(&pg->pgraph_lock);
			//qemu_mutex_unlock_iothread();

			break;
		}
		case NV097_SET_ZSTENCIL_CLEAR_VALUE:
			pg->regs[NV_PGRAPH_ZSTENCILCLEARVALUE] = parameter;
			break;

		case NV097_SET_COLOR_CLEAR_VALUE:
			pg->regs[NV_PGRAPH_COLORCLEARVALUE] = parameter;
			break;

		case NV097_CLEAR_SURFACE: {
			pg->clear_surface = parameter;
			if (pgraph_draw_clear != nullptr) {
				pgraph_draw_clear(d);
			}
			break;
		}

		case NV097_SET_CLEAR_RECT_HORIZONTAL:
			pg->regs[NV_PGRAPH_CLEARRECTX] = parameter;
			break;
		case NV097_SET_CLEAR_RECT_VERTICAL:
			pg->regs[NV_PGRAPH_CLEARRECTY] = parameter;
			break;

		CASE_2(NV097_SET_SPECULAR_FOG_FACTOR, 4) :
			slot = (method - NV097_SET_SPECULAR_FOG_FACTOR) / 4;
			pg->regs[NV_PGRAPH_SPECFOGFACTOR0 + slot * 4] = parameter;
			break;

		case NV097_SET_SHADER_CLIP_PLANE_MODE:
			pg->regs[NV_PGRAPH_SHADERCLIPMODE] = parameter;
			break;

		CASE_8(NV097_SET_COMBINER_COLOR_OCW, 4) :
			slot = (method - NV097_SET_COMBINER_COLOR_OCW) / 4;
			pg->regs[NV_PGRAPH_COMBINECOLORO0 + slot * 4] = parameter;
			break;

		case NV097_SET_COMBINER_CONTROL:
			pg->regs[NV_PGRAPH_COMBINECTL] = parameter;
			break;

		case NV097_SET_SHADOW_ZSLOPE_THRESHOLD:
			pg->regs[NV_PGRAPH_SHADOWZSLOPETHRESHOLD] = parameter;
			assert(parameter == 0x7F800000); /* FIXME: Unimplemented */
			break;

		case NV097_SET_SHADER_STAGE_PROGRAM:
			pg->regs[NV_PGRAPH_SHADERPROG] = parameter;
			break;

		case NV097_SET_SHADER_OTHER_STAGE_INPUT:
			// Native port: xemu 36334d95 keeps SHADERCTL bits 0..11 for the dot
			// mapping (method 0x1e74); the inherited code overwrote them.
			SET_MASK(pg->regs[NV_PGRAPH_SHADERCTL], 0xFFFF000u, GET_MASK(parameter, 0xFFFF000u));
			break;
		case 0x1e74: // NV097_SET_DOT_RGBMAPPING (xemu 36334d95)
			// Envytools f102b823 MthdKelvinTexShaderDotmapping: class 0x97 accepts
			// three 3-bit maps (mask 0x777); maps >= 11 invalid (cannot occur
			// within 3 bits); maps 5/6 additionally need DEBUG_B bits, rejected.
			if (parameter & ~0x777u) Nv2aGlUnknown("invalid DOT_RGBMAPPING parameter %#x", parameter);
			for (int stage = 0; stage < 3; stage++) {
				unsigned map = (parameter >> (stage * 4)) & 7u;
				if (map == 5 || map == 6) Nv2aGlUnknown("DOT_RGBMAPPING %#x uses debug-gated map %u", parameter, map);
			}
			SET_MASK(pg->regs[NV_PGRAPH_SHADERCTL], 0xFFFu, parameter);
			break;

		case NV097_SET_TRANSFORM_EXECUTION_MODE:
			// Test-case : Whiplash
			SET_MASK(pg->regs[NV_PGRAPH_CSV0_D], NV_PGRAPH_CSV0_D_MODE,
				GET_MASK(parameter,
					NV097_SET_TRANSFORM_EXECUTION_MODE_MODE));
			SET_MASK(pg->regs[NV_PGRAPH_CSV0_D], NV_PGRAPH_CSV0_D_RANGE_MODE,
				GET_MASK(parameter,
					NV097_SET_TRANSFORM_EXECUTION_MODE_RANGE_MODE));
			break;
		case NV097_SET_TRANSFORM_PROGRAM_CXT_WRITE_EN:
			// Test-case : Whiplash
			pg->enable_vertex_program_write = parameter;
			break;
		case NV097_SET_TRANSFORM_PROGRAM_LOAD:
			assert(parameter < NV2A_MAX_TRANSFORM_PROGRAM_LENGTH);
			SET_MASK(pg->regs[NV_PGRAPH_CHEOPS_OFFSET],
				NV_PGRAPH_CHEOPS_OFFSET_PROG_LD_PTR, parameter);
			break;
		case NV097_SET_TRANSFORM_PROGRAM_START:
			assert(parameter < NV2A_MAX_TRANSFORM_PROGRAM_LENGTH);
			SET_MASK(pg->regs[NV_PGRAPH_CSV0_C],
				NV_PGRAPH_CSV0_C_CHEOPS_PROGRAM_START, parameter);
			break;
		case NV097_SET_TRANSFORM_CONSTANT_LOAD:
			assert(parameter < NV2A_VERTEXSHADER_CONSTANTS);
			SET_MASK(pg->regs[NV_PGRAPH_CHEOPS_OFFSET],
				NV_PGRAPH_CHEOPS_OFFSET_CONST_LD_PTR, parameter);
			NV2A_DPRINTF("load to %d\n", parameter);
			break;

        case 0x17c4: // NV097_SET_LIGHT_TWO_SIDE_ENABLE (nxdk 14d5ee97)
            // Envytools f102b823 MthdKelvinLightTwoSideEnable: values 0/1 into
            // BUNDLE_RASTER bit24 (and XF mode, consumed by the transform). The
            // native pixel shader selects back colours (B0/B1) for back-facing
            // primitives when it is set (nv2a_psh.cpp). Fixed-function lighting,
            // which would have to compute the back colours, is refused separately.
            if (parameter > 1) Nv2aGlUnknown("invalid LIGHT_TWO_SIDE_ENABLE parameter %#x", parameter);
            SET_MASK(pg->regs[NV_PGRAPH_SETUPRASTER], 1u << 24, parameter);
            break;
        case 0x1d84: // NV2A_OCCLUDE_ZSTENCIL_EN (Cxbx 585c49a XbConvert.h: D3DRS_OCCLUSIONCULLENABLE/STENCILCULLENABLE)
            // Envytools f102b823 MthdKelvinUnk1d84: class 0x97 accepts bits 0..1,
            // latched into Z_CONFIG bits 1..2. These enable the hierarchical
            // (compression-tag) early rejection of fragments that would fail the
            // depth or stencil test anyway; such culling is conservative and so
            // output-invariant, and both reference renderers (Cxbx 585c49a, xemu
            // 36334d95) render without it. The per-fragment tests are always
            // performed by GL, so the latch has no further consumer here.
            if (parameter & ~3u) Nv2aGlUnknown("invalid OCCLUDE_ZSTENCIL_EN parameter %#x", parameter);
            SET_MASK(pg->regs[NV_PGRAPH_ZCOMPRESSOCCLUDE], 6u, parameter);
            break;
        case 0x31c: case 0x320: case 0x324: // NV097_SET_POINT/LINE/POLY_SMOOTH_ENABLE (xemu 36334d95)
            // Envytools f102b823 MthdKelvin{Point,Line,Polygon}SmoothEnable:
            // values 0/1 into BUNDLE_RASTER bits 9/10/11 (xemu's SETUPRASTER
            // *SMOOTHENABLE). Antialiased primitives are not rendered by the
            // port; an enabled bit stops at raster time.
            if (parameter > 1) Nv2aGlUnknown("invalid smooth-enable parameter %#x for method %#x", parameter, method);
            SET_MASK(pg->regs[NV_PGRAPH_SETUPRASTER], 1u << (9 + (method - 0x31c) / 4), parameter);
            break;
        case 0x1e6c: // NV097_SET_SHADOW_COMPARE_FUNC (nxdk 14d5ee97; xemu SET_SHADOW_DEPTH_FUNC)
            // Envytools f102b823 MthdKelvinTexZcomp: class 0x97 accepts 0..7.
            // Stored as xemu's SHADOWCTL_SHADOW_ZFUNC. It is consumed only when a
            // depth-format texture is sampled, which stops at texture binding.
            if (parameter & ~7u) Nv2aGlUnknown("invalid SHADOW_COMPARE_FUNC parameter %#x", parameter);
            SET_MASK(pg->regs[NV_PGRAPH_SHADOWCTL], 7u, parameter);
            break;
        case 0xae0: case 0xae4: case 0xae8: case 0xaec: // NV097_SET_COLOR_KEY_COLOR(i)
            // Envytools f102b823 MthdKelvinTexColorKey: any value, per-stage
            // BUNDLE_TEX_COLOR_KEY; stored as xemu's NV_PGRAPH_COLORKEYCOLOR0+4i
            // (0x1870). Consumed only by a texture colour-key mode, which stops.
            pg->regs[0x1870 + (method - 0xae0)] = parameter;
            break;
        case 0x294: // NV097_SET_LIGHT_CONTROL (xemu 36334d95 SET_LIGHT_CONTROL)
            // xemu: bit0 SEPARATE_SPECULAR -> CSV0_C bit18, bit16 LOCALEYE ->
            // CSV0_C bit30, bit17 ALPHA_FROM_MATERIAL_SPECULAR -> CSV0_C bit17.
            // Other bits are not established: stop. Consumed only by
            // fixed-function lighting, which stops at shader generation.
            if (parameter & ~0x30001u) Nv2aGlUnknown("LIGHT_CONTROL parameter %#x not provided", parameter);
            SET_MASK(pg->regs[NV_PGRAPH_CSV0_C], 1u << 18, parameter & 1u);
            SET_MASK(pg->regs[NV_PGRAPH_CSV0_C], 1u << 30, (parameter >> 16) & 1u);
            SET_MASK(pg->regs[NV_PGRAPH_CSV0_C], 1u << 17, (parameter >> 17) & 1u);
            break;
        case 0x3b8: // NV097_SET_SPECULAR_ENABLE (nxdk 14d5ee97)
            // Envytools f102b823 MthdKelvinSpecularEnable: values 0/1 (CONFIG_B
            // bit5, XF mode A bit16); stored as xemu's CSV0_C_SPECULAR_ENABLE
            // (bit16) and consumed by the vertex shader outputs.
            if (parameter > 1) Nv2aGlUnknown("invalid SPECULAR_ENABLE parameter %#x", parameter);
            SET_MASK(pg->regs[NV_PGRAPH_CSV0_C], 1u << 16, parameter);
            break;
        case 0x43c: // NV097_SET_POINT_SIZE (nxdk 14d5ee97)
            // Envytools f102b823 MthdKelvinPointSize: class 0x97 accepts values
            // below 0x200 (unsigned 6.3 pixels), stored as xemu's POINTSIZE.
            // Point rasterisation size is not wired into the port: drawing
            // points stops (see pgraph_require_depth_control_semantics).
            if (parameter >= 0x200) Nv2aGlUnknown("invalid POINT_SIZE parameter %#x", parameter);
            pg->regs[0x198c] = parameter;
            break;
        case 0x318: // NV097_SET_POINT_PARAMS_ENABLE (nxdk 14d5ee97)
            // Envytools f102b823 MthdKelvinPointParamsEnable: values 0/1 into
            // BUNDLE_CONFIG_B bit9 and XF mode B bit25. Point size attenuation is
            // not provided: drawing points while enabled stops.
            if (parameter > 1) Nv2aGlUnknown("invalid POINT_PARAMS_ENABLE parameter %#x", parameter);
            pg->point_params_enable = parameter;
            break;
        case 0x147c: // NV097_SET_STIPPLE_ENABLE (nxdk 14d5ee97)
            // Envytools f102b823 MthdKelvinPolygonStippleEnable: class 0x97
            // accepts 0 or 1 (the bundle bit location differs by chipset, so it
            // is kept as a field). Polygon stipple is not rendered: enabling it
            // stops at the next raster submission.
            if (parameter > 1) Nv2aGlUnknown("invalid STIPPLE_ENABLE parameter %#x", parameter);
            pg->polygon_stipple_enable = parameter;
            break;
        case NV097_SET_ZMIN_MAX_CONTROL:
            // Envytools f102b823 MthdKelvinDepthClamp: class 0x97 accepts only
            // bits 0, 4 and 8; others raise a data error (not modelled: stop).
            if (parameter & ~0x111u)
                Nv2aGlUnknown("invalid ZMIN_MAX_CONTROL parameter %#x", parameter);
            SET_MASK(pg->regs[NV_PGRAPH_SETUPRASTER], 1u << 30, parameter & 1u);
            SET_MASK(pg->regs[NV_PGRAPH_SETUPRASTER], 1u << 5, (parameter >> 8) & 1u);
            SET_MASK(pg->regs[NV_PGRAPH_ZCOMPRESSOCCLUDE], 1u << 4, (parameter >> 4) & 1u);
            pg->zmin_max_control_written = true;
            pg->zmin_max_control_parameter = parameter;
            break;
        case 0x9f8: // NV097_SET_SWATH_WIDTH
            // Envytools f102b823 MthdKelvinUnk3f0 establishes the complete
            // class-0x97 input domain and BUNDLE_RASTER bits25..27. Pixel
            // semantics are still unresolved; every host raster entry point
            // stops before consuming this state (including the value 0xf).
            if (parameter > 4 && parameter != 15)
                Nv2aGlUnknown("invalid SWATH_WIDTH parameter %#x", parameter);
            SET_MASK(pg->regs[NV_PGRAPH_SETUPRASTER], 0x0e000000u, parameter & 7u);
            pg->swath_width_written = true;
            pg->swath_width_parameter = parameter;
            pg->swath_width_subchannel = subchannel;
            break;
        case 0x37c: // NV097_SET_SHADE_MODE
            if (parameter != GL_SMOOTH) Nv2aGlUnknown("shade mode %#x not provided", parameter);
            // Envytools: BUNDLE_CONFIG_B bit7. The shader generator interpolates
            // D0/D1/B0/B1 and divides by interpolated inv_w, i.e. smooth shading.
            SET_MASK(pg->regs[NV_PGRAPH_CONTROL_3], 0x80u, 1);
            glShadeModel(GL_SMOOTH);
            pgraph_debug_check_gl(__LINE__);   // per-frame check otherwise
            break;
        case 0x1d7c: // NV097_SET_MULTISAMPLE_CONTROL
            // Envytools BUNDLE_MULTISAMPLE at 0x19bc. Disabled multisampling
            // has no sample-mask effect; enabled/alpha coverage needs a real
            // multisample backing and remains unsupported here.
            if ((parameter & ~0xffff0000u) != 0)
                Nv2aGlUnknown("multisample/alpha coverage %#x not provided", parameter);
            pg->regs[0x19bc] = parameter;
            glDisable(GL_MULTISAMPLE);
            glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
            glDisable(GL_SAMPLE_ALPHA_TO_ONE);
            pgraph_debug_check_gl(__LINE__);   // per-frame check otherwise
            break;
        case 0x380: // NV097_SET_LINE_WIDTH: unsigned 6.3 fixed point
            if (parameter > 0x1ff) Nv2aGlUnknown("line width %#x out of range", parameter);
            // Envytools MthdKelvinLineWidth -> BUNDLE_RASTER bits 12..20.
            SET_MASK(pg->regs[NV_PGRAPH_SETUPRASTER], 0x001ff000u, parameter);
            break;
        case NV097_SET_COMPRESS_ZBUFFER_EN:
            if (parameter > 1) Nv2aGlUnknown("Z compression enable %#x not provided", parameter);
            // Envytools: BUNDLE_Z_CONFIG (PGRAPH 0x1a84), bit 0. Logical
            // expanded Z24S8 backing is validated by pgraph_validate_depth_tile;
            // compression changes physical storage, not depth-test results.
            SET_MASK(pg->regs[0x1a84], 1u, parameter);
            break;
        case 0x16bc: // NV097_SET_EDGE_FLAG (envytools nv20_3d.xml)
            // Geometry generation emits all boundary edges. That exactly covers
            // true; false would require per-vertex edge suppression there.
            if (parameter != 1) Nv2aGlUnknown("edge suppression value %#x not provided", parameter);
            glEdgeFlag(GL_TRUE);
            pgraph_debug_check_gl(__LINE__);   // per-frame check otherwise
            break;
		case NV097_SET_FLAT_SHADE_OP: 
			assert(parameter <= 1);
            // Envytools MthdKelvinFlatshadeFirst: 1 selects the first vertex.
            // This sets actual host rasterizer state. Flat interpolation itself
            // is a separate shade-mode method, still unsupported by this port.
            SET_MASK(pg->regs[NV_PGRAPH_CONTROL_3], 1u, parameter); // BUNDLE_CONFIG_B bit 0
            glProvokingVertex(parameter ? GL_FIRST_VERTEX_CONVENTION : GL_LAST_VERTEX_CONVENTION);
            pgraph_debug_check_gl(__LINE__);   // per-frame check otherwise
			break;
		default:
			Nv2aGlUnknown("unhandled graphics class %#x method %#x parameter %#x", graphics_class, method, parameter);
			break;
		}
		break;
	}

	default:
		Nv2aGlUnknown("unknown graphics class %#x method %#x parameter %#x", graphics_class, method, parameter);
		break;
	}

}

static void pgraph_switch_context(NV2AState *d, unsigned int channel_id)
{
    bool channel_valid =
        d->pgraph.regs[NV_PGRAPH_CTX_CONTROL] & NV_PGRAPH_CTX_CONTROL_CHID;
    unsigned pgraph_channel_id = GET_MASK(d->pgraph.regs[NV_PGRAPH_CTX_USER], NV_PGRAPH_CTX_USER_CHID);
	// Cxbx Note : This isn't present in xqemu / OpenXbox : d->pgraph.pgraph_lock.lock();
    bool valid = channel_valid && pgraph_channel_id == channel_id;
	if (!valid) {
        SET_MASK(d->pgraph.regs[NV_PGRAPH_TRAPPED_ADDR],
                 NV_PGRAPH_TRAPPED_ADDR_CHID, channel_id);

        NV2A_DPRINTF("pgraph switching to ch %d\n", channel_id);

        /* TODO: hardware context switching */
        assert(!(d->pgraph.regs[NV_PGRAPH_DEBUG_3]
                & NV_PGRAPH_DEBUG_3_HW_CONTEXT_SWITCH));

		qemu_mutex_unlock(&d->pgraph.pgraph_lock);
		qemu_mutex_lock_iothread();
		d->pgraph.pending_interrupts |= NV_PGRAPH_INTR_CONTEXT_SWITCH; // TODO : Should this be done before unlocking pgraph_lock?
		update_irq(d);

		qemu_mutex_lock(&d->pgraph.pgraph_lock);
		qemu_mutex_unlock_iothread();

        // wait for the interrupt to be serviced
		while (d->pgraph.pending_interrupts & NV_PGRAPH_INTR_CONTEXT_SWITCH) {
			qemu_cond_wait(&d->pgraph.interrupt_cond, &d->pgraph.pgraph_lock);
		}
	}
}

static void pgraph_wait_fifo_access(NV2AState *d);
// Native port: bulk SET_TRANSFORM_CONSTANT words for consecutive increasing
// methods (pusher fast path). The first word takes the ordinary path (context
// switch registers, FIFO access); the rest repeat its constant-load effect
// without a dispatch each (~64% of all method words in busy rooms).
void pgraph_load_transform_constants(NV2AState *d, unsigned subchannel, uint32_t method, const uint32_t* words, uint32_t count) {
    PGRAPHState *pg = &d->pgraph;
    const bool program = method >= NV097_SET_TRANSFORM_PROGRAM && method < NV097_SET_TRANSFORM_PROGRAM + 0x80;
    const uint32_t base = program ? NV097_SET_TRANSFORM_PROGRAM : NV097_SET_TRANSFORM_CONSTANT;
    if (method < base || method + 4 * (count - 1) >= base + 0x80 || method % 4)
        Nv2aGlUnknown("bulk transform words %#x x%u outside the method range", method, count);
    pgraph_wait_fifo_access(d);
    pgraph_handle_method(d, subchannel, method, words[0]);
    if (program) {   // SET_TRANSFORM_PROGRAM: the same effect for the remaining words
        for (uint32_t i = 1; i < count; ++i) {
            unsigned slot = (method + 4 * i - base) / 4;
            int program_load = GET_MASK(pg->regs[NV_PGRAPH_CHEOPS_OFFSET], NV_PGRAPH_CHEOPS_OFFSET_PROG_LD_PTR);
            if (program_load >= NV2A_MAX_TRANSFORM_PROGRAM_LENGTH) Nv2aGlUnknown("transform program load pointer %d out of range", program_load);
            pg->program_data[program_load][slot % 4] = words[i];
            if (slot % 4 == 3) SET_MASK(pg->regs[NV_PGRAPH_CHEOPS_OFFSET], NV_PGRAPH_CHEOPS_OFFSET_PROG_LD_PTR, program_load + 1);
        }
        g_current_method = method + 4 * (count - 1);
        return;
    }
    for (uint32_t i = 1; i < count; ++i) {
        unsigned slot = (method + 4 * i - NV097_SET_TRANSFORM_CONSTANT) / 4;
        int const_load = GET_MASK(pg->regs[NV_PGRAPH_CHEOPS_OFFSET], NV_PGRAPH_CHEOPS_OFFSET_CONST_LD_PTR);
        if (const_load >= NV2A_VERTEXSHADER_CONSTANTS) Nv2aGlUnknown("transform constant load pointer %d out of range", const_load);
        uint32_t parameter = words[i];
        pg->vsh_constants_dirty[const_load] |= (parameter != pg->vsh_constants[const_load][slot % 4]);
        pg->vsh_constants[const_load][slot % 4] = parameter;
        if (slot % 4 == 3) SET_MASK(pg->regs[NV_PGRAPH_CHEOPS_OFFSET], NV_PGRAPH_CHEOPS_OFFSET_CONST_LD_PTR, const_load + 1);
    }
    g_current_method = method + 4 * (count - 1);
}
static void pgraph_wait_fifo_access(NV2AState *d) {
    while (!(d->pgraph.regs[NV_PGRAPH_FIFO] & NV_PGRAPH_FIFO_ACCESS)) {
		qemu_cond_wait(&d->pgraph.fifo_access_cond, &d->pgraph.pgraph_lock);
	}
}

static void pgraph_log_method(unsigned int subchannel,
								unsigned int graphics_class,
								unsigned int method, uint32_t parameter) {
	static unsigned int last = 0;
	static unsigned int count = 0;

	extern const char *NV2AMethodToString(DWORD dwMethod); // implemented in PushBuffer.cpp

	if (last == 0x1800 && method != last) {
		const char* method_name = NV2AMethodToString(last); // = 'NV2A_VB_ELEMENT_U16'
		NV2A_GL_DPRINTF(true, "d->pgraph method (%d) 0x%08X %s * %d",
						subchannel, last, method_name, count);
	}
	if (method != 0x1800) {
		// const char* method_name = NV2AMethodToString(method);
		// unsigned int nmethod = 0;
		// switch (graphics_class) {
		// case NV_KELVIN_PRIMITIVE:
		// 	nmethod = method | (0x5c << 16);
		// 	break;
		// case NV_CONTEXT_SURFACES_2D:
		// 	nmethod = method | (0x6d << 16);
		// 	break;
        // case NV_CONTEXT_PATTERN:
        // 	nmethod = method | (0x68 << 16);
        // 	break;
		// default:
		// 	break;
		// }
		// if (method_name) {
		// 	NV2A_DPRINTF("d->pgraph method (%d): %s (0x%x)\n",
		// 		subchannel, method_name, parameter);
		// } else {
			NV2A_DPRINTF("pgraph method (%d): 0x%08X -> 0x%04x (0x%x)\n",
				subchannel, graphics_class, method, parameter);
		// }

	}
	if (method == last) { count++; }
	else { count = 0; }
	last = method;
}

static void pgraph_allocate_inline_buffer_vertices(PGRAPHState *pg,
                                                   unsigned int attr)
{
    unsigned int i;
    VertexAttribute *vertex_attribute = &pg->vertex_attributes[attr];

    if (vertex_attribute->inline_buffer || pg->inline_buffer_length == 0) {
        return;
    }

    /* Now upload the previous vertex_attribute value */
    if (!vertex_attribute->inline_buffer_storage) {
        vertex_attribute->inline_buffer_storage = (float*)g_malloc(NV2A_MAX_BATCH_LENGTH * sizeof(float) * 4);
        if (!vertex_attribute->inline_buffer_storage) Nv2aGlUnknown("cannot allocate inline vertex stream");
    }
    vertex_attribute->inline_buffer = vertex_attribute->inline_buffer_storage;
    for (i = 0; i < pg->inline_buffer_length; i++) {
        memcpy(&vertex_attribute->inline_buffer[i * 4],
               vertex_attribute->inline_value,
               sizeof(float) * 4);
    }
}

static void pgraph_finish_inline_buffer_vertex(PGRAPHState *pg)
{
	unsigned int i;

    assert(pg->inline_buffer_length < NV2A_MAX_BATCH_LENGTH);

    for (i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
        VertexAttribute *vertex_attribute = &pg->vertex_attributes[i];
        if (vertex_attribute->inline_buffer) {
            memcpy(&vertex_attribute->inline_buffer[
                      pg->inline_buffer_length * 4],
                   vertex_attribute->inline_value,
                   sizeof(float) * 4);
        }
    }

    pg->inline_buffer_length++;
}

void pgraph_init(NV2AState *d)
{
    int i;

    PGRAPHState *pg = &d->pgraph;

	qemu_mutex_init(&pg->pgraph_lock);
	qemu_cond_init(&pg->interrupt_cond);
	qemu_cond_init(&pg->fifo_access_cond);
	qemu_cond_init(&pg->flip_3d);

	if (!(pg->opengl_enabled))
		return;

	/* attach OpenGL render plugins */
	OpenGL_init_pgraph_plugins();

	/* fire up opengl */

    pg->gl_context = glo_context_create();
    assert(pg->gl_context);
    g_array_buffer_valid=false;
    glGetFloatv(GL_ALIASED_LINE_WIDTH_RANGE, g_line_limits); g_line_width = -1.0f;

#ifdef DEBUG_NV2A_GL
    glEnable(GL_DEBUG_OUTPUT);
#endif


    /* DXT textures */
    assert(glo_check_extension("GL_EXT_texture_compression_s3tc"));
    /*  Internal RGB565 texture format */
    assert(glo_check_extension("GL_ARB_ES2_compatibility"));

    GLint max_vertex_attributes;
    glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &max_vertex_attributes);
    assert(max_vertex_attributes >= NV2A_VERTEXSHADER_ATTRIBUTES);

    glGenFramebuffers(1, &pg->gl_framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, pg->gl_framebuffer);

    /* need a valid framebuffer to start with */
    glGenTextures(1, &pg->gl_color_buffer);
    Nv2aForeignBindTexture(GL_TEXTURE_2D, pg->gl_color_buffer);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 640, 480,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, pg->gl_color_buffer, 0);

    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER)
            == GL_FRAMEBUFFER_COMPLETE);

    //glPolygonMode( GL_FRONT_AND_BACK, GL_LINE );

#ifdef USE_TEXTURE_CACHE
    pg->texture_cache = g_lru_cache_new_full(
        0,
        NULL,
        texture_key_destroy,
        0,
        NULL,
        texture_binding_destroy,
        texture_key_hash,
        texture_key_equal,
        texture_key_retrieve,
        NULL,
        NULL
        );

    g_lru_cache_set_max_size(pg->texture_cache, 512);
#endif

#ifdef USE_SHADER_CACHE
    pg->shader_cache = g_hash_table_new(shader_hash, shader_equal);
#endif

    for (i=0; i<NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
        glGenBuffers(1, &pg->vertex_attributes[i].gl_converted_buffer);
        glGenBuffers(1, &pg->vertex_attributes[i].gl_inline_buffer);
    }
    glGenBuffers(1, &pg->gl_inline_array_buffer);
    glGenBuffers(1, &pg->gl_element_buffer);

    glGenBuffers(1, &pg->gl_memory_buffer);
    Nv2aBindArrayBuffer( pg->gl_memory_buffer);
    glBufferData(GL_ARRAY_BUFFER,
                 d->vram_size,
                 NULL,
                 GL_DYNAMIC_DRAW);

    glGenVertexArrays(1, &pg->gl_vertex_array);
    glBindVertexArray(pg->gl_vertex_array);
    memset(g_attrib_state, 0, sizeof g_attrib_state);

//    assert(glGetError() == GL_NO_ERROR);

    glo_set_current(NULL);
}

void pgraph_destroy(PGRAPHState *pg)
{
    for (auto& a : pg->vertex_attributes) {
        g_free(a.inline_buffer_storage); a.inline_buffer_storage = a.inline_buffer = nullptr;
    }

	qemu_mutex_destroy(&pg->pgraph_lock);
	qemu_cond_destroy(&pg->interrupt_cond);
	qemu_cond_destroy(&pg->fifo_access_cond);
	qemu_cond_destroy(&pg->flip_3d);

	if (pg->opengl_enabled) {
		glo_set_current(pg->gl_context);

		if (pg->gl_color_buffer) {
			Nv2aOwnedRetireTexture(pg->gl_color_buffer); Nv2aForeignDeleteTextures(1, &pg->gl_color_buffer);
		}
		if (pg->gl_zeta_buffer) {
			Nv2aOwnedRetireTexture(pg->gl_zeta_buffer); Nv2aForeignDeleteTextures(1, &pg->gl_zeta_buffer);
		}
		glDeleteFramebuffers(1, &pg->gl_framebuffer);

		// TODO: clear out shader cached
		// TODO: clear out texture cache

		glo_set_current(NULL);

		glo_context_destroy(pg->gl_context);
	}
}

// Diagnostic reference path for paired performance runs; output is identical.
static bool Nv2aLegacyDraws() {
    static const bool legacy = [] {
        char v[8] = {}; DWORD n = GetEnvironmentVariableA("GH_GPU_LEGACY_DRAWS", v, sizeof v);
        if (n && (n != 1 || v[0] != '1')) Nv2aGlUnknown("GH_GPU_LEGACY_DRAWS must be 1 or unset");
        return n != 0;
    }();
    return legacy;
}

// Primitive-mode aliases share a GL program and its uniform storage. Changing
// only the alias must not force a program rebind or a full constant upload.
static bool Nv2aProgramChanged(const ShaderBinding* old, const ShaderBinding* next) {
    return Nv2aLegacyState() ? old != next : (!old || old->gl_program != next->gl_program);
}
// MSVC inlined the 2.5 KiB constant-size memcmp as a DWORD loop at a
// measured hot site. Compare the same bytes with unaligned SSE2 loads;
// no hash-only equality or omitted program/padding bytes.
static bool Nv2aShaderStateEqual(const ShaderState* a, const ShaderState* b) {
    const auto* x=reinterpret_cast<const uint8_t*>(a);
    const auto* y=reinterpret_cast<const uint8_t*>(b);
    size_t i=0;
    for(;i+16<=sizeof(ShaderState);i+=16) {
        __m128i vx=_mm_loadu_si128(reinterpret_cast<const __m128i*>(x+i));
        __m128i vy=_mm_loadu_si128(reinterpret_cast<const __m128i*>(y+i));
        if(_mm_movemask_epi8(_mm_cmpeq_epi8(vx,vy))!=0xffff)return false;
    }
    for(;i<sizeof(ShaderState);++i)if(x[i]!=y[i])return false;
    return true;
}
static unsigned long long g_program_alias_reuses=0;
static bool Nv2aLegacyConstants() {
    static const bool legacy=[] {
        char v[8]={}; DWORD n=GetEnvironmentVariableA("GH_GPU_LEGACY_CONSTANTS",v,sizeof v);
        if(n && (n!=1 || v[0]!='1')) Nv2aGlUnknown("GH_GPU_LEGACY_CONSTANTS must be 1 or unset");
        return n!=0;
    }();
    return legacy;
}
static unsigned long long g_vsh_cached_vectors=0;
static unsigned long long g_vsh_upload_calls = 0, g_vsh_upload_vectors = 0;
static void pgraph_upload_vertex_constants(PGRAPHState* pg, ShaderBinding* binding,
                                            bool binding_changed, bool scalar) {
    // c[] is one vec4 uniform array. Coalesce only adjacent dirty entries whose
    // queried GL locations are consecutive. Clean and inactive entries split
    // runs. Program switches inspect every active constant, but the per-program
    // cache can skip identical values or trim unchanged ends of an upload.
    // Do not split holes inside a run: that would increase driver call count.
    for (unsigned i = 0; i < NV2A_VERTEXSHADER_CONSTANTS;) {
        if (!pg->vsh_constants_dirty[i] && !binding_changed) { ++i; continue; }
        const GLint loc = binding->vsh_constant_loc[i];
        pg->vsh_constants_dirty[i] = false;
        if (loc < 0) { ++i; continue; }
        const unsigned first = i++;
        while (!scalar && i < NV2A_VERTEXSHADER_CONSTANTS &&
               (pg->vsh_constants_dirty[i] || binding_changed) &&
               binding->vsh_constant_loc[i] == loc + GLint(i - first)) {
            pg->vsh_constants_dirty[i++] = false;
        }
        unsigned begin=first, end=i;
        auto* cache=binding->uniform_cache;
        if(cache && !scalar && !Nv2aLegacyConstants()) {
            auto same=[&](unsigned n) {
                return cache->vertex_valid[n] && !memcmp(cache->vertex[n],pg->vsh_constants[n],16);
            };
            while(begin<end && same(begin))++begin;
            while(end>begin && same(end-1))--end;
            g_vsh_cached_vectors+=(i-first)-(end-begin);
        }
        if(begin==end)continue;
        glUniform4fv(loc+GLint(begin-first), end-begin, reinterpret_cast<const GLfloat*>(pg->vsh_constants[begin]));
        if(cache) {
            memcpy(cache->vertex[begin],pg->vsh_constants[begin],(end-begin)*16);
            for(unsigned n=begin;n<end;++n)cache->vertex_valid[n]=true;
        }
        ++g_vsh_upload_calls; g_vsh_upload_vectors += end-begin;
    }
}

static void pgraph_update_shader_constants(PGRAPHState *pg,
                                           ShaderBinding *binding,
                                           bool binding_changed,
                                           bool vertex_program,
                                           bool fixed_function)
{
	Nv2aProf prof_scope(18);
	assert(pg->opengl_enabled);

	unsigned int i, j;

    /* update combiner constants */
    for (i = 0; i<= 8; i++) {
        uint32_t constant[2];
        if (i == 8) {
            /* final combiner */
            constant[0] = pg->regs[NV_PGRAPH_SPECFOGFACTOR0];
            constant[1] = pg->regs[NV_PGRAPH_SPECFOGFACTOR1];
        } else {
            constant[0] = pg->regs[NV_PGRAPH_COMBINEFACTOR0 + i * 4];
            constant[1] = pg->regs[NV_PGRAPH_COMBINEFACTOR1 + i * 4];
        }

        for (j = 0; j < 2; j++) {
            GLint loc = binding->psh_constant_loc[i][j];
            if (loc != -1) {
                float value[4];
                value[0] = (float) ((constant[j] >> 16) & 0xFF) / 255.0f;
                value[1] = (float) ((constant[j] >> 8) & 0xFF) / 255.0f;
                value[2] = (float) (constant[j] & 0xFF) / 255.0f;
                value[3] = (float) ((constant[j] >> 24) & 0xFF) / 255.0f;

                Nv2aUniform4fv(binding, i * 2 + j, loc, value);
            }
        }
    }
    if (binding->alpha_ref_loc != -1) {
        float alpha_ref = GET_MASK(pg->regs[NV_PGRAPH_CONTROL_0],
                                   NV_PGRAPH_CONTROL_0_ALPHAREF) / 255.0f;
        Nv2aUniform1f(binding, 18, binding->alpha_ref_loc, alpha_ref);
    }


    /* For each texture stage */
    for (i = 0; i < NV2A_MAX_TEXTURES; i++) {
        // char name[32];
        GLint loc;

        /* Bump luminance only during stages 1 - 3 */
        if (i > 0) {
            loc = binding->bump_mat_loc[i];
            if (loc != -1) {
                if (Nv2aUniformChanged(binding, 19 + (i - 1) * 3, pg->bump_env_matrix[i - 1], 16))
                    glUniformMatrix2fv(loc, 1, GL_FALSE, pg->bump_env_matrix[i - 1]);
            }
            loc = binding->bump_scale_loc[i];
            if (loc != -1) {
                Nv2aUniform1f(binding, 20 + (i - 1) * 3, loc, *(float*)&pg->regs[
                                NV_PGRAPH_BUMPSCALE1 + (i - 1) * 4]);
            }
            loc = binding->bump_offset_loc[i];
            if (loc != -1) {
                Nv2aUniform1f(binding, 21 + (i - 1) * 3, loc, *(float*)&pg->regs[
                            NV_PGRAPH_BUMPOFFSET1 + (i - 1) * 4]);
            }
        }

    }

    if (binding->fog_color_loc != -1) {
        uint32_t fog_color = pg->regs[NV_PGRAPH_FOGCOLOR];
        GLfloat v[4]={GET_MASK(fog_color, NV_PGRAPH_FOGCOLOR_RED) / 255.0f,
            GET_MASK(fog_color, NV_PGRAPH_FOGCOLOR_GREEN) / 255.0f,
            GET_MASK(fog_color, NV_PGRAPH_FOGCOLOR_BLUE) / 255.0f,
            GET_MASK(fog_color, NV_PGRAPH_FOGCOLOR_ALPHA) / 255.0f};
        Nv2aUniform4fv(binding, 31, binding->fog_color_loc, v);
    }
    if (binding->fog_param_loc[0] != -1) {
        Nv2aUniform1f(binding, 32, binding->fog_param_loc[0],
                    *(float*)&pg->regs[NV_PGRAPH_FOGPARAM0]);
    }
    if (binding->fog_param_loc[1] != -1) {
        Nv2aUniform1f(binding, 33, binding->fog_param_loc[1],
                    *(float*)&pg->regs[NV_PGRAPH_FOGPARAM1]);
    }

    float zclip_max = *(float*)&pg->regs[NV_PGRAPH_ZCLIPMAX];
    float zclip_min = *(float*)&pg->regs[NV_PGRAPH_ZCLIPMIN];

    if (fixed_function) {
        /* update lighting constants */
        struct {
            uint32_t* v;
            bool* dirty;
            GLint* locs;
            size_t len;
        } lighting_arrays[] = {
			// TODO : Change pointers into offset_of(), so this variable can become static
            {&pg->ltctxa[0][0], &pg->ltctxa_dirty[0], binding->ltctxa_loc, NV2A_LTCTXA_COUNT},
            {&pg->ltctxb[0][0], &pg->ltctxb_dirty[0], binding->ltctxb_loc, NV2A_LTCTXB_COUNT},
            {&pg->ltc1[0][0], &pg->ltc1_dirty[0], binding->ltc1_loc, NV2A_LTC1_COUNT},
        };

        for (i=0; i<ARRAY_SIZE(lighting_arrays); i++) {
            uint32_t *lighting_v = lighting_arrays[i].v;
            bool *lighting_dirty = lighting_arrays[i].dirty;
            GLint *lighting_locs = lighting_arrays[i].locs;
            size_t lighting_len = lighting_arrays[i].len;
            for (j=0; j<lighting_len; j++) {
                if (!lighting_dirty[j] && !binding_changed) continue;
                GLint loc = lighting_locs[j];
                if (loc != -1) {
                    glUniform4fv(loc, 1, (const GLfloat*)&lighting_v[j*4]);
                }
                lighting_dirty[j] = false;
            }
        }


        for (i = 0; i < NV2A_MAX_LIGHTS; i++) {
            GLint loc;
            loc = binding->light_infinite_half_vector_loc[i];
            if (loc != -1) {
                Nv2aUniform3fv(binding, 34 + i * 4 + 0, loc, pg->light_infinite_half_vector[i]);
            }
            loc = binding->light_infinite_direction_loc[i];
            if (loc != -1) {
                Nv2aUniform3fv(binding, 34 + i * 4 + 1, loc, pg->light_infinite_direction[i]);
            }

            loc = binding->light_local_position_loc[i];
            if (loc != -1) {
                Nv2aUniform3fv(binding, 34 + i * 4 + 2, loc, pg->light_local_position[i]);
            }
            loc = binding->light_local_attenuation_loc[i];
            if (loc != -1) {
                Nv2aUniform3fv(binding, 34 + i * 4 + 3, loc, pg->light_local_attenuation[i]);
            }
        }

        /* estimate the viewport by assuming it matches the surface ... */
        //FIXME: Get surface dimensions?
        unsigned int vw, vh;
        pgraph_get_surface_dimensions(pg, &vw, &vh);
        float m11 = 0.5f * vw;
        float m22 = -0.5f * vh;
        float m33 = zclip_max - zclip_min;
        //float m41 = m11;
        //float m42 = -m22;
        float m43 = zclip_min;
        //float m44 = 1.0f;

        if (m33 == 0.0f) {
            m33 = 1.0f;
        }
        float invViewport[16] = {
            1.0f/m11, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f/m22, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f/m33, 0.0f,
            -1.0f, 1.0f, -m43/m33, 1.0f
        };

        if (binding->inv_viewport_loc != -1) {
            if (Nv2aUniformChanged(binding, 66, invViewport, sizeof invViewport))
            glUniformMatrix4fv(binding->inv_viewport_loc,
                               1, GL_FALSE, &invViewport[0]);
        }

    }

    pgraph_upload_vertex_constants(pg, binding, binding_changed, Nv2aLegacyDraws());

    if (binding->surface_size_loc != -1) {
        unsigned int sw, sh;
        pgraph_get_surface_dimensions(pg, &sw, &sh);
        Nv2aUniform2f(binding, 67, binding->surface_size_loc, (GLfloat)sw, (GLfloat)sh);
    }

    if (binding->clip_range_loc != -1) {
        Nv2aUniform2f(binding, 68, binding->clip_range_loc, zclip_min, zclip_max);
    }
    if (binding->line_width_loc != -1) Nv2aUniform1f(binding, 69, binding->line_width_loc, pg->line_width_px);

}

static void pgraph_bind_shaders(PGRAPHState *pg)
{
    Nv2aProf prof_scope(7);
	assert(pg->opengl_enabled);

	unsigned int i, j;

	uint32_t csv0_d = pg->regs[NV_PGRAPH_CSV0_D];
    bool vertex_program = GET_MASK(csv0_d,
                                   NV_PGRAPH_CSV0_D_MODE) == 2;

    bool fixed_function = GET_MASK(csv0_d,
                                   NV_PGRAPH_CSV0_D_MODE) == 0;

	uint32_t csv0_c = pg->regs[NV_PGRAPH_CSV0_C];
    int program_start = GET_MASK(csv0_c,
                                 NV_PGRAPH_CSV0_C_CHEOPS_PROGRAM_START);

    NV2A_GL_DGROUP_BEGIN("%s (VP: %s FFP: %s)", __func__,
                         vertex_program ? "yes" : "no",
                         fixed_function ? "yes" : "no");

	ShaderBinding* old_binding = pg->shader_binding;

	ShaderState state;
	// Native port: zero padding and every field not set below, so the shader
	// cache key is deterministic.
	memset(&state, 0, sizeof(state));
	/* register combiner stuff */
	state.psh.window_clip_exclusive = pg->regs[NV_PGRAPH_SETUPRASTER]
                                       & NV_PGRAPH_SETUPRASTER_WINDOWCLIPTYPE,
	state.psh.combiner_control = pg->regs[NV_PGRAPH_COMBINECTL];
	state.psh.shader_stage_program = pg->regs[NV_PGRAPH_SHADERPROG];
	// Dot mapping is decoded and validated by the texture shader translator.
	state.psh.other_stage_input = pg->regs[NV_PGRAPH_SHADERCTL];
	state.psh.final_inputs_0 = pg->regs[NV_PGRAPH_COMBINESPECFOG0];
	state.psh.final_inputs_1 = pg->regs[NV_PGRAPH_COMBINESPECFOG1];
	state.psh.two_sided_lighting = (pg->regs[NV_PGRAPH_SETUPRASTER] & (1u << 24)) != 0;
	uint32_t control0 = pg->regs[NV_PGRAPH_CONTROL_0];

	state.psh.alpha_test = control0
		& NV_PGRAPH_CONTROL_0_ALPHATESTENABLE;
	state.psh.alpha_func = (enum PshAlphaFunc)GET_MASK(control0,
		NV_PGRAPH_CONTROL_0_ALPHAFUNC);

    /* fixed function stuff */
	state.skinning = (enum VshSkinning)GET_MASK(csv0_d,
		NV_PGRAPH_CSV0_D_SKIN);
	state.lighting = csv0_c
		& NV_PGRAPH_CSV0_C_LIGHTING;
	// Native port: the inherited fixed-function lighting generator is
	// incomplete (no specular power, spot lights, separate specular or light
	// control). Refuse it instead of rendering approximate colours.
	if (fixed_function && state.lighting)
		Nv2aGlUnknown("fixed-function lighting not provided (CSV0_C %#x, CSV0_D %#x)",
			csv0_c, pg->regs[NV_PGRAPH_CSV0_D]);
	state.normalization = csv0_c
		& NV_PGRAPH_CSV0_C_NORMALIZATION_ENABLE;

	state.fixed_function = fixed_function;

    /* vertex program stuff */
	state.vertex_program = vertex_program;
	state.z_perspective = control0
		& NV_PGRAPH_CONTROL_0_Z_PERSPECTIVE_ENABLE;
	state.specular_enable = (pg->regs[NV_PGRAPH_CSV0_C] >> 16) & 1;
	state.wide_lines = pg->wide_lines && pg->primitive_mode >= PRIM_TYPE_LINES && pg->primitive_mode <= PRIM_TYPE_LINE_STRIP;

    /* geometry shader stuff */
	state.primitive_mode = (enum ShaderPrimitiveMode)pg->primitive_mode;
	state.polygon_front_mode = (enum ShaderPolygonMode)GET_MASK(pg->regs[NV_PGRAPH_SETUPRASTER],
		NV_PGRAPH_SETUPRASTER_FRONTFACEMODE);
	state.polygon_back_mode = (enum ShaderPolygonMode)GET_MASK(pg->regs[NV_PGRAPH_SETUPRASTER],
		NV_PGRAPH_SETUPRASTER_BACKFACEMODE);

    state.program_length = 0;
    // The whole key was already zeroed above, including the unused token tail.

    if (vertex_program) {
        // copy in vertex program tokens
        for (i = program_start; i < NV2A_MAX_TRANSFORM_PROGRAM_LENGTH; i++) {
            uint32_t *cur_token = (uint32_t*)&pg->program_data[i];
            memcpy(&state.program_data[state.program_length],
                   cur_token,
                   VSH_TOKEN_SIZE * sizeof(uint32_t));
            state.program_length++;

            if (vsh_get_field(cur_token, FLD_FINAL)) {
                break;
            }
        }
    }

    /* Texgen */
    for (i = 0; i < 4; i++) {
        unsigned int reg = (i < 2) ? NV_PGRAPH_CSV1_A : NV_PGRAPH_CSV1_B;
        for (j = 0; j < 4; j++) {
            unsigned int masks[] = {
                // NOTE: For some reason, Visual Studio thinks NV_PGRAPH_xxxx is signed integer. (possible bug?)
                (i % 2U) ? (unsigned int)NV_PGRAPH_CSV1_A_T1_S : (unsigned int)NV_PGRAPH_CSV1_A_T0_S,
                (i % 2U) ? (unsigned int)NV_PGRAPH_CSV1_A_T1_T : (unsigned int)NV_PGRAPH_CSV1_A_T0_T,
                (i % 2U) ? (unsigned int)NV_PGRAPH_CSV1_A_T1_R : (unsigned int)NV_PGRAPH_CSV1_A_T0_R,
                (i % 2U) ? (unsigned int)NV_PGRAPH_CSV1_A_T1_Q : (unsigned int)NV_PGRAPH_CSV1_A_T0_Q
            };
            state.texgen[i][j] = (enum VshTexgen)GET_MASK(pg->regs[reg], masks[j]);
        }
    }

    /* Fog */
    state.fog_enable = pg->regs[NV_PGRAPH_CONTROL_3]
                           & NV_PGRAPH_CONTROL_3_FOGENABLE;
    if (state.fog_enable) {
        /*FIXME: Use CSV0_D? */
        state.fog_mode = (enum VshFogMode)GET_MASK(pg->regs[NV_PGRAPH_CONTROL_3],
                                  NV_PGRAPH_CONTROL_3_FOG_MODE);
        state.foggen = (enum VshFoggen)GET_MASK(csv0_d,
                                NV_PGRAPH_CSV0_D_FOGGENMODE);
    } else {
        /* FIXME: Do we still pass the fogmode? */
        state.fog_mode = (enum VshFogMode)0;
        state.foggen = (enum VshFoggen)0;
    }

    /* Texture matrices */
    for (i = 0; i < 4; i++) {
        state.texture_matrix_enable[i] = pg->texture_matrix_enable[i];
    }

    /* Lighting */
    if (state.lighting) {
        for (i = 0; i < NV2A_MAX_LIGHTS; i++) {
            state.light[i] = (enum VshLight)GET_MASK(csv0_d,
                                      NV_PGRAPH_CSV0_D_LIGHT0 << (i * 2));
        }
    }

    /* Window clip
     *
     * Optimization note: very quickly check to ignore any repeated or zero-size
     * clipping regions. Note that if region number 7 is valid, but the rest are
     * not, we will still add all of them. Clip regions seem to be typically
     * front-loaded (meaning the first one or two regions are populated, and the
     * following are zeroed-out), so let's avoid adding any more complicated
     * masking or copying logic here for now unless we discover a valid case.
     */
    assert(!state.psh.window_clip_exclusive); /* FIXME: Untested */
    state.psh.window_clip_count = 0;
    uint32_t last_x = 0, last_y = 0;

    for (i = 0; i < 8; i++) {
        const uint32_t x = pg->regs[NV_PGRAPH_WINDOWCLIPX0 + i * 4];
        const uint32_t y = pg->regs[NV_PGRAPH_WINDOWCLIPY0 + i * 4];
        const uint32_t x_min = GET_MASK(x, NV_PGRAPH_WINDOWCLIPX0_XMIN);
        const uint32_t x_max = GET_MASK(x, NV_PGRAPH_WINDOWCLIPX0_XMAX);
        const uint32_t y_min = GET_MASK(y, NV_PGRAPH_WINDOWCLIPY0_YMIN);
        const uint32_t y_max = GET_MASK(y, NV_PGRAPH_WINDOWCLIPY0_YMAX);

        /* Check for zero width or height clipping region */
        if ((x_min == x_max) || (y_min == y_max)) {
            continue;
        }

        /* Check for in-order duplicate regions */
        if ((x == last_x) && (y == last_y)) {
            continue;
        }

        NV2A_DPRINTF("Clipping Region %d: min=(%d, %d) max=(%d, %d)\n",
            i, x_min, y_min, x_max, y_max);

        state.psh.window_clip_count = i + 1;
        last_x = x;
        last_y = y;
    }

    /* FIXME: We should memset(state, 0x00, sizeof(state)) instead */
    memset(state.psh.rgb_inputs, 0, sizeof(state.psh.rgb_inputs));
    memset(state.psh.rgb_outputs, 0, sizeof(state.psh.rgb_outputs));
    memset(state.psh.alpha_inputs, 0, sizeof(state.psh.alpha_inputs));
    memset(state.psh.alpha_outputs, 0, sizeof(state.psh.alpha_outputs));

    /* Copy content of enabled combiner stages */
    unsigned int num_stages = pg->regs[NV_PGRAPH_COMBINECTL] & 0xFF;
    for (i = 0; i < num_stages; i++) {
        state.psh.rgb_inputs[i] = pg->regs[NV_PGRAPH_COMBINECOLORI0 + i * 4];
        state.psh.rgb_outputs[i] = pg->regs[NV_PGRAPH_COMBINECOLORO0 + i * 4];
        state.psh.alpha_inputs[i] = pg->regs[NV_PGRAPH_COMBINEALPHAI0 + i * 4];
        state.psh.alpha_outputs[i] = pg->regs[NV_PGRAPH_COMBINEALPHAO0 + i * 4];
        //constant_0[i] = pg->regs[NV_PGRAPH_COMBINEFACTOR0 + i * 4];
        //constant_1[i] = pg->regs[NV_PGRAPH_COMBINEFACTOR1 + i * 4];
    }

    for (i = 0; i < 4; i++) {
        state.psh.rect_tex[i] = false;
        bool enabled = pg->regs[NV_PGRAPH_TEXCTL0_0 + i*4]
                         & NV_PGRAPH_TEXCTL0_0_ENABLE;
        unsigned int color_format =
            GET_MASK(pg->regs[NV_PGRAPH_TEXFMT0 + i*4],
                     NV_PGRAPH_TEXFMT0_COLOR);

        if (enabled && pgraph_texture_shader_fetches(pg->regs[NV_PGRAPH_SHADERPROG], i) && kelvin_color_format_map[color_format].encoding == linear) {
            state.psh.rect_tex[i] = true;
        }

        for (j = 0; j < 4; j++) {
            state.psh.compare_mode[i][j] =
                (pg->regs[NV_PGRAPH_SHADERCLIPMODE] >> (4 * i + j)) & 1;
        }
        state.psh.alphakill[i] = pg->regs[NV_PGRAPH_TEXCTL0_0 + i*4]
                               & NV_PGRAPH_TEXCTL0_0_ALPHAKILLEN;
    }

#ifdef USE_SHADER_CACHE
	// Native port: consecutive draws usually repeat the whole state; compare
	// with the previous draw's state before hashing it for the cache.
	static ShaderState last_state; static ShaderBinding* last_binding = nullptr;
	bool state_repeated = last_binding && Nv2aShaderStateEqual(&last_state, &state);
	ShaderBinding* cached_shader = state_repeated ? last_binding
	                             : (ShaderBinding*)g_hash_table_lookup(pg->shader_cache, &state);

    if (cached_shader) {
        pg->shader_binding = cached_shader;
    } else {
#endif
        bool compiled = false;
        pg->shader_binding = generate_shaders_dedup(state, &compiled);
        if (compiled) ++g_nv2a_shader_compiles;
        else if (g_shader_dedup_hits % 200 == 1) Nv2aGlLog("shader dedup: %llu states reused an identical compiled program (%llu compiles)", g_shader_dedup_hits, g_nv2a_shader_compiles);
        {   // diagnostic: which ShaderState bytes differ between consecutive compiles
            static ShaderState last; static bool have = false;
            if (have && g_nv2a_shader_compiles >= 300 && g_nv2a_shader_compiles < 306) {
                const uint8_t* a = (const uint8_t*)&last; const uint8_t* b = (const uint8_t*)&state;
                char line[512] = ""; int n = 0; int runs = 0;
                for (size_t k = 0; k < sizeof(ShaderState) && runs < 12; k++) if (a[k] != b[k]) {
                    size_t e = k; while (e < sizeof(ShaderState) && a[e] != b[e]) e++;
                    n += snprintf(line + n, sizeof line - n, " [%zu..%zu)", k, e); k = e; runs++;
                }
                Nv2aGlLog("shader state diff (size %zu, psh at %zu, program_data at %zu, length at %zu):%s", sizeof(ShaderState),
                    offsetof(ShaderState, psh), offsetof(ShaderState, program_data), offsetof(ShaderState, program_length), line);
            }
            last = state; have = true;
        }

#ifdef USE_SHADER_CACHE
        /* cache it */
        ShaderState *cache_state = (ShaderState *)g_malloc(sizeof(*cache_state));
        memcpy(cache_state, &state, sizeof(*cache_state));
        g_hash_table_insert(pg->shader_cache, cache_state,
                            (gpointer)pg->shader_binding);
        if (g_nv2a_shader_compiles < 8 || (g_nv2a_shader_compiles >= 300 && g_nv2a_shader_compiles < 304))
            Nv2aGlLog("shader cache: compile %llu hash %08x entries %zu relookup %p", g_nv2a_shader_compiles, shader_hash(&state),
                pg->shader_cache->map.size(), g_hash_table_lookup(pg->shader_cache, &state));
    }
#endif

#ifdef USE_SHADER_CACHE
    if(!state_repeated) last_state = state;
    last_binding = pg->shader_binding;
#endif
    bool binding_changed = Nv2aProgramChanged(old_binding, pg->shader_binding);
    if(!binding_changed && old_binding!=pg->shader_binding) ++g_program_alias_reuses;

    if (binding_changed) glUseProgram(pg->shader_binding->gl_program);

    /* Clipping regions */
    for (i = 0; i < state.psh.window_clip_count; i++) {
        if (pg->shader_binding->clip_region_loc[i] == -1) {
            continue;
        }

        uint32_t x   = pg->regs[NV_PGRAPH_WINDOWCLIPX0 + i * 4];
        GLuint x_min = GET_MASK(x, NV_PGRAPH_WINDOWCLIPX0_XMIN);
        GLuint x_max = GET_MASK(x, NV_PGRAPH_WINDOWCLIPX0_XMAX);

        /* Adjust y-coordinates for the OpenGL viewport: translate coordinates
         * to have the origin at the bottom-left of the surface (as opposed to
         * top-left), and flip y-min and y-max accordingly.
         */
        uint32_t y   = pg->regs[NV_PGRAPH_WINDOWCLIPY0 + i * 4];
        unsigned int wc_w, wc_h;
        pgraph_get_surface_dimensions(pg, &wc_w, &wc_h);
        GLuint y_min = (wc_h - 1) -
                       GET_MASK(y, NV_PGRAPH_WINDOWCLIPY0_YMAX);
        GLuint y_max = (wc_h - 1) -
                       GET_MASK(y, NV_PGRAPH_WINDOWCLIPY0_YMIN);

        pgraph_apply_anti_aliasing_factor(pg, &x_min, &y_min);
        pgraph_apply_anti_aliasing_factor(pg, &x_max, &y_max);

        Nv2aUniform4i(pg->shader_binding, 70 + i, pg->shader_binding->clip_region_loc[i],
                    x_min * g_render_scale, y_min * g_render_scale, (x_max + 1) * g_render_scale, (y_max + 1) * g_render_scale);
    }

    pgraph_update_shader_constants(pg, pg->shader_binding, binding_changed,
                                   vertex_program, fixed_function);

    NV2A_GL_DGROUP_END();
}

static bool pgraph_get_framebuffer_dirty(PGRAPHState *pg)
{
    bool shape_changed = memcmp(&pg->surface_shape, &pg->last_surface_shape,
                                sizeof(SurfaceShape)) != 0;
    if (!shape_changed || (!pg->surface_shape.color_format
            && !pg->surface_shape.zeta_format)) {
        return false;
    }
    return true;
}

static bool pgraph_get_color_write_enabled(PGRAPHState *pg)
{
	return pg->regs[NV_PGRAPH_CONTROL_0] & (
		NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE
		| NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE
		| NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE
		| NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE);
}

static bool pgraph_get_zeta_write_enabled(PGRAPHState *pg)
{
	return pg->regs[NV_PGRAPH_CONTROL_0] & (
		NV_PGRAPH_CONTROL_0_ZWRITEENABLE
		| NV_PGRAPH_CONTROL_0_STENCIL_WRITE_ENABLE);
}

static void pgraph_set_surface_dirty(PGRAPHState *pg, bool color, bool zeta)
{
    NV2A_DPRINTF("pgraph_set_surface_dirty(%d, %d) -- %d %d\n",
                 color, zeta,
                 pgraph_get_color_write_enabled(pg), pgraph_get_zeta_write_enabled(pg));
    /* FIXME: Does this apply to CLEARs too? */
    color = color && pgraph_get_color_write_enabled(pg);
    zeta = zeta && pgraph_get_zeta_write_enabled(pg);
    pg->surface_color.draw_dirty |= color;
    pg->surface_zeta.draw_dirty |= zeta;
}

// The backing store is logical expanded VRAM (as in Cxbx), not a dump of
// compressed DRAM. Validate every use of the supported compressed-depth tile:
// the logical pixels must be Z24S8, with the same pitch and bounded tile range.
static void pgraph_validate_depth_tile(PGRAPHState *pg, uint32_t base,
                                       uint32_t pitch, uint32_t height, bool color)
{
    uint64_t end = uint64_t(base) + uint64_t(pitch) * height;
    for (unsigned int i = 0; i < 8; ++i) {
        if (!pg->rdi_memory_written[36+i] || !pg->rdi_memory[36+i]) continue;
        uint32_t tile = pg->rdi_memory[4+i];
        if (!(tile & 2)) continue; // nxdk: bit1 is tile-valid
        uint32_t first = tile & 0xffffc000;
        uint64_t last = uint64_t(pg->rdi_memory[12+i]) + 1;
        if (end <= first || base >= last) continue;
        if (color || pg->surface_shape.zeta_format != NV097_SET_SURFACE_FORMAT_ZETA_Z24S8 ||
            pg->surface_shape.z_format || base < first || end > last ||
            pitch != pg->rdi_memory[20+i])
            Nv2aGlUnknown("Z24S8 tile %u incompatible surface base %#x pitch %#x height %u color %u",
                          i, base, pitch, height, color);
    }
}

static void pgraph_update_surface_part(NV2AState *d, bool upload, bool color) {
    Nv2aProf prof_scope(10);
    PGRAPHState *pg = &d->pgraph;
    // Native port: an upload of a bound surface that is neither rebound nor due
    // its once-per-frame guest-write check does nothing below (dirty stays
    // false); return before re-deriving the surface and allocating buffers.
    {
        Surface *s = color ? &pg->surface_color : &pg->surface_zeta;
        GLuint bound = color ? pg->gl_color_buffer : pg->gl_zeta_buffer;
        if (upload && bound && !s->buffer_dirty && s->checked_epoch == g_nv2a_present_epoch) return;
    }

    unsigned int width, height;
    pgraph_get_surface_dimensions(pg, &width, &height);
    pgraph_apply_anti_aliasing_factor(pg, &width, &height);

    Surface *surface;
    hwaddr dma_address;
    GLuint *gl_buffer;
    unsigned int bytes_per_pixel;
    GLint gl_internal_format;
    GLenum gl_format, gl_type, gl_attachment;

    if (color) {
        surface = &pg->surface_color;
        dma_address = pg->dma_color;
        gl_buffer = &pg->gl_color_buffer;

        assert(pg->surface_shape.color_format != 0);
        assert(pg->surface_shape.color_format
                < ARRAY_SIZE(kelvin_surface_color_format_map));
        SurfaceColorFormatInfo f =
            kelvin_surface_color_format_map[pg->surface_shape.color_format];
        if (f.bytes_per_pixel == 0) {
            fprintf(stderr, "nv2a: unimplemented color surface format 0x%x\n",
                    pg->surface_shape.color_format);
            abort();
        }

        bytes_per_pixel = f.bytes_per_pixel;
        gl_internal_format = f.gl_internal_format;
        gl_format = f.gl_format;
        gl_type = f.gl_type;
        gl_attachment = GL_COLOR_ATTACHMENT0;

    } else {
        surface = &pg->surface_zeta;
        dma_address = pg->dma_zeta;
        gl_buffer = &pg->gl_zeta_buffer;

        assert(pg->surface_shape.zeta_format != 0);
        switch (pg->surface_shape.zeta_format) {
        case NV097_SET_SURFACE_FORMAT_ZETA_Z16:
            bytes_per_pixel = 2;
            gl_format = GL_DEPTH_COMPONENT;
            gl_attachment = GL_DEPTH_ATTACHMENT;
            if (pg->surface_shape.z_format) {
                gl_type = GL_HALF_FLOAT;
                gl_internal_format = GL_DEPTH_COMPONENT32F;
            } else {
                gl_type = GL_UNSIGNED_SHORT;
                gl_internal_format = GL_DEPTH_COMPONENT16;
            }
            break;
        case NV097_SET_SURFACE_FORMAT_ZETA_Z24S8:
            bytes_per_pixel = 4;
            gl_format = GL_DEPTH_STENCIL;
            gl_attachment = GL_DEPTH_STENCIL_ATTACHMENT;
            if (pg->surface_shape.z_format) {
                assert(false);
                gl_type = GL_FLOAT_32_UNSIGNED_INT_24_8_REV;
                gl_internal_format = GL_DEPTH32F_STENCIL8;
            } else {
                gl_type = GL_UNSIGNED_INT_24_8;
                gl_internal_format = GL_DEPTH24_STENCIL8;
            }
            break;
        default:
            assert(false);
            break;
        }
    }


    DMAObject dma = nv_dma_load(d, dma_address);
    /* There's a bunch of bugs that could cause us to hit this function
     * at the wrong time and get a invalid dma object.
     * Check that it's sane. */
    assert(dma.dma_class == NV_DMA_IN_MEMORY_CLASS);
    pgraph_validate_depth_tile(pg, dma.address + surface->offset, surface->pitch, height, color);

    assert(dma.address + surface->offset != 0);
    assert(surface->offset <= dma.limit);
    assert(surface->offset + surface->pitch * height <= dma.limit + 1);

    hwaddr data_len;
    uint8_t *data = (uint8_t*)nv_dma_map(d, dma_address, &data_len);

    /* TODO */
    // assert(pg->surface_clip_x == 0 && pg->surface_clip_y == 0);

    bool swizzle = (pg->surface_type == NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE);

    uint8_t *buf = data + surface->offset;
    if (swizzle) {
        buf = (uint8_t*)g_malloc(height * surface->pitch);
    }

    bool dirty = surface->buffer_dirty;
    // Native port: guest writes are tracked by page generation (gpu.inc)
    // instead of the reference's "always dirty" colour uploads.
    // The bound render target is checked for guest writes when it is (re)bound
    // and once per presented frame, not on every draw: CPU writes into the
    // current render target take effect from the next frame.
    uint64_t write_gen = surface->memory_gen;
    bool check_writes = surface->buffer_dirty || surface->checked_epoch != g_nv2a_present_epoch;
    if (check_writes) {
        write_gen = Nv2aGlCurrentGeneration();
        surface->checked_epoch = g_nv2a_present_epoch;
    }
    if (color) {
#if 1
        if (check_writes && Nv2aGlRangeGeneration(dma.address + surface->offset, surface->pitch * height) > surface->memory_gen)
            dirty = true;
#else
        dirty |= memory_region_test_and_clear_dirty(d->vram,
                                               dma.address + surface->offset,
                                               surface->pitch * height,
                                               DIRTY_MEMORY_NV2A);
#endif
    }
    if (upload && dirty) {
        /* surface modified (or moved) by the cpu.
         * copy it into the opengl renderbuffer */
        // TODO: Why does this assert?
		//assert(!surface->draw_dirty);
		assert(surface->pitch % bytes_per_pixel == 0);

		// Native port: surface texture cache. A GL texture whose surface is
		// switched away is kept with the guest-memory generation it matched;
		// switching back while guest memory is unchanged re-attaches it
		// instead of re-uploading from RAM.
		struct SurfCacheEntry { bool color; uint32_t addr, w, h, fmt, pitch; bool swz; GLuint tex; uint64_t gen; };
		static std::vector<SurfCacheEntry> surface_cache;
		uint32_t key_addr = dma.address + surface->offset;
		GLuint reuse = 0; GLint reuse_w = 0;
		if (pg->opengl_enabled) {
			if (*gl_buffer && surface->bound_valid) {
				// The same memory drawn with a narrower clip (same rows, pitch and
				// format) keeps its texture: columns map left-aligned and the row
				// flip depends only on the height.
				bool same = surface->bound_addr == key_addr && surface->bound_w >= width && surface->bound_h == height &&
				            surface->bound_fmt == gl_internal_format && surface->bound_pitch == surface->pitch && surface->bound_swizzle == swizzle;
				if (same && Nv2aGlRangeGeneration(key_addr, surface->pitch * height) <= surface->memory_gen) {
					reuse = *gl_buffer; reuse_w = (GLint)surface->bound_w; *gl_buffer = 0; ++g_surf_stats[2];
				} else if (!same) {
					for (size_t k = 0; k < surface_cache.size(); k++) {
						const SurfCacheEntry& e = surface_cache[k];
						if (e.color == color && e.addr == surface->bound_addr && e.w == surface->bound_w && e.h == surface->bound_h &&
						    e.fmt == surface->bound_fmt && e.pitch == surface->bound_pitch && e.swz == surface->bound_swizzle) {
							Nv2aOwnedRetireTexture(surface_cache[k].tex); Nv2aForeignDeleteTextures(1, &surface_cache[k].tex); surface_cache.erase(surface_cache.begin() + k); break;
						}
					}
					if (surface_cache.size() >= 32) { Nv2aOwnedRetireTexture(surface_cache[0].tex); Nv2aForeignDeleteTextures(1, &surface_cache[0].tex); surface_cache.erase(surface_cache.begin()); }
					surface_cache.push_back({color, surface->bound_addr, surface->bound_w, surface->bound_h, surface->bound_fmt,
					                         surface->bound_pitch, surface->bound_swizzle, *gl_buffer, surface->memory_gen});
					glFramebufferTexture2D(GL_FRAMEBUFFER, gl_attachment, GL_TEXTURE_2D, 0, 0);
					*gl_buffer = 0;
				}
			}
			for (size_t k = 0; !reuse && k < surface_cache.size(); k++) {
				const SurfCacheEntry& e = surface_cache[k];
				if (e.color == color && e.addr == key_addr && e.w >= width && e.h == height && e.fmt == gl_internal_format &&
				    e.pitch == surface->pitch && e.swz == swizzle) {
					if (Nv2aGlRangeGeneration(key_addr, surface->pitch * height) <= e.gen) { reuse = e.tex; reuse_w = (GLint)e.w; ++g_surf_stats[0]; }
					else { Nv2aOwnedRetireTexture(surface_cache[k].tex); Nv2aForeignDeleteTextures(1, &surface_cache[k].tex); ++g_surf_stats[1]; }
					surface_cache.erase(surface_cache.begin() + k);
					break;
				}
			}
		}

        if (!reuse) {
            static int logged = 0;
            if (logged < 40 && g_nv2a_surface_uploads > 600) { ++logged;
                Nv2aGlLog("surface upload %s addr %#x %ux%u pitch %u fmt %#x swz %d buffer_dirty %d bound %d (%#x %ux%u) gen range %llu mem %llu",
                    color ? "colour" : "zeta", key_addr, width, height, surface->pitch, gl_internal_format, swizzle, surface->buffer_dirty,
                    surface->bound_valid, surface->bound_addr, surface->bound_w, surface->bound_h,
                    (unsigned long long)Nv2aGlRangeGeneration(key_addr, surface->pitch * height), (unsigned long long)surface->memory_gen); }
        }
        // Native port: a colour surface whose memory is exactly one pending owned
        // surface (e.g. the GPU 2D blit target the title then renders into) starts
        // from that texture on the GPU, at render scale, instead of a download and
        // a 1x re-upload of the same bytes.
        GLuint inherit = 0;
        if (!reuse && color && pg->opengl_enabled && !g_owned_sync)
            inherit = Nv2aOwnedMatch(key_addr, surface->pitch * height, width, height, bytes_per_pixel, gl_format, gl_type, swizzle, surface->pitch);
        if (inherit && *gl_buffer && inherit == *gl_buffer) inherit = 0;   // the outgoing texture is retired (and completed) below
        if (!reuse && !inherit) Nv2aOwnedFlushFor("surface-upload", key_addr, surface->pitch * height);
        if (swizzle && !reuse && !inherit) {
            unswizzle_rect(data + surface->offset,
                           width, height,
                           buf,
                           surface->pitch,
                           bytes_per_pixel);
        }

		if (pg->opengl_enabled) {
			if (!color) {
				/* need to clear the depth_stencil and depth attachment for zeta */
				glFramebufferTexture2D(GL_FRAMEBUFFER,
									   GL_DEPTH_ATTACHMENT,
									   GL_TEXTURE_2D,
									   0, 0);
				glFramebufferTexture2D(GL_FRAMEBUFFER,
									   GL_DEPTH_STENCIL_ATTACHMENT,
									   GL_TEXTURE_2D,
									   0, 0);
			}

			glFramebufferTexture2D(GL_FRAMEBUFFER,
								   gl_attachment,
								   GL_TEXTURE_2D,
								   0, 0);

			if (*gl_buffer) {
				Nv2aOwnedRetireTexture(*gl_buffer);
				Nv2aForeignDeleteTextures(1, gl_buffer);
				*gl_buffer = 0;
			}

			if (reuse) {
				*gl_buffer = reuse;
			} else if (inherit) {
				const unsigned sc = Nv2aGlRenderScale();
				glGenTextures(1, gl_buffer);
				Nv2aForeignBindTexture(GL_TEXTURE_2D, *gl_buffer);
				glTexImage2D(GL_TEXTURE_2D, 0, gl_internal_format, width * sc, height * sc, 0, gl_format, gl_type, nullptr);
				static GLuint ifbo[2] = {};
				if (!ifbo[0]) glGenFramebuffers(2, ifbo);
				GLint rd = 0, dr = 0; glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rd); glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &dr);
				GLboolean sc_on = glIsEnabled(GL_SCISSOR_TEST); if (sc_on) glDisable(GL_SCISSOR_TEST);
				glBindFramebuffer(GL_READ_FRAMEBUFFER, ifbo[0]); glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, inherit, 0);
				glBindFramebuffer(GL_DRAW_FRAMEBUFFER, ifbo[1]); glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *gl_buffer, 0);
				glBlitFramebuffer(0, 0, width * sc, height * sc, 0, 0, width * sc, height * sc, GL_COLOR_BUFFER_BIT, GL_NEAREST);
				++g_nv2a_render_serial;
				glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
				glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
				glBindFramebuffer(GL_READ_FRAMEBUFFER, rd); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dr);
				if (sc_on) glEnable(GL_SCISSOR_TEST);
				Nv2aOwnedMarkSuperseded(key_addr, surface->pitch * height, inherit);
				write_gen = Nv2aGlRangeGeneration(key_addr, surface->pitch * height);
				if (++g_surface_inherits % 2000 == 1) Nv2aGlLog("surface initialised on GPU from pending owned texture (%llu so far): %#x %ux%u", g_surface_inherits, key_addr, width, height);
			} else {
			glGenTextures(1, gl_buffer);
			Nv2aForeignBindTexture(GL_TEXTURE_2D, *gl_buffer);
			++g_nv2a_surface_uploads;

			/* This is VRAM so we can't do this inplace! */
			uint8_t *flipped_buf = (uint8_t*)g_malloc(width * height * bytes_per_pixel);
			unsigned int irow;
			for (irow = 0; irow < height; irow++) {
				memcpy(&flipped_buf[width * (height - irow - 1)
										 * bytes_per_pixel],
					   &buf[surface->pitch * irow],
					   width * bytes_per_pixel);
			}

			pgraph_check_gl_errors("pending before a surface upload");
			Nv2aSurfaceImage(gl_internal_format, width, height, gl_format, gl_type, bytes_per_pixel, flipped_buf);
			GLenum upload_error = glGetError();
			if (upload_error != GL_NO_ERROR) {
				GLint pbo = 0, row = 0, align = 0, active = 0;
				glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &pbo); glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row);
				glGetIntegerv(GL_UNPACK_ALIGNMENT, &align); glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
				Nv2aGlUnknown("surface upload glTexImage2D error %#x (%ux%u internal %#x format %#x type %#x; unpack buffer %d row %d align %d, active unit %#x)",
					upload_error, width, height, gl_internal_format, gl_format, gl_type, pbo, row, align, active);
			}

			g_free(flipped_buf);
			}
			GLint tex_w = width;
			if (reuse) tex_w = reuse_w;   // recorded with the texture (no GL query)
			surface->bound_valid = true; surface->bound_addr = key_addr; surface->bound_w = (uint32_t)tex_w; surface->bound_h = height;
			surface->bound_fmt = gl_internal_format; surface->bound_pitch = surface->pitch; surface->bound_swizzle = swizzle;

			glFramebufferTexture2D(GL_FRAMEBUFFER,
								   gl_attachment,
								   GL_TEXTURE_2D,
								   *gl_buffer, 0);

			// A reused texture was complete in this role before; an incomplete
			// framebuffer would also fail the next draw (per-frame error check).
			GLenum fb_status = (reuse && !Nv2aGlDebugChecks()) ? GL_FRAMEBUFFER_COMPLETE : glCheckFramebufferStatus(GL_FRAMEBUFFER);
			if (fb_status != GL_FRAMEBUFFER_COMPLETE) {
				GLuint texs[2] = {pg->gl_color_buffer, pg->gl_zeta_buffer};
				for (int k = 0; k < 2; k++) {
					GLint w = -1, h = -1, f = -1;
					if (texs[k]) { Nv2aForeignBindTexture(GL_TEXTURE_2D, texs[k]);
						glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
						glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
						glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &f); }
					Nv2aGlLog("framebuffer %s texture %u: %dx%d internal %#x", k ? "zeta" : "colour", texs[k], w, h, f);
				}
				Nv2aGlUnknown("incomplete framebuffer %#x after %s upload: %ux%u internal %#x format %#x type %#x (color fmt %#x zeta fmt %#x, AA %u)",
					fb_status, color ? "colour" : "zeta", width, height, gl_internal_format, gl_format, gl_type,
					pg->surface_shape.color_format, pg->surface_shape.zeta_format, pg->surface_shape.anti_aliasing);
			}
		}

        // Native port: the vertex-fetch mirror (gl_memory_buffer) is refreshed
        // lazily from page generations when vertex data is bound; a forced
        // full-surface copy here is unnecessary.
        surface->buffer_dirty = false;
        surface->memory_gen = write_gen;

#ifdef DEBUG_NV2A
        uint8_t *out = data + surface->offset + 64;
        NV2A_DPRINTF("upload_surface %s 0x%" HWADDR_PRIx " - 0x%" HWADDR_PRIx ", "
                      "(0x%" HWADDR_PRIx " - 0x%" HWADDR_PRIx ", "
                        "%d %d, %d %d, %d) - %x %x %x %x\n",
            color ? "color" : "zeta",
            dma.address, dma.address + dma.limit,
            dma.address + surface->offset,
            dma.address + surface->pitch * height,
            pg->surface_shape.clip_x, pg->surface_shape.clip_y,
            pg->surface_shape.clip_width,
            pg->surface_shape.clip_height,
            surface->pitch,
            out[0], out[1], out[2], out[3]);
#endif
    }

    if (!upload && surface->draw_dirty) {
        if (g_current_method < 0x2000) ++g_readback_reasons[g_current_method / 4];
		if (pg->opengl_enabled && !g_owned_sync) {
			g_download_counts[g_download_site]++;
			g_owned_register_surface_gen = surface->memory_gen;
			surface->memory_gen = Nv2aOwnedRegister(dma.address + surface->offset, surface->pitch * height, width, height, surface->pitch,
			                                        bytes_per_pixel, gl_format, gl_type, swizzle, *gl_buffer);
            if (color && !swizzle && bytes_per_pixel == 4) Nv2aRememberScanout(*gl_buffer, dma.address + surface->offset, width, height, surface->pitch, surface->memory_gen);
			surface->draw_dirty = false;
			surface->write_enabled_cache = false;
			if (swizzle) g_free(buf);
			return;
		}
		if (pg->opengl_enabled) {
			/* read the opengl framebuffer into the surface */

			glo_readpixels(gl_format, gl_type,
						   bytes_per_pixel, surface->pitch,
						   width, height,
						   buf);
			{ static unsigned long long total = 0; g_download_counts[g_download_site]++;
			  if (++total % 2000 == 0) { char line[512]; int n = 0;
			    for (auto& kv : g_download_counts) n += snprintf(line + n, sizeof line - n, " site%d:%llu", kv.first, kv.second);
			    Nv2aGlLog("downloads %llu by site:%s", total, line); } }
			{ static int logged = 0; if (Nv2aGlTraceEnabled() && logged < 30 && swizzle) { logged++;
				uint64_t sum = 0; for (unsigned yy = 0; yy < height; yy++) for (unsigned xx = 0; xx < width * bytes_per_pixel; xx++) sum += buf[yy * surface->pitch + xx];
				Nv2aGlLog("download %s %#x %ux%u swizzle %d mean byte %.2f", color ? "colour" : "zeta", dma.address + surface->offset, width, height, swizzle, (double)sum / (width * height * bytes_per_pixel)); } }

//			assert(glGetError() == GL_NO_ERROR);
		}

        if (swizzle) {
            swizzle_rect(buf,
                         width, height,
                         data + surface->offset,
                         surface->pitch,
                         bytes_per_pixel);
        }

        // memory_region_set_client_dirty(d->vram,
        //                                dma.address + surface->offset,
        //                                surface->pitch * height,
        //                                DIRTY_MEMORY_VGA);

        // Native port: the vertex-fetch mirror (gl_memory_buffer) is refreshed
        // lazily from page generations when vertex data is bound; a forced
        // full-surface copy here is unnecessary.

        surface->draw_dirty = false;
        surface->write_enabled_cache = false;
        // Our own download is now in guest memory: stamp it and match it.
        surface->memory_gen = Nv2aGlHarvestRange(dma.address + surface->offset, surface->pitch * height);
        if (color && !swizzle && bytes_per_pixel == 4) Nv2aRememberScanout(*gl_buffer, dma.address + surface->offset, width, height, surface->pitch, surface->memory_gen);

#ifdef DEBUG_NV2A
        uint8_t *out = data + surface->offset + 64;
        NV2A_DPRINTF("read_surface %s 0x%" HWADDR_PRIx " - 0x%" HWADDR_PRIx ", "
                      "(0x%" HWADDR_PRIx " - 0x%" HWADDR_PRIx ", "
                        "%d %d, %d %d, %d) - %x %x %x %x\n",
            color ? "color" : "zeta",
            dma.address, dma.address + dma.limit,
            dma.address + surface->offset,
            dma.address + surface->pitch * pg->surface_shape.clip_height,
            pg->surface_shape.clip_x, pg->surface_shape.clip_y,
            pg->surface_shape.clip_width, pg->surface_shape.clip_height,
            surface->pitch,
            out[0], out[1], out[2], out[3]);
#endif
    }

    if (swizzle) {
        g_free(buf);
    }
}

static void pgraph_update_surface(NV2AState *d, bool upload,
	bool color_write, bool zeta_write)
{
    PGRAPHState *pg = &d->pgraph;

	if (!pg->opengl_enabled) {
		return;
	}

    pg->surface_shape.z_format = GET_MASK(pg->regs[NV_PGRAPH_SETUPRASTER],
                                          NV_PGRAPH_SETUPRASTER_Z_FORMAT);

    /* FIXME: Does this apply to CLEARs too? */
    color_write = color_write && pgraph_get_color_write_enabled(pg);
    zeta_write = zeta_write && pgraph_get_zeta_write_enabled(pg);

    if (upload && pgraph_get_framebuffer_dirty(pg)) {
        assert(!pg->surface_color.draw_dirty);
        assert(!pg->surface_zeta.draw_dirty);

        pg->surface_color.buffer_dirty = true;
        pg->surface_zeta.buffer_dirty = true;

		glFramebufferTexture2D(GL_FRAMEBUFFER,
								GL_COLOR_ATTACHMENT0,
								GL_TEXTURE_2D,
								0, 0);

		// Native port: the textures are kept (detached) for the surface cache
		// in pgraph_update_surface_part, which retires or reuses them.

		glFramebufferTexture2D(GL_FRAMEBUFFER,
								GL_DEPTH_ATTACHMENT,
								GL_TEXTURE_2D,
								0, 0);
		glFramebufferTexture2D(GL_FRAMEBUFFER,
								GL_DEPTH_STENCIL_ATTACHMENT,
								GL_TEXTURE_2D,
								0, 0);

		memcpy(&pg->last_surface_shape, &pg->surface_shape,
            sizeof(SurfaceShape));
	}

    if ((color_write || (!upload && pg->surface_color.write_enabled_cache))
        && (upload || pg->surface_color.draw_dirty)) {
        pgraph_update_surface_part(d, upload, true);
    }


    if ((zeta_write || (!upload && pg->surface_zeta.write_enabled_cache))
        && (upload || pg->surface_zeta.draw_dirty)) {
        pgraph_update_surface_part(d, upload, false);
    }

}

// Last resolved native texture per unit. pg owns the binding reference;
// never reuse a surface-backed copy, skip write harvesting, or trust only
// register identity. Shared texture objects can have their sampler changed
// by another unit, which must also defeat this shortcut.
struct Nv2aResolvedUnit {
    bool valid = false;
    uint32_t key[24] = {}, params[8] = {};
    uint8_t* ram = nullptr;
    TextureBinding* binding = nullptr;
    uint32_t addr = 0, size = 0, palette = 0, palette_watch = 0, palette_bytes = 0;
    uint64_t generation = 0;
    unsigned long long verify_epoch = 0;   // frame of the last full resolve (periodic content verification)
};
static Nv2aResolvedUnit g_resolved_units[4];
// Periodic verification of cached textures against guest memory: every
// kTextureVerifyFrames each cached texture's bytes are re-hashed; a mismatch
// with unchanged write generations is logged and re-uploaded. Guards against
// any write path that escapes write-watch/stamping (long-session "RGB vomit").
// GH_NO_TEXTURE_VERIFY=1 disables it.
static const unsigned long long kTextureVerifyFrames = 90;
static bool Nv2aTextureVerifyEnabled() { static const bool on = getenv("GH_NO_TEXTURE_VERIFY") == nullptr; return on; }
static uint64_t Nv2aFastHash(const uint8_t* p, size_t n) {
    uint64_t h = 0x9e3779b97f4a7c15ull ^ n; size_t i = 0;
    for (; i + 8 <= n; i += 8) { uint64_t w; memcpy(&w, p + i, 8); h = (h ^ w) * 0xff51afd7ed558ccdull; h ^= h >> 29; }
    for (; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}
static void Nv2aTextureUnitKey(NV2AState* d, unsigned unit, uint32_t (&key)[24]) {
    auto* pg = &d->pgraph;
    const unsigned regs[] = {NV_PGRAPH_TEXCTL0_0, NV_PGRAPH_TEXCTL1_0, NV_PGRAPH_TEXFMT0,
        NV_PGRAPH_TEXFILTER0, NV_PGRAPH_TEXADDRESS0, NV_PGRAPH_TEXPALETTE0,
        NV_PGRAPH_BORDERCOLOR0, NV_PGRAPH_TEXOFFSET0, NV_PGRAPH_TEXIMAGERECT0};
    for (unsigned j=0;j<9;++j) key[j]=pg->regs[regs[j]+unit*4];
    key[9]=pg->dma_a; key[10]=pg->dma_b;
    // A DMA object can be reprogrammed without changing its instance number.
    for (unsigned j=0;j<2;++j) {
        uint32_t instance=key[9+j];
        if (uint64_t(instance)+16 <= d->pramin.ramin_size)
            memcpy(key+11+j*4,d->pramin.ramin_ptr+instance,16);
        else memset(key+11+j*4,0,16);
    }
    key[19]=pg->dxt1_noise[0]; key[20]=pg->dxt1_noise[1];
    key[21]=pg->dxt1_noise_written[0]; key[22]=pg->dxt1_noise_written[1];
    key[23]=(pg->regs[NV_PGRAPH_SHADERPROG]>>(unit*5))&31;
}
static uint64_t Nv2aUnitGeneration(const Nv2aResolvedUnit& u) {
    return std::max(Nv2aGlRangeGeneration(u.addr,u.size),
        u.palette_bytes ? Nv2aGlRangeGeneration(u.palette,u.palette_bytes) : 0);
}

static void pgraph_bind_textures(NV2AState *d)
{
    Nv2aProf prof_scope(2);
    int i;
    PGRAPHState *pg = &d->pgraph;

	if (!(pg->opengl_enabled))
		return;

    NV2A_GL_DGROUP_BEGIN("%s", __func__);

    for (i=0; i<NV2A_MAX_TEXTURES; i++) {
        auto& resolved = g_resolved_units[i];
        if (!pgraph_texture_shader_fetches(pg->regs[NV_PGRAPH_SHADERPROG], i)) {
            resolved.valid = false;
            Nv2aEmptyTextureUnit(i, false);
            continue;
        }
        uint32_t unit_key[24]; Nv2aTextureUnitKey(d, i, unit_key);
#ifndef USE_TEXTURE_CACHE
        static const bool audit = getenv("GH_TEXTURE_AUDIT") != nullptr;
        if (resolved.valid && !audit && !Nv2aGlTraceEnabled() && resolved.ram == d->vram_ptr &&
            resolved.binding == pg->texture_binding[i] && !memcmp(unit_key,resolved.key,sizeof unit_key) &&
            !Nv2aOwnedPendingRange(resolved.addr,resolved.size) &&
            !Nv2aOwnedPendingRange(resolved.palette,resolved.palette_watch) &&
            (!Nv2aTextureVerifyEnabled() || g_nv2a_present_epoch - resolved.verify_epoch < kTextureVerifyFrames)) {
            Nv2aHarvestTexture(resolved.addr,resolved.size);
            if (resolved.palette_watch) Nv2aHarvestTexture(resolved.palette,resolved.palette_watch);
            if (Nv2aUnitGeneration(resolved) == resolved.generation && resolved.binding->param_valid &&
                !memcmp(resolved.params,resolved.binding->param_key,sizeof resolved.params)) {
                Nv2aBindTextureUnit(i,resolved.binding->gl_target,resolved.binding->gl_texture);
                pg->texture_dirty[i] = false; ++g_texture_unit_hits;
                continue;
            }
        }
#endif
        resolved.valid = false;


        uint32_t ctl_0 = pg->regs[NV_PGRAPH_TEXCTL0_0 + i*4];
        uint32_t ctl_1 = pg->regs[NV_PGRAPH_TEXCTL1_0 + i*4];
        uint32_t fmt = pg->regs[NV_PGRAPH_TEXFMT0 + i*4];
        uint32_t filter = pg->regs[NV_PGRAPH_TEXFILTER0 + i*4];
        uint32_t address =  pg->regs[NV_PGRAPH_TEXADDRESS0 + i*4];
        uint32_t palette =  pg->regs[NV_PGRAPH_TEXPALETTE0 + i*4];

        bool enabled = ctl_0 & NV_PGRAPH_TEXCTL0_0_ENABLE;
        // Native port: TEXCTL0 bits0..1 are the colour-key mode (xemu 36334d95
        // NV_PGRAPH_TEXCTL0_0_COLORKEYMODE); the port's shaders do not apply it.
        if (enabled && (ctl_0 & 3u))
            Nv2aGlUnknown("texture colour-key mode %u on stage %d (key %#x) not provided",
                ctl_0 & 3u, i, pg->regs[0x1870 + i * 4]);
        unsigned int min_mipmap_level =
            GET_MASK(ctl_0, NV_PGRAPH_TEXCTL0_0_MIN_LOD_CLAMP);
        unsigned int max_mipmap_level =
            GET_MASK(ctl_0, NV_PGRAPH_TEXCTL0_0_MAX_LOD_CLAMP);

        unsigned int pitch =
            GET_MASK(ctl_1, NV_PGRAPH_TEXCTL1_0_IMAGE_PITCH);

        bool dma_select =
            fmt & NV_PGRAPH_TEXFMT0_CONTEXT_DMA;
        bool cubemap =
            fmt & NV_PGRAPH_TEXFMT0_CUBEMAPENABLE;
        unsigned int dimensionality =
            GET_MASK(fmt, NV_PGRAPH_TEXFMT0_DIMENSIONALITY);
        unsigned int color_format = GET_MASK(fmt, NV_PGRAPH_TEXFMT0_COLOR);
        unsigned int levels = GET_MASK(fmt, NV_PGRAPH_TEXFMT0_MIPMAP_LEVELS);
        unsigned int log_width = GET_MASK(fmt, NV_PGRAPH_TEXFMT0_BASE_SIZE_U);
        unsigned int log_height = GET_MASK(fmt, NV_PGRAPH_TEXFMT0_BASE_SIZE_V);
        unsigned int log_depth = GET_MASK(fmt, NV_PGRAPH_TEXFMT0_BASE_SIZE_P);

        unsigned int rect_width =
            GET_MASK(pg->regs[NV_PGRAPH_TEXIMAGERECT0 + i*4],
                     NV_PGRAPH_TEXIMAGERECT0_WIDTH);
        unsigned int rect_height =
            GET_MASK(pg->regs[NV_PGRAPH_TEXIMAGERECT0 + i*4],
                     NV_PGRAPH_TEXIMAGERECT0_HEIGHT);
#ifdef DEBUG_NV2A
        unsigned int lod_bias =
            GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MIPMAP_LOD_BIAS);
#endif
        unsigned int min_filter = GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MIN);
        unsigned int mag_filter = GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MAG);

        unsigned int addru = GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRU);
        unsigned int addrv = GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRV);
        unsigned int addrp = GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRP);

		bool border_source_color = (fmt & NV_PGRAPH_TEXFMT0_BORDER_SOURCE); // != NV_PGRAPH_TEXFMT0_BORDER_SOURCE_TEXTURE;
        uint32_t border_color = pg->regs[NV_PGRAPH_BORDERCOLOR0 + i*4];

        unsigned int offset = pg->regs[NV_PGRAPH_TEXOFFSET0 + i*4];

        bool palette_dma_select =
            palette & NV_PGRAPH_TEXPALETTE0_CONTEXT_DMA;
        unsigned int palette_length_index =
            GET_MASK(palette, NV_PGRAPH_TEXPALETTE0_LENGTH);
        unsigned int palette_offset =
            palette & NV_PGRAPH_TEXPALETTE0_OFFSET;

        unsigned int palette_length = 0;
        switch (palette_length_index) {
        case NV_PGRAPH_TEXPALETTE0_LENGTH_256: palette_length = 256; break;
        case NV_PGRAPH_TEXPALETTE0_LENGTH_128: palette_length = 128; break;
        case NV_PGRAPH_TEXPALETTE0_LENGTH_64: palette_length = 64; break;
        case NV_PGRAPH_TEXPALETTE0_LENGTH_32: palette_length = 32; break;
        default: assert(false); break;
        }

        /* Check for unsupported features */
        assert(!(filter & NV_PGRAPH_TEXFILTER0_ASIGNED));
        assert(!(filter & NV_PGRAPH_TEXFILTER0_RSIGNED));
        assert(!(filter & NV_PGRAPH_TEXFILTER0_GSIGNED));
        assert(!(filter & NV_PGRAPH_TEXFILTER0_BSIGNED));

        if (!enabled) {
            Nv2aEmptyTextureUnit(i, true);
            continue;
        }
        Nv2aActivateTextureUnit(i);

        // The shortcut above preserves fetch-time write harvesting; all
        // changed/owned/audited textures retain the complete resolution path.

        NV2A_DPRINTF(" texture %d is format 0x%x, off 0x%x (r %d, %d or %d, %d, %d; %d%s),"
                        " filter %x %x, levels %d-%d %d bias %d\n",
                     i, color_format, offset,
                     rect_width, rect_height,
                     1 << log_width, 1 << log_height, 1 << log_depth,
                     pitch,
                     cubemap ? "; cubemap" : "",
                     min_filter, mag_filter,
                     min_mipmap_level, max_mipmap_level, levels,
                     lod_bias);

        assert(color_format < ARRAY_SIZE(kelvin_color_format_map));
        // Native port: depth texture formats 0x2A..0x31 are sampled through
        // SHADOWCTL depth comparison on hardware; the port's pixel shader does
        // not implement it and its format entries are unverified.
        if (color_format == 0x0C &&
            (pg->dxt1_noise_written[0] != pg->dxt1_noise_written[1] || pg->dxt1_noise[0] != pg->dxt1_noise[1] ||
             !pg->dxt1_noise_written[0] || pg->dxt1_noise[0]))
            Nv2aGlUnknown("DXT1 texture on stage %d with decompression noise %s not provided", i,
                pg->dxt1_noise_written[0] ? "enabled or inconsistent" : "state unestablished");
        ColorFormatInfo fi = kelvin_color_format_map[color_format];
        { static unsigned seen[64]; static int n = 0; bool found = false;
          for (int q = 0; q < n; q++) if (seen[q] == color_format) found = true;
          if (!found && n < 64) { seen[n++] = color_format; Nv2aGlLog("texture format %#x first used: stage %d log %ux%u rect %ux%u encoding %d internal %#x", color_format, i, log_width, log_height, rect_width, rect_height, fi.encoding, fi.gl_internal_format); } }
        if (color_format >= 0x2A && color_format <= 0x31)
            Nv2aGlUnknown("depth texture format %#x on stage %d (shadow compare %u) not provided",
                color_format, i, pg->regs[NV_PGRAPH_SHADOWCTL] & 7u);
        ColorFormatInfo f = kelvin_color_format_map[color_format];
        
        if (f.bytes_per_pixel == 0) {
            fprintf(stderr, "nv2a: unimplemented texture color format 0x%x\n",
                    color_format);
            abort();
        }

        unsigned int width, height, depth;
        if (f.encoding == linear) {
            assert(dimensionality == 2);
            width = rect_width;
            height = rect_height;
            depth = 1;
        } else {
            width = 1 << log_width;
            height = 1 << log_height;
            depth = 1 << log_depth;

            /* FIXME: What about 3D mipmaps? */
            levels = MIN(levels, max_mipmap_level + 1);
            if (f.encoding == swizzled) {
                /* Discard mipmap levels that would be smaller than 1x1.
                 * FIXME: Is this actually needed?
                 *
                 * >> Level 0: 32 x 4
                 *    Level 1: 16 x 2
                 *    Level 2: 8 x 1
                 *    Level 3: 4 x 1
                 *    Level 4: 2 x 1
                 *    Level 5: 1 x 1
                 */
                levels = MIN(levels, MAX(log_width, log_height) + 1);
            } else {
                /* OpenGL requires DXT textures to always have a width and
                 * height a multiple of 4. The Xbox and DirectX handles DXT
                 * textures smaller than 4 by padding the reset of the block.
                 *
                 * See:
                 * https://msdn.microsoft.com/en-us/library/windows/desktop/bb204843(v=vs.85).aspx
                 * https://msdn.microsoft.com/en-us/library/windows/desktop/bb694531%28v=vs.85%29.aspx#Virtual_Size
                 *
                 * Work around this for now by discarding mipmap levels that
                 * would result in too-small textures. A correct solution
                 * will be to decompress these levels manually, or add texture
                 * sampling logic.
                 *
                 * >> Level 0: 64 x 8
                 *    Level 1: 32 x 4
                 *    Level 2: 16 x 2 << Ignored
                 * >> Level 0: 16 x 16
                 *    Level 1: 8 x 8
                 *    Level 2: 4 x 4 << OK!
                 */
                if (log_width < 2 || log_height < 2) {
                    /* Base level is smaller than 4x4... */
                    levels = 1;
                } else {
                    levels = MIN(levels, MIN(log_width, log_height) - 1);
                }
            }
            assert(levels > 0);
            // Native port (as xemu 36334d95 texture.c): a base level beyond the
            // uploaded chain leaves the GL texture incomplete (samples black).
            min_mipmap_level = MIN(levels - 1, min_mipmap_level);
            max_mipmap_level = MIN(levels - 1, max_mipmap_level);
        }

        hwaddr dma_len;
        uint8_t *texture_data;
        if (dma_select) {
            texture_data = (uint8_t*)nv_dma_map(d, pg->dma_b, &dma_len);
        } else {
            texture_data = (uint8_t*)nv_dma_map(d, pg->dma_a, &dma_len);
        }
        assert(offset < dma_len);
        texture_data += offset;

        hwaddr palette_dma_len;
        uint8_t *palette_data;
        if (palette_dma_select) {
            palette_data = (uint8_t*)nv_dma_map(d, pg->dma_b, &palette_dma_len);
        } else {
            palette_data = (uint8_t*)nv_dma_map(d, pg->dma_a, &palette_dma_len);
        }
        assert(palette_offset < palette_dma_len);
        palette_data += palette_offset;

        NV2A_DPRINTF(" - 0x%tx\n", texture_data - d->vram_ptr);

        size_t length = 0;
        if (f.encoding == linear) {
            assert(cubemap == false);
            assert(dimensionality == 2);
            length = height * pitch;
        } else {
            if (dimensionality >= 2) {
                unsigned int w = width, h = height;
                unsigned int level;
                if (f.encoding == swizzled) {
                    for (level = 0; level < levels; level++) {
                        w = MAX(w, 1); h = MAX(h, 1);
                        length += w * h * f.bytes_per_pixel;
                        w /= 2;
                        h /= 2;
                    }
                } else {
                    /* Compressed textures are a bit different */
                    unsigned int block_size;
                    if (f.gl_internal_format ==
                            GL_COMPRESSED_RGBA_S3TC_DXT1_EXT) {
                        block_size = 8;
                    } else {
                        block_size = 16;
                    }

                    for (level = 0; level < levels; level++) {
                        w = MAX(w, 4); h = MAX(h, 4);
                        length += w/4 * h/4 * block_size;
                        w /= 2; h /= 2;
                    }
                }
                if (cubemap) {
                    assert(dimensionality == 2);
                    length *= 6;
                }
                if (dimensionality >= 3) {
                    length *= depth;
                }
            }
        }

		// Native port: a texture that is exactly a pending (renderer-owned)
		// surface with the same texel layout is copied from the surface on the
		// GPU; any other texture completes the deferred downloads it reads.
		GLuint owner_texture = 0;
		if (!cubemap && dimensionality == 2 && levels == 1 && depth <= 1 &&
		    (color_format == NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8 || color_format == NV097_SET_TEXTURE_FORMAT_COLOR_SZ_X8R8G8B8 ||
		     color_format == NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8 || color_format == NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X8R8G8B8 ||
		     color_format == NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R5G6B5 || color_format == NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R5G6B5))
			owner_texture = Nv2aOwnedMatch((uint32_t)(texture_data - d->vram_ptr), (uint32_t)length, width, height, f.bytes_per_pixel,
			                               f.gl_format, f.gl_type, f.encoding == swizzled, pitch);
		bool owner_depth = false;
		if (!owner_texture && !cubemap && dimensionality == 2 && levels == 1 && depth <= 1 && f.encoding == linear &&
		    (color_format == NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8 || color_format == NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X8R8G8B8) &&
		    (owner_texture = Nv2aOwnedMatchDepth((uint32_t)(texture_data - d->vram_ptr), (uint32_t)length, width, height, pitch)))
			owner_depth = true;
		{   // Diagnostics: textures over pending renderer-owned memory (render-to-texture,
		    // GPU blit targets), once per shape: whether the GPU copy path matched.
		    uint32_t taddr = (uint32_t)(texture_data - d->vram_ptr);
		    if (Nv2aOwnedPendingRange(taddr, (uint32_t)length)) {
		        static std::set<uint64_t> seen;
		        uint64_t k = (uint64_t(taddr) << 32) ^ (uint64_t(color_format) << 24) ^ (uint64_t(width) << 12) ^ height ^ (uint64_t(owner_texture != 0) << 63);
		        if (seen.size() < 200 && seen.insert(k).second) {
		            char owners[600]; Nv2aOwnedDescribe(taddr, (uint32_t)length, owners, sizeof owners);
		            Nv2aGlLog("texture over owned surface: stage %d addr %#x fmt %#x %ux%u pitch %u levels %u dim %u cube %d encoding %d gl %#x/%#x len %zu -> %s; shaderprog %08x combinectl %08x frame %llu; owners:%s",
		                i, taddr, color_format, width, height, pitch, levels, dimensionality, (int)cubemap, (int)f.encoding, f.gl_format, f.gl_type, (size_t)length, owner_depth ? "GPU depth copy" : owner_texture ? "GPU copy" : "download",
		                pg->regs[NV_PGRAPH_SHADERPROG], pg->regs[NV_PGRAPH_COMBINECTL], g_nv2a_present_epoch, owners);
		        }
		    }
		}
		if (!owner_texture) {
			Nv2aOwnedFlushFor(f.encoding == swizzled ? "texture-swizzled" : f.encoding == linear ? "texture-linear" : "texture-compressed", (uint32_t)(texture_data - d->vram_ptr), (uint32_t)length);
			if (palette_length) Nv2aOwnedFlush((uint32_t)(palette_data - d->vram_ptr), (uint32_t)palette_length * 4);
            // CPU decoding/reloading can write after the pusher's PUT-time
            // harvest (e.g. while FLIP_STALL waits). A cached texture must
            // observe writes at its own fetch, even with unchanged registers.
            Nv2aHarvestTexture((uint32_t)(texture_data - d->vram_ptr), (uint32_t)length);
            if (palette_length) Nv2aHarvestTexture((uint32_t)(palette_data - d->vram_ptr), (uint32_t)palette_length * 4);
		}
		TextureShape state;
		memset(&state, 0, sizeof(state)); // native port: deterministic padding (cache key)
		state.cubemap = cubemap;
		state.dimensionality = dimensionality;
		state.color_format = color_format;
		state.levels = levels;
		state.width = width;
		state.height = height;
		state.depth = depth;
		state.min_mipmap_level = min_mipmap_level;
		state.max_mipmap_level = max_mipmap_level;
        state.pitch = pitch;

        { // diagnostic: dump linear 32-bit textures (first use of each address, then every 200th bind)
          static std::unordered_map<uint32_t, unsigned> binds;
          if (Nv2aGlTraceEnabled() && (f.encoding == linear || f.encoding == swizzled) && f.bytes_per_pixel == 4 && width <= 1024 && height <= 1024 && depth <= 1) {
            uint32_t phys = (uint32_t)(texture_data - d->vram_ptr);
            unsigned c = binds[phys]++;
            if (c == 0 || c % 200 == 0) Nv2aGlDumpTexture(phys, width, height, f.encoding == swizzled ? width * 4 : pitch, color_format, c);
          } }
#ifdef USE_TEXTURE_CACHE
		TextureKey key;
		key.state = state;
		key.data_hash = fast_hash(texture_data, length, 5003)
			^ fnv_hash(palette_data, palette_length);
		key.texture_data = texture_data;
		key.palette_data = palette_data;

        gpointer cache_key = g_malloc(sizeof(TextureKey));
        memcpy(cache_key, &key, sizeof(TextureKey));

        GError *err;
        TextureBinding *binding = (TextureBinding *)g_lru_cache_get(pg->texture_cache, cache_key, &err);
        assert(binding);
        binding->refcnt++;
#else
        // Native port: textures are cached by shape and guest address and
        // re-uploaded only when guest writes touched their data or palette
        // since the cached upload (page generations, gpu.inc).
        TextureBinding *binding = nullptr;
        if (owner_texture) {
            // One GL texture per (address, shape), refreshed from the surface at each bind.
            // Least-recently-used entries are released past the cap: scenes keep
            // allocating render targets at new addresses/shapes, and a fixed
            // limit stopped the game after enough room changes (STOP 228).
            struct DirectTexture { TextureShape shape; uint32_t addr; TextureBinding* binding; uint64_t last_use; };
            static std::vector<DirectTexture> direct;
            static uint64_t direct_clock = 0;
            uint32_t addr = (uint32_t)(texture_data - d->vram_ptr);
            for (auto& t : direct) if (t.addr == addr && !memcmp(&t.shape, &state, sizeof state)) { binding = t.binding; t.last_use = ++direct_clock; }
            if (!binding) {
                constexpr size_t kDirectTextureCap = 128;
                if (direct.size() >= kDirectTextureCap) {
                    size_t oldest = 0;
                    for (size_t j = 1; j < direct.size(); ++j) if (direct[j].last_use < direct[oldest].last_use) oldest = j;
                    texture_binding_destroy(direct[oldest].binding);   // drops the list's reference; a bound slot keeps its own
                    direct.erase(direct.begin() + oldest);
                    ++g_direct_texture_evictions;
                }
                binding = (TextureBinding*)g_malloc(sizeof(TextureBinding));
                binding->param_valid = false;
                binding->gl_target = GL_TEXTURE_2D; binding->refcnt = 1;   // the direct list's reference
                binding->scale = Nv2aGlRenderScale();   // keeps the surface's render resolution
                glGenTextures(1, &binding->gl_texture);
                Nv2aForeignBindTexture(GL_TEXTURE_2D, binding->gl_texture);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
                if (f.gl_swizzle_mask) glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, f.gl_swizzle_mask);
                glTexImage2D(GL_TEXTURE_2D, 0, f.gl_internal_format, width * binding->scale, height * binding->scale, 0, f.gl_format, f.gl_type, nullptr);
                direct.push_back({state, addr, binding, ++direct_clock});
            }
            if (owner_depth) Nv2aOwnedCopyDepthAsColor(owner_texture, binding->gl_texture, width, height, f.gl_internal_format);
            else Nv2aOwnedCopyFlipped(owner_texture, binding->gl_texture, width, height);
            ++g_direct_texture_copies;
            binding->refcnt++;       // pg->texture_binding[i]'s reference
        } else {
            struct CachedTexture { TextureShape shape; uint8_t* data; uint8_t* palette; size_t length, palette_bytes; TextureBinding* binding; uint64_t gen, audit_hash, verify_hash; unsigned long long verify_epoch; };
            static std::unordered_map<uint64_t, std::vector<CachedTexture>> cache;
            static size_t cached_count = 0;
            size_t palette_bytes = (color_format == NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8) ? palette_length * 4 : 0;
            uint64_t now = Nv2aGlCurrentGeneration();
            uint64_t data_gen = std::max(Nv2aGlRangeGeneration((uint32_t)(texture_data - d->vram_ptr), (uint32_t)length),
                                         palette_bytes ? Nv2aGlRangeGeneration((uint32_t)(palette_data - d->vram_ptr), (uint32_t)palette_bytes) : 0);
            struct { TextureShape s; uint8_t* t; uint8_t* p; } keyrec; memset(&keyrec, 0, sizeof keyrec);
            keyrec.s = state; keyrec.t = texture_data; keyrec.p = palette_bytes ? palette_data : nullptr;
            uint64_t key = fnv_hash((const uint8_t*)&keyrec, sizeof keyrec);
            CachedTexture* found = nullptr;
            for (auto& c : cache[key])
                if (!memcmp(&c.shape, &state, sizeof state) && c.data == texture_data && c.palette == keyrec.p && c.length == length) found = &c;
            static const bool audit = getenv("GH_TEXTURE_AUDIT") != nullptr;
            uint64_t audit_hash = audit ? fnv_hash(texture_data, length) ^ (palette_bytes ? fnv_hash(palette_data, palette_bytes) : 0) : 0;
            bool stale = audit && found && data_gen <= found->gen && audit_hash != found->audit_hash;
            if (stale) Nv2aGlLog("texture cache stale: addr %#x fmt %#x %ux%u bytes %zu gen %llu <= %llu frame %llu", unsigned(texture_data-d->vram_ptr), color_format, width, height, length, data_gen, found->gen, g_nv2a_present_epoch);
            auto content_hash = [&]() { return Nv2aFastHash(texture_data, length) ^ (palette_bytes ? Nv2aFastHash(palette_data, palette_bytes) * 31 : 0); };
            if (!stale && found && data_gen <= found->gen && Nv2aTextureVerifyEnabled() && g_nv2a_present_epoch - found->verify_epoch >= kTextureVerifyFrames) {
                found->verify_epoch = g_nv2a_present_epoch;
                if (content_hash() != found->verify_hash) {
                    stale = true;
                    static unsigned long long healed = 0;
                    if (++healed <= 60 || healed % 500 == 0)
                        Nv2aGlLog("texture verify %llu: cached texture at %#x (fmt %#x %ux%u, %zu bytes%s) differs from memory with unchanged generation %llu <= %llu; re-uploading (frame %llu)",
                            healed, unsigned(texture_data-d->vram_ptr), color_format, width, height, length, palette_bytes ? ", palette" : "", data_gen, found->gen, g_nv2a_present_epoch);
                }
            }
            if (found && data_gen <= found->gen && !stale) {
                binding = found->binding;
            } else {
                if (cached_count > 4096) {
                    for (auto& bucket : cache) for (auto& c : bucket.second) texture_binding_destroy(c.binding);
                    cache.clear(); cached_count = 0; found = nullptr;
                }
                binding = generate_texture(state, texture_data, palette_data);
                // generate_texture's initial reference belongs to the cache
                uint64_t vh = Nv2aTextureVerifyEnabled() ? content_hash() : 0;
                if (found) { texture_binding_destroy(found->binding); found->binding = binding; found->gen = now; found->audit_hash = audit_hash; found->verify_hash = vh; found->verify_epoch = g_nv2a_present_epoch; }
                else { cache[key].push_back({state, texture_data, keyrec.p, length, palette_bytes, binding, now, audit_hash, vh, g_nv2a_present_epoch}); ++cached_count; }
            }
            binding->refcnt++;       // pg->texture_binding[i]'s reference
        }
#endif

        Nv2aBindTextureUnit(i, binding->gl_target, binding->gl_texture);


        if (f.encoding == linear) {
            /* sometimes games try to set mipmap min filters on linear textures.
             * this could indicate a bug... */
            switch (min_filter) {
            case NV_PGRAPH_TEXFILTER0_MIN_BOX_NEARESTLOD:
            case NV_PGRAPH_TEXFILTER0_MIN_BOX_TENT_LOD:
                min_filter = NV_PGRAPH_TEXFILTER0_MIN_BOX_LOD0;
                break;
            case NV_PGRAPH_TEXFILTER0_MIN_TENT_NEARESTLOD:
            case NV_PGRAPH_TEXFILTER0_MIN_TENT_TENT_LOD:
                min_filter = NV_PGRAPH_TEXFILTER0_MIN_TENT_LOD0;
                break;
            }
        }

        // Wrap value 0 is not a D3D address mode, but the title samples with it
        // (TEXADDRESS 0xffff0000 seen mid-game, Owner session 2026-10-17). xemu
        // passes 0 to glTexParameteri, which GL rejects, so its textures keep the
        // default GL_REPEAT. Do the same explicitly and record it once per value.
        if (!addru || (dimensionality > 1 && !addrv) || (dimensionality > 2 && !addrp)) {
            static std::set<uint32_t> noted;
            if (noted.size() < 16 && noted.insert(address).second)
                Nv2aGlLog("texture wrap 0 on stage %u (mode %#x fmt %#x, address %#x): using REPEAT as xemu", i, (pg->regs[NV_PGRAPH_SHADERPROG] >> (i*5)) & 31, color_format, address);
            if (!addru) addru = 1;
            if (dimensionality > 1 && !addrv) addrv = 1;
            if (dimensionality > 2 && !addrp) addrp = 1;
        }
        // Native port: sampler parameters are per texture object; re-apply them
        // only when they differ from what this object last received.
        unsigned anisotropy = Nv2aTextureAnisotropy(binding->gl_target, !owner_texture && f.encoding != linear && levels > 1,
            pgraph_texture_min_filter_map[min_filter],pgraph_texture_mag_filter_map[mag_filter]);
        uint32_t param_key[8] = { min_filter, mag_filter, addru, dimensionality > 1 ? addrv : 0u, dimensionality > 2 ? addrp : 0u,
                                  border_source_color ? 1u : 0u, border_source_color ? border_color : 0u, anisotropy };
        if (!binding->param_valid || memcmp(binding->param_key, param_key, sizeof param_key)) {
        memcpy(binding->param_key, param_key, sizeof param_key); binding->param_valid = true;
        glTexParameteri(binding->gl_target, GL_TEXTURE_MIN_FILTER,
            pgraph_texture_min_filter_map[min_filter]);
        glTexParameteri(binding->gl_target, GL_TEXTURE_MAG_FILTER,
            pgraph_texture_mag_filter_map[mag_filter]);

        Nv2aApplyAnisotropy(binding->gl_target,anisotropy);

        /* Texture wrapping */
        assert(addru < ARRAY_SIZE(pgraph_texture_addr_map));
        glTexParameteri(binding->gl_target, GL_TEXTURE_WRAP_S,
            pgraph_texture_addr_map[addru]);
        if (dimensionality > 1) {
            assert(addrv < ARRAY_SIZE(pgraph_texture_addr_map));
            glTexParameteri(binding->gl_target, GL_TEXTURE_WRAP_T,
                pgraph_texture_addr_map[addrv]);
        }
        if (dimensionality > 2) {
            assert(addrp < ARRAY_SIZE(pgraph_texture_addr_map));
            glTexParameteri(binding->gl_target, GL_TEXTURE_WRAP_R,
                pgraph_texture_addr_map[addrp]);
        }

        /* FIXME: Only upload if necessary? [s, t or r = GL_CLAMP_TO_BORDER] */
        if (border_source_color) {
            GLfloat gl_border_color[] = {
                /* FIXME: Color channels might be wrong order */
                ((border_color >> 16) & 0xFF) / 255.0f, /* red */
                ((border_color >> 8) & 0xFF) / 255.0f,  /* green */
                (border_color & 0xFF) / 255.0f,         /* blue */
                ((border_color >> 24) & 0xFF) / 255.0f  /* alpha */
            };
            glTexParameterfv(binding->gl_target, GL_TEXTURE_BORDER_COLOR,
                gl_border_color);
        }

        }
        if (pg->texture_binding[i]) {
            texture_binding_destroy(pg->texture_binding[i]);
        }
        pg->texture_binding[i] = binding;
        pg->texture_dirty[i] = false;
#ifndef USE_TEXTURE_CACHE
        if (!owner_texture) {
            memcpy(resolved.key,unit_key,sizeof unit_key);
            memcpy(resolved.params,binding->param_key,sizeof resolved.params);
            resolved.ram=d->vram_ptr; resolved.binding=binding;
            resolved.addr=uint32_t(texture_data-d->vram_ptr); resolved.size=uint32_t(length);
            resolved.palette=uint32_t(palette_data-d->vram_ptr); resolved.palette_watch=palette_length*4;
            resolved.palette_bytes=color_format==NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8 ? palette_length*4 : 0;
            resolved.generation=Nv2aUnitGeneration(resolved); resolved.valid=true; resolved.verify_epoch=g_nv2a_present_epoch;
        }
#endif
    }
    NV2A_GL_DGROUP_END();
}

static void pgraph_apply_anti_aliasing_factor(PGRAPHState *pg,
                                              unsigned int *width,
                                              unsigned int *height)
{
    switch (pg->surface_shape.anti_aliasing) {
    case NV097_SET_SURFACE_FORMAT_ANTI_ALIASING_CENTER_1:
        break;
    case NV097_SET_SURFACE_FORMAT_ANTI_ALIASING_CENTER_CORNER_2:
        if (width) { *width *= 2; }
        break;
    case NV097_SET_SURFACE_FORMAT_ANTI_ALIASING_SQUARE_OFFSET_4:
        if (width) { *width *= 2; }
        if (height) { *height *= 2; }
        break;
    default:
        assert(false);
        break;
    }
}

static void pgraph_get_surface_dimensions(PGRAPHState *pg,
                                          unsigned int *width,
                                          unsigned int *height)
{
    bool swizzle = (pg->surface_type == NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE);
    if (swizzle) {
        *width = 1 << pg->surface_shape.log_width;
        *height = 1 << pg->surface_shape.log_height;
    } else {
        // Native port: screen coordinates are absolute; the clip rectangle
        // (SET_SURFACE_CLIP_HORIZONTAL/VERTICAL x,y,width,height) limits drawing
        // inside the surface. The surface therefore extends to clip_x+width and
        // clip_y+height (the reference used width/height and ignored x/y).
        *width = pg->surface_shape.clip_x + pg->surface_shape.clip_width;
        *height = pg->surface_shape.clip_y + pg->surface_shape.clip_height;
    }
}

static void pgraph_update_memory_buffer(NV2AState *d, hwaddr addr, hwaddr size,
                                        bool f)
{
	Nv2aProf prof_scope(17);
	Nv2aBindArrayBuffer( d->pgraph.gl_memory_buffer);

	hwaddr end = TARGET_PAGE_ALIGN(addr + size);
	addr &= TARGET_PAGE_MASK;
	Nv2aOwnedFlushFor("vertex-memory", (uint32_t)addr, (uint32_t)(end - addr));

	assert(end <= d->vram_size);

	// Native port: copy only pages written since this mirror last copied them
	// (or every page when forced), using the guest write generations.
	auto& copied_gen = d->pgraph.vertex_memory_generation;
	hwaddr run = end;
	for (hwaddr page = addr; page <= end; page += TARGET_PAGE_SIZE) {
		bool stale = page < end && (f || Nv2aGlRangeGeneration((uint32_t)page, TARGET_PAGE_SIZE) > copied_gen[page >> 12]);
		if (stale) {
			if (run == end) run = page;
			copied_gen[page >> 12] = Nv2aGlRangeGeneration((uint32_t)page, TARGET_PAGE_SIZE);
		} else if (run != end) {
			glBufferSubData(GL_ARRAY_BUFFER, run, page - run, d->vram_ptr + run);
			g_vertex_mirror_stats[0]++; g_vertex_mirror_stats[1] += page - run;
			run = end;
		}
	}

//		auto error = glGetError();
//		assert(error == GL_NO_ERROR);
}

static void pgraph_bind_vertex_attributes(NV2AState *d,
                                          unsigned int num_elements,
                                          bool inline_data,
                                          unsigned int inline_stride)
{
	unsigned int i, j;
    PGRAPHState *pg = &d->pgraph;
    Nv2aProf prof_scope(16);

	assert(pg->opengl_enabled);

    if (inline_data) {
        NV2A_GL_DGROUP_BEGIN("%s (num_elements: %d inline stride: %d)",
                             __func__, num_elements, inline_stride);
    } else {
        NV2A_GL_DGROUP_BEGIN("%s (num_elements: %d)", __func__, num_elements);
    }

    for (i=0; i<NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
        VertexAttribute *vertex_attribute = &pg->vertex_attributes[i];
        if (vertex_attribute->count) {
            uint8_t *data;
            unsigned int in_stride;
            if (inline_data && vertex_attribute->needs_conversion) {
                data = (uint8_t*)pg->inline_array
                        + vertex_attribute->inline_array_offset;
                in_stride = inline_stride;
            } else {
                hwaddr dma_len;
                if (vertex_attribute->dma_select) {
                    data = (uint8_t*)nv_dma_map(d, pg->dma_vertex_b, &dma_len);
                } else {
                    data = (uint8_t*)nv_dma_map(d, pg->dma_vertex_a, &dma_len);
                }

                assert(vertex_attribute->offset < dma_len);
                data += vertex_attribute->offset;

                in_stride = vertex_attribute->stride;
            }

            if (vertex_attribute->needs_conversion) {
                NV2A_DPRINTF("converted %d\n", i);

                unsigned int out_stride = vertex_attribute->converted_size
                                        * vertex_attribute->converted_count;

                if (num_elements > vertex_attribute->converted_elements) {
                    vertex_attribute->converted_buffer = (uint8_t*)g_realloc(
                        vertex_attribute->converted_buffer,
                        num_elements * out_stride);
                }

                for (j=vertex_attribute->converted_elements; j<num_elements; j++) {
                    uint8_t *in = data + j * in_stride;
                    uint8_t *out = vertex_attribute->converted_buffer + j * out_stride;

                    switch (vertex_attribute->format) {
                    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_CMP: {
                        uint32_t p = ldl_le_p((uint32_t*)in);
                        float *xyz = (float*)out;
                        xyz[0] = ((int32_t)(((p >>  0) & 0x7FF) << 21) >> 21)
                                                                      / 1023.0f;
                        xyz[1] = ((int32_t)(((p >> 11) & 0x7FF) << 21) >> 21)
                                                                      / 1023.0f;
                        xyz[2] = ((int32_t)(((p >> 22) & 0x3FF) << 22) >> 22)
                                                                       / 511.0f;
                        break;
                    }
                    default:
                        assert(false);
                        break;
                    }
                }


                Nv2aBindArrayBuffer( vertex_attribute->gl_converted_buffer);
                if (num_elements != vertex_attribute->converted_elements) {
                    glBufferData(GL_ARRAY_BUFFER,
                                 num_elements * out_stride,
                                 vertex_attribute->converted_buffer,
                                 GL_DYNAMIC_DRAW);
                    vertex_attribute->converted_elements = num_elements;
                }


                Nv2aAttribPointer(i, vertex_attribute->gl_converted_buffer,
                    vertex_attribute->converted_count,
                    vertex_attribute->gl_type,
                    vertex_attribute->gl_normalize,
                    out_stride,
                    0);
            } else if (inline_data) {
                Nv2aBindArrayBuffer( pg->gl_inline_array_buffer);
                Nv2aAttribPointer(i, pg->gl_inline_array_buffer,
                                      vertex_attribute->gl_count,
                                      vertex_attribute->gl_type,
                                      vertex_attribute->gl_normalize,
                                      inline_stride,
                                      (void*)(uintptr_t)vertex_attribute->inline_array_offset);
            } else {
                hwaddr addr = data - d->vram_ptr;
                pgraph_update_memory_buffer(d, addr,
                                            num_elements * vertex_attribute->stride,
                                            false);
                Nv2aAttribPointer(i, pg->gl_memory_buffer,
                    vertex_attribute->gl_count,
                    vertex_attribute->gl_type,
                    vertex_attribute->gl_normalize,
                    vertex_attribute->stride,
                    (void*)(uint64_t)(addr));
            }
            Nv2aAttribEnable(i, true);
        } else {
            Nv2aAttribEnable(i, false);

            Nv2aAttribValue(i, vertex_attribute->inline_value);
        }
    }
    NV2A_GL_DGROUP_END();
}

static unsigned int pgraph_bind_inline_array(NV2AState *d)
{
	int i;

    PGRAPHState *pg = &d->pgraph;

	assert(pg->opengl_enabled);

    unsigned int offset = 0;
    for (i=0; i<NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
        VertexAttribute *vertex_attribute = &pg->vertex_attributes[i];
        if (vertex_attribute->count) {
            vertex_attribute->inline_array_offset = offset;

            NV2A_DPRINTF("bind inline vertex_attribute %d size=%d, count=%d\n",
                i, vertex_attribute->size, vertex_attribute->count);
            offset += vertex_attribute->size * vertex_attribute->count;
            assert(offset % 4 == 0);
        }
    }

    unsigned int vertex_size = offset;


    unsigned int index_count = pg->inline_array_length*4 / vertex_size;

    NV2A_DPRINTF("draw inline array %d, %d\n", vertex_size, index_count);

    Nv2aBindArrayBuffer( pg->gl_inline_array_buffer);
    glBufferData(GL_ARRAY_BUFFER, pg->inline_array_length*4, pg->inline_array,
                 GL_DYNAMIC_DRAW);

    pgraph_bind_vertex_attributes(d, index_count, true, vertex_size);

    return index_count;
}

/* 16 bit to [0.0, F16_MAX = 511.9375] */
static float convert_f16_to_float(uint16_t f16) {
    if (f16 == 0x0000) { return 0.0f; }
    uint32_t i = (f16 << 11) + 0x3C000000;
    return *(float*)&i;
}

/* 24 bit to [0.0, F24_MAX] */
static float convert_f24_to_float(uint32_t f24) {
    assert(!(f24 >> 24));
    f24 &= 0xFFFFFF;
    if (f24 == 0x000000) { return 0.0f; }
    uint32_t i = f24 << 7;
    return *(float*)&i;
}

extern void __R6G5B5ToARGBRow_C(const uint8_t* src_r6g5b5, uint8_t* dst_argb, int width);
extern void ____YUY2ToARGBRow_C(const uint8_t* src_yuy2, uint8_t* rgb_buf, int width);
extern void ____UYVYToARGBRow_C(const uint8_t* src_uyvy, uint8_t* rgb_buf, int width);

/* 'converted_format' indicates the format that results when convert_texture_data() returns non-NULL converted_data. */
static const int converted_format = NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8;

static uint8_t* convert_texture_data(const unsigned int color_format,
                                     const uint8_t *data,
                                     const uint8_t *palette_data,
                                     const unsigned int width,
                                     const unsigned int height,
                                     const unsigned int depth,
                                     const unsigned int row_pitch,
                                     const unsigned int slice_pitch)
{
	// Note : Unswizzle is already done when entering here
	switch (color_format) {
	case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8: {
		// Test-case : WWE RAW2
		assert(depth == 1); /* FIXME */
		uint8_t* converted_data = (uint8_t*)g_malloc(width * height * 4);
		unsigned int x, y;
		for (y = 0; y < height; y++) {
			for (x = 0; x < width; x++) {
				uint8_t index = data[y * row_pitch + x];
				uint32_t color = *(uint32_t*)(palette_data + index * 4);
				*(uint32_t*)(converted_data + y * width * 4 + x * 4) = color;
			}
		}
		return converted_data;
	}
	case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X7SY9: {
		assert(false); /* FIXME */
		return NULL;
	}
	case NV097_SET_TEXTURE_FORMAT_COLOR_LC_IMAGE_CR8YB8CB8YA8: {
		// Test-case : WWE RAW2
		assert(depth == 1); /* FIXME */
		uint8_t* converted_data = (uint8_t*)g_malloc(width * height * 4);
		unsigned int y;
		for (y = 0; y < height; y++) {
			const uint8_t* line = &data[y * width * 2];
			uint8_t* pixel = &converted_data[(y * width) * 4];
			____YUY2ToARGBRow_C(line, pixel, width);
			// Note : LC_IMAGE_CR8YB8CB8YA8 suggests UYVY format,
			// but for an unknown reason, the actual encoding is YUY2
		}
		return converted_data;
	}	
	case NV097_SET_TEXTURE_FORMAT_COLOR_LC_IMAGE_YB8CR8YA8CB8: {
		assert(depth == 1); /* FIXME */
		uint8_t* converted_data = (uint8_t*)g_malloc(width * height * 4);
		unsigned int y;
		for (y = 0; y < height; y++) {
			const uint8_t* line = &data[y * width * 2];
			uint8_t* pixel = &converted_data[(y * width) * 4];
			____UYVYToARGBRow_C(line, pixel, width); // TODO : Validate LC_IMAGE_YB8CR8YA8CB8 indeed requires ____UYVYToARGBRow_C()
		}
		return converted_data;
	}
	case NV097_SET_TEXTURE_FORMAT_COLOR_LC_IMAGE_A4V6YB6A4U6YA6: {
		assert(false); /* FIXME */
		return NULL;
	}
	case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8CR8CB8Y8: {
		assert(false); /* FIXME */
		return NULL;
	}
	case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R6G5B5:
	case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R6G5B5: {
		assert(depth == 1); /* FIXME */
		uint8_t *converted_data = (uint8_t*)g_malloc(width * height * 4);
		unsigned int y;
		for (y = 0; y < height; y++) {
			uint16_t rgb655 = *(uint16_t*)(data + y * row_pitch);
			int8_t *pixel = (int8_t*)&converted_data[(y * width) * 4];
			__R6G5B5ToARGBRow_C((const uint8_t*)rgb655, (uint8_t*)pixel, width);
		}
		return converted_data;
	}
	default:
        return NULL;
    }
}

/* returns the format of the output, either identical to the input format, or the converted format - see converted_format */
static int upload_gl_texture(GLenum gl_target,
                              const TextureShape s,
                              const uint8_t *texture_data,
                              const uint8_t *palette_data)
{
	//assert(pg->opengl_enabled);
    int resulting_format = s.color_format;
    ColorFormatInfo f = kelvin_color_format_map[s.color_format];

    if (f.encoding == linear) {
        assert(gl_target == GL_TEXTURE_2D && s.levels == 1);
        GLint unpack_row, unpack_alignment;
        glGetIntegerv(GL_UNPACK_ROW_LENGTH,&unpack_row); glGetIntegerv(GL_UNPACK_ALIGNMENT,&unpack_alignment);
        glPixelStorei(GL_UNPACK_ALIGNMENT,1);
        /* Can't handle strides unaligned to pixels */
        assert(s.pitch % f.bytes_per_pixel == 0);
        glPixelStorei(GL_UNPACK_ROW_LENGTH,
                      s.pitch / f.bytes_per_pixel);

		uint8_t *unswizzled = NULL;
        if (f.encoding == swizzled) { // TODO : Verify this works correctly
            unswizzled = (uint8_t*)g_malloc(s.height * s.pitch);
            unswizzle_rect(texture_data, s.width, s.height,
                            unswizzled, s.pitch, f.bytes_per_pixel);
        }
        uint8_t *converted = convert_texture_data(s.color_format, unswizzled ? unswizzled : texture_data,
                                                  palette_data,
                                                  s.width, s.height, 1,
                                                  s.pitch, 0);

        resulting_format = converted ? converted_format : s.color_format;
        ColorFormatInfo cf = kelvin_color_format_map[resulting_format];
        if (converted) glPixelStorei(GL_UNPACK_ROW_LENGTH,s.width); // converted rows are tightly packed
        glTexImage2D(gl_target, 0, cf.gl_internal_format,
                     s.width, s.height, 0,
                     cf.gl_format, cf.gl_type,
                     converted ? converted : unswizzled ? unswizzled : texture_data);

        if (converted) {
          g_free(converted);
        }
		if (unswizzled) {
			g_free(unswizzled);
        }

        glPixelStorei(GL_UNPACK_ROW_LENGTH, unpack_row);
        glPixelStorei(GL_UNPACK_ALIGNMENT, unpack_alignment);
        return resulting_format;
    }

    switch(gl_target) {
    case GL_TEXTURE_1D:
        assert(false);
        break;
    case GL_TEXTURE_2D:
    case GL_TEXTURE_CUBE_MAP_POSITIVE_X:
    case GL_TEXTURE_CUBE_MAP_NEGATIVE_X:
    case GL_TEXTURE_CUBE_MAP_POSITIVE_Y:
    case GL_TEXTURE_CUBE_MAP_NEGATIVE_Y:
    case GL_TEXTURE_CUBE_MAP_POSITIVE_Z:
    case GL_TEXTURE_CUBE_MAP_NEGATIVE_Z: {

        unsigned int width = s.width, height = s.height;

        unsigned int level;
        for (level = 0; level < s.levels; level++) {
            if (f.encoding == compressed) {

                // EXT_texture_compression_s3tc: 4x4 blocks pad storage, not
                // the logical image extent. Expanding a 256x2 image to 256x4
                // makes normalized sampling reach undefined padding rows.
                width = MAX(width, 1); height = MAX(height, 1);

                unsigned int block_size;
                if (f.gl_internal_format == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT) {
                    block_size = 8;
                } else {
                    block_size = 16;
                }

                const unsigned int storage_bytes = ((width + 3) / 4) * ((height + 3) / 4) * block_size;
                glCompressedTexImage2D(gl_target, level, f.gl_internal_format,
                                       width, height, 0, storage_bytes, texture_data);
                GLenum error = glGetError();
                if (error != GL_NO_ERROR)
                    Nv2aGlUnknown("compressed texture upload error %#x: format %#x level %u %ux%u (%u bytes)",
                        error, f.gl_internal_format, level, width, height, storage_bytes);

                texture_data += storage_bytes;
            } else {

                width = MAX(width, 1); height = MAX(height, 1);

                unsigned int pitch = width * f.bytes_per_pixel;
                uint8_t *unswizzled = NULL;
                if (f.encoding == swizzled) {
                    unswizzled = (uint8_t*)g_malloc(height * pitch);
                    unswizzle_rect(texture_data, width, height,
                                   unswizzled, pitch, f.bytes_per_pixel);
                }

                uint8_t *converted = convert_texture_data(s.color_format, unswizzled ? unswizzled : texture_data,
                                                          palette_data,
                                                          width, height, 1,
                                                          pitch, 0);

                resulting_format = converted ? converted_format : s.color_format;
                ColorFormatInfo cf = kelvin_color_format_map[resulting_format];
                glTexImage2D(gl_target, level, cf.gl_internal_format,
                             width, height, 0,
                             cf.gl_format, cf.gl_type,
                             converted ? converted : unswizzled ? unswizzled : texture_data);

                if (converted) {
                    g_free(converted);
                }
                if (unswizzled) {
                    g_free(unswizzled);
                }

                texture_data += pitch * height;
            }

            width /= 2;
            height /= 2;
        }

        break;
    }
    case GL_TEXTURE_3D: {

        unsigned int width = s.width, height = s.height, depth = s.depth;

        unsigned int level;
        for (level = 0; level < s.levels; level++) {

            unsigned int row_pitch = width * f.bytes_per_pixel;
            unsigned int slice_pitch = row_pitch * height;
            uint8_t *unswizzled = NULL;
            if (f.encoding == swizzled) {
                unswizzled = (uint8_t*)g_malloc(slice_pitch * depth);
                unswizzle_box(texture_data, width, height, depth, unswizzled,
                               row_pitch, slice_pitch, f.bytes_per_pixel);
            }
            uint8_t *converted = convert_texture_data(s.color_format, unswizzled ? unswizzled : texture_data,
                                                      palette_data,
                                                      width, height, depth,
                                                      row_pitch, slice_pitch);

            resulting_format = converted ? converted_format : s.color_format;
            ColorFormatInfo cf = kelvin_color_format_map[resulting_format];
            glTexImage3D(gl_target, level, cf.gl_internal_format,
                         width, height, depth, 0,
                         cf.gl_format, cf.gl_type,
                         converted ? converted : unswizzled ? unswizzled : texture_data);

            if (converted) {
                g_free(converted);
            }
            if (unswizzled) {
                g_free(unswizzled);
            }

            texture_data += width * height * depth * f.bytes_per_pixel;

            width /= 2;
            height /= 2;
            depth /= 2;
        }
        break;
    }
    default:
        assert(false);
        break;
    }
	return resulting_format;
}

static TextureBinding* generate_texture(const TextureShape s,
                                        const uint8_t *texture_data,
                                        const uint8_t *palette_data)
{
	// assert(pg->opengl_enabled);

    ColorFormatInfo f = kelvin_color_format_map[s.color_format];

    /* Create a new opengl texture */
    GLuint gl_texture;
    glGenTextures(1, &gl_texture);

    GLenum gl_target;
    if (s.cubemap) {
        assert(f.encoding != linear);
        assert(s.dimensionality == 2);
        gl_target = GL_TEXTURE_CUBE_MAP;
    } else {
        if (f.encoding == linear) { /* FIXME : Include compressed too? (!= swizzled) */
            // Linear NV2A coordinates are in texels. Normalize them in the
            // generated shader, as xemu 36334d95 psh.c normN(), so ordinary
            // GL_TEXTURE_2D can implement repeat/mirror as well as clamps.
            gl_target = GL_TEXTURE_2D;
            assert(s.dimensionality == 2);
        } else {
            switch(s.dimensionality) {
            case 1: gl_target = GL_TEXTURE_1D; break;
            case 2: gl_target = GL_TEXTURE_2D; break;
            case 3: gl_target = GL_TEXTURE_3D; break;
            default:
                assert(false);
                break;
            }
        }
    }

    Nv2aForeignBindTexture(gl_target, gl_texture);

    NV2A_GL_DLABEL(GL_TEXTURE, gl_texture,
                   "format: 0x%02X%s, %d dimensions%s, width: %d, height: %d, depth: %d",
                   s.color_format, (f.encoding == linear) ? "" : (f.encoding == swizzled) ? " (SZ)" : " (DXT)", // compressed
                   s.dimensionality, s.cubemap ? " (Cubemap)" : "",
                   s.width, s.height, s.depth);

    if (f.encoding == linear) {
        glTexParameteri(gl_target, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(gl_target, GL_TEXTURE_MAX_LEVEL, 0);
    }
    /* Linear textures don't support mipmapping */
    if (f.encoding != linear) {
        glTexParameteri(gl_target, GL_TEXTURE_BASE_LEVEL,
            s.min_mipmap_level);
        glTexParameteri(gl_target, GL_TEXTURE_MAX_LEVEL,
            s.levels - 1);
    }

	/* Set this before calling upload_gl_texture() to prevent potential conversions */
    if (f.gl_swizzle_mask) {
        glTexParameteriv(gl_target, GL_TEXTURE_SWIZZLE_RGBA,
                         f.gl_swizzle_mask);
    }

    if (gl_target == GL_TEXTURE_CUBE_MAP) {

        size_t length = 0;
        unsigned int w = s.width, h = s.height;
        unsigned int level;
        for (level = 0; level < s.levels; level++) {
            /* FIXME: This is wrong for compressed textures and textures with 1x? non-square mipmaps */
            length += w * h * f.bytes_per_pixel;
            w /= 2;
            h /= 2;
        }

        upload_gl_texture(GL_TEXTURE_CUBE_MAP_POSITIVE_X,
                          s, texture_data + 0 * length, palette_data);
        upload_gl_texture(GL_TEXTURE_CUBE_MAP_NEGATIVE_X,
                          s, texture_data + 1 * length, palette_data);
        upload_gl_texture(GL_TEXTURE_CUBE_MAP_POSITIVE_Y,
                          s, texture_data + 2 * length, palette_data);
        upload_gl_texture(GL_TEXTURE_CUBE_MAP_NEGATIVE_Y,
                          s, texture_data + 3 * length, palette_data);
        upload_gl_texture(GL_TEXTURE_CUBE_MAP_POSITIVE_Z,
                          s, texture_data + 4 * length, palette_data);
        upload_gl_texture(GL_TEXTURE_CUBE_MAP_NEGATIVE_Z,
                          s, texture_data + 5 * length, palette_data);
    } else {
        upload_gl_texture(gl_target, s, texture_data, palette_data);
    }

    ++g_nv2a_texture_uploads;
    TextureBinding* ret = (TextureBinding *)g_malloc(sizeof(TextureBinding));
    ret->param_valid = false;
    ret->gl_target = gl_target;
    ret->gl_texture = gl_texture;
    ret->refcnt = 1;
    ret->scale = 1;
    return ret;
}

// NOTE: Might want to change guint to guint64 for return.
/* functions for texture LRU cache */
static guint texture_key_hash(gconstpointer key)
{
    const TextureKey *k = (const TextureKey *)key;
    uint64_t state_hash = fnv_hash(
        (const uint8_t*)&k->state, sizeof(TextureShape));
    return guint(state_hash ^ k->data_hash);
}
static gboolean texture_key_equal(gconstpointer a, gconstpointer b)
{
    const TextureKey *ak = (const TextureKey *)a, *bk = (const TextureKey *)b;
    return memcmp(&ak->state, &bk->state, sizeof(TextureShape)) == 0
            && ak->data_hash == bk->data_hash;
}
static gpointer texture_key_retrieve(gpointer key, gpointer user_data, GError **error)
{
    const TextureKey *k = (const TextureKey *)key;
    TextureBinding *v = generate_texture(k->state,
                                         k->texture_data,
                                         k->palette_data);
    if (error != NULL) {
        *error = NULL;
    }
    return v;
}
static void texture_key_destroy(gpointer data)
{
    g_free(data);
}
static void texture_binding_destroy(gpointer data)
{
    TextureBinding *binding = (TextureBinding *)data;

	// assert(pg->opengl_enabled);

    assert(binding->refcnt > 0);
    binding->refcnt--;
    if (binding->refcnt == 0) {
        Nv2aForeignDeleteTextures(1, &binding->gl_texture);
        g_free(binding);
    }
}

// NOTE: Might want to change guint to guint64 for return.
/* hash and equality for shader cache hash table */
static guint shader_hash(gconstpointer key)
{
    static_assert(sizeof(ShaderState) % 4 == 0, "ShaderState word hash");
    const uint32_t* w = static_cast<const uint32_t*>(key);
    if (Nv2aLegacyDraws()) {
        uint64_t h = 0xcbf29ce484222325ULL;
        for (size_t i = 0; i < sizeof(ShaderState) / 4; ++i) { h ^= w[i]; h *= 0x100000001b3ULL; }
        return guint(h ^ (h >> 32));
    }
    // A bucket selector, not an equality test: retain the full byte comparison
    // below, including unused program storage. Skip the usually zero unused
    // token tail and use native 32-bit multiplies in this 32-bit process.
    const ShaderState* state = static_cast<const ShaderState*>(key);
    const size_t begin = offsetof(ShaderState, program_data) / 4;
    const size_t tail = offsetof(ShaderState, program_length) / 4;
    const size_t used = state->program_length >= 0 && state->program_length <= NV2A_MAX_TRANSFORM_PROGRAM_LENGTH
        ? size_t(state->program_length) * VSH_TOKEN_SIZE : tail - begin;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < begin + used; ++i) { h ^= w[i]; h *= 16777619u; }
    for (size_t i = tail; i < sizeof(ShaderState) / 4; ++i) { h ^= w[i]; h *= 16777619u; }
    return h;
}
static gboolean shader_equal(gconstpointer a, gconstpointer b)
{
    const ShaderState *as = (const ShaderState *)a, *bs = (const ShaderState *)b;
    return Nv2aShaderStateEqual(as, bs);
}

static unsigned int kelvin_map_stencil_op(uint32_t parameter)
{
	unsigned int op;
	switch (parameter) {
	case NV097_SET_STENCIL_OP_V_KEEP:
		op = NV_PGRAPH_CONTROL_2_STENCIL_OP_V_KEEP; break;
	case NV097_SET_STENCIL_OP_V_ZERO:
		op = NV_PGRAPH_CONTROL_2_STENCIL_OP_V_ZERO; break;
	case NV097_SET_STENCIL_OP_V_REPLACE:
		op = NV_PGRAPH_CONTROL_2_STENCIL_OP_V_REPLACE; break;
	case NV097_SET_STENCIL_OP_V_INCRSAT:
		op = NV_PGRAPH_CONTROL_2_STENCIL_OP_V_INCRSAT; break;
	case NV097_SET_STENCIL_OP_V_DECRSAT:
		op = NV_PGRAPH_CONTROL_2_STENCIL_OP_V_DECRSAT; break;
	case NV097_SET_STENCIL_OP_V_INVERT:
		op = NV_PGRAPH_CONTROL_2_STENCIL_OP_V_INVERT; break;
	case NV097_SET_STENCIL_OP_V_INCR:
		op = NV_PGRAPH_CONTROL_2_STENCIL_OP_V_INCR; break;
	case NV097_SET_STENCIL_OP_V_DECR:
		op = NV_PGRAPH_CONTROL_2_STENCIL_OP_V_DECR; break;
	default:
		assert(false);
		break;
	}
	return op;
}

static unsigned int kelvin_map_polygon_mode(uint32_t parameter)
{
	unsigned int mode;
	switch (parameter) {
	case NV097_SET_FRONT_POLYGON_MODE_V_POINT:
		mode = NV_PGRAPH_SETUPRASTER_FRONTFACEMODE_POINT; break;
	case NV097_SET_FRONT_POLYGON_MODE_V_LINE:
		mode = NV_PGRAPH_SETUPRASTER_FRONTFACEMODE_LINE; break;
	case NV097_SET_FRONT_POLYGON_MODE_V_FILL:
		mode = NV_PGRAPH_SETUPRASTER_FRONTFACEMODE_FILL; break;
	default:
		assert(false);
		break;
	}
	return mode;
}

static unsigned int kelvin_map_texgen(uint32_t parameter, unsigned int channel)
{
	assert(channel < 4);
	unsigned int texgen;
	switch (parameter) {
	case NV097_SET_TEXGEN_S_DISABLE:
		texgen = NV_PGRAPH_CSV1_A_T0_S_DISABLE; break;
	case NV097_SET_TEXGEN_S_EYE_LINEAR:
		texgen = NV_PGRAPH_CSV1_A_T0_S_EYE_LINEAR; break;
	case NV097_SET_TEXGEN_S_OBJECT_LINEAR:
		texgen = NV_PGRAPH_CSV1_A_T0_S_OBJECT_LINEAR; break;
	case NV097_SET_TEXGEN_S_SPHERE_MAP:
		assert(channel < 2);
		texgen = NV_PGRAPH_CSV1_A_T0_S_SPHERE_MAP; break;
	case NV097_SET_TEXGEN_S_REFLECTION_MAP:
		assert(channel < 3);
		texgen = NV_PGRAPH_CSV1_A_T0_S_REFLECTION_MAP; break;
	case NV097_SET_TEXGEN_S_NORMAL_MAP:
		assert(channel < 3);
		texgen = NV_PGRAPH_CSV1_A_T0_S_NORMAL_MAP; break;
	default:
		assert(false);
		break;
	}
	return texgen;
}

static uint64_t fnv_hash(const uint8_t *data, size_t len)
{
    /* 64 bit Fowler/Noll/Vo FNV-1a hash code */
    uint64_t hval = 0xcbf29ce484222325ULL;
    const uint8_t *dp = data;
    const uint8_t *de = data + len;
    while (dp < de) {
        hval ^= (uint64_t) *dp++;
        hval += (hval << 1) + (hval << 4) + (hval << 5) +
            (hval << 7) + (hval << 8) + (hval << 40);
    }

    return hval;
}

static uint64_t fast_hash(const uint8_t *data, size_t len, unsigned int samples)
{
#ifdef __SSE4_2__
    uint64_t h[4] = {len, 0, 0, 0};
    assert(samples > 0);

    if (len < 8 || len % 8) {
        return fnv_hash(data, len);
    }

    assert(len >= 8 && len % 8 == 0);
    const uint64_t *dp = (const uint64_t*)data;
    const uint64_t *de = dp + (len / 8);
    size_t step = len / 8 / samples;
    if (step == 0) step = 1;

    while (dp < de - step * 3) {
        h[0] = __builtin_ia32_crc32di(h[0], dp[step * 0]);
        h[1] = __builtin_ia32_crc32di(h[1], dp[step * 1]);
        h[2] = __builtin_ia32_crc32di(h[2], dp[step * 2]);
        h[3] = __builtin_ia32_crc32di(h[3], dp[step * 3]);
        dp += step * 4;
    }
    if (dp < de - step * 0)
        h[0] = __builtin_ia32_crc32di(h[0], dp[step * 0]);
    if (dp < de - step * 1)
        h[1] = __builtin_ia32_crc32di(h[1], dp[step * 1]);
    if (dp < de - step * 2)
        h[2] = __builtin_ia32_crc32di(h[2], dp[step * 2]);

    return h[0] + (h[1] << 10) + (h[2] << 21) + (h[3] << 32);
#else
    return fnv_hash(data, len);
#endif
}
