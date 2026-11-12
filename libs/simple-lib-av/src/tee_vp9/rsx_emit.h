/* TEE VP9 building block - made by tools/make_baustein.sh from ps3/source/rsx_emit.h; do not edit here: change the
   project's file and run the script again. */
/* rsx_emit.h : the RSX command words the prediction code (rsx_mc.c) writes, as functions of our own.

   Why: the building block's Sony SDK build has no librsx, and libgcm's own functions are not known to write the same
   words. With these, both builds write what was tested on the console. Every function is PSL1GHT's librsx function of
   that name cut down to what rsx_mc.c uses (ppu/librsx/commands_impl.h, vertex_program.c, rsx_internal.c; the register
   numbers from its nv40.h). PSL1GHT is Copyright (c) 2011 PSL1GHT Development Team, MIT licence: its notice has to go
   along with a program that ships this file.

   Checked against librsx itself: a PSL1GHT build with -DRSX_EMIT_CHECK lets librsx write every call for real, writes
   the same call with the function here into a scratch buffer and compares the words (rsx_emit_check.h).

   The context is the SDK's command buffer context - PSL1GHT's gcmContextData, Sony's CellGcmContextData: both have
   `current` (the next word) and `end`. Define RE_CTX to that type before including this file. No function asks the SDK
   for room: rsx_mc.c makes room before a row and before a draw (gfx_cb_room / gfx_cb_restart). A call that would write
   past `end` writes nothing and counts in re_overflow - that has to stay 0.

   Constants: the RE_ names are the hardware's values (libgcm's GCM_ / CELL_GCM_ names of the same meaning). */
#ifndef RSX_EMIT_H
#define RSX_EMIT_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef RE_CTX
#error "define RE_CTX: the SDK's command buffer context type (gcmContextData, CellGcmContextData)"
#endif

/* ---- values ------------------------------------------------------------------------------------------------- */

#define RE_FALSE                0
#define RE_TRUE                 1
#define RE_LOCATION_RSX         0           /* the RSX's own memory */
#define RE_LOCATION_CELL        1           /* main memory mapped for the RSX */

#define RE_TYPE_TRIANGLES       5
#define RE_TYPE_QUADS           8
#define RE_INDEX_32B            0
#define RE_INDEX_16B            1
#define RE_VERTEX_F32           2           /* a vertex attribute's data type: floats */
#define RE_ATTRIB_POS           0
#define RE_ATTRIB_TEX0          8
#define RE_ATTRIB_TEX1          9
#define RE_ATTRIB_TEX2          10

#define RE_SURFACE_LINEAR       1
#define RE_SURFACE_CENTER_1     0
#define RE_SURFACE_X8R8G8B8     5
#define RE_SURFACE_B8           9
#define RE_SURFACE_Z24S8        2
#define RE_SURFACE_TARGET_0     1
#define RE_MRT                  4

#define RE_TEX_B8               0x81        /* (libgcm's value; librsx calls it 1 and sets the top bit when it loads) */
#define RE_TEX_LIN              0x20
#define RE_TEX_UNRM             0x40
#define RE_TEX_2D               2
#define RE_TEX_NEAREST          1
#define RE_TEX_LINEAR           2
#define RE_TEX_QUINCUNX         1
#define RE_TEX_CLAMP_TO_EDGE    3
#define RE_TEX_ZFUNC_LESS       1
#define RE_TEX_ANISO_1          0
#define RE_TEX_REMAP_B_TO_ALL   0x0000AAFFu /* order XYXY, every channel from B, type "remap" for all four */
#define RE_INVALIDATE_TEXTURE   1

#define RE_COLOR_MASK_B         0x00000001u
#define RE_COLOR_MASK_G         0x00000100u
#define RE_COLOR_MASK_R         0x00010000u
#define RE_COLOR_MASK_A         0x01000000u

/* a program constant's type (cgcomp's) */
#define RE_PARAM_FLOAT          5
#define RE_PARAM_FLOAT1         6
#define RE_PARAM_FLOAT2         7
#define RE_PARAM_FLOAT3         8
#define RE_PARAM_FLOAT4         9
#define RE_PARAM_FLOAT3x4       10
#define RE_PARAM_FLOAT4x4       11
#define RE_PARAM_FLOAT3x3       12
#define RE_PARAM_FLOAT4x3       13

