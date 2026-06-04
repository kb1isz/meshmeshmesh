// ============================================================================
// settings.cpp — Radio parameter management, settings UI, and touch handling
// ============================================================================
#include "globals.h"

const float BANDWIDTH_OPTIONS[] = {7.8, 10.4, 15.6, 20.8, 31.25, 41.7, 62.5, 125.0, 250.0, 500.0};
const uint8_t BANDWIDTH_OPTION_COUNT = sizeof(BANDWIDTH_OPTIONS) / sizeof(BANDWIDTH_OPTIONS[0]);

float normalizedBandwidth(float requested) {
  float best = BANDWIDTH_OPTIONS[0];
  float bestDelta = fabs(requested - best);
  for (uint8_t i = 0; i < BANDWIDTH_OPTION_COUNT; ++i) {
    const float delta = fabs(requested - BANDWIDTH_OPTIONS[i]);
    if (delta < bestDelta) { best = BANDWIDTH_OPTIONS[i]; bestDelta = delta; }
  }
  return best;
}

uint8_t bandwidthIndex(float bandwidth) {
  uint8_t best = 0;
  float bestDelta = fabs(bandwidth - BANDWIDTH_OPTIONS[0]);
  for (uint8_t i = 1; i < BANDWIDTH_OPTION_COUNT; ++i) {
    const float delta = fabs(bandwidth - BANDWIDTH_OPTIONS[i]);
    if (delta < bestDelta) { best = i; bestDelta = delta; }
  }
  return best;
}

bool validRadioSettings(const RadioSettings &settings) {
  return settings.frequency >= 150.0 && settings.frequency <= 960.0 &&
         (settings.bandwidth == 7.8 || settings.bandwidth == 10.4 || settings.bandwidth == 15.6 ||
          settings.bandwidth == 20.8 || settings.bandwidth == 31.25 || settings.bandwidth == 41.7 ||
          settings.bandwidth == 62.5 || settings.bandwidth == 125.0 || settings.bandwidth == 250.0 ||
          settings.bandwidth == 500.0) &&
         settings.spreadingFactor >= 5 && settings.spreadingFactor <= 12 &&
         settings.codingRate >= 5 && settings.codingRate <= 8 &&
         settings.txPower >= -9 && settings.txPower <= 22;
}

void loadRadioSettings() {
  if (preferences.begin("lora", false)) {
    radioSettings.frequency = preferences.isKey("freq") ? preferences.getFloat("freq", RADIO_FREQ_MHZ) : RADIO_FREQ_MHZ;
    radioSettings.bandwidth = preferences.isKey("bw") ? preferences.getFloat("bw", 125.0) : 125.0;
    radioSettings.spreadingFactor = preferences.isKey("sf") ? preferences.getUChar("sf", 10) : 10;
    radioSettings.codingRate = preferences.isKey("cr") ? preferences.getUChar("cr", 6) : 6;
    radioSettings.txPower = preferences.isKey("pwr") ? preferences.getChar("pwr", 22) : 22;
#if ENABLE_NODE_ROLES
    nodeRole = preferences.getUChar("role", static_cast<uint8_t>(NodeRole::Relay)) == static_cast<uint8_t>(NodeRole::Leaf)
                 ? NodeRole::Leaf : NodeRole::Relay;
#endif
    preferences.end();
  } else { radioSettings = RadioSettings{}; }
  if (!validRadioSettings(radioSettings)) radioSettings = RadioSettings{};

  if (preferences.begin("identity", false)) {
    deviceName = preferences.isKey("name") ? preferences.getString("name", defaultDeviceName()) : defaultDeviceName();
    encryptionKey = preferences.isKey("key") ? preferences.getString("key", "change-me") : "change-me";
    preferences.end();
  } else {
    deviceName = defaultDeviceName();
    encryptionKey = "change-me";
  }
  deviceName.trim(); encryptionKey.trim();
  if (deviceName.length() == 0) deviceName = defaultDeviceName();
  if (encryptionKey.length() == 0) encryptionKey = "change-me";
}

