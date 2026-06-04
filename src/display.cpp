// ============================================================================
// display.cpp — UI rendering for all screens
//
// Renders:
//  - T-Deck: ST7789 320x240 color LCD with chat, mesh dashboard, settings
//  - Heltec V3: SSD1306 128x64 monochrome OLED status display
//
// Screen modes: Chat (group/direct tabs), Mesh Dashboard, Settings, Frequency Entry
// ============================================================================

#include "globals.h"

// ─── Color palette ───────────────────────────────────────────────────────────
// Dark theme with accent colors for readability on the ST7789 LCD.
constexpr uint16_t C_BG        = BLACK;            // 0x0000  main background
constexpr uint16_t C_HEADER_BG = 0x0841;           // very dark blue header bar
constexpr uint16_t C_DIVIDER   = 0x18C3;           // subtle grey separator
constexpr uint16_t C_CHAT_BUBBLE_ME  = 0x000C;     // dark navy for my messages
constexpr uint16_t C_CHAT_BUBBLE_THEM = 0x0800;    // dark maroon for their msgs
constexpr uint16_t C_INPUT_BG  = 0x10A2;           // dark slate input field
constexpr uint16_t C_ACCENT    = 0x067F;           // teal accent for active tabs
constexpr uint16_t C_ACCENT_DIM = 0x0210;          // dimmer teal for inactive
constexpr uint16_t C_SUCCESS   = GREEN;            // 0x07E0
constexpr uint16_t C_WARNING   = COLOR_ORANGE;     // 0xFD20
constexpr uint16_t C_DANGER    = RED;              // 0xF800
constexpr uint16_t C_MUTED     = 0x632C;           // warm grey for secondary text
constexpr uint16_t C_WHITE     = WHITE;            // 0xFFFF

// Layout constants
constexpr int16_t HEADER_H     = 22;   // top header bar height
constexpr int16_t TAB_H        = 20;   // tab button height
constexpr int16_t TAB_Y        = HEADER_H + 2;
constexpr int16_t CHAT_TOP     = TAB_Y + TAB_H + 4;
constexpr int16_t CHAT_BOTTOM  = 198;
constexpr int16_t INPUT_TOP    = CHAT_BOTTOM + 2;
constexpr int16_t STATUS_TOP   = 228;
constexpr int16_t ROW_H        = 20;   // chat row height

// ─── Helpers ─────────────────────────────────────────────────────────────────

void flushDisplay() { gfx.display(); }

void clearScreen() {
  gfx.fillScreen(C_BG);
}

// Draw a horizontal divider line.
static void drawDivider(int16_t y) {
  gfx.drawFastHLine(0, y, UI_WIDTH, C_DIVIDER);
}

// Draw a filled rounded rectangle (no outline).
static void drawPill(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  gfx.fillRoundRect(x, y, w, h, 4, color);
}

// Draw a rounded button with fill, border, and centered text label.
void drawButton(int16_t x, int16_t y, int16_t w, int16_t h, const String &label, uint16_t fill, uint16_t border) {
  gfx.fillRect(x, y, w, h, C_BG);
  gfx.fillRoundRect(x, y, w, h, 5, fill);
  gfx.drawRoundRect(x, y, w, h, 5, border);
  gfx.setTextColor(C_WHITE, fill);
  // Center text horizontally in the button.
  const int16_t labelW = label.length() * 6;  // font char width ≈ 6px
  gfx.setCursor(x + (w - labelW) / 2, y + 5);
  gfx.print(label);
}

// ─── Chat screen ─────────────────────────────────────────────────────────────

