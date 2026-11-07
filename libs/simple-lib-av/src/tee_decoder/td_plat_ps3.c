/* TEE Decoder building block - made by tools/make_baustein.sh from ps3/source/plat_ps3.c; do not edit here: change the
   project's file and run the script again. */
#include "td_names.h"
/* TEE Decoder, the platform (plat.h) on the PS3's PPU with PSL1GHT: SPU threads (one group each, as TEE CABAC
   SPU Test: the system can then stop one without stopping the others), one lv2 event queue for all answers
   plus a local event port that other PPU threads use to wake the decoder thread, PPU threads, the timebase.
   The SPU programs come as arrays (td_spu_images.c, made from the built programs by tools/bin2c.py), so an
   app needs no SPU toolchain; the test app adds the CABAC benchmark's variants with TDEC_BENCH. */
#include "td_plat.h"

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

#include "td_cabac_job.h"
#include "td_pixel_job.h"
#ifdef TDEC_BENCH
#include "spu_cabac_prof_bin.h"
#include "spu_cabac_v0_bin.h"
#endif

extern const unsigned char tdec_spu_cabac[], tdec_spu_pixel[];   /* td_spu_images.c */

#define SPU_PRIO     100
#define WAKE_NAME    0x5445454457414B45ull       /* "TEEDWAKE" */
#define E_TIMEDOUT   0x8001000B

static sysSpuImage img_cabac, img_pixel;
#ifdef TDEC_BENCH
static sysSpuImage img_cabac_v0, img_cabac_prof;
#endif
static sys_spu_group_t groups[PLAT_MAX_SPES];
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

/* sys_memory_get_user_memory_size (syscall 352): total and available user memory */
static s32 user_memory(u32 *info)
{
	lv2syscall1(352, (u64)(uintptr_t)info);
	return_to_user_prog(s32);
}

void plat_log_memory(const char *when)
{
	u32 info[2] __attribute__((aligned(8))) = { 0, 0 };

	if (user_memory(info) == 0)
		plat_log("user memory %s: %u MB of %u MB free", when, info[1] >> 20, info[0] >> 20);
}