/* the registers (methods) written */
enum {
	RE_R_SEMA_OFFSET_406E   = 0x0064,       /* the wait label's index ... */
	RE_R_SEMA_ACQUIRE_406E  = 0x0068,       /* ... and the value waited for */
	RE_R_DMA_COLOR1         = 0x018c,
	RE_R_DMA_COLOR0         = 0x0194,
	RE_R_DMA_ZETA           = 0x0198,
	RE_R_DMA_COLOR2         = 0x01b4,
	RE_R_RT_HORIZ           = 0x0200,
	RE_R_RT_FORMAT          = 0x0208,
	RE_R_RT_ENABLE          = 0x0220,
	RE_R_ZETA_PITCH         = 0x022c,
	RE_R_COLOR2_PITCH       = 0x0280,
	RE_R_WINDOW_OFFSET      = 0x02b8,
	RE_R_VIEWPORT_CLIP      = 0x02c0,       /* + 8 * n */
	RE_R_DITHER_ENABLE      = 0x0300,
	RE_R_ALPHA_TEST_ENABLE  = 0x0304,
	RE_R_BLEND_ENABLE       = 0x0310,
	RE_R_COLOR_MASK         = 0x0324,
	RE_R_STENCIL_ENABLE     = 0x0328,
	RE_R_MRT_COLOR_MASK     = 0x0370,
	RE_R_LOGIC_OP_ENABLE    = 0x0374,
	RE_R_DEPTH_RANGE        = 0x0394,
	RE_R_SCISSOR_HORIZ      = 0x08c0,
	RE_R_FP_ADDRESS         = 0x08e4,
	RE_R_VIEWPORT_HORIZ     = 0x0a00,
	RE_R_VIEWPORT_OFFSET    = 0x0a20,
	RE_R_DEPTH_WRITE_ENABLE = 0x0a70,
	RE_R_DEPTH_TEST_ENABLE  = 0x0a74,
	RE_R_TEX_COORD_CONTROL  = 0x0b40,       /* + 4 * n */
	RE_R_VP_UPLOAD_INST     = 0x0b80,
	RE_R_VTXBUF_ADDRESS     = 0x1680,       /* + 4 * attribute */
	RE_R_VTX_CACHE_INV2     = 0x1710,
	RE_R_VTX_CACHE_INV      = 0x1714,
	RE_R_VTXFMT             = 0x1740,       /* + 4 * attribute */
	RE_R_BEGIN_END          = 0x1808,
	RE_R_VB_VERTEX_BATCH    = 0x1814,
	RE_R_VB_INDEX_OFFSET    = 0x181c,
	RE_R_VB_INDEX_DRAW      = 0x1824,
	RE_R_CULL_FACE_ENABLE   = 0x183c,
	RE_R_TEX_SIZE1          = 0x1840,       /* + 4 * unit */
	RE_R_TEX_OFFSET         = 0x1a00,       /* + 32 * unit, as the five below */
	RE_R_TEX_WRAP           = 0x1a08,
	RE_R_TEX_ENABLE         = 0x1a0c,
	RE_R_TEX_SWIZZLE        = 0x1a10,
	RE_R_TEX_FILTER         = 0x1a14,
	RE_R_TEX_SIZE0          = 0x1a18,
	RE_R_FP_CONTROL         = 0x1d60,
	RE_R_SEMA_OFFSET        = 0x1d6c,       /* the back-end label's index ... */
	RE_R_SEMA_BACKEND_WRITE = 0x1d70,       /* ... and the value written when the back end gets there */
	RE_R_DEPTH_CONTROL      = 0x1d78,
	RE_R_SHADER_WINDOW      = 0x1d88,
	RE_R_VP_UPLOAD_FROM_ID  = 0x1e9c,
	RE_R_TRANSFORM_TIMEOUT  = 0x1ef8,
	RE_R_VP_UPLOAD_CONST_ID = 0x1efc,
	RE_R_TEX_CACHE_CTL      = 0x1fd8,
	RE_R_VP_ATTRIB_EN       = 0x1ff0,
	RE_R_VP_RESULT_EN       = 0x1ff4
};

