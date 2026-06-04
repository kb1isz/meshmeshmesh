// ============================================================================
// ble.cpp — BLE mesh transport: persistent links, scanning, and packet exchange
//
// Implements:
//  - BLE GATT service for mesh packet relay (when ENABLE_BLE_MESH=1)
//  - Manufacturer advertisement beacon with node ID and name
//  - Periodic scanning for nearby T-Deck mesh nodes
//  - Persistent BLE connection pool (BleLink) for low-latency multi-hop mesh
//  - BLE link keepalive, idle timeout, and automatic reconnect
//  - BLE transmit queue with send over links or fallback to connect→write→disconnect
//
// SECURITY NOTE: When BLE is enabled, the mesh packet GATT characteristic
// accepts unauthenticated writes from any BLE device. No pairing, bonding,
// or BLE security mode is enforced. This means any nearby BLE device can
// inject packets into the mesh. For production use, implement BLE pairing
// with LE Secure Connections and authenticated writes (NIMBLE_PROPERTY::WRITE_AUTHEN).
//
// BLE mesh is enabled by default (ENABLE_BLE_MESH=1 in platformio.ini).
// When disabled via platformio.ini, all BLE functions are no-ops to reduce power consumption.
// ============================================================================

#include "globals.h"

// MeshPacketCallbacks inner class
// Handles incoming BLE GATT writes to the mesh packet characteristic.
// WARNING: The `new` allocation in setupBleLocation() is matched by NimBLE's
// internal ownership — verify against your NimBLE-Arduino version.
namespace {
class MeshPacketCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *characteristic, NimBLEConnInfo &connInfo) override {
    const std::string value = characteristic->getValue();
    // Packets use their actual wire length, not the padded MAX_PACKET_LEN.
    if (value.size() >= HEADER_LEN + 2 && value.size() <= MAX_PACKET_LEN) {
      const NimBLEAddress peer = connInfo.getAddress();
      const std::string peerText = peer.toString();
      portENTER_CRITICAL(&blePacketMux);
      memcpy(blePacketBuffer, value.data(), value.size());
      blePacketLength = value.size();
      strlcpy(blePacketPeerAddress, peerText.c_str(), sizeof(blePacketPeerAddress));
      blePacketPeerAddressType = peer.getType();
      blePacketHasPeer = true;
      blePacketPending = true;
      portEXIT_CRITICAL(&blePacketMux);
    }
  }
};
} // namespace

// ============================================================================
//  BLE Link Pool (persistent connections to peer mesh nodes)
// ============================================================================

// Busy flag to prevent overlapping NimBLE client operations (connect/teardown).
// Set during link establishment or teardown, cleared when the operation completes.
static bool bleLinkBusy = false;

// Cooldown tracking — after tearing down a link, prevent reconnecting to the
// same node for a short period so NimBLE's soft device timers settle.
// This avoids `ble_hs_timer_exp` asserts from overlapping client operations.
#define BLE_LINK_COOLDOWN_MS 10000
#define BLE_LINK_COOLDOWN_SLOTS 4
static uint32_t linkCooldownNode[BLE_LINK_COOLDOWN_SLOTS] = {0};
static uint32_t linkCooldownUntil[BLE_LINK_COOLDOWN_SLOTS] = {0};
static uint8_t linkCooldownNext = 0;

// Record a node ID in the cooldown table so it won't be reconnected immediately.
static void recordLinkCooldown(uint32_t nodeId) {
  const uint32_t until = millis() + BLE_LINK_COOLDOWN_MS;
  linkCooldownNode[linkCooldownNext] = nodeId;
  linkCooldownUntil[linkCooldownNext] = until;
  linkCooldownNext = (linkCooldownNext + 1) % BLE_LINK_COOLDOWN_SLOTS;
}

// Check if a node is currently in cooldown (recently torn down).
static bool isLinkOnCooldown(uint32_t nodeId) {
  const uint32_t now = millis();
  for (uint8_t i = 0; i < BLE_LINK_COOLDOWN_SLOTS; i++) {
    if (linkCooldownNode[i] == nodeId && now < linkCooldownUntil[i]) return true;
    // Expire stale entries
    if (linkCooldownNode[i] != 0 && now >= linkCooldownUntil[i]) {
      linkCooldownNode[i] = 0;
      linkCooldownUntil[i] = 0;
    }
  }
  return false;
}

