#pragma once
// ============================================================================
// config.h — Constants, structs, enums, and build-time configuration
//
// This file defines all compile-time constants, the wire packet structure,
// metadata structs for mesh state, and build-flag overridable defaults.
// ============================================================================

#include <Arduino.h>
#ifndef BOARD_HELTEC_V3
#include <Arduino_GFX_Library.h>
#endif
#ifdef BOARD_HELTEC_V3
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#endif
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <RadioLib.h>
#include <SPI.h>
#include <stdarg.h>
#ifndef BOARD_HELTEC_V3
#include <touch/TouchDrvGT911.hpp>
#endif
#include <Wire.h>
#include <math.h>
#include <mbedtls/aes.h>
#include <mbedtls/sha256.h>

// --- Heltec V3 monochrome display color aliases ---
// The SSD1306 OLED is monochrome; all named colors collapse to WHITE.
#ifdef BOARD_HELTEC_V3
#ifndef RED
#define RED SSD1306_WHITE
#endif
#ifndef GREEN
#define GREEN SSD1306_WHITE
#endif
#ifndef BLUE
#define BLUE SSD1306_WHITE
#endif
#ifndef YELLOW
#define YELLOW SSD1306_WHITE
#endif
#ifndef CYAN
#define CYAN SSD1306_WHITE
#endif
#ifndef MAGENTA
#define MAGENTA SSD1306_WHITE
#endif
#endif

// ============================================================================
//  Board-specific pin definitions
// ============================================================================
#ifdef BOARD_HELTEC_V3
#define BOARD_POWERON -1
#define BOARD_I2C_SDA SDA_OLED
#define BOARD_I2C_SCL SCL_OLED
#define BOARD_OLED_RST RST_OLED
#define BOARD_SPI_MOSI MOSI
#define BOARD_SPI_MISO MISO
#define BOARD_SPI_SCK SCK
#define RADIO_CS_PIN SS
#define RADIO_BUSY_PIN BUSY_LoRa
#define RADIO_RST_PIN RST_LoRa
#define RADIO_DIO1_PIN DIO0
#define BATTERY_ADC_PIN 1
#else
#define BOARD_POWERON 10
#define BOARD_I2C_SDA 18
#define BOARD_I2C_SCL 8
#define BOARD_TOUCH_INT 16
#define LILYGO_KB_SLAVE_ADDRESS 0x55
#define LILYGO_KB_BRIGHTNESS_CMD 0x01
#define LILYGO_KB_ALT_B_BRIGHTNESS_CMD 0x02

#define BOARD_TFT_CS 12
#define BOARD_TFT_DC 11
#define BOARD_TFT_BACKLIGHT 42
#define BOARD_SPI_MOSI 41
#define BOARD_SPI_MISO 38
#define BOARD_SPI_SCK 40

#define RADIO_CS_PIN 9
#define RADIO_BUSY_PIN 13
#define RADIO_RST_PIN 17
#define RADIO_DIO1_PIN 45
#define BATTERY_ADC_PIN 4
#endif

// ============================================================================
//  Build-flag overridable defaults
// ============================================================================
#ifndef RADIO_FREQ_MHZ
#define RADIO_FREQ_MHZ 915.0
#endif

#ifndef CHAT_MAX_RETRIES
#define CHAT_MAX_RETRIES 8
#endif

#ifndef CHAT_ACK_TIMEOUT_MS
#define CHAT_ACK_TIMEOUT_MS 2500
#endif

#ifndef ENABLE_BLE_MESH
#define ENABLE_BLE_MESH 0
#endif

#ifndef HELTEC_ENABLE_UART0_OUTPUT
#define HELTEC_ENABLE_UART0_OUTPUT 1
#endif

#ifndef HELTEC_ENABLE_UART0_INPUT
#define HELTEC_ENABLE_UART0_INPUT 1
#endif

// ============================================================================
//  Frequency hopping — disabled by default
// ============================================================================
#ifndef ENABLE_FREQ_HOPPING
#define ENABLE_FREQ_HOPPING 0
#endif

