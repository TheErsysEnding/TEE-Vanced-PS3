#pragma once

// ui-clock - the time of day at the top right of the list pages and Settings (Ember, Bild 3 and 4): the
// console's local time in Manrope 28/700, textPrimary, in the 12- or 24-hour form the XMB is set to.
//
// The digits are pre-rendered once (UiDigits, ui-draw.h) and every digit steps by the same width, so the
// clock neither rasterises anything while it is drawn nor shifts sideways as the minutes change. The time is
// read from the RTC at most twice a second, whatever the frame rate.

// Once at start, after initUiFonts (main.c does): reads the XMB's 12/24-hour setting and renders the digits.
// An unreadable setting means 24 hours.
void uiInitClock(void);

// Draws the local time with its right edge at rightX and its baseline at baselineY, e.g. "21:42", or
// "9:42 PM" on a console set to 12 hours. Nothing before uiInitClock, and nothing while the RTC cannot be
// read.
void uiDrawClock(int rightX, int baselineY);

// The widest the clock can be in the form in use ("00:00", or "00:00 AM" on 12 hours), for a screen that
// lays something out to the clock's left and must not move it as the hour gains a digit. 0 before
// uiInitClock.
int uiGetClockWidth(void);