// Draw the top header bar with node ID, mesh icon, and gear icon.
static void drawHeader() {
  gfx.fillRect(0, 0, UI_WIDTH, HEADER_H, C_HEADER_BG);
  drawDivider(HEADER_H);

  gfx.setTextColor(CYAN, C_HEADER_BG);
  gfx.setCursor(6, 4);
  gfx.printf("LoRa Chat  %08lX", static_cast<unsigned long>(localNodeId));

  // Mesh icon (M) — top right area
  gfx.fillRoundRect(248, 2, 28, 18, 4, COLOR_DARK_GREY);
  gfx.drawRoundRect(248, 2, 28, 18, 4, C_WHITE);
  gfx.setTextColor(C_WHITE, COLOR_DARK_GREY);
  gfx.setCursor(257, 6);
  gfx.print("M");

  // Gear icon — simple filled circle with a dot
  gfx.fillRoundRect(282, 2, 32, 18, 4, COLOR_DARK_GREY);
  gfx.drawRoundRect(282, 2, 32, 18, 4, C_WHITE);
  gfx.setTextColor(C_WHITE, COLOR_DARK_GREY);
  gfx.setCursor(287, 6);
  gfx.print("\x08");  // gear symbol approximated with *
  gfx.setCursor(292, 6);
  gfx.print("*");
}

void drawGearIcon() { /* now drawn inline in drawHeader */ }
void drawMeshIcon()  { /* now drawn inline in drawHeader */ }

// Draw a chat tab pill.
void drawChatTab(int16_t x, int16_t w, const String &label, bool active) {
  const uint16_t fill = active ? C_ACCENT : COLOR_DARK_GREY;
  const uint16_t border = active ? C_ACCENT : C_DIVIDER;
  drawButton(x, TAB_Y, w, TAB_H, label, fill, border);
}

// Render chat messages with word-wrap and bubble backgrounds.
// Renders newest messages at the bottom (working upward) so the latest content
// is always visible even when older messages scroll off the top.
// Messages wider than 48 chars wrap across multiple rows.
// Outgoing -> right aligned (navy bubbles), incoming -> left (maroon).
void drawChatRows() {
  gfx.fillRect(0, CHAT_TOP, UI_WIDTH, CHAT_BOTTOM - CHAT_TOP, C_BG);
  ChatBuffer &buffer = activeChatBuffer();

  constexpr int16_t MAX_LINE_CHARS = 48;  // ~ fits 320px at font size 1

  // Render newest messages first — start at the bottom and work upward.
  int16_t y = CHAT_BOTTOM - ROW_H;

  for (int8_t mi = static_cast<int8_t>(buffer.count) - 1; mi >= 0 && y >= CHAT_TOP; --mi) {
    const ChatEntry &entry = buffer.entries[mi];
    const bool isMe = entry.text.startsWith("me ");
    String remaining = entry.text;

    // Collect all wrapped lines for this message so we can render them
    // bottom-up (last line at the bottom, first line above it).
    String lines[10];  // max 10 lines per message (480 chars, plenty of room)
    uint8_t lineCount = 0;
    while (remaining.length() > 0 && lineCount < 10) {
      lines[lineCount] = remaining.substring(0, MAX_LINE_CHARS);
      const size_t taken = min(static_cast<size_t>(remaining.length()),
                               static_cast<size_t>(MAX_LINE_CHARS));
      remaining = remaining.substring(taken);
      lineCount++;
    }

    // Render this message's lines bottom-up into the chat area.
    for (int8_t li = static_cast<int8_t>(lineCount) - 1; li >= 0 && y >= CHAT_TOP; --li) {
      const String &line = lines[li];

      if (isMe) {
        const int16_t textW = line.length() * 6;
        const int16_t bx = UI_WIDTH - textW - 12;
        gfx.fillRoundRect(bx - 4, y - 1, textW + 14, ROW_H - 2, 3, C_CHAT_BUBBLE_ME);
        gfx.setTextColor(entry.color, C_CHAT_BUBBLE_ME);
        gfx.setCursor(bx, y + 2);
        gfx.print(line);
      } else {
        gfx.fillRoundRect(4, y - 1, line.length() * 6 + 14, ROW_H - 2, 3, C_CHAT_BUBBLE_THEM);
        gfx.setTextColor(entry.color, C_CHAT_BUBBLE_THEM);
        gfx.setCursor(10, y + 2);
        gfx.print(line);
      }
      y -= ROW_H;
    }
  }
}

