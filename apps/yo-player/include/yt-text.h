#pragma once

// yt-text (V3.1.0) - the few phrases YouTube writes into a video's meta line, in the app's language. The app asks
// YouTube in English (hl=en: the parser goes by its words - " views", " ago", " watching"), so "1.2M views",
// "3 weeks ago", "Streamed 2 days ago", "52K watching", "4.6M subscribers" are put into the language in use here, in
// the form its plural takes for the number (i18n.h trn). The number stays as YouTube wrote it. A phrase it does not know,
// and every phrase while the app is in English, stays as it came. Returns out.
const char *localizeYouTubeText(const char *in, char *out, int cap);