void saveRadioSettings() {
  preferences.begin("lora", false);
  preferences.putFloat("freq", radioSettings.frequency);
  preferences.putFloat("bw", radioSettings.bandwidth);
  preferences.putUChar("sf", radioSettings.spreadingFactor);
  preferences.putUChar("cr", radioSettings.codingRate);
  preferences.putChar("pwr", radioSettings.txPower);
#if ENABLE_NODE_ROLES
  preferences.putUChar("role", static_cast<uint8_t>(nodeRole));
#endif
  preferences.end();
  preferences.begin("identity", false);
  preferences.putString("name", deviceName);
  preferences.putString("key", encryptionKey);
  preferences.end();
}

// Forward declaration for computeMaxPacketAirtimeMs() which is defined later in this file.
static uint32_t computeMaxPacketAirtimeMs();

bool applyRadioSettings(bool haltOnFailure) {
  radioPacketReceived = false;
  const int state = radio.begin(radioSettings.frequency, radioSettings.bandwidth,
                                radioSettings.spreadingFactor, radioSettings.codingRate,
                                LORA_SYNC_WORD, radioSettings.txPower);
  if (state != RADIOLIB_ERR_NONE) {
    radioStarted = false; radioTransmitting = false; loraTx.active = false;
    loraQueueHead = loraQueueTail = loraQueueCount = 0;
    statusLine = "radio init failed: " + String(state); addLine(statusLine); drawUi();
    return false;
  }
  radioStarted = true; radioTransmitting = false; loraTx.active = false;
  loraQueueHead = loraQueueTail = loraQueueCount = 0;
  // Hardware CRC disabled — our protocol embeds a CRC-16/CCITT in the last 2
  // bytes of every packet (encodePacket/decodePacket). Enabling SX1262 hardware
  // CRC causes readData() to return ERR_WRONG_MODEM (-7) on mismatch, which
  // prevents our software decode from ever seeing the bytes.
  radio.setCRC(false); radio.setDio1Action(onRadioPacketReceived);
  radio.startReceive(); statusLine = "ready " + settingsSummary();

  // Compute dynamic hop interval based on max packet airtime.
  // One TX per slot: 2× airtime for TX + guard time. Clamped to minimum 2000ms.
  const uint32_t airtime = computeMaxPacketAirtimeMs();
  hopIntervalMs = max(static_cast<uint32_t>(2000), airtime * 2);
  hopLastTxSlot = 0xFF;
  hopLastRxSlot = 0xFF;
  appPrintf("[hop] airtime=%u ms interval=%u ms\n", airtime, hopIntervalMs);
#if ENABLE_FREQ_HOPPING
  buildHopChannels();
#endif

  return true;
}

void setupRadio() { SPI.begin(BOARD_SPI_SCK, BOARD_SPI_MISO, BOARD_SPI_MOSI); applyRadioSettings(true); }

void openSettingsScreen() {
  draftSettings = radioSettings;
  draftFrequencyText = String(radioSettings.frequency, 1);
  draftDeviceName = deviceName; draftEncryptionKey = encryptionKey;
  settingsTab = SettingsTab::LoRa; selectedSettingsRow = 0;
  screenMode = ScreenMode::Settings; statusLine = "edit settings";
  drawSettingsScreen();
}

void closeSettingsScreen() { screenMode = ScreenMode::Chat; statusLine = "ready " + settingsSummary(); clearScreen(); drawUi(); }
void closeMeshScreen() { screenMode = ScreenMode::Chat; clearScreen(); drawUi(); }

void cycleDraftOption(uint8_t row) {
  switch (row) {
  case 1: draftSettings.bandwidth = BANDWIDTH_OPTIONS[(bandwidthIndex(draftSettings.bandwidth) + 1) % BANDWIDTH_OPTION_COUNT]; break;
  case 2: draftSettings.spreadingFactor = draftSettings.spreadingFactor >= 12 ? 5 : draftSettings.spreadingFactor + 1; break;
  case 3: draftSettings.codingRate = draftSettings.codingRate >= 8 ? 5 : draftSettings.codingRate + 1; break;
  case 4: draftSettings.txPower = draftSettings.txPower >= 22 ? -9 : draftSettings.txPower + 1; break;
  default: break;
  }
}

