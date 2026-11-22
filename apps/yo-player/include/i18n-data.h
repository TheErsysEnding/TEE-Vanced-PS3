#pragma once

// i18n-data - the tables tools/geni18n.py writes into src/i18n-data.c (V3.1.0). Only i18n.c reads them.

typedef enum {
   I18N_PLURAL_NONE,   // one form (Japanese, Korean, Chinese, Vietnamese, Indonesian)
   I18N_PLURAL_ONE,    // one (1) | other - English, German and most others
   I18N_PLURAL_FR,     // one (0, 1) | other - French, Brazilian Portuguese
   I18N_PLURAL_RO,     // one | few (0, 2-19, x01-x19) | other - Romanian
   I18N_PLURAL_CS,     // one | few (2-4) | other - Czech
   I18N_PLURAL_PL,     // one | few (x2-x4, not 12-14) | many - Polish
   I18N_PLURAL_RU,     // one (x1, not 11) | few (x2-x4, not 12-14) | many - Russian, Ukrainian
   I18N_PLURAL_AR,     // zero | one | two | few (x03-x10) | many (x11-x99) | other - Arabic
} I18nPlural;

typedef struct {
   const char *code;                 // "de"
   const char *name;                 // its own name for itself
   int         script;               // I18nScript
   int         plural;               // I18nPlural: the forms its plural texts list, in that order
   const unsigned char *flag;        // PNG (src/assets/flags.c), or NULL
   const unsigned int  *flagSize;
   const unsigned char *packed;      // zlib: every key's text NUL-terminated in the order of I18N_KEYS, "" where it
   unsigned int packedSize;          // has none (the English is used); a plural text's forms joined by 0x1F.
   unsigned int textSize;            // ... unpacked. English: NULL, 0, 0
   const char *const *eager;         // the I18N_EAGER texts, unpacked (NULL entries: the English); English: NULL
} I18nLang;

extern const int I18N_KEY_COUNT;
extern const char *const I18N_KEYS[];      // the English texts (a plural one by its singular form), in strcmp order
extern const char *const I18N_PLURALS[];   // per key: the English plural form, NULL for a text that is not one
extern const int I18N_EAGER_COUNT;
extern const short I18N_EAGER[];           // the keys trIn is asked for, in key order
extern const I18nLang I18N_LANGS[];        // English first
extern const int I18N_LANG_COUNT;
