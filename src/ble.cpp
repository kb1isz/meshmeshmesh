// ============================================================================
// ble.cpp — BLE mesh transport: scanning and packet exchange
//
// Implements:
//  - BLE GATT service for mesh packet relay (when ENABLE_BLE_MESH=1)
//  - Manufacturer advertisement beacon with node ID and name
//  - Periodic scanning for nearby T-Deck mesh nodes
//  - BLE transmit queue with on-demand connect→write→disconnect delivery
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

class MeshServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *server, NimBLEConnInfo &connInfo) override {
    (void)server;
    (void)connInfo;
    bleLine = "BLE peer connected";
  }

  void onDisconnect(NimBLEServer *server, NimBLEConnInfo &connInfo, int reason) override {
    (void)server;
    (void)connInfo;
    (void)reason;
    bleLine = "BLE peer disconnected";
    restartBleAdvertisement();
  }
};
} // namespace

// ============================================================================
//  BLE Link Pool (dormant persistent-link support)
// ============================================================================

// Busy flag for the dormant persistent-link path.
static bool bleLinkBusy = false;

// Cooldown tracking — after tearing down a link, prevent reconnecting to the
// same node for a short period so NimBLE's soft device timers settle.
// This avoids `ble_hs_timer_exp` asserts from overlapping client operations.
#define BLE_LINK_COOLDOWN_MS 10000
#define BLE_LINK_COOLDOWN_SLOTS 4

// Global client-operation guard — after any NimBLE client create/connect/disconnect,
// wait this long before creating another client. This prevents ble_hs_timer_exp
// asserts from stale timer handles in the NimBLE soft device.
#define BLE_CLIENT_OP_COOLDOWN_MS 2000
static uint32_t lastClientOpAt = 0;
static uint32_t linkCooldownNode[BLE_LINK_COOLDOWN_SLOTS] = {0};
static uint32_t linkCooldownUntil[BLE_LINK_COOLDOWN_SLOTS] = {0};
static uint8_t linkCooldownNext = 0;
static uint32_t lastBleAdvRefreshAt = 0;
static TaskHandle_t bleTxTaskHandle = nullptr;

constexpr size_t BLE_ADV_MARKER_LEN = 4;
constexpr size_t BLE_ADV_NONCE_SEED_LEN = 4;
constexpr size_t BLE_ADV_PLAIN_LEN = 18;
constexpr size_t BLE_ADV_NAME_LEN = 6;
constexpr size_t BLE_ADV_TOTAL_LEN = BLE_ADV_MARKER_LEN + BLE_ADV_NONCE_SEED_LEN + BLE_ADV_PLAIN_LEN;

static void buildBleAdvNonce(uint32_t seed, uint8_t nonce[AES_BLOCK_LEN]) {
  memset(nonce, 0, AES_BLOCK_LEN);
  nonce[0] = 'T';
  nonce[1] = 'D';
  nonce[2] = 'A';
  nonce[3] = '4';
  nonce[4] = static_cast<uint8_t>(seed & 0xFF);
  nonce[5] = static_cast<uint8_t>((seed >> 8) & 0xFF);
  nonce[6] = static_cast<uint8_t>((seed >> 16) & 0xFF);
  nonce[7] = static_cast<uint8_t>((seed >> 24) & 0xFF);
}

static bool cryptBleAdvPayload(uint32_t nonceSeed, const uint8_t *input, uint8_t *output) {
  uint8_t key[AES_KEY_LEN];
  uint8_t nonce[AES_BLOCK_LEN];
  deriveEncryptionKey(key);
  buildBleAdvNonce(nonceSeed, nonce);
  const bool ok = aesCtrCrypt(key, nonce, input, BLE_ADV_PLAIN_LEN, output);
  memset(key, 0, sizeof(key));
  memset(nonce, 0, sizeof(nonce));
  return ok;
}

static bool bleClientOpCoolingDown() {
  return millis() - lastClientOpAt < BLE_CLIENT_OP_COOLDOWN_MS;
}

