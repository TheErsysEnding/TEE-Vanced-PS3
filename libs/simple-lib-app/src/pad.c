// pad - implementation
#include "pad.h"
#include <cell/pad.h>
#include <string.h>

#define PAD_BUTTON_COUNT 16

// which digital word (1 or 2) each button lives in, and its control bit - indexed by PadButton so one
// table drives both the edge derivation and the live down-mask (no duplicated per-button mapping).
static const struct { int word; uint16_t bit; } padControlBits[PAD_BUTTON_COUNT] = {
   { 1, CELL_PAD_CTRL_UP },       { 1, CELL_PAD_CTRL_DOWN },   { 1, CELL_PAD_CTRL_LEFT },  { 1, CELL_PAD_CTRL_RIGHT },
   { 2, CELL_PAD_CTRL_CROSS },    { 2, CELL_PAD_CTRL_CIRCLE }, { 2, CELL_PAD_CTRL_SQUARE }, { 2, CELL_PAD_CTRL_TRIANGLE },
   { 2, CELL_PAD_CTRL_L1 },       { 2, CELL_PAD_CTRL_R1 },     { 2, CELL_PAD_CTRL_L2 },    { 2, CELL_PAD_CTRL_R2 },
   { 1, CELL_PAD_CTRL_START },    { 1, CELL_PAD_CTRL_SELECT }, { 1, CELL_PAD_CTRL_L3 },    { 1, CELL_PAD_CTRL_R3 },
};

static CellPadData current;
static PadButtonState buttonStates[PAD_BUTTON_COUNT];
static Stick leftStick;
static Stick rightStick;
static volatile uint16_t rawDigital1;   // latest DIGITAL1 bits from pollPad (dpad, start, select, L3/R3)
static volatile uint16_t rawDigital2;   // latest DIGITAL2 bits from pollPad (face buttons, L1/R1/L2/R2)
static uint16_t prev1 = 0;              // DIGITAL1 bits at the previous updatePadEdges, for edge detection
static uint16_t prev2 = 0;

// (TEE Vanced V3.0.0, GitHub issue #6) the PS3's BD Remote Control. libpad lists it as a device of its own
// (CELL_PAD_DEV_TYPE_BD_REMOCON) on whichever port it got, and reports no bits but one key code at a time, held until
// CELL_PAD_BTN_CODE_BD_RELEASE. Each key is read as the pad button it stands for and joins the controller's bits, so
// every screen takes the remote without knowing of it. The ports are looked through every BD_SCAN_POLLS polls.
#define BD_SCAN_POLLS 60
static int      bdPort = -1;              // the remote's port, -1 while there is none
static unsigned bdPolls;
static uint16_t bdDigital1, bdDigital2;   // the key it holds down, as pad bits