// Draw the text input field and status bar at the bottom.
void drawBottom() {
#ifdef BOARD_HELTEC_V3
  drawStatusScreen();
  return;
#endif
  if (screenMode != ScreenMode::Chat) return;

  // Input field background with border
  drawDivider(INPUT_TOP - 1);
  gfx.fillRect(0, INPUT_TOP, UI_WIDTH, STATUS_TOP - INPUT_TOP, C_INPUT_BG);
  gfx.drawRoundRect(4, INPUT_TOP + 2, UI_WIDTH - 8, STATUS_TOP - INPUT_TOP - 4, 4, C_DIVIDER);

  gfx.setTextColor(C_WHITE, C_INPUT_BG);
  gfx.setCursor(10, INPUT_TOP + 6);
  gfx.print("> ");
  // Show the message text, leaving room for the counter on the right.
  const String visibleInput = inputLine.substring(0, 36);
  gfx.print(visibleInput);

  // Character count in the input field's right margin (e.g. "13/47").
  gfx.setTextColor(C_MUTED, C_INPUT_BG);
  gfx.setCursor(UI_WIDTH - 50, INPUT_TOP + 6);
  gfx.printf("%d/%d", static_cast<int>(inputLine.length()), MAX_CHAT_TEXT_LEN);

  // Status bar at the very bottom
  gfx.fillRect(0, STATUS_TOP, UI_WIDTH, UI_HEIGHT - STATUS_TOP, C_HEADER_BG);
  gfx.setCursor(6, STATUS_TOP + 2);
  const bool busy = pending.active || pendingQueueCount > 0;
  gfx.setTextColor(busy ? C_WARNING : C_SUCCESS, C_HEADER_BG);
  gfx.print(statusLine.substring(0, 38));

  // Battery indicator on the right
  const uint16_t batColor = batteryPercent <= 20 ? C_DANGER : (batteryPercent <= 50 ? C_WARNING : C_SUCCESS);
  gfx.setTextColor(batColor, C_HEADER_BG);
  gfx.setCursor(UI_WIDTH - 72, STATUS_TOP + 2);
  gfx.printf("%d%%", static_cast<int>(batteryPercent));

  // Current frequency next to battery
  float currentFreq = radioSettings.frequency;
#if ENABLE_FREQ_HOPPING
  if (hoppingSynced) currentFreq = hopChannels[hopSlot];
#endif
  gfx.setTextColor(C_MUTED, C_HEADER_BG);
  gfx.setCursor(UI_WIDTH - 104, STATUS_TOP + 2);
  gfx.printf("%.1f", currentFreq);
  flushDisplay();
}

// ─── Mesh Dashboard ──────────────────────────────────────────────────────────

