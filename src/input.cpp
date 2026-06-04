// ============================================================================
// input.cpp — Keyboard, touch, serial input handling, and display setup
//
// Handles all user input channels:
//  - T-Deck physical I2C keyboard (or Heltec V3 USB/Serial0)
//  - Touch screen on T-Deck (GT911 controller)
//  - Slash-command parsing and execution
//  - Settings screen character entry (frequency, name, key)
//
// Also includes hardware setup functions for display, keyboard, and touch.
// ============================================================================

#include "globals.h"

// Read a single key from the T-Deck I2C keyboard.
// Returns 0 if no key is available. On Heltec V3, always returns 0.
char readKeyboard() {
#ifdef BOARD_HELTEC_V3
  return 0;
#else
  Wire.requestFrom(LILYGO_KB_SLAVE_ADDRESS, 1);
  if (!Wire.available()) return 0;
  const char key = static_cast<char>(Wire.read());
  if (key != 0) appPrintf("keyboard: 0x%02X '%c'\n", static_cast<uint8_t>(key), isPrintable(key) ? key : '.');
  return key;
#endif
}

// Set the T-Deck keyboard backlight brightness (0-255).
void setKeyboardBrightness(uint8_t value) {
#ifdef BOARD_HELTEC_V3
  (void)value;
#else
  Wire.beginTransmission(LILYGO_KB_SLAVE_ADDRESS);
  Wire.write(LILYGO_KB_BRIGHTNESS_CMD);
  Wire.write(value);
  Wire.endTransmission();
#endif
}

// Set the T-Deck keyboard default (alt-B) backlight brightness.
void setKeyboardDefaultBrightness(uint8_t value) {
#ifdef BOARD_HELTEC_V3
  (void)value;
#else
  Wire.beginTransmission(LILYGO_KB_SLAVE_ADDRESS);
  Wire.write(LILYGO_KB_ALT_B_BRIGHTNESS_CMD);
  Wire.write(value);
  Wire.endTransmission();
#endif
}

// --- Settings input char routing ---

// Handle keyboard input while the settings screen is active.
// Routes characters based on the active settings tab (LoRa or Security).
//
// LoRa tab:
//   - Row 0 (Frequency): pressing '0'-'9' or '.' opens the frequency entry
//     keypad. Digits '1'-'4' ALSO cycle rows 1-4 when NOT editing row 0.
//     To enter a frequency like "915.0", open the frequency entry screen
//     first by tapping the frequency row (touch), then type on the keypad.
//   - Rows 1-4: digits '1'-'4' cycle their respective dropdown values.
//
// Security tab:
//   - Row 0 (Name): up to 16 printable characters.
//   - Row 1 (Key): up to 32 printable characters.
void handleSettingsInputChar(char key) {
  if (key == 0) return;
  if (key == '\r' || key == '\n') { saveDraftSettings(); return; }
  if (key == 0x0C) {
    if (settingsTab == SettingsTab::LoRa) { draftFrequencyText = ""; selectedSettingsRow = 0; }
    else if (selectedSettingsRow == 0) draftDeviceName = "";
    else draftEncryptionKey = "";
    drawSettingsScreen();
    return;
  }
  if (key == '\b' || key == 0x7F) {
    if (settingsTab == SettingsTab::LoRa && selectedSettingsRow == 0 && draftFrequencyText.length() > 0) {
      draftFrequencyText.remove(draftFrequencyText.length() - 1); drawSettingsScreen();
    } else if (settingsTab == SettingsTab::Security && selectedSettingsRow == 0 && draftDeviceName.length() > 0) {
      draftDeviceName.remove(draftDeviceName.length() - 1); drawSettingsScreen();
    } else if (settingsTab == SettingsTab::Security && selectedSettingsRow == 1 && draftEncryptionKey.length() > 0) {
      draftEncryptionKey.remove(draftEncryptionKey.length() - 1); drawSettingsScreen();
    }
    return;
  }
  // On the LoRa tab, digits '1'-'4' cycle rows 1-4 AND select that row.
  // Only row 0 (frequency) uses the frequency entry keypad for digits.
  if (settingsTab == SettingsTab::LoRa && key >= '1' && key <= '4' && selectedSettingsRow != 0) {
    selectedSettingsRow = static_cast<uint8_t>(key - '0');
    cycleDraftOption(selectedSettingsRow);
    drawSettingsScreen();
    return;
  }
  if (settingsTab == SettingsTab::LoRa && selectedSettingsRow == 0 &&
      ((key >= '0' && key <= '9') || key == '.') && draftFrequencyText.length() < 7) {
    openFrequencyEntryScreen();
    handleFrequencyInputChar(key);
    return;
  }
  if (settingsTab == SettingsTab::Security && isPrintable(key)) {
    if (selectedSettingsRow == 0 && draftDeviceName.length() < 16) { draftDeviceName += key; drawSettingsScreen(); }
    else if (selectedSettingsRow == 1 && draftEncryptionKey.length() < 32) { draftEncryptionKey += key; drawSettingsScreen(); }
  }
}

