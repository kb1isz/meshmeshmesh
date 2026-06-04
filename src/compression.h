#pragma once
// ============================================================================
// compression.h — Smaz short-string compression API
//
// Smaz is a compression library for very short strings (English chat text).
// Uses a hardcoded codebook of 254 common English words/digraphs/trigraphs.
// Public domain (original by Salvatore Sanfilippo / antirez).
// ============================================================================

#include <stddef.h>
#include <stdint.h>

// Compress a null-terminated string `in` of length `inLen` into `out`.
// `out` must be at least `inLen` (compression may expand pathological input).
// Returns the number of bytes written to `out`, or 0 on failure.
size_t smaz_compress(const char *in, size_t inLen, uint8_t *out);

// Decompress compressed data `in` of length `inLen` into `out`.
// `out` must be at least 4096 bytes (max decompressed size).
// Returns the number of bytes written to `out`, or 0 on failure.
size_t smaz_decompress(const uint8_t *in, size_t inLen, char *out, size_t outCapacity);

// Maximum guaranteed decompressed size for a single smaz block.
#define SMAZ_MAX_DECOMPRESSED 4096