static void bleTransmitTask(void *parameter) {
  (void)parameter;
  for (;;) {
    serviceBleTransmit();
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

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
    lastClientOpAt = millis();
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
  // Global cooldown: skip this attempt if NimBLE timers may still be settling
  // from a previous client operation. The caller (serviceBleLinks) will retry
  // on the next service cycle (2 seconds).
  if (bleClientOpCoolingDown()) return false;

  NimBLEClient *client = NimBLEDevice::createClient();
  if (client == nullptr) return false;
  lastClientOpAt = millis();

  client->setConnectTimeout(BLE_CONNECT_TIMEOUT_MS);

  if (!client->connect(NimBLEAddress(std::string(addr.c_str()), addrType), true, false, false)) {
    // connect() never completed — no timer handles pending, safe to delete
    recordLinkCooldown(nodeId);
    NimBLEDevice::deleteClient(client);
    lastClientOpAt = millis();
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
    recordLinkCooldown(nodeId);
    lastClientOpAt = millis();
    packetStats.bleConnectFail++;
    return false;
  }

  NimBLERemoteCharacteristic *txChar = service->getCharacteristic(NimBLEUUID(BLE_MESH_PACKET_UUID));
  if (txChar == nullptr) {
    client->disconnect();
    recordLinkCooldown(nodeId);
    lastClientOpAt = millis();
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

// Update or create a BLE node entry from a scan result. Also creates a 1-hop
// route entry to this node so the transmitter can try BLE before LoRa.
void updateBleNode(uint32_t nodeId, const String &name, int rssi, const String &address, uint8_t addressType) {
#if !ENABLE_BLE_MESH
  (void)nodeId; (void)name; (void)rssi; (void)address; (void)addressType;
  return;
#endif
  if (nodeId == 0 || nodeId == localNodeId) return;

  BleNode *slot = nullptr;
  bool isNewNode = true;
  for (auto &node : bleNodes) {
    if (node.active && node.nodeId == nodeId) { slot = &node; isNewNode = false; break; }
    if (!node.active && slot == nullptr) slot = &node;
  }
  if (slot == nullptr) slot = &bleNodes[0];
  if (slot->active && slot->nodeId != nodeId) isNewNode = true;

  slot->active = true;
  slot->nodeId = nodeId;
  slot->name = name.substring(0, 16);
  slot->address = address;
  slot->addressType = addressType;
  slot->rssi = rssi;
  slot->lastSeenAt = millis();
  updateRoute(nodeId, nodeId, 1);
  bleLine = "BLE " + nodeIdHex(nodeId).substring(4) + " " + String(rssi) + "dBm";
  if (isNewNode) {
    if (!hoppingSynced) {
      queueBleHelloTo(nodeId, false);
    }
  }
}

// Decode an encrypted BLE manufacturer data beacon into node ID, optional FHSS
// sync, and name. Only the outer marker and nonce seed are plaintext:
//   ['T','D','E','4', 4-byte nonce seed, AES-CTR(ciphertext...)]
// Decrypted payload:
//   [4-byte LE nodeId, state, slot, 4-byte LE networkTime, 6-byte name, crc16]
// Returns false if the beacon is not a valid T-Deck mesh advertisement.
bool decodeBleBeaconData(const std::string &data, uint32_t &nodeId, String &name,
                         bool &hasSync, bool &authoritativeSync,
                         uint8_t &syncSlot, int32_t &syncNetworkTime) {
  hasSync = false;
  authoritativeSync = false;
  syncSlot = 0;
  syncNetworkTime = 0;
  if (data.size() != BLE_ADV_TOTAL_LEN || data[0] != 'T' || data[1] != 'D' || data[2] != 'E' || data[3] != '4')
    return false;

  const uint32_t nonceSeed =
      static_cast<uint32_t>(static_cast<uint8_t>(data[4])) |
      (static_cast<uint32_t>(static_cast<uint8_t>(data[5])) << 8) |
      (static_cast<uint32_t>(static_cast<uint8_t>(data[6])) << 16) |
      (static_cast<uint32_t>(static_cast<uint8_t>(data[7])) << 24);
  uint8_t cipher[BLE_ADV_PLAIN_LEN];
  uint8_t plain[BLE_ADV_PLAIN_LEN];
  memcpy(cipher, data.data() + BLE_ADV_MARKER_LEN + BLE_ADV_NONCE_SEED_LEN, sizeof(cipher));
  if (!cryptBleAdvPayload(nonceSeed, cipher, plain)) return false;

  const uint16_t expectedCrc = static_cast<uint16_t>(plain[16]) | (static_cast<uint16_t>(plain[17]) << 8);
  if (crc16Ccitt(plain, 16) != expectedCrc) {
    memset(plain, 0, sizeof(plain));
    return false;
  }

  nodeId = readU32(plain, 0);
  hasSync = true;
  authoritativeSync = plain[4] == 'S';
  syncSlot = plain[5];
  syncNetworkTime = static_cast<int32_t>(readU32(plain, 6));
  name = "";
  for (size_t j = 0; j < BLE_ADV_NAME_LEN; ++j) {
    const char c = static_cast<char>(plain[10 + j]);
    if (c == 0) break;
    name += c;
  }
  memset(plain, 0, sizeof(plain));
  return nodeId != 0;
}

// Configure the BLE advertisement with mesh beacon data and device name.
void configureBleAdvertisement() {
#if !ENABLE_BLE_MESH
  bleReady = false;
  bleLine = "BLE disabled";
  return;
#endif
  uint8_t plain[BLE_ADV_PLAIN_LEN] = {};
  const int32_t networkTime = hoppingSynced ? static_cast<int32_t>(hopNetworkTime()) : 0;
  writeU32(plain, 0, localNodeId);
  plain[4] = hoppingSynced ? 'S' : 'D';
  plain[5] = hopSlot;
  writeU32(plain, 6, static_cast<uint32_t>(networkTime));
  const String advertisedName = deviceName.substring(0, BLE_ADV_NAME_LEN);
  memcpy(plain + 10, advertisedName.c_str(), advertisedName.length());
  const uint16_t crc = crc16Ccitt(plain, 16);
  plain[16] = static_cast<uint8_t>(crc & 0xFF);
  plain[17] = static_cast<uint8_t>((crc >> 8) & 0xFF);

  uint8_t manufacturerData[BLE_ADV_TOTAL_LEN] = {'T', 'D', 'E', '4'};
  const uint32_t nonceSeed = esp_random();
  manufacturerData[4] = static_cast<uint8_t>(nonceSeed & 0xFF);
  manufacturerData[5] = static_cast<uint8_t>((nonceSeed >> 8) & 0xFF);
  manufacturerData[6] = static_cast<uint8_t>((nonceSeed >> 16) & 0xFF);
  manufacturerData[7] = static_cast<uint8_t>((nonceSeed >> 24) & 0xFF);
  if (!cryptBleAdvPayload(nonceSeed, plain, manufacturerData + BLE_ADV_MARKER_LEN + BLE_ADV_NONCE_SEED_LEN)) {
    memset(plain, 0, sizeof(plain));
    bleLine = "BLE adv encrypt failed";
    return;
  }
  memset(plain, 0, sizeof(plain));

  NimBLEAdvertisementData advertisement;
  advertisement.setFlags(0x06);
  advertisement.setManufacturerData(std::string(reinterpret_cast<char *>(manufacturerData), sizeof(manufacturerData)));

  NimBLEAdvertisementData scanResponse;

  NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
  advertising->stop();
  advertising->setAdvertisementData(advertisement);
  advertising->setScanResponseData(scanResponse);
  advertising->start();
  lastBleAdvRefreshAt = millis();
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
  bleServer->setCallbacks(new MeshServerCallbacks());
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
  if (bleTxTaskHandle == nullptr) {
    const BaseType_t created = xTaskCreatePinnedToCore(
        bleTransmitTask,
        "ble_tx",
        4096,
        nullptr,
        1,
        &bleTxTaskHandle,
        0);
    if (created != pdPASS) {
      bleLine = "BLE tx worker failed";
    }
  }
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
  if (!bleScanInProgress && now - lastBleAdvRefreshAt >= BLE_ADV_REFRESH_MS) {
    configureBleAdvertisement();
  }

  if (!bleScanInProgress) {
    if (now - lastBleScanAt < BLE_SCAN_INTERVAL_MS) return;
    lastBleScanAt = now;
    bleScanStartedAt = now;
    packetStats.bleScans++;
    bleLine = "BLE scanning";
    NimBLEDevice::getAdvertising()->stop();
    vTaskDelay(pdMS_TO_TICKS(10));
    bleScanInProgress = bleScan->start(BLE_SCAN_DURATION_MS, false, true);
    if (!bleScanInProgress) {
      bleLine = "BLE scan busy";
      restartBleAdvertisement();
    }
    return;
  }

  // Wait for the scan to complete (allow 100ms grace period).
  if (bleScan->isScanning() && now - bleScanStartedAt < BLE_SCAN_DURATION_MS + 100) return;

  bleScanInProgress = false;
  NimBLEScanResults results = bleScan->getResults();
  bleLine = "BLE scan " + String(results.getCount()) + " adv";
  uint8_t meshBeaconCount = 0;
  for (int i = 0; i < results.getCount(); ++i) {
    const NimBLEAdvertisedDevice *device = results.getDevice(i);
    if (device == nullptr) continue;

    uint32_t nodeId = 0;
    String name = "";
    bool hasSync = false;
    bool authoritativeSync = false;
    uint8_t syncSlot = 0;
    int32_t syncNetworkTime = 0;
    for (uint8_t dataIndex = 0; dataIndex < device->getManufacturerDataCount(); ++dataIndex) {
      if (decodeBleBeaconData(device->getManufacturerData(dataIndex), nodeId, name,
                              hasSync, authoritativeSync, syncSlot, syncNetworkTime)) break;
    }
    if (nodeId == 0) continue;
    meshBeaconCount++;
    if (device->haveName()) name = String(device->getName().c_str());
    if (name.length() == 0) name = nodeIdHex(nodeId);
    updateBleNode(nodeId, name, device->getRSSI(), String(device->getAddress().toString().c_str()), device->getAddressType());
    packetStats.bleSeen++;
    if (hasSync && authoritativeSync && !hoppingSynced) {
      applyHopSync(syncSlot, syncNetworkTime);
      hoppingSynced = true;
      lastHopSyncAt = millis();
      bleLine = "BLE sync " + nodeIdHex(nodeId).substring(4);
    }
  }
  bleScan->clearResults();
  restartBleAdvertisement();
  if (screenMode == ScreenMode::Mesh) drawMeshScreen();
}

// ============================================================================
//  BLE Link Service — cleanup for dormant persistent links
// ============================================================================

// Service any pre-existing persistent BLE links. Background link establishment
// is intentionally disabled: repeated proactive NimBLE client connects can trip
// ble_hs_timer_exp asserts on ESP32. Mesh packets use on-demand writes instead.
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
      teardownLink(link);
      continue;
    }
    // Check if the client is still connected (may have dropped)
    if (link.client != nullptr && !link.client->isConnected()) {
      teardownLink(link);
    }
  }

  // Update activity timestamps for still-connected links (keepalive)
  for (auto &link : bleLinks) {
    if (!link.active) continue;
    if (link.client != nullptr && link.client->isConnected()) {
      link.lastSeenAt = now;
    }
  }
}

