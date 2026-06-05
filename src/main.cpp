// ============================================================================
// main.cpp — Application entry point and main event loop
// ============================================================================
#include "globals.h"

// ============================================================================
//  ALL global variable definitions (declared extern in globals.h)
// ============================================================================

DisplayAdapter gfx;
Module radioModule(RADIO_CS_PIN, RADIO_DIO1_PIN, RADIO_RST_PIN, RADIO_BUSY_PIN);
SX1262 radio = &radioModule;
#ifndef BOARD_HELTEC_V3
TouchDrvGT911 touch;
#endif
Preferences preferences;
NimBLEScan *bleScan = nullptr;
NimBLEServer *bleServer = nullptr;
NimBLECharacteristic *blePacketCharacteristic = nullptr;

RadioSettings radioSettings;
RadioSettings draftSettings;
SettingsTab settingsTab = SettingsTab::LoRa;
ChatTab chatTab = ChatTab::Group;
PendingMessage pending;
PendingMessage pendingQueue[TX_QUEUE_DEPTH];
RelayMessage relay;
GroupAckJob groupAckJobs[TX_QUEUE_DEPTH];
HeardAck heardAcks[32];
LoRaTxJob loraTx;
LoRaTxJob loraQueue[TX_QUEUE_DEPTH];
BleTxJob bleQueue[TX_QUEUE_DEPTH];
SeenMessage seen[32];
RouteEntry routes[16];
BleNode bleNodes[12];
ChatBuffer groupChat;
DirectChat directChats[6];
PacketStats packetStats;
uint8_t seenNext = 0;
uint8_t heardAckNext = 0;
String inputLine;
#ifdef BOARD_HELTEC_V3
String uart0InputLine;
#endif
ScreenMode screenMode = ScreenMode::Chat;
String draftFrequencyText;
String keypadFrequencyText;
String deviceName;
String encryptionKey;
String draftDeviceName;
String draftEncryptionKey;
bool serialLineJustSubmitted = false;
uint8_t selectedSettingsRow = 0;
uint8_t pendingQueueHead = 0, pendingQueueTail = 0, pendingQueueCount = 0;
uint8_t loraQueueHead = 0, loraQueueTail = 0, loraQueueCount = 0;
uint8_t bleQueueHead = 0, bleQueueTail = 0, bleQueueCount = 0;
uint32_t localNodeId = 0;
uint32_t nextMessageId = 1;
uint32_t selectedDestination = BROADCAST_NODE;
uint32_t selectedDirectNode = 0;
uint32_t lastHelloAt = 0;
uint32_t lastRouteDiscoveryAt = 0;
uint32_t routeDiscoveryStartedAt = 0;
uint32_t lastBleScanAt = 0;
uint32_t bleScanStartedAt = 0;
uint32_t lastStatusAt = 0;
uint32_t lastSerialStatusAt = 0;
uint32_t lastBleTxAt = 0;
uint32_t loraClearChannelAt = 0;
uint32_t lastMeshDrawAt = 0;
String statusLine = "booting";
String aodvLine = "mesh idle";
String bleLine = "BLE idle";
volatile bool radioPacketReceived = false;
bool radioStarted = false;
bool radioTransmitting = false;
bool loraBackoffActive = false;
bool touchReady = false;
bool bleReady = false;
bool touchWasPressed = false;
bool blePacketPending = false;
bool bleScanInProgress = false;
bool bleTxBusy = false;
BleLink bleLinks[BLE_LINK_POOL_SIZE];
uint32_t lastBleLinkServiceAt = 0;
uint8_t bleLinkCount = 0;
uint8_t blePacketBuffer[MAX_PACKET_LEN];
size_t blePacketLength = 0;
char blePacketPeerAddress[18] = "";
uint8_t blePacketPeerAddressType = 0;
bool blePacketHasPeer = false;
portMUX_TYPE blePacketMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE loraQueueMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE bleQueueMux = portMUX_INITIALIZER_UNLOCKED;
uint32_t lastTouchAt = 0;
float batteryVoltage = 0.0f;
uint8_t batteryPercent = 0;
uint32_t lastBatteryReadAt = 0;