// Find a link slot by node ID. Returns nullptr if not found.
static BleLink *findLinkByNodeId(uint32_t nodeId) {
  for (auto &link : bleLinks)
    if (link.active && link.nodeId == nodeId) return &link;
  return nullptr;
}

// Find a link slot by BLE address string. Returns nullptr if not found.
static BleLink *findLinkByAddress(const String &addr) {
  for (auto &link : bleLinks)
    if (link.active && link.address.equalsIgnoreCase(addr)) return &link;
  return nullptr;
}

// Find an empty (inactive) link slot. Returns nullptr if pool is full.
static BleLink *findEmptyLinkSlot() {
  for (auto &link : bleLinks)
    if (!link.active) return &link;
  return nullptr;
}

// Disconnect and free a BLE link slot gracefully.
// Note: we do NOT call NimBLEDevice::deleteClient() here because NimBLE's
// internal timer handles from the connection may still be pending in the
// soft device. Deleting the client while timers are queued causes
// ble_hs_timer_exp asserts. We just disconnect and forget the pointer;
// NimBLE will clean up the client internally.
static void teardownLink(BleLink &link) {
  if (link.client != nullptr) {
    if (link.client->isConnected()) {
      link.client->disconnect();
    }
    // Give NimBLE time to process the disconnect and destroy internal
    // timer handles before we might create a new client.
    vTaskDelay(pdMS_TO_TICKS(100));
    link.client = nullptr;
  }
  recordLinkCooldown(link.nodeId);
  link.txChar = nullptr;
  link.active = false;
  link.nodeId = 0;
  link.address = "";
  link.connectedAt = 0;
  link.lastActivityAt = 0;
  link.lastSeenAt = 0;
  if (bleLinkCount > 0) bleLinkCount--;
}

// Connect to a peer mesh node and establish a persistent BLE link.
// Places the connected link into the pool on success.
// Returns true if the link was established.
static bool establishLink(BleLink &link, uint32_t nodeId, const String &addr, uint8_t addrType) {
  NimBLEClient *client = NimBLEDevice::createClient();
  if (client == nullptr) return false;

  client->setConnectTimeout(BLE_CONNECT_TIMEOUT_MS);

  if (!client->connect(NimBLEAddress(std::string(addr.c_str()), addrType), true, false, false)) {
    // connect() never completed — no timer handles pending, safe to delete
    NimBLEDevice::deleteClient(client);
    packetStats.bleConnectFail++;
    return false;
  }

  // Allow service discovery to propagate before querying.
  // getService() without the blocking flag may return nullptr if NimBLE
  // hasn't finished the attribute protocol exchange yet.
  vTaskDelay(pdMS_TO_TICKS(50));

  // Get the remote service and mesh packet characteristic.
  // IMPORTANT: Do NOT call NimBLEDevice::deleteClient() after a successful
  // connect(), even on service discovery failure. The connection has timers
  // in the BLE soft device that cause ble_hs_timer_exp asserts if the
  // client is deleted. Just disconnect and let NimBLE clean up internally.
  NimBLERemoteService *service = client->getService(NimBLEUUID(BLE_MESH_SERVICE_UUID));
  if (service == nullptr) {
    client->disconnect();
    packetStats.bleConnectFail++;
    return false;
  }

  NimBLERemoteCharacteristic *txChar = service->getCharacteristic(NimBLEUUID(BLE_MESH_PACKET_UUID));
  if (txChar == nullptr) {
    client->disconnect();
    packetStats.bleConnectFail++;
    return false;
  }

  const uint32_t now = millis();
  link.active = true;
  link.nodeId = nodeId;
  link.client = client;
  link.txChar = txChar;
  link.connectedAt = now;
  link.lastActivityAt = now;
  link.lastSeenAt = now;
  link.address = addr;
  link.addressType = addrType;
  bleLinkCount++;
  return true;
}

