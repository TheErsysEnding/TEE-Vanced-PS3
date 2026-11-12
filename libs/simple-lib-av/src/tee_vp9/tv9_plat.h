/* TEE VP9 building block - made by tools/make_baustein.sh from ps3/source/tv9_plat.h; do not edit here: change the
   project's file and run the script again. */
/* TEE VP9: what the decoder (tee_vp9.c) needs from the system. Two implementations: the PS3's PPU with PSL1GHT
   (tv9_plat_ps3.c) and with Sony's Cell SDK (tv9_plat_cell.c). It is the TEE Decoder's plat.h with other names, so
   that both decoders can be linked into one program (one of them open at a time: the console has 6 SPEs). */
#ifndef TV9_PLAT_H
#define TV9_PLAT_H

#include <stddef.h>
#include <stdint.h>

#define TV9P_MAX_SPES 6             /* never more: asking for 7 froze the console (TEE PS3 Live, 2026-11-04) */

/* the SPU programs loaded, the event queue made (again after tv9p_shutdown); 0 = ok */
int  tv9p_init(void);
void tv9p_shutdown(void);
void tv9p_set_log(void (*fn)(const char *line));   /* where tv9p_log's lines go (NULL: nowhere) */

/* SPEs: the entropy ones first (0 .. nentropy - 1), then the pixel ones; 0 = ok */
int  tv9p_spes_start(int nentropy, int npixel);
void tv9p_spes_stop(int hung);
int  tv9p_spe_send(int spe, uint32_t ea);   /* a job's address into the SPE's inbound mailbox */

/* the next event within timeout_us (>= 1): 1 an SPE's answer (*spe, *word = (seq & 0xFFFF) << 8 | status), 2 a
   wake-up from another thread, 0 none in time, -1 error */
int  tv9p_wait(int timeout_us, int *spe, uint32_t *word);
void tv9p_wake(void);

uint64_t tv9p_ticks(void);                  /* the PPU timebase */
double   tv9p_tick_mhz(void);               /* ticks per microsecond */
uint32_t tv9p_ea(const void *p);            /* an address as the SPEs see it */
void     tv9p_sync(void);                   /* stores so far are visible to the SPEs and other threads */
void    *tv9p_alloc(size_t size, size_t align);   /* memory the SPEs can reach */
void     tv9p_free(void *p);

typedef void (*tv9p_thread_fn)(void *arg);
void *tv9p_thread_start(tv9p_thread_fn fn, void *arg, int prio, const char *name);   /* NULL on failure */
void  tv9p_thread_join(void *thread);
void  tv9p_sleep_us(int us);

/* the decoder's log lines */
void tv9p_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
