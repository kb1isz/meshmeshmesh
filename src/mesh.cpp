// ============================================================================
// mesh.cpp — Mesh routing, transmit queues, ACK handling, and packet dispatch
// ============================================================================
#include "globals.h"

void noteTx(uint8_t type) {
  packetStats.tx++;
  if (type == PACKET_TYPE_DATA) packetStats.dataTx++;
  else if (type == PACKET_TYPE_ACK) packetStats.ackTx++;
  else if (type == PACKET_TYPE_HELLO) packetStats.helloTx++;
  else if (type == PACKET_TYPE_RREQ) packetStats.rreqTx++;
  else if (type == PACKET_TYPE_RREP) packetStats.rrepTx++;
}

void noteRx(uint8_t type) {
  packetStats.rx++;
  if (type == PACKET_TYPE_DATA) packetStats.dataRx++;
  else if (type == PACKET_TYPE_ACK) packetStats.ackRx++;
  else if (type == PACKET_TYPE_HELLO) packetStats.helloRx++;
  else if (type == PACKET_TYPE_RREQ) packetStats.rreqRx++;
  else if (type == PACKET_TYPE_RREP) packetStats.rrepRx++;
}

// --- Transmit queue helpers ---

bool enqueueLoRaPacket(const uint8_t *encoded, size_t len, uint8_t type) {
  if (!radioStarted || len == 0 || len > MAX_PACKET_LEN) return false;
  portENTER_CRITICAL(&loraQueueMux);
  if (loraQueueCount >= TX_QUEUE_DEPTH) { portEXIT_CRITICAL(&loraQueueMux); statusLine = "LoRa queue full"; return false; }

  if (type == PACKET_TYPE_ACK) {
    loraQueueHead = (loraQueueHead + TX_QUEUE_DEPTH - 1) % TX_QUEUE_DEPTH;
    LoRaTxJob &job = loraQueue[loraQueueHead];
    memcpy(job.encoded, encoded, len);
    job.len = len; job.type = type; job.active = true;
  } else {
    LoRaTxJob &job = loraQueue[loraQueueTail];
    memcpy(job.encoded, encoded, len);
    job.len = len; job.type = type; job.active = true;
    loraQueueTail = (loraQueueTail + 1) % TX_QUEUE_DEPTH;
  }
  loraQueueCount++;
  portEXIT_CRITICAL(&loraQueueMux);
  statusLine = "queued LoRa";
  return true;
}

bool dequeueLoRaPacket(LoRaTxJob &job) {
  portENTER_CRITICAL(&loraQueueMux);
  if (loraQueueCount == 0) { portEXIT_CRITICAL(&loraQueueMux); return false; }
  job = loraQueue[loraQueueHead];
  loraQueue[loraQueueHead].active = false;
  loraQueueHead = (loraQueueHead + 1) % TX_QUEUE_DEPTH;
  loraQueueCount--;
  portEXIT_CRITICAL(&loraQueueMux);
  return true;
}

void scheduleLoRaBackoff(uint32_t minMs, uint32_t maxMs) {
  if (maxMs < minMs) maxMs = minMs;
  loraClearChannelAt = millis() + random(minMs, maxMs + 1);
  loraBackoffActive = true;
}

// FHSS TX gating: one packet per slot, and if we received in the previous
// slot we stay silent to give other devices (especially booters) a clean window.
#if ENABLE_FREQ_HOPPING
static bool hopTxAllowed() {
  // Rule 1: no TX if we already transmitted this slot
  if (hopSlot == hopLastTxSlot) return false;
  // Rule 2: no TX if we received a valid packet in the previous slot
  if (hopLastRxSlot != 0xFF) {
    const uint8_t nextSlot = static_cast<uint8_t>((hopLastRxSlot + 1) % hopCount);
    if (hopSlot == nextSlot) return false;
  }
  return true;
}
#endif

bool loraClearToSend() {
  if (!radioStarted || radioTransmitting) return false;
  const bool hadPendingRx = radioPacketReceived;
  radioPacketReceived = false;
  const int state = radio.scanChannel();
  if (radioPacketReceived) { /* ISR fired during scan */ }
  else { radioPacketReceived = hadPendingRx; }
  if (state == RADIOLIB_CHANNEL_FREE) { loraBackoffActive = false; return true; }
  radio.startReceive();
  if (state == RADIOLIB_PREAMBLE_DETECTED || state == RADIOLIB_LORA_DETECTED) {
    scheduleLoRaBackoff(LORA_CCA_BUSY_MIN_MS, LORA_CCA_BUSY_MAX_MS);
    statusLine = "channel busy; backoff"; drawBottom();
    return false;
  }
  statusLine = "CAD err " + String(state) + "; sending"; drawBottom();
  loraBackoffActive = false;
  return true;
}

bool startLoRaJob(const LoRaTxJob &job) {
  if (!radioStarted || radioTransmitting || !job.active || job.len == 0 || job.len > MAX_PACKET_LEN) return false;
  radioPacketReceived = false;
  const int state = radio.startTransmit(job.encoded, job.len);
  if (state != RADIOLIB_ERR_NONE) { radio.startReceive(); statusLine = "LoRa tx failed: " + String(state); return false; }
  loraTx = job;
  loraTx.startedAt = millis();
  loraTx.active = true;
  radioTransmitting = true;
  return true;
}

