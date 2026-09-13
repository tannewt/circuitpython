"""
Process raw qstr file and output qstr data with length, hash and data bytes.

This script is only regularly tested with the same version of Python used
during CI, typically the latest "3.x". However, incompatibilities with any
supported CPython version are unintended.

For documentation about the format of compressed translated strings, see
supervisor/shared/translate/compressed_string.h
"""

from __future__ import print_function

import bisect
from dataclasses import dataclass, field
import re
import sys

import collections
import gettext
import pathlib

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(errors="backslashreplace")

sys.path.append(str(pathlib.Path(__file__).parent.parent / "tools/huffman"))

import huffman
from html.entities import codepoint2name
import math


codepoint2name[ord("-")] = "hyphen"

# add some custom names to map characters that aren't in HTML
codepoint2name[ord(" ")] = "space"
codepoint2name[ord("'")] = "squot"
codepoint2name[ord(",")] = "comma"
codepoint2name[ord(".")] = "dot"
codepoint2name[ord(":")] = "colon"
codepoint2name[ord(";")] = "semicolon"
codepoint2name[ord("/")] = "slash"
codepoint2name[ord("%")] = "percent"
codepoint2name[ord("#")] = "hash"
codepoint2name[ord("(")] = "paren_open"
codepoint2name[ord(")")] = "paren_close"
codepoint2name[ord("[")] = "bracket_open"
codepoint2name[ord("]")] = "bracket_close"
codepoint2name[ord("{")] = "brace_open"
codepoint2name[ord("}")] = "brace_close"
codepoint2name[ord("*")] = "star"
codepoint2name[ord("!")] = "bang"
codepoint2name[ord("\\")] = "backslash"
codepoint2name[ord("+")] = "plus"
codepoint2name[ord("$")] = "dollar"
codepoint2name[ord("=")] = "equals"
codepoint2name[ord("?")] = "question"
codepoint2name[ord("@")] = "at_sign"
codepoint2name[ord("^")] = "caret"
codepoint2name[ord("|")] = "pipe"
codepoint2name[ord("~")] = "tilde"

C_ESCAPES = {
    "\a": "\\a",
    "\b": "\\b",
    "\f": "\\f",
    "\n": "\\n",
    "\r": "\\r",
    "\t": "\\t",
    "\v": "\\v",
    "'": "\\'",
    '"': '\\"',
}

# Reserved symbol values. 2 and 3 are unused for now but must not appear in
# translated text either.
QSTR_ESC = "\1"
RESERVED_CHARS = {"\1", "\2", "\3"}

# The first non-ASCII symbol value in the dense alphabet.
ALPHABET_BASE = 0x80

# Huffman codes are chosen per "class" of the previously decoded character.
# These base classes are fixed in the C decoder (translate.c, base_class());
# the generator collapses them with class_map[] into TRANSLATION_CLASSES tables.
NUM_BASE_CLASSES = 7
# Presets to try, as class_map. Each maps a base class to a table index.
CLASS_PRESETS = (
    (0, 0, 0, 0, 0, 0, 0),  # a single table: the tables cost more than they save
    (0, 1, 1, 2, 2, 2, 1),  # space, letter, other
    (0, 1, 2, 3, 4, 5, 1),  # non-ASCII shares the lowercase table
    (0, 1, 2, 3, 4, 5, 6),  # non-ASCII has its own table
)


def base_class(c):
    """Class of the character preceding the next symbol; None means start of string.
    Computed on the (possibly remapped) character: remapped characters are >= 0x80
    exactly when the original is, which is all the classification looks at."""
    if c is None or c == " ":
        return 0
    o = ord(c)
    if 0x61 <= o <= 0x7A:
        return 1
    if 0x41 <= o <= 0x5A:
        return 2
    if 0x30 <= o <= 0x39:
        return 3
    if o == 0x25:  # '%'
        return 5
    if o >= 0x80:
        return 6
    return 4