// ============================================================================
//  Original BLE API (some functions modified to use links)
// ============================================================================

// Look up a BLE node by its mesh node ID.
// Only returns nodes that are still within the BLE_NODE_TTL_MS window
// and have a valid BLE address string.
bool findBleNode(uint32_t nodeId, BleNode &out) {
#if !ENABLE_BLE_MESH
  (void)nodeId; (void)out;
  return false;
#endif
  const uint32_t now = millis();
  for (const auto &node : bleNodes) {
    if (node.active && node.nodeId == nodeId && now - node.lastSeenAt <= BLE_NODE_TTL_MS && node.address.length() > 0) {
      out = node;
      return true;
    }
  }
  return false;
}

// Update or create a BLE node entry from a scan result.
// Also creates a 1-hop route entry to this node (direct BLE reachable).
// Additionally, the scan result triggers a persistent link establishment
// in serviceBleLinks() — we just record the node here.
void updateBleNode(uint32_t nodeId, const String &name, int rssi, const String &address, uint8_t addressType) {
#if !ENABLE_BLE_MESH
  (void)nodeId; (void)name; (void)rssi; (void)address; (void)addressType;
  return;
#endif
  if (nodeId == 0 || nodeId == localNodeId) return;

  BleNode *slot = nullptr;
  for (auto &node : bleNodes) {
    if (node.active && node.nodeId == nodeId) { slot = &node; break; }
    if (!node.active && slot == nullptr) slot = &node;
  }
  if (slot == nullptr) slot = &bleNodes[0];

  slot->active = true;
  slot->nodeId = nodeId;
  slot->name = name.substring(0, 16);
  slot->address = address;
  slot->addressType = addressType;
  slot->rssi = rssi;
  slot->lastSeenAt = millis();
  updateRoute(nodeId, nodeId, 1);
  bleLine = "BLE " + nodeIdHex(nodeId).substring(4) + " " + String(rssi) + "dBm";
}

// Decode a BLE manufacturer data beacon into node ID and name.
// Beacon format: ['T','D','M', version{1|2}, 4-byte LE nodeId, name...]
// Returns false if the beacon is not a valid T-Deck mesh advertisement.
bool decodeBleBeaconData(const std::string &data, uint32_t &nodeId, String &name) {
  if (data.size() < 8 || data[0] != 'T' || data[1] != 'D' || data[2] != 'M' || (data[3] != '1' && data[3] != '2'))
    return false;
  nodeId = static_cast<uint8_t>(data[4]) |
           (static_cast<uint32_t>(static_cast<uint8_t>(data[5])) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(data[6])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(data[7])) << 24);
  name = "";
  for (size_t j = 8; j < data.size(); ++j) {
    if (data[j] == 0) break;
    name += static_cast<char>(data[j]);
  }
  return nodeId != 0;
}

// Configure the BLE advertisement with mesh beacon data and device name.
void configureBleAdvertisement() {
#if !ENABLE_BLE_MESH
  bleReady = false;
  bleLine = "BLE disabled";
  return;
#endif
  uint8_t manufacturerData[20] = {
      'T', 'D', 'M', '2',
      static_cast<uint8_t>(localNodeId & 0xFF),
      static_cast<uint8_t>((localNodeId >> 8) & 0xFF),
      static_cast<uint8_t>((localNodeId >> 16) & 0xFF),
      static_cast<uint8_t>((localNodeId >> 24) & 0xFF)};
  const String advertisedName = deviceName.substring(0, 12);
  memcpy(manufacturerData + 8, advertisedName.c_str(), advertisedName.length());

  NimBLEAdvertisementData advertisement;
  advertisement.setFlags(0x04);
  advertisement.addServiceUUID(NimBLEUUID(BLE_MESH_ADV_UUID));
  advertisement.setManufacturerData(std::string(reinterpret_cast<char *>(manufacturerData), sizeof(manufacturerData)));

  NimBLEAdvertisementData scanResponse;
  scanResponse.setName(deviceName.c_str());

  NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
  advertising->stop();
  advertising->setAdvertisementData(advertisement);
  advertising->setScanResponseData(scanResponse);
  advertising->start();
  bleReady = true;
  bleLine = "BLE advertising " + nodeIdHex(localNodeId).substring(4);
}

