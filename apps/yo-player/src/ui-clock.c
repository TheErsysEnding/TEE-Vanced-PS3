// ui-clock - the time of day in the Ember headers (see ui-clock.h).

#include "ui-clock.h"
#include "ui-draw.h"     // UiDigits, uiDrawLabelAt
#include "ui-fonts.h"    // uiInitLabel, uiTextYForBaseline, uiLabelYForBaseline
#include "theme.h"       // activeTheme->textPrimary
#include "dbg.h"         // logInfo
#include <cell/rtc.h>                      // cellRtcGetCurrentClockLocalTime (the RTC module: initRtc in main)
#include <sysutil/sysutil_sysparam.h>      // cellSysutilGetSystemParamInt: the XMB's time format
#include <sys/sys_time.h>                  // sys_time_get_system_time
#include <stdio.h>       // snprintf

#define CLOCK_SIZE       28                 // Ember's header clock: 28/700 in text-1 (Bild 3 and 4)
#define CLOCK_WEIGHT     UI_WEIGHT_BOLD
#define CLOCK_REREAD_US  500000             // a minute changes once a minute; twice a second is plenty

static UiDigits digits;             // "0123456789:" and the space, rendered once, tinted when drawn
static Label    meridiem[2];        // "AM" and "PM" - rendered only when the console uses 12 hours
static int      ready;
static int      twelveHour;
static char     shown[8];           // what the clock shows: "21:42", or "9:42" with meridiem[afternoon]
static int      afternoon;
static int      readOnce;
static uint64_t readAtUs;

void uiInitClock(void)
{
   if (ready) return;
   int format = CELL_SYSUTIL_TIME_FMT_CLOCK24;
   int rc = cellSysutilGetSystemParamInt(CELL_SYSUTIL_SYSTEMPARAM_ID_TIME_FORMAT, &format);
   twelveHour = (rc == 0 && format == CELL_SYSUTIL_TIME_FMT_CLOCK12);

   uiInitDigits(&digits, CLOCK_WEIGHT, CLOCK_SIZE);
   if (twelveHour) {
      uiInitLabel(&meridiem[0], CLOCK_WEIGHT, 0, 0, AUTO, CLOCK_SIZE, activeTheme->textPrimary, TEXT_NOWRAP, "AM");
      uiInitLabel(&meridiem[1], CLOCK_WEIGHT, 0, 0, AUTO, CLOCK_SIZE, activeTheme->textPrimary, TEXT_NOWRAP, "PM");
   }
   ready = 1;
   logInfo("[ui] clock: %d-hour, as the XMB is set%s\n", twelveHour ? 12 : 24,
           rc == 0 ? "" : " (setting unreadable, 24 assumed)");
}

// Reads the RTC when the last reading is half a second old. A failed reading keeps whatever was shown
// before; before the first good one there is nothing to show.
static void readTime(void)
{
   uint64_t now = sys_time_get_system_time();
   if (readOnce && now - readAtUs < CLOCK_REREAD_US) return;
   readOnce = 1;
   readAtUs = now;

   CellRtcDateTime time;
   if (cellRtcGetCurrentClockLocalTime(&time) != 0) return;
   int hour = (int)time.hour % 24, minute = (int)time.minute % 60;
   if (twelveHour) {
      afternoon = hour >= 12;
      hour %= 12;
      if (hour == 0) hour = 12;   // 12-hour clocks count 12, 1, ... 11
      snprintf(shown, sizeof shown, "%d:%02d", hour, minute);
   } else {
      snprintf(shown, sizeof shown, "%02d:%02d", hour, minute);
   }
}

static int meridiemWidth(void)
{
   int wider = meridiem[0].tt.tex.w > meridiem[1].tt.tex.w ? meridiem[0].tt.tex.w : meridiem[1].tt.tex.w;
   return wider > 0 ? uiGetDigitsWidth(&digits, " ") + wider : 0;
}

void uiDrawClock(int rightX, int baselineY)
{
   if (!ready) return;
   readTime();
   if (!shown[0]) return;

   const Label *suffix = twelveHour ? &meridiem[afternoon] : NULL;
   int spaceW  = uiGetDigitsWidth(&digits, " ");
   int suffixW = (suffix && suffix->tt.tex.w > 0) ? spaceW + suffix->tt.tex.w : 0;
   int timeW   = uiGetDigitsWidth(&digits, shown);
   int x = rightX - suffixW - timeW;

   uiDrawDigits(&digits, x, uiTextYForBaseline(digits.font, digits.size, baselineY), shown, activeTheme->textPrimary);
   if (suffixW > 0) uiDrawLabelAt(suffix, x + timeW + spaceW, uiLabelYForBaseline(suffix, baselineY));
}

int uiGetClockWidth(void)
{
   if (!ready) return 0;
   // every digit takes the same cell, so "00:00" is as wide as any time; 12 hours adds the wider of AM/PM
   return uiGetDigitsWidth(&digits, "00:00") + (twelveHour ? meridiemWidth() : 0);
}