void drawMeshScreen() {
  const uint32_t now = millis();
  clearScreen();

  // Header
  gfx.fillRect(0, 0, UI_WIDTH, HEADER_H, C_HEADER_BG);
  drawDivider(HEADER_H);
  gfx.setTextColor(CYAN, C_HEADER_BG);
  gfx.setCursor(6, 4);  gfx.print("Mesh Dashboard");
  drawButton(270, 2, 44, 18, "Back", COLOR_DARK_GREY, C_DIVIDER);

  gfx.setTextSize(1);

  // ── Node info ──
  int16_t y = 26;
  gfx.setTextColor(C_ACCENT, C_BG);
  gfx.setCursor(8, y); gfx.print("Node");  y += 14;
  gfx.setTextColor(C_WHITE, C_BG);
  gfx.setCursor(8, y); gfx.print(nodeIdHex(localNodeId));
  gfx.setCursor(140, y); gfx.print("Dest ");
  gfx.print(selectedDestination == BROADCAST_NODE ? "ALL" : nodeIdHex(selectedDestination).substring(4));
  y += 14;
  gfx.setCursor(8, y); gfx.print("Routes ");
  gfx.setTextColor(C_ACCENT, C_BG); gfx.print(activeRouteCount());
  gfx.setTextColor(C_WHITE, C_BG);
  gfx.print("  BLE ");
  gfx.setTextColor(C_ACCENT, C_BG); gfx.print(activeBleNodeCount());
  gfx.setTextColor(C_WHITE, C_BG);
  gfx.print("  HELLO "); gfx.print((now - lastHelloAt) / 1000); gfx.print("s ago");
  y += 16;

  // ── Counters ──
  drawDivider(y); y += 3;
  gfx.setTextColor(C_MUTED, C_BG);
  gfx.setCursor(8, y); gfx.print("Traffic"); y += 14;
  gfx.setTextColor(C_WHITE, C_BG);
  gfx.setCursor(8, y);
  gfx.print("RX "); gfx.print(packetStats.rx);
  gfx.print("  TX "); gfx.print(packetStats.tx);
  gfx.print("  FWD "); gfx.print(packetStats.forwarded);
  y += 14;
  gfx.setCursor(8, y);
  gfx.print("DATA "); gfx.print(packetStats.dataRx); gfx.print("/"); gfx.print(packetStats.dataTx);
  gfx.print("  ACK "); gfx.print(packetStats.ackRx); gfx.print("/"); gfx.print(packetStats.ackTx);
  y += 14;
  gfx.setCursor(8, y);
  gfx.print("HELLO "); gfx.print(packetStats.helloRx); gfx.print("/"); gfx.print(packetStats.helloTx);
  gfx.print("  RREQ "); gfx.print(packetStats.rreqRx); gfx.print("/"); gfx.print(packetStats.rreqTx);
  gfx.print("  RREP "); gfx.print(packetStats.rrepRx); gfx.print("/"); gfx.print(packetStats.rrepTx);
  y += 16;

  // ── Frequency Hopping (FHSS) ──
#if ENABLE_FREQ_HOPPING
  drawDivider(y); y += 3;
  gfx.setTextColor(C_MUTED, C_BG);
  gfx.setCursor(8, y); gfx.print("FHSS"); y += 14;
  gfx.setTextColor(C_WHITE, C_BG);
  gfx.setCursor(8, y);
  if (hoppingSynced) {
    gfx.setTextColor(C_SUCCESS, C_BG); gfx.print("SYNCED");
    gfx.setTextColor(C_WHITE, C_BG);
    gfx.setCursor(72, y); gfx.print("Slot");
    gfx.setTextColor(C_ACCENT, C_BG); gfx.print(hopSlot);
    gfx.print("/"); gfx.print(hopCount);
    const uint32_t syncAgeSec = (millis() - lastHopSyncAt) / 1000;
    gfx.setTextColor(C_WHITE, C_BG);
    gfx.setCursor(172, y); gfx.print(syncAgeSec); gfx.print("s");
  } else {
    gfx.setTextColor(C_DANGER, C_BG); gfx.print("NOT SYNCED");
  }
  gfx.setTextColor(C_WHITE, C_BG);
  y += 14;
  gfx.setCursor(8, y);
  gfx.print("Freq ");
  gfx.setTextColor(C_ACCENT, C_BG);
  gfx.print(String(hopChannels[hopSlot], 1));
  gfx.print(" MHz");
  gfx.setTextColor(C_WHITE, C_BG);
  gfx.setCursor(128, y);
  gfx.print("Epoch ");
  gfx.setTextColor(C_ACCENT, C_BG);
  gfx.print(hopEpochOffset);
  gfx.print(" ms");
  gfx.setTextColor(C_WHITE, C_BG);
  gfx.setCursor(240, y);
  gfx.print("NT ");
  gfx.setTextColor(C_ACCENT, C_BG);
  gfx.print(static_cast<int32_t>(hopNetworkTime()));
  y += 14;
#endif

  // ── Features ──
  {
    drawDivider(y); y += 3;
    gfx.setTextColor(C_MUTED, C_BG);
    gfx.setCursor(8, y); gfx.print("Features"); y += 14;
    gfx.setTextColor(C_WHITE, C_BG);
    gfx.setCursor(8, y);
    gfx.print("Role ");
    gfx.setTextColor(getNodeRole() == NodeRole::Relay ? C_ACCENT : C_WARNING, C_BG);
    gfx.print(getNodeRole() == NodeRole::Relay ? "Relay" : "Leaf");
    gfx.setTextColor(C_WHITE, C_BG);
    gfx.print("  Frag ");
    gfx.setTextColor(C_ACCENT, C_BG);
    gfx.print(packetStats.fragRx);
    gfx.setTextColor(C_WHITE, C_BG);
    gfx.print("/");
    gfx.setTextColor(C_ACCENT, C_BG);
    gfx.print(packetStats.fragAssembled);
    y += 14;
    gfx.setTextColor(C_WHITE, C_BG);
    gfx.setCursor(8, y);
    gfx.print("EM ");
    gfx.setTextColor(COLOR_ALERT, C_BG);
    gfx.print(packetStats.emergencyTx);
    gfx.print("/");
    gfx.print(packetStats.emergencyRx);
    gfx.setTextColor(C_WHITE, C_BG);
    gfx.print("  S-F ");
    gfx.setTextColor(C_ACCENT, C_BG);
    gfx.print(packetStats.storedSent);
    gfx.setTextColor(C_WHITE, C_BG);
    gfx.print("  Cont ");
    gfx.setTextColor(C_ACCENT, C_BG);
    // Count online contacts
    uint8_t onlineContacts = 0;
    for (const auto &c : contacts) { if (c.active && c.online) onlineContacts++; }
    gfx.print(onlineContacts);
    y += 16;
  }

  // ── Battery ──
  {
    const uint16_t batColor = batteryPercent <= 20 ? C_DANGER : (batteryPercent <= 50 ? C_WARNING : C_SUCCESS);
    const int16_t batY = y;
    drawDivider(batY); y = batY + 3;
    gfx.setTextColor(C_MUTED, C_BG);
    gfx.setCursor(8, y); gfx.print("Battery"); y += 14;
    gfx.setTextColor(batColor, C_BG);
    gfx.setCursor(8, y);
    gfx.printf("%.2fV %d%%", batteryVoltage, static_cast<int>(batteryPercent));
    y += 14;
  }

  // ── BLE status ──
  drawDivider(y); y += 3;
  gfx.setTextColor(C_MUTED, C_BG);
  gfx.setCursor(8, y); gfx.print("BLE"); y += 14;
  gfx.setTextColor(C_WHITE, C_BG);
  gfx.setCursor(8, y); gfx.print(bleLine.substring(0, 42));
  y += 14;
  gfx.setCursor(8, y);
  gfx.print("seen "); gfx.print(packetStats.bleSeen);
  gfx.print("  scan "); gfx.print(packetStats.bleScans);
  gfx.print("  fail "); gfx.print(packetStats.bleConnectFail + packetStats.bleWriteFail);
  y += 14;
  // Show up to 3 BLE nodes inline
  uint8_t bleCount = 0;
  for (const auto &node : bleNodes) {
    if (!node.active || now - node.lastSeenAt > BLE_NODE_TTL_MS) continue;
    gfx.setCursor(8, y);
    gfx.print(nodeIdHex(node.nodeId).substring(4));
    gfx.print(" "); gfx.print(node.rssi); gfx.print("dBm");
    y += 12; bleCount++;
    if (bleCount >= 3) break;
  }
  if (bleCount == 0) { gfx.setTextColor(C_MUTED, C_BG); gfx.setCursor(8, y); gfx.print("none nearby"); y += 12; }
  y += 4;

  // ── Routes ──
  drawDivider(y); y += 3;
  gfx.setTextColor(C_MUTED, C_BG);
  gfx.setCursor(8, y); gfx.print("Routes"); y += 14;
  gfx.setTextColor(C_WHITE, C_BG);
  uint8_t routeCount = 0;
  for (const auto &route : routes) {
    if (!route.active || now - route.lastSeenAt > ROUTE_TTL_MS) continue;
    gfx.setCursor(8, y);
    gfx.print(nodeIdHex(route.destination).substring(4));
    gfx.print(" via "); gfx.print(nodeIdHex(route.nextHop).substring(4));
    gfx.print(" h"); gfx.print(route.hops);
    gfx.print(" "); gfx.print((now - route.lastSeenAt) / 1000); gfx.print("s");
    y += 12; routeCount++;
    if (routeCount >= 4) break;
  }
  if (routeCount == 0) { gfx.setTextColor(C_MUTED, C_BG); gfx.setCursor(8, y); gfx.print("none yet"); }
  flushDisplay();
}

