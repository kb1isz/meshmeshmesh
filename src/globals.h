#pragma once
// ============================================================================
// globals.h — Central include, display adapter, and global declarations
// ============================================================================
#include "config.h"

// ============================================================================
//  Display Adapter — abstracts ST7789 (T-Deck) vs SSD1306 (Heltec V3)
// ============================================================================
#ifdef BOARD_HELTEC_V3
class DisplayAdapter {
public:
  DisplayAdapter() : oled(128, 64, &Wire, BOARD_OLED_RST) {}

  bool begin() { return oled.begin(SSD1306_SWITCHCAPVCC, 0x3C); }
  void setRotation(uint8_t rotation) { oled.setRotation(rotation); }
  void fillScreen(uint16_t color) { oled.fillScreen(toOledColor(color)); }
  void display() { oled.display(); }
  void setTextSize(uint8_t size) { oled.setTextSize(size); }
  void setTextColor(uint16_t color) { oled.setTextColor(toOledColor(color)); }
  void setTextColor(uint16_t color, uint16_t background) {
    oled.setTextColor(toOledColor(color), toOledColor(background));
  }
  void setCursor(int16_t x, int16_t y) { oled.setCursor(x, y); }
  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) { oled.fillRect(x, y, w, h, toOledColor(color)); }
  void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) { oled.drawFastHLine(x, y, w, toOledColor(color)); }
  void drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) { oled.drawFastVLine(x, y, h, toOledColor(color)); }
  void drawCircle(int16_t x, int16_t y, int16_t r, uint16_t color) { oled.drawCircle(x, y, r, toOledColor(color)); }
  void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) { oled.drawLine(x0, y0, x1, y1, toOledColor(color)); }
  void fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color) {
    oled.fillRoundRect(x, y, w, h, r, toOledColor(color));
  }
  void drawRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color) {
    oled.drawRoundRect(x, y, w, h, r, toOledColor(color));
  }

  template <typename T>
  void print(const T &value) { oled.print(value); }

  void printf(const char *format, ...) {
    char buffer[96];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    oled.print(buffer);
  }

private:
  uint16_t toOledColor(uint16_t color) const {
    return color == BLACK ? SSD1306_BLACK : SSD1306_WHITE;
  }
  Adafruit_SSD1306 oled;
};
#else
class DisplayAdapter {
public:
  DisplayAdapter()
      : bus(new Arduino_ESP32SPI(BOARD_TFT_DC, BOARD_TFT_CS, BOARD_SPI_SCK, BOARD_SPI_MOSI, BOARD_SPI_MISO)),
        panel(new Arduino_ST7789(bus, GFX_NOT_DEFINED, 1, true, 240, 320)) {}

  bool begin() { return panel->begin(); }
  void setRotation(uint8_t rotation) { panel->setRotation(rotation); }
  void fillScreen(uint16_t color) { panel->fillScreen(color); }
  void display() {}
  void setTextSize(uint8_t size) { panel->setTextSize(size); }
  void setTextColor(uint16_t color) { panel->setTextColor(color); }
  void setTextColor(uint16_t color, uint16_t background) { panel->setTextColor(color, background); }
  void setCursor(int16_t x, int16_t y) { panel->setCursor(x, y); }
  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) { panel->fillRect(x, y, w, h, color); }
  void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) { panel->drawFastHLine(x, y, w, color); }
  void drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) { panel->drawFastVLine(x, y, h, color); }
  void drawCircle(int16_t x, int16_t y, int16_t r, uint16_t color) { panel->drawCircle(x, y, r, color); }
  void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) { panel->drawLine(x0, y0, x1, y1, color); }
  void fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color) { panel->fillRoundRect(x, y, w, h, r, color); }
  void drawRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color) { panel->drawRoundRect(x, y, w, h, r, color); }

  template <typename T>
  void print(const T &value) { panel->print(value); }

  void printf(const char *format, ...) {
    char buffer[128];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    panel->print(buffer);
  }

private:
  Arduino_DataBus *bus;
  Arduino_GFX *panel;
};
#endif

// ============================================================================
//  Serial I/O helpers
// ============================================================================
#ifdef BOARD_HELTEC_V3
inline void appSerialBegin() {
  Serial.begin(115200);
#if HELTEC_ENABLE_UART0_OUTPUT || HELTEC_ENABLE_UART0_INPUT
  Serial0.begin(115200);
#endif
}
inline void appPrintln(const String &line = "") {
  Serial.println(line);
#if HELTEC_ENABLE_UART0_OUTPUT
  Serial0.println(line);
#endif
}
inline void appPrintf(const char *format, ...) {
  char buffer[192];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  Serial.print(buffer);
#if HELTEC_ENABLE_UART0_OUTPUT
  Serial0.print(buffer);
#endif
}
#else
inline void appSerialBegin() {
  Serial.begin(115200);
}
inline void appPrintln(const String &line = "") {
  Serial.println(line);
}
inline void appPrintf(const char *format, ...) {
  char buffer[192];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  Serial.print(buffer);
}
#endif

