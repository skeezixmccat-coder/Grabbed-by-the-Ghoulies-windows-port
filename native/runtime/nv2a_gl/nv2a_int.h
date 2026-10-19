// Modified for the Ghoulies native Windows preview, 2026-10-14 through 2026-10-19.
// Port integration, validation and rendering changes: see docs/PORT-PROVENANCE.md.
// Source : https://github.com/xqemu/xqemu/blob/master/hw/xbox/nv2a_int.h
/*
 * QEMU Geforce NV2A internal definitions
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2018 Matt Borgerson
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 or
 * (at your option) version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses/>.
 */

#ifndef HW_NV2A_INT_H
#define HW_NV2A_INT_H

#include "compat.h"
// Native port: the reference undefines USE_SHADER_CACHE here; it is kept enabled
// (compat.h) now that ShaderState is zero-initialised before use.




#ifdef USE_SHADER_CACHE
#endif

#include "swizzle.h"

#include "nv2a_debug.h" // For HWADDR_PRIx, NV2A_DPRINTF, NV2A_GL_DPRINTF, etc.
#include "nv2a_shaders.h" // For ShaderBinding, etc
#include "nv2a_regs.h" // For NV2A_MAX_TEXTURES, etc


typedef xbox::addr_xt hwaddr; // Compatibility; Cxbx uses xbox::addr_xt, xqemu and OpenXbox use hwaddr 
typedef uint32_t value_t; // Compatibility; Cxbx values are uint32_t (xqemu and OpenXbox use uint64_t)

#define NV_PMC_SIZE                 0x001000
#define _NV_PFIFO_SIZE              0x002000 // Underscore prefix to prevent clash with NV_PFIFO_SIZE
#define NV_PVIDEO_SIZE              0x001000
#define NV_PTIMER_SIZE              0x001000
#define NV_PFB_SIZE                 0x001000
#define NV_PGRAPH_SIZE              0x002000
#define NV_PCRTC_SIZE               0x001000
#define NV_PRAMDAC_SIZE             0x001000

#define VSH_TOKEN_SIZE 4 // Compatibility; TODO : Move this to nv2a_vsh.h
#define MAX(a,b) ((a)>(b) ? (a) : (b)) // Compatibility
#define MIN(a,b) ((a)<(b) ? (a) : (b)) // Compatibility

#define g_free(x) free(x) // Compatibility
#define g_malloc(x) malloc(x) // Compatibility
#define g_malloc0(x) calloc(1, x) // Compatibility
#define g_realloc(x, y) realloc(x, y) // Compatibility

#undef USE_TEXTURE_CACHE

#if __cplusplus >= 201402L
#  define NV2A_CONSTEXPR constexpr
#else
#  define NV2A_CONSTEXPR static
#endif

// Preserve ffs(0)==0 and the one-based bit index, including bit 31.
// The old shift loop survived /O2 even for constant register masks.
#include <bit>
static constexpr int ffs(int valu) {
    return valu ? std::countr_zero(static_cast<unsigned int>(valu)) + 1 : 0;
}

#define GET_MASK(v, mask) (((v) & (mask)) >> (ffs(mask)-1))

#define SET_MASK(v, mask, val)                            \
    do {                                                  \
        const unsigned int __val = (val);                 \
        const unsigned int __mask = (mask);               \
        (v) &= ~(__mask);                                 \
        (v) |= ((__val) << (ffs(__mask) - 1)) & (__mask); \
    } while (0)

// Power-of-two CASE statements
#define CASE_1(v, step) case (v)
#define CASE_2(v, step) CASE_1(v, step) : CASE_1(v + (step) * 1, step)
#define CASE_4(v, step) CASE_2(v, step) : CASE_2(v + (step) * 2, step)
#define CASE_8(v, step) CASE_4(v, step) : CASE_4(v + (step) * 4, step)
#define CASE_16(v, step) CASE_8(v, step) : CASE_8(v + (step) * 8, step)
#define CASE_32(v, step) CASE_16(v, step) : CASE_16(v + (step) * 16, step)
#define CASE_64(v, step) CASE_32(v, step) : CASE_32(v + (step) * 32, step)
#define CASE_128(v, step) CASE_64(v, step) : CASE_64(v + (step) * 64, step)
#define CASE_256(v, step) CASE_128(v, step) : CASE_128(v + (step) * 128, step)

// Non-power-of-two CASE statements
#define CASE_3(v, step) CASE_2(v, step) : CASE_1(v + (step) * 2, step)

#define NV2A_DEVICE(obj) \
    OBJECT_CHECK(NV2AState, (obj), "nv2a")

//void reg_log_read(int block, hwaddr addr, uint64_t val);
//void reg_log_write(int block, hwaddr addr, uint64_t val);

enum FIFOEngine {
	ENGINE_SOFTWARE = 0,
	ENGINE_GRAPHICS = 1,
	ENGINE_DVD = 2,
};

typedef struct DMAObject {
	unsigned int dma_class;
	unsigned int dma_target;
	xbox::addr_xt address;
	xbox::addr_xt limit;
} DMAObject;

