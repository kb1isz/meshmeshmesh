// ============================================================================
// features.cpp — New feature implementations
//
// Implements:
//  1. Message Fragmentation  — Multi-packet long messages
//  2. Store-and-Forward     — Flash-backed undelivered message queue
//  3. Persistent History    — Chat log saved to NVS flash
//  4. Contact List / Presence — Named contacts with online indicators
//  5. Emergency Messages     — Distress priority with rapid TX
//  6. GPS Position Sharing   — I2C GPS telemetry (ifdef-guarded)
//  7. Node Roles (Relay/Leaf) — Battery saving via role-based behavior
// ============================================================================

#include "globals.h"

// ============================================================================
//  Feature global definitions (declared extern in globals.h)
// ============================================================================
FragmentAssembly fragmentAssemblies[4];
StoredMessage storedMessages[STORE_FORWARD_MAX_MESSAGES];
ContactEntry contacts[16];
PositionData lastPosition;
NodeRole nodeRole = NodeRole::Relay;
uint32_t lastPositionBeaconAt = 0;
uint32_t lastContactDiscoveryAt = 0;
uint32_t lastFragServiceAt = 0;
uint32_t lastHistorySaveAt = 0;

// ============================================================================
//  1. Message Fragmentation
// ============================================================================

// Wire format for PACKET_TYPE_FRAGMENT body:
//   [12 hex chars header][...encrypted payload fragment as hex text...]
// Header bytes before hex encoding:
//   [1 byte total fragments][1 byte this fragment index][4 byte base message id]
// The encrypted payload is split across fragments. Each fragment contains a
// contiguous portion of the hex-encoded encrypted blob. The receiving end
// collects all fragments, concatenates them, then decrypts the full payload.

bool sendFragmented(const String &body, uint32_t destination, uint32_t conversation) {
#if ENABLE_MESSAGE_FRAGMENTATION
  (void)conversation;
  // Encrypt the full message first
  const String fullEncrypted = encryptedPayloadFor(body);
  if (fullEncrypted.length() == 0) { statusLine = "encryption failed"; drawBottom(); return false; }

  // Calculate how many fragments we need
  // Each fragment body carries twelve hex header chars plus hex payload, so only
  // half of the remaining packet body can be encrypted binary data.
  constexpr uint8_t FRAG_HEADER_CHARS = 12;
  constexpr uint8_t FRAG_HEADER_BYTES = 6;
  constexpr uint8_t FRAG_DATA_BYTES = (MAX_BODY_LEN - FRAG_HEADER_CHARS) / 2;

  // Convert hex payload to binary for splitting
  uint8_t encryptedBin[AES_BLOCK_LEN + 16 + 1 + MAX_LONG_CHAT_TEXT_LEN];
  size_t encryptedLen = 0;
  if (!hexToBytes(fullEncrypted, encryptedBin, sizeof(encryptedBin), encryptedLen)) {
    statusLine = "encryption too large"; drawBottom(); return false;
  }

  // Binary fragments: each carries a portion of the encrypted binary data
  // Fragment body format: hex([total][index][base id]) + hex(data)
  // Each packet gets its own messageId for ACK/retry; the base id in the body
  // lets the receiver reassociate the fragments.
  const uint32_t baseMsgId = nextMessageId++;
  const uint8_t totalFrags = (encryptedLen + FRAG_DATA_BYTES - 1) / FRAG_DATA_BYTES;
  if (totalFrags > MAX_FRAGMENTS) { statusLine = "too many fragments"; drawBottom(); return false; }
  nextMessageId = baseMsgId + totalFrags;
  if (nextMessageId == 0) nextMessageId = 1;

  for (uint8_t i = 0; i < totalFrags; ++i) {
    const size_t offset = static_cast<size_t>(i) * FRAG_DATA_BYTES;
    const size_t fragLen = min(encryptedLen - offset, static_cast<size_t>(FRAG_DATA_BYTES));

    uint8_t fragHeader[FRAG_HEADER_BYTES];
    fragHeader[0] = totalFrags;
    fragHeader[1] = i;
    fragHeader[2] = static_cast<uint8_t>(baseMsgId & 0xFF);
    fragHeader[3] = static_cast<uint8_t>((baseMsgId >> 8) & 0xFF);
    fragHeader[4] = static_cast<uint8_t>((baseMsgId >> 16) & 0xFF);
    fragHeader[5] = static_cast<uint8_t>((baseMsgId >> 24) & 0xFF);

    String fragBody = bytesToHex(fragHeader, sizeof(fragHeader));
    // Append the binary fragment data as hex string (to keep it text-safe)
    fragBody += bytesToHex(encryptedBin + offset, fragLen);

    // Queue each fragment as a separate pending message with a unique packet id.
    PendingMessage fragMsg;
    fragMsg.active = true;
    fragMsg.destination = destination;
    fragMsg.conversation = destination;
    fragMsg.messageId = baseMsgId + i;
    fragMsg.body = fragBody;
    fragMsg.fragmentCount = totalFrags;
    fragMsg.fragmentIndex = i;

    BleNode bleNode;
    fragMsg.prefersBle = destination != BROADCAST_NODE && findBleNode(destination, bleNode);

    if (!enqueuePendingMessage(fragMsg)) { statusLine = "frag queue full"; drawBottom(); return false; }
  }

  statusLine = "fragmented " + String(static_cast<int>(totalFrags)) + " parts";
  drawBottom();
  return true;
#else
  // Fragmentation disabled — fall through to normal send
  (void)destination;
  (void)conversation;
  queueOutgoing(body);
  return true;
#endif
}