// ─── Settings Screen ─────────────────────────────────────────────────────────

void drawSettingRow(uint8_t row, int16_t y, const String &label, const String &value, bool dropdown) {
  const bool selected = row == selectedSettingsRow;
  const uint16_t fill = selected ? COLOR_SELECTED_ROW : C_BG;
  const uint16_t border = selected ? C_ACCENT : C_DIVIDER;
  gfx.fillRoundRect(8, y, 304, 24, 4, fill);
  gfx.drawRoundRect(8, y, 304, 24, 4, border);
  gfx.setTextColor(C_WHITE, fill);
  gfx.setCursor(16, y + 5); gfx.print(label);
  gfx.setTextColor(selected ? C_WHITE : C_ACCENT, fill);
  gfx.setCursor(106, y + 5); gfx.print(value);
  if (dropdown) { gfx.setTextColor(C_MUTED, fill); gfx.setCursor(292, y + 5); gfx.print("\x1E"); }
}

void drawSettingsScreen() {
  clearScreen();

  // Header
  gfx.fillRect(0, 0, UI_WIDTH, HEADER_H, C_HEADER_BG);
  drawDivider(HEADER_H);
  gfx.setTextColor(CYAN, C_HEADER_BG);
  gfx.setCursor(6, 4); gfx.print("Settings");

  // Tab bar
  const int16_t tabY = HEADER_H + 4;
  drawButton(8, tabY, 148, 24, "Radio",
    settingsTab == SettingsTab::LoRa ? C_ACCENT : COLOR_DARK_GREY,
    settingsTab == SettingsTab::LoRa ? C_ACCENT : C_DIVIDER);
  drawButton(164, tabY, 148, 24, "Security",
    settingsTab == SettingsTab::Security ? C_ACCENT : COLOR_DARK_GREY,
    settingsTab == SettingsTab::Security ? C_ACCENT : C_DIVIDER);

  if (settingsTab == SettingsTab::LoRa) {
    drawSettingRow(0, 64, "Frequency", draftFrequencyText + " MHz", false);
    drawSettingRow(1, 92, "Bandwidth", String(draftSettings.bandwidth, 2) + " kHz", true);
    drawSettingRow(2, 120, "Spreading", "SF " + String(draftSettings.spreadingFactor), true);
    drawSettingRow(3, 148, "Coding Rate", "CR " + String(draftSettings.codingRate), true);
    drawSettingRow(4, 176, "TX Power", String(draftSettings.txPower) + " dBm", true);
  } else {
    String maskedKey;
    for (uint8_t i = 0; i < min(static_cast<size_t>(draftEncryptionKey.length()), static_cast<size_t>(18)); ++i)
      maskedKey += "*";
    drawSettingRow(0, 76, "Device Name", draftDeviceName.substring(0, 22), false);
    drawSettingRow(1, 108, "Encryption Key", maskedKey, false);
    gfx.setTextColor(C_MUTED, C_BG);
    gfx.setCursor(16, 140); gfx.print("Type with the physical keyboard");
    gfx.setCursor(16, 152); gfx.print("Press Enter to save");
  }

  // Buttons at bottom
  drawButton(8, 200, 148, 28, "Cancel", COLOR_DARK_GREY, C_DIVIDER);
  drawButton(164, 200, 148, 28, "Save", C_SUCCESS, C_SUCCESS);

  gfx.setTextColor(radioStarted ? C_SUCCESS : C_DANGER, C_BG);
  gfx.setCursor(8, 234); gfx.print(statusLine.substring(0, 48));
  flushDisplay();
}