// Handle keyboard input while the frequency entry keypad is active.
// Accepts digits, '.', backspace, and Enter to accept.
void handleFrequencyInputChar(char key) {
  if (key == 0) return;
  if (key == '\r' || key == '\n') { acceptFrequencyEntry(); return; }
  if (key == 0x0C) { keypadFrequencyText = ""; drawFrequencyEntryScreen(); return; }
  if (key == '\b' || key == 0x7F) {
    if (keypadFrequencyText.length() > 0) { keypadFrequencyText.remove(keypadFrequencyText.length() - 1); drawFrequencyEntryScreen(); }
    return;
  }
  if (((key >= '0' && key <= '9') || key == '.') && keypadFrequencyText.length() < 7) {
    keypadFrequencyText += key;
    drawFrequencyEntryScreen();
  }
}

// --- Command handler ---

// Parse and execute a slash command from the input line.
// Returns true if the line started with '/' and was handled as a command.
// Returns false if the line should be treated as a chat message.
bool handleCommand(String line) {
  line.trim();
  if (!line.startsWith("/")) return false;

  String command = line;
  command.toLowerCase();

  if (command == "/help") {
    addLine("/settings shows LoRa");
    addLine("/freq 915.0 /bw 125");
    addLine("/sf 7-12 /cr 5-8 /pwr -9..22");
    addLine("/name <id> /key <secret>");
    statusLine = "commands shown";
    appPrintln("Commands: /id /settings /freq <mhz> /bw <khz> /sf <5-12> /cr <5-8> /pwr <-9..22> /name <id> /key <secret> /to all|<node> /discover <node> /routes /defaults /send <msg>");
    drawUi();
    return true;
  }

  if (command == "/id") { printIdentity(); statusLine = "identity shown"; drawUi(); return true; }

  if (command.startsWith("/send ")) {
    String body = line.substring(6); body.trim();
    if (body.length() == 0) { statusLine = "empty message"; drawBottom(); return true; }
    queueOutgoing(body);
#ifdef BOARD_HELTEC_V3
    appPrintln("Queued message.");
#endif
    drawUi(); return true;
  }

  if (command == "/settings") {
    addLine(settingsSummary());
    printIdentity();
    appPrintf("Node: %s\n", nodeIdHex(localNodeId).c_str());
    appPrintf("Name: %s\n", deviceName.c_str());
    appPrintf("LoRa: %s\n", settingsSummary().c_str());
    appPrintf("Destination: %s\n", selectedDestination == BROADCAST_NODE ? "ALL" : nodeIdHex(selectedDestination).c_str());
    statusLine = "settings shown"; drawUi(); return true;
  }

  if (command == "/routes") {
    addLine("routes:"); appPrintln("Routes:");
    const uint32_t now = millis();
    for (const auto &route : routes) {
      if (route.active && now - route.lastSeenAt <= ROUTE_TTL_MS) {
        addLine(nodeIdHex(route.destination).substring(4) + " via " + nodeIdHex(route.nextHop).substring(4) + " h" + String(route.hops));
        appPrintf("%s via %s h%u age %lus\n", nodeIdHex(route.destination).c_str(), nodeIdHex(route.nextHop).c_str(), route.hops, static_cast<unsigned long>((now - route.lastSeenAt) / 1000));
      }
    }
    statusLine = "routes shown"; drawUi(); return true;
  }

  if (command == "/to all") {
    selectGroupChat(); statusLine = "destination all"; addLine("to: all");
#ifdef BOARD_HELTEC_V3
    appPrintln("Destination set to all.");
#endif
    drawUi(); return true;
  }

  if (command.startsWith("/to ")) {
    uint32_t target = 0;
    if (!parseNodeId(command.substring(4), target)) { statusLine = "bad node id"; drawBottom(); return true; }
    selectDirectChat(target);
    statusLine = "to " + nodeIdHex(target).substring(4);
    addLine("to: " + nodeIdHex(target));
#ifdef BOARD_HELTEC_V3
    appPrintln("Destination set to " + nodeIdHex(target) + "; discovering route.");
#endif
    sendRouteRequest(target); drawUi(); return true;
  }

  if (command.startsWith("/discover ")) {
    uint32_t target = 0;
    if (parseNodeId(command.substring(10), target)) {
      sendRouteRequest(target);
#ifdef BOARD_HELTEC_V3
      appPrintln("Discovering " + nodeIdHex(target) + ".");
#endif
    } else { statusLine = "bad node id"; drawBottom(); }
    return true;
  }

  if (command == "/defaults") { RadioSettings defaults; return applyCommandSettings(defaults, "defaults"); }

  if (command.startsWith("/name ")) {
    if (hasPendingMessages()) { statusLine = "finish pending send first"; drawUi(); return true; }
    String updatedName = line.substring(6); updatedName.trim();
    if (updatedName.length() == 0 || updatedName.length() > 16) { statusLine = "name 1..16 chars"; drawUi(); return true; }
    deviceName = updatedName; saveRadioSettings(); configureBleAdvertisement();
    addLine("name: " + deviceName); appPrintf("Name saved: %s\n", deviceName.c_str());
    statusLine = "name saved"; drawUi(); return true;
  }

  if (command.startsWith("/key ")) {
    if (hasPendingMessages()) { statusLine = "finish pending send first"; drawUi(); return true; }
    String updatedKey = line.substring(5); updatedKey.trim();
    if (updatedKey.length() == 0 || updatedKey.length() > 32) { statusLine = "key 1..32 chars"; drawUi(); return true; }
    encryptionKey = updatedKey; saveRadioSettings();
#if ENABLE_FREQ_HOPPING
    buildHopChannels();
    hoppingSynced = false;
    hopSlot = 0;
    if (radioStarted && hopChannels != nullptr && hopCount > 0) retuneToFrequency(hopChannels[0]);
#endif
    addLine("key saved"); appPrintln("Encryption key saved.");
    statusLine = "key saved"; drawUi(); return true;
  }

  RadioSettings updated = radioSettings;
  if (command.startsWith("/freq ")) { updated.frequency = command.substring(6).toFloat(); return applyCommandSettings(updated, "freq"); }
  if (command.startsWith("/bw ")) { updated.bandwidth = normalizedBandwidth(command.substring(4).toFloat()); return applyCommandSettings(updated, "bw"); }
  if (command.startsWith("/sf ")) { updated.spreadingFactor = static_cast<uint8_t>(command.substring(4).toInt()); return applyCommandSettings(updated, "sf"); }
  if (command.startsWith("/cr ")) { updated.codingRate = static_cast<uint8_t>(command.substring(4).toInt()); return applyCommandSettings(updated, "cr"); }
  if (command.startsWith("/pwr ")) { updated.txPower = static_cast<int8_t>(command.substring(5).toInt()); return applyCommandSettings(updated, "pwr"); }

  // --- New feature commands ---

  if (command.startsWith("/frag ")) {
    String body = line.substring(6); body.trim();
    if (body.length() == 0) { statusLine = "empty message"; drawBottom(); return true; }
    uint32_t dest = (chatTab == ChatTab::Direct && selectedDirectNode != 0) ? selectedDirectNode : BROADCAST_NODE;
    sendFragmented(body, dest, dest);
    if (dest == BROADCAST_NODE) {
      addGroupEntry("me [frag]: " + body.substring(0, MAX_CHAT_TEXT_LEN), YELLOW, 0, true, false);
    } else {
      addDirectEntry(dest, nodeDisplayName(dest), "me [frag]: " + body.substring(0, MAX_CHAT_TEXT_LEN), YELLOW, 0, true, false);
    }
    drawUi(); return true;
  }

  if (command.startsWith("/emergency ") || command == "/sos") {
    String body;
    if (command.startsWith("/sos")) body = "SOS EMERGENCY";
    else body = command.substring(11);
    body.trim();
    if (body.length() == 0) body = "SOS EMERGENCY";
    sendEmergency(body);
    drawUi(); return true;
  }

  if (command == "/role relay") {
    setNodeRole(NodeRole::Relay);
    addLine("role: relay");
    appPrintln("Node role set to Relay (full mesh participation).");
    drawUi(); return true;
  }

  if (command == "/role leaf") {
    setNodeRole(NodeRole::Leaf);
    addLine("role: leaf");
    appPrintln("Node role set to Leaf (no forwarding, battery saving).");
    drawUi(); return true;
  }

  if (command == "/contacts") {
    addLine("contacts:");
    appPrintln("Contacts:");
    const uint32_t now = millis();
    for (const auto &c : contacts) {
      if (c.active) {
        const String status = c.online ? "ONLINE" : "offline";
        addLine(c.name + " " + nodeIdHex(c.nodeId).substring(4) + " " + status + " " + String(c.lastRssi) + "dBm");
        appPrintf("%s %s %s %ddBm %lus ago\n", c.name.c_str(), nodeIdHex(c.nodeId).c_str(), status.c_str(), c.lastRssi, static_cast<unsigned long>((now - c.lastSeenAt) / 1000));
      }
    }
    statusLine = "contacts shown";
    drawUi(); return true;
  }

  if (command == "/history") {
    saveChatHistory();
    statusLine = "history saved";
    drawBottom(); return true;
  }

  if (command.startsWith("/store ")) {
    // Manually store a message for later delivery (requires active direct destination)
    String body = line.substring(7); body.trim();
    if (body.length() == 0 || selectedDirectNode == 0) { statusLine = "/store <msg> (set /to first)"; drawBottom(); return true; }
    StoredMessage sm;
    sm.active = true;
    sm.destination = selectedDirectNode;
    sm.messageId = nextMessageId++;
    sm.conversation = selectedDirectNode;
    sm.body = encryptedPayloadFor(body);
    sm.rawText = body;
    sm.isDirect = true;
    sm.destName = nodeDisplayName(selectedDirectNode);
    storeMessage(sm);
    addDirectEntry(selectedDirectNode, sm.destName, "stored: " + body, YELLOW, sm.messageId, true, false);
    drawUi(); return true;
  }

  statusLine = "unknown command: /help";
#ifdef BOARD_HELTEC_V3
  appPrintln("Unknown command. Type /help.");
#endif
  drawBottom();
  return true;
}