# this must match the equivalent function in qstr.c
def compute_hash(qstr, bytes_hash):
    hash = 5381
    for b in qstr:
        hash = (hash * 33) ^ b
    # Make sure that valid hash is never zero, zero means "hash not computed"
    return (hash & ((1 << (8 * bytes_hash)) - 1)) or 1


def translate(translation_file, i18ns):
    with open(translation_file, "rb") as f:
        table = gettext.GNUTranslations(f)

        translations = []
        for original in i18ns:
            unescaped = original
            for s, replacement in C_ESCAPES.items():
                unescaped = unescaped.replace(replacement, s)
            if original == "en_US":
                translation = table.info()["language"]
            else:
                translation = table.gettext(unescaped)
            # Add in carriage returns to work in terminals
            translation = translation.replace("\n", "\r\n")
            translations.append((original, translation))
        return translations


class TextSplitter:
    def __init__(self, words):
        words = sorted(words, key=lambda x: len(x), reverse=True)
        self.words = set(words)
        if words:
            pat = "|".join(re.escape(w) for w in words) + "|."
        else:
            pat = "."
        self.pat = re.compile(pat, flags=re.DOTALL)

    def iter_words(self, text):
        s = []
        words = self.words
        for m in self.pat.finditer(text):
            t = m.group(0)
            if t in words:
                if s:
                    yield (False, "".join(s))
                    s = []
                yield (True, t)
            else:
                s.append(t)
        if s:
            yield (False, "".join(s))

    def iter(self, text):
        for m in self.pat.finditer(text):
            yield m.group(0)


def iter_substrings(s, minlen, maxlen):
    len_s = len(s)
    maxlen = min(len_s, maxlen)
    for n in range(minlen, maxlen + 1):
        for begin in range(0, len_s - n + 1):
            yield s[begin : begin + n]


# Languages whose non-ASCII alphabet does not fit the dense 8-bit alphabet and
# therefore use 16-bit table entries.
translation_requires_uint16 = {"ja", "ko"}


@dataclass
class EncodingTable:
    # One entry per class table: list of symbols in canonical order.
    values: list
    # One entry per class table: list of code-length counts, padded to lengths_row.
    lengths: list
    lengths_row: int
    class_map: tuple
    # Per class table: symbol -> canonical code (string of '0'/'1').
    canonical: list
    # Dictionary words, in remapped characters, sorted by length.
    words: list
    word_start: int
    # Dense alphabet: index -> original character. Empty in uint16 mode.
    alphabet: list
    # Original character -> remapped character (identity for ASCII).
    remap: dict
    translation_qstr_bits: int
    qstrs: dict
    qstrs_inv: dict
    values_type: str
    # Remapped text -> token list used to encode it. A token is a str (character
    # or word) or a ("q", qstr) tuple.
    tokens: dict = field(default_factory=dict)


def remap_text(text, remap):
    return "".join(remap.get(c, c) for c in text)


def is_qstr(token):
    return isinstance(token, tuple)


def token_len(token):
    return len(token[1]) if is_qstr(token) else len(token)


def token_symbol(token):
    """The Huffman symbol a token is coded as (qstrs share one escape symbol)."""
    return QSTR_ESC if is_qstr(token) else token


def code_lengths(counter):
    """Huffman code lengths for a symbol counter. A lone symbol gets a 1-bit code."""
    if len(counter) == 1:
        return {next(iter(counter)): 1}
    cb = huffman.codebook(counter.items())
    return {k: len(v) for k, v in cb.items()}


