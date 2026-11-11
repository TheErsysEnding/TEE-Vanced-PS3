/* TEE VP9 building block - made by tools/make_baustein.sh from ps3/source/tv9_plat_ps3.c; do not edit here: change the
   project's file and run the script again. */
/* TEE VP9, the platform (tv9_plat.h) on the PS3's PPU with PSL1GHT: SPU threads (one group each: the system can
   then stop one without stopping the others), one lv2 event queue for all answers plus a local event port that
   other PPU threads use to wake the decoder thread, PPU threads, the timebase. After the TEE Decoder's plat_ps3.c.
   The SPU programs come as arrays tv9_spu_entropy / tv9_spu_pixel: in the test application they are the built
   programs (spu_bin.h, spu_pixel_bin.h, with -DTV9_TEST_APP), in the building block tv9_spu_images.c. */
#include "tv9_plat.h"

#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ppu-asm.h>
#include <ppu-lv2.h>
#include <ppu-types.h>
#include <sys/event_queue.h>
#include <sys/spu.h>
#include <sys/systime.h>
#include <sys/thread.h>
#include <lv2/systime.h>

#include "vp9_job.h"
#include "vp9_pix_job.h"

#ifdef TV9_TEST_APP
#include "spu_bin.h"
#include "spu_pixel_bin.h"
#define tv9_spu_entropy spu_bin
#define tv9_spu_pixel   spu_pixel_bin
#else
extern const unsigned char tv9_spu_entropy[], tv9_spu_pixel[];   /* tv9_spu_images.c */
#endif

#define SPU_PRIO     100
#define WAKE_NAME    0x5445455639574B45ull       /* "TEEV9WKE" */
#define E_TIMEDOUT   0x8001000B

static sysSpuImage img_entropy, img_pixel;
static sys_spu_group_t groups[TV9P_MAX_SPES];
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
	sys_event_queue_attr_t qattr = { SYS_EVENT_QUEUE_FIFO, SYS_EVENT_QUEUE_PPU, "teevp9m" };
	static int spu_init;
	s32 rc;

	if (ready)
		return 0;
	tb_mhz = (double)sysGetTimebaseFrequency() / 1e6;
	if (!spu_init) {                    /* an app that set the SPUs up itself keeps its setting (the call fails) */
		rc = sysSpuInitialize(TV9P_MAX_SPES, 0);
		tv9p_log("sysSpuInitialize(%d, 0) = 0x%08x, timebase %.3f MHz", TV9P_MAX_SPES, (unsigned)rc, tb_mhz);
		spu_init = 1;
	}
	if (sysSpuImageImport(&img_entropy, tv9_spu_entropy, 0) != 0 || sysSpuImageImport(&img_pixel, tv9_spu_pixel, 0) != 0) {
		tv9p_log("the SPU images did not load");
		return -1;
	}
	if (sysEventQueueCreate(&queue, &qattr, SYS_EVENT_QUEUE_KEY_LOCAL, 64) != 0) {
		tv9p_log("the event queue did not come");
		return -1;
	}
	if (sysEventPortCreate(&wake_port, SYS_EVENT_PORT_LOCAL, WAKE_NAME) != 0 || sysEventPortConnectLocal(wake_port, queue) != 0) {
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
	sysEventPortDisconnect(wake_port);
	sysEventPortDestroy(wake_port);
	sysEventQueueDestroy(queue, 0);
	sysSpuImageClose(&img_entropy);
	sysSpuImageClose(&img_pixel);
	ready = 0;
}