static void bdKeyToBits(int code, uint16_t *digital1, uint16_t *digital2)
{
   *digital1 = *digital2 = 0;
   switch (code) {
   case CELL_PAD_BTN_CODE_BD_UP:         *digital1 = CELL_PAD_CTRL_UP;    break;
   case CELL_PAD_BTN_CODE_BD_DOWN:       *digital1 = CELL_PAD_CTRL_DOWN;  break;
   case CELL_PAD_BTN_CODE_BD_LEFT:
   case CELL_PAD_BTN_CODE_BD_SCAN_REV:   *digital1 = CELL_PAD_CTRL_LEFT;  break;   // back in a video, as LEFT does
   case CELL_PAD_BTN_CODE_BD_RIGHT:
   case CELL_PAD_BTN_CODE_BD_SCAN_FWD:   *digital1 = CELL_PAD_CTRL_RIGHT; break;
   case CELL_PAD_BTN_CODE_BD_START:
   case CELL_PAD_BTN_CODE_BD_POPUP_MENU:
   case CELL_PAD_BTN_CODE_BD_TOPMENU:    *digital1 = CELL_PAD_CTRL_START; break;   // the menus: Settings / Quality
   case CELL_PAD_BTN_CODE_BD_SELECT:     *digital1 = CELL_PAD_CTRL_SELECT; break;
   case CELL_PAD_BTN_CODE_BD_L3:         *digital1 = CELL_PAD_CTRL_L3;    break;
   case CELL_PAD_BTN_CODE_BD_R3:
   case CELL_PAD_BTN_CODE_BD_DISPLAY:    *digital1 = CELL_PAD_CTRL_R3;    break;   // the stats panel in a video
   case CELL_PAD_BTN_CODE_BD_ENTER:
   case CELL_PAD_BTN_CODE_BD_CROSS:
   case CELL_PAD_BTN_CODE_BD_PLAY:
   case CELL_PAD_BTN_CODE_BD_PAUSE:      *digital2 = CELL_PAD_CTRL_CROSS; break;   // X: open, and play/pause in a video
   case CELL_PAD_BTN_CODE_BD_RETURN:
   case CELL_PAD_BTN_CODE_BD_CIRCLE:
   case CELL_PAD_BTN_CODE_BD_STOP:       *digital2 = CELL_PAD_CTRL_CIRCLE; break;  // O: back, and out of a video
   case CELL_PAD_BTN_CODE_BD_TRIANGLE:
   case CELL_PAD_BTN_CODE_BD_SUBTITLE:   *digital2 = CELL_PAD_CTRL_TRIANGLE; break; // subtitles in a video
   case CELL_PAD_BTN_CODE_BD_SQUARE:     *digital2 = CELL_PAD_CTRL_SQUARE; break;
   case CELL_PAD_BTN_CODE_BD_L1:
   case CELL_PAD_BTN_CODE_BD_PREV:       *digital2 = CELL_PAD_CTRL_L1;    break;
   case CELL_PAD_BTN_CODE_BD_R1:
   case CELL_PAD_BTN_CODE_BD_NEXT:       *digital2 = CELL_PAD_CTRL_R1;    break;
   case CELL_PAD_BTN_CODE_BD_L2:         *digital2 = CELL_PAD_CTRL_L2;    break;
   case CELL_PAD_BTN_CODE_BD_R2:         *digital2 = CELL_PAD_CTRL_R2;    break;
   default: break;   // CELL_PAD_BTN_CODE_BD_RELEASE and the keys the app has no use for: nothing held
   }
}

// the remote's port: from libpad's list of what is connected where
static void findBdRemote(void)
{
   CellPadInfo2 info;
   if (cellPadGetInfo2(&info) != CELL_OK) return;
   int found = -1;
   for (int port = 0; port < CELL_PAD_MAX_PORT_NUM && found < 0; port++)
      if ((info.port_status[port] & CELL_PAD_STATUS_CONNECTED) && info.device_type[port] == CELL_PAD_DEV_TYPE_BD_REMOCON)
         found = port;
   if (found != bdPort) { bdPort = found; bdDigital1 = bdDigital2 = 0; }
}

// the key the remote holds now (its data is read on its port alone: a second read of the port would find it taken)
static void pollBdRemote(void)
{
   if (bdPolls++ % BD_SCAN_POLLS == 0) findBdRemote();
   if (bdPort < 0) return;
   uint32_t type = 0;
   CellPadData data;
   if (cellPadGetDataExtra((uint32_t)bdPort, &type, &data) != CELL_OK || type != CELL_PAD_DEV_TYPE_BD_REMOCON) {
      bdPort = -1;
      bdDigital1 = bdDigital2 = 0;
      return;
   }
   if (data.len > 0) bdKeyToBits(data.button[CELL_PAD_BTN_OFFSET_BD_CODE], &bdDigital1, &bdDigital2);
}

