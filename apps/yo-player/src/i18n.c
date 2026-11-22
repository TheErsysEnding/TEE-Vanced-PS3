// i18n (V3.1.0) - the language in use and tr() over the tables tools/geni18n.py makes (see i18n.h).

#include "i18n.h"
#include "i18n-data.h"
#include "settings.h"   // getLanguageSetting / saveLanguageSetting
#include "zip.h"        // tinfl_decompress (the vendored miniz inflater)
#include "dbg.h"

#include <stdlib.h>
#include <string.h>

static int current;          // index into I18N_LANGS (0 = English)
static int chosen;           // language= is in the settings
static unsigned revision;
static char *unpacked;       // the language in use, unpacked (NULL for English)
static const char **texts;   // I18N_KEY_COUNT pointers into it, NULL where it has no translation

// how many forms a plural text of each rule lists (I18nPlural order)
static const int PLURAL_FORMS[] = { 1, 2, 2, 3, 3, 3, 3, 6 };

// the form a language takes for n (CLDR's cardinal rules, integers only)
static int pluralIndex(int rule, long n)
{
   unsigned long v = n < 0 ? (unsigned long)(-n) : (unsigned long)n;
   unsigned long n10 = v % 10, n100 = v % 100;
   int teen = n100 >= 12 && n100 <= 14;
   switch (rule) {
   case I18N_PLURAL_ONE: return v == 1 ? 0 : 1;
   case I18N_PLURAL_FR:  return v <= 1 ? 0 : 1;
   case I18N_PLURAL_RO:  return v == 1 ? 0 : (v == 0 || (n100 >= 1 && n100 <= 19)) ? 1 : 2;
   case I18N_PLURAL_CS:  return v == 1 ? 0 : (v >= 2 && v <= 4) ? 1 : 2;
   case I18N_PLURAL_PL:  return v == 1 ? 0 : (n10 >= 2 && n10 <= 4 && !teen) ? 1 : 2;
   case I18N_PLURAL_RU:  return (n10 == 1 && n100 != 11) ? 0 : (n10 >= 2 && n10 <= 4 && !teen) ? 1 : 2;
   case I18N_PLURAL_AR:  return v == 0 ? 0 : v == 1 ? 1 : v == 2 ? 2 : (n100 >= 3 && n100 <= 10) ? 3 : n100 >= 11 ? 4 : 5;
   default:              return 0;
   }
}

