#pragma once
// channel-avatars - the channels' pictures as textures (V3.0.0 Test 15): before a card's meta line, in the Home
// preview, in Home's list and on its details panel. The picture's address comes from the extractor's cache
// (getChannelAvatarUrl: the feeds that carry one - search, Live, channel pages, the account's channel list); this
// module fetches them one at a time on a thread of its own and keeps the last AVATAR_TEXTURES of them decoded,
// the least recently drawn one going first. UI thread only, apart from the fetch.

#include "gfx.h"   // GfxTexture

// the picture of channelId once it is here - NULL until then, and asking for it: whatever was asked for last is
// fetched first, so what is on screen now comes before what scrolled away. NULL for good when the fetch failed.
const GfxTexture *getChannelAvatarTexture(const char *channelId);

// 1 when a picture of the channel is known (getChannelAvatarUrl), so one can come: a card leaves room for it
int  hasChannelAvatar(const char *channelId);

void updateChannelAvatars(void);   // UI thread, every frame: adopts a fetched picture and starts the next fetch
void stopChannelAvatars(void);     // joins a fetch in flight (before playback: no fetch overlaps the stream)
void termChannelAvatars(void);     // ... and frees every texture