def canonical_codes(lengths):
    """Canonical Huffman codes from a symbol -> length dict.
    Returns (values in canonical order, per-length counts, symbol -> code)."""
    if not lengths:
        return [], [], {}
    values = []
    length_count = collections.Counter()
    canonical = {}
    renumbered = 0
    last_length = None
    for atom, length in sorted(lengths.items(), key=lambda x: (x[1], x[0])):
        values.append(atom)
        length_count[length] += 1
        if last_length:
            renumbered <<= length - last_length
        canonical[atom] = "{0:0{width}b}".format(renumbered, width=length)
        renumbered += 1
        last_length = length
    counts = [length_count.get(i, 0) for i in range(1, max(length_count) + 2)]
    return values, counts, canonical


def parse_optimal(
    text, lens, class_map, words_by_first, qstrs_by_first, qstr_bits, unknown_len=32
):
    """Tokenize text with the fewest bits given per-class code lengths.
    Symbols missing from a class table are allowed at a penalty so a parse always exists."""
    n = len(text)
    best = [math.inf] * (n + 1)
    back = [None] * (n + 1)
    best[0] = 0
    for i in range(n):
        if best[i] == math.inf:
            continue
        table = lens[class_map[base_class(text[i - 1] if i else None)]]
        c = text[i]
        cands = [c]
        for w in words_by_first.get(c, ()):
            if text.startswith(w, i):
                cands.append(w)
        for q in qstrs_by_first.get(c, ()):
            if text.startswith(q, i):
                cands.append(("q", q))
        for tok in cands:
            cost = table.get(token_symbol(tok), unknown_len)
            if is_qstr(tok):
                cost += qstr_bits
            j = i + token_len(tok)
            if best[i] + cost < best[j]:
                best[j] = best[i] + cost
                back[j] = tok
    out = []
    i = n
    while i > 0:
        tok = back[i]
        out.append(tok)
        i -= token_len(tok)
    out.reverse()
    return out


def tally(texts, tokens, class_map):
    """Symbol counts per class table for the given tokenizations."""
    counts = collections.defaultdict(collections.Counter)
    for text in texts:
        pos = 0
        for tok in tokens[text]:
            cls = class_map[base_class(text[pos - 1] if pos else None)]
            counts[cls][token_symbol(tok)] += 1
            pos += token_len(tok)
    return counts


def encoded_bits(text, tokens, lens, class_map, qstr_bits):
    bits = 0
    pos = 0
    for tok in tokens:
        cls = class_map[base_class(text[pos - 1] if pos else None)]
        bits += lens[cls][token_symbol(tok)]
        if is_qstr(tok):
            bits += qstr_bits
        pos += token_len(tok)
    return bits