// Hop interval is now computed dynamically from LoRa airtime (see settings.cpp).
// Default fallback for non-FHSS or initial boot.
#ifndef HOP_INTERVAL_MS
#define HOP_INTERVAL_MS 2000
#endif

// ISM band range for frequency hopping (902–928 MHz in US/AU, 863–870 in EU).
#ifndef HOP_BAND_MIN_MHZ
#define HOP_BAND_MIN_MHZ 902.0
#endif

#ifndef HOP_BAND_MAX_MHZ
#define HOP_BAND_MAX_MHZ 928.0
#endif

// Maximum time (ms) to wait on the rendezvous channel during boot sync
// before giving up and starting solo.
#ifndef HOP_RENDEZVOUS_TIMEOUT_MS
#define HOP_RENDEZVOUS_TIMEOUT_MS 180000
#endif

// ============================================================================
//  Feature flags (default: all features enabled)
// ============================================================================
#ifndef ENABLE_MESSAGE_FRAGMENTATION
#define ENABLE_MESSAGE_FRAGMENTATION 1
#endif

#ifndef ENABLE_STORE_FORWARD
#define ENABLE_STORE_FORWARD 1
#endif

#ifndef ENABLE_PERSISTENT_HISTORY
#define ENABLE_PERSISTENT_HISTORY 1
#endif

#ifndef ENABLE_CONTACTS
#define ENABLE_CONTACTS 1
#endif

#ifndef ENABLE_EMERGENCY_MSG
#define ENABLE_EMERGENCY_MSG 1
#endif

#ifndef ENABLE_GPS_TELEMETRY
#define ENABLE_GPS_TELEMETRY 0  // Disabled by default — requires I2C GPS hardware
#endif

#ifndef ENABLE_NODE_ROLES
#define ENABLE_NODE_ROLES 1
#endif

// ============================================================================
//  Protocol constants
// ============================================================================

// Packet version — increment when wire format changes.
constexpr uint8_t PACKET_VERSION = 1;

// Packet types carried over LoRa and BLE mesh.
constexpr uint8_t PACKET_TYPE_DATA = 1;
constexpr uint8_t PACKET_TYPE_ACK = 2;
constexpr uint8_t PACKET_TYPE_HELLO = 3;
constexpr uint8_t PACKET_TYPE_RREQ = 4;  // Route request (AODV)
constexpr uint8_t PACKET_TYPE_RREP = 5;  // Route reply (AODV)
constexpr uint8_t PACKET_TYPE_FRAGMENT = 6;   // Multi-packet message fragment
constexpr uint8_t PACKET_TYPE_EMERGENCY = 7;   // Emergency/priority message
constexpr uint8_t PACKET_TYPE_POSITION = 8;    // GPS/position beacon
constexpr uint8_t PACKET_TYPE_CONTACT_QUERY = 9;   // Contact directory query
constexpr uint8_t PACKET_TYPE_CONTACT_REPLY = 10;  // Contact directory reply

// Maximum body length in bytes (before hex-encoding for encrypted payloads).
constexpr uint8_t MAX_BODY_LEN = 152;

// Maximum user-facing chat text length that fits in one encrypted packet.
// Increased from 43 to 75 thanks to smaz compression (~43% gain on chat text).
constexpr uint8_t MAX_CHAT_TEXT_LEN = 75;

// Maximum user-facing long message length when fragmentation is enabled.
// Increased from 340 to 600 thanks to smaz compression across 8 fragments.
constexpr uint16_t MAX_LONG_CHAT_TEXT_LEN = 600;

// Maximum fragments per multi-packet message.
constexpr uint8_t MAX_FRAGMENTS = 8;

// Timeout for collecting fragments (ms).
constexpr uint32_t FRAGMENT_TIMEOUT_MS = 30000;

// AES-256 key and block sizes (in bytes).
constexpr uint8_t AES_KEY_LEN = 32;
constexpr uint8_t AES_BLOCK_LEN = 16;

