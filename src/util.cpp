// ============================================================================
// util.cpp — Utility functions: identity, routing, chat buffers, dedup
//
// Implements:
//  - Node ID derivation and display (eFuse MAC -> SplitMix64 -> node ID)
//  - Settings summary formatting
//  - Chat buffer management (group and direct conversations)
//  - Route table lookup and update
//  - Message deduplication via circular seen-message buffer
//  - Direct chat navigation helpers
// ============================================================================

#include "globals.h"

// Format a 32-bit node ID as 8 uppercase hex digits.
String nodeIdHex(uint32_t nodeId) {
  char buffer[9];
  snprintf(buffer, sizeof(buffer), "%08lX", static_cast<unsigned long>(nodeId));
  return String(buffer);
}

// Format the ESP32 eFuse MAC as a 16-character hex string.
String efuseMacHex() {
  const uint64_t mac = ESP.getEfuseMac();
  char buffer[17];
  snprintf(buffer, sizeof(buffer), "%08lX%08lX",
           static_cast<unsigned long>((mac >> 32) & 0xFFFFFFFFUL),
           static_cast<unsigned long>(mac & 0xFFFFFFFFUL));
  return String(buffer);
}

// Derive a stable node ID from the eFuse MAC using SplitMix64.
//
// SplitMix64 constants (from David Stafford's algorithm):
//   GOLDEN = 0x9E3779B97F4A7C15 (the golden ratio φ derivative)
//   MUL1   = 0xBF58476D1CE4E5B9
//   MUL2   = 0x94D049BB133111EB
//
// The result is truncated to 32 bits; if the result is BROADCAST_NODE (0),
// it is forced to 1 to avoid collision with the broadcast address.
uint32_t deriveNodeId(uint64_t efuseMac) {
  // Mix the eFuse MAC with SplitMix64 avalanche constants.
  uint64_t mixed = efuseMac + 0x9E3779B97F4A7C15ULL;  // SplitMix64: golden ratio
  mixed = (mixed ^ (mixed >> 30)) * 0xBF58476D1CE4E5B9ULL;
  mixed = (mixed ^ (mixed >> 27)) * 0x94D049BB133111EBULL;
  mixed ^= mixed >> 31;
  uint32_t nodeId = static_cast<uint32_t>(mixed ^ (mixed >> 32));
  if (nodeId == BROADCAST_NODE) nodeId = 1;
  return nodeId;
}

// Print the device identity to both the serial console and the chat display.
void printIdentity() {
  appPrintf("Identity:\n");
  appPrintf("  eFuse MAC: %s\n", efuseMacHex().c_str());
  appPrintf("  node id:   %s\n", nodeIdHex(localNodeId).c_str());
  appPrintf("  default:   %s\n", defaultDeviceName().c_str());
  appPrintf("  saved name:%s\n", deviceName.c_str());
  addLine("id " + nodeIdHex(localNodeId));
}

// Build a compact string summary of current radio settings.
String settingsSummary() {
  return String(radioSettings.frequency, 1) + "MHz bw" + String(radioSettings.bandwidth, 0) +
         " sf" + String(radioSettings.spreadingFactor) + " cr" + String(radioSettings.codingRate) +
         " pwr" + String(radioSettings.txPower);
}

uint16_t radioSettingsFingerprint() {
  uint8_t payload[16];
  const uint16_t frequency10 = static_cast<uint16_t>(radioSettings.frequency * 10.0f + 0.5f);
  const uint16_t bandwidth10 = static_cast<uint16_t>(radioSettings.bandwidth * 10.0f + 0.5f);
  const uint16_t hopMin10 = static_cast<uint16_t>(HOP_BAND_MIN_MHZ * 10.0f + 0.5f);
  const uint16_t hopMax10 = static_cast<uint16_t>(HOP_BAND_MAX_MHZ * 10.0f + 0.5f);
  payload[0] = static_cast<uint8_t>(frequency10 & 0xFF);
  payload[1] = static_cast<uint8_t>(frequency10 >> 8);
  payload[2] = static_cast<uint8_t>(bandwidth10 & 0xFF);
  payload[3] = static_cast<uint8_t>(bandwidth10 >> 8);
  payload[4] = radioSettings.spreadingFactor;
  payload[5] = radioSettings.codingRate;
  payload[6] = LORA_SYNC_WORD;
  payload[7] = ENABLE_FREQ_HOPPING ? 1 : 0;
  payload[8] = static_cast<uint8_t>(hopMin10 & 0xFF);
  payload[9] = static_cast<uint8_t>(hopMin10 >> 8);
  payload[10] = static_cast<uint8_t>(hopMax10 & 0xFF);
  payload[11] = static_cast<uint8_t>(hopMax10 >> 8);
  payload[12] = hopCount;
  payload[13] = 0;
  payload[14] = 0;
  payload[15] = 0;
  return crc16Ccitt(payload, sizeof(payload));
}