// ============================================================================
//  Forward declarations
// ============================================================================

void IRAM_ATTR onRadioPacketReceived();

void flushDisplay();
void clearScreen();
void drawUi();
void drawBottom();
void drawStatusScreen();
void drawMeshScreen();
void drawSettingsScreen();
void drawFrequencyEntryScreen();
void drawButton(int16_t x, int16_t y, int16_t w, int16_t h, const String &label, uint16_t color);
void drawGearIcon();
void drawMeshIcon();
void drawChatTab(int16_t x, int16_t w, const String &label, bool active);
void drawChatRows();
void drawSettingRow(uint8_t row, int16_t y, const String &label, const String &value, bool dropdown);
void drawBootScreen(const String &line);

String nodeIdHex(uint32_t nodeId);
String efuseMacHex();
uint32_t deriveNodeId(uint64_t efuseMac);
void printIdentity();
uint8_t activeRouteCount();
uint8_t activeBleNodeCount();
void selectGroupChat();
void selectDirectChat(uint32_t nodeId);
void selectNextDirectChat();
String directChatLabel();
String nodeDisplayName(uint32_t nodeId);
String settingsSummary();
String defaultDeviceName();
bool hasPendingMessages();
void updateRoute(uint32_t destination, uint32_t nextHop, uint8_t hops);
bool findRoute(uint32_t destination, RouteEntry &out);

uint16_t crc16Ccitt(const uint8_t *data, size_t len);
void writeU32(uint8_t *buffer, size_t offset, uint32_t value);
uint32_t readU32(const uint8_t *buffer, size_t offset);
size_t encodePacket(uint8_t type, uint32_t source, uint32_t destination, uint32_t messageId, uint8_t ttl, uint32_t nextHop, const String &body, uint8_t *out);
bool decodePacket(const uint8_t *buffer, size_t len, Packet &packet);

String encryptedPayloadFor(const String &message);
bool decryptPayload(const String &body, String &sender, String &message);
void deriveEncryptionKey(uint8_t out[AES_KEY_LEN]);
bool aesCtrCrypt(const uint8_t key[AES_KEY_LEN], const uint8_t nonce[AES_BLOCK_LEN], const uint8_t *input, size_t len, uint8_t *output);

void addEntryToBuffer(ChatBuffer &buffer, const ChatEntry &entry);
DirectChat *getDirectChat(uint32_t nodeId, const String &name);
ChatBuffer &activeChatBuffer();
void addGroupEntry(const String &line, uint16_t color = WHITE, uint32_t messageId = 0, bool outgoing = false, bool acked = false);
void addDirectEntry(uint32_t nodeId, const String &name, const String &line, uint16_t color = WHITE, uint32_t messageId = 0, bool outgoing = false, bool acked = false);
void addLine(const String &line);
void markMessageAcked(uint32_t messageId);
void markMessageFailed(uint32_t messageId);
void markAckedInBuffer(ChatBuffer &buffer, uint32_t messageId);
void markFailedInBuffer(ChatBuffer &buffer, uint32_t messageId);
void updateTransportInBuffer(ChatBuffer &buffer, uint32_t messageId, bool useBle);
void updateMessageTransport(uint32_t messageId, bool useBle);

bool isDuplicate(uint32_t source, uint32_t messageId);
void handleIncoming(const Packet &packet);
void sendRouteRequest(uint32_t target);
void sendHello();
void queueBleHelloBroadcast(bool authoritativeSync);
void queueBleHelloTo(uint32_t target, bool authoritativeSync);
bool sendHelloImmediate(bool trackSlot = true, bool authoritativeSync = false);
void queueRelay(const Packet &packet);
void forwardUnicast(const Packet &packet);
void cancelGroupAck(uint32_t dataSource, uint32_t messageId);
void noteGroupAckHeard(uint32_t dataSource, uint32_t messageId);
bool hasHeardGroupAck(uint32_t dataSource, uint32_t messageId);
void scheduleGroupAck(const Packet &packet);
void sendGroupAck(uint32_t dataSource, uint32_t messageId);

bool enqueuePendingMessage(const PendingMessage &msg);
bool dequeuePendingMessage(PendingMessage &msg);
bool enqueueLoRaPacket(const uint8_t *encoded, size_t len, uint8_t type);
bool dequeueLoRaPacket(LoRaTxJob &job);
void noteTx(uint8_t type);
void noteRx(uint8_t type);
bool transmitPacket(uint8_t type, uint32_t destination, uint32_t messageId, uint8_t ttl, uint32_t nextHop, const String &body);
bool transmitPacketFrom(uint8_t type, uint32_t source, uint32_t destination, uint32_t messageId, uint8_t ttl, uint32_t nextHop, const String &body);
void sendAck(const Packet &packet);
void queueOutgoing(String body);
void scheduleLoRaBackoff(uint32_t minMs, uint32_t maxMs);
bool startLoRaJob(const LoRaTxJob &job);

