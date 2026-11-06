#pragma once

// account screen - signing in to YouTube (account.h): a QR code for google.com/device and the code to type there,
// while a worker asks Google every few seconds whether it has been entered. Signed in, the screen closes itself
// a moment later. Opened from Settings' Account card; O goes back at any time (a code entered afterwards is
// simply never asked about). X asks for a new code once one has run out or failed.

#include "sidebar.h"   // SidebarItem

// from: the rail entry the screen was opened from, marked in its collapsed rail (Settings, or For you on Home)
void openAccountScreen(SidebarItem from);