// Default time-to-live for broadcast/mesh packets.
constexpr uint8_t MESH_DEFAULT_TTL = 3;

// ============================================================================
//  Wire Packet Format
//
//  Byte layout (MAX_PACKET_LEN = 180 bytes total):
//
//  Offset  Size  Field          Description
//  ------  ----  -----          -----------
//   0      1     magic[0]       'T' (0x54)
//   1      1     magic[1]       'D' (0x44)
//   2      1     version        PACKET_VERSION (1)
//   3      1     type           Packet type (1-10)
//   4      4     source         32-bit little-endian source node ID
//   8      4     destination    32-bit little-endian destination node ID
//  12      4     messageId      32-bit little-endian message ID
//  16      1     bodyLen        Length of body payload (0..MAX_BODY_LEN)
//  17      1     ttl            Time-to-live (decremented each hop)
//  18      4     nextHop        32-bit little-endian next-hop node ID
//  22      4     previousHop    32-bit little-endian previous-hop node ID
//  26    var     body           Payload (bodyLen bytes, zero-padded to MAX_BODY_LEN)
// 178      2     crc16          CRC-16/CCITT over bytes 0..177
// ============================================================================
constexpr size_t HEADER_LEN = 26;
constexpr size_t MAX_PACKET_LEN = HEADER_LEN + MAX_BODY_LEN + 2;  // 26 + 152 + 2 = 180

// Reserved node ID for broadcast messages.
constexpr uint32_t BROADCAST_NODE = 0;

// ============================================================================
//  Timing constants (milliseconds)
// ============================================================================

// Route entries expire after this duration without refresh.
constexpr uint32_t ROUTE_TTL_MS = 300000;  // 5 minutes

// Interval between HELLO beacon broadcasts.
constexpr uint32_t HELLO_INTERVAL_MS = 60000;  // 1 minute

// Interval between route discovery attempts when a route is unknown.
constexpr uint32_t ROUTE_DISCOVERY_INTERVAL_MS = 5000;  // 5 seconds

// Maximum time to wait for a route discovery response.
constexpr uint32_t ROUTE_DISCOVERY_TIMEOUT_MS = 30000;  // 30 seconds

// Interval between BLE scans for nearby mesh nodes.
constexpr uint32_t BLE_SCAN_INTERVAL_MS = 45000;  // 45 seconds

// Duration of each BLE scan window.
constexpr uint32_t BLE_SCAN_DURATION_MS = 500;

// BLE-discovered nodes expire after this duration without refresh.
constexpr uint32_t BLE_NODE_TTL_MS = 180000;  // 3 minutes

// BLE connection timeout per remote device.
constexpr uint32_t BLE_CONNECT_TIMEOUT_MS = 350;

// Settle delay between BLE transmissions to multiple nodes.
constexpr uint32_t BLE_TX_SETTLE_MS = 250;

// Maximum time a LoRa transmission may occupy the radio.
constexpr uint32_t LORA_TX_TIMEOUT_MS = 15000;  // 15 seconds

// LoRa CCA (Clear Channel Assessment) backoff ranges.
// Initial backoff before first CCA attempt.
constexpr uint32_t LORA_CCA_INITIAL_MIN_MS = 25;
constexpr uint32_t LORA_CCA_INITIAL_MAX_MS = 220;

// Backoff when channel is sensed busy (LoRa preamble or data detected).
constexpr uint32_t LORA_CCA_BUSY_MIN_MS = 180;
constexpr uint32_t LORA_CCA_BUSY_MAX_MS = 900;

// Group ACK delay range — receivers wait a random interval before sending ACK
// so that a single ACK can suppress others (reduces airtime).
constexpr uint32_t GROUP_ACK_DELAY_MIN_MS = 120;
constexpr uint32_t GROUP_ACK_DELAY_MAX_MS = 850;

// Heard ACK entries expire after this duration.
constexpr uint32_t GROUP_ACK_SEEN_TTL_MS = 30000;