void saveDraftSettings() {
  if (hasPendingMessages()) { statusLine = "finish pending send first"; drawSettingsScreen(); return; }
  draftSettings.frequency = draftFrequencyText.toFloat();
  draftSettings.bandwidth = normalizedBandwidth(draftSettings.bandwidth);
  draftDeviceName.trim(); draftEncryptionKey.trim();
  if (!validRadioSettings(draftSettings)) { statusLine = "invalid settings"; drawSettingsScreen(); return; }
  if (draftDeviceName.length() == 0 || draftEncryptionKey.length() == 0) {
    statusLine = "name/key required"; settingsTab = SettingsTab::Security; drawSettingsScreen(); return;
  }
  const RadioSettings previous = radioSettings;
  const String previousDeviceName = deviceName;
  const String previousEncryptionKey = encryptionKey;
  radioSettings = draftSettings;
  deviceName = draftDeviceName.substring(0, 16);
  encryptionKey = draftEncryptionKey.substring(0, 32);
  if (!applyRadioSettings(false)) {
    radioSettings = previous;
    deviceName = previousDeviceName;
    encryptionKey = previousEncryptionKey;
    applyRadioSettings(false);
    statusLine = "radio rejected settings"; drawSettingsScreen(); return;
  }
  saveRadioSettings(); configureBleAdvertisement();
  addLine("saved: " + settingsSummary()); closeSettingsScreen();
}

bool applyCommandSettings(const RadioSettings &updated, const String &label) {
  if (hasPendingMessages()) { statusLine = "finish pending send first"; drawBottom(); drawSettingsScreen(); return true; }
  if (!validRadioSettings(updated)) { statusLine = "invalid setting"; drawBottom(); drawSettingsScreen(); return true; }
  const RadioSettings previous = radioSettings;
  radioSettings = updated;
  if (!applyRadioSettings(false)) { radioSettings = previous; applyRadioSettings(false); drawUi(); return true; }
  saveRadioSettings(); addLine(label + ": " + settingsSummary()); drawUi();
  return true;
}

void openFrequencyEntryScreen() { keypadFrequencyText = draftFrequencyText; screenMode = ScreenMode::FrequencyEntry; drawFrequencyEntryScreen(); }
void acceptFrequencyEntry() { if (keypadFrequencyText.length() > 0) draftFrequencyText = keypadFrequencyText; screenMode = ScreenMode::Settings; selectedSettingsRow = 0; drawSettingsScreen(); }
void cancelFrequencyEntry() { screenMode = ScreenMode::Settings; selectedSettingsRow = 0; drawSettingsScreen(); }

void serviceTouch() {
#ifdef BOARD_HELTEC_V3
  return;
#else
  if (!touchReady) return;
  const TouchPoints &points = touch.getTouchPoints();
  if (!points.hasPoints()) { touchWasPressed = false; return; }
  if (touchWasPressed || millis() - lastTouchAt < 180) return;
  const TouchPoint &point = points.getPoint(0);
  touchWasPressed = true; lastTouchAt = millis();
  handleTouchPoint(point.x, point.y);
#endif
}

