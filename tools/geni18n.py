# geni18n - the app's translations into apps/yo-player/src/i18n-data.c (V3.1.0, see apps/yo-player/include/i18n.h).
#
#   python -I tools/geni18n.py            (from anywhere; prints what it found and what it left out)
#
# The English texts are taken from the code: every tr("..."), N_("..."), trIn(x, "...") and trn(n, "one", "other")
# (adjacent literals are joined, as the compiler joins them). Each gets a number in apps/yo-player/i18n/keys.txt that
# stays the same for good (a new text gets the next one; a text gone from the code keeps its number and is just not
# used), so a translation file is "number<TAB>text" - and a changed English text is a new number, never a stale
# translation. A plural text (trn) is written "one || other" there.
#
# A language is apps/yo-player/i18n/<code>.txt:
#   @name Deutsch            its own name for itself
#   @script latin            latin (Manrope: Latin, Cyrillic, Greek, Vietnamese), cjk (FONT_POP) or arabic (DejaVu)
#   @flag de                 the flag in src/assets/flags.c (tools/make-flags.py)
#   @order 2                 its place in the picker
#   @plural one              the forms its plural texts list: none (1 form), one / fr (2), ro / cs / pl / ru (3), ar (6)
#   17<TAB>Einstellungen
#   42<TAB>%d Video || %d Videos
# A translation must have the English text's printf conversions (%s, %d, %02d ...) in the same order, every form of a
# plural one too, else it is left out (the English is shown): a printf given the wrong arguments would crash the app.
# A text longer than a label holds (255 bytes of UTF-8) is left out too. Arabic stays in reading order here: the font
# renderer joins its letters and lays each line out right to left when it draws it (font.c).
#
# Output: every language but English deflated (zlib) - only the one in use is unpacked on the console, where 4K playback
# leaves about 3 MB - and the texts trIn asks for (the language picker's) unpacked for every language.
import os, re, sys, zlib

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
APP = os.path.join(ROOT, 'apps', 'yo-player')
SRC = os.path.join(APP, 'src')
I18N = os.path.join(APP, 'i18n')
OUT = os.path.join(SRC, 'i18n-data.c')
LABEL_BYTES = 255    # what a Label holds: a longer text must be shown with uiInitLongLabel / uiSetLongTextLabel
MAX_BYTES = 1000     # past this a text is left out
LONG_OK = set()      # (filled in main) the English texts the code shows with a long label

LITS = r'((?:"(?:[^"\\\n]|\\.)*"\s*)+)'
ONE_LIT = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
CALL_TR = re.compile(r'\b(?:tr|N_)\(\s*' + LITS + r'\)')
CALL_TRIN = re.compile(r'\btrIn\(\s*[^,()]+,\s*' + LITS + r'\)')
CALL_TRN = re.compile(r'\btrn\(\s*[^,]+?,\s*' + LITS + r',\s*' + LITS + r'\)')
CONV = re.compile(r'%[-+ 0#]*\d*(?:\.\d+)?(?:l|ll|h|z)?[sdiuxXcfp]')
PLURALS = {'none': (0, 1), 'one': (1, 2), 'fr': (2, 2), 'ro': (3, 3), 'cs': (4, 3), 'pl': (5, 3), 'ru': (6, 3), 'ar': (7, 6)}
SEP = ' || '


def convs(text):   # a text's printf conversions; '%%' is a percent sign, none
    return CONV.findall(text.replace('%%', ''))


def c_unescape(s):
    out = bytearray()
    i = 0
    while i < len(s):
        ch = s[i]
        if ch != '\\':
            out += ch.encode('utf-8'); i += 1; continue
        n = s[i + 1]
        if n == 'x':
            j = i + 2
            while j < len(s) and j < i + 4 and s[j] in '0123456789abcdefABCDEF': j += 1
            out.append(int(s[i + 2:j], 16)); i = j
        elif n in 'nt"\\\'':
            out += {'n': b'\n', 't': b'\t', '"': b'"', '\\': b'\\', "'": b"'"}[n]; i += 2
        else:
            raise ValueError('escape \\%s in %r' % (n, s))
    return out.decode('utf-8')


