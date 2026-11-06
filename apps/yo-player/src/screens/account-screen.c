// account screen - implementation (see account-screen.h). Laid out like the other Ember pages: the collapsed rail
// marking Settings, the title on the header band with a note under it, one panel below - the QR code on its left,
// the three steps on its right, the code itself large enough to read from the sofa - and the hints at the bottom.

#include "screens/account-screen.h"
#include "account.h"
#include "qr-device.h"
#include "sidebar.h"            // drawSidebarPassive: the rail marks Settings, the page this screen belongs to
#include "ui-clock.h"
#include "ui-hints.h"           // UiHints; brings ui-draw.h and ui-fonts.h
#include "ui/console-glyphs.h"

#include "gfx.h"
#include "theme.h"
#include "anim.h"               // interpolateColor: the panel's hairline edge
#include "pad.h"
#include "font.h"
#include "ui/label.h"
#include "screen-manager.h"
#include "thread.h"             // the worker, and the lock its results pass under
#include "dbg.h"
#include <sys/sys_time.h>
#include <string.h>
#include <stdio.h>

#define CONTENT_X      176
#define HEADER_Y       67
#define HEADER_H       45
#define CLOCK_SIZE     28
#define CLOCK_WEIGHT   UI_WEIGHT_BOLD
#define NOTE_DY        43       // the title's baseline -> the note's, where Settings has its version line
#define BODY_Y         192
#define BOX_RADIUS     UI_RADIUS_PANEL
#define BOX_EDGE       2
#define BOX_PAD        64
#define BOX_BOTTOM_PAD 60       // the last line's baseline -> the panel's bottom edge
#define QR_SCALE       10       // 37 modules with the quiet zone -> 370 px
#define LINK_GAP       46       // the code's bottom -> the address under it
#define COLUMN_GAP     96       // the code -> the steps
#define STEP_DOT       46       // a step's number on an accent-soft circle
#define STEP_TEXT_GAP  22       // the circle -> the step's words
#define STEP_SIZE      28       // a step's words, 28/700
#define STEP_1_TO_2    150      // step 1's circle -> step 2's: room for the address under step 1
#define STEP_2_TO_3    184      // step 2's circle -> step 3's: room for the code under step 2
#define ADDRESS_SIZE   40       // google.com/device under step 1, 40/800 in the accent's text colour
#define ADDRESS_DY     96       // the step's circle top -> the address's baseline
#define CODE_SIZE      76       // the code under step 2, 76/800
#define CODE_DY        124      // the step's circle top -> the code's baseline
#define STATUS_DY      96       // step 3's circle top -> the status line's baseline
#define STATUS_SIZE    26
#define CLOSE_AFTER_US (1800ull * 1000ull)   // signed in: the screen stays this long, then closes itself
#define TEXT_TINTABLE  0xFFFFFFFFu

typedef enum { STAGE_ASKING, STAGE_SHOWING, STAGE_SIGNED_IN, STAGE_EXPIRED, STAGE_DENIED, STAGE_FAILED } Stage;

#define HINT_NEW_CODE  0
#define HINT_BACK      1

static Label   titleLabel, noteLabel, linkLabel, addressLabel, codeLabel, statusLabel;
static Label   stepLabels[3], stepNumbers[3];
static UiHints hints;
static SidebarItem openedFrom = SIDE_SETTINGS;   // the rail entry marked while this screen is up

static struct {
   int screenW, screenH;
   int titleY, noteBaseline, clockBaseline;
   int qrX, qrY, linkBaseline, stepsX, stepY[3], boxH;
   uint32_t boxEdge;
   // what the screen shows - copied from the worker's results under the lock at the start of every update
   Stage stage;
   AccountCode code;
   uint64_t signedInAtUs;
} state;

// The worker's results, written under workerLock and only while their attempt is the current one: closing the
// screen or asking for a new code moves `attempt` on, and a worker that finds itself outdated drops what it has
// and ends. It is detached, so nothing on screen ever waits for it - an ask in flight can take seconds.
static sys_lwmutex_t workerLock;
static int           workerLockReady;
static volatile unsigned attempt;
static Stage         workerStage;
static AccountCode   workerCode;

static int isCurrent(unsigned mine) { return mine == attempt; }

static void publish(unsigned mine, Stage stage, const AccountCode *code)
{
   lock(&workerLock);
   if (isCurrent(mine)) {
      if (code) workerCode = *code;
      workerStage = stage;
   }
   unlock(&workerLock);
}

