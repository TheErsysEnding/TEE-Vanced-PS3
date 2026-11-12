#pragma once
// welcome-screen - what a new version brings, once (V3.0.0 Test 15, TEE's wording): the first start of a version shows
// its changes - with 1440p60 in large letters, and what to expect of it - and, after X, a page "Support
// TheErsysEnding:" with the link and its QR code. Seen once per version (welcome.txt in the data folder holds the
// version last welcomed), then never again until the next one.

int  isWelcomeDue(void);   // 1 when this version has not been welcomed yet
void openWelcome(void);    // pushes the two pages over the screen on show; O or X on the last page closes them
