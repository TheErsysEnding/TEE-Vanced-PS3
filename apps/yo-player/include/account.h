#pragma once

// account - signing in to YouTube the way its TV app does, and the token the account's own lists are fetched with.
//
// The console shows a code; the viewer opens google.com/device on a phone, signs in to Google there and types the
// code; the console, asking every few seconds, then receives a token. The password never touches the console. The
// PS3's own YouTube app signed in exactly like this. The app introduces itself as YouTube's TV app to do it (the
// way microG speaks for Google's apps on Android) - YouTube answers a signed-in request only in its TV format then
// (youtube.c, the account feeds), and Google may stop accepting that one day: playback never depends on it.
//
// Stored: the refresh token, as plain text in <data>/account.txt. Whoever can read the console's disk - webMAN's
// FTP has no password unless one is set - can use the YouTube account with it (not sign in to the Google account,
// not see the password). Signing out (Settings) withdraws it at Google and deletes the file. No token, device code
// or sign-in code is ever written to the log.

#include "extractor.h"   // AccountPlaylistInfo

#define ACCOUNT_CODE_CAP 24

// YouTube's TV app as the signed-in requests present it - the sign-in itself (account.c) and the account's lists
// (youtube.c). Probed in Nov 2026: with this token YouTube answers the TVHTML5 client and refuses the WEB one.
#define ACCOUNT_TV_USER_AGENT \
   "Mozilla/5.0 (ChromiumStylePlatform) Cobalt/25.lts.30.1034943-gold (unlike Gecko), Unknown_TV_Unknown_0/Unknown (Unknown, Unknown)"
#define ACCOUNT_TV_CLIENT_VERSION "7.20250923.13.00"

// a sign-in code as Google hands it out
typedef struct {
   char userCode[ACCOUNT_CODE_CAP];   // what the viewer types: "ABC-DEF-GHIJ"
   char address[64];                  // where, as Google names it: "https://www.google.com/device"
   char deviceCode[256];              // what the console asks with: secret, never shown or logged
   int  intervalSeconds;              // how often Google wants to be asked
   int  expiresInSeconds;             // how long the code is good for
} AccountCode;

typedef enum {
   ACCOUNT_POLL_PENDING,     // not entered yet: ask again after the interval
   ACCOUNT_POLL_SLOW_DOWN,   // asked too often: wait longer before the next ask
   ACCOUNT_POLL_GRANTED,     // signed in, and the token is kept (getAccountRevision has moved on)
   ACCOUNT_POLL_DENIED,      // the viewer refused on Google's page
   ACCOUNT_POLL_EXPIRED,     // the code ran out: a new one is needed
   ACCOUNT_POLL_FAILED,      // no answer, or none that made sense: try again
} AccountPoll;

void initAccount(void);            // at startup, after migrateAppData: reads account.txt
int  isAccountSignedIn(void);
unsigned getAccountRevision(void); // +1 on every sign-in and sign-out, so the account's lists can drop their copies

// These block on the network: call them from a worker thread, never from a screen's update.
int  requestAccountCode(AccountCode *out);              // 0 ok, negative on failure
AccountPoll pollAccountCode(const AccountCode *code);   // one ask; GRANTED stores the token
// The Authorization header's value for a signed-in request ("Bearer ..."), renewing the short-lived access token
// when it is due - one renewal for every caller at once. 0 ok; -1 when not signed in, or when the renewal failed
// (a sign-in Google has withdrawn - removed in the Google account, say - signs the console out).
int  getAccountAuthorization(char *out, int cap);
// YouTube refused the access token before its time (HTTP 401): forget it, so the next getAccountAuthorization renews.
void dropAccountAccess(void);

// UI thread: forgets the sign-in at once (file and memory); Google is told on a worker of its own.
void signOutAccount(void);

// ---- what the screens need to know of the account's own lists ----
//
// Which channels the account follows and which videos are in its Watch Later: loaded on a worker once per sign-in
// (the first syncAccountLists after it), and changed at once by the app's own buttons - Square, Triangle on a
// channel page - while YouTube is told on a worker. A change YouTube refuses is undone, and takeAccountFailure
// hands its message to the screen on top.

void syncAccountLists(void);                          // UI thread, every frame: starts the load when one is due
int  isAccountFollowing(const char *channelId);
int  isInAccountWatchLater(const char *videoId);
void setAccountFollowing(const char *channelId, int follow);
void setAccountWatchLater(const char *videoId, int add);
unsigned getAccountListsRevision(void);   // +1 whenever either list changes, loaded or by a button
unsigned getAccountChangesRevision(void); // +1 only when a button changed one (pages built from them reload)
int  takeAccountFailure(char *out, int cap);          // 1, with the message, when a change was refused since the last call

// The account's playlists (Liked videos and its own), loaded with the lists above. refreshAccountLists asks for all of
// them again - the Playlists page does on every opening, as a playlist may have been made on the phone meanwhile;
// what was loaded before stays in force until the new answer is in.
int  getAccountPlaylistCount(void);
int  getAccountPlaylistAt(int index, AccountPlaylistInfo *out);   // 1 when there is one at index
void refreshAccountLists(void);
