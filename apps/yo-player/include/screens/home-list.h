#pragma once
// home-list - Home's list view (V3.0.0 Test 15, TEE's design). DOWN on a page of cards opens it, eased in over a
// quarter of a second: every video of the feed from top to bottom - its picture, its title and under it the best
// quality the player plays of it ("1440p60", no codec), then its channel with the channel's picture - and on the
// right the selection's details on its own picture: title, channel, views, the quality and what else it has
// (subtitles, audio tracks, a live stream's rewind), the start of its description, and a bar with the channel's
// picture and two buttons, Channel and Subscribe. UP off the first video or O closes it again.
//
// The list keeps the grid's own selection (setGridSelection), so X, Square, Triangle, R3 and L3 do on the list
// exactly what they do on a card, and closing it leaves the cards on the video the list was at. The grid's pad is
// muted while it is open; Home keeps calling updateVideoGrid, which still fetches the pictures and the next page.

#include "video-grid.h"
#include "button-repeat.h"   // ButtonRepeat

typedef enum {
   HOME_LIST_HANDLED,     // the frame's input was the list's
   HOME_LIST_PASS,        // X, Square, Triangle, R3, L3 ... are the screen's, as on a card
   HOME_LIST_CHANNEL,     // X on the details' Channel button
   HOME_LIST_SUBSCRIBE,   // X on its Subscribe button (the screen subscribes, or unsubscribes)
   HOME_LIST_TOP,         // UP off the first video of an embedded list: the screen's control above it (Search's field)
} HomeListAction;

// (Test 16) one per screen - Home's, opened with DOWN; Search's, embedded (always open, its first row under the
// field). The labels are shared: Search, over Home, takes them over and Home's come back on its next draw.
typedef struct {
   int          listTop;             // the first row's top edge
   int          embedded;            // always open, nothing eased; UP off the first row is HOME_LIST_TOP
   int          target;              // 1 open (or opening), 0 closed (or closing)
   float        animFrom;
   uint64_t     animSinceUs;
   int          top;                 // the first row on screen
   int          focusButtons, button;   // the details' buttons have the focus; which one (0 Channel, 1 Subscribe)
   int          focusHidden;         // the screen's own control has the focus: the selected row looks like the rest
   int          lastSelected;
   uint64_t     selectedSinceUs;
   ButtonRepeat verticalRepeat, pageRepeat;
   int          skipInput;           // the frame that opened it: its DOWN must not move the selection too
   uint64_t     lastUpdateUs;        // its last updateHomeList: after a gap (the field, the rail, a notice had the pad)
                                     // a button still held waits the hold delay, as on a fresh screen
} HomeList;

void initHomeList(HomeList *list, int listTop, int embedded);   // listTop 0: Home's
void termHomeList(HomeList *list);
void openHomeList(HomeList *list, VideoGrid *grid);
void closeHomeList(HomeList *list, int animated);
int  isHomeListOpen(HomeList *list);          // open or opening: it has the pad
int  isHomeListShown(HomeList *list);         // anything of it on screen, closing included
int  isHomeListCovering(HomeList *list);      // fully open: nothing of the page under it shows
int  homeListButtonsFocused(HomeList *list);  // the details' buttons have the focus (LEFT is theirs, not the rail's)
int  homeListSubscribeFocused(HomeList *list);
void setHomeListFocusHidden(HomeList *list, int hidden);
void resumeHomeList(HomeList *list);          // a button held over from a closed screen waits the hold delay
HomeListAction updateHomeList(HomeList *list, VideoGrid *grid);   // after updateVideoGrid, while the feed is ready
void reapHomeList(HomeList *list);            // every frame: a resolve that finished meanwhile is kept, even shut
void drawHomeList(HomeList *list, VideoGrid *grid);   // over the page, before the header and the hints
void stopHomeList(void);                      // joins the resolve and picture fetches (before another screen)

// (Test 16) X on Subscribe: the channel of `item` followed or not - the account's while signed in (the button flips at
// once, YouTube is told on a worker), else the console's list. 1 with a message for the screen's notice when the
// console's list could not take it.
int  toggleHomeListSubscription(const SearchResult *item, char *message, int cap);