static void signInWorker(uint64_t arg)
{
   unsigned mine = (unsigned)arg;
   AccountCode local;   // the worker's own copy: the screen gets one under the lock
   if (requestAccountCode(&local) != 0) { publish(mine, STAGE_FAILED, NULL); exitThread(); }
   publish(mine, STAGE_SHOWING, &local);

   uint64_t deadline = sys_time_get_system_time() + (uint64_t)local.expiresInSeconds * 1000000ull;
   int interval = local.intervalSeconds;
   while (isCurrent(mine)) {
      for (int slice = 0; slice < interval * 10 && isCurrent(mine); slice++) sleepMs(100);
      if (!isCurrent(mine)) break;
      AccountPoll answer = pollAccountCode(&local);
      if (answer == ACCOUNT_POLL_GRANTED) { publish(mine, STAGE_SIGNED_IN, NULL); break; }
      if (answer == ACCOUNT_POLL_DENIED)  { publish(mine, STAGE_DENIED, NULL); break; }
      if (answer == ACCOUNT_POLL_EXPIRED || sys_time_get_system_time() > deadline) {
         publish(mine, STAGE_EXPIRED, NULL);
         break;
      }
      if (answer == ACCOUNT_POLL_SLOW_DOWN) interval += 5;
      // PENDING, or a FAILED ask (a dropped connection, say): ask again until the code runs out
   }
   memset(&local, 0, sizeof local);
   exitThread();
}

static void startAttempt(void)
{
   lock(&workerLock);
   unsigned mine = ++attempt;
   workerStage = STAGE_ASKING;
   memset(&workerCode, 0, sizeof workerCode);
   unlock(&workerLock);
   sys_ppu_thread_t tid;
   if (spawnThread(&tid, signInWorker, (uint64_t)mine, THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB,
                   "account-signin") != 0) {
      logError("[account] the sign-in worker could not start\n");
      publish(mine, STAGE_FAILED, NULL);
   }
}

static const char *statusText(Stage stage)
{
   switch (stage) {
   case STAGE_ASKING:    return "Getting a code from Google...";
   case STAGE_SHOWING:   return "Waiting for you to sign in. This page goes on by itself.";
   case STAGE_SIGNED_IN: return "Signed in. Your lists now come from your YouTube account.";
   case STAGE_EXPIRED:   return "This code has run out. Press X for a new one.";
   case STAGE_DENIED:    return "Access was not allowed on Google's page. Press X to try again.";
   default:              return "Google could not be reached. Press X to try again.";
   }
}

static uint32_t statusColor(Stage stage)
{
   if (stage == STAGE_SIGNED_IN) return activeTheme->accentText;
   if (stage == STAGE_ASKING || stage == STAGE_SHOWING) return activeTheme->textSecondary;
   return activeTheme->textPrimary;
}