// ─── Frequency Entry Keypad ──────────────────────────────────────────────────

void drawFrequencyEntryScreen() {
  clearScreen();

  // Header
  gfx.fillRect(0, 0, UI_WIDTH, HEADER_H, C_HEADER_BG);
  drawDivider(HEADER_H);
  gfx.setTextColor(CYAN, C_HEADER_BG);
  gfx.setCursor(6, 4); gfx.print("Set Frequency (MHz)");

  // Display value
  gfx.fillRoundRect(8, 28, 304, 28, 4, COLOR_SELECTED_ROW);
  gfx.drawRoundRect(8, 28, 304, 28, 4, C_ACCENT);
  gfx.setTextColor(C_WHITE, COLOR_SELECTED_ROW);
  gfx.setCursor(18, 36); gfx.print(keypadFrequencyText.length() > 0 ? keypadFrequencyText : "0.0");

  // Keypad grid — 4 rows x 3 columns
  const char *labels[] = {"1","2","3","4","5","6","7","8","9",".","0","Del"};
  for (uint8_t i = 0; i < 12; ++i) {
    const int16_t col = i % 3, row = i / 3;
    const int16_t bx = 14 + col * 98;
    const int16_t by = 66 + row * 32;
    const bool isSpecial = (labels[i][0] == '.' || labels[i][0] == 'D');
    const uint16_t fill = isSpecial ? COLOR_DARK_GREY : 0x18E3;  // lighter blue-grey for digits
    drawButton(bx, by, 90, 28, labels[i], fill, C_DIVIDER);
  }

  // Action buttons
  drawButton(14, 198, 140, 26, "Cancel", COLOR_DARK_GREY, C_DIVIDER);
  drawButton(166, 198, 140, 26, "OK", C_SUCCESS, C_SUCCESS);
  flushDisplay();
}

