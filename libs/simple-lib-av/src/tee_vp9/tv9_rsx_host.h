/* TEE VP9 building block - made by tools/make_baustein.sh from ps3/source/tv9_rsx_host.h; do not edit here: change the
   project's file and run the script again. */
/* tv9_rsx_host.h : what the building block's RSX part (tvp9_config.rsx, rsx_mc.c) asks of the application's display
   code. The RSX has one command buffer and one state; the application draws its screen with it while the block
   predicts. The application implements the functions below (the test application: gfx.c).

   The rules behind them - they held on the console with a picture drawn at every vertical blank (test 3.70):
   1. One command buffer, one lock. Whoever writes commands holds the lock from its first command to its flush.
   2. Room is made before writing. When it is short: flush, wait until the RSX has read everything, start again at
      the buffer's beginning. The buffer never wraps and libgcm's callback never runs (its segment switch hung the
      RSX): set the context's end to the real end of a buffer of 512 KB or more.
   3. Each side sets every state it depends on. The block sets its target, viewport, tests, textures and vertex
      arrays for every row; its programs only when the application says it has drawn (tv9h_state_gen).
   4. A flip without a wait in the command stream (no cellGcmSetWaitFlip): the RSX would stand still until the
      vertical blank and every prediction behind it too. The PPU waits for the flip instead (cellGcmGetFlipStatus)
      before it draws into the buffer that was on the screen.
   5. A picture of the decoder that the RSX reads as a texture is released when the flip after its draw has happened.
   The block uses the labels 200, 201 and 210 - 212; the application leaves them alone. */
#ifndef TV9_RSX_HOST_H
#define TV9_RSX_HOST_H

#include <stdint.h>

/* the command buffer context the application writes its own commands with (Sony: CellGcmContextData *, as
   gCellGcmCurrentContext; PSL1GHT: gcmContextData *) */
void *tv9h_context(void);

/* the lock of rule 1 (not recursive; the block holds it for a row's commands, well under a millisecond as a rule) */
void tv9h_lock(void);
void tv9h_unlock(void);

/* with the lock held: the bytes left before the command buffer's end */
int tv9h_cb_room(void);
/* with the lock held, after a flush: the next commands from the buffer's beginning, once the RSX has read up to
   here (a jump command, put = the beginning, wait for get). 0, or -1 when it did not within timeout_us - then
   nothing is reset and the block gives the RSX up for this stream */
int tv9h_cb_restart(int timeout_us);

/* a number the application counts up (with the lock held) every time it has written commands that load a program or
   leave other state than the block's behind */
unsigned tv9h_state_gen(void);

/* the block starts (1) or ends (0) predicting. An application that keeps rule 4 has nothing to do here; one that
   cannot (a status screen with a flip wait, as the test application's) stops drawing meanwhile */
void tv9h_predicting(int on);

/* RSX (local) memory for the block: 3 surfaces rows high, an index buffer, the fragment programs - about 1.3 MB for
   1440p, taken in tvp9_open / at the first key frame and given back at tvp9_close. NULL: none */
void *tv9h_local_alloc(uint32_t align, uint32_t bytes);
void tv9h_local_free(void *p);

#endif