// --- Main input dispatch ---

// Process a single character from any input source.
// `commandsOnly`: if true, only slash commands are handled; plain text is
//   treated as noise (used for UART0 noise immunity on Heltec V3).
// `lineOverride`: if provided, uses this String as the line buffer instead
//   of the global inputLine (used for separate UART0 buffer).
void handleInputChar(char key, bool commandsOnly, String *lineOverride) {
  if (key == 0) return;
  String &lineBuffer = lineOverride == nullptr ? inputLine : *lineOverride;

  if (!commandsOnly && screenMode == ScreenMode::Settings) { handleSettingsInputChar(key); return; }
  if (!commandsOnly && screenMode == ScreenMode::FrequencyEntry) { handleFrequencyInputChar(key); return; }

  if (key == 0x0C) { lineBuffer = ""; if (!commandsOnly) drawBottom(); return; }

  if (key == '\r' || key == '\n') {
    if (serialLineJustSubmitted && lineBuffer.length() == 0) { serialLineJustSubmitted = false; return; }
    String submitted = lineBuffer;
    lineBuffer = "";
    serialLineJustSubmitted = true;
#ifdef BOARD_HELTEC_V3
    submitted.trim();
    if (submitted.length() == 0) { if (!commandsOnly) { appPrintln("Type /help for commands."); drawUi(); } return; }
    if (commandsOnly && !submitted.startsWith("/")) { statusLine = "ignored uart noise"; return; }
    appPrintln("> " + submitted);
#endif
    if (!handleCommand(submitted)) {
      if (commandsOnly) { statusLine = "commands need /"; drawBottom(); return; }
      queueOutgoing(submitted);
#ifdef BOARD_HELTEC_V3
      appPrintln("Queued message.");
#endif
    }
    drawUi();
    return;
  }

  if (key == '\b' || key == 0x7F) {
    if (lineBuffer.length() > 0) { lineBuffer.remove(lineBuffer.length() - 1); if (!commandsOnly) drawBottom(); }
    return;
  }

  if (isPrintable(key) && lineBuffer.length() < MAX_BODY_LEN) {
    serialLineJustSubmitted = false;
    lineBuffer += key;
    if (!commandsOnly) drawBottom();
  }
}