void handleFragment(const Packet &packet) {
#if ENABLE_MESSAGE_FRAGMENTATION
  constexpr uint8_t FRAG_HEADER_CHARS = 12;
  constexpr uint8_t FRAG_HEADER_BYTES = 6;
  if (packet.body.length() < FRAG_HEADER_CHARS) return;
  uint8_t fragHeader[FRAG_HEADER_BYTES];
  size_t fragHeaderLen = 0;
  if (!hexToBytes(packet.body.substring(0, FRAG_HEADER_CHARS), fragHeader, sizeof(fragHeader), fragHeaderLen) ||
      fragHeaderLen != sizeof(fragHeader)) return;
  const uint8_t totalFrags = fragHeader[0];
  const uint8_t fragIndex = fragHeader[1];
  const uint32_t baseMsgId = static_cast<uint32_t>(fragHeader[2]) |
                             (static_cast<uint32_t>(fragHeader[3]) << 8) |
                             (static_cast<uint32_t>(fragHeader[4]) << 16) |
                             (static_cast<uint32_t>(fragHeader[5]) << 24);
  if (totalFrags == 0 || totalFrags > MAX_FRAGMENTS || fragIndex >= totalFrags) return;
  if (baseMsgId == 0) return;
  if (packet.body.length() < FRAG_HEADER_CHARS + 2) return;  // Need at least one byte of hex data

  packetStats.fragRx++;

  // Find or create an assembly slot
  FragmentAssembly *slot = nullptr;
  uint32_t now = millis();

  // First, expire stale assemblies
  for (auto &assembly : fragmentAssemblies) {
    if (assembly.active && now - assembly.startedAt > FRAGMENT_TIMEOUT_MS) {
      assembly.active = false;
      packetStats.fragTimeout++;
    }
  }

  for (auto &assembly : fragmentAssemblies) {
    if (assembly.active && assembly.source == packet.source && assembly.messageId == baseMsgId) {
      slot = &assembly;
      break;
    }
    if (!assembly.active && slot == nullptr) slot = &assembly;
  }

  if (slot == nullptr) return;  // No free assembly slot

  if (!slot->active) {
    slot->active = true;
    slot->source = packet.source;
    slot->messageId = baseMsgId;
    slot->total = totalFrags;
    slot->received = 0;
    slot->startedAt = now;
    // Initialize all parts as empty
    for (uint8_t i = 0; i < MAX_FRAGMENTS; ++i) slot->parts[i] = "";
  }

  // Check if we already have this fragment
  if (slot->received & (1 << fragIndex)) return;

  // Store the hex-encoded fragment payload (everything after the header)
  slot->parts[fragIndex] = packet.body.substring(FRAG_HEADER_CHARS);
  slot->received |= static_cast<uint8_t>(1 << fragIndex);

  // Check if all fragments are received
  if (slot->received == (static_cast<uint8_t>(1 << totalFrags) - 1)) {
    // Concatenate all fragment payloads
    String fullHex;
    for (uint8_t i = 0; i < totalFrags; ++i) {
      fullHex += slot->parts[i];
    }

    // Decrypt the full payload
    String sender, message;
    if (decryptPayload(fullHex, sender, message)) {
      const String line = sender.substring(0, 12) + ": " + message;
      if (packet.destination == BROADCAST_NODE) addGroupEntry(line, WHITE);
      else addDirectEntry(slot->source, sender, line, MAGENTA);
      statusLine = "frag assembled " + String(static_cast<int>(totalFrags)) + " parts";
      packetStats.fragAssembled++;
      drawUi();
    } else {
      packetStats.decryptFail++;
      statusLine = "frag decrypt failed";
    }

    slot->active = false;
  }
#else
  (void)packet;
#endif
}