// ─── Main UI dispatcher ──────────────────────────────────────────────────────

void drawUi() {
#ifdef BOARD_HELTEC_V3
  drawStatusScreen();
  return;
#endif
  if (screenMode == ScreenMode::Mesh) { drawMeshScreen(); return; }
  if (screenMode == ScreenMode::Settings) { drawSettingsScreen(); return; }
  if (screenMode == ScreenMode::FrequencyEntry) { drawFrequencyEntryScreen(); return; }

  // ── Chat screen ──
  clearScreen();
  drawHeader();

  // Tab bar
  drawChatTab(4, 86, "Group", chatTab == ChatTab::Group);
  drawChatTab(94, 110, directChatLabel(), chatTab == ChatTab::Direct);
  drawChatTab(208, 32, ">", false);

  drawChatRows();
  drawBottom();
}

// ─── Heltec V3 status screen (unchanged) ────────────────────────────────────

void drawStatusScreen() {
  const uint32_t now = millis();
  clearScreen();
  gfx.setTextSize(1);
  gfx.setTextColor(WHITE, BLACK);
  gfx.setCursor(0, 0);
  gfx.print("Heltec LoRa ");
  gfx.print(nodeIdHex(localNodeId).substring(4));

  gfx.setCursor(0, 10);
  gfx.print(radioStarted ? "RF ok " : "RF off ");
  gfx.print(String(radioSettings.frequency, 1));
  gfx.print("MHz SF");
  gfx.print(radioSettings.spreadingFactor);

  gfx.setCursor(0, 20);
  gfx.print("Dest ");
  gfx.print(selectedDestination == BROADCAST_NODE ? "ALL" : nodeIdHex(selectedDestination).substring(4));
  gfx.print(" R"); gfx.print(activeRouteCount());
  gfx.print(" B"); gfx.print(activeBleNodeCount());

  gfx.setCursor(0, 30);
  gfx.print("RX/TX "); gfx.print(packetStats.rx);
  gfx.print("/"); gfx.print(packetStats.tx);
  gfx.print(" F"); gfx.print(packetStats.forwarded);

  gfx.setCursor(0, 40);
  gfx.print(aodvLine.substring(0, 21));

  gfx.setCursor(0, 50);
  gfx.print(statusLine.substring(0, 8));
  // Battery voltage and percent on Heltec V3 OLED (right side of line 6)
  gfx.setCursor(60, 50);
  gfx.printf("%.1fV %d%%", batteryVoltage, static_cast<int>(batteryPercent));
  // Pending/Hello indicator on the far right
  if (pending.active || pendingQueueCount > 0) {
    gfx.print(" Q"); gfx.print(pendingQueueCount + (pending.active ? 1 : 0));
  } else if (now - lastHelloAt < HELLO_INTERVAL_MS) {
    gfx.setCursor(102, 50);
    gfx.print(" H"); gfx.print((now - lastHelloAt) / 1000); gfx.print("s");
  }
  flushDisplay();
}

#ifdef BOARD_HELTEC_V3
void drawBootScreen(const String &line) {
  clearScreen();
  gfx.setTextSize(1);
  gfx.setTextColor(WHITE, BLACK);
  gfx.setCursor(0, 0); gfx.print("Heltec V3");
  gfx.setCursor(0, 12); gfx.print("LoRa Messenger");
  gfx.setCursor(0, 30); gfx.print(line.substring(0, 21));
  flushDisplay();
}
#endif