def joined(lits):
    return ''.join(c_unescape(m.group(1)) for m in ONE_LIT.finditer(lits))


def c_literal(text):
    # every byte above ASCII as \xNN; a hex digit right after one starts a new literal ("\xB7" "a")
    out, prev_hex = '"', False
    for b in text.encode('utf-8'):
        c = chr(b)
        if b >= 0x80:
            out += '\\x%02X' % b; prev_hex = True; continue
        if prev_hex and c in '0123456789abcdefABCDEF':
            out += '" "'
        prev_hex = False
        if c == '"': out += '\\"'
        elif c == '\\': out += '\\\\'
        elif c == '\n': out += '\\n'
        else: out += c
    return out + '"'


def source_keys():
    plain, plural, eager, where = set(), {}, set(), {}
    for dirpath, _, files in os.walk(SRC):
        for f in files:
            if not f.endswith('.c') or f == 'i18n-data.c': continue
            text = open(os.path.join(dirpath, f), encoding='utf-8').read()
            text = re.sub(r'//[^\n]*', lambda m: ' ' * len(m.group(0)) if '"' not in m.group(0) else m.group(0), text)
            for m in CALL_TR.finditer(text):
                k = joined(m.group(1)); plain.add(k); where.setdefault(k, f)
            for m in CALL_TRIN.finditer(text):
                k = joined(m.group(1)); plain.add(k); eager.add(k); where.setdefault(k, f)
            for m in CALL_TRN.finditer(text):
                one, other = joined(m.group(1)), joined(m.group(2))
                if one in plural and plural[one] != other:
                    sys.exit('trn("%s") has two plural forms: "%s" and "%s"' % (one, plural[one], other))
                plural[one] = other; where.setdefault(one, f)
    both = plain & set(plural)
    if both:
        sys.exit('used both by tr() and by trn(): %s' % ', '.join(sorted(both)))
    return plain, plural, eager, where


def key_text(k, plural):
    return k + SEP + plural[k] if k in plural else k


def read_ids():
    ids = {}
    path = os.path.join(I18N, 'keys.txt')
    if os.path.exists(path):
        for line in open(path, encoding='utf-8'):
            line = line.rstrip('\n')
            if not line or line.startswith('#'): continue
            parts = line.split('\t')
            ids[parts[1].replace('\\n', '\n')] = int(parts[0])
    return ids


def write_ids(ids, used):
    path = os.path.join(I18N, 'keys.txt')
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('# TEE Vanced: the English texts of the app and their numbers (tools/geni18n.py keeps this file).\n')
        f.write('# A number stays for good; a translation file says "number<TAB>text". A plural text is "one || other".\n')
        f.write('# * = no longer in the code.\n')
        for text, n in sorted(ids.items(), key=lambda kv: kv[1]):
            f.write('%d\t%s%s\n' % (n, text.replace('\n', '\\n'), '' if text in used else '\t*'))