def compute_huffman_coding(qstrs, translation_name, translations, f, compression_level):
    # possible future improvement: some languages are better when consider len(k) > 2. try both?
    qstrs = dict((k, v) for k, v in qstrs.items() if len(k) > 3)
    qstr_strs = list(qstrs.keys())
    original_texts = [t[1] for t in translations]
    words = []

    for text in original_texts:
        bad = RESERVED_CHARS.intersection(text)
        if bad:
            raise ValueError(f"Translation contains reserved character {bad!r}: {text!r}")

    translation_name = translation_name.split("/")[-1].split(".")[0]

    # Dense alphabet: non-ASCII characters are renumbered from 0x80 by decreasing
    # frequency so that all symbols fit in 8 bits. If there are too many of them,
    # fall back to raw 16-bit code points.
    hi_count = collections.Counter(c for t in original_texts for c in t if ord(c) >= 0x80)
    if len(hi_count) <= 0x7F:
        alphabet = [c for c, _ in hi_count.most_common()]
        remap = {c: chr(ALPHABET_BASE + i) for i, c in enumerate(alphabet)}
        word_start = ALPHABET_BASE + len(alphabet)
        max_words = 0x100 - word_start
        values_type = "uint8_t"
    else:
        if translation_name not in translation_requires_uint16:
            raise ValueError(
                f"Translation {translation_name} expected to fit in 8 bits but required 16 bits"
            )
        alphabet = []
        remap = {}
        # Words take the unused code points from 0x80 up to the lowest one in use.
        end_unused = min([0xFF] + [o for o in map(ord, hi_count) if o < 0xFF])
        word_start = ALPHABET_BASE
        max_words = end_unused - word_start
        values_type = "uint16_t"
    if compression_level < 5:
        max_words = 0
    bits_per_codepoint = 16 if values_type == "uint16_t" else 8

    texts = [remap_text(t, remap) for t in original_texts]

    # Prune the qstrs to only those that appear in the texts
    qstr_counters = collections.Counter()
    qstr_extractor = TextSplitter(qstr_strs)
    for t in texts:
        for qstr in qstr_extractor.iter(t):
            if qstr in qstr_strs:
                qstr_counters[qstr] += 1
    qstr_strs = list(qstr_counters.keys())

    while len(words) < max_words:
        # Until the dictionary is filled to capacity, use a heuristic to find
        # the best "word" (2- to 11-gram) to add to it.
        #
        # The TextSplitter allows us to avoid considering parts of the text
        # that are already covered by a previously chosen word, for example
        # if "the" is in words then not only will "the" not be considered
        # again, neither will "there" or "wither", since they have "the"
        # as substrings.
        extractor = TextSplitter(words + qstr_strs)
        counter = collections.Counter()
        for t in texts:
            for atom in extractor.iter(t):
                if atom in qstrs:
                    atom = QSTR_ESC
                counter[atom] += 1
        cb = huffman.codebook(counter.items())
        lengths = sorted(dict((v, len(cb[k])) for k, v in counter.items()).items())

        def bit_length(s):
            return sum(len(cb[c]) for c in s)

        def est_len(occ):
            idx = bisect.bisect_left(lengths, (occ, 0))
            return lengths[idx][1] + 1

        # The cost of adding a dictionary word is just its storage size
        # while its savings is close to the difference between the original
        # huffman bit-length of the string and the estimated bit-length
        # of the dictionary word, times the number of times the word appears.
        #
        # The savings is not strictly accurate because including a word into
        # the Huffman tree bumps up the encoding lengths of all words in the
        # same subtree.  In the extreme case when the new word is so frequent
        # that it gets a one-bit encoding, all other words will cost an extra
        # bit each. This is empirically modeled by the constant factor added to
        # cost, but the specific value used isn't "proven" to be correct.
        #
        # Another source of inaccuracy is that compressed strings end up
        # on byte boundaries, not bit boundaries, so saving 1 bit somewhere
        # might not save a byte.
        #
        # In fact, when this change was first made, some translations (luckily,
        # ones on boards not at all close to full) wasted up to 40 bytes,
        # while the most constrained boards typically gained 100 bytes or
        # more.
        #
        # The difference between the two is the estimated net savings, in bits.
        def est_net_savings(s, occ):
            savings = occ * (bit_length(s) - est_len(occ))
            cost = len(s) * bits_per_codepoint + 24
            return savings - cost

        counter = collections.Counter()
        for t in texts:
            for found, word in extractor.iter_words(t):
                if not found:
                    for substr in iter_substrings(word, minlen=2, maxlen=11):
                        counter[substr] += 1

        # Score the candidates we found.  This is a semi-empirical formula that
        # attempts to model the number of bits saved as closely as possible.
        #
        # It attempts to compute the codeword lengths of the original word
        # to the codeword length the dictionary entry would get, times
        # the number of occurrences, less the ovehead of the entries in the
        # words[] array.
        #
        # The set of candidates is pruned by estimating their relative value and
        # picking to top 100 scores.

        counter = sorted(counter.items(), key=lambda x: math.log(x[1]) * len(x[0]), reverse=True)[
            :100
        ]
        scores = sorted(
            ((s, -est_net_savings(s, occ)) for (s, occ) in counter if occ > 1),
            key=lambda x: x[1],
        )

        # Pick the one with the highest score.  The score must be negative.
        if not scores or scores[0][-1] >= 0:
            break

        word = scores[0][0]
        words.append(word)

    # Now that the dictionary is fixed, find the tokenization of each string that
    # takes the fewest bits, with Huffman codes chosen per class of the previous
    # character. Code lengths and tokenization depend on each other, so iterate
    # a few rounds from the greedy tokenization; the final tokenization and the
    # codes built from it are what get emitted, so they are always consistent.
    use_qstrs = compression_level > 3
    dp_qstrs = qstr_strs if use_qstrs else []
    # Upper bound on the qstr index width while parsing; the real width is computed below.
    est_qstr_bits = max([0] + [qstrs[q] for q in dp_qstrs]).bit_length()
    words_by_first = collections.defaultdict(list)
    for w in words:
        words_by_first[w[0]].append(w)
    qstrs_by_first = collections.defaultdict(list)
    for q in dp_qstrs:
        qstrs_by_first[q[0]].append(q)

    greedy = TextSplitter(words + dp_qstrs)
    greedy_tokens = {}
    for t in texts:
        greedy_tokens[t] = [("q", a) if a in qstrs else a for a in greedy.iter(t)]

    max_translation_encoded_length = max(len(t.encode("utf-8")) for t in original_texts)
    encoded_length_bits = max_translation_encoded_length.bit_length()

    def table_bytes(counts, used_words):
        nclasses = max(class_map) + 1
        row = max(max(code_lengths(cn).values()) for cn in counts.values()) + 1
        mchar = bits_per_codepoint // 8
        n = sum(len(cn) for cn in counts.values()) * mchar  # values
        n += nclasses * row  # lengths
        n += 2 * (nclasses + 1)  # values_offset
        n += NUM_BASE_CLASSES  # class_map
        n += sum(len(w) for w in used_words) * mchar  # words
        if used_words:
            n += len(used_words[-1]) - len(used_words[0]) + 1  # wlencount
        n += 2 * len(alphabet)
        return n

    best = None
    for class_map in CLASS_PRESETS:
        tokens = greedy_tokens
        counts = tally(texts, tokens, class_map)
        lens = {cls: code_lengths(cn) for cls, cn in counts.items()}
        for _ in range(3):
            tokens = {
                t: parse_optimal(t, lens, class_map, words_by_first, qstrs_by_first, est_qstr_bits)
                for t in texts
            }
            counts = tally(texts, tokens, class_map)
            lens = {cls: code_lengths(cn) for cls, cn in counts.items()}
        used_symbols = set().union(*counts.values())
        used_words = sorted((w for w in words if w in used_symbols), key=len)
        size = table_bytes(counts, used_words) + sum(
            (encoded_length_bits + encoded_bits(t, tokens[t], lens, class_map, est_qstr_bits) + 7)
            // 8
            for t in texts
        )
        if best is None or size < best[0]:
            best = (size, class_map, tokens, counts, lens, used_words)
    size, class_map, tokens, counts, lens, words = best

    used_qstr = 0
    for toks in tokens.values():
        for tok in toks:
            if is_qstr(tok):
                used_qstr = max(used_qstr, qstrs[tok[1]])
    translation_qstr_bits = used_qstr.bit_length()

    word_end = word_start + len(words) - 1
    word_code = {w: chr(word_start + i) for i, w in enumerate(words)}

    def symbol_value(sym):
        return ord(word_code.get(sym, sym))

    nclasses = max(class_map) + 1
    values = []
    lengths_rows = []
    canonical = []
    for cls in range(nclasses):
        # Symbols are ordered by (length, value) so that the C decoder's canonical
        # walk lands on the same index.
        table = lens.get(cls, {})
        by_value = {chr(symbol_value(s)): l for s, l in table.items()}
        v, counts_row, canon = canonical_codes(by_value)
        values.append([ord(x) for x in v])
        lengths_rows.append(counts_row)
        canonical.append({s: canon[chr(symbol_value(s))] for s in table})
    lengths_row = max(1, max(len(r) for r in lengths_rows))
    lengths_rows = [r + [0] * (lengths_row - len(r)) for r in lengths_rows]
    assert all(len(code) >= 1 for canon in canonical for code in canon.values())

    f.write(f"// # words {len(words)}\n")
    f.write(
        "// words {}\n".format([remap_text(w, {v: k for k, v in remap.items()}) for w in words])
    )
    f.write(f"// # alphabet {len(alphabet)}\n")
    f.write(f"// class_map {class_map} tables {nclasses}\n")
    for cls in range(nclasses):
        f.write(f"// class {cls}: {len(values[cls])} symbols, lengths {lengths_rows[cls]}\n")

    maxlen = len(words[-1]) if words else 0
    minlen = len(words[0]) if words else 0
    wlencount = [len([None for w in words if len(w) == l]) for l in range(minlen, maxlen + 1)]

    f.write("typedef {} mchar_t;\n".format(values_type))
    f.write("#define compress_max_length_bits ({})\n".format(encoded_length_bits))
    f.write("#define TRANSLATION_CLASSES {}\n".format(nclasses))
    f.write("#define LENGTHS_ROW {}\n".format(lengths_row))
    f.write(
        "const uint8_t class_map[{}] = {{ {} }};\n".format(
            NUM_BASE_CLASSES, ", ".join(map(str, class_map))
        )
    )
    f.write(
        "const uint8_t lengths[] = {{ {} }};\n".format(
            ", ".join(str(x) for row in lengths_rows for x in row)
        )
    )
    offsets = [0]
    for v in values:
        offsets.append(offsets[-1] + len(v))
    f.write(
        "const uint16_t values_offset[{}] = {{ {} }};\n".format(
            nclasses + 1, ", ".join(map(str, offsets))
        )
    )
    f.write(
        "const mchar_t values[] = {{ {} }};\n".format(", ".join(str(x) for v in values for x in v))
    )
    f.write("#define alphabet_size {}\n".format(len(alphabet)))
    f.write(
        "const uint16_t alphabet[] = {{ {} }};\n".format(", ".join(str(ord(c)) for c in alphabet))
    )
    f.write(
        "const mchar_t words[] = {{ {} }};\n".format(
            ", ".join(str(ord(c)) for w in words for c in w)
        )
    )
    f.write("const uint8_t wlencount[] = {{ {} }};\n".format(", ".join(str(p) for p in wlencount)))
    f.write("#define word_start {}\n".format(word_start))
    f.write("#define word_end {}\n".format(word_end))
    f.write("#define minlen {}\n".format(minlen))
    f.write("#define maxlen {}\n".format(maxlen))
    f.write("#define translation_qstr_bits {}\n".format(translation_qstr_bits))

    qstrs_inv = dict((v, k) for k, v in qstrs.items())
    return EncodingTable(
        values,
        lengths_rows,
        lengths_row,
        class_map,
        canonical,
        words,
        word_start,
        alphabet,
        remap,
        translation_qstr_bits,
        qstrs,
        qstrs_inv,
        values_type,
        tokens,
    )