typedef struct VertexAttribute {
	bool dma_select;
	xbox::addr_xt offset;

	/* inline arrays are packed in order?
	* Need to pass the offset to converted attributes */
	unsigned int inline_array_offset;

	float inline_value[4];

	unsigned int format;
	unsigned int size; /* size of the data type */
	unsigned int count; /* number of components */
	uint32_t stride;

	bool needs_conversion;
	uint8_t *converted_buffer;
	unsigned int converted_elements;
	unsigned int converted_size;
	unsigned int converted_count;

	float *inline_buffer;
    float *inline_buffer_storage; // retained allocation; inline_buffer still denotes this draw's active stream

	GLint gl_count;
	GLenum gl_type;
	GLboolean gl_normalize;

	GLuint gl_converted_buffer;
	GLuint gl_inline_buffer;
} VertexAttribute;

typedef struct Surface {
	bool draw_dirty;
	bool buffer_dirty;
	bool write_enabled_cache;
	unsigned int pitch;

	xbox::addr_xt offset;
	uint64_t memory_gen; // native port: guest-memory generation the GL copy matches
	uint64_t checked_epoch; // native port: present epoch of the last guest-write check
	// native port: identity of the GL texture currently attached (surface cache)
	bool bound_valid; uint32_t bound_addr, bound_w, bound_h, bound_fmt, bound_pitch; bool bound_swizzle;
} Surface;

typedef struct SurfaceShape {
	unsigned int z_format;
	unsigned int color_format;
	unsigned int zeta_format;
	unsigned int log_width, log_height;
	unsigned int clip_x, clip_y;
	unsigned int clip_width, clip_height;
	unsigned int anti_aliasing;
} SurfaceShape;

typedef struct TextureShape {
	bool cubemap;
	unsigned int dimensionality;
	unsigned int color_format;
	unsigned int levels;
	unsigned int width, height, depth;

	unsigned int min_mipmap_level, max_mipmap_level;
	unsigned int pitch;
} TextureShape;

typedef struct TextureKey {
	TextureShape state;
	uint64_t data_hash;
	uint8_t* texture_data;
	uint8_t* palette_data;
} TextureKey;

typedef struct TextureBinding {
	GLenum gl_target;
	GLuint gl_texture;
	unsigned int refcnt;
	// Native port: sampler parameters last applied to this texture object.
	uint32_t param_key[8];
	bool param_valid;
	unsigned scale; // native port: GL size / guest size (render-scaled surface copies)
} TextureBinding;

typedef struct KelvinState {
	xbox::addr_xt object_instance;
} KelvinState;

typedef struct ContextSurfaces2DState {
	xbox::addr_xt object_instance;
	xbox::addr_xt dma_image_source;
	xbox::addr_xt dma_image_dest;
	unsigned int color_format;
	unsigned int source_pitch, dest_pitch;
	xbox::addr_xt source_offset, dest_offset;
} ContextSurfaces2DState;

typedef struct ImageBlitState {
	xbox::addr_xt object_instance;
	xbox::addr_xt context_surfaces;
	// Native port: context objects bound at 0x184..0x198 (colour key, clip,
	// pattern, rop, beta1, beta4), indexed by (method - 0x184) / 4.
	xbox::addr_xt context_objects[6];
	unsigned int operation;
	unsigned int in_x, in_y;
	unsigned int out_x, out_y;
	unsigned int width, height;
} ImageBlitState;