def read_language(path, by_number, keys, plural, report):
    meta, texts = {}, {}
    code = os.path.splitext(os.path.basename(path))[0]
    for ln, line in enumerate(open(path, encoding='utf-8-sig'), 1):
        line = line.rstrip('\n')
        if not line.strip() or line.startswith('#'): continue
        if line.startswith('@'):
            k, _, v = line[1:].partition(' ')
            meta[k] = v.strip()
            continue
        n, sep, text = line.partition('\t')
        if not sep or not n.strip().isdigit():
            report('%s:%d: not "number<TAB>text"' % (code, ln)); continue
        keytext = by_number.get(int(n))
        if keytext is None:
            report('%s:%d: no text has number %s' % (code, ln, n)); continue
        english = keytext.split(SEP)[0] if keytext.split(SEP)[0] in plural else keytext
        if english not in keys: continue   # a text no longer in the code
        text = text.replace('\\n', '\n')
        if english in plural:
            forms = text.split(SEP)
            want = PLURALS.get(meta.get('plural', 'one'), (1, 2))[1]
            if len(forms) != want:
                report('%s:%d: %d forms, the language takes %d - left out' % (code, ln, len(forms), want)); continue
            bad = [f for f in forms if convs(f) != convs(english)]
            if bad:
                report('%s:%d: a form\'s conversions do not match the English %s - left out' % (code, ln, convs(english)))
                continue
            long_ = [f for f in forms if len(f.encode('utf-8')) > MAX_BYTES]
            if long_:
                report('%s:%d: a form is %d bytes - left out' % (code, ln, len(long_[0].encode('utf-8'))))
                continue
            if any(len(f.encode('utf-8')) > LABEL_BYTES for f in forms) and english not in LONG_OK:
                report('%s:%d: a form passes 255 bytes and the code shows it in a plain label' % (code, ln))
            texts[english] = forms
        else:
            if convs(text) != convs(english):
                report('%s:%d: %s does not match the English %s - left out' % (code, ln, convs(text), convs(english)))
                continue
            size = len(text.encode('utf-8'))
            if size > MAX_BYTES:
                report('%s:%d: %d bytes - left out' % (code, ln, size)); continue
            if size > LABEL_BYTES and english not in LONG_OK:
                report('%s:%d: %d bytes, and the code shows it in a plain label (255) - shorten it' % (code, ln, size))
            texts[english] = text
        if meta.get('script') == 'arabic' and re.search('[ً-ٰٟ]', '|'.join(texts[english]) if english in plural else texts[english]):
            report('%s:%d: vowel marks - the renderer leaves them out' % (code, ln))
    return code, meta, texts