void handleTouchPoint(int16_t x, int16_t y) {
  if (screenMode == ScreenMode::Chat) {
    if (y <= 22) { if (x >= 248 && x <= 276) { screenMode = ScreenMode::Mesh; drawMeshScreen(); return; } if (x >= 282 && x <= 314) openSettingsScreen(); return; }
    if (y >= 24 && y <= 44 && x <= 240) {
      if (x < 94) selectGroupChat();
      else if (x < 208) { if (selectedDirectNode == 0) selectDirectChat(firstDirectNode()); else selectDirectChat(selectedDirectNode); }
      else selectNextDirectChat();
      drawUi(); return;
    }
    return;
  }
  if (screenMode == ScreenMode::Mesh) { if (y <= 22 && x >= 270) closeMeshScreen(); return; }
  if (screenMode == ScreenMode::FrequencyEntry) {
    if (y >= 198 && y <= 224) { if (x < 160) cancelFrequencyEntry(); else acceptFrequencyEntry(); return; }
    if (y >= 66 && y < 194) {
      const int16_t col = constrain((x - 14) / 98, 0, 2), row = constrain((y - 66) / 32, 0, 3);
      const char keys[] = {'1','2','3','4','5','6','7','8','9','.','0','\b'};
      const char key = keys[row * 3 + col];
      if (key == '\b') { if (keypadFrequencyText.length() > 0) keypadFrequencyText.remove(keypadFrequencyText.length() - 1); }
      else if ((key >= '0' && key <= '9') || key == '.') { if (keypadFrequencyText.length() < 7) keypadFrequencyText += key; }
      drawFrequencyEntryScreen();
    }
    return;
  }
  if (y >= 26 && y <= 50) { settingsTab = x < 160 ? SettingsTab::LoRa : SettingsTab::Security; selectedSettingsRow = 0; drawSettingsScreen(); return; }
  if (y >= 200 && y <= 228) { if (x < 160) closeSettingsScreen(); else saveDraftSettings(); return; }
  if (settingsTab == SettingsTab::LoRa) {
    const int16_t rowY[] = {64, 92, 120, 148, 176};
    for (uint8_t row = 0; row < 5; ++row) {
      if (y >= rowY[row] && y < rowY[row] + 24) {
        selectedSettingsRow = row;
        if (row == 0) openFrequencyEntryScreen(); else { cycleDraftOption(row); drawSettingsScreen(); }
        return;
      }
    }
  } else {
    if (y >= 76 && y < 100) { selectedSettingsRow = 0; drawSettingsScreen(); return; }
    if (y >= 108 && y < 132) { selectedSettingsRow = 1; drawSettingsScreen(); return; }
  }
}

// ============================================================================
//  Frequency hopping — Epoch-based time-slotted FHSS
//
//  All devices compute hopSlot from a shared time formula:
//    networkTime = millis() + hopEpochOffset
//    hopSlot = (networkTime / HOP_INTERVAL_MS) % HOP_COUNT
//
//  hopEpochOffset aligns each device's local millis() to the network epoch.
//  It is computed from any received HELLO and corrected on-the-fly, so there
//  is no free-running clock drift.
// ============================================================================

// Compute the LoRa packet airtime for the maximum payload size (MAX_PACKET_LEN bytes)
// using the standard Semtech LoRa airtime formula.
// Returns the time in ms for a single 180-byte packet at the current settings.
static uint32_t computeMaxPacketAirtimeMs() {
  // SF = spreading factor (7-12), BW = bandwidth in Hz, CR = coding rate (5=4/5, 6=4/6, 7=4/7, 8=4/8)
  const uint32_t bwHz = static_cast<uint32_t>(radioSettings.bandwidth * 1000.0f);
  const uint8_t sf = radioSettings.spreadingFactor;
  const uint8_t crDenom = radioSettings.codingRate; // denominator of code rate (5-8)

  // Symbol period: Tsym = 2^SF / BW (in seconds)
  const double bwExp = static_cast<double>(bwHz);
  const double tsym = static_cast<double>(1UL << sf) / bwExp; // seconds

  // Preamble duration: (preamble_length + 4.25) * Tsym
  // SX1262 default preamble = 8 symbols
  const double tPreamble = (8.0 + 4.25) * tsym;

  // Payload symbols formula (Semtech SX1262 datasheet):
  // payloadSymbNb = 8 + max(ceil((8 * PL - 4 * SF + 28 + 16*CRC - 20*H) / (4 * (SF - 2*DE))) * (CR + 4), 0)
  // PL = payload bytes, SF = spreading factor, CRC = 0 (disabled), H = 0 (explicit header), DE = low data rate optimization
  const double pl = static_cast<double>(MAX_PACKET_LEN);
  const double de = (bwHz <= 125000 && sf >= 11) ? 1.0 : 0.0;
  const double cr = static_cast<double>(crDenom);
  const double h = 0.0; // explicit header
  const double crc = 0.0; // hardware CRC disabled

  double payloadSymbNb = 8.0 + ceil((8.0 * pl - 4.0 * sf + 28.0 + 16.0 * crc - 20.0 * h)
                            / (4.0 * (sf - 2.0 * de)))
                         * (cr + 4.0);
  if (payloadSymbNb < 0) payloadSymbNb = 0;

  // Total packet time: Tpacket = tPreamble + payloadSymbNb * Tsym
  const double tPacket = (tPreamble + payloadSymbNb * tsym) * 1000.0; // ms

  return static_cast<uint32_t>(ceil(tPacket));
}

