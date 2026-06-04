// ============================================================================
// protocol.cpp — Wire packet encoding/decoding, CRC16, and node ID parsing
//
// Implements the on-wire packet format defined in config.h:
//   [2 magic][1 version][1 type][4 source][4 dest][4 msgId][1 bodyLen][1 ttl]
//   [4 nextHop][4 prevHop][bodyLen bytes body + zero-padding][2 CRC16-CCITT]
//
// All multi-byte integers are little-endian.
// ============================================================================

#include "globals.h"

// Compute CRC-16/CCITT (0xFFFF initial, 0x1021 polynomial).
// Used to detect corruption on received packets.
uint16_t crc16Ccitt(const uint8_t *data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; ++i) {
    crc ^= static_cast<uint16_t>(data[i]) << 8;
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021) : static_cast<uint16_t>(crc << 1);
    }
  }
  return crc;
}

// Write a 32-bit value in little-endian byte order.
void writeU32(uint8_t *buffer, size_t offset, uint32_t value) {
  buffer[offset] = static_cast<uint8_t>(value & 0xFF);
  buffer[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
  buffer[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
  buffer[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

// Read a 32-bit value in little-endian byte order.
uint32_t readU32(const uint8_t *buffer, size_t offset) {
  return static_cast<uint32_t>(buffer[offset]) |
         (static_cast<uint32_t>(buffer[offset + 1]) << 8) |
         (static_cast<uint32_t>(buffer[offset + 2]) << 16) |
         (static_cast<uint32_t>(buffer[offset + 3]) << 24);
}

// Encode a mesh packet into the wire format buffer.
//
// The output buffer `out` must be at least MAX_PACKET_LEN bytes.
// The body is copied up to MAX_BODY_LEN bytes; the remaining body area is
// zero-padded. The CRC is computed over the entire MAX_PACKET_LEN - 2 bytes
// (header + full body area), meaning trailing zeros contribute to the CRC.
// This is intentional: a receiver that gets a truncated packet will fail CRC.
//
// Returns the total encoded length (always MAX_PACKET_LEN).
size_t encodePacket(uint8_t type, uint32_t source, uint32_t destination, uint32_t messageId, uint8_t ttl, uint32_t nextHop, const String &body, uint8_t *out) {
  memset(out, 0, MAX_PACKET_LEN);
  const uint8_t bodyLen = min(static_cast<size_t>(body.length()), static_cast<size_t>(MAX_BODY_LEN));
  out[0] = 'T';
  out[1] = 'D';
  out[2] = PACKET_VERSION;
  out[3] = type;
  writeU32(out, 4, source);
  writeU32(out, 8, destination);
  writeU32(out, 12, messageId);
  out[16] = bodyLen;
  out[17] = ttl;
  writeU32(out, 18, nextHop);
  writeU32(out, 22, localNodeId);  // previousHop
  memcpy(out + HEADER_LEN, body.c_str(), bodyLen);
  // CRC covers only the actual header + body (not the zero-padded area to
  // MAX_PACKET_LEN). The SX1262 in explicit header mode returns only the
  // actual payload length (HEADER_LEN + bodyLen + 2 CRC bytes), so the
  // receiver never sees the zero padding. Computing CRC over the padded
  // area would cause permanent CRC mismatch on receive.
  const size_t actualLen = HEADER_LEN + bodyLen;
  const uint16_t crc = crc16Ccitt(out, actualLen);
  out[actualLen] = static_cast<uint8_t>(crc & 0xFF);
  out[actualLen + 1] = static_cast<uint8_t>((crc >> 8) & 0xFF);
  return actualLen + 2;
}

// Decode a wire-format packet into a Packet struct.
//
// Validates magic bytes ('T','D'), version, body length, CRC, and that
// the packet is not from ourselves or looping back to us.
// The buffer may be shorter than MAX_PACKET_LEN — SX1262 explicit header
// mode returns only HEADER_LEN + bodyLen + 2 CRC bytes.
//
// Returns false if any validation fails.
bool decodePacket(const uint8_t *buffer, size_t len, Packet &packet) {
  if (len < HEADER_LEN + 2 || buffer[0] != 'T' || buffer[1] != 'D' || buffer[2] != PACKET_VERSION) return false;
  const uint8_t bodyLen = buffer[16];
  if (bodyLen > MAX_BODY_LEN) return false;
  // Expected wire length from the bodyLen field. Accept if the actual
  // length matches (SX1262 returns the exact on-air payload length).
  const size_t expectedLen = HEADER_LEN + bodyLen + 2;
  if (len != expectedLen) return false;
  const uint16_t expectedCrc = static_cast<uint16_t>(buffer[len - 2]) | (static_cast<uint16_t>(buffer[len - 1]) << 8);
  if (crc16Ccitt(buffer, len - 2) != expectedCrc) return false;

  packet.type = buffer[3];
  packet.source = readU32(buffer, 4);
  packet.destination = readU32(buffer, 8);
  packet.messageId = readU32(buffer, 12);
  packet.ttl = buffer[17];
  packet.nextHop = readU32(buffer, 18);
  packet.previousHop = readU32(buffer, 22);
  packet.body = "";
  for (uint8_t i = 0; i < bodyLen; ++i) packet.body += static_cast<char>(buffer[HEADER_LEN + i]);
  // Reject packets from ourselves (don't process our own transmissions)
  // or where we are the previous hop (loop prevention).
  return packet.source != localNodeId && packet.previousHop != localNodeId &&
         (packet.nextHop == BROADCAST_NODE || packet.nextHop == localNodeId);
}

// Parse a hex node ID from a string (with optional "0x" prefix).
// Accepts 1-8 hex digits. Sets `nodeId` on success.
// Returns false for empty strings, >8 hex digits, or result of 0.
bool parseNodeId(const String &text, uint32_t &nodeId) {
  String value = text;
  value.trim();
  if (value.startsWith("0x") || value.startsWith("0X")) value = value.substring(2);
  if (value.length() == 0 || value.length() > 8) return false;
  nodeId = static_cast<uint32_t>(strtoul(value.c_str(), nullptr, 16));
  return nodeId != 0;
}