def main():
    plain, plural, eager, where = source_keys()
    keys = plain | set(plural)
    ids = read_ids()
    nxt = max(ids.values(), default=0) + 1
    used = set()
    for k in sorted(keys):
        kt = key_text(k, plural)
        used.add(kt)
        if kt not in ids: ids[kt] = nxt; nxt += 1
    write_ids(ids, used)
    by_number = {n: t for t, n in ids.items()}
    long_path = os.path.join(I18N, 'long.txt')   # the texts the code shows with a long label
    if os.path.exists(long_path):
        for line in open(long_path, encoding='utf-8'):
            line = line.strip()
            if line and not line.startswith('#') and int(line) in by_number:
                LONG_OK.add(by_number[int(line)].split(SEP)[0])

    problems = []
    for k in sorted(keys):   # an English text that is itself too long for a label
        for form in ([k, plural[k]] if k in plural else [k]):
            if len(form.encode('utf-8')) > LABEL_BYTES:
                problems.append('English "%s..." (%s) is %d bytes - split it' % (form[:40], where.get(k), len(form.encode('utf-8'))))
    langs = []
    for f in sorted(os.listdir(I18N)):
        if not f.endswith('.txt') or f in ('keys.txt', 'long.txt'): continue
        code, meta, texts = read_language(os.path.join(I18N, f), by_number, keys, plural, problems.append)
        if meta.get('plural', 'one') not in PLURALS:
            sys.exit('%s: @plural %s is none of %s' % (code, meta.get('plural'), ', '.join(PLURALS)))
        langs.append((int(meta.get('order', '99')), code, meta, texts))
    langs.sort()
    if not langs or langs[0][1] != 'en':
        sys.exit('en.txt must be there and come first (@order 0)')

    sorted_keys = sorted(keys, key=lambda t: t.encode('utf-8'))   # strcmp order
    eager_idx = [i for i, k in enumerate(sorted_keys) if k in eager]
    out = ['// generated by tools/geni18n.py from apps/yo-player/i18n/*.txt - do not edit; run the tool again.',
           '#include "i18n-data.h"', '#include <stddef.h>', '']
    flags = sorted({m.get('flag', '') for _, _, m, _ in langs if m.get('flag')})
    for fl in flags:
        name = 'flag' + fl[:1].upper() + fl[1:]
        out.append('extern const unsigned char %sData[];' % name)
        out.append('extern const unsigned int  %sDataSize;' % name)
    out += ['', 'const int I18N_KEY_COUNT = %d;' % len(sorted_keys), 'const char *const I18N_KEYS[] = {']
    out += ['   %s,' % c_literal(k) for k in sorted_keys]
    out += ['};', '', 'const char *const I18N_PLURALS[] = {']
    out += ['   %s,' % (c_literal(plural[k]) if k in plural else 'NULL') for k in sorted_keys]
    out += ['};', '', 'const int I18N_EAGER_COUNT = %d;' % len(eager_idx),
            'const short I18N_EAGER[] = { %s };' % ', '.join(str(i) for i in eager_idx) if eager_idx else 'const short I18N_EAGER[] = { 0 };', '']
    stats = []
    for _, code, meta, texts in langs[1:]:
        ident = code.replace('-', '_')
        blob = bytearray()
        for k in sorted_keys:
            t = texts.get(k)
            if t is None: blob += b'\0'
            elif isinstance(t, list): blob += '\x1f'.join(t).encode('utf-8') + b'\0'
            else: blob += t.encode('utf-8') + b'\0'
        packed = zlib.compress(bytes(blob), 9)
        stats.append((code, len(blob), len(packed)))
        out.append('static const unsigned char P_%s[] = {' % ident)
        for i in range(0, len(packed), 24):
            out.append('  ' + ','.join('0x%02x' % b for b in packed[i:i + 24]) + ',')
        out += ['};', 'static const char *const E_%s[] = {' % ident]
        out += ['   %s,' % (c_literal(texts[sorted_keys[i]]) if sorted_keys[i] in texts and not isinstance(texts[sorted_keys[i]], list) else 'NULL') for i in eager_idx] or ['   NULL,']
        out += ['};', '']
    script = {'latin': 0, 'cjk': 1, 'arabic': 2}
    out.append('const I18nLang I18N_LANGS[] = {')
    for _, code, meta, texts in langs:
        fl = meta.get('flag', '')
        flag = ('flag' + fl[:1].upper() + fl[1:]) if fl else None
        ident = code.replace('-', '_')
        rule = PLURALS[meta.get('plural', 'one')][0]
        if code == 'en':
            packed = 'NULL, 0, 0, NULL'
        else:
            size = [s for s in stats if s[0] == code][0]
            packed = 'P_%s, sizeof P_%s, %d, E_%s' % (ident, ident, size[1], ident)
        out.append('   { %s, %s, %d, %d, %s, %s, %s },   // %d of %d texts' % (
            c_literal(code), c_literal(meta.get('name', code)), script.get(meta.get('script', 'latin'), 0), rule,
            flag + 'Data' if flag else 'NULL', '&' + flag + 'DataSize' if flag else 'NULL', packed,
            len(texts) if code != 'en' else len(keys), len(keys)))
    out += ['};', 'const int I18N_LANG_COUNT = %d;' % len(langs), '']
    open(OUT, 'w', encoding='utf-8', newline='\n').write('\n'.join(out))

    print('%d texts in the code (%d plural, %d for the picker); %d languages' % (len(keys), len(plural), len(eager), len(langs)))
    total_raw = total_packed = 0
    for _, code, meta, texts in langs:
        if code == 'en': continue
        raw, packed = [(s[1], s[2]) for s in stats if s[0] == code][0]
        total_raw += raw; total_packed += packed
        missing = [k for k in sorted_keys if k not in texts]
        print('  %-6s %3d of %d  %6d B -> %6d B%s' % (code, len(texts), len(keys), raw, packed,
              ('  missing: ' + ', '.join('%d' % ids[key_text(k, plural)] for k in missing[:12]) + (' ...' if len(missing) > 12 else '')) if missing else ''))
    print('  all languages: %d B of text, %d B packed' % (total_raw, total_packed))
    for p in problems: print('  !', p)


if __name__ == '__main__':
    main()