// Service all input sources. Called from loop().
// Reads keyboard, USB serial, and (on Heltec V3) UART0 serial.
void serviceInput() {
  handleInputChar(readKeyboard());
  while (Serial.available()) handleInputChar(static_cast<char>(Serial.read()));
#ifdef BOARD_HELTEC_V3
#if HELTEC_ENABLE_UART0_INPUT
  // UART0 is command-only to prevent floating-pin noise from sending chat.
  while (Serial0.available()) handleInputChar(static_cast<char>(Serial0.read()), true, &uart0InputLine);
#endif
#endif
}

// --- Display / hardware setup ---

// Initialize the display (ST7789 on T-Deck, SSD1306 on Heltec V3).
void setupDisplay() {
#ifdef BOARD_HELTEC_V3
  Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL);
  pinMode(Vext, OUTPUT); digitalWrite(Vext, LOW);
  pinMode(BOARD_OLED_RST, OUTPUT); digitalWrite(BOARD_OLED_RST, LOW);
  delay(20); digitalWrite(BOARD_OLED_RST, HIGH);
#else
  pinMode(BOARD_TFT_BACKLIGHT, OUTPUT); digitalWrite(BOARD_TFT_BACKLIGHT, HIGH);
#endif
  gfx.begin();
#ifdef BOARD_HELTEC_V3
  gfx.setRotation(0);
