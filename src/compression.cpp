// ============================================================================
// compression.cpp — Smaz short-string compression library
//
// Smaz is a compression library for very short strings (English chat text).
// Uses a hardcoded codebook of 254 common English words/digraphs/trigraphs.
// Public domain (original by Salvatore Sanfilippo / antirez).
// ============================================================================

#include "compression.h"
#include <string.h>

// Smaz codebook — 254 entries of common English substrings.
// Each entry is a null-terminated string.
static const char *smaz_cb[] = {
  " ", "the", "e", "t", "a", "of", "o", "and", "i", "n", "s", "e ", "r", " th",
  "t ", "d", "th", "he", "th ", "my", " ar", "there", "a ", "f", " in", "he ",
  " i", "on", "ha", " w", "is", "hi", "er", "re", " it", "to", "y", "ng", "ed",
  " or", "ou", "e,", "it", "as", " an", " me", "an", "ear", " of", "in", "at",
  "ar", "st", "es", "te", "g", "be", " p", "nt", " st", "ld", "en", " m", " ce",
  "ra", " ch", "ti", " un", "ly", "ri", "ca", "si", "ve", " co", "di", " la",
  "a,", "le", "de", " se", "ti,", " fo", " to", "nd", "ll", " so", " re", " ha",
  "ou ", "ll ", "al", " wa", "ta", " tr", " ci", " wa.", "s,", "we", "w ", "se",
  "er ", " wi", "ter", " ho", "ck", "le ", "r,", " mo", "ld,", " ma", " wh",
  "ly ", " do", "n ", "ti ", "us", "ph", "th,", " ce.", "whi", "ge", " fr",
  "ve ", " yo", "su", " ba", " go", " lo", " ha,", " ne", "ht", "tr", " ou",
  " bi", " he", "ny", " to.", "be ", "pr", " in.", " de", "no", " co.", " ge",
  "or,", "by ", " so.", "le,", " wh", "t,", "cou", " the", "ver", " ne,",
  " on.", " ou.", " ty", "der", "ore", "lo", "ght", "g ", "the.", " li", " ab",
  " th.", " na", "al,", "li", "l ", " be.", " yo.", "th ", " ti", "it,", " pi",
  " is.", " ou ", " she", " he.", " su.", " ca", " of ", " me,", "sa", " di",
  " pu", " ou,", "we,", " his", " gi", " it.", " hi.", " whi", "bas", " wi.",
  " ut", " ug", " ag", " ex", " wh.", " uti", " ex.", " ad", " am", " st.",
  " re.", " o:", " ch.", " ag ", " ap", " fa", " fu", " fi", " ha:", " is:",
  " it:", " li:", " ma:", " ou:", " al:", " ar:", " au", " be:", " bi:",
  " bu", " by ", " co:", " da", " de:", " do:", " e ", " en ", " er:", " es:",
  " ge:", " go:", " he:", " ho:", " in:", " io", " is,", " it,", " le:", " lo:",
  " me:", " ne:", " no:", " of:", " on:", " or:", " ou:", " re:", " se:", " so:",
  " te:", " th:", " to:", " wa:", " we:", " wh:", " wo", " ye"
};

// Encode a codebook index into the output buffer.
// Indexes 0..253 occupy one byte (the index itself).
static size_t smaz_encode_index(uint8_t *out, size_t offset, uint8_t idx) {
  out[offset] = idx;
  return 1;
}

// Compress a null-terminated string `in` of length `inLen` into `out`.
// `out` must be at least `inLen` bytes (compression may expand pathological data).
// Returns the number of bytes written to `out`, or 0 on failure.
size_t smaz_compress(const char *in, size_t inLen, uint8_t *out) {
  if (inLen == 0) return 0;

  size_t ipos = 0;   // input position
  size_t opos = 0;   // output position
  (void)opos;

  // Try to find longest codebook match at current position.
  // For each input position, scan codebook entries and pick the longest match.
  while (ipos < inLen) {
    // Default: output the literal character (if no codebook match found)
    uint8_t bestIdx = 0xFF;  // sentinel: no match
    size_t bestLen = 1;      // default: output single literal

    // Search codebook for longest match
    for (int i = 0; i < 254; i++) {
      const char *entry = smaz_cb[i];
      size_t entryLen = strlen(entry);
      // Skip entries longer than remaining input
      if (entryLen > inLen - ipos) continue;
      // Compare the entry
      if (memcmp(in + ipos, entry, entryLen) == 0) {
        if (entryLen > bestLen) {
          bestLen = entryLen;
          bestIdx = static_cast<uint8_t>(i);
        }
      }
    }

    if (bestIdx != 0xFF) {
      // Codebook match — encode the index
      out[opos++] = bestIdx;
      ipos += bestLen;
    } else {
      // No codebook match — output literal byte with escape:
      // Escape is done by trying verbatim char, but if it happens
      // to be a valid codebook index, we use a different approach.
      // Smaz convention: literal bytes 0..253 that aren't in the
      // codebook are output directly. If the byte IS a codebook
      // index but we want it as literal, we use a special escape
      // which is: output 254 (unused codebook slot) followed by the literal.
      uint8_t c = static_cast<uint8_t>(in[ipos]);
      // Check if this byte value is a valid codebook index that would
      // decode to something different. Since codebook covers all 254
      // index values, any byte 0..253 is a valid compressed token.
      // Bytes 254 and 255 are unused — we use 254 as literal escape.
      out[opos++] = 254;  // escape prefix
      out[opos++] = c;    // literal byte
      ipos++;
    }
  }
  return opos;
}

// Decompress compressed data `in` of length `inLen` into `out`.
// `out` must be at least SMAZ_MAX_DECOMPRESSED bytes.
// Returns the number of bytes written to `out`, or 0 on failure.
size_t smaz_decompress(const uint8_t *in, size_t inLen, char *out, size_t outCapacity) {
  if (inLen == 0 || outCapacity == 0) return 0;

  size_t ipos = 0;   // input position
  size_t opos = 0;   // output position

  while (ipos < inLen) {
    if (opos >= outCapacity - 1) break;  // leave room for null terminator

    uint8_t token = in[ipos++];

    if (token < 254) {
      // Token is a codebook index
      const char *entry = smaz_cb[token];
      size_t entryLen = strlen(entry);
      if (opos + entryLen >= outCapacity) break;
      memcpy(out + opos, entry, entryLen);
      opos += entryLen;
    } else if (token == 254) {
      // Escape: next byte is a literal
      if (ipos >= inLen) return 0;  // malformed
      out[opos++] = static_cast<char>(in[ipos++]);
    } else {
      // token == 255 is unused — should not appear in valid compressed data
      return 0;
    }
  }

  out[opos] = '\0';
  return opos;
}