static void initAccountScreen(void)
{
   state.screenW = getGfxScreenWidth();
   state.screenH = getGfxScreenHeight();
   state.boxEdge = interpolateColor(activeTheme->surface1, uiWithAlpha(activeTheme->hairline, 0xFF),
                                    (float)(activeTheme->hairline >> 24) / 255.0f);
   if (!workerLockReady && createLock(&workerLock) == 0) workerLockReady = 1;

   uiInitLabel(&titleLabel, UI_PAGE_TITLE_WEIGHT, CONTENT_X, 0, AUTO, UI_PAGE_TITLE_SIZE, activeTheme->textPrimary,
               TEXT_NOWRAP, "Sign in to YouTube");
   uiInitLabel(&noteLabel, UI_BODY_WEIGHT, CONTENT_X, 0, UI_SAFE_RIGHT - CONTENT_X, UI_BODY_SIZE,
               activeTheme->textSecondary, TEXT_WRAP,
               "Your subscriptions, Watch Later, history and recommendations then come from your YouTube account. "
               "Your password is only ever typed on Google's own page.");
   uiInitTextLabel(&linkLabel, UI_WEIGHT_BOLD, 0, 0, AUTO, UI_BODY_SIZE, activeTheme->textSecondary, TEXT_NOWRAP,
                   "Scan with your phone");
   static const char *STEPS[3] = { "On your phone or computer, open", "Enter this code",
                                   "Choose your Google account and allow access" };
   for (int i = 0; i < 3; i++) {
      char number[4];
      snprintf(number, sizeof number, "%d", i + 1);
      uiInitLabel(&stepNumbers[i], UI_WEIGHT_EXTRABOLD, 0, 0, AUTO, 24, activeTheme->accentText, TEXT_NOWRAP, number);
      uiInitLabel(&stepLabels[i], UI_WEIGHT_BOLD, 0, 0, AUTO, STEP_SIZE, activeTheme->textPrimary, TEXT_NOWRAP, STEPS[i]);
   }
   uiInitTextLabel(&addressLabel, UI_WEIGHT_EXTRABOLD, 0, 0, AUTO, ADDRESS_SIZE, activeTheme->accentText, TEXT_NOWRAP,
                   QR_DEVICE_SHOWN);
   uiInitTextLabel(&codeLabel, UI_WEIGHT_EXTRABOLD, 0, 0, AUTO, CODE_SIZE, activeTheme->textPrimary, TEXT_NOWRAP, "...");
   uiInitTextLabel(&statusLabel, UI_WEIGHT_SEMIBOLD, 0, 0, AUTO, STATUS_SIZE, TEXT_TINTABLE, TEXT_NOWRAP,
                   statusText(STAGE_ASKING));

   uiInitHints(&hints);
   uiAddHint(&hints, GLYPH_CROSS,  "New code");   // HINT_NEW_CODE: only once a code has run out or failed
   uiAddHint(&hints, GLYPH_CIRCLE, "Back");       // HINT_BACK

   state.titleY       = uiLabelYForCenter(&titleLabel, HEADER_Y, HEADER_H);
   state.noteBaseline = state.titleY + uiGetTextMetrics(titleLabel.font, titleLabel.size).baseline + NOTE_DY;
   Font *clockFont     = uiGetFont(CLOCK_WEIGHT);
   state.clockBaseline = uiTextYForCenter(clockFont, CLOCK_SIZE, HEADER_Y, HEADER_H)
                         + uiGetTextMetrics(clockFont, CLOCK_SIZE).baseline;
   UiTextMetrics note = uiGetTextMetrics(noteLabel.font, noteLabel.size);
   int noteLast = state.noteBaseline + (noteLabel.tt.tex.h > note.texH ? noteLabel.tt.tex.h - note.texH : 0);
   int top = noteLast + 48 > BODY_Y ? noteLast + 48 : BODY_Y;

   int qrSpan = (QR_DEVICE_SIZE + 2 * QR_QUIET_MODULES) * QR_SCALE;
   state.qrX = CONTENT_X + BOX_PAD;
   state.qrY = top + BOX_PAD;
   state.linkBaseline = state.qrY + qrSpan + LINK_GAP;
   state.stepsX = state.qrX + qrSpan + COLUMN_GAP;
   state.stepY[0] = state.qrY;
   state.stepY[1] = state.stepY[0] + STEP_1_TO_2;
   state.stepY[2] = state.stepY[1] + STEP_2_TO_3;
   int lastBaseline = state.stepY[2] + STATUS_DY;
   if (state.linkBaseline > lastBaseline) lastBaseline = state.linkBaseline;
   state.boxH = lastBaseline + BOX_BOTTOM_PAD - top;

   state.stage = STAGE_ASKING;
   memset(&state.code, 0, sizeof state.code);
   state.signedInAtUs = 0;
   startAttempt();
   uiLogVram("account screen");
}

static void closeScreen(void)
{
   lock(&workerLock);
   attempt++;   // whatever the worker still has in hand is dropped
   unlock(&workerLock);
   popScreen();
}

static void updateAccountScreen(void)
{
   lock(&workerLock);
   Stage stage = workerStage;
   if (stage == STAGE_SHOWING && strcmp(state.code.userCode, workerCode.userCode) != 0) state.code = workerCode;
   unlock(&workerLock);

   if (stage != state.stage) {
      state.stage = stage;
      setLabelText(&statusLabel, statusText(stage));
      if (stage == STAGE_SHOWING) setLabelText(&codeLabel, state.code.userCode);
      if (stage == STAGE_ASKING) setLabelText(&codeLabel, "...");
      if (stage == STAGE_SIGNED_IN) state.signedInAtUs = sys_time_get_system_time();
      uiSetHintShown(&hints, HINT_NEW_CODE, stage == STAGE_EXPIRED || stage == STAGE_DENIED || stage == STAGE_FAILED);
      uiSetHintCaption(&hints, HINT_BACK, stage == STAGE_SIGNED_IN ? "Done" : "Back");
   }

   if (isPadButtonPressed(PAD_BTN_CIRCLE)) { closeScreen(); return; }
   if (stage == STAGE_SIGNED_IN) {
      if (isPadButtonPressed(PAD_BTN_CROSS) || sys_time_get_system_time() - state.signedInAtUs > CLOSE_AFTER_US)
         closeScreen();
      return;
   }
   if ((stage == STAGE_EXPIRED || stage == STAGE_DENIED || stage == STAGE_FAILED) && isPadButtonPressed(PAD_BTN_CROSS))
      startAttempt();
}