// Frequency hopping state
float *hopChannels = nullptr;      // dynamically allocated from bandwidth
uint8_t hopCount = 0;
uint8_t hopSlot = 0;
bool hoppingSynced = false;
int32_t hopEpochOffset = 0;
uint32_t lastHopSyncAt = 0;
uint32_t hopIntervalMs = 2000;
uint8_t hopLastTxSlot = 0xFF;  // slot of last TX (init to invalid)
uint8_t hopLastRxSlot = 0xFF;  // slot of last RX (init to invalid)

// ============================================================================
//  ISR
// ============================================================================
void IRAM_ATTR onRadioPacketReceived() {
  radioPacketReceived = true;
}

// ============================================================================
//  setup()
// ============================================================================
void setup() {
  appSerialBegin();
#ifdef BOARD_HELTEC_V3
  const uint32_t serialStartAt = millis();
  while (!Serial && millis() - serialStartAt < 2500) delay(10);
  delay(250);
  appPrintln();
  appPrintln("Heltec V3 LoRa messenger booting...");
#endif
#if BOARD_POWERON >= 0
  pinMode(BOARD_POWERON, OUTPUT);
  digitalWrite(BOARD_POWERON, HIGH);
#endif

  randomSeed(esp_random());

  localNodeId = deriveNodeId(ESP.getEfuseMac());
  nextMessageId = esp_random();
  if (nextMessageId == 0) nextMessageId = 1;

#ifdef BOARD_HELTEC_V3
  appPrintln("Init display...");
#endif
  setupDisplay();
#ifdef BOARD_HELTEC_V3
  drawBootScreen("serial booting");
#endif
  drawUi();
#ifdef BOARD_HELTEC_V3
  appPrintln("Init serial input...");
#endif
  setupKeyboard();
  setupTouch();
#ifdef BOARD_HELTEC_V3
  appPrintln("Load settings...");
#endif
  loadRadioSettings();

  // Load persistent chat history
  loadChatHistory();

  // Restore any stored messages from NVS flash (survive reboot)
  loadStoredMessages();

#ifdef BOARD_HELTEC_V3
  appPrintln(ENABLE_BLE_MESH ? "Init BLE..." : "BLE disabled; LoRa-only mode.");
#endif
  setupBleLocation();
#ifdef BOARD_HELTEC_V3
  appPrintln("Init LoRa...");
#endif
  setupRadio();

#if ENABLE_FREQ_HOPPING
  retuneToFrequency(hopChannels[0]);
  hoppingBootSync();
#endif

  // Read battery once at boot for immediate display
  batteryVoltage = readBatteryVoltage();
  batteryPercent = computeBatteryPercent(batteryVoltage);
  lastBatteryReadAt = millis();

  sendHello();
#ifdef BOARD_HELTEC_V3
  addLine("serial message ready");
  appPrintln("Heltec V3 LoRa messenger ready.");
  appPrintln("Type chat text and press Enter, or use /help for commands.");
#else
  addLine("type message, press enter");
#endif
  drawUi();
}

// ============================================================================
//  loop()
// ============================================================================
void loop() {
  serviceInput();
  serviceTouch();
  serviceBlePacket();
  serviceRadio();
  serviceLoRaTransmit();
  serviceGroupAcks();
  servicePending();
  serviceRelay();
  serviceBeacon();
  serviceBleLinks();
  serviceBleLocation();
  serviceHopping();
  serviceBattery();

  // New feature service calls
  serviceFragments();
  retryStoredMessages();
  serviceContactDiscovery();

  // Save chat history periodically (every 30 seconds)
  if (millis() - lastHistorySaveAt > 30000) {
    saveChatHistory();
  }

  // Live-update the Mesh dashboard while it's visible.
  if (screenMode == ScreenMode::Mesh && millis() - lastMeshDrawAt > 500) {
    lastMeshDrawAt = millis();
    drawMeshScreen();
  }

  if (millis() - lastStatusAt > 10000 && !hasPendingMessages()) {
    lastStatusAt = millis();
    statusLine = "ready " + settingsSummary();
    drawBottom();
  }

  vTaskDelay(pdMS_TO_TICKS(10));
}