// ============================================================================
//  BLE Transmit — send over an existing link or on-demand connection
// ============================================================================

// Send an encoded mesh packet over a persistent BLE link.
// Returns true if the write succeeded.
static bool sendOverLink(BleLink &link, const uint8_t *encoded, size_t len) {
  if (link.txChar == nullptr || link.client == nullptr || !link.client->isConnected()) return false;
  const bool ok = link.txChar->writeValue(encoded, len, true);
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

  // No persistent link — do connect→write→disconnect.
  // Global cooldown: skip if NimBLE timers may still be settling.
  // The caller (processBleTxJob) will return to serviceBleTransmit which
  // will re-queue or retry on the next cycle.
  if (bleClientOpCoolingDown()) return false;
  if (bleScan != nullptr && bleScan->isScanning()) { bleScan->stop(); bleScanInProgress = false; }
  NimBLEDevice::getAdvertising()->stop();
  vTaskDelay(pdMS_TO_TICKS(20));

  NimBLEClient *client = NimBLEDevice::createClient();
  if (client == nullptr) { lastClientOpAt = millis(); restartBleAdvertisement(); return false; }
  lastClientOpAt = millis();
  client->setConnectTimeout(BLE_CONNECT_TIMEOUT_MS);

  bool ok = false;
  if (client->connect(NimBLEAddress(std::string(node.address.c_str()), node.addressType), true, false, false)) {
    vTaskDelay(pdMS_TO_TICKS(75));
    NimBLERemoteService *service = client->getService(NimBLEUUID(BLE_MESH_SERVICE_UUID));
    if (service != nullptr) {
      NimBLERemoteCharacteristic *characteristic = service->getCharacteristic(NimBLEUUID(BLE_MESH_PACKET_UUID));
      if (characteristic != nullptr) {
        // Require an ATT write response. A no-response write followed by an
        // immediate disconnect can be dropped while still returning success.
        ok = characteristic->writeValue(encoded, len, true);
        if (ok) vTaskDelay(pdMS_TO_TICKS(30));
        else bleLine = "BLE write failed";
      } else {
        bleLine = "BLE char missing";
      }
    } else {
      bleLine = "BLE service missing";
    }
    client->disconnect();
    // Do NOT call NimBLEDevice::deleteClient() after a successful connect().
    // The connection has timer handles queued in the BLE soft device;
    // deleting the client causes ble_hs_timer_exp asserts. Just disconnect
    // and let NimBLE clean up the client internally.
    vTaskDelay(pdMS_TO_TICKS(100));
    lastClientOpAt = millis();
  } else {
    // connect() never completed — no timer handles pending, safe to delete
    NimBLEDevice::deleteClient(client);
    lastClientOpAt = millis();
    packetStats.bleConnectFail++;
    bleLine = "BLE connect failed";
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

  if (target == BROADCAST_NODE) {
    bool queuedAny = false;
    const uint32_t now = millis();
    for (const auto &node : bleNodes) {
      if (!node.active || node.address.length() == 0 || now - node.lastSeenAt > BLE_NODE_TTL_MS) continue;
      if (enqueueBlePacket(node.nodeId, encoded, len, type)) queuedAny = true;
    }
    if (queuedAny) bleLine = "BLE broadcast queued";
    return queuedAny;
  }

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
// Tries an existing link first, then falls back to on-demand connect→write→disconnect.
bool processBleTxJob(const BleTxJob &job) {
  if (!job.active) return false;

  // Broadcast packets are expanded into per-peer jobs by enqueueBlePacket().
  // Processing only one target here avoids dropping later peers during NimBLE's
  // post-connect cooldown window.
  const bool sent = sendBlePacketToBlocking(job.target, job.encoded, job.len);
  if (sent) noteTx(job.type);
  return sent;
}

// Service the BLE transmit queue. Called from loop().
void serviceBleTransmit() {
#if !ENABLE_BLE_MESH
  return;
#endif
  if (bleTxBusy || millis() - lastBleTxAt < BLE_TX_SETTLE_MS) return;
  if (bleClientOpCoolingDown()) return;
  BleTxJob job;
  if (dequeueBlePacket(job)) {
    bleTxBusy = true;
    const bool sent = processBleTxJob(job);
    bleTxBusy = false;
    const bool packetIsBroadcast = job.len >= 12 && readU32(job.encoded, 8) == BROADCAST_NODE;
    if (!sent && !packetIsBroadcast && job.target != BROADCAST_NODE && radioStarted) {
      enqueueLoRaPacket(job.encoded, job.len, job.type);
      bleLine = "BLE failed; LoRa fallback";
    }
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
  } else {
    bleLine = "BLE decode fail " + String(len) + "B";
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
