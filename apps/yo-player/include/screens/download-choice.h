#pragma once

// download-choice - the page R3 opens on a video (V3.0.0): download it as 1440p60 VP9 or 1080p60 H.264 for the
// app's own Downloads page, or as the MP4 the XMB plays. The last choice is preselected (settings.txt
// download-quality=).

#include "extractor.h"   // SearchResult

void openDownloadChoice(const SearchResult *item);