static void drawTinted(const Label *label, int x, int y, uint32_t color)
{
   if (label->tt.tex.w <= 0) return;
   drawGfxTexture(x, y, label->tt.tex.w, label->tt.tex.h, label->tt.tex, 0.0f, 0.0f, 1.0f, 1.0f, color,
                  GFX_FILTER_NEAREST);
}

// a step: its number on an accent-soft circle, the words centred on the circle beside it
static void drawStep(int i)
{
   int x = state.stepsX, y = state.stepY[i];
   uiFillCircle(x, y, STEP_DOT, activeTheme->accentSoft);
   uiDrawLabelAt(&stepNumbers[i], x + (STEP_DOT - stepNumbers[i].tt.tex.w) / 2, uiLabelYForCenter(&stepNumbers[i], y, STEP_DOT));
   uiDrawLabelAt(&stepLabels[i], x + STEP_DOT + STEP_TEXT_GAP, uiLabelYForCenter(&stepLabels[i], y, STEP_DOT));
}

static void drawAccountScreen(void)
{
   fillGfxRectangle(0, 0, state.screenW, state.screenH, activeTheme->appBg);
   drawSidebarPassive(state.screenH, openedFrom);

   uiDrawLabelAt(&titleLabel, CONTENT_X, state.titleY);
   uiDrawLabelAt(&noteLabel, CONTENT_X, uiLabelYForBaseline(&noteLabel, state.noteBaseline));
   uiDrawClock(UI_SAFE_RIGHT, state.clockBaseline);

   int top = state.qrY - BOX_PAD, width = UI_SAFE_RIGHT - CONTENT_X;
   uiFillRounded(CONTENT_X, top, width, state.boxH, BOX_RADIUS, state.boxEdge);
   uiFillRounded(CONTENT_X + BOX_EDGE, top + BOX_EDGE, width - 2 * BOX_EDGE, state.boxH - 2 * BOX_EDGE,
                 BOX_RADIUS - BOX_EDGE, activeTheme->surface1);

   drawQrDevice(state.qrX, state.qrY, QR_SCALE);
   int qrSpan = (QR_DEVICE_SIZE + 2 * QR_QUIET_MODULES) * QR_SCALE;
   uiDrawLabelAt(&linkLabel, state.qrX + (qrSpan - linkLabel.tt.tex.w) / 2, uiLabelYForBaseline(&linkLabel, state.linkBaseline));

   int wordsX = state.stepsX + STEP_DOT + STEP_TEXT_GAP;
   for (int i = 0; i < 3; i++) drawStep(i);
   uiDrawLabelAt(&addressLabel, wordsX, uiLabelYForBaseline(&addressLabel, state.stepY[0] + ADDRESS_DY));
   uiDrawLabelAt(&codeLabel, wordsX, uiLabelYForBaseline(&codeLabel, state.stepY[1] + CODE_DY));
   drawTinted(&statusLabel, wordsX, uiLabelYForBaseline(&statusLabel, state.stepY[2] + STATUS_DY), statusColor(state.stage));

   uiDrawHints(&hints, UI_SAFE_RIGHT, UI_HINTS_BOTTOM_Y, UI_ALIGN_RIGHT);
}

static void termAccountScreen(void)
{
   lock(&workerLock);
   attempt++;   // the screen can also go without closeScreen (the stack unwinding): the worker must not outlive it
   unlock(&workerLock);
   uiTermHints(&hints);
   freeLabel(&titleLabel);
   freeLabel(&noteLabel);
   freeLabel(&linkLabel);
   freeLabel(&addressLabel);
   freeLabel(&codeLabel);
   freeLabel(&statusLabel);
   for (int i = 0; i < 3; i++) { freeLabel(&stepLabels[i]); freeLabel(&stepNumbers[i]); }
   memset(&state.code, 0, sizeof state.code);
}

// Screen vtable order: init, resume, update, draw, suspend, term, status.
static Screen accountScreen = { initAccountScreen, NULL, updateAccountScreen, drawAccountScreen, NULL,
                                termAccountScreen, SCREEN_TERMINATED };

void openAccountScreen(SidebarItem from) { openedFrom = from; pushScreen(&accountScreen); }
