// ============================================================================
// crypto.cpp — Encryption, hex encoding, and node identity derivation
//
// Implements:
//  - AES-CTR encryption/decryption using mbedtls (ESP32 built-in)
//  - SHA-256 key derivation from the user-provided encryption passphrase
//  - Hex encoding/decoding for wire-safe binary payloads
//  - Node ID derivation from eFuse MAC using SplitMix64
//
// SECURITY NOTE: This firmware uses AES-CTR which provides confidentiality
// but NO authentication/integrity protection. A future version should
// migrate to AES-GCM or AES-CTR+HMAC to prevent ciphertext manipulation.
// Additionally, the encryption key is derived from the shared secret via
// SHA-256 and stored in NVS flash. Physical access to the device allows
// reading the key. Enable ESP32 flash encryption for production use.
// ============================================================================

#include "globals.h"

// Convert a 4-bit value (0-15) to its uppercase hex character.
char hexNibble(uint8_t value) {
  value &= 0x0F;
  return value < 10 ? static_cast<char>('0' + value) : static_cast<char>('A' + value - 10);
}

// Convert a single hex character (0-9, a-f, A-F) to its 4-bit value.
// Returns -1 for invalid characters.
int8_t fromHexNibble(char value) {
  if (value >= '0' && value <= '9') return static_cast<int8_t>(value - '0');
  if (value >= 'a' && value <= 'f') return static_cast<int8_t>(value - 'a' + 10);
  if (value >= 'A' && value <= 'F') return static_cast<int8_t>(value - 'A' + 10);
  return -1;
}

// Convert a byte array to an uppercase hex string.
// `data` points to the binary buffer, `len` is the number of bytes.
// Returns a String of length 2 * len.
String bytesToHex(const uint8_t *data, size_t len) {
  String out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out += hexNibble(data[i] >> 4);
    out += hexNibble(data[i]);
  }
  return out;
}

// Convert a hex string to a byte array.
// `hex` must have even length. `out` receives the decoded bytes.
// `maxLen` limits output length. `outLen` receives actual decoded length.
// Returns false if hex is malformed or too long.
bool hexToBytes(const String &hex, uint8_t *out, size_t maxLen, size_t &outLen) {
  if (hex.length() % 2 != 0 || hex.length() / 2 > maxLen) return false;
  outLen = hex.length() / 2;
  for (size_t i = 0; i < outLen; ++i) {
    const int8_t high = fromHexNibble(hex[i * 2]);
    const int8_t low = fromHexNibble(hex[i * 2 + 1]);
    if (high < 0 || low < 0) return false;
    out[i] = static_cast<uint8_t>((high << 4) | low);
  }
  return true;
}

// Derive a 256-bit AES key from the user's encryptionKey passphrase using SHA-256.
// The key is written directly into the `out` buffer (AES_KEY_LEN = 32 bytes).
void deriveEncryptionKey(uint8_t out[AES_KEY_LEN]) {
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts_ret(&sha, 0);
  mbedtls_sha256_update_ret(&sha, reinterpret_cast<const uint8_t *>(encryptionKey.c_str()), encryptionKey.length());
  mbedtls_sha256_finish_ret(&sha, out);
  mbedtls_sha256_free(&sha);
}

// Perform AES-256-CTR encryption or decryption (CTR is symmetric).
// `key` is the 32-byte AES key derived from the passphrase.
// `nonce` is a 16-byte initialization nonce (must be unique per message).
// `input` is the plaintext/ciphertext. `len` is the input length in bytes.
// `output` receives the ciphertext/plaintext (may alias `input`).
// Returns false if AES key setup fails.
bool aesCtrCrypt(const uint8_t key[AES_KEY_LEN], const uint8_t nonce[AES_BLOCK_LEN], const uint8_t *input, size_t len, uint8_t *output) {
  mbedtls_aes_context aes;
  uint8_t nonceCounter[AES_BLOCK_LEN];
  uint8_t streamBlock[AES_BLOCK_LEN];
  size_t offset = 0;

  memcpy(nonceCounter, nonce, AES_BLOCK_LEN);

  mbedtls_aes_init(&aes);
  if (mbedtls_aes_setkey_enc(&aes, key, AES_KEY_LEN * 8) != 0) {
    mbedtls_aes_free(&aes);
    // Zero stack buffers to minimize key material exposure.
    memset(nonceCounter, 0, sizeof(nonceCounter));
    memset(streamBlock, 0, sizeof(streamBlock));
    return false;
  }
  const int result = mbedtls_aes_crypt_ctr(&aes, len, &offset, nonceCounter, streamBlock, input, output);
  mbedtls_aes_free(&aes);
  // Zero stack buffers after use to reduce window for key material extraction.
  memset(nonceCounter, 0, sizeof(nonceCounter));
  memset(streamBlock, 0, sizeof(streamBlock));
  return result == 0;
}