void serviceFragments() {
#if ENABLE_MESSAGE_FRAGMENTATION
  const uint32_t now = millis();
  for (auto &assembly : fragmentAssemblies) {
    if (!assembly.active) continue;
    if (now - assembly.startedAt > FRAGMENT_TIMEOUT_MS) {
      assembly.active = false;
      packetStats.fragTimeout++;
    }
  }
#endif
}

// ============================================================================
//  2. Store-and-Forward — Long-term undelivered message queue
//
// Flow:
//  1. Normal delivery attempts (servicePending) exhaust retries
//  2. promotePendingToStored() copies the failed message into storedMessages[]
//  3. retryStoredMessages() retries periodically (every 60s)
//  4. triggerStoredMessageRetry(nodeId) retries immediately on HELLO/route events
//  5. persisted to NVS flash so messages survive reboot
// ============================================================================

// Persist stored messages to NVS flash.
// Saves count + per-message data so they survive reboot.
void persistStoredMessages() {
#if ENABLE_STORE_FORWARD
  uint8_t count = 0;
  for (const auto &sm : storedMessages) { if (sm.active) count++; }

  preferences.begin("store_fwd", false);
  preferences.putUChar("count", count);

  uint8_t idx = 0;
  for (const auto &sm : storedMessages) {
    if (!sm.active) continue;
    const String p = "s" + String(static_cast<int>(idx));
    preferences.putUInt((p + "_d").c_str(), sm.destination);
    preferences.putUInt((p + "_m").c_str(), sm.messageId);
    preferences.putUInt((p + "_c").c_str(), sm.conversation);
    preferences.putString((p + "_b").c_str(), sm.body);
    preferences.putString((p + "_t").c_str(), sm.rawText);
    preferences.putBool((p + "_dir").c_str(), sm.isDirect);
    preferences.putString((p + "_dn").c_str(), sm.destName);
    preferences.putUChar((p + "_a").c_str(), sm.attempts);
    idx++;
  }
  preferences.end();
#else
  // No-op
#endif
}

// Restore stored messages from NVS flash.
void loadStoredMessages() {
#if ENABLE_STORE_FORWARD
  preferences.begin("store_fwd", true);  // read-only

  const uint8_t count = preferences.getUChar("count", 0);
  for (uint8_t idx = 0; idx < count && idx < STORE_FORWARD_MAX_MESSAGES; ++idx) {
    const String p = "s" + String(static_cast<int>(idx));
    const uint32_t dest = preferences.getUInt((p + "_d").c_str(), 0);
    if (dest == 0) continue;  // skip invalid entries

    StoredMessage &sm = storedMessages[idx];
    sm.active = true;
    sm.destination = dest;
    sm.messageId = preferences.getUInt((p + "_m").c_str(), 0);
    sm.conversation = preferences.getUInt((p + "_c").c_str(), 0);
    sm.body = preferences.getString((p + "_b").c_str(), "");
    sm.rawText = preferences.getString((p + "_t").c_str(), "");
    sm.isDirect = preferences.getBool((p + "_dir").c_str(), false);
    sm.destName = preferences.getString((p + "_dn").c_str(), "");
    sm.attempts = preferences.getUChar((p + "_a").c_str(), 0);
    // Schedule first retry after the normal interval from boot
    sm.nextRetryAt = millis() + STORE_FORWARD_RETRY_MS;

    appPrintf("[sfwd] restored stored msg to %08lX (%s)\n",
              static_cast<unsigned long>(sm.destination), sm.rawText.c_str());
  }
  preferences.end();
#else
  // No-op
#endif
}