// ============================================================================
//  Store-and-Forward timing
// ============================================================================

// Retry interval for undelivered stored messages (ms).
constexpr uint32_t STORE_FORWARD_RETRY_MS = 60000;  // 1 minute

// Maximum number of stored message retries.
constexpr uint8_t STORE_FORWARD_MAX_RETRIES = 60;   // ~1 hour at 1 min intervals

// Maximum number of messages in the flash store.
constexpr uint8_t STORE_FORWARD_MAX_MESSAGES = 20;

// ============================================================================
//  Emergency message constants
// ============================================================================

// Emergency messages bypass the TX queue and send immediately via sendHelloImmediate-style.
// Emergency retries are faster.
constexpr uint32_t EMERGENCY_ACK_TIMEOUT_MS = 1000;
constexpr uint8_t EMERGENCY_MAX_RETRIES = 20;

// ============================================================================
//  Node roles
// ============================================================================

// Node role type.
enum class NodeRole : uint8_t {
  Relay = 0,  // Full mesh participation (forward/relay HELLOs)
  Leaf  = 1,  // No forwarding, no HELLO beaconing — battery saving
};

// ============================================================================
//  Queue sizes
// ============================================================================

// Depth of all circular transmit and pending queues.
constexpr uint8_t TX_QUEUE_DEPTH = 10;

// ============================================================================
//  BLE UUIDs and advertisement constants
// ============================================================================

// BLE GATT service UUID for mesh packet exchange.
const char *const BLE_MESH_SERVICE_UUID = "7b3f0c70-3f7a-4f1d-a6aa-68fc0b4f6b01";

// BLE GATT characteristic UUID for writing mesh packets.
const char *const BLE_MESH_PACKET_UUID = "7b3f0c71-3f7a-4f1d-a6aa-68fc0b4f6b01";

// 16-bit UUID used in BLE manufacturer advertisement data to identify
// T-Deck mesh nodes during scanning.
constexpr uint16_t BLE_MESH_ADV_UUID = 0xFD6F;  // Reserved for prototyping

// ============================================================================
//  UI Colors (RGB565 for T-Deck, monochrome aliases for Heltec V3)
// ============================================================================

// Dark grey for inactive UI elements.
constexpr uint16_t COLOR_DARK_GREY = 0x7BEF;

// Orange for outgoing / pending messages.
constexpr uint16_t COLOR_ORANGE = 0xFD20;

// Selected / highlighted row fill color in settings screens.
constexpr uint16_t COLOR_SELECTED_ROW = 0x2104;  // Dark blue-grey

// Emergency message color (bright red).
constexpr uint16_t COLOR_ALERT = 0xF800;

// ============================================================================
//  Radio defaults
// ============================================================================

// SX1262 LoRa sync word (0x12 = private network, 0x34 = public).
constexpr uint8_t LORA_SYNC_WORD = 0x12;

// ============================================================================
//  Battery monitoring constants
// ============================================================================

// Interval between battery voltage readings (ms).
constexpr uint32_t BATTERY_READ_INTERVAL_MS = 30000;

// ADC reference for ESP32 (12-bit, 0-4095 maps to 0-3.3V).
constexpr float BATTERY_ADC_REF_VOLTAGE = 3.3f;
constexpr uint16_t BATTERY_ADC_MAX = 4095;

// Voltage divider ratio (resistor-divider: R1 = R2, so V_actual = ADC * 2).
constexpr float BATTERY_VOLTAGE_DIVIDER = 2.0f;

// LiPo voltage range for percentage mapping.
constexpr float BATTERY_VOLTAGE_MIN = 3.20f;  // 0% — safe cutoff
constexpr float BATTERY_VOLTAGE_MAX = 4.15f;  // 100% — fully charged

// ============================================================================
//  Display dimensions
// ============================================================================
#ifdef BOARD_HELTEC_V3
constexpr int16_t UI_WIDTH = 128;
constexpr int16_t UI_HEIGHT = 64;
#else
constexpr int16_t UI_WIDTH = 320;
constexpr int16_t UI_HEIGHT = 240;
#endif