#define RE_DMA_FRAME_BUFFER     0xFEED0000u
#define RE_MAX_COUNT            0x7ffu      /* the most words one method header takes */

/* a method header: n words for the registers from reg on; _NI: all n for reg itself */
#define RE_M(reg, n)            (((uint32_t)(n) << 18) | (uint32_t)(reg))
#define RE_M_NI(reg, n)         (RE_M(reg, n) | 0x40000000u)

/* ---- structures --------------------------------------------------------------------------------------------- */

/* a render target (librsx's gcmSurface, the fields that reach the RSX) */
typedef struct {
	uint8_t type, antialias, color_format, color_target;
	uint8_t color_location[RE_MRT];
	uint32_t color_offset[RE_MRT], color_pitch[RE_MRT];
	uint8_t depth_format, depth_location;
	uint32_t depth_offset, depth_pitch;
	uint16_t width, height, x, y;
} re_surface;

/* a texture (librsx's gcmTexture) */
typedef struct {
	uint8_t format, mipmap, dimension, cubemap;
	uint32_t remap;
	uint16_t width, height, depth;
	uint8_t location;
	uint32_t pitch, offset;
} re_texture;

/* the programs as cgcomp writes them (.vpo / .fpo, PSL1GHT's rsxVertexProgram, rsxFragmentProgram, rsxProgramConst):
   a header, then tables and the code at the offsets it gives, all big-endian */
typedef struct {
	uint16_t magic, pad0;
	uint16_t num_regs, num_attr, num_const, num_insn;
	uint32_t attr_off, const_off, ucode_off;
	uint32_t input_mask, output_mask;
	uint16_t const_start, insn_start;
} re_vp;

typedef struct {
	uint16_t magic, pad0;
	uint16_t num_regs, num_attr, num_const, num_insn;
	uint32_t attr_off, const_off, ucode_off;
	uint32_t fp_control;
	uint16_t texcoords, texcoord2d, texcoord3d, pad1;
} re_fp;

typedef struct {
	uint32_t name_off, index;
	uint8_t type, is_internal, count, pad0;
	union {
		uint32_t u;
		float f;
	} values[4];
} re_const;

typedef char re_assert_vp[sizeof(re_vp) == 36 ? 1 : -1];
typedef char re_assert_fp[sizeof(re_fp) == 36 ? 1 : -1];
typedef char re_assert_const[sizeof(re_const) == 28 ? 1 : -1];

/* ---- the programs' parts ------------------------------------------------------------------------------------ */

static inline const void *re_vp_ucode(const re_vp *vp, uint32_t *bytes)
{
	*bytes = (uint32_t)vp->num_insn * 16u;
	return (const uint8_t *)vp + vp->ucode_off;
}

static inline const void *re_fp_ucode(const re_fp *fp, uint32_t *bytes)
{
	*bytes = (uint32_t)fp->num_insn * 16u;
	return (const uint8_t *)fp + fp->ucode_off;
}

static inline const re_const *re_vp_consts(const re_vp *vp)
{
	return (const re_const *)(const void *)((const uint8_t *)vp + vp->const_off);
}

/* a vertex program's constant by its name (letters compared without case, as librsx does); NULL: none */
static inline const re_const *re_vp_const(const re_vp *vp, const char *name)
{
	const re_const *k = re_vp_consts(vp);
	uint32_t i;

	for (i = 0; i < vp->num_const; i++) {
		const char *a = name, *b;

		if (!k[i].name_off)
			continue;
		b = (const char *)vp + k[i].name_off;
		for (;; a++, b++) {
			const int ca = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a, cb = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;

			if (ca != cb)
				break;
			if (!ca)
				return &k[i];
		}
	}
	return NULL;
}

/* ---- commands ----------------------------------------------------------------------------------------------- */

static unsigned re_overflow __attribute__((unused));

/* the place for n words, taken; NULL when they do not fit before the buffer's end */
static inline uint32_t *re_begin(RE_CTX *c, uint32_t n)
{
	uint32_t *const p = (uint32_t *)c->current;

	if (__builtin_expect(p + n > (uint32_t *)c->end, 0)) {
		re_overflow++;
		return NULL;
	}
	c->current += n;
	return p;
}