// Find a free slot and store a message. Persists to NVS.
void storeMessage(const StoredMessage &msg) {
#if ENABLE_STORE_FORWARD
  // Find free slot
  StoredMessage *slot = nullptr;
  for (auto &sm : storedMessages) {
    if (!sm.active) { slot = &sm; break; }
  }
  if (slot == nullptr) {
    statusLine = "store full";
    drawBottom();
    return;  // No space in store
  }

  *slot = msg;
  slot->nextRetryAt = millis() + STORE_FORWARD_RETRY_MS;

  persistStoredMessages();

  statusLine = "stored for later delivery";
  drawBottom();
#else
  (void)msg;
#endif
}

// Called immediately when a HELLO or route discovery indicates a node is reachable.
// Forces the next retry attempt for any stored messages destined for that node.
void triggerStoredMessageRetry(uint32_t nodeId) {
#if ENABLE_STORE_FORWARD
  if (nodeId == 0 || nodeId == BROADCAST_NODE) return;
  bool found = false;
  for (auto &sm : storedMessages) {
    if (!sm.active) continue;
    if (sm.destination != nodeId) continue;
    // Force immediate retry by setting nextRetryAt to now
    sm.nextRetryAt = millis();
    found = true;
  }
  if (found) {
    appPrintf("[sfwd] triggered retry for %08lX\n", static_cast<unsigned long>(nodeId));
  }
#else
  (void)nodeId;
#endif
}

void retryStoredMessages() {
#if ENABLE_STORE_FORWARD
  const uint32_t now = millis();
  bool anyChanged = false;
  for (auto &sm : storedMessages) {
    if (!sm.active) continue;
    if (timeBefore(sm.nextRetryAt)) continue;

    if (sm.attempts >= STORE_FORWARD_MAX_RETRIES) {
      // Expire the message
      sm.active = false;
      anyChanged = true;
      appPrintf("[sfwd] expired stored msg to %08lX (%s)\n",
                static_cast<unsigned long>(sm.destination), sm.rawText.c_str());
      continue;
    }

    // Check if we have a route now
    RouteEntry route;
    if (!findRoute(sm.destination, route)) {
      // Still no route — retry later after normal interval
      sm.nextRetryAt = now + STORE_FORWARD_RETRY_MS;
      continue;
    }

    // Enqueue as a proper PendingMessage so ACK tracking works.
    // The stored slot stays active until ACK received; pending clears it on success.
    // If pending fails (all retries exhausted), the stored slot remains for next try.
    sm.attempts++;
    sm.nextRetryAt = now + 600000;  // 10 min backstop — pending handles short-term retries

    PendingMessage msg;
    msg.active = true;
    msg.destination = sm.destination;
    msg.conversation = sm.conversation;
    msg.body = sm.body;
    msg.messageId = nextMessageId++;
    msg.storedSlotIndex = static_cast<uint8_t>(&sm - storedMessages);  // index of this slot

    if (enqueuePendingMessage(msg)) {
      packetStats.storedSent++;
      statusLine = "stored msg queued for delivery";
      drawBottom();
    } else {
      // Pending queue full — try again at normal interval
      sm.nextRetryAt = now + STORE_FORWARD_RETRY_MS;
    }
  }
  if (anyChanged) {
    persistStoredMessages();
  }
#else
  // No-op
#endif
}

// Called when servicePending() exhausts retries — promote the failed
// pending message into the long-term store-and-forward queue.
void promotePendingToStored() {
#if ENABLE_STORE_FORWARD
  if (!pending.active) return;
  if (pending.destination == BROADCAST_NODE) return;  // Don't store broadcast messages

  StoredMessage sm;
  sm.active = true;
  sm.destination = pending.destination;
  sm.messageId = pending.messageId;
  sm.conversation = pending.conversation;
  sm.body = pending.body;
  sm.isDirect = (pending.conversation != BROADCAST_NODE);
  sm.destName = nodeDisplayName(pending.destination);

  // Extract the original plaintext if we can find it in the chat buffer
  ChatBuffer &buf = activeChatBuffer();
  for (int8_t i = static_cast<int8_t>(buf.count) - 1; i >= 0; --i) {
    if (buf.entries[i].messageId == pending.messageId) {
      sm.rawText = buf.entries[i].text;
      break;
    }
  }
  if (sm.rawText.length() == 0) {
    sm.rawText = "(original text unavailable)";
  }

  storeMessage(sm);
#else
  // No-op
#endif
}