// Generate the default device name from the node ID.
String defaultDeviceName() {
  return "Node-" + nodeIdHex(localNodeId);
}

// Count active (non-expired) route entries.
uint8_t activeRouteCount() {
  uint8_t count = 0;
  const uint32_t now = millis();
  for (const auto &route : routes)
    if (route.active && now - route.lastSeenAt <= ROUTE_TTL_MS) count++;
  return count;
}

// Check if any messages are currently pending delivery.
bool hasPendingMessages() {
  return pending.active || pendingQueueCount > 0;
}

// --- Chat buffer helpers ---

// Add a chat entry to a buffer. The buffer holds up to 8 entries.
// When full, the oldest entry is evicted (shifted out).
void addEntryToBuffer(ChatBuffer &buffer, const ChatEntry &entry) {
  if (buffer.count < 8) {
    buffer.entries[buffer.count++] = entry;
  } else {
    for (uint8_t i = 1; i < 8; ++i) buffer.entries[i - 1] = buffer.entries[i];
    buffer.entries[7] = entry;
  }
}

// Find or create a direct chat conversation for a given node.
// If `name` is provided and non-empty, updates the display name.
// Evicts the oldest inactive slot if all are full.
DirectChat *getDirectChat(uint32_t nodeId, const String &name) {
  if (nodeId == 0 || nodeId == BROADCAST_NODE) return nullptr;
  DirectChat *slot = nullptr;
  for (auto &chat : directChats) {
    if (chat.active && chat.nodeId == nodeId) {
      if (name.length() > 0) chat.name = name.substring(0, 12);
      return &chat;
    }
    if (!chat.active && slot == nullptr) slot = &chat;
  }
  if (slot == nullptr) slot = &directChats[0];
  *slot = DirectChat();
  slot->active = true;
  slot->nodeId = nodeId;
  slot->name = name.length() > 0 ? name.substring(0, 12) : nodeIdHex(nodeId).substring(4);
  return slot;
}

// Get the chat buffer for the currently active conversation.
ChatBuffer &activeChatBuffer() {
  if (chatTab == ChatTab::Direct && selectedDirectNode != 0) {
    DirectChat *chat = getDirectChat(selectedDirectNode, "");
    if (chat != nullptr) return chat->buffer;
  }
  return groupChat;
}

// Add an entry to the group chat buffer.
void addGroupEntry(const String &line, uint16_t color, uint32_t messageId, bool outgoing, bool acked) {
  ChatEntry entry;
  entry.text = line; entry.color = color; entry.messageId = messageId;
  entry.outgoing = outgoing; entry.acked = acked; entry.direct = false;
  addEntryToBuffer(groupChat, entry);
}

// Add an entry to a direct chat buffer (creating it if needed).
void addDirectEntry(uint32_t nodeId, const String &name, const String &line, uint16_t color, uint32_t messageId, bool outgoing, bool acked) {
  DirectChat *chat = getDirectChat(nodeId, name);
  if (chat != nullptr) {
    ChatEntry entry;
    entry.text = line; entry.color = color; entry.messageId = messageId;
    entry.outgoing = outgoing; entry.acked = acked; entry.direct = true;
    addEntryToBuffer(chat->buffer, entry);
  }
}

// Add a simple status line to the active chat buffer.
void addLine(const String &line) {
  ChatEntry entry;
  entry.text = line;
  addEntryToBuffer(activeChatBuffer(), entry);
#ifdef BOARD_HELTEC_V3
  appPrintln(line);
#endif
}

// Mark a message as acknowledged in a specific chat buffer.
void markAckedInBuffer(ChatBuffer &buffer, uint32_t messageId) {
  for (uint8_t i = 0; i < buffer.count; ++i) {
    ChatEntry &entry = buffer.entries[i];
    if (entry.outgoing && entry.messageId == messageId) {
      entry.acked = true; entry.color = GREEN;
      if (!entry.text.endsWith(" ack")) entry.text += " ack";
    }
  }
}