void serviceLoRaTransmit() {
  if (!radioStarted) return;
  if (!radioTransmitting) {
    if (radioPacketReceived) return;
    if (loraQueueCount == 0) { loraBackoffActive = false; return; }
    if (!loraBackoffActive) {
      portENTER_CRITICAL(&loraQueueMux);
      const bool isAck = (loraQueueCount > 0 && loraQueue[loraQueueHead].active
                          && loraQueue[loraQueueHead].type == PACKET_TYPE_ACK);
      portEXIT_CRITICAL(&loraQueueMux);
      if (isAck) { scheduleLoRaBackoff(5, 15); }
      else { scheduleLoRaBackoff(LORA_CCA_INITIAL_MIN_MS, LORA_CCA_INITIAL_MAX_MS); }
      return;
    }
    if (timeBefore(loraClearChannelAt)) return;
    if (!loraClearToSend()) return;
#if ENABLE_FREQ_HOPPING
    // One TX per slot, and no TX in the slot after an RX.
    if (!hopTxAllowed()) return;
#endif
    LoRaTxJob next;
    if (dequeueLoRaPacket(next)) startLoRaJob(next);
    return;
  }
  if (!loraTx.active) { radioTransmitting = false; return; }
  const bool timedOut = millis() - loraTx.startedAt > LORA_TX_TIMEOUT_MS;
  if (!radioPacketReceived && !timedOut) return;
  radioPacketReceived = false;
  const int state = radio.finishTransmit();
  radioTransmitting = false;
  if (state == RADIOLIB_ERR_NONE && !timedOut) { noteTx(loraTx.type); statusLine = "LoRa sent";
#if ENABLE_FREQ_HOPPING
    hopLastTxSlot = hopSlot;
#endif
  }
  else statusLine = timedOut ? "LoRa tx timeout" : "LoRa tx failed: " + String(state);
  loraTx.active = false;
  radio.startReceive();
}

void serviceRadio() {
  if (!radioStarted || radioTransmitting || !radioPacketReceived) return;
  radioPacketReceived = false;
  uint8_t buffer[MAX_PACKET_LEN];
  const int state = radio.readData(buffer, sizeof(buffer));
  if (state == RADIOLIB_ERR_NONE) {
    const size_t packetLen = radio.getPacketLength();
    // Skip false ISR triggers (noise at ~-111 RSSI) that cause readData()
    // to return ERR_NONE but getPacketLength() returns 0.
    if (packetLen == 0) {
#if ENABLE_FREQ_HOPPING
      appPrintf("[dbg] RX len=0 rssi=%d (noise trigger) slot=%u freq=%.1f MHz synced=%c\n",
                static_cast<int>(radio.getRSSI()),
                hopSlot,
                static_cast<double>(hoppingSynced ? hopChannels[hopSlot] : hopChannels[0]),
                hoppingSynced ? 'Y' : 'N');
#endif
      return;
    }
    Packet packet;
#if ENABLE_FREQ_HOPPING
    const bool syncState = hoppingSynced;
    const uint8_t rxSlot = hopSlot;
    // Report the actual frequency the radio is tuned to, independent of sync state.
    const float rxFreq = hoppingSynced ? hopChannels[hopSlot] : (radioStarted ? hopChannels[0] : radioSettings.frequency);
#endif
    const bool decoded = decodePacket(buffer, packetLen, packet);
    if (decoded) {
      noteRx(packet.type);
      packet.rssi = static_cast<int16_t>(radio.getRSSI());
      handleIncoming(packet);
#if ENABLE_FREQ_HOPPING
      // Track the slot where we received a valid packet.
      // If we RX in slot N, we skip TX in slot N+1 to give other
      // devices a clean window (especially booters on the rendezvous channel).
      hopLastRxSlot = hopSlot;
#endif
    }
#if ENABLE_FREQ_HOPPING
    // DEBUG: log every received packet with hop context
    {
      appPrintf("[dbg] RX len=%u rssi=%d decode=%c slot=%u freq=%.1f MHz synced=%c\n",
                packetLen, static_cast<int>(radio.getRSSI()),
                decoded ? 'Y' : 'N',
                rxSlot, static_cast<double>(rxFreq), syncState ? 'Y' : 'N');
      if (!decoded) {
        // Dump raw first bytes of failed packet
        appPrintf("[dbg] RAW pkt=%02x %02x %02x %02x %02x %02x\n",
                  buffer[0], buffer[1], buffer[2], buffer[3], buffer[4], buffer[5]);
      }
      if (decoded && packet.type == PACKET_TYPE_HELLO) {
        appPrintf("[dbg] HELLO body=\"%s\"\n", packet.body.c_str());
      }
    }
#endif
  }
  radio.startReceive();
}

