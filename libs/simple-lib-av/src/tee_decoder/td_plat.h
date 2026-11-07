/* TEE Decoder building block - made by tools/make_baustein.sh from ps3/source/plat.h; do not edit here: change the
   project's file and run the script again. */
/* TEE Decoder: what the decoder core (decoder.c) needs from the system. Two implementations: the PS3's PPU
   (plat_ps3.c: SPU threads, an lv2 event queue, PPU threads) and the Mac simulation (mac/plat_sim.c: every
   SPE a process of its own on shared memory, pipes for mailboxes and events). */
#ifndef PLAT_H
#define PLAT_H

#include <stddef.h>
#include <stdint.h>

#define PLAT_MAX_SPES 6             /* never more: asking for 7 froze the console (TEE PS3 Live, 2026-11-04) */

/* the SPU programs loaded, the event queue made (again after plat_shutdown); 0 = ok. One decoder at a time. */
int  plat_init(void);
void plat_shutdown(void);
void plat_set_log(void (*fn)(const char *line));   /* where plat_log's lines go (NULL: nowhere) */

/* SPEs: the CABAC ones first (0 .. ncabac - 1), then the pixel ones; 0 = ok */
int  plat_spes_start(int ncabac, int npixel);
void plat_spes_stop(int hung);
int  plat_spe_send(int spe, uint32_t ea);   /* a job's address into the SPE's inbound mailbox */
/* the CABAC benchmark (the PS3 test app, built with TDEC_BENCH): SPE k runs CABAC program variant k - 0 the frozen parser (1.0.2), 1 the
   tuned one, 2 the tuned one with profile timers - and answers like a CABAC SPE; plat_spes_stop ends it */
int  plat_bench_start(int nvariants);

/* the next event within timeout_us (>= 1): 1 an SPE's answer (*spe, *word = seq << 8 | status, *value),
   2 a wake-up from another thread, 0 none in time, -1 error */
int  plat_wait(int timeout_us, int *spe, uint32_t *word, uint32_t *value);
void plat_wake(void);

uint64_t plat_ticks(void);                  /* the PPU timebase (Mac: nanoseconds) */
double   plat_tick_mhz(void);               /* ticks per microsecond */
uint32_t plat_ea(const void *p);            /* an address as the SPEs see it */
void     plat_sync(void);                   /* stores so far are visible to the SPEs and other threads */
void    *plat_alloc(size_t size, size_t align);   /* memory the SPEs can reach */
void     plat_free(void *p);

typedef void (*plat_thread_fn)(void *arg);
void *plat_thread_start(plat_thread_fn fn, void *arg, int prio, const char *name);   /* NULL on failure */
void  plat_thread_join(void *thread);
void  plat_sleep_us(int us);

/* the decoder's log lines */
void plat_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
