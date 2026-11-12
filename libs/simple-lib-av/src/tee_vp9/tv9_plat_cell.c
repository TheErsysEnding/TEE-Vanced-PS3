/* TEE VP9 building block - made by tools/make_baustein.sh from ps3/cell/tv9_plat_cell.c; do not edit here: change the
   project's file and run the script again. */
/* TEE VP9, the platform (tv9_plat.h) on the PS3's PPU with Sony's Cell SDK (an app like TEE Vanced, built with
   ppu-lv2-gcc or SNC) - the same as tv9_plat_ps3.c (PSL1GHT) with the SDK's names: SPU threads (one group each), one
   lv2 event queue for all answers plus a local event port that other PPU threads use to wake the decoder thread,
   PPU threads, the timebase. An app compiles this file or tv9_plat_ps3.c, never both. After the TEE Decoder's
   plat_cell.c, which runs in TEE Vanced.
   The SPU programs are the arrays of tv9_spu_images.c: an SPU ELF is loaded by the kernel, whichever SDK the PPU
   side was built with.
   State (0.1.0): compiled with the SDK 4.75's ppu-lv2-gcc (no warnings); not run on the console yet - the first
   app that uses it checks it with a stream it knows (TEE-VP9-INTEGRATION.md). */
#include "tv9_plat.h"

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

#include "vp9_job.h"
#include "vp9_pix_job.h"

#define SPU_PRIO     100
#define WAKE_NAME    0x5445455639574B45ull       /* "TEEV9WKE" */

extern const unsigned char tv9_spu_entropy[], tv9_spu_pixel[];   /* tv9_spu_images.c */

static sys_spu_image_t img_entropy, img_pixel;
static sys_spu_thread_group_t groups[TV9P_MAX_SPES];
static sys_spu_thread_t threads[TV9P_MAX_SPES];
static int nstarted, nent, ready;
static sys_event_queue_t queue;
static sys_event_port_t wake_port;
static volatile int wake_pending;
static double tb_mhz;
static void (*log_fn)(const char *line);

void tv9p_set_log(void (*fn)(const char *line))
{
	log_fn = fn;
}

int tv9p_init(void)
{
	sys_event_queue_attribute_t qattr;
	static int spu_init;
	int rc;

	if (ready)
		return 0;
	tb_mhz = (double)sys_time_get_timebase_frequency() / 1e6;
	if (!spu_init) {                    /* an app that set the SPUs up itself keeps its setting (the call fails) */
		rc = sys_spu_initialize(TV9P_MAX_SPES, 0);
		tv9p_log("sys_spu_initialize(%d, 0) = 0x%08x, timebase %.3f MHz", TV9P_MAX_SPES, (unsigned)rc, tb_mhz);
		spu_init = 1;
	}
	if (sys_spu_image_import(&img_entropy, tv9_spu_entropy, SYS_SPU_IMAGE_PROTECT) != CELL_OK ||
	    sys_spu_image_import(&img_pixel, tv9_spu_pixel, SYS_SPU_IMAGE_PROTECT) != CELL_OK) {
		tv9p_log("the SPU images did not load");
		return -1;
	}
	sys_event_queue_attribute_initialize(qattr);
	qattr.attr_protocol = SYS_SYNC_FIFO;
	qattr.type = SYS_PPU_QUEUE;
	if (sys_event_queue_create(&queue, &qattr, SYS_EVENT_QUEUE_LOCAL, 64) != CELL_OK) {
		tv9p_log("the event queue did not come");
		return -1;
	}
	if (sys_event_port_create(&wake_port, SYS_EVENT_PORT_LOCAL, WAKE_NAME) != CELL_OK ||
	    sys_event_port_connect_local(wake_port, queue) != CELL_OK) {
		tv9p_log("the wake-up port did not connect");
		return -1;
	}
	wake_pending = 0;
	ready = 1;
	return 0;
}

void tv9p_shutdown(void)
{
	if (!ready)
		return;
	sys_event_port_disconnect(wake_port);
	sys_event_port_destroy(wake_port);
	sys_event_queue_destroy(queue, SYS_EVENT_QUEUE_DESTROY_FORCE);
	sys_spu_image_close(&img_entropy);
	sys_spu_image_close(&img_pixel);
	ready = 0;
}

