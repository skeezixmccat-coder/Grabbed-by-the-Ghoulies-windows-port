#include <atomic>
// Locally authored host rendering scale. Guest dimensions, pitches, shader
// coordinates and RAM layouts stay in Xbox pixels. Only GL raster storage
// and host presentation grow. Restart the process to change scale.
static unsigned g_render_scale = 1;
extern "C" unsigned Nv2aGlRenderScale() { return g_render_scale; }
static void Nv2aResolutionInit() {
    char value[16] = {};
    DWORD n = GetEnvironmentVariableA("GH_RENDER_SCALE", value, sizeof value);
    if (!n) { g_render_scale = 1; return; }
    if (n != 1 || value[0] < '1' || value[0] > '4')
        Nv2aGlUnknown("GH_RENDER_SCALE must be 1, 2, 3 or 4 (restart required)");
    g_render_scale = value[0] - '0';
}

static void Nv2aSurfaceImage(GLint internal, unsigned w, unsigned h, GLenum format, GLenum type,
                             unsigned bpp, const uint8_t* data) {
    if (g_render_scale == 1) { glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, format, type, data); return; }
    // Uploads are rare and must reproduce the guest texels exactly, including
    // packed depth/stencil. Replication avoids filtering depth or alpha.
    // Build each scaled row once, then duplicate it (the per-texel memcpy
    // version cost tens of ms per 640x480 upload at 3x).
    const unsigned s = g_render_scale;
    const size_t row_bytes = size_t(w) * s * bpp;
    std::vector<uint8_t> expanded(row_bytes * h * s);
    for (unsigned y = 0; y < h; ++y) {
        uint8_t* out = expanded.data() + size_t(y) * s * row_bytes;
        const uint8_t* in = data + size_t(y) * w * bpp;
        if (bpp == 4) {
            uint32_t* o = reinterpret_cast<uint32_t*>(out);
            for (unsigned x = 0; x < w; ++x) { uint32_t v; memcpy(&v, in + size_t(x) * 4, 4); for (unsigned k = 0; k < s; ++k) *o++ = v; }
        } else {
            for (unsigned x = 0; x < w; ++x) for (unsigned k = 0; k < s; ++k) memcpy(out + (size_t(x) * s + k) * bpp, in + size_t(x) * bpp, bpp);
        }
        for (unsigned k = 1; k < s; ++k) memcpy(out + k * row_bytes, out, row_bytes);
    }
    glTexImage2D(GL_TEXTURE_2D, 0, internal, w * g_render_scale, h * g_render_scale, 0, format, type, expanded.data());
}

// Resolve into the original format/size before guest visibility. Each GL
// context owns its FBOs (the download worker uses a shared second context).
// NEAREST also supports integer packed depth/stencil without inventing values.
static void Nv2aReadSurface(GLuint tex, unsigned w, unsigned h, GLenum format, GLenum type,
                            unsigned bpp, void* pixels) {
    GLuint source = tex;
    if (g_render_scale > 1) {
        GLint old_tex, read, draw;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_tex);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read); glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
        Nv2aForeignBindTexture(GL_TEXTURE_2D, tex);
        GLint internal = 0; glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &internal);
        bool depth = format == GL_DEPTH_COMPONENT || format == GL_DEPTH_STENCIL;
        GLenum attachment = format == GL_DEPTH_STENCIL ? GL_DEPTH_STENCIL_ATTACHMENT : depth ? GL_DEPTH_ATTACHMENT : GL_COLOR_ATTACHMENT0;
        GLbitfield mask = depth ? GL_DEPTH_BUFFER_BIT : GL_COLOR_BUFFER_BIT;
        if (format == GL_DEPTH_STENCIL) mask |= GL_STENCIL_BUFFER_BIT;
        struct Resolve { unsigned w, h; GLint internal; GLuint texture, fb[2]; };
        static thread_local std::vector<Resolve> cache;
        Resolve* r = nullptr;
        for (auto& c : cache) if (c.w == w && c.h == h && c.internal == internal) r = &c;
        if (!r) {
            if (cache.size() >= 64) Nv2aGlUnknown("more than 64 surface resolve shapes");
            Resolve c{w, h, internal, 0, {0, 0}};
            glGenTextures(1, &c.texture); Nv2aForeignBindTexture(GL_TEXTURE_2D, c.texture);
            glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, format, type, nullptr);
            glGenFramebuffers(2, c.fb);
            cache.push_back(c); r = &cache.back();
        }
        GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST); if (scissor) glDisable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, r->fb[0]);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, attachment, GL_TEXTURE_2D, tex, 0);
        glReadBuffer(depth ? GL_NONE : GL_COLOR_ATTACHMENT0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, r->fb[1]);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, attachment, GL_TEXTURE_2D, r->texture, 0);
        glDrawBuffer(depth ? GL_NONE : GL_COLOR_ATTACHMENT0);
        if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE ||
            glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            Nv2aGlUnknown("incomplete framebuffer resolving scaled surface");
        glBlitFramebuffer(0, 0, w * g_render_scale, h * g_render_scale, 0, 0, w, h, mask, GL_NEAREST);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, attachment, GL_TEXTURE_2D, 0, 0);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, read); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
        if (scissor) glEnable(GL_SCISSOR_TEST);
        Nv2aForeignBindTexture(GL_TEXTURE_2D, old_tex);
        source = r->texture;
    }
    glGetTextureSubImage(source, 0, 0, 0, 0, w, h, 1, format, type, GLsizei(size_t(w) * h * bpp), pixels);
}