bool transmitPacketFrom(uint8_t type, uint32_t source, uint32_t destination, uint32_t messageId, uint8_t ttl, uint32_t nextHop, const String &body) {
  uint8_t encoded[MAX_PACKET_LEN];
  const size_t len = encodePacket(type, source, destination, messageId, ttl, nextHop, body, encoded);
  bool sentBle = false;
  if (nextHop != BROADCAST_NODE) {
    BleNode bleNode;
    const bool bleReachable = findBleNode(nextHop, bleNode);
    if (bleReachable) sentBle = enqueueBlePacket(nextHop, encoded, len, type);
    if (sentBle || bleReachable) { if (sentBle) noteTx(type); return sentBle; }
  }
  if (nextHop == BROADCAST_NODE) {
    sentBle = hasBleTargets() && enqueueBlePacket(BROADCAST_NODE, encoded, len, type);
    if (sentBle) { noteTx(type); if (type == PACKET_TYPE_DATA) statusLine = "sent BLE; queued LoRa"; }
  }
  if (!radioStarted) return sentBle;
  return enqueueLoRaPacket(encoded, len, type) || sentBle;
}

bool transmitPacket(uint8_t type, uint32_t destination, uint32_t messageId, uint8_t ttl, uint32_t nextHop, const String &body) {
  return transmitPacketFrom(type, localNodeId, destination, messageId, ttl, nextHop, body);
}

void sendAck(const Packet &packet) {
#if ENABLE_BLE_MESH
  BleNode bleNode;
  if (findBleNode(packet.source, bleNode)) {
    uint8_t ackPacket[MAX_PACKET_LEN];
    const size_t len = encodePacket(PACKET_TYPE_ACK, localNodeId, packet.source, packet.messageId, MESH_DEFAULT_TTL, BROADCAST_NODE, "", ackPacket);
    if (sendBlePacketToBlocking(packet.source, ackPacket, len)) return;
  }
#endif
  RouteEntry route;
  const uint32_t nextHop = findRoute(packet.source, route) ? route.nextHop : packet.previousHop;
  transmitPacket(PACKET_TYPE_ACK, packet.source, packet.messageId, MESH_DEFAULT_TTL, nextHop, "");
}

// HELLO beacon carries hop sync data for epoch-based FHSS.
// Format: "Name//slot//nt" — e.g., "Node-1234//3//45000"
// - slot: current hopSlot index
// - nt: sender's hopNetworkTime() at transmission time (for epoch offset calc)
//   Using network time instead of local millis() is critical: it allows the
//   receiver to compute the correct hopEpochOffset regardless of the sender's
//   own offset, because network time is the synchronized reference.
void sendHello() {
#if ENABLE_FREQ_HOPPING
  // When hopping, use immediate TX so the HELLO carries the correct
  // frequency and network-time for the current slot. Queued TX (via
  // transmitPacket) would encode stale data by the time CCA backoff
  // completes and serviceHopping() may have already changed frequencies.
  sendHelloImmediate();
#else
  String body = deviceName.substring(0, 10) + "//" + String(hopSlot) + "//" + String(static_cast<int32_t>(hopNetworkTime()));
  transmitPacket(PACKET_TYPE_HELLO, BROADCAST_NODE, esp_random(), 1, BROADCAST_NODE, body);
  lastHelloAt = millis();
  aodvLine = "HELLO sent";
#endif
}

// Send a HELLO reply immediately via direct TX, bypassing the queue and CCA.
// This is critical for frequency hopping: the reply must go out NOW on the
// current frequency before serviceHopping() changes it. The normal sendHello()
// uses transmitPacket() which enqueues and may have 25-900ms backoff.
// If trackSlot is true (default), hopLastTxSlot is updated so the one-TX-per-slot
// gate applies. Set trackSlot=false for the proactive rendezvous HELLO to avoid
// blocking data/ACK TX in the same slot.
// Returns true if the HELLO was actually sent, false if the radio was busy.
bool sendHelloImmediate(bool trackSlot) {
  if (!radioStarted || radioTransmitting) return false;
  String body = deviceName.substring(0, 10) + "//" + String(hopSlot) + "//" + String(static_cast<int32_t>(hopNetworkTime()));
  uint8_t enc[MAX_PACKET_LEN];
  const size_t len = encodePacket(PACKET_TYPE_HELLO, localNodeId, BROADCAST_NODE,
                                   esp_random(), 1, BROADCAST_NODE, body, enc);
  if (len == 0) return false;
  radioPacketReceived = false;
  const int txState = radio.startTransmit(enc, len);
  if (txState != RADIOLIB_ERR_NONE) {
    radio.startReceive();
    statusLine = "HELLO tx failed: " + String(txState);
    return false;
  }
  const uint32_t txStartedAt = millis();
#if ENABLE_FREQ_HOPPING
  const uint32_t txTimeoutMs = max(static_cast<uint32_t>(500), (hopIntervalMs / 2) + 250);
#else
  const uint32_t txTimeoutMs = LORA_TX_TIMEOUT_MS;
#endif
  while (!radioPacketReceived && millis() - txStartedAt < txTimeoutMs) delay(1);
  const bool timedOut = millis() - txStartedAt >= txTimeoutMs;
  const int finishState = radio.finishTransmit();
  radioTransmitting = false; loraTx.active = false;
  radio.startReceive();
  if (timedOut || finishState != RADIOLIB_ERR_NONE) {
    statusLine = timedOut ? "HELLO tx timeout" : "HELLO tx failed: " + String(finishState);
    return false;
  }
  aodvLine = "HELLO immediate";
  noteTx(PACKET_TYPE_HELLO);
  lastHelloAt = millis();
#if ENABLE_FREQ_HOPPING
  if (trackSlot) {
    hopLastTxSlot = hopSlot;
  }
#endif
  return true;
}

