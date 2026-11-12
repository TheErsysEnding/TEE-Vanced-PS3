/* TEE VP9 building block - made by tools/make_baustein.sh from ps3/source/rsx_plat.h; do not edit here: change the
   project's file and run the script again. */
/* rsx_plat.h : what the RSX part (rsx_mc.c) takes from the SDK's graphics library under one set of names - Sony's
   libgcm (the compiler defines __CELLOS_LV2__) or PSL1GHT's libgcm_sys. Everything else it needs from the system
   comes through tv9_plat.h, and from the application's display code through tv9_rsx_host.h. Its command words are its
   own (rsx_emit.h): no command function of either library is used. */
#ifndef RSX_PLAT_H
#define RSX_PLAT_H

#include <stdint.h>

#ifdef __CELLOS_LV2__                       /* ---- Sony's SDK ---- */

#include <cell/gcm.h>

typedef CellGcmContextData rxp_ctx;         /* the command buffer context: begin, end, current */
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef int64_t s64;
typedef float f32;
typedef volatile uint32_t vu32;

/* a label: a word the RSX writes when its back end gets to a command */
static inline vu32 *rxp_label(u8 index)
{
	return (vu32 *)cellGcmGetLabelAddress(index);
}

/* main memory (1 MB aligned, whole megabytes) made readable and writable for the RSX; its offset there. 0 = ok */
static inline int rxp_map_main(void *ea, u32 bytes, u32 *offset)
{
	return cellGcmMapMainMemory(ea, bytes, offset);
}

static inline int rxp_unmap_main(void *ea)
{
	return cellGcmUnmapEaIoAddress(ea);
}

/* an address in RSX memory or in mapped main memory as the offset the RSX's commands take. 0 = ok */
static inline int rxp_offset(const void *ea, u32 *offset)
{
	return cellGcmAddressToOffset(ea, offset);
}

/* the commands written so far are the RSX's to read (libgcm's cellGcmFlush, written out) */
static inline void rxp_flush(rxp_ctx *c)
{
	CellGcmControl volatile *control = cellGcmGetControlRegister();
	uint32_t offset = 0;

	__asm__ volatile("sync" ::: "memory");
	cellGcmAddressToOffset(c->current, &offset);
	control->put = offset;
}

#else                                       /* ---- PSL1GHT ---- */

#include <ppu-types.h>
#include <rsx/rsx.h>

typedef gcmContextData rxp_ctx;

static inline vu32 *rxp_label(u8 index)
{
	return (vu32 *)gcmGetLabelAddress(index);
}

static inline int rxp_map_main(void *ea, u32 bytes, u32 *offset)
{
	return gcmMapMainMemory(ea, bytes, offset);
}

static inline int rxp_unmap_main(void *ea)
{
	return gcmUnmapEaIoAddress(ea);
}

static inline int rxp_offset(const void *ea, u32 *offset)
{
	return rsxAddressToOffset((void *)(uintptr_t)ea, offset);
}

static inline void rxp_flush(rxp_ctx *c)
{
	rsxFlushBuffer(c);
}

#endif

#endif