void buildHopChannels() {
  // Compute number of channels that fit in the ISM band at the current bandwidth.
  const float bwMHz = radioSettings.bandwidth / 1000.0f;
  const uint8_t bwKhz = static_cast<uint8_t>(radioSettings.bandwidth);
  const uint8_t channels = static_cast<uint8_t>((HOP_BAND_MAX_MHZ - HOP_BAND_MIN_MHZ) / bwMHz);
  if (channels < 2) {
    appPrintf("[hop] too few channels (%u) for bw=%.0f kHz\n", channels, radioSettings.bandwidth);
    return;
  }

  // Allocate/free the hop channel array.
  if (hopChannels != nullptr) free(hopChannels);
  hopChannels = static_cast<float *>(malloc(channels * sizeof(float)));
  if (hopChannels == nullptr) {
    hopCount = 0;
    appPrintln("[hop] channel allocation failed");
    return;
  }
  hopCount = channels;

  // Generate evenly spaced channels across the full ISM band.
  for (uint8_t i = 0; i < hopCount; ++i) {
    hopChannels[i] = HOP_BAND_MIN_MHZ + (i + 0.5f) * bwMHz;
  }

  // Shuffle using encryption key as seed (same algorithm as before).
  uint8_t keyHash[AES_KEY_LEN]; deriveEncryptionKey(keyHash);
  uint32_t seed = (static_cast<uint32_t>(keyHash[0]) << 24) | (static_cast<uint32_t>(keyHash[1]) << 16)
                | (static_cast<uint32_t>(keyHash[2]) << 8) | static_cast<uint32_t>(keyHash[3]);
  for (uint8_t i = hopCount - 1; i > 0; --i) {
    seed = seed * 1103515245 + 12345;
    const uint8_t j = static_cast<uint8_t>(seed % (i + 1));
    const float tmp = hopChannels[i]; hopChannels[i] = hopChannels[j]; hopChannels[j] = tmp;
  }

  // Diagnostic: print the hop channel list so it can be compared between devices.
  appPrintf("[hop] %u channels bw=%u kHz:", hopCount, bwKhz);
  for (uint8_t i = 0; i < hopCount; ++i) appPrintf(" %.1f", hopChannels[i]);
  appPrintln();
}

// Compute the synchronized network time (local millis() corrected by epoch offset).
int32_t hopNetworkTime() {
  return static_cast<int32_t>(millis()) + hopEpochOffset;
}

// Compute the current hop slot for a given network time.
// Works correctly for negative networkTime via unsigned wrapping:
// casting int32_t to uint32_t and then modulo gives the correct result.
uint8_t hopNetworkSlot(int32_t networkTime) {
  if (hopCount == 0 || hopIntervalMs == 0) return 0;
  // Cast to uint32_t wraps negative values correctly for modulo arithmetic.
  return static_cast<uint8_t>((static_cast<uint32_t>(networkTime) / hopIntervalMs) % hopCount);
}

// Milliseconds remaining in the current hop slot.
uint32_t hopSlotRemainingMs(int32_t networkTime) {
  if (hopIntervalMs == 0) return 0;
  const uint32_t pos = networkTime >= 0 ? static_cast<uint32_t>(networkTime) : 0;
  const uint32_t elapsed = pos % hopIntervalMs;
  return hopIntervalMs - elapsed;
}

// Track time of last sync for diagnostics on the mesh dashboard.
extern uint32_t lastHopSyncAt;