/* one register, one value */
static inline void re_reg1(RE_CTX *c, uint32_t reg, uint32_t value)
{
	uint32_t *const p = re_begin(c, 2);

	if (!p)
		return;
	p[0] = RE_M(reg, 1);
	p[1] = value;
}

/* the label `index` gets `value` when the RSX's back end is through with everything before this */
static inline void re_set_write_backend_label(RE_CTX *c, uint8_t index, uint32_t value)
{
	uint32_t *const p = re_begin(c, 4);

	if (!p)
		return;
	p[0] = RE_M(RE_R_SEMA_OFFSET, 1);
	p[1] = 0x10u * index;
	p[2] = RE_M(RE_R_SEMA_BACKEND_WRITE, 1);
	p[3] = (value & 0xff00ff00u) | ((value >> 16) & 0xffu) | ((value & 0xffu) << 16);
}

/* the RSX's front end waits here until the label `index` holds `value` */
static inline void re_set_wait_label(RE_CTX *c, uint8_t index, uint32_t value)
{
	uint32_t *const p = re_begin(c, 4);

	if (!p)
		return;
	p[0] = RE_M(RE_R_SEMA_OFFSET_406E, 1);
	p[1] = 0x10u * index;
	p[2] = RE_M(RE_R_SEMA_ACQUIRE_406E, 1);
	p[3] = value;
}

static inline void re_set_surface(RE_CTX *c, const re_surface *s)
{
	const uint32_t log2w = 31u - (uint32_t)__builtin_clz(s->width), log2h = 31u - (uint32_t)__builtin_clz(s->height);
	uint32_t *const p = re_begin(c, 32);

	if (!p)
		return;
	p[0] = RE_M(RE_R_DMA_COLOR0, 1);
	p[1] = RE_DMA_FRAME_BUFFER + s->color_location[0];
	p[2] = RE_M(RE_R_DMA_COLOR1, 1);
	p[3] = RE_DMA_FRAME_BUFFER + s->color_location[1];
	p[4] = RE_M(RE_R_DMA_COLOR2, 2);
	p[5] = RE_DMA_FRAME_BUFFER + s->color_location[2];
	p[6] = RE_DMA_FRAME_BUFFER + s->color_location[3];
	p[7] = RE_M(RE_R_DMA_ZETA, 1);
	p[8] = RE_DMA_FRAME_BUFFER + s->depth_location;
	p[9] = RE_M(RE_R_RT_FORMAT, 6);
	p[10] = (log2h << 24) | (log2w << 16) | ((uint32_t)s->antialias << 12) | ((uint32_t)s->type << 8) |
	        ((uint32_t)s->depth_format << 5) | s->color_format;
	p[11] = s->color_pitch[0];
	p[12] = s->color_offset[0];
	p[13] = s->depth_offset;
	p[14] = s->color_offset[1];
	p[15] = s->color_pitch[1];
	p[16] = RE_M(RE_R_ZETA_PITCH, 1);
	p[17] = s->depth_pitch;
	p[18] = RE_M(RE_R_COLOR2_PITCH, 4);
	p[19] = s->color_pitch[2];
	p[20] = s->color_pitch[3];
	p[21] = s->color_offset[2];
	p[22] = s->color_offset[3];
	p[23] = RE_M(RE_R_RT_ENABLE, 1);
	p[24] = s->color_target;
	p[25] = RE_M(RE_R_WINDOW_OFFSET, 1);
	p[26] = ((uint32_t)s->y << 16) | s->x;
	p[27] = RE_M(RE_R_RT_HORIZ, 2);
	p[28] = ((uint32_t)s->width << 16) | s->x;
	p[29] = ((uint32_t)s->height << 16) | s->y;
	p[30] = RE_M(RE_R_SHADER_WINDOW, 1);
	p[31] = (1u << 12) | s->height;
}