// Restart BLE advertising (e.g., after a connection was closed).
void restartBleAdvertisement() {
#if !ENABLE_BLE_MESH
  return;
#endif
  if (!bleReady) return;
  NimBLEDevice::getAdvertising()->start();
}

// One-time BLE initialization: create GATT server, configure advertisement, start scanner.
void setupBleLocation() {
#if !ENABLE_BLE_MESH
  bleReady = false;
  bleLine = "BLE disabled";
  return;
#endif
  NimBLEDevice::init(deviceName.c_str());
  NimBLEDevice::setMTU(185);
  bleServer = NimBLEDevice::createServer();
  NimBLEService *service = bleServer->createService(NimBLEUUID(BLE_MESH_SERVICE_UUID));
  blePacketCharacteristic = service->createCharacteristic(
      NimBLEUUID(BLE_MESH_PACKET_UUID),
      NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  // NOTE: The callback object is allocated here and managed by NimBLE internally.
  // See the MeshPacketCallbacks class comment above for ownership information.
  blePacketCharacteristic->setCallbacks(new MeshPacketCallbacks());
  bleServer->start();

  configureBleAdvertisement();
  bleScan = NimBLEDevice::getScan();
  bleScan->setActiveScan(true);
  bleScan->setInterval(1349);  // 1349 * 0.625ms ≈ 843ms scan interval
  bleScan->setWindow(449);     // 449 * 0.625ms ≈ 281ms scan window
  // Stagger initial scan start to avoid synchronized scanning with peers.
  lastBleScanAt = millis() - BLE_SCAN_INTERVAL_MS + random(0, 3000);
}

// Service the periodic BLE scan cycle. Called from loop().
// Alternates between: idle (waiting for interval), scanning, and processing results.
void serviceBleLocation() {
#if !ENABLE_BLE_MESH
  return;
#endif
  if (bleScan == nullptr) return;
  const uint32_t now = millis();
  // Don't scan while BLE transmit is busy or pending.
  if (bleTxBusy || bleQueueCount > 0 || now - lastBleTxAt < 1000) return;

  if (!bleScanInProgress) {
    if (now - lastBleScanAt < BLE_SCAN_INTERVAL_MS) return;
    lastBleScanAt = now;
    bleScanStartedAt = now;
    packetStats.bleScans++;
    bleLine = "BLE scanning";
    bleScanInProgress = bleScan->start(BLE_SCAN_DURATION_MS, false, true);
    if (!bleScanInProgress) bleLine = "BLE scan busy";
    return;
  }

  // Wait for the scan to complete (allow 100ms grace period).
  if (bleScan->isScanning() && now - bleScanStartedAt < BLE_SCAN_DURATION_MS + 100) return;

  bleScanInProgress = false;
  NimBLEScanResults results = bleScan->getResults();
  bleLine = "BLE scan " + String(results.getCount()) + " adv";
  for (int i = 0; i < results.getCount(); ++i) {
    const NimBLEAdvertisedDevice *device = results.getDevice(i);
    if (device == nullptr) continue;

    uint32_t nodeId = 0;
    String name = "";
    for (uint8_t dataIndex = 0; dataIndex < device->getManufacturerDataCount(); ++dataIndex) {
      if (decodeBleBeaconData(device->getManufacturerData(dataIndex), nodeId, name)) break;
    }
    if (nodeId == 0) continue;
    if (device->haveName()) name = String(device->getName().c_str());
    if (name.length() == 0) name = nodeIdHex(nodeId);
    updateBleNode(nodeId, name, device->getRSSI(), String(device->getAddress().toString().c_str()), device->getAddressType());
    packetStats.bleSeen++;
  }
  bleScan->clearResults();
  restartBleAdvertisement();
  if (screenMode == ScreenMode::Mesh) drawMeshScreen();
}

// ============================================================================
//  BLE Link Service — persistent connection pool management
// ============================================================================

// Service the persistent BLE link pool. Called from loop().
//  1. Iterates scan result BleNode records and attempts to establish links
//     for any node not already connected (up to BLE_LINK_POOL_SIZE).
//  2. Disconnects links that have been idle too long.
//  3. Reconnects links to nodes still in the scan table but whose
//     client connection dropped.
void serviceBleLinks() {
#if !ENABLE_BLE_MESH
  return;
#endif
  if (!bleReady) return;

  const uint32_t now = millis();
  if (now - lastBleLinkServiceAt < BLE_LINK_SERVICE_INTERVAL_MS) return;
  lastBleLinkServiceAt = now;

  // Phase 1: Teardown stale links (idle timeout)
  for (auto &link : bleLinks) {
    if (!link.active) continue;
    if (now - link.lastActivityAt >= BLE_LINK_IDLE_TIMEOUT_MS) {
      appPrintf("[ble] link idle timeout: node=%08lX\n", static_cast<unsigned long>(link.nodeId));
      teardownLink(link);
      continue;
    }
    // Check if the client is still connected (may have dropped)
    if (link.client != nullptr && !link.client->isConnected()) {
      appPrintf("[ble] link dropped: node=%08lX\n", static_cast<unsigned long>(link.nodeId));
      teardownLink(link);
    }
  }

  // Phase 2: Establish links to newly discovered BLE nodes.
  // Guard: only one connect operation at a time, and skip nodes that
  // have been recently torn down (cooldown) to let NimBLE timers settle.
  // This prevents ble_hs_timer_exp asserts from overlapping operations.
  if (!bleLinkBusy) {
    bleLinkBusy = true;
    for (const auto &node : bleNodes) {
      if (!node.active) continue;
      if (now - node.lastSeenAt > BLE_NODE_TTL_MS) continue;
      if (node.nodeId == localNodeId || node.nodeId == 0) continue;
      if (node.address.length() == 0) continue;

      // Skip if this node was recently torn down (cooldown)
      if (isLinkOnCooldown(node.nodeId)) continue;

      // Skip if we already have a link to this node or address
      if (findLinkByNodeId(node.nodeId) != nullptr) continue;
      if (findLinkByAddress(node.address) != nullptr) continue;

      // Check if the pool is full
      BleLink *slot = findEmptyLinkSlot();
      if (slot == nullptr) break;  // pool is full, stop trying

      appPrintf("[ble] establishing link to node=%08lX addr=%s\n",
                static_cast<unsigned long>(node.nodeId), node.address.c_str());
      establishLink(*slot, node.nodeId, node.address, node.addressType);
      // Only attempt one connection per service cycle to avoid flooding
      // NimBLE with concurrent operations.
      break;
    }
    bleLinkBusy = false;
  }

  // Phase 3: Update activity timestamps for still-connected links (keepalive)
  for (auto &link : bleLinks) {
    if (!link.active) continue;
    if (link.client != nullptr && link.client->isConnected()) {
      link.lastSeenAt = now;
    }
  }
}

// ============================================================================
//  BLE Transmit — send over persistent link, fallback to blocking
// ============================================================================

// Send an encoded mesh packet over a persistent BLE link.
// Returns true if the write succeeded.
static bool sendOverLink(BleLink &link, const uint8_t *encoded, size_t len) {
  if (link.txChar == nullptr || link.client == nullptr || !link.client->isConnected()) return false;
  const bool ok = link.txChar->writeValue(encoded, len, false);
  if (ok) {
    link.lastActivityAt = millis();
    packetStats.bleTx++;
  } else {
    packetStats.bleWriteFail++;
  }
  return ok;
}

// Send a mesh packet to a specific BLE node (blocking connect/write/disconnect).
// This is the fallback when no persistent link exists to the target.
// Stops scanning and advertising during the connection attempt.
// Returns true if the packet was successfully written to the remote characteristic.
bool sendBlePacketToBlocking(uint32_t nodeId, const uint8_t *encoded, size_t len) {
#if !ENABLE_BLE_MESH
  (void)nodeId; (void)encoded; (void)len;
  return false;
#endif
  if (!bleReady) return false;
  BleNode node;
  if (!findBleNode(nodeId, node)) return false;

  // First, try to use an existing persistent link
  BleLink *link = findLinkByNodeId(nodeId);
  if (link != nullptr) {
    if (sendOverLink(*link, encoded, len)) return true;
    // Link write failed, tear it down and fall through to new connection
    teardownLink(*link);
  }

  // No persistent link — do connect→write→disconnect
  if (bleScan != nullptr && bleScan->isScanning()) { bleScan->stop(); bleScanInProgress = false; }
  NimBLEDevice::getAdvertising()->stop();
  vTaskDelay(pdMS_TO_TICKS(20));

  NimBLEClient *client = NimBLEDevice::createClient();
  if (client == nullptr) { restartBleAdvertisement(); return false; }
  client->setConnectTimeout(BLE_CONNECT_TIMEOUT_MS);

  bool ok = false;
  if (client->connect(NimBLEAddress(std::string(node.address.c_str()), node.addressType), true, false, false)) {
    NimBLERemoteService *service = client->getService(NimBLEUUID(BLE_MESH_SERVICE_UUID));
    if (service != nullptr) {
      NimBLERemoteCharacteristic *characteristic = service->getCharacteristic(NimBLEUUID(BLE_MESH_PACKET_UUID));
      if (characteristic != nullptr) ok = characteristic->writeValue(encoded, len, false);
    }
    client->disconnect();
    // Do NOT call NimBLEDevice::deleteClient() after a successful connect().
    // The connection has timer handles queued in the BLE soft device;
    // deleting the client causes ble_hs_timer_exp asserts. Just disconnect
    // and let NimBLE clean up the client internally.
    vTaskDelay(pdMS_TO_TICKS(100));
  } else {
    // connect() never completed — no timer handles pending, safe to delete
    NimBLEDevice::deleteClient(client);
    packetStats.bleConnectFail++;
  }
  if (ok) packetStats.bleTx++; else packetStats.bleWriteFail++;
  lastBleTxAt = millis();
  restartBleAdvertisement();
  return ok;
}

// Enqueue a packet for BLE transmission.
bool enqueueBlePacket(uint32_t target, const uint8_t *encoded, size_t len, uint8_t type) {
#if !ENABLE_BLE_MESH
  (void)target; (void)encoded; (void)len; (void)type;
  return false;
#endif
  if (!bleReady || len == 0 || len > MAX_PACKET_LEN) return false;

  portENTER_CRITICAL(&bleQueueMux);
  if (bleQueueCount >= TX_QUEUE_DEPTH) { portEXIT_CRITICAL(&bleQueueMux); bleLine = "BLE queue full"; return false; }

  BleTxJob &job = bleQueue[bleQueueTail];
  memcpy(job.encoded, encoded, len);
  job.len = len;
  job.type = type;
  job.target = target;
  job.active = true;
  bleQueueTail = (bleQueueTail + 1) % TX_QUEUE_DEPTH;
  bleQueueCount++;
  portEXIT_CRITICAL(&bleQueueMux);
  bleLine = "BLE queued";
  return true;
}

// Dequeue a packet from the BLE transmit queue.
bool dequeueBlePacket(BleTxJob &job) {
  portENTER_CRITICAL(&bleQueueMux);
  if (bleQueueCount == 0) { portEXIT_CRITICAL(&bleQueueMux); return false; }
  job = bleQueue[bleQueueHead];
  bleQueue[bleQueueHead].active = false;
  bleQueueHead = (bleQueueHead + 1) % TX_QUEUE_DEPTH;
  bleQueueCount--;
  portEXIT_CRITICAL(&bleQueueMux);
  return true;
}

// Check if any BLE nodes are currently reachable.
bool hasBleTargets() {
#if !ENABLE_BLE_MESH
  return false;
#endif
  const uint32_t now = millis();
  for (const auto &node : bleNodes) {
    if (node.active && node.address.length() > 0 && now - node.lastSeenAt <= BLE_NODE_TTL_MS) return true;
  }
  return false;
}

// Process a BLE transmit job, sending to a specific node or broadcasting to all nearby.
// Tries persistent links first, falls back to blocking connect→write→disconnect.
void processBleTxJob(const BleTxJob &job) {
  if (!job.active) return;

  if (job.target != BROADCAST_NODE) {
    // sendBlePacketToBlocking tries persistent link first then falls
    // back to blocking connect→write→disconnect. This avoids duplicating
    // the link check logic here.
    sendBlePacketToBlocking(job.target, job.encoded, job.len);
    return;
  }

  // Broadcast: send to all known BLE nodes
  const uint32_t now = millis();
  for (const auto &node : bleNodes) {
    if (!node.active || node.address.length() == 0 || now - node.lastSeenAt > BLE_NODE_TTL_MS) continue;
    // sendBlePacketToBlocking handles persistent link first, then fallback
    sendBlePacketToBlocking(node.nodeId, job.encoded, job.len);
    vTaskDelay(pdMS_TO_TICKS(BLE_TX_SETTLE_MS));
  }
}

// Service the BLE transmit queue. Called from loop().
void serviceBleTransmit() {
#if !ENABLE_BLE_MESH
  return;
#endif
  if (bleTxBusy || millis() - lastBleTxAt < BLE_TX_SETTLE_MS) return;
  BleTxJob job;
  if (dequeueBlePacket(job)) {
    bleTxBusy = true;
    processBleTxJob(job);
    bleTxBusy = false;
  }
}

// Process a received BLE mesh packet (called from the main loop after ISR capture).
void handleBlePacket(const uint8_t *buffer, size_t len, const char *peerAddress, uint8_t peerAddressType, bool hasPeer) {
#if !ENABLE_BLE_MESH
  (void)buffer; (void)len; (void)peerAddress; (void)peerAddressType; (void)hasPeer;
  return;
#endif
  Packet packet;
  if (decodePacket(buffer, len, packet)) {
    if (hasPeer && peerAddress != nullptr && peerAddress[0] != '\0') {
      updateBleNode(packet.source, nodeDisplayName(packet.source), 0, String(peerAddress), peerAddressType);
    }
    packetStats.bleRx++;
    noteRx(packet.type);
    packet.rssi = 0;
    bleLine = "BLE rx " + nodeIdHex(packet.source).substring(4);
    handleIncoming(packet);
  }
}

// Service incoming BLE packets captured by the GATT callback.
// Copies data out of the ISR-protected buffer before processing.
void serviceBlePacket() {
#if !ENABLE_BLE_MESH
  return;
#endif
  if (!blePacketPending) return;

  uint8_t localBuffer[MAX_PACKET_LEN];
  char localPeerAddress[sizeof(blePacketPeerAddress)];
  uint8_t localPeerAddressType = 0;
  bool localHasPeer = false;
  portENTER_CRITICAL(&blePacketMux);
  const size_t localLen = min(blePacketLength, static_cast<size_t>(MAX_PACKET_LEN));
  memcpy(localBuffer, blePacketBuffer, min(localLen, static_cast<size_t>(MAX_PACKET_LEN)));
  strlcpy(localPeerAddress, blePacketPeerAddress, sizeof(localPeerAddress));
  localPeerAddressType = blePacketPeerAddressType;
  localHasPeer = blePacketHasPeer;
  blePacketPending = false;
  blePacketLength = 0;
  blePacketPeerAddress[0] = '\0';
  blePacketHasPeer = false;
  portEXIT_CRITICAL(&blePacketMux);
  handleBlePacket(localBuffer, localLen, localPeerAddress, localPeerAddressType, localHasPeer);
}

// Count active (non-expired) BLE-discovered nodes.
uint8_t activeBleNodeCount() {
#if !ENABLE_BLE_MESH
  return 0;
#else
  uint8_t count = 0;
  const uint32_t now = millis();
  for (const auto &node : bleNodes)
    if (node.active && now - node.lastSeenAt <= BLE_NODE_TTL_MS) count++;
  return count;
#endif
}