// A scan-out may already have been downloaded in synchronous mode. Retain
// its GL texture identity while it exists, and reject it after guest writes.
struct ScaledScanout { GLuint tex; uint32_t addr, w, h, pitch; uint64_t gen; };
static std::vector<ScaledScanout> g_scaled_scanouts;
static void Nv2aRememberScanout(GLuint tex, uint32_t addr, uint32_t w, uint32_t h, uint32_t pitch, uint64_t gen) {
    if (g_render_scale == 1 || w != 640 || h != 480 || pitch != 2560) return;
    for (auto& s : g_scaled_scanouts) if (s.tex == tex) { s = {tex, addr, w, h, pitch, gen}; return; }
    g_scaled_scanouts.push_back({tex, addr, w, h, pitch, gen});
}
static void Nv2aForgetScanout(GLuint tex) {
    for (size_t i = 0; i < g_scaled_scanouts.size();)
        if (g_scaled_scanouts[i].tex == tex) g_scaled_scanouts.erase(g_scaled_scanouts.begin() + i); else ++i;
}
// Returns bottom-up BGRA. False means the scan-out is CPU supplied (e.g. a
// movie/upload), so its actual native pixels must be presented instead.
extern "C" bool Nv2aGlReadScaledScanout(uint32_t addr, uint32_t pitch, uint32_t* pixels) {
    if (g_render_scale == 1) return false;
    Nv2aGlHarvestRange(addr, pitch * 480);
    for (auto& s : g_scaled_scanouts) if (s.addr == addr && s.pitch == pitch &&
        Nv2aGlRangeGeneration(addr, pitch * 480) <= s.gen) {
        GLint align, row, pbo; glGetIntegerv(GL_PACK_ALIGNMENT, &align); glGetIntegerv(GL_PACK_ROW_LENGTH, &row);
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pbo);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0); glPixelStorei(GL_PACK_ALIGNMENT, 1); glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        unsigned w = 640 * g_render_scale, h = 480 * g_render_scale;
        glGetTextureSubImage(s.tex, 0, 0, 0, 0, w, h, 1, GL_BGRA, GL_UNSIGNED_BYTE, w * h * 4, pixels);
        GLenum error = glGetError();
        glPixelStorei(GL_PACK_ALIGNMENT, align); glPixelStorei(GL_PACK_ROW_LENGTH, row); glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
        if (error != GL_NO_ERROR) Nv2aGlUnknown("scaled scan-out read failed (GL %#x)", error);
        return true;
    }
    return false;
}

