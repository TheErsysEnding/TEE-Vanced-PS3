#pragma once

// account page - the signed-in account's own corner, the rail's "Account" entry (signed out it is "Login", the
// sign-in: account-screen.h). Its lists in a fixed order - For you, Subscriptions, All channels A-Z, Watch Later,
// History, Liked videos, Playlists - and Sign out at the foot. A list opens on Home (showHomeSection, home.h),
// where the menu's own entries show the account's lists too. "All channels" is a page of its own: every channel
// the account follows, A to Z, X opening the channel and Triangle unsubscribing.

void openAccountPage(void);