// Apply a sync signal from a remote HELLO.
// remoteSlot        = the sender's hopSlot
// remoteNetworkTime = the sender's hopNetworkTime() at transmission time
//
// The sender computes its network time as (senderMillis + senderEpochOffset).
// The sender's slot is: (networkTime / HOP_INTERVAL_MS) % HOP_COUNT.
//
// For us to be on the same slot at the same wall-clock moment, we need:
//   (now + hopEpochOffset) / HOP_INTERVAL_MS % HOP_COUNT == remoteSlot
//
// At the moment the sender transmitted, senderNetworkTime was remoteNetworkTime
// and the sender was on remoteSlot. We received it at our local millis() + ε.
// Setting hopEpochOffset = remoteNetworkTime - localMillis makes our network
// time equal the sender's network time (ignoring propagation delay ε).
//
// The offset is set directly (no smoothing). Within ±HOP_INTERVAL_MS of any
// valid offset, the modulo preserves the correct slot — integer rounding buys
// nothing and only causes drift.
void applyHopSync(uint8_t remoteSlot, int32_t remoteNetworkTime) {
  if (hopChannels == nullptr || hopCount == 0) return;
  const int32_t oldOffset = hopEpochOffset;
  hopEpochOffset = remoteNetworkTime - static_cast<int32_t>(millis());
  // Snap hopSlot to current network time, but do NOT retune or set
  // hoppingSynced here. The caller decides when to mark us as synced.
  // Parsing a HELLO during boot sweep should update the offset but not
  // stop the sweep — another HELLO or the sweep conclusion will lock it in.
  const int32_t nt = hopNetworkTime();
  hopSlot = hopNetworkSlot(nt);
  // Diagnostic: show the sync event.
  appPrintf("[hop] sync slot=%u remoteNT=%ld localMs=%lu epo=%ld (was %ld) nt=%ld -> slot=%u freq=%.1f\n",
    static_cast<unsigned int>(remoteSlot),
    static_cast<long>(remoteNetworkTime),
    static_cast<unsigned long>(millis()),
    static_cast<long>(hopEpochOffset),
    static_cast<long>(oldOffset),
    static_cast<long>(nt),
    static_cast<unsigned int>(hopSlot),
    hopChannels[hopSlot]);
}

// Tune radio to a specific frequency. Returns false if the radio is busy
// (transmitting or actively receiving), or if the frequency change fails.
bool retuneToFrequency(float freq) {
  if (!radioStarted || radioTransmitting) return false;
  // Don't retune if a packet is currently arriving (ISR fired but not yet
  // processed). The pending hop will be caught on the next serviceHopping() call.
  if (radioPacketReceived) return false;
  const int state = radio.setFrequency(freq);
  if (state != RADIOLIB_ERR_NONE) return false;
  radio.startReceive();
  return true;
}

