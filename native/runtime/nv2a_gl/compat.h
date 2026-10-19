// Compatibility layer for the ported XQEMU/Cxbx-Reloaded NV2A PGRAPH code
// (GPL-2.0; see docs/SOURCES.md). Replaces QEMU threading, glib, GLEW
// initialisation and offscreen-context helpers with Windows equivalents, and
// turns assertions into explicit runtime stops.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#define GLEW_STATIC
#define GLEW_NO_GLU
#include "glew.h"

namespace xbox { typedef uint32_t addr_xt; }

// Explicit failure instead of silent assert (the runtime reports and stops).
extern "C" __declspec(noreturn) void Nv2aGlFail(const char* what, const char* file, int line);
#undef assert
#define assert(x) do { if (!(x)) Nv2aGlFail(#x, __FUNCTION__, __LINE__); } while (0)

// QEMU threading shims
struct QemuMutex { CRITICAL_SECTION cs; };
struct QemuCond { CONDITION_VARIABLE cv; };
inline void qemu_mutex_init(QemuMutex* m) { InitializeCriticalSection(&m->cs); }
inline void qemu_mutex_destroy(QemuMutex* m) { DeleteCriticalSection(&m->cs); }
inline void qemu_mutex_lock(QemuMutex* m) { EnterCriticalSection(&m->cs); }
inline void qemu_mutex_unlock(QemuMutex* m) { LeaveCriticalSection(&m->cs); }
inline void qemu_cond_init(QemuCond* c) { InitializeConditionVariable(&c->cv); }
inline void qemu_cond_destroy(QemuCond*) {}
inline void qemu_cond_wait(QemuCond* c, QemuMutex* m) { SleepConditionVariableCS(&c->cv, &m->cs, INFINITE); }
inline void qemu_cond_signal(QemuCond* c) { WakeConditionVariable(&c->cv); }
inline void qemu_cond_broadcast(QemuCond* c) { WakeAllConditionVariable(&c->cv); }

// Offscreen GL context (WGL) shims
typedef struct GloContext GloContext;
GloContext* glo_context_create();
void glo_set_current(GloContext* context);
void glo_context_destroy(GloContext* context);
bool glo_check_extension(const char* name);
void glo_readpixels(GLenum gl_format, GLenum gl_type, unsigned int bytes_per_pixel, unsigned int stride,
                    unsigned int width, unsigned int height, void* data);

// glib type names used by the (disabled) texture/shader cache helpers
typedef const void* gconstpointer; typedef void* gpointer; typedef unsigned int guint; typedef int gboolean;
struct GError;
// Native port: minimal GHashTable for the reference's shader cache (enabled:
// without it every state update compiled and leaked a new GL program).
// Keys/values are owned by the caller, as the reference uses them.
#include <unordered_map>
#include <utility>
struct GHashTable {
    guint (*hash)(gconstpointer);
    gboolean (*equal)(gconstpointer, gconstpointer);
    std::unordered_multimap<guint, std::pair<gpointer, gpointer>> map;
};
inline GHashTable* g_hash_table_new(guint (*hash)(gconstpointer), gboolean (*equal)(gconstpointer, gconstpointer)) {
    GHashTable* t = new GHashTable; t->hash = hash; t->equal = equal; return t;
}
inline gpointer g_hash_table_lookup(GHashTable* t, gconstpointer key) {
    auto range = t->map.equal_range(t->hash(key));
    for (auto it = range.first; it != range.second; ++it) if (t->equal(it->second.first, key)) return it->second.second;
    return nullptr;
}
inline void g_hash_table_insert(GHashTable* t, gpointer key, gpointer value) { t->map.emplace(t->hash(key), std::make_pair(key, value)); }
#define USE_SHADER_CACHE 1
#include <intrin.h>
// Native port: lightweight GPU-thread profiling (diagnostics only).
extern "C" unsigned long long g_nv2a_prof[32];
extern "C" bool g_nv2a_profile_enabled;
// TSC ticks (QueryPerformanceCounter around every method cost ~6% of the GPU thread).
struct Nv2aProf { int id; unsigned long long t0; Nv2aProf(int i) : id(i), t0(g_nv2a_profile_enabled ? __rdtsc() : 0) {}
    ~Nv2aProf() { if(t0) g_nv2a_prof[id] += __rdtsc() - t0; } };
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif
inline void stq_le_p(uint64_t* p, uint64_t v) { *p = v; }
inline uint32_t ldl_le_p(const uint32_t* p) { return *p; }
inline void stl_le_p(uint32_t* p, uint32_t v) { *p = v; }