// ============================================================================
//  GPS / Position constants
// ============================================================================
#if ENABLE_GPS_TELEMETRY
// Time between automatic position broadcasts (ms).
constexpr uint32_t POSITION_BROADCAST_INTERVAL_MS = 300000;  // 5 minutes

// Time between GPS fix attempts (ms).
constexpr uint32_t GPS_FIX_INTERVAL_MS = 10000;  // 10 seconds
#endif

// ============================================================================
//  Data structures
// ============================================================================

// Decoded representation of a wire-format mesh packet.
struct Packet {
  uint8_t type = 0;
  uint32_t source = 0;
  uint32_t destination = BROADCAST_NODE;
  uint32_t messageId = 0;
  uint32_t nextHop = BROADCAST_NODE;
  uint32_t previousHop = BROADCAST_NODE;
  uint8_t ttl = 0;
  int16_t rssi = 0;
  String body;
};

// Outgoing message queued for reliable delivery with ACK retry.
struct PendingMessage {
  bool active = false;
  uint32_t destination = BROADCAST_NODE;
  uint32_t messageId = 0;
  uint32_t conversation = BROADCAST_NODE;  // Destination for display grouping
  bool prefersBle = false;                 // True if destination is BLE-reachable
  String body;                              // Pre-encrypted payload (hex string)
  uint8_t attempts = 0;                    // Number of transmit attempts so far
  uint32_t lastSentAt = 0;                 // Timestamp of last transmission
  bool isEmergency = false;                // True for emergency priority messages
  uint8_t fragmentCount = 0;              // >0 if this is a multi-fragment message
  uint8_t fragmentIndex = 0;              // Index of this fragment (0..fragmentCount-1)
  uint8_t storedSlotIndex = 0xFF;          // 0xFF = not from store, else index into storedMessages[]
};

// Single entry in a chat display buffer.
struct ChatEntry {
  String text;
  uint16_t color = WHITE;
  uint32_t messageId = 0;
  bool outgoing = false;
  bool acked = false;
  bool direct = false;
  bool isEmergency = false;  // Show with red styling
};

// Circular buffer of the 8 most recent chat entries for one conversation.
// When full, the oldest entry is evicted.
struct ChatBuffer {
  ChatEntry entries[8];
  uint8_t count = 0;
};

// Per-destination direct chat conversation state.
struct DirectChat {
  bool active = false;
  uint32_t nodeId = 0;
  String name;
  ChatBuffer buffer;
};

// Deduplication entry — source+messageId pairs that have been seen.
// Stored in a fixed-size circular buffer (32 slots).
struct SeenMessage {
  uint32_t source = 0;
  uint32_t messageId = 0;
};

// Queued relay for broadcast/flood forwarding.
// After a random delay, the packet is retransmitted with decremented TTL.
struct RelayMessage {
  bool active = false;
  uint8_t type = PACKET_TYPE_DATA;
  uint32_t source = 0;
  uint32_t destination = BROADCAST_NODE;
  uint32_t messageId = 0;
  uint8_t ttl = 0;
  uint32_t dueAt = 0;  // Timestamp when relay is eligible
  String body;
  bool isEmergency = false;  // Emergency messages relay with shorter delay
};

// Scheduled group ACK — receivers delay ACK randomly to reduce airtime.
struct GroupAckJob {
  bool active = false;
  uint32_t dataSource = 0;
  uint32_t messageId = 0;
  uint32_t dueAt = 0;
};

// Record of a recently heard group ACK (used for suppression).
struct HeardAck {
  bool active = false;
  uint32_t dataSource = 0;
  uint32_t messageId = 0;
  uint32_t lastSeenAt = 0;
};

// Mesh route table entry (AODV-style).
struct RouteEntry {
  bool active = false;
  uint32_t destination = 0;
  uint32_t nextHop = 0;
  uint8_t hops = 0;
  uint32_t lastSeenAt = 0;
};

