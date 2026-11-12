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

typedef enum {
   HOME_LIST_HANDLED,     // the frame's input was the list's
   HOME_LIST_PASS,        // X, Square, Triangle, R3, L3 ... are Home's, as on a card
   HOME_LIST_CHANNEL,     // X on the details' Channel button
   HOME_LIST_SUBSCRIBE,   // X on its Subscribe button (Home subscribes, or unsubscribes)
} HomeListAction;

void initHomeList(void);
void termHomeList(void);
void openHomeList(VideoGrid *grid);
void closeHomeList(int animated);
int  isHomeListOpen(void);          // open or opening: it has the pad
int  isHomeListShown(void);         // anything of it on screen, closing included
int  isHomeListCovering(void);      // fully open: nothing of the page under it shows
int  homeListButtonsFocused(void);  // the details' buttons have the focus (LEFT is theirs, not the rail's)
int  homeListSubscribeFocused(void);
void resumeHomeList(void);          // a button held over from a closed screen waits the hold delay
HomeListAction updateHomeList(VideoGrid *grid);   // after updateVideoGrid, while the feed is ready
void reapHomeList(void);            // every frame: a resolve that finished meanwhile is kept, even with the list shut
void drawHomeList(VideoGrid *grid); // over the page, before the header and the hints
void stopHomeList(void);            // joins its resolve and picture fetches (before another screen)