int tv9p_spes_start(int nentropy, int npixel)
{
	int k, rc;

	if (!ready || nentropy + npixel > TV9P_MAX_SPES)
		return -1;
	nstarted = 0;
	nent = nentropy;
	for (k = 0; k < nentropy + npixel; k++) {
		char name[16];
		sys_spu_thread_group_attribute_t gattr;
		sys_spu_thread_attribute_t tattr;
		sys_spu_thread_argument_t arg;
		int ent = k < nentropy;
		uint8_t port = ent ? VP9_EVENT_PORT : PIX_EVENT_PORT;

		snprintf(name, sizeof(name), "teevp9m%d", k);
		sys_spu_thread_group_attribute_initialize(gattr);
		sys_spu_thread_group_attribute_name(gattr, name);
		rc = sys_spu_thread_group_create(&groups[k], 1, SPU_PRIO, &gattr);
		if (rc != CELL_OK) {
			tv9p_log("SPU group %d did not come: 0x%08x", k, (unsigned)rc);
			tv9p_spes_stop(1);
			return -1;
		}
		sys_spu_thread_attribute_initialize(tattr);
		sys_spu_thread_attribute_name(tattr, "teevp9m");
		memset(&arg, 0, sizeof(arg));
		rc = sys_spu_thread_initialize(&threads[k], groups[k], 0, ent ? &img_entropy : &img_pixel, &tattr, &arg);
		if (rc == CELL_OK) {
			rc = sys_spu_thread_connect_event(threads[k], queue, SYS_SPU_THREAD_EVENT_USER, port);
			if (rc == CELL_OK) {
				rc = sys_spu_thread_group_start(groups[k]);
				if (rc != CELL_OK)
					sys_spu_thread_disconnect_event(threads[k], SYS_SPU_THREAD_EVENT_USER, port);
			}
		}
		if (rc != CELL_OK) {
			tv9p_log("SPU %d (%s) did not start: 0x%08x", k, ent ? "entropy" : "pixel", (unsigned)rc);
			sys_spu_thread_group_destroy(groups[k]);
			tv9p_spes_stop(1);
			return -1;
		}
		nstarted++;
	}
	tv9p_log("%d SPU threads running (%d entropy, %d pixel; one group each, priority %d)", nstarted, nentropy, npixel, SPU_PRIO);
	return 0;
}

void tv9p_spes_stop(int hung)
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
		sys_spu_thread_disconnect_event(threads[k], SYS_SPU_THREAD_EVENT_USER, k < nent ? VP9_EVENT_PORT : PIX_EVENT_PORT);
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

int tv9p_spe_send(int spe, uint32_t ea)
{
	return sys_spu_thread_write_spu_mb(threads[spe], ea) == CELL_OK ? 0 : -1;
}

int tv9p_wait(int timeout_us, int *spe, uint32_t *word)
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
				__sync();
				return 1;
			}
		tv9p_log("an event from an unknown SPU thread 0x%llx", (unsigned long long)ev.data1);
		return 0;
	}
	wake_pending = 0;
	return 2;
}

/* at most one wake-up waits in the queue: the SPEs' answers must always find room (a full queue would
   lose a thrown event) */
void tv9p_wake(void)
{
	if (wake_pending)
		return;
	wake_pending = 1;
	__sync();
	if (sys_event_port_send(wake_port, 0, 0, 0) != CELL_OK)
		wake_pending = 0;
}

uint64_t tv9p_ticks(void)
{
	return __mftb();
}

double tv9p_tick_mhz(void)
{
	if (tb_mhz <= 0)
		tb_mhz = (double)sys_time_get_timebase_frequency() / 1e6;
	return tb_mhz;
}

uint32_t tv9p_ea(const void *p)
{
	return (uint32_t)(uintptr_t)p;
}

void tv9p_sync(void)
{
	__sync();
}

void *tv9p_alloc(size_t size, size_t align)
{
	return memalign(align, size);
}

void tv9p_free(void *p)
{
	free(p);
}

typedef struct {
	sys_ppu_thread_t id;
	tv9p_thread_fn fn;
	void *arg;
} thread_box;

static void thread_entry(uint64_t a)
{
	thread_box *b = (thread_box *)(uintptr_t)a;

	b->fn(b->arg);
	sys_ppu_thread_exit(0);
}

void *tv9p_thread_start(tv9p_thread_fn fn, void *arg, int prio, const char *name)
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

void tv9p_thread_join(void *thread)
{
	thread_box *b = thread;
	uint64_t ret;

	sys_ppu_thread_join(b->id, &ret);
	free(b);
}

void tv9p_sleep_us(int us)
{
	sys_timer_usleep((usecond_t)us);
}

void tv9p_log(const char *fmt, ...)
{
	char buf[1536];                     /* (the RSX part's report lines are long) */
	va_list ap;

	if (!log_fn)
		return;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	log_fn(buf);
}