typedef struct PGRAPHState {
	bool opengl_enabled; // == bLLE_GPU
	QemuMutex pgraph_lock;

	uint32_t pending_interrupts;
	uint32_t enabled_interrupts;
	QemuCond interrupt_cond;

	/* subchannels state we're not sure the location of... */
	ContextSurfaces2DState context_surfaces_2d;
	ImageBlitState image_blit;
	KelvinState kelvin;
    // NV0039 setup state; transfer methods remain explicit failures until used.
    uint32_t m2mf_object, m2mf_dma_notify, m2mf_dma_input, m2mf_dma_output;

	QemuCond fifo_access_cond;
	QemuCond flip_3d;

	xbox::addr_xt dma_color, dma_zeta;
	Surface surface_color, surface_zeta;
	unsigned int surface_type;
	SurfaceShape surface_shape;
	SurfaceShape last_surface_shape;

	xbox::addr_xt dma_a, dma_b;
#ifdef USE_TEXTURE_CACHE
	GLruCache *texture_cache;
#endif
	bool texture_dirty[NV2A_MAX_TEXTURES];
	TextureBinding *texture_binding[NV2A_MAX_TEXTURES];

#ifdef USE_SHADER_CACHE
	GHashTable *shader_cache;
#endif
	ShaderBinding *shader_binding;

	bool texture_matrix_enable[NV2A_MAX_TEXTURES];

	/* FIXME: Move to NV_PGRAPH_BUMPMAT... */
	float bump_env_matrix[NV2A_MAX_TEXTURES - 1][4]; /* 3 allowed stages with 2x2 matrix each */

	GloContext *gl_context;
	GLuint gl_framebuffer;
	GLuint gl_color_buffer, gl_zeta_buffer;

	xbox::addr_xt dma_state;
	xbox::addr_xt dma_notifies;
	xbox::addr_xt dma_semaphore;

	xbox::addr_xt dma_report;
	xbox::addr_xt report_offset;
	bool zpass_pixel_count_enable;
	unsigned int zpass_pixel_count_result;
	unsigned int gl_zpass_pixel_count_query_count;
	GLuint* gl_zpass_pixel_count_queries;

	xbox::addr_xt dma_vertex_a, dma_vertex_b;

	unsigned int primitive_mode;
    bool draw_begin_active; // native: validate paired BEGIN/END, including zero vertices

	unsigned int clear_surface;

	bool enable_vertex_program_write;

	uint32_t program_data[NV2A_MAX_TRANSFORM_PROGRAM_LENGTH][VSH_TOKEN_SIZE];

	uint32_t vsh_constants[NV2A_VERTEXSHADER_CONSTANTS][4];
	bool vsh_constants_dirty[NV2A_VERTEXSHADER_CONSTANTS];

	/* lighting constant arrays */
	uint32_t ltctxa[NV2A_LTCTXA_COUNT][4];
	bool ltctxa_dirty[NV2A_LTCTXA_COUNT];
	uint32_t ltctxb[NV2A_LTCTXB_COUNT][4];
	bool ltctxb_dirty[NV2A_LTCTXB_COUNT];
	uint32_t ltc1[NV2A_LTC1_COUNT][4];
	bool ltc1_dirty[NV2A_LTC1_COUNT];

	// should figure out where these are in lighting context
	float light_infinite_half_vector[NV2A_MAX_LIGHTS][3];
	float light_infinite_direction[NV2A_MAX_LIGHTS][3];
	float light_local_position[NV2A_MAX_LIGHTS][3];
	float light_local_attenuation[NV2A_MAX_LIGHTS][3];

	VertexAttribute vertex_attributes[NV2A_VERTEXSHADER_ATTRIBUTES];

	unsigned int inline_array_length;
	uint32_t inline_array[NV2A_MAX_BATCH_LENGTH];
	GLuint gl_inline_array_buffer;

	unsigned int inline_elements_length;
	uint16_t inline_elements[NV2A_MAX_BATCH_LENGTH]; // Cxbx-Reloaded TODO : Restore uint32_t once HLE_draw_inline_elements can using that

	unsigned int inline_buffer_length;

	unsigned int draw_arrays_length;
	unsigned int draw_arrays_max_count;

	/* FIXME: Unknown size, possibly endless, 1000 will do for now */
	GLint gl_draw_arrays_start[1000];
	GLsizei gl_draw_arrays_count[1000];

	GLuint gl_element_buffer;
	GLuint gl_memory_buffer;
    uint64_t vertex_memory_generation[0x4000]; // per-device 4 KiB mirror validity
	GLuint gl_vertex_array;

    // Diagnostic provenance for the validated SWATH_WIDTH register latch.
    // All raster consumption remains gated until its effect is established.
    bool swath_width_written;
	bool zmin_max_control_written;
	float line_width_px;
	bool wide_lines;
	uint32_t point_params_enable;
	uint32_t dxt1_noise[2];
	bool dxt1_noise_written[2];
	uint32_t polygon_stipple_enable;
	uint32_t zmin_max_control_parameter;
    uint32_t swath_width_parameter;
    unsigned int swath_width_subchannel;

    // RDI copies of memory-controller configuration. These are distinct banks;
    // do not alias them to the directly addressed PGRAPH registers.
    uint32_t rdi_memory[0x2c];
    bool rdi_memory_written[0x2c];
    uint32_t rdi_fd[15];
    bool rdi_fd_written[15];
	uint32_t regs[NV_PGRAPH_SIZE]; // TODO : union
} PGRAPHState;

typedef struct NV2AState {
    uint8_t *vram_ptr;
    size_t vram_size;
    struct { uint8_t *ramin_ptr; size_t ramin_size; } pramin;
    PGRAPHState pgraph;
} NV2AState;


typedef value_t(*read_func)(NV2AState *d, hwaddr addr); //, unsigned int size);
typedef void(*write_func)(NV2AState *d, hwaddr addr, value_t val); //, unsigned int size);

typedef struct {
	read_func read;
	write_func write;
} MemoryRegionOps;

#if 0
// Valid after PCI init :
#define NV20_REG_BASE_KERNEL 0xFD000000

typedef volatile DWORD *PPUSH;

typedef struct {
	DWORD Ignored[0x10];
	PPUSH Put; // On Xbox1, this field is only written to by the CPU (the GPU uses this as a trigger to start executing from the given address)
	PPUSH Get; // On Xbox1, this field is only read from by the CPU (the GPU reflects in here where it is/stopped executing)
	PPUSH Reference; // TODO : xbox::addr_xt / void* / DWORD ? 
	DWORD Ignored2[0x7ED];
} Nv2AControlDma;
#endif

#endif