// BLE-discovered mesh node information.
struct BleNode {
  bool active = false;
  uint32_t nodeId = 0;
  int rssi = 0;
  uint8_t addressType = 0;
  uint32_t lastSeenAt = 0;
  String name;
  String address;  // BLE MAC address string
};

// Fragment reassembly state for multi-packet messages.
struct FragmentAssembly {
  bool active = false;
  uint32_t source = 0;
  uint32_t messageId = 0;
  uint8_t total = 0;
  uint8_t received = 0;  // bitmask of received fragments (bit 0 = fragment 0)
  String parts[MAX_FRAGMENTS];
  uint32_t startedAt = 0;
};

// Store-and-forward: a message stored in flash for later delivery.
struct StoredMessage {
  bool active = false;
  uint32_t destination = 0;
  uint32_t messageId = 0;
  uint32_t conversation = 0;
  String body;        // Pre-encrypted payload
  String rawText;     // Original plaintext for display
  bool isDirect = false;
  String destName;    // Human-readable destination name
  uint8_t attempts = 0;
  uint32_t nextRetryAt = 0;
};

// Contacts: persistent mesh contact book.
struct ContactEntry {
  bool active = false;
  uint32_t nodeId = 0;
  String name;
  uint32_t lastSeenAt = 0;
  int16_t lastRssi = 0;
  bool online = false;
};

// GPS position data.
struct PositionData {
  bool valid = false;
  float latitude = 0.0f;
  float longitude = 0.0f;
  float altitude = 0.0f;
  uint32_t updatedAt = 0;
};

// Counters for all packet statistics (displayed on mesh dashboard).
struct PacketStats {
  uint32_t rx = 0;
  uint32_t tx = 0;
  uint32_t forwarded = 0;
  uint32_t dataRx = 0;
  uint32_t dataTx = 0;
  uint32_t ackRx = 0;
  uint32_t ackTx = 0;
  uint32_t helloRx = 0;
  uint32_t helloTx = 0;
  uint32_t rreqRx = 0;
  uint32_t rreqTx = 0;
  uint32_t rrepRx = 0;
  uint32_t rrepTx = 0;
  uint32_t decryptFail = 0;
  uint32_t routeUpdates = 0;
  uint32_t bleScans = 0;
  uint32_t bleTx = 0;
  uint32_t bleRx = 0;
  uint32_t bleSeen = 0;
  uint32_t bleConnectFail = 0;
  uint32_t bleWriteFail = 0;
  uint32_t fragRx = 0;
  uint32_t fragAssembled = 0;
  uint32_t fragTimeout = 0;
  uint32_t emergencyRx = 0;
  uint32_t emergencyTx = 0;
  uint32_t positionRx = 0;
  uint32_t positionTx = 0;
  uint32_t storedSent = 0;
};

// Queued LoRa transmit job in the circular TX queue.
struct LoRaTxJob {
  bool active = false;
  uint8_t type = 0;
  size_t len = 0;
  uint32_t startedAt = 0;
  uint8_t encoded[MAX_PACKET_LEN];
};

// Queued BLE transmit job in the circular BLE queue.
struct BleTxJob {
  bool active = false;
  uint8_t type = 0;
  size_t len = 0;
  uint32_t target = BROADCAST_NODE;
  uint8_t encoded[MAX_PACKET_LEN];
};

// Current (committed) radio settings.
struct RadioSettings {
  float frequency = RADIO_FREQ_MHZ;
  float bandwidth = 125.0;
  uint8_t spreadingFactor = 10;
  uint8_t codingRate = 6;
  int8_t txPower = 22;
};

// ============================================================================
//  UI Enums
// ============================================================================

// Top-level screen the display is currently showing.
enum class ScreenMode {
  Chat,
  Mesh,
  Settings,
  FrequencyEntry,
};

// Active tab on the settings screen.
enum class SettingsTab {
  LoRa,
  Security,
};

// Active tab on the chat screen.
enum class ChatTab {
  Group,
  Direct,
};