void serviceBeacon() {
  if (millis() - lastHelloAt > HELLO_INTERVAL_MS) sendHello();
}

void sendRouteRequest(uint32_t target) {
  if (target == BROADCAST_NODE) return;
  transmitPacket(PACKET_TYPE_RREQ, target, esp_random(), MESH_DEFAULT_TTL + 1, BROADCAST_NODE, "");
  lastRouteDiscoveryAt = millis();
  if (routeDiscoveryStartedAt == 0) routeDiscoveryStartedAt = lastRouteDiscoveryAt;
  statusLine = "discover " + nodeIdHex(target).substring(4);
  aodvLine = "RREQ " + nodeIdHex(target).substring(4);
  drawBottom();
}

void sendRouteReply(uint32_t target, uint32_t requestId) {
  RouteEntry route;
  if (findRoute(target, route)) {
    transmitPacket(PACKET_TYPE_RREP, target, requestId, MESH_DEFAULT_TTL + 1, route.nextHop, "");
    aodvLine = "RREP to " + nodeIdHex(target).substring(4);
  }
}

void queueRelay(const Packet &packet) {
  if (packet.ttl <= 1 || packet.destination != BROADCAST_NODE) return;
  relay.active = true;
  relay.type = packet.type;
  relay.source = packet.source;
  relay.destination = packet.destination;
  relay.messageId = packet.messageId;
  relay.ttl = packet.ttl - 1;
  relay.body = packet.body;
  relay.isEmergency = packet.type == PACKET_TYPE_EMERGENCY;
  relay.dueAt = millis() + random(250, 900);
}

void serviceRelay() {
  if (!relay.active || timeBefore(relay.dueAt)) return;
  if (transmitPacketFrom(relay.type, relay.source, relay.destination, relay.messageId, relay.ttl, BROADCAST_NODE, relay.body)) {
    packetStats.forwarded++;
    statusLine = relay.isEmergency ? "emergency relay" : "relayed ttl " + String(relay.ttl);
    aodvLine = "flood relay " + nodeIdHex(relay.source).substring(4);
    drawBottom();
  }
  relay.active = false;
}

void forwardRouteReply(const Packet &packet) {
  RouteEntry route;
  if (packet.ttl <= 1 || !findRoute(packet.destination, route)) return;
  if (transmitPacketFrom(PACKET_TYPE_RREP, packet.source, packet.destination, packet.messageId, packet.ttl - 1, route.nextHop, packet.body)) {
    packetStats.forwarded++;
    aodvLine = "fwd RREP " + nodeIdHex(packet.source).substring(4);
  }
}

void forwardUnicast(const Packet &packet) {
  RouteEntry route;
  if (packet.ttl <= 1 || !findRoute(packet.destination, route)) return;
  if (transmitPacketFrom(packet.type, packet.source, packet.destination, packet.messageId, packet.ttl - 1, route.nextHop, packet.body)) {
    packetStats.forwarded++;
    aodvLine = "fwd unicast " + nodeIdHex(packet.destination).substring(4);
  }
}

bool hasHeardGroupAck(uint32_t dataSource, uint32_t messageId) {
  const uint32_t now = millis();
  for (const auto &entry : heardAcks)
    if (entry.active && entry.dataSource == dataSource && entry.messageId == messageId && now - entry.lastSeenAt < GROUP_ACK_SEEN_TTL_MS)
      return true;
  return false;
}

void cancelGroupAck(uint32_t dataSource, uint32_t messageId) {
  for (auto &job : groupAckJobs)
    if (job.active && job.dataSource == dataSource && job.messageId == messageId) job.active = false;
}

void noteGroupAckHeard(uint32_t dataSource, uint32_t messageId) {
  if (dataSource == 0 || messageId == 0) return;
  const uint32_t now = millis();
  for (auto &entry : heardAcks) {
    if (entry.active && entry.dataSource == dataSource && entry.messageId == messageId) {
      entry.lastSeenAt = now; cancelGroupAck(dataSource, messageId); return;
    }
  }
  heardAcks[heardAckNext].active = true;
  heardAcks[heardAckNext].dataSource = dataSource;
  heardAcks[heardAckNext].messageId = messageId;
  heardAcks[heardAckNext].lastSeenAt = now;
  heardAckNext = static_cast<uint8_t>((heardAckNext + 1) % 32);
  cancelGroupAck(dataSource, messageId);
}