// Boot-time sync: rendezvous on a single channel until an authoritative network
// sync is heard. If none is heard before HOP_RENDEZVOUS_TIMEOUT_MS, this node
// starts a new cold-boot epoch and broadcasts it on channel 0.
//
// Strategy:
//  - Tune to hopChannels[0] (the well-known rendezvous frequency).
//  - First transmit a non-authoritative discovery HELLO immediately, then listen
//    for a full hop interval. This guarantees the booter catches any reply from
//    a synced device even if the synced device responds after stagger + airtime.
//  - Repeat: TX, listen-hopIntervalMs, TX, listen. A synced device visiting
//    channel 0 will send at most one HELLO per slot, and the booter's long
//    listen window ensures it doesn't miss the reply.
//  - As soon as an authoritative SYNC HELLO arrives, parseHelloSlot() computes
//    hopEpochOffset and sets hoppingSynced = true.
//  - On timeout, cold-start the epoch with current slot pinned to channel 0 and
//    immediately broadcast an authoritative SYNC HELLO for other waiting nodes.
void hoppingBootSync() {
  if (!radioStarted || hopChannels == nullptr || hopCount == 0) return;
  appPrintln("Hopping: rendezvous on ch0...");
  const float rendezvousFreq = hopChannels[0];
  if (!retuneToFrequency(rendezvousFreq)) return;
  radio.startReceive();

  const uint32_t scanStart = millis();

  while (!hoppingSynced) {
    if (millis() - scanStart >= HOP_RENDEZVOUS_TIMEOUT_MS) {
      hopSlot = 0;
      hopEpochOffset = -static_cast<int32_t>(millis());
      hoppingSynced = true;
      lastHopSyncAt = millis();
      retuneToFrequency(hopChannels[0]);
      statusLine = "FHSS cold start";
      drawBottom();
      appPrintf("[hop] cold-start epoch after %lu ms slot=%u epo=%ld\n",
                static_cast<unsigned long>(millis() - scanStart),
                static_cast<unsigned int>(hopSlot),
                static_cast<long>(hopEpochOffset));
      sendHelloImmediate(false, true);
      break;
    }

    // DEBUG: log TX cycle
    {
      const uint32_t elapsed = millis() - scanStart;
      appPrintf("[dbg] BOOT TX cycle %lu s onto ch0 (txLast=%u rxLast=%u slot=%u hopInt=%lu)\n",
                static_cast<unsigned long>(elapsed / 1000),
                hopLastTxSlot, hopLastRxSlot, hopSlot,
                static_cast<unsigned long>(hopIntervalMs));
    }

    // Transmit discovery HELLO first.
    statusLine = "FHSS ch0 TX " + String((millis() - scanStart) / 1000) + "s";
    drawBottom();
    sendHelloImmediate(true, false);

    // Then listen for a full hop interval. The reply from a synced device
    // will arrive within the airtime (~350ms @ SF10/125kHz) plus 0-50ms
    // backoff, so a full hop interval gives huge margin.
    if (hoppingSynced) break;
    statusLine = "FHSS ch0 listen " + String((millis() - scanStart) / 1000) + "s";
    drawBottom();
    const uint32_t listenDeadline = millis() + hopIntervalMs;
    while (millis() < listenDeadline && !hoppingSynced) {
      serviceRadio();
      delay(5);
    }
    // DEBUG: log listen end
    if (!hoppingSynced) {
      appPrintf("[dbg] BOOT listen ended, no sync yet (%lu s elapsed)\n",
                static_cast<unsigned long>((millis() - scanStart) / 1000));
    }
  }

  appPrintf("[hop] synced slot=%u nt=%ld epo=%ld after %lu ms\n",
            static_cast<unsigned int>(hopSlot),
            static_cast<long>(hopNetworkTime()),
            static_cast<long>(hopEpochOffset),
            millis() - scanStart);
  lastHopSyncAt = millis();
  statusLine = "FHSS synced slot " + String(hopSlot + 1);
  drawBottom();
}

// Called every loop iteration. Computes the current hop slot from the
// synchronized network time and retunes if the slot changed.
//
// Key difference from the old design: slot is computed mathematically from
// (millis() + hopEpochOffset), so even if a hop was deferred because the radio
// was busy, the next call will compute the correct current slot — no
// accumulated error.
//
// Proactive HELLO: on the device's dedicated rendezvous slot (localNodeId % hopCount),
// quickly tune to ch0, send a HELLO, and return to the slot channel. This ensures
// booting devices hear at least one HELLO per full cycle while spending minimal
// time away from the active slot channel.
void serviceHopping() {
  if (!ENABLE_FREQ_HOPPING) return;
  if (!hoppingSynced || !radioStarted) return;
  if (hopChannels == nullptr || hopCount == 0) return;

  // Don't hop while transmitting — the slot calculation will catch us up.
  if (radioTransmitting) return;

  const int32_t nt = hopNetworkTime();
  const uint8_t newSlot = hopNetworkSlot(nt);
  if (newSlot == hopSlot) return;

  hopSlot = newSlot;

  // Proactive HELLO on the rendezvous channel for this device's dedicated slot.
  if (hopSlot == static_cast<uint8_t>(localNodeId % hopCount)) {
    retuneToFrequency(hopChannels[0]);
    sendHelloImmediate(false, true);  // trackSlot=false — don't block data/ACK in this slot
    retuneToFrequency(hopChannels[hopSlot]);
  } else {
    retuneToFrequency(hopChannels[hopSlot]);
  }

  // Diagnostic: print every normal hop.
  appPrintf("[hop] HOP slot=%u freq=%.1f ms=%lu nt=%ld epo=%ld\n",
    static_cast<unsigned int>(hopSlot), hopChannels[hopSlot],
    static_cast<unsigned long>(millis()),
    static_cast<long>(nt),
    static_cast<long>(hopEpochOffset));
}