// ============================================================================
//  3. Persistent Chat History
// ============================================================================

void saveChatHistory() {
#if ENABLE_PERSISTENT_HISTORY
  // Save group chat buffer
  preferences.begin("chat_hist", false);
  preferences.putUChar("gc_count", groupChat.count);
  for (uint8_t i = 0; i < groupChat.count; ++i) {
    const String key = "gc_t" + String(static_cast<int>(i));
    preferences.putString(key.c_str(), groupChat.entries[i].text);
  }

  // Save direct chat buffers (up to 6)
  preferences.putUChar("dc_count", 0);
  uint8_t dcCount = 0;
  for (auto &dc : directChats) {
    if (dc.active) {
      const String prefix = "dc" + String(static_cast<int>(dcCount));
      preferences.putUInt((prefix + "_id").c_str(), dc.nodeId);
      preferences.putString((prefix + "_name").c_str(), dc.name);
      preferences.putUChar((prefix + "_cnt").c_str(), dc.buffer.count);
      for (uint8_t j = 0; j < dc.buffer.count; ++j) {
        const String key = prefix + "_t" + String(static_cast<int>(j));
        preferences.putString(key.c_str(), dc.buffer.entries[j].text);
      }
      dcCount++;
    }
  }
  preferences.putUChar("dc_count", dcCount);
  preferences.end();
  lastHistorySaveAt = millis();
#else
  // No-op
#endif
}

void loadChatHistory() {
#if ENABLE_PERSISTENT_HISTORY
  preferences.begin("chat_hist", true);  // read-only

  // Restore group chat
  const uint8_t gcCount = preferences.getUChar("gc_count", 0);
  for (uint8_t i = 0; i < gcCount; ++i) {
    const String key = "gc_t" + String(static_cast<int>(i));
    const String text = preferences.getString(key.c_str(), "");
    if (text.length() > 0) {
      addGroupEntry(text, WHITE);
    }
  }

  // Restore direct chats
  const uint8_t dcCount = preferences.getUChar("dc_count", 0);
  for (uint8_t i = 0; i < dcCount; ++i) {
    const String prefix = "dc" + String(static_cast<int>(i));
    const uint32_t nodeId = preferences.getUInt((prefix + "_id").c_str(), 0);
    const String name = preferences.getString((prefix + "_name").c_str(), "");
    const uint8_t cnt = preferences.getUChar((prefix + "_cnt").c_str(), 0);
    if (nodeId != 0) {
      DirectChat *dc = getDirectChat(nodeId, name);
      if (dc != nullptr) {
        for (uint8_t j = 0; j < cnt; ++j) {
          const String key = prefix + "_t" + String(static_cast<int>(j));
          const String text = preferences.getString(key.c_str(), "");
          if (text.length() > 0) {
            ChatEntry entry;
            entry.text = text;
            entry.color = WHITE;
            addEntryToBuffer(dc->buffer, entry);
          }
        }
      }
    }
  }

  preferences.end();
#else
  // No-op
#endif
}

// ============================================================================
//  4. Contact List / Presence
// ============================================================================

void addContact(uint32_t nodeId, const String &name, int16_t rssi) {
#if ENABLE_CONTACTS
  if (nodeId == 0 || nodeId == BROADCAST_NODE) return;

  // Check if contact already exists
  for (auto &contact : contacts) {
    if (contact.active && contact.nodeId == nodeId) {
      contact.lastSeenAt = millis();
      contact.lastRssi = rssi;
      contact.online = true;
      if (name.length() > 0) contact.name = name;
      return;
    }
  }

  // Find free slot
  ContactEntry *slot = nullptr;
  for (auto &contact : contacts) {
    if (!contact.active) { slot = &contact; break; }
  }
  if (slot == nullptr) return;  // Contact list full

  slot->active = true;
  slot->nodeId = nodeId;
  slot->name = name.length() > 0 ? name : nodeIdHex(nodeId).substring(4);
  slot->lastSeenAt = millis();
  slot->lastRssi = rssi;
  slot->online = true;
#else
  (void)nodeId;
  (void)name;
  (void)rssi;
#endif
}