void scheduleGroupAck(const Packet &packet) {
  const uint32_t now = millis();
  for (const auto &entry : heardAcks) {
    if (entry.active && entry.dataSource == packet.source
        && entry.messageId == packet.messageId
        && now - entry.lastSeenAt < 2000) { return; }
  }
  GroupAckJob *slot = nullptr;
  for (auto &job : groupAckJobs) {
    if (job.active && job.dataSource == packet.source && job.messageId == packet.messageId) return;
    if (!job.active && slot == nullptr) slot = &job;
  }
  if (slot == nullptr) slot = &groupAckJobs[0];
  slot->active = true;
  slot->dataSource = packet.source;
  slot->messageId = packet.messageId;
  slot->dueAt = now + random(GROUP_ACK_DELAY_MIN_MS, GROUP_ACK_DELAY_MAX_MS + 1);
}

void sendGroupAck(uint32_t dataSource, uint32_t messageId) {
  noteGroupAckHeard(dataSource, messageId);
  transmitPacket(PACKET_TYPE_ACK, dataSource, messageId, MESH_DEFAULT_TTL, BROADCAST_NODE, "");
}

void serviceGroupAcks() {
  for (auto &job : groupAckJobs) {
    if (!job.active || timeBefore(job.dueAt)) continue;
    const uint32_t dataSource = job.dataSource;
    const uint32_t messageId = job.messageId;
    job.active = false;
    if (!hasHeardGroupAck(dataSource, messageId)) sendGroupAck(dataSource, messageId);
  }
}

bool enqueuePendingMessage(const PendingMessage &msg) {
  if (pendingQueueCount >= TX_QUEUE_DEPTH) return false;
  pendingQueue[pendingQueueTail] = msg;
  pendingQueueTail = (pendingQueueTail + 1) % TX_QUEUE_DEPTH;
  pendingQueueCount++;
  return true;
}

bool dequeuePendingMessage(PendingMessage &msg) {
  if (pendingQueueCount == 0) return false;
  msg = pendingQueue[pendingQueueHead];
  pendingQueue[pendingQueueHead] = PendingMessage();
  pendingQueueHead = (pendingQueueHead + 1) % TX_QUEUE_DEPTH;
  pendingQueueCount--;
  return true;
}

void queueOutgoing(String body) {
  body.trim();
  if (body.length() == 0) return;
  if (ENABLE_MESSAGE_FRAGMENTATION && body.length() > MAX_CHAT_TEXT_LEN) {
    uint32_t destination = BROADCAST_NODE;
    if (chatTab == ChatTab::Direct) {
      if (selectedDirectNode == 0) selectDirectChat(firstDirectNode());
      if (selectedDirectNode == 0) { statusLine = "no direct node"; drawBottom(); return; }
      destination = selectedDirectNode;
      selectedDestination = selectedDirectNode;
    } else {
      selectedDestination = BROADCAST_NODE;
    }
    if (!sendFragmented(body, destination, destination)) return;
    if (destination == BROADCAST_NODE) {
      addGroupEntry("me [frag]: " + body.substring(0, MAX_LONG_CHAT_TEXT_LEN), COLOR_ORANGE, 0, true, false);
#ifdef BOARD_HELTEC_V3
      appPrintln("me [frag all]: " + body.substring(0, MAX_LONG_CHAT_TEXT_LEN));
#endif
    } else {
      addDirectEntry(destination, nodeDisplayName(destination),
                     "me [frag ...]: " + body.substring(0, MAX_LONG_CHAT_TEXT_LEN),
                     YELLOW, 0, true, false);
#ifdef BOARD_HELTEC_V3
      appPrintln("me [frag " + nodeIdHex(destination).substring(4) + "]: " + body.substring(0, MAX_LONG_CHAT_TEXT_LEN));
#endif
    }
    drawUi();
    return;
  }
  PendingMessage message;
  message.active = true;
  if (chatTab == ChatTab::Direct) {
    if (selectedDirectNode == 0) selectDirectChat(firstDirectNode());
    if (selectedDirectNode == 0) { statusLine = "no direct node"; drawBottom(); return; }
    selectedDestination = selectedDirectNode;
  } else { selectedDestination = BROADCAST_NODE; }
  message.destination = selectedDestination;
  message.conversation = selectedDestination;
  BleNode bleNode;
  message.prefersBle = message.destination != BROADCAST_NODE && findBleNode(message.destination, bleNode);
  message.messageId = nextMessageId++;
  const String displayBody = body.substring(0, MAX_CHAT_TEXT_LEN);
  message.body = encryptedPayloadFor(displayBody);
  if (message.body.length() == 0) { statusLine = "encryption failed"; drawBottom(); return; }
  if (!enqueuePendingMessage(message)) { statusLine = "message queue full"; drawBottom(); return; }
  if (message.conversation == BROADCAST_NODE) {
    addGroupEntry("me [all]: " + displayBody, COLOR_ORANGE, message.messageId, true, false);
#ifdef BOARD_HELTEC_V3
    appPrintln("me [all]: " + displayBody);
#endif
  } else {
    const String transport = message.prefersBle ? "BLE" : "LoRa";
    addDirectEntry(message.conversation, nodeDisplayName(message.conversation),
                   "me [" + transport + " ...]: " + displayBody,
                   message.prefersBle ? CYAN : YELLOW, message.messageId, true, false);
#ifdef BOARD_HELTEC_V3
    appPrintln("me [" + nodeIdHex(message.conversation).substring(4) + "]: " + displayBody);
#endif
  }
  statusLine = pending.active ? "queued msg " + String(pendingQueueCount) + "/" + String(TX_QUEUE_DEPTH) : "queued msg";
  drawUi();
}