int plat_init(void)
{
	sys_event_queue_attr_t qattr = { SYS_EVENT_QUEUE_FIFO, SYS_EVENT_QUEUE_PPU, "teedec" };
	static int spu_init;
	s32 rc;

	if (ready)
		return 0;
	tb_mhz = (double)sysGetTimebaseFrequency() / 1e6;
	if (!spu_init) {                    /* an app that set the SPUs up itself keeps its setting (the call fails) */
		rc = sysSpuInitialize(PLAT_MAX_SPES, 0);
		plat_log("sysSpuInitialize(%d, 0) = 0x%08x, timebase %.3f MHz", PLAT_MAX_SPES, (unsigned)rc, tb_mhz);
		spu_init = 1;
	}
	if (sysSpuImageImport(&img_cabac, tdec_spu_cabac, 0) != 0 || sysSpuImageImport(&img_pixel, tdec_spu_pixel, 0) != 0) {
		plat_log("the SPU images did not load");
		return -1;
	}
#ifdef TDEC_BENCH
	if (sysSpuImageImport(&img_cabac_v0, spu_cabac_v0_bin, 0) != 0 || sysSpuImageImport(&img_cabac_prof, spu_cabac_prof_bin, 0) != 0) {
		plat_log("the benchmark's SPU images did not load");
		return -1;
	}
#endif
	if (sysEventQueueCreate(&queue, &qattr, SYS_EVENT_QUEUE_KEY_LOCAL, 64) != 0) {
		plat_log("the event queue did not come");
		return -1;
	}
	if (sysEventPortCreate(&wake_port, SYS_EVENT_PORT_LOCAL, WAKE_NAME) != 0 || sysEventPortConnectLocal(wake_port, queue) != 0) {
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
	sysEventPortDisconnect(wake_port);
	sysEventPortDestroy(wake_port);
	sysEventQueueDestroy(queue, 0);
	sysSpuImageClose(&img_cabac);
	sysSpuImageClose(&img_pixel);
#ifdef TDEC_BENCH
	sysSpuImageClose(&img_cabac_v0);
	sysSpuImageClose(&img_cabac_prof);
#endif
	ready = 0;
}

/* SPE k runs cimg[k] (k < ncabac, CABAC answers) or the pixel program */
static int spes_start(int ncabac, int npixel, sysSpuImage *const *cimg)
{
	sysSpuThreadAttribute tattr = { "teedec", 7, SPU_THREAD_ATTR_NONE };
	sysSpuThreadArgument arg = { 0, 0, 0, 0 };
	int k;
	s32 rc;

	if (!ready || ncabac + npixel > PLAT_MAX_SPES)
		return -1;
	nstarted = 0;
	ncab = ncabac;
	for (k = 0; k < ncabac + npixel; k++) {
		char name[16];
		sysSpuThreadGroupAttribute gattr;
		int cabac = k < ncabac;

		snprintf(name, sizeof(name), "teedec%d", k);
		gattr.nsize = strlen(name) + 1;
		gattr.name = name;
		gattr.type = 0;
		memset(&gattr.option, 0, sizeof(gattr.option));
		rc = sysSpuThreadGroupCreate(&groups[k], 1, SPU_PRIO, &gattr);
		if (rc != 0) {
			plat_log("SPU group %d did not come: 0x%08x", k, (unsigned)rc);
			plat_spes_stop(1);
			return -1;
		}
		rc = sysSpuThreadInitialize(&threads[k], groups[k], 0, cabac ? cimg[k] : &img_pixel, &tattr, &arg);
		if (rc == 0) {
			rc = sysSpuThreadConnectEvent(threads[k], queue, SPU_THREAD_EVENT_USER, cabac ? CABAC_EVENT_PORT : PIX_EVENT_PORT);
			if (rc == 0) {
				rc = sysSpuThreadGroupStart(groups[k]);
				if (rc != 0)
					sysSpuThreadDisconnectEvent(threads[k], SPU_THREAD_EVENT_USER, cabac ? CABAC_EVENT_PORT : PIX_EVENT_PORT);
			}
		}
		if (rc != 0) {
			plat_log("SPU %d (%s) did not start: 0x%08x", k, cabac ? "CABAC" : "pixel", (unsigned)rc);
			sysSpuThreadGroupDestroy(groups[k]);
			plat_spes_stop(1);
			return -1;
		}
		nstarted++;
	}
	plat_log("%d SPU threads running (%d CABAC, %d pixel; one group each, priority %d)", nstarted, ncabac, npixel, SPU_PRIO);
	return 0;
}

int plat_spes_start(int ncabac, int npixel)
{
	sysSpuImage *const cimg[PLAT_MAX_SPES] = { &img_cabac, &img_cabac, &img_cabac, &img_cabac, &img_cabac, &img_cabac };

	return spes_start(ncabac, npixel, cimg);
}

#ifdef TDEC_BENCH
int plat_bench_start(int nvariants)
{
	sysSpuImage *const cimg[3] = { &img_cabac_v0, &img_cabac, &img_cabac_prof };

	if (nvariants < 1 || nvariants > 3)
		return -1;
	return spes_start(nvariants, 0, cimg);
}
#endif

void plat_spes_stop(int hung)
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
		sysSpuThreadDisconnectEvent(threads[k], SPU_THREAD_EVENT_USER, k < ncab ? CABAC_EVENT_PORT : PIX_EVENT_PORT);
		sysSpuThreadGroupDestroy(groups[k]);
	}
	nstarted = 0;
	{
		sys_event_t ev;

		while (sysEventQueueReceive(queue, &ev, 1) == 0)     /* nothing left over for the next run */
			;
	}
	wake_pending = 0;
}

int plat_spe_send(int spe, uint32_t ea)
{
	return sysSpuThreadWriteMb(threads[spe], ea) == 0 ? 0 : -1;
}

int plat_wait(int timeout_us, int *spe, uint32_t *word, uint32_t *value)
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
				*value = (uint32_t)ev.data_3;
				__asm__ volatile("sync" ::: "memory");
				return 1;
			}
		plat_log("an event from an unknown SPU thread 0x%llx", (unsigned long long)ev.data_1);
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
	__asm__ volatile("sync" ::: "memory");
	if (sysEventPortSend(wake_port, 0, 0, 0) != 0)
		wake_pending = 0;
}

uint64_t plat_ticks(void)
{
	uint64_t t;

	__asm__ volatile("mftb %0" : "=r"(t));
	return t;
}

double plat_tick_mhz(void)
{
	if (tb_mhz <= 0)
		tb_mhz = (double)sysGetTimebaseFrequency() / 1e6;
	return tb_mhz;
}

uint32_t plat_ea(const void *p)
{
	return (uint32_t)(uintptr_t)p;
}

void plat_sync(void)
{
	__asm__ volatile("sync" ::: "memory");
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

static void thread_entry(void *a)
{
	thread_box *b = a;

	b->fn(b->arg);
	sysThreadExit(0);
}

void *plat_thread_start(plat_thread_fn fn, void *arg, int prio, const char *name)
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

void plat_thread_join(void *thread)
{
	thread_box *b = thread;
	u64 ret;

	sysThreadJoin(b->id, &ret);
	free(b);
}

void plat_sleep_us(int us)
{
	usleep((useconds_t)us);
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