void configureBleAdvertisement();
void restartBleAdvertisement();
bool sendBlePacketToBlocking(uint32_t nodeId, const uint8_t *encoded, size_t len);
bool enqueueBlePacket(uint32_t target, const uint8_t *encoded, size_t len, uint8_t type);
bool dequeueBlePacket(BleTxJob &job);
bool hasBleTargets();
bool findBleNode(uint32_t nodeId, BleNode &out);
void updateBleNode(uint32_t nodeId, const String &name, int rssi, const String &address, uint8_t addressType);
bool processBleTxJob(const BleTxJob &job);

void servicePending();
void serviceRelay();
void serviceBeacon();
void serviceRadio();
void serviceLoRaTransmit();
void serviceBleTransmit();
void serviceBlePacket();
void serviceBleLinks();
void serviceBleLocation();
void serviceGroupAcks();
void serviceInput();
void serviceTouch();
void serviceStoreForward();    // Store-and-forward retry service

void loadRadioSettings();
void saveRadioSettings();
void openSettingsScreen();
void closeSettingsScreen();
void closeMeshScreen();
bool applyRadioSettings(bool haltOnFailure);
void setupRadio();
void setupDisplay();
void setupKeyboard();
void setupTouch();
void setupBleLocation();
bool validRadioSettings(const RadioSettings &settings);
void handleTouchPoint(int16_t x, int16_t y);
bool handleCommand(String line);
void handleSettingsInputChar(char key);
void handleFrequencyInputChar(char key);
void setKeyboardBrightness(uint8_t value);
void setKeyboardDefaultBrightness(uint8_t value);
void handleInputChar(char key, bool commandsOnly = false, String *lineOverride = nullptr);

// Frequency hopping
void buildHopChannels();
void hoppingBootSync();
void serviceHopping();
bool retuneToFrequency(float freq);
int32_t hopNetworkTime();
uint8_t hopNetworkSlot(int32_t networkTime);
uint32_t hopSlotRemainingMs(int32_t networkTime);
void applyHopSync(uint8_t remoteSlot, int32_t remoteNetworkTime);

// Message fragmentation
bool sendFragmented(const String &body, uint32_t destination, uint32_t conversation);
void handleFragment(const Packet &packet);
void serviceFragments();

// Emergency messages
void sendEmergency(const String &message);
void handleEmergencyAck(const Packet &packet);

// Store-and-forward
void storeMessage(const StoredMessage &msg);
void retryStoredMessages();
void promotePendingToStored();
void triggerStoredMessageRetry(uint32_t nodeId);
void persistStoredMessages();
void loadStoredMessages();

// Persistent chat history
void saveChatHistory();
void loadChatHistory();

// Contact list
void addContact(uint32_t nodeId, const String &name, int16_t rssi);
void updateContactSeen(uint32_t nodeId, const String &name, int16_t rssi);
bool findContact(uint32_t nodeId, ContactEntry &out);
void serviceContactDiscovery();

// Node roles
NodeRole getNodeRole();
void setNodeRole(NodeRole role);

// Position / GPS
#if ENABLE_GPS_TELEMETRY
void serviceGps();
void servicePositionBeacon();
void handlePositionData(const Packet &packet);
void sendPosition();
#endif

// Overflow-safe millis comparison
inline bool timeBefore(uint32_t targetTime) {
  return static_cast<int32_t>(targetTime - millis()) > 0;
}

// ============================================================================
//  Global externs
// ============================================================================
extern DisplayAdapter gfx;
extern Module radioModule;
extern SX1262 radio;
#ifndef BOARD_HELTEC_V3
extern TouchDrvGT911 touch;
#endif
extern Preferences preferences;
extern NimBLEScan *bleScan;
extern NimBLEServer *bleServer;
extern NimBLECharacteristic *blePacketCharacteristic;
extern BleLink bleLinks[BLE_LINK_POOL_SIZE];
extern uint32_t lastBleLinkServiceAt;
extern uint8_t bleLinkCount;

