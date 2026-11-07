/* TEE Decoder building block - made by tools/make_baustein.sh from ps3/source/plat_cell.c; do not edit here: change the
   project's file and run the script again. */
#include "td_names.h"
/* TEE Decoder, the platform (plat.h) on the PS3's PPU with Sony's Cell SDK (an app like TEE Vanced, built with
   ppu-lv2-gcc or SNC) - the same as plat_ps3.c (PSL1GHT) with the SDK's names: SPU threads (one group each), one
   lv2 event queue for all answers plus a local event port that other PPU threads use to wake the decoder
   thread, PPU threads, the timebase. An app compiles this file or plat_ps3.c, never both.
   The SPU programs are the same arrays (td_spu_images.c): an SPU ELF is loaded by the kernel, whichever SDK the
   PPU side was built with.
   State (1.3.0): compiled with the SDK; not run on the console yet - the first app that uses it checks it with
   the test streams (TEE-DECODER-INTEGRATION.md). */
#include "td_plat.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ppu_intrinsics.h>
#include <sys/event.h>
#include <sys/memory.h>
#include <sys/ppu_thread.h>
#include <sys/return_code.h>
#include <sys/spu_image.h>
#include <sys/spu_initialize.h>
#include <sys/spu_thread.h>
#include <sys/spu_thread_group.h>
#include <sys/sys_time.h>
#include <sys/timer.h>

#include "td_cabac_job.h"
#include "td_pixel_job.h"

#define SPU_PRIO     100
#define WAKE_NAME    0x5445454457414B45ull       /* "TEEDWAKE" */

extern const unsigned char tdec_spu_cabac[], tdec_spu_pixel[];   /* td_spu_images.c */

static sys_spu_image_t img_cabac, img_pixel;
static sys_spu_thread_group_t groups[PLAT_MAX_SPES];
static sys_spu_thread_t threads[PLAT_MAX_SPES];
static int nstarted, ncab, ready;
static sys_event_queue_t queue;
static sys_event_port_t wake_port;
static volatile int wake_pending;
static double tb_mhz;
static void (*log_fn)(const char *line);

void plat_set_log(void (*fn)(const char *line))
{
	log_fn = fn;
}

void plat_log_memory(const char *when)
{
	sys_memory_info_t info;

	if (sys_memory_get_user_memory_size(&info) == CELL_OK)
		plat_log("user memory %s: %u MB of %u MB free", when, (unsigned)(info.available_user_memory >> 20),
		         (unsigned)(info.total_user_memory >> 20));
}

int plat_init(void)
{
	sys_event_queue_attribute_t qattr;
	static int spu_init;
	int rc;

	if (ready)
		return 0;
	tb_mhz = (double)sys_time_get_timebase_frequency() / 1e6;
	if (!spu_init) {                    /* an app that set the SPUs up itself keeps its setting (the call fails) */
		rc = sys_spu_initialize(PLAT_MAX_SPES, 0);
		plat_log("sys_spu_initialize(%d, 0) = 0x%08x, timebase %.3f MHz", PLAT_MAX_SPES, (unsigned)rc, tb_mhz);
		spu_init = 1;
	}
	if (sys_spu_image_import(&img_cabac, tdec_spu_cabac, SYS_SPU_IMAGE_PROTECT) != CELL_OK ||
	    sys_spu_image_import(&img_pixel, tdec_spu_pixel, SYS_SPU_IMAGE_PROTECT) != CELL_OK) {
		plat_log("the SPU images did not load");
		return -1;
	}
	sys_event_queue_attribute_initialize(qattr);
	qattr.attr_protocol = SYS_SYNC_FIFO;
	qattr.type = SYS_PPU_QUEUE;
	if (sys_event_queue_create(&queue, &qattr, SYS_EVENT_QUEUE_LOCAL, 64) != CELL_OK) {
		plat_log("the event queue did not come");
		return -1;
	}
	if (sys_event_port_create(&wake_port, SYS_EVENT_PORT_LOCAL, WAKE_NAME) != CELL_OK ||
	    sys_event_port_connect_local(wake_port, queue) != CELL_OK) {
		plat_log("the wake-up port did not connect");
		return -1;
	}
	wake_pending = 0;
	ready = 1;
	return 0;
}

void plat_shutdown(void)
{
	if (!ready)
		return;
	sys_event_port_disconnect(wake_port);
	sys_event_port_destroy(wake_port);
	sys_event_queue_destroy(queue, SYS_EVENT_QUEUE_DESTROY_FORCE);
	sys_spu_image_close(&img_cabac);
	sys_spu_image_close(&img_pixel);
	ready = 0;
}