int tv9p_spes_start(int nentropy, int npixel)
{
	sysSpuThreadAttribute tattr = { "teevp9m", 8, SPU_THREAD_ATTR_NONE };
	sysSpuThreadArgument arg = { 0, 0, 0, 0 };
	int k;
	s32 rc;

	if (!ready || nentropy + npixel > TV9P_MAX_SPES)
		return -1;
	nstarted = 0;
	nent = nentropy;
	for (k = 0; k < nentropy + npixel; k++) {
		char name[16];
		sysSpuThreadGroupAttribute gattr;
		const int ent = k < nentropy;

		snprintf(name, sizeof(name), "teevp9m%d", k);
		gattr.nsize = strlen(name) + 1;
		gattr.name = name;
		gattr.type = 0;
		memset(&gattr.option, 0, sizeof(gattr.option));
		rc = sysSpuThreadGroupCreate(&groups[k], 1, SPU_PRIO, &gattr);
		if (rc != 0) {
			tv9p_log("SPU group %d did not come: 0x%08x", k, (unsigned)rc);
			tv9p_spes_stop(1);
			return -1;
		}
		rc = sysSpuThreadInitialize(&threads[k], groups[k], 0, ent ? &img_entropy : &img_pixel, &tattr, &arg);
		if (rc == 0) {
			rc = sysSpuThreadConnectEvent(threads[k], queue, SPU_THREAD_EVENT_USER, ent ? VP9_EVENT_PORT : PIX_EVENT_PORT);
			if (rc == 0) {
				rc = sysSpuThreadGroupStart(groups[k]);
				if (rc != 0)
					sysSpuThreadDisconnectEvent(threads[k], SPU_THREAD_EVENT_USER, ent ? VP9_EVENT_PORT : PIX_EVENT_PORT);
			}
		}
		if (rc != 0) {
			tv9p_log("SPU %d (%s) did not start: 0x%08x", k, ent ? "entropy" : "pixel", (unsigned)rc);
			sysSpuThreadGroupDestroy(groups[k]);
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
	u32 cause, status;
	int k;

	for (k = 0; k < nstarted; k++) {
		if (hung)
			sysSpuThreadGroupTerminate(groups[k], 0);
		else
			sysSpuThreadWriteMb(threads[k], 0);
	}
	for (k = 0; k < nstarted; k++) {
		sysSpuThreadGroupJoin(groups[k], &cause, &status);
		sysSpuThreadDisconnectEvent(threads[k], SPU_THREAD_EVENT_USER, k < nent ? VP9_EVENT_PORT : PIX_EVENT_PORT);
		sysSpuThreadGroupDestroy(groups[k]);
	}
	nstarted = 0;
	if (ready) {
		sys_event_t ev;

		while (sysEventQueueReceive(queue, &ev, 1) == 0)     /* nothing left over for the next run */
			;
	}
	wake_pending = 0;
}

int tv9p_spe_send(int spe, uint32_t ea)
{
	return sysSpuThreadWriteMb(threads[spe], ea) == 0 ? 0 : -1;
}

int tv9p_wait(int timeout_us, int *spe, uint32_t *word)
{
	sys_event_t ev;
	s32 rc = sysEventQueueReceive(queue, &ev, (u64)(timeout_us < 1 ? 1 : timeout_us));
	int k;

	if (rc == (s32)E_TIMEDOUT)
		return 0;
	if (rc != 0)
		return -1;
	if (ev.source == SPU_THREAD_EVENT_USER_KEY) {
		for (k = 0; k < nstarted; k++)
			if (threads[k] == (sys_spu_thread_t)ev.data_1) {
				*spe = k;
				*word = (uint32_t)(ev.data_2 & 0xFFFFFF);
				__asm__ volatile("sync" ::: "memory");
				return 1;
			}
		tv9p_log("an event from an unknown SPU thread 0x%llx", (unsigned long long)ev.data_1);
		return 0;
	}
	wake_pending = 0;
	return 2;
}

/* at most one wake-up waits in the queue: the SPEs' answers must always find room (a full queue would lose a
   thrown event) */
void tv9p_wake(void)
{
	if (wake_pending)
		return;
	wake_pending = 1;
	__asm__ volatile("sync" ::: "memory");
	if (sysEventPortSend(wake_port, 0, 0, 0) != 0)
		wake_pending = 0;
}

uint64_t tv9p_ticks(void)
{
	uint64_t t;

	__asm__ volatile("mftb %0" : "=r"(t));
	return t;
}

double tv9p_tick_mhz(void)
{
	if (tb_mhz <= 0)
		tb_mhz = (double)sysGetTimebaseFrequency() / 1e6;
	return tb_mhz;
}

uint32_t tv9p_ea(const void *p)
{
	return (uint32_t)(uintptr_t)p;
}

void tv9p_sync(void)
{
	__asm__ volatile("sync" ::: "memory");
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

static void thread_entry(void *a)
{
	thread_box *b = a;

	b->fn(b->arg);
	sysThreadExit(0);
}

void *tv9p_thread_start(tv9p_thread_fn fn, void *arg, int prio, const char *name)
{
	thread_box *b = malloc(sizeof(thread_box));

	if (!b)
		return NULL;
	b->fn = fn;
	b->arg = arg;
	if (sysThreadCreate(&b->id, thread_entry, b, prio, 0x10000, THREAD_JOINABLE, (char *)name) != 0) {
		free(b);
		return NULL;
	}
	return b;
}

void tv9p_thread_join(void *thread)
{
	thread_box *b = thread;
	u64 ret;

	sysThreadJoin(b->id, &ret);
	free(b);
}

void tv9p_sleep_us(int us)
{
	usleep((useconds_t)us);
}

void tv9p_log(const char *fmt, ...)
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
