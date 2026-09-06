// ps-button - implementation. See ps-button.h for why this is possible from a game at all.

#include "ps-button.h"

#include <cell/pad.h>
#include <string.h>

#include "dbg.h"
#include "thread.h"   // sleepMs

// How long the PS bit stays set. webMAN-MOD's virtual pad uses 70 ms for the same job on real hardware, so
// that is the number with evidence behind it. Sony's sample sets the bit for a single frame (~17 ms), which
// may well be enough - but a press that is too short fails silently, and 70 ms is still one deliberate
// button press, not a hang.
#define PS_HOLD_MS 70

// A frame with sticks centred, sensors at rest and nothing held - what "no input" looks like on the wire.
// Both the press and the release start from this, so the release really does clear everything.
static void clearFrame(CellPadData *frame)
{
   memset(frame, 0, sizeof *frame);
   frame->len = CELL_PAD_MAX_CODES;
   frame->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X]  = 0x80;
   frame->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y]  = 0x80;
   frame->button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X] = 0x80;
   frame->button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y] = 0x80;
   frame->button[CELL_PAD_BTN_OFFSET_SENSOR_X] = 0x200;
   frame->button[CELL_PAD_BTN_OFFSET_SENSOR_Y] = 0x200;
   frame->button[CELL_PAD_BTN_OFFSET_SENSOR_Z] = 0x200;
   frame->button[CELL_PAD_BTN_OFFSET_SENSOR_G] = 0x200;
}

int pressPsButton(void)
{
   // The virtual pad exists only for the duration of the press. Keeping it registered would hold a
   // controller port for the whole session and, if the app died, leave a ghost controller behind that the
   // user can only clear by restarting the console - a well-known nuisance with this API.
   int32_t handle = cellPadLddRegisterController();
   if (handle < 0) {
      logError("[ps] could not register the virtual pad, rc=0x%x\n", (unsigned)handle);
      return -1;
   }

   // Which port it landed on is the one fact worth logging: if it ever took port 0 the real controller
   // would have been pushed aside, and pollPad() reads port 0 and nothing else.
   int32_t port = cellPadLddGetPortNo(handle);
   logInfo("[ps] virtual pad registered, handle=%d port=%d\n", (int)handle, (int)port);

   CellPadData frame;
   clearFrame(&frame);
   frame.button[0] |= CELL_PAD_CTRL_LDD_PS;   // button[0] is the LDD-only slot; DIGITAL1/2 are 2 and 3
   int32_t rc = cellPadLddDataInsert(handle, &frame);
   if (rc != 0) logError("[ps] press insert rc=0x%x\n", (unsigned)rc);

   sleepMs(PS_HOLD_MS);

   clearFrame(&frame);
   int32_t releaseRc = cellPadLddDataInsert(handle, &frame);
   if (releaseRc != 0) logError("[ps] release insert rc=0x%x\n", (unsigned)releaseRc);

   cellPadLddUnregisterController(handle);
   logInfo("[ps] PS button sent (press rc=0x%x, release rc=0x%x)\n", (unsigned)rc, (unsigned)releaseRc);
   return rc == 0 ? 0 : -1;
}