def decompress(encoding_table, encoded, encoded_length_bits):
    """Decode as the C decoder does (translate.c). Returns the original text."""
    et = encoding_table
    alphabet_size = len(et.alphabet)
    word_end = et.word_start + len(et.words) - 1

    def bititer():
        for byte in encoded:
            for bit in (0x80, 0x40, 0x20, 0x10, 0x8, 0x4, 0x2, 0x1):
                yield bool(byte & bit)

    nextbit = bititer().__next__

    def getnbits(n):
        bits = 0
        for i in range(n):
            bits = (bits << 1) | nextbit()
        return bits

    dec = []
    last = None
    length = getnbits(encoded_length_bits)
    decoded = 0

    def emit(u):
        nonlocal last, decoded
        if ALPHABET_BASE <= u < ALPHABET_BASE + alphabet_size:
            c = et.alphabet[u - ALPHABET_BASE]
        else:
            c = chr(u)
        dec.append(c)
        decoded += len(c.encode("utf-8"))
        last = c

    while decoded < length:
        cls = et.class_map[base_class(last)]
        lengths = et.lengths[cls]
        bits = 0
        bit_length = 0
        max_code = lengths[0]
        searched_length = lengths[0]
        while True:
            bits = (bits << 1) | nextbit()
            bit_length += 1
            if max_code > 0 and bits < max_code:
                break
            max_code = (max_code << 1) + lengths[bit_length]
            searched_length += lengths[bit_length]
        v = et.values[cls][searched_length + bits - max_code]
        if v == 1:
            qstr_idx = getnbits(et.translation_qstr_bits)
            s = et.qstrs_inv[qstr_idx]
            dec.append(s)
            decoded += len(s.encode("utf-8"))
            last = s[-1]
        elif et.word_start <= v <= word_end:
            for c in et.words[v - et.word_start]:
                emit(ord(c))
        else:
            emit(v)
    return "".join(dec)