// Mark a message as acknowledged across all chat buffers.
void markMessageAcked(uint32_t messageId) {
  markAckedInBuffer(groupChat, messageId);
  for (auto &chat : directChats)
    if (chat.active) markAckedInBuffer(chat.buffer, messageId);
}

// Mark an unacknowledged message as failed in a specific chat buffer.
void markFailedInBuffer(ChatBuffer &buffer, uint32_t messageId) {
  for (uint8_t i = 0; i < buffer.count; ++i) {
    ChatEntry &entry = buffer.entries[i];
    if (entry.outgoing && entry.messageId == messageId && !entry.acked) {
      entry.color = RED;
      if (!entry.text.endsWith(" failed")) entry.text += " failed";
    }
  }
}

// Mark a message as failed across all chat buffers.
void markMessageFailed(uint32_t messageId) {
  markFailedInBuffer(groupChat, messageId);
  for (auto &chat : directChats)
    if (chat.active) markFailedInBuffer(chat.buffer, messageId);
}

// Update the transport indicator (BLE vs LoRa) in a chat buffer.
void updateTransportInBuffer(ChatBuffer &buffer, uint32_t messageId, bool useBle) {
  for (uint8_t i = 0; i < buffer.count; ++i) {
    ChatEntry &entry = buffer.entries[i];
    if (!entry.outgoing || entry.messageId != messageId || entry.acked) continue;
    entry.color = useBle ? CYAN : YELLOW;
    const int start = entry.text.indexOf('[');
    const int end = entry.text.indexOf(']');
    if (start >= 0 && end > start)
      entry.text = entry.text.substring(0, start + 1) + String(useBle ? "BLE ..." : "LoRa ...") + entry.text.substring(end);
  }
}

// Update the transport indicator across all chat buffers.
void updateMessageTransport(uint32_t messageId, bool useBle) {
  updateTransportInBuffer(groupChat, messageId, useBle);
  for (auto &chat : directChats)
    if (chat.active) updateTransportInBuffer(chat.buffer, messageId, useBle);
}

// --- Route and name helpers ---

// Switch to group chat mode (broadcast destination).
void selectGroupChat() {
  chatTab = ChatTab::Group;
  selectedDestination = BROADCAST_NODE;
  statusLine = "group chat";
}

// Switch to direct chat with a specific node.
void selectDirectChat(uint32_t nodeId) {
  if (nodeId == 0 || nodeId == BROADCAST_NODE) { statusLine = "no direct nodes"; return; }
  chatTab = ChatTab::Direct;
  selectedDirectNode = nodeId;
  selectedDestination = nodeId;
  getDirectChat(nodeId, nodeDisplayName(nodeId));
  statusLine = "direct " + nodeIdHex(nodeId).substring(4);
}

// Cycle to the next available direct chat node.
// Collects candidates from BLE nodes, routes, and existing conversations.
void selectNextDirectChat() {
  uint32_t candidates[18];
  uint8_t count = 0;
  const uint32_t now = millis();
  auto addCandidate = [&](uint32_t id) {
    if (id == 0 || id == BROADCAST_NODE || count >= 18) return;
    for (uint8_t i = 0; i < count; ++i) if (candidates[i] == id) return;
    candidates[count++] = id;
  };

  for (const auto &node : bleNodes)
    if (node.active && now - node.lastSeenAt <= BLE_NODE_TTL_MS) addCandidate(node.nodeId);
  for (const auto &route : routes)
    if (route.active && now - route.lastSeenAt <= ROUTE_TTL_MS) addCandidate(route.destination);
  for (const auto &chat : directChats)
    if (chat.active) addCandidate(chat.nodeId);

  if (count == 0) { statusLine = "no direct nodes"; return; }
  uint8_t next = 0;
  for (uint8_t i = 0; i < count; ++i)
    if (candidates[i] == selectedDirectNode) { next = static_cast<uint8_t>((i + 1) % count); break; }
  selectDirectChat(candidates[next]);
}

// Get the label for the direct chat tab (display name or "Direct").
String directChatLabel() {
  if (selectedDirectNode == 0) return "Direct";
  return nodeDisplayName(selectedDirectNode);
}

// Get the best available display name for a node.
// Prefers BLE-discovered name, then direct chat name, then node ID hex.
String nodeDisplayName(uint32_t nodeId) {
  const uint32_t now = millis();
  for (const auto &node : bleNodes)
    if (node.active && node.nodeId == nodeId && now - node.lastSeenAt <= BLE_NODE_TTL_MS && node.name.length() > 0)
      return node.name.substring(0, 12);
  for (const auto &chat : directChats)
    if (chat.active && chat.nodeId == nodeId && chat.name.length() > 0)
      return chat.name.substring(0, 12);
  return nodeIdHex(nodeId).substring(4);
}