extern RadioSettings radioSettings;
extern RadioSettings draftSettings;
extern SettingsTab settingsTab;
extern ChatTab chatTab;
extern PendingMessage pending;
extern PendingMessage pendingQueue[TX_QUEUE_DEPTH];
extern RelayMessage relay;
extern GroupAckJob groupAckJobs[TX_QUEUE_DEPTH];
extern HeardAck heardAcks[32];
extern LoRaTxJob loraTx;
extern LoRaTxJob loraQueue[TX_QUEUE_DEPTH];
extern BleTxJob bleQueue[TX_QUEUE_DEPTH];
extern SeenMessage seen[32];
extern RouteEntry routes[16];
extern BleNode bleNodes[12];
extern ChatBuffer groupChat;
extern DirectChat directChats[6];
extern PacketStats packetStats;
extern uint8_t seenNext;
extern uint8_t heardAckNext;
extern String inputLine;
#ifdef BOARD_HELTEC_V3
extern String uart0InputLine;
#endif
extern ScreenMode screenMode;
extern String draftFrequencyText;
extern String keypadFrequencyText;
extern String deviceName;
extern String encryptionKey;
extern String draftDeviceName;
extern String draftEncryptionKey;
extern bool serialLineJustSubmitted;
extern uint8_t selectedSettingsRow;
extern uint8_t pendingQueueHead, pendingQueueTail, pendingQueueCount;
extern uint8_t loraQueueHead, loraQueueTail, loraQueueCount;
extern uint8_t bleQueueHead, bleQueueTail, bleQueueCount;
extern uint32_t localNodeId;
extern uint32_t nextMessageId;
extern uint32_t selectedDestination;
extern uint32_t selectedDirectNode;
extern uint32_t lastHelloAt;
extern uint32_t lastRouteDiscoveryAt;
extern uint32_t routeDiscoveryStartedAt;
extern uint32_t lastBleScanAt;
extern uint32_t bleScanStartedAt;
extern uint32_t lastStatusAt;
extern uint32_t lastSerialStatusAt;
extern uint32_t lastBleTxAt;
extern uint32_t loraClearChannelAt;
extern uint32_t lastMeshDrawAt;
extern String statusLine;
extern String aodvLine;
extern String bleLine;
extern volatile bool radioPacketReceived;
extern bool radioStarted;
extern bool radioTransmitting;
extern bool loraBackoffActive;
extern bool touchReady;
extern bool bleReady;
extern bool touchWasPressed;
extern bool blePacketPending;
extern bool bleScanInProgress;
extern bool bleTxBusy;
extern uint8_t blePacketBuffer[MAX_PACKET_LEN];
extern size_t blePacketLength;
extern char blePacketPeerAddress[18];
extern uint8_t blePacketPeerAddressType;
extern bool blePacketHasPeer;
extern portMUX_TYPE blePacketMux;
extern portMUX_TYPE loraQueueMux;
extern portMUX_TYPE bleQueueMux;
extern uint32_t lastTouchAt;
extern float batteryVoltage;
extern uint8_t batteryPercent;
extern uint32_t lastBatteryReadAt;

float readBatteryVoltage();
uint8_t computeBatteryPercent(float voltage);
void serviceBattery();

// Frequency hopping
extern float *hopChannels;        // dynamically allocated from bandwidth
extern uint8_t hopCount;          // number of hop channels
extern uint8_t hopSlot;
extern bool hoppingSynced;
extern int32_t hopEpochOffset;
extern uint32_t lastHopSyncAt;
extern uint32_t hopIntervalMs;    // ms per hop (computed from LoRa airtime)
extern uint8_t hopLastTxSlot;     // slot of last TX (for one-TX-per-slot gate)
extern uint8_t hopLastRxSlot;     // slot of last RX (block TX in next slot)

extern const float BANDWIDTH_OPTIONS[10];
extern const uint8_t BANDWIDTH_OPTION_COUNT;

float normalizedBandwidth(float requested);
uint8_t bandwidthIndex(float bandwidth);
void cycleDraftOption(uint8_t row);
void saveDraftSettings();
bool applyCommandSettings(const RadioSettings &updated, const String &label);
void openFrequencyEntryScreen();
void acceptFrequencyEntry();
void cancelFrequencyEntry();
char readKeyboard();
char hexNibble(uint8_t value);
int8_t fromHexNibble(char value);
String bytesToHex(const uint8_t *data, size_t len);
bool hexToBytes(const String &hex, uint8_t *out, size_t maxLen, size_t &outLen);
bool parseNodeId(const String &text, uint32_t &nodeId);
uint32_t firstDirectNode();
bool decodeBleBeaconData(const std::string &data, uint32_t &nodeId, String &name,
                         bool &hasSync, bool &authoritativeSync,
                         uint8_t &syncSlot, int32_t &syncNetworkTime);

// New feature globals
extern FragmentAssembly fragmentAssemblies[4];
extern StoredMessage storedMessages[STORE_FORWARD_MAX_MESSAGES];
extern ContactEntry contacts[16];
extern PositionData lastPosition;
extern NodeRole nodeRole;
extern uint32_t lastPositionBeaconAt;
extern uint32_t lastContactDiscoveryAt;
extern uint32_t lastFragServiceAt;
extern uint32_t lastHistorySaveAt;