def compress(encoding_table, decompressed, encoded_length_bits, len_translation_encoded):
    """Encode a translation using the tokenization chosen in compute_huffman_coding()."""
    if not isinstance(decompressed, str):
        raise TypeError()
    et = encoding_table
    text = remap_text(decompressed, et.remap)
    tokens = et.tokens[text]

    enc = 1

    def put_bit(enc, b):
        return (enc << 1) | bool(b)

    def put_bits(enc, b, n):
        for i in range(n - 1, -1, -1):
            enc = put_bit(enc, b & (1 << i))
        return enc

    def put_code(enc, cls, sym):
        for b in et.canonical[cls][sym]:
            enc = put_bit(enc, b == "1")
        return enc

    enc = put_bits(enc, len_translation_encoded, encoded_length_bits)

    pos = 0
    for tok in tokens:
        cls = et.class_map[base_class(text[pos - 1] if pos else None)]
        enc = put_code(enc, cls, token_symbol(tok))
        if is_qstr(tok):
            enc = put_bits(enc, et.qstrs[tok[1]], et.translation_qstr_bits)
        pos += token_len(tok)

    while enc.bit_length() % 8 != 1:
        enc = put_bit(enc, 0)

    r = enc.to_bytes((enc.bit_length() + 7) // 8, "big")
    return r[1:]


def qstr_escape(qst):
    def esc_char(m):
        c = ord(m.group(0))
        try:
            name = codepoint2name[c]
        except KeyError:
            name = "0x%02x" % c
        return "_" + name + "_"

    return re.sub(r"[^A-Za-z0-9_]", esc_char, qst)


def parse_qstrs(infile):
    r = {}
    rx = re.compile(
        r'QDEF(?P<pool>[01])\([A-Za-z0-9_]+,\s*\d+,\s*\d+,\s*(?P<cstr>"(?:[^"\\\\]|\\.)*")\)'
    )
    content = infile.read()
    matches = rx.finditer(content)
    qdef0_qstrs = []
    qdef1_qstrs = []
    for match in matches:
        qstr = eval(match.group("cstr"))
        if match.group("pool") == "0":
            qdef0_qstrs.append(qstr)
        else:
            qdef1_qstrs.append(qstr)
    for i, qstr in enumerate(qdef0_qstrs):
        r[qstr] = i
    for i, qstr in enumerate(qdef1_qstrs, start=len(qdef0_qstrs)):
        r[qstr] = i
    return r


def parse_input_headers(infiles):
    i18ns = set()

    # read the TRANSLATE strings in from the input files
    for infile in infiles:
        with open(infile, "rt") as f:
            for line in f:
                line = line.strip()
                match = re.match(r'^TRANSLAT(E|ION)\("(.*)"(, \d+)?\)$', line)
                if match:
                    i18ns.add(match.group(2))
                    continue

    return i18ns


def escape_bytes(qstr):
    if all(32 <= ord(c) <= 126 and c != "\\" and c != '"' for c in qstr):
        # qstr is all printable ASCII so render it as-is (for easier debugging)
        return qstr
    else:
        # qstr contains non-printable codes so render entire thing as hex pairs
        qbytes = bytes(qstr, "utf8")
        return "".join(("\\x%02x" % b) for b in qbytes)


def make_bytes(cfg_bytes_len, cfg_bytes_hash, qstr):
    qbytes = bytes(qstr, "utf8")
    qlen = len(qbytes)
    qhash = compute_hash(qbytes, cfg_bytes_hash)
    if qlen >= (1 << (8 * cfg_bytes_len)):
        print("qstr is too long:", qstr)
        assert False
    qdata = escape_bytes(qstr)
    return '%d, %d, "%s"' % (qhash, qlen, qdata)


def output_translation_data(encoding_table, i18ns, out):
    # print out the starter of the generated C file
    out.write("// This file was automatically generated by maketranslatedata.py\n")
    out.write('#include "supervisor/shared/translate/compressed_string.h"\n')
    out.write("\n")

    total_text_size = 0
    total_text_compressed_size = 0
    max_translation_encoded_length = max(
        len(translation.encode("utf-8")) for original, translation in i18ns
    )
    encoded_length_bits = max_translation_encoded_length.bit_length()
    for i, translation in enumerate(i18ns):
        original, translation = translation
        translation_encoded = translation.encode("utf-8")
        compressed = compress(
            encoding_table, translation, encoded_length_bits, len(translation_encoded)
        )
        total_text_compressed_size += len(compressed)
        decompressed = decompress(encoding_table, compressed, encoded_length_bits)
        assert decompressed == translation, (decompressed, translation)
        for c, replacement in C_ESCAPES.items():
            decompressed = decompressed.replace(c, replacement)
        formatted = ["{:d}".format(x) for x in compressed]
        out.write(
            "const struct compressed_string translation{} = {{ .data = {}, .tail = {{ {} }} }}; // {}\n".format(
                i,
                formatted[0],
                ", ".join(formatted[1:]),
                original,
            )
        )
        total_text_size += len(translation.encode("utf-8"))

    out.write("\n")
    out.write("// {} bytes worth of translations\n".format(total_text_size))
    out.write("// {} bytes worth of translations compressed\n".format(total_text_compressed_size))
    out.write("// {} bytes saved\n".format(total_text_size - total_text_compressed_size))


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(
        description="Process TRANSLATE strings into headers for compilation"
    )
    parser.add_argument(
        "infiles", metavar="N", type=str, nargs="+", help="an integer for the accumulator"
    )
    parser.add_argument(
        "--translation", default=None, type=str, help="translations for i18n() items"
    )
    parser.add_argument(
        "--compression_level",
        type=int,
        default=9,
        help="degree of compression (>5: construct dictionary; >3: use qstrs)",
    )
    parser.add_argument(
        "--compression_filename",
        type=argparse.FileType("w", encoding="UTF-8"),
        help="header for compression info",
    )
    parser.add_argument(
        "--translation_filename",
        type=argparse.FileType("w", encoding="UTF-8"),
        help="c file for translation data",
    )
    parser.add_argument(
        "--qstrdefs_filename",
        type=argparse.FileType("r", encoding="UTF-8"),
        help="",
    )

    args = parser.parse_args()

    qstrs = parse_qstrs(args.qstrdefs_filename)
    i18ns = parse_input_headers(args.infiles)
    i18ns = sorted(i18ns)
    translations = translate(args.translation, i18ns)
    encoding_table = compute_huffman_coding(
        qstrs, args.translation, translations, args.compression_filename, args.compression_level
    )
    output_translation_data(encoding_table, translations, args.translation_filename)