void updateContactSeen(uint32_t nodeId, const String &name, int16_t rssi) {
#if ENABLE_CONTACTS
  // Update last-seen for matching contact
  for (auto &contact : contacts) {
    if (contact.active && contact.nodeId == nodeId) {
      contact.lastSeenAt = millis();
      contact.lastRssi = rssi;
      contact.online = true;
      if (name.length() > 0) contact.name = name;
      return;
    }
  }
  // Not found — add as new contact
  addContact(nodeId, name, rssi);
#else
  (void)nodeId;
  (void)name;
  (void)rssi;
#endif
}

bool findContact(uint32_t nodeId, ContactEntry &out) {
#if ENABLE_CONTACTS
  for (auto &contact : contacts) {
    if (contact.active && contact.nodeId == nodeId) {
      out = contact;
      return true;
    }
  }
  return false;
#else
  (void)nodeId;
  (void)out;
  return false;
#endif
}

void serviceContactDiscovery() {
#if ENABLE_CONTACTS
  const uint32_t now = millis();
  // Periodically send contact queries via broadcast
  if (now - lastContactDiscoveryAt > HELLO_INTERVAL_MS * 3) {
    lastContactDiscoveryAt = now;
    // Broadcast a contact query to discover nodes
    transmitPacket(PACKET_TYPE_CONTACT_QUERY, BROADCAST_NODE, esp_random(), 1, BROADCAST_NODE,
                   deviceName.substring(0, 16));
  }

  // Mark contacts as offline after 2x route TTL without contact
  for (auto &contact : contacts) {
    if (!contact.active) continue;
    if (now - contact.lastSeenAt > ROUTE_TTL_MS * 2) {
      contact.online = false;
    }
  }
#else
  // No-op
#endif
}

// ============================================================================
//  5. Emergency Priority Messages
// ============================================================================

void sendEmergency(const String &message) {
#if ENABLE_EMERGENCY_MSG
  String body = message.substring(0, MAX_CHAT_TEXT_LEN);
  body.trim();
  if (body.length() == 0) return;

  PendingMessage emsg;
  emsg.active = true;
  emsg.destination = BROADCAST_NODE;
  emsg.conversation = BROADCAST_NODE;
  emsg.messageId = nextMessageId++;
  emsg.body = encryptedPayloadFor(body);
  emsg.isEmergency = true;
  emsg.attempts = 0;

  if (emsg.body.length() == 0) { statusLine = "emergency encrypt failed"; drawBottom(); return; }

  // Emergency goes to the front of the pending queue
  // Use a slot-based insertion: put at head
  if (pendingQueueCount >= TX_QUEUE_DEPTH) {
    // Overwrite the oldest non-emergency pending message
    for (int8_t i = static_cast<int8_t>(pendingQueueCount) - 1; i >= 0; --i) {
      const uint8_t idx = static_cast<uint8_t>((pendingQueueHead + i) % TX_QUEUE_DEPTH);
      if (!pendingQueue[idx].isEmergency) {
        pendingQueue[idx] = emsg;
        packetStats.emergencyTx++;
        addGroupEntry("*** EMERGENCY ***: " + body, COLOR_ALERT, emsg.messageId, true, false);
        statusLine = "emergency queued";
        drawUi();
        return;
      }
    }
    statusLine = "emergency queue full";
    drawBottom();
    return;
  }

  // Put the emergency at the front of the pending queue.
  pendingQueueHead = (pendingQueueHead + TX_QUEUE_DEPTH - 1) % TX_QUEUE_DEPTH;
  pendingQueue[pendingQueueHead] = emsg;
  pendingQueueCount++;
  packetStats.emergencyTx++;

  addGroupEntry("*** EMERGENCY ***: " + body, COLOR_ALERT, emsg.messageId, true, false);
  statusLine = "emergency queued";
  drawUi();
#else
  (void)message;
  statusLine = "emergency messages disabled";
  drawBottom();
#endif
}