// Encrypt a chat message for transmission.
//
// Wire format (before hex encoding):
//   [16 bytes nonce][ciphertext...]
// Total: AES_BLOCK_LEN + plainLen bytes, then hex-encoded to 2x that.
//
// Plaintext format (before encryption):
//   [sender name, up to 16 chars]\n[message, up to MAX_CHAT_TEXT_LEN chars]
//
// The nonce is generated from the ESP32 hardware RNG (esp_fill_random).
// Returns an empty string on encryption failure.
String encryptedPayloadFor(const String &message) {
  String plain = deviceName.substring(0, 16) + "\n" + message.substring(0, MAX_CHAT_TEXT_LEN);
  uint8_t key[AES_KEY_LEN];
  uint8_t nonce[AES_BLOCK_LEN];
  uint8_t cipher[64];
  uint8_t framed[AES_BLOCK_LEN + sizeof(cipher)];

  deriveEncryptionKey(key);
  esp_fill_random(nonce, sizeof(nonce));
  const size_t plainLen = min(static_cast<size_t>(plain.length()), sizeof(cipher));
  if (!aesCtrCrypt(key, nonce, reinterpret_cast<const uint8_t *>(plain.c_str()), plainLen, cipher)) {
    memset(key, 0, sizeof(key));
    memset(nonce, 0, sizeof(nonce));
    return "";
  }
  memcpy(framed, nonce, AES_BLOCK_LEN);
  memcpy(framed + AES_BLOCK_LEN, cipher, plainLen);

  // Zero sensitive stack data before returning.
  memset(key, 0, sizeof(key));
  memset(nonce, 0, sizeof(nonce));
  memset(cipher, 0, sizeof(cipher));

  return bytesToHex(framed, AES_BLOCK_LEN + plainLen);
}

// Decrypt a received chat message.
//
// Expects the body to be a hex-encoded [nonce|ciphertext] blob.
// On success, extracts the sender name into `sender` and the message into `message`.
// Returns false if decryption fails or the format is invalid.
bool decryptPayload(const String &body, String &sender, String &message) {
  uint8_t framed[MAX_BODY_LEN / 2];
  uint8_t key[AES_KEY_LEN];
  uint8_t plain[64];
  size_t framedLen = 0;

  if (!hexToBytes(body, framed, sizeof(framed), framedLen) || framedLen <= AES_BLOCK_LEN) return false;

  const size_t cipherLen = framedLen - AES_BLOCK_LEN;
  deriveEncryptionKey(key);

  if (!aesCtrCrypt(key, framed, framed + AES_BLOCK_LEN, cipherLen, plain)) {
    memset(key, 0, sizeof(key));
    memset(plain, 0, sizeof(plain));
    return false;
  }
  memset(key, 0, sizeof(key));

  // Build decoded string from plaintext, stopping at first NUL byte.
  String decoded;
  for (size_t i = 0; i < cipherLen; ++i) {
    if (plain[i] == 0) break;
    decoded += static_cast<char>(plain[i]);
  }
  memset(plain, 0, sizeof(plain));

  // Parse "sender\nmessage" format.
  const int separator = decoded.indexOf('\n');
  if (separator <= 0) return false;
  sender = decoded.substring(0, separator);
  message = decoded.substring(separator + 1);
  return sender.length() > 0 && message.length() > 0;
}