// A language's texts, unpacked: one block of NUL-terminated texts in key order (a plural text's forms one after the
// other) and a pointer to each. 0 when done (English: nothing to unpack, both NULL), -1 on no memory or bad data.
static int unpack(int lang, char **outBlock, const char ***outTexts)
{
   *outBlock = NULL;
   *outTexts = NULL;
   const I18nLang *l = &I18N_LANGS[lang];
   if (!l->packed || l->textSize == 0) return 0;
   char *block = (char *)malloc(l->textSize);
   const char **ptrs = (const char **)malloc(sizeof *ptrs * (size_t)I18N_KEY_COUNT);
   tinfl_decompressor *inflater = (tinfl_decompressor *)malloc(sizeof *inflater);   // ~11 KB, only while unpacking
   if (!block || !ptrs || !inflater) {
      free(block); free(ptrs); free(inflater);
      return -1;
   }
   tinfl_init(inflater);
   size_t in = l->packedSize, out = l->textSize;
   tinfl_status status = tinfl_decompress(inflater, l->packed, &in, (mz_uint8 *)block, (mz_uint8 *)block, &out,
                                          TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
   free(inflater);
   if (status != TINFL_STATUS_DONE || out != l->textSize || block[l->textSize - 1] != 0) {
      free(block); free(ptrs);
      return -1;
   }
   const char *p = block, *end = block + l->textSize;
   for (int k = 0; k < I18N_KEY_COUNT; k++) {
      if (p >= end) { free(block); free(ptrs); return -1; }
      ptrs[k] = *p ? p : NULL;
      p += strlen(p) + 1;   // stays inside: the block ends with a NUL
   }
   for (char *c = block; c < block + l->textSize; c++)   // the forms of a plural text: each a string of its own
      if (*c == 0x1F) *c = 0;
   *outBlock = block;
   *outTexts = ptrs;
   return 0;
}

static int use(int lang)
{
   char *block;
   const char **ptrs;
   if (unpack(lang, &block, &ptrs) != 0) return -1;
   free(unpacked);
   free((void *)texts);
   unpacked = block;
   texts = ptrs;
   current = lang;
   return 0;
}

void initI18n(void)
{
   char code[16];
   chosen = getLanguageSetting(code, sizeof code);
   int want = 0;
   if (chosen) {
      for (int i = 0; i < I18N_LANG_COUNT; i++)
         if (strcmp(I18N_LANGS[i].code, code) == 0) want = i;
      if (want == 0 && strcmp(code, "en") != 0) logWarn("[i18n] language=%s is none of the app's, using English\n", code);
   }
   if (want && use(want) != 0) {
      logError("[i18n] %s could not be unpacked (memory?), using English\n", I18N_LANGS[want].code);
      want = 0;
   }
   current = want;
   logInfo("[i18n] %d languages, %d texts; in use: %s%s\n", I18N_LANG_COUNT, I18N_KEY_COUNT, I18N_LANGS[current].code,
           chosen ? "" : " (not chosen yet: the first start asks)");
}

int  i18nCount(void) { return I18N_LANG_COUNT; }
const char *i18nCode(int i) { return i >= 0 && i < I18N_LANG_COUNT ? I18N_LANGS[i].code : "en"; }
const char *i18nName(int i) { return i >= 0 && i < I18N_LANG_COUNT ? I18N_LANGS[i].name : "English"; }
I18nScript i18nScriptOf(int i) { return i >= 0 && i < I18N_LANG_COUNT ? (I18nScript)I18N_LANGS[i].script : I18N_LATIN; }
int  i18nIndex(void) { return current; }
I18nScript i18nScript(void) { return i18nScriptOf(current); }
int  i18nWasChosen(void) { return chosen; }
unsigned i18nRevision(void) { return revision; }

int setI18nIndex(int i)
{
   if (i < 0 || i >= I18N_LANG_COUNT) return -1;
   int changed = i != current || !chosen;   // before use(), which makes it current
   if (i != current && use(i) != 0) {
      logError("[i18n] %s could not be unpacked (memory?): the language stays %s\n", I18N_LANGS[i].code,
               I18N_LANGS[current].code);
      return -1;
   }
   if (changed) revision++;
   current = i;
   chosen = 1;
   int rc = saveLanguageSetting(I18N_LANGS[i].code);
   logInfo("[i18n] language: %s (%s)\n", I18N_LANGS[i].code, rc == 0 ? "saved" : "NOT saved");
   return rc;
}

const uint8_t *i18nFlag(int i, unsigned int *size)
{
   if (i < 0 || i >= I18N_LANG_COUNT || !I18N_LANGS[i].flag) { *size = 0; return NULL; }
   *size = *I18N_LANGS[i].flagSize;
   return I18N_LANGS[i].flag;
}

int i18nCountMissing(int i, int (*missing)(const char *text))
{
   if (i < 0 || i >= I18N_LANG_COUNT || !missing) return 0;
   int count = missing(I18N_LANGS[i].name);
   char *block = unpacked;
   const char **ptrs = texts;
   int own = 0;
   if (i != current) {
      if (unpack(i, &block, &ptrs) != 0) return 0;   // not known: offered (the log of unpack's failure is the caller's)
      own = 1;
   }
   if (block) {   // every text and every form: the block is them one after the other
      for (const char *p = block; p < block + I18N_LANGS[i].textSize; p += strlen(p) + 1)
         if (*p) count += missing(p);
   }
   if (own) { free(block); free((void *)ptrs); }
   return count;
}

// the key's place in the sorted English texts, -1 if it is none of them
static int keyIndex(const char *english)
{
   int lo = 0, hi = I18N_KEY_COUNT - 1;
   while (lo <= hi) {
      int mid = (lo + hi) / 2;
      int c = strcmp(english, I18N_KEYS[mid]);
      if (c == 0) return mid;
      if (c < 0) hi = mid - 1; else lo = mid + 1;
   }
   return -1;
}

const char *tr(const char *english)
{
   if (!english) return "";
   if (!texts) return english;
   int k = keyIndex(english);
   if (k < 0 || !texts[k] || I18N_PLURALS[k]) return english;   // a plural text goes through trn
   return texts[k];
}

const char *trIn(int lang, const char *english)
{
   if (!english) return "";
   if (lang == current) return tr(english);
   if (lang <= 0 || lang >= I18N_LANG_COUNT || !I18N_LANGS[lang].eager) return english;
   int k = keyIndex(english);
   int lo = 0, hi = I18N_EAGER_COUNT - 1;
   while (k >= 0 && lo <= hi) {
      int mid = (lo + hi) / 2;
      if (I18N_EAGER[mid] == k) return I18N_LANGS[lang].eager[mid] ? I18N_LANGS[lang].eager[mid] : english;
      if (I18N_EAGER[mid] < k) lo = mid + 1; else hi = mid - 1;
   }
   return english;
}

const char *trn(long n, const char *one, const char *other)
{
   if (!one || !other) return "";
   int k = texts ? keyIndex(one) : -1;
   if (k < 0 || !texts[k] || !I18N_PLURALS[k]) return n == 1 || n == -1 ? one : other;
   int rule = I18N_LANGS[current].plural;
   int form = pluralIndex(rule, n), forms = rule >= 0 && rule < (int)(sizeof PLURAL_FORMS / sizeof *PLURAL_FORMS) ? PLURAL_FORMS[rule] : 1;
   if (form >= forms) form = forms - 1;
   const char *p = texts[k];
   for (int f = 0; f < form; f++) p += strlen(p) + 1;   // the generator writes exactly `forms` of them
   return p;
}