void handleEmergencyAck(const Packet &packet) {
#if ENABLE_EMERGENCY_MSG
  // Emergency ACK handling — same logic as regular ACK but with priority display
  if (pending.active && packet.messageId == pending.messageId && pending.isEmergency) {
    statusLine = "EMERGENCY delivered";
    markMessageAcked(packet.messageId);
    pending.active = false;
    drawUi();
  }
#else
  (void)packet;
#endif
}

// ============================================================================
//  6. GPS Position Sharing (ifdef-guarded, disabled by default)
// ============================================================================
#if ENABLE_GPS_TELEMETRY

// Placeholder I2C GPS handling — requires hardware-specific implementation
// for modules like NEO-6M, NEO-8M, BN-220, etc.
// The GPS fix acquisition and NMEA parsing would go here.

void serviceGps() {
  // TODO: Read from I2C GPS module (e.g., using TinyGPS++ library)
  // For now, this is a stub that clears the valid flag if no update received recently.
  const uint32_t now = millis();
  if (lastPosition.valid && now - lastPosition.updatedAt > GPS_FIX_INTERVAL_MS * 6) {
    lastPosition.valid = false;
  }
}

void sendPosition() {
  // Format: "lat:xx.xxxxxx,lon:yyy.yyyyyy,alt:####"
  // Encrypted like a regular chat message
  if (!lastPosition.valid) return;

  char buf[64];
  snprintf(buf, sizeof(buf), "lat:%.6f,lon:%.6f,alt:%.0f",
           lastPosition.latitude, lastPosition.longitude, lastPosition.altitude);

  String posBody = bytesToHex(reinterpret_cast<const uint8_t *>(buf), strlen(buf));
  transmitPacket(PACKET_TYPE_POSITION, BROADCAST_NODE, esp_random(), 1, BROADCAST_NODE, posBody);
  packetStats.positionTx++;
  lastPositionBeaconAt = millis();
}

void handlePositionData(const Packet &packet) {
  // Decode position data from a received packet
  const String &body = packet.body;
  if (body.length() < 10) return;

  // The body is a hex-encoded string like "lat:xx.xxxxxx,lon:yyy.yyyyyy,alt:####"
  uint8_t binary[64];
  size_t binLen = 0;
  if (!hexToBytes(body, binary, sizeof(binary), binLen)) return;

  String decoded;
  for (size_t i = 0; i < binLen; ++i) decoded += static_cast<char>(binary[i]);

  // Parse position data
  PositionData pos;
  pos.valid = true;
  pos.updatedAt = millis();

  // Parse "lat:xxx,lon:yyy,alt:zzz"
  // Using simple substring extraction
  int latIdx = decoded.indexOf("lat:");
  int lonIdx = decoded.indexOf("lon:");
  int altIdx = decoded.indexOf("alt:");

  if (latIdx >= 0 && lonIdx >= 0) {
    pos.latitude = decoded.substring(latIdx + 4, lonIdx - 1).toFloat();
    pos.longitude = decoded.substring(lonIdx + 4, altIdx >= 0 ? altIdx - 1 : decoded.length()).toFloat();
  }
  if (altIdx >= 0) {
    pos.altitude = decoded.substring(altIdx + 4).toFloat();
  }

  packetStats.positionRx++;
  statusLine = "pos from " + nodeIdHex(packet.source).substring(4);
  drawBottom();
}

void servicePositionBeacon() {
  if (millis() - lastPositionBeaconAt > POSITION_BROADCAST_INTERVAL_MS) {
    sendPosition();
  }
}
#endif  // ENABLE_GPS_TELEMETRY

// ============================================================================
//  7. Node Roles (Relay / Leaf)
// ============================================================================

NodeRole getNodeRole() {
  return nodeRole;
}

void setNodeRole(NodeRole role) {
  nodeRole = role;
  saveRadioSettings();  // Persist role preference alongside radio settings
  statusLine = role == NodeRole::Relay ? "role: relay" : "role: leaf";
  drawBottom();
}