/* (librsx writes offset and scale twice) */
static inline void re_set_viewport(RE_CTX *c, uint16_t x, uint16_t y, uint16_t width, uint16_t height, float min, float max,
                                   const float scale[4], const float offset[4])
{
	uint32_t *const p = re_begin(c, 24);

	if (!p)
		return;
	p[0] = RE_M(RE_R_VIEWPORT_HORIZ, 2);
	p[1] = ((uint32_t)width << 16) | x;
	p[2] = ((uint32_t)height << 16) | y;
	p[3] = RE_M(RE_R_DEPTH_RANGE, 2);
	memcpy(&p[4], &min, 4);
	memcpy(&p[5], &max, 4);
	p[6] = RE_M(RE_R_VIEWPORT_OFFSET, 8);
	memcpy(&p[7], offset, 16);
	memcpy(&p[11], scale, 16);
	p[15] = RE_M(RE_R_VIEWPORT_OFFSET, 8);
	memcpy(&p[16], offset, 16);
	memcpy(&p[20], scale, 16);
}

static inline void re_set_scissor(RE_CTX *c, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
	uint32_t *const p = re_begin(c, 3);

	if (!p)
		return;
	p[0] = RE_M(RE_R_SCISSOR_HORIZ, 2);
	p[1] = ((uint32_t)w << 16) | x;
	p[2] = ((uint32_t)h << 16) | y;
}

static inline void re_set_viewport_clip(RE_CTX *c, uint8_t sel, uint16_t width, uint16_t height)
{
	uint32_t *const p = re_begin(c, 3);

	if (!p)
		return;
	p[0] = RE_M(RE_R_VIEWPORT_CLIP + 8u * sel, 2);
	p[1] = (uint32_t)(width - 1) << 16;
	p[2] = (uint32_t)(height - 1) << 16;
}

static inline void re_set_depth_test_enable(RE_CTX *c, uint32_t on)   { re_reg1(c, RE_R_DEPTH_TEST_ENABLE, on); }
static inline void re_set_depth_write_enable(RE_CTX *c, uint32_t on)  { re_reg1(c, RE_R_DEPTH_WRITE_ENABLE, on); }
static inline void re_set_stencil_test_enable(RE_CTX *c, uint32_t on) { re_reg1(c, RE_R_STENCIL_ENABLE, on); }
static inline void re_set_alpha_test_enable(RE_CTX *c, uint32_t on)   { re_reg1(c, RE_R_ALPHA_TEST_ENABLE, on); }
static inline void re_set_blend_enable(RE_CTX *c, uint32_t on)        { re_reg1(c, RE_R_BLEND_ENABLE, on); }
static inline void re_set_logic_op_enable(RE_CTX *c, uint32_t on)     { re_reg1(c, RE_R_LOGIC_OP_ENABLE, on); }
static inline void re_set_cull_face_enable(RE_CTX *c, uint32_t on)    { re_reg1(c, RE_R_CULL_FACE_ENABLE, on); }
static inline void re_set_dither_enable(RE_CTX *c, uint32_t on)       { re_reg1(c, RE_R_DITHER_ENABLE, on); }
static inline void re_set_color_mask(RE_CTX *c, uint32_t mask)        { re_reg1(c, RE_R_COLOR_MASK, mask); }
static inline void re_set_color_mask_mrt(RE_CTX *c, uint32_t mask)    { re_reg1(c, RE_R_MRT_COLOR_MASK, mask); }

static inline void re_set_zminmax_control(RE_CTX *c, uint8_t cull_near_far, uint8_t z_clamp, uint8_t cull_ignore_w)
{
	re_reg1(c, RE_R_DEPTH_CONTROL, (uint32_t)cull_near_far | ((uint32_t)z_clamp << 4) | ((uint32_t)cull_ignore_w << 8));
}

/* the vertex program's code, its input and output masks */
static inline void re_load_vertex_program_block(RE_CTX *c, const re_vp *vp, const void *ucode)
{
	const uint8_t *data = ucode;
	const uint32_t loop = vp->num_insn / 8u, rest = (vp->num_insn % 8u) * 4u;
	uint32_t *p = re_begin(c, 9 + loop * 33 + (rest ? rest + 1 : 0)), i;

	if (!p)
		return;
	*p++ = RE_M(RE_R_VP_UPLOAD_FROM_ID, 2);
	*p++ = vp->insn_start;
	*p++ = vp->insn_start;
	for (i = 0; i < loop; i++) {
		*p++ = RE_M(RE_R_VP_UPLOAD_INST, 32);
		memcpy(p, data, 32 * 4);
		p += 32;
		data += 32 * 4;
	}
	if (rest) {
		*p++ = RE_M(RE_R_VP_UPLOAD_INST, rest);
		memcpy(p, data, rest * 4);
		p += rest;
	}
	*p++ = RE_M(RE_R_VP_ATTRIB_EN, 1);
	*p++ = vp->input_mask;
	*p++ = RE_M(RE_R_VP_RESULT_EN, 1);
	*p++ = vp->output_mask | (1u << 5);     /* (+ the point size output) */
	*p++ = RE_M(RE_R_TRANSFORM_TIMEOUT, 1);
	*p++ = vp->num_regs <= 32 ? 0x0020FFFFu : 0x0030FFFFu;
}