// ---- Asynchronous high-resolution presentation ----
// At each flip the GPU thread copies the scan-out (rows flipped to top-down)
// into one of two present textures and fences it; a presenter thread with its
// own shared context waits for the fence and reads the pixels back. The GPU
// thread no longer waits for the host GPU to finish the frame nor for an
// 11 MiB read at every flip (~3 ms of each 33 ms frame at 3x). A frame is
// dropped from the display only if the presenter is still busy with two.
extern HDC g_gl_present_dc; extern HGLRC g_gl_present_rc;
typedef void (*Nv2aPresentSink)(const uint32_t* top_down, unsigned w, unsigned h);
struct PresentSlot { GLuint tex = 0; unsigned w = 0, h = 0; GLsync fence = nullptr; std::atomic<bool> busy{false}; };
static PresentSlot g_present_slots[2];
static HANDLE g_present_wake = nullptr;
static std::atomic<int> g_present_queued{-1};
static Nv2aPresentSink g_present_sink = nullptr;
extern "C" unsigned long long g_present_async = 0, g_present_dropped = 0;
static DWORD WINAPI Nv2aPresentWorker(void*) {
    if (!wglMakeCurrent(g_gl_present_dc, g_gl_present_rc)) Nv2aGlUnknown("presenter GL context cannot be made current");
    glPixelStorei(GL_PACK_ALIGNMENT, 4); glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    std::vector<uint32_t> pixels;
    for (;;) {
        WaitForSingleObject(g_present_wake, INFINITE);
        int i = g_present_queued.exchange(-1);
        if (i < 0) continue;
        PresentSlot& s = g_present_slots[i];
        GLenum r = glClientWaitSync(s.fence, GL_SYNC_FLUSH_COMMANDS_BIT, 5000000000ull);
        if (r != GL_ALREADY_SIGNALED && r != GL_CONDITION_SATISFIED) Nv2aGlUnknown("present fence wait failed (%#x)", r);
        glDeleteSync(s.fence); s.fence = nullptr;
        unsigned w = s.w, h = s.h;
        pixels.resize(size_t(w) * h);
        glGetTextureSubImage(s.tex, 0, 0, 0, 0, w, h, 1, GL_BGRA, GL_UNSIGNED_BYTE, (GLsizei)(pixels.size() * 4), pixels.data());
        GLenum e = glGetError();
        if (e != GL_NO_ERROR) Nv2aGlUnknown("present read failed (GL %#x)", e);
        s.busy.store(false, std::memory_order_release);
        g_present_sink(pixels.data(), w, h);
    }
}
extern "C" bool Nv2aGlQueueScaledScanout(uint32_t addr, uint32_t pitch, Nv2aPresentSink sink) {
    if (g_render_scale == 1 || !g_gl_present_rc) return false;
    Nv2aGlHarvestRange(addr, pitch * 480);
    ScaledScanout* m = nullptr;
    for (auto& s : g_scaled_scanouts) if (s.addr == addr && s.pitch == pitch && Nv2aGlRangeGeneration(addr, pitch * 480) <= s.gen) m = &s;
    if (!m) return false;
    if (!g_present_wake) {
        g_present_sink = sink;
        g_present_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!g_present_wake || !CreateThread(nullptr, 0x100000, Nv2aPresentWorker, nullptr, 0, nullptr)) Nv2aGlUnknown("cannot create the presenter thread");
    }
    int i = -1;
    for (int k = 0; k < 2 && i < 0; ++k) if (!g_present_slots[k].busy.load(std::memory_order_acquire)) i = k;
    if (i < 0) {   // both busy: one being read, one still queued -> replace the queued frame
        i = g_present_queued.exchange(-1);
        if (i < 0) { ++g_present_dropped; return true; }
        glDeleteSync(g_present_slots[i].fence); g_present_slots[i].fence = nullptr; ++g_present_dropped;
    }
    PresentSlot& s = g_present_slots[i];
    const unsigned w = 640 * g_render_scale, h = 480 * g_render_scale;
    if (!s.tex || s.w != w || s.h != h) {
        if (s.tex) Nv2aForeignDeleteTextures(1, &s.tex);
        glGenTextures(1, &s.tex); Nv2aForeignBindTexture(GL_TEXTURE_2D, s.tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
        s.w = w; s.h = h;
    }
    static GLuint fbo[2] = {};
    if (!fbo[0]) glGenFramebuffers(2, fbo);
    GLint read = 0, draw = 0; glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read); glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST); if (scissor) glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo[0]); glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m->tex, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo[1]); glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s.tex, 0);
    glBlitFramebuffer(0, 0, w, h, 0, h, w, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, read); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    s.fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0); glFlush();
    s.busy.store(true, std::memory_order_release);
    int old = g_present_queued.exchange(i);
    if (old >= 0) {   // the presenter had not taken it yet
        glDeleteSync(g_present_slots[old].fence); g_present_slots[old].fence = nullptr;
        g_present_slots[old].busy.store(false, std::memory_order_release); ++g_present_dropped;
    }
    ++g_present_async;
    SetEvent(g_present_wake);
    return true;
}
