#pragma once

// i18n (V3.1.0) - the app's own texts in many languages, chosen at the first start (TEE: "beim ersten mal start der app
// [soll] eine sprache ausgewählt werden (standard englisch) und man dann viele sprachen auswählen kann und es sollen die
// jeweiligen länder flaggen angezeigt werden").
//
// Every fixed text is written in English in the code and goes through tr(): tr("Settings") is the text in the language
// in use, or the English itself where a translation is missing. The translations live in apps/yo-player/i18n/<code>.txt
// (one line a text, by the number apps/yo-player/i18n/keys.txt gives it); tools/geni18n.py makes src/i18n-data.c from
// them (checked in: the build does not need Python). A translation whose %s / %d do not match the English is left out by
// the generator - a printf with the wrong arguments would crash - and so is one longer than a label holds (255 bytes).
//
// Memory: with 4K playing the console has about 3 MB left, so the languages are kept deflated in the app and only the one
// in use is unpacked (a few tens of KB). The picker's own few texts (trIn) are kept unpacked for every language.
//
// Scripts: Latin, Cyrillic, Greek and Vietnamese are Manrope's; Japanese, Korean and Chinese the console's FONT_POP;
// Arabic DejaVu Sans Bold (embedded, src/assets). Arabic is right to left: the font renderer joins its letters and lays
// each line out right to left when it draws it (font.c). The layout stays as it is - left aligned, the menu on the left;
// only the letters of a line run right to left (TEE: "nicht das das ganze menü mit umgewandelt wird von rechts nach
// links").
//
// The language is settings.txt language=<code>; none yet means the first start, which asks (screens/language-screen.c).

#include <stdint.h>

typedef enum { I18N_LATIN, I18N_CJK, I18N_ARABIC } I18nScript;

void initI18n(void);                 // after loadSettings: reads language= and unpacks that language's texts
int  i18nCount(void);
const char *i18nCode(int index);     // "de"
const char *i18nName(int index);     // its own name for itself ("Deutsch", "日本語")
I18nScript  i18nScriptOf(int index);
int  i18nIndex(void);                // the language in use (0 = English)
I18nScript i18nScript(void);         // ... and its script
int  setI18nIndex(int index);        // in use at once and saved as language=; 0 when it was unpacked and saved
int  i18nWasChosen(void);            // language= is in the settings: not the first start
unsigned i18nRevision(void);         // +1 with every change: a screen built before redoes its labels

// the PNG of the language's flag (96x64), NULL if none
const uint8_t *i18nFlag(int index, unsigned int *size);

// how many characters of a language's texts `missing` says no font can draw: the picker leaves out a CJK language the
// console's font set falls short of. Unpacks that language for the count (main thread).
int  i18nCountMissing(int index, int (*missing)(const char *text));

// the text in the language in use; english itself when there is no translation. Never NULL.
const char *tr(const char *english);
// marks a text in a static table for the generator; tr() it where it is shown
#define N_(english) (english)
// one of the picker's texts in a given language (the picker shows its title in the language under the cursor)
const char *trIn(int index, const char *english);
// a text with a number in it, in the form the language takes for n ("1 video", "2 videos"; Russian has three forms,
// Arabic six). one and other are the English forms; the format's conversions are the caller's.
const char *trn(long n, const char *one, const char *other);