#else
  gfx.setRotation(1);
#endif
  clearScreen(); flushDisplay();
}

// Initialize the T-Deck I2C keyboard. On Heltec V3, sets status only.
void setupKeyboard() {
#ifndef BOARD_HELTEC_V3
  Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL);
  delay(500);
  Wire.requestFrom(LILYGO_KB_SLAVE_ADDRESS, 1);
  if (Wire.read() == -1) { statusLine = "keyboard offline; USB serial works"; return; }
  setKeyboardDefaultBrightness(127);
  setKeyboardBrightness(80);
#else
  statusLine = "serial ready";
#endif
}

// Initialize the T-Deck GT911 touch controller. On Heltec V3, does nothing.
void setupTouch() {
#ifdef BOARD_HELTEC_V3
  return;
#else
  pinMode(BOARD_TOUCH_INT, INPUT);
  touch.setPins(-1, BOARD_TOUCH_INT);
  touchReady = touch.begin(Wire, GT911_SLAVE_ADDRESS_L);
  if (!touchReady) { appPrintln("GT911 touch not found"); statusLine = "touch offline; commands work"; return; }
  touch.setMaxCoordinates(320, 240);
  touch.setSwapXY(true);
  touch.setMirrorXY(false, true);
  appPrintln("GT911 touch ready");
#endif
}