void servicePending() {
  if (!pending.active) { if (!dequeuePendingMessage(pending)) return; routeDiscoveryStartedAt = 0; }
  const uint32_t now = millis();
  const uint32_t ackTimeoutMs = pending.isEmergency ? EMERGENCY_ACK_TIMEOUT_MS : CHAT_ACK_TIMEOUT_MS;
  const uint8_t maxRetries = pending.isEmergency ? EMERGENCY_MAX_RETRIES : CHAT_MAX_RETRIES;
  if (pending.attempts > 0 && now - pending.lastSentAt < ackTimeoutMs) return;
  if (pending.attempts >= maxRetries) {
    statusLine = "delivery failed";
    markMessageFailed(pending.messageId);
    // Auto-promote to long-term store for non-broadcast messages
    // (skip if this already is a stored message — it stays in the store)
    if (pending.storedSlotIndex == 0xFF) {
      promotePendingToStored();
    }
    pending.active = false; routeDiscoveryStartedAt = 0; drawUi();
    return;
  }
  pending.attempts++; pending.lastSentAt = now;
  RouteEntry route;
  uint32_t nextHop = BROADCAST_NODE;
  if (pending.destination != BROADCAST_NODE) {
    if (!findRoute(pending.destination, route)) {
      if (routeDiscoveryStartedAt > 0 && now - routeDiscoveryStartedAt > ROUTE_DISCOVERY_TIMEOUT_MS) {
        statusLine = "route discovery failed";
        markMessageFailed(pending.messageId);
        // Don't promote stored messages again — they stay in the store for next HELLO/route event
        if (pending.storedSlotIndex == 0xFF) {
          promotePendingToStored();
        }
        pending.active = false; routeDiscoveryStartedAt = 0; drawUi();
        return;
      }
      if (now - lastRouteDiscoveryAt > ROUTE_DISCOVERY_INTERVAL_MS) sendRouteRequest(pending.destination);
      else { statusLine = "waiting route"; drawBottom(); }
      pending.attempts--; return;
    }
    routeDiscoveryStartedAt = 0; nextHop = route.nextHop;
  }
  BleNode bleNode;
  const bool useBle = nextHop != BROADCAST_NODE && findBleNode(nextHop, bleNode);
  if (pending.destination != BROADCAST_NODE) updateMessageTransport(pending.messageId, useBle);
  const uint8_t packetType = pending.isEmergency ? PACKET_TYPE_EMERGENCY :
                             (pending.fragmentCount > 0 ? PACKET_TYPE_FRAGMENT : PACKET_TYPE_DATA);
  statusLine = pending.isEmergency ? "SOS try " + String(pending.attempts) + "/" + String(maxRetries) :
               "sending try " + String(pending.attempts) + "/" + String(maxRetries);
  transmitPacket(packetType, pending.destination, pending.messageId, MESH_DEFAULT_TTL, nextHop, pending.body);
  drawUi();
}

void handleRouteRequest(const Packet &packet) {
  updateRoute(packet.source, packet.previousHop, static_cast<uint8_t>(MESH_DEFAULT_TTL + 2 - packet.ttl));
  const bool duplicate = isDuplicate(packet.source, packet.messageId);
  if (packet.destination == localNodeId) {
    sendRouteReply(packet.source, packet.messageId);
    aodvLine = "RREQ hit " + nodeIdHex(packet.source).substring(4);
    return;
  }
  if (!duplicate && packet.ttl > 1 &&
      transmitPacketFrom(PACKET_TYPE_RREQ, packet.source, packet.destination, packet.messageId, packet.ttl - 1, BROADCAST_NODE, packet.body)) {
    packetStats.forwarded++;
    aodvLine = "fwd RREQ " + nodeIdHex(packet.destination).substring(4);
  }
}

void handleRouteReply(const Packet &packet) {
  updateRoute(packet.source, packet.previousHop, static_cast<uint8_t>(MESH_DEFAULT_TTL + 2 - packet.ttl));
  if (packet.destination == localNodeId) {
    statusLine = "route " + nodeIdHex(packet.source).substring(4);
    aodvLine = "route ready " + nodeIdHex(packet.source).substring(4);
    // We now have a route to this node (possibly multi-hop) — mark as online
    // and trigger any stored messages destined for it
    updateContactSeen(packet.source, "", packet.rssi);
    triggerStoredMessageRetry(packet.source);
    drawBottom();
    return;
  }
  forwardRouteReply(packet);
}