// Get the first available direct node ID (from BLE nodes, routes, or chats).
uint32_t firstDirectNode() {
  const uint32_t now = millis();
  for (const auto &node : bleNodes)
    if (node.active && now - node.lastSeenAt <= BLE_NODE_TTL_MS) return node.nodeId;
  for (const auto &route : routes)
    if (route.active && route.destination != BROADCAST_NODE && now - route.lastSeenAt <= ROUTE_TTL_MS) return route.destination;
  for (const auto &chat : directChats)
    if (chat.active) return chat.nodeId;
  return 0;
}

// Update or create a route entry. Only overwrites an existing entry if the
// new route has fewer hops or the old entry has expired.
void updateRoute(uint32_t destination, uint32_t nextHop, uint8_t hops) {
  if (destination == 0 || destination == localNodeId || nextHop == 0 || nextHop == localNodeId) return;

  RouteEntry *slot = nullptr;
  for (auto &route : routes) {
    if (route.active && route.destination == destination) { slot = &route; break; }
    if (!route.active && slot == nullptr) slot = &route;
  }
  if (slot == nullptr) slot = &routes[0];

  // Only replace if this route is better (fewer hops) or the old entry expired.
  if (!slot->active || slot->destination != destination || hops <= slot->hops || millis() - slot->lastSeenAt > ROUTE_TTL_MS) {
    slot->active = true;
    slot->destination = destination;
    slot->nextHop = nextHop;
    slot->hops = hops;
    packetStats.routeUpdates++;
    aodvLine = "route " + nodeIdHex(destination).substring(4) + " via " + nodeIdHex(nextHop).substring(4);
  }
  slot->lastSeenAt = millis();
}

// Find a route to the given destination. Checks BLE nodes first (1-hop),
// then the route table. Returns true if a valid route was found.
bool findRoute(uint32_t destination, RouteEntry &out) {
  const uint32_t now = millis();
  BleNode bleNode;
  if (findBleNode(destination, bleNode)) {
    out.active = true; out.destination = destination; out.nextHop = destination;
    out.hops = 1; out.lastSeenAt = bleNode.lastSeenAt;
    return true;
  }
  for (const auto &route : routes)
    if (route.active && route.destination == destination && now - route.lastSeenAt <= ROUTE_TTL_MS) { out = route; return true; }
  return false;
}

// ============================================================================
//  Battery monitoring
// ============================================================================

// Read the battery voltage from the ADC pin.
// Converts raw ADC reading (12-bit, 0-4095) to voltage at the battery,
// accounting for the 1:2 resistor divider and 3.3V ADC reference.
float readBatteryVoltage() {
  const uint16_t raw = analogRead(BATTERY_ADC_PIN);
  const float vAdc = static_cast<float>(raw) * BATTERY_ADC_REF_VOLTAGE / static_cast<float>(BATTERY_ADC_MAX);
  return vAdc * BATTERY_VOLTAGE_DIVIDER;
}

// Map battery voltage to a percentage (linear interpolation, clamped 0-100).
uint8_t computeBatteryPercent(float voltage) {
  if (voltage >= BATTERY_VOLTAGE_MAX) return 100;
  if (voltage <= BATTERY_VOLTAGE_MIN) return 0;
  const float pct = (voltage - BATTERY_VOLTAGE_MIN) /
                    (BATTERY_VOLTAGE_MAX - BATTERY_VOLTAGE_MIN) * 100.0f;
  return static_cast<uint8_t>(pct);
}

// Service routine: read battery at interval, update globals.
void serviceBattery() {
  const uint32_t now = millis();
  if (now - lastBatteryReadAt < BATTERY_READ_INTERVAL_MS) return;
  lastBatteryReadAt = now;
  batteryVoltage = readBatteryVoltage();
  batteryPercent = computeBatteryPercent(batteryVoltage);
}

// Check if a message has already been seen (deduplication).
// If not seen, records it in the circular seen-message buffer.
// Returns true if the message is a duplicate.
bool isDuplicate(uint32_t source, uint32_t messageId) {
  for (const auto &entry : seen)
    if (entry.source == source && entry.messageId == messageId) return true;
  seen[seenNext].source = source;
  seen[seenNext].messageId = messageId;
  seenNext = static_cast<uint8_t>((seenNext + 1) % 32);
  return false;
}