static PadButtonState getState(int held, int wasHeld)
{
   if (held && !wasHeld) return PAD_BUTTON_STATE_PRESSED;
   if (held && wasHeld) return PAD_BUTTON_STATE_HELD;
   if (!held && wasHeld) return PAD_BUTTON_STATE_RELEASED;
   return PAD_BUTTON_STATE_UP;
}

static int buttonHeld(int button, uint16_t digital1, uint16_t digital2)
{
   uint16_t word = padControlBits[button].word == 1 ? digital1 : digital2;
   return (word & padControlBits[button].bit) != 0;
}

void initPad(void)
{
   // Ask for every port, not just the one the player is holding. pollPad() still reads port 0 and only
   // port 0, so this changes nothing about input - but registering a virtual controller (see ps-button.c)
   // needs a free slot, and with max_connect = 1 the real pad occupies the only one, so the registration
   // would come back CELL_PAD_ERROR_TOO_MANY_DEVICES.
   cellPadInit(CELL_PAD_MAX_PORT_NUM);
   memset(&current, 0, sizeof(current));
   memset(buttonStates, 0, sizeof(buttonStates));
   memset(&leftStick, 0, sizeof(leftStick));
   memset(&rightStick, 0, sizeof(rightStick));
   rawDigital1 = 0;
   rawDigital2 = 0;
   prev1 = 0;
   prev2 = 0;
   bdPort = -1;
   bdPolls = 0;
   bdDigital1 = bdDigital2 = 0;
}

void pollPad(void)
{
   CellPadData nextPadData;
   pollBdRemote();   // (V3.0.0) port 0 too, when the remote got it: then its data is the remote's, not a pad's
   if (bdPort != 0 && cellPadGetData(0, &nextPadData) == CELL_OK && nextPadData.len > 0) current = nextPadData;

   rawDigital1 = (uint16_t)(current.button[CELL_PAD_BTN_OFFSET_DIGITAL1] | bdDigital1);
   rawDigital2 = (uint16_t)(current.button[CELL_PAD_BTN_OFFSET_DIGITAL2] | bdDigital2);

   // sticks carry no edges, so publish them straight from the poll. SDK reports unsigned 0..255,
   // centered at 128; shift to signed -128..127.
   leftStick.x = current.button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X] - 128;
   leftStick.y = current.button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y] - 128;
   rightStick.x = current.button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X] - 128;
   rightStick.y = current.button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y] - 128;
}

void updatePadEdges(void)
{
   uint16_t digital1 = rawDigital1, digital2 = rawDigital2;
   for (int button = 0; button < PAD_BUTTON_COUNT; button++)
      buttonStates[button] = getState(buttonHeld(button, digital1, digital2), buttonHeld(button, prev1, prev2));
   prev1 = digital1;
   prev2 = digital2;
}

void updatePad(void)
{
   pollPad();
   updatePadEdges();
}

unsigned getPadDownButtons(void)
{
   uint16_t digital1 = rawDigital1, digital2 = rawDigital2;
   unsigned mask = 0;
   for (int button = 0; button < PAD_BUTTON_COUNT; button++)
      if (buttonHeld(button, digital1, digital2)) mask |= 1u << button;
   return mask;
}

PadButtonState getPadButtonState(PadButton button)
{
   return buttonStates[button];
}

int isPadButtonPressed(PadButton button)
{
   return getPadButtonState(button) == PAD_BUTTON_STATE_PRESSED;
}

int isPadButtonHeld(PadButton button)
{
   return getPadButtonState(button) == PAD_BUTTON_STATE_HELD;
}

int isPadButtonDown(PadButton button)
{
   PadButtonState state = getPadButtonState(button);
   return state == PAD_BUTTON_STATE_PRESSED || state == PAD_BUTTON_STATE_HELD;
}

int isPadButtonReleased(PadButton button)
{
   return getPadButtonState(button) == PAD_BUTTON_STATE_RELEASED;
}

Stick getPadLeftStick(void)
{
   return leftStick;
}

Stick getPadRightStick(void)
{
   return rightStick;
}