// Extract hop sync data from HELLO beacon body.
// Supports both legacy format ("Name//3") and epoch format ("Name//3//1234567").
// Calls applyHopSync() to compute hopEpochOffset and align the slot clock.
//
// IMPORTANT: When already synced, we only reply with our own HELLO so booting
// devices can sync to us. We do NOT apply the remote's sync data — a booting
// device's slot/millis are uncalibrated and would corrupt our epoch offset.
// Only the booting device should sync (it receives our reply and computes the
// correct offset from our known-good data).
static bool parseHelloSlot(const String &body) {
  const int sep1 = body.indexOf("//");
  if (sep1 < 0 || static_cast<unsigned int>(sep1 + 2) >= body.length()) return false;

  const uint8_t remoteSlot = static_cast<uint8_t>(body.substring(sep1 + 2).toInt()) % hopCount;

  const int sep2 = body.indexOf("//", sep1 + 2);
  if (sep2 >= 0 && static_cast<unsigned int>(sep2 + 1) < body.length()) {
    // Epoch format: includes sender's network time.
    const int32_t remoteNetworkTime = static_cast<int32_t>(body.substring(sep2 + 2).toInt());
    if (!hoppingSynced) {
      // Not synced yet — accept sync from any device. applyHopSync() sets
      // the offset and slot (no retune, no hoppingSynced flag). We mark
      // ourselves synced so the boot sweep exits and serviceHopping() begins.
      applyHopSync(remoteSlot, remoteNetworkTime);
      hoppingSynced = true;
      lastHopSyncAt = millis();
      return true;
    }
    // Already synced — don't let a booter corrupt our offset.
    return false;
  }
  if (!hoppingSynced) {
    // Legacy format fallback (no millis). Set slot directly — less precise
    // but keeps backward compatibility with pre-epoch firmware.
    hopSlot = remoteSlot;
    hoppingSynced = true;
    lastHopSyncAt = millis();
    retuneToFrequency(hopChannels[hopSlot]);
    return true;
  }
  return false;
}