/* const_cnt constants of four floats each, from the constant base_const on */
static inline void re_load_vp_parameter_block(RE_CTX *c, uint32_t base_const, uint32_t const_cnt, const void *value)
{
	const uint8_t *v = value;
	const uint32_t block_cnt = (const_cnt * 4) >> 5, block_rem = (const_cnt * 4) & 0x1f;
	uint32_t *p = re_begin(c, block_cnt * 34 + (block_rem ? 2 + block_rem : 0)), i;

	if (!p)
		return;
	for (i = 0; i < block_cnt; i++) {
		*p++ = RE_M(RE_R_VP_UPLOAD_CONST_ID, 33);
		*p++ = base_const + i * 8;
		memcpy(p, v, 32 * 4);
		p += 32;
		v += 32 * 4;
	}
	if (block_rem) {
		*p++ = RE_M(RE_R_VP_UPLOAD_CONST_ID, block_rem + 1);
		*p++ = base_const + block_cnt * 8;
		memcpy(p, v, block_rem * 4);
	}
}

/* the vertex program and the constants cgcomp fixed in it */
static inline void re_load_vertex_program(RE_CTX *c, const re_vp *vp, const void *ucode)
{
	const re_const *k = re_vp_consts(vp);
	uint32_t i;

	re_load_vertex_program_block(c, vp, ucode);
	for (i = 0; i < vp->num_const; i++)
		if (k[i].is_internal)
			re_load_vp_parameter_block(c, k[i].index + vp->const_start, 1, k[i].values);
}