int plat_spes_start(int ncabac, int npixel)
{
	int k, rc;

	if (!ready || ncabac + npixel > PLAT_MAX_SPES)
		return -1;
	nstarted = 0;
	ncab = ncabac;
	for (k = 0; k < ncabac + npixel; k++) {
		char name[16];
		sys_spu_thread_group_attribute_t gattr;
		sys_spu_thread_attribute_t tattr;
		sys_spu_thread_argument_t arg;
		int cabac = k < ncabac;
		uint8_t port = cabac ? CABAC_EVENT_PORT : PIX_EVENT_PORT;

		snprintf(name, sizeof(name), "teedec%d", k);
		sys_spu_thread_group_attribute_initialize(gattr);
		sys_spu_thread_group_attribute_name(gattr, name);
		rc = sys_spu_thread_group_create(&groups[k], 1, SPU_PRIO, &gattr);
		if (rc != CELL_OK) {
			plat_log("SPU group %d did not come: 0x%08x", k, (unsigned)rc);
			plat_spes_stop(1);
			return -1;
		}
		sys_spu_thread_attribute_initialize(tattr);
		sys_spu_thread_attribute_name(tattr, "teedec");
		memset(&arg, 0, sizeof(arg));
		rc = sys_spu_thread_initialize(&threads[k], groups[k], 0, cabac ? &img_cabac : &img_pixel, &tattr, &arg);
		if (rc == CELL_OK) {
			rc = sys_spu_thread_connect_event(threads[k], queue, SYS_SPU_THREAD_EVENT_USER, port);
			if (rc == CELL_OK) {
				rc = sys_spu_thread_group_start(groups[k]);
				if (rc != CELL_OK)
					sys_spu_thread_disconnect_event(threads[k], SYS_SPU_THREAD_EVENT_USER, port);
			}
		}
		if (rc != CELL_OK) {
			plat_log("SPU %d (%s) did not start: 0x%08x", k, cabac ? "CABAC" : "pixel", (unsigned)rc);
			sys_spu_thread_group_destroy(groups[k]);
			plat_spes_stop(1);
			return -1;
		}
		nstarted++;
	}
	plat_log("%d SPU threads running (%d CABAC, %d pixel; one group each, priority %d)", nstarted, ncabac, npixel, SPU_PRIO);
	return 0;
}

void plat_spes_stop(int hung)
{
	int k, cause, status;

	for (k = 0; k < nstarted; k++) {
		if (hung)
			sys_spu_thread_group_terminate(groups[k], 0);
		else
			sys_spu_thread_write_spu_mb(threads[k], 0);
	}
	for (k = 0; k < nstarted; k++) {
		sys_spu_thread_group_join(groups[k], &cause, &status);
		sys_spu_thread_disconnect_event(threads[k], SYS_SPU_THREAD_EVENT_USER, k < ncab ? CABAC_EVENT_PORT : PIX_EVENT_PORT);
		sys_spu_thread_group_destroy(groups[k]);
	}
	nstarted = 0;
	{
		sys_event_t ev;

		while (sys_event_queue_receive(queue, &ev, 1) == CELL_OK)     /* nothing left over for the next run */
			;
	}
	wake_pending = 0;
}

int plat_spe_send(int spe, uint32_t ea)
{
	return sys_spu_thread_write_spu_mb(threads[spe], ea) == CELL_OK ? 0 : -1;
}

int plat_wait(int timeout_us, int *spe, uint32_t *word, uint32_t *value)
{
	sys_event_t ev;
	int rc = sys_event_queue_receive(queue, &ev, (usecond_t)(timeout_us < 1 ? 1 : timeout_us));
	int k;

	if (rc == ETIMEDOUT)
		return 0;
	if (rc != CELL_OK)
		return -1;
	if (ev.source == SYS_SPU_THREAD_EVENT_USER_KEY) {
		for (k = 0; k < nstarted; k++)
			if (threads[k] == (sys_spu_thread_t)ev.data1) {
				*spe = k;
				*word = (uint32_t)(ev.data2 & 0xFFFFFF);
				*value = (uint32_t)ev.data3;
				__sync();
				return 1;
			}
		plat_log("an event from an unknown SPU thread 0x%llx", (unsigned long long)ev.data1);
		return 0;
	}
	wake_pending = 0;
	return 2;
}

/* at most one wake-up waits in the queue: the SPEs' answers must always find room (a full queue would
   lose a thrown event) */
void plat_wake(void)
{
	if (wake_pending)
		return;
	wake_pending = 1;
	__sync();
	if (sys_event_port_send(wake_port, 0, 0, 0) != CELL_OK)
		wake_pending = 0;
}

uint64_t plat_ticks(void)
{
	return __mftb();
}

double plat_tick_mhz(void)
{
	if (tb_mhz <= 0)
		tb_mhz = (double)sys_time_get_timebase_frequency() / 1e6;
	return tb_mhz;
}

uint32_t plat_ea(const void *p)
{
	return (uint32_t)(uintptr_t)p;
}

void plat_sync(void)
{
	__sync();
}

void *plat_alloc(size_t size, size_t align)
{
	return memalign(align, size);
}

void plat_free(void *p)
{
	free(p);
}

typedef struct {
	sys_ppu_thread_t id;
	plat_thread_fn fn;
	void *arg;
} thread_box;

static void thread_entry(uint64_t a)
{
	thread_box *b = (thread_box *)(uintptr_t)a;

	b->fn(b->arg);
	sys_ppu_thread_exit(0);
}

void *plat_thread_start(plat_thread_fn fn, void *arg, int prio, const char *name)
{
	thread_box *b = malloc(sizeof(thread_box));

	if (!b)
		return NULL;
	b->fn = fn;
	b->arg = arg;
	if (sys_ppu_thread_create(&b->id, thread_entry, (uint64_t)(uintptr_t)b, prio, 0x10000, SYS_PPU_THREAD_CREATE_JOINABLE,
	                          name) != CELL_OK) {
		free(b);
		return NULL;
	}
	return b;
}

void plat_thread_join(void *thread)
{
	thread_box *b = thread;
	uint64_t ret;

	sys_ppu_thread_join(b->id, &ret);
	free(b);
}

void plat_sleep_us(int us)
{
	sys_timer_usleep((usecond_t)us);
}

void plat_log(const char *fmt, ...)
{
	char buf[512];
	va_list ap;

	if (!log_fn)
		return;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	log_fn(buf);
}