void handleIncoming(const Packet &packet) {
  updateRoute(packet.previousHop, packet.previousHop, 1);
  if (packet.source != packet.previousHop) updateRoute(packet.source, packet.previousHop, 2);

  // Leaf nodes only update routes — they never relay or forward
  if (getNodeRole() == NodeRole::Leaf) {
    if (packet.type == PACKET_TYPE_HELLO) {
      // Leaf still learns about neighbors via HELLOs
      updateContactSeen(packet.source, packet.body.substring(0, min(static_cast<int>(packet.body.length()), static_cast<int>(10))), packet.rssi);
      drawBottom();
      return;
    }
    // Leaf: ignore route requests, route replies, and forwarding duties
    if (packet.type != PACKET_TYPE_DATA && packet.type != PACKET_TYPE_FRAGMENT &&
        packet.type != PACKET_TYPE_EMERGENCY) return;
  }

  if (packet.type == PACKET_TYPE_HELLO) {
    statusLine = "heard " + packet.body.substring(0, min(static_cast<int>(packet.body.length()), 12));
    aodvLine = "HELLO " + packet.body.substring(0, min(static_cast<int>(packet.body.length()), 12));
    // Update contact list from HELLO
    updateContactSeen(packet.source, packet.body.substring(0, min(static_cast<int>(packet.body.length()), static_cast<int>(10))), packet.rssi);
    // HELLO means this node is directly reachable — trigger store-and-forward retries
    triggerStoredMessageRetry(packet.source);
#if ENABLE_FREQ_HOPPING
    // Sync first (applyHopSync no longer retunes), so any reply transmits the
    // correct synced network time — not the pre-sync raw millis.
    const bool wasSynced = hoppingSynced;
    parseHelloSlot(packet.body);

    // DEBUG: log every HELLO and the reply decision
    {
      static uint32_t lastHelloReplyAt = 0;
      const uint32_t now = millis();
      const uint32_t age = now - lastHelloReplyAt;
      const bool replyReady = wasSynced && age > hopIntervalMs;
      appPrintf("[dbg] HELLO wasSynced=%c age=%lu/%lu ms replyReady=%c slot=%u freq=%.1f MHz\n",
                wasSynced ? 'Y' : 'N',
                static_cast<unsigned long>(age),
                static_cast<unsigned long>(hopIntervalMs),
                replyReady ? 'Y' : 'N',
                hopSlot, static_cast<double>(hopChannels[hopSlot]));
      if (replyReady) {
        // Spread replies from multiple synced peers so a joining node is less
        // likely to hear overlapping HELLO responses.
        const uint32_t replyWindowMs = max(static_cast<uint32_t>(100), min(static_cast<uint32_t>(1200), hopIntervalMs / 2));
        const uint32_t nodeStaggerMs = 50 + ((localNodeId % 17) * 65);
        const uint32_t replyDelayMs = min(replyWindowMs, nodeStaggerMs + static_cast<uint32_t>(random(0, 31)));
        delay(replyDelayMs);
        if (sendHelloImmediate()) {
          appPrintf("[dbg] HELLO REPLY SENT OK\n");
          lastHelloReplyAt = now;
        } else {
          appPrintf("[dbg] HELLO REPLY FAILED (radio busy)\n");
        }
      }
    }
#endif
    drawBottom();
    return;
  }
  if (packet.type == PACKET_TYPE_RREQ) { handleRouteRequest(packet); return; }
  if (packet.type == PACKET_TYPE_RREP) { handleRouteReply(packet); return; }

  if (packet.type == PACKET_TYPE_ACK) {
    noteGroupAckHeard(packet.destination, packet.messageId);
    if (packet.destination != localNodeId) {
      if (packet.nextHop == BROADCAST_NODE) return;
      forwardUnicast(packet);
      return;
    }
    // Check for emergency ACK first
    if (pending.active && packet.messageId == pending.messageId && pending.isEmergency) {
      statusLine = "EMERGENCY delivered";
      markMessageAcked(packet.messageId);
      pending.active = false;
      drawUi();
      return;
    }
    if (pending.active && packet.messageId == pending.messageId) {
      statusLine = "delivered";
      markMessageAcked(packet.messageId);
      // If this was from the store-and-forward queue, clear the stored slot
      if (pending.storedSlotIndex != 0xFF && pending.storedSlotIndex < STORE_FORWARD_MAX_MESSAGES) {
        const uint8_t slot = pending.storedSlotIndex;
        if (storedMessages[slot].active) {
          storedMessages[slot].active = false;
          if (storedMessages[slot].isDirect) {
            addDirectEntry(storedMessages[slot].destination, storedMessages[slot].destName,
                           "sfwd delivered: " + storedMessages[slot].rawText, GREEN);
          } else {
            addGroupEntry("sfwd delivered: " + storedMessages[slot].rawText, GREEN);
          }
          persistStoredMessages();
        }
      }
      pending.active = false;
      drawUi();
    }
    return;
  }

  // Handle new packet types
  if (packet.type == PACKET_TYPE_FRAGMENT) {
    if (packet.destination != BROADCAST_NODE && packet.destination != localNodeId) { forwardUnicast(packet); return; }
    if (packet.destination == localNodeId) sendAck(packet);
    if (packet.destination == BROADCAST_NODE) { scheduleGroupAck(packet); queueRelay(packet); }
    handleFragment(packet);
    return;
  }

  if (packet.type == PACKET_TYPE_EMERGENCY) {
    packetStats.emergencyRx++;
    if (packet.destination != BROADCAST_NODE && packet.destination != localNodeId) { forwardUnicast(packet); return; }
    if (packet.destination == localNodeId) sendAck(packet);
    if (packet.destination == BROADCAST_NODE) scheduleGroupAck(packet);
    // Emergency messages are always treated as broadcast — relay immediately with short delay
    if (!isDuplicate(packet.source, packet.messageId)) {
      String sender, message;
      if (decryptPayload(packet.body, sender, message)) {
        const String line = "*** EMERGENCY from " + sender.substring(0, 8) + " ***: " + message;
        addGroupEntry(line, COLOR_ALERT, packet.messageId, false, false);
        statusLine = "EMERGENCY from " + nodeIdHex(packet.source).substring(4);
        drawUi();
      } else {
        packetStats.decryptFail++;
        statusLine = "emergency decrypt failed";
        drawBottom();
      }
    }
    // Emergency relay always forwards (even leaf nodes relay emergencies)
    if (packet.ttl > 1) {
      relay.active = true;
      relay.type = PACKET_TYPE_EMERGENCY;
      relay.source = packet.source;
      relay.destination = BROADCAST_NODE;
      relay.messageId = packet.messageId;
      relay.ttl = packet.ttl - 1;
      relay.body = packet.body;
      relay.isEmergency = true;
      relay.dueAt = millis() + 50;  // Very short delay for emergency relay
    }
    return;
  }

#if ENABLE_GPS_TELEMETRY
  if (packet.type == PACKET_TYPE_POSITION) {
    handlePositionData(packet);
    return;
  }
#endif

  if (packet.type == PACKET_TYPE_CONTACT_QUERY) {
    // Reply with our device name
    transmitPacket(PACKET_TYPE_CONTACT_REPLY, packet.source, packet.messageId, 1, BROADCAST_NODE,
                   deviceName.substring(0, 16));
    return;
  }

  if (packet.type == PACKET_TYPE_CONTACT_REPLY) {
    // Learn about the responding node
    addContact(packet.source, packet.body.substring(0, min(static_cast<int>(packet.body.length()), 16)), packet.rssi);
    updateRoute(packet.previousHop, packet.previousHop, 1);
    statusLine = "contact " + packet.body.substring(0, min(static_cast<int>(packet.body.length()), 12));
    drawBottom();
    return;
  }

  // Fall through: handle DATA packets
  if (packet.type != PACKET_TYPE_DATA) return;

  if (packet.destination != BROADCAST_NODE && packet.destination != localNodeId) { forwardUnicast(packet); return; }
  if (packet.destination == localNodeId) sendAck(packet);

  if (packet.destination == BROADCAST_NODE) { scheduleGroupAck(packet); queueRelay(packet); }

  const bool duplicate = isDuplicate(packet.source, packet.messageId);
  if (!duplicate) {
    String sender, message;
    if (decryptPayload(packet.body, sender, message)) {
      const String line = sender.substring(0, 12) + ": " + message;
      if (packet.destination == BROADCAST_NODE) addGroupEntry(line, WHITE);
      else addDirectEntry(packet.source, sender, line, MAGENTA);
#ifdef BOARD_HELTEC_V3
      appPrintln(line);
#endif
      statusLine = "rx encrypted " + String(packet.rssi) + " dBm";
    } else {
      packetStats.decryptFail++;
      statusLine = "decrypt failed";
    }
    drawUi();
  }
}