/* a constant of the vertex program */
static inline void re_set_vp_parameter(RE_CTX *c, const re_vp *vp, const re_const *param, const float *value)
{
	const uint32_t base = vp->const_start;
	float params[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	uint32_t i;

	switch (param->type) {
	case RE_PARAM_FLOAT3x4:
	case RE_PARAM_FLOAT4x4:
		re_load_vp_parameter_block(c, param->index + base, param->count, value);
		return;
	case RE_PARAM_FLOAT3x3:
	case RE_PARAM_FLOAT4x3:
		for (i = 0; i < param->count; i++, value += 3) {
			params[0] = value[0];
			params[1] = value[1];
			params[2] = value[2];
			re_load_vp_parameter_block(c, param->index + base + i, 1, params);
		}
		return;
	case RE_PARAM_FLOAT4:
		re_load_vp_parameter_block(c, param->index + base, 1, value);
		return;
	case RE_PARAM_FLOAT3:
		params[2] = value[2];
		/* fall through */
	case RE_PARAM_FLOAT2:
		params[1] = value[1];
		/* fall through */
	case RE_PARAM_FLOAT:
	case RE_PARAM_FLOAT1:
		params[0] = value[0];
		break;
	default:
		break;
	}
	re_load_vp_parameter_block(c, param->index + base, 1, params);
}

/* the fragment program whose code lies at `offset` in `location`, its texture coordinates and control word */
static inline void re_load_fragment_program_location(RE_CTX *c, const re_fp *fp, uint32_t offset, uint32_t location)
{
	uint32_t texcoords = fp->texcoords, tc2d = fp->texcoord2d, tc3d = fp->texcoord3d;
	const uint32_t regs = fp->num_regs > 2 ? fp->num_regs : 2;
	uint32_t n = 4, t, i;
	uint32_t *p;

	for (t = texcoords; t; t >>= 1)
		n += (t & 1) * 2;
	p = re_begin(c, n);
	if (!p)
		return;
	*p++ = RE_M(RE_R_FP_ADDRESS, 1);
	*p++ = (location + 1) | (offset & 0x1fffffffu);
	for (i = 0; texcoords; i++) {
		if (texcoords & 1) {
			*p++ = RE_M(RE_R_TEX_COORD_CONTROL + 4 * i, 1);
			*p++ = ((tc3d & 1) << 4) | (tc2d & 1);
		}
		texcoords >>= 1;
		tc2d >>= 1;
		tc3d >>= 1;
	}
	*p++ = RE_M(RE_R_FP_CONTROL, 1);
	*p++ = fp->fp_control | (regs << 24) | (1u << 10);
}

static inline void re_invalidate_vertex_cache(RE_CTX *c)
{
	uint32_t *const p = re_begin(c, 8);

	if (!p)
		return;
	p[0] = RE_M(RE_R_VTX_CACHE_INV2, 1);
	p[1] = 0;
	p[2] = RE_M(RE_R_VTX_CACHE_INV, 1);
	p[3] = 0;
	p[4] = RE_M(RE_R_VTX_CACHE_INV, 1);
	p[5] = 0;
	p[6] = RE_M(RE_R_VTX_CACHE_INV, 1);
	p[7] = 0;
}

static inline void re_invalidate_texture_cache(RE_CTX *c, uint32_t type)
{
	re_reg1(c, RE_R_TEX_CACHE_CTL, type);
}

static inline void re_load_texture(RE_CTX *c, uint8_t unit, const re_texture *t)
{
	uint32_t *const p = re_begin(c, 9);

	if (!p)
		return;
	p[0] = RE_M(RE_R_TEX_OFFSET + 32u * unit, 2);
	p[1] = t->offset;
	p[2] = ((uint32_t)t->location + 1) | ((uint32_t)t->cubemap << 2) | ((uint32_t)t->dimension << 4) |
	       ((uint32_t)t->format << 8) | ((uint32_t)t->mipmap << 16) | (1u << 3) | 0x8000u;   /* (1 << 3: no border) */
	p[3] = RE_M(RE_R_TEX_SWIZZLE + 32u * unit, 1);
	p[4] = t->remap;
	p[5] = RE_M(RE_R_TEX_SIZE0 + 32u * unit, 1);
	p[6] = ((uint32_t)t->width << 16) | t->height;
	p[7] = RE_M(RE_R_TEX_SIZE1 + 4u * unit, 1);
	p[8] = ((uint32_t)t->depth << 20) | t->pitch;
}

static inline void re_texture_control(RE_CTX *c, uint8_t unit, uint32_t enable, uint16_t minlod, uint16_t maxlod, uint8_t maxaniso)
{
	re_reg1(c, RE_R_TEX_ENABLE + 32u * unit,
	        (enable << 31) | ((uint32_t)minlod << 19) | ((uint32_t)maxlod << 7) | ((uint32_t)maxaniso << 4));
}

static inline void re_texture_filter(RE_CTX *c, uint8_t unit, uint16_t bias, uint8_t min, uint8_t mag, uint8_t conv)
{
	re_reg1(c, RE_R_TEX_FILTER + 32u * unit,
	        ((uint32_t)mag << 24) | ((uint32_t)min << 16) | ((uint32_t)conv << 13) | (bias & 0x1fffu));
}

static inline void re_texture_wrap_mode(RE_CTX *c, uint8_t unit, uint8_t wraps, uint8_t wrapt, uint8_t wrapr, uint8_t unsigned_remap,
                                        uint8_t zfunc, uint8_t gamma)
{
	re_reg1(c, RE_R_TEX_WRAP + 32u * unit,
	        (uint32_t)wraps | ((uint32_t)wrapt << 8) | ((uint32_t)wrapr << 16) | ((uint32_t)unsigned_remap << 12) |
	        ((uint32_t)gamma << 20) | ((uint32_t)zfunc << 28));
}

/* a vertex attribute: elems values of dtype, stride bytes apart, from offset in location on */
static inline void re_bind_vertex_array_attrib(RE_CTX *c, uint8_t attr, uint16_t frequency, uint32_t offset, uint8_t stride,
                                               uint8_t elems, uint8_t dtype, uint8_t location)
{
	uint32_t *const p = re_begin(c, 4);

	if (!p)
		return;
	p[0] = RE_M(RE_R_VTXFMT + 4u * attr, 1);
	p[1] = ((uint32_t)frequency << 16) | ((uint32_t)stride << 8) | ((uint32_t)elems << 4) | dtype;
	p[2] = RE_M(RE_R_VTXBUF_ADDRESS + 4u * attr, 1);
	p[3] = ((uint32_t)location << 31) | offset;
}

/* count vertices from start on, in batches of up to 256 */
static inline void re_draw_vertex_array(RE_CTX *c, uint32_t type, uint32_t start, uint32_t count)
{
	uint32_t lcount, loop, rest, i, j;
	uint32_t *p;

	--count;
	lcount = count & 0xff;
	count >>= 8;
	loop = count / RE_MAX_COUNT;
	rest = count % RE_MAX_COUNT;
	p = re_begin(c, 8 + loop * (1 + RE_MAX_COUNT) + (rest ? 1 + rest : 0) + 2);
	if (!p)
		return;
	*p++ = RE_M_NI(RE_R_VTX_CACHE_INV, 3);
	*p++ = 0;
	*p++ = 0;
	*p++ = 0;
	*p++ = RE_M(RE_R_BEGIN_END, 1);
	*p++ = type;
	*p++ = RE_M(RE_R_VB_VERTEX_BATCH, 1);
	*p++ = (lcount << 24) | start;
	start += lcount + 1;
	for (i = 0; i < loop; i++) {
		*p++ = RE_M_NI(RE_R_VB_VERTEX_BATCH, RE_MAX_COUNT);
		for (j = 0; j < RE_MAX_COUNT; j++, start += 0x100)
			*p++ = 0xff000000u | start;
	}
	if (rest) {
		*p++ = RE_M_NI(RE_R_VB_VERTEX_BATCH, rest);
		for (j = 0; j < rest; j++, start += 0x100)
			*p++ = 0xff000000u | start;
	}
	*p++ = RE_M(RE_R_BEGIN_END, 1);
	*p++ = 0;
}

/* count indices of the index buffer at offset in location, in batches of up to 256 */
static inline void re_draw_index_array(RE_CTX *c, uint8_t type, uint32_t offset, uint32_t count, uint8_t data_type, uint8_t location)
{
	const uint32_t mis = (((offset + 127) & ~127u) - offset) >> (data_type == RE_INDEX_32B ? 2 : 1);
	const uint32_t odd = mis && mis < count;
	uint32_t start = 0, left = count - (odd ? mis : 0), full = 0, mcount, i;
	uint32_t *p;

	while (left > 0x7ff00) {
		full++;
		left -= 0x7ff00;
	}
	mcount = (left + 0xff) >> 8;
	p = re_begin(c, 7 + odd * 2 + full * (1 + RE_MAX_COUNT) + 1 + mcount + 2);
	if (!p)
		return;
	*p++ = RE_M(RE_R_VTX_CACHE_INV, 1);
	*p++ = 0;
	*p++ = RE_M(RE_R_VB_INDEX_OFFSET, 2);
	*p++ = offset;
	*p++ = ((uint32_t)data_type << 4) | location;
	*p++ = RE_M(RE_R_BEGIN_END, 1);
	*p++ = type;
	if (odd) {                              /* (up to the next 128-byte boundary of the index buffer first) */
		*p++ = RE_M(RE_R_VB_INDEX_DRAW, 1);
		*p++ = ((mis - 1) << 24) | start;
		start += mis;
		count -= mis;
	}
	while (count > 0x7ff00) {
		*p++ = RE_M_NI(RE_R_VB_INDEX_DRAW, RE_MAX_COUNT);
		for (i = 0; i < RE_MAX_COUNT; i++, start += 0x100)
			*p++ = 0xff000000u | start;
		count -= 0x7ff00;
	}
	*p++ = RE_M_NI(RE_R_VB_INDEX_DRAW, mcount);
	while (count > 0x100) {
		*p++ = 0xff000000u | start;
		start += 0x100;
		count -= 0x100;
	}
	if (count)
		*p++ = ((count - 1) << 24) | start;
	*p++ = RE_M(RE_R_BEGIN_END, 1);
	*p++ = 0;
